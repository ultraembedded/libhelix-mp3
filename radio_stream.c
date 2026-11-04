#ifndef ENABLE_RADIO
#define ENABLE_RADIO 0
#endif
#if ENABLE_RADIO
#include "radio_stream.h"
#include "radio_stream_probe.h"
#include "radio_runtime_flags.h"
#include "amiga_display_text.h"
#include "radio_metadata.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include "miniamp_memguard.h"

/* Network input ring (compressed stream bytes buffered ahead of the decoder).
 * Its size in seconds is bitrate-dependent: the old 64 KiB was ~4 s at 128 kbit
 * but only ~1.6 s at 320 kbit, so high-bitrate internet streams ran out of
 * buffer on any network jitter -- while the identical file offline (no jitter)
 * played fine. 256 KiB gives ~6.4 s at 320 kbit / ~16 s at 128 kbit, enough to
 * ride out typical stalls. Override -DRADIO_RING_BYTES for tight-RAM machines. */
#ifndef RADIO_RING_BYTES
#define RADIO_RING_BYTES 262144UL
#endif
/* Per-read chunk for the worker pump. A TLS record holds up to 16 KiB of
 * application data, and SSL_read() returns at most one record's worth per call
 * with the remainder buffered inside AmiSSL. Reading in 1 KiB slices therefore
 * throttled HTTPS to a fraction of a record per pump while the rest sat in the
 * SSL buffer (the socket looked "empty" to the worker), so higher-bitrate HTTPS
 * streams starved even though the identical HTTP stream -- where recv() reflects
 * the kernel buffer directly -- kept up. Sizing the chunk to a full TLS record
 * lets one SSL_read() drain a whole record, which is what higher-bitrate HTTPS
 * needs. The worker task has a 128 KiB stack, so a 16 KiB read buffer is safe. */
#ifndef RADIO_READ_CHUNK
#define RADIO_READ_CHUNK 16384
#endif
/* Prebuffer before playback starts. Keep it a small fraction of the (now
 * larger) ring so channel changes still start quickly (~0.8 s at 320 kbit,
 * ~2 s at 128 kbit); the rest of the ring then fills as headroom during play. */
#ifndef RADIO_START_THRESHOLD
#define RADIO_START_THRESHOLD (RADIO_RING_BYTES / 8)
#endif
#ifndef RADIO_LOW_WATER_BYTES
#define RADIO_LOW_WATER_BYTES 4096UL
#endif
#ifndef RADIO_RECONNECT_MAX
#define RADIO_RECONNECT_MAX 10
#endif
#ifndef RADIO_RECONNECT_BACKOFF_PUMPS
#define RADIO_RECONNECT_BACKOFF_PUMPS 16
#endif
#define RADIO_HEADER_MAX 4096
/* The ICY length byte describes up to 255 * 16 = 4080 payload bytes. Some
 * Bauer streams use nearly that allowance for a complete RadioInfo XML
 * document; retaining only the old first 511 bytes discarded DB_SONG_NAME
 * before it could be extracted. */
#define RADIO_META_MAX RADIO_METADATA_ICY_MAX
#define RADIO_META_TEXT_MAX 512
#ifndef RADIO_ZERO_BYTE_PUMP_MAX
#define RADIO_ZERO_BYTE_PUMP_MAX 64
#endif
#ifndef RADIO_START_TIMEOUT_PUMPS
#define RADIO_START_TIMEOUT_PUMPS 150
#endif
/* Mid-stream stall watchdog: consecutive would-block pumps (each preceded by
 * radio_backoff_sleep()'s ~40ms yield, so 750 is roughly 30 seconds) with no
 * data after playback has started.  A dead relay that keeps the TCP session
 * open but never sends another byte (and never a FIN) otherwise pumps forever:
 * the startup timeout (RADIO_START_TIMEOUT_PUMPS) only arms before everPlayed,
 * so the stream sat in "Buffering" for good.  On expiry the stream fails with
 * set_error() exactly like the startup timeout, so the child unwinds and
 * exits; see the comment at the check itself for why this is not an
 * automatic reconnect. */
#ifndef RADIO_STALL_TIMEOUT_PUMPS
#define RADIO_STALL_TIMEOUT_PUMPS 750
#endif
#if defined(RADIO_DEBUG) || defined(RADIO_DEBUG_OPEN)
#define RADIO_OPEN_DEBUG_PRINTF(x) RADIO_DBG_PRINTF(x)
#else
#define RADIO_OPEN_DEBUG_PRINTF(x) ((void)0)
#endif
#undef RADIO_STOP_DEBUG_PRINTF
#if defined(RADIO_DEBUG) || RADIO_DEBUG_STOP
#define RADIO_STOP_DEBUG_PRINTF(x) RADIO_DBG_PRINTF(x)
#define RADIO_CLEANUP_DEBUG_PRINTF(x) RADIO_DBG_PRINTF(x)
#else
#define RADIO_STOP_DEBUG_PRINTF(x) ((void)0)
#define RADIO_CLEANUP_DEBUG_PRINTF(x) ((void)0)
#endif

#if defined(AMIGA_M68K)
#include <stdarg.h>
#include <exec/semaphores.h>
#include <proto/exec.h>

/* MP3_TLS_TRACE gate: the per-operation TLS/network lifecycle traces below
 * (radio-tls-order/-close, radio-net*, radio-diag-leak, radio-resource,
 * radio-tcp, radio-pump, ...) are diagnostic noise for a normal user, so
 * radio_trace() prints only when MP3_TLS_TRACE is set (env var or ENV:). A
 * normal release build stays silent and pays no printf/fflush/serial cost; a
 * user hitting a problem can enable it for field logs without a debug rebuild.
 * Rare one-shot events (radio-memory corruption, radio-tls-poison FIRST) keep
 * plain printf() and stay always-visible. */
static int radio_tls_trace_enabled(void)
{
    static int cached = -1;
    if (cached < 0) cached = radio_runtime_flag_enabled("MP3_TLS_TRACE") ? 1 : 0;
    return cached;
}
static void radio_trace(const char *fmt, ...)
{
    va_list ap;
    if (!radio_tls_trace_enabled()) return;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
}

/* Every printf() call in this file -- not just the RADIO_DBG-wrapped ones --
 * runs on whichever task happens to own the RadioStream in question (the net
 * worker task, a playback child, or occasionally the GUI task), against the
 * same shared stdout as amiga_mp3dec.c/minimp3r.c's own output. A field log
 * caught two tasks' printf() output physically spliced together mid-line
 * (see radio_debug.h's RADIO_DBG comment for the exact bytes); RADIO_DBG/
 * RADIO_DBG_PRINTF lock around their own call, but this file also has many
 * always-on (non-debug-gated) printf() calls -- "radio-cleanup: abort
 * SSL_free policy...", "radio-memcheck: ...", "APP_CLOSE: ..." and others --
 * that were still unlocked and could race against them or each other.
 * Redirect every printf() in this file through one locked wrapper, sharing
 * radio_console_lock with amiga_mp3dec.c/minimp3r.c (declared extern, same
 * pattern as radio_debug.h) instead of auditing each call site individually. */
extern struct SignalSemaphore radio_console_lock;
static int RadioStreamLockedPrintf(const char *fmt, ...)
{
    int r;
    va_list ap;
    ObtainSemaphore(&radio_console_lock);
    va_start(ap, fmt);
    r = vprintf(fmt, ap);
    va_end(ap);
    ReleaseSemaphore(&radio_console_lock);
    return r;
}
#ifdef printf
#undef printf
#endif
#define printf RadioStreamLockedPrintf
#endif


#if defined(AMIGA_M68K)
#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/memory.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <exec/ports.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <exec/semaphores.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/bsdsocket.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
extern struct ExecBase *SysBase;
struct Library *SocketBase = NULL;
/* Single-worker-task architecture (HAVE_AMISSL builds): every read/write of
 * AmiSSLBase/AmiSSLExtBase/AmiSSLMasterBase/SocketBase now happens only on
 * the one long-lived radio net worker task started by
 * radio_net_worker_ensure_started() -- see the "single AmiSSL/bsdsocket
 * worker" block below. Because exactly one task ever touches them, there is
 * no more cross-task race to guard: Radio_AmiSslLock()/Unlock() are kept as
 * trivial no-ops purely for API compatibility with radio_stream_probe.c. */
int Radio_AmiSslLock(void) { return 1; }
void Radio_AmiSslUnlock(void) { }
#define RADIO_SOCKET long
#define RADIO_INVALID_SOCKET (-1)
#define radio_close_socket(s) CloseSocket(s)
#if defined(HAVE_AMISSL)
#include <libraries/amisslmaster.h>
#include <proto/amisslmaster.h>
#include <proto/amissl.h>
#include <amissl/amissl.h>
#include <errno.h>
#ifndef BIO_NOCLOSE
#define BIO_NOCLOSE 0
#endif
/* Strong definitions, private to the radio net worker task -- see the
 * "single AmiSSL/bsdsocket worker" block below.  radio_stream_probe.c still
 * references these same symbol names (required by the AmiSSL/bsdsocket
 * proto-header stubs, which resolve calls through a global of this exact
 * name) but only ever executes on this same worker task, via
 * Radio_RunOnNetWorker(); radio_browser_http.c and every other (GUI/opener)
 * task never see a non-NULL value from Radio_GetNetworkBases()/
 * Radio_GetAmiSslShared() and always fall back to opening their own
 * plain bsdsocket.library base instead. */
struct Library *AmiSSLMasterBase = NULL;
struct Library *AmiSSLBase = NULL;
struct Library *AmiSSLExtBase = NULL;
static int radio_amissl_initialized = 0;
/* Set alongside RadioStream::sslStatePoisoned (see Radio_Pump()'s
 * SSL_ERROR_SSL handling) -- mirrors it at file scope so the worker's own
 * per-task AmiSSL cleanup can be skipped too. One worker task only ever
 * touches AmiSSL, so a single flag is enough. */
static int radio_amissl_task_poisoned = 0;
/* Reason slug from whichever session most recently set radio_amissl_task_poisoned,
 * so the worker's CleanupAmiSSL skip can log why, even though that state is
 * task-global and no longer has the RadioStream. */
static char radio_amissl_task_poison_reason[24];

/* bsdsocket.library and AmiSSL are opened once by the net worker task at
 * startup and closed once when it exits -- these count those two events
 * specifically (expected to reach 1 each over an entire run), as opposed to
 * the per-connection socket/SSL/SSL_CTX counts declared further down, which
 * are expected to churn constantly. Declared up here because the worker
 * entry point below (radio_net_worker_entry) increments them at file-scope
 * before any per-connection counters are declared. */
static long radio_socket_library_open_count = 0;
static long radio_socket_library_close_count = 0;
static long radio_amissl_init_count = 0;
static long radio_amissl_cleanup_count = 0;
static long radio_amisslmaster_open_count = 0;
static long radio_amisslmaster_close_count = 0;
static long radio_openamissltags_count = 0;
static long radio_closeamissl_count = 0;
static SSL_CTX *radio_worker_ssl_ctx = NULL;
static int radio_worker_ssl_ctx_poisoned = 0;


/* ------------------------------------------------------------------------
 * Single long-lived AmiSSL/bsdsocket worker task.
 *
 * Mirrors amissl_child_worker_repro.c exactly: one persistent task opens
 * bsdsocket.library + amisslmaster.library + OpenAmiSSLTags() (with
 * AmiSSL_InitAmiSSL, TRUE and its own private AmiSSL_ErrNoPtr storage,
 * radio_net_worker_errno_store -- never the shared/global errno) exactly
 * once for the whole app run, then owns the pump loop for every active
 * station. Station open/close requests are still dispatched to that task,
 * but steady-state reads happen autonomously there (not as per-pump RPCs),
 * and each station only creates/frees a per-connection SSL/SSL_CTX/socket,
 * never touching the library bases again -- before
 * finally closing everything exactly once, in the same order the repro
 * uses, when the app asks it to shut down.
 *
 * The GUI/opener task and radio_stream_probe.c never read or write
 * SocketBase/AmiSSLBase/AmiSSLExtBase/AmiSSLMasterBase/the worker's errno
 * store directly -- they hand a small closure to Radio_RunOnNetWorker(),
 * which either runs it immediately (already on the worker task -- e.g.
 * nested calls from within a job) or ships it to the worker task over an
 * AmigaOS message port and blocks (with a generous but bounded timeout,
 * consistent with this file's existing "never wait on a foreign task
 * forever" philosophy -- see the old Radio_AmiSslLock() comment this
 * replaces) until the worker replies. */
static struct Task *radio_net_worker_task = NULL;
static struct MsgPort *radio_net_worker_port = NULL;
static volatile int radio_net_worker_ready = 0;     /* worker finished start-up (libs open or not) */
static volatile int radio_net_worker_libs_ok = 0;   /* worker's bsdsocket.library open succeeded (message loop is running) */
static volatile int radio_net_worker_https_ok = 0;  /* worker's AmiSSL instance also opened -- HTTPS available */
static long radio_net_worker_errno_store = 0;       /* worker task's own AmiSSL_ErrNoPtr storage */
static const char *radio_net_worker_shutdown_stage = "not-started";
static volatile unsigned long radio_net_worker_heartbeat = 0;
static volatile unsigned long radio_net_worker_last_session = 0;
static const char * volatile radio_net_worker_stage = "startup";
static const char * volatile radio_net_worker_last_op = "none";
/* Tentative declarations for shutdown breadcrumbs emitted before the full
 * resource-counter block appears later in the file. */
static long radio_open_socket_count;
static long radio_active_ssl_count;
static long radio_active_ssl_ctx_count;
typedef enum {
    RADIO_WORKER_IDLE = 0,
    RADIO_WORKER_PROBING,
    RADIO_WORKER_OPENING,
    RADIO_WORKER_PLAYING,
    RADIO_WORKER_STOPPING,
    RADIO_WORKER_CLOSING
} RadioWorkerState;
static volatile RadioWorkerState radio_worker_state = RADIO_WORKER_IDLE;
static RadioStream *radio_net_worker_streams = NULL;  /* worker-owned list of streams to pump autonomously */
static int radio_net_worker_pump_active = 0;          /* reentrancy guard for the autonomous pump loop */

static int radio_pump_body(RadioStream *rs);
static void radio_worker_pump_active_streams(void);
static void radio_stream_lock(RadioStream *rs);
static SSL_CTX *radio_worker_get_ssl_ctx(const char *category, unsigned long session_id);
static int radio_worker_shutdown_ssl_ctx(void);
static void radio_stream_unlock(RadioStream *rs);

/* ~60s at ~40ms/poll: generous enough to cover connect_http()'s worst-case
 * DNS + TCP connect + TLS handshake budget end to end, while still bounded
 * -- a caller must never Wait() forever on a worker that might be wedged
 * inside a blocking bsdsocket/AmiSSL call (this file's history has already
 * seen CleanupAmiSSL() loop forever on corrupted internals). */
#define RADIO_NET_WORKER_WAIT_TRIES 1500
/* ~10s: bounded wait for the worker task itself to start up and finish
 * opening its libraries. */
#define RADIO_NET_WORKER_START_TRIES 250

typedef struct RadioNetWorkerJob {
    struct Message msg;
    void (*fn)(void *arg);
    void *arg;
    int isShutdown;
} RadioNetWorkerJob;

static RadioNetWorkerJob *radio_net_job_alloc(void)
{
#if defined(AMIGA_M68K)
    return (RadioNetWorkerJob *)AllocVec(sizeof(RadioNetWorkerJob), MEMF_ANY | MEMF_CLEAR);
#else
    return (RadioNetWorkerJob *)calloc(1, sizeof(RadioNetWorkerJob));
#endif
}

static void radio_net_job_free(RadioNetWorkerJob *job)
{
    if (!job) return;
#if defined(AMIGA_M68K)
    FreeVec(job);
#else
    free(job);
#endif
}

#endif /* HAVE_AMISSL: the allocators below also serve plain HTTP. */

/* Every per-request / per-connection network object -- the open+IO request
 * blocks, their duplicated URL/host/category strings, the payload buffers and
 * the RadioNetTransport itself -- is allocated with AllocVec/FreeVec instead
 * of the C library malloc/free. These objects cross the GUI/opener <-> net
 * worker task boundary: the opener task allocates a request, the worker frees
 * it on the abandon path, and the worker allocates and frees its own transport
 * and a fresh IO buffer on every single read while the GUI keeps allocating
 * too. libnix's malloc arena is NOT task-safe, so sharing it between those
 * tasks was silently corrupting the exec heap (surfacing later as a
 * "bad chunk size" in the free list). AllocVec/FreeVec route straight to
 * Exec's AllocMem/FreeMem, which serialise internally, so ownership may move
 * between tasks and concurrent alloc/free from GUI + worker is safe -- the
 * same isolation the standalone amissl_child_worker_repro gets for free by
 * being a separate executable with its own C runtime. */
static void *radio_net_alloc0(size_t n)
{
#if defined(AMIGA_M68K)
    return AllocVec(n, MEMF_ANY | MEMF_CLEAR);
#else
    return calloc(1, n);
#endif
}

static void *radio_net_alloc_raw(size_t n)
{
#if defined(AMIGA_M68K)
    return AllocVec(n, MEMF_ANY);
#else
    return malloc(n);
#endif
}

static void radio_net_free(void *p)
{
    if (!p) return;
#if defined(AMIGA_M68K)
    FreeVec(p);
#else
    free(p);
#endif
}

#if defined(HAVE_AMISSL)
static int radio_net_worker_is_self(void)
{
    return radio_net_worker_task != NULL && FindTask(NULL) == radio_net_worker_task;
}

/* True when called BY the worker task itself -- the only task that ever
 * runs OpenAmiSSLTags()/InitAmiSSL() in this process, so it is always "the
 * opener" per the AmiSSL v5/v6 SDK and must never run a manual
 * InitAmiSSL()/CleanupAmiSSL() pair on top of that. */
int Radio_AmiSslTaskIsOpener(void) { return radio_net_worker_is_self(); }

static void radio_worker_breadcrumb(const char *stage, const char *op, unsigned long session)
{
    radio_net_worker_stage = stage ? stage : "<null>";
    radio_net_worker_last_op = op ? op : "<null>";
    radio_net_worker_last_session = session;
    radio_net_worker_heartbeat++;
}

#ifdef RADIO_AMISSL_LIFECYCLE_DIAG
static void radio_amissl_lifecycle_diag_impl(const char *op, unsigned long session, SSL *ssl, SSL_CTX *ctx)
{
    struct Task *task = FindTask(NULL);
    const char *task_name = "(unnamed)";
    if (task && task->tc_Node.ln_Name)
        task_name = task->tc_Node.ln_Name;
    radio_trace("radio-amissl-life: op=%s task=%p taskName=\"%s\" AmiSSLBase=%p AmiSSLExtBase=%p AmiSSLMasterBase=%p SocketBase=%p ssl=%p ctx=%p session=%lu\n",
        op ? op : "(null)", (void *)task, task_name, (void *)AmiSSLBase,
        (void *)AmiSSLExtBase, (void *)AmiSSLMasterBase, (void *)SocketBase,
        (void *)ssl, (void *)ctx, session);
}
#define radio_amissl_lifecycle_diag(op, session, ssl, ctx) radio_amissl_lifecycle_diag_impl((op), (session), (ssl), (ctx))
#else
#define radio_amissl_lifecycle_diag(op, session, ssl, ctx) ((void)0)
#endif

static void radio_net_worker_entry(void)
{
    struct MsgPort *port;
    int shuttingDown = 0;
    int safe_to_close_amissl = 1;
    int diag_leak_abandon = radio_runtime_diag_leak_ssl_enabled();
    RadioNetWorkerJob *shutdownJob = NULL;

    RADIO_DBG(printf("radio-net-worker: starting task=%p\n", (void *)FindTask(NULL)););
    radio_worker_breadcrumb("startup", "worker-entry", 0);

    radio_amissl_lifecycle_diag("OpenLibrary(bsdsocket)-before", 0, NULL, NULL);
    SocketBase = OpenLibrary("bsdsocket.library", 4);
    if (SocketBase) radio_socket_library_open_count++;
    radio_amissl_lifecycle_diag("OpenLibrary(bsdsocket)-after", 0, NULL, NULL);
    if (SocketBase)
        radio_amissl_lifecycle_diag("OpenLibrary(amisslmaster)-before", 0, NULL, NULL);
    if (SocketBase)
        AmiSSLMasterBase = OpenLibrary("amisslmaster.library", AMISSLMASTER_MIN_VERSION);
    if (AmiSSLMasterBase) radio_amisslmaster_open_count++;
    radio_amissl_lifecycle_diag("OpenLibrary(amisslmaster)-after", 0, NULL, NULL);
    if (SocketBase && AmiSSLMasterBase) {
        radio_amissl_lifecycle_diag("InitAmiSSLMaster-via-OpenAmiSSLTags-before", 0, NULL, NULL);
        radio_amissl_lifecycle_diag("OpenAmiSSL-before", 0, NULL, NULL);
        radio_amissl_lifecycle_diag("InitAmiSSL-before", 0, NULL, NULL);
        if (OpenAmiSSLTags(AMISSL_CURRENT_VERSION,
                           AmiSSL_UsesOpenSSLStructs, TRUE,
                           AmiSSL_InitAmiSSL, TRUE,
                           AmiSSL_GetAmiSSLBase, (ULONG)&AmiSSLBase,
                           AmiSSL_GetAmiSSLExtBase, (ULONG)&AmiSSLExtBase,
                           AmiSSL_SocketBase, (ULONG)SocketBase,
                           AmiSSL_ErrNoPtr, (ULONG)&radio_net_worker_errno_store,
                           TAG_DONE) != 0) {
            AmiSSLBase = NULL;
            AmiSSLExtBase = NULL;
        } else {
            radio_openamissltags_count++;
            radio_amissl_init_count++;
            radio_amissl_lifecycle_diag("OpenAmiSSL-after", 0, NULL, NULL);
            radio_amissl_lifecycle_diag("InitAmiSSL-after", 0, NULL, NULL);
            radio_amissl_lifecycle_diag("OPENSSL_init_ssl-implicit", 0, NULL, NULL);
        }
    }
    radio_amissl_initialized = (AmiSSLBase != NULL);
    radio_net_worker_libs_ok = (SocketBase != NULL) ? 1 : 0;
    radio_net_worker_https_ok = (AmiSSLBase != NULL) ? 1 : 0;
    RADIO_DBG(printf("radio-net-worker: libs_ok=%d https_ok=%d SocketBase=%p AmiSSLMasterBase=%p AmiSSLBase=%p AmiSSLExtBase=%p ErrNoPtr=%p\n",
        radio_net_worker_libs_ok, radio_net_worker_https_ok, (void *)SocketBase, (void *)AmiSSLMasterBase,
        (void *)AmiSSLBase, (void *)AmiSSLExtBase, (void *)&radio_net_worker_errno_store););

    port = radio_net_worker_libs_ok ? CreateMsgPort() : NULL;
    radio_net_worker_port = port;
    radio_net_worker_ready = 1; /* publish last: port + libs_ok are now safe to read */

    if (port) {
        for (;;) {
            RadioNetWorkerJob *job;
            radio_worker_breadcrumb("worker-loop", "poll", 0);
            while ((job = (RadioNetWorkerJob *)GetMsg(port)) != NULL) {
                if (job->isShutdown) {
                    radio_worker_breadcrumb("job-start", "shutdown", 0);
                    shutdownJob = job;
                    shuttingDown = 1;
                } else {
                    radio_worker_breadcrumb("job-start", "run", 0);
                    if (job->fn)
                        job->fn(job->arg);
                    radio_worker_breadcrumb("job-reply", "run", 0);
                    ReplyMsg(&job->msg);
                }
            }
            if (shuttingDown) break;
            radio_worker_pump_active_streams();
            /* Service an active stream once per Amiga tick. The pump itself
             * does not sleep on WANT_READ/EWOULDBLOCK in this worker path,
             * avoiding the old double-delay throughput ceiling. */
            Delay(radio_net_worker_streams ? 1 : 2);
        }
    }

    radio_net_worker_shutdown_stage = "shutdown-begin";
    radio_worker_breadcrumb("shutdown-begin", "shutdown", 0);
    RADIO_DBG(printf("radio-net-worker: shutdown begin task=%p shutdownJob=%p\n", (void *)FindTask(NULL), (void *)shutdownJob););
    if (diag_leak_abandon) {
        /* MP3_DIAG_LEAK_SSL: leaked per-connection SSL objects may still
         * reference the shared SSL_CTX and AmiSSL internal state, so the
         * final teardown must not walk that object graph at all: no shared
         * SSL_CTX_free(), no CloseAmiSSL(), no CloseLibrary() on
         * AmiSSLMasterBase or the worker's SocketBase. This is deliberate,
         * expected abandonment for the diagnostic run, not a shutdown
         * failure -- the worker still exits normally below (message port
         * teardown is plain Exec and touches nothing of AmiSSL's). */
        radio_net_worker_shutdown_stage = "diag-leak teardown skipped";
        radio_worker_breadcrumb("diag-leak teardown skipped", "shutdown", 0);
        radio_trace("radio-diag-leak: final AmiSSL/SSL_CTX/library teardown skipped; reboot required after diagnostic run\n");
    }
    if (AmiSSLBase && !diag_leak_abandon) {
        radio_net_worker_shutdown_stage = "before CloseAmiSSL";
        radio_worker_breadcrumb("before CloseAmiSSL", "CloseAmiSSL", 0);
        /* Only GUI radio-browser builds link radio_stream_probe.c.
         * The decoder-only fast030 radio build shares this worker teardown
         * path but has no probe module, so avoid a hard linker dependency
         * on rb_probe_shutdown_tls_context(). */
#ifdef RADIO_HAVE_STREAM_PROBE
        rb_probe_shutdown_tls_context();
#endif
        /* Production TLS lifecycle keeps only the worker-lifetime shared
         * SSL_CTX. Per-connection SSL objects are closed/freed before this
         * worker shutdown point, after their raw sockets have been closed. */
        safe_to_close_amissl = radio_worker_shutdown_ssl_ctx();
        if (safe_to_close_amissl) {
            RADIO_DBG(printf("radio-worker-risk: before CloseAmiSSL workerTask=%p active_ssl=%ld active_ctx=%ld open_socket=%ld\n",
                (void *)radio_net_worker_task, radio_active_ssl_count, radio_active_ssl_ctx_count, radio_open_socket_count););
            RADIO_DBG(printf("radio-net-worker: before CloseAmiSSL base=%p ext=%p\n", (void *)AmiSSLBase, (void *)AmiSSLExtBase););
            radio_amissl_lifecycle_diag("CloseAmiSSL-before-implicit-CleanupAmiSSL", 0, NULL, NULL);
            CloseAmiSSL();
            AmiSSLBase = NULL;
            AmiSSLExtBase = NULL;
            radio_amissl_cleanup_count++;
            radio_closeamissl_count++;
            radio_amissl_initialized = 0;
            radio_net_worker_shutdown_stage = "after CloseAmiSSL";
            radio_worker_breadcrumb("after CloseAmiSSL", "CloseAmiSSL", 0);
            radio_amissl_lifecycle_diag("CloseAmiSSL-after-implicit-CleanupAmiSSL", 0, NULL, NULL);
            RADIO_DBG(printf("radio-net-worker: after CloseAmiSSL\n"););
        } else {
            RADIO_DBG(printf("radio-net-worker: final CloseAmiSSL/AmiSSL library closes quarantined because shared SSL_CTX shutdown found poison memory_poisoned=%d tls_poisoned=%d ctx_poisoned=%d\n",
                Radio_IsMemoryPoisoned(), Radio_IsTlsPoisoned(), radio_worker_ssl_ctx_poisoned););
            AmiSSLBase = NULL;
            AmiSSLExtBase = NULL;
        }
    }
    if (AmiSSLMasterBase && safe_to_close_amissl && !diag_leak_abandon) {
        radio_net_worker_shutdown_stage = "before CloseLibrary AmiSSLMasterBase";
        radio_worker_breadcrumb("before CloseLibrary AmiSSLMasterBase", "CloseLibrary", 0);
        RADIO_DBG(printf("radio-net-worker: before CloseLibrary AmiSSLMasterBase=%p\n", (void *)AmiSSLMasterBase););
        radio_amissl_lifecycle_diag("CloseLibrary(amisslmaster)-before", 0, NULL, NULL);
        CloseLibrary(AmiSSLMasterBase);
        AmiSSLMasterBase = NULL;
        radio_amisslmaster_close_count++;
        radio_net_worker_shutdown_stage = "after CloseLibrary AmiSSLMasterBase";
        radio_worker_breadcrumb("after CloseLibrary AmiSSLMasterBase", "CloseLibrary", 0);
        radio_amissl_lifecycle_diag("CloseLibrary(amisslmaster)-after", 0, NULL, NULL);
        RADIO_DBG(printf("radio-net-worker: after CloseLibrary AmiSSLMasterBase\n"););
    }
    if (SocketBase && safe_to_close_amissl && !diag_leak_abandon) {
        radio_net_worker_shutdown_stage = "before CloseLibrary SocketBase";
        radio_worker_breadcrumb("before CloseLibrary SocketBase", "CloseLibrary", 0);
        RADIO_DBG(printf("radio-net-worker: before CloseLibrary SocketBase=%p\n", (void *)SocketBase););
        radio_amissl_lifecycle_diag("CloseLibrary(bsdsocket)-before", 0, NULL, NULL);
        CloseLibrary(SocketBase);
        SocketBase = NULL;
        radio_socket_library_close_count++;
        radio_net_worker_shutdown_stage = "after CloseLibrary SocketBase";
        radio_worker_breadcrumb("after CloseLibrary SocketBase", "CloseLibrary", 0);
        radio_amissl_lifecycle_diag("CloseLibrary(bsdsocket)-after", 0, NULL, NULL);
        RADIO_DBG(printf("radio-net-worker: after CloseLibrary SocketBase\n"););
    }
    radio_amissl_initialized = 0;
    if (port) {
        radio_net_worker_shutdown_stage = "before DeleteMsgPort";
        radio_worker_breadcrumb("before DeleteMsgPort", "DeleteMsgPort", 0);
        RADIO_DBG(printf("radio-net-worker: before DeleteMsgPort port=%p\n", (void *)port););
        DeleteMsgPort(port);
        radio_net_worker_shutdown_stage = "after DeleteMsgPort";
        radio_worker_breadcrumb("after DeleteMsgPort", "DeleteMsgPort", 0);
        RADIO_DBG(printf("radio-net-worker: after DeleteMsgPort\n"););
    }
    radio_net_worker_port = NULL;
    radio_net_worker_ready = 0;
    radio_net_worker_libs_ok = 0;
    radio_net_worker_https_ok = 0;
    radio_net_worker_shutdown_stage = "cleanup-complete";
    radio_worker_breadcrumb("cleanup-complete", "shutdown-reply", 0);
    if (shutdownJob) {
        RADIO_DBG(printf("radio-net-worker: shutdown complete; replying to shutdown request\n"););
        ReplyMsg(&shutdownJob->msg);
    }
    /* radio_net_worker_task itself is left as-is: Radio_NetworkShutdown()
     * (the only caller that gets here) is about to exit the whole app, and
     * clearing it here would race the exiting worker task with any other
     * task still holding a (now stale) reference. */
}

/* Lazily start the worker (safe to call repeatedly -- Radio_NetworkInit()
 * and the first station open both run on the GUI/opener task, so there is
 * no concurrent-start race to guard) and wait, bounded, for it to finish
 * opening its libraries. Returns 1 if the worker is up and its libraries
 * opened, 0 otherwise (never started, or its own OpenAmiSSLTags()/
 * OpenLibrary() failed). */
static int radio_net_worker_ensure_started(void)
{
    int tries;
    if (radio_net_worker_ready) return radio_net_worker_libs_ok;
    if (!radio_net_worker_task) {
        struct Process *proc = CreateNewProcTags(
            NP_Entry, (ULONG)radio_net_worker_entry,
            NP_Name, (ULONG)"MintAMP radio net worker",
            NP_Priority, 0,
            NP_StackSize, 131072,
            NP_CopyVars, FALSE,
            TAG_DONE);
        if (!proc) return 0;
        radio_net_worker_task = (struct Task *)proc;
    }
    for (tries = 0; tries < RADIO_NET_WORKER_START_TRIES && !radio_net_worker_ready; tries++)
        Delay(2);
    return radio_net_worker_ready ? radio_net_worker_libs_ok : 0;
}

/* Run fn(arg) synchronously, either directly (already on the worker task --
 * e.g. a job calling back into another helper) or by shipping it to the
 * worker task over its message port and blocking for the reply. This is the
 * ONLY way any code in this program may touch bsdsocket.library/AmiSSL: no
 * other task ever calls socket()/connect()/SSL_*()/InitAmiSSL() itself.
 * Returns 1 if fn ran (on whichever task), 0 if the worker could not be
 * started or the wait timed out. On a timeout, job/replyPort are
 * deliberately leaked rather than freed -- the worker may still be about to
 * write its reply into them -- same "leak rather than risk a crash"
 * trade-off this file already makes for a wedged CleanupAmiSSL(). */
int Radio_RunOnNetWorker(void (*fn)(void *arg), void *arg)
{
    RadioNetWorkerJob *job;
    struct MsgPort *replyPort;
    int tries;

    if (!fn) return 0;
    if (radio_net_worker_is_self()) { fn(arg); return 1; }
    if (!radio_net_worker_ensure_started()) return 0;

    job = radio_net_job_alloc();
    if (!job) return 0;
    replyPort = CreateMsgPort();
    if (!replyPort) { radio_net_job_free(job); return 0; }

    memset(&job->msg, 0, sizeof(job->msg));
    job->msg.mn_ReplyPort = replyPort;
    job->msg.mn_Length = sizeof(*job);
    job->fn = fn;
    job->arg = arg;
    job->isShutdown = 0;
    PutMsg(radio_net_worker_port, &job->msg);

    for (tries = 0; tries < RADIO_NET_WORKER_WAIT_TRIES; tries++) {
        if (GetMsg(replyPort)) {
            DeleteMsgPort(replyPort);
            radio_net_job_free(job);
            return 1;
        }
        Delay(2);
    }
    RADIO_DBG(printf("radio-net-worker: job dispatch timed out, worker may be wedged -- leaking job=%p replyPort=%p\n", (void *)job, (void *)replyPort););
    /* The job itself may be sitting inside a masked blocking bsdsocket call
     * (gethostbyname() in connect_http() or in the stream/favicon prober --
     * both wrap their call in SBTC_BREAKMASK/SIGBREAKF_CTRL_C for exactly
     * this reason). Without this kick a wedged resolver leaves the worker
     * task permanently unable to run any later job -- every subsequent
     * search/probe/play would also queue behind it and time out the same
     * way, forever, instead of just this one attempt. Same "safe to signal
     * regardless of what the worker is doing" reasoning as
     * Radio_RequestStop()'s own call to this. */
    if (radio_net_worker_task)
        Signal(radio_net_worker_task, SIGBREAKF_CTRL_C);
    return 0;
}

/* Ask the worker task to close its libraries and exit, then wait, bounded,
 * for it to actually do so. Only called once, from Radio_NetworkShutdown(). */
static int radio_net_worker_stop(void)
{
    RadioNetWorkerJob *job;
    struct MsgPort *replyPort;
    int tries;
    int stopped;

    if (!radio_net_worker_ready || !radio_net_worker_port) return 1; /* never started, or already gone */

    job = radio_net_job_alloc();
    if (!job) return 0;
    replyPort = CreateMsgPort();
    if (!replyPort) { radio_net_job_free(job); return 0; }
    memset(&job->msg, 0, sizeof(job->msg));
    job->msg.mn_ReplyPort = replyPort;
    job->msg.mn_Length = sizeof(*job);
    job->fn = NULL;
    job->arg = NULL;
    job->isShutdown = 1;
    PutMsg(radio_net_worker_port, &job->msg);

    for (tries = 0; tries < RADIO_NET_WORKER_WAIT_TRIES; tries++) {
        if (GetMsg(replyPort)) {
            stopped = (!radio_net_worker_ready && radio_net_worker_port == NULL &&
                !radio_net_worker_libs_ok && !radio_net_worker_https_ok);
            DeleteMsgPort(replyPort);
            radio_net_job_free(job);
            if (!stopped) {
                RADIO_DBG(printf("radio-netshutdown: shutdown reply received but final state is not clean shutdownStage=\"%s\" stage=\"%s\" lastOp=\"%s\" lastSession=%lu heartbeat=%lu workerTask=%p ready=%d port=%p SocketBase=%p AmiSSLBase=%p AmiSSLMasterBase=%p libs_ok=%d https_ok=%d\n",
                    radio_net_worker_shutdown_stage ? radio_net_worker_shutdown_stage : "<unset>",
                    radio_net_worker_stage ? (const char *)radio_net_worker_stage : "<unset>",
                    radio_net_worker_last_op ? (const char *)radio_net_worker_last_op : "<unset>",
                    radio_net_worker_last_session, radio_net_worker_heartbeat,
                    (void *)radio_net_worker_task, radio_net_worker_ready,
                    (void *)radio_net_worker_port, (void *)SocketBase,
                    (void *)AmiSSLBase, (void *)AmiSSLMasterBase,
                    radio_net_worker_libs_ok, radio_net_worker_https_ok););
            }
            return stopped;
        }
        Delay(2);
    }
    RADIO_DBG(printf("radio-netshutdown: timeout stage=\"%s\" lastOp=\"%s\" lastSession=%lu heartbeat=%lu shutdownStage=\"%s\" workerTask=%p ready=%d port=%p SocketBase=%p AmiSSLBase=%p AmiSSLMasterBase=%p libs_ok=%d https_ok=%d job=%p replyPort=%p\n",
        radio_net_worker_stage ? (const char *)radio_net_worker_stage : "<unset>",
        radio_net_worker_last_op ? (const char *)radio_net_worker_last_op : "<unset>",
        radio_net_worker_last_session, radio_net_worker_heartbeat,
        radio_net_worker_shutdown_stage ? radio_net_worker_shutdown_stage : "<unset>",
        (void *)radio_net_worker_task, radio_net_worker_ready,
        (void *)radio_net_worker_port, (void *)SocketBase,
        (void *)AmiSSLBase, (void *)AmiSSLMasterBase,
        radio_net_worker_libs_ok, radio_net_worker_https_ok,
        (void *)job, (void *)replyPort););
    return 0;
}
#endif /* HAVE_AMISSL */
#if !defined(HAVE_AMISSL)
static void radio_worker_breadcrumb(const char *stage, const char *op, unsigned long session)
{
    (void)stage;
    (void)op;
    (void)session;
}
static long radio_socket_library_open_count = 0;
static long radio_socket_library_close_count = 0;
static long radio_amissl_init_count = 0;
static long radio_amissl_cleanup_count = 0;
static long radio_amisslmaster_open_count = 0;
static long radio_amisslmaster_close_count = 0;
static long radio_openamissltags_count = 0;
static long radio_closeamissl_count = 0;
static const char * volatile radio_net_worker_stage = "not-available";
static const char * volatile radio_net_worker_last_op = "none";
static volatile unsigned long radio_net_worker_heartbeat = 0;
static int radio_net_worker_is_self(void) { return 1; }
int Radio_RunOnNetWorker(void (*fn)(void *arg), void *arg)
{
    if (!fn) return 0;
    fn(arg);
    return 1;
}
#endif
#else
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#define RADIO_SOCKET int
#define RADIO_INVALID_SOCKET (-1)
#define radio_close_socket(s) close(s)
int Radio_AmiSslLock(void) { return 0; }
void Radio_AmiSslUnlock(void) { }
static int radio_net_worker_is_self(void) { return 1; }
int Radio_RunOnNetWorker(void (*fn)(void *arg), void *arg)
{
    if (!fn) return 0;
    fn(arg);
    return 1;
}
#endif

/* Non-blocking ioctl request and "no data yet" errno values.  Define local
 * fallbacks rather than pulling in <sys/ioctl.h>/<sys/errno.h>, whose presence
 * varies across the m68k netinclude (and to avoid the header churn that broke
 * an earlier attempt).  0x8004667E is the standard BSD/bsdsocket FIONBIO. */

static void radio_format_ipv4_be(unsigned long addr_be, char *out, int out_size)
{
    unsigned char *b;

    if (!out || out_size <= 0) return;
    b = (unsigned char *)&addr_be;
    sprintf(out, "%u.%u.%u.%u",
            (unsigned int)b[0],
            (unsigned int)b[1],
            (unsigned int)b[2],
            (unsigned int)b[3]);
}

#ifndef FIONBIO
#define FIONBIO 0x8004667EUL
#endif
#ifndef EWOULDBLOCK
#define EWOULDBLOCK 35
#endif
#ifndef EAGAIN
#define EAGAIN 35
#endif
#ifndef EINPROGRESS
#define EINPROGRESS 36
#endif
#ifndef EISCONN
#define EISCONN 56
#endif

typedef enum {
    RADIO_PARSE_HEADER,
    RADIO_PARSE_AUDIO,
    RADIO_PARSE_META_LEN,
    RADIO_PARSE_META_PAYLOAD
} RadioParseState;

/* Transport close mode: GRACEFUL attempts SSL_shutdown() (only meaningful for
 * a clean stop of a healthy, still-connected TLS session); ABORT always skips
 * it.  Calling SSL_shutdown() on a session whose peer/socket is already in an
 * error, HTTP-error, or closed state is what produces the recoverable AmiSSL
 * alerts this mode split exists to avoid -- every failure/timeout/error path
 * must use ABORT. */
typedef enum {
    RADIO_CLOSE_GRACEFUL = 0,
    RADIO_CLOSE_ABORT = 1
} RadioCloseMode;

static void close_current_socket_mode_local(RadioStream *rs, RadioCloseMode mode);

typedef enum {
    STREAM_ALLOCATED = 1u << 0,
    TRANSPORT_CONNECTED = 1u << 1,
    TLS_DONE = 1u << 2,
    HEADER_DONE = 1u << 3,
    DECODER_STARTED = 1u << 4,
    PLAYBACK_STARTED = 1u << 5
} RadioStreamStateFlag;
static const char *radio_close_mode_name(RadioCloseMode mode) { return mode == RADIO_CLOSE_GRACEFUL ? "graceful" : "abort"; }

#define RADIO_STREAM_MAGIC 0x52535452UL

struct RadioStream {
    unsigned long magic;
    RADIO_SOCKET sock;
    RadioStatus status;
    /* Sized to match the probe's RB_PROBE_MAX_* (512/256/512): the old
     * url[256]/host[128]/path[192] silently truncated long tokenized stream
     * paths -- f121.rndfnk.com's 194-char path lost the end of its auth
     * token, the server answered the mangled request with data AmiSSL's
     * record parser could not survive, and playback died with "invalid
     * record" while the (512-byte) probe of the same URL worked fine. */
    char url[512], host[256], path[512];
    int port, bitrate, metaint, audioUntilMeta, headerDone;
    unsigned int streamStateFlags;
    int decoderStarted, playbackStarted, audioInitialized;
    char contentType[64], title[128], stationName[128], genre[64], streamUrl[128], error[128];
    unsigned char *ring;
    unsigned char *ringAlloc;
    unsigned long ringLastWrite;
    unsigned long rpos, wpos, used, size;
    char header[RADIO_HEADER_MAX];
    int headerLen;
    RadioParseState parseState;
    unsigned char meta[RADIO_META_MAX];
    int metaLen, metaGot, metaLeft;
    int reconnectAttempts, reconnectDelay;
    int zeroBytePumps;
    int startPumps;
    int stallPumps;   /* consecutive would-block pumps since last data, mid-stream */
    int everPlayed;
    int firstDataLogged;
    int firstMetaLogged;
    int stopping;
    struct in_addr hostAddr;   /* cached DNS result so reconnects skip gethostbyname() */
    int haveHostAddr;
    int isSSL;
    unsigned long session_id;
    clock_t lastMemReportClock;
    unsigned int cleanup_count, stop_request_count, task_exit_count;
    unsigned int ssl_free_count, ssl_ctx_free_count, socket_close_count, decoder_free_count;
    unsigned int stream_buffer_free_count, audio_buffer_free_count, amissl_cleanup_count;
    unsigned int sslFreed, ctxFreed, socketClosed, cleanupDone;
    unsigned long generation;
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    /* Which task called Radio_OpenWithHostAddr()/owns this session's
     * Radio_Pump()/Radio_ReadAudio() calls -- the single net worker task
     * peeks at this task's own pending SIGBREAKF_CTRL_C (radio_is_stopping())
     * so a GUI front end that signals this task directly (as both front
     * ends already do to interrupt playback) still aborts a connect/
     * handshake/read the worker is running on this session's behalf. */
    struct Task *requestingTask;
    SSL *ssl;
    SSL_CTX *ctx;
    int sslHandshakeDone;
    int sslStatePoisoned; /* see radio_ssl_read()'s SSL_ERROR_SSL handling */
    int tlsFaultCounted;  /* this session already counted toward the fault fuse */
    int lastSslError;     /* last SSL_get_error() value that set sslStatePoisoned, for pre-SSL_free diagnostics */
    char lastSslOp[24];   /* reason slug ("ssl-read-syscall", etc.) that set sslStatePoisoned */
    /* SSL_ERROR_ZERO_RETURN (a clean TLS close_notify) is NOT a fatal fault --
     * sslStatePoisoned/HTTPS-wide quarantine must not fire for it -- but
     * radio_stream_probe.c's rb_probe_ssl_read_retrying() found AmiSSL can
     * still leave SSL_free() unsafe after this specific "clean" close.
     * Mirror its narrowly-scoped sslReadCloseSeen flag here: skip SSL_free
     * for *this object only*, with no task-wide poisoning and no HTTPS
     * disablement. */
    int sslReadCloseSeen;
    struct SignalSemaphore workerLock;
    struct RadioStream *workerNext;
    volatile int workerRegistered;
    volatile int workerStopRequested;
    volatile int workerClosing;
    volatile int workerClosedAck;
    volatile int workerDetached;
    volatile int workerAbandoned;
    volatile int workerPumpInProgress;
    volatile int workerCloseRequested;
    volatile unsigned long workerPumpGeneration;
#endif
    /* Set once a read/write/connect fault is classified fatal (see
     * radio_ssl_error_is_fatal()): this session is done, permanently. No
     * further pumping, no reconnect, no AAC-timeout wait -- Radio_Pump() and
     * reconnect_http() both refuse to do anything once these are set. */
	int fatalStop;
	int noReconnect;
	/* Set when this session failed only because the peer dropped the
	 * connection (SSL_ERROR_SYSCALL with an empty error queue) -- a network
	 * event, not AmiSSL corruption. The SSL object is still leaked at cleanup
	 * (freeing one that took an I/O fault is what caused AN_BadFreeAddr), but
	 * the shared worker SSL_CTX is NOT poisoned, so HTTPS stays usable. */
	int sslDroppedTransport;
	unsigned long workerReadCalls;
	unsigned long workerReadBytes;
	unsigned long workerWantReadCount;
	unsigned long workerPumpZeroCount;
	unsigned long workerBackpressureCount;
	unsigned long workerPartialConsumeCount;
	unsigned long workerDroppedInputPreventedCount;
	unsigned long workerLastStatsClock;
	/* radio_ssl_close_stream_mode()/close_current_socket() can be re-entered
     * (abort path, then Radio_Close()'s own call); once a session has fully
     * closed its socket and freed/quarantined SSL, repeat calls must be a
     * cheap no-op instead of re-running (and re-logging) the whole sequence. */
    int closeCleanupDone;
};

static unsigned long radio_next_session_id = 1;
static unsigned long radio_current_generation = 0;
static long radio_active_stream_sessions = 0;
static long radio_active_stream_tasks = 0;
static long radio_open_socket_count = 0;
static long radio_playback_open_socket_count = 0;
static long radio_active_ssl_count = 0;
static long radio_active_ssl_ctx_count = 0;
static long radio_active_decoder_count = 0;
static long radio_active_audio_buffer_count = 0;
static long radio_active_stream_buffer_count = 0;
static long radio_active_icy_metadata_count = 0;
static long radio_gui_listbrowser_node_count = 0;
static long radio_gui_string_count = 0;
static int radio_atexit_registered = 0;
static int radioMemoryPoisoned = 0;
int radioAmiSslPoisoned = 0;
static unsigned long radio_poison_session_id = 0;
static char radio_poison_url[256];
/* First (root-cause) reason AmiSSL was marked poisoned this run; later
 * Radio_MarkTlsPoisoned() calls are consequences of the first and do not
 * overwrite it. */
static char radio_tls_poison_reason[96];

/* Radio_NetworkShutdown() must be idempotent: the app-close path has a
 * fallback call site, and running the AmiSSL/bsdsocket teardown twice is
 * exactly the kind of double-cleanup this file's counters exist to catch. */
static int radio_network_shutdown_started = 0;

static void radio_copy_string(char *dst, size_t dstSize, const char *src);

static void radio_debug_mem_report(unsigned long session_id, const char *where)
{
#if defined(AMIGA_M68K)
    RADIO_DBG(printf("radio-mem: session=%lu %s AvailMem(any)=%lu fast=%lu chip=%lu\n",
        session_id, where ? where : "", (unsigned long)AvailMem(MEMF_ANY),
        (unsigned long)AvailMem(MEMF_FAST), (unsigned long)AvailMem(MEMF_CHIP)));
#else
    RADIO_DBG(printf("radio-mem: session=%lu %s AvailMem(any)=n/a fast=n/a chip=n/a\n",
        session_id, where ? where : ""));
#endif
}

/* Debug-only escape hatch (MP3_PANIC_EXIT_ON_MEM_CORRUPT=1 in the
 * environment): on detected corruption, skip every further cleanup attempt
 * -- tidy or otherwise -- and terminate this task/process immediately. Any
 * further tidy cleanup (freeing the ring, the RadioStream, the decoder,
 * disposing GUI objects, closing shared libraries) is itself a write through
 * the same allocator/library state that triggered this, so for soak-testing
 * whether cleanup-after-corruption is what turns a detected fault into a
 * recoverable alert, this mode removes cleanup from the equation entirely. */
static int radio_panic_exit_on_mem_corrupt(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *v = getenv("MP3_PANIC_EXIT_ON_MEM_CORRUPT");
        cached = (v && *v && *v != '0') ? 1 : 0;
    }
    return cached;
}

void Radio_MarkMemoryPoisoned(const char *where)
{
    radioMemoryPoisoned = 1;
    printf("radio-memory: Memory corruption detected - restart app (where=%s session=%lu url=\"%s\")\n",
        where ? where : "", radio_poison_session_id,
        radio_poison_url[0] ? radio_poison_url : "");
    /* A corrupt heap must also hard-quarantine AmiSSL (audit rule 9): TLS
     * poison blocks every later HTTPS probe/playback this run, makes each
     * task's cleanup skip CleanupAmiSSL(), and makes Radio_NetworkShutdown()
     * abandon the instance instead of walking CleanupAmiSSL()/CloseAmiSSL()/
     * CloseLibrary() through damaged memory. Previously only the ring-canary
     * path set this, so a MiniMem report without a TLS fault still cleaned
     * AmiSSL up on a known-corrupt heap. */
    Radio_MarkTlsPoisoned(where && where[0] ? where : "MiniMem heap corruption");
    if (radio_panic_exit_on_mem_corrupt()) {
        printf("radio-memory: MP3_PANIC_EXIT_ON_MEM_CORRUPT active -- exiting immediately without cleanup (where=%s)\n",
            where ? where : "");
        fflush(stdout);
        exit(20);
    }
}

int Radio_IsMemoryPoisoned(void)
{
    return radioMemoryPoisoned;
}

const char *Radio_TlsPoisonedMessage(void)
{
    /* Only detected memory corruption reaches this state now (TLS faults
     * quarantine and continue). Reboot, not just app restart: once poisoned
     * cleanup has been skipped, the corrupted amissl.library stays resident
     * with a nonzero open count (AmigaOS never reclaims library opens from
     * an exited process), so a relaunch gets handed the same broken
     * library. */
    return "HTTPS disabled after TLS/memory poison; restart the app before using HTTPS.";
}

/* Fatal TLS faults quarantine the faulting connection SSL and now poison the
 * worker-owned shared SSL_CTX for the rest of this application run. The
 * context itself is deliberately kept until worker shutdown (or leaked if
 * poisoned), and no later HTTPS connection may allocate a new SSL from it. */
static long radio_tls_fault_count = 0;
/* Diagnostic skip/quarantine marker.  This alone is not enough to abandon
 * the long-lived worker during Radio_NetworkShutdown(): MP3_SKIP_ABORT_SSL_FREE
 * intentionally sets it for clean isolation runs with no TLS fault, no memory
 * poison and no active SSL/socket objects.  Final shutdown is abandoned only
 * for a real fault/poison condition (see Radio_NetworkShutdown()). */
static int radio_tls_shutdown_quarantine = 0;

void Radio_SetTlsFaultContext(unsigned long session_id, const char *url)
{
    radio_poison_session_id = session_id;
    radio_copy_string(radio_poison_url, sizeof(radio_poison_url), url ? url : "");
}

void Radio_ReportTlsFault(const char *where)
{
    radio_tls_fault_count++;
    radio_tls_shutdown_quarantine = 1;
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    radio_worker_ssl_ctx_poisoned = 1;
#endif
    Radio_MarkTlsPoisoned(where && where[0] ? where : "fatal TLS fault");
    RADIO_DBG(printf("radio-tls: TLS fault %ld where=%s session=%lu url=\"%s\" -- objects quarantined, shared SSL_CTX poisoned, HTTPS disabled for this run\n",
        radio_tls_fault_count, where ? where : "",
        radio_poison_session_id, radio_poison_url[0] ? radio_poison_url : ""));
    (void)where;
}

void Radio_MarkTlsPoisoned(const char *where)
{
    if (!radioAmiSslPoisoned) {
        printf("%s\n", Radio_TlsPoisonedMessage());
        /* Release-visible FIRST-poison reason. This fires exactly once (guarded
         * by !radioAmiSslPoisoned) and outside any connect/handshake retry loop,
         * so it exposes what actually triggered the poison without the
         * per-iteration debug prints that would change handshake timing. The
         * message text alone is ambiguous -- a fatal TLS fault
         * (Radio_ReportTlsFault) and a memory-corruption cascade
         * (Radio_MarkMemoryPoisoned -> here) print the same line -- so log the
         * where/session/url and whether the heap was already flagged, which
         * distinguishes the two: memoryPoisoned=1 means a MiniMem canary tripped
         * first, memoryPoisoned=0 means a fatal SSL result poisoned directly. */
        printf("radio-tls-poison: FIRST where=\"%s\" session=%lu url=\"%s\" memoryPoisoned=%d\n",
            where ? where : "", radio_poison_session_id,
            radio_poison_url[0] ? radio_poison_url : "", radioMemoryPoisoned ? 1 : 0);
        if (where && where[0]) {
            strncpy(radio_tls_poison_reason, where, sizeof(radio_tls_poison_reason) - 1);
            radio_tls_poison_reason[sizeof(radio_tls_poison_reason) - 1] = '\0';
        }
    }
    radioAmiSslPoisoned = 1;
    radio_tls_shutdown_quarantine = 1;
    RADIO_DBG(printf("radio-tls: AmiSSL poisoned where=%s session=%lu url=\"%s\"\n",
        where ? where : "", radio_poison_session_id,
        radio_poison_url[0] ? radio_poison_url : ""));
}

int Radio_IsTlsPoisoned(void)
{
    return radioAmiSslPoisoned;
}

const char *Radio_TlsPoisonReason(void)
{
    return radio_tls_poison_reason[0] ? radio_tls_poison_reason : "not-poisoned";
}

#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
static SSL_CTX *radio_worker_get_ssl_ctx(const char *category, unsigned long session_id)
{
    const SSL_METHOD *method;

    if (!radio_net_worker_is_self()) {
        RADIO_DBG(printf("radio-resource: shared SSL_CTX refused off worker category=%s session=%lu\n",
            category ? category : "unknown", session_id););
        return NULL;
    }
    if (radio_worker_ssl_ctx_poisoned || Radio_IsMemoryPoisoned() || Radio_IsTlsPoisoned()) {
        RADIO_DBG(printf("radio-resource: shared SSL_CTX refused poisoned category=%s session=%lu ctx=%p\n",
            category ? category : "unknown", session_id, (void *)radio_worker_ssl_ctx););
        return NULL;
    }
    if (radio_worker_ssl_ctx) {
        RADIO_DBG(printf("radio-resource: shared SSL_CTX reused category=%s session=%lu ctx=%p\n",
            category ? category : "unknown", session_id, (void *)radio_worker_ssl_ctx););
        return radio_worker_ssl_ctx;
    }
    method = SSLv23_client_method();
    if (!method)
        return NULL;
    Radio_DebugCheckExecMem("before shared SSL_CTX_new");
    if (Radio_IsMemoryPoisoned() || Radio_IsTlsPoisoned())
        return NULL;
    radio_amissl_lifecycle_diag("SSL_CTX_new-before", session_id, NULL, NULL);
    radio_worker_ssl_ctx = SSL_CTX_new(method);
    radio_amissl_lifecycle_diag("SSL_CTX_new-after", session_id, NULL, radio_worker_ssl_ctx);
    Radio_DebugCheckExecMem("after shared SSL_CTX_new");
    if (Radio_IsMemoryPoisoned() || Radio_IsTlsPoisoned()) {
        radio_worker_ssl_ctx_poisoned = 1;
        Radio_MarkTlsPoisoned("shared SSL_CTX_new post-check poison");
        return NULL;
    }
    if (!radio_worker_ssl_ctx)
        return NULL;
    radio_active_ssl_ctx_count++;
    RADIO_DBG(printf("radio-resource: shared SSL_CTX created ctx=%p\n",
        (void *)radio_worker_ssl_ctx););
    SSL_CTX_set_verify(radio_worker_ssl_ctx, SSL_VERIFY_NONE, NULL);
#ifdef SSL_OP_IGNORE_UNEXPECTED_EOF
    SSL_CTX_set_options(radio_worker_ssl_ctx, SSL_OP_IGNORE_UNEXPECTED_EOF);
#endif
    RADIO_DBG(printf("radio-resource: shared SSL_CTX reused category=%s session=%lu ctx=%p\n",
        category ? category : "unknown", session_id, (void *)radio_worker_ssl_ctx););
    return radio_worker_ssl_ctx;
}

void *Radio_GetWorkerSslCtx(const char *category, unsigned long session_id)
{
    return (void *)radio_worker_get_ssl_ctx(category, session_id);
}

void Radio_MarkWorkerSslCtxPoisoned(const char *where)
{
    radio_worker_ssl_ctx_poisoned = 1;
    Radio_MarkTlsPoisoned(where && where[0] ? where : "shared SSL_CTX poisoned");
}

static int radio_worker_shutdown_ssl_ctx(void)
{
    if (Radio_IsMemoryPoisoned())
        return 0;
    if (!radio_worker_ssl_ctx)
        return Radio_IsTlsPoisoned() ? 0 : 1;
    if (radio_active_ssl_count != 0) {
        radio_worker_ssl_ctx_poisoned = 1;
        RADIO_DBG(printf("radio-resource: shared SSL_CTX quarantined ctx=%p active_ssl_count=%ld\n",
            (void *)radio_worker_ssl_ctx, radio_active_ssl_count););
    }
    if (radio_worker_ssl_ctx_poisoned || Radio_IsMemoryPoisoned() || Radio_IsTlsPoisoned()) {
        RADIO_DBG(printf("radio-resource: shared SSL_CTX quarantined ctx=%p\n",
            (void *)radio_worker_ssl_ctx););
        radio_worker_ssl_ctx = NULL;
        if (radio_active_ssl_ctx_count > 0)
            radio_active_ssl_ctx_count--;
        return 0;
    }
    Radio_DebugCheckExecMem("before shared SSL_CTX_free");
    if (Radio_IsMemoryPoisoned() || Radio_IsTlsPoisoned()) {
        radio_worker_ssl_ctx_poisoned = 1;
        RADIO_DBG(printf("radio-resource: shared SSL_CTX quarantined ctx=%p\n",
            (void *)radio_worker_ssl_ctx););
        radio_worker_ssl_ctx = NULL;
        if (radio_active_ssl_ctx_count > 0)
            radio_active_ssl_ctx_count--;
        return 0;
    }
    radio_amissl_lifecycle_diag("SSL_CTX_free-before", 0, NULL, radio_worker_ssl_ctx);
    SSL_CTX_free(radio_worker_ssl_ctx);
    radio_amissl_lifecycle_diag("SSL_CTX_free-after", 0, NULL, radio_worker_ssl_ctx);
    Radio_DebugCheckExecMem("after shared SSL_CTX_free");
    if (Radio_IsMemoryPoisoned() || Radio_IsTlsPoisoned()) {
        radio_worker_ssl_ctx_poisoned = 1;
        RADIO_DBG(printf("radio-resource: shared SSL_CTX freed but post-free heap/TLS poison detected ctx=%p\n",
            (void *)radio_worker_ssl_ctx););
        radio_worker_ssl_ctx = NULL;
        if (radio_active_ssl_ctx_count > 0)
            radio_active_ssl_ctx_count--;
        return 0;
    }
    RADIO_DBG(printf("radio-resource: shared SSL_CTX freed at worker shutdown ctx=%p\n",
        (void *)radio_worker_ssl_ctx););
    radio_worker_ssl_ctx = NULL;
    if (radio_active_ssl_ctx_count > 0)
        radio_active_ssl_ctx_count--;
    return 1;
}
#endif

int Radio_CheckMiniMem(const char *where)
{
    int corrupt = MiniMem_CheckAll(where);
    if (corrupt > 0)
        Radio_MarkMemoryPoisoned(where);
    return corrupt;
}

/* Proactive exec-heap validation (debug builds): walks every MemHeader's
 * free-chunk list under Forbid() checking the same invariants exec's own
 * FreeMem() trips AN_MemCorrupt (81000005) on -- chunk inside its header's
 * bounds, aligned, sanely sized, strictly ascending, free-byte total
 * matching mh_Free. A TLS-clean run still died with AN_MemCorrupt inside a
 * healthy SSL_free() after three perfect play/stop cycles, so something
 * corrupts exec memory silently during NORMAL operation and is only
 * discovered ages later; running this at every session boundary brackets
 * the corruptor to one interval in the log instead. Read-only, so it is
 * safe to call anywhere; costs one list walk (a few hundred nodes). */
void Radio_DebugCheckExecMem(const char *where)
{
#if defined(AMIGA_M68K) && defined(RADIO_DEBUG)
    struct MemHeader *mh;
    long headers = 0;
    long chunks = 0;
    unsigned long total_free = 0;
    const char *fault = NULL;
    void *fault_addr = NULL;
    Forbid();
    for (mh = (struct MemHeader *)((struct ExecBase *)SysBase)->MemList.lh_Head;
         mh->mh_Node.ln_Succ && !fault;
         mh = (struct MemHeader *)mh->mh_Node.ln_Succ) {
        struct MemChunk *mc = mh->mh_First;
        unsigned long header_free = 0;
        long guard = 0;
        headers++;
        while (mc) {
            if ((void *)mc < mh->mh_Lower || (void *)mc >= mh->mh_Upper) { fault = "chunk outside header"; fault_addr = (void *)mc; break; }
            if (((unsigned long)mc) & 3UL) { fault = "misaligned chunk"; fault_addr = (void *)mc; break; }
            if (mc->mc_Bytes == 0 || (unsigned long)mc + mc->mc_Bytes > (unsigned long)mh->mh_Upper) { fault = "bad chunk size"; fault_addr = (void *)mc; break; }
            if (mc->mc_Next && mc->mc_Next <= mc) { fault = "non-ascending chunk"; fault_addr = (void *)mc; break; }
            header_free += mc->mc_Bytes;
            chunks++;
            if (++guard > 65536) { fault = "chunk list loop"; fault_addr = (void *)mc; break; }
            mc = mc->mc_Next;
        }
        if (!fault && header_free != mh->mh_Free) { fault = "mh_Free mismatch"; fault_addr = (void *)mh; }
        total_free += header_free;
    }
    Permit();
    if (fault) {
        printf("radio-memcheck: CORRUPT %s at %p where=%s (exec heap damaged BEFORE this point)\n",
            fault, fault_addr, where ? where : "");
        fflush(stdout);
        /* This walk previously only printed: a real exec-heap corruption hit
         * in production here (a "bad chunk size" fault) and the app kept
         * probing/reconnecting/starting new playback children on a heap
         * already known to be damaged. Radio_MarkMemoryPoisoned() sets the
         * same radioMemoryPoisoned flag every probe entry point
         * (rb_probe_stream_url_impl/rb_probe_fetch_binary_impl) and playback
         * entry point (RadioDoProbeAndPlay/StartPlayback in minimp3r.c)
         * already checks before doing any DNS/socket/SSL/CreateNewProc work
         * -- it also hard-poisons TLS (Radio_MarkTlsPoisoned(), called
         * internally) so no further SSL_new() runs either. */
        Radio_MarkMemoryPoisoned(where);
    } else {
        printf("radio-memcheck: OK where=%s headers=%ld chunks=%ld free=%lu\n",
            where ? where : "", headers, chunks, total_free);
        fflush(stdout);
    }
#else
    (void)where;
#endif
}

static int radio_stream_magic_valid(const RadioStream *rs, const char *where)
{
    int ok = rs && rs->magic == RADIO_STREAM_MAGIC;
    if (!ok) RADIO_DBG(printf("radio-guard: BAD RadioStream magic where=%s rs=%p magic=%08lx expected=%08lx\n", where ? where : "", (void *)rs, rs ? rs->magic : 0UL, RADIO_STREAM_MAGIC););
    return ok;
}

static void radio_resource_summary(const RadioStream *rs, const char *where)
{
    RADIO_DBG(printf("radio-summary: session=%lu %s active_stream_sessions=%ld active_stream_tasks=%ld open_socket_count=%ld playback_open_socket_count=%ld active_ssl_count=%ld active_ssl_ctx_count=%ld active_decoder_count=%ld active_audio_buffer_count=%ld active_stream_buffer_count=%ld active_icy_metadata_count=%ld active_gui_nodes=%ld active_gui_strings=%ld amissl_init_count=%ld cleanup_count=%u\n",
        rs ? rs->session_id : 0, where ? where : "", radio_active_stream_sessions,
        radio_active_stream_tasks, radio_open_socket_count, radio_playback_open_socket_count, radio_active_ssl_count,
        radio_active_ssl_ctx_count, radio_active_decoder_count,
        radio_active_audio_buffer_count, radio_active_stream_buffer_count,
        radio_active_icy_metadata_count, radio_gui_listbrowser_node_count,
        radio_gui_string_count, radio_amissl_init_count, rs ? rs->cleanup_count : 0));
}
static void radio_app_exit_report(void)
{
    radio_debug_mem_report(0, "before app close");
    radio_resource_summary(NULL, "before app close");
}

/* App-side stop flag (Radio_SetStopFlag).  Polled by radio_is_stopping() so
 * the pump/connect/read-audio wait loops notice a Stop click even while
 * stalled on a dead socket that never delivers another byte -- the only
 * other stop propagation path (Radio_RequestStop() from InputSourceClose()/
 * InputSourceRead()) runs between Radio_ReadAudio() calls, which a stalled
 * buffering loop never returns to.
 *
 * Deliberately a pointer to a data flag, NOT a callback: polling means only
 * data reads through the pointer, and a data read cannot raise the
 * odd-address instruction-fetch fault (guru 80000003) that calling through
 * a corrupted function pointer can.  Both GUI front-ends set their shared
 * interrupt flag before signalling the playback child, so reading the flag
 * is sufficient; the pending-CTRL_C case is handled below with a direct,
 * statically linked SetSignal() call instead of app code. */
static const volatile int *radio_external_stop_flag = 0;

void Radio_SetStopFlag(const volatile int *flag) { radio_external_stop_flag = flag; }

static int radio_is_stopping(const RadioStream *rs)
{
    if (!rs || rs->stopping || rs->status == RADIO_STATUS_STOPPING || rs->status == RADIO_STATUS_CLOSED)
        return 1;
    if (rs->generation != radio_current_generation)
        return 1;
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    if (rs->workerStopRequested || rs->workerClosing || rs->workerCloseRequested || rs->workerDetached)
        return 1;
#endif
    if (radio_external_stop_flag && *radio_external_stop_flag)
        return 1;
#if defined(AMIGA_M68K)
    /* A pending break on the pumping task (the GUIs signal their playback
     * child with SIGBREAKF_CTRL_C; a CLI run gets it from the shell).
     * SetSignal(0,0) only samples -- the signal stays pending for the
     * existing break checks in the decoder loops to latch as before. */
    if (SetSignal(0, 0) & SIGBREAKF_CTRL_C)
        return 1;
#if defined(HAVE_AMISSL)
    /* This connect/handshake/read loop may actually be running on the net
     * worker task (see Radio_RunOnNetWorker()), a different task from the
     * one both GUI front ends signal directly to interrupt playback
     * (SignalPlaybackChildCtrlC() et al target the playback child by name/
     * Task pointer, unaware the worker task even exists). Peek at that
     * requesting task's own pending SIGBREAKF_CTRL_C under Forbid() -- the
     * same safe, well-known technique those front ends already use to find
     * the task in the first place -- so a Stop click still aborts a
     * connect/handshake/read the worker is running on this session's
     * behalf. */
    if (rs->requestingTask && rs->requestingTask != (struct Task *)FindTask(NULL)) {
        ULONG pending;
        Forbid();
        pending = rs->requestingTask->tc_SigRecvd;
        Permit();
        if (pending & SIGBREAKF_CTRL_C)
            return 1;
    }
#endif
#endif
    return 0;
}
static void close_current_socket(RadioStream *rs);
static int radio_contains_nocase(const char *s, const char *needle)
{
    int n, i;
    if (!s || !needle) return 0;
    n = (int)strlen(needle);
    if (n <= 0) return 1;
    for (i = 0; s[i]; i++) {
        int j;
        for (j = 0; j < n && s[i + j] && tolower((unsigned char)s[i + j]) == tolower((unsigned char)needle[j]); j++) ;
        if (j == n) return 1;
    }
    return 0;
}
static int radio_url_looks_hls(const char *url) { return radio_contains_nocase(url, ".m3u8"); }
static void radio_duplicate_cleanup_warning(RadioStream *rs, const char *what, unsigned int count)
{
    if (rs && count > 1)
        RADIO_DBG(printf("radio-cleanup warning: session=%lu duplicate %s count=%u; skipping duplicate operation\n", rs->session_id, what, count););
}
static void radio_reset_session_state(RadioStream *rs)
{
    if (!rs) return;
    rs->sock = RADIO_INVALID_SOCKET;
    rs->isSSL = 0;
    rs->lastMemReportClock = 0;
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    rs->ssl = NULL;
    rs->ctx = NULL;
    rs->sslHandshakeDone = 0;
    rs->tlsFaultCounted = 0;
#endif
    rs->bitrate = rs->metaint = rs->audioUntilMeta = rs->headerDone = 0;
    rs->streamStateFlags = 0;
    rs->decoderStarted = rs->playbackStarted = rs->audioInitialized = 0;
    rs->contentType[0] = rs->title[0] = rs->stationName[0] = rs->genre[0] = rs->streamUrl[0] = rs->error[0] = 0;
    rs->rpos = rs->wpos = rs->used = 0;
    rs->headerLen = 0; rs->header[0] = 0;
    rs->parseState = RADIO_PARSE_HEADER;
    rs->metaLen = rs->metaGot = rs->metaLeft = 0;
    rs->reconnectAttempts = rs->reconnectDelay = rs->zeroBytePumps = rs->startPumps = 0;
    rs->stallPumps = 0;
	rs->everPlayed = rs->firstDataLogged = rs->haveHostAddr = 0;
	rs->workerReadCalls = rs->workerReadBytes = rs->workerWantReadCount = 0;
	rs->workerPumpZeroCount = rs->workerBackpressureCount = 0;
	rs->workerPartialConsumeCount = rs->workerDroppedInputPreventedCount = 0;
	rs->workerLastStatsClock = 0;
	rs->sslFreed = rs->ctxFreed = rs->socketClosed = rs->cleanupDone = 0;
    rs->closeCleanupDone = 0;
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    InitSemaphore(&rs->workerLock);
    rs->workerNext = NULL;
    rs->workerRegistered = 0;
    rs->workerStopRequested = 0;
    rs->workerClosing = 0;
    rs->workerClosedAck = 0;
    rs->workerDetached = 0;
    rs->workerAbandoned = 0;
    rs->workerPumpInProgress = 0;
    rs->workerCloseRequested = 0;
    rs->workerPumpGeneration = 0;
#endif
}


#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
static void radio_stream_lock(RadioStream *rs)
{
    if (rs) ObtainSemaphore(&rs->workerLock);
}

static void radio_stream_unlock(RadioStream *rs)
{
    if (rs) ReleaseSemaphore(&rs->workerLock);
}
#else
static void radio_stream_lock(RadioStream *rs) { (void)rs; }
static void radio_stream_unlock(RadioStream *rs) { (void)rs; }
#endif

#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
static void radio_worker_risk_log(const char *stage, RadioStream *rs)
{
    unsigned long used = 0;
    unsigned long freeBytes = 0;
    RadioStatus status = RADIO_STATUS_CLOSED;
    long fd = RADIO_INVALID_SOCKET;
    SSL *ssl = NULL;
    SSL_CTX *ctx = NULL;

    if (!rs) {
        radio_worker_breadcrumb(stage, "risk", 0);
        RADIO_DBG(printf("radio-worker-risk: %s session=0 workerTask=%p\n",
            stage ? stage : "<null>", (void *)radio_net_worker_task););
        return;
    }
    radio_stream_lock(rs);
    used = rs->used;
    freeBytes = rs->size > rs->used ? rs->size - rs->used : 0;
    status = rs->status;
    fd = rs->sock;
    ssl = rs->ssl;
    ctx = rs->ctx;
    radio_stream_unlock(rs);
    radio_worker_breadcrumb(stage, "risk", rs->session_id);
    RADIO_DBG(printf("radio-worker-risk: %s session=%lu workerTask=%p status=%d used=%lu free=%lu ssl=%p ctx=%p fd=%ld stop=%d closing=%d closeReq=%d registered=%d detached=%d pumpInProgress=%d\n",
        stage ? stage : "<null>", rs->session_id, (void *)radio_net_worker_task,
        (int)status, used, freeBytes, (void *)ssl, (void *)ctx, fd,
        rs->workerStopRequested, rs->workerClosing, rs->workerCloseRequested,
        rs->workerRegistered, rs->workerDetached, rs->workerPumpInProgress););
}
#else
static void radio_worker_risk_log(const char *stage, RadioStream *rs) { (void)stage; (void)rs; }
#endif

#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
static void radio_worker_register_stream(RadioStream *rs)
{
    if (!rs || rs->workerRegistered) return;
    rs->workerNext = radio_net_worker_streams;
    radio_net_worker_streams = rs;
    rs->workerRegistered = 1;
    RADIO_DBG(printf("radio-net-worker: registered pump stream session=%lu\n", rs->session_id););
}

static void radio_worker_unregister_stream(RadioStream *rs)
{
    RadioStream **pp;
    if (!rs || !rs->workerRegistered) return;
    radio_worker_breadcrumb("before stop/unregister", "unregister", rs->session_id);
    RADIO_DBG(printf("radio-worker: close requested session=%lu pumpInProgress=%d\n",
        rs->session_id, rs->workerPumpInProgress););
    radio_stream_lock(rs);
    rs->workerCloseRequested = 1;
    radio_stream_unlock(rs);
    while (rs->workerPumpInProgress) {
        RADIO_DBG(printf("radio-worker: waiting for pump leave before unregister session=%lu pumpInProgress=%d\n",
            rs->session_id, rs->workerPumpInProgress););
        Delay(1);
    }
    RADIO_DBG(printf("radio-worker: close proceeding session=%lu pumpInProgress=0\n", rs->session_id););
    pp = &radio_net_worker_streams;
    while (*pp) {
        if (*pp == rs) {
            *pp = rs->workerNext;
            radio_stream_lock(rs);
            rs->workerNext = NULL;
            rs->workerRegistered = 0;
            rs->workerClosing = 0;
            rs->workerClosedAck = 1;
            rs->workerDetached = 1;
            radio_stream_unlock(rs);
            if (!radio_net_worker_streams) radio_worker_state = RADIO_WORKER_IDLE;
            RADIO_DBG(printf("radio-net-worker: unregistered pump stream session=%lu\n", rs->session_id););
            radio_worker_breadcrumb("after stop/unregister", "unregister", rs->session_id);
            return;
        }
        pp = &(*pp)->workerNext;
    }
    radio_stream_lock(rs);
    rs->workerNext = NULL;
    rs->workerRegistered = 0;
    rs->workerClosing = 0;
    rs->workerClosedAck = 1;
    rs->workerDetached = 1;
    radio_stream_unlock(rs);
    if (!radio_net_worker_streams) radio_worker_state = RADIO_WORKER_IDLE;
    radio_worker_breadcrumb("after stop/unregister", "unregister", rs->session_id);
}

static void radio_worker_unregister_stream_job(void *arg)
{
    radio_worker_unregister_stream((RadioStream *)arg);
}

static void radio_worker_close_detach_stream_job(void *arg)
{
    RadioStream *rs = (RadioStream *)arg;
    if (!rs) return;
    radio_worker_state = RADIO_WORKER_CLOSING;
    radio_stream_lock(rs);
    rs->workerCloseRequested = 1;
    rs->workerClosing = 1;
    radio_stream_unlock(rs);
    RADIO_DBG(printf("radio-cleanup: active abort begin session=%lu\n", rs->session_id););
    RADIO_DBG(printf("radio-cleanup: active abort mark closing session=%lu\n", rs->session_id););
    RADIO_DBG(printf("radio-cleanup: active abort unregister start session=%lu\n", rs->session_id););
    radio_worker_unregister_stream(rs);
    RADIO_DBG(printf("radio-cleanup: active abort unregister end session=%lu\n", rs->session_id););
    close_current_socket_mode_local(rs, RADIO_CLOSE_ABORT);
    radio_stream_lock(rs);
    rs->workerClosing = 0;
    rs->workerClosedAck = 1;
    rs->workerDetached = 1;
    radio_stream_unlock(rs);
    if (!radio_net_worker_streams) radio_worker_state = RADIO_WORKER_IDLE;
    RADIO_DBG(printf("radio-cleanup: active abort complete session=%lu\n", rs->session_id););
}

static void radio_worker_pump_active_streams(void)
{
    RadioStream *rs;
    if (radio_net_worker_pump_active) return;
    radio_net_worker_pump_active = 1;
    radio_worker_breadcrumb("pump-active-start", "pump", 0);
    rs = radio_net_worker_streams;
    while (rs) {
        RadioStream *next = rs->workerNext;
        int skip = 0;
        radio_stream_lock(rs);
        skip = (rs->generation != radio_current_generation) ||
            rs->workerStopRequested || rs->workerClosing || rs->workerCloseRequested ||
            rs->workerDetached || rs->status == RADIO_STATUS_STOPPING ||
            rs->status == RADIO_STATUS_CLOSED || rs->status == RADIO_STATUS_ERROR;
        radio_stream_unlock(rs);
        if (skip) {
            RADIO_DBG(printf("radio-worker: pump skip stopping/closing session=%lu\n", rs->session_id););
            radio_worker_unregister_stream(rs);
        } else if (rs->used < rs->size) {
            int budget;
            for (budget = 0; budget < 8 && rs->used < rs->size; budget++) {
                radio_worker_breadcrumb("pump-session", "pump", rs->session_id);
                radio_stream_lock(rs);
                if (rs->generation != radio_current_generation ||
                    rs->workerStopRequested || rs->workerClosing || rs->workerCloseRequested ||
                    rs->workerDetached || rs->status == RADIO_STATUS_STOPPING ||
                    rs->status == RADIO_STATUS_CLOSED || rs->status == RADIO_STATUS_ERROR) {
                    radio_stream_unlock(rs);
                    RADIO_DBG(printf("radio-worker: pump skip before enter session=%lu\n", rs->session_id););
                    break;
                }
                rs->workerPumpInProgress = 1;
                rs->workerPumpGeneration = rs->generation;
                radio_stream_unlock(rs);
                RADIO_DBG(printf("radio-worker: pump enter session=%lu\n", rs->session_id););
                if (radio_pump_body(rs) <= 0)
                {
                    radio_stream_lock(rs);
                    rs->workerPumpInProgress = 0;
                    rs->workerPumpGeneration = 0;
                    radio_stream_unlock(rs);
                    RADIO_DBG(printf("radio-worker: pump leave session=%lu\n", rs->session_id););
                    break;
                }
                radio_stream_lock(rs);
                rs->workerPumpInProgress = 0;
                rs->workerPumpGeneration = 0;
                radio_stream_unlock(rs);
                RADIO_DBG(printf("radio-worker: pump leave session=%lu\n", rs->session_id););
                if (rs->status == RADIO_STATUS_ERROR || rs->status == RADIO_STATUS_CLOSED)
                    break;
            }
        }
        rs = next;
    }
    radio_net_worker_pump_active = 0;
}
#endif

/* Yield the CPU briefly during reconnect backoff.  reconnect_http() is the only
 * pump path that does not block on the socket, so without this the player
 * process spins at 100% CPU while the stream is down or re-buffering, starving
 * Workbench and the GUI's window redraws (the "desktop/mouse locks up while the
 * internet buffers" symptom). */
static void radio_backoff_sleep(void)
{
#if defined(AMIGA_M68K)
    Delay(2); /* ~40ms (2 ticks @ 50Hz) */
#else
    usleep(40000);
#endif
}

/* Put the stream socket into non-blocking mode.  WinUAE's built-in
 * bsdsocket.library runs a *blocking* socket call synchronously and freezes the
 * whole emulation (mouse included) until it returns - so a blocking recv() that
 * waits for the ring to refill freezes the machine for ~1s every time the
 * stream re-buffers.  With a non-blocking socket recv() returns immediately and
 * we yield with Delay() (a pure timer wait the emulator does not stall on). */
static void radio_set_nonblocking(RADIO_SOCKET s)
{
#if defined(AMIGA_M68K)
    long nb = 1;
    IoctlSocket(s, FIONBIO, (char *)&nb);
#else
    int fl = fcntl(s, F_GETFL, 0);
    if (fl >= 0)
        fcntl(s, F_SETFL, fl | O_NONBLOCK);
#endif
}

/* True when the last socket call failed only because no data is ready yet. */
static int radio_would_block(void)
{
#if defined(AMIGA_M68K)
    long e = Errno();
    return e == EWOULDBLOCK || e == EAGAIN;
#else
    return errno == EWOULDBLOCK || errno == EAGAIN;
#endif
}

static long radio_sock_errno(void)
{
#if defined(AMIGA_M68K)
    return Errno();
#else
    return errno;
#endif
}

static long radio_ioerr_value(void)
{
#if defined(AMIGA_M68K)
    return IoErr();
#else
    return 0;
#endif
}

static unsigned long radio_monotonic_ticks(void)
{
#if defined(AMIGA_M68K)
    struct DateStamp ds;
    DateStamp(&ds);
    return ((unsigned long)ds.ds_Days * 24UL * 60UL * 50UL) +
        ((unsigned long)ds.ds_Minute * 60UL * 50UL) +
        (unsigned long)ds.ds_Tick;
#else
    return (unsigned long)(clock() * 50UL / CLOCKS_PER_SEC);
#endif
}

static int radio_deadline_pending(unsigned long deadline_ticks)
{
    return (long)(deadline_ticks - radio_monotonic_ticks()) >= 0;
}

#if defined(HAVE_AMISSL)
static int radio_ssl_wait_ready(RADIO_SOCKET sock, int ssl_error)
{
    fd_set rfds;
    fd_set wfds;
    struct timeval tv;
    int nfds;

    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    if (ssl_error == SSL_ERROR_WANT_READ)
        FD_SET((int)sock, &rfds);
    else if (ssl_error == SSL_ERROR_WANT_WRITE)
        FD_SET((int)sock, &wfds);
    else
        return -1;

    tv.tv_sec = 0;
    tv.tv_usec = 160000;
    nfds = (int)sock + 1;
#if defined(AMIGA_M68K)
    return WaitSelect(nfds, &rfds, &wfds, NULL, &tv, NULL);
#else
    return select(nfds, &rfds, &wfds, NULL, &tv);
#endif
}

#endif /* HAVE_AMISSL: SO_ERROR is also used by plain HTTP transports. */

static int radio_socket_connect_error(RADIO_SOCKET sock, long *out_error)
{
    int so_error = 0;
    int r;
#if defined(AMIGA_M68K)
    LONG optlen = sizeof(so_error);
#else
    socklen_t optlen = sizeof(so_error);
#endif
    r = getsockopt(sock, SOL_SOCKET, SO_ERROR, (char *)&so_error, &optlen);
    if (r != 0) {
        if (out_error) *out_error = radio_sock_errno();
        return -1;
    }
    if (out_error) *out_error = so_error;
    return so_error == 0 ? 0 : -1;
}

static void radio_log_socket_failure(RadioStream *rs, const char *context, const char *where)
{
    RADIO_DBG(printf("radio-socket: %s socket failed session=%lu host=%s fd=-1 errno=%ld IoErr=%ld open_socket_count=%ld playback_open_socket_count=%ld active_stream_sessions=%ld active_stream_tasks=%ld active_ssl_count=%ld active_ssl_ctx_count=%ld current_state=%d previous_stream_state=n/a old_child_done_posted=n/a parent_done_received=n/a where=%s\n",
        context ? context : "playback", rs ? rs->session_id : 0, rs ? rs->host : "",
        radio_sock_errno(), radio_ioerr_value(), radio_open_socket_count,
        radio_playback_open_socket_count, radio_active_stream_sessions,
        radio_active_stream_tasks, radio_active_ssl_count, radio_active_ssl_ctx_count,
        rs ? (int)rs->status : -1, where ? where : ""));
}

/* Forward declaration so the SSL helpers can call set_error before it is
 * defined later in this translation unit. */
static void set_error(RadioStream *rs, const char *msg);

#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
/* radio_net_adopt_context()/RadioNetContext (the old "each playback child
 * opens its own bsdsocket.library base and swaps the shared SocketBase
 * global to point at it") are gone: the single net worker task above owns
 * SocketBase/AmiSSLBase/AmiSSLExtBase/AmiSSLMasterBase for the whole app
 * run and every socket()/connect()/SSL_*() call in this file now only ever
 * runs on that one task (via Radio_RunOnNetWorker()), so there is nothing
 * left to adopt or swap. Kept as a no-op purely so the many existing call
 * sites below (radio_wait_connected(), radio_send_all(), connect_http(),
 * radio_ssl_do_handshake(), radio_ssl_connect(), radio_ssl_close_stream_mode(),
 * radio_abort_current_socket(), close_current_socket_mode())
 * do not all need editing. */
static void radio_net_adopt_context(RadioStream *rs) { (void)rs; }

/* No more per-child InitAmiSSL()/CleanupAmiSSL(): the worker task ran
 * OpenAmiSSLTags()/InitAmiSSL() exactly once at start-up (see
 * radio_net_worker_entry() above) and stays initialised for the app's whole
 * run, across every station switch. connect_http()/radio_ssl_connect() just
 * need to confirm the worker's AmiSSL instance actually came up. */
static int radio_net_worker_amissl_ready(RadioStream *rs)
{
    if (Radio_IsTlsPoisoned()) {
        /* AmiSSL cleanup has already been skipped/leaked for this run --
         * never call back into it. */
        set_error(rs, Radio_TlsPoisonedMessage());
        return -1;
    }
    if (!AmiSSLBase || !SocketBase) {
        set_error(rs, "AmiSSL unavailable: network worker not running");
        return -1;
    }
    return 0;
}

/* Kept for API compatibility (radio_browser_http.c calls this to find out
 * whether it is safe to open its own independent bsdsocket.library base
 * right now).  With the single-worker architecture every subsystem opens
 * its own private bsdsocket base and none of them ever touch the worker's
 * SocketBase, so there is no more contention to report. */
static const char *radio_worker_state_name(RadioWorkerState state)
{
    switch (state) {
        case RADIO_WORKER_IDLE: return "idle";
        case RADIO_WORKER_PROBING: return "probing";
        case RADIO_WORKER_OPENING: return "opening";
        case RADIO_WORKER_PLAYING: return "playing";
        case RADIO_WORKER_STOPPING: return "stopping";
        case RADIO_WORKER_CLOSING: return "closing";
    }
    return "unknown";
}

int Radio_WorkerIsIdle(void) { return radio_worker_state == RADIO_WORKER_IDLE && radio_net_worker_streams == NULL; }
const char *Radio_WorkerStateName(void) { return radio_worker_state_name(radio_worker_state); }
int Radio_PlaybackOwnsNetwork(void) { return !Radio_WorkerIsIdle(); }

/* No per-child InitAmiSSL()/CleanupAmiSSL()/bsdsocket.library close any
 * more: the worker task's own AmiSSL init/instance and bsdsocket base stay
 * open across every station switch and are only closed once, by
 * radio_net_worker_entry()'s own teardown when Radio_NetworkShutdown() asks
 * the worker to exit. Radio_Close() used to call radio_net_close_child()
 * here; there is nothing left for it to do per station. */

static int radio_ssl_free_before_socket_close(RadioStream *rs);
static void radio_ssl_close_stream_mode(RadioStream *rs, RadioCloseMode mode)
{
    if (!rs) return;
    radio_net_adopt_context(rs);
    RADIO_CLEANUP_DEBUG_PRINTF(("radio-cleanup: HTTPS cleanup start mode=%s ssl=%p ctx=%p fd=%ld handshake=%d\n", radio_close_mode_name(mode), (void *)rs->ssl, (void *)rs->ctx, (long)rs->sock, rs->sslHandshakeDone));
    RADIO_DBG(printf("radio-cleanup: ssl-close mode=%s session=%lu status=%d sslHandshakeDone=%d ssl=%p ctx=%p fd=%ld open_socket_count=%ld\n",
        radio_close_mode_name(mode), rs->session_id, (int)rs->status, rs->sslHandshakeDone,
        (void *)rs->ssl, (void *)rs->ctx, (long)rs->sock, radio_open_socket_count););
    if (rs->ssl && !rs->sslFreed) {
        /* A genuine AmiSSL/heap fault both leaks (quarantines) the SSL and
         * poisons the shared instance. A bare peer-close (sslDroppedTransport)
         * must STILL leak the SSL -- freeing one that took an I/O fault is what
         * produced the AN_BadFreeAddr alert -- but must NOT poison the shared
         * worker SSL_CTX, or every later HTTPS attempt this run is refused for
         * what was only a network drop (the CD32-vs-WinUAE difference). */
        int real_fault = rs->sslStatePoisoned || rs->fatalStop ||
            Radio_IsMemoryPoisoned() || Radio_IsTlsPoisoned() ||
            (rs->noReconnect && !rs->sslDroppedTransport);
        int quarantine = real_fault || rs->noReconnect || rs->sslDroppedTransport;
        if (quarantine) {
            RADIO_DBG(printf("radio-cleanup: SSL_free skipped (quarantined%s) session=%lu ssl=%p\n",
                real_fault ? "/poisoned" : "/transport-drop", rs->session_id, (void *)rs->ssl);)
            if (real_fault) {
                radio_tls_shutdown_quarantine = 1;
                radio_worker_ssl_ctx_poisoned = 1;
            }
            rs->sslFreed = 1;
            if (radio_active_ssl_count > 0) radio_active_ssl_count--;
            rs->ssl = NULL;
            rs->sslHandshakeDone = 0;
        } else if (radio_runtime_diag_leak_ssl_enabled()) {
            radio_trace("radio-diag-leak: category=playback session=%lu ssl=%p action=quarantined-no-SSL_free\n",
                rs->session_id, (void *)rs->ssl);
            rs->sslFreed = 1;
            if (radio_active_ssl_count > 0) radio_active_ssl_count--;
            rs->ssl = NULL;
            rs->sslHandshakeDone = 0;
        } else if (rs->sock == RADIO_INVALID_SOCKET || rs->socketClosed) {
            SSL *ssl_to_free = rs->ssl;
            Radio_DebugCheckExecMem("before playback SSL_free after socket close");
            radio_trace("radio-tls-order: category=playback session=%lu step=SSL_free-begin ssl=%p\n",
                rs->session_id, (void *)ssl_to_free);
            radio_amissl_lifecycle_diag("SSL_free-before", rs->session_id, ssl_to_free, rs->ctx);
            SSL_free(ssl_to_free);
            radio_amissl_lifecycle_diag("SSL_free-after", rs->session_id, ssl_to_free, rs->ctx);
            radio_trace("radio-tls-order: category=playback session=%lu step=SSL_free-complete\n", rs->session_id);
            Radio_DebugCheckExecMem("after playback SSL_free after socket close");
            rs->sslFreed = 1;
            if (radio_active_ssl_count > 0) radio_active_ssl_count--;
            rs->ssl = NULL;
            rs->sslHandshakeDone = 0;
        } else {
            RADIO_DBG(printf("radio-cleanup: SSL_free deferred until after CloseSocket session=%lu ssl=%p fd=%ld\n",
                rs->session_id, (void *)rs->ssl, (long)rs->sock);)
        }
    } else {
        RADIO_CLEANUP_DEBUG_PRINTF(("radio-cleanup: SSL_free skipped\n"));
    }
    rs->ctx = NULL;
    RADIO_CLEANUP_DEBUG_PRINTF(("radio-cleanup: HTTPS cleanup complete\n"));
}

/* Safe default used by every error/timeout/handshake-failure path: never
 * attempts SSL_shutdown() on a session that may already be in an error or
 * peer-closed state. */
static void radio_ssl_close_stream(RadioStream *rs);
static void radio_abort_current_socket(RadioStream *rs);
static void close_current_socket(RadioStream *rs);

/* Every non-blocking SSL_connect/SSL_read/SSL_write call site classifies its
 * SSL_get_error() result into three buckets: WANT_READ/WANT_WRITE (retry
 * later, nothing wrong), ZERO_RETURN (a clean TLS close_notify EOF -- the
 * existing reconnect/end-of-stream path handles this safely, no quarantine
 * needed), and everything else, which is fatal. That third bucket used to be
 * narrowed to "SSL_ERROR_SSL or a non-empty OpenSSL error queue", which
 * missed SSL_ERROR_SYSCALL with an empty queue (a bare socket-level failure
 * AmiSSL couldn't attach a record-layer reason to) and any other/unknown
 * SSL_get_error() code -- letting SSL_free()/SSL_CTX_free() run on an SSL
 * object that just took a fatal I/O fault, corrupting AmiSSL's internals
 * (observed as an AN_BadFreeAddr 0100000F recoverable alert). Treat every
 * outcome outside the two known-safe buckets as fatal instead. */
/* Whether to leak (rather than SSL_free()/SSL_CTX_free()) an abort-mode
 * close. This must NOT be an unconditional "always skip" for every abort --
 * a normal user Stop/station-switch is not a fault, and leaking its SSL/
 * SSL_CTX on every station switch is what starves CleanupAmiSSL()/
 * CloseAmiSSL() and produces the app-close alert later (see
 * docs/amissl-lifecycle-audit.md F2). Quarantine/leak only when this
 * session (or the whole AmiSSL instance) actually took a real fault --
 * a classified-fatal SSL_connect/SSL_read/SSL_write error
 * (rs->noReconnect/rs->fatalStop), detected ring/heap corruption
 * (rs->sslStatePoisoned), or a task-wide TLS/memory poison flag. */
static int radio_skip_abort_ssl_free(RadioStream *rs)
{
    Radio_LogRuntimeFlagsOnce();
    if (radio_runtime_flag_enabled("MP3_ALLOW_ABORT_SSL_FREE"))
        return 0;
    if (radio_runtime_flag_enabled("MP3_SKIP_ABORT_SSL_FREE"))
        return 1;
    if (rs && (rs->sslStatePoisoned || rs->fatalStop || rs->noReconnect))
        return 1;
    if (Radio_IsMemoryPoisoned() || Radio_IsTlsPoisoned())
        return 1;
    return 0;
}

static int radio_ssl_error_is_fatal(int e)
{
    return e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE &&
        e != SSL_ERROR_ZERO_RETURN;
}

/* Whether a "fatal" SSL fault (radio_ssl_error_is_fatal) should poison the
 * whole shared AmiSSL instance -- disabling HTTPS for the rest of the run --
 * as opposed to merely quarantining this one connection's SSL object.
 *
 * The per-connection quarantine (leak instead of SSL_free) must still happen
 * for ANY fatal outcome: freeing an SSL that just took an I/O fault is what
 * produced the AN_BadFreeAddr alert, so callers keep setting ssl_poisoned/
 * noReconnect regardless. But instance-wide poison is far heavier and should be
 * reserved for a genuine AmiSSL-internal fault, not a network event.
 *
 * A bare transport close -- the peer resetting or EOF-ing the socket, which
 * AmiSSL reports as SSL_ERROR_SYSCALL with an EMPTY error queue (lib_error 0)
 * -- is a network drop, not AmiSSL corruption. It is exactly what a slow or
 * marginal link provokes (a CDN hanging up mid-probe/artwork/read), and it was
 * wrongly poisoning HTTPS for the whole session on the CD32 while a fast WinUAE
 * link never tripped it. Poison only when the OpenSSL error queue actually
 * holds a library error (a real protocol/record/state fault); treat a bare
 * SYSCALL/EOF close as connection-local. ZERO_RETURN is already non-fatal. */
static int radio_tls_fault_should_poison(int e, unsigned long lib_error)
{
    (void)e;
    (void)lib_error;
    /* Policy: a fatal per-connection TLS fault quarantines that connection --
     * its SSL is leaked, never freed after an I/O fault (the AN_BadFreeAddr
     * guard) -- but must NOT poison the shared AmiSSL instance / disable HTTPS
     * for the rest of the run.
     *
     * Every "fatal" outcome observed on real CD32 hardware was a per-server
     * network or protocol event, not AmiSSL corruption, and always with
     * memoryPoisoned=0: a bare EPIPE mid-write (OpenSSL 3.0 reports it in the
     * queue as ERR_SYSTEM_FLAG|errno, e.g. 0x80000020), and a peer "tlsv1 alert
     * decode error" on a single album-art host (lib_error 0x0a00041a). Each of
     * those was disabling ALL HTTPS for the session -- one flaky station or
     * artwork server taking out streaming until an app restart -- while a fast
     * WinUAE link rarely tripped it. The instance was never actually damaged.
     *
     * Genuine AmiSSL/heap corruption is detected independently by the exec-heap
     * memcheck -> Radio_MarkMemoryPoisoned() (which hard-poisons TLS and does
     * disable HTTPS), so real corruption is still caught. Connection-level TLS
     * faults therefore quarantine only; they never poison. */
    return 0;
}


#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
static void radio_tls_raw_ch(char c)
{
    register unsigned long d0 __asm("d0") = (unsigned long)(unsigned char)c;
    __asm volatile ("move.l 4.w,%%a6\n\tjsr -516(%%a6)"
        : "+d"(d0) : : "d1", "a0", "a1", "a6", "cc", "memory");
}

static void radio_tls_raw_str(const char *s)
{
    while (s && *s) radio_tls_raw_ch(*s++);
}

static void radio_tls_raw_hex(unsigned long v)
{
    static const char h[] = "0123456789abcdef";
    int shift;
    radio_tls_raw_ch('0'); radio_tls_raw_ch('x');
    for (shift = 28; shift >= 0; shift -= 4)
        radio_tls_raw_ch(h[(v >> shift) & 0x0fUL]);
}

static void radio_tls_connect_marker(const char *tag, unsigned long session, int attempt,
    SSL *ssl, SSL_CTX *ctx, long fd, int ret)
{
    radio_tls_raw_str(tag);
    radio_tls_raw_str(" session="); radio_tls_raw_hex(session);
    radio_tls_raw_str(" attempt="); radio_tls_raw_hex((unsigned long)attempt);
    radio_tls_raw_str(" ssl="); radio_tls_raw_hex((unsigned long)ssl);
    radio_tls_raw_str(" ctx="); radio_tls_raw_hex((unsigned long)ctx);
    radio_tls_raw_str(" fd="); radio_tls_raw_hex((unsigned long)fd);
    radio_tls_raw_str(" ret="); radio_tls_raw_hex((unsigned long)ret);
    radio_tls_raw_ch('\n');
}

static void radio_tls_range_snapshot(RadioStream *rs, const char *where)
{
    if (!rs) return;
    radio_tls_raw_str("TLS-RANGE where="); radio_tls_raw_str(where);
    radio_tls_raw_str(" session="); radio_tls_raw_hex(rs->session_id);
    radio_tls_raw_str(" rs="); radio_tls_raw_hex((unsigned long)rs);
    radio_tls_raw_str(" rsEnd="); radio_tls_raw_hex((unsigned long)(rs + 1));
    radio_tls_raw_str(" ring="); radio_tls_raw_hex((unsigned long)rs->ring);
    radio_tls_raw_str(" ringEnd="); radio_tls_raw_hex((unsigned long)(rs->ring ? rs->ring + rs->size : 0));
    radio_tls_raw_str(" ringAlloc="); radio_tls_raw_hex((unsigned long)rs->ringAlloc);
    radio_tls_raw_str(" ssl="); radio_tls_raw_hex((unsigned long)rs->ssl);
    radio_tls_raw_str(" ctx="); radio_tls_raw_hex((unsigned long)rs->ctx);
    radio_tls_raw_str(" url="); radio_tls_raw_hex((unsigned long)rs->url);
    radio_tls_raw_str(" host="); radio_tls_raw_hex((unsigned long)rs->host);
    radio_tls_raw_str(" path="); radio_tls_raw_hex((unsigned long)rs->path);
    radio_tls_raw_ch('\n');
}
#else
static void radio_tls_connect_marker(const char *tag, unsigned long session, int attempt,
    SSL *ssl, SSL_CTX *ctx, long fd, int ret)
{
    (void)tag; (void)session; (void)attempt; (void)ssl; (void)ctx; (void)fd; (void)ret;
}

static void radio_tls_range_snapshot(RadioStream *rs, const char *where)
{
    (void)rs; (void)where;
}
#endif

/* Connect/handshake poll budget (defined below, near radio_wait_connected):
 * default ~12s, tunable via MP3_CONNECT_SECONDS. Forward-declared here because
 * the TLS handshake poll loop is compiled ahead of the definition. */
static int radio_connect_poll_tries(void);

/* Poll SSL_connect on the non-blocking socket — same budget as radio_wait_connected. */
static int radio_ssl_do_handshake(RadioStream *rs)
{
    int tries = 0;
    int budget = radio_connect_poll_tries();
    unsigned long deadline_ticks = radio_monotonic_ticks() + (unsigned long)((budget + 24) / 25) * 50UL;
    int last_error = 0;
    radio_net_adopt_context(rs);
    /* Start with a clean OpenSSL error queue: a stale entry left by an
     * earlier failed connection would otherwise be misread as this
     * connection's fatal error by the fault handling below. */
    ERR_clear_error();
    while (radio_deadline_pending(deadline_ticks)) {
        int r, e;
        if (radio_is_stopping(rs)) return -1;
        tries++;
        RADIO_DBG(printf("BEFORE SSL_connect session=%lu attempt=%d ssl=%p ctx=%p fd=%ld\n",
            rs ? rs->session_id : 0, tries, rs ? (void *)rs->ssl : 0,
            rs ? (void *)rs->ctx : 0, rs ? (long)rs->sock : -1L););
        radio_tls_range_snapshot(rs, "before-SSL_connect");
        Radio_DebugCheckExecMem("before SSL_connect");
        radio_tls_connect_marker("TLS-CONNECT-ENTER", rs ? rs->session_id : 0, tries,
            rs ? rs->ssl : NULL, rs ? rs->ctx : NULL, rs ? (long)rs->sock : -1L, 0);
        radio_amissl_lifecycle_diag("SSL_connect-before", rs->session_id, rs->ssl, rs->ctx);
        r = SSL_connect(rs->ssl);
        radio_tls_connect_marker("TLS-CONNECT-RETURN", rs ? rs->session_id : 0, tries,
            rs ? rs->ssl : NULL, rs ? rs->ctx : NULL, rs ? (long)rs->sock : -1L, r);
        RADIO_DBG(printf("SSL_CONNECT_RET session=%lu attempt=%d ret=%d ssl=%p ctx=%p fd=%ld\n",
            rs ? rs->session_id : 0, tries, r, rs ? (void *)rs->ssl : 0,
            rs ? (void *)rs->ctx : 0, rs ? (long)rs->sock : -1L););
        radio_amissl_lifecycle_diag("SSL_connect-after", rs->session_id, rs->ssl, rs->ctx);
        Radio_DebugCheckExecMem("after SSL_connect");
        if (Radio_IsMemoryPoisoned() || Radio_IsTlsPoisoned()) {
            rs->sslStatePoisoned = 1; rs->fatalStop = 1; rs->noReconnect = 1;
            radio_worker_ssl_ctx_poisoned = 1;
            RADIO_DBG(printf("radio-tls: SSL_connect returned %d but post-connect memory/TLS poison was detected session=%lu\n", r, rs ? rs->session_id : 0););
            return -1;
        }
        if (r == 1) {
            RADIO_DBG(printf("AFTER SSL_connect success session=%lu attempt=%d ssl=%p ctx=%p fd=%ld\n",
                rs ? rs->session_id : 0, tries, rs ? (void *)rs->ssl : 0,
                rs ? (void *)rs->ctx : 0, rs ? (long)rs->sock : -1L););
            RADIO_DBG(printf("SSL_CONNECT_ATTEMPT session=%lu attempt=%d ret=%d err=0 fd=%ld ssl=%p ctx=%p handshake=%d\n", rs ? rs->session_id : 0, tries, r, rs ? (long)rs->sock : -1L, rs ? (void *)rs->ssl : 0, rs ? (void *)rs->ctx : 0, rs ? rs->sslHandshakeDone : 0););
            if (rs) rs->sslHandshakeDone = 1;
            RADIO_DBG(printf("SSL_CONNECT_DONE session=%lu\n", rs ? rs->session_id : 0););
            return 0;
        }
        Radio_DebugCheckExecMem("before SSL_get_error after SSL_connect");
        radio_tls_connect_marker("TLS-CONNECT-GETERROR-BEGIN", rs ? rs->session_id : 0, tries,
            rs ? rs->ssl : NULL, rs ? rs->ctx : NULL, rs ? (long)rs->sock : -1L, r);
        e = SSL_get_error(rs->ssl, r);
        radio_tls_connect_marker("TLS-CONNECT-GETERROR-END", rs ? rs->session_id : 0, tries,
            rs ? rs->ssl : NULL, rs ? rs->ctx : NULL, rs ? (long)rs->sock : -1L, e);
        Radio_DebugCheckExecMem("after SSL_get_error after SSL_connect");
        RADIO_DBG(printf("AFTER SSL_connect fail session=%lu attempt=%d ret=%d err=%d ssl=%p ctx=%p fd=%ld\n",
            rs ? rs->session_id : 0, tries, r, e, rs ? (void *)rs->ssl : 0,
            rs ? (void *)rs->ctx : 0, rs ? (long)rs->sock : -1L););
        last_error = e;
        RADIO_DBG(printf("SSL_CONNECT_ATTEMPT session=%lu attempt=%d ret=%d err=%d fd=%ld ssl=%p ctx=%p handshake=%d\n", rs ? rs->session_id : 0, tries, r, e, rs ? (long)rs->sock : -1L, rs ? (void *)rs->ssl : 0, rs ? (void *)rs->ctx : 0, rs ? rs->sslHandshakeDone : 0););
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
            int wr;
            RADIO_DBG(printf("SSL_CONNECT_WANT_%s session=%lu attempt=%d wait-ready\n",
                e == SSL_ERROR_WANT_READ ? "READ" : "WRITE", rs ? rs->session_id : 0, tries););
            wr = radio_ssl_wait_ready(rs->sock, e);
            if (wr > 0) continue;
            if (wr == 0) continue;
            if (radio_is_stopping(rs)) return -1;
            printf("radio-tls: SSL_connect readiness wait failed session=%lu attempt=%d ssl_error=%d socket_error=%ld fd=%ld\n",
                rs ? rs->session_id : 0, tries, e, radio_sock_errno(), rs ? (long)rs->sock : -1L);
            if (rs) set_error(rs, "TLS readiness wait failed");
            return -1;
        }
        /* SSL_ERROR_WANT_READ/WRITE just means "call again later" on a
         * non-blocking socket; anything else is a real handshake failure,
         * and the OpenSSL/AmiSSL error queue has the actual reason (cipher
         * mismatch, protocol version, rejected cert, ...) that
         * SSL_get_error()'s numeric code alone doesn't say. */
        {
            unsigned long ssl_lib_error = ERR_get_error();
            char ssl_error_buf[160];
            char handshake_error[128];
            ssl_error_buf[0] = '\0';
            if (ssl_lib_error != 0)
                ERR_error_string_n(ssl_lib_error, ssl_error_buf, sizeof(ssl_error_buf));
            printf("radio-tls: SSL_connect final failure session=%lu attempt=%d ret=%d ssl_error=%d lib_error=%08lx reason=\"%s\" fd=%ld\n",
                rs ? rs->session_id : 0, tries, r, e, ssl_lib_error,
                ssl_error_buf[0] ? ssl_error_buf : "none", rs ? (long)rs->sock : -1L);
            if (rs && radio_ssl_error_is_fatal(e)) {
                rs->lastSslError = e;
                rs->noReconnect = 1;
                if (!radio_tls_fault_should_poison(e, ssl_lib_error))
                    rs->sslDroppedTransport = 1; /* peer close: leak SSL, keep HTTPS usable */
                strcpy(rs->lastSslOp, (e == SSL_ERROR_SYSCALL && ssl_lib_error == 0) ?
                    "ssl-connect-syscall" : "ssl-connect-fatal");
                if (ssl_error_buf[0]) {
                    sprintf(handshake_error, "TLS handshake failed: %.105s", ssl_error_buf);
                    set_error(rs, handshake_error);
                } else if (e == SSL_ERROR_SYSCALL && ssl_lib_error == 0) {
                    set_error(rs, "TLS handshake failed: peer closed connection");
                }
                RADIO_DBG(printf("radio-tls: session=%lu handshake failed; connection SSL quarantined (poison=%d)\n", rs->session_id, radio_tls_fault_should_poison(e, ssl_lib_error)));
            }
            /* Drain the rest of the queue so the failure cannot masquerade
             * as a later session's fault. */
            ERR_clear_error();
        }
        return -1;
    }
    printf("radio-tls: SSL_connect retry budget expired session=%lu budget=%d attempts=%d last_ssl_error=%d fd=%ld memory_poison=%d tls_poison=%d\n",
        rs ? rs->session_id : 0, budget, tries, last_error, rs ? (long)rs->sock : -1L,
        Radio_IsMemoryPoisoned() ? 1 : 0, Radio_IsTlsPoisoned() ? 1 : 0);
    return -1;
}

static int radio_ssl_connect(RadioStream *rs)
{
    int set_fd_ok;
    radio_net_adopt_context(rs);
    if (radio_net_worker_amissl_ready(rs) != 0) return -1;
    rs->ctx = radio_worker_get_ssl_ctx("playback", rs->session_id);
    rs->ctxFreed = 0;
    if (!rs->ctx) { set_error(rs, "AmiSSL init failed"); return -1; }
    /* Fresh SSL per HTTPS socket in normal and diagnostic modes. */
    RADIO_DBG(printf("BEFORE SSL_new session=%lu ctx=%p\n",
        rs->session_id, (void *)rs->ctx););
    Radio_DebugCheckExecMem("before SSL_new");
    if (Radio_IsMemoryPoisoned() || Radio_IsTlsPoisoned()) {
        radio_worker_ssl_ctx_poisoned = 1;
        set_error(rs, "AmiSSL init failed");
        return -1;
    }
    radio_amissl_lifecycle_diag("SSL_new-before", rs->session_id, NULL, rs->ctx);
    rs->ssl = SSL_new(rs->ctx);
    radio_amissl_lifecycle_diag("SSL_new-after", rs->session_id, rs->ssl, rs->ctx);
    Radio_DebugCheckExecMem("after SSL_new");
    if (Radio_IsMemoryPoisoned() || Radio_IsTlsPoisoned()) {
        radio_worker_ssl_ctx_poisoned = 1;
        rs->sslStatePoisoned = 1;
        rs->fatalStop = 1;
        rs->noReconnect = 1;
        set_error(rs, "AmiSSL init failed");
        return -1;
    }
    RADIO_DBG(printf("AFTER SSL_new session=%lu ssl=%p ctx=%p\n",
        rs->session_id, (void *)rs->ssl, (void *)rs->ctx););
    if (rs->ssl) {
        rs->sslFreed = 0;
        rs->sslReadCloseSeen = 0;
        radio_active_ssl_count++;
        RADIO_DBG(printf("radio-resource: session=%lu SSL allocated active_ssl_count=%ld\n", rs->session_id, radio_active_ssl_count));
    }
    if (!rs->ssl) { radio_ssl_close_stream(rs); set_error(rs, "AmiSSL init failed"); return -1; }
#ifdef SSL_CTRL_SET_TLSEXT_HOSTNAME
    radio_tls_range_snapshot(rs, "before-SSL_set_tlsext_host_name");
    SSL_set_tlsext_host_name(rs->ssl, rs->host);
    radio_tls_range_snapshot(rs, "after-SSL_set_tlsext_host_name");
    Radio_DebugCheckExecMem("after SSL_set_tlsext_host_name");
#endif
#ifdef RADIO_SSL_VERIFY_PEER
    {
        X509_VERIFY_PARAM *verify_param = SSL_get0_param(rs->ssl);
        if (verify_param) X509_VERIFY_PARAM_set1_host(verify_param, rs->host, 0);
    }
#endif
    set_fd_ok = SSL_set_fd(rs->ssl, (int)rs->sock);
    if (!set_fd_ok) { RADIO_DBG(printf("radio-tls: SSL_set_fd failed session=%lu fd=%ld ssl=%p ctx=%p\n", rs->session_id, (long)rs->sock, (void *)rs->ssl, (void *)rs->ctx);); radio_ssl_close_stream(rs); set_error(rs, "TLS handshake failed"); return -1; }
    if (radio_ssl_do_handshake(rs) != 0) {
        RADIO_DBG(printf("radio-tls: handshake cleanup session=%lu error_ptr=%p stream_range=%p..%p status=%d open_socket_count=%ld active_ssl_count=%ld active_ssl_ctx_count=%ld SocketBase=%p AmiSSLBase=%p AmiSSLMasterBase=%p\n", rs->session_id, (void *)rs->error, (void *)rs, (void *)(rs + 1), (int)rs->status, radio_open_socket_count, radio_active_ssl_count, radio_active_ssl_ctx_count, (void *)SocketBase, (void *)AmiSSLBase, (void *)AmiSSLMasterBase););
        radio_ssl_close_stream(rs);
        if (!rs->error[0]) set_error(rs, "TLS handshake failed"); return -1;
    }
    return 0;
}

/* Normal Stop/station-switch teardown on real CD32 hardware must not enter
 * SSL_shutdown(): even readiness-driven SSL_shutdown() can wedge AmiSSL while
 * the peer is still streaming.  A healthy per-connection SSL object can still
 * be released cleanly without leaking:
 *
 *   1. stop/unregister worker pumping before this helper is called;
 *   2. mark close_notify as locally sent (no socket I/O);
 *   3. ensure the SSL-owned socket BIO does not close the Amiga descriptor;
 *   4. SSL_free() while the descriptor is still valid;
 *   5. let radio_abort_current_socket() CloseSocket() exactly once.
 *
 * SSL_set_fd() normally creates a BIO_NOCLOSE socket BIO, but setting it
 * explicitly here makes ownership unambiguous across AmiSSL versions.
 * Fatal/peer-drop/diagnostic sessions return 0 and keep the existing
 * quarantine policy in radio_ssl_close_stream_mode(). */
static int radio_ssl_free_before_socket_close(RadioStream *rs)
{
    SSL *ssl_to_free;
    BIO *rbio;
    BIO *wbio;

    if (!rs) return 0;
    if (Radio_IsMemoryPoisoned() || Radio_IsTlsPoisoned()) return 0;
    if (rs->sslStatePoisoned || rs->fatalStop || rs->noReconnect) return 0;
    if (!rs->ssl || rs->sslFreed) return 0;
    if (!rs->sslHandshakeDone) return 0;
    if (rs->sock == RADIO_INVALID_SOCKET || rs->socketClosed) return 0;
    if (radio_runtime_diag_leak_ssl_enabled()) return 0;

    radio_net_adopt_context(rs);
    ssl_to_free = rs->ssl;
    rbio = SSL_get_rbio(ssl_to_free);
    wbio = SSL_get_wbio(ssl_to_free);

    SSL_set_shutdown(ssl_to_free, SSL_SENT_SHUTDOWN);
    if (rbio) BIO_set_close(rbio, BIO_NOCLOSE);
    if (wbio && wbio != rbio) BIO_set_close(wbio, BIO_NOCLOSE);

    Radio_DebugCheckExecMem("before playback SSL_free before socket close");
    radio_trace("radio-tls-order: category=playback session=%lu step=SSL_free-begin ssl=%p fd=%ld\n",
        rs->session_id, (void *)ssl_to_free, (long)rs->sock);
    radio_amissl_lifecycle_diag("SSL_free-before", rs->session_id, ssl_to_free, rs->ctx);
    SSL_free(ssl_to_free);
    radio_amissl_lifecycle_diag("SSL_free-after", rs->session_id, ssl_to_free, rs->ctx);
    radio_trace("radio-tls-order: category=playback session=%lu step=SSL_free-complete\n",
        rs->session_id);
    Radio_DebugCheckExecMem("after playback SSL_free before socket close");

    rs->sslFreed = 1;
    if (radio_active_ssl_count > 0) radio_active_ssl_count--;
    rs->ssl = NULL;
    rs->sslHandshakeDone = 0;
    rs->ctx = NULL; /* borrowed shared context remains owned by the worker */
    return 1;
}

static void radio_ssl_close_stream(RadioStream *rs) { radio_ssl_close_stream_mode(rs, RADIO_CLOSE_ABORT); }

#endif /* AMIGA_M68K && HAVE_AMISSL */

#if !defined(AMIGA_M68K) || !defined(HAVE_AMISSL)
static void radio_net_adopt_context(RadioStream *rs) { (void)rs; }
int Radio_PlaybackOwnsNetwork(void) { return 0; }
void *Radio_GetWorkerSslCtx(const char *category, unsigned long session_id) { (void)category; (void)session_id; return NULL; }
void Radio_MarkWorkerSslCtxPoisoned(const char *where) { (void)where; }
#endif

/* Number of ~40ms connect() poll iterations to attempt before giving up
 * (shared by radio_wait_connected() for playback and
 * radio_net_transport_wait_connected() for the probe/artwork transport).
 *
 * The historical budget was a hard-coded 150 (~6s). That is comfortable on a
 * fast emulated network but too tight on a slow/lossy real-hardware link -- on
 * a 115200-baud WiFi modem a single dropped TCP SYN already costs a ~3s kernel
 * retransmit, so a perfectly good but slightly slow station can exceed 6s and
 * fail the probe with "timeout while connecting" while faster ones succeed.
 *
 * The default is raised to ~12s and made tunable at runtime via
 * MP3_CONNECT_SECONDS (an env var or an ENV: variable, clamped to 3..80s) so a
 * slow link can be given more headroom without a rebuild. Larger values do
 * extend how long a genuinely dead server holds the "Connecting..." state up,
 * because the probe connect is a synchronous net-worker round trip -- hence the
 * clamp. Read once and cached: the value cannot change mid-run, and this keeps
 * the poll loops free of repeated getenv()/GetVar() calls. */
static int radio_connect_poll_tries(void)
{
    static int cached = -1;
    if (cached < 0) {
        int secs = 12;
        const char *v = radio_runtime_flag_raw_getenv("MP3_CONNECT_SECONDS");
        if (!v || !*v) v = radio_runtime_flag_raw_getvar("MP3_CONNECT_SECONDS");
        if (v && *v) {
            int parsed = atoi(v);
            if (parsed > 0) secs = parsed;
        }
        if (secs < 3) secs = 3;
        if (secs > 80) secs = 80;
        cached = secs * 25; /* ~40ms per poll (radio_backoff_sleep) => 25 polls/sec */
    }
    return cached;
}

/* Drive a non-blocking connect() to completion by re-issuing connect() and
 * yielding with Delay() between tries, so the connect never blocks (and so
 * never freezes WinUAE's emulation).  Returns 0 on success, -1 on failure or
 * stop/timeout.  Success is connect()==0 or EISCONN; anything else is treated
 * as "still connecting" until the timeout, which keeps us robust even if a
 * particular stack reports a non-standard in-progress errno. */
static int radio_wait_connected(RadioStream *rs, struct sockaddr_in *sa)
{
    int tries;
    int budget = radio_connect_poll_tries();
    radio_net_adopt_context(rs);
    RADIO_DBG(printf("radio-connect: session=%lu wait_connected enter fd=%ld host=%s budget=%d\n", rs ? rs->session_id : 0, rs ? (long)rs->sock : -1L, rs ? rs->host : "", budget););
    /* ~12s default at 40ms/poll (tunable via MP3_CONNECT_SECONDS). */
    for (tries = 0; tries < budget; tries++) {
        long e;
        int cr;
        if (radio_is_stopping(rs)) {
            RADIO_DBG(printf("radio-connect: session=%lu wait_connected stopping tries=%d\n", rs ? rs->session_id : 0, tries););
            return -1;
        }
        radio_backoff_sleep();
        if (radio_is_stopping(rs)) {
            RADIO_DBG(printf("radio-connect: session=%lu wait_connected stopping tries=%d\n", rs ? rs->session_id : 0, tries););
            return -1;
        }
        cr = connect(rs->sock, (struct sockaddr *)sa, sizeof(*sa));
        if (cr == 0) {
            RADIO_DBG(printf("radio-connect: session=%lu wait_connected connected tries=%d\n", rs ? rs->session_id : 0, tries););
            return 0;
        }
        e = radio_sock_errno();
        RADIO_DBG(printf("radio-connect: session=%lu wait_connected tries=%d cr=%d errno=%ld\n", rs ? rs->session_id : 0, tries, cr, e););
        if (e == EISCONN) {
            RADIO_DBG(printf("radio-connect: session=%lu wait_connected connected (EISCONN) tries=%d\n", rs ? rs->session_id : 0, tries););
            return 0;
        }
    }
    RADIO_DBG(printf("radio-connect: session=%lu wait_connected timed out\n", rs ? rs->session_id : 0););
    return -1;
}

/* Send the whole request on the (now non-blocking) socket, yielding on a
 * would-block partial send instead of failing. */
static int radio_send_all(RadioStream *rs, const char *buf, int len)
{
    int sent = 0, tries = 0;
    radio_net_adopt_context(rs);
    while (sent < len && tries < 150) {
        int r;
        if (radio_is_stopping(rs))
            return -1;
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
        if (rs->isSSL && rs->ssl) {
            radio_net_adopt_context(rs);
            RADIO_DBG(printf("radio-ssl-write: session=%lu sslHandshakeDone=%d before SSL_write\n", rs->session_id, rs->sslHandshakeDone););
            if (rs->sslHandshakeDone != 1) {
                RADIO_DBG(printf("radio-ssl-write: ERROR session=%lu skipped SSL_write because handshake is incomplete sslHandshakeDone=%d\n", rs->session_id, rs->sslHandshakeDone););
                set_error(rs, "TLS handshake incomplete");
                close_current_socket(rs);
                return -1;
            }
            ERR_clear_error(); /* clean queue so ERR_get_error() below reflects only this SSL_write: a bare SYSCALL/EPIPE drop must stay lib_error=0 and quarantine, not poison */
            radio_amissl_lifecycle_diag("SSL_write-before", rs->session_id, rs->ssl, rs->ctx);
            r = (int)SSL_write(rs->ssl, buf + sent, len - sent);
            radio_amissl_lifecycle_diag("SSL_write-after", rs->session_id, rs->ssl, rs->ctx);
            if (r > 0) { sent += r; continue; }
            {
                int e = SSL_get_error(rs->ssl, r);
                unsigned long ssl_lib_error;
                if (e == SSL_ERROR_WANT_WRITE || e == SSL_ERROR_WANT_READ) {
                    radio_backoff_sleep(); tries++; continue;
                }
                /* TLS write failure: fatal cases quarantine this connection
                 * and poison the shared context; nonfatal cases free only
                 * the per-connection SSL. */
                ssl_lib_error = ERR_get_error();
                RADIO_DBG(printf("radio-ssl-write: session=%lu write failed ssl_error=%d lib_error=%08lx\n", rs->session_id, e, ssl_lib_error));
                if (radio_ssl_error_is_fatal(e)) {
                    rs->lastSslError = e;
                    rs->noReconnect = 1;
                    if (!radio_tls_fault_should_poison(e, ssl_lib_error))
                        rs->sslDroppedTransport = 1; /* peer close: leak SSL, keep HTTPS usable */
                    strcpy(rs->lastSslOp, (e == SSL_ERROR_SYSCALL && ssl_lib_error == 0) ?
                        "ssl-write-syscall" : "ssl-write-fatal");
                    RADIO_DBG(printf("radio-ssl-write: session=%lu write failure; connection SSL quarantined (poison=%d)\n", rs->session_id, radio_tls_fault_should_poison(e, ssl_lib_error)));
                    ERR_clear_error();
                }
            }
            return -1;
        }
#endif
        r = (int)send(rs->sock, (char *)buf + sent, len - sent, 0);
        if (r > 0) { sent += r; continue; }
        if (r < 0 && radio_would_block()) { radio_backoff_sleep(); tries++; continue; }
        return -1;
    }
    return sent == len ? 0 : -1;
}

struct RadioNetTransport {
    RADIO_SOCKET sock;
    int use_tls;
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    SSL_CTX *ctx;
    SSL *ssl;
#endif
    int handshake_done;
    int ssl_poisoned;          /* real fault: leak the SSL AND poison the shared instance */
    int ssl_dropped_transport; /* bare peer close: leak the SSL, do NOT poison the instance */
    int ssl_teardown_unsafe;    /* incomplete shutdown: leak this SSL, do NOT poison the instance */
    char category[16];
    unsigned long session_id;
    unsigned long host_addr_be;
    int socket_counted;
    int ssl_counted;
    int ctx_counted;
};

static int radio_net_transport_wait_connected(RadioNetTransport *t, struct sockaddr_in *sa)
{
    int tries;
    int budget = radio_connect_poll_tries();
    for (tries = 0; tries < budget; tries++) {
        long e;
        int cr;
        radio_backoff_sleep();
        cr = connect(t->sock, (struct sockaddr *)sa, sizeof(*sa));
        if (cr == 0) return 0;
        e = radio_sock_errno();
        if (e == EISCONN) return 0;
    }
    return -1;
}

static void radio_net_transport_count_socket(RadioNetTransport *t);
static void radio_net_transport_count_ssl(RadioNetTransport *t);
static void radio_net_close_transport_worker(RadioNetTransport *t, int graceful);

static int radio_net_transport_tls_connect(RadioNetTransport *t, const char *host)
{
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    int tries = 0;
    int budget = radio_connect_poll_tries();
    unsigned long deadline_ticks = radio_monotonic_ticks() + (unsigned long)((budget + 24) / 25) * 50UL;
    if (radio_net_worker_amissl_ready(NULL) != 0) return -1;
    /* Borrow the worker-owned shared SSL_CTX -- created once, retained for the
     * whole run, already configured VERIFY_NONE + IGNORE_UNEXPECTED_EOF -- the
     * same context playback uses. Creating and freeing a fresh SSL_CTX per
     * probe/artwork fetch churned AmiSSL and hard-locked the real CD32; one
     * retained context reused for every connection is how AmiSSL is meant to be
     * used. Borrowed: not counted as transport-owned and never SSL_CTX_free'd
     * at close (freed once at worker shutdown). */
    t->ctx = radio_worker_get_ssl_ctx(t->category, t->session_id);
    Radio_DebugCheckExecMem("after transport shared SSL_CTX");
    if (!t->ctx || Radio_IsMemoryPoisoned() || Radio_IsTlsPoisoned()) return -1;
    Radio_DebugCheckExecMem("before transport SSL_new");
    radio_amissl_lifecycle_diag("SSL_new-before", t->session_id, NULL, t->ctx);
    t->ssl = SSL_new(t->ctx);
    radio_amissl_lifecycle_diag("SSL_new-after", t->session_id, t->ssl, t->ctx);
    if (t->ssl) radio_net_transport_count_ssl(t);
    Radio_DebugCheckExecMem("after transport SSL_new");
    if (!t->ssl || Radio_IsMemoryPoisoned() || Radio_IsTlsPoisoned()) return -1;
#ifdef SSL_CTRL_SET_TLSEXT_HOSTNAME
    if (host && host[0]) SSL_set_tlsext_host_name(t->ssl, host);
#endif
    Radio_DebugCheckExecMem("after transport SSL_set_tlsext_host_name");
    Radio_DebugCheckExecMem("before transport SSL_set_fd");
    if (SSL_set_fd(t->ssl, (int)t->sock) != 1) {
        Radio_DebugCheckExecMem("after transport SSL_set_fd");
        return -1;
    }
    Radio_DebugCheckExecMem("after transport SSL_set_fd");
    ERR_clear_error();
    while (radio_deadline_pending(deadline_ticks)) {
        int r, e;
        tries++;
        Radio_DebugCheckExecMem("before transport SSL_connect");
        radio_tls_connect_marker("TLS-CONNECT-ENTER", t->session_id, tries, t->ssl, t->ctx, (long)t->sock, 0);
        radio_amissl_lifecycle_diag("SSL_connect-before", t->session_id, t->ssl, t->ctx);
        r = SSL_connect(t->ssl);
        radio_tls_connect_marker("TLS-CONNECT-RETURN", t->session_id, tries, t->ssl, t->ctx, (long)t->sock, r);
        RADIO_DBG(printf("SSL_CONNECT_RET transport session=%lu attempt=%d ret=%d ssl=%p ctx=%p fd=%ld\n",
            t->session_id, tries, r, (void *)t->ssl, (void *)t->ctx, (long)t->sock););
        radio_amissl_lifecycle_diag("SSL_connect-after", t->session_id, t->ssl, t->ctx);
        Radio_DebugCheckExecMem("after transport SSL_connect");
        if (r == 1) { t->handshake_done = 1; return 0; }
        Radio_DebugCheckExecMem("before transport SSL_get_error after SSL_connect");
        radio_tls_connect_marker("TLS-CONNECT-GETERROR-BEGIN", t->session_id, tries, t->ssl, t->ctx, (long)t->sock, r);
        e = SSL_get_error(t->ssl, r);
        radio_tls_connect_marker("TLS-CONNECT-GETERROR-END", t->session_id, tries, t->ssl, t->ctx, (long)t->sock, e);
        Radio_DebugCheckExecMem("after transport SSL_get_error after SSL_connect");
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
            int wr = radio_ssl_wait_ready(t->sock, e);
            if (wr > 0) continue;
            if (wr == 0) continue;
            radio_trace("radio-net-tls: transport SSL_connect readiness wait failed session=%lu attempt=%d ssl_error=%d socket_error=%ld fd=%ld\n",
                t->session_id, tries, e, radio_sock_errno(), (long)t->sock);
            return -1;
        }
        /* Non-retriable handshake result. Decode the OpenSSL/AmiSSL error queue
         * so the exact cause is visible (release-visible: this path is only
         * reached on failure and returns immediately, so no per-iteration cost
         * and no effect on handshake timing). This is the probe/transport
         * counterpart of the decode radio_ssl_do_handshake() already does for
         * the playback path. Distinguishes a genuine protocol/cipher fault
         * (SSL_ERROR_SSL, lib_error set) from the server closing mid-handshake
         * (SSL_ERROR_SYSCALL/ZERO_RETURN, lib_error 0) -- the latter being what
         * a too-slow client on a slow link provokes. */
        {
            unsigned long ssl_lib_error = ERR_get_error();
            char ssl_error_buf[160];
            ssl_error_buf[0] = '\0';
            if (ssl_lib_error != 0)
                ERR_error_string_n(ssl_lib_error, ssl_error_buf, sizeof(ssl_error_buf));
            radio_trace("radio-net-tls: transport SSL_connect failed session=%lu attempt=%d ssl_error=%d lib_error=%08lx reason=\"%s\"\n",
                t->session_id, tries, e, ssl_lib_error,
                ssl_error_buf[0] ? ssl_error_buf : "none");
            if (radio_ssl_error_is_fatal(e)) {
                /* Always quarantine this connection's SSL (leak, not free) to
                 * avoid the AN_BadFreeAddr crash -- but only poison the shared
                 * AmiSSL instance for a genuine internal fault, not a bare
                 * peer close on a slow link (see radio_tls_fault_should_poison). */
                if (radio_tls_fault_should_poison(e, ssl_lib_error)) {
                    char where_buf[200];
                    t->ssl_poisoned = 1;
                    sprintf(where_buf, "transport SSL_connect fatal ssl_error=%d %.150s",
                        e, ssl_error_buf[0] ? ssl_error_buf : "");
                    Radio_MarkWorkerSslCtxPoisoned(where_buf);
                } else {
                    /* Leak this SSL at cleanup, but keep the shared instance
                     * usable -- the cleanup path keys poison off ssl_poisoned,
                     * so use the drop-only flag instead. */
                    t->ssl_dropped_transport = 1;
                    radio_trace("radio-net-tls: transport SSL_connect peer-close (ssl_error=%d lib_error=0) session=%lu -- connection quarantined, shared AmiSSL NOT poisoned\n",
                        e, t->session_id);
                }
                ERR_clear_error();
            }
        }
        return -1;
    }
#else
    (void)t; (void)host;
#endif
    return -1;
}

typedef struct RadioNetOpenArgs {
    int state;
#if defined(AMIGA_M68K)
    struct SignalSemaphore lock;
#endif
    char *url;
    char *host;
    char *category;
    int port;
    int use_tls;
    unsigned long session_id;
    int fail_reason;
    unsigned long resolved_addr_be;
    RadioNetTransport *result;
} RadioNetOpenArgs;

/* Phase of the most recent RadioNet_Open() (RADIO_NET_OPEN_* from
 * radio_stream.h), plus the IP gethostbyname() resolved for it (0 if none).
 * Set from the per-request fields once the open the caller waited on has
 * completed; opens are serialized on the one net worker task, so a
 * NULL-getting caller reads the reason (and resolved IP) for its own open.
 * The resolved IP lets a probe put the actual host/IP/port it tried into the
 * on-screen error, which is the only diagnostic channel on a headless Amiga. */
static int radio_net_last_open_error = RADIO_NET_OPEN_OK;
static unsigned long radio_net_last_open_addr_be = 0;
int RadioNet_LastOpenError(void) { return radio_net_last_open_error; }
unsigned long RadioNet_LastOpenAddr(void) { return radio_net_last_open_addr_be; }

typedef struct RadioNetIoArgs {
    int state;
#if defined(AMIGA_M68K)
    struct SignalSemaphore lock;
#endif
    RadioNetTransport *transport;
    char *buffer;
    int length;
    int result;
    int graceful;
} RadioNetIoArgs;

enum {
    RADIO_NET_REQ_PENDING = 0,
    RADIO_NET_REQ_COMPLETED = 1,
    RADIO_NET_REQ_ABANDONED = 2
};

static void radio_net_open_req_init(RadioNetOpenArgs *a)
{
    if (!a) return;
    a->state = RADIO_NET_REQ_PENDING;
#if defined(AMIGA_M68K)
    InitSemaphore(&a->lock);
#endif
}

static void radio_net_io_req_init(RadioNetIoArgs *a)
{
    if (!a) return;
    a->state = RADIO_NET_REQ_PENDING;
#if defined(AMIGA_M68K)
    InitSemaphore(&a->lock);
#endif
}

static void radio_net_open_req_lock(RadioNetOpenArgs *a)
{
#if defined(AMIGA_M68K)
    if (a) ObtainSemaphore(&a->lock);
#else
    (void)a;
#endif
}

static void radio_net_open_req_unlock(RadioNetOpenArgs *a)
{
#if defined(AMIGA_M68K)
    if (a) ReleaseSemaphore(&a->lock);
#else
    (void)a;
#endif
}

static void radio_net_io_req_lock(RadioNetIoArgs *a)
{
#if defined(AMIGA_M68K)
    if (a) ObtainSemaphore(&a->lock);
#else
    (void)a;
#endif
}

static void radio_net_io_req_unlock(RadioNetIoArgs *a)
{
#if defined(AMIGA_M68K)
    if (a) ReleaseSemaphore(&a->lock);
#else
    (void)a;
#endif
}

static char *radio_net_strdup(const char *s)
{
    char *copy;
    size_t n;
    if (!s) s = "";
    n = strlen(s) + 1;
    copy = (char *)radio_net_alloc_raw(n);
    if (copy) memcpy(copy, s, n);
    return copy;
}

static void radio_net_open_args_free(RadioNetOpenArgs *a)
{
    if (!a) return;
    radio_net_free(a->url);
    radio_net_free(a->host);
    radio_net_free(a->category);
    radio_net_free(a);
}

static void radio_net_io_args_free(RadioNetIoArgs *a)
{
    if (!a) return;
    radio_net_free(a->buffer);
    radio_net_free(a);
}

static void radio_net_transport_count_socket(RadioNetTransport *t)
{
    if (t && !t->socket_counted) { radio_open_socket_count++; t->socket_counted = 1; }
}

static void radio_net_transport_count_ssl(RadioNetTransport *t)
{
    if (t && !t->ssl_counted) { radio_active_ssl_count++; t->ssl_counted = 1; }
}

static void radio_net_open_complete(RadioNetOpenArgs *a, RadioNetTransport *t)
{
    int abandoned;
    if (!a) { if (t) RadioNet_Close(t, 0); return; }
    radio_net_open_req_lock(a);
    abandoned = (a->state == RADIO_NET_REQ_ABANDONED);
    if (!abandoned) {
        a->result = t;
        a->state = RADIO_NET_REQ_COMPLETED;
    }
    radio_net_open_req_unlock(a);
    if (abandoned) {
        if (t) radio_net_close_transport_worker(t, 0);
        radio_net_open_args_free(a);
    }
}

static void radio_net_io_complete(RadioNetIoArgs *a)
{
    int abandoned;
    if (!a) return;
    radio_net_io_req_lock(a);
    abandoned = (a->state == RADIO_NET_REQ_ABANDONED);
    if (!abandoned)
        a->state = RADIO_NET_REQ_COMPLETED;
    radio_net_io_req_unlock(a);
    if (abandoned)
        radio_net_io_args_free(a);
}

static void radio_net_open_worker(void *arg)
{
    RadioNetOpenArgs *a = (RadioNetOpenArgs *)arg;
    RadioNetTransport *t;
    const struct hostent *he;
    struct sockaddr_in sa;
    if (!radio_net_worker_is_self()) {
        RADIO_DBG(printf("radio-net: open refused off worker\n");)
        if (a) { a->fail_reason = RADIO_NET_OPEN_ERR_OTHER; radio_net_open_complete(a, NULL); }
        return;
    }
    if (!a || !a->host) { if (a) { a->fail_reason = RADIO_NET_OPEN_ERR_OTHER; radio_net_open_complete(a, NULL); } return; }
    a->fail_reason = RADIO_NET_OPEN_ERR_OTHER;
    t = (RadioNetTransport *)radio_net_alloc0(sizeof(*t));
    if (!t) { a->fail_reason = RADIO_NET_OPEN_ERR_SOCKET; radio_net_open_complete(a, NULL); return; }
    t->sock = RADIO_INVALID_SOCKET;
    t->use_tls = a->use_tls;
    t->session_id = a->session_id;
    radio_copy_string(t->category, sizeof(t->category), a->category ? a->category : "transport");
    he = gethostbyname((char *)a->host);
    if (!he || !he->h_addr_list || !he->h_addr_list[0]) { a->fail_reason = RADIO_NET_OPEN_ERR_DNS; radio_net_free(t); radio_net_open_complete(a, NULL); return; }
    memcpy(&t->host_addr_be, he->h_addr_list[0], 4);
    memcpy(&a->resolved_addr_be, he->h_addr_list[0], 4);
    t->sock = socket(AF_INET, SOCK_STREAM, 0);
    if (t->sock == RADIO_INVALID_SOCKET) { a->fail_reason = RADIO_NET_OPEN_ERR_SOCKET; radio_net_free(t); radio_net_open_complete(a, NULL); return; }
    radio_net_transport_count_socket(t);
    radio_set_nonblocking(t->sock);
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)a->port);
    memcpy(&sa.sin_addr, he->h_addr_list[0], 4);
    if (connect(t->sock, (struct sockaddr *)&sa, sizeof(sa)) < 0 &&
        radio_net_transport_wait_connected(t, &sa) != 0) {
        a->fail_reason = RADIO_NET_OPEN_ERR_CONNECT;
        radio_net_close_transport_worker(t, 0);
        radio_net_open_complete(a, NULL);
        return;
    }
    if (a->use_tls) {
        long so_error = 0;
        if (radio_socket_connect_error(t->sock, &so_error) != 0) {
            radio_trace("radio-net-tcp: transport connected socket check failed session=%lu socket_error=%ld fd=%ld\n",
                t->session_id, so_error, (long)t->sock);
            a->fail_reason = RADIO_NET_OPEN_ERR_CONNECT;
            radio_net_close_transport_worker(t, 0);
            radio_net_open_complete(a, NULL);
            return;
        }
    }
    if (a->use_tls && radio_net_transport_tls_connect(t, a->host) != 0) {
        a->fail_reason = RADIO_NET_OPEN_ERR_TLS;
        radio_net_close_transport_worker(t, 0);
        radio_net_open_complete(a, NULL);
        return;
    }
    a->fail_reason = RADIO_NET_OPEN_OK;
    radio_net_open_complete(a, t);
}

RadioNetTransport *RadioNet_Open(const char *url, const char *host, int port, int use_tls, const char *category, unsigned long session_id)
{
    RadioNetOpenArgs *args;
    RadioNetTransport *result;
    radio_net_last_open_error = RADIO_NET_OPEN_ERR_OTHER;
    radio_net_last_open_addr_be = 0;
    args = (RadioNetOpenArgs *)radio_net_alloc0(sizeof(*args));
    if (!args) return NULL;
    args->url = radio_net_strdup(url ? url : "");
    args->host = radio_net_strdup(host ? host : "");
    args->category = radio_net_strdup(category ? category : "transport");
    args->port = port;
    args->use_tls = use_tls;
    args->session_id = session_id;
    args->fail_reason = RADIO_NET_OPEN_ERR_OTHER;
    if (!args->host || !args->category || (url && !args->url)) { radio_net_open_args_free(args); return NULL; }
    radio_net_open_req_init(args);
    if (radio_net_worker_is_self()) {
        radio_net_open_worker(args);
    } else if (!Radio_RunOnNetWorker(radio_net_open_worker, args)) {
        radio_net_open_req_lock(args);
        if (args->state == RADIO_NET_REQ_COMPLETED) {
            result = args->result;
            radio_net_last_open_error = args->fail_reason;
            radio_net_last_open_addr_be = args->resolved_addr_be;
            radio_net_open_req_unlock(args);
            radio_net_open_args_free(args);
            return result;
        }
        args->state = RADIO_NET_REQ_ABANDONED;
        radio_net_last_open_error = RADIO_NET_OPEN_ERR_OTHER;
        radio_net_last_open_addr_be = 0;
        radio_net_open_req_unlock(args);
        return NULL;
    }
    radio_net_open_req_lock(args);
    result = args->result;
    radio_net_last_open_error = args->fail_reason;
    radio_net_last_open_addr_be = args->resolved_addr_be;
    radio_net_open_req_unlock(args);
    radio_net_open_args_free(args);
    return result;
}

static void radio_net_write_worker(void *arg)
{
    RadioNetIoArgs *a = (RadioNetIoArgs *)arg;
    RadioNetTransport *t;
    int done = 0, tries = 0;
    if (!radio_net_worker_is_self()) { RADIO_DBG(printf("radio-net: write refused off worker\n");) if (a) { a->result = -1; radio_net_io_complete(a); } return; }
    if (!a || !a->transport || !a->buffer || a->length < 0) { if (a) { a->result = -1; radio_net_io_complete(a); } return; }
    t = a->transport;
    while (done < a->length && tries < 250) {
        int n;
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
        if (t->use_tls && t->ssl) {
            int write_ret;
            ERR_clear_error(); /* clean queue so ERR_get_error() below reflects only this SSL_write: a bare SYSCALL/EPIPE drop must stay lib_error=0 and quarantine, not poison */
            radio_amissl_lifecycle_diag("SSL_write-before", t->session_id, t->ssl, t->ctx);
            n = (int)SSL_write(t->ssl, a->buffer + done, a->length - done);
            radio_amissl_lifecycle_diag("SSL_write-after", t->session_id, t->ssl, t->ctx);
            if (n > 0) { done += n; continue; }
            write_ret = n; /* preserve raw SSL_write return before SSL_get_error() overwrites n */
            n = SSL_get_error(t->ssl, n);
            if (n == SSL_ERROR_WANT_READ || n == SSL_ERROR_WANT_WRITE) { radio_backoff_sleep(); tries++; continue; }
            if (radio_ssl_error_is_fatal(n)) {
                unsigned long le = ERR_get_error();
                radio_trace("radio-net-tls: transport SSL_write failed category=%s session=%lu ret=%d ssl_error=%d lib_error=%08lx socket_error=%ld fd=%ld\n",
                    t->category, t->session_id, write_ret, n, le, radio_sock_errno(), (long)t->sock);
                if (radio_tls_fault_should_poison(n, le)) {
                    t->ssl_poisoned = 1;
                    Radio_ReportTlsFault("transport fatal TLS write");
                } else {
                    t->ssl_dropped_transport = 1; /* leak SSL at cleanup, don't poison instance */
                    radio_trace("radio-net: transport TLS write peer-close (ssl_error=%d lib_error=0) session=%lu -- quarantined, shared AmiSSL NOT poisoned\n", n, t->session_id);
                }
                ERR_clear_error();
            }
            a->result = -1; radio_net_io_complete(a); return;
        }
#endif
        n = (int)send(t->sock, a->buffer + done, a->length - done, 0);
        if (n > 0) { done += n; continue; }
        if (n < 0 && radio_would_block()) { radio_backoff_sleep(); tries++; continue; }
        a->result = -1; radio_net_io_complete(a); return;
    }
    a->result = (done == a->length) ? 0 : -1;
    radio_net_io_complete(a);
}

int RadioNet_Write(RadioNetTransport *t, const void *buffer, int length)
{
    RadioNetIoArgs *args;
    int result;
    if (!t || !buffer || length < 0) return -1;
    args = (RadioNetIoArgs *)radio_net_alloc0(sizeof(*args));
    if (!args) return -1;
    radio_net_io_req_init(args);
    args->transport = t;
    args->length = length;
    if (length > 0) {
        args->buffer = (char *)radio_net_alloc_raw((size_t)length);
        if (!args->buffer) { radio_net_io_args_free(args); return -1; }
        memcpy(args->buffer, buffer, (size_t)length);
    }
    if (radio_net_worker_is_self()) radio_net_write_worker(args);
    else if (!Radio_RunOnNetWorker(radio_net_write_worker, args)) {
        radio_net_io_req_lock(args);
        if (args->state == RADIO_NET_REQ_COMPLETED) { result = args->result; radio_net_io_req_unlock(args); radio_net_io_args_free(args); return result; }
        args->state = RADIO_NET_REQ_ABANDONED;
        radio_net_io_req_unlock(args);
        return -1;
    }
    radio_net_io_req_lock(args);
    result = args->result;
    radio_net_io_req_unlock(args);
    radio_net_io_args_free(args);
    return result;
}

static void radio_net_read_worker(void *arg)
{
    RadioNetIoArgs *a = (RadioNetIoArgs *)arg;
    RadioNetTransport *t;
    int tries;
    if (!radio_net_worker_is_self()) { RADIO_DBG(printf("radio-net: read refused off worker\n");) if (a) { a->result = -1; radio_net_io_complete(a); } return; }
    if (!a || !a->transport || !a->buffer || a->length <= 0) { if (a) { a->result = -1; radio_net_io_complete(a); } return; }
    t = a->transport;
    for (tries = 0; tries < 250; tries++) {
        int n;
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
        if (t->use_tls && t->ssl) {
            ERR_clear_error(); /* clean queue so ERR_get_error() below reflects only this SSL_read: a bare SYSCALL/EPIPE drop must stay lib_error=0 and quarantine, not poison */
            radio_amissl_lifecycle_diag("SSL_read-before", t->session_id, t->ssl, t->ctx);
            n = (int)SSL_read(t->ssl, a->buffer, a->length);
            radio_amissl_lifecycle_diag("SSL_read-after", t->session_id, t->ssl, t->ctx);
            if (n > 0) { a->result = n; radio_net_io_complete(a); return; }
            n = SSL_get_error(t->ssl, n);
#ifdef SSL_ERROR_ZERO_RETURN
            if (n == SSL_ERROR_ZERO_RETURN) { a->result = 0; radio_net_io_complete(a); return; }
#endif
            if (n == SSL_ERROR_WANT_READ || n == SSL_ERROR_WANT_WRITE) { radio_backoff_sleep(); continue; }
            {
                unsigned long le = ERR_get_error();
                if (radio_tls_fault_should_poison(n, le)) {
                    t->ssl_poisoned = 1;
                    Radio_ReportTlsFault("transport fatal TLS read");
                } else {
                    t->ssl_dropped_transport = 1; /* leak SSL at cleanup, don't poison instance */
                    radio_trace("radio-net: transport TLS read peer-close (ssl_error=%d lib_error=0) session=%lu -- quarantined, shared AmiSSL NOT poisoned\n", n, t->session_id);
                }
                ERR_clear_error();
            }
            a->result = -1;
            radio_net_io_complete(a);
            return;
        }
#endif
        n = (int)recv(t->sock, a->buffer, a->length, 0);
        if (n >= 0) { a->result = n; radio_net_io_complete(a); return; }
        if (!radio_would_block()) { a->result = -1; radio_net_io_complete(a); return; }
        radio_backoff_sleep();
    }
    a->result = -1;
    radio_net_io_complete(a);
}

int RadioNet_Read(RadioNetTransport *t, void *buffer, int length)
{
    RadioNetIoArgs *args;
    int result;
    if (!t || !buffer || length <= 0) return -1;
    args = (RadioNetIoArgs *)radio_net_alloc0(sizeof(*args));
    if (!args) return -1;
    radio_net_io_req_init(args);
    args->transport = t;
    args->length = length;
    args->buffer = (char *)radio_net_alloc_raw((size_t)length);
    if (!args->buffer) { radio_net_io_args_free(args); return -1; }
    args->result = -1;
    if (radio_net_worker_is_self()) radio_net_read_worker(args);
    else if (!Radio_RunOnNetWorker(radio_net_read_worker, args)) {
        radio_net_io_req_lock(args);
        if (args->state == RADIO_NET_REQ_COMPLETED) {
            result = args->result;
            if (result > 0) memcpy(buffer, args->buffer, (size_t)result);
            radio_net_io_req_unlock(args);
            radio_net_io_args_free(args);
            return result;
        }
        args->state = RADIO_NET_REQ_ABANDONED;
        radio_net_io_req_unlock(args);
        return -1;
    }
    radio_net_io_req_lock(args);
    result = args->result;
    if (result > 0) memcpy(buffer, args->buffer, (size_t)result);
    radio_net_io_req_unlock(args);
    radio_net_io_args_free(args);
    return result;
}

static void radio_net_close_transport_worker(RadioNetTransport *t, int graceful)
{
    if (!radio_net_worker_is_self()) { RADIO_DBG(printf("radio-net: close refused off worker\n");) return; }
    if (!t) return;
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    /* Healthy graceful teardown frees the per-connection SSL with NO network
     * I/O while the descriptor is still valid, mirroring the playback path
     * (radio_ssl_free_before_socket_close()). The previous readiness-driven
     * SSL_shutdown() loop here did socket I/O during teardown and hard-locked
     * real CD32 hardware on station switches. Fatal/dropped/poisoned/diag-leak
     * sessions fall through to the quarantine handling below with t->ssl set. */
    if (graceful && t->ssl && t->handshake_done && !t->ssl_poisoned &&
        !t->ssl_dropped_transport && !t->ssl_teardown_unsafe &&
        !Radio_IsMemoryPoisoned() && !Radio_IsTlsPoisoned() &&
        !radio_runtime_diag_leak_ssl_enabled() &&
        t->sock != RADIO_INVALID_SOCKET) {
        BIO *rbio = SSL_get_rbio(t->ssl);
        BIO *wbio = SSL_get_wbio(t->ssl);
        SSL_set_shutdown(t->ssl, SSL_SENT_SHUTDOWN);
        if (rbio) BIO_set_close(rbio, BIO_NOCLOSE);
        if (wbio && wbio != rbio) BIO_set_close(wbio, BIO_NOCLOSE);
        radio_trace("radio-tls-order: category=%s session=%lu step=SSL_free-begin ssl=%p fd=%ld\n",
            t->category, t->session_id, (void *)t->ssl, (long)t->sock);
        radio_amissl_lifecycle_diag("SSL_free-before", t->session_id, t->ssl, t->ctx);
        SSL_free(t->ssl);
        radio_amissl_lifecycle_diag("SSL_free-after", t->session_id, t->ssl, t->ctx);
        radio_trace("radio-tls-order: category=%s session=%lu step=SSL_free-complete\n",
            t->category, t->session_id);
        t->ssl = NULL;
        if (t->ssl_counted && radio_active_ssl_count > 0) radio_active_ssl_count--;
        t->ssl_counted = 0;
    }
#endif
    if (t->sock != RADIO_INVALID_SOCKET) {
        radio_trace("radio-tls-order: category=%s session=%lu step=CloseSocket-begin fd=%ld\n", t->category, t->session_id, (long)t->sock);
        radio_close_socket(t->sock);
        radio_trace("radio-tls-order: category=%s session=%lu step=CloseSocket-complete\n", t->category, t->session_id);
        t->sock = RADIO_INVALID_SOCKET;
        if (t->socket_counted && radio_open_socket_count > 0) radio_open_socket_count--;
        t->socket_counted = 0;
    }
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    if (t->ssl) {
        if (t->ssl_poisoned || Radio_IsMemoryPoisoned() || Radio_IsTlsPoisoned()) {
            Radio_MarkWorkerSslCtxPoisoned("transport close fatal SSL quarantined");
            t->ssl = NULL;
        } else if (t->ssl_dropped_transport || t->ssl_teardown_unsafe) {
            /* Peer dropped the connection: leak this SSL (freeing one that took
             * an I/O fault is what caused AN_BadFreeAddr) but do NOT poison the
             * shared instance -- the shared SSL_CTX is untouched by a transport
             * EOF, so the next HTTPS attempt must stay allowed. */
            radio_trace("radio-net: category=%s session=%lu ssl=%p action=%s\n",
                t->category, t->session_id, (void *)t->ssl,
                t->ssl_teardown_unsafe ? "teardown-quarantine-no-poison" : "transport-drop-quarantine-no-poison");
            t->ssl = NULL;
        } else if (radio_runtime_diag_leak_ssl_enabled()) {
            radio_trace("radio-diag-leak: category=%s session=%lu ssl=%p action=quarantined-no-SSL_free\n", t->category, t->session_id, (void *)t->ssl);
            t->ssl = NULL;
        } else {
            radio_trace("radio-tls-order: category=%s session=%lu step=SSL_free-begin ssl=%p\n", t->category, t->session_id, (void *)t->ssl);
            radio_amissl_lifecycle_diag("SSL_free-before", t->session_id, t->ssl, t->ctx);
            SSL_free(t->ssl);
            radio_amissl_lifecycle_diag("SSL_free-after", t->session_id, t->ssl, t->ctx);
            radio_trace("radio-tls-order: category=%s session=%lu step=SSL_free-complete\n", t->category, t->session_id);
            t->ssl = NULL;
            if (t->ssl_counted && radio_active_ssl_count > 0) radio_active_ssl_count--;
            t->ssl_counted = 0;
        }
    }
    if (t->ctx) {
        /* Borrowed shared worker SSL_CTX: retained for the whole run and freed
         * only at worker shutdown -- never SSL_CTX_free'd per transport. */
        t->ctx = NULL;
    }
#endif
    radio_net_free(t);
}

static void radio_net_close_worker(void *arg)
{
    RadioNetIoArgs *a = (RadioNetIoArgs *)arg;
    RadioNetTransport *t = a ? a->transport : NULL;
    int graceful = a ? a->graceful : 0;
    radio_net_close_transport_worker(t, graceful);
    if (a) {
        a->transport = NULL;
        a->result = 0;
        radio_net_io_complete(a);
    }
}

void RadioNet_Close(RadioNetTransport *t, int graceful)
{
    RadioNetIoArgs *args;
    if (!t) return;
    if (radio_net_worker_is_self()) {
        radio_net_close_transport_worker(t, graceful);
        return;
    }
    args = (RadioNetIoArgs *)radio_net_alloc0(sizeof(*args));
    if (!args) {
        Radio_MarkTlsPoisoned("RadioNet_Close request allocation failed");
        return;
    }
    radio_net_io_req_init(args);
    args->transport = t;
    args->graceful = graceful;
    if (!Radio_RunOnNetWorker(radio_net_close_worker, args)) {
        radio_net_io_req_lock(args);
        if (args->state == RADIO_NET_REQ_COMPLETED) {
            radio_net_io_req_unlock(args);
            radio_net_io_args_free(args);
            return;
        }
        args->state = RADIO_NET_REQ_ABANDONED;
        radio_net_io_req_unlock(args);
        Radio_MarkTlsPoisoned("RadioNet_Close dispatch timed out");
        return;
    }
    radio_net_io_args_free(args);
}

unsigned long RadioNet_HostAddr(RadioNetTransport *t)
{
    return t ? t->host_addr_be : 0;
}

static void set_status(RadioStream *rs, RadioStatus status) { if (rs) { radio_stream_lock(rs); if (rs->status != RADIO_STATUS_ERROR && rs->status != RADIO_STATUS_CLOSED && rs->status != RADIO_STATUS_STOPPING) rs->status = status; radio_stream_unlock(rs); } }

static void radio_copy_metadata_string(char *dst, size_t dstSize, const char *src, const char *label)
{
    size_t rawLen, safeLen;
    if (!dst || dstSize == 0)
        return;
    if (!src)
        src = "";
    rawLen = strlen(src);
    safeLen = AmigaUtf8ToDisplay(dst, dstSize, src);
    RADIO_DBG(printf("radio-icy: sanitized %s rawLen=%lu sanitizedLen=%lu dstSize=%lu%s\n",
        label ? label : "metadata", (unsigned long)rawLen, (unsigned long)safeLen,
        (unsigned long)dstSize, (rawLen && safeLen + 1 >= dstSize) ? " truncated" : ""););
}

static void radio_copy_metadata_bytes(char *dst, size_t dstSize, const unsigned char *src, int srcLen, const char *label)
{
    size_t rawLen, safeLen;
    char raw[RADIO_META_TEXT_MAX];
    if (!dst || dstSize == 0)
        return;
    rawLen = (src && srcLen > 0) ? (size_t)srcLen : 0;
    if (rawLen >= sizeof(raw))
        rawLen = sizeof(raw) - 1;
    if (src && rawLen > 0)
        memcpy(raw, src, rawLen);
    raw[rawLen] = 0;
    safeLen = AmigaUtf8ToDisplay(dst, dstSize, raw);
    RADIO_DBG(printf("radio-icy: sanitized %s rawLen=%lu sanitizedLen=%lu dstSize=%lu%s\n",
        label ? label : "metadata", (unsigned long)((src && srcLen > 0) ? (size_t)srcLen : 0), (unsigned long)safeLen,
        (unsigned long)dstSize, (src && srcLen > 0 && safeLen + 1 >= dstSize) ? " truncated" : ""););
}

static void radio_copy_string(char *dst, size_t dstSize, const char *src)
{
    if (!dst || dstSize == 0)
        return;
    if (!src)
        src = "";
    if (strlen(src) >= dstSize)
        RADIO_DBG(printf("radio-string: truncated copy dstSize=%lu srcLen=%lu src=\"%s\"\n", (unsigned long)dstSize, (unsigned long)strlen(src), src););
    snprintf(dst, dstSize, "%s", src);
    dst[dstSize - 1] = 0;
}

static void radio_copy_bytes(char *dst, size_t dstSize, const unsigned char *src, int srcLen)
{
    size_t copyLen;
    if (!dst || dstSize == 0)
        return;
    dst[0] = 0;
    if (!src || srcLen <= 0)
        return;
    copyLen = (size_t)srcLen;
    if (copyLen >= dstSize) {
        RADIO_DBG(printf("radio-string: truncated byte copy dstSize=%lu srcLen=%lu\n", (unsigned long)dstSize, (unsigned long)copyLen););
        copyLen = dstSize - 1;
    }
    memcpy(dst, src, copyLen);
    dst[copyLen] = 0;
}

static void set_error(RadioStream *rs, const char *msg) { if (rs) { radio_stream_lock(rs); radio_copy_string(rs->error,sizeof(rs->error),msg); rs->status = RADIO_STATUS_ERROR; radio_stream_unlock(rs); RADIO_OPEN_DEBUG_PRINTF(("radio-open: %s\n", msg ? msg : "error")); } }
static void radio_ring_set_canary(RadioStream *rs);
static int radio_ring_check_canary(RadioStream *rs, const char *where);
static int radio_ring_validate_locked(RadioStream *rs, const char *where)
{
    if (!rs || !rs->ring || !rs->size) return -1;
    if (rs->used > rs->size || rs->rpos >= rs->size || rs->wpos >= rs->size) {
        RADIO_DBG(printf("radio-ring: INVALID state session=%lu generation=%lu where=%s fill=%lu capacity=%lu rpos=%lu wpos=%lu closing=%d status=%d url=\"%s\"\n",
            rs->session_id, rs->generation, where ? where : "",
            rs->used, rs->size, rs->rpos, rs->wpos, rs->stopping, (int)rs->status, rs->url););
        rs->stopping = 1;
        rs->fatalStop = 1;
        rs->noReconnect = 1;
        rs->status = RADIO_STATUS_ERROR;
        radio_copy_string(rs->error, sizeof(rs->error), "Invalid radio ring state");
        Radio_MarkMemoryPoisoned(where);
        return -1;
    }
    return 0;
}

static int radio_session_current_locked(RadioStream *rs, const char *where)
{
    if (!rs) return 0;
    if (rs->generation != radio_current_generation || rs->stopping ||
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
        rs->workerClosing || rs->workerCloseRequested || rs->workerDetached ||
#endif
        rs->status == RADIO_STATUS_STOPPING || rs->status == RADIO_STATUS_CLOSED ||
        rs->status == RADIO_STATUS_ERROR) {
        RADIO_DBG(printf("radio-session: stale/closing access refused session=%lu generation=%lu current=%lu where=%s closing=%d status=%d\n",
            rs->session_id, rs->generation, radio_current_generation,
            where ? where : "", rs->stopping, (int)rs->status););
        return 0;
    }
    return 1;
}

static int ring_write(RadioStream *rs, const unsigned char *p, int n)
{
    unsigned long bytes, freeBytes, first, second, beforeWpos, beforeUsed;
    if (!rs || !p || n <= 0 || !rs->ring || !rs->size) return 0;
    if (radio_ring_check_canary(rs, "before ring_write") < 0) return 0;
    radio_stream_lock(rs);
    if (!radio_session_current_locked(rs, "ring_write") ||
        radio_ring_validate_locked(rs, "ring_write entry") < 0) {
        radio_stream_unlock(rs);
        return 0;
    }
    freeBytes = rs->size - rs->used;
    bytes = (unsigned long)n;
    if (bytes > freeBytes) bytes = freeBytes;
    beforeWpos = rs->wpos;
    beforeUsed = rs->used;
    if (bytes == 0) {
        rs->ringLastWrite = 0;
        RADIO_DBG(printf("radio-ring: no write session=%lu icy_state=%d metaint=%d requested=%d fill=%lu ring_free=%lu wpos=%lu capacity=%lu url=\"%s\"\n",
            rs->session_id, (int)rs->parseState, rs->metaint, n, rs->used,
            rs->size > rs->used ? rs->size - rs->used : 0, rs->wpos, rs->size, rs->url););
        radio_stream_unlock(rs);
        return 0;
    }
    first = bytes;
    if (first > rs->size - rs->wpos) first = rs->size - rs->wpos;
    second = bytes - first;
    if (first > rs->size - beforeWpos || second > beforeWpos ||
        first + second != bytes || bytes > freeBytes) {
        RADIO_DBG(printf("radio-ring: INVALID write plan session=%lu generation=%lu bytes=%lu free=%lu first=%lu second=%lu wpos=%lu size=%lu\n",
            rs->session_id, rs->generation, bytes, freeBytes, first, second, beforeWpos, rs->size););
        rs->stopping = 1; rs->fatalStop = 1; rs->noReconnect = 1; rs->status = RADIO_STATUS_ERROR;
        radio_stream_unlock(rs);
        return 0;
    }
    memcpy(rs->ring + rs->wpos, p, (size_t)first);
    if (second) memcpy(rs->ring, p + first, (size_t)second);
    rs->wpos = (rs->wpos + bytes) % rs->size;
    rs->used += bytes;
    rs->ringLastWrite = bytes;
    RADIO_DBG(printf("radio-ring: write session=%lu generation=%lu icy_state=%d metaint=%d bytes_to_ring=%lu first=%lu second=%lu wpos_before=%lu wpos_after=%lu fill_before=%lu fill_after=%lu free_before=%lu free_after=%lu wrapped=%d url=\"%s\"\n",
        rs->session_id, rs->generation, (int)rs->parseState, rs->metaint, bytes, first, second,
        beforeWpos, rs->wpos, beforeUsed, rs->used, freeBytes, rs->size - rs->used, second ? 1 : 0, rs->url););
    radio_stream_unlock(rs);
    radio_ring_check_canary(rs, "after ring_write");
    return (int)bytes;
}
static int ring_read(RadioStream *rs, unsigned char *p, int n) { int i=0; if(!rs||!p||n<=0)return 0; radio_stream_lock(rs); if(!radio_session_current_locked(rs, "ring_read") || radio_ring_validate_locked(rs, "ring_read entry") < 0 || !rs->headerDone||!rs->decoderStarted){ RADIO_DBG(printf("radio-guard: ring_read refused session=%lu headerDone=%d decoderStarted=%d firstData=%d status=%d used=%lu\n", rs->session_id, rs->headerDone, rs->decoderStarted, rs->firstDataLogged, (int)rs->status, rs->used);); radio_stream_unlock(rs); return 0; } radio_stream_unlock(rs); if (radio_ring_check_canary(rs, "before ring_read") < 0) return 0; radio_stream_lock(rs); if (radio_ring_validate_locked(rs, "ring_read locked") < 0) { radio_stream_unlock(rs); return 0; } while (i<n && rs->used) { p[i++]=rs->ring[rs->rpos++]; if(rs->rpos>=rs->size)rs->rpos=0; rs->used--; } radio_stream_unlock(rs); radio_ring_check_canary(rs, "after ring_read"); return i; }
static int ci_starts(const char *s,const char *p){ while(*p) { if(tolower((unsigned char)*s++)!=tolower((unsigned char)*p++)) return 0; } return 1; }
static int ci_equals(const char *a,const char *b){ while(*a&&*b){ if(tolower((unsigned char)*a++)!=tolower((unsigned char)*b++)) return 0; } return *a==0&&*b==0; }
static char *trim(char *s){ char *e; while(*s&&isspace((unsigned char)*s))s++; e=s+strlen(s); while(e>s&&isspace((unsigned char)e[-1]))*--e=0; return s; }

#define RADIO_RING_CANARY 0x5A
#define RADIO_RING_GUARD_BYTES 16UL
static void radio_ring_set_canary(RadioStream *rs)
{
    unsigned long i;
    if (!rs || !rs->ringAlloc || !rs->size) return;
    for (i = 0; i < RADIO_RING_GUARD_BYTES; i++) {
        rs->ringAlloc[i] = RADIO_RING_CANARY;
        rs->ringAlloc[RADIO_RING_GUARD_BYTES + rs->size + i] = RADIO_RING_CANARY;
    }
}
static int radio_ring_check_canary(RadioStream *rs, const char *where)
{
    unsigned long i;
    int bad = 0;
    if (!rs || !rs->ringAlloc || !rs->size) return 0;
    for (i = 0; i < RADIO_RING_GUARD_BYTES; i++) {
        if (rs->ringAlloc[i] != RADIO_RING_CANARY ||
            rs->ringAlloc[RADIO_RING_GUARD_BYTES + rs->size + i] != RADIO_RING_CANARY) {
            bad = 1;
            break;
        }
    }
    if (bad) {
        radio_poison_session_id = rs->session_id;
        radio_copy_string(radio_poison_url, sizeof(radio_poison_url), rs->url);
        RADIO_DBG(printf("radio-canary: CORRUPTED buffer=stream/audio ring session=%lu where=%s expected_capacity=%lu last_write_size=%lu codec=%s url=\"%s\"\n",
            rs->session_id, where ? where : "", rs->size, rs->ringLastWrite,
            ((ci_starts(rs->contentType,"audio/aac") || ci_starts(rs->contentType,"audio/aacp") || radio_contains_nocase(rs->path,"aac")) ? "AAC" : "MP3/unknown"),
            rs->url));
        rs->stopping = 1;
        rs->status = RADIO_STATUS_ERROR;
        set_error(rs, "Memory corruption detected - restart app");
        Radio_MarkMemoryPoisoned(where);
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
        /* One soak-test session hit this with no SSL_ERROR_SSL involved at
         * all -- a perfectly healthy, actively-streaming connection whose
         * ring buffer was found corrupted mid-stream, which later crashed
         * inside an unprotected SSL_CTX_free() because nothing had marked
         * this session poisoned. Any detected ring corruption, regardless
         * of how it got there, means AmiSSL-adjacent heap state for this
         * session may already be damaged -- so treat it exactly like the
         * SSL_ERROR_SSL case and skip further SSL_free()/SSL_CTX_free()/
         * CleanupAmiSSL() calls for it. */
        rs->sslStatePoisoned = 1;
        rs->fatalStop = 1;
        rs->noReconnect = 1;
        rs->lastSslError = 0;
        strcpy(rs->lastSslOp, "ring-corruption");
        radio_amissl_task_poisoned = 1;
        strcpy(radio_amissl_task_poison_reason, "ring-corruption");
        Radio_MarkTlsPoisoned("ring corruption during AmiSSL session");
#endif
        return -1;
    }
    return 0;
}

static int parse_url(RadioStream *rs, const char *url)
{
    const char *p, *slash, *colon;
    int hl, default_port;
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    if (!url) return -1;
    if (strncmp(url, "https://", 8) == 0) {
        rs->isSSL = 1; default_port = 443; p = url + 8;
    } else if (strncmp(url, "http://", 7) == 0) {
        rs->isSSL = 0; default_port = 80; p = url + 7;
    } else {
        return -1;
    }
#else
    if (!url || strncmp(url, "http://", 7)) return -1;
    default_port = 80; p = url + 7;
#endif
    slash = strchr(p, '/');
    if (!slash) slash = p + strlen(p);
    colon = (const char *)memchr(p, ':', (size_t)(slash - p));
    hl = (int)((colon ? colon : slash) - p);
    if (hl <= 0 || hl >= (int)sizeof(rs->host)) return -1;
    memcpy(rs->host, p, (size_t)hl); rs->host[hl] = 0;
    rs->port = colon ? atoi(colon + 1) : default_port;
    if (rs->port <= 0) rs->port = default_port;
    if (*slash && strlen(slash) >= sizeof(rs->path)) {
        /* Refuse rather than truncate: a cut-off path (usually the end of
         * an auth token) makes the server respond in ways that have proven
         * fatal to AmiSSL's record parser. */
        RADIO_DBG(printf("radio-open: path too long (%lu >= %lu) url=\"%s\"\n",
            (unsigned long)strlen(slash), (unsigned long)sizeof(rs->path), url));
        return -1;
    }
    radio_copy_string(rs->path, sizeof(rs->path), *slash ? slash : "/");
    radio_copy_string(rs->url, sizeof(rs->url), url);
    return 0;
}

static void reset_parser(RadioStream *rs)
{
    rs->headerDone = 0; rs->headerLen = 0; rs->header[0] = 0;
    rs->parseState = RADIO_PARSE_HEADER;
    rs->metaint = 0; rs->audioUntilMeta = 0;
    rs->metaLen = rs->metaGot = rs->metaLeft = 0;
    rs->rpos = rs->wpos = rs->used = 0;
    rs->zeroBytePumps = 0;
    rs->firstDataLogged = 0;
    rs->contentType[0] = 0; rs->bitrate = 0;
    rs->stationName[0] = 0; rs->genre[0] = 0; rs->streamUrl[0] = 0;
    rs->title[0] = 0;
}

static int connect_http(RadioStream *rs){
    struct sockaddr_in sa; char req[1024]; int n; int cr;
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    /* connect_http() only ever runs on the net worker task now (dispatched
     * via Radio_RunOnNetWorker() from Radio_OpenWithHostAddr()), so
     * SocketBase is already the worker's own, opened once at worker
     * start-up -- nothing left to open/adopt per station here. isSSL
     * streams additionally need the worker's AmiSSL instance; that is
     * checked inside radio_ssl_connect() below via
     * radio_net_worker_amissl_ready(). */
    RADIO_DBG(printf("radio-net-worker: session=%lu worker context active before DNS/socket/connect/TLS SocketBase=%p AmiSSLMasterBase=%p AmiSSLBase=%p AmiSSLExtBase=%p initialized=%d\n",
        rs->session_id, (void *)SocketBase, (void *)AmiSSLMasterBase, (void *)AmiSSLBase,
        (void *)AmiSSLExtBase, radio_amissl_initialized););
#elif defined(AMIGA_M68K)
    radio_net_adopt_context(rs);
#endif
#if defined(AMIGA_M68K)
    if(!SocketBase) SocketBase=OpenLibrary("bsdsocket.library",4); if(!SocketBase){ set_error(rs,"bsdsocket.library unavailable"); RADIO_OPEN_DEBUG_PRINTF(("radio-open: bsdsocket open failed\n")); return -1; }
#endif
    if (radio_is_stopping(rs)) return -1;
    /* Resolve once and cache it; gethostbyname() is blocking and would freeze
     * the emulator on every reconnect if we re-resolved each time. */
    if(rs->haveHostAddr){
        unsigned long addr_be;
        char addr_text[16];
        memset(&addr_be, 0, sizeof(addr_be));
        memcpy(&addr_be, &rs->hostAddr.s_addr, sizeof(rs->hostAddr.s_addr));
        radio_format_ipv4_be(addr_be, addr_text, (int)sizeof(addr_text));
        RADIO_DBG(printf("radio-dns: session=%lu using cached probe DNS %s\n", rs->session_id, addr_text););
    } else {
        struct hostent *he;
        RADIO_DBG(printf("radio-child-net: before DNS session=%lu host=%s SocketBase=%p\n",
            rs->session_id, rs->host, (void *)SocketBase););
        RADIO_DBG(printf("radio-dns: WARNING blocking DNS lookup in playback child host=%s\n", rs->host););
#if defined(AMIGA_M68K)
        /* gethostbyname() is a genuinely blocking bsdsocket.library call with
         * no non-blocking mode of its own, unlike connect()/recv() elsewhere
         * in this file.  Without a break mask, the Stop button's
         * SIGBREAKF_CTRL_C (see StopPlayback() in both front ends) cannot
         * touch it: the task just sits inside the resolver until it answers
         * or times out, which some nameservers/stacks never do quickly --
         * exactly the "Stopping..." that never finishes when a stream is
         * asked to stop while it's still resolving its host (e.g. the user
         * switches stations again before the previous one finished
         * connecting).  SBTC_BREAKMASK tells bsdsocket.library which signals
         * should abort a blocking call early for the calling task.
         *
         * Scoped tightly to just this one call: it is cleared again
         * immediately below, win or lose.  Leaving it set would also apply
         * to every later blocking bsdsocket call this task makes, including
         * whatever AmiSSL does internally inside SSL_connect()/SSL_read() a
         * few lines below for HTTPS streams -- and AmiSSL's SSL_connect() is
         * already documented above (radio_net_worker_entry()) as sensitive to
         * exactly this kind of external signal interference, having
         * previously gone unresponsive to Stop when the surrounding signal
         * plumbing changed. Getting an unexpected EINTR-style abort deep
         * inside a library that was never written to expect one is a
         * plausible way to corrupt its state rather than cleanly cancel it,
         * so the mask must not outlive this single call.
         *
         * The mask alone only says *which* signal would abort this call --
         * nothing actually sends SIGBREAKF_CTRL_C to this (worker) task
         * otherwise, so a wedged resolver previously had nothing to react
         * to (see Radio_RequestStop() below, which now signals this task).
         * Clear any stale pending CTRL_C first: a signal left over from an
         * earlier stop request that arrived after that request's own
         * gethostbyname() call had already returned would otherwise sit
         * pending and immediately abort *this*, unrelated, later call
         * (e.g. the next station the user opens) the moment the mask goes
         * live below. Safe to clear here because this code always runs on
         * the worker task itself (radio_net_worker_is_self() is enforced
         * by every caller of connect_http()). */
        SetSignal(0, SIGBREAKF_CTRL_C);
        {
            /* SocketBaseTagList() with an array, not the SocketBaseTags()
             * varargs macro, which GCC 16's NDK inlines fail to compile. */
            struct TagItem mask_tags[2];
            mask_tags[0].ti_Tag = SBTM_SETVAL(SBTC_BREAKMASK);
            mask_tags[0].ti_Data = (ULONG)SIGBREAKF_CTRL_C;
            mask_tags[1].ti_Tag = TAG_DONE;
            mask_tags[1].ti_Data = 0;
            SocketBaseTagList(mask_tags);
            he=gethostbyname(rs->host);
            mask_tags[0].ti_Data = 0UL;
            SocketBaseTagList(mask_tags);
        }
#else
        he=gethostbyname(rs->host);
#endif
        if(!he || !he->h_addr){
            if (radio_is_stopping(rs)) { set_error(rs,"stopped"); return -1; }
            set_error(rs,"cannot resolve stream host"); RADIO_OPEN_DEBUG_PRINTF(("radio-open: DNS failed for %s\n", rs->host)); return -1;
        }
        memcpy(&rs->hostAddr, he->h_addr, sizeof(rs->hostAddr));
        rs->haveHostAddr=1;
    }
    if (radio_is_stopping(rs)) return -1;
    RADIO_DBG(printf("radio-child-net: before socket session=%lu host=%s SocketBase=%p\n",
        rs->session_id, rs->host, (void *)SocketBase););
    rs->sock=socket(AF_INET,SOCK_STREAM,0); if(rs->sock!=RADIO_INVALID_SOCKET){ rs->socketClosed = 0; rs->closeCleanupDone = 0; radio_open_socket_count++; radio_playback_open_socket_count++; RADIO_DBG(printf("radio-socket: playback socket opened session=%lu host=%s fd=%ld open_socket_count=%ld playback_open_socket_count=%ld\n", rs->session_id, rs->host, (long)rs->sock, radio_open_socket_count, radio_playback_open_socket_count);) } if(rs->sock==RADIO_INVALID_SOCKET){ radio_log_socket_failure(rs, "playback", "socket"); set_error(rs,"Cannot create socket - TCP stack may still be releasing previous stream"); return -1; }
    /* Go non-blocking BEFORE connect so the connect never stalls the machine. */
    radio_set_nonblocking(rs->sock);
    memset(&sa,0,sizeof(sa)); sa.sin_family=AF_INET; sa.sin_port=htons((unsigned short)rs->port); sa.sin_addr=rs->hostAddr;
    if (radio_is_stopping(rs)) { close_current_socket(rs); return -1; }
    RADIO_DBG(printf("radio-child-net: before connect session=%lu fd=%ld host=%s SocketBase=%p\n",
        rs->session_id, (long)rs->sock, rs->host, (void *)SocketBase););
    RADIO_DBG(printf("radio-connect: session=%lu initial connect() fd=%ld host=%s port=%d\n", rs->session_id, (long)rs->sock, rs->host, rs->port););
    cr=connect(rs->sock,(struct sockaddr*)&sa,sizeof(sa));
    RADIO_DBG(printf("radio-connect: session=%lu initial connect() cr=%d errno=%ld\n", rs->session_id, cr, cr < 0 ? radio_sock_errno() : 0L););
    if(cr<0 && radio_wait_connected(rs,&sa)!=0){ close_current_socket(rs); set_error(rs,"TCP connect failed"); RADIO_OPEN_DEBUG_PRINTF(("radio-open: connect failed to %s:%d\n", rs->host, rs->port)); return -1; }
    rs->streamStateFlags |= TRANSPORT_CONNECTED;
    if (radio_is_stopping(rs)) { close_current_socket(rs); return -1; }
    RADIO_DBG(printf("radio-connect: session=%lu TCP connected, starting SSL/HTTP request phase isSSL=%d\n", rs->session_id, rs->isSSL););
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    if (rs->isSSL) {
        long so_error = 0;
        if (radio_socket_connect_error(rs->sock, &so_error) != 0) {
            radio_trace("radio-tcp: connected socket check failed session=%lu socket_error=%ld fd=%ld\n",
                rs->session_id, so_error, (long)rs->sock);
            close_current_socket(rs); set_error(rs, "TCP connect failed"); return -1;
        }
        RADIO_DBG(printf("radio-child-net: before TLS session=%lu fd=%ld host=%s SocketBase=%p AmiSSLBase=%p AmiSSLExtBase=%p\n",
            rs->session_id, (long)rs->sock, rs->host, (void *)SocketBase,
            (void *)AmiSSLBase, (void *)AmiSSLExtBase););
        rs->sslHandshakeDone = 0;
        if (radio_ssl_connect(rs) != 0) { close_current_socket(rs); return -1; }
        rs->streamStateFlags |= TLS_DONE;
        if (radio_is_stopping(rs)) { close_current_socket(rs); return -1; }
    }
#endif
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    RADIO_DBG(printf("radio-http: session=%lu starting HTTP request phase isSSL=%d sslHandshakeDone=%d\n", rs->session_id, rs->isSSL, rs->sslHandshakeDone););
#else
    RADIO_DBG(printf("radio-http: session=%lu starting HTTP request phase isSSL=%d sslHandshakeDone=0\n", rs->session_id, rs->isSSL););
#endif
    n=snprintf(req,sizeof(req),"GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: BoingPlayer/0.1 AmigaOS\r\nIcy-MetaData: 1\r\nConnection: close\r\n\r\n",rs->path,rs->host);
    if(n<0 || n>=(int)sizeof(req)){
        /* Never send a truncated request: a cut-off auth token is exactly
         * what provoked the malformed server response behind the
         * "invalid record" AmiSSL fault chain. */
        close_current_socket(rs); set_error(rs,"stream URL too long"); return -1;
    }
    if(radio_send_all(rs,req,n)!=0){ close_current_socket(rs); set_error(rs, rs->isSSL ? "HTTPS read failed" : "cannot send HTTP request"); return -1; }
    reset_parser(rs);
    rs->decoderStarted = rs->playbackStarted = 0;
    rs->streamStateFlags &= (STREAM_ALLOCATED | TRANSPORT_CONNECTED | TLS_DONE);
    return 0;
}

#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
typedef struct RadioOpenJobArgs {
    RadioStream *rs;
    int result;
} RadioOpenJobArgs;

/* connect_http()/close_current_socket() touch bsdsocket.library/AmiSSL, so
 * they may only run on the net worker task -- Radio_OpenWithHostAddr() runs
 * on whichever task is starting this station (the playback child) and hands
 * this closure to Radio_RunOnNetWorker() instead of calling them directly. */
static void radio_worker_job_open(void *arg)
{
    RadioOpenJobArgs *a = (RadioOpenJobArgs *)arg;
    radio_worker_state = RADIO_WORKER_OPENING;
    /* Consume any SIGBREAKF_CTRL_C left pending on this persistent worker task
     * by an earlier session's Radio_RequestStop() wake-up Signal() (see the
     * Signal() comment there). That bit is only a mechanism to break a wedged
     * blocking call; the durable "this session is stopping" state lives in
     * rs->stopping / rs->workerStop* / rs->generation / the shared external
     * stop flag, all still honoured by the radio_is_stopping() checks in
     * connect_http() below. Left pending, the stale bit trips connect_http()'s
     * very first radio_is_stopping() check, which returns -1 without setting an
     * error -- surfacing as the generic "cannot open radio stream" -- and
     * because every probe-then-play caches the host address (rs->haveHostAddr),
     * connect_http() takes its cached-DNS path and never reaches its own later
     * SetSignal() clear, so the bit stays pending and dead-ends every following
     * open until an unrelated browser fetch happens to consume it (the "search
     * again to un-stick it" workaround). Only clear it for a genuinely fresh
     * open: if any durable stop indicator for this session is set, a real Stop
     * raced in and must still abort here, so leave the signal alone. */
    if (a && a->rs &&
        !a->rs->stopping && !a->rs->workerStopRequested &&
        !a->rs->workerCloseRequested && !a->rs->workerDetached &&
        a->rs->status != RADIO_STATUS_STOPPING &&
        a->rs->status != RADIO_STATUS_CLOSED &&
        a->rs->generation == radio_current_generation &&
        !(radio_external_stop_flag && *radio_external_stop_flag))
        SetSignal(0, SIGBREAKF_CTRL_C);
    a->result = connect_http(a->rs);
    if (a->result == 0) radio_worker_register_stream(a->rs);
    else { close_current_socket(a->rs); radio_worker_state = RADIO_WORKER_IDLE; }
}
#endif

static void radio_abort_current_socket_local(RadioStream *rs)
{
    if (!rs) return;
    radio_net_adopt_context(rs);
    radio_stream_magic_valid(rs, "radio_abort_current_socket");
    if (rs->sock != RADIO_INVALID_SOCKET && !rs->socketClosed) {
        long closing_fd = (long)rs->sock;
        long before_all = radio_open_socket_count;
        long before_playback = radio_playback_open_socket_count;
        radio_worker_risk_log("before CloseSocket", rs);
        RADIO_DBG(printf("radio-cleanup: active abort CloseSocket start session=%lu fd=%ld\n",
            rs->session_id, closing_fd););
        RADIO_CLEANUP_DEBUG_PRINTF(("radio-cleanup: abort CloseSocket start fd=%ld\n", (long)rs->sock));
        RADIO_DBG(printf("BEFORE CloseSocket session=%lu fd=%ld open_socket_count=%ld playback_open_socket_count=%ld\n",
            rs->session_id, (long)rs->sock, radio_open_socket_count,
            radio_playback_open_socket_count););
        rs->socket_close_count++;
        Radio_DebugCheckExecMem("before playback CloseSocket");
        radio_trace("radio-tls-order: category=playback session=%lu step=CloseSocket-begin fd=%ld\n", rs->session_id, closing_fd);
        radio_close_socket(rs->sock);
        radio_trace("radio-tls-order: category=playback session=%lu step=CloseSocket-complete\n", rs->session_id);
        Radio_DebugCheckExecMem("after playback CloseSocket");
        radio_worker_breadcrumb("after CloseSocket", "CloseSocket", rs->session_id);
        RADIO_DBG(printf("radio-cleanup: abort raw socket closed before SSL_free session=%lu fd=%ld\n", rs->session_id, closing_fd);)
        RADIO_DBG(printf("AFTER CloseSocket session=%lu fd=%ld socket_close_count=%u\n",
            rs->session_id, closing_fd, rs->socket_close_count););
        rs->sock = RADIO_INVALID_SOCKET;
        rs->socketClosed = 1;
        if (radio_open_socket_count > 0) radio_open_socket_count--;
        if (radio_playback_open_socket_count > 0) radio_playback_open_socket_count--;
        RADIO_DBG(printf("radio-socket: playback socket close session=%lu fd=%ld open_socket_count %ld->%ld playback_open_socket_count %ld->%ld\n", rs->session_id, closing_fd, before_all, radio_open_socket_count, before_playback, radio_playback_open_socket_count););
        RADIO_CLEANUP_DEBUG_PRINTF(("radio-cleanup: abort CloseSocket done\n"));
        RADIO_DBG(printf("radio-cleanup: active abort CloseSocket end session=%lu fd=%ld\n",
            rs->session_id, closing_fd););
        RADIO_STOP_DEBUG_PRINTF(("radio-stop: socket aborted\n"));
    } else {
        RADIO_CLEANUP_DEBUG_PRINTF(("radio-cleanup: abort CloseSocket skipped fd=-1\n"));
    }
}

#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
static void radio_abort_current_socket_job(void *arg) { radio_abort_current_socket_local((RadioStream *)arg); }

/* Controlled worker-owned stop for Radio_RequestStop(): run the exact same
 * atomic unregister+free+close sequence as Radio_Close()'s own
 * radio_worker_close_detach_stream_job(), immediately, instead of only doing
 * the unregister+abort half here and leaving SSL_free()/SSL_CTX_free() for
 * Radio_Close()'s separate, later worker round-trip.
 *
 * That split had a real gap: radio_worker_unregister_stream() removes this
 * session from radio_net_worker_streams and can make Radio_WorkerIsIdle()
 * (and so Radio_PlaybackOwnsNetwork()) report "network free" the moment this
 * job returns -- before SSL_free() ever runs, since that only happened later
 * when Radio_Close() dispatched its own separate job. In that window a new
 * station's probe/connect could be dispatched and run on this same worker
 * task while this session's SSL object was still waiting to be freed --
 * exactly the kind of AmiSSL-instance-wide interference the manual BIO fd
 * detach in radio_abort_current_socket_local() defends against for the raw
 * socket, but does not (and cannot) cover for AmiSSL's own internal state.
 * Doing the full close here, in one uninterrupted worker-task call, performs
 * the free-before-close sequence (SSL_free() while the descriptor is valid,
 * then CloseSocket(), with no SSL_shutdown() network I/O) with no gap for
 * anything else to run in between, and removes the "network owned"
 * signal at the very end instead of the very start.
 *
 * radio_worker_close_detach_stream_job() (and everything it calls) is
 * already idempotent -- Radio_Close() calls it again afterwards and finds
 * everything already unregistered/closed/freed, a cheap no-op via the
 * existing early-return guards. */
static void radio_worker_stop_unregister_abort_job(void *arg)
{
    radio_worker_close_detach_stream_job(arg);
}
#endif

/* Self-dispatching: CloseSocket() touches bsdsocket.library/AmiSSL, so it may
 * only run on the net worker task. Radio_RequestStop() calls this directly
 * from whichever (foreign) task owns this session; close_current_socket_
 * mode() below (itself self-dispatching) calls it again once already
 * running on the worker task, where radio_net_worker_is_self() short-
 * circuits straight to the local call with no IPC round trip. */
static void radio_abort_current_socket(RadioStream *rs)
{
    if (!rs) return;
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    if (!radio_net_worker_is_self()) { Radio_RunOnNetWorker(radio_abort_current_socket_job, rs); return; }
#endif
    radio_abort_current_socket_local(rs);
}

static void close_current_socket_mode_local(RadioStream *rs, RadioCloseMode mode)
{
    long before;
    if (!rs) return;
    radio_net_adopt_context(rs);
    radio_stream_magic_valid(rs, "close_current_socket");
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    if (rs->sock == RADIO_INVALID_SOCKET && rs->socketClosed &&
        (!rs->ssl || rs->sslFreed)) {
#else
    if (rs->sock == RADIO_INVALID_SOCKET && rs->socketClosed) {
#endif
        /* Already fully closed: nothing left to abort or free. A session
         * marked fatalStop/noReconnect (or one bouncing through repeated
         * abort calls before this was added) could otherwise re-enter this
         * whole sequence -- and re-log it -- on every pump. Log once, not
         * hundreds of times. */
        if (!rs->closeCleanupDone) {
            rs->closeCleanupDone = 1;
            RADIO_DBG(printf("radio-cleanup: close mode=%s session=%lu already closed -- no-op\n",
                radio_close_mode_name(mode), rs->session_id));
        }
        return;
    }
    before = radio_open_socket_count;
    RADIO_DBG(printf("radio-cleanup: close mode=%s session=%lu status=%d fd=%ld open_socket_count_before=%ld\n",
        radio_close_mode_name(mode), rs->session_id, (int)rs->status, (long)rs->sock, before););
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    /* Healthy TLS teardown keeps the worker-owned shared SSL_CTX retained and
     * performs no network I/O: SSL_free() while the descriptor is valid, then
     * CloseSocket(). Fatal/poisoned sessions still close the raw socket first
     * and quarantine SSL instead of making further AmiSSL calls. */
    {
        int fatal_close = rs->sslStatePoisoned || rs->fatalStop || rs->noReconnect ||
            Radio_IsMemoryPoisoned() || Radio_IsTlsPoisoned();
        int ssl_freed_before_close;
        int ssl_diag_leaked = radio_runtime_diag_leak_ssl_enabled() &&
            rs->ssl && !rs->sslFreed && !fatal_close;
        ssl_freed_before_close = radio_ssl_free_before_socket_close(rs);
#endif
        radio_abort_current_socket(rs);
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
        radio_ssl_close_stream_mode(rs, mode);
        fatal_close = rs->sslStatePoisoned || rs->fatalStop || rs->noReconnect ||
            Radio_IsMemoryPoisoned() || Radio_IsTlsPoisoned();
        radio_trace("radio-tls-close: category=playback session=%lu health=%s shutdown=skipped ssl=%s socket=closed ctx=%s\n",
            rs->session_id, fatal_close ? "fatal" : "healthy",
            ssl_diag_leaked ? "diag-leaked" :
                (ssl_freed_before_close ? "freed-before-close" :
                    (fatal_close ? "quarantined" : "freed-after-close")),
            fatal_close ? "quarantined" : "shared-retained");
    }
#endif
    RADIO_DBG(printf("radio-cleanup: close mode=%s session=%lu complete open_socket_count_after=%ld\n",
        radio_close_mode_name(mode), rs->session_id, radio_open_socket_count););
}

#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
typedef struct RadioCloseModeJobArgs {
    RadioStream *rs;
    RadioCloseMode mode;
} RadioCloseModeJobArgs;
static void close_current_socket_mode_job(void *arg)
{
    RadioCloseModeJobArgs *a = (RadioCloseModeJobArgs *)arg;
    close_current_socket_mode_local(a->rs, a->mode);
}
#endif

/* Self-dispatching, same reasoning as radio_abort_current_socket() above:
 * this (and everything it calls -- radio_ssl_close_stream_mode(),
 * radio_abort_current_socket()) touches bsdsocket.library/AmiSSL and may
 * only run on the net worker task. Called directly from foreign-task entry
 * points (Radio_Close(), Radio_ReadStartupAudio(), Radio_FailStartup()) as
 * well as from worker-context helpers (connect_http(), radio_pump_body(),
 * reconnect_http()) that are already running on the worker task by the time
 * they get here. */
static void close_current_socket_mode(RadioStream *rs, RadioCloseMode mode)
{
    if (!rs) return;
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    if (!radio_net_worker_is_self()) {
        RadioCloseModeJobArgs args;
        args.rs = rs;
        args.mode = mode;
        Radio_RunOnNetWorker(close_current_socket_mode_job, &args);
        return;
    }
#endif
    close_current_socket_mode_local(rs, mode);
}

/* Default close path: the authoritative worker-owned close function classifies
 * the session from its fatal/poison flags. Healthy stops/station switches free
 * the per-connection SSL before closing the raw socket; fatal or poisoned
 * sessions get raw-socket-only quarantine. */
static void close_current_socket(RadioStream *rs) { close_current_socket_mode(rs, RADIO_CLOSE_ABORT); }

static int reconnect_http(RadioStream *rs)
{
    close_current_socket(rs);
    if (rs && rs->noReconnect) {
        /* A fatal SSL fault already marked this session terminal -- never
         * re-run connect_http()'s DNS/socket/SSL_connect against a task
         * whose AmiSSL state that fault may have damaged. */
        set_error(rs, "TLS read failed");
        return -1;
    }
    if (radio_is_stopping(rs)) { if (rs) rs->status = RADIO_STATUS_CLOSED; return -1; }
    if (rs->reconnectAttempts >= RADIO_RECONNECT_MAX) { set_error(rs,"radio reconnect attempts exhausted"); return -1; }
    if (rs->reconnectDelay > 0) { rs->reconnectDelay--; set_status(rs, RADIO_STATUS_RECONNECTING); radio_backoff_sleep(); return 0; }
    rs->reconnectAttempts++;
    set_status(rs, rs->reconnectAttempts == 1 ? RADIO_STATUS_CONNECTING : RADIO_STATUS_RECONNECTING);
    if (connect_http(rs) == 0) { set_status(rs, RADIO_STATUS_BUFFERING); return 1; }
    if (rs->noReconnect) {
        /* connect_http() just hit a fatal SSL_connect fault (see
         * radio_ssl_do_handshake()): do not schedule yet another reconnect
         * attempt for this call either, fail now. */
        set_error(rs, "TLS read failed");
        return -1;
    }
    rs->reconnectDelay = RADIO_RECONNECT_BACKOFF_PUMPS * rs->reconnectAttempts;
    set_status(rs, RADIO_STATUS_RECONNECTING);
    return 0;
}

static void parse_headers(RadioStream *rs,char *h){ if (radio_stream_magic_valid(rs, "parse_headers") < 1) return; char *line=strtok(h,"\r\n"); int code=0; if(line && ci_starts(line,"ICY")) code=200; else if(line && ci_starts(line,"HTTP/")) sscanf(line,"HTTP/%*s %d",&code); else { set_error(rs,"invalid HTTP stream response"); RADIO_OPEN_DEBUG_PRINTF(("radio-open: HTTP header failed\n")); return; } if(code<200||code>299){ char msg[64]; sprintf(msg,"HTTP %d stream error",code); set_error(rs,msg); RADIO_OPEN_DEBUG_PRINTF(("radio-open: HTTP header failed status %d\n", code)); return; } while((line=strtok(NULL,"\r\n"))){ char *v=strchr(line,':'); if(!v) continue; *v++=0; line=trim(line); v=trim(v); if(ci_equals(line,"Content-Type")) radio_copy_string(rs->contentType,sizeof(rs->contentType),v); else if(ci_equals(line,"icy-metaint")){ rs->metaint=atoi(v); rs->audioUntilMeta=rs->metaint; } else if(ci_equals(line,"icy-br")) rs->bitrate=atoi(v); else if(ci_equals(line,"icy-name")) radio_copy_metadata_string(rs->stationName,sizeof(rs->stationName),v,"icy-name"); else if(ci_equals(line,"icy-genre")) radio_copy_metadata_string(rs->genre,sizeof(rs->genre),v,"icy-genre"); else if(ci_equals(line,"icy-url")) radio_copy_string(rs->streamUrl,sizeof(rs->streamUrl),v); } if(rs->contentType[0] && (ci_starts(rs->contentType,"application/vnd.apple.mpegurl") || ci_starts(rs->contentType,"application/x-mpegurl"))) { set_error(rs,"HLS stream not supported"); RADIO_OPEN_DEBUG_PRINTF(("radio-open: HLS content type unsupported: %s\n", rs->contentType)); return; } RADIO_OPEN_DEBUG_PRINTF(("radio-open: final URL=%s content-type=%s URL codec hint=%s final selected codec=%s\n",
    rs->url, rs->contentType,
    radio_contains_nocase(rs->path,"mp3") ? "MP3" : (radio_contains_nocase(rs->path,"aac") ? "AAC" : "none"),
    (ci_starts(rs->contentType,"audio/mpeg") || ci_starts(rs->contentType,"audio/mp3") || radio_contains_nocase(rs->path,"mp3")) ? "MP3" :
    ((ci_starts(rs->contentType,"audio/aac") || ci_starts(rs->contentType,"audio/aacp") || radio_contains_nocase(rs->path,"aac")) ? "AAC" : "unknown"))); }

static void parse_meta(RadioStream *rs,const unsigned char *m,int n)
{
    char oldTitle[128];
    char parsedTitle[128];
    int result;
    if (!rs || !m || n <= 0)
        return;
    if (!radio_stream_magic_valid(rs, "parse_meta"))
        return;
    if (n >= RADIO_META_MAX)
        RADIO_DBG(printf("radio-icy: metadata truncated session=%lu metaLen=%d capacity=%lu station=\"%s\" url=\"%s\"\n", rs->session_id, n, (unsigned long)RADIO_META_MAX, rs->stationName, rs->url););
    result = RadioMetadata_ExtractIcyTitle(
        parsedTitle, sizeof(parsedTitle), m, (size_t)n);
    if (result == RADIO_METADATA_TITLE) {
        radio_copy_string(oldTitle, sizeof(oldTitle), rs->title);
        radio_copy_metadata_string(rs->title, sizeof(rs->title), parsedTitle,
                                   "StreamTitle");
        if (strcmp(oldTitle, rs->title) != 0)
            RADIO_DBG(printf("radio-resource: session=%lu ICY metadata updated title=\"%s\" (fixed buffer, active_icy_metadata_count=%ld)\n", rs->session_id, rs->title, radio_active_icy_metadata_count););
    } else if (result == RADIO_METADATA_XML_IGNORED) {
        RADIO_DBG(printf("radio-icy: ignored XML StreamTitle without recognised song fields session=%lu station=\"%s\"\n", rs->session_id, rs->stationName););
    }
}

static int process_bytes(RadioStream *rs, const unsigned char *b, int n)
{
    int i = 0;
    if (!rs || !b || n <= 0) return 0;
    while (i < n) {
        int avail;
        if (rs->stopping) break;
        if (rs->parseState == RADIO_PARSE_HEADER) {
            if (rs->headerLen >= RADIO_HEADER_MAX - 1) { set_error(rs,"HTTP header too large"); return -1; }
            rs->header[rs->headerLen++] = (char)b[i++]; rs->header[rs->headerLen] = 0;
            if (rs->headerLen >= 4 && !memcmp(rs->header + rs->headerLen - 4, "\r\n\r\n", 4)) {
                RADIO_DBG(printf("radio-http-diag: session=%lu raw HTTP response header (%d bytes) follows:\n%s--- end of header ---\n", rs->session_id, rs->headerLen, rs->header););
                rs->headerDone = 1; rs->streamStateFlags |= HEADER_DONE; parse_headers(rs, rs->header); if (rs->status == RADIO_STATUS_ERROR) return -1; rs->parseState = RADIO_PARSE_AUDIO;
            }
            continue;
        }

        avail = n - i;
        if (rs->metaint > 0 && rs->parseState == RADIO_PARSE_AUDIO && rs->audioUntilMeta <= 0)
            rs->parseState = RADIO_PARSE_META_LEN;

        RADIO_DBG(printf("radio-icy: loop session=%lu icy_state=%d metaint=%d audio_until_meta=%d meta_remaining=%d src_avail=%d fill=%lu ring_free=%lu wpos=%lu url=\"%s\"\n",
            rs->session_id, (int)rs->parseState, rs->metaint, rs->audioUntilMeta,
            rs->metaLeft, avail, rs->used, rs->size > rs->used ? rs->size - rs->used : 0, rs->wpos, rs->url););

        if (rs->parseState == RADIO_PARSE_META_LEN) {
            int lenByte = b[i++];
            rs->metaLen = lenByte * 16;
            rs->metaGot = 0;
            rs->metaLeft = rs->metaLen;
            if (!rs->firstMetaLogged) {
                rs->firstMetaLogged = 1;
                RADIO_DBG(printf("radio-icy: first metadata block session=%lu len_byte=%d meta_len=%d metaint=%d fill=%lu url=\"%s\"\n",
                    rs->session_id, lenByte, rs->metaLen, rs->metaint, rs->used, rs->url););
            }
            RADIO_DBG(printf("radio-icy: metadata length session=%lu len_byte=%d meta_len=%d src_avail_after=%d metaint=%d url=\"%s\"\n",
                rs->session_id, lenByte, rs->metaLen, n - i, rs->metaint, rs->url););
            if (rs->metaLen > RADIO_META_MAX)
                RADIO_DBG(printf("radio-icy: metadata payload exceeds buffer session=%lu metaLen=%d capacity=%lu; truncating parse copy station=\"%s\" url=\"%s\"\n", rs->session_id, rs->metaLen, (unsigned long)RADIO_META_MAX, rs->stationName, rs->url););
            if (rs->metaLen == 0) {
                rs->audioUntilMeta = rs->metaint;
                rs->parseState = RADIO_PARSE_AUDIO;
            } else {
                rs->parseState = RADIO_PARSE_META_PAYLOAD;
            }
            continue;
        }

        if (rs->parseState == RADIO_PARSE_META_PAYLOAD) {
            int take = rs->metaLeft < avail ? rs->metaLeft : avail;
            int copy = take;
            if (copy > RADIO_META_MAX - 1 - rs->metaGot) copy = RADIO_META_MAX - 1 - rs->metaGot;
            if (copy > 0) {
                memcpy(rs->meta + rs->metaGot, b + i, (size_t)copy);
                rs->metaGot += copy;
            }
            i += take;
            rs->metaLeft -= take;
            RADIO_DBG(printf("radio-icy: metadata payload session=%lu consumed=%d copied=%d meta_remaining=%d src_avail_after=%d url=\"%s\"\n",
                rs->session_id, take, copy, rs->metaLeft, n - i, rs->url););
            if (rs->metaLeft <= 0) {
                if (rs->metaGot > 0) parse_meta(rs, rs->meta, rs->metaGot);
                rs->audioUntilMeta = rs->metaint;
                rs->parseState = RADIO_PARSE_AUDIO;
            }
            continue;
        }

        if (rs->parseState == RADIO_PARSE_AUDIO) {
            int audioAvail = avail;
            int wanted, wrote;
            if (rs->metaint > 0 && rs->audioUntilMeta < audioAvail) audioAvail = rs->audioUntilMeta;
            wanted = audioAvail;
            if (wanted > 0 && rs->size > rs->used && (unsigned long)wanted > rs->size - rs->used)
                wanted = (int)(rs->size - rs->used);
            if (wanted <= 0) {
                RADIO_DBG(printf("radio-ring: full/wait session=%lu icy_state=%d metaint=%d audio_until_meta=%d src_avail=%d fill=%lu ring_free=%lu wpos=%lu capacity=%lu url=\"%s\"\n",
                    rs->session_id, (int)rs->parseState, rs->metaint, rs->audioUntilMeta, avail,
                    rs->used, rs->size > rs->used ? rs->size - rs->used : 0, rs->wpos, rs->size, rs->url););
                break;
            }
            RADIO_DBG(printf("radio-icy: audio write plan session=%lu metaint=%d audio_until_meta_before=%d src_avail=%d bytes_to_ring=%d fill_before=%lu ring_free_before=%lu wpos_before=%lu url=\"%s\"\n",
                rs->session_id, rs->metaint, rs->audioUntilMeta, avail, wanted, rs->used,
                rs->size > rs->used ? rs->size - rs->used : 0, rs->wpos, rs->url););
            wrote = ring_write(rs, b + i, wanted);
            if (wrote > 0 && rs->headerDone && !rs->decoderStarted) { rs->decoderStarted = 1; rs->streamStateFlags |= DECODER_STARTED; radio_active_decoder_count++; RADIO_DBG(printf("radio-resource: session=%lu decoder path started active_decoder_count=%ld first_write=%d fill=%lu metaint=%d\n", rs->session_id, radio_active_decoder_count, wrote, rs->used, rs->metaint);); }
            i += wrote;
            if (rs->metaint > 0) rs->audioUntilMeta -= wrote;
            RADIO_DBG(printf("radio-icy: audio write done session=%lu wrote=%d audio_until_meta_after=%d src_avail_after=%d fill_after=%lu ring_free_after=%lu wpos_after=%lu url=\"%s\"\n",
                rs->session_id, wrote, rs->audioUntilMeta, n - i, rs->used,
                rs->size > rs->used ? rs->size - rs->used : 0, rs->wpos, rs->url););
            if (wrote <= 0) break;
            continue;
        }
    }
    return i;
}

static int radio_note_start_wait(RadioStream *rs, const char *message)
{
    if (!rs || rs->everPlayed) return 0;
    if (rs->status == RADIO_STATUS_ERROR) {
        /* Already failed for a specific reason (e.g. a fatal TLS fault
         * already set "TLS read failed" via set_error() this same
         * Radio_Pump() call) -- a generic startup-timeout message here would
         * silently overwrite and hide the real diagnosis. First error wins. */
        return -1;
    }
    rs->startPumps++;
    if (rs->startPumps >= RADIO_START_TIMEOUT_PUMPS) {
        set_error(rs, message);
        close_current_socket(rs);
        return -1;
    }
	return 0;
}

static void radio_worker_maybe_log_stats(RadioStream *rs)
{
	clock_t now;
	unsigned long elapsedMs;
	unsigned long fill;
	unsigned long freeBytes;
	RadioStatus status;

	if (!rs) return;
	now = clock();
	if (!rs->workerLastStatsClock) {
		rs->workerLastStatsClock = (unsigned long)now;
		return;
	}
	elapsedMs = (unsigned long)((now - (clock_t)rs->workerLastStatsClock) * 1000UL / CLOCKS_PER_SEC);
	if (elapsedMs < 2000UL) return;
	radio_stream_lock(rs);
	fill = rs->used;
	freeBytes = rs->size > rs->used ? rs->size - rs->used : 0;
	status = rs->status;
	radio_stream_unlock(rs);
	RADIO_DBG(printf("radio-worker: session=%lu pump reads=%lu bytes=%lu wantRead=%lu zero=%lu backpressure=%lu partial=%lu preventedDrop=%lu ringFill=%lu ringFree=%lu status=%d stage=\"%s\" lastOp=\"%s\" heartbeat=%lu\n",
		rs->session_id,
		rs->workerReadCalls,
		rs->workerReadBytes,
		rs->workerWantReadCount,
		rs->workerPumpZeroCount,
		rs->workerBackpressureCount,
		rs->workerPartialConsumeCount,
		rs->workerDroppedInputPreventedCount,
		fill,
		freeBytes,
		(int)status,
		radio_net_worker_stage ? (const char *)radio_net_worker_stage : "<unset>",
		radio_net_worker_last_op ? (const char *)radio_net_worker_last_op : "<unset>",
		radio_net_worker_heartbeat);)
	rs->workerLastStatsClock = (unsigned long)now;
}

RadioStream *Radio_OpenWithHostAddr(const char *url, int haveHostAddr, unsigned long hostAddrBe)
{
    RadioStream *rs = (RadioStream *)calloc(1, sizeof(*rs));
    if (!rs) return NULL;
    /* radio_stream_magic_valid() (used by close_current_socket(),
     * parse_headers(), parse_meta(), ...) compares against this field to
     * catch a corrupted/stale RadioStream* before it gets dereferenced --
     * that check is only meaningful if the magic is actually stamped on a
     * freshly allocated stream, so it must happen before anything else can
     * observe or act on rs. */
    rs->magic = RADIO_STREAM_MAGIC;
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    /* Recorded before anything else can run a worker job for this session --
     * see the struct field comment (radio_is_stopping()'s cross-task
     * SIGBREAKF_CTRL_C bridge). */
    rs->requestingTask = FindTask(NULL);
#endif
    if (!radio_atexit_registered) { atexit(radio_app_exit_report); radio_atexit_registered = 1; }
    radio_reset_session_state(rs);
    rs->session_id = radio_next_session_id++;
    rs->generation = ++radio_current_generation;
    if (radioMemoryPoisoned) {
        rs->status = RADIO_STATUS_ERROR;
        radio_copy_string(rs->url, sizeof(rs->url), url ? url : "");
        set_error(rs, "Memory corruption detected; restart MintAMP before playing radio.");
        RADIO_OPEN_DEBUG_PRINTF(("radio-open: refused stream start after memory poison session=%lu url=\"%s\"\n", rs->session_id, rs->url));
        return rs;
    }
    if (url && !strncmp(url, "https://", 8) && Radio_IsTlsPoisoned()) {
        rs->status = RADIO_STATUS_ERROR;
        radio_copy_string(rs->url, sizeof(rs->url), url);
        set_error(rs, Radio_TlsPoisonedMessage());
        RADIO_OPEN_DEBUG_PRINTF(("radio-open: refused HTTPS stream start after AmiSSL poison session=%lu url=\"%s\"\n", rs->session_id, rs->url));
        return rs;
    }
    if (haveHostAddr) {
        memcpy(&rs->hostAddr, &hostAddrBe, sizeof(rs->hostAddr));
        rs->haveHostAddr = 1;
    }
    radio_active_stream_sessions++;
    radio_active_stream_tasks++;
    rs->streamStateFlags |= STREAM_ALLOCATED;
    RADIO_DBG(printf("radio-resource: session=%lu generation=%lu stream session/task allocated active_stream_sessions=%ld active_stream_tasks=%ld active_decoder_count=%ld\n", rs->session_id, rs->generation, radio_active_stream_sessions, radio_active_stream_tasks, radio_active_decoder_count););
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    RADIO_DBG(printf("radio-ssl-diag: Radio_Open session=%lu url=\"%s\" inherited AmiSSL state: initialized=%d base=%p ext=%p master=%p ssl_count=%ld ssl_ctx_count=%ld\n",
        rs->session_id, url ? url : "(null)", radio_amissl_initialized, (void *)AmiSSLBase, (void *)AmiSSLExtBase, (void *)AmiSSLMasterBase, radio_active_ssl_count, radio_active_ssl_ctx_count));
#endif
    radio_debug_mem_report(rs->session_id, "before stream start");
    radio_poison_session_id = rs->session_id;
    radio_copy_string(radio_poison_url, sizeof(radio_poison_url), url ? url : "");
    if (Radio_CheckMiniMem("before ring buffer allocated") > 0) {
        set_error(rs, "Memory corruption detected - restart app");
        RADIO_OPEN_DEBUG_PRINTF(("radio-open: refused ring allocation after MiniMem corruption session=%lu url=\"%s\"\n", rs->session_id, radio_poison_url));
        return rs;
    }
    rs->status = RADIO_STATUS_CONNECTING;
    rs->size = RADIO_RING_BYTES;
    RADIO_DBG(printf("radio-resource: session=%lu ring buffer owner url=\"%s\" allocFile=radio_stream.c allocLine=Radio_OpenWithHostAddr\n",
        rs->session_id, radio_poison_url);)
    rs->ringAlloc = (unsigned char *)malloc(rs->size + 2UL * RADIO_RING_GUARD_BYTES);
    if (rs->ringAlloc) { rs->ring = rs->ringAlloc + RADIO_RING_GUARD_BYTES; radio_ring_set_canary(rs); }
    /* Printed unconditionally (not RADIO_DBG-gated) so it's visible even if
     * a build is run without the console attached to a log file -- this is
     * the address to arm a live debugger watchpoint on when chasing the
     * ring-corruption-with-no-detectable-trigger case: front guard is
     * [ringAlloc, ringAlloc+16), rear guard is
     * [ringAlloc+16+size, ringAlloc+32+size). */
    if (rs->ringAlloc)
        radio_trace("radio-resource: session=%lu ring buffer allocated at %p (front guard %p..%p, data %p..%p, rear guard %p..%p)\n",
            rs->session_id, (void *)rs->ringAlloc,
            (void *)rs->ringAlloc, (void *)(rs->ringAlloc + RADIO_RING_GUARD_BYTES),
            (void *)rs->ring, (void *)(rs->ring + rs->size),
            (void *)(rs->ringAlloc + RADIO_RING_GUARD_BYTES + rs->size),
            (void *)(rs->ringAlloc + 2UL * RADIO_RING_GUARD_BYTES + rs->size));
    /* The soak test caught the ring's malloc-guard header already zeroed
     * (magic=00000000) by session 4, in a run where headerDone was still 0
     * -- meaning ring_write()/ring_read() were never even called this
     * session, ruling those out as the source. Checking immediately after
     * this fresh allocation (before anything touches the buffer) tells us
     * whether malloc() is handing back an already-damaged block -- pointing
     * at heap/free-list corruption from an earlier session -- or whether
     * this allocation starts clean and something corrupts it afterward. */
    if (Radio_CheckMiniMem("after ring buffer allocated") > 0) {
        set_error(rs, "Memory corruption detected - restart app");
        return rs;
    }
    if (rs->ring) { rs->audioInitialized = 1; radio_active_stream_buffer_count++; radio_active_audio_buffer_count++; RADIO_DBG(printf("radio-resource: session=%lu stream/audio buffer allocated active_stream_buffer_count=%ld active_audio_buffer_count=%ld\n", rs->session_id, radio_active_stream_buffer_count, radio_active_audio_buffer_count)); }
    if (!rs->ring) {
        rs->size = 0;
        set_error(rs, "not enough memory for radio buffer");
        RADIO_OPEN_DEBUG_PRINTF(("radio-open: Radio_Open returning error\n"));
        return rs;
    }
    if (radio_url_looks_hls(url)) {
        set_error(rs, "HLS stream not supported");
        radio_debug_mem_report(rs->session_id, "after failed probe cleanup");
        radio_resource_summary(rs, "after failed probe cleanup");
        RADIO_OPEN_DEBUG_PRINTF(("radio-open: HLS URL rejected before direct playback\n"));
        return rs;
    }
    if (parse_url(rs, url)) {
        set_error(rs,
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
            "only direct http:// or https:// stream URLs are supported"
#else
            "only direct http:// stream URLs are supported"
#endif
        );
        radio_debug_mem_report(rs->session_id, "after failed probe cleanup");
        radio_resource_summary(rs, "after failed probe cleanup");
        RADIO_OPEN_DEBUG_PRINTF(("radio-open: Radio_Open returning error\n"));
        return rs;
    }
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    {
        RadioOpenJobArgs openArgs;
        openArgs.rs = rs;
        openArgs.result = -1;
        if (!Radio_RunOnNetWorker(radio_worker_job_open, &openArgs))
            set_error(rs, "AmiSSL unavailable: network worker not running");
        else if (openArgs.result == 0) {
            rs->status = RADIO_STATUS_BUFFERING;
            radio_debug_mem_report(rs->session_id, "after HTTP request phase started");
        } else if (rs->status != RADIO_STATUS_ERROR)
            set_error(rs, rs->error[0] ? rs->error : "cannot open radio stream");
    }
    if (rs->status == RADIO_STATUS_ERROR) {
        radio_debug_mem_report(rs->session_id, "after failed probe cleanup");
        radio_resource_summary(rs, "after failed probe cleanup");
        RADIO_OPEN_DEBUG_PRINTF(("radio-open: Radio_Open returning error\n"));
    }
#else
    if (connect_http(rs) == 0) {
        rs->status = RADIO_STATUS_BUFFERING;
        radio_debug_mem_report(rs->session_id, "after HTTP request phase started");
    }
    else {
        close_current_socket(rs);
        if (rs->status != RADIO_STATUS_ERROR)
            set_error(rs, rs->error[0] ? rs->error : "cannot open radio stream");
        radio_debug_mem_report(rs->session_id, "after failed probe cleanup");
        radio_resource_summary(rs, "after failed probe cleanup");
        RADIO_OPEN_DEBUG_PRINTF(("radio-open: Radio_Open returning error\n"));
    }
#endif
    return rs;
}

RadioStream *Radio_Open(const char *url)
{
    return Radio_OpenWithHostAddr(url, 0, 0);
}
void Radio_RequestStop(RadioStream *rs)
{
    RadioCloseMode mode;
    if (!rs) return;
    radio_debug_mem_report(rs->session_id, "before stop");
    rs->stop_request_count++;
    RADIO_STOP_DEBUG_PRINTF(("radio-stop: session=%lu stop requested count=%u status=%d fd=%ld\n", rs->session_id, rs->stop_request_count, (int)rs->status, (long)rs->sock));
    if (rs->status == RADIO_STATUS_CLOSED) return;
    /* On real CD32 hardware, SSL_shutdown() during a live station switch can
     * wedge AmiSSL. The worker therefore performs one atomic non-leaking close:
     * unregister the old stream, mark its SSL locally shut down, free the SSL
     * while its BIO still has a valid descriptor, then CloseSocket(). Fatal
     * SSL objects retain the existing connection-local quarantine policy. */
    mode = RADIO_CLOSE_ABORT;
    RADIO_DBG(printf("radio-cleanup: close mode=%s session=%lu status=%d (Radio_RequestStop)\n", radio_close_mode_name(mode), rs->session_id, (int)rs->status););
    radio_stream_lock(rs);
    rs->stopping = 1;
    if (rs->generation == radio_current_generation)
        radio_current_generation++;
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    rs->workerStopRequested = 1;
    rs->workerCloseRequested = 1;
    radio_worker_state = RADIO_WORKER_STOPPING;
#endif
    rs->reconnectAttempts = RADIO_RECONNECT_MAX;
    rs->reconnectDelay = 0;
    rs->status = RADIO_STATUS_STOPPING;
    radio_stream_unlock(rs);
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    /* Wake a worker task that might be wedged inside connect_http()'s
     * gethostbyname() call (the one masked blocking call in this file --
     * see the SBTC_BREAKMASK comment there) resolving THIS session's host.
     * Without this, the stop flags set above only take effect once that
     * call eventually returns on its own, which for a slow/unresponsive
     * resolver could be a long time or never (the original wedged-forever
     * report this fixed). Signal() is safe to call regardless of what the
     * worker is actually doing right now: its own poll loop
     * (radio_net_worker_entry()) never Wait()s on this signal, so this
     * either aborts an in-flight masked gethostbyname() immediately, or
     * just leaves the bit pending harmlessly until that call's own
     * SetSignal(0, SIGBREAKF_CTRL_C) clears it before its next attempt
     * (for this or a later session). */
    if (radio_net_worker_task && !radio_net_worker_is_self())
        Signal(radio_net_worker_task, SIGBREAKF_CTRL_C);
#endif
    /* Run the full unregister+free+close sequence as one
     * uninterrupted worker-owned step (radio_worker_stop_unregister_abort_
     * job() above, which just calls radio_worker_close_detach_stream_job())
     * instead of only aborting the socket here and leaving SSL_free() for
     * Radio_Close()'s own later, separate round trip -- see that function's
     * comment for why the gap between the two mattered. */
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    if (!radio_net_worker_is_self())
        Radio_RunOnNetWorker(radio_worker_stop_unregister_abort_job, rs);
    else
        radio_worker_stop_unregister_abort_job(rs);
#else
    radio_abort_current_socket(rs);
#endif
    RADIO_STOP_DEBUG_PRINTF(("radio-stop: marked stopping\n"));
}
void Radio_Close(RadioStream *rs)
{
    if (!rs) return;
    RADIO_DBG(printf("radio-http-diag: Radio_Close session=%lu status=%d everPlayed=%d headerDone=%d firstData=%d used=%lu metaint=%d reconnectAttempts=%d startPumps=%d stopping=%d stopReq=%u contentType=\"%s\" host=\"%s\" path=\"%s\" error=\"%s\"\n",
        rs->session_id, (int)rs->status, rs->everPlayed, rs->headerDone, rs->firstDataLogged, rs->used, rs->metaint,
        rs->reconnectAttempts, rs->startPumps, rs->stopping, rs->stop_request_count,
        rs->contentType, rs->host, rs->path, rs->error));
    RADIO_STOP_DEBUG_PRINTF(("radio-stop: Radio_Close entered session=%lu\n", rs->session_id));
    rs->cleanup_count++;
    rs->cleanupDone = 1;
    if (rs->cleanup_count > 1) radio_duplicate_cleanup_warning(rs, "session cleanup", rs->cleanup_count);
    RADIO_DBG(printf("radio-teardown: before Radio_Close second stop phase (Radio_RequestStop re-entry) session=%lu\n", rs->session_id));
    Radio_RequestStop(rs);
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    if (!radio_net_worker_is_self()) {
        if (!Radio_RunOnNetWorker(radio_worker_close_detach_stream_job, rs)) {
            radio_stream_lock(rs);
            rs->workerAbandoned = 1;
            radio_stream_unlock(rs);
            RADIO_DBG(printf("radio-net-worker: close/detach timed out for session=%lu -- leaking RadioStream/ring and refusing to free while worker may still touch them\n", rs->session_id););
            return;
        }
    } else
        radio_worker_close_detach_stream_job(rs);
    {
        int workerClosedAck, workerDetached;
        radio_stream_lock(rs);
        workerClosedAck = rs->workerClosedAck;
        workerDetached = rs->workerDetached;
        if (!workerClosedAck || !workerDetached) rs->workerAbandoned = 1;
        radio_stream_unlock(rs);
        if (!workerClosedAck || !workerDetached) {
            RADIO_DBG(printf("radio-net-worker: close returned without closed/detached ack for session=%lu -- leaking RadioStream/ring\n", rs->session_id););
            return;
        }
    }
#else
    close_current_socket(rs);
#endif
    rs->status = RADIO_STATUS_CLOSED;
    rs->stream_buffer_free_count++;
    rs->audio_buffer_free_count++;
    {
        /* Fatal TLS teardown quarantine: once a fatal SSL fault or ring
         * corruption has poisoned this session (Radio_IsSessionFatal()),
         * the whole object graph is unsafe -- not just the SSL/SSL_CTX
         * objects already gated below. Skip the ring buffer free too and
         * leak it, same reasoning as the corrupted-canary leak this already
         * did for a narrower case. */
        int fatal = Radio_IsSessionFatal(rs);
        Radio_DebugCheckExecMem("before ring buffer free/skip");
        if (rs->stream_buffer_free_count > 1) radio_duplicate_cleanup_warning(rs, "stream buffer free", rs->stream_buffer_free_count);
        else { if (rs->ringAlloc) {
            /* radio_ring_check_canary() returning -1 means the guard bytes
             * around this block are already corrupted -- in a HEAPGUARD debug
             * build, MiniMem_Free() defensively refuses to free a block whose
             * header doesn't look valid, but a release build has no such
             * safety net: calling the real free() on an already-corrupted
             * block risks smashing the allocator's own free-list instead of
             * just leaking one buffer. Leak it deliberately instead. */
            if (!fatal && radio_ring_check_canary(rs, "before cleanup") == 0)
                free(rs->ringAlloc);
            else
                RADIO_DBG(printf("radio-cleanup: session=%lu url=\"%s\" ring buffer free skipped (%s), quarantining/leaking to avoid smashing the allocator\n",
                    rs->session_id, rs->url, fatal ? "fatal TLS quarantine" : "corrupted"));
            if (radio_active_stream_buffer_count > 0) radio_active_stream_buffer_count--;
        } }
        Radio_DebugCheckExecMem("after ring buffer free/skip");
    }
    if (rs->audioInitialized || rs->playbackStarted) {
        if (rs->audio_buffer_free_count > 1) radio_duplicate_cleanup_warning(rs, "audio buffer free", rs->audio_buffer_free_count);
        else { if (radio_active_audio_buffer_count > 0) radio_active_audio_buffer_count--; }
    }
    if (rs->decoderStarted) {
        rs->decoder_free_count++;
        if (rs->decoder_free_count > 1) radio_duplicate_cleanup_warning(rs, "decoder free", rs->decoder_free_count);
        else { if (radio_active_decoder_count > 0) radio_active_decoder_count--; }
    } else {
        RADIO_DBG(printf("radio-cleanup: session=%lu decoder cleanup skipped (decoderStarted=0 headerDone=%d firstData=%d)\n", rs->session_id, rs->headerDone, rs->firstDataLogged););
    }
    rs->ring = NULL; rs->ringAlloc = NULL;
    rs->size = rs->used = rs->rpos = rs->wpos = 0;
    rs->task_exit_count++;
    if (radio_active_stream_tasks > 0) radio_active_stream_tasks--;
    if (radio_active_stream_sessions > 0) radio_active_stream_sessions--;
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    /* The SSL_CTX is worker-owned and shared for the worker lifetime. A
     * playback session only drops its borrowed pointer here; final context
     * free/quarantine happens in radio_worker_shutdown_ssl_ctx(). */
    rs->ctx = NULL;
    rs->ctxFreed = 1;
    rs->amissl_cleanup_count++;
#endif
    radio_debug_mem_report(rs->session_id, "after stop cleanup");
    radio_resource_summary(rs, "after stop cleanup");
    RADIO_STOP_DEBUG_PRINTF(("radio-stop: stream task exiting / Radio_Close exited session=%lu task_exit_count=%u cleanup_count=%u stop_request_count=%u ssl_free_count=%u socket_close_count=%u decoder_free_count=%u\n", rs->session_id, rs->task_exit_count, rs->cleanup_count, rs->stop_request_count, rs->ssl_free_count, rs->socket_close_count, rs->decoder_free_count));
    if (Radio_IsSessionFatal(rs)) {
        /* The RadioStream struct itself is part of the unsafe object graph
         * once a fatal TLS fault or ring corruption has been detected: leak
         * it rather than free() it. This mirrors the ring-buffer/SSL/
         * SSL_CTX/CleanupAmiSSL quarantine above -- a leak is better than
         * risking a free-list write into already-damaged exec memory. */
        RADIO_DBG(printf("radio-cleanup: RadioStream struct free skipped (fatal TLS quarantine) session=%lu -- leaking\n", rs->session_id));
    } else {
        free(rs);
    }
}

void Radio_GetNetworkStats(long *active_stream_sessions, long *active_stream_tasks,
    long *open_socket_count, long *active_ssl_count, long *active_ssl_ctx_count)
{
    if (active_stream_sessions) *active_stream_sessions = radio_active_stream_sessions;
    if (active_stream_tasks) *active_stream_tasks = radio_active_stream_tasks;
    if (open_socket_count) *open_socket_count = radio_open_socket_count;
    if (active_ssl_count) *active_ssl_count = radio_active_ssl_count;
    if (active_ssl_ctx_count) *active_ssl_ctx_count = radio_active_ssl_ctx_count;
}

/* Full set of per-session resource counters, for a caller that wants to
 * confirm every prior radio session's resources have actually been released
 * before starting a new one (a queued station switch, say) -- not just that
 * the old child Task has been reaped by DOS. */
void Radio_GetTeardownStats(long *active_stream_sessions, long *active_stream_tasks,
    long *open_socket_count, long *playback_open_socket_count,
    long *active_decoder_count, long *active_audio_buffer_count,
    long *active_stream_buffer_count)
{
    if (active_stream_sessions) *active_stream_sessions = radio_active_stream_sessions;
    if (active_stream_tasks) *active_stream_tasks = radio_active_stream_tasks;
    if (open_socket_count) *open_socket_count = radio_open_socket_count;
    if (playback_open_socket_count) *playback_open_socket_count = radio_playback_open_socket_count;
    if (active_decoder_count) *active_decoder_count = radio_active_decoder_count;
    if (active_audio_buffer_count) *active_audio_buffer_count = radio_active_audio_buffer_count;
    if (active_stream_buffer_count) *active_stream_buffer_count = radio_active_stream_buffer_count;
}

/* The GUI/opener task (and every other subsystem: radio_browser_http.c,
 * etc.) always gets NULL here in HAVE_AMISSL builds -- the net worker task's
 * SocketBase/AmiSSLBase/AmiSSLMasterBase are private to that one task and
 * are never read, swapped, or shared with any other task.  radio_stream_
 * probe.c's own code only ever runs via Radio_RunOnNetWorker(), i.e. it IS
 * the worker task by the time it calls this, so it (and only it) still gets
 * the real pointers -- see Radio_GetAmiSslShared()'s comment below. */
void Radio_GetNetworkBases(void **socket_base, void **amissl_base, void **amissl_master_base)
{
#if defined(AMIGA_M68K)
#if defined(HAVE_AMISSL)
    if (radio_net_worker_is_self()) {
        if (socket_base) *socket_base = (void *)SocketBase;
        if (amissl_base) *amissl_base = (void *)AmiSSLBase;
        if (amissl_master_base) *amissl_master_base = (void *)AmiSSLMasterBase;
        return;
    }
#else
    /* Plain-HTTP-only m68k build: no worker task, bsdsocket.library is a
     * single ordinary shared library opened once by Radio_NetworkInit() --
     * unchanged from before this file's single-worker rework. */
    if (socket_base) *socket_base = (void *)SocketBase;
    if (amissl_base) *amissl_base = 0;
    if (amissl_master_base) *amissl_master_base = 0;
    return;
#endif
#endif
    if (socket_base) *socket_base = 0;
    if (amissl_base) *amissl_base = 0;
    if (amissl_master_base) *amissl_master_base = 0;
}

/* True once the net worker task (HAVE_AMISSL builds) or Radio_NetworkInit()
 * itself (plain-HTTP builds) has successfully opened bsdsocket.library --
 * lets the GUI grey out internet-radio features up front on a machine with
 * no network stack installed, instead of failing later on first connect.
 * A plain status flag, not the SocketBase pointer itself. */
int Radio_HasNetwork(void)
{
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    return radio_net_worker_libs_ok != 0;
#elif defined(AMIGA_M68K)
    return SocketBase != NULL;
#else
    return 0;
#endif
}

/* True once the net worker task has successfully opened its AmiSSL instance
 * -- lets the GUI grey out the HTTPS scheme option up front on a machine
 * without AmiSSL installed (always false in builds without HAVE_AMISSL). */
int Radio_HasHttps(void)
{
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    return radio_net_worker_https_ok != 0;
#else
    return 0;
#endif
}

/* Runtime accessor so radio_stream_probe.c can use the net worker's AmiSSL
 * instance instead of opening a second one of its own.  The probe file
 * declares its own AmiSSLBase/AmiSSLExtBase/AmiSSLMasterBase as weak symbols
 * (required by the AmiSSL proto-header call stubs, which resolve through a
 * global of that exact name in whichever translation unit calls them) that
 * do not reliably merge with this file's strong definitions under the m68k
 * hunk linker, so it adopts the values through this function call instead --
 * which works with every linker AND (unlike the old design) never exposes
 * the worker's bases to any task other than the worker itself: probe.c's
 * code now only ever runs via Radio_RunOnNetWorker(), so by the time it
 * calls this, it IS the worker task. */
void Radio_GetAmiSslShared(void **amissl_base, void **amissl_ext_base, void **amissl_master_base)
{
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    if (radio_net_worker_is_self()) {
        if (amissl_base) *amissl_base = (void *)AmiSSLBase;
        if (amissl_ext_base) *amissl_ext_base = (void *)AmiSSLExtBase;
        if (amissl_master_base) *amissl_master_base = (void *)AmiSSLMasterBase;
        return;
    }
#endif
    if (amissl_base) *amissl_base = 0;
    if (amissl_ext_base) *amissl_ext_base = 0;
    if (amissl_master_base) *amissl_master_base = 0;
}

/* Start the single long-lived radio net worker task (HAVE_AMISSL builds) or
 * open bsdsocket.library directly (plain-HTTP-only m68k builds without
 * AmiSSL, which have no worker task and no per-task AmiSSL lifecycle to
 * manage). Safe to call more than once -- a no-op if already up -- and safe
 * even if the caller never uses HTTPS/radio at all. */
void Radio_NetworkInit(void)
{
#if defined(AMIGA_M68K)
#if defined(HAVE_AMISSL)
    if (!radio_net_worker_ensure_started())
        RADIO_DBG(printf("radio-netinit: net worker start/AmiSSL open failed (HTTPS unavailable this run)\n"););
#else
    if (!SocketBase) {
        SocketBase = OpenLibrary("bsdsocket.library", 4);
        if (SocketBase) {
            radio_socket_library_open_count++;
            RADIO_DBG(printf("radio-netinit: bsdsocket.library opened socket_library_open_count=%ld\n", radio_socket_library_open_count););
        }
    }
#endif
    Radio_DebugCheckExecMem("after Radio_NetworkInit");
#endif
}

/* Final network-library teardown at application exit, called only after
 * every playback child has been stopped and reaped.  This is now the only
 * place bsdsocket.library/AmiSSL are closed -- the net worker task itself is
 * asked to close them and exit exactly once, here. */
void Radio_NetworkShutdown(void)
{
#if defined(AMIGA_M68K)
    if (radio_network_shutdown_started) {
        RADIO_DBG(printf("radio-netshutdown: already ran, skipping duplicate shutdown\n"););
        return;
    }
    radio_network_shutdown_started = 1;
#if defined(HAVE_AMISSL)
    if (radio_tls_shutdown_quarantine) {
        RADIO_DBG(printf("radio-netshutdown: shutdown_quarantine=1 (tls_fault_count=%ld tls_poisoned=%d memory_poisoned=%d task_poisoned=%d active_ssl_count=%ld active_ssl_ctx_count=%ld open_socket_count=%ld), still asking worker to perform owner-task AmiSSL teardown\n",
            radio_tls_fault_count, Radio_IsTlsPoisoned(), Radio_IsMemoryPoisoned(),
            radio_amissl_task_poisoned, radio_active_ssl_count,
            radio_active_ssl_ctx_count, radio_open_socket_count););
    }
    /* Ask the worker task to run CloseAmiSSL() (which performs the implicit
     * CleanupAmiSSL because OpenAmiSSLTags() used AmiSSL_InitAmiSSL=TRUE),
     * then CloseLibrary() and exit; radio_net_worker_stop() waits, bounded,
     * for it to finish. */
    if (!radio_net_worker_stop()) {
        RADIO_DBG(printf("radio-netshutdown: net worker did not confirm shutdown within the timeout\n"););
    } else if (radio_runtime_diag_leak_ssl_enabled()) {
        /* MP3_DIAG_LEAK_SSL is an explicit diagnostic mode; production
         * shutdown always waits for the worker-owned lifecycle teardown. */
        RADIO_DBG(printf("radio-netshutdown: worker exited with final AmiSSL/library teardown deliberately skipped (MP3_DIAG_LEAK_SSL)\n"););
    }
#else
    if (SocketBase) {
        if (radio_open_socket_count == 0) {
            CloseLibrary(SocketBase);
            SocketBase = NULL;
            radio_socket_library_close_count++;
            RADIO_DBG(printf("radio-netshutdown: bsdsocket.library closed socket_library_close_count=%ld\n", radio_socket_library_close_count););
        } else {
            /* A socket is somehow still open (leaked/late child) -- closing
             * the library out from under it is worse than leaking it for the
             * few ms until process exit. */
            RADIO_DBG(printf("radio-netshutdown: bsdsocket.library left open (unsafe) open_socket_count=%ld\n", radio_open_socket_count););
        }
    }
#endif
    RADIO_DBG(printf("radio-netshutdown: final counters socket_library_open_count=%ld socket_library_close_count=%ld amisslmaster_open_count=%ld amisslmaster_close_count=%ld openamissltags_count=%ld closeamissl_count=%ld amissl_init_count=%ld amissl_cleanup_count=%ld active_socket_count=%ld active_ssl_count=%ld active_ssl_ctx_count=%ld tls_poisoned=%d poison_reason=\"%s\"\n",
        radio_socket_library_open_count, radio_socket_library_close_count,
        radio_amisslmaster_open_count, radio_amisslmaster_close_count,
        radio_openamissltags_count, radio_closeamissl_count,
        radio_amissl_init_count, radio_amissl_cleanup_count,
        radio_open_socket_count, radio_active_ssl_count,
        radio_active_ssl_ctx_count, Radio_IsTlsPoisoned(),
        Radio_TlsPoisonReason()););
    RADIO_DBG(printf("radio-netshutdown: tls_fault_count=%ld shutdown_quarantine=%d\n",
        radio_tls_fault_count, radio_tls_shutdown_quarantine););
#endif
}

/* The bulk of Radio_Pump()'s work (SSL_read()/recv(), reconnect_http(),
 * close_current_socket() on error/stop) touches bsdsocket.library/AmiSSL, so
 * in HAVE_AMISSL builds this only ever runs on the net worker task
 * -- the net worker's autonomous loop calls this for registered streams.
 * Radio_Pump() itself is intentionally only a cheap status/buffer check in
 * those builds. Non-AmiSSL builds still call it directly, unchanged. */
static int radio_pump_body(RadioStream *rs)
{
    unsigned char b[RADIO_READ_CHUNK];
    int n, wb, requested, consumed;
    unsigned long usedSnapshot, sizeSnapshot, ringFreeSnapshot;
    int headerDoneSnapshot, parseStateSnapshot, audioUntilMetaSnapshot, metaLeftSnapshot;
    if (!rs || rs->status == RADIO_STATUS_ERROR) return -1;
    radio_net_adopt_context(rs);
    radio_stream_lock(rs);
    if (!radio_session_current_locked(rs, "radio_pump_body entry")) {
        radio_stream_unlock(rs);
        return 0;
    }
    radio_stream_unlock(rs);
    if (rs->fatalStop) { set_error(rs, "TLS read failed"); return -1; }
    if (radio_is_stopping(rs)) {
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
        RADIO_DBG(printf("radio-pump: stop/detach observed before SSL_read session=%lu -- no further read\n", rs->session_id);)
        return 0;
#else
        close_current_socket(rs); rs->status = RADIO_STATUS_CLOSED; return 0;
#endif
    }
    if (rs->sock == RADIO_INVALID_SOCKET) {
        if (!rs->everPlayed) { set_error(rs, "radio stream closed before playback started"); return -1; }
        return reconnect_http(rs);
    }
    wb = 0;
    requested = (int)sizeof(b);
    radio_stream_lock(rs);
    usedSnapshot = rs->used;
    sizeSnapshot = rs->size;
    ringFreeSnapshot = sizeSnapshot > usedSnapshot ? sizeSnapshot - usedSnapshot : 0;
    headerDoneSnapshot = rs->headerDone;
    parseStateSnapshot = (int)rs->parseState;
    audioUntilMetaSnapshot = rs->audioUntilMeta;
    metaLeftSnapshot = rs->metaLeft;
    radio_stream_unlock(rs);
    if (headerDoneSnapshot) {
        if (ringFreeSnapshot == 0) {
            rs->workerBackpressureCount++;
            if (rs->workerBackpressureCount == 1 || (rs->workerBackpressureCount % 25UL) == 0) {
                RADIO_DBG(printf("radio-worker: backpressure session=%lu ringFill=%lu ringFree=%lu parseState=%d audioUntilMeta=%d metaLeft=%d -- not reading socket\n",
                    rs->session_id, usedSnapshot, ringFreeSnapshot, parseStateSnapshot,
                    audioUntilMetaSnapshot, metaLeftSnapshot);)
            }
            radio_backoff_sleep();
            radio_worker_maybe_log_stats(rs);
            return 0;
        }
        if ((unsigned long)requested > ringFreeSnapshot)
            requested = (int)ringFreeSnapshot;
    }
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    radio_stream_lock(rs);
    if (rs->generation != radio_current_generation ||
        rs->workerStopRequested || rs->workerClosing || rs->workerCloseRequested ||
        rs->workerDetached || !rs->workerRegistered ||
        rs->status == RADIO_STATUS_STOPPING || rs->status == RADIO_STATUS_CLOSED ||
        rs->status == RADIO_STATUS_ERROR) {
        RadioStatus stopStatus = rs->status;
        int stopReq = rs->workerStopRequested;
        int closing = rs->workerClosing;
        int closeReq = rs->workerCloseRequested;
        int detached = rs->workerDetached;
        int registered = rs->workerRegistered;
        radio_stream_unlock(rs);
        RADIO_DBG(printf("radio-pump: stop/detach observed before SSL_read session=%lu status=%d stopReq=%d closing=%d closeReq=%d registered=%d detached=%d -- no further read\n",
            rs->session_id, (int)stopStatus, stopReq, closing, closeReq, registered, detached);)
        return 0;
    }
    radio_stream_unlock(rs);
    if (rs->isSSL && rs->ssl) {
        radio_worker_risk_log("before SSL_read", rs);
        RADIO_DBG(printf("radio-ssl-read: session=%lu sslHandshakeDone=%d before SSL_read\n", rs->session_id, rs->sslHandshakeDone););
        if (rs->sslHandshakeDone != 1) {
            RADIO_DBG(printf("radio-ssl-read: ERROR session=%lu skipped SSL_read because handshake is incomplete sslHandshakeDone=%d\n", rs->session_id, rs->sslHandshakeDone););
            set_error(rs, "TLS handshake incomplete");
            close_current_socket(rs);
            return -1;
	        }
	        radio_net_adopt_context(rs);
	        rs->workerReadCalls++;
            ERR_clear_error(); /* clean queue so ERR_get_error() below reflects only this SSL_read: a bare SYSCALL/EPIPE drop must stay lib_error=0 and quarantine, not poison */
            radio_amissl_lifecycle_diag("SSL_read-before", rs->session_id, rs->ssl, rs->ctx);
	        n = (int)SSL_read(rs->ssl, (char *)b, requested);
            radio_amissl_lifecycle_diag("SSL_read-after", rs->session_id, rs->ssl, rs->ctx);
            radio_worker_risk_log("after SSL_read", rs);
	        RADIO_DBG(printf("radio-ssl-read: session=%lu ssl=%p ctx=%p fd=%ld dst=%p dst_cap=%d requested=%d returned=%d fill=%lu ring_free=%lu\n",
	            rs->session_id, (void *)rs->ssl, (void *)rs->ctx, (long)rs->sock, (void *)b, (int)sizeof(b), requested, n, rs->used, rs->size > rs->used ? rs->size - rs->used : 0));
	        if (n <= 0) {
	            int e = SSL_get_error(rs->ssl, n);
	            RADIO_DBG(printf("radio-ssl-read: session=%lu SSL_get_error=%d ret=%d fd=%ld\n", rs->session_id, e, n, (long)rs->sock));
	            if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) { wb = 1; rs->workerWantReadCount++; }
	            else if (e == SSL_ERROR_ZERO_RETURN) {
	                RADIO_DBG(printf("radio-ssl-read: session=%lu SSL_ERROR_ZERO_RETURN clean close\n", rs->session_id));
	            }
            else {
                /* Not a "call again later" condition -- a real record-layer
                 * failure (bad MAC, unexpected message, truncated record,
                 * ...). The handshake-failure path already logs the
                 * OpenSSL/AmiSSL error-queue detail behind SSL_get_error()'s
                 * bare numeric code; this path never did, so a read fault
                 * like this had no detail to go on. This SSL object is about
                 * to be torn down via close_current_socket(); fatal cases
                 * quarantine the SSL instead of freeing it. */
                unsigned long ssl_lib_error = ERR_get_error();
                char ssl_error_buf[160];
                ssl_error_buf[0] = '\0';
                if (ssl_lib_error != 0)
                    ERR_error_string_n(ssl_lib_error, ssl_error_buf, sizeof(ssl_error_buf));
                RADIO_DBG(printf("radio-ssl-read: session=%lu read failed ssl_error=%d lib_error=%08lx (%s)\n",
                    rs->session_id, e, ssl_lib_error, ssl_error_buf[0] ? ssl_error_buf : "none"));
                if (radio_ssl_error_is_fatal(e)) {
                    rs->lastSslError = e;
                    rs->noReconnect = 1;
                    if (!radio_tls_fault_should_poison(e, ssl_lib_error))
                        rs->sslDroppedTransport = 1; /* peer close: leak SSL, keep HTTPS usable */
                    strcpy(rs->lastSslOp, (e == SSL_ERROR_SYSCALL && ssl_lib_error == 0) ?
                        "ssl-read-syscall" : "ssl-read-fatal");
                    RADIO_DBG(printf("radio-ssl-read: session=%lu read failure; connection SSL quarantined (poison=%d)\n", rs->session_id, radio_tls_fault_should_poison(e, ssl_lib_error)));
                    ERR_clear_error();
                }
            }
        }
    } else
	#endif
	    {
	        rs->workerReadCalls++;
	        n = (int)recv(rs->sock, (char *)b, requested, 0);
	        if (n < 0 && radio_would_block()) { wb = 1; rs->workerWantReadCount++; }
	    }
	    if (n > 0) rs->workerReadBytes += (unsigned long)n;
	    else rs->workerPumpZeroCount++;
	    radio_worker_maybe_log_stats(rs);
	    if (radio_is_stopping(rs)) {
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
            RADIO_DBG(printf("radio-pump: stop/detach observed after SSL_read session=%lu -- close deferred to worker close job\n", rs->session_id);)
            return 0;
#else
            close_current_socket(rs); rs->status = RADIO_STATUS_CLOSED; return 0;
#endif
        }
    /* non-blocking socket (or SSL WANT_READ): no data yet — yield */
    if (n < 0 && wb) {
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
        /* The autonomous worker loop yields after this pump pass. Sleeping
         * here as well caused two delays after every empty socket read. */
#else
        radio_backoff_sleep();
#endif
        if (radio_note_start_wait(rs, rs->isSSL ? "HTTPS stream start timeout" : "radio stream start timed out") < 0) return -1;
        if (rs->everPlayed && ++rs->stallPumps >= RADIO_STALL_TIMEOUT_PUMPS) {
            /* Fail the stream through the same set_error/close path as the
             * startup timeout (radio_note_start_wait) so the decoder unwinds
             * and the playback child exits cleanly.  Deliberately NOT an
             * automatic reconnect: tearing down and re-creating the AmiSSL
             * session unattended exercises the SSL_free()/SSL_CTX churn this
             * file's fault-quarantine comments exist for, and doing that in a
             * loop against an already-dead relay risks exactly the alerts the
             * quarantine defends against.  The user can restart the stream
             * from a clean session with one click instead. */
            RADIO_DBG(printf("radio-stream: session=%lu stalled for %d silent pumps, failing stream url=\"%s\"\n", rs->session_id, rs->stallPumps, rs->url););
            rs->stallPumps = 0;
            set_error(rs, "radio stream stalled - no data received");
            close_current_socket(rs);
            return -1;
        }
        return 0;
    }
    if (n <= 0) {
        close_current_socket(rs);
        if (rs->fatalStop || rs->noReconnect) {
            /* A fatal SSL fault (SSL_ERROR_SSL/SYSCALL/unknown) was just
             * classified above: this session is terminal, full stop. Every
             * other branch below either waits for the AAC/HTTPS stream-start
             * timeout or schedules a reconnect -- both re-enter
             * SSL_connect()/SSL_read() against per-task AmiSSL state this
             * fault may have already damaged, which is what looped
             * "close mode=abort" for a long time and eventually corrupted
             * the exec heap. Fail the stream now instead. */
            set_error(rs, "TLS read failed");
            RADIO_DBG(printf("radio-stream: session=%lu TLS/socket failure marked noReconnect -- refusing reconnect/timeout wait, failing stream\n", rs->session_id));
            return -1;
        }
        if (!rs->headerDone || !rs->firstDataLogged) { rs->decoderStarted = 0; rs->streamStateFlags &= ~DECODER_STARTED; set_error(rs, rs->isSSL ? "HTTPS read failed" : "HTTP header read failed"); RADIO_OPEN_DEBUG_PRINTF(("radio-open: HTTP header failed before ready headerDone=%d firstData=%d\n", rs->headerDone, rs->firstDataLogged)); return -1; }
        if (!rs->everPlayed) { set_error(rs, "radio stream ended before audio buffered"); return -1; }
        rs->reconnectDelay = RADIO_RECONNECT_BACKOFF_PUMPS;
        set_status(rs, RADIO_STATUS_RECONNECTING);
        return 0;
    }
    rs->zeroBytePumps = 0;
    rs->stallPumps = 0;
    if (!rs->firstDataLogged) {
        RADIO_DBG(printf("radio-stream: first data received fd=%ld ssl=%p\n",
            (long)rs->sock,
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
            (void *)rs->ssl
#else
            (void *)0
#endif
        ));
        rs->firstDataLogged = 1;
    }
    if (!rs->everPlayed) rs->startPumps = 0;
    consumed = process_bytes(rs, b, n);
    if (consumed < 0) return -1;
    if (consumed < n) {
        unsigned long fill, freeBytes;
        int parseState, audioUntilMeta, metaLeft;
        rs->workerPartialConsumeCount++;
        rs->workerDroppedInputPreventedCount += (unsigned long)(n - consumed);
        radio_stream_lock(rs);
        fill = rs->used;
        freeBytes = rs->size > rs->used ? rs->size - rs->used : 0;
        parseState = (int)rs->parseState;
        audioUntilMeta = rs->audioUntilMeta;
        metaLeft = rs->metaLeft;
        radio_stream_unlock(rs);
        radio_trace("radio-pump: ERROR would drop input bytes session=%lu consumed=%d total=%d ringFill=%lu ringFree=%lu parseState=%d audioUntilMeta=%d metaLeft=%d\n",
            rs->session_id, consumed, n, fill, freeBytes, parseState, audioUntilMeta, metaLeft);
        set_error(rs, "radio pump backpressure would drop stream bytes");
        return -1;
    }
    if (rs->status == RADIO_STATUS_PLAYING || rs->everPlayed) {
        clock_t now = clock();
        if (!rs->lastMemReportClock) rs->lastMemReportClock = now;
        if ((unsigned long)((now - rs->lastMemReportClock) * 1000UL / CLOCKS_PER_SEC) >= 10000UL) {
            radio_debug_mem_report(rs->session_id, "playing 10s sample");
            Radio_DebugCheckExecMem("playing 10s sample");
            rs->lastMemReportClock = now;
        }
    }
    if (radio_is_stopping(rs)) { close_current_socket(rs); rs->status = RADIO_STATUS_CLOSED; return 0; }
    if (rs->headerDone && rs->used >= RADIO_START_THRESHOLD) {
        if (!rs->everPlayed) {
            RADIO_DBG(printf("radio-stream: first decoder frame / playback buffer ready fd=%ld\n", (long)rs->sock);)
            /* Narrows the corruption window seen on session 4 of the SSL/MP3
             * soak test: the ring's own canary passed every ring_write()
             * during buffering (those checks are silent on success), but was
             * found zeroed by the time MP3InitDecoder() ran. This is the
             * exact instant buffering completes, still inside radio_stream.c
             * -- if this check ever fails, the corruption is happening
             * somewhere in this file's own buffering/parse path, not in the
             * decoder/audio-device setup that runs after this point. */
            radio_ring_check_canary(rs, "buffering complete, before decoder handoff");
        }
        rs->reconnectAttempts = 0; rs->reconnectDelay = 0; rs->everPlayed = 1; rs->playbackStarted = 1; rs->streamStateFlags |= PLAYBACK_STARTED; set_status(rs, RADIO_STATUS_PLAYING);
    } else if (rs->headerDone && rs->status != RADIO_STATUS_PLAYING)
        set_status(rs, RADIO_STATUS_BUFFERING);
    return n;
}

int Radio_Pump(RadioStream *rs)
{
    if (!rs) return -1;
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    {
        RadioStatus status;
        unsigned long used;
        int headerDone;
        radio_stream_lock(rs);
        status = rs->status;
        used = rs->used;
        headerDone = rs->headerDone;
        radio_stream_unlock(rs);
        if (status == RADIO_STATUS_ERROR) return -1;
        if (status == RADIO_STATUS_CLOSED || status == RADIO_STATUS_STOPPING) return 0;
        if (headerDone && used >= RADIO_START_THRESHOLD && status != RADIO_STATUS_PLAYING)
            set_status(rs, RADIO_STATUS_PLAYING);
        if (used > 0) return (int)used;
        if (status == RADIO_STATUS_CONNECTING || status == RADIO_STATUS_BUFFERING || status == RADIO_STATUS_RECONNECTING)
            radio_backoff_sleep();
        return 0;
    }
#else
    return radio_pump_body(rs);
#endif
}

int Radio_ReadAudio(RadioStream *rs,unsigned char *buf,int maxBytes)
{
	int got;
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
	RadioStatus status;
	unsigned long used;
	int headerDone;
	int decoderStarted;
	int everPlayed;
	int stopping;
#endif
	if(!rs||!buf||maxBytes<=0)return 0;
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
	radio_stream_lock(rs);
	status = rs->status;
	used = rs->used;
	headerDone = rs->headerDone;
	decoderStarted = rs->decoderStarted;
	everPlayed = rs->everPlayed;
	stopping = rs->stopping || rs->workerStopRequested || rs->workerClosing ||
		rs->workerCloseRequested || rs->workerDetached;
	radio_stream_unlock(rs);
	if(stopping || status==RADIO_STATUS_STOPPING || status==RADIO_STATUS_CLOSED || status==RADIO_STATUS_ERROR)
		return 0;
	if(!headerDone || !decoderStarted) {
		if(status==RADIO_STATUS_PLAYING || status==RADIO_STATUS_BUFFERING || status==RADIO_STATUS_CONNECTING || status==RADIO_STATUS_RECONNECTING)
			RADIO_DBG(printf("radio-read: transient zero session=%lu status=%d used=%lu headerDone=%d decoderStarted=%d everPlayed=%d stopping=%d\n",
				rs->session_id, (int)status, used, headerDone, decoderStarted, everPlayed, stopping);)
		return 0;
	}
	/* Do not nibble newly-arrived packets immediately after an underrun.
	 * Once the stream enters BUFFERING, wait for the normal start threshold
	 * before handing compressed bytes back to the decoder.  This gives the
	 * 256 KiB ring real hysteresis instead of repeated play/pause bursts. */
	if(status==RADIO_STATUS_BUFFERING) {
		if(used<RADIO_START_THRESHOLD)
			return 0;
		RADIO_DBG(printf("radio-read: rebuffer complete session=%lu fill=%lu threshold=%lu\n",
			rs->session_id, used, (unsigned long)RADIO_START_THRESHOLD);)
		set_status(rs,RADIO_STATUS_PLAYING);
		status=RADIO_STATUS_PLAYING;
	}
	if(used==0) {
		if(everPlayed && status==RADIO_STATUS_PLAYING) {
			RADIO_DBG(printf("radio-read: underrun session=%lu fill=0 entering buffering\n",
				rs->session_id);)
			set_status(rs,RADIO_STATUS_BUFFERING);
		}
		return 0;
	}
#else
	if(radio_is_stopping(rs)) return 0;
	while(!radio_is_stopping(rs) && rs->status!=RADIO_STATUS_PLAYING && rs->used<RADIO_START_THRESHOLD && rs->status!=RADIO_STATUS_ERROR) {
		if(Radio_Pump(rs)<=0 && !rs->everPlayed && (++rs->zeroBytePumps>=RADIO_ZERO_BYTE_PUMP_MAX || radio_note_start_wait(rs,"radio stream did not buffer audio")<0)) {
			if(rs->status!=RADIO_STATUS_ERROR) set_error(rs,"radio stream did not buffer audio");
			break;
		}
	}
	while(!radio_is_stopping(rs) && rs->used==0 && rs->status!=RADIO_STATUS_ERROR) {
		if(Radio_Pump(rs)<=0 && !rs->everPlayed && (++rs->zeroBytePumps>=RADIO_ZERO_BYTE_PUMP_MAX || radio_note_start_wait(rs,"radio stream did not deliver audio")<0)) {
			if(rs->status!=RADIO_STATUS_ERROR) set_error(rs,"radio stream did not deliver audio");
			break;
		}
	}
	if(radio_is_stopping(rs)||!rs->headerDone||!rs->decoderStarted||rs->status==RADIO_STATUS_ERROR) return 0;
#endif
	got=ring_read(rs,buf,maxBytes);
	/* everPlayed is true after the first successful prebuffer.  The old
	 * !everPlayed test therefore disabled low-water rebuffering for the
	 * entire steady-state stream and caused rapid stop/start playback. */
	if(rs->everPlayed && rs->status==RADIO_STATUS_PLAYING && rs->used<RADIO_LOW_WATER_BYTES) {
		RADIO_DBG(printf("radio-read: low water session=%lu fill=%lu threshold=%lu entering buffering\n",
			rs->session_id, rs->used, (unsigned long)RADIO_LOW_WATER_BYTES);)
		set_status(rs,RADIO_STATUS_BUFFERING);
	}
	if(rs->status==RADIO_STATUS_BUFFERING && rs->used>=RADIO_START_THRESHOLD)
		set_status(rs,RADIO_STATUS_PLAYING);
	return got;
}
int Radio_ReadStartupAudio(RadioStream *rs,unsigned char *buf,int maxBytes,unsigned long timeoutMs){ clock_t start; int got; if(!rs||!buf||maxBytes<=0)return 0; start=clock(); while(!radio_is_stopping(rs)&&rs->used==0&&rs->status!=RADIO_STATUS_ERROR){ if(Radio_Pump(rs)<0)break; if(timeoutMs>0 && (unsigned long)((clock()-start)*1000UL/CLOCKS_PER_SEC)>=timeoutMs){ set_error(rs,"AAC stream start timeout"); close_current_socket(rs); break; } } if(radio_is_stopping(rs)||!rs->headerDone||!rs->decoderStarted||rs->status==RADIO_STATUS_ERROR) return 0; got=ring_read(rs,buf,maxBytes); if(!rs->everPlayed&&rs->headerDone&&rs->status!=RADIO_STATUS_PLAYING&&rs->status!=RADIO_STATUS_ERROR) set_status(rs,RADIO_STATUS_BUFFERING); return got; }
void Radio_FailStartup(RadioStream *rs,const char *message){ if(!rs)return; set_error(rs,message&&message[0]?message:"AAC stream start timeout"); radio_stream_lock(rs); rs->stopping=1; rs->reconnectAttempts=RADIO_RECONNECT_MAX; rs->reconnectDelay=0; radio_stream_unlock(rs); close_current_socket(rs); }
RadioStatus Radio_GetStatus(RadioStream *rs){ return rs?rs->status:RADIO_STATUS_CLOSED; }
const char *Radio_GetTitle(RadioStream *rs){ return rs?rs->title:""; }
const char *Radio_GetStationName(RadioStream *rs){ return rs?rs->stationName:""; }
const char *Radio_GetGenre(RadioStream *rs){ return rs?rs->genre:""; }
const char *Radio_GetStreamUrl(RadioStream *rs){ return rs?rs->streamUrl:""; }
int Radio_GetMetaInt(RadioStream *rs){ return rs?rs->metaint:0; }
const char *Radio_GetContentType(RadioStream *rs){ return rs?rs->contentType:""; }
const char *Radio_GetError(RadioStream *rs){ return rs?(rs->error[0]?rs->error:""):"radio not open"; }
int Radio_GetBitrate(RadioStream *rs){ return rs?rs->bitrate:0; }
int Radio_GetBufferedBytes(RadioStream *rs){ return rs?(int)rs->used:0; }
unsigned long Radio_GetSessionId(RadioStream *rs){ return rs?rs->session_id:0; }
int Radio_IsSessionFatal(RadioStream *rs){
#if defined(AMIGA_M68K) && defined(HAVE_AMISSL)
    return rs ? (rs->sslStatePoisoned || rs->fatalStop) : 0;
#else
    return rs ? rs->fatalStop : 0;
#endif
}
const char *Radio_StatusText(RadioStatus s){ switch(s){case RADIO_STATUS_CONNECTING:return "Connecting";case RADIO_STATUS_BUFFERING:return "Buffering";case RADIO_STATUS_PLAYING:return "Playing";case RADIO_STATUS_RECONNECTING:return "Reconnecting";case RADIO_STATUS_STOPPING:return "Stopping";case RADIO_STATUS_CLOSED:return "Closed";case RADIO_STATUS_ERROR:return "Error";default:return "Idle";} }

#endif /* ENABLE_RADIO */
