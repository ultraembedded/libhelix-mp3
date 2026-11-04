#ifdef MINTAMP_SKIN
#include "skin_controls.h"
#endif
/* Minimal AmigaOS/m68k-friendly command-line MP3 decoder.
 *
 * Builds the public decoder (mp3dec.c, mp3tabs.c) plus the portable real C files and writes raw
 * PCM or Amiga IFF-8SVX audio.  The code intentionally uses plain C library
 * calls only so it can be compiled by m68k-amigaos-gcc for 68020 systems.
 */

#include <stdio.h>
#include "integer_stats.h"
#include <stdlib.h>
#include <string.h>
#include "miniamp_memguard.h"
#include "radio_stream.h"
#include "radio_oggflac.h"
#if ENABLE_RADIO && !defined(main)
#include "radio_stream_probe.h"
#endif
#include <time.h>
#include <stdarg.h>
#ifndef AMIGA_M68K
#include <signal.h>
#include <unistd.h>
#endif

#if defined(AMIGA_M68K)
#include <exec/semaphores.h>
#include <proto/exec.h>
/* Storage for the cross-task stdout lock declared extern by radio_debug.h/
 * radio_stream.h (RADIO_DBG/RADIO_STOP_DEBUG_PRINTF) and by this file's own
 * MiniAmp3Printf-family wrappers below. minimp3r.c's real main() -- not
 * amiga_mp3dec.c's own main(), which becomes HelixAmp3CliMain, the
 * per-playback-child entry point, when included into minimp3r.c -- calls
 * InitSemaphore() on this exactly once, before any worker task or playback
 * child that could race on it is ever spawned. */
struct SignalSemaphore radio_console_lock;
#endif

#if defined(AMIGA_M68K) && (defined(__amigaos__) || defined(__AMIGA__) || defined(__MORPHOS__))
#define HAVE_AMIGA_AUDIO_DEVICE 1
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/io.h>
#include <exec/ports.h>
#include <devices/audio.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#ifndef AUDIONAME
#define AUDIONAME "audio.device"
#endif
#endif

#include "mp3dec.h"
#include "assembly.h"
#include "statname.h"

#define MINTAMP_CLI_VERSION "1.4.0"
#if defined(AMIGA_M68K) && !defined(MINTAMP_EMBEDDED_FRONTEND)
/* Version metadata for the standalone amiga_mp3dec.fastexp edition.  GUI
 * translation units define MINTAMP_EMBEDDED_FRONTEND before including this
 * source so their binaries expose only the frontend-specific Version tag. */
static const char gMintAmpCliVersionTag[] __attribute__((used)) =
	"\0$VER: amiga_mp3dec.fastexp " MINTAMP_CLI_VERSION " (08.10.2026)";
#endif

/*
 * AmigaOS priority for the playback child process (minimp3r.c's radio
 * player and amiga_mp3gui.c's file player both create theirs with this).
 * 0 (default/Workbench priority) keeps CPU-bound decoding from starving
 * the GUI's own event loop -- otherwise Stop becomes hard to press during
 * heavy decode, the same tradeoff that makes Songplayer feel like it
 * nearly locks up the GUI while it plays.
 *
 * Tried bumping this to 1 to let playback preempt the GUI process on
 * contention; real-hardware testing showed the app doing "weird things"
 * with the bump active, so this is back to 0 (the original, tested
 * behavior) rather than trading a hiccup-reduction guess for a worse,
 * less predictable failure mode. Revisit only with a specific, reproduced
 * symptom in hand, not as a blind A/B.
 */
#ifndef AMIGA_PLAYBACK_TASK_PRIORITY
#define AMIGA_PLAYBACK_TASK_PRIORITY 0
#endif

volatile int gMiniAmp3EmbeddedPlayback;
static int gMiniAmp3DebugPlayRequested;

#if defined(AMIGA_M68K)
/* Shared GUI/decoder stop latch. */
static volatile int gPlaybackInterrupted;
#else
static volatile sig_atomic_t gPlaybackInterrupted;
#endif

static int MiniAmp3ConsoleSuppressed(void)
{
	return gMiniAmp3EmbeddedPlayback != 0;
}

#if defined(AMIGA_M68K)
/* Every printf/fprintf/fputs/puts/putchar/fwrite call in this file (and,
 * since minimp3r.c #includes it as one translation unit, in minimp3r.c's own
 * GUI code too) funnels through these Mini* wrappers via the #define printf
 * MiniAmp3Printf-style redirect below. The net worker task, every playback
 * child, and the GUI task each call these concurrently against the same
 * shared stdout -- confirmed directly in a captured field log, where two
 * tasks' printf() output was physically spliced together mid-line. Lock
 * around the actual I/O call in each wrapper (radio_console_lock, shared
 * with radio_stream.c's RADIO_DBG macros via radio_debug.h/radio_stream.h,
 * initialized once at true program startup in minimp3r.c's real main()) so
 * concurrent debug/status output can no longer corrupt the C library's
 * shared stdio buffer state. radio_console_lock itself is defined near the
 * top of this file. */
#define MINIAMP3_IO_LOCK() ObtainSemaphore(&radio_console_lock)
#define MINIAMP3_IO_UNLOCK() ReleaseSemaphore(&radio_console_lock)
#else
#define MINIAMP3_IO_LOCK() ((void)0)
#define MINIAMP3_IO_UNLOCK() ((void)0)
#endif

static int MiniAmp3Printf(const char *fmt, ...)
{
	int r;
	va_list ap;
	if (MiniAmp3ConsoleSuppressed())
		return 0;
	MINIAMP3_IO_LOCK();
	va_start(ap, fmt);
	r = vprintf(fmt, ap);
	va_end(ap);
	MINIAMP3_IO_UNLOCK();
	return r;
}

static int MiniAmp3Fprintf(FILE *stream, const char *fmt, ...)
{
	int r;
	va_list ap;
	if (MiniAmp3ConsoleSuppressed() && (stream == stdout || stream == stderr))
		return 0;
	MINIAMP3_IO_LOCK();
	va_start(ap, fmt);
	r = vfprintf(stream, fmt, ap);
	va_end(ap);
	MINIAMP3_IO_UNLOCK();
	return r;
}

static int MiniAmp3Fputs(const char *s, FILE *stream)
{
	int r;
	if (MiniAmp3ConsoleSuppressed() && (stream == stdout || stream == stderr))
		return 0;
	MINIAMP3_IO_LOCK();
	r = fputs(s, stream);
	MINIAMP3_IO_UNLOCK();
	return r;
}

static int MiniAmp3Puts(const char *s)
{
	int r;
	if (MiniAmp3ConsoleSuppressed())
		return 0;
	MINIAMP3_IO_LOCK();
	r = puts(s);
	MINIAMP3_IO_UNLOCK();
	return r;
}

static int MiniAmp3Putchar(int c)
{
	int r;
	if (MiniAmp3ConsoleSuppressed())
		return c;
	MINIAMP3_IO_LOCK();
	r = putchar(c);
	MINIAMP3_IO_UNLOCK();
	return r;
}

static int MiniAmp3Fflush(FILE *stream)
{
	int r;
	if (MiniAmp3ConsoleSuppressed() && (!stream || stream == stdout || stream == stderr))
		return 0;
	MINIAMP3_IO_LOCK();
	r = fflush(stream);
	MINIAMP3_IO_UNLOCK();
	return r;
}

static size_t MiniAmp3Fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream)
{
	size_t r;
	if (MiniAmp3ConsoleSuppressed() && (stream == stdout || stream == stderr))
		return nmemb;
	MINIAMP3_IO_LOCK();
	r = fwrite(ptr, size, nmemb, stream);
	MINIAMP3_IO_UNLOCK();
	return r;
}

#ifdef RADIO_DEBUG
/* Bypasses MiniAmp3ConsoleSuppressed() entirely -- defined before the
 * printf -> MiniAmp3Printf redirect below, so its own vprintf() call is the
 * real C library one, not the suppressible wrapper.  Every other printf()
 * in this file goes silent for the whole radio playback child (Embedded
 * Playback sets gMiniAmp3EmbeddedPlayback for its entire run), which is why
 * PrintPlaybackCleanupStatus()'s playback buffer canary result -- the one
 * diagnostic that reports on AllocMem-based buffers the malloc-based
 * MiniMem guard cannot see -- never showed up in any radio session log. */
static void RadioDebugUnsuppressedPrintf(const char *fmt, ...)
{
	va_list ap;
	MINIAMP3_IO_LOCK();
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	fflush(stdout);
	MINIAMP3_IO_UNLOCK();
}
#endif

#ifdef printf
#undef printf
#endif
#ifdef fprintf
#undef fprintf
#endif
#ifdef fputs
#undef fputs
#endif
#ifdef puts
#undef puts
#endif
#ifdef putchar
#undef putchar
#endif
#ifdef fflush
#undef fflush
#endif
#define printf MiniAmp3Printf
#define fprintf MiniAmp3Fprintf
#define fputs MiniAmp3Fputs
#define puts MiniAmp3Puts
#define putchar MiniAmp3Putchar
#define fflush MiniAmp3Fflush
#define fwrite MiniAmp3Fwrite
#ifdef RADIO_DEBUG
#define RADIO_INPUT_DIAG_PRINTF RadioDebugUnsuppressedPrintf
#else
#define RADIO_INPUT_DIAG_PRINTF MiniAmp3Printf
#endif

#if defined(AMIGA_M68K)
/* Tell AmigaOS to provide at least 256 KB of stack for this executable. */
static const char amigaStackCookie[] __attribute__((used, aligned(4))) = "$STACK:262144";
#endif

void STATNAME(FDCT32)(int *x, int *d, int offset, int oddBlock, int gb);
void STATNAME(FDCT32_C_REFERENCE)(int *x, int *d, int offset, int oddBlock, int gb);
void STATNAME(FDCT32Half)(int *x, int *d, int offset, int oddBlock, int gb);
void STATNAME(FDCT32Half_TEST_ACTIVE)(int *x, int *d, int offset, int oddBlock, int gb);
int STATNAME(FDCT32Half_AMIGA_M68K_ASM_RUNTIME)(void);
void STATNAME(FDCT32Quarter)(int *x, int *d, int offset, int oddBlock, int gb, int phase, int stride);
int STATNAME(FDCT32_HAS_AMIGA_M68K_ASM_RUNTIME)(void);
void STATNAME(AntiAlias_C_REFERENCE)(int *x, int nBfly);
void STATNAME(AntiAlias_TEST_ACTIVE)(int *x, int nBfly);
int STATNAME(AntiAlias_HAS_AMIGA_M68K_ASM_RUNTIME)(void);
int STATNAME(IMDCT36_C_REFERENCE)(int *xCurr, int *xPrev, int *y, int btCurr, int btPrev, int blockIdx, int gb);
int STATNAME(IMDCT36_TEST_ACTIVE)(int *xCurr, int *xPrev, int *y, int btCurr, int btPrev, int blockIdx, int gb);
int STATNAME(IMDCT36_HAS_AMIGA_M68K_ASM_RUNTIME)(void);
int STATNAME(IMDCTThinOutputSelftest)(void);
int STATNAME(IMDCTSubbandCapSelftest)(void);
int STATNAME(AntiAliasSubbandCapSelftest)(void);
int STATNAME(IMDCT36AsmGeneralPathSelftest)(void);
int STATNAME(DequantSubbandCapSelftest)(void);
int STATNAME(CollapseStereoToMonoSelftest)(void);
int STATNAME(MidSideProcSubbandCapSelftest)(void);
int STATNAME(CollapseStereoToMonoSubbandCapSelftest)(void);
int STATNAME(IntensityProcSubbandCapSelftest)(void);
int STATNAME(FDCT32HalfSparse16Selftest)(void);
void STATNAME(PolyphaseMonoFast_C_REFERENCE)(short *pcm, int *vbuf, const int *coefBase);
void STATNAME(PolyphaseMonoFast_TEST_ACTIVE)(short *pcm, int *vbuf, const int *coefBase);
int STATNAME(PolyphaseMonoFast_HAS_AMIGA_M68K_ASM_RUNTIME)(void);
void STATNAME(PolyphaseStereo)(short *pcm, int *vbuf, const int *coefBase);
void MP3EnableFdct32HalfSparse16PreconditionCheck(int enabled);
extern volatile unsigned long gFdct32HalfSparse16PreconditionViolations;
extern volatile unsigned long gFdct32HalfSparse16PreconditionChecks;
int PolyphaseMonoFastLowrate(short *pcm, int *vbuf, const int *coefBase, int stride, int *phase);
int PolyphaseStereoFastLowrate(short *pcm, int *vbuf, const int *coefBase, int stride, int *phase);
int STATNAME(MonoFastPolyphaseStride3_Amiga_m68k_IsActive)(void);
int STATNAME(StereoFastPolyphaseStride3_Amiga_m68k_IsActive)(void);
int STATNAME(MonoFastPolyphaseStride5_Amiga_m68k_IsActive)(void);
int STATNAME(MonoFastPolyphaseStride4AllPhases_Amiga_m68k_IsActive)(void);
#define AMIGA_POLYPHASE_STEREO_FULL STATNAME(PolyphaseStereo)
#define AMIGA_POLYPHASE_MONO_STRIDE3_HAS_ASM STATNAME(MonoFastPolyphaseStride3_Amiga_m68k_IsActive)
#define AMIGA_POLYPHASE_STEREO_STRIDE3_HAS_ASM STATNAME(StereoFastPolyphaseStride3_Amiga_m68k_IsActive)
#define AMIGA_POLYPHASE_MONO_STRIDE5_HAS_ASM STATNAME(MonoFastPolyphaseStride5_Amiga_m68k_IsActive)
#define AMIGA_POLYPHASE_MONO_STRIDE4_ALLPHASES_HAS_ASM STATNAME(MonoFastPolyphaseStride4AllPhases_Amiga_m68k_IsActive)
int STATNAME(PolyphaseMonoFastLowrateStride2_C_REFERENCE)(short *pcm, int *vbuf, const int *coefBase);
int STATNAME(PolyphaseMonoFastLowrateStride2_TEST_ACTIVE)(short *pcm, int *vbuf, const int *coefBase);
int STATNAME(PolyphaseMonoFastLowrateStride2_HAS_AMIGA_M68K_ASM_RUNTIME)(void);
int STATNAME(PolyphaseMonoFastLowrateStride4_C_REFERENCE)(short *pcm, int *vbuf, const int *coefBase);
int STATNAME(PolyphaseMonoFastLowrateStride4_TEST_ACTIVE)(short *pcm, int *vbuf, const int *coefBase);
int STATNAME(PolyphaseMonoFastLowrateStride4_HAS_AMIGA_M68K_ASM_RUNTIME)(void);
int STATNAME(PolyphaseStereoFastLowrateStride2_C_REFERENCE)(short *pcm, int *vbuf, const int *coefBase);
int STATNAME(PolyphaseStereoFastLowrateStride2_TEST_ACTIVE)(short *pcm, int *vbuf, const int *coefBase);
int STATNAME(PolyphaseStereoFastLowrateStride2Reduced_C_REFERENCE)(short *pcm, int *vbuf, const int *coefBase);
int STATNAME(PolyphaseStereoFastLowrateStride2Reduced_TEST_ACTIVE)(short *pcm, int *vbuf, const int *coefBase);
int STATNAME(PolyphaseMonoFastLowrateStride2Reduced_C_REFERENCE)(short *pcm, int *vbuf, const int *coefBase);
int STATNAME(PolyphaseMonoFastLowrateStride2Reduced_TEST_ACTIVE)(short *pcm, int *vbuf, const int *coefBase);
int STATNAME(StereoFastPolyphaseStride2_HAS_AMIGA_M68K_ASM_RUNTIME)(void);
int STATNAME(StereoFastPolyphaseStride4_HAS_AMIGA_M68K_ASM_RUNTIME)(void);
void MP3GetStereoStride2PolyphaseCounters(unsigned long *asmCalls, unsigned long *cCalls,
	unsigned long *reducedCalls);
void MP3ResetStereoStride2PolyphaseCounters(void);
void MP3GetStereoStride4PolyphaseCounters(unsigned long *asmCalls, unsigned long *cCalls,
	unsigned long *reducedCalls);
void MP3ResetStereoStride4PolyphaseCounters(void);
void MP3GetMonoStride2PolyphaseCounters(unsigned long *asmCalls, unsigned long *cCalls,
	unsigned long *reducedCalls);
void MP3ResetMonoStride2PolyphaseCounters(void);
int STATNAME(PolyphaseStereoFastLowrateStride4_C_REFERENCE)(short *pcm, int *vbuf, const int *coefBase, int phase);
int STATNAME(PolyphaseStereoFastLowrateStride4_TEST_ACTIVE)(short *pcm, int *vbuf, const int *coefBase, int phase);
int STATNAME(PolyphaseStereoFastLowrateStride5_C_REFERENCE)(short *pcm, int *vbuf, const int *coefBase, int phase);
int STATNAME(PolyphaseStereoFastLowrateStride5_TEST_ACTIVE)(short *pcm, int *vbuf, const int *coefBase, int phase);
int STATNAME(StereoFastPolyphaseStride5_Amiga_m68k_IsActive)(void);
int STATNAME(PolyphaseMonoFastLowrateStride4Reduced_TEST_ACTIVE)(short *pcm, int *vbuf, const int *coefBase, int phase);
int STATNAME(PolyphaseStereoFastLowrateStride4Reduced_TEST_ACTIVE)(short *pcm, int *vbuf, const int *coefBase, int phase);
int STATNAME(PolyphaseStereoFastLowrateStride4Reduced_C_REFERENCE)(short *pcm, int *vbuf, const int *coefBase, int phase);
int STATNAME(PolyphaseStereoFastLowrateStride4Reduced_HAS_AMIGA_M68K_ASM_RUNTIME)(void);
#if defined(AMIGA_M68K) && defined(AMIGA_M68K_ASM_POLYPHASE)
extern void StereoFastPolyphaseStride4Half_Amiga_m68k(short *pcm, int *vbuf,
	const int *coefBase, int phase) __asm__("StereoFastPolyphaseStride4Half_Amiga_m68k")
	__attribute__((weak));
extern void StereoFastPolyphaseStride4HalfReduced_Amiga_m68k(short *pcm, int *vbuf,
	const int *coefBase, int phase) __asm__("StereoFastPolyphaseStride4HalfReduced_Amiga_m68k")
	__attribute__((weak));
extern void StereoFastPolyphaseStride4Phase0_Amiga_m68k(short *pcm, int *vbuf,
	const int *coefBase) __asm__("StereoFastPolyphaseStride4Phase0_Amiga_m68k")
	__attribute__((weak));
extern void StereoFastPolyphaseStride4Phase1_Amiga_m68k(short *pcm, int *vbuf,
	const int *coefBase) __asm__("StereoFastPolyphaseStride4Phase1_Amiga_m68k")
	__attribute__((weak));
extern void StereoFastPolyphaseStride4Phase2_Amiga_m68k(short *pcm, int *vbuf,
	const int *coefBase) __asm__("StereoFastPolyphaseStride4Phase2_Amiga_m68k")
	__attribute__((weak));
extern void StereoFastPolyphaseStride4Phase3_Amiga_m68k(short *pcm, int *vbuf,
	const int *coefBase) __asm__("StereoFastPolyphaseStride4Phase3_Amiga_m68k")
	__attribute__((weak));
extern volatile unsigned long StereoFastPolyphaseStride4Half_Amiga_m68k_PhaseCounts[4]
	__asm__("StereoFastPolyphaseStride4Half_Amiga_m68k_PhaseCounts")
	__attribute__((weak));
#endif
int STATNAME(AmigaM68KPolyphaseMonoFast_IsActive)(void);
int STATNAME(AmigaM68KPolyphaseMonoFastStride2_IsActive)(void);
int STATNAME(AmigaM68KPolyphaseMonoFastStride2Reduced_IsActive)(void);
int STATNAME(PolyphaseMonoFastLowrateStride2Reduced_HAS_AMIGA_M68K_ASM_RUNTIME)(void);
int STATNAME(DecodeHuffmanPairs_C_REFERENCE)(int *xy, int nVals, int tabIdx, int bitsLeft, unsigned char *buf, int bitOffset);
int STATNAME(DecodeHuffmanPairs_TEST_ACTIVE)(int *xy, int nVals, int tabIdx, int bitsLeft, unsigned char *buf, int bitOffset);
int STATNAME(DecodeHuffmanPairs_HAS_AMIGA_M68K_ASM_RUNTIME)(void);
const char *STATNAME(DecodeHuffmanPairs_AMIGA_M68K_ASM_NOTE)(void);
int STATNAME(DequantBlock_C_REFERENCE)(int *inbuf, int *outbuf, int num, int scale);
int STATNAME(DequantBlock_TEST_ACTIVE)(int *inbuf, int *outbuf, int num, int scale);
int STATNAME(DequantBlock_HAS_AMIGA_M68K_ASM_RUNTIME)(void);
int STATNAME(BitstreamRefillSelftest)(void);
extern const int STATNAME(polyCoef)[264];
#define AMIGA_FDCT32 STATNAME(FDCT32)
#define AMIGA_FDCT32_C_REFERENCE STATNAME(FDCT32_C_REFERENCE)
#define AMIGA_FDCT32_HALF STATNAME(FDCT32Half)
#define AMIGA_FDCT32_HALF_TEST_ACTIVE STATNAME(FDCT32Half_TEST_ACTIVE)
#define AMIGA_FDCT32_HALF_HAS_ASM STATNAME(FDCT32Half_AMIGA_M68K_ASM_RUNTIME)
#define AMIGA_FDCT32_QUARTER STATNAME(FDCT32Quarter)
#define AMIGA_FDCT32_HAS_ASM STATNAME(FDCT32_HAS_AMIGA_M68K_ASM_RUNTIME)
#define AMIGA_ANTIALIAS_C_REFERENCE STATNAME(AntiAlias_C_REFERENCE)
#define AMIGA_ANTIALIAS_TEST_ACTIVE STATNAME(AntiAlias_TEST_ACTIVE)
#define AMIGA_ANTIALIAS_HAS_ASM STATNAME(AntiAlias_HAS_AMIGA_M68K_ASM_RUNTIME)
#define AMIGA_IMDCT36_C_REFERENCE STATNAME(IMDCT36_C_REFERENCE)
#define AMIGA_IMDCT36_TEST_ACTIVE STATNAME(IMDCT36_TEST_ACTIVE)
#define AMIGA_IMDCT36_HAS_ASM STATNAME(IMDCT36_HAS_AMIGA_M68K_ASM_RUNTIME)
#define AMIGA_IMDCT_THIN_SELFTEST STATNAME(IMDCTThinOutputSelftest)
#define AMIGA_IMDCT_SUBBAND_CAP_SELFTEST STATNAME(IMDCTSubbandCapSelftest)
#define AMIGA_ANTIALIAS_SUBBAND_CAP_SELFTEST STATNAME(AntiAliasSubbandCapSelftest)
#define AMIGA_IMDCT36_ASM_GENERAL_PATH_SELFTEST STATNAME(IMDCT36AsmGeneralPathSelftest)
#define AMIGA_DEQUANT_SUBBAND_CAP_SELFTEST STATNAME(DequantSubbandCapSelftest)
#define AMIGA_COLLAPSE_STEREO_TO_MONO_SELFTEST STATNAME(CollapseStereoToMonoSelftest)
#define AMIGA_MIDSIDE_SUBBAND_CAP_SELFTEST STATNAME(MidSideProcSubbandCapSelftest)
#define AMIGA_COLLAPSE_STEREO_TO_MONO_SUBBAND_CAP_SELFTEST STATNAME(CollapseStereoToMonoSubbandCapSelftest)
#define AMIGA_INTENSITY_SUBBAND_CAP_SELFTEST STATNAME(IntensityProcSubbandCapSelftest)
#define AMIGA_FDCT32_HALF_SPARSE16_SELFTEST STATNAME(FDCT32HalfSparse16Selftest)
#define AMIGA_POLYPHASE_MONO_FAST_C_REFERENCE STATNAME(PolyphaseMonoFast_C_REFERENCE)
#define AMIGA_POLYPHASE_MONO_FAST_TEST_ACTIVE STATNAME(PolyphaseMonoFast_TEST_ACTIVE)
#define AMIGA_POLYPHASE_MONO_FAST_HAS_ASM STATNAME(PolyphaseMonoFast_HAS_AMIGA_M68K_ASM_RUNTIME)
#define AMIGA_POLYPHASE_MONO_FAST_STRIDE2_C_REFERENCE STATNAME(PolyphaseMonoFastLowrateStride2_C_REFERENCE)
#define AMIGA_POLYPHASE_MONO_FAST_STRIDE2_TEST_ACTIVE STATNAME(PolyphaseMonoFastLowrateStride2_TEST_ACTIVE)
#define AMIGA_POLYPHASE_MONO_FAST_STRIDE2_HAS_ASM STATNAME(PolyphaseMonoFastLowrateStride2_HAS_AMIGA_M68K_ASM_RUNTIME)
#define AMIGA_POLYPHASE_MONO_FAST_STRIDE4_C_REFERENCE STATNAME(PolyphaseMonoFastLowrateStride4_C_REFERENCE)
#define AMIGA_POLYPHASE_MONO_FAST_STRIDE4_TEST_ACTIVE STATNAME(PolyphaseMonoFastLowrateStride4_TEST_ACTIVE)
#define AMIGA_POLYPHASE_MONO_FAST_STRIDE4_HAS_ASM STATNAME(PolyphaseMonoFastLowrateStride4_HAS_AMIGA_M68K_ASM_RUNTIME)
#define AMIGA_POLYPHASE_STEREO_FAST_STRIDE2_C_REFERENCE STATNAME(PolyphaseStereoFastLowrateStride2_C_REFERENCE)
#define AMIGA_POLYPHASE_STEREO_FAST_STRIDE2_TEST_ACTIVE STATNAME(PolyphaseStereoFastLowrateStride2_TEST_ACTIVE)
#define AMIGA_POLYPHASE_STEREO_FAST_STRIDE2_REDUCED_C_REFERENCE STATNAME(PolyphaseStereoFastLowrateStride2Reduced_C_REFERENCE)
#define AMIGA_POLYPHASE_STEREO_FAST_STRIDE2_REDUCED_TEST_ACTIVE STATNAME(PolyphaseStereoFastLowrateStride2Reduced_TEST_ACTIVE)
#define AMIGA_POLYPHASE_MONO_FAST_STRIDE2_REDUCED_C_REFERENCE STATNAME(PolyphaseMonoFastLowrateStride2Reduced_C_REFERENCE)
#define AMIGA_POLYPHASE_MONO_FAST_STRIDE2_REDUCED_TEST_ACTIVE STATNAME(PolyphaseMonoFastLowrateStride2Reduced_TEST_ACTIVE)
#define AMIGA_POLYPHASE_MONO_FAST_STRIDE2_REDUCED_HAS_ASM STATNAME(PolyphaseMonoFastLowrateStride2Reduced_HAS_AMIGA_M68K_ASM_RUNTIME)
int STATNAME(PolyphaseStereoFastLowrateStride2Reduced_HAS_AMIGA_M68K_ASM_RUNTIME)(void);
#define AMIGA_POLYPHASE_STEREO_FAST_STRIDE2_REDUCED_HAS_ASM STATNAME(PolyphaseStereoFastLowrateStride2Reduced_HAS_AMIGA_M68K_ASM_RUNTIME)
#define AMIGA_POLYPHASE_STEREO_FAST_STRIDE2_HAS_ASM STATNAME(StereoFastPolyphaseStride2_HAS_AMIGA_M68K_ASM_RUNTIME)
#define AMIGA_POLYPHASE_STEREO_FAST_STRIDE4_HAS_ASM STATNAME(StereoFastPolyphaseStride4_HAS_AMIGA_M68K_ASM_RUNTIME)
#define AMIGA_POLYPHASE_STEREO_FAST_STRIDE4_C_REFERENCE STATNAME(PolyphaseStereoFastLowrateStride4_C_REFERENCE)
#define AMIGA_POLYPHASE_STEREO_FAST_STRIDE4_TEST_ACTIVE STATNAME(PolyphaseStereoFastLowrateStride4_TEST_ACTIVE)
#define AMIGA_POLYPHASE_STEREO_FAST_STRIDE5_C_REFERENCE STATNAME(PolyphaseStereoFastLowrateStride5_C_REFERENCE)
#define AMIGA_POLYPHASE_STEREO_FAST_STRIDE5_TEST_ACTIVE STATNAME(PolyphaseStereoFastLowrateStride5_TEST_ACTIVE)
#define AMIGA_POLYPHASE_STEREO_FAST_STRIDE5_IS_ACTIVE STATNAME(StereoFastPolyphaseStride5_Amiga_m68k_IsActive)
#define AMIGA_POLYPHASE_MONO_FAST_STRIDE4_REDUCED_TEST_ACTIVE STATNAME(PolyphaseMonoFastLowrateStride4Reduced_TEST_ACTIVE)
#define AMIGA_POLYPHASE_STEREO_FAST_STRIDE4_REDUCED_TEST_ACTIVE STATNAME(PolyphaseStereoFastLowrateStride4Reduced_TEST_ACTIVE)
#define AMIGA_POLYPHASE_STEREO_FAST_STRIDE4_REDUCED_C_REFERENCE STATNAME(PolyphaseStereoFastLowrateStride4Reduced_C_REFERENCE)
#define AMIGA_POLYPHASE_STEREO_FAST_STRIDE4_REDUCED_HAS_ASM STATNAME(PolyphaseStereoFastLowrateStride4Reduced_HAS_AMIGA_M68K_ASM_RUNTIME)
#define AMIGA_M68K_POLYPHASE_MONO_FAST_IS_ACTIVE STATNAME(AmigaM68KPolyphaseMonoFast_IsActive)
#define AMIGA_M68K_POLYPHASE_MONO_FAST_STRIDE2_IS_ACTIVE STATNAME(AmigaM68KPolyphaseMonoFastStride2_IsActive)
#define AMIGA_HUFFMAN_PAIRS_C_REFERENCE STATNAME(DecodeHuffmanPairs_C_REFERENCE)
#define AMIGA_HUFFMAN_PAIRS_TEST_ACTIVE STATNAME(DecodeHuffmanPairs_TEST_ACTIVE)
#define AMIGA_HUFFMAN_PAIRS_HAS_ASM STATNAME(DecodeHuffmanPairs_HAS_AMIGA_M68K_ASM_RUNTIME)
#define AMIGA_HUFFMAN_PAIRS_ASM_NOTE STATNAME(DecodeHuffmanPairs_AMIGA_M68K_ASM_NOTE)
#define AMIGA_DEQUANT_BLOCK_C_REFERENCE STATNAME(DequantBlock_C_REFERENCE)
#define AMIGA_DEQUANT_BLOCK_TEST_ACTIVE STATNAME(DequantBlock_TEST_ACTIVE)
#define AMIGA_DEQUANT_BLOCK_HAS_ASM STATNAME(DequantBlock_HAS_AMIGA_M68K_ASM_RUNTIME)
#define AMIGA_INTENSITY_SCALE_RUN_C_REFERENCE STATNAME(IntensityScaleRun_C_REFERENCE)
#define AMIGA_INTENSITY_SCALE_RUN1_TEST_ACTIVE STATNAME(IntensityScaleRun1_TEST_ACTIVE)
#define AMIGA_INTENSITY_SCALE_RUN3_TEST_ACTIVE STATNAME(IntensityScaleRun3_TEST_ACTIVE)
#define AMIGA_INTENSITY_SCALE_RUN_HAS_ASM STATNAME(IntensityScaleRun_HAS_AMIGA_M68K_ASM_RUNTIME)
#define AMIGA_BITSTREAM_REFILL_SELFTEST STATNAME(BitstreamRefillSelftest)
#define AMIGA_POLY_COEF STATNAME(polyCoef)

#define READBUF_SIZE (1024 * 16)
#define OUTBUF_SAMPS (MAX_NCHAN * MAX_NGRAN * MAX_NSAMP)

#ifdef HAVE_AMIGA_AUDIO_DEVICE
#include "decoders/decoder_module.h"
#endif

/* Fake-stereo (pseudo-stereo) widener parameters; see FakeStereo below. */
#define FAKE_STEREO_MAX_DELAY 256
#define FAKE_STEREO_DELAY_MASK (FAKE_STEREO_MAX_DELAY - 1)
#define FAKE_STEREO_DEFAULT_DELAY 96
#define FAKE_STEREO_DEFAULT_SHIFT 2
#define AMIGA_IMDCT_BLOCK_SIZE 18
#define AMIGA_IMDCT_NBANDS 32
#define AMIGA_POLYPHASE_NBANDS 32
#define AMIGA_POLYPHASE_VBUF_LENGTH (17 * 2 * AMIGA_POLYPHASE_NBANDS)
#define AMIGA_AUDIO_MAX_CHANNEL_BYTES 65534UL

#define OUT_PCM16 0
#define OUT_S8    1
#define OUT_8SVX  2

#define SVX_COMP_NONE 0
#define SVX_COMP_FIBDELTA 1

typedef struct DecodeOptions {
	const char *inName;
	const char *outName;
	int outFormat;
	int mono;
	int compression;
	int bench;
	int decodeOnly;
	int noOutput;
	int selftestMulshift;
	int selftestClz;
	int selftestFdct32;
	int selftestFdct32Half;
	int selftestFdct32HalfDebug;
	int selftestVerbose;
	int selftestImdct;
	int selftestImdctThin;
	int selftestSubbandCap;
	int selftestAntialiasSubbandCap;
	int selftestImdct36AsmGeneralPath;
	int selftestDequantSubbandCap;
	int selftestCollapseStereoToMono;
	int selftestMidSideSubbandCap;
	int selftestCollapseStereoToMonoSubbandCap;
	int selftestIntensitySubbandCap;
	int selftestFdct32HalfSparse16;
	int selftestAntialias;
	int selftestPolyphase;
	int selftestPolyphaseStride2;
	int selftestPolyphaseStride2Reduced;
	int selftestPolyphaseStride4;
	int selftestPolyphaseStride4Stereo;
	int selftestPolyphaseStride4StereoReduced;
	int selftestPlanarS8TrueStereo;
	int selftestPolyphaseStride2Stereo;
	int selftestPolyphaseStride2StereoReduced;
	int selftestPolyphaseStride5Stereo;
	int selftestPolyphaseStride3;
	int selftestPolyphaseStride3Stereo;
	int selftestPolyphaseStride5;
	int selftestPolyphaseStride4AllPhases;
	int forceCPolyphaseStride2Stereo;
	int selftestFastLowrate;
	int selftestReducedTaps;
	int selftestFdct32Quarter;
	int selftestFdct32QuarterStereo;
	int selftestHuffman;
	int selftestDequant;
	int selftestIntensity;
	int selftestBitstream;
	int selftestMonoFastLowrateStereo;
	int selftestQuality;
	int selftestFakeStereo;
	int checksum;
	int outputRate;
	int fastLowrate;
	int superfastLowrate;
	int quality;
	int qualitySpecified;
	int expPoly;
	int expHuff;
	int expImdctThin;
	int expReducedTaps;
	int subbandCap;
	int expFdct32Quarter;
	int help;
	int debugArgv;
	int debugFastLowrate;
	int debugSubbandPrecondition;
	int debugPlay;
	int debugTone;
	int debugCleanup;
	int debugDecoder;
	int testAac;
	int play;
	int stereo;
	int fakeStereo;
	int fakeStereoDelay;
	int fakeStereoShift;
	int decodeThenPlay;
	int playLifecycleTest;
	int audioOpenSilentTest;
	int startupVolumeSelftest;
	int bufferSeconds;
	int volumePercent;
	int fastMem;
	int info;
	int noMonoMSSideSkip;
	int radioStream;
	int haveRadioHostAddr;
	const char *radioCodecHint;
	unsigned long radioHostAddrBe;
} DecodeOptions;

typedef struct Mp3InputInfo {
	int id3v2Detected;
	int id3v2Major;
	int id3v2Revision;
	int id3v2Flags;
	unsigned long id3v2SkipBytes;
	int firstFrameFound;
	unsigned long firstFrameOffset;
	MP3FrameInfo firstFrameInfo;
} Mp3InputInfo;

typedef struct InputSource {
	FILE *file;
#ifdef HAVE_AMIGA_AUDIO_DEVICE
	BPTR amigaFile;
	int useAmigaDos;
#endif
	unsigned char *memory;
	unsigned long memorySize;
	unsigned long memoryPos;
	unsigned char prefix[4096];
	unsigned long prefixSize;
	unsigned long prefixPos;
	Mp3InputInfo info;
	RadioStream *radio;
	/* Set while an Ogg FLAC radio stream is read as native FLAC; the struct
	 * and its RADIO_OGGFLAC_PAGE_MAX page buffer share one allocation. */
	RadioOggFlac *oggFlac;
} InputSource;

static void InputSourceInit(InputSource *input, FILE *file);
#ifdef HAVE_AMIGA_AUDIO_DEVICE
static void InputSourceInitAmigaDos(InputSource *input, BPTR amigaFile);
#endif
static int InputSourcePrepareMp3(InputSource *input);
static void GuiPublishRadioMetadata(RadioStream *radio);
static void GuiMarkRadioStopped(void);

typedef struct DecodeStats {
	unsigned long decodedFrames;
	unsigned long outputSamples;
	unsigned long pcmChecksum;
	int sampleRate;
	int outputSampleRate;
	int channels;
	int outputChannels;
	int bitrate;
	unsigned long underruns;
	unsigned long underrunBuffers[3];
	unsigned long lateBuffers;
	long minimumSpareMilliseconds;
	int spareTimeMeasured;
} DecodeStats;

typedef struct TimingStats {
	clock_t frameDecode;
	clock_t pcmConvert;
	clock_t svxWrite;
	clock_t fibCompress;
	clock_t fileWrite;
} TimingStats;

typedef struct RateState {
	int inRate;
	int outRate;
	int channels;
	unsigned long phase;
} RateState;

typedef struct SvxWriter {
	FILE *fp;
	long formSizePos;
	long oneShotPos;
	long bodySizePos;
	unsigned long sourceSamples;
	unsigned long bodyBytes;
	int compression;
	int noOutput;
	int fibStarted;
	signed char fibPrev;
	int fibHaveHighNibble;
	unsigned char fibPending;
} SvxWriter;

#ifdef AMIGA_M68K
typedef struct NormalizedArgs {
	int argc;
	char **argv;
	char *storage;
} NormalizedArgs;

static const char *AmigaBaseName(const char *path)
{
	const char *base;

	base = path;
	while (*path) {
		if (*path == '/' || *path == ':' || *path == '\\')
			base = path + 1;
		path++;
	}

	return base;
}

static int AmigaAsciiLower(int c)
{
	if (c >= 'A' && c <= 'Z')
		return c - 'A' + 'a';

	return c;
}

static int AmigaArgIsProgramName(const char *arg)
{
	const char *base;
	const char *prefix;

	if (!arg || !arg[0])
		return 0;

	base = AmigaBaseName(arg);
	prefix = "amiga_mp3dec";
	while (*prefix) {
		if (AmigaAsciiLower((unsigned char)*base) != *prefix)
			return 0;
		base++;
		prefix++;
	}

	return *base == '\0' || *base == '.';
}


static int AmigaIsArgSpace(char c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static int AmigaTailHasSplittableSpace(const char *arg)
{
	if (!arg)
		return 0;
	while (*arg) {
		if (AmigaIsArgSpace(*arg))
			return 1;
		arg++;
	}
	return 0;
}

static int AmigaCountTailTokens(const char *src)
{
	int tokens = 0;
	char quote = '\0';
	int inToken = 0;

	while (*src) {
		if (quote) {
			if (*src == quote)
				quote = '\0';
			inToken = 1;
		} else if (*src == '"' || *src == '\'') {
			quote = *src;
			if (!inToken) {
				tokens++;
				inToken = 1;
			}
		} else if (AmigaIsArgSpace(*src)) {
			inToken = 0;
		} else if (!inToken) {
			tokens++;
			inToken = 1;
		}
		src++;
	}

	return tokens;
}

static void AmigaSplitTail(char *src, char **argv, int *argc)
{
	char *read;
	char *write;
	char *token;
	char quote;

	read = src;
	while (*read) {
		while (AmigaIsArgSpace(*read))
			read++;
		if (!*read)
			break;

		token = read;
		write = read;
		quote = '\0';
		while (*read) {
			if (quote) {
				if (*read == quote) {
					quote = '\0';
					read++;
					continue;
				}
			} else if (*read == '"' || *read == '\'') {
				quote = *read++;
				continue;
			} else if (AmigaIsArgSpace(*read)) {
				break;
			}
			*write++ = *read++;
		}
		if (*read)
			read++;
		*write = '\0';
		argv[(*argc)++] = token;
		while (AmigaIsArgSpace(*read))
			read++;
	}
}

static int AmigaArgStringNeedsSplit(int argc, char **argv)
{
	if (!argv)
		return 0;

	if (argc == 1 && argv[0])
		return !AmigaArgIsProgramName(argv[0]) ||
			AmigaTailHasSplittableSpace(argv[0]);

	if (argc == 2 && argv[0] && argv[1] && AmigaArgIsProgramName(argv[0]))
		return AmigaTailHasSplittableSpace(argv[1]);

	return 0;
}


static int AmigaNormalizeArgs(int argc, char **argv, NormalizedArgs *normalized)
{
	const char *src;
	int tokens;
	int outArgc;

	normalized->argc = argc;
	normalized->argv = argv;
	normalized->storage = NULL;

	if (!AmigaArgStringNeedsSplit(argc, argv))
		return 0;

	if (argc == 2 && argv[0] && AmigaArgIsProgramName(argv[0]))
		src = argv[1];
	else
		src = argv[0];

	tokens = AmigaCountTailTokens(src);
	normalized->argv = (char **)malloc((tokens + 2) * sizeof(char *));
	if (!normalized->argv)
		return -1;

	normalized->storage = (char *)malloc(strlen(src) + 1);
	if (!normalized->storage) {
		free(normalized->argv);
		normalized->argv = argv;
		return -1;
	}

	strcpy(normalized->storage, src);
	normalized->argv[0] = (char *)"amiga_mp3dec";
	outArgc = 1;
	AmigaSplitTail(normalized->storage, normalized->argv, &outArgc);
	if (outArgc > 1 && AmigaArgIsProgramName(normalized->argv[1])) {
		int i;
		for (i = 1; i < outArgc; i++)
			normalized->argv[i] = normalized->argv[i + 1];
		outArgc--;
	}
	normalized->argv[outArgc] = NULL;
	normalized->argc = outArgc;

	return 0;
}


static void AmigaFreeNormalizedArgs(NormalizedArgs *normalized)
{
	if (normalized->storage) {
		free(normalized->storage);
		free(normalized->argv);
	}
	normalized->storage = NULL;
	normalized->argv = NULL;
	normalized->argc = 0;
}
#else
typedef struct NormalizedArgs {
	int argc;
	char **argv;
} NormalizedArgs;

static int AmigaNormalizeArgs(int argc, char **argv, NormalizedArgs *normalized)
{
	normalized->argc = argc;
	normalized->argv = argv;
	return 0;
}

static void AmigaFreeNormalizedArgs(NormalizedArgs *normalized)
{
	(void)normalized;
}
#endif

static void PrintArgvDebug(int argc, char **argv)
{
	int i;

	printf("argc: %d\n", argc);
	for (i = 0; i < argc; i++)
		printf("argv[%d]: %s\n", i, argv[i] ? argv[i] : "(null)");
}

static void PrintUsage(const char *prog)
{
	printf("usage: %s [options] infile.mp3 outfile\n", prog);
	printf("options:\n");
	printf("  --mono       mix stereo to mono before writing\n");
	printf("  --s8         write raw signed 8-bit PCM instead of signed 16-bit PCM\n");
	printf("  --8svx       write Amiga IFF-8SVX signed 8-bit output (implies mono)\n");
	printf("  --fibdelta   use 8SVX Fibonacci Delta compression (implies --8svx)\n");
	printf("  --bench      print elapsed decode/write time and realtime ratio\n");
	printf("  --info       print MP3/ID3 metadata; alone, inspect without decoding\n");
	printf("  --play       AmigaOS experimental audio.device Paula playback (mono s8)\n");
	printf("  --stereo     opt-in stereo output for --play or --decode-only benchmarking\n");
	printf("               stereo rates: 8820, 11025, 14700, 22050, or PAL-top 28600 Hz\n");
	printf("               mono rates: 8287 default, 8820, 11025, 14700, 22050, or PAL-top 28600 Hz\n");
	printf("  --fake-stereo  --play pseudo-stereo width from the mono decode (mono CPU cost)\n");
	printf("               energy-symmetric cross-delay; mutually exclusive with --stereo\n");
	printf("  --fake-stereo-delay N  fake-stereo delay in samples (1-%d, default %d)\n",
		FAKE_STEREO_MAX_DELAY, FAKE_STEREO_DEFAULT_DELAY);
	printf("  --fake-stereo-shift K  fake-stereo cross-bleed >>K (0-8, default %d; higher=wider, 0=mono)\n",
		FAKE_STEREO_DEFAULT_SHIFT);
	printf("  --play-fast-path accepted alias; --play already uses reduced-overhead playback\n");
	printf("  --decode-then-play decode whole MP3 to RAM, then play (debug for --play)\n");
	printf("  --selftest-play-cleanup open/submit/cleanup audio.device five times\n");
	printf("  --selftest-startup-volume verify startup CMD_WRITE volume setup\n");
	printf("  --play-lifecycle-test legacy alias for --selftest-play-cleanup\n");
	printf("  --buffer-seconds N playback seconds per half-buffer (default 4, clamped 1-10)\n");
	printf("  --volume N   audio.device master volume percent for --play (0-100, default 100)\n");
	printf("  --fast-mem   preload the compressed MP3 into Fast RAM before decoding/playback\n");
	printf("  --decode-only decode frames only; skip PCM conversion and output\n");
	printf("  --no-output  run conversion/compression paths but discard output bytes\n");
	printf("  --rate HZ    output/downsample rate: 28600, 22050, 14700, 11025, 8820, or 8287 Hz\n");
	printf("               28600/22050 playback is experimental/high CPU and may underrun\n");
	printf("  --fast-lowrate lower-quality Amiga conversion; requires --rate\n");
	printf("  --superfast-lowrate sparse low-rate mode; use --rate 8287, 8820, 11025, 14700, or 22050\n");
	printf("                 defaults to 11025 if no --rate is specified\n");
	printf("  --ultrafast  cap IMDCT to 26 subbands (~18 kHz) at full 44.1 kHz rate;\n");
	printf("                 saves ~18%% IMDCT work with negligible audible impact\n");
	printf("  --subband-cap N limit IMDCT to N active subbands 1-32 (use after --rate/--fast-lowrate)\n");
	printf("  --quality N set quality/speed level (0 fastest, 1 fast, 2 balanced, 3 accurate)\n");
	printf("               default: 1 for --fast-lowrate --rate 11025, 14700, or 22050, otherwise 3\n");
	printf("               0 enables Superfast FDCT32 quarter + Huffman asm; 1 adds reduced taps; 3 is original behavior\n");
	printf("               individual --exp-* flags may still be enabled independently\n");
	printf("  --exp-poly  use experimental 68030 asm mono polyphase when compiled in\n");
	printf("  --exp-huff  use experimental 68030 inline-asm Huffman pair refill when compiled in\n");
	printf("  --exp-imdct-thin request experimental fast-lowrate IMDCT output thinning\n");
	printf("  --exp-reduced-taps use experimental reduced-tap fast-lowrate dewindow\n");
	printf("  --exp-fdct32-quarter use experimental stride-4 quarter-rate FDCT32 approximation\n");
	printf("  --selftest-mulshift compare C and optional asm MULSHIFT32 helpers\n");
	printf("  --selftest-clz compare C and optional m68k bfffo CLZ helpers\n");
	printf("  --selftest-fdct32 compare C reference and optional m68k asm FDCT32 path\n");
	printf("  --selftest-fdct32half compare FDCT32Half even-row stores against full FDCT32\n");
	printf("  --selftest-fdct32half-debug print first FDCT32Half mismatch dependencies\n");
	printf("  --selftest-verbose print every selftest mismatch instead of the first only\n");
	printf("  --selftest-imdct compare C reference and optional m68k asm long IMDCT path\n");
	printf("  --selftest-imdct-thin verify exact selected IMDCT bands and deterministic sparse output\n");
	printf("  --selftest-subband-cap verify low-rate mono IMDCT subband cap behavior\n");
	printf("  --selftest-antialias-subband-cap verify capping antialias butterflies to the subband cap leaves kept bands bit-exact\n");
	printf("  --selftest-imdct36-asm-general-path verify m68k asm IMDCT36 matches C reference for transition/mixed-window (btCurr/btPrev != 0) blocks, not just the fast long-window path\n");
	printf("  --selftest-dequant-subband-cap verify capping dequant long-block work to the subband cap leaves kept samples, cbEndL, and guard-bit count bit-exact\n");
	printf("  --selftest-collapse-stereo-mono verify the stereo-to-mono collapse (plain LR stereo source, mono output) asm matches the C reference bit-exact\n");
	printf("  --selftest-midside-subband-cap verify capping MidSideProc's scan to the fast-lowrate subband cap is safe and equivalent to a zeroed discarded region\n");
	printf("  --selftest-collapse-stereo-mono-subband-cap verify capping stereo-to-mono collapse to the dequant subband cap matches an uncapped collapse with the discarded region zeroed, including gb[0] and nonZeroBound[0]\n");
	printf("  --selftest-intensity-subband-cap verify capping IntensityProcMPEG1/2's scan to the fast-lowrate subband cap matches an uncapped call with the discarded region zeroed, for MPEG1/MPEG2 and long/short/mixed blocks\n");
	printf("  --selftest-fdct32half-sparse16 verify prototype sparse-input FDCT32Half (NOT wired into playback) matches the reference when subbands 16-31 are zero\n");
	printf("  --selftest-antialias compare C reference and optional m68k asm antialias path\n");
	printf("  --selftest-polyphase compare C fast mono polyphase and optional m68k asm path\n");
	printf("  --selftest-polyphase-stride2 compare C and optional asm stride-2 mono polyphase paths\n");
	printf("  --selftest-polyphase-stride2-reduced compare C and optional asm reduced stride-2 mono polyphase paths\n");
	printf("  --selftest-polyphase-stride4 compare C and optional asm stride-4 mono polyphase paths\n");
	printf("  --selftest-polyphase-stride4-stereo compare stereo stride-4 compact polyphase output\n");
	printf("  --selftest-polyphase-stride4-stereo-reduced compare C and optional asm reduced stride-4 stereo polyphase paths\n");
	printf("  --selftest-planar-s8-stereo compare C and optional asm true-stereo interleaved s16 -> planar s8 conversion\n");
	printf("  --selftest-polyphase-stride2-stereo compare stereo stride-2 compact polyphase output\n");
	printf("  --selftest-polyphase-stride2-stereo-reduced compare stereo reduced stride-2 compact polyphase output\n");
	printf("  --selftest-polyphase-stride5-stereo compare stereo stride-5 compact polyphase output\n");
	printf("  --selftest-polyphase-stride3 verify stride-3 (14700 Hz) mono hand-unrolled polyphase output against full-band synthesis\n");
	printf("  --selftest-polyphase-stride3-stereo verify stride-3 (14700 Hz) stereo hand-unrolled polyphase output against full-band synthesis\n");
	printf("  --selftest-polyphase-stride5 verify stride-5 (8820/8287 Hz) mono polyphase output against full-band synthesis\n");
	printf("  --selftest-polyphase-stride4-allphases verify stride-4 (11025 Hz) mono polyphase output at all 4 phases against full-band synthesis\n");
	printf("  --force-c-polyphase-stride2-stereo benchmark stereo stride-2 C fallback in this binary\n");
	printf("  --selftest-fastlowrate compare synthetic stride decimation paths\n");
	printf("  --selftest-reduced-taps compare full and reduced stride-4 dewindow paths\n");
	printf("  --selftest-fdct32-quarter inspect lossy stride-4 quarter-rate FDCT32 scatter\n");
	printf("  --selftest-fdct32-quarter-stereo verify independent stereo stride-4 quarter FDCT32 dispatch\n");
	printf("  --selftest-huffman compare C and active Huffman pair decode paths\n");
	printf("  --selftest-dequant compare C and optional m68k asm dequant block paths\n");
	printf("  --selftest-intensity compare C and optional m68k asm intensity scale paths\n");
	printf("  --selftest-bitstream compare C and optional m68k move.l bitstream refill paths\n");
	printf("  --selftest-mono-fastlowrate-stereo verify stereo-to-mono low-rate accounting\n");
	printf("  --selftest-quality verify --quality flag mapping and auto-default selection\n");
	printf("  --selftest-fake-stereo verify pseudo-stereo mono-compatibility and delay line\n");
	printf("  --checksum  print a 32-bit checksum of decoded PCM samples\n");
	printf("  --no-ms-mono-skip force full two-channel M/S decode before mono regression checks\n");
	printf("  --debug-fastlowrate print per-frame/granule fast-lowrate placement\n");
	printf("  --debug-subband-precondition check FDCT32HalfSparse16's buf[16..31]==0 precondition on every real 22050 Hz block and print violation counts at exit\n");
	printf("  --debug-play print audio.device playback startup diagnostics\n");
	printf("  --debug-tone submit a generated signed-8 Paula test tone through --play audio path\n");
	printf("  --debug-cleanup print playback resource cleanup diagnostics\n");
	printf("  --debug-decoder print generic decoder module/rate diagnostics\n");
	printf("  --test-aac FILE smoke-test ADTS AAC module loading and one-frame decode\n");
	printf("  --radio-codec-hint CODEC  Radio Browser codec hint (MP3, AAC, AAC+)\n");
	printf("  --debug-argv print argc/argv after Amiga argument normalization\n");
	printf("  --show-argv  alias for --debug-argv\n");
	printf("\n");
	printf("default output is raw signed 16-bit big-endian PCM.\n");
	printf("outfile ending in :, /, or \\ is treated as a directory/volume.\n");
}

static int ParseBufferSecondsOption(const char *arg, int *outSeconds)
{
	char *end;
	long value;

	if (!arg || !arg[0])
		return -1;
	value = strtol(arg, &end, 10);
	if (end == arg || *end != '\0' || value <= 0)
		return -1;
	if (value > 10)
		value = 10;
	*outSeconds = (int)value;
	return 0;
}

static int ParseVolumeOption(const char *arg, int *outPercent)
{
	char *end;
	long value;

	if (!arg || !arg[0])
		return -1;
	value = strtol(arg, &end, 10);
	if (end == arg || *end != '\0' || value < 0 || value > 100)
		return -1;
	*outPercent = (int)value;
	return 0;
}

#ifndef HAVE_AMIGA_AUDIO_DEVICE
typedef unsigned short UWORD;
#endif
#define AMIGA_AUDIO_DEVICE_MAX_VOLUME 64U

static UWORD VolumePercentToAudioDevice(int percent)
{
	if (percent < 0)
		percent = 0;
	if (percent > 100)
		percent = 100;
	return (UWORD)(((unsigned int)percent * AMIGA_AUDIO_DEVICE_MAX_VOLUME + 50U) / 100U);
}

volatile unsigned short gMiniAmp3RequestedVolume = 100;
volatile unsigned long gMiniAmp3VolumeSequence;

static void ApplyQualityOptions(DecodeOptions *opt)
{
	int quality;

	quality = opt->qualitySpecified ? opt->quality :
		(opt->fastLowrate && (opt->outputRate == 11025 || opt->outputRate == 14700 ||
			opt->outputRate == 22050) ? 1 : 3);
	opt->quality = quality;

	switch (quality) {
	case 0:
		opt->expHuff = 1;
		opt->expFdct32Quarter = 1;
		/* fall through */
	case 1:
		opt->expReducedTaps = 1;
		/* fall through */
	case 2:
		opt->expPoly = 1;
		break;
	case 3:
	default:
		break;
	}
}

static int ParseOptions(int argc, char **argv, DecodeOptions *opt)
{
	int i;

	memset(opt, 0, sizeof(*opt));
	opt->outFormat = OUT_PCM16;
	opt->compression = SVX_COMP_NONE;
	opt->outputRate = 0;
	opt->quality = 3;
	opt->qualitySpecified = 0;
	opt->bufferSeconds = 4;
	opt->volumePercent = 100;
	opt->fakeStereoDelay = FAKE_STEREO_DEFAULT_DELAY;
	opt->fakeStereoShift = FAKE_STEREO_DEFAULT_SHIFT;
#if defined(DEBUG_DECODER) && DEBUG_DECODER
	opt->debugDecoder = 1;
#endif

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--mono")) {
			opt->mono = 1;
		} else if (!strcmp(argv[i], "--s8")) {
			opt->outFormat = OUT_S8;
		} else if (!strcmp(argv[i], "--8svx")) {
			opt->outFormat = OUT_8SVX;
			opt->mono = 1;
		} else if (!strcmp(argv[i], "--fibdelta")) {
			opt->outFormat = OUT_8SVX;
			opt->mono = 1;
			opt->compression = SVX_COMP_FIBDELTA;
		} else if (!strcmp(argv[i], "--bench")) {
			opt->bench = 1;
		} else if (!strcmp(argv[i], "--info")) {
			opt->info = 1;
		} else if (!strcmp(argv[i], "--play")) {
			opt->play = 1;
			opt->outFormat = OUT_S8;
			opt->mono = 1;
		} else if (!strcmp(argv[i], "--stereo")) {
			opt->stereo = 1;
		} else if (!strcmp(argv[i], "--fake-stereo")) {
			opt->fakeStereo = 1;
		} else if (!strcmp(argv[i], "--fake-stereo-delay")) {
			if (i + 1 >= argc) {
				fprintf(stderr, "--fake-stereo-delay requires a value (1-%d)\n",
					FAKE_STEREO_MAX_DELAY);
				return -1;
			}
			i++;
			opt->fakeStereoDelay = atoi(argv[i]);
		} else if (!strcmp(argv[i], "--fake-stereo-shift")) {
			if (i + 1 >= argc) {
				fprintf(stderr, "--fake-stereo-shift requires a value (0-8)\n");
				return -1;
			}
			i++;
			opt->fakeStereoShift = atoi(argv[i]);
		} else if (!strcmp(argv[i], "--play-fast-path")) {
			opt->play = 1;
			opt->outFormat = OUT_S8;
			opt->mono = 1;
		} else if (!strcmp(argv[i], "--radio-stream")) {
#if ENABLE_RADIO
			opt->radioStream = 1;
			opt->play = 1;
			opt->outFormat = OUT_S8;
			opt->mono = 1;
#else
			fprintf(stderr, "--radio-stream requested, but radio support not built; rebuild with RADIO=1 or HAVE_BSDSOCKET=1\n");
			return -1;
#endif
		} else if (!strcmp(argv[i], "--radio-host-addr-be")) {
			if (++i >= argc)
				return -1;
			opt->radioHostAddrBe = strtoul(argv[i], NULL, 0);
			opt->haveRadioHostAddr = 1;
		} else if (!strcmp(argv[i], "--radio-codec-hint")) {
			if (++i >= argc)
				return -1;
			opt->radioCodecHint = argv[i];
		} else if (!strcmp(argv[i], "--decode-then-play")) {
			opt->play = 1;
			opt->decodeThenPlay = 1;
			opt->outFormat = OUT_S8;
			opt->mono = 1;
		} else if (!strcmp(argv[i], "--selftest-audio-open-silent")) {
			opt->audioOpenSilentTest = 1;
			opt->play = 1;
			opt->outFormat = OUT_S8;
			opt->mono = 1;
		} else if (!strcmp(argv[i], "--selftest-startup-volume")) {
			opt->startupVolumeSelftest = 1;
		} else if (!strcmp(argv[i], "--selftest-play-cleanup") ||
			!strcmp(argv[i], "--play-lifecycle-test")) {
			opt->play = 1;
			opt->playLifecycleTest = 1;
			opt->outFormat = OUT_S8;
			opt->mono = 1;
		} else if (!strcmp(argv[i], "--buffer-seconds")) {
			if (++i >= argc)
				return -1;
			if (ParseBufferSecondsOption(argv[i], &opt->bufferSeconds) != 0) {
				fprintf(stderr, "--buffer-seconds requires a positive integer (1-10 seconds)\n");
				return -1;
			}
		} else if (!strcmp(argv[i], "--volume")) {
			if (++i >= argc)
				return -1;
			if (ParseVolumeOption(argv[i], &opt->volumePercent) != 0) {
				fprintf(stderr, "--volume requires an integer from 0 to 100\n");
				return -1;
			}
		} else if (!strcmp(argv[i], "--fast-mem")) {
			opt->fastMem = 1;
		} else if (!strcmp(argv[i], "--decode-only")) {
			opt->decodeOnly = 1;
			opt->noOutput = 1;
		} else if (!strcmp(argv[i], "--no-output")) {
			opt->noOutput = 1;
		} else if (!strcmp(argv[i], "--selftest-mulshift")) {
			opt->selftestMulshift = 1;
		} else if (!strcmp(argv[i], "--selftest-clz")) {
			opt->selftestClz = 1;
		} else if (!strcmp(argv[i], "--selftest-fdct32")) {
			opt->selftestFdct32 = 1;
		} else if (!strcmp(argv[i], "--selftest-fdct32half")) {
			opt->selftestFdct32Half = 1;
		} else if (!strcmp(argv[i], "--selftest-fdct32half-debug")) {
			opt->selftestFdct32Half = 1;
			opt->selftestFdct32HalfDebug = 1;
		} else if (!strcmp(argv[i], "--selftest-verbose")) {
			opt->selftestVerbose = 1;
		} else if (!strcmp(argv[i], "--selftest-imdct")) {
			opt->selftestImdct = 1;
		} else if (!strcmp(argv[i], "--selftest-imdct-thin")) {
			opt->selftestImdctThin = 1;
		} else if (!strcmp(argv[i], "--selftest-subband-cap")) {
			opt->selftestSubbandCap = 1;
		} else if (!strcmp(argv[i], "--selftest-antialias-subband-cap")) {
			opt->selftestAntialiasSubbandCap = 1;
		} else if (!strcmp(argv[i], "--selftest-imdct36-asm-general-path")) {
			opt->selftestImdct36AsmGeneralPath = 1;
		} else if (!strcmp(argv[i], "--selftest-dequant-subband-cap")) {
			opt->selftestDequantSubbandCap = 1;
		} else if (!strcmp(argv[i], "--selftest-collapse-stereo-mono")) {
			opt->selftestCollapseStereoToMono = 1;
		} else if (!strcmp(argv[i], "--selftest-midside-subband-cap")) {
			opt->selftestMidSideSubbandCap = 1;
		} else if (!strcmp(argv[i], "--selftest-collapse-stereo-mono-subband-cap")) {
			opt->selftestCollapseStereoToMonoSubbandCap = 1;
		} else if (!strcmp(argv[i], "--selftest-intensity-subband-cap")) {
			opt->selftestIntensitySubbandCap = 1;
		} else if (!strcmp(argv[i], "--selftest-fdct32half-sparse16")) {
			opt->selftestFdct32HalfSparse16 = 1;
		} else if (!strcmp(argv[i], "--selftest-antialias")) {
			opt->selftestAntialias = 1;
		} else if (!strcmp(argv[i], "--selftest-polyphase")) {
			opt->selftestPolyphase = 1;
		} else if (!strcmp(argv[i], "--selftest-polyphase-stride2")) {
			opt->selftestPolyphaseStride2 = 1;
		} else if (!strcmp(argv[i], "--selftest-polyphase-stride2-reduced")) {
			opt->selftestPolyphaseStride2Reduced = 1;
		} else if (!strcmp(argv[i], "--selftest-polyphase-stride4")) {
			opt->selftestPolyphaseStride4 = 1;
		} else if (!strcmp(argv[i], "--selftest-polyphase-stride4-stereo")) {
			opt->selftestPolyphaseStride4Stereo = 1;
		} else if (!strcmp(argv[i], "--selftest-polyphase-stride4-stereo-reduced")) {
			opt->selftestPolyphaseStride4StereoReduced = 1;
		} else if (!strcmp(argv[i], "--selftest-planar-s8-stereo")) {
			opt->selftestPlanarS8TrueStereo = 1;
		} else if (!strcmp(argv[i], "--selftest-polyphase-stride2-stereo")) {
			opt->selftestPolyphaseStride2Stereo = 1;
		} else if (!strcmp(argv[i], "--selftest-polyphase-stride2-stereo-reduced")) {
			opt->selftestPolyphaseStride2StereoReduced = 1;
		} else if (!strcmp(argv[i], "--selftest-polyphase-stride5-stereo")) {
			opt->selftestPolyphaseStride5Stereo = 1;
		} else if (!strcmp(argv[i], "--selftest-polyphase-stride5")) {
			opt->selftestPolyphaseStride5 = 1;
		} else if (!strcmp(argv[i], "--selftest-polyphase-stride4-allphases")) {
			opt->selftestPolyphaseStride4AllPhases = 1;
		} else if (!strcmp(argv[i], "--selftest-polyphase-stride3")) {
			opt->selftestPolyphaseStride3 = 1;
		} else if (!strcmp(argv[i], "--selftest-polyphase-stride3-stereo")) {
			opt->selftestPolyphaseStride3Stereo = 1;
		} else if (!strcmp(argv[i], "--force-c-polyphase-stride2-stereo")) {
			opt->forceCPolyphaseStride2Stereo = 1;
		} else if (!strcmp(argv[i], "--selftest-fastlowrate")) {
			opt->selftestFastLowrate = 1;
		} else if (!strcmp(argv[i], "--selftest-reduced-taps")) {
			opt->selftestReducedTaps = 1;
		} else if (!strcmp(argv[i], "--selftest-fdct32-quarter")) {
			opt->selftestFdct32Quarter = 1;
		} else if (!strcmp(argv[i], "--selftest-fdct32-quarter-stereo")) {
			opt->selftestFdct32QuarterStereo = 1;
		} else if (!strcmp(argv[i], "--selftest-huffman")) {
			opt->selftestHuffman = 1;
		} else if (!strcmp(argv[i], "--selftest-dequant")) {
			opt->selftestDequant = 1;
		} else if (!strcmp(argv[i], "--selftest-intensity") ||
			!strcmp(argv[i], "--Selftest-Intensity")) {
			opt->selftestIntensity = 1;
		} else if (!strcmp(argv[i], "--selftest-bitstream")) {
			opt->selftestBitstream = 1;
		} else if (!strcmp(argv[i], "--selftest-mono-fastlowrate-stereo")) {
			opt->selftestMonoFastLowrateStereo = 1;
		} else if (!strcmp(argv[i], "--selftest-quality")) {
			opt->selftestQuality = 1;
		} else if (!strcmp(argv[i], "--selftest-fake-stereo")) {
			opt->selftestFakeStereo = 1;
		} else if (!strcmp(argv[i], "--checksum")) {
			opt->checksum = 1;
		} else if (!strcmp(argv[i], "--fast-lowrate")) {
			opt->fastLowrate = 1;
		} else if (!strcmp(argv[i], "--superfast-lowrate")) {
			opt->fastLowrate = 1;
			opt->superfastLowrate = 1;
		} else if (!strcmp(argv[i], "--exp-poly")) {
			opt->expPoly = 1;
		} else if (!strcmp(argv[i], "--exp-huff")) {
			opt->expHuff = 1;
		} else if (!strcmp(argv[i], "--exp-imdct-thin")) {
			opt->expImdctThin = 1;
		} else if (!strcmp(argv[i], "--no-ms-mono-skip")) {
			opt->noMonoMSSideSkip = 1;
		} else if (!strcmp(argv[i], "--exp-reduced-taps")) {
			opt->expReducedTaps = 1;
		} else if (!strcmp(argv[i], "--ultrafast")) {
			opt->subbandCap = 26;
		} else if (!strcmp(argv[i], "--subband-cap")) {
			if (++i >= argc)
				return -1;
			opt->subbandCap = atoi(argv[i]);
			if (opt->subbandCap < 1 || opt->subbandCap > 32) {
				fprintf(stderr, "error: --subband-cap N must be 1-32\n");
				return -1;
			}
		} else if (!strcmp(argv[i], "--exp-fdct32-quarter")) {
			opt->expFdct32Quarter = 1;
		} else if (!strcmp(argv[i], "--quality")) {
			if (++i >= argc)
				return -1;
			if (argv[i][0] < '0' || argv[i][0] > '3' || argv[i][1] != '\0') {
				fprintf(stderr, "--quality requires 0, 1, 2, or 3\n");
				return -1;
			}
			opt->quality = argv[i][0] - '0';
			opt->qualitySpecified = 1;
		} else if (!strcmp(argv[i], "--rate")) {
			if (++i >= argc)
				return -1;
			opt->outputRate = atoi(argv[i]);
			if (opt->outputRate != 28600 && opt->outputRate != 22050 &&
				opt->outputRate != 14700 && opt->outputRate != 11025 &&
				opt->outputRate != 8820 && opt->outputRate != 8287)
				return -1;
		} else if (!strcmp(argv[i], "--debug-fastlowrate")) {
			opt->debugFastLowrate = 1;
		} else if (!strcmp(argv[i], "--debug-subband-precondition")) {
			opt->debugSubbandPrecondition = 1;
		} else if (!strcmp(argv[i], "--debug-play")) {
			opt->debugPlay = 1;
		} else if (!strcmp(argv[i], "--debug-tone")) {
			opt->debugTone = 1;
			opt->debugPlay = 1;
			opt->play = 1;
			opt->outFormat = OUT_S8;
		} else if (!strcmp(argv[i], "--debug-cleanup")) {
			opt->debugCleanup = 1;
		} else if (!strcmp(argv[i], "--debug-decoder")) {
			opt->debugDecoder = 1;
		} else if (!strcmp(argv[i], "--test-aac")) {
			opt->testAac = 1;
			opt->debugDecoder = 1;
		} else if (!strcmp(argv[i], "--debug-argv") ||
			!strcmp(argv[i], "--show-argv")) {
			opt->debugArgv = 1;
		} else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			opt->help = 1;
			return 0;
		} else if (argv[i][0] == '-') {
			fprintf(stderr, "unknown option: %s\n", argv[i]);
			return -1;
		} else if (!opt->inName) {
			opt->inName = argv[i];
		} else if (!opt->outName) {
			opt->outName = argv[i];
		} else {
			return -1;
		}
	}

	if (opt->help)
		return 0;

if (opt->selftestMulshift ||
    opt->selftestClz ||
    opt->selftestFdct32 ||
    opt->selftestFdct32Half ||
    opt->selftestImdct ||
    opt->selftestImdctThin ||
    opt->selftestSubbandCap ||
    opt->selftestAntialiasSubbandCap ||
    opt->selftestImdct36AsmGeneralPath ||
    opt->selftestDequantSubbandCap ||
    opt->selftestCollapseStereoToMono ||
    opt->selftestMidSideSubbandCap ||
    opt->selftestCollapseStereoToMonoSubbandCap ||
    opt->selftestIntensitySubbandCap ||
    opt->selftestFdct32HalfSparse16 ||
    opt->selftestAntialias ||
    opt->selftestPolyphase ||
    opt->selftestPolyphaseStride2 ||
    opt->selftestPolyphaseStride2Reduced ||
    opt->selftestPolyphaseStride4 ||
    opt->selftestPolyphaseStride4Stereo ||
    opt->selftestPolyphaseStride4StereoReduced ||
    opt->selftestPlanarS8TrueStereo ||
    opt->selftestPolyphaseStride2Stereo ||
    opt->selftestPolyphaseStride2StereoReduced ||
    opt->selftestPolyphaseStride5Stereo ||
    opt->selftestPolyphaseStride3 ||
    opt->selftestPolyphaseStride3Stereo ||
    opt->selftestPolyphaseStride5 ||
    opt->selftestPolyphaseStride4AllPhases ||
    opt->selftestFastLowrate ||
    opt->selftestReducedTaps ||
    opt->selftestFdct32Quarter ||
    opt->selftestFdct32QuarterStereo ||
    opt->selftestHuffman ||
    opt->selftestDequant ||
    opt->selftestBitstream ||
    opt->selftestMonoFastLowrateStereo ||
    opt->selftestQuality ||
    opt->selftestFakeStereo)
		return 0;

	if (opt->stereo && !opt->play && !opt->decodeOnly) {
		fprintf(stderr, "--stereo is only supported with --play or --decode-only\n");
		return -1;
	}
	if (opt->fakeStereo && !opt->play) {
		fprintf(stderr, "--fake-stereo is only supported with --play\n");
		return -1;
	}
	if (opt->fakeStereo && opt->stereo) {
		fprintf(stderr, "--fake-stereo and --stereo are mutually exclusive\n");
		return -1;
	}
	if (opt->fakeStereo &&
		(opt->fakeStereoDelay < 1 || opt->fakeStereoDelay > FAKE_STEREO_MAX_DELAY)) {
		fprintf(stderr, "--fake-stereo-delay must be 1-%d samples\n",
			FAKE_STEREO_MAX_DELAY);
		return -1;
	}
	if (opt->fakeStereo &&
		(opt->fakeStereoShift < 0 || opt->fakeStereoShift > 8)) {
		fprintf(stderr, "--fake-stereo-shift must be 0-8\n");
		return -1;
	}

	if (opt->superfastLowrate && !opt->outputRate)
		opt->outputRate = 11025;

	if (opt->play && !opt->outputRate)
		opt->outputRate = opt->stereo ? 8820 : 8287;

	if (opt->play && opt->outputRate != 8287 && opt->outputRate != 8820 &&
		opt->outputRate != 11025 && opt->outputRate != 14700 &&
		opt->outputRate != 22050 && opt->outputRate != 28600) {
		fprintf(stderr, "--play supports --rate 8287, 8820, 11025, 14700, 22050, or 28600 only\n");
		return -1;
	}
	if (opt->play && opt->stereo && opt->outputRate == 8287) {
		fprintf(stderr, "--stereo playback supports --rate 8820, 11025, 14700, 22050, or PAL-top 28600 only\n");
		return -1;
	}
	if (opt->play) {
		opt->mono = opt->stereo ? 0 : 1;
		opt->outFormat = OUT_S8;
		if (opt->outputRate != 28600)
			opt->fastLowrate = 1;
		opt->noOutput = 1;
	}

	if (opt->superfastLowrate && opt->outputRate != 11025 && opt->outputRate != 14700 &&
		opt->outputRate != 22050 && opt->outputRate != 8820 && opt->outputRate != 8287) {
		fprintf(stderr, "--superfast-lowrate supports only --rate 8287, 8820, 11025, 14700, or 22050\n");
		return -1;
	}
	if (opt->fastLowrate && (opt->outputRate != 22050 && opt->outputRate != 14700 &&
		opt->outputRate != 11025 && opt->outputRate != 8820 &&
		opt->outputRate != 8287)) {
		fprintf(stderr, "--fast-lowrate requires --rate 22050, 14700, 11025, 8820, or 8287\n");
		return -1;
	}

	ApplyQualityOptions(opt);

	if (opt->playLifecycleTest || opt->audioOpenSilentTest)
		return 0;

	if (!opt->inName || (!opt->outName && !opt->noOutput && !opt->play && !opt->info))
		return -1;

	return 0;
}

static unsigned long SynchsafeSize(const unsigned char *p)
{
	return ((unsigned long)(p[0] & 0x7f) << 21) |
		((unsigned long)(p[1] & 0x7f) << 14) |
		((unsigned long)(p[2] & 0x7f) << 7) |
		(unsigned long)(p[3] & 0x7f);
}

static unsigned long BigEndianSize(const unsigned char *p, int bytes)
{
	unsigned long value;
	int i;

	value = 0;
	for (i = 0; i < bytes; i++)
		value = (value << 8) | p[i];
	return value;
}

static void PrintTagText(const char *label, const unsigned char *data,
	unsigned long bytes)
{
	unsigned long i;
	int encoding;
	int bigEndian;
	int printed;

	if (!bytes)
		return;
	encoding = data[0];
	data++;
	bytes--;
	printf("%s: ", label);
	printed = 0;
	if (encoding == 1 || encoding == 2) {
		bigEndian = encoding == 2;
		if (bytes >= 2 && data[0] == 0xfe && data[1] == 0xff) {
			bigEndian = 1;
			data += 2;
			bytes -= 2;
		} else if (bytes >= 2 && data[0] == 0xff && data[1] == 0xfe) {
			bigEndian = 0;
			data += 2;
			bytes -= 2;
		}
		for (i = 0; i + 1 < bytes; i += 2) {
			unsigned int ch;
			ch = bigEndian ? ((unsigned int)data[i] << 8) | data[i + 1] :
				((unsigned int)data[i + 1] << 8) | data[i];
			if (!ch)
				break;
			putchar(ch >= 32 && ch <= 126 ? (int)ch : '?');
			printed = 1;
		}
	} else {
		for (i = 0; i < bytes && data[i]; i++) {
			unsigned char ch;
			ch = data[i];
			putchar((ch >= 32 && ch != 127) ? ch : ' ');
			printed = 1;
		}
	}
	if (!printed)
		printf("(empty)");
	putchar('\n');
}

static const char *TagFrameLabel(const char *id)
{
	if (!strcmp(id, "TIT2") || !strcmp(id, "TT2")) return "title";
	if (!strcmp(id, "TPE1") || !strcmp(id, "TP1")) return "artist";
	if (!strcmp(id, "TALB") || !strcmp(id, "TAL")) return "album";
	if (!strcmp(id, "TRCK") || !strcmp(id, "TRK")) return "track";
	if (!strcmp(id, "TDRC") || !strcmp(id, "TYER") || !strcmp(id, "TYE")) return "year";
	if (!strcmp(id, "TCON") || !strcmp(id, "TCO")) return "genre";
	if (!strcmp(id, "TCOM") || !strcmp(id, "TCM")) return "composer";
	if (!strcmp(id, "TPE2") || !strcmp(id, "TP2")) return "album artist";
	if (!strcmp(id, "TPUB") || !strcmp(id, "TPB")) return "publisher";
	if (!strcmp(id, "TCOP") || !strcmp(id, "TCR")) return "copyright";
	return id;
}

static void PrintCommentTag(const unsigned char *data, unsigned long bytes)
{
	unsigned long pos;
	unsigned long terminatorBytes;
	unsigned char *text;

	if (bytes < 5)
		return;
	terminatorBytes = (data[0] == 1 || data[0] == 2) ? 2UL : 1UL;
	pos = 4;
	while (pos + terminatorBytes <= bytes) {
		if (data[pos] == 0 && (terminatorBytes == 1 || data[pos + 1] == 0)) {
			pos += terminatorBytes;
			break;
		}
		pos += terminatorBytes;
	}
	if (pos >= bytes)
		return;
	text = (unsigned char *)malloc((size_t)(bytes - pos + 1));
	if (!text)
		return;
	text[0] = data[0];
	memcpy(text + 1, data + pos, (size_t)(bytes - pos));
	PrintTagText("comment", text, bytes - pos + 1);
	free(text);
}

static unsigned long PrintId3v2(FILE *fp)
{
	unsigned char header[10];
	unsigned long tagBytes;
	unsigned long pos;
	int major;

	if (fseek(fp, 0, SEEK_SET) != 0 || fread(header, 1, sizeof(header), fp) != sizeof(header) ||
		memcmp(header, "ID3", 3) != 0)
		return 0;
	major = header[3];
	tagBytes = SynchsafeSize(header + 6);
	printf("ID3v2: 2.%d.%d (%lu bytes)\n", major, header[4], tagBytes);
	pos = 0;
	if ((header[5] & 0x40) && major >= 3) {
		unsigned char extSize[4];
		unsigned long skipBytes;
		if (fread(extSize, 1, sizeof(extSize), fp) != sizeof(extSize))
			return 10UL + tagBytes;
		skipBytes = major == 4 ? SynchsafeSize(extSize) : BigEndianSize(extSize, 4) + 4UL;
		if (skipBytes < 4 || skipBytes > tagBytes ||
			fseek(fp, (long)(skipBytes - 4UL), SEEK_CUR) != 0)
			return 10UL + tagBytes;
		pos = skipBytes;
	}
	while (pos + (major == 2 ? 6UL : 10UL) <= tagBytes) {
		unsigned char frameHeader[10];
		unsigned char *frame;
		unsigned long frameBytes;
		int headerBytes;
		char id[5];

		headerBytes = major == 2 ? 6 : 10;
		if (fread(frameHeader, 1, (size_t)headerBytes, fp) != (size_t)headerBytes)
			break;
		pos += (unsigned long)headerBytes;
		if (!frameHeader[0])
			break;
		memset(id, 0, sizeof(id));
		memcpy(id, frameHeader, major == 2 ? 3 : 4);
		if (major == 2)
			frameBytes = BigEndianSize(frameHeader + 3, 3);
		else if (major == 4)
			frameBytes = SynchsafeSize(frameHeader + 4);
		else
			frameBytes = BigEndianSize(frameHeader + 4, 4);
		if (!frameBytes || frameBytes > tagBytes - pos)
			break;
		if ((id[0] == 'T' || !strcmp(id, "COMM") || !strcmp(id, "COM")) &&
			frameBytes <= 1024UL * 1024UL) {
			frame = (unsigned char *)malloc((size_t)frameBytes);
			if (!frame || fread(frame, 1, (size_t)frameBytes, fp) != (size_t)frameBytes) {
				free(frame);
				break;
			}
			if (!strcmp(id, "COMM") || !strcmp(id, "COM"))
				PrintCommentTag(frame, frameBytes);
			else
				PrintTagText(TagFrameLabel(id), frame, frameBytes);
			free(frame);
		} else {
			if (!strcmp(id, "APIC") || !strcmp(id, "PIC"))
				printf("embedded artwork: %lu bytes\n", frameBytes);
			if (fseek(fp, (long)frameBytes, SEEK_CUR) != 0)
				break;
		}
		pos += frameBytes;
	}
	return 10UL + tagBytes + ((header[5] & 0x10) ? 10UL : 0UL);
}

static void PrintFixedId3v1Text(const char *label, const unsigned char *data, int bytes)
{
	int end;
	int i;

	end = bytes;
	while (end > 0 && (data[end - 1] == 0 || data[end - 1] == ' '))
		end--;
	if (!end)
		return;
	printf("ID3v1 %s: ", label);
	for (i = 0; i < end; i++)
		putchar(data[i] >= 32 && data[i] != 127 ? data[i] : ' ');
	putchar('\n');
}

static void PrintId3v1(FILE *fp, long fileSize)
{
	unsigned char tag[128];

	if (fileSize < 128 || fseek(fp, fileSize - 128, SEEK_SET) != 0 ||
		fread(tag, 1, sizeof(tag), fp) != sizeof(tag) || memcmp(tag, "TAG", 3) != 0)
		return;
	printf("ID3v1: present\n");
	PrintFixedId3v1Text("title", tag + 3, 30);
	PrintFixedId3v1Text("artist", tag + 33, 30);
	PrintFixedId3v1Text("album", tag + 63, 30);
	PrintFixedId3v1Text("year", tag + 93, 4);
	if (tag[125] == 0 && tag[126] != 0)
		printf("ID3v1 track: %u\n", (unsigned int)tag[126]);
	printf("ID3v1 genre index: %u\n", (unsigned int)tag[127]);
}

static void PrintFirstFrameInfo(const Mp3InputInfo *inputInfo)
{
	const MP3FrameInfo *info;
	const char *version;

	if (!inputInfo->firstFrameFound) {
		printf("first MPEG frame offset: not found\n");
		printf("MPEG audio: no valid frame found after tags\n");
		return;
	}
	info = &inputInfo->firstFrameInfo;
	version = info->version == MPEG1 ? "1" : (info->version == MPEG2 ? "2" : "2.5");
	printf("first MPEG frame offset: %lu\n", inputInfo->firstFrameOffset);
	printf("MPEG audio: version %s, layer %d\n", version, info->layer);
	printf("sample rate: %d Hz\n", info->samprate);
	printf("channels: %d\n", info->nChans);
	printf("bitrate: %d bps\n", info->bitrate);
}

static void PrintMp3Info(FILE *fp, const char *name)
{
	long fileSize;
	InputSource input;

	fileSize = -1;
	if (fseek(fp, 0, SEEK_END) == 0)
		fileSize = ftell(fp);
	printf("file: %s\n", name);
	if (fileSize >= 0)
		printf("file size: %lu bytes\n", (unsigned long)fileSize);
	InputSourceInit(&input, fp);
	InputSourcePrepareMp3(&input);
	printf("ID3v2 detected: %s\n", input.info.id3v2Detected ? "yes" : "no");
	if (input.info.id3v2Detected)
		printf("ID3v2 version: 2.%d.%d\n", input.info.id3v2Major,
			input.info.id3v2Revision);
	printf("ID3v2 size skipped: %lu bytes\n", input.info.id3v2SkipBytes);
	PrintId3v2(fp);
	PrintFirstFrameInfo(&input.info);
	if (fileSize >= 0)
		PrintId3v1(fp, fileSize);
	fseek(fp, 0, SEEK_SET);
}


static const char *PathBaseName(const char *path)
{
	const char *base;

	base = path;
	while (path && *path) {
		if (*path == '/' || *path == ':' || *path == '\\')
			base = path + 1;
		path++;
	}

	return base ? base : "";
}

static int OutputNameIsDirectory(const char *path)
{
	size_t len;

	if (!path || !path[0])
		return 0;
	len = strlen(path);
	return path[len - 1] == ':' || path[len - 1] == '/' || path[len - 1] == '\\';
}

static const char *DefaultOutputExtension(const DecodeOptions *opt)
{
	if (opt->outFormat == OUT_8SVX)
		return ".8svx";
	if (opt->outFormat == OUT_S8)
		return ".s8";
	return ".pcm";
}

static char *BuildDirectoryOutputName(const char *dir, const char *input,
	const DecodeOptions *opt)
{
	const char *base;
	const char *dot;
	const char *ext;
	size_t dirLen;
	size_t stemLen;
	size_t extLen;
	char *name;

	base = PathBaseName(input);
	if (!base[0])
		base = "output";
	dot = strrchr(base, '.');
	stemLen = dot && dot != base ? (size_t)(dot - base) : strlen(base);
	ext = DefaultOutputExtension(opt);
	dirLen = strlen(dir);
	extLen = strlen(ext);

	name = (char *)malloc(dirLen + stemLen + extLen + 1);
	if (!name)
		return NULL;
	memcpy(name, dir, dirLen);
	memcpy(name + dirLen, base, stemLen);
	memcpy(name + dirLen + stemLen, ext, extLen + 1);
	return name;
}

static void *AllocFastInputMemory(unsigned long bytes)
{
#ifdef HAVE_AMIGA_AUDIO_DEVICE
	return AllocMem(bytes, MEMF_FAST);
#else
	return malloc((size_t)bytes);
#endif
}

static void FreeFastInputMemory(void *memory, unsigned long bytes)
{
#ifdef HAVE_AMIGA_AUDIO_DEVICE
	if (memory)
		FreeMem(memory, bytes);
#else
	(void)bytes;
	free(memory);
#endif
}

static void InputSourceInit(InputSource *input, FILE *file)
{
	memset(input, 0, sizeof(*input));
	input->file = file;
}

static void InputSourceInitRadio(InputSource *input, RadioStream *radio)
{
	memset(input, 0, sizeof(*input));
	input->radio = radio;
#if ENABLE_RADIO
	/* Let radio_stream.c's internal wait loops poll the shared interrupt
	 * flag so a Stop click reaches a stream stalled inside
	 * Radio_ReadAudio()/Radio_Pump() on a dead socket.  Without this the
	 * only stop checks ran between InputSourceRead() calls, and a stream
	 * stuck in "Buffering" never returned from the pump loop to see them:
	 * Stop timed out, the playback child could not be reaped, and the GUI
	 * froze in its shutdown wait.  The cast covers the non-Amiga build
	 * where the flag is volatile sig_atomic_t (int-sized on all supported
	 * hosts). */
	Radio_SetStopFlag((const volatile int *)&gPlaybackInterrupted);
#endif
}

#ifdef HAVE_AMIGA_AUDIO_DEVICE
static void InputSourceInitAmigaDos(InputSource *input, BPTR amigaFile)
{
	memset(input, 0, sizeof(*input));
	input->amigaFile = amigaFile;
	input->useAmigaDos = amigaFile ? 1 : 0;
}
#endif

/* Test mode (MP3_POST_AUDIO_LEAK_TEST=1 in the environment): the caller skips
 * MP3FreeDecoder()/InputSourceClose() entirely and leaks the decoder, ring
 * buffer, and RadioStream struct instead of freeing them.  See the call site
 * in main()'s radio-streaming teardown. */
static int AmigaPostAudioLeakTestEnabled(void)
{
	static int cached = -1;
	if (cached < 0) {
		const char *v = getenv("MP3_POST_AUDIO_LEAK_TEST");
		cached = (v && *v && *v != '0') ? 1 : 0;
	}
	return cached;
}

static void InputSourceClose(InputSource *input)
{
	int radioFatal = 0;

	if (!input)
		return;
#if ENABLE_RADIO
	RADIO_STOP_DEBUG_PRINTF(("radio-stop: InputSourceClose entered\n"));
	radioFatal = input->radio ? Radio_IsSessionFatal(input->radio) : 0;
	RADIO_DBG(printf("radio-teardown: before fast-input-memory free/skip session=%lu ptr=%p size=%lu fatal=%d\n",
		input->radio ? Radio_GetSessionId(input->radio) : 0UL, (void *)input->memory, input->memorySize, radioFatal));
	Radio_DebugCheckExecMem("before fast input memory free/skip");
#endif
	if (radioFatal) {
		/* Fatal TLS teardown quarantine (see Radio_IsSessionFatal()): this
		 * buffer may have been mid-write from the same corrupted AmiSSL/
		 * bsdsocket state that poisoned the session -- leak it instead of
		 * risking a free-list write into already-damaged exec memory. */
		RADIO_DBG(printf("radio-teardown: fast-input-memory free skipped (fatal TLS quarantine) session=%lu -- leaking\n",
			input->radio ? Radio_GetSessionId(input->radio) : 0UL));
	} else {
		FreeFastInputMemory(input->memory, input->memorySize);
	}
	input->memory = NULL;
	input->memorySize = 0;
	input->memoryPos = 0;
#if ENABLE_RADIO
	Radio_DebugCheckExecMem("after fast input memory free/skip");
#endif
	if (input->radio) {
		RadioStream *radio = input->radio;
		RadioStatus status = Radio_GetStatus(radio);
		int buffered = Radio_GetBufferedBytes(radio);
		const char *error = Radio_GetError(radio);
		if (!gPlaybackInterrupted &&
			status != RADIO_STATUS_ERROR &&
			status != RADIO_STATUS_STOPPING &&
			status != RADIO_STATUS_CLOSED &&
			(buffered > 0 || (error && !error[0]))) {
			RADIO_INPUT_DIAG_PRINTF("radio-input: WARNING decoder exiting normal while radio buffered=%d status=%d error=\"%s\" session=%lu\n",
				buffered, (int)status, error ? error : "", Radio_GetSessionId(radio));
		}
		RADIO_DBG(printf("radio-teardown: before first Radio_RequestStop (InputSourceClose) session=%lu\n",
			Radio_GetSessionId(radio)));
		Radio_RequestStop(radio);
		input->radio = NULL;
		GuiMarkRadioStopped();
		Radio_Close(radio);
	}
#ifdef HAVE_AMIGA_AUDIO_DEVICE
	if (input->useAmigaDos && input->amigaFile) {
		Close(input->amigaFile);
		input->amigaFile = (BPTR)0;
	}
	input->useAmigaDos = 0;
#endif
	free(input->oggFlac);
	input->oggFlac = NULL;
#if ENABLE_RADIO
	RADIO_STOP_DEBUG_PRINTF(("radio-stop: InputSourceClose exited\n"));
#endif
}

static void CloseInputFile(FILE **file, int debugCleanup)
{
	if (!file || !*file)
		return;
	fclose(*file);
	*file = NULL;
	if (debugCleanup)
		printf("debug-cleanup: input file closed: yes\n");
}

/* These two helpers have no radio dependency (DateStamp/clock() and
 * Delay()/usleep() only) and are referenced unconditionally by
 * InputSourceRead()'s live-stream branch below, so they must be defined in
 * every build -- not just #if ENABLE_RADIO -- or a non-radio link fails with
 * an undefined reference to radio_input_clock_ms. */
static unsigned long radio_input_clock_ms(void)
{
#if defined(AMIGA_M68K) && defined(HAVE_AMIGA_AUDIO_DEVICE)
	struct DateStamp ds;
	DateStamp(&ds);
	return (unsigned long)ds.ds_Days * 86400000UL +
		(unsigned long)ds.ds_Minute * 60000UL +
		(unsigned long)ds.ds_Tick * 20UL;
#else
	return (unsigned long)(clock() * 1000UL / CLOCKS_PER_SEC);
#endif
}

static void radio_input_wait_tick(void)
{
#if defined(AMIGA_M68K) && defined(HAVE_AMIGA_AUDIO_DEVICE)
	Delay(1);
#else
	usleep(20000);
#endif
}

static size_t InputSourceReadRaw(InputSource *input, void *dest, size_t bytes)
{
	if (input && input->prefixPos < input->prefixSize) {
		unsigned long avail = input->prefixSize - input->prefixPos;
		size_t take = bytes < (size_t)avail ? bytes : (size_t)avail;
		memcpy(dest, input->prefix + input->prefixPos, take);
		input->prefixPos += (unsigned long)take;
		if (take == bytes)
			return take;
		return take + InputSourceReadRaw(input, (unsigned char *)dest + take, bytes - take);
	}
	if (input && input->radio) {
		unsigned long startMs = radio_input_clock_ms();
		unsigned long lastLogMs = 0;
		const unsigned long maxWaitMs = 15000UL;

		for (;;) {
			RadioStatus status;
			int buffered;
			size_t got;

			if (gPlaybackInterrupted) {
				Radio_RequestStop(input->radio);
				GuiMarkRadioStopped();
				return 0;
			}

			status = Radio_GetStatus(input->radio);
			if (status == RADIO_STATUS_ERROR ||
				status == RADIO_STATUS_STOPPING ||
				status == RADIO_STATUS_CLOSED) {
				GuiPublishRadioMetadata(input->radio);
				return 0;
			}

			got = (size_t)Radio_ReadAudio(input->radio, (unsigned char *)dest, (int)bytes);
			if (got > 0) {
				status = Radio_GetStatus(input->radio);
				if (status != RADIO_STATUS_STOPPING && status != RADIO_STATUS_CLOSED)
					GuiPublishRadioMetadata(input->radio);
				return got;
			}

			status = Radio_GetStatus(input->radio);
			buffered = Radio_GetBufferedBytes(input->radio);
			if (status == RADIO_STATUS_ERROR ||
				status == RADIO_STATUS_STOPPING ||
				status == RADIO_STATUS_CLOSED) {
				GuiPublishRadioMetadata(input->radio);
				return 0;
			}

			if (radio_input_clock_ms() - lastLogMs >= 1000UL) {
				RADIO_INPUT_DIAG_PRINTF("radio-input: zero read requested=%lu got=0 status=%d buffered=%d error=\"%s\" session=%lu -- waiting, not EOF\n",
					(unsigned long)bytes,
					(int)status,
					buffered,
					Radio_GetError(input->radio),
					Radio_GetSessionId(input->radio));
				lastLogMs = radio_input_clock_ms();
			}

			if (radio_input_clock_ms() - startMs >= maxWaitMs) {
				RADIO_INPUT_DIAG_PRINTF("radio-input: live stream read timeout requested=%lu status=%d buffered=%d error=\"%s\" session=%lu\n",
					(unsigned long)bytes,
					(int)status,
					buffered,
					Radio_GetError(input->radio),
					Radio_GetSessionId(input->radio));
				Radio_FailStartup(input->radio, "radio stream stalled - no audio delivered");
				GuiPublishRadioMetadata(input->radio);
				return 0;
			}

			radio_input_wait_tick();
		}
	}
	if (input->memory) {
		unsigned long available;

		available = input->memorySize - input->memoryPos;
		if (bytes > (size_t)available)
			bytes = (size_t)available;
		if (bytes > 0) {
			memcpy(dest, input->memory + input->memoryPos, bytes);
			input->memoryPos += (unsigned long)bytes;
		}
		return bytes;
	}
#ifdef HAVE_AMIGA_AUDIO_DEVICE
	if (input->useAmigaDos) {
		LONG nRead;

		if (!input->amigaFile || bytes == 0)
			return 0;
		if (bytes > (size_t)2147483647L)
			bytes = (size_t)2147483647L;
		nRead = Read(input->amigaFile, dest, (LONG)bytes);
		return nRead < 0 ? 0 : (size_t)nRead;
	}
#endif
	return fread(dest, 1, bytes, input->file);
}

static size_t InputSourceReadRawCb(void *ctx, unsigned char *dest, size_t bytes)
{
	return InputSourceReadRaw((InputSource *)ctx, dest, bytes);
}

static size_t InputSourceRead(InputSource *input, void *dest, size_t bytes)
{
	if (input && input->oggFlac)
		return radio_oggflac_read(input->oggFlac, InputSourceReadRawCb, input,
			(unsigned char *)dest, bytes);
	return InputSourceReadRaw(input, dest, bytes);
}

#ifdef HAVE_AMIGA_AUDIO_DEVICE
/* Reads the first bytes of a radio stream into the replay prefix (so the
 * decoder still sees them) and reports whether it is FLAC, in Ogg or
 * native, or Ogg Vorbis (see radio_oggflac.h).  Only for a fresh stream. */
static int RadioSniffFlac(InputSource *input)
{
	unsigned char head[RADIO_SNIFF_BYTES];
	unsigned long total = 0;

	if (!input || !input->radio || input->prefixSize != 0)
		return RADIO_SNIFF_UNKNOWN;
	while (total < sizeof(head)) {
		size_t got = InputSourceReadRaw(input, head + total, sizeof(head) - (size_t)total);
		if (got == 0)
			break;
		total += (unsigned long)got;
	}
	memcpy(input->prefix, head, (size_t)total);
	input->prefixSize = total;
	input->prefixPos = 0;
	return radio_oggflac_sniff(head, (size_t)total);
}

static int InputSourceStartOggFlac(InputSource *input)
{
	unsigned char *mem = (unsigned char *)malloc(sizeof(RadioOggFlac) + RADIO_OGGFLAC_PAGE_MAX);
	if (!mem)
		return 0;
	input->oggFlac = (RadioOggFlac *)mem;
	radio_oggflac_init(input->oggFlac, mem + sizeof(RadioOggFlac));
	return 1;
}
#endif

static unsigned long InputSourceTell(const InputSource *input)
{
	if (input->radio)
		return 0;
	if (input->memory)
		return input->memoryPos;
#ifdef HAVE_AMIGA_AUDIO_DEVICE
	if (input->useAmigaDos) {
		LONG pos = input->amigaFile ? Seek(input->amigaFile, 0, OFFSET_CURRENT) : -1;
		return pos < 0 ? 0UL : (unsigned long)pos;
	}
#endif
	{
		long pos = ftell(input->file);
		return pos < 0 ? 0UL : (unsigned long)pos;
	}
}

static void InputSourceSeek(InputSource *input, unsigned long pos)
{
	if (input->radio)
		return;
	if (input->memory) {
		input->memoryPos = pos <= input->memorySize ? pos : input->memorySize;
	} else {
#ifdef HAVE_AMIGA_AUDIO_DEVICE
		if (input->useAmigaDos) {
			if (input->amigaFile)
				Seek(input->amigaFile, (LONG)pos, OFFSET_BEGINNING);
		} else
#endif
		{
			fseek(input->file, (long)pos, SEEK_SET);
		}
	}
}

#define FAST_INPUT_PRELOAD_CHUNK 32768UL

static int FastInputPreloadStopRequested(void)
{
#ifdef HAVE_AMIGA_AUDIO_DEVICE
	if (SetSignal(0, 0) & SIGBREAKF_CTRL_C)
		gPlaybackInterrupted = 1;
#endif
	return gPlaybackInterrupted != 0;
}

static int InputSourcePreloadFastMemory(InputSource *input)
{
	long fileSize;
	unsigned char *memory;
	size_t copied;
#ifdef HAVE_AMIGA_AUDIO_DEVICE
	if (input->useAmigaDos) {
		LONG oldPos;
		LONG endPos;

		if (!input->amigaFile)
			return -1;
		oldPos = Seek(input->amigaFile, 0, OFFSET_END);
		if (oldPos < 0) {
			Seek(input->amigaFile, 0, OFFSET_BEGINNING);
			return -1;
		}
		endPos = Seek(input->amigaFile, 0, OFFSET_CURRENT);
		if (endPos <= 0 || (unsigned long)endPos > (unsigned long)(size_t)-1) {
			Seek(input->amigaFile, 0, OFFSET_BEGINNING);
			return -1;
		}
		fileSize = endPos;
		if (Seek(input->amigaFile, 0, OFFSET_BEGINNING) < 0)
			return -1;
	} else
#endif
	{
		if (fseek(input->file, 0, SEEK_END) != 0)
			return -1;
		fileSize = ftell(input->file);
		if (fileSize <= 0 || (unsigned long)fileSize > (unsigned long)(size_t)-1) {
			fseek(input->file, 0, SEEK_SET);
			return -1;
		}
		if (fseek(input->file, 0, SEEK_SET) != 0)
			return -1;
	}

	if (FastInputPreloadStopRequested())
		return 1;
	memory = (unsigned char *)AllocFastInputMemory((unsigned long)fileSize);
	if (!memory)
		return -1;

	copied = 0;
	while (copied < (size_t)fileSize) {
		size_t chunk;
		size_t nRead;

		if (FastInputPreloadStopRequested()) {
			FreeFastInputMemory(memory, (unsigned long)fileSize);
			InputSourceSeek(input, 0);
			return 1;
		}
		chunk = (size_t)fileSize - copied;
		if (chunk > (size_t)FAST_INPUT_PRELOAD_CHUNK)
			chunk = (size_t)FAST_INPUT_PRELOAD_CHUNK;
		nRead = InputSourceRead(input, memory + copied, chunk);
		if (nRead != chunk) {
			FreeFastInputMemory(memory, (unsigned long)fileSize);
			InputSourceSeek(input, 0);
			return -1;
		}
		copied += nRead;
	}
	if (FastInputPreloadStopRequested()) {
		FreeFastInputMemory(memory, (unsigned long)fileSize);
		InputSourceSeek(input, 0);
		return 1;
	}

	input->memory = memory;
	input->memorySize = (unsigned long)fileSize;
	input->memoryPos = 0;
	printf("fast-mem input preload: copying %lu bytes to Fast RAM\n", input->memorySize);
	return 0;
}

static int MpegHeaderLooksValid(const unsigned char *header)
{
	if (header[0] != 0xff || (header[1] & 0xe0) != 0xe0)
		return 0;
	/* Reject reserved MPEG version, anything other than Layer III, the reserved
	 * bitrate index, and the reserved sample-rate index. */
	if ((header[1] & 0x18) == 0x08 || (header[1] & 0x06) != 0x02)
		return 0;
	if ((header[2] & 0xf0) == 0xf0 || (header[2] & 0x0c) == 0x0c)
		return 0;
	return 1;
}

static int FindValidatedMpegSync(const unsigned char *buf, int nBytes)
{
	int i;

	for (i = 0; i <= nBytes - 4; i++) {
		if (MpegHeaderLooksValid(buf + i))
			return i;
	}
	return -1;
}

static int InputSourcePrepareMp3(InputSource *input)
{
	if (input->radio)
		return 0;
	unsigned char header[10];
	unsigned char scan[READBUF_SIZE];
	unsigned long scanBase;
	unsigned long tagBytes;
	size_t nRead;
	int keep;
	HMP3Decoder decoder;

	memset(&input->info, 0, sizeof(input->info));
	InputSourceSeek(input, 0);
	nRead = InputSourceRead(input, header, sizeof(header));
	if (nRead == sizeof(header) && memcmp(header, "ID3", 3) == 0) {
		input->info.id3v2Detected = 1;
		input->info.id3v2Major = header[3];
		input->info.id3v2Revision = header[4];
		input->info.id3v2Flags = header[5];
		if (!(header[6] & 0x80) && !(header[7] & 0x80) &&
			!(header[8] & 0x80) && !(header[9] & 0x80)) {
			tagBytes = SynchsafeSize(header + 6);
			input->info.id3v2SkipBytes = 10UL + tagBytes;
			if (header[5] & 0x10)
				input->info.id3v2SkipBytes += 10UL;
		}
	}

	InputSourceSeek(input, input->info.id3v2SkipBytes);
	scanBase = input->info.id3v2SkipBytes;
	keep = 0;
	decoder = MP3InitDecoder();
	if (!decoder)
		return -1;
	for (;;) {
		int offset;
		int available;

		nRead = InputSourceRead(input, scan + keep, sizeof(scan) - (size_t)keep);
		available = keep + (int)nRead;
		offset = FindValidatedMpegSync(scan, available);
		while (offset >= 0) {
			MP3FrameInfo frameInfo;
			if (MP3GetNextFrameInfo(decoder, &frameInfo, scan + offset) == ERR_MP3_NONE) {
				input->info.firstFrameFound = 1;
				input->info.firstFrameOffset = scanBase + (unsigned long)offset;
				input->info.firstFrameInfo = frameInfo;
				MP3FreeDecoder(decoder);
				InputSourceSeek(input, input->info.firstFrameOffset);
				return 0;
			}
			offset++;
			if (offset > available - 4)
				break;
			{
				int next = FindValidatedMpegSync(scan + offset, available - offset);
				offset = next < 0 ? -1 : offset + next;
			}
		}
		if (nRead == 0)
			break;
		keep = available < 3 ? available : 3;
		memmove(scan, scan + available - keep, (size_t)keep);
		scanBase += (unsigned long)(available - keep);
	}
	MP3FreeDecoder(decoder);
	InputSourceSeek(input, input->info.id3v2SkipBytes);
	return 0;
}


static void InputSourceAlignDecodePointer(unsigned char *readBuf, unsigned char **readPtr, int *bytesLeft)
{
#if defined(AMIGA_M68K)
	unsigned long addr;
	if (!readBuf || !readPtr || !*readPtr || !bytesLeft || *bytesLeft <= 0)
		return;
	addr = (unsigned long)*readPtr;
	if ((addr & 3UL) != 0 && *readPtr != readBuf) {
		memmove(readBuf, *readPtr, (size_t)*bytesLeft);
		*readPtr = readBuf;
	}
#else
	(void)readBuf;
	(void)readPtr;
	(void)bytesLeft;
#endif
}

static int FillReadBuffer(unsigned char *readBuf, unsigned char *readPtr, int bufSize,
	int bytesLeft, InputSource *input)
{
	int nRead;

	memmove(readBuf, readPtr, bytesLeft);
	nRead = (int)InputSourceRead(input, readBuf + bytesLeft,
		(size_t)(bufSize - bytesLeft));
	if (nRead < bufSize - bytesLeft) {
		memset(readBuf + bytesLeft + nRead, 0,
			bufSize - bytesLeft - nRead);
	}

	return nRead;
}

static TimingStats *gTiming;

static unsigned long ClocksToMilliseconds(clock_t c)
{
	if (CLOCKS_PER_SEC <= 0 || c <= 0)
		return 0;
	return IntegerScale((uint32_t)c, (uint32_t)CLOCKS_PER_SEC, 1000, 0);
}

static void PrintIntegerRatio(const char *label, unsigned long numerator,
	unsigned long denominator, int digits, const char *suffix)
{
	char value[32];
	IntegerRatioText(value, sizeof(value), (uint32_t)numerator,
		(uint32_t)denominator, digits);
	printf("%s%s%s", label, value, suffix);
}

static void PrintClockSeconds(const char *label, clock_t ticks)
{
	PrintIntegerRatio(label, ticks > 0 ? (unsigned long)ticks : 0,
		CLOCKS_PER_SEC > 0 ? (unsigned long)CLOCKS_PER_SEC : 0, 3, " s\n");
}

static int TimedFputc(int c, FILE *fp)
{
	clock_t t0;
	int r;

	if (!fp)
		return c;
	if (!gTiming)
		return fputc(c, fp);
	t0 = clock();
	r = fputc(c, fp);
	gTiming->fileWrite += clock() - t0;
	return r;
}

static size_t TimedFwrite(const void *ptr, size_t size, size_t nmemb, FILE *fp)
{
	clock_t t0;
	size_t r;

	if (!fp)
		return nmemb;
	if (!gTiming)
		return fwrite(ptr, size, nmemb, fp);
	t0 = clock();
	r = fwrite(ptr, size, nmemb, fp);
	gTiming->fileWrite += clock() - t0;
	return r;
}

static void WriteU16BE(FILE *fp, unsigned int v)
{
	TimedFputc((int)((v >> 8) & 0xff), fp);
	TimedFputc((int)(v & 0xff), fp);
}

static void WriteU32BE(FILE *fp, unsigned long v)
{
	TimedFputc((int)((v >> 24) & 0xff), fp);
	TimedFputc((int)((v >> 16) & 0xff), fp);
	TimedFputc((int)((v >> 8) & 0xff), fp);
	TimedFputc((int)(v & 0xff), fp);
}

static void PatchU32BE(FILE *fp, long pos, unsigned long v)
{
	long cur;

	cur = ftell(fp);
	fseek(fp, pos, SEEK_SET);
	WriteU32BE(fp, v);
	fseek(fp, cur, SEEK_SET);
}

static signed char Sample16ToS8(short s)
{
	return (signed char)(s >> 8);
}

#if defined(AMIGA_M68K) && defined(AMIGA_M68K_ASM_PLANARS8)
extern void PlanarS8FromInterleavedStereo_Amiga_m68k(signed char *left,
	signed char *right, const short *pcm, int count)
	__asm__("PlanarS8FromInterleavedStereo_Amiga_m68k")
	__attribute__((weak));

static int PlanarS8FromInterleavedStereo_Amiga_m68k_IsActive(void)
{
	return PlanarS8FromInterleavedStereo_Amiga_m68k ? 1 : 0;
}
#endif

/* Branch-free true-stereo interleaved-s16 -> planar-s8 conversion: no
 * per-sample channel/fakeStereo test, since the caller only reaches this
 * once it already knows the block is real (non-fake) stereo. Dispatches to
 * PlanarS8FromInterleavedStereo_Amiga_m68k() when built with
 * AMIGA_M68K_ASM_PLANARS8 and the weak symbol resolved; the C loop below is
 * the unconditional reference and is bit-exact with it (Sample16ToS8() is
 * `s >> 8`, which on this big-endian target is just the sample's high byte).
 */
static void PlanarS8FillTrueStereo(signed char *left, signed char *right,
	const short *pcm, int count)
{
#if defined(AMIGA_M68K) && defined(AMIGA_M68K_ASM_PLANARS8)
	if (PlanarS8FromInterleavedStereo_Amiga_m68k_IsActive()) {
		PlanarS8FromInterleavedStereo_Amiga_m68k(left, right, pcm, count);
		return;
	}
#endif
	{
		int i;
		for (i = 0; i < count; i++) {
			left[i] = Sample16ToS8(pcm[2 * i]);
			right[i] = Sample16ToS8(pcm[2 * i + 1]);
		}
	}
}

/* Unconditional C reference for the conversion above -- ground truth for
 * SelftestPlanarS8TrueStereo(), same role as the polyphase _C_REFERENCE
 * wrappers elsewhere in this file. */
static void PlanarS8FillTrueStereo_C_REFERENCE(signed char *left,
	signed char *right, const short *pcm, int count)
{
	int i;

	for (i = 0; i < count; i++) {
		left[i] = Sample16ToS8(pcm[2 * i]);
		right[i] = Sample16ToS8(pcm[2 * i + 1]);
	}
}

static int SelftestPlanarS8TrueStereo(void)
{
	enum { COUNT = 600 };
	static short pcm[COUNT * 2];
	static signed char cleft[COUNT];
	static signed char cright[COUNT];
	static signed char aleft[COUNT];
	static signed char aright[COUNT];
	unsigned long seed;
	int i;
	int failures;

	seed = 0x9e3779b9UL;
	for (i = 0; i < COUNT * 2; i++) {
		seed = seed * 1664525UL + 1013904223UL;
		pcm[i] = (short)seed;
	}
	/* Deterministic edge values alongside the random fill. */
	pcm[0] = 0;
	pcm[1] = -1;
	pcm[2] = 32767;
	pcm[3] = -32768;

	PlanarS8FillTrueStereo_C_REFERENCE(cleft, cright, pcm, COUNT);
	PlanarS8FillTrueStereo(aleft, aright, pcm, COUNT);

	failures = 0;
	for (i = 0; i < COUNT; i++) {
		if (cleft[i] != aleft[i] || cright[i] != aright[i]) {
			printf("PlanarS8 true-stereo mismatch [%d]: c=(%d,%d) asm=(%d,%d) pcm=(%d,%d)\n",
				i, (int)cleft[i], (int)cright[i], (int)aleft[i], (int)aright[i],
				(int)pcm[2 * i], (int)pcm[2 * i + 1]);
			failures++;
		}
	}
	/* count == 0 must be a safe no-op. */
	PlanarS8FillTrueStereo(aleft, aright, pcm, 0);

	printf("Planar S8 true-stereo asm requested: %s\n",
#ifdef AMIGA_M68K_ASM_PLANARS8
		"yes"
#else
		"no"
#endif
	);
	printf("Planar S8 true-stereo asm active: %s\n",
#if defined(AMIGA_M68K) && defined(AMIGA_M68K_ASM_PLANARS8)
		PlanarS8FromInterleavedStereo_Amiga_m68k_IsActive() ? "yes" : "no"
#else
		"no"
#endif
	);
	printf("Planar S8 true-stereo selftest cases: %d\n", (int)COUNT);
	printf("Planar S8 true-stereo selftest failures: %d\n", failures);
	return failures ? 1 : 0;
}

static short ClipToS16(int v)
{
	if (v > 32767)
		return 32767;
	if (v < -32768)
		return -32768;
	return (short)v;
}

static int MixFrame(const short *in, short *out, int inSamps, int channels, int mono)
{
	int i;
	int n;

	if (!mono || channels == 1) {
		memmove(out, in, inSamps * sizeof(short));
		return inSamps;
	}

	n = inSamps / 2;
	for (i = 0; i < n; i++) {
		out[i] = (short)(((int)in[2 * i] + (int)in[2 * i + 1]) / 2);
	}

	return n;
}

static int WriteRawSamples(FILE *fp, const short *pcm, int nSamps, int format)
{
	int i;

	if (format == OUT_S8) {
		for (i = 0; i < nSamps; i++)
			TimedFputc((int)(unsigned char)Sample16ToS8(pcm[i]), fp);
	} else {
		for (i = 0; i < nSamps; i++)
			WriteU16BE(fp, (unsigned int)(unsigned short)pcm[i]);
	}

	return (fp && ferror(fp)) ? -1 : 0;
}

static int SvxBegin(SvxWriter *svx, FILE *fp, int sampleRate, int compression)
{
	memset(svx, 0, sizeof(*svx));
	svx->fp = fp;
	svx->compression = compression;

	TimedFwrite("FORM", 1, 4, fp);
	svx->formSizePos = ftell(fp);
	WriteU32BE(fp, 0);
	TimedFwrite("8SVX", 1, 4, fp);

	TimedFwrite("VHDR", 1, 4, fp);
	WriteU32BE(fp, 20);
	svx->oneShotPos = ftell(fp);
	WriteU32BE(fp, 0);              /* oneShotHiSamples */
	WriteU32BE(fp, 0);              /* repeatHiSamples */
	WriteU32BE(fp, 0);              /* samplesPerHiCycle */
	WriteU16BE(fp, (unsigned int)sampleRate);
	TimedFputc(1, fp);                   /* ctOctave */
	TimedFputc(compression, fp);         /* sCompression */
	WriteU32BE(fp, 0x00010000UL);   /* volume */

	TimedFwrite("BODY", 1, 4, fp);
	svx->bodySizePos = ftell(fp);
	WriteU32BE(fp, 0);

	return ferror(fp) ? -1 : 0;
}

static void SvxWriteByte(SvxWriter *svx, unsigned char b)
{
	TimedFputc((int)b, svx->noOutput ? NULL : svx->fp);
	svx->bodyBytes++;
}

static int FibDeltaNibble(signed char prev, signed char sample)
{
	static const int deltaTable[16] = {
		-34, -21, -13, -8, -5, -3, -2, -1,
		0, 1, 2, 3, 5, 8, 13, 21
	};
	int best;
	int bestErr;
	int i;

	best = 0;
	bestErr = 32767;
	for (i = 0; i < 16; i++) {
		int predicted = (int)prev + deltaTable[i];
		int err;
		if (predicted < -128)
			predicted = -128;
		else if (predicted > 127)
			predicted = 127;
		err = predicted - (int)sample;
		if (err < 0)
			err = -err;
		if (err < bestErr) {
			bestErr = err;
			best = i;
		}
	}

	return best;
}

static signed char FibDeltaApply(signed char prev, int nibble)
{
	static const int deltaTable[16] = {
		-34, -21, -13, -8, -5, -3, -2, -1,
		0, 1, 2, 3, 5, 8, 13, 21
	};
	int v;

	v = (int)prev + deltaTable[nibble & 15];
	if (v < -128)
		v = -128;
	else if (v > 127)
		v = 127;
	return (signed char)v;
}

static void SvxStartFibDelta(SvxWriter *svx, signed char predictor)
{
	/*
	 * 8SVX Fibonacci Delta (D1) BODY data starts with two bytes before
	 * the packed nibble stream.  The D1 unpacker seeds its predictor from
	 * source[1], but it does not copy that byte to the output; every output
	 * sample must still be represented by a following delta nibble.
	 */
	SvxWriteByte(svx, 0);
	SvxWriteByte(svx, (unsigned char)predictor);
	svx->fibPrev = predictor;
	svx->fibStarted = 1;
}

static void SvxWriteFibSample(SvxWriter *svx, signed char sample)
{
	clock_t t0;
	int nibble;

	if (!svx->fibStarted)
		SvxStartFibDelta(svx, sample);

	if (gTiming) {
		t0 = clock();
		nibble = FibDeltaNibble(svx->fibPrev, sample);
		svx->fibPrev = FibDeltaApply(svx->fibPrev, nibble);
		gTiming->fibCompress += clock() - t0;
	} else {
		nibble = FibDeltaNibble(svx->fibPrev, sample);
		svx->fibPrev = FibDeltaApply(svx->fibPrev, nibble);
	}
	if (!svx->fibHaveHighNibble) {
		svx->fibPending = (unsigned char)((nibble & 15) << 4);
		svx->fibHaveHighNibble = 1;
	} else {
		SvxWriteByte(svx, (unsigned char)(svx->fibPending | (nibble & 15)));
		svx->fibHaveHighNibble = 0;
	}
}

static int SvxWriteSamples(SvxWriter *svx, const short *pcm, int nSamps)
{
	int i;

	for (i = 0; i < nSamps; i++) {
		signed char s8 = Sample16ToS8(pcm[i]);
		if (svx->compression == SVX_COMP_FIBDELTA)
			SvxWriteFibSample(svx, s8);
		else
			SvxWriteByte(svx, (unsigned char)s8);
		svx->sourceSamples++;
	}

	return (!svx->noOutput && ferror(svx->fp)) ? -1 : 0;
}

static int SvxEnd(SvxWriter *svx)
{
	unsigned long formSize;
	long endPos;

	if (svx->compression == SVX_COMP_FIBDELTA) {
		if (!svx->fibStarted)
			SvxStartFibDelta(svx, 0);
		if (svx->fibHaveHighNibble) {
			SvxWriteByte(svx, svx->fibPending);
			svx->fibHaveHighNibble = 0;
		}
	}

	if (svx->bodyBytes & 1)
		TimedFputc(0, svx->noOutput ? NULL : svx->fp);

	if (svx->noOutput)
		return 0;

	endPos = ftell(svx->fp);
	formSize = (unsigned long)(endPos - 8);
	PatchU32BE(svx->fp, svx->oneShotPos, svx->sourceSamples);
	PatchU32BE(svx->fp, svx->bodySizePos, svx->bodyBytes);
	PatchU32BE(svx->fp, svx->formSizePos, formSize);

	return ferror(svx->fp) ? -1 : 0;
}

static unsigned long UpdatePcmChecksum(unsigned long checksum, const short *pcm, int nSamps)
{
	int i;

	for (i = 0; i < nSamps; i++) {
		unsigned int sample = (unsigned int)(unsigned short)pcm[i];
		checksum ^= (unsigned long)(sample & 0xffU);
		checksum = (checksum * 16777619UL) & 0xffffffffUL;
		checksum ^= (unsigned long)((sample >> 8) & 0xffU);
		checksum = (checksum * 16777619UL) & 0xffffffffUL;
	}

	return checksum;
}

static void UpdateFirstFrameStats(DecodeStats *stats, const MP3FrameInfo *info)
{
#ifdef MINTAMP_SKIN
	gSkinAudio.channels=info->nChans;
	gSkinAudio.bitrate=info->bitrate/1000;
	gSkinAudio.eq_supported=1;
#endif
	if (!stats->sampleRate && info->samprate)
		stats->sampleRate = info->samprate;
	if (!stats->channels && info->nChans)
		stats->channels = info->nChans;
	if (!stats->bitrate && info->bitrate)
		stats->bitrate = info->bitrate;
}

static int FastLowrateStrideForOutputRate(int outputRate)
{
	if (outputRate == 22050)
		return 2;
	if (outputRate == 14700)
		return 3;
	if (outputRate == 11025)
		return 4;
	return 5;
}

static int FastLowrateActualOutputRate(const DecodeOptions *opt, int inputSampleRate)
{
	int stride;

	if (inputSampleRate <= 0)
		return opt->outputRate;

	stride = FastLowrateStrideForOutputRate(opt->outputRate);
	return inputSampleRate / stride;
}

static int EffectiveOutputSampleRate(const DecodeOptions *opt, int inputSampleRate)
{
	if (inputSampleRate <= 0)
		return opt->outputRate;
	if (opt->fastLowrate)
		return FastLowrateActualOutputRate(opt, inputSampleRate);
	if (!opt->decodeOnly && opt->outputRate && inputSampleRate > opt->outputRate)
		return opt->outputRate;

	return inputSampleRate;
}

static int PlaybackOutputSampleRate(const DecodeOptions *opt, const DecodeStats *stats)
{
	if (stats->outputSampleRate > 0)
		return stats->outputSampleRate;
	if (opt->fastLowrate && stats->sampleRate > 0)
		return FastLowrateActualOutputRate(opt, stats->sampleRate);
	if (opt->outputRate > 0)
		return opt->outputRate;
	return stats->sampleRate;
}

static void PrintFastLowrateOutputRateDifference(const DecodeOptions *opt,
	int actualOutputRate)
{
	if (opt->fastLowrate && opt->outputRate > 0 && actualOutputRate > 0 &&
		actualOutputRate != opt->outputRate) {
		printf("requested output rate: %d Hz\n", opt->outputRate);
		printf("actual fast-lowrate output rate: %d Hz\n", actualOutputRate);
	}
}

static int OutputChannelCount(const DecodeOptions *opt, const DecodeStats *stats)
{
	int outputChannels;

	if (stats->outputChannels > 0)
		return stats->outputChannels;

	if (opt->stereo)
		outputChannels = 2;
	else if (opt->mono)
		outputChannels = 1;
	else
		outputChannels = stats->channels;

	if (outputChannels <= 0)
		outputChannels = 1;

	return outputChannels;
}

static unsigned long PerChannelEmittedSamples(const DecodeOptions *opt,
	const DecodeStats *stats)
{
	int outputChannels;

	outputChannels = OutputChannelCount(opt, stats);
	return outputChannels > 1 ?
		stats->outputSamples / (unsigned long)outputChannels : stats->outputSamples;
}

static unsigned long DecodedAudioMilliseconds(const DecodeOptions *opt,
	const DecodeStats *stats)
{
	int sampleRate;
	unsigned long perChannelSamples;

	if (stats->outputSamples == 0)
		return 0;

	if (opt->fastLowrate)
		sampleRate = PlaybackOutputSampleRate(opt, stats);
	else
		sampleRate = stats->outputSampleRate ?
			stats->outputSampleRate : stats->sampleRate;

	if (sampleRate <= 0)
		return 0;

	perChannelSamples = PerChannelEmittedSamples(opt, stats);
	return IntegerScale((uint32_t)perChannelSamples, (uint32_t)sampleRate, 1000, 0);
}

static void PrintDecodedSeconds(const char *label, const DecodeOptions *opt,
	const DecodeStats *stats)
{
	int rate = opt->fastLowrate ? PlaybackOutputSampleRate(opt, stats) :
		(stats->outputSampleRate ? stats->outputSampleRate : stats->sampleRate);
	PrintIntegerRatio(label, PerChannelEmittedSamples(opt, stats),
		rate > 0 ? (unsigned long)rate : 0, 6, "\n");
}

static void PrintBenchmarkSeconds(const DecodeOptions *opt, const DecodeStats *stats,
	clock_t start, clock_t end)
{
	unsigned long elapsed = end > start && start != (clock_t)-1 && end != (clock_t)-1 ?
		ClocksToMilliseconds((clock_t)((unsigned long)end - (unsigned long)start)) : 0;
	unsigned long audio = DecodedAudioMilliseconds(opt, stats);
	PrintIntegerRatio("elapsed seconds: ", elapsed, 1000, 3, "\n");
	if (elapsed && audio)
		PrintIntegerRatio("decode speed: ", audio, elapsed, 2, "x realtime\n");
}

static void PrintOutputStats(const DecodeOptions *opt, const DecodeStats *stats)
{
	unsigned long perChannelSamples;
	int outputChannels;

	outputChannels = OutputChannelCount(opt, stats);
	perChannelSamples = PerChannelEmittedSamples(opt, stats);

	printf("input channels: %d\n", stats->channels);
	printf("output channels: %d\n", outputChannels);
	printf("total emitted samples: %lu\n", stats->outputSamples);
	printf("per-channel emitted samples: %lu\n", perChannelSamples);
	PrintDecodedSeconds("decoded audio seconds used for realtime calculation: ", opt, stats);
}

static int DownsampleFrame(RateState *rate, const short *in, short *out, int nSamps,
	int inRate, int outRate, int channels)
{
	unsigned long inFrames;
	unsigned long produced;
	unsigned long consume;

	if (outRate <= 0 || outRate >= inRate || channels <= 0) {
		if (out != in)
			memmove(out, in, nSamps * sizeof(short));
		return nSamps;
	}

	if (rate->inRate != inRate || rate->outRate != outRate ||
		rate->channels != channels) {
		rate->inRate = inRate;
		rate->outRate = outRate;
		rate->channels = channels;
		rate->phase = 0;
	}

	inFrames = (unsigned long)(nSamps / channels);
	produced = 0;
	while (rate->phase / (unsigned long)outRate < inFrames) {
		unsigned long srcFrame = rate->phase / (unsigned long)outRate;
		int ch;
		for (ch = 0; ch < channels; ch++)
			out[produced * (unsigned long)channels + (unsigned long)ch] =
				in[srcFrame * (unsigned long)channels + (unsigned long)ch];
		produced++;
		rate->phase += (unsigned long)inRate;
	}
	consume = inFrames * (unsigned long)outRate;
	if (rate->phase >= consume)
		rate->phase -= consume;
	else
		rate->phase = 0;

	return (int)(produced * (unsigned long)channels);
}


static int FastLowrateSelectFrame(int *phase, const short *in, short *out,
	int nSamps, int stride, int channels)
{
	int inFrames;
	int frame;
	int produced;

	if (stride < 2 || channels <= 0) {
		if (out != in)
			memmove(out, in, nSamps * sizeof(short));
		return nSamps;
	}

	inFrames = nSamps / channels;
	produced = 0;
	for (frame = 0; frame < inFrames; frame++) {
		if (*phase == 0) {
			int ch;
			for (ch = 0; ch < channels; ch++)
				out[produced * channels + ch] = in[frame * channels + ch];
			produced++;
		}
		(*phase)++;
		if (*phase >= stride)
			*phase = 0;
	}
	return produced * channels;
}


static int QualitySelftestExpect(const char *name, DecodeOptions opt,
	int expReducedTaps, int expFdct32Quarter, int expImdctThin,
	int expPoly, int expHuff, int expectedQuality)
{
	ApplyQualityOptions(&opt);
	if (opt.quality != expectedQuality ||
		opt.expReducedTaps != expReducedTaps ||
		opt.expFdct32Quarter != expFdct32Quarter ||
		opt.expImdctThin != expImdctThin ||
		opt.expPoly != expPoly || opt.expHuff != expHuff) {
		fprintf(stderr,
			"quality selftest %s mismatch: quality=%d reduced=%d fdct32q=%d imdctThin=%d poly=%d huff=%d\n",
			name, opt.quality, opt.expReducedTaps, opt.expFdct32Quarter,
			opt.expImdctThin, opt.expPoly, opt.expHuff);
		fprintf(stderr,
			"quality selftest %s expected: quality=%d reduced=%d fdct32q=%d imdctThin=%d poly=%d huff=%d\n",
			name, expectedQuality, expReducedTaps, expFdct32Quarter,
			expImdctThin, expPoly, expHuff);
		return -1;
	}
	return 0;
}

static int SelftestQuality(void)
{
	DecodeOptions opt;
	int failures;

	failures = 0;

	memset(&opt, 0, sizeof(opt));
	opt.quality = 0;
	opt.qualitySpecified = 1;
	failures += QualitySelftestExpect("quality0", opt, 1, 1, 0, 1, 1, 0) != 0;

	memset(&opt, 0, sizeof(opt));
	opt.quality = 1;
	opt.qualitySpecified = 1;
	failures += QualitySelftestExpect("quality1", opt, 1, 0, 0, 1, 0, 1) != 0;

	memset(&opt, 0, sizeof(opt));
	opt.quality = 2;
	opt.qualitySpecified = 1;
	failures += QualitySelftestExpect("quality2", opt, 0, 0, 0, 1, 0, 2) != 0;

	memset(&opt, 0, sizeof(opt));
	opt.quality = 3;
	opt.qualitySpecified = 1;
	failures += QualitySelftestExpect("quality3", opt, 0, 0, 0, 0, 0, 3) != 0;

	memset(&opt, 0, sizeof(opt));
	opt.expHuff = 1;
	opt.expFdct32Quarter = 1;
	opt.expReducedTaps = 1;
	opt.expImdctThin = 1;
	opt.expPoly = 1;
	opt.quality = 3;
	opt.qualitySpecified = 1;
	failures += QualitySelftestExpect("quality3-explicit-flags", opt, 1, 1, 1, 1, 1, 3) != 0;

	memset(&opt, 0, sizeof(opt));
	opt.fastLowrate = 1;
	opt.outputRate = 11025;
	failures += QualitySelftestExpect("auto-fast-lowrate-11025", opt, 1, 0, 0, 1, 0, 1) != 0;

	memset(&opt, 0, sizeof(opt));
	opt.fastLowrate = 1;
	opt.outputRate = 22050;
	failures += QualitySelftestExpect("auto-fast-lowrate-22050", opt, 1, 0, 0, 1, 0, 1) != 0;

	memset(&opt, 0, sizeof(opt));
	opt.fastLowrate = 1;
	opt.outputRate = 14700;
	failures += QualitySelftestExpect("auto-fast-lowrate-14700", opt, 1, 0, 0, 1, 0, 1) != 0;

	memset(&opt, 0, sizeof(opt));
	opt.fastLowrate = 1;
	opt.outputRate = 8820;
	failures += QualitySelftestExpect("auto-fast-lowrate-8820", opt, 0, 0, 0, 0, 0, 3) != 0;

	memset(&opt, 0, sizeof(opt));
	failures += QualitySelftestExpect("auto-default", opt, 0, 0, 0, 0, 0, 3) != 0;

	printf("Quality selftest cases: %d\n", 9);
	printf("Quality selftest failures: %d\n", failures);
	if (!failures)
		printf("Quality selftest passed\n");
	return failures ? -1 : 0;
}

static int SelftestStartupVolume(void)
{
	static const int percents[] = { 0, 50, 100 };
	static const UWORD expected[] = { 0, 32, 64 };
	int failures = 0;
	int stereo;
	unsigned int i;

	for (stereo = 0; stereo <= 1; stereo++) {
		for (i = 0; i < sizeof(percents) / sizeof(percents[0]); i++) {
			UWORD mapped = VolumePercentToAudioDevice(percents[i]);
			UWORD request0 = mapped;
			UWORD request1 = mapped;
			if (mapped != expected[i]) {
				fprintf(stderr,
					"startup volume selftest %s %d%% mapped %u expected %u\n",
					stereo ? "stereo" : "mono", percents[i],
					(unsigned int)mapped, (unsigned int)expected[i]);
				failures++;
			}
			if (request0 != mapped || (stereo && request1 != mapped)) {
				fprintf(stderr,
					"startup volume selftest %s %d%% request mismatch ch0=%u ch1=%u mapped=%u\n",
					stereo ? "stereo" : "mono", percents[i],
					(unsigned int)request0, (unsigned int)request1,
					(unsigned int)mapped);
				failures++;
			}
			printf("startup volume selftest: %s %d%% -> ioa_Volume %u%s\n",
				stereo ? "stereo" : "mono", percents[i],
				(unsigned int)mapped,
				stereo ? " on both channel requests" : "");
		}
	}
	printf("startup volume selftest failures: %d\n", failures);
	return failures ? -1 : 0;
}

static int SelftestFastLowrate(void)
{
	enum { CHANNELS = 1, TOTAL_FRAMES = 2304, CHUNK_FRAMES = 576 };
	short input[TOTAL_FRAMES * CHANNELS];
	short normal[TOTAL_FRAMES * CHANNELS];
	short fast[TOTAL_FRAMES * CHANNELS];
	RateState rateState;
	int fastPhase;
	int normalCount;
	int fastCount;
	int offset;
	int i;
	int failures;
	int inSamps;

	for (i = 0; i < TOTAL_FRAMES; i++) {
		input[i] = (short)((i % 257) * 127 - 16384);
		if ((i % 509) == 0)
			input[i] = 30000;
	}

	memset(&rateState, 0, sizeof(rateState));
	fastPhase = 0;
	normalCount = 0;
	fastCount = 0;
	for (offset = 0; offset < TOTAL_FRAMES; offset += CHUNK_FRAMES) {
		inSamps = CHUNK_FRAMES * CHANNELS;
		normalCount += DownsampleFrame(&rateState, input + offset * CHANNELS,
			normal + normalCount, inSamps, 44100, 11025, CHANNELS);
		fastCount += FastLowrateSelectFrame(&fastPhase, input + offset * CHANNELS,
			fast + fastCount, inSamps, 4, CHANNELS);
	}

	failures = 0;
	if (normalCount != fastCount) {
		fprintf(stderr, "fast-lowrate selftest count mismatch: normal=%d fast=%d\n",
			normalCount, fastCount);
		failures++;
	}
	for (i = 0; i < normalCount && i < fastCount; i++) {
		if (normal[i] != fast[i]) {
			fprintf(stderr, "fast-lowrate selftest mismatch at %d: normal=%d fast=%d\n",
				i, normal[i], fast[i]);
			failures++;
			break;
		}
	}
	if (!failures) {
		printf("fast-lowrate selftest passed: stride 4 selects the same positions "
			"as 44100->11025 normal decimation across chunk boundaries (%d samples)\n",
			normalCount);
		printf("note: --rate 8820/8287 uses fixed stride 5; 8287 intentionally differs "
			"from rational 44100->8287 normal --rate positions.\n");
	}

	{
		short normal3[TOTAL_FRAMES * CHANNELS];
		short fast3[TOTAL_FRAMES * CHANNELS];
		int normal3Count;
		int fast3Count;
		int fast3Phase;

		memset(&rateState, 0, sizeof(rateState));
		fast3Phase = 0;
		normal3Count = 0;
		fast3Count = 0;
		for (offset = 0; offset < TOTAL_FRAMES; offset += CHUNK_FRAMES) {
			inSamps = CHUNK_FRAMES * CHANNELS;
			normal3Count += DownsampleFrame(&rateState, input + offset * CHANNELS,
				normal3 + normal3Count, inSamps, 44100, 14700, CHANNELS);
			fast3Count += FastLowrateSelectFrame(&fast3Phase, input + offset * CHANNELS,
				fast3 + fast3Count, inSamps, 3, CHANNELS);
		}
		if (normal3Count != fast3Count) {
			fprintf(stderr, "fast-lowrate selftest stride3 count mismatch: normal=%d fast=%d\n",
				normal3Count, fast3Count);
			failures++;
		}
		for (i = 0; i < normal3Count && i < fast3Count; i++) {
			if (normal3[i] != fast3[i]) {
				fprintf(stderr, "fast-lowrate selftest stride3 mismatch at %d: normal=%d fast=%d\n",
					i, normal3[i], fast3[i]);
				failures++;
				break;
			}
		}
		if (i == normal3Count && normal3Count == fast3Count)
			printf("fast-lowrate selftest passed: stride 3 selects the same positions "
				"as 44100->14700 normal decimation across chunk boundaries (%d samples)\n",
				normal3Count);
	}

	return failures ? 1 : 0;
}



static int gSelftestVerbose;
static int gSelftestFdct32HalfDebug;

static const char *DescribeFdct32HalfDest(int destIndex, int offset, int oddBlock, int *row, const char **expr)
{
	int oddBase = oddBlock ? AMIGA_POLYPHASE_VBUF_LENGTH : 0;
	int evenBase = oddBlock ? 0 : AMIGA_POLYPHASE_VBUF_LENGTH;
	int delayOff = (offset - oddBlock) & 7;
	int highBase = offset + oddBase;
	int lowBase = 16 + delayOff + evenBase;
	static const char *highExpr[8] = {
		"buf[1]", "buf[9]+buf[13]", "buf[5]", "buf[13]+buf[11]",
		"buf[3]", "buf[11]+buf[15]", "buf[7]", "buf[15]"
	};
	static const char *lowExpr[8] = {
		"buf[1]", "buf[14]+buf[9]", "buf[6]", "buf[10]+buf[14]",
		"buf[2]", "buf[12]+buf[10]", "buf[4]", "buf[8]+buf[12]"
	};
	int k;
	if (destIndex == 64 * 16 + delayOff + evenBase || destIndex == 64 * 16 + delayOff + evenBase + 8) {
		*row = 0;
		*expr = "buf[0]";
		return "CENTRE";
	}
	for (k = 0; k < 8; k++) {
		if (destIndex == highBase + 128 * k || destIndex == highBase + 128 * k + 8) {
			*row = 16 + 2 * k;
			*expr = highExpr[k];
			return "HIGH";
		}
		if (destIndex == lowBase + 128 * k || destIndex == lowBase + 128 * k + 8) {
			*row = 16 - 2 * k;
			*expr = lowExpr[k];
			return "LOW";
		}
	}
	*row = -1;
	*expr = "unmapped destination";
	return "UNKNOWN";
}

static void PrintFdct32HalfDebug(unsigned long index, int destIndex, int offset, int oddBlock, int gb,
	const int *cbuf, const int *hbuf, const int *xbuf, int fullValue, int cHalfValue, int asmHalfValue)
{
	int row;
	const char *expr;
	const char *lane = DescribeFdct32HalfDest(destIndex, offset, oddBlock, &row, &expr);
	printf("FDCT32Half debug first mismatch: case=%lu offset=%d oddBlock=%d gb=%d dest=%d lane=%s row=%d\n",
		index, offset, oddBlock, gb, destIndex, lane, row);
	printf("  C half=%ld asm half=%ld full FDCT32=%ld dependency=%s\n",
		(long)cHalfValue, (long)asmHalfValue, (long)fullValue, expr);
	printf("  C intermediate buf[0..15]:");
	for (row = 0; row < 16; row++) printf(" %ld", (long)hbuf[row]);
	printf("\n  asm intermediate buf[0..15]:");
	for (row = 0; row < 16; row++) printf(" %ld", (long)xbuf[row]);
	printf("\n  full intermediate buf[0..31]:");
	for (row = 0; row < 32; row++) printf(" %ld", (long)cbuf[row]);
	printf("\n");
}

static int TestFdct32Case(unsigned long index, unsigned long seed, int offset,
	int oddBlock, int gb)
{
	static int cbuf[32];
	static int abuf[32];
	static int hbuf[32];
	static int cdest[4096];
	static int adest[4096];
	static int hdest[4096];
	static int xbuf[32];
	static int xdest[4096];
	int i;
	int halfWrites;
	int failed;

	failed = 0;
	for (i = 0; i < 32; i++) {
		seed = seed * 1664525UL + 1013904223UL;
		cbuf[i] = ((int)seed) >> 8;
		abuf[i] = cbuf[i];
		hbuf[i] = cbuf[i];
		xbuf[i] = cbuf[i];
	}
	for (i = 0; i < 4096; i++) {
		cdest[i] = (int)(0x55aa0000UL ^ (unsigned long)i);
		adest[i] = cdest[i];
		hdest[i] = cdest[i];
		xdest[i] = cdest[i];
	}

	AMIGA_FDCT32_C_REFERENCE(cbuf, cdest, offset, oddBlock, gb);
	AMIGA_FDCT32(abuf, adest, offset, oddBlock, gb);
	AMIGA_FDCT32_HALF(hbuf, hdest, offset, oddBlock, gb);
	AMIGA_FDCT32_HALF_TEST_ACTIVE(xbuf, xdest, offset, oddBlock, gb);

	for (i = 0; i < 32; i++) {
		if (abuf[i] != cbuf[i]) {
			printf("FDCT32 buffer mismatch %lu[%d]: C=%ld asm=%ld offset=%d odd=%d gb=%d\n",
				index, i, (long)cbuf[i], (long)abuf[i], offset, oddBlock, gb);
			failed = 1;
			if (!gSelftestVerbose) return -1;
		}
	}
	for (i = 0; i < 4096; i++) {
		if (adest[i] != cdest[i]) {
			printf("FDCT32 dest mismatch %lu[%d]: C=%ld asm=%ld offset=%d odd=%d gb=%d\n",
				index, i, (long)cdest[i], (long)adest[i], offset, oddBlock, gb);
			failed = 1;
			if (!gSelftestVerbose) return -1;
		}
	}
	halfWrites = 0;
	for (i = 0; i < 4096; i++) {
		if (hdest[i] != (int)(0x55aa0000UL ^ (unsigned long)i)) {
			halfWrites++;
			if (hdest[i] != cdest[i]) {
				printf("FDCT32 half dest mismatch %lu[%d]: full=%ld half=%ld offset=%d odd=%d gb=%d\n",
					index, i, (long)cdest[i], (long)hdest[i], offset, oddBlock, gb);
				failed = 1;
				if (!gSelftestVerbose) return -1;
			}
		}
	}
	for (i = 0; i < 4096; i++) {
		if (xdest[i] != hdest[i]) {
			printf("FDCT32 half asm dest mismatch %lu[%d]: C=%ld asm=%ld offset=%d odd=%d gb=%d\n",
				index, i, (long)hdest[i], (long)xdest[i], offset, oddBlock, gb);
			if (gSelftestFdct32HalfDebug)
				PrintFdct32HalfDebug(index, i, offset, oddBlock, gb, cbuf, hbuf, xbuf, cdest[i], hdest[i], xdest[i]);
			failed = 1;
			if (!gSelftestVerbose) return -1;
		}
	}
	if (halfWrites != 34) {
		printf("FDCT32 half write count mismatch %lu: got=%d expected=34 offset=%d odd=%d gb=%d\n",
			index, halfWrites, offset, oddBlock, gb);
		failed = 1;
		if (!gSelftestVerbose) return -1;
	}
	return failed ? -1 : 0;
}

static int SelftestFdct32Half(void);

static int SelftestFdct32(void)
{
	unsigned long i;
	unsigned long failures;
	unsigned long seed;

	failures = 0;
	seed = 0x31415926UL;
	for (i = 0; i < 4096UL; i++) {
		seed = seed * 1664525UL + 1013904223UL;
		if (TestFdct32Case(i, seed, (int)(seed & 7), (int)((seed >> 3) & 1),
			(int)((seed >> 4) % 8)) != 0)
			failures++;
	}

	printf("FDCT32 asm requested: %s\n",
#ifdef AMIGA_M68K_ASM_FDCT32
		"yes"
#else
		"no"
#endif
	);
	printf("FDCT32 asm active: %s\n", AMIGA_FDCT32_HAS_ASM() ? "yes" : "no");
	printf("FDCT32 selftest cases: %lu\n", i);
	printf("FDCT32 selftest failures: %lu\n", failures);
	return failures ? 1 : 0;
}


static int SelftestFdct32Half(void)
{
	unsigned long i;
	unsigned long failures;
	unsigned long seed;

	failures = 0;
	seed = 0x32483248UL;
	i = 0;
	{
		int offset, oddBlock, gb, variant;
		for (variant = 0; variant < 32; variant++) {
			for (gb = 0; gb < 8; gb++) {
				for (oddBlock = 0; oddBlock < 2; oddBlock++) {
					for (offset = 0; offset < 8; offset++) {
						seed = seed * 1664525UL + 1013904223UL;
						if (TestFdct32Case(i, seed, offset, oddBlock, gb) != 0)
							failures++;
						i++;
					}
				}
			}
		}
	}

	printf("FDCT32Half asm requested: %s\n",
#if defined(AMIGA_M68K_ASM_FDCT32) && !defined(AMIGA_FORCE_FDCT32_HALF_C)
		"yes"
#else
		"no"
#endif
	);
	printf("FDCT32Half asm active: %s\n", AMIGA_FDCT32_HALF_HAS_ASM() ? "yes" : "no");
	printf("FDCT32Half selftest cases: %lu\n", i);
	printf("FDCT32Half selftest failures: %lu\n", failures);
	return failures ? 1 : 0;
}


static void FillImdctSentinel(int *y)
{
	int i;
	for (i = 0; i < AMIGA_IMDCT_BLOCK_SIZE * AMIGA_IMDCT_NBANDS; i++)
		y[i] = (int)(0x13570000UL ^ (unsigned long)i);
}

static int TestImdctCase(unsigned long index, int pattern, unsigned long seed,
	int btCurr, int btPrev, int blockIdx, int gb)
{
	static int cx[18];
	static int ax[18];
	static int cp[9];
	static int ap[9];
	static int cy[AMIGA_IMDCT_BLOCK_SIZE * AMIGA_IMDCT_NBANDS];
	static int ay[AMIGA_IMDCT_BLOCK_SIZE * AMIGA_IMDCT_NBANDS];
	int i;
	int cm;
	int am;

	for (i = 0; i < 18; i++) {
		seed = seed * 1664525UL + 1013904223UL;
		if (pattern == 0)
			cx[i] = 0;
		else if (pattern == 1)
			cx[i] = ((int)seed) >> 10;
		else
			cx[i] = (i & 1) ? 0x03ffffff : (int)0xfc000000UL;
		ax[i] = cx[i];
	}
	for (i = 0; i < 9; i++) {
		seed = seed * 1664525UL + 1013904223UL;
		if (pattern == 0)
			cp[i] = 0;
		else if (pattern == 1)
			cp[i] = ((int)seed) >> 12;
		else
			cp[i] = (i & 1) ? 0x01ffffff : (int)0xfe000000UL;
		ap[i] = cp[i];
	}
	FillImdctSentinel(cy);
	FillImdctSentinel(ay);

	cm = AMIGA_IMDCT36_C_REFERENCE(cx, cp, cy + blockIdx, btCurr, btPrev, blockIdx, gb);
	am = AMIGA_IMDCT36_TEST_ACTIVE(ax, ap, ay + blockIdx, btCurr, btPrev, blockIdx, gb);
	if (am != cm) {
		printf("IMDCT36 mOut mismatch %lu: first=%ld second=%ld btCurr=%d btPrev=%d block=%d gb=%d\n",
			index, (long)cm, (long)am, btCurr, btPrev, blockIdx, gb);
		return -1;
	}
	for (i = 0; i < 18; i++) {
		if (ax[i] != cx[i]) {
			printf("IMDCT36 input mismatch %lu[%d]: first=%ld second=%ld btCurr=%d btPrev=%d block=%d gb=%d\n",
				index, i, (long)cx[i], (long)ax[i], btCurr, btPrev, blockIdx, gb);
			return -1;
		}
	}
	for (i = 0; i < 9; i++) {
		if (ap[i] != cp[i]) {
			printf("IMDCT36 overlap mismatch %lu[%d]: first=%ld second=%ld btCurr=%d btPrev=%d block=%d gb=%d\n",
				index, i, (long)cp[i], (long)ap[i], btCurr, btPrev, blockIdx, gb);
			return -1;
		}
	}
	for (i = 0; i < AMIGA_IMDCT_BLOCK_SIZE * AMIGA_IMDCT_NBANDS; i++) {
		if (ay[i] != cy[i]) {
			printf("IMDCT36 output mismatch %lu[%d]: first=%ld second=%ld btCurr=%d btPrev=%d block=%d gb=%d\n",
				index, i, (long)cy[i], (long)ay[i], btCurr, btPrev, blockIdx, gb);
			return -1;
		}
	}
	return 0;
}

static int TestAntialiasCase(unsigned long index, unsigned long seed, int pattern, int nBfly)
{
	static int cx[AMIGA_IMDCT_BLOCK_SIZE * AMIGA_IMDCT_NBANDS];
	static int ax[AMIGA_IMDCT_BLOCK_SIZE * AMIGA_IMDCT_NBANDS];
	int i;
	int nSamps;

	nSamps = (nBfly + 1) * AMIGA_IMDCT_BLOCK_SIZE;
	for (i = 0; i < nSamps; i++) {
		seed = seed * 1664525UL + 1013904223UL;
		if (pattern == 0)
			cx[i] = 0;
		else if (pattern == 1)
			cx[i] = ((int)seed) >> 7;
		else
			cx[i] = (i & 1) ? 0x03ffffff : (int)0xfc000000UL;
		ax[i] = cx[i];
	}

	AMIGA_ANTIALIAS_C_REFERENCE(cx, nBfly);
	AMIGA_ANTIALIAS_TEST_ACTIVE(ax, nBfly);

	for (i = 0; i < nSamps; i++) {
		if (ax[i] != cx[i]) {
			printf("AntiAlias mismatch %lu[%d]: first=%ld second=%ld nBfly=%d pattern=%d\n",
				index, i, (long)cx[i], (long)ax[i], nBfly, pattern);
			return -1;
		}
	}
	return 0;
}

static int SelftestAntialias(void)
{
	unsigned long i;
	unsigned long failures;
	unsigned long seed;
	int pattern;
	int nBfly;

	failures = 0;
	seed = 0x0aa51aa5UL;
	for (i = 0; i < 4096UL; i++) {
		seed = seed * 1664525UL + 1013904223UL;
		pattern = (i < 16UL) ? 0 : ((i < 32UL) ? 2 : 1);
		nBfly = (int)(seed % AMIGA_IMDCT_NBANDS);
		if (TestAntialiasCase(i, seed, pattern, nBfly) != 0)
			failures++;
	}

	printf("AntiAlias asm requested: %s\n",
#ifdef AMIGA_M68K_ASM_ANTIALIAS
		"yes"
#else
		"no"
#endif
	);
	printf("AntiAlias asm active: %s\n", AMIGA_ANTIALIAS_HAS_ASM() ? "yes" : "no");
	printf("AntiAlias selftest cases: %lu\n", 4096UL);
	printf("AntiAlias selftest failures: %lu\n", failures);
	return failures ? 1 : 0;
}


static int SelftestImdct(void)
{
	unsigned long i;
	unsigned long failures;
	unsigned long seed;
	unsigned long fallbackCases;
	int pattern;
	int btCurr;
	int btPrev;
	int blockIdx;
	int gb;
	
	failures = 0;
	fallbackCases = 0;
	seed = 0x27182818UL;

	/* Zero, edge-value, and deterministic random long-block cases. */
	for (i = 0; i < 4096UL; i++) {
		seed = seed * 1664525UL + 1013904223UL;
		pattern = (i < 16UL) ? 0 : ((i < 32UL) ? 2 : 1);
		btCurr = 0;
		btPrev = 0;
		blockIdx = (int)((seed >> 8) & 31);
		gb = (int)((seed >> 13) % 8);
		if (TestImdctCase(i, pattern, seed, btCurr, btPrev, blockIdx, gb) != 0)
			failures++;
	}

	/* Non-common long windows, and the block types used around mixed/short transitions,
	 * now route through IMDCT36_GeneralWindow (shared between the C reference and the
	 * m68k asm general path) rather than a full C fallback -- this loop's coverage
	 * doubles as a live-hardware check on that path (see IMDCT36AsmGeneralPathSelftest
	 * for the dedicated, larger-trial-count version of the same comparison).
	 */
	for (i = 0; i < 256UL; i++) {
		seed = seed * 1664525UL + 1013904223UL;
		pattern = (int)(seed % 3UL);
		/* Cycle through every non-common current/previous window pair. */
		btCurr = (int)((i >> 2) & 3UL);
		btPrev = (int)(i & 3UL);
		if (btCurr == 0 && btPrev == 0)
			btPrev = 1;
		blockIdx = (int)((seed >> 10) & 31);
		gb = (int)((seed >> 15) % 8);
		if (TestImdctCase(4096UL + i, pattern, seed, btCurr, btPrev, blockIdx, gb) != 0)
			failures++;
		fallbackCases++;
	}

	printf("IMDCT asm requested: %s\n",
#ifdef AMIGA_M68K_ASM_IMDCT
		"yes"
#else
		"no"
#endif
	);
	printf("IMDCT asm active: %s\n", AMIGA_IMDCT36_HAS_ASM() ? "yes" : "no");
	printf("IMDCT selftest long cases: %lu\n", 4096UL);
	printf("IMDCT selftest fallback cases: %lu\n", fallbackCases);
	printf("IMDCT selftest failures: %lu\n", failures);
	return failures ? 1 : 0;
}


static int TestPolyphaseCase(unsigned long index, unsigned long seed, int pattern)
{
	static int cvbuf[AMIGA_POLYPHASE_VBUF_LENGTH];
	static int avbuf[AMIGA_POLYPHASE_VBUF_LENGTH];
	static short cpcm[AMIGA_POLYPHASE_NBANDS];
	static short apcm[AMIGA_POLYPHASE_NBANDS];
	int i;

	for (i = 0; i < AMIGA_POLYPHASE_VBUF_LENGTH; i++) {
		seed = seed * 1664525UL + 1013904223UL;
		if (pattern == 0)
			cvbuf[i] = 0;
		else if (pattern == 1)
			cvbuf[i] = ((int)seed) >> 9;
		else
			cvbuf[i] = (i & 1) ? 0x03ffffff : (int)0xfc000000UL;
		avbuf[i] = cvbuf[i];
	}
	for (i = 0; i < AMIGA_POLYPHASE_NBANDS; i++) {
		cpcm[i] = (short)(0x6000 + i);
		apcm[i] = (short)(0x6000 + i);
	}

	AMIGA_POLYPHASE_MONO_FAST_C_REFERENCE(cpcm, cvbuf, AMIGA_POLY_COEF);
	AMIGA_POLYPHASE_MONO_FAST_TEST_ACTIVE(apcm, avbuf, AMIGA_POLY_COEF);

	for (i = 0; i < AMIGA_POLYPHASE_VBUF_LENGTH; i++) {
		if (avbuf[i] != cvbuf[i]) {
			printf("PolyphaseMonoFast vbuf mismatch %lu[%d]: first=%ld second=%ld pattern=%d\n",
				index, i, (long)cvbuf[i], (long)avbuf[i], pattern);
			return -1;
		}
	}
	for (i = 0; i < AMIGA_POLYPHASE_NBANDS; i++) {
		if (apcm[i] != cpcm[i]) {
			printf("PolyphaseMonoFast output mismatch %lu[%d]: first=%ld second=%ld pattern=%d\n",
				index, i, (long)cpcm[i], (long)apcm[i], pattern);
			return -1;
		}
	}
	return 0;
}

static int SelftestPolyphase(void)
{
	unsigned long i;
	unsigned long failures;
	unsigned long seed;
	int pattern;

	failures = 0;
	seed = 0x16180339UL;
	for (i = 0; i < 4096UL; i++) {
		seed = seed * 1664525UL + 1013904223UL;
		pattern = (i < 16UL) ? 0 : ((i < 32UL) ? 2 : 1);
		if (TestPolyphaseCase(i, seed, pattern) != 0)
			failures++;
	}

	printf("Polyphase asm requested: %s\n",
#ifdef AMIGA_M68K_ASM_POLYPHASE
		"yes"
#else
		"no"
#endif
	);
	printf("Polyphase asm active: %s\n", AMIGA_POLYPHASE_MONO_FAST_HAS_ASM() ? "yes" : "no");
	printf("Polyphase selftest cases: %lu\n", i);
	printf("Polyphase selftest failures: %lu\n", failures);
	return failures ? 1 : 0;
}

static int TestPolyphaseStride2Case(unsigned long index, unsigned long seed, int pattern)
{
	static int cvbuf[AMIGA_POLYPHASE_VBUF_LENGTH];
	static int avbuf[AMIGA_POLYPHASE_VBUF_LENGTH];
	static short cpcm[AMIGA_POLYPHASE_NBANDS];
	static short apcm[AMIGA_POLYPHASE_NBANDS];
	int ccount;
	int acount;
	int i;

	for (i = 0; i < AMIGA_POLYPHASE_VBUF_LENGTH; i++) {
		seed = seed * 1664525UL + 1013904223UL;
		if (pattern == 0)
			cvbuf[i] = 0;
		else if (pattern == 1)
			cvbuf[i] = ((int)seed) >> 9;
		else
			cvbuf[i] = (i & 1) ? 0x03ffffff : (int)0xfc000000UL;
		avbuf[i] = cvbuf[i];
	}
	for (i = 0; i < AMIGA_POLYPHASE_NBANDS; i++) {
		cpcm[i] = (short)(0x7000 + i);
		apcm[i] = (short)(0x7000 + i);
	}

	ccount = AMIGA_POLYPHASE_MONO_FAST_STRIDE2_C_REFERENCE(cpcm, cvbuf, AMIGA_POLY_COEF);
	acount = AMIGA_POLYPHASE_MONO_FAST_STRIDE2_TEST_ACTIVE(apcm, avbuf, AMIGA_POLY_COEF);

	if (ccount != 16 || acount != 16) {
		printf("PolyphaseMonoFast stride2 count mismatch %lu: first=%d second=%d pattern=%d\n",
			index, ccount, acount, pattern);
		return -1;
	}
	for (i = 0; i < AMIGA_POLYPHASE_VBUF_LENGTH; i++) {
		if (avbuf[i] != cvbuf[i]) {
			printf("PolyphaseMonoFast stride2 vbuf mismatch %lu[%d]: first=%ld second=%ld pattern=%d\n",
				index, i, (long)cvbuf[i], (long)avbuf[i], pattern);
			return -1;
		}
	}
	for (i = 0; i < AMIGA_POLYPHASE_NBANDS; i++) {
		if (apcm[i] != cpcm[i]) {
			printf("PolyphaseMonoFast stride2 output mismatch %lu[%d]: first=%ld second=%ld pattern=%d\n",
				index, i, (long)cpcm[i], (long)apcm[i], pattern);
			return -1;
		}
	}
	return 0;
}

static int SelftestPolyphaseStride2(void)
{
	unsigned long i;
	unsigned long failures;
	unsigned long seed;
	int pattern;

	failures = 0;
	seed = 0x27182818UL;
	for (i = 0; i < 4096UL; i++) {
		seed = seed * 1664525UL + 1013904223UL;
		pattern = (i < 16UL) ? 0 : ((i < 32UL) ? 2 : 1);
		if (TestPolyphaseStride2Case(i, seed, pattern) != 0)
			failures++;
	}

	printf("Polyphase stride2 asm requested: %s\n",
#ifdef AMIGA_M68K_ASM_POLYPHASE
		"yes"
#else
		"no"
#endif
	);
	printf("Polyphase stride2 asm active: %s\n",
		AMIGA_POLYPHASE_MONO_FAST_STRIDE2_HAS_ASM() ? "yes" : "no");
	printf("Polyphase stride2 selftest cases: %lu\n", i);
	printf("Polyphase stride2 selftest failures: %lu\n", failures);
	return failures ? 1 : 0;
}


static int TestPolyphaseStride2ReducedCase(unsigned long index, unsigned long seed, int pattern)
{
	static int cvbuf[AMIGA_POLYPHASE_VBUF_LENGTH];
	static int avbuf[AMIGA_POLYPHASE_VBUF_LENGTH];
	static short cpcm[AMIGA_POLYPHASE_NBANDS];
	static short apcm[AMIGA_POLYPHASE_NBANDS];
	int ccount;
	int acount;
	int i;

	for (i = 0; i < AMIGA_POLYPHASE_VBUF_LENGTH; i++) {
		seed = seed * 1664525UL + 1013904223UL;
		if (pattern == 0)
			cvbuf[i] = 0;
		else if (pattern == 1)
			cvbuf[i] = ((int)seed) >> 9;
		else
			cvbuf[i] = (i & 1) ? 0x03ffffff : (int)0xfc000000UL;
		avbuf[i] = cvbuf[i];
	}
	for (i = 0; i < AMIGA_POLYPHASE_NBANDS; i++) {
		cpcm[i] = (short)(0x7000 + i);
		apcm[i] = (short)(0x7000 + i);
	}

	ccount = AMIGA_POLYPHASE_MONO_FAST_STRIDE2_REDUCED_C_REFERENCE(cpcm, cvbuf, AMIGA_POLY_COEF);
	acount = AMIGA_POLYPHASE_MONO_FAST_STRIDE2_REDUCED_TEST_ACTIVE(apcm, avbuf, AMIGA_POLY_COEF);

	if (ccount != 16 || acount != 16) {
		printf("PolyphaseMonoFast stride2 reduced count mismatch %lu: first=%d second=%d pattern=%d\n",
			index, ccount, acount, pattern);
		return -1;
	}
	for (i = 0; i < AMIGA_POLYPHASE_VBUF_LENGTH; i++) {
		if (avbuf[i] != cvbuf[i]) {
			printf("PolyphaseMonoFast stride2 reduced vbuf mismatch %lu[%d]: first=%ld second=%ld pattern=%d\n",
				index, i, (long)cvbuf[i], (long)avbuf[i], pattern);
			return -1;
		}
	}
	for (i = 0; i < AMIGA_POLYPHASE_NBANDS; i++) {
		if (apcm[i] != cpcm[i]) {
			printf("PolyphaseMonoFast stride2 reduced output mismatch %lu[%d]: first=%ld second=%ld pattern=%d\n",
				index, i, (long)cpcm[i], (long)apcm[i], pattern);
			return -1;
		}
	}
	return 0;
}

static int SelftestPolyphaseStride2Reduced(void)
{
	unsigned long i;
	unsigned long failures;
	unsigned long seed;
	int pattern;

	failures = 0;
	seed = 0x27182818UL;
	for (i = 0; i < 4096UL; i++) {
		seed = seed * 1664525UL + 1013904223UL;
		pattern = (i < 16UL) ? 0 : ((i < 32UL) ? 2 : 1);
		if (TestPolyphaseStride2ReducedCase(i, seed, pattern) != 0)
			failures++;
	}

	printf("Polyphase stride2 reduced asm requested: %s\n",
#ifdef AMIGA_M68K_ASM_POLYPHASE
		"yes"
#else
		"no"
#endif
	);
	printf("Polyphase stride2 reduced asm active: %s\n",
		AMIGA_POLYPHASE_MONO_FAST_STRIDE2_REDUCED_HAS_ASM() ? "yes" : "no");
	printf("Polyphase stride2 reduced selftest cases: %lu\n", i);
	printf("Polyphase stride2 reduced selftest failures: %lu\n", failures);
	return failures ? 1 : 0;
}

static int TestPolyphaseStride4Case(unsigned long index, unsigned long seed, int pattern)
{
	static int cvbuf[AMIGA_POLYPHASE_VBUF_LENGTH];
	static int avbuf[AMIGA_POLYPHASE_VBUF_LENGTH];
	static short cpcm[AMIGA_POLYPHASE_NBANDS];
	static short apcm[AMIGA_POLYPHASE_NBANDS];
	int ccount;
	int acount;
	int i;

	for (i = 0; i < AMIGA_POLYPHASE_VBUF_LENGTH; i++) {
		seed = seed * 1664525UL + 1013904223UL;
		if (pattern == 0)
			cvbuf[i] = 0;
		else if (pattern == 1)
			cvbuf[i] = ((int)seed) >> 9;
		else
			cvbuf[i] = (i & 1) ? 0x03ffffff : (int)0xfc000000UL;
		avbuf[i] = cvbuf[i];
	}
	for (i = 0; i < AMIGA_POLYPHASE_NBANDS; i++) {
		cpcm[i] = (short)(0x7100 + i);
		apcm[i] = (short)(0x7100 + i);
	}

	ccount = AMIGA_POLYPHASE_MONO_FAST_STRIDE4_C_REFERENCE(cpcm, cvbuf, AMIGA_POLY_COEF);
	acount = AMIGA_POLYPHASE_MONO_FAST_STRIDE4_TEST_ACTIVE(apcm, avbuf, AMIGA_POLY_COEF);

	if (ccount != 8 || acount != 8) {
		printf("PolyphaseMonoFast stride4 count mismatch %lu: first=%d second=%d pattern=%d\n",
			index, ccount, acount, pattern);
		return -1;
	}
	for (i = 0; i < AMIGA_POLYPHASE_VBUF_LENGTH; i++) {
		if (avbuf[i] != cvbuf[i]) {
			printf("PolyphaseMonoFast stride4 vbuf mismatch %lu[%d]: first=%ld second=%ld pattern=%d\n",
				index, i, (long)cvbuf[i], (long)avbuf[i], pattern);
			return -1;
		}
	}
	for (i = 0; i < AMIGA_POLYPHASE_NBANDS; i++) {
		if (apcm[i] != cpcm[i]) {
			printf("PolyphaseMonoFast stride4 output mismatch %lu[%d]: first=%ld second=%ld pattern=%d\n",
				index, i, (long)cpcm[i], (long)apcm[i], pattern);
			return -1;
		}
	}
	return 0;
}

static int SelftestPolyphaseStride4(void)
{
	unsigned long i;
	unsigned long failures;
	unsigned long seed;
	int pattern;

	failures = 0;
	seed = 0x31415926UL;
	for (i = 0; i < 500UL; i++) {
		seed = seed * 1664525UL + 1013904223UL;
		pattern = (i < 8UL) ? 0 : ((i < 16UL) ? 2 : 1);
		if (TestPolyphaseStride4Case(i, seed, pattern) != 0)
			failures++;
	}

	printf("Polyphase stride4 asm requested: %s\n",
#ifdef AMIGA_M68K_ASM_POLYPHASE
		"yes"
#else
		"no"
#endif
	);
	printf("Polyphase stride4 asm active: %s\n",
		AMIGA_POLYPHASE_MONO_FAST_STRIDE4_HAS_ASM() ? "yes" : "no");
	printf("Polyphase stride4 selftest cases: %lu\n", i);
	printf("Polyphase stride4 selftest failures: %lu\n", failures);
	return failures ? 1 : 0;
}

static int TestPolyphaseStride4StereoCase(unsigned long index, unsigned long seed, int pattern, int phase)
{
	static int cvbuf[AMIGA_POLYPHASE_VBUF_LENGTH];
	static int avbuf[AMIGA_POLYPHASE_VBUF_LENGTH];
	static short cpcm[AMIGA_POLYPHASE_NBANDS * 2];
	static short apcm[AMIGA_POLYPHASE_NBANDS * 2];
	int ccount;
	int acount;
	int i;
	int lane;

	for (i = 0; i < AMIGA_POLYPHASE_VBUF_LENGTH; i++) {
		seed = seed * 1664525UL + 1013904223UL;
		if (pattern == 0) {
			cvbuf[i] = 0;
		} else if (pattern == 1) {
			cvbuf[i] = ((int)seed) >> 9;
		} else if (pattern == 2) {
			cvbuf[i] = (i & 1) ? 0x03ffffff : (int)0xfc000000UL;
		} else if (pattern == 3) {
			cvbuf[i] = (i == (int)((index + (unsigned long)phase * 37UL) %
				AMIGA_POLYPHASE_VBUF_LENGTH)) ? 0x02000000 : 0;
		} else {
			/* Left/right asymmetric stereo addressing check: every 64-int
			 * block stores 32 left entries followed by 32 right entries.
			 */
			lane = i & 63;
			if (lane < 32)
				cvbuf[i] = (int)(0x01000000 + ((i * 97) & 0x000fffff));
			else
				cvbuf[i] = (int)(0xff000000UL + ((i * 193) & 0x000fffff));
		}
		avbuf[i] = cvbuf[i];
	}
	for (i = 0; i < AMIGA_POLYPHASE_NBANDS * 2; i++) {
		cpcm[i] = (short)(0x7200 + i);
		apcm[i] = (short)(0x7200 + i);
	}

	ccount = AMIGA_POLYPHASE_STEREO_FAST_STRIDE4_C_REFERENCE(cpcm, cvbuf, AMIGA_POLY_COEF, phase);
	acount = AMIGA_POLYPHASE_STEREO_FAST_STRIDE4_TEST_ACTIVE(apcm, avbuf, AMIGA_POLY_COEF, phase);

	if (ccount != 16 || acount != 16) {
		printf("PolyphaseStereoFast stride4 count mismatch %lu phase=%d: first=%d second=%d pattern=%d\n",
			index, phase, ccount, acount, pattern);
		return -1;
	}
	for (i = 0; i < AMIGA_POLYPHASE_VBUF_LENGTH; i++) {
		if (avbuf[i] != cvbuf[i]) {
			printf("PolyphaseStereoFast stride4 vbuf mismatch %lu phase=%d[%d]: first=%ld second=%ld pattern=%d\n",
				index, phase, i, (long)cvbuf[i], (long)avbuf[i], pattern);
			return -1;
		}
	}
	for (i = 0; i < AMIGA_POLYPHASE_NBANDS * 2; i++) {
		if (apcm[i] != cpcm[i]) {
			printf("PolyphaseStereoFast stride4 output mismatch %lu phase=%d[%d]: first=%ld second=%ld pattern=%d\n",
				index, phase, i, (long)cpcm[i], (long)apcm[i], pattern);
			return -1;
		}
	}
	return 0;
}

#if defined(AMIGA_M68K) && defined(AMIGA_M68K_ASM_POLYPHASE)
static void InitPolyphaseStride4StereoDiagnosticVector(int *vbuf, short *pcm,
	unsigned long seed, int pattern, int phase)
{
	int i;
	int lane;

	for (i = 0; i < AMIGA_POLYPHASE_VBUF_LENGTH; i++) {
		seed = seed * 1664525UL + 1013904223UL;
		if (pattern == 0)
			vbuf[i] = 0;
		else if (pattern == 1)
			vbuf[i] = ((int)seed) >> 9;
		else if (pattern == 2)
			vbuf[i] = (i & 1) ? 0x03ffffff : (int)0xfc000000UL;
		else if (pattern == 3)
			vbuf[i] = (i == (phase * 41 + 17)) ? 0x02000000 : 0;
		else {
			lane = i & 63;
			vbuf[i] = (lane < 32) ?
				(int)(0x01000000 + ((i * 97) & 0x000fffff)) :
				(int)(0xff000000UL + ((i * 193) & 0x000fffff));
		}
	}
	for (i = 0; i < AMIGA_POLYPHASE_NBANDS * 2; i++)
		pcm[i] = (short)(0x7300 + i);
}

static int PolyphaseStride4StereoDiagnosticMatches(const short *refPcm,
	const int *refVbuf, int refCount, const short *testPcm,
	const int *testVbuf, int testCount)
{
	int i;

	if (testCount != refCount)
		return 0;
	for (i = 0; i < AMIGA_POLYPHASE_NBANDS * 2; i++) {
		if (testPcm[i] != refPcm[i])
			return 0;
	}
	for (i = 0; i < AMIGA_POLYPHASE_VBUF_LENGTH; i++) {
		if (testVbuf[i] != refVbuf[i])
			return 0;
	}
	return 1;
}

static void SelftestPolyphaseStride4StereoKernelMapping(void)
{
	static int baseVbuf[AMIGA_POLYPHASE_VBUF_LENGTH];
	static int refVbuf[AMIGA_POLYPHASE_VBUF_LENGTH];
	static int kernelVbuf[4][AMIGA_POLYPHASE_VBUF_LENGTH];
	static int dispatcherVbuf[AMIGA_POLYPHASE_VBUF_LENGTH];
	static short basePcm[AMIGA_POLYPHASE_NBANDS * 2];
	static short refPcm[AMIGA_POLYPHASE_NBANDS * 2];
	static short kernelPcm[4][AMIGA_POLYPHASE_NBANDS * 2];
	static short dispatcherPcm[AMIGA_POLYPHASE_NBANDS * 2];
	int matches[4][4];
	int dispatcherMatches[4];
	int phase;
	int kernel;
	int i;
	int refCount;
	int dispatcherCount;
	void (*kernelFns[4])(short *, int *, const int *);

	if (!StereoFastPolyphaseStride4Half_Amiga_m68k ||
		!StereoFastPolyphaseStride4Phase0_Amiga_m68k ||
		!StereoFastPolyphaseStride4Phase1_Amiga_m68k ||
		!StereoFastPolyphaseStride4Phase2_Amiga_m68k ||
		!StereoFastPolyphaseStride4Phase3_Amiga_m68k)
		return;

	kernelFns[0] = StereoFastPolyphaseStride4Phase0_Amiga_m68k;
	kernelFns[1] = StereoFastPolyphaseStride4Phase1_Amiga_m68k;
	kernelFns[2] = StereoFastPolyphaseStride4Phase2_Amiga_m68k;
	kernelFns[3] = StereoFastPolyphaseStride4Phase3_Amiga_m68k;

	for (phase = 0; phase < 4; phase++) {
		for (kernel = 0; kernel < 4; kernel++)
			matches[phase][kernel] = 1;
		dispatcherMatches[phase] = 1;
	}

	if (StereoFastPolyphaseStride4Half_Amiga_m68k_PhaseCounts) {
		for (phase = 0; phase < 4; phase++)
			StereoFastPolyphaseStride4Half_Amiga_m68k_PhaseCounts[phase] = 0;
	}

	for (phase = 0; phase < 4; phase++) {
		for (i = 0; i < 5; i++) {
			InitPolyphaseStride4StereoDiagnosticVector(baseVbuf, basePcm,
				0x2468ace0UL + (unsigned long)i * 0x10203UL, i, phase);
			memcpy(refVbuf, baseVbuf, sizeof(refVbuf));
			memcpy(refPcm, basePcm, sizeof(refPcm));
			refCount = AMIGA_POLYPHASE_STEREO_FAST_STRIDE4_C_REFERENCE(refPcm,
				refVbuf, AMIGA_POLY_COEF, phase);
			for (kernel = 0; kernel < 4; kernel++) {
				memcpy(kernelVbuf[kernel], baseVbuf, sizeof(baseVbuf));
				memcpy(kernelPcm[kernel], basePcm, sizeof(basePcm));
				kernelFns[kernel](kernelPcm[kernel], kernelVbuf[kernel],
					AMIGA_POLY_COEF);
				if (!PolyphaseStride4StereoDiagnosticMatches(refPcm, refVbuf,
					refCount, kernelPcm[kernel], kernelVbuf[kernel], 16))
					matches[phase][kernel] = 0;
			}
			memcpy(dispatcherVbuf, baseVbuf, sizeof(baseVbuf));
			memcpy(dispatcherPcm, basePcm, sizeof(basePcm));
			StereoFastPolyphaseStride4Half_Amiga_m68k(dispatcherPcm,
				dispatcherVbuf, AMIGA_POLY_COEF, phase);
			dispatcherCount = 16;
			if (!PolyphaseStride4StereoDiagnosticMatches(refPcm, refVbuf,
				refCount, dispatcherPcm, dispatcherVbuf, dispatcherCount))
				dispatcherMatches[phase] = 0;
		}
	}

	printf("Polyphase stride4 stereo direct kernel mapping diagnostic:\n");
	for (phase = 0; phase < 4; phase++) {
		printf("logical phase %d: kernel0=%s kernel1=%s kernel2=%s kernel3=%s dispatcher=%s\n",
			phase,
			matches[phase][0] ? "yes" : "no",
			matches[phase][1] ? "yes" : "no",
			matches[phase][2] ? "yes" : "no",
			matches[phase][3] ? "yes" : "no",
			dispatcherMatches[phase] ? "yes" : "no");
	}
	if (StereoFastPolyphaseStride4Half_Amiga_m68k_PhaseCounts) {
		printf("Polyphase stride4 stereo dispatcher phase counts: phase0=%lu phase1=%lu phase2=%lu phase3=%lu\n",
			StereoFastPolyphaseStride4Half_Amiga_m68k_PhaseCounts[0],
			StereoFastPolyphaseStride4Half_Amiga_m68k_PhaseCounts[1],
			StereoFastPolyphaseStride4Half_Amiga_m68k_PhaseCounts[2],
			StereoFastPolyphaseStride4Half_Amiga_m68k_PhaseCounts[3]);
	}
}
#else
static void SelftestPolyphaseStride4StereoKernelMapping(void)
{
}
#endif

static int SelftestPolyphaseStride4Stereo(void)
{
	unsigned long i;
	unsigned long failures;
	unsigned long seed;
	int pattern;
	int phase;

	failures = 0;
	seed = 0x57721566UL;
	for (i = 0; i < 500UL; i++) {
		seed = seed * 1664525UL + 1013904223UL;
		if (i < 8UL)
			pattern = 0;
		else if (i < 16UL)
			pattern = 3;
		else if (i < 24UL)
			pattern = 2;
		else if (i < 32UL)
			pattern = 4;
		else
			pattern = 1;
		for (phase = 0; phase < 4; phase++) {
			if (TestPolyphaseStride4StereoCase(i, seed, pattern, phase) != 0)
				failures++;
		}
	}

	SelftestPolyphaseStride4StereoKernelMapping();
	printf("Polyphase stride4 stereo selftest patterns: zero, impulse, alternating extremes, left/right asymmetric, deterministic random\n");
	printf("Polyphase stride4 stereo selftest cases: %lu\n", i * 4UL);
	printf("Polyphase stride4 stereo selftest failures: %lu\n", failures);
	return failures ? 1 : 0;
}

/* Reduced-tap counterpart of TestPolyphaseStride4StereoCase(): bit-exact
 * comparison between the plain-C reduced kernel (always compiled, ground
 * truth for the reduced math) and whatever PolyphaseStereoFastLowrateStride4Reduced_TEST_ACTIVE
 * actually dispatches to -- the reduced-tap m68k asm kernel when active,
 * else the same C kernel.  This is the direct correctness check for
 * StereoFastPolyphaseStride4HalfReduced_Amiga_m68k, mirroring how
 * TestPolyphaseStride2StereoReducedCase() proves the stride-2 reduced asm
 * kernel against its C reference.
 */
static int TestPolyphaseStride4StereoReducedCase(unsigned long index, unsigned long seed, int pattern, int phase)
{
	static int cvbuf[AMIGA_POLYPHASE_VBUF_LENGTH];
	static int avbuf[AMIGA_POLYPHASE_VBUF_LENGTH];
	static short cpcm[AMIGA_POLYPHASE_NBANDS * 2];
	static short apcm[AMIGA_POLYPHASE_NBANDS * 2];
	int ccount;
	int acount;
	int i;
	int lane;

	for (i = 0; i < AMIGA_POLYPHASE_VBUF_LENGTH; i++) {
		seed = seed * 1664525UL + 1013904223UL;
		if (pattern == 0) {
			cvbuf[i] = 0;
		} else if (pattern == 1) {
			cvbuf[i] = ((int)seed) >> 9;
		} else if (pattern == 2) {
			cvbuf[i] = (i & 1) ? 0x03ffffff : (int)0xfc000000UL;
		} else if (pattern == 3) {
			cvbuf[i] = (i == (int)((index + (unsigned long)phase * 37UL) %
				AMIGA_POLYPHASE_VBUF_LENGTH)) ? 0x02000000 : 0;
		} else {
			lane = i & 63;
			if (lane < 32)
				cvbuf[i] = (int)(0x01000000 + ((i * 97) & 0x000fffff));
			else
				cvbuf[i] = (int)(0xff000000UL + ((i * 193) & 0x000fffff));
		}
		avbuf[i] = cvbuf[i];
	}
	for (i = 0; i < AMIGA_POLYPHASE_NBANDS * 2; i++) {
		cpcm[i] = (short)(0x7600 + i);
		apcm[i] = (short)(0x7600 + i);
	}

	ccount = AMIGA_POLYPHASE_STEREO_FAST_STRIDE4_REDUCED_C_REFERENCE(cpcm, cvbuf, AMIGA_POLY_COEF, phase);
	acount = AMIGA_POLYPHASE_STEREO_FAST_STRIDE4_REDUCED_TEST_ACTIVE(apcm, avbuf, AMIGA_POLY_COEF, phase);

	if (ccount != 16 || acount != 16) {
		printf("PolyphaseStereoFast stride4 reduced count mismatch %lu phase=%d: first=%d second=%d pattern=%d\n",
			index, phase, ccount, acount, pattern);
		return -1;
	}
	for (i = 0; i < AMIGA_POLYPHASE_VBUF_LENGTH; i++) {
		if (avbuf[i] != cvbuf[i]) {
			printf("PolyphaseStereoFast stride4 reduced vbuf mismatch %lu phase=%d[%d]: first=%ld second=%ld pattern=%d\n",
				index, phase, i, (long)cvbuf[i], (long)avbuf[i], pattern);
			return -1;
		}
	}
	for (i = 0; i < AMIGA_POLYPHASE_NBANDS * 2; i++) {
		if (apcm[i] != cpcm[i]) {
			printf("PolyphaseStereoFast stride4 reduced output mismatch %lu phase=%d[%d]: first=%ld second=%ld pattern=%d\n",
				index, phase, i, (long)cpcm[i], (long)apcm[i], pattern);
			return -1;
		}
	}
	return 0;
}

static int SelftestPolyphaseStride4StereoReduced(void)
{
	unsigned long i;
	unsigned long failures;
	unsigned long seed;
	int pattern;
	int phase;

	failures = 0;
	seed = 0x1a2b3c4dUL;
	for (i = 0; i < 500UL; i++) {
		seed = seed * 1664525UL + 1013904223UL;
		if (i < 8UL)
			pattern = 0;
		else if (i < 16UL)
			pattern = 3;
		else if (i < 24UL)
			pattern = 2;
		else if (i < 32UL)
			pattern = 4;
		else
			pattern = 1;
		for (phase = 0; phase < 4; phase++) {
			if (TestPolyphaseStride4StereoReducedCase(i, seed, pattern, phase) != 0)
				failures++;
		}
	}

	printf("Polyphase stride4 stereo reduced asm requested: %s\n",
#ifdef AMIGA_M68K_ASM_POLYPHASE
		"yes"
#else
		"no"
#endif
	);
	printf("Polyphase stride4 stereo reduced asm active: %s\n",
		AMIGA_POLYPHASE_STEREO_FAST_STRIDE4_REDUCED_HAS_ASM() ? "yes" : "no");
	printf("Polyphase stride4 stereo reduced selftest patterns: zero, impulse, alternating extremes, left/right asymmetric, deterministic random\n");
	printf("Polyphase stride4 stereo reduced selftest cases: %lu\n", i * 4UL);
	printf("Polyphase stride4 stereo reduced selftest failures: %lu\n", failures);
	return failures ? 1 : 0;
}


static int TestPolyphaseStride2StereoCase(unsigned long index, unsigned long seed, int pattern)
{
	static int cvbuf[AMIGA_POLYPHASE_VBUF_LENGTH];
	static int avbuf[AMIGA_POLYPHASE_VBUF_LENGTH];
	static short cpcm[40];
	static short apcm[40];
	int ccount;
	int acount;
	int i;

	for (i = 0; i < AMIGA_POLYPHASE_VBUF_LENGTH; i++) {
		seed = seed * 1664525UL + 1013904223UL;
		switch (pattern) {
		case 0: cvbuf[i] = 0; break;
		case 1: cvbuf[i] = ((int)seed) >> 9; break;
		case 2: cvbuf[i] = 0x03ffffff; break;
		case 3: cvbuf[i] = (int)0xfc000000UL; break;
		case 4: cvbuf[i] = (i & 1) ? 0x03ffffff : (int)0xfc000000UL; break;
		case 5: cvbuf[i] = (i & 32) ? 0 : (((int)seed) >> 9); break;
		case 6: cvbuf[i] = (i & 32) ? (((int)seed) >> 9) : 0; break;
		case 8: cvbuf[i] = (i & 32) ? 0 : (0x00100000 + ((i & 31) << 12)); break;
		case 9: cvbuf[i] = (i & 32) ? (0xffe00000 + ((i & 31) << 11)) : 0; break;
		default: cvbuf[i] = (i & 32) ? (((int)(seed ^ 0x55aa33ccUL)) >> 8) : (((int)seed) >> 10); break;
		}
		avbuf[i] = cvbuf[i];
	}
	for (i = 0; i < 40; i++) {
		cpcm[i] = (short)(0x7300 + i);
		apcm[i] = (short)(0x7300 + i);
	}

	ccount = AMIGA_POLYPHASE_STEREO_FAST_STRIDE2_C_REFERENCE(cpcm + 4, cvbuf, AMIGA_POLY_COEF);
	acount = AMIGA_POLYPHASE_STEREO_FAST_STRIDE2_TEST_ACTIVE(apcm + 4, avbuf, AMIGA_POLY_COEF);

	if (ccount != 32 || acount != 32) {
		printf("PolyphaseStereoFast stride2 count mismatch %lu: first=%d second=%d pattern=%d\n",
			index, ccount, acount, pattern);
		return -1;
	}
	for (i = 0; i < AMIGA_POLYPHASE_VBUF_LENGTH; i++) {
		if (avbuf[i] != cvbuf[i]) {
			printf("PolyphaseStereoFast stride2 vbuf mismatch %lu[%d]: first=%ld second=%ld pattern=%d\n",
				index, i, (long)cvbuf[i], (long)avbuf[i], pattern);
			return -1;
		}
	}
	for (i = 0; i < 40; i++) {
		if (apcm[i] != cpcm[i]) {
			int j;
			printf("PolyphaseStereoFast stride2 output/sentinel mismatch %lu[%d]: first=%ld second=%ld pattern=%d\n",
				index, i, (long)cpcm[i], (long)apcm[i], pattern);
			printf("PolyphaseStereoFast stride2 diagnostic: expected L0=%ld actual L0=%ld expected R0=%ld actual R0=%ld\n",
				(long)cpcm[4], (long)apcm[4], (long)cpcm[5], (long)apcm[5]);
			printf("PolyphaseStereoFast stride2 diagnostic: left vbuf base=%p right vbuf base=%p phase=0 vindex=0\n",
				(void *)cvbuf, (void *)(cvbuf + 32));
			printf("PolyphaseStereoFast stride2 expected/actual packed LR samples:\n");
			for (j = 0; j < 32; j++)
				printf("  [%02d] expected=%ld actual=%ld%s\n", j,
					(long)cpcm[4 + j], (long)apcm[4 + j],
					(j & 1) ? " R" : " L");
			return -1;
		}
	}
	return 0;
}

static int SelftestPolyphaseStride2Stereo(void)
{
	unsigned long i;
	unsigned long failures;
	unsigned long seed;
	int pattern;

	failures = 0;
	seed = 0x66260700UL;
	if (TestPolyphaseStride2StereoCase(0, seed, 8) != 0)
		failures++;
	if (TestPolyphaseStride2StereoCase(1, seed ^ 0x13579bdfUL, 9) != 0)
		failures++;
	for (i = 0; i < 512UL; i++) {
		seed = seed * 1664525UL + 1013904223UL;
		pattern = (i < 8UL) ? (int)i : 1;
		if (TestPolyphaseStride2StereoCase(i, seed, pattern) != 0)
			failures++;
	}

	printf("Polyphase stride2 stereo asm requested: %s\n",
#ifdef AMIGA_M68K_ASM_POLYPHASE
		"yes"
#else
		"no"
#endif
	);
	printf("Polyphase stride2 stereo asm active: %s\n",
		AMIGA_POLYPHASE_STEREO_FAST_STRIDE2_HAS_ASM() ? "yes" : "no");
	printf("Polyphase stride2 stereo selftest cases: %lu\n", i);
	printf("Polyphase stride2 stereo selftest failures: %lu\n", failures);
	return failures ? 1 : 0;
}



static int TestPolyphaseStride2StereoReducedCase(unsigned long index, unsigned long seed, int pattern)
{
	static int cvbuf[AMIGA_POLYPHASE_VBUF_LENGTH];
	static int avbuf[AMIGA_POLYPHASE_VBUF_LENGTH];
	static short cpcm[40];
	static short apcm[40];
	int ccount;
	int acount;
	int i;

	for (i = 0; i < AMIGA_POLYPHASE_VBUF_LENGTH; i++) {
		seed = seed * 1664525UL + 1013904223UL;
		switch (pattern) {
		case 0: cvbuf[i] = 0; break;
		case 1: cvbuf[i] = ((int)seed) >> 9; break;
		case 2: cvbuf[i] = 0x03ffffff; break;
		case 3: cvbuf[i] = (int)0xfc000000UL; break;
		case 4: cvbuf[i] = (i & 1) ? 0x03ffffff : (int)0xfc000000UL; break;
		case 5: cvbuf[i] = (i & 32) ? 0 : (((int)seed) >> 9); break;
		case 6: cvbuf[i] = (i & 32) ? (((int)seed) >> 9) : 0; break;
		case 8: cvbuf[i] = (i & 32) ? 0 : (0x00100000 + ((i & 31) << 12)); break;
		case 9: cvbuf[i] = (i & 32) ? (0xffe00000 + ((i & 31) << 11)) : 0; break;
		default: cvbuf[i] = (i & 32) ? (((int)(seed ^ 0x55aa33ccUL)) >> 8) : (((int)seed) >> 10); break;
		}
		avbuf[i] = cvbuf[i];
	}
	for (i = 0; i < 40; i++) {
		cpcm[i] = (short)(0x7400 + i);
		apcm[i] = (short)(0x7400 + i);
	}

	ccount = AMIGA_POLYPHASE_STEREO_FAST_STRIDE2_REDUCED_C_REFERENCE(cpcm + 4, cvbuf, AMIGA_POLY_COEF);
	acount = AMIGA_POLYPHASE_STEREO_FAST_STRIDE2_REDUCED_TEST_ACTIVE(apcm + 4, avbuf, AMIGA_POLY_COEF);

	if (ccount != 32 || acount != 32) {
		printf("PolyphaseStereoFast stride2 reduced count mismatch %lu: first=%d second=%d pattern=%d\n",
			index, ccount, acount, pattern);
		return -1;
	}
	for (i = 0; i < AMIGA_POLYPHASE_VBUF_LENGTH; i++) {
		if (avbuf[i] != cvbuf[i]) {
			printf("PolyphaseStereoFast stride2 reduced vbuf mismatch %lu[%d]: first=%ld second=%ld pattern=%d\n",
				index, i, (long)cvbuf[i], (long)avbuf[i], pattern);
			return -1;
		}
	}
	for (i = 0; i < 40; i++) {
		if (apcm[i] != cpcm[i]) {
			int j;
			printf("PolyphaseStereoFast stride2 reduced output/sentinel mismatch %lu[%d]: first=%ld second=%ld pattern=%d\n",
				index, i, (long)cpcm[i], (long)apcm[i], pattern);
			printf("PolyphaseStereoFast stride2 reduced expected/actual packed LR samples:\n");
			for (j = 0; j < 32; j++)
				printf("  [%02d] expected=%ld actual=%ld%s\n", j,
					(long)cpcm[4 + j], (long)apcm[4 + j],
					(j & 1) ? " R" : " L");
			return -1;
		}
	}
	return 0;
}

static int SelftestPolyphaseStride2StereoReduced(void)
{
	unsigned long i;
	unsigned long failures;
	unsigned long seed;
	int pattern;

	failures = 0;
	seed = 0x66260700UL;
	if (TestPolyphaseStride2StereoReducedCase(0, seed, 8) != 0)
		failures++;
	if (TestPolyphaseStride2StereoReducedCase(1, seed ^ 0x13579bdfUL, 9) != 0)
		failures++;
	for (i = 0; i < 512UL; i++) {
		seed = seed * 1664525UL + 1013904223UL;
		pattern = (i < 8UL) ? (int)i : 1;
		if (TestPolyphaseStride2StereoReducedCase(i, seed, pattern) != 0)
			failures++;
	}

	printf("Polyphase stride2 stereo reduced asm requested: %s\n",
#ifdef AMIGA_M68K_ASM_POLYPHASE
		"yes"
#else
		"no"
#endif
	);
	printf("Polyphase stride2 stereo reduced asm active: %s\n",
		AMIGA_POLYPHASE_STEREO_FAST_STRIDE2_REDUCED_HAS_ASM() ? "yes" : "no");
	printf("Polyphase stride2 stereo reduced selftest cases: %lu\n", i + 2UL);
	printf("Polyphase stride2 stereo reduced selftest failures: %lu\n", failures);
	return failures ? 1 : 0;
}

static const int kStride5StereoExpectedCounts[5] = { 14, 12, 12, 12, 14 };

static int TestPolyphaseStride5StereoCase(unsigned long index, unsigned long seed, int pattern, int phase)
{
	static int cvbuf[AMIGA_POLYPHASE_VBUF_LENGTH];
	static int avbuf[AMIGA_POLYPHASE_VBUF_LENGTH];
	static short cpcm[AMIGA_POLYPHASE_NBANDS * 2];
	static short apcm[AMIGA_POLYPHASE_NBANDS * 2];
	int ccount;
	int acount;
	int expected;
	int i;
	int lane;

	for (i = 0; i < AMIGA_POLYPHASE_VBUF_LENGTH; i++) {
		seed = seed * 1664525UL + 1013904223UL;
		if (pattern == 0) {
			cvbuf[i] = 0;
		} else if (pattern == 1) {
			cvbuf[i] = ((int)seed) >> 9;
		} else if (pattern == 2) {
			cvbuf[i] = (i & 1) ? 0x03ffffff : (int)0xfc000000UL;
		} else if (pattern == 3) {
			cvbuf[i] = (i == (int)((index + (unsigned long)phase * 37UL) %
				AMIGA_POLYPHASE_VBUF_LENGTH)) ? 0x02000000 : 0;
		} else {
			/* Left/right asymmetric stereo addressing check: every 64-int
			 * block stores 32 left entries followed by 32 right entries.
			 */
			lane = i & 63;
			if (lane < 32)
				cvbuf[i] = (int)(0x01000000 + ((i * 97) & 0x000fffff));
			else
				cvbuf[i] = (int)(0xff000000UL + ((i * 193) & 0x000fffff));
		}
		avbuf[i] = cvbuf[i];
	}
	for (i = 0; i < AMIGA_POLYPHASE_NBANDS * 2; i++) {
		cpcm[i] = (short)(0x7200 + i);
		apcm[i] = (short)(0x7200 + i);
	}

	ccount = AMIGA_POLYPHASE_STEREO_FAST_STRIDE5_C_REFERENCE(cpcm, cvbuf, AMIGA_POLY_COEF, phase);
	acount = AMIGA_POLYPHASE_STEREO_FAST_STRIDE5_TEST_ACTIVE(apcm, avbuf, AMIGA_POLY_COEF, phase);

	expected = kStride5StereoExpectedCounts[phase];
	if (ccount != expected || acount != expected) {
		printf("PolyphaseStereoFast stride5 count mismatch %lu phase=%d: first=%d second=%d expected=%d pattern=%d\n",
			index, phase, ccount, acount, expected, pattern);
		return -1;
	}
	for (i = 0; i < AMIGA_POLYPHASE_VBUF_LENGTH; i++) {
		if (avbuf[i] != cvbuf[i]) {
			printf("PolyphaseStereoFast stride5 vbuf mismatch %lu phase=%d[%d]: first=%ld second=%ld pattern=%d\n",
				index, phase, i, (long)cvbuf[i], (long)avbuf[i], pattern);
			return -1;
		}
	}
	for (i = 0; i < AMIGA_POLYPHASE_NBANDS * 2; i++) {
		if (apcm[i] != cpcm[i]) {
			printf("PolyphaseStereoFast stride5 output mismatch %lu phase=%d[%d]: first=%ld second=%ld pattern=%d\n",
				index, phase, i, (long)cpcm[i], (long)apcm[i], pattern);
			return -1;
		}
	}
	return 0;
}

static int SelftestPolyphaseStride5Stereo(void)
{
	unsigned long i;
	unsigned long failures;
	unsigned long seed;
	int pattern;
	int phase;

	failures = 0;
	seed = 0x41c64e6dUL;
	for (i = 0; i < 500UL; i++) {
		seed = seed * 1664525UL + 1013904223UL;
		if (i < 8UL)
			pattern = 0;
		else if (i < 16UL)
			pattern = 3;
		else if (i < 24UL)
			pattern = 2;
		else if (i < 32UL)
			pattern = 4;
		else
			pattern = 1;
		for (phase = 0; phase < 5; phase++) {
			if (TestPolyphaseStride5StereoCase(i, seed, pattern, phase) != 0)
				failures++;
		}
	}

	printf("Polyphase stride5 stereo asm requested: %s\n",
#ifdef AMIGA_M68K_ASM_POLYPHASE
		"yes"
#else
		"no"
#endif
	);
	printf("Polyphase stride5 stereo asm active: %s\n",
		AMIGA_POLYPHASE_STEREO_FAST_STRIDE5_IS_ACTIVE() ? "yes" : "no");
	printf("Polyphase stride5 stereo selftest patterns: zero, impulse, alternating extremes, left/right asymmetric, deterministic random\n");
	printf("Polyphase stride5 stereo selftest cases: %lu\n", i * 5UL);
	printf("Polyphase stride5 stereo selftest failures: %lu\n", failures);
	return failures ? 1 : 0;
}

/*
 * Stride 3 (14700 Hz output from 44100 Hz) has a hand-unrolled C polyphase
 * kernel (PolyphaseMonoFastLowrateStride3()/PolyphaseStereoFastLowrateStride3()
 * in real/polyphase.c, dispatched from PolyphaseMonoFastLowrate()/
 * PolyphaseStereoFastLowrate()), matching the stride 4/5 style -- no asm
 * kernel yet. This proves that dispatch independently: an un-decimated
 * PolyphaseMonoFast_C_REFERENCE()/PolyphaseStereo() call is the ground
 * truth, and this function separately (re-)derives -- without calling
 * into the code under test -- which of those 32 samples a continuously
 * running "keep every 3rd sample, phase carried across frames" decimator
 * starting at startPhase should keep, matching the same contract stride
 * 2/4/5 already rely on via PolyphaseAdvanceLowratePhase().
 */
static int TestPolyphaseStride3Case(unsigned long index, unsigned long seed, int pattern,
	int startPhase, int stereo)
{
	static int cvbuf[AMIGA_POLYPHASE_VBUF_LENGTH * 2];
	static int avbuf[AMIGA_POLYPHASE_VBUF_LENGTH * 2];
	static short full[AMIGA_POLYPHASE_NBANDS * 2];
	static short pcm[AMIGA_POLYPHASE_NBANDS * 2];
	int expectedIndex[AMIGA_POLYPHASE_NBANDS];
	int expectedCount;
	int vbufLen;
	int i;
	int phase;
	int count;
	int running;

	vbufLen = stereo ? AMIGA_POLYPHASE_VBUF_LENGTH * 2 : AMIGA_POLYPHASE_VBUF_LENGTH;
	for (i = 0; i < vbufLen; i++) {
		seed = seed * 1664525UL + 1013904223UL;
		if (pattern == 0)
			cvbuf[i] = 0;
		else if (pattern == 1)
			cvbuf[i] = ((int)seed) >> 9;
		else
			cvbuf[i] = (i & 1) ? 0x03ffffff : (int)0xfc000000UL;
		avbuf[i] = cvbuf[i];
	}
	for (i = 0; i < AMIGA_POLYPHASE_NBANDS * 2; i++)
		full[i] = pcm[i] = (short)(0x6c00 + i);

	if (stereo)
		AMIGA_POLYPHASE_STEREO_FULL(full, cvbuf, AMIGA_POLY_COEF);
	else
		AMIGA_POLYPHASE_MONO_FAST_C_REFERENCE(full, cvbuf, AMIGA_POLY_COEF);

	expectedCount = 0;
	running = startPhase;
	for (i = 0; i < AMIGA_POLYPHASE_NBANDS; i++) {
		if (running == 0)
			expectedIndex[expectedCount++] = i;
		running++;
		if (running >= 3)
			running = 0;
	}

	phase = startPhase;
	if (stereo)
		count = PolyphaseStereoFastLowrate(pcm, avbuf, AMIGA_POLY_COEF, 3, &phase);
	else
		count = PolyphaseMonoFastLowrate(pcm, avbuf, AMIGA_POLY_COEF, 3, &phase);

	if (count != expectedCount * (stereo ? 2 : 1)) {
		printf("Polyphase stride3 count mismatch %lu startPhase=%d stereo=%d pattern=%d: got=%d expected=%d\n",
			index, startPhase, stereo, pattern, count, expectedCount * (stereo ? 2 : 1));
		return -1;
	}
	for (i = 0; i < vbufLen; i++) {
		if (avbuf[i] != cvbuf[i]) {
			printf("Polyphase stride3 vbuf mismatch %lu[%d] startPhase=%d stereo=%d pattern=%d: first=%ld second=%ld\n",
				index, i, startPhase, stereo, pattern, (long)cvbuf[i], (long)avbuf[i]);
			return -1;
		}
	}
	for (i = 0; i < expectedCount; i++) {
		if (stereo) {
			if (pcm[i * 2] != full[expectedIndex[i] * 2] ||
				pcm[i * 2 + 1] != full[expectedIndex[i] * 2 + 1]) {
				printf("Polyphase stride3 stereo output mismatch %lu[%d] startPhase=%d pattern=%d "
					"fullIdx=%d: got=(%d,%d) want=(%d,%d)\n",
					index, i, startPhase, pattern, expectedIndex[i],
					pcm[i * 2], pcm[i * 2 + 1], full[expectedIndex[i] * 2], full[expectedIndex[i] * 2 + 1]);
				return -1;
			}
		} else if (pcm[i] != full[expectedIndex[i]]) {
			printf("Polyphase stride3 mono output mismatch %lu[%d] startPhase=%d pattern=%d fullIdx=%d: "
				"got=%d want=%d\n",
				index, i, startPhase, pattern, expectedIndex[i], pcm[i], full[expectedIndex[i]]);
			return -1;
		}
	}
	{
		int expectedEndPhase = (startPhase + AMIGA_POLYPHASE_NBANDS) % 3;
		if (phase != expectedEndPhase) {
			printf("Polyphase stride3 phase mismatch %lu startPhase=%d stereo=%d: got=%d expected=%d\n",
				index, startPhase, stereo, phase, expectedEndPhase);
			return -1;
		}
	}
	return 0;
}

static int SelftestPolyphaseStride3Common(int stereo)
{
	unsigned long i;
	unsigned long failures;
	unsigned long seed;
	int pattern;
	int startPhase;

	failures = 0;
	seed = 0x2b7e1516UL;
	for (i = 0; i < 1500UL; i++) {
		seed = seed * 1664525UL + 1013904223UL;
		pattern = (i < 24UL) ? 0 : ((i < 48UL) ? 2 : 1);
		startPhase = (int)(i % 3UL);
		if (TestPolyphaseStride3Case(i, seed, pattern, startPhase, stereo) != 0)
			failures++;
	}

	printf("Polyphase stride3 %s asm requested: %s\n", stereo ? "stereo" : "mono",
#ifdef AMIGA_M68K_ASM_POLYPHASE
		"yes"
#else
		"no"
#endif
	);
	printf("Polyphase stride3 %s asm active: %s\n", stereo ? "stereo" : "mono",
		(stereo ? AMIGA_POLYPHASE_STEREO_STRIDE3_HAS_ASM() : AMIGA_POLYPHASE_MONO_STRIDE3_HAS_ASM())
			? "yes" : "no");
	printf("Polyphase stride3 %s selftest cases: %lu\n", stereo ? "stereo" : "mono", i);
	printf("Polyphase stride3 %s selftest failures: %lu\n", stereo ? "stereo" : "mono", failures);
	return failures ? 1 : 0;
}

static int SelftestPolyphaseStride3(void)
{
	return SelftestPolyphaseStride3Common(0);
}

static int SelftestPolyphaseStride3Stereo(void)
{
	return SelftestPolyphaseStride3Common(1);
}

/*
 * Stride 5 (8820/8287 Hz) mono now has a full 5-phase m68k asm kernel
 * (MonoFastPolyphaseStride5Phase0..4_Amiga_m68k in
 * real/amiga_m68k_polyphase.S) alongside the existing hand-unrolled C
 * path (PolyphaseMonoFastLowrateStride5) -- stereo already had full asm
 * coverage for this stride; mono did not. Same independent-reference
 * methodology as the stride3 selftest above: an un-decimated
 * AMIGA_POLYPHASE_MONO_FAST_C_REFERENCE call is the ground truth, and
 * this function separately (re-)derives which of those 32 samples a
 * continuously running "keep every 5th sample, phase carried across
 * frames" decimator starting at startPhase should keep, without calling
 * into the code under test.
 */
static int TestPolyphaseStride5MonoCase(unsigned long index, unsigned long seed, int pattern,
	int startPhase)
{
	static int cvbuf[AMIGA_POLYPHASE_VBUF_LENGTH];
	static int avbuf[AMIGA_POLYPHASE_VBUF_LENGTH];
	static short full[AMIGA_POLYPHASE_NBANDS];
	static short pcm[AMIGA_POLYPHASE_NBANDS];
	int expectedIndex[AMIGA_POLYPHASE_NBANDS];
	int expectedCount;
	int i;
	int phase;
	int count;
	int running;

	for (i = 0; i < AMIGA_POLYPHASE_VBUF_LENGTH; i++) {
		seed = seed * 1664525UL + 1013904223UL;
		if (pattern == 0)
			cvbuf[i] = 0;
		else if (pattern == 1)
			cvbuf[i] = ((int)seed) >> 9;
		else
			cvbuf[i] = (i & 1) ? 0x03ffffff : (int)0xfc000000UL;
		avbuf[i] = cvbuf[i];
	}
	for (i = 0; i < AMIGA_POLYPHASE_NBANDS; i++)
		full[i] = pcm[i] = (short)(0x6c00 + i);

	AMIGA_POLYPHASE_MONO_FAST_C_REFERENCE(full, cvbuf, AMIGA_POLY_COEF);

	expectedCount = 0;
	running = startPhase;
	for (i = 0; i < AMIGA_POLYPHASE_NBANDS; i++) {
		if (running == 0)
			expectedIndex[expectedCount++] = i;
		running++;
		if (running >= 5)
			running = 0;
	}

	phase = startPhase;
	count = PolyphaseMonoFastLowrate(pcm, avbuf, AMIGA_POLY_COEF, 5, &phase);

	if (count != expectedCount) {
		printf("Polyphase stride5 mono count mismatch %lu startPhase=%d pattern=%d: got=%d expected=%d\n",
			index, startPhase, pattern, count, expectedCount);
		return -1;
	}
	for (i = 0; i < AMIGA_POLYPHASE_VBUF_LENGTH; i++) {
		if (avbuf[i] != cvbuf[i]) {
			printf("Polyphase stride5 mono vbuf mismatch %lu[%d] startPhase=%d pattern=%d\n",
				index, i, startPhase, pattern);
			return -1;
		}
	}
	for (i = 0; i < expectedCount; i++) {
		if (pcm[i] != full[expectedIndex[i]]) {
			printf("Polyphase stride5 mono output mismatch %lu[%d] startPhase=%d pattern=%d fullIdx=%d: "
				"got=%d want=%d\n",
				index, i, startPhase, pattern, expectedIndex[i], pcm[i], full[expectedIndex[i]]);
			return -1;
		}
	}
	{
		int expectedEndPhase = (startPhase + AMIGA_POLYPHASE_NBANDS) % 5;
		if (phase != expectedEndPhase) {
			printf("Polyphase stride5 mono phase mismatch %lu startPhase=%d: got=%d expected=%d\n",
				index, startPhase, phase, expectedEndPhase);
			return -1;
		}
	}
	return 0;
}

static int SelftestPolyphaseStride5(void)
{
	unsigned long i;
	unsigned long failures;
	unsigned long seed;
	int pattern;
	int startPhase;

	failures = 0;
	seed = 0x5eed5eedUL;
	for (i = 0; i < 2000UL; i++) {
		seed = seed * 1664525UL + 1013904223UL;
		pattern = (i < 30UL) ? 0 : ((i < 60UL) ? 2 : 1);
		startPhase = (int)(i % 5UL);
		if (TestPolyphaseStride5MonoCase(i, seed, pattern, startPhase) != 0)
			failures++;
	}

	printf("Polyphase stride5 mono asm requested: %s\n",
#ifdef AMIGA_M68K_ASM_POLYPHASE
		"yes"
#else
		"no"
#endif
	);
	printf("Polyphase stride5 mono asm active: %s\n",
		AMIGA_POLYPHASE_MONO_STRIDE5_HAS_ASM() ? "yes" : "no");
	printf("Polyphase stride5 mono selftest cases: %lu\n", i);
	printf("Polyphase stride5 mono selftest failures: %lu\n", failures);
	return failures ? 1 : 0;
}

/*
 * Stride 4 (11025 Hz) mono now has asm kernels for all 4 phases
 * (MonoFastPolyphaseStride4_Amiga_m68k for phase 0, plus
 * MonoFastPolyphaseStride4Phase1/2/3_Amiga_m68k) -- previously only
 * phase 0 had an asm kernel, so 3 of every 4 frames fell back to C.
 * The original --selftest-polyphase-stride4 only ever exercises phase 0
 * (via PolyphaseMonoFastLowrateStride4_C_REFERENCE/_TEST_ACTIVE, both
 * hardcoded to phase 0), so this uses the same independent full-band
 * reference methodology as the stride 3/5 selftests to cover all 4
 * phases uniformly.
 */
static int TestPolyphaseStride4AllPhasesCase(unsigned long index, unsigned long seed, int pattern,
	int startPhase)
{
	static int cvbuf[AMIGA_POLYPHASE_VBUF_LENGTH];
	static int avbuf[AMIGA_POLYPHASE_VBUF_LENGTH];
	static short full[AMIGA_POLYPHASE_NBANDS];
	static short pcm[AMIGA_POLYPHASE_NBANDS];
	int expectedIndex[AMIGA_POLYPHASE_NBANDS];
	int expectedCount;
	int i;
	int phase;
	int count;
	int running;

	for (i = 0; i < AMIGA_POLYPHASE_VBUF_LENGTH; i++) {
		seed = seed * 1664525UL + 1013904223UL;
		if (pattern == 0)
			cvbuf[i] = 0;
		else if (pattern == 1)
			cvbuf[i] = ((int)seed) >> 9;
		else
			cvbuf[i] = (i & 1) ? 0x03ffffff : (int)0xfc000000UL;
		avbuf[i] = cvbuf[i];
	}
	for (i = 0; i < AMIGA_POLYPHASE_NBANDS; i++)
		full[i] = pcm[i] = (short)(0x6c00 + i);

	AMIGA_POLYPHASE_MONO_FAST_C_REFERENCE(full, cvbuf, AMIGA_POLY_COEF);

	expectedCount = 0;
	running = startPhase;
	for (i = 0; i < AMIGA_POLYPHASE_NBANDS; i++) {
		if (running == 0)
			expectedIndex[expectedCount++] = i;
		running++;
		if (running >= 4)
			running = 0;
	}

	phase = startPhase;
	count = PolyphaseMonoFastLowrate(pcm, avbuf, AMIGA_POLY_COEF, 4, &phase);

	if (count != expectedCount) {
		printf("Polyphase stride4 all-phases count mismatch %lu startPhase=%d pattern=%d: got=%d expected=%d\n",
			index, startPhase, pattern, count, expectedCount);
		return -1;
	}
	for (i = 0; i < AMIGA_POLYPHASE_VBUF_LENGTH; i++) {
		if (avbuf[i] != cvbuf[i]) {
			printf("Polyphase stride4 all-phases vbuf mismatch %lu[%d] startPhase=%d pattern=%d\n",
				index, i, startPhase, pattern);
			return -1;
		}
	}
	for (i = 0; i < expectedCount; i++) {
		if (pcm[i] != full[expectedIndex[i]]) {
			printf("Polyphase stride4 all-phases output mismatch %lu[%d] startPhase=%d pattern=%d fullIdx=%d: "
				"got=%d want=%d\n",
				index, i, startPhase, pattern, expectedIndex[i], pcm[i], full[expectedIndex[i]]);
			return -1;
		}
	}
	{
		int expectedEndPhase = (startPhase + AMIGA_POLYPHASE_NBANDS) % 4;
		if (phase != expectedEndPhase) {
			printf("Polyphase stride4 all-phases phase mismatch %lu startPhase=%d: got=%d expected=%d\n",
				index, startPhase, phase, expectedEndPhase);
			return -1;
		}
	}
	return 0;
}

static int SelftestPolyphaseStride4AllPhases(void)
{
	unsigned long i;
	unsigned long failures;
	unsigned long seed;
	int pattern;
	int startPhase;

	failures = 0;
	seed = 0x40114014UL;
	for (i = 0; i < 2000UL; i++) {
		seed = seed * 1664525UL + 1013904223UL;
		pattern = (i < 30UL) ? 0 : ((i < 60UL) ? 2 : 1);
		startPhase = (int)(i % 4UL);
		if (TestPolyphaseStride4AllPhasesCase(i, seed, pattern, startPhase) != 0)
			failures++;
	}

	printf("Polyphase stride4 all-phases asm requested: %s\n",
#ifdef AMIGA_M68K_ASM_POLYPHASE
		"yes"
#else
		"no"
#endif
	);
	printf("Polyphase stride4 all-phases asm active: %s\n",
		AMIGA_POLYPHASE_MONO_STRIDE4_ALLPHASES_HAS_ASM() ? "yes" : "no");
	printf("Polyphase stride4 all-phases selftest cases: %lu\n", i);
	printf("Polyphase stride4 all-phases selftest failures: %lu\n", failures);
	return failures ? 1 : 0;
}


static void PrintIntegerRms(const char *label, const IntegerRms *rms,
	const char *suffix)
{
	char value[32];
	IntegerRmsText(value, sizeof(value), rms);
	printf("%s%s%s", label, value, suffix);
}

#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE) && defined(AMIGA_FAST_FDCT32_QUARTER)
static int Fdct32QuarterIsActiveIndex(const int *active, int nactive, int idx)
{
	int i;
	for (i = 0; i < nactive; i++) {
		if (active[i] == idx)
			return 1;
	}
	return 0;
}

#endif

static int SelftestFdct32Quarter(void)
{
#if !(defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE) && defined(AMIGA_FAST_FDCT32_QUARTER))
	printf("FDCT32Quarter compile flag: no\n");
	printf("FDCT32Quarter selftest not run: quarter FDCT body is not compiled in this build\n");
	MP3SetExperimentalFDCT32Quarter(1);
	printf("FDCT32Quarter stride gate: stride 2 call=%s, stride 4 call=%s\n",
		(2 == 4 && MP3ExperimentalFDCT32QuarterEnabled()) ? "yes" : "no",
		(4 == 4 && MP3ExperimentalFDCT32QuarterEnabled()) ? "yes" : "no");
	MP3SetExperimentalFDCT32Quarter(0);
	printf("FDCT32Quarter selftest PASS (unavailable in this build)\n");
	return 0;
#else
	enum { CASES = 500, DEST_WORDS = 4096, ACTIVE = 9 };
	static int hbuf[32];
	static int qbuf[32];
	static int hdest[DEST_WORDS];
	static int qdest[DEST_WORDS];
	unsigned long seed;
	unsigned long i;
	unsigned long activeScatterMismatches;
	unsigned long staleMismatches;
	IntegerRms rms;
	int j;

	seed = 0x4d504733UL;
	activeScatterMismatches = 0;
	staleMismatches = 0;
	memset(&rms, 0, sizeof(rms));

	for (i = 0; i < CASES; i++) {
		int offset;
		int oddBlock;
		int gb;
		int phase;
		int oddBase;
		int evenBase;
		int delayOff;
		int active[ACTIVE];

		offset = (int)(i & 7UL);
		oddBlock = (int)((i >> 3) & 1UL);
		gb = (int)((i >> 4) & 7UL);
		phase = (int)((i >> 7) & 3UL);
		for (j = 0; j < 32; j++) {
			seed = seed * 1664525UL + 1013904223UL;
			if (j < 8)
				hbuf[j] = ((int)seed) >> 9;
			else
				hbuf[j] = 0;
			qbuf[j] = hbuf[j];
		}
		for (j = 0; j < DEST_WORDS; j++) {
			hdest[j] = (int)(0x55aa0000UL ^ (unsigned long)j);
			qdest[j] = hdest[j];
		}

		oddBase = oddBlock ? AMIGA_POLYPHASE_VBUF_LENGTH : 0;
		evenBase = oddBlock ? 0 : AMIGA_POLYPHASE_VBUF_LENGTH;
		delayOff = (offset - oddBlock) & 7;
		active[0] = 64 * 16 + delayOff + evenBase;
		active[1] = offset + oddBase + 64 * 0;
		active[2] = offset + oddBase + 64 * 4;
		active[3] = offset + oddBase + 64 * 8;
		active[4] = offset + oddBase + 64 * 12;
		active[5] = 16 + delayOff + evenBase + 64 * 0;
		active[6] = 16 + delayOff + evenBase + 64 * 4;
		active[7] = 16 + delayOff + evenBase + 64 * 8;
		active[8] = 16 + delayOff + evenBase + 64 * 12;

		AMIGA_FDCT32_HALF(hbuf, hdest, offset, oddBlock, gb);
		AMIGA_FDCT32_QUARTER(qbuf, qdest, offset, oddBlock, gb, phase, 4);

		for (j = 0; j < ACTIVE; j++) {
			int idx = active[j];
			if (hdest[idx] == (int)(0x55aa0000UL ^ (unsigned long)idx) ||
				qdest[idx] == (int)(0x55aa0000UL ^ (unsigned long)idx) ||
				hdest[idx + 8] != hdest[idx] || qdest[idx + 8] != qdest[idx])
				activeScatterMismatches++;
			else {
				IntegerRmsAdd(&rms, hdest[idx], qdest[idx]);
			}
		}
		for (j = 0; j < 16; j++) {
			int idx = offset + oddBase + 64 * j;
			if (!Fdct32QuarterIsActiveIndex(active, ACTIVE, idx) &&
				(qdest[idx] != 0 || qdest[idx + 8] != 0))
				staleMismatches++;
			idx = 16 + delayOff + evenBase + 64 * j;
			if (!Fdct32QuarterIsActiveIndex(active, ACTIVE, idx) &&
				(qdest[idx] != 0 || qdest[idx + 8] != 0))
				staleMismatches++;
		}
	}

	printf("FDCT32Quarter compile flag: %s\n",
#ifdef AMIGA_FAST_FDCT32_QUARTER
		"yes"
#else
		"no"
#endif
	);
	printf("FDCT32Quarter selftest cases: %lu\n", (unsigned long)CASES);
	printf("FDCT32Quarter active scatter positions: 9 (mismatches: %lu)\n",
		activeScatterMismatches);
	printf("FDCT32Quarter stale quarter-rate row clears: %lu mismatches\n",
		staleMismatches);
	PrintIntegerRms("FDCT32Quarter RMS difference vs FDCT32Half active rows: ",
		&rms, " counts (integer)\n");
	MP3SetExperimentalFDCT32Quarter(1);
	printf("FDCT32Quarter stride gate: stride 2 call=%s, stride 4 call=%s\n",
		(2 == 4 && MP3ExperimentalFDCT32QuarterEnabled()) ? "yes" : "no",
		(4 == 4 && MP3ExperimentalFDCT32QuarterEnabled()) ? "yes" : "no");
	MP3SetExperimentalFDCT32Quarter(0);
	printf("FDCT32Quarter selftest PASS (lossy approximation)\n");
	return 0;
#endif
}

static int SelftestFdct32QuarterStereo(void)
{
#if !(defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE) && defined(AMIGA_FAST_FDCT32_QUARTER))
	printf("FDCT32Quarter stereo compile flag: no\n");
	printf("FDCT32Quarter stereo selftest not run: quarter FDCT body is not compiled in this build\n");
	return 0;
#else
	enum { CASES = 500, DEST_WORDS = 4096, ACTIVE = 9, CHANNEL_OFFSET = 32 };
	static int input[2][32];
	static int referenceInput[2][32];
	static int reference[2][DEST_WORDS + CHANNEL_OFFSET];
	static int actual[DEST_WORDS + CHANNEL_OFFSET];
	unsigned long seed[2];
	unsigned long i;
	unsigned long activationMismatches;
	unsigned long independenceMismatches;
	IntegerRms rms[2];
	int ch;
	int j;

	seed[0] = 0x4d504733UL;
	seed[1] = 0x53544552UL;
	activationMismatches = 0;
	independenceMismatches = 0;
	memset(rms, 0, sizeof(rms));
	MP3SetExperimentalFDCT32Quarter(1);

	for (i = 0; i < CASES; i++) {
		int offset = (int)(i & 7UL);
		int oddBlock = (int)((i >> 3) & 1UL);
		int gb = (int)((i >> 4) & 7UL);
		int phase = (int)((i >> 7) & 3UL);
		int oddBase = oddBlock ? AMIGA_POLYPHASE_VBUF_LENGTH : 0;
		int evenBase = oddBlock ? 0 : AMIGA_POLYPHASE_VBUF_LENGTH;
		int delayOff = (offset - oddBlock) & 7;
		int active[ACTIVE];
		int channel0AfterFirst[ACTIVE];

		active[0] = 64 * 16 + delayOff + evenBase;
		active[1] = offset + oddBase + 64 * 0;
		active[2] = offset + oddBase + 64 * 4;
		active[3] = offset + oddBase + 64 * 8;
		active[4] = offset + oddBase + 64 * 12;
		active[5] = 16 + delayOff + evenBase + 64 * 0;
		active[6] = 16 + delayOff + evenBase + 64 * 4;
		active[7] = 16 + delayOff + evenBase + 64 * 8;
		active[8] = 16 + delayOff + evenBase + 64 * 12;

		for (ch = 0; ch < 2; ch++) {
			for (j = 0; j < 32; j++) {
				seed[ch] = seed[ch] * 1664525UL + 1013904223UL;
				input[ch][j] = j < 8 ? ((int)seed[ch]) >> (ch ? 8 : 9) : 0;
				referenceInput[ch][j] = input[ch][j];
			}
			for (j = 0; j < DEST_WORDS + CHANNEL_OFFSET; j++)
				reference[ch][j] = (int)(0x55aa0000UL ^ (unsigned long)j);
			AMIGA_FDCT32_HALF(referenceInput[ch],
				reference[ch] + ch * CHANNEL_OFFSET,
				offset, oddBlock, gb);
		}
		for (j = 0; j < DEST_WORDS + CHANNEL_OFFSET; j++)
			actual[j] = (int)(0x55aa0000UL ^ (unsigned long)j);

		FDCT32FastLowrate(input[0], actual, offset, oddBlock, gb, 4, phase);
		for (j = 0; j < ACTIVE; j++)
			channel0AfterFirst[j] = actual[active[j]];
		FDCT32FastLowrate(input[1], actual + CHANNEL_OFFSET, offset, oddBlock,
			gb, 4, phase);
		for (j = 0; j < ACTIVE; j++) {
			if (actual[active[j]] != channel0AfterFirst[j])
				independenceMismatches++;
		}

		for (ch = 0; ch < 2; ch++) {
			for (j = 0; j < ACTIVE; j++) {
				int idx = active[j] + ch * CHANNEL_OFFSET;
				int sentinel = (int)(0x55aa0000UL ^ (unsigned long)idx);
				int got = actual[idx];
				if (got == sentinel)
					activationMismatches++;
				else {
					IntegerRmsAdd(&rms[ch], reference[ch][idx], got);
				}
			}
		}
	}
	MP3SetExperimentalFDCT32Quarter(0);

	printf("FDCT32Quarter stereo selftest cases: %lu\n", (unsigned long)CASES);
	printf("FDCT32Quarter stereo activation mismatches: %lu\n",
		activationMismatches);
	printf("FDCT32Quarter stereo channel-independence mismatches: %lu\n",
		independenceMismatches);
	PrintIntegerRms("FDCT32Quarter stereo channel 0 RMS difference vs full FDCT32 (active rows): ",
		&rms[0], " counts (integer)\n");
	PrintIntegerRms("FDCT32Quarter stereo channel 1 RMS difference vs full FDCT32 (active rows): ",
		&rms[1], " counts (integer)\n");
	if (activationMismatches || independenceMismatches) {
		printf("FDCT32Quarter stereo selftest FAIL\n");
		return 1;
	}
	printf("FDCT32Quarter stereo selftest PASS (lossy approximation)\n");
	return 0;
#endif
}


static int SelftestReducedTaps(void)
{
	enum { CASES = 500 };
	static int vbuf[AMIGA_POLYPHASE_VBUF_LENGTH];
	static short fullMono[AMIGA_POLYPHASE_NBANDS];
	static short reducedMono[AMIGA_POLYPHASE_NBANDS];
	static short fullStereo[AMIGA_POLYPHASE_NBANDS * 2];
	static short reducedStereo[AMIGA_POLYPHASE_NBANDS * 2];
	static short fullStride2Mono[AMIGA_POLYPHASE_NBANDS];
	static short reducedStride2Mono[AMIGA_POLYPHASE_NBANDS];
	static short fullStride2Stereo[AMIGA_POLYPHASE_NBANDS * 2];
	static short reducedStride2Stereo[AMIGA_POLYPHASE_NBANDS * 2];
	static int independenceVbuf[AMIGA_POLYPHASE_VBUF_LENGTH];
	static short independencePcm[AMIGA_POLYPHASE_NBANDS * 2];
	unsigned long seed;
	unsigned long i;
	unsigned long monoCountMismatches;
	unsigned long stereoCountMismatches;
	unsigned long stride2StereoCountMismatches;
	unsigned long stride2MonoCountMismatches;
	unsigned long stride2MonoOverrunMismatches;
	unsigned long stride2StereoIndependenceMismatches;
	IntegerRms monoRms, stereoRms, stride2MonoRms, stride2StereoRms[2];
	int j;

	seed = 0x8a7c4d11UL;
	monoCountMismatches = 0;
	stereoCountMismatches = 0;
	stride2StereoCountMismatches = 0;
	stride2MonoCountMismatches = 0;
	stride2MonoOverrunMismatches = 0;
	stride2StereoIndependenceMismatches = 0;
	memset(&monoRms, 0, sizeof(monoRms));
	memset(&stereoRms, 0, sizeof(stereoRms));
	memset(&stride2MonoRms, 0, sizeof(stride2MonoRms));
	memset(stride2StereoRms, 0, sizeof(stride2StereoRms));

	for (i = 0; i < CASES; i++) {
		int phase;
		int fullMonoCount;
		int reducedMonoCount;
		int fullStereoCount;
		int reducedStereoCount;
		int fullStride2StereoCount;
		int reducedStride2StereoCount;
		int fullStride2MonoCount;
		int reducedStride2MonoCount;

		phase = (int)(i & 3UL);
		for (j = 0; j < AMIGA_POLYPHASE_VBUF_LENGTH; j++) {
			seed = seed * 1664525UL + 1013904223UL;
			vbuf[j] = ((int)seed) >> 9;
		}
		for (j = 0; j < AMIGA_POLYPHASE_NBANDS; j++) {
			fullMono[j] = (short)(0x7300 + j);
			reducedMono[j] = (short)(0x7400 + j);
			fullStride2Mono[j] = (short)(0x7900 + j);
			reducedStride2Mono[j] = (short)(0x7a00 + j);
		}
		for (j = 0; j < AMIGA_POLYPHASE_NBANDS * 2; j++) {
			fullStereo[j] = (short)(0x7500 + j);
			reducedStereo[j] = (short)(0x7600 + j);
			fullStride2Stereo[j] = (short)(0x7700 + j);
			reducedStride2Stereo[j] = (short)(0x7800 + j);
		}

		fullMonoCount = AMIGA_POLYPHASE_MONO_FAST_STRIDE4_C_REFERENCE(
			fullMono, vbuf, AMIGA_POLY_COEF);
		reducedMonoCount = AMIGA_POLYPHASE_MONO_FAST_STRIDE4_REDUCED_TEST_ACTIVE(
			reducedMono, vbuf, AMIGA_POLY_COEF, 0);
		fullStereoCount = AMIGA_POLYPHASE_STEREO_FAST_STRIDE4_C_REFERENCE(
			fullStereo, vbuf, AMIGA_POLY_COEF, phase);
		reducedStereoCount = AMIGA_POLYPHASE_STEREO_FAST_STRIDE4_REDUCED_TEST_ACTIVE(
			reducedStereo, vbuf, AMIGA_POLY_COEF, phase);
		fullStride2StereoCount = AMIGA_POLYPHASE_STEREO_FAST_STRIDE2_C_REFERENCE(
			fullStride2Stereo, vbuf, AMIGA_POLY_COEF);
		reducedStride2StereoCount = AMIGA_POLYPHASE_STEREO_FAST_STRIDE2_REDUCED_TEST_ACTIVE(
			reducedStride2Stereo, vbuf, AMIGA_POLY_COEF);
		fullStride2MonoCount = AMIGA_POLYPHASE_MONO_FAST_STRIDE2_C_REFERENCE(
			fullStride2Mono, vbuf, AMIGA_POLY_COEF);
		reducedStride2MonoCount = AMIGA_POLYPHASE_MONO_FAST_STRIDE2_REDUCED_TEST_ACTIVE(
			reducedStride2Mono, vbuf, AMIGA_POLY_COEF);

		if (fullMonoCount != 8 || reducedMonoCount != 8)
			monoCountMismatches++;
		if (fullStereoCount != 16 || reducedStereoCount != 16)
			stereoCountMismatches++;
		if (fullStride2StereoCount != 32 || reducedStride2StereoCount != 32)
			stride2StereoCountMismatches++;
		if (fullStride2MonoCount != 16 || reducedStride2MonoCount != 16)
			stride2MonoCountMismatches++;
		if (reducedStride2MonoCount == 16) {
			for (j = 16; j < AMIGA_POLYPHASE_NBANDS; j++)
				if (reducedStride2Mono[j] != (short)(0x7a00 + j))
					stride2MonoOverrunMismatches++;
		}
		if (reducedMonoCount == 8) {
			for (j = 0; j < 8; j++) {
				IntegerRmsAdd(&monoRms, fullMono[j], reducedMono[j]);
			}
		}
		if (reducedStereoCount == 16) {
			for (j = 0; j < 16; j++) {
				IntegerRmsAdd(&stereoRms, fullStereo[j], reducedStereo[j]);
			}
		}
		if (reducedStride2MonoCount == 16) {
			for (j = 0; j < 16; j++) {
				IntegerRmsAdd(&stride2MonoRms, fullStride2Mono[j], reducedStride2Mono[j]);
			}
		}
		if (reducedStride2StereoCount == 32) {
			for (j = 0; j < 16; j++) {
				IntegerRmsAdd(&stride2StereoRms[0], fullStride2Stereo[j * 2],
					reducedStride2Stereo[j * 2]);
				IntegerRmsAdd(&stride2StereoRms[1], fullStride2Stereo[j * 2 + 1],
					reducedStride2Stereo[j * 2 + 1]);
			}
		}

		for (j = 0; j < AMIGA_POLYPHASE_VBUF_LENGTH; j++)
			independenceVbuf[j] = ((j & 63) < 32) ? vbuf[j] : 0;
		for (j = 0; j < AMIGA_POLYPHASE_NBANDS * 2; j++)
			independencePcm[j] = 0;
		if (AMIGA_POLYPHASE_STEREO_FAST_STRIDE2_REDUCED_TEST_ACTIVE(
			independencePcm, independenceVbuf, AMIGA_POLY_COEF) == 32) {
			for (j = 0; j < 16; j++)
				if (independencePcm[j * 2 + 1] != 0)
					stride2StereoIndependenceMismatches++;
		}
		for (j = 0; j < AMIGA_POLYPHASE_VBUF_LENGTH; j++)
			independenceVbuf[j] = ((j & 63) >= 32) ? vbuf[j] : 0;
		for (j = 0; j < AMIGA_POLYPHASE_NBANDS * 2; j++)
			independencePcm[j] = 0;
		if (AMIGA_POLYPHASE_STEREO_FAST_STRIDE2_REDUCED_TEST_ACTIVE(
			independencePcm, independenceVbuf, AMIGA_POLY_COEF) == 32) {
			for (j = 0; j < 16; j++)
				if (independencePcm[j * 2] != 0)
					stride2StereoIndependenceMismatches++;
		}
	}

	printf("Reduced taps compile flag: %s\n",
#ifdef AMIGA_FAST_REDUCED_TAPS
		"yes"
#else
		"no"
#endif
	);
	printf("Reduced taps selftest cases: %lu\n", (unsigned long)CASES);
	printf("Reduced taps mono output samples per call: 8 (mismatches: %lu)\n",
		monoCountMismatches);
	printf("Reduced taps stereo output frames per call: 8 (16 shorts, mismatches: %lu)\n",
		stereoCountMismatches);
	printf("Reduced taps stride2 stereo output frames per call: 16 (32 shorts, mismatches: %lu)\n",
		stride2StereoCountMismatches);
	printf("Reduced taps stride2 mono output samples per call: 16 (mismatches: %lu)\n",
		stride2MonoCountMismatches);
	printf("Reduced taps stride2 mono overrun/aliasing mismatches: %lu\n",
		stride2MonoOverrunMismatches);
	printf("Reduced taps stride2 stereo channel-independence mismatches: %lu\n",
		stride2StereoIndependenceMismatches);
	PrintIntegerRms("Reduced taps mono RMS difference: ", &monoRms,
		" counts (integer; target < 500)\n");
	PrintIntegerRms("Reduced taps stereo RMS difference: ", &stereoRms,
		" counts (integer; target < 500)\n");
	PrintIntegerRms("Reduced taps stride2 mono RMS difference: ", &stride2MonoRms,
		" counts (integer; informational)\n");
	PrintIntegerRms("Reduced taps stride2 stereo RMS difference: L=", &stride2StereoRms[0], " R=");
	PrintIntegerRms("", &stride2StereoRms[1], " counts (integer; informational)\n");
	if (stride2StereoCountMismatches || stride2MonoCountMismatches ||
		stride2MonoOverrunMismatches ||
		stride2StereoIndependenceMismatches) {
		printf("Reduced taps selftest FAIL\n");
		return 1;
	}
	printf("Reduced taps selftest PASS (lossy approximation)\n");
	return 0;
}

static int SelftestMonoFastLowrateStereo(void)
{
	enum {
		IN_CHANNELS = 2,
		OUT_CHANNELS = 1,
		TOTAL_FRAMES = 44100,
		CHUNK_FRAMES = 1152,
		STRIDE = 4,
		EXPECTED = TOTAL_FRAMES / STRIDE
	};
	short input[CHUNK_FRAMES * IN_CHANNELS];
	short lowrate[CHUNK_FRAMES * IN_CHANNELS];
	short mono[CHUNK_FRAMES];
	DecodeOptions opt;
	DecodeStats stats;
	int phase;
	int offset;
	int failures;

	memset(&opt, 0, sizeof(opt));
	memset(&stats, 0, sizeof(stats));
	opt.mono = 1;
	opt.fastLowrate = 1;
	opt.outputRate = 11025;
	stats.sampleRate = 44100;
	stats.outputSampleRate = 11025;
	stats.channels = IN_CHANNELS;
	stats.outputChannels = OUT_CHANNELS;
	phase = 0;
	failures = 0;

	for (offset = 0; offset < TOTAL_FRAMES; offset += CHUNK_FRAMES) {
		int frames;
		int i;
		int selected;
		int mixed;

		frames = TOTAL_FRAMES - offset;
		if (frames > CHUNK_FRAMES)
			frames = CHUNK_FRAMES;
		for (i = 0; i < frames; i++) {
			int frame = offset + i;
			input[2 * i] = (short)((frame % 251) * 101 - 12000);
			input[2 * i + 1] = (short)(12000 - (frame % 197) * 97);
		}
		selected = FastLowrateSelectFrame(&phase, input, lowrate,
			frames * IN_CHANNELS, STRIDE, IN_CHANNELS);
		mixed = MixFrame(lowrate, mono, selected, IN_CHANNELS, 1);
		stats.outputSamples += (unsigned long)mixed;
	}

	if (stats.outputSamples != EXPECTED) {
		fprintf(stderr,
			"mono fast-lowrate stereo selftest count mismatch: got=%lu expected=%d\n",
			stats.outputSamples, EXPECTED);
		failures++;
	}
	if (PerChannelEmittedSamples(&opt, &stats) != EXPECTED) {
		fprintf(stderr,
			"mono fast-lowrate stereo selftest per-channel mismatch: got=%lu expected=%d\n",
			PerChannelEmittedSamples(&opt, &stats), EXPECTED);
		failures++;
	}
	if (DecodedAudioMilliseconds(&opt, &stats) < 999 ||
		DecodedAudioMilliseconds(&opt, &stats) > 1001) {
		fprintf(stderr,
			"mono fast-lowrate stereo selftest milliseconds mismatch: got=%lu expected=1000\n",
			DecodedAudioMilliseconds(&opt, &stats));
		failures++;
	}
	if (!failures)
		printf("mono fast-lowrate stereo selftest passed: 44100 Hz stereo -> 11025 Hz mono emitted %lu samples\n",
			stats.outputSamples);
	return failures ? 1 : 0;
}

static void InitNoOutputSvx(SvxWriter *svx, int compression)
{
	memset(svx, 0, sizeof(*svx));
	svx->compression = compression;
	svx->noOutput = 1;
}

static unsigned long NextRand32(unsigned long *state)
{
	*state = (*state * 1664525UL) + 1013904223UL;
	return *state;
}

static int SelftestHuffman(const DecodeOptions *opt)
{
	enum { HUFFMAN_SELFTEST_CASES = 1000, HUFFMAN_PAIR_TABS = 32 };
	static unsigned char buf[128];
	static int cxy[MAX_NSAMP];
	static int axy[MAX_NSAMP];
	unsigned long seed;
	unsigned long failures;
	unsigned long i;
	int j;

	seed = 0x68756666UL;
	failures = 0;
	for (i = 0; i < HUFFMAN_SELFTEST_CASES; i++) {
		int nVals;
		int tabIdx;
		int bitsLeft;
		int bitOffset;
		int cret;
		int aret;

		for (j = 0; j < (int)sizeof(buf); j++) {
			if (j < (int)sizeof(buf) - 8)
				buf[j] = (unsigned char)(NextRand32(&seed) >> 24);
			else
				buf[j] = 0;
		}
		for (j = 0; j < MAX_NSAMP; j++) {
			cxy[j] = (int)(0x5a5a0000UL ^ (unsigned long)j);
			axy[j] = cxy[j];
		}

		tabIdx = (int)(NextRand32(&seed) % HUFFMAN_PAIR_TABS);
		nVals = (int)(NextRand32(&seed) % ((MAX_NSAMP / 2) + 1)) * 2;
		bitsLeft = (int)(NextRand32(&seed) % 769UL);
		bitOffset = (int)(NextRand32(&seed) & 7UL);

		cret = AMIGA_HUFFMAN_PAIRS_C_REFERENCE(cxy, nVals, tabIdx, bitsLeft, buf, bitOffset);
		aret = AMIGA_HUFFMAN_PAIRS_TEST_ACTIVE(axy, nVals, tabIdx, bitsLeft, buf, bitOffset);
		if (aret != cret) {
			printf("Huffman selftest bitsUsed mismatch %lu: tab=%d nVals=%d bitsLeft=%d bitOffset=%d first=%d second=%d\n",
				i, tabIdx, nVals, bitsLeft, bitOffset, cret, aret);
			failures++;
			continue;
		}
		for (j = 0; j < nVals; j++) {
			if (axy[j] != cxy[j]) {
				printf("Huffman selftest xy mismatch %lu[%d]: tab=%d nVals=%d bitsLeft=%d bitOffset=%d first=%ld second=%ld bitsUsed=%d\n",
					i, j, tabIdx, nVals, bitsLeft, bitOffset, (long)cxy[j], (long)axy[j], cret);
				failures++;
				break;
			}
		}
	}

	printf("Huffman asm compiled: %s\n", AMIGA_HUFFMAN_PAIRS_HAS_ASM() ? "yes" : "no");
	printf("Huffman runtime default: C\n");
	printf("Huffman selftest candidate: %s\n", AMIGA_HUFFMAN_PAIRS_HAS_ASM() ? "asm" : "C");
	printf("Huffman forced by --exp-huff: %s\n", (opt && opt->expHuff) ? "yes" : "no");
	printf("Huffman asm active: %s\n", AMIGA_HUFFMAN_PAIRS_ASM_NOTE());
	printf("Huffman selftest cases: %lu\n", i);
	printf("Huffman selftest failures: %lu\n", failures);
	return failures ? 1 : 0;
}


static int TestIntensityCase(unsigned long index, unsigned long seed, int pattern,
	int stride, int count, int fl, int fr, int seedL, int seedR)
{
	static int cx0[MAX_NSAMP];
	static int cx1[MAX_NSAMP];
	static int ax0[MAX_NSAMP];
	static int ax1[MAX_NSAMP];
	int i;
	int cmOutL, cmOutR, amOutL, amOutR;
	int span;

	span = (count > 0) ? ((count - 1) * stride + 1) : 1;
	if (span > MAX_NSAMP)
		span = MAX_NSAMP;
	for (i = 0; i < MAX_NSAMP; i++) {
		seed = seed * 1664525UL + 1013904223UL;
		if (pattern == 0)
			cx0[i] = 0;
		else if (pattern == 1)
			cx0[i] = (i == (span >> 1)) ? 0x02000000 : 0;
		else if (pattern == 2)
			cx0[i] = (i & 1) ? 0x03ffffff : (int)0xfc000000UL;
		else
			cx0[i] = ((int)seed) >> 6;
		cx1[i] = (int)(0x5a5a0000UL ^ (unsigned long)i);
		ax0[i] = cx0[i];
		ax1[i] = cx1[i];
	}
	cmOutL = amOutL = seedL;
	cmOutR = amOutR = seedR;
	if (count > 1) {
		int firstCount = count >> 1;
		int secondCount = count - firstCount;
		int offset = firstCount * stride;
		AMIGA_INTENSITY_SCALE_RUN_C_REFERENCE(cx0, cx1, fl, fr, firstCount, stride, &cmOutL, &cmOutR);
		AMIGA_INTENSITY_SCALE_RUN_C_REFERENCE(cx0 + offset, cx1 + offset, fl, fr, secondCount, stride, &cmOutL, &cmOutR);
		if (stride == 1) {
			AMIGA_INTENSITY_SCALE_RUN1_TEST_ACTIVE(ax0, ax1, fl, fr, firstCount, &amOutL, &amOutR);
			AMIGA_INTENSITY_SCALE_RUN1_TEST_ACTIVE(ax0 + offset, ax1 + offset, fl, fr, secondCount, &amOutL, &amOutR);
		} else {
			AMIGA_INTENSITY_SCALE_RUN3_TEST_ACTIVE(ax0, ax1, fl, fr, firstCount, &amOutL, &amOutR);
			AMIGA_INTENSITY_SCALE_RUN3_TEST_ACTIVE(ax0 + offset, ax1 + offset, fl, fr, secondCount, &amOutL, &amOutR);
		}
	} else {
		AMIGA_INTENSITY_SCALE_RUN_C_REFERENCE(cx0, cx1, fl, fr, count, stride, &cmOutL, &cmOutR);
		if (stride == 1)
			AMIGA_INTENSITY_SCALE_RUN1_TEST_ACTIVE(ax0, ax1, fl, fr, count, &amOutL, &amOutR);
		else
			AMIGA_INTENSITY_SCALE_RUN3_TEST_ACTIVE(ax0, ax1, fl, fr, count, &amOutL, &amOutR);
	}
	if (cmOutL != amOutL || cmOutR != amOutR) {
		printf("Intensity selftest mask mismatch %lu: stride=%d count=%d fl=%ld fr=%ld C(%ld,%ld) active(%ld,%ld)\n",
			index, stride, count, (long)fl, (long)fr, (long)cmOutL, (long)cmOutR, (long)amOutL, (long)amOutR);
		return -1;
	}
	for (i = 0; i < MAX_NSAMP; i++) {
		if (cx0[i] != ax0[i] || cx1[i] != ax1[i]) {
			printf("Intensity selftest sample mismatch %lu[%d]: stride=%d count=%d fl=%ld fr=%ld C(%ld,%ld) active(%ld,%ld)\n",
				index, i, stride, count, (long)fl, (long)fr, (long)cx0[i], (long)cx1[i], (long)ax0[i], (long)ax1[i]);
			return -1;
		}
	}
	return 0;
}

static int SelftestIntensity(void)
{
	unsigned long cases;
	unsigned long failures;
	unsigned long seed;
	int strideIdx;

	cases = 0;
	failures = 0;
	seed = 0x51a1e5c7UL;
	for (strideIdx = 0; strideIdx < 2; strideIdx++) {
		int stride = strideIdx ? 3 : 1;
		int maxCount = strideIdx ? (MAX_NSAMP / 3) : MAX_NSAMP;
		int pattern;
		for (pattern = 0; pattern < 4; pattern++) {
			int countCase;
			for (countCase = 0; countCase < 96; countCase++) {
				int count;
				int fl;
				int fr;
				int seedL;
				int seedR;
				seed = NextRand32(&seed);
				count = (countCase < 8) ? countCase : (int)(seed % (unsigned long)(maxCount + 1));
				seed = NextRand32(&seed);
				fl = ((int)seed) >> 1;
				seed = NextRand32(&seed);
				fr = ((int)seed) >> 1;
				seed = NextRand32(&seed);
				seedL = (int)(seed | 0x01010101UL);
				seed = NextRand32(&seed);
				seedR = (int)(seed | 0x02020202UL);
				if (TestIntensityCase(cases, seed, pattern, stride, count, fl, fr, seedL, seedR) != 0) {
					failures++;
					if (failures >= 16) {
						printf("Intensity selftest stopped after 16 failures\n");
						break;
					}
				}
				cases++;
			}
		}
	}
	printf("Intensity asm requested: %s\n",
#ifdef AMIGA_M68K_ASM_INTENSITY
		"yes"
#else
		"no"
#endif
	);
	printf("Intensity asm active: %s\n", AMIGA_INTENSITY_SCALE_RUN_HAS_ASM() ? "yes" : "no");
	printf("Intensity selftest cases: %lu\n", cases);
	printf("Intensity selftest failures: %lu\n", failures);
	return failures ? 1 : 0;
}

static int SelftestDequant(void)
{
	enum { DEQUANT_X_MAX = 8206 };
	unsigned long cases;
	unsigned long failures;
	int scale;

	cases = 0;
	failures = 0;
	for (scale = -47; scale <= 0; scale++) {
		int x;
		for (x = 0; x <= DEQUANT_X_MAX; x++) {
			int signCase;
			for (signCase = 0; signCase < (x ? 2 : 1); signCase++) {
				int cin;
				int ain;
				int cout;
				int aout;
				int cmask;
				int amask;

				cin = signCase ? (int)(0x80000000UL | (unsigned long)x) : x;
				ain = cin;
				cout = (int)0x55aa55aaUL;
				aout = (int)0xaa55aa55UL;
				cmask = AMIGA_DEQUANT_BLOCK_C_REFERENCE(&cin, &cout, 1, scale);
				amask = AMIGA_DEQUANT_BLOCK_TEST_ACTIVE(&ain, &aout, 1, scale);
				cases++;
				if (cmask != amask || cout != aout || cin != ain) {
					printf("Dequant selftest mismatch scale=%d x=%d sign=%d C(out=%ld mask=%ld in=%ld) active(out=%ld mask=%ld in=%ld)\n",
						scale, x, signCase, (long)cout, (long)cmask, (long)cin,
						(long)aout, (long)amask, (long)ain);
					failures++;
					if (failures >= 16) {
						printf("Dequant selftest stopped after 16 failures\n");
						printf("Dequant selftest cases: %lu\n", cases);
						printf("Dequant selftest failures: %lu\n", failures);
						return 1;
					}
				}
			}
		}
	}

	printf("Dequant asm requested: %s\n",
#ifdef AMIGA_M68K_ASM_DEQUANT
		"yes"
#else
		"no"
#endif
	);
	printf("Dequant asm active: %s\n", AMIGA_DEQUANT_BLOCK_HAS_ASM() ? "yes" : "no");
	printf("Dequant selftest cases: %lu\n", cases);
	printf("Dequant selftest failures: %lu\n", failures);
	return failures ? 1 : 0;
}

static int SelftestCLZReference(int x)
{
	unsigned int ux;
	int numZeros;

	ux = (unsigned int)x;
	if (!ux)
		return 32;
	numZeros = 0;
	while (!(ux & 0x80000000UL)) {
		numZeros++;
		ux <<= 1;
	}
	return numZeros;
}

static int TestCLZValue(int x, unsigned long index)
{
	int c;
	int a;

	c = SelftestCLZReference(x);
	a = CLZ(x);
	if (a != c) {
		printf("CLZ mismatch %lu: x=0x%08lx first=%ld second=%ld\n",
			index, (unsigned long)(unsigned int)x, (long)c, (long)a);
		return -1;
	}
	return 0;
}

static int SelftestClz(void)
{
	static const int edges[] = {
		0,
		1,
		0x7fffffffL,
		(int)0xffffffffUL
	};
	unsigned long failures;
	unsigned long tested;
	unsigned long seed;
	unsigned long i;

	failures = 0;
	tested = 0;
	seed = 0x636c7a21UL;

	for (i = 0; i < sizeof(edges) / sizeof(edges[0]); i++) {
		if (TestCLZValue(edges[i], tested) != 0)
			failures++;
		tested++;
	}

	for (i = 0; i < 32UL; i++) {
		int x = (int)(1UL << i);
		if (TestCLZValue(x, tested) != 0)
			failures++;
		tested++;
	}

	for (i = 0; i < 10000UL; i++) {
		int x = (int)NextRand32(&seed);
		if (TestCLZValue(x, tested) != 0)
			failures++;
		tested++;
	}

	printf("CLZ bfffo asm available: %s\n",
#if defined(CLZ_HAS_AMIGA_M68K_ASM) && CLZ_HAS_AMIGA_M68K_ASM
		"yes"
#else
		"no (C reference path only in this build)"
#endif
	);
	printf("CLZ selftest cases: %lu\n", tested);
	printf("CLZ selftest failures: %lu\n", failures);
	return failures ? 1 : 0;
}

static int TestMulshiftPair(int x, int y, unsigned long index)
{
	int c = MULSHIFT32_C_REFERENCE(x, y);
#if MULSHIFT32_HAS_AMIGA_M68K_ASM
	int a = MULSHIFT32_AMIGA_M68K_ASM(x, y);
#else
	int a = c;
#endif
	if (a != c) {
		printf("MULSHIFT32 mismatch %lu: x=%ld y=%ld C=%ld asm=%ld\n",
			index, (long)x, (long)y, (long)c, (long)a);
		return -1;
	}
	return 0;
}

static int SelftestMulshift(void)
{
	static const int edges[] = {
		0, 1, -1, 2, -2, 0x7fffffffL, (int)0x80000000UL,
		0x40000000L, (int)0xc0000000UL, 0x12345678L, (int)0x87654321UL
	};
	unsigned long i;
	unsigned long failures = 0;
	unsigned long tested = 0;
	unsigned long seed = 0x1234abcdUL;

	for (i = 0; i < sizeof(edges) / sizeof(edges[0]); i++) {
		unsigned long j;
		for (j = 0; j < sizeof(edges) / sizeof(edges[0]); j++) {
			if (TestMulshiftPair(edges[i], edges[j], tested) != 0)
				failures++;
			tested++;
		}
	}

	for (i = 0; i < 100000UL; i++) {
		int x = (int)NextRand32(&seed);
		int y = (int)NextRand32(&seed);
		if (TestMulshiftPair(x, y, tested) != 0)
			failures++;
		tested++;
	}

	printf("MULSHIFT32 asm available: %s\n",
#if MULSHIFT32_HAS_AMIGA_M68K_ASM
		"yes"
#else
		"no (C reference path only in this build)"
#endif
	);
	printf("MULSHIFT32 selftest cases: %lu\n", tested);
	printf("MULSHIFT32 selftest failures: %lu\n", failures);
	return failures ? 1 : 0;
}


/*
 * Fake-stereo (pseudo-stereo) widener.  Runs on the mono decode path so the
 * stereo impression costs ~mono CPU.  Energy-symmetric cross-delay:
 *   L = mono    + (delayed >> shift)
 *   R = delayed + (mono    >> shift)
 * Because L and R are symmetric in (mono <-> delayed), E[L^2] == E[R^2] for any
 * stationary input, so neither channel is louder.  (A plain L=mono+w / R=mono-w
 * comb instead leans correlated bass into one channel, making the other sound
 * quieter.)  delay is in output samples; larger shift = less cross-bleed = wider
 * (shift 0 collapses to mono).
 */
#ifdef HAVE_AMIGA_AUDIO_DEVICE
/* Path to the decoders/ directory (including trailing slash).
 * Set by the GUI on startup so the playback subprocess can find modules. */
char gDecoderModulesPath[512];
#define EXPECTED_FLAC_DECODER_PATH "VHD0:libhelix-mp3/decoders/flac.decoder"

static void InitDecoderModulesPath(void)
{
	BPTR progDir;
	char dirPath[512];

	if (gDecoderModulesPath[0])
		return;

	progDir = GetProgramDir();
	if (!progDir || !NameFromLock(progDir, (STRPTR)dirPath, (LONG)sizeof(dirPath))) {
		strncpy(dirPath, "PROGDIR:", sizeof(dirPath) - 1);
		dirPath[sizeof(dirPath) - 1] = '\0';
	} else {
		int l = (int)strlen(dirPath);
		if (l > 0 && l + 1 < (int)sizeof(dirPath) &&
			dirPath[l - 1] != '/' && dirPath[l - 1] != ':') {
			dirPath[l]     = '/';
			dirPath[l + 1] = '\0';
		}
	}
	strncat(dirPath, "decoders/", sizeof(dirPath) - strlen(dirPath) - 1);
	strncpy(gDecoderModulesPath, dirPath, sizeof(gDecoderModulesPath) - 1);
	gDecoderModulesPath[sizeof(gDecoderModulesPath) - 1] = '\0';
}
#endif

/* Returns pointer to the extension part of a filename (after the last dot),
 * or NULL if there is no extension.  The returned pointer is into 'path'. */
static const char *GetFileExtension(const char *path)
{
	const char *ext = NULL;
	const char *p;

	if (!path)
		return NULL;
	for (p = path; *p; p++) {
		if (*p == '/' || *p == ':')
			ext = NULL;
		else if (*p == '.')
			ext = p + 1;
	}
	return (ext && *ext) ? ext : NULL;
}

/* Case-insensitive string compare (portable; no strcasecmp in all environments) */
static int StrCaseCmp(const char *a, const char *b)
{
	unsigned char ca, cb;

	do {
		ca = (unsigned char)*a++;
		cb = (unsigned char)*b++;
		if (ca >= 'A' && ca <= 'Z') ca += 32;
		if (cb >= 'A' && cb <= 'Z') cb += 32;
	} while (ca && ca == cb);
	return (int)ca - (int)cb;
}

static int StrCaseStarts(const char *s, const char *prefix)
{
	unsigned char ca, cb;

	if (!s || !prefix)
		return 0;
	while (*prefix) {
		ca = (unsigned char)*s++;
		cb = (unsigned char)*prefix++;
		if (ca >= 'A' && ca <= 'Z') ca += 32;
		if (cb >= 'A' && cb <= 'Z') cb += 32;
		if (ca != cb)
			return 0;
	}
	return 1;
}

#if ENABLE_RADIO && !defined(main)
/* The command-line player has no GUI to probe a URL before playing it, so
 * follow redirects, .pls/.m3u station links (with mirror fallback) and
 * playlist-generator links here, as the GUIs do. If the probe fails the URL
 * is played as given, exactly as before, so nothing that played stops. The
 * GUIs (where main is renamed) have probed already and skip this. */
static void CliResolveRadioUrl(DecodeOptions *opt)
{
	static RbStreamInfo info;
	static unsigned char peek[RB_PROBE_PEEK_SIZE];
	static char resolved[512];
	int peekLen = 0;
	int rc;

	if (!opt || !opt->inName || opt->haveRadioHostAddr)
		return;
	/* The GUIs do this at start-up; without AmiSSL the probe resolves and
	 * connects on this task, so bsdsocket.library must be open first. */
	Radio_NetworkInit();
	if (!Radio_HasNetwork())
		return;
	rc = rb_probe_stream_url(opt->inName, &info, peek, (int)sizeof(peek), &peekLen);
	if (rc < 0 || !info.final_url[0]) {
		fprintf(stderr, "radio-cli: probe failed (%s); trying the URL as given\n",
			rb_probe_error_text(rc));
		return;
	}
	if (strcmp(info.final_url, opt->inName) != 0)
		fprintf(stderr, "radio-cli: %s -> %s\n", opt->inName, info.final_url);
	strncpy(resolved, info.final_url, sizeof(resolved) - 1);
	resolved[sizeof(resolved) - 1] = '\0';
	opt->inName = resolved;
	opt->haveRadioHostAddr = info.have_host_addr;
	opt->radioHostAddrBe = info.host_addr_be;
	if (!opt->radioCodecHint) {
		if (info.codec == RB_STREAM_CODEC_FLAC) opt->radioCodecHint = "FLAC";
		else if (info.codec == RB_STREAM_CODEC_AAC) opt->radioCodecHint = "AAC";
	}
}
#endif

static const char *RadioDecoderExtFromContentType(const char *contentType)
{
	if (!contentType || !contentType[0])
		return NULL;
	if (StrCaseStarts(contentType, "audio/aac") ||
		StrCaseStarts(contentType, "audio/aacp") ||
		StrCaseStarts(contentType, "audio/x-aac") ||
		StrCaseStarts(contentType, "audio/mp4"))
		return "aac";
	if (StrCaseStarts(contentType, "audio/flac") ||
		StrCaseStarts(contentType, "audio/x-flac"))
		return "flac";
	if (StrCaseStarts(contentType, "audio/ogg") ||
		StrCaseStarts(contentType, "audio/vorbis") ||
		StrCaseStarts(contentType, "audio/x-vorbis") ||
		StrCaseStarts(contentType, "application/ogg"))
		return "ogg";
	if (StrCaseStarts(contentType, "audio/mpeg") ||
		StrCaseStarts(contentType, "audio/mp3"))
		return "mp3";
	return NULL;
}

static int RadioUrlHasMp3Hint(const char *url)
{
	const char *p;
	if (!url)
		return 0;
	for (p = url; *p; p++) {
		if ((p[0] == 'm' || p[0] == 'M') &&
			(p[1] == 'p' || p[1] == 'P') &&
			(p[2] == '3'))
			return 1;
	}
	return 0;
}

/* Radio Browser directories commonly tag Opus-in-Ogg mounts with codec
 * "OPUS" and/or Content-Type "audio/opus", and Icecast otherwise reports the
 * same generic "audio/ogg"/"application/ogg" Content-Type it uses for
 * Vorbis-in-Ogg -- so this must be checked before RadioDecoderExtFromContentType
 * folds both down to the same "ogg" extension string. This module only links
 * Tremor (integer Vorbis); handing it an Opus identification header is not a
 * format error Tremor is guaranteed to reject cleanly (see the matching
 * OggS+OpusHead guard in radio_stream_probe.c's rb_probe_detect_codec()),
 * so callers must refuse the stream here rather than let it reach the OGG
 * decoder module. */
static int RadioStreamLooksLikeOpus(const char *url, const char *contentType, const char *codecHint)
{
	const char *ext = GetFileExtension(url);
	if (codecHint && StrCaseCmp(codecHint, "OPUS") == 0)
		return 1;
	if (contentType && (StrCaseStarts(contentType, "audio/opus") ||
		StrCaseStarts(contentType, "audio/x-opus")))
		return 1;
	if (ext && StrCaseCmp(ext, "opus") == 0)
		return 1;
	return 0;
}

static const char *RadioDecoderExtFromUrlOrTypeHint(const char *url, const char *contentType, const char *codecHint)
{
	const char *ext = GetFileExtension(url);
	const char *typeExt = RadioDecoderExtFromContentType(contentType);
	if (codecHint && (StrCaseCmp(codecHint, "AAC") == 0 || StrCaseCmp(codecHint, "AAC+") == 0 || StrCaseCmp(codecHint, "AACP") == 0))
		return "aac";
	if (codecHint && StrCaseCmp(codecHint, "MP3") == 0 && !typeExt)
		return "mp3";
	if (codecHint && StrCaseCmp(codecHint, "FLAC") == 0)
		return "flac";
	if (typeExt)
		return typeExt;
	if (ext && (StrCaseCmp(ext, "aac") == 0 ||
		StrCaseCmp(ext, "aacp") == 0 ||
		StrCaseCmp(ext, "flac") == 0 ||
		StrCaseCmp(ext, "fla") == 0 ||
		StrCaseCmp(ext, "ogg") == 0 ||
		StrCaseCmp(ext, "oga") == 0 ||
		StrCaseCmp(ext, "mp3") == 0))
		return StrCaseCmp(ext, "aacp") == 0 ? "aac" : ext;
	if (RadioUrlHasMp3Hint(url))
		return "mp3";
	return NULL;
}

static const char *RadioDecoderExtFromUrlOrType(const char *url, const char *contentType)
{
	return RadioDecoderExtFromUrlOrTypeHint(url, contentType, NULL);
}

typedef struct FakeStereo {
	short hist[FAKE_STEREO_MAX_DELAY];
	int pos;
	int delay;
	int shift;
	int enabled;
	int configured;
} FakeStereo;

static void FakeStereoInit(FakeStereo *fs, int enabled, int delay, int shift)
{
	int i;

	for (i = 0; i < FAKE_STEREO_MAX_DELAY; i++)
		fs->hist[i] = 0;
	fs->pos = 0;
	if (delay < 1)
		delay = 1;
	if (delay > FAKE_STEREO_MAX_DELAY)
		delay = FAKE_STEREO_MAX_DELAY;
	fs->delay = delay;
	if (shift < 0)
		shift = 0;
	if (shift > 8)
		shift = 8;
	fs->shift = shift;
	fs->enabled = enabled ? 1 : 0;
	fs->configured = 1;
}

static void FakeStereoProcess(FakeStereo *fs, int mono, short *outL, short *outR)
{
	int idx, d, l, r;

	idx = (fs->pos + FAKE_STEREO_MAX_DELAY - fs->delay) & FAKE_STEREO_DELAY_MASK;
	d = (int)fs->hist[idx];
	/*
	 * Energy-symmetric cross-delay widener.  L leads with the current sample,
	 * R leads with the delayed sample, each with a >>shift cross-bleed of the
	 * other.  Because the two channels are symmetric in (mono <-> delayed),
	 * E[L^2] == E[R^2] for any stationary input, so neither side is louder --
	 * unlike a plain L=mono+w / R=mono-w comb, where correlated bass leans into
	 * one channel.  Larger shift = less cross-bleed = wider (shift 0 == mono).
	 */
	l = mono + (d >> fs->shift);
	r = d + (mono >> fs->shift);
	/* Pseudo-stereo sounds quieter than the real mono path because centre energy
	 * is spread across channels.  Give it a modest fixed-point makeup gain.
	 * R additionally gets 7/4 instead of L's 3/2: it leads with the *delayed*
	 * copy, and the precedence (Haas) effect makes the lagging side sound
	 * quieter than the leading L even at equal energy, so the extra boost pulls
	 * the perceived image back toward centre. */
	l = (l * 3) / 2;
	r = (r * 7) / 4;
	*outL = ClipToS16(l);
	*outR = ClipToS16(r);
	fs->hist[fs->pos] = (short)mono;
	fs->pos = (fs->pos + 1) & FAKE_STEREO_DELAY_MASK;
}

static int SelftestFakeStereo(void)
{
	FakeStereo fs;
	short mono[2048], L[2048], R[2048];
	int failures = 0;
	int i;
	int delay = 64;
	int shift = 2;
	long sumL2 = 0, sumR2 = 0, diff, tol;
	unsigned long seed = 0x1234567UL;

	/* moderate amplitude so the cross-delay sum never clips, keeping it exact */
	for (i = 0; i < 2048; i++) {
		seed = seed * 1664525UL + 1013904223UL;
		mono[i] = (short)(((int)(seed >> 16) & 0x3fff) - 0x2000); /* -8192..8191 */
	}
	FakeStereoInit(&fs, 1, delay, shift);
	for (i = 0; i < 2048; i++)
		FakeStereoProcess(&fs, mono[i], &L[i], &R[i]);

	/* exact cross-delay formula with makeup gain: L = (mono + (d>>shift))*3/2,
	 * R = (d + (mono>>shift))*7/4, with d = mono[i-delay] (0 during warm-up). */
	for (i = 0; i < 2048; i++) {
		int d = (i >= delay) ? (int)mono[i - delay] : 0;
		int rawL = mono[i] + (d >> shift);
		int rawR = d + ((int)mono[i] >> shift);
		int el = ClipToS16((rawL * 3) / 2);
		int er = ClipToS16((rawR * 7) / 4);
		if ((int)L[i] != el || (int)R[i] != er) {
			printf("fake-stereo formula fail %d: L=%d/%d R=%d/%d\n",
				i, L[i], el, R[i], er);
			failures++;
			break;
		}
	}
	/* Energy balance: the raw cross-delay is energy-symmetric, but R carries a
	 * deliberate makeup boost (7/4 vs L's 3/2) to offset the precedence effect,
	 * so R should be louder than L by ~ (7/4 / (3/2))^2 = 49/36.  Measured over
	 * the steady state (past the first `delay` warm-up samples, where R has not
	 * yet seen any delayed history). */
	for (i = delay; i < 2048; i++) {
		sumL2 += (long)L[i] * (long)L[i];
		sumR2 += (long)R[i] * (long)R[i];
	}
	{
		long expectedR2 = sumL2 * 49 / 36;
		diff = (sumR2 > expectedR2) ? (sumR2 - expectedR2) : (expectedR2 - sumR2);
		tol = expectedR2 / 16;	/* within ~6% of the intended boost */
		if (sumR2 <= sumL2 || diff > tol) {
			printf("fake-stereo balance off: sumL2=%ld sumR2=%ld expectedR2=%ld diff=%ld tol=%ld\n",
				sumL2, sumR2, expectedR2, diff, tol);
			failures++;
		}
	}
	/* width must actually be present (channels differ) */
	{
		int differs = 0;
		for (i = delay; i < 2048; i++)
			if (L[i] != R[i]) { differs = 1; break; }
		if (!differs) {
			printf("fake-stereo produced no stereo width\n");
			failures++;
		}
	}
	printf("fake-stereo selftest delay=%d shift=%d cases=2048 sumL2=%ld sumR2=%ld failures=%d\n",
		delay, shift, sumL2, sumR2, failures);
	return failures ? 1 : 0;
}

typedef struct DecodeStream {
	InputSource *input;
	HMP3Decoder decoder;
	unsigned char readBuf[READBUF_SIZE];
	unsigned char *readPtr;
	short decodeBuf[OUTBUF_SAMPS];
	short writeBuf[OUTBUF_SAMPS];
	short rateBuf[OUTBUF_SAMPS];
	union {
		signed char interleaved[OUTBUF_SAMPS];
		signed char planar[2][OUTBUF_SAMPS / 2];
	} spill;
	int spillPos;
	int spillCount;
	int planarSpillPos;
	int planarSpillCount;
	int bytesLeft;
	int eofReached;
	int outOfData;
	int decodeError;
	int effectiveRate;
	DecodeStats *stats;
	TimingStats *timing;
	RateState rateState;
	FakeStereo fakeStereo;
} DecodeStream;

static void DecodeStreamInit(DecodeStream *stream, InputSource *input,
	HMP3Decoder decoder, DecodeStats *stats, TimingStats *timing)
{
	memset(stream, 0, sizeof(*stream));
	stream->input = input;
	stream->decoder = decoder;
	stream->readPtr = stream->readBuf;
	stream->stats = stats;
	stream->timing = timing;
}


static int DecodeStreamCopySpill(DecodeStream *stream, signed char *dest,
	int maxBytes, int *outBytes)
{
	int n;

	if (stream->spillPos >= stream->spillCount) {
		stream->spillPos = 0;
		stream->spillCount = 0;
		return 0;
	}
	n = stream->spillCount - stream->spillPos;
	if (n > maxBytes)
		n = maxBytes;
	memcpy(dest + *outBytes, stream->spill.interleaved + stream->spillPos, n);
	stream->spillPos += n;
	*outBytes += n;
	if (stream->spillPos >= stream->spillCount) {
		stream->spillPos = 0;
		stream->spillCount = 0;
	}
	return n;
}

static int DecodeStreamFillS8(DecodeStream *stream, const DecodeOptions *opt,
	signed char *dest, int maxBytes)
{
	MP3FrameInfo info;
	int produced;

	produced = 0;
	DecodeStreamCopySpill(stream, dest, maxBytes, &produced);
	while (produced < maxBytes && !stream->outOfData && !gPlaybackInterrupted) {
		int nRead;
		int offset;
		int err;
		unsigned char *frameStart;
		int frameBytes;

		if (stream->bytesLeft < 2 * MAINBUF_SIZE && !stream->eofReached) {
			nRead = FillReadBuffer(stream->readBuf, stream->readPtr,
				READBUF_SIZE, stream->bytesLeft, stream->input);
			stream->bytesLeft += nRead;
			stream->readPtr = stream->readBuf;
			if (nRead == 0)
				stream->eofReached = 1;
		}

		offset = FindValidatedMpegSync(stream->readPtr, stream->bytesLeft);
		if (offset < 0) {
			if (stream->eofReached)
				break;
			if (stream->bytesLeft > 3) {
				stream->readPtr += stream->bytesLeft - 3;
				stream->bytesLeft = 3;
			}
			continue;
		}
		stream->readPtr += offset;
		stream->bytesLeft -= offset;
		InputSourceAlignDecodePointer(stream->readBuf, &stream->readPtr,
			&stream->bytesLeft);
		frameStart = stream->readPtr;
		frameBytes = stream->bytesLeft;

		if (stream->timing) {
			clock_t t0 = clock();
			err = MP3Decode(stream->decoder, &stream->readPtr,
				&stream->bytesLeft, stream->decodeBuf, 0);
			stream->timing->frameDecode += clock() - t0;
		} else {
			err = MP3Decode(stream->decoder, &stream->readPtr,
				&stream->bytesLeft, stream->decodeBuf, 0);
		}
#if defined(AMIGA_M68K)
		/* Poll CTRL-C signal so stop requests are noticed within one frame
		 * decode rather than waiting for the full buffer fill to complete.
		 * SetSignal(0,0) reads without clearing, so the bit stays set for
		 * the WaitIO path that also checks it. */
		if (SetSignal(0, 0) & SIGBREAKF_CTRL_C)
			gPlaybackInterrupted = 1;
#endif
		if (gPlaybackInterrupted)
			break;
		if (err) {
			if (err == ERR_MP3_INDATA_UNDERFLOW &&
				stream->stats->decodedFrames == 0 && frameBytes > 1) {
				stream->readPtr = frameStart + 1;
				stream->bytesLeft = frameBytes - 1;
			} else if (err == ERR_MP3_INDATA_UNDERFLOW) {
				stream->outOfData = 1;
			} else if (err == ERR_MP3_MAINDATA_UNDERFLOW) {
				/* Need more main data from later frames; keep decoding. */
			} else if (stream->stats->decodedFrames == 0 && frameBytes > 1) {
				/* A false-positive first header must not make the whole file fail. */
				stream->readPtr = frameStart + 1;
				stream->bytesLeft = frameBytes - 1;
			} else {
				fprintf(stderr, "decode error %d after %lu frames\n",
					err, stream->stats->decodedFrames);
				stream->decodeError = 1;
				stream->outOfData = 1;
			}
			continue;
		}

		MP3GetLastFrameInfo(stream->decoder, &info);
		UpdateFirstFrameStats(stream->stats, &info);
		if (!stream->effectiveRate) {
			stream->effectiveRate = EffectiveOutputSampleRate(opt, info.samprate);
			stream->stats->outputSampleRate = stream->effectiveRate;
		}

		{
			int outSamps;
			int outChannels;
			int i;
			clock_t t0;

			if (stream->timing)
				t0 = clock();
			if (opt->stereo) {
				if (info.nChans == 1) {
					int frames = info.outputSamps;
					for (i = 0; i < frames; i++) {
						stream->writeBuf[2 * i] = stream->decodeBuf[i];
						stream->writeBuf[2 * i + 1] = stream->decodeBuf[i];
					}
					outSamps = frames * 2;
				} else {
					outSamps = MixFrame(stream->decodeBuf, stream->writeBuf,
						info.outputSamps, info.nChans, 0);
				}
				outChannels = 2;
			} else {
				outChannels = MP3GetOutputChannels(stream->decoder);
				if (info.nChans > 1 && outChannels == 1) {
					memmove(stream->writeBuf, stream->decodeBuf,
						info.outputSamps * sizeof(short));
					outSamps = info.outputSamps;
				} else {
					outSamps = MixFrame(stream->decodeBuf, stream->writeBuf,
						info.outputSamps, info.nChans, 1);
					outChannels = 1;
				}
			}
			if (!opt->fastLowrate && opt->outputRate &&
				info.samprate > opt->outputRate) {
				outSamps = DownsampleFrame(&stream->rateState,
					stream->writeBuf, stream->rateBuf, outSamps,
					info.samprate, opt->outputRate, outChannels);
				memmove(stream->writeBuf, stream->rateBuf,
					outSamps * sizeof(short));
			}
			if (opt->checksum)
				stream->stats->pcmChecksum = UpdatePcmChecksum(
					stream->stats->pcmChecksum, stream->writeBuf, outSamps);
			if (stream->timing)
				stream->timing->pcmConvert += clock() - t0;

			/*
			 * Playback usually has enough room for a whole decoded fast-lowrate
			 * frame.  Convert those samples straight into the caller's chip/work
			 * buffer instead of first filling spill[] and then memcpy()ing it out.
			 * Only the tail that does not fit is kept in spill[] for the next call.
			 */
			{
				int direct;
				int spill;

				direct = outSamps;
				if (direct > maxBytes - produced)
					direct = maxBytes - produced;
				i = 0;
				if (direct >= 4) {
					int direct4 = direct & ~3;

					for (; i < direct4; i += 4) {
						dest[produced + i] = Sample16ToS8(stream->writeBuf[i]);
						dest[produced + i + 1] = Sample16ToS8(stream->writeBuf[i + 1]);
						dest[produced + i + 2] = Sample16ToS8(stream->writeBuf[i + 2]);
						dest[produced + i + 3] = Sample16ToS8(stream->writeBuf[i + 3]);
					}
				}
				for (; i < direct; i++)
					dest[produced + i] = Sample16ToS8(stream->writeBuf[i]);
				produced += direct;

				spill = outSamps - direct;
				if (spill > 0) {
					stream->spillPos = 0;
					stream->spillCount = spill;
					for (i = 0; i < spill; i++)
						stream->spill.interleaved[i] =
							Sample16ToS8(stream->writeBuf[direct + i]);
				}
			}
			stream->stats->outputSamples += (unsigned long)outSamps;
			stream->stats->decodedFrames++;
			if (stream->stats->decodedFrames == 1)
				RADIO_DBG(printf("radio-decode: first successful MP3 frame decode samprate=%d chans=%d output_samps=%d effective_rate=%d produced=%d\n",
					info.samprate, info.nChans, outSamps, stream->effectiveRate, produced);)
		}
	}

	return produced;
}

static int DecodeStreamCopyPlanarSpill(DecodeStream *stream, signed char *left,
	signed char *right, int maxFrames, int *outFrames)
{
	int n;

	if (stream->planarSpillPos >= stream->planarSpillCount) {
		stream->planarSpillPos = 0;
		stream->planarSpillCount = 0;
		return 0;
	}
	n = stream->planarSpillCount - stream->planarSpillPos;
	if (n > maxFrames)
		n = maxFrames;
	memcpy(left + *outFrames, stream->spill.planar[0] + stream->planarSpillPos, n);
	memcpy(right + *outFrames, stream->spill.planar[1] + stream->planarSpillPos, n);
	stream->planarSpillPos += n;
	*outFrames += n;
	if (stream->planarSpillPos >= stream->planarSpillCount) {
		stream->planarSpillPos = 0;
		stream->planarSpillCount = 0;
	}
	return n;
}

/* Stereo streaming writes converted samples straight into Paula's planar buffers. */
static int DecodeStreamFillPlanarS8(DecodeStream *stream, const DecodeOptions *opt,
	signed char *left, signed char *right, int maxFrames)
{
	MP3FrameInfo info;
	int produced;

	produced = 0;
	if (!stream->fakeStereo.configured)
		FakeStereoInit(&stream->fakeStereo, opt->fakeStereo,
			opt->fakeStereoDelay, opt->fakeStereoShift);
	DecodeStreamCopyPlanarSpill(stream, left, right, maxFrames, &produced);
	while (produced < maxFrames && !stream->outOfData && !gPlaybackInterrupted) {
		const short *pcm;
		int frames;
		int channels;
		int nRead;
		int offset;
		int err;
		int i;
		int direct;
		unsigned char *frameStart;
		int frameBytes;
		clock_t t0;

		if (stream->bytesLeft < 2 * MAINBUF_SIZE && !stream->eofReached) {
			nRead = FillReadBuffer(stream->readBuf, stream->readPtr,
				READBUF_SIZE, stream->bytesLeft, stream->input);
			stream->bytesLeft += nRead;
			stream->readPtr = stream->readBuf;
			if (nRead == 0)
				stream->eofReached = 1;
		}

		offset = FindValidatedMpegSync(stream->readPtr, stream->bytesLeft);
		if (offset < 0) {
			if (stream->eofReached)
				break;
			if (stream->bytesLeft > 3) {
				stream->readPtr += stream->bytesLeft - 3;
				stream->bytesLeft = 3;
			}
			continue;
		}
		stream->readPtr += offset;
		stream->bytesLeft -= offset;
		InputSourceAlignDecodePointer(stream->readBuf, &stream->readPtr,
			&stream->bytesLeft);
		frameStart = stream->readPtr;
		frameBytes = stream->bytesLeft;

		if (stream->timing) {
			t0 = clock();
			err = MP3Decode(stream->decoder, &stream->readPtr,
				&stream->bytesLeft, stream->decodeBuf, 0);
			stream->timing->frameDecode += clock() - t0;
		} else {
			err = MP3Decode(stream->decoder, &stream->readPtr,
				&stream->bytesLeft, stream->decodeBuf, 0);
		}
#if defined(AMIGA_M68K)
		if (SetSignal(0, 0) & SIGBREAKF_CTRL_C)
			gPlaybackInterrupted = 1;
#endif
		if (gPlaybackInterrupted)
			break;
		if (err) {
			if (err == ERR_MP3_INDATA_UNDERFLOW &&
				stream->stats->decodedFrames == 0 && frameBytes > 1) {
				stream->readPtr = frameStart + 1;
				stream->bytesLeft = frameBytes - 1;
			} else if (err == ERR_MP3_INDATA_UNDERFLOW) {
				stream->outOfData = 1;
			} else if (err == ERR_MP3_MAINDATA_UNDERFLOW) {
				/* Need more main data from later frames; keep decoding. */
			} else if (stream->stats->decodedFrames == 0 && frameBytes > 1) {
				stream->readPtr = frameStart + 1;
				stream->bytesLeft = frameBytes - 1;
			} else {
				fprintf(stderr, "decode error %d after %lu frames\n",
					err, stream->stats->decodedFrames);
				stream->decodeError = 1;
				stream->outOfData = 1;
			}
			continue;
		}

		MP3GetLastFrameInfo(stream->decoder, &info);
		UpdateFirstFrameStats(stream->stats, &info);
		if (!stream->effectiveRate) {
			stream->effectiveRate = EffectiveOutputSampleRate(opt, info.samprate);
			stream->stats->outputSampleRate = stream->effectiveRate;
		}
		if (stream->timing)
			t0 = clock();

		channels = info.nChans > 1 ? 2 : 1;
		pcm = stream->decodeBuf;
		frames = info.outputSamps / channels;
		if (!opt->fastLowrate && opt->outputRate && info.samprate > opt->outputRate) {
			if (channels == 1) {
				for (i = frames - 1; i >= 0; i--) {
					stream->writeBuf[2 * i] = stream->decodeBuf[i];
					stream->writeBuf[2 * i + 1] = stream->decodeBuf[i];
				}
				pcm = stream->writeBuf;
			} else {
				pcm = stream->decodeBuf;
			}
			frames = DownsampleFrame(&stream->rateState, pcm, stream->rateBuf,
				frames * 2, info.samprate, opt->outputRate, 2) / 2;
			pcm = stream->rateBuf;
			channels = 2;
		}

		if (stream->stats && opt->checksum) {
			if (channels == 2) {
				stream->stats->pcmChecksum = UpdatePcmChecksum(
					stream->stats->pcmChecksum, pcm, frames * 2);
			} else {
				for (i = 0; i < frames; i++) {
					short pair[2];
					pair[0] = pcm[i];
					pair[1] = pcm[i];
					stream->stats->pcmChecksum = UpdatePcmChecksum(
						stream->stats->pcmChecksum, pair, 2);
				}
			}
		}

		direct = frames;
		if (direct > maxFrames - produced)
			direct = maxFrames - produced;
		if (channels == 2) {
			PlanarS8FillTrueStereo(left + produced, right + produced, pcm, direct);
		} else {
			for (i = 0; i < direct; i++) {
				short wl, wr;
				if (stream->fakeStereo.enabled) {
					FakeStereoProcess(&stream->fakeStereo, pcm[i], &wl, &wr);
				} else {
					wl = pcm[i];
					wr = pcm[i];
				}
				left[produced + i] = Sample16ToS8(wl);
				right[produced + i] = Sample16ToS8(wr);
			}
		}
		stream->planarSpillPos = 0;
		stream->planarSpillCount = frames - direct;
		if (channels == 2) {
			PlanarS8FillTrueStereo(stream->spill.planar[0], stream->spill.planar[1],
				pcm + 2 * direct, frames - direct);
		} else {
			for (i = direct; i < frames; i++) {
				int spill = i - direct;
				if (stream->fakeStereo.enabled) {
					short wl, wr;
					FakeStereoProcess(&stream->fakeStereo, pcm[i], &wl, &wr);
					stream->spill.planar[0][spill] = Sample16ToS8(wl);
					stream->spill.planar[1][spill] = Sample16ToS8(wr);
				} else {
					stream->spill.planar[0][spill] = Sample16ToS8(pcm[i]);
					stream->spill.planar[1][spill] = stream->spill.planar[0][spill];
				}
			}
		}
		produced += direct;
		stream->stats->outputSamples += (unsigned long)frames * 2UL;
		stream->stats->decodedFrames++;
		if (stream->timing)
			stream->timing->pcmConvert += clock() - t0;
	}
	return produced;
}

/* Shared status block written by the playback subprocess and read by the GUI
 * timer tick.  Both run in the same AmigaOS process address space so a plain
 * volatile struct is sufficient -- no Exec locking needed for this
 * loosely-consistent, one-writer/one-reader use. */
typedef struct GuiPlaybackStatus {
	volatile int           phase;            /* GUIPLAY_PHASE_* constants below */
	volatile long          spareMs;          /* last measured spare ms before buf end */
	volatile unsigned long underruns;        /* running total underrun count */
	volatile unsigned long decodedFrames;    /* MP3 frames decoded so far */
	volatile int           sampleRate;       /* source/decoder-input rate (Hz) for the progress clock */
	volatile unsigned long halfBufferMs;     /* selected playback half-buffer duration */
	volatile unsigned long fastInputBytes;   /* compressed local input copied to Fast RAM */
	volatile unsigned long runId;          /* playback generation that owns this status */
	volatile int           cleanupComplete;/* audio.device and buffers fully released */
	volatile int           cleanupStage;   /* GUIPLAY_CLEANUP_* diagnostic stage */
	volatile int           startupStage;   /* GUISTART_* diagnostic stage */
	volatile int           requestedRate;
	volatile int           effectiveRate;
	volatile unsigned int  paulaPeriod;
	volatile unsigned long requestedBytes;
	volatile unsigned long tryBytes;
	volatile int           lastError;
	volatile int           openDeviceResult;
	volatile int           radioActive;
	volatile int           radioStatus;
	volatile int           radioBitrateKbps;
	volatile int           radioBufferedBytes;
	volatile int           radioMetaInt;
	volatile char          radioTitle[128];
	volatile char          radioStationName[128];
	volatile char          radioGenre[64];
	volatile char          radioStreamUrl[128];
	volatile char          radioContentType[64];
	volatile char          radioError[128];
} GuiPlaybackStatus;

GuiPlaybackStatus gGuiPlaybackStatus;

/* Seek (fast-forward / rewind) request channel.  Both GUI frontends run in the
 * same AmigaOS address space as the playback child, so a plain volatile pair is
 * enough: the GUI sets gSeekTargetSecs then raises gSeekRequest, and the decode
 * loop consumes it on its next buffer fill (see DecodeStreamApplySeek).  Only
 * seekable local input (file or fast-RAM copy) honours it; radio streams and
 * the non-MP3 generic path ignore it.  Reset per playback in MP3ResetStatics. */
volatile int  gSeekRequest;
volatile long gSeekTargetSecs;

static void GuiCopyVolatileString(volatile char *dst, unsigned long dstSize, const char *src)
{
	unsigned long i;
	if (!dst || dstSize == 0)
		return;
	if (!src)
		src = "";
	for (i = 0; i + 1 < dstSize && src[i]; i++)
		dst[i] = src[i];
	dst[i] = 0;
}

static void GuiPublishRadioMetadata(RadioStream *radio)
{
	if (!radio)
		return;
	gGuiPlaybackStatus.radioActive = 1;
	gGuiPlaybackStatus.radioStatus = (int)Radio_GetStatus(radio);
	gGuiPlaybackStatus.radioBitrateKbps = Radio_GetBitrate(radio);
	gGuiPlaybackStatus.radioBufferedBytes = Radio_GetBufferedBytes(radio);
	gGuiPlaybackStatus.radioMetaInt = Radio_GetMetaInt(radio);
	GuiCopyVolatileString(gGuiPlaybackStatus.radioTitle, sizeof(gGuiPlaybackStatus.radioTitle), Radio_GetTitle(radio));
	GuiCopyVolatileString(gGuiPlaybackStatus.radioStationName, sizeof(gGuiPlaybackStatus.radioStationName), Radio_GetStationName(radio));
	GuiCopyVolatileString(gGuiPlaybackStatus.radioGenre, sizeof(gGuiPlaybackStatus.radioGenre), Radio_GetGenre(radio));
	GuiCopyVolatileString(gGuiPlaybackStatus.radioStreamUrl, sizeof(gGuiPlaybackStatus.radioStreamUrl), Radio_GetStreamUrl(radio));
	GuiCopyVolatileString(gGuiPlaybackStatus.radioContentType, sizeof(gGuiPlaybackStatus.radioContentType), Radio_GetContentType(radio));
	GuiCopyVolatileString(gGuiPlaybackStatus.radioError, sizeof(gGuiPlaybackStatus.radioError), Radio_GetError(radio));
}


static void GuiMarkRadioError(void)
{
	gGuiPlaybackStatus.radioActive = 0;
	gGuiPlaybackStatus.radioStatus = (int)RADIO_STATUS_ERROR;
	gGuiPlaybackStatus.radioBufferedBytes = 0;
	gGuiPlaybackStatus.radioBitrateKbps = 0;
	gGuiPlaybackStatus.radioMetaInt = 0;
	GuiCopyVolatileString(gGuiPlaybackStatus.radioError, sizeof(gGuiPlaybackStatus.radioError), "");
}

static void GuiMarkRadioErrorText(const char *message)
{
	GuiMarkRadioError();
	GuiCopyVolatileString(gGuiPlaybackStatus.radioError, sizeof(gGuiPlaybackStatus.radioError),
		message && message[0] ? message : "radio stream failed");
}

static void GuiMarkRadioStopped(void)
{
	gGuiPlaybackStatus.radioActive = 0;
	gGuiPlaybackStatus.radioStatus = (int)RADIO_STATUS_CLOSED;
	gGuiPlaybackStatus.radioBufferedBytes = 0;
	gGuiPlaybackStatus.radioBitrateKbps = 0;
	gGuiPlaybackStatus.radioMetaInt = 0;
	GuiCopyVolatileString(gGuiPlaybackStatus.radioTitle, sizeof(gGuiPlaybackStatus.radioTitle), "");
	GuiCopyVolatileString(gGuiPlaybackStatus.radioStationName, sizeof(gGuiPlaybackStatus.radioStationName), "");
	GuiCopyVolatileString(gGuiPlaybackStatus.radioGenre, sizeof(gGuiPlaybackStatus.radioGenre), "");
	GuiCopyVolatileString(gGuiPlaybackStatus.radioStreamUrl, sizeof(gGuiPlaybackStatus.radioStreamUrl), "");
	GuiCopyVolatileString(gGuiPlaybackStatus.radioContentType, sizeof(gGuiPlaybackStatus.radioContentType), "");
}

#define GUIPLAY_PHASE_IDLE      0   /* not playing */
#define GUIPLAY_PHASE_BUFFERING 1   /* filling initial buffers */
#define GUIPLAY_PHASE_PLAYING   2   /* steady-state streaming */
#define GUIPLAY_PHASE_UNDERRUN  3   /* underrun just occurred */
#define GUIPLAY_PHASE_DONE      4   /* playback finished normally */
#define GUIPLAY_PHASE_STOPPING  5
#define GUIPLAY_PHASE_ERROR     6
#define GUIPLAY_PHASE_ERROR     6   /* Stop/EOF cleanup is releasing audio */

#define GUIPLAY_CLEANUP_NONE          0
#define GUIPLAY_CLEANUP_ABORT_REAP    1
#define GUIPLAY_CLEANUP_DEVICE_CLOSED 2
#define GUIPLAY_CLEANUP_BUFFERS_FREED 3
#define GUIPLAY_CLEANUP_COMPLETE      4

#define GUISTART_NONE                  0
#define GUISTART_CHILD_ENTERED         10
#define GUISTART_ARGS_READY            20
#define GUISTART_INPUT_OPEN            30
#define GUISTART_INPUT_FOPEN_BEFORE    31
#define GUISTART_INPUT_FOPEN_AFTER     32
#define GUISTART_INPUT_PRELOAD_FASTMEM 35
#define GUISTART_INPUT_PREPARE         40
#define GUISTART_DECODER_ALLOC         50
#define GUISTART_DECODER_CONFIG        60
#define GUISTART_FASTLOWRATE_WARN_BEFORE 61
#define GUISTART_FASTLOWRATE_WARN_AFTER  62
#define GUISTART_PROBE_RATE            70
#define GUISTART_PROBE_RATE_DONE       80
#define GUISTART_STREAM_INIT           90
#define GUISTART_PREFILL               100
#define GUISTART_PREFILL_DONE          110
#define GUISTART_AUDIO_SETUP           120
#define GUISTART_CREATE_PORT           130
#define GUISTART_ALLOC_CHIP_BUFFERS    140
#define GUISTART_CREATE_IOREQUESTS     150
#define GUISTART_OPEN_DEVICE           160
#define GUISTART_OPEN_DEVICE_DONE      170
#define GUISTART_ALLOC_WORK_BUFFERS    180
#define GUISTART_AUDIO_SETUP_DONE      190
#define GUISTART_FILL_BUFFER_A         200
#define GUISTART_FILL_BUFFER_A_DONE    210
#define GUISTART_FILL_BUFFER_B         220
#define GUISTART_FILL_BUFFER_B_DONE    230
#define GUISTART_PREPARE_A             240
#define GUISTART_PREPARE_B             250
#define GUISTART_COMMIT_A              260
#define GUISTART_PLAYING               270
#define GUISTART_FAILED                900
#define GUISTART_CLEANUP               910

#ifdef MINIAMP3_DEBUG
#ifndef MINIAMP3_DEBUG_FMT_PTR
#if defined(AMIGA_M68K)
#define MINIAMP3_DEBUG_FMT_PTR(p) ((ULONG)(p))
#else
#define MINIAMP3_DEBUG_FMT_PTR(p) (p)
#endif
#endif

const char *GuiStartupStageName(int stage)
{
	switch (stage) {
	case GUISTART_NONE: return "none";
	case GUISTART_CHILD_ENTERED: return "child entered";
	case GUISTART_ARGS_READY: return "args ready";
	case GUISTART_INPUT_OPEN: return "input open";
	case GUISTART_INPUT_FOPEN_BEFORE: return "input fopen before";
	case GUISTART_INPUT_FOPEN_AFTER: return "input fopen after";
	case GUISTART_INPUT_PRELOAD_FASTMEM: return "copying input to Fast RAM";
	case GUISTART_INPUT_PREPARE: return "input prepare";
	case GUISTART_DECODER_ALLOC: return "decoder alloc";
	case GUISTART_DECODER_CONFIG: return "decoder config";
	case GUISTART_FASTLOWRATE_WARN_BEFORE: return "fast-lowrate warning gate before";
	case GUISTART_FASTLOWRATE_WARN_AFTER: return "fast-lowrate warning gate after";
	case GUISTART_PROBE_RATE: return "probing input rate";
	case GUISTART_PROBE_RATE_DONE: return "input rate probed";
	case GUISTART_STREAM_INIT: return "stream init";
	case GUISTART_PREFILL: return "prefill decode";
	case GUISTART_PREFILL_DONE: return "prefill done";
	case GUISTART_AUDIO_SETUP: return "audio setup";
	case GUISTART_CREATE_PORT: return "creating msg port";
	case GUISTART_ALLOC_CHIP_BUFFERS: return "allocating chip buffers";
	case GUISTART_CREATE_IOREQUESTS: return "creating IO requests";
	case GUISTART_OPEN_DEVICE: return "opening audio.device";
	case GUISTART_OPEN_DEVICE_DONE: return "audio.device opened";
	case GUISTART_ALLOC_WORK_BUFFERS: return "allocating work buffers";
	case GUISTART_AUDIO_SETUP_DONE: return "audio setup done";
	case GUISTART_FILL_BUFFER_A: return "filling playback buffer A";
	case GUISTART_FILL_BUFFER_A_DONE: return "buffer A filled";
	case GUISTART_FILL_BUFFER_B: return "filling playback buffer B";
	case GUISTART_FILL_BUFFER_B_DONE: return "buffer B filled";
	case GUISTART_PREPARE_A: return "preparing buffer A";
	case GUISTART_PREPARE_B: return "preparing buffer B";
	case GUISTART_COMMIT_A: return "submitting first buffer";
	case GUISTART_PLAYING: return "playing";
	case GUISTART_FAILED: return "failed";
	case GUISTART_CLEANUP: return "cleanup";
	default: return "unknown";
	}
}

static void GuiWriteDetailedStartupLog(int stage)
{
#if defined(AMIGA_M68K) && defined(HAVE_AMIGA_AUDIO_DEVICE)
	{
		BPTR log;
		char line[320];
		int len;

		len = snprintf(line, sizeof(line),
			"runId=%lu stage=%d name=%s requestedRate=%d effectiveRate=%d period=%u requestedBytes=%lu tryBytes=%lu openDeviceResult=%d interrupted=%d task=%p process=%p\n",
			gGuiPlaybackStatus.runId, stage,
			MINIAMP3_DEBUG_FMT_PTR(GuiStartupStageName(stage)),
			gGuiPlaybackStatus.requestedRate, gGuiPlaybackStatus.effectiveRate,
			gGuiPlaybackStatus.paulaPeriod, gGuiPlaybackStatus.requestedBytes,
			gGuiPlaybackStatus.tryBytes, gGuiPlaybackStatus.openDeviceResult,
			gPlaybackInterrupted,
			MINIAMP3_DEBUG_FMT_PTR((void *)FindTask(NULL)),
			MINIAMP3_DEBUG_FMT_PTR((void *)FindTask(NULL)));
		if (len < 0)
			return;
		if (len >= (int)sizeof(line))
			len = (int)sizeof(line) - 1;
		log = Open((STRPTR)"T:MintAMP-startup.log", MODE_READWRITE);
		if (log) {
			Seek(log, 0, OFFSET_END);
			Write(log, line, len);
			Close(log);
		}
	}
#else
	{
		FILE *log = fopen("MintAMP-startup.log", "a");
		if (log) {
			fprintf(log, "runId=%lu stage=%d name=%s\n",
				gGuiPlaybackStatus.runId, stage, GuiStartupStageName(stage));
			fclose(log);
		}
	}
#endif
}
#endif /* MINIAMP3_DEBUG */

static void GuiPublishStartupStage(int stage)
{
	if (gGuiPlaybackStatus.startupStage == stage)
		return;
	gGuiPlaybackStatus.startupStage = stage;
	/* Two hard machine halts (one #80000008, one dead black screen) have
	 * landed in the silent window between radio_stream.c's "first decoder
	 * frame / playback buffer ready" line and the first post-startup pump
	 * line -- exactly the span these stages cover. Emitting them into the
	 * debug log (flushed, so the last one survives the halt) turns "died
	 * somewhere in decoder/audio startup" into a precise stage number. */
	RADIO_DBG(printf("radio-startup: stage=%d\n", stage);)
#ifdef MINIAMP3_DEBUG
	GuiWriteDetailedStartupLog(stage);
#endif
}

static void GuiSetPlaybackPhase(int phase)
{
	if (gGuiPlaybackStatus.phase == phase)
		return;
	gGuiPlaybackStatus.phase = phase;
}

static int AmigaPlaybackStopRequested(const DecodeOptions *opt, const char *where)
{
#if defined(AMIGA_M68K)
	ULONG pending = SetSignal(0, 0);
	if (pending & SIGBREAKF_CTRL_C)
		gPlaybackInterrupted = 1;
#endif
	if (gPlaybackInterrupted) {
#if defined(MINIAMP3_DEBUG)
		printf("miniamp3-debug: Stop observed %s\n", where);
#endif
		if (opt && opt->debugCleanup)
			printf("debug-cleanup: Stop observed %s\n", where);
		return 1;
	}
	return 0;
}

#if !defined(AMIGA_M68K)
static void PlaybackSignalHandler(int signum)
{
	(void)signum;
	gPlaybackInterrupted = 1;
}
#endif

int MP3ResetStatics(void)
{
	extern void AmigaResetPolyphaseStatics(void);

	/* MintAMP calls the CLI main() repeatedly in one GUI process.  Clear
	 * frontend globals and decoder file-scope controls so each playback starts
	 * from the same state as a fresh command-line process.
	 *
	 * The current Helix synthesis and IMDCT overlap buffers are allocated inside
	 * MP3DecInfo by MP3InitDecoder(), so they are already fresh per decode.
	 */
	gPlaybackInterrupted = 0;
	gSeekRequest = 0;
	gSeekTargetSecs = 0;
#ifdef MINTAMP_SKIN
	gSkinAudio.pause_requested=gSkinAudio.paused=0;
	gSkinAudio.channels=gSkinAudio.bitrate=gSkinAudio.eq_supported=0;
	gSkinAudio.completed_ok=0; gSkinAudio.pause_catchup=0;
    gSkinAudio.position_ms=0; gSkinAudio.position_valid=0; gSkinAudio.seek_sequence=0;
    gSkinAudio.visual_sequence=gSkinAudio.visual_request;
    memset((void *)gSkinAudio.visual_pcm,0,sizeof(gSkinAudio.visual_pcm));
#endif
	memset((void *)&gGuiPlaybackStatus, 0, sizeof(gGuiPlaybackStatus));
	gTiming = NULL;
	MP3SetExperimentalHuffman(0);
	AmigaResetPolyphaseStatics();
	return 0;
}

typedef struct PlaybackCleanupStatus {
	unsigned long ioCompleted;
	unsigned long ioAborted;
	unsigned long ioRequestsDeleted;
	unsigned long messagePortsDeleted;
	unsigned long chipBuffersFreed;
	unsigned long workBuffersFreed;
	unsigned long canaryErrors;
	unsigned long devicesClosed;
	/* Buffers whose IO could not be confirmed retired: leaked on purpose
	 * instead of freed, so audio.device cannot DMA into freed memory. */
	unsigned long quarantinedBuffers;
} PlaybackCleanupStatus;

static void PlaybackCleanupStatusInit(PlaybackCleanupStatus *status)
{
	if (status)
		memset(status, 0, sizeof(*status));
}

static void PrintPlaybackCleanupStatus(const DecodeOptions *opt,
	const PlaybackCleanupStatus *status)
{
	/* --debug-cleanup normally gates this (it's noisy for a plain CLI
	 * decode), but a RADIO_DEBUG build already commits to noisy diagnostic
	 * logging everywhere else -- and the playback buffer canary check this
	 * prints is otherwise invisible in every radio session log, since
	 * nothing in the radio streaming path ever passes --debug-cleanup to
	 * the playback child. */
#ifndef RADIO_DEBUG
	if (!opt->debugCleanup || !status)
		return;
#else
	(void)opt;
	if (!status)
		return;
	/* MiniAmp3Printf (see the printf redirect above) silently drops
	 * everything for the entire radio playback child, so route this
	 * specific diagnostic around it instead. */
#undef printf
#define printf RadioDebugUnsuppressedPrintf
#endif
	printf("debug-cleanup: outstanding audio IOs completed/aborted: %lu/%lu\n",
		status->ioCompleted, status->ioAborted);
	printf("debug-cleanup: audio.device closed: %s (%lu)\n",
		status->devicesClosed ? "yes" : "not opened", status->devicesClosed);
	printf("debug-cleanup: IO requests deleted: %lu\n",
		status->ioRequestsDeleted);
	printf("debug-cleanup: message ports deleted: %lu\n",
		status->messagePortsDeleted);
	printf("debug-cleanup: chip buffers freed: %lu\n",
		status->chipBuffersFreed);
	printf("debug-cleanup: work buffers freed: %lu\n",
		status->workBuffersFreed);
	printf("debug-cleanup: playback buffer canaries: %s (%lu errors)\n",
		status->canaryErrors ? "CORRUPTED" : "ok", status->canaryErrors);
	printf("debug-cleanup: buffers quarantined (IO not confirmed retired): %lu\n",
		status->quarantinedBuffers);
#ifdef RADIO_DEBUG
#undef printf
#define printf MiniAmp3Printf
#endif
}

static unsigned int AmigaPalAudioPeriod(int outputRate)
{
	if (outputRate <= 0)
		return 0;
	return (unsigned int)((3546895UL + ((unsigned long)outputRate / 2UL)) /
		(unsigned long)outputRate);
}

/* Keep no more than two live audio.device writes per channel.  The earlier
 * three-request mono ring can leave Stop blocked while audio.device unwinds the
 * queued writes on real hardware.  Arrays remain sized for the stereo Fast-RAM
 * decode-ahead slot C, but only A/B are submitted to Paula. */
#define AMIGA_MONO_AUDIO_SLOTS 3
#define AMIGA_STEREO_AUDIO_SLOTS 2
#define AMIGA_STEREO_DECODE_SLOTS 3
#define AMIGA_AUDIO_PLAYBACK_SLOTS AMIGA_MONO_AUDIO_SLOTS

static int AmigaAudioLiveSlots(int stereo)
{
	return stereo ? AMIGA_STEREO_AUDIO_SLOTS : AMIGA_MONO_AUDIO_SLOTS;
}

#ifdef HAVE_AMIGA_AUDIO_DEVICE
#ifndef NDEBUG
#define PLAYBACK_GUARD_BYTES 16UL
#define PLAYBACK_GUARD_VALUE 0xa5
#else
#define PLAYBACK_GUARD_BYTES 0UL
#endif

/* Cleanup progresses strictly forward through these stages; AmigaAudioClose()
 * records the current one before each group of operations so a duplicate
 * call (or a crash mid-cleanup) can be diagnosed from the last stage logged
 * instead of guessing which step was in flight. */
typedef enum {
	AUDIO_CLEANUP_NOT_STARTED = 0,
	AUDIO_CLEANUP_ABORTING_IO,
	AUDIO_CLEANUP_CLOSING_DEVICE,
	AUDIO_CLEANUP_FREEING_BUFFERS,
	AUDIO_CLEANUP_DONE
} AudioCleanupState;

typedef struct AmigaAudioPlayer {
#ifdef MINTAMP_SKIN
    SkinClock skin_clock;
    unsigned long skin_seek_sequence;
    unsigned char skin_counted[3];
#endif
	struct MsgPort *port;
	struct IOAudio *req[3][2];
	struct IOAudio *closeReq[2]; /* dedicated close request per channel */
	int deviceOpen[2];
	int sent[3][2];
	/* Set by AmigaAudioReapOutstanding() when a request's completion could
	 * not be confirmed (bounded CheckIO spin timed out): audio.device may
	 * still hold/DMA the buffer that request's ioa_Data points at, so
	 * AmigaAudioClose() must not free that slot/channel's chip or work
	 * buffer even though the IORequest itself gets leaked. */
	int ioUnsafe[3][2];
	int prepared[3];
	int stereo;
	unsigned int period;
	signed char *splitBuf[3][2];
	void *splitBase[3][2];
	unsigned long splitTotalBytes[3][2];
	unsigned long splitBytes;
	signed char *splitWorkBuf[3][2];
	void *splitWorkBase[3][2];
	unsigned long splitWorkTotalBytes[3][2];
	unsigned long splitWorkBytes;
	signed char *workBuf[3];
	void *workBase[3];
	unsigned long workTotalBytes[3];
	unsigned long workBytes;
	int workChip;
	volatile int cleanupStarted;
	AudioCleanupState cleanupState;
	int debugCleanup;
	int stopping;
	int outputStride;
	int debugPlay;
	int startupVolumeDebugPrinted;
	unsigned long cleanupInvocationCount;
	unsigned long sessionId;
	UWORD requestVolume;
	unsigned short lastVolumePercent;
	unsigned long lastVolumeSequence;
} AmigaAudioPlayer;

static const char *AmigaAudioCleanupStateName(AudioCleanupState state)
{
	switch (state) {
	case AUDIO_CLEANUP_NOT_STARTED:    return "NOT_STARTED";
	case AUDIO_CLEANUP_ABORTING_IO:    return "ABORTING_IO";
	case AUDIO_CLEANUP_CLOSING_DEVICE: return "CLOSING_DEVICE";
	case AUDIO_CLEANUP_FREEING_BUFFERS:return "FREEING_BUFFERS";
	case AUDIO_CLEANUP_DONE:           return "DONE";
	default:                           return "UNKNOWN";
	}
}

static int PlaybackBufferCanaryOk(const void *base, unsigned long bytes)
{
#ifndef NDEBUG
	const unsigned char *p;
	unsigned long i;

	if (!base)
		return 1;
	p = (const unsigned char *)base;
	for (i = 0; i < PLAYBACK_GUARD_BYTES; i++) {
		if (p[i] != PLAYBACK_GUARD_VALUE ||
			p[PLAYBACK_GUARD_BYTES + bytes + i] != PLAYBACK_GUARD_VALUE)
			return 0;
	}
#else
	(void)base;
	(void)bytes;
#endif
	return 1;
}

#if defined(MINIAMP_AUDIO_WORKBUF_CANARY_DEBUG)
/* Diagnostic only: proactively check a just-filled work-buffer slot's guard
 * bytes right after the decode fill that wrote it. This scans guard bytes and
 * must stay out of normal RC/release playback hot paths unless explicitly
 * enabled with MINIAMP_AUDIO_WORKBUF_CANARY_DEBUG. */
static void AmigaAudioCheckWorkBufferCanary(AmigaAudioPlayer *player, int slot,
	unsigned long lastFillLen, const char *where)
{
	int ch;
	if (!player || !player->stereo)
		return;
	for (ch = 0; ch < 2; ch++) {
		if (player->splitWorkBase[slot][ch] &&
			!PlaybackBufferCanaryOk(player->splitWorkBase[slot][ch], player->splitWorkBytes)) {
			fprintf(stderr,
				"radio-buffer-guard: CORRUPT work buffer slot=%d ch=%d ptr=%p base=%p bytes=%lu lastFillLen=%lu where=%s\n",
				slot, ch, (void *)player->splitWorkBuf[slot][ch],
				player->splitWorkBase[slot][ch], player->splitWorkBytes,
				lastFillLen, where ? where : "");
			fflush(stderr);
		}
	}
}
#endif

static unsigned long AmigaGuardedTotalBytes(unsigned long requestedBytes)
{
	unsigned long total;

	total = requestedBytes + 2UL * PLAYBACK_GUARD_BYTES;
	return (total + 7UL) & ~7UL;
}

static signed char *AmigaAllocGuarded(unsigned long requestedBytes, int chip,
	void **allocationBaseOut, unsigned long *totalAllocationBytesOut)
{
	unsigned long totalAllocationBytes;
	unsigned char *allocationBase;
	signed char *userPointer;

	totalAllocationBytes = AmigaGuardedTotalBytes(requestedBytes);
	allocationBase = (unsigned char *)AllocMem(totalAllocationBytes,
		(chip ? MEMF_CHIP : MEMF_FAST) | MEMF_CLEAR);
	if (!allocationBase)
		return NULL;
	userPointer = (signed char *)(allocationBase + PLAYBACK_GUARD_BYTES);
#ifndef NDEBUG
	memset(allocationBase, PLAYBACK_GUARD_VALUE, PLAYBACK_GUARD_BYTES);
	memset(allocationBase + PLAYBACK_GUARD_BYTES + requestedBytes,
		PLAYBACK_GUARD_VALUE, PLAYBACK_GUARD_BYTES);
#endif
	*allocationBaseOut = allocationBase;
	if (totalAllocationBytesOut)
		*totalAllocationBytesOut = totalAllocationBytes;
#ifdef RADIO_DEBUG
	RadioDebugUnsuppressedPrintf("audio-guard-alloc: base=%p user=%p requested=%lu total=%lu\n",
		(void *)allocationBase, (void *)userPointer, requestedBytes,
		totalAllocationBytes);
#else
	printf("audio-guard-alloc: base=%p user=%p requested=%lu total=%lu\n",
		(void *)allocationBase, (void *)userPointer, requestedBytes,
		totalAllocationBytes);
	fflush(stdout);
#endif
	return userPointer;
}

static void AmigaFreeGuarded(void **allocationBasePtr, void **userPointerPtr,
	unsigned long requestedBytes, unsigned long totalAllocationBytes, int chip,
	PlaybackCleanupStatus *status)
{
	void *allocationBase;
	void *userPointer;

	(void)chip;
	allocationBase = *allocationBasePtr;
	if (!allocationBase)
		return;
	userPointer = userPointerPtr ? *userPointerPtr :
		(void *)((unsigned char *)allocationBase + PLAYBACK_GUARD_BYTES);
	if (totalAllocationBytes == 0)
		totalAllocationBytes = AmigaGuardedTotalBytes(requestedBytes);
#ifdef RADIO_DEBUG
	RadioDebugUnsuppressedPrintf("audio-guard-free: task=%p base=%p user=%p requested=%lu total=%lu\n",
		(void *)FindTask(NULL), allocationBase, userPointer, requestedBytes, totalAllocationBytes);
#else
	printf("audio-guard-free: task=%p base=%p user=%p requested=%lu total=%lu\n",
		(void *)FindTask(NULL), allocationBase, userPointer, requestedBytes, totalAllocationBytes);
	fflush(stdout);
#endif
	if (!PlaybackBufferCanaryOk(allocationBase, requestedBytes) && status)
		status->canaryErrors++;
	FreeMem(allocationBase, totalAllocationBytes);
	Radio_DebugCheckExecMem("audio-guard-free: after FreeMem");
	/* Base and user pointers cleared immediately: the caller's
	 * player->splitBase[]/splitBuf[] (etc.) are the only owners, and leaving
	 * either non-NULL after this FreeMem is what a later cleanup pass would
	 * re-free as AN_FreeTwice. */
	*allocationBasePtr = NULL;
	if (userPointerPtr)
		*userPointerPtr = NULL;
}

static void AmigaAudioCleanupTrace(const AmigaAudioPlayer *player, const char *msg)
{
	/* Same reasoning as PrintPlaybackCleanupStatus() above: --debug-cleanup
	 * is never passed to the radio playback child, so player->debugCleanup
	 * is always 0 there, and MiniAmp3Printf silently drops everything for
	 * the child's whole lifetime regardless -- both gates need bypassing
	 * under RADIO_DEBUG for this per-IORequest cleanup trace to ever be
	 * visible in a radio session log. */
#ifndef RADIO_DEBUG
	if (!player || !player->debugCleanup) {
		(void)msg;
		return;
	}
	printf("debug-cleanup: %s", msg);
#else
	if (!player) { (void)msg; return; }
	RadioDebugUnsuppressedPrintf("debug-cleanup: %s", msg);
#endif
}

static void AmigaAudioCleanupTrace4(const AmigaAudioPlayer *player,
	const char *fmt, unsigned long a, unsigned long b,
	unsigned long c, unsigned long d)
{
#ifndef RADIO_DEBUG
	if (!player || !player->debugCleanup) {
		(void)fmt;
		(void)a;
		(void)b;
		(void)c;
		(void)d;
		return;
	}
	/* Two separate printf() calls build one logical line -- each one taken
	 * on its own is atomic (MiniAmp3Printf locks internally), but the gap
	 * between the two calls is not, so another task's complete message can
	 * land in between them. AmigaOS semaphores are safely re-entrant for
	 * the owning task, so an outer lock spanning both calls nests cleanly
	 * with each call's own inner lock instead of deadlocking. */
	MINIAMP3_IO_LOCK();
	printf("debug-cleanup: ");
	printf(fmt, a, b, c, d);
	MINIAMP3_IO_UNLOCK();
#else
	if (!player) { (void)fmt; (void)a; (void)b; (void)c; (void)d; return; }
	MINIAMP3_IO_LOCK();
	RadioDebugUnsuppressedPrintf("debug-cleanup: ");
	RadioDebugUnsuppressedPrintf(fmt, a, b, c, d);
	MINIAMP3_IO_UNLOCK();
#endif
}

static void AmigaAudioCleanupTraceState(const AmigaAudioPlayer *player,
	const char *msg, AudioCleanupState state, unsigned long count)
{
#ifndef RADIO_DEBUG
	if (!player || !player->debugCleanup)
		return;
	printf("debug-cleanup: %s state=%s cleanup#=%lu\n",
		msg, AmigaAudioCleanupStateName(state), count);
#else
	if (!player)
		return;
	RadioDebugUnsuppressedPrintf("debug-cleanup: %s state=%s cleanup#=%lu\n",
		msg, AmigaAudioCleanupStateName(state), count);
#endif
}

/* Full-detail allocation/free trace for every guarded audio cleanup object:
 * session id, slot, channel, pointer, size, owner (what the buffer is) and
 * the call site that allocated/freed it.  Same gating as
 * AmigaAudioCleanupTrace()/AmigaAudioCleanupTrace4() above. */
static void AmigaAudioLogBufferEvent(const AmigaAudioPlayer *player,
	const char *event, const char *owner, const char *site,
	long slot, long ch, const void *ptr, unsigned long size)
{
	/* task=FindTask(NULL) ties every audio buffer/IORequest free to the task
	 * that actually runs it.  The audio player is torn down by the playback
	 * child, not the GUI/main task, so a recoverable AN_FreeTwice/
	 * AN_BadFreeAddr whose task pointer matches this line is a child-task
	 * defect, and one that does NOT match points the hunt back at the GUI
	 * task's own frees instead. */
#ifndef RADIO_DEBUG
	if (!player || !player->debugCleanup)
		return;
	printf("debug-cleanup: %s owner=%s site=%s task=%p session=%lu slot=%ld ch=%ld ptr=%p size=%lu cleanup#=%lu\n",
		event, owner, site, (void *)FindTask(NULL), player->sessionId, slot, ch, ptr, size,
		player->cleanupInvocationCount);
#else
	if (!player)
		return;
	RadioDebugUnsuppressedPrintf("debug-cleanup: %s owner=%s site=%s task=%p session=%lu slot=%ld ch=%ld ptr=%p size=%lu cleanup#=%lu\n",
		event, owner, site, (void *)FindTask(NULL), player->sessionId, slot, ch, ptr, size,
		player->cleanupInvocationCount);
#endif
}

/* Temporary diagnostic mode (MP3_SAFE_CLEANUP_LEAK_TEST=1 in the environment):
 * AbortIO/WaitIO/reap and CloseDevice run exactly as normal, but chip/work
 * buffers, IORequests and the message port are deliberately leaked instead of
 * freed/deleted.  If the recoverable exec alerts stop happening with this
 * set, the root cause is confirmed to be a use-after-free/double-free
 * somewhere in the normal free/delete path below rather than in AbortIO,
 * WaitIO, or CloseDevice themselves. */
static int AmigaAudioSafeCleanupLeakTestEnabled(void)
{
	static int cached = -1;
	if (cached < 0) {
		const char *v = getenv("MP3_SAFE_CLEANUP_LEAK_TEST");
		cached = (v && *v && *v != '0') ? 1 : 0;
	}
	return cached;
}

/* Test mode (MP3_NO_AUDIO_SINK=1 in the environment): network, SSL and the
 * decoder all run exactly as normal, but audio.device is never opened and no
 * CMD_WRITE is ever submitted -- chip/work buffers still get allocated, and
 * still get torn down by AmigaAudioClose(), but nothing DMAs into them.  If
 * a soak run in this mode never corrupts exec memory, the corruptor is in
 * audio output/cleanup, not in the network/SSL/decoder pipeline. */
static int AmigaAudioNoSinkEnabled(void)
{
	static int cached = -1;
	if (cached < 0) {
		const char *v = getenv("MP3_NO_AUDIO_SINK");
		cached = (v && *v && *v != '0') ? 1 : 0;
	}
	return cached;
}

static unsigned long AmigaAudioDrainReplies(AmigaAudioPlayer *player)
{
	unsigned long drained = 0;

	if (player->port) {
		while (GetMsg(player->port))
			drained++;
	}
	return drained;
}

/* AbortIO() is asynchronous on audio.device: an aborted CMD_WRITE still
 * completes and posts a reply to the port afterwards.  Every submitted
 * request must therefore be reaped with WaitIO() before CloseDevice()/
 * DeleteIORequest(), or the device replies into freed memory -- exec then
 * reports the damage later as recoverable AN_FreeTwice (01000009) /
 * AN_BadFreeAddr (0100000F) alerts (observed as four alerts at app exit
 * after a mid-play Stop aborted four queued writes and forgot them).  The
 * old GetMsg()-drain only collected replies that had already arrived; it
 * could not wait for the aborted ones still in flight.  The wait here is
 * bounded so a wedged unit cannot hang Stop: a request that never
 * completes keeps its sent[] flag and AmigaAudioClose() deliberately
 * leaks its IORequest instead of deleting it. */
static void AmigaAudioReapOutstanding(AmigaAudioPlayer *player)
{
	int i;
	int ch;

	for (i = 0; i < AMIGA_AUDIO_PLAYBACK_SLOTS; i++) {
		for (ch = 0; ch < 2; ch++) {
			struct IORequest *req = (struct IORequest *)player->req[i][ch];
			if (!req || !player->sent[i][ch])
				continue;
#if defined(AMIGA_M68K)
			{
				int spins = 0;
				while (!CheckIO(req) && spins < 50) {
					Delay(1);
					spins++;
				}
				if (!CheckIO(req)) {
					/* Ownership was never handed back by audio.device: it may
					 * still complete this CMD_WRITE and DMA/write into
					 * ioa_Data at any time.  Mark the slot/channel unsafe so
					 * AmigaAudioClose() quarantines (leaks) its buffer and
					 * IORequest instead of freeing them out from under the
					 * device -- freeing here is exactly the corrupt-after-
					 * cleanup bug this file exists to prevent. */
					player->ioUnsafe[i][ch] = 1;
					AmigaAudioCleanupTrace4(player,
						"request index=%ld channel=%ld never completed after AbortIO -- leaking IORequest\n",
						(unsigned long)i, (unsigned long)ch, 0, 0);
					continue;
				}
			}
#endif
			WaitIO(req);
			player->sent[i][ch] = 0;
			player->ioUnsafe[i][ch] = 0;
			AmigaAudioCleanupTrace4(player,
				"request index=%ld channel=%ld reaped after abort\n",
				(unsigned long)i, (unsigned long)ch, 0, 0);
			Radio_DebugCheckExecMem("audio cleanup: after WaitIO/reap");
		}
	}
}

static void AmigaAudioClose(AmigaAudioPlayer *player,
	PlaybackCleanupStatus *status)
{
	int i;
	int ch;
	int leakTest;

	if (!player)
		return;
	leakTest = AmigaAudioSafeCleanupLeakTestEnabled();
	if (leakTest)
		AmigaAudioCleanupTrace(player, "MP3_SAFE_CLEANUP_LEAK_TEST active: buffers/IORequests/message port will be leaked, not freed\n");
	if (player->cleanupStarted) {
		/* Idempotency guard: a second call must never touch objects the
		 * first call already freed/deleted/NULLed.  Log the state the first
		 * call reached and return untouched. */
		AmigaAudioCleanupTraceState(player, "cleanup already started -- ignoring duplicate call",
			player->cleanupState, player->cleanupInvocationCount);
		return;
	}
	player->cleanupStarted = 1;
	player->stopping = 1;
	player->cleanupInvocationCount++;
	AmigaAudioCleanupTrace4(player, "cleanup begin count=%lu outputRate=%ld stride=%ld\n",
		player->cleanupInvocationCount, (unsigned long)gGuiPlaybackStatus.sampleRate,
		(unsigned long)player->outputStride, 0);
	AmigaAudioCleanupTrace(player, "volume control request: none allocated (next-buffer ioa_Volume updates)\n");
	gGuiPlaybackStatus.cleanupStage = GUIPLAY_CLEANUP_ABORT_REAP;
	player->cleanupState = AUDIO_CLEANUP_ABORTING_IO;
	Radio_DebugCheckExecMem("audio cleanup: before AbortIO");
	/* First request cancellation for the entire ring, then reap it in a second
	 * pass.  Waiting one slot at a time lets audio.device advance the next queued
	 * write between AbortIO calls and can prolong or stall Stop while the queue is
	 * being unwound. */
	for (i = 0; i < AMIGA_AUDIO_PLAYBACK_SLOTS; i++) {
		for (ch = 0; ch < 2; ch++) {
			if (player->req[i][ch] && player->sent[i][ch]) {
				int done = CheckIO((struct IORequest *)player->req[i][ch]) != 0;
				int aborted = 0;
				AmigaAudioCleanupTrace4(player, "request index=%ld channel=%ld submitted=%ld CheckIO=%ld\n",
					(unsigned long)i, (unsigned long)ch,
					(unsigned long)player->sent[i][ch], (unsigned long)done);
				if (!done) {
					AbortIO((struct IORequest *)player->req[i][ch]);
					aborted = 1;
					if (status)
						status->ioAborted++;
					Radio_DebugCheckExecMem("audio cleanup: after AbortIO");
				} else if (status) {
					status->ioCompleted++;
				}
				AmigaAudioCleanupTrace4(player, "request index=%ld channel=%ld AbortIO issued=%ld\n",
					(unsigned long)i, (unsigned long)ch, (unsigned long)aborted, 0);
			}
		}
	}
	/* Reap every submitted request before CloseDevice(): AbortIO() only
	 * *requests* cancellation -- the device still completes and replies each
	 * aborted CMD_WRITE afterwards, so closing the unit and deleting the
	 * IORequests with replies still in flight let audio.device write into
	 * freed memory (the source of the recoverable AN_FreeTwice/
	 * AN_BadFreeAddr alerts at app exit after a mid-play Stop).  Aborted
	 * requests complete near-instantly, so this does not stall Stop; see
	 * AmigaAudioReapOutstanding() for the bounded-wait escape hatch. */
	AmigaAudioReapOutstanding(player);
	player->cleanupState = AUDIO_CLEANUP_CLOSING_DEVICE;
	/* CloseDevice after reaping: uses closeReq (not req[0][ch]) so it never
	 * reuses an IORequest that carried a CMD_WRITE. */
	if (player->deviceOpen[0] && player->closeReq[0]) {
		CloseDevice((struct IORequest *)player->closeReq[0]);
		AmigaAudioCleanupTrace4(player, "channel=%ld device closed=1\n", 0, 0, 0, 0);
		player->deviceOpen[0] = 0;
		if (status)
			status->devicesClosed++;
	}
	Radio_DebugCheckExecMem("audio cleanup: after CloseDevice channel 0");
	if (player->deviceOpen[1] && player->closeReq[1]) {
		CloseDevice((struct IORequest *)player->closeReq[1]);
		AmigaAudioCleanupTrace4(player, "channel=%ld device closed=1\n", 1, 0, 0, 0);
		player->deviceOpen[1] = 0;
		if (status)
			status->devicesClosed++;
	}
	Radio_DebugCheckExecMem("audio cleanup: after CloseDevice channel 1");
	gGuiPlaybackStatus.cleanupStage = GUIPLAY_CLEANUP_DEVICE_CLOSED;
	{
		unsigned long drained = AmigaAudioDrainReplies(player);
		AmigaAudioCleanupTrace4(player, "reply drained=%ld\n",
			drained, 0, 0, 0);
	}
	player->cleanupState = AUDIO_CLEANUP_FREEING_BUFFERS;
	for (i = 0; i < AMIGA_AUDIO_PLAYBACK_SLOTS; i++) {
		for (ch = 0; ch < 2; ch++) {
			/* Verify IO ownership before freeing: only free a slot/channel's
			 * chip/work buffer if its IO was never submitted, completed on
			 * its own (CheckIO), or was aborted AND successfully reaped by
			 * AmigaAudioReapOutstanding().  If ioUnsafe is set the device
			 * never handed the buffer back -- quarantine (leak) it instead
			 * of freeing memory audio.device may still be writing into. */
			if (player->splitBase[i][ch]) {
				unsigned long size = (player->stereo && ch == 0) ?
					player->splitBytes * 2UL : player->splitBytes;
				if (player->ioUnsafe[i][ch] || leakTest) {
					AmigaAudioLogBufferEvent(player, "quarantine", "chip-buffer",
						"AmigaAudioClose", i, ch, player->splitBase[i][ch], size);
					player->splitBase[i][ch] = NULL;
					player->splitBuf[i][ch] = NULL;
					player->splitTotalBytes[i][ch] = 0;
					if (player->stereo && ch == 0)
						player->splitBuf[i][1] = NULL;
					if (status)
						status->quarantinedBuffers++;
				} else {
					AmigaAudioLogBufferEvent(player, "free", "chip-buffer",
						"AmigaAudioClose", i, ch, player->splitBase[i][ch], size);
					AmigaFreeGuarded(&player->splitBase[i][ch],
						(void **)&player->splitBuf[i][ch], size,
						player->splitTotalBytes[i][ch], 1, status);
					player->splitTotalBytes[i][ch] = 0;
					player->splitBuf[i][ch] = NULL;
					if (player->stereo && ch == 0)
						player->splitBuf[i][1] = NULL;
					if (status)
						status->chipBuffersFreed++;
				}
				AmigaAudioCleanupTrace4(player, "slot=%ld channel=%ld chip buffer step done\n",
					(unsigned long)i, (unsigned long)ch, 0, 0);
				Radio_DebugCheckExecMem("audio cleanup: after freeing chip buffer");
			}
			if (player->splitWorkBase[i][ch]) {
				if (player->ioUnsafe[i][ch] || leakTest) {
					AmigaAudioLogBufferEvent(player, "quarantine", "work-buffer-split",
						"AmigaAudioClose", i, ch, player->splitWorkBase[i][ch],
						player->splitWorkBytes);
					player->splitWorkBase[i][ch] = NULL;
					player->splitWorkBuf[i][ch] = NULL;
					player->splitWorkTotalBytes[i][ch] = 0;
					if (status)
						status->quarantinedBuffers++;
				} else {
					AmigaAudioLogBufferEvent(player, "free", "work-buffer-split",
						"AmigaAudioClose", i, ch, player->splitWorkBase[i][ch],
						player->splitWorkBytes);
					AmigaFreeGuarded(&player->splitWorkBase[i][ch],
						(void **)&player->splitWorkBuf[i][ch],
						player->splitWorkBytes,
						player->splitWorkTotalBytes[i][ch], 0, status);
					player->splitWorkTotalBytes[i][ch] = 0;
					if (status)
						status->workBuffersFreed++;
				}
				AmigaAudioCleanupTrace4(player, "slot=%ld channel=%ld work buffer step done\n",
					(unsigned long)i, (unsigned long)ch, 0, 0);
				Radio_DebugCheckExecMem("audio cleanup: after freeing work buffer");
			}
		}
		if (player->workBase[i]) {
			/* Mono work buffer is shared by both channels' writes for this
			 * slot: unsafe if either channel's IO was never confirmed retired. */
			int unsafe = player->ioUnsafe[i][0] || player->ioUnsafe[i][1] || leakTest;
			if (unsafe) {
				AmigaAudioLogBufferEvent(player, "quarantine", "work-buffer-mono",
					"AmigaAudioClose", i, -1, player->workBase[i], player->workBytes);
				player->workBase[i] = NULL;
				player->workBuf[i] = NULL;
				player->workTotalBytes[i] = 0;
				if (status)
					status->quarantinedBuffers++;
			} else {
				AmigaAudioLogBufferEvent(player, "free", "work-buffer-mono",
					"AmigaAudioClose", i, -1, player->workBase[i], player->workBytes);
				AmigaFreeGuarded(&player->workBase[i],
					(void **)&player->workBuf[i], player->workBytes,
					player->workTotalBytes[i], player->workChip, status);
				player->workTotalBytes[i] = 0;
				if (status) {
					if (player->workChip)
						status->chipBuffersFreed++;
					else
						status->workBuffersFreed++;
				}
			}
			AmigaAudioCleanupTrace4(player, "slot=%ld work buffer step done\n",
				(unsigned long)i, 0, 0, 0);
			Radio_DebugCheckExecMem("audio cleanup: after freeing work buffer");
		}
	}
	gGuiPlaybackStatus.cleanupStage = GUIPLAY_CLEANUP_BUFFERS_FREED;
	{
		int leaked_requests = 0;
		for (i = 0; i < AMIGA_AUDIO_PLAYBACK_SLOTS; i++) {
			for (ch = 0; ch < 2; ch++) {
				if (!player->req[i][ch])
					continue;
				if (player->sent[i][ch] || player->ioUnsafe[i][ch] || leakTest) {
					/* Never completed after AbortIO (see
					 * AmigaAudioReapOutstanding()): the device may still
					 * reply into it, so deleting it would recreate the
					 * freed-memory alerts this cleanup exists to prevent.
					 * Leak it (and the shared reply port below). */
					AmigaAudioCleanupTrace4(player, "slot=%ld channel=%ld IORequest leaked (still owned by audio.device)\n",
						(unsigned long)i, (unsigned long)ch, 0, 0);
					player->req[i][ch] = NULL;
					leaked_requests++;
					continue;
				}
				DeleteIORequest((struct IORequest *)player->req[i][ch]);
				player->req[i][ch] = NULL;
				AmigaAudioCleanupTrace4(player, "slot=%ld channel=%ld IORequest deleted\n",
					(unsigned long)i, (unsigned long)ch, 0, 0);
				Radio_DebugCheckExecMem("audio cleanup: after deleting IORequest");
				if (status)
					status->ioRequestsDeleted++;
			}
		}
		for (ch = 0; ch < 2; ch++) {
			if (player->closeReq[ch]) {
				if (leakTest) {
					AmigaAudioCleanupTrace4(player, "channel=%ld closeReq leaked (MP3_SAFE_CLEANUP_LEAK_TEST)\n",
						(unsigned long)ch, 0, 0, 0);
					player->closeReq[ch] = NULL;
					continue;
				}
				DeleteIORequest((struct IORequest *)player->closeReq[ch]);
				player->closeReq[ch] = NULL;
				AmigaAudioCleanupTrace4(player, "channel=%ld closeReq deleted\n",
					(unsigned long)ch, 0, 0, 0);
				Radio_DebugCheckExecMem("audio cleanup: after deleting closeReq");
				if (status)
					status->ioRequestsDeleted++;
			}
		}
		if (player->port) {
			if (leaked_requests || leakTest) {
				AmigaAudioCleanupTrace4(player, "message port leaked (%ld unreaped IORequests may still reply to it)\n",
					(unsigned long)leaked_requests, 0, 0, 0);
				player->port = NULL;
			} else {
				Radio_DebugCheckExecMem("audio cleanup: before deleting message port");
				DeleteMsgPort(player->port);
				player->port = NULL;
				AmigaAudioCleanupTrace(player, "message port deleted\n");
				Radio_DebugCheckExecMem("audio cleanup: after deleting message port");
				if (status)
					status->messagePortsDeleted++;
			}
		}
	}
	/* The leak decisions above are recorded; reset sent[]/ioUnsafe[] so a
	 * reused player cannot mistake a future IORequest for a submitted one. */
	for (i = 0; i < AMIGA_AUDIO_PLAYBACK_SLOTS; i++)
		for (ch = 0; ch < 2; ch++) {
			player->sent[i][ch] = 0;
			player->ioUnsafe[i][ch] = 0;
		}
	player->stereo = 0;
	player->period = 0;
	player->splitBytes = 0;
	player->splitWorkBytes = 0;
	player->workBytes = 0;
	player->workChip = 0;
	gGuiPlaybackStatus.cleanupStage = GUIPLAY_CLEANUP_COMPLETE;
	gGuiPlaybackStatus.cleanupComplete = 1;
	player->cleanupState = AUDIO_CLEANUP_DONE;
	AmigaAudioCleanupTraceState(player, "cleanupComplete set=1", player->cleanupState,
		player->cleanupInvocationCount);
	AmigaAudioCleanupTrace4(player, "quarantined buffers=%lu\n",
		status ? status->quarantinedBuffers : 0, 0, 0, 0);
	Radio_DebugCheckExecMem("after audio device cleanup");
}

static int AmigaAudioOpenOne(AmigaAudioPlayer *player, int ch,
	const UBYTE *channels, unsigned long channelCount)
{
	int i;

	{
		int liveSlots = AmigaAudioLiveSlots(player->stereo);

		for (i = 0; i < liveSlots; i++) {
			GuiPublishStartupStage(GUISTART_CREATE_IOREQUESTS);
			player->req[i][ch] = (struct IOAudio *)CreateIORequest(player->port,
				sizeof(struct IOAudio));
			if (!player->req[i][ch])
				return -1;
		}
	}
	player->req[0][ch]->ioa_Request.io_Message.mn_Node.ln_Pri = ADALLOC_MINPREC;
	player->req[0][ch]->ioa_Data = (UBYTE *)channels;
	player->req[0][ch]->ioa_Length = channelCount;
	if (AmigaAudioNoSinkEnabled()) {
		AmigaAudioCleanupTrace4(player, "MP3_NO_AUDIO_SINK active: audio.device ch=%ld not opened\n",
			(unsigned long)ch, 0, 0, 0);
		/* deviceOpen[ch] stays 0: AmigaAudioClose() must not CloseDevice() a
		 * unit that was never OpenDevice()'d. */
	} else {
		int openDeviceResult;

		GuiPublishStartupStage(GUISTART_OPEN_DEVICE);
		if (AmigaPlaybackStopRequested(NULL, "before opening audio.device"))
			return -1;
		openDeviceResult = OpenDevice(AUDIONAME, 0,
			(struct IORequest *)player->req[0][ch], 0);
#ifdef MINIAMP3_DEBUG
		gGuiPlaybackStatus.openDeviceResult = openDeviceResult;
#else
		if (!gMiniAmp3EmbeddedPlayback)
			gGuiPlaybackStatus.openDeviceResult = openDeviceResult;
#endif
		GuiPublishStartupStage(GUISTART_OPEN_DEVICE_DONE);
		if (player->debugPlay) {
			printf("debug-play: audio setup OpenDevice ch=%d rc=%d allocatedChannels=",
				ch, openDeviceResult);
			for (i = 0; i < (int)channelCount; i++)
				printf("%s%u", i ? "," : "", (unsigned int)channels[i]);
			printf("\n");
		}
		if (openDeviceResult != 0)
			return -1;
		player->deviceOpen[ch] = 1;
	}
	{
		/* Allocate a dedicated IORequest for CloseDevice so req[0][ch] remains
		 * free for CMD_WRITE even when CloseDevice is called mid-playback. */
		struct Message message;

		player->closeReq[ch] = (struct IOAudio *)CreateIORequest(player->port,
			sizeof(struct IOAudio));
		if (!player->closeReq[ch])
			return -1;
		message = player->closeReq[ch]->ioa_Request.io_Message;
		memcpy(player->closeReq[ch], player->req[0][ch], sizeof(struct IOAudio));
		player->closeReq[ch]->ioa_Request.io_Message = message;
		player->closeReq[ch]->ioa_Request.io_Message.mn_ReplyPort = player->port;
	}
	{
		for (i = 1; i < AmigaAudioLiveSlots(player->stereo); i++) {
			struct Message message;

			/* Preserve CreateIORequest's private message-node state.  Copying the
			 * opened request over that node can corrupt Exec message-port lists. */
			message = player->req[i][ch]->ioa_Request.io_Message;
			memcpy(player->req[i][ch], player->req[0][ch], sizeof(struct IOAudio));
			player->req[i][ch]->ioa_Request.io_Message = message;
			player->req[i][ch]->ioa_Request.io_Message.mn_ReplyPort = player->port;
		}
	}
	return 0;
}

static int AmigaAudioOpen(AmigaAudioPlayer *player, unsigned int period,
	int stereo, unsigned long maxBytes, int initialVolumePercent)
{
	UBYTE monoChannels[] = { 1, 2, 4, 8 };
	UBYTE leftChannels[] = { 1, 8 };
	UBYTE rightChannels[] = { 2, 4 };
	int i;
	static unsigned long gAmigaAudioSessionCounter = 0;
	memset(player, 0, sizeof(*player));
	player->sessionId = ++gAmigaAudioSessionCounter;
	player->cleanupState = AUDIO_CLEANUP_NOT_STARTED;
	player->period = period;
	player->stereo = stereo;
	player->debugPlay = gMiniAmp3DebugPlayRequested;
	if (initialVolumePercent < 0)
		initialVolumePercent = 0;
	if (initialVolumePercent > 100)
		initialVolumePercent = 100;
	player->lastVolumePercent = (unsigned short)initialVolumePercent;
	player->lastVolumeSequence = gMiniAmp3VolumeSequence;
	player->requestVolume = VolumePercentToAudioDevice(initialVolumePercent);
	if (player->debugPlay) {
		printf("debug-play: audio setup mode=%s outputRate=%d requestedPeriod=%u calculatedPeriod=%u volume=%u maxTotalBytes=%lu perChannelBytes=%lu signedness=signed-8\n",
			stereo ? "stereo" : "mono", gGuiPlaybackStatus.effectiveRate, period,
			AmigaPalAudioPeriod(gGuiPlaybackStatus.effectiveRate),
			(unsigned int)player->requestVolume, maxBytes,
			stereo ? maxBytes / 2UL : maxBytes);
	}
	GuiPublishStartupStage(GUISTART_CREATE_PORT);
	player->port = CreateMsgPort();
	if (!player->port)
		return -1;
	if (stereo) {
		player->splitBytes = maxBytes / 2UL;
		if (player->splitBytes == 0)
			player->splitBytes = 1;
		for (i = 0; i < AMIGA_STEREO_AUDIO_SLOTS; i++) {
			GuiPublishStartupStage(GUISTART_ALLOC_CHIP_BUFFERS);
			/* Allocate one contiguous chip buffer per stereo slot so Paula sees
			 * left at base and right immediately after the per-channel span. */
			player->splitBuf[i][0] = AmigaAllocGuarded(player->splitBytes * 2UL, 1,
				&player->splitBase[i][0],
				&player->splitTotalBytes[i][0]);
			if (!player->splitBuf[i][0]) {
				int wasInterrupted = gPlaybackInterrupted;
				AmigaAudioClose(player, NULL);
				if (!wasInterrupted)
					gPlaybackInterrupted = 0;
				return -1;
			}
			AmigaAudioLogBufferEvent(player, "alloc", "chip-buffer-stereo",
				"AmigaAudioOpen", i, 0, player->splitBase[i][0],
				player->splitBytes * 2UL);
			player->splitBuf[i][1] = player->splitBuf[i][0] + player->splitBytes;
			player->splitBase[i][1] = NULL;
			player->splitTotalBytes[i][1] = 0;
		}
		if (AmigaAudioOpenOne(player, 0, leftChannels, sizeof(leftChannels)) != 0 ||
			AmigaAudioOpenOne(player, 1, rightChannels, sizeof(rightChannels)) != 0) {
			int wasInterrupted = gPlaybackInterrupted;
			AmigaAudioClose(player, NULL);
			if (!wasInterrupted)
				gPlaybackInterrupted = 0;
			return -1;
		}
	} else {
		player->splitBytes = maxBytes;
		for (i = 0; i < AMIGA_AUDIO_PLAYBACK_SLOTS; i++) {
			GuiPublishStartupStage(GUISTART_ALLOC_CHIP_BUFFERS);
			player->splitBuf[i][0] = AmigaAllocGuarded(player->splitBytes, 1,
				&player->splitBase[i][0],
				&player->splitTotalBytes[i][0]);
			if (!player->splitBuf[i][0]) {
				int wasInterrupted = gPlaybackInterrupted;
				AmigaAudioClose(player, NULL);
				if (!wasInterrupted)
					gPlaybackInterrupted = 0;
				return -1;
			}
			AmigaAudioLogBufferEvent(player, "alloc", "chip-buffer-mono",
				"AmigaAudioOpen", i, 0, player->splitBase[i][0],
				player->splitBytes);
		}
		if (AmigaAudioOpenOne(player, 0, monoChannels, sizeof(monoChannels)) != 0) {
			int wasInterrupted = gPlaybackInterrupted;
			AmigaAudioClose(player, NULL);
			if (!wasInterrupted)
				gPlaybackInterrupted = 0;
			return -1;
		}
	}
	return 0;
}

static void AmigaAudioPrepareOne(AmigaAudioPlayer *player, int index,
	int ch, signed char *buf, unsigned long len)
{
	struct IOAudio *req = player->req[index][ch];

	req->ioa_Request.io_Command = CMD_WRITE;
	req->ioa_Request.io_Flags = ADIOF_PERVOL;
	req->ioa_Data = (UBYTE *)buf;
	req->ioa_Length = len;
	req->ioa_Period = player->period;
	req->ioa_Volume = player->requestVolume;
	req->ioa_Cycles = 1;
}


static void AmigaAudioPrintBufferStats(const char *label,
	const signed char *buf, unsigned long len)
{
	unsigned long i;
	unsigned long nonzero;
	unsigned long unsignedSilence80;
	int minv;
	int maxv;

	if (!buf || len == 0) {
		printf("debug-play: %s stats ptr=%p len=%lu empty=1\n",
			label, (const void *)buf, len);
		return;
	}
	minv = buf[0];
	maxv = buf[0];
	nonzero = 0;
	unsignedSilence80 = 0;
	for (i = 0; i < len; i++) {
		int v = buf[i];
		if (v < minv) minv = v;
		if (v > maxv) maxv = v;
		if (v != 0) nonzero++;
		if (((unsigned char)buf[i]) == 0x80U) unsignedSilence80++;
	}
	printf("debug-play: %s stats ptr=%p len=%lu min=%d max=%d nonzero=%lu unsignedSilence0x80=%lu signedSilenceExpected=0 first16=",
		label, (const void *)buf, len, minv, maxv, nonzero, unsignedSilence80);
	for (i = 0; i < len && i < 16UL; i++)
		printf("%s%02x", i ? " " : "", (unsigned int)((unsigned char)buf[i]));
	printf("\n");
}

static void AmigaAudioPrintStartupVolumeDebug(AmigaAudioPlayer *player,
	int index)
{
	if (!player || !player->debugPlay || player->startupVolumeDebugPrinted)
		return;
	player->startupVolumeDebugPrinted = 1;
	printf("debug-play: parsed --volume percent: %u\n",
		(unsigned int)player->lastVolumePercent);
	printf("debug-play: shared requested percent: %u\n",
		(unsigned int)(gMiniAmp3RequestedVolume > 100 ? 100 :
			gMiniAmp3RequestedVolume));
	printf("debug-play: mapped ioa_Volume: %u\n",
		(unsigned int)player->requestVolume);
	if (player->stereo) {
		printf("debug-play: request A channel 0 ioa_Volume: %u\n",
			(unsigned int)player->req[index][0]->ioa_Volume);
		printf("debug-play: request A channel 1 ioa_Volume: %u\n",
			(unsigned int)player->req[index][1]->ioa_Volume);
		printf("debug-play: request flags: ch0=0x%lx ch1=0x%lx\n",
			(unsigned long)player->req[index][0]->ioa_Request.io_Flags,
			(unsigned long)player->req[index][1]->ioa_Request.io_Flags);
	} else {
		printf("debug-play: request A channel 0 ioa_Volume: %u\n",
			(unsigned int)player->req[index][0]->ioa_Volume);
		printf("debug-play: request flags: 0x%lx\n",
			(unsigned long)player->req[index][0]->ioa_Request.io_Flags);
	}
}

static const char *PlaybackBufferName(int index);

static void AmigaAudioCommitOne(AmigaAudioPlayer *player, int index, int ch)
{
	if (gPlaybackInterrupted || player->stopping)
		return;
	if (AmigaAudioNoSinkEnabled()) {
		/* Never submitted: leave sent[index][ch] at 0 so AmigaAudioWait()
		 * does not block on a CMD_WRITE that was never issued, and so
		 * AmigaAudioClose() never AbortIO()/WaitIO()s or frees anything
		 * tied to a request audio.device was never given. */
		return;
	}
	player->req[index][ch]->ioa_Request.io_Command = CMD_WRITE;
	player->req[index][ch]->ioa_Request.io_Flags = ADIOF_PERVOL;
	player->req[index][ch]->ioa_Request.io_Error = 0;
	player->req[index][ch]->ioa_Volume = player->requestVolume;
	player->sent[index][ch] = 1;
	if (player->debugPlay) {
		printf("debug-play: submit buffer=%s ch=%d leftPtr=%p rightPtr=%p ioa_Data=%p ioa_Length=%lu ioa_Period=%u ioa_Volume=%u ioa_Cycles=%u CheckIOBefore=%ld\n",
			PlaybackBufferName(index), ch, (void *)player->splitBuf[index][0],
			(void *)player->splitBuf[index][1],
			(void *)player->req[index][ch]->ioa_Data,
			(unsigned long)player->req[index][ch]->ioa_Length,
			(unsigned int)player->req[index][ch]->ioa_Period,
			(unsigned int)player->req[index][ch]->ioa_Volume,
			(unsigned int)player->req[index][ch]->ioa_Cycles,
			(long)CheckIO((struct IORequest *)player->req[index][ch]));
		AmigaAudioPrintBufferStats(ch ? "right-before-BeginIO" : "left-before-BeginIO",
			(signed char *)player->req[index][ch]->ioa_Data,
			(unsigned long)player->req[index][ch]->ioa_Length);
	}
	BeginIO((struct IORequest *)player->req[index][ch]);
	if (player->debugPlay)
		printf("debug-play: BeginIO called buffer=%s ch=%d result=unavailable(void) io_Error=%ld\n",
			PlaybackBufferName(index), ch,
			(long)player->req[index][ch]->ioa_Request.io_Error);
}

static void AmigaPlaybackCopy(const signed char *src, signed char *dest,
	unsigned long bytes)
{
	CopyMem((APTR)src, (APTR)dest, bytes);
}

static unsigned long PlaybackMaxChunkBytes(int stereo);

static int AmigaAudioPrepare(AmigaAudioPlayer *player, int index,
	signed char *buf, unsigned long len)
{
	if (gPlaybackInterrupted || player->stopping)
		return -1;
	if (len == 0 || len > PlaybackMaxChunkBytes(player->stereo) ||
		(player->stereo && (len & 1UL)))
		return -1;
	if (player->stereo) {
		unsigned long frames = len / 2UL;
		unsigned long i;

		if (frames > player->splitBytes)
			return -1;
		if (player->splitWorkBuf[index][0] && player->splitWorkBuf[index][1]) {
			if (frames > player->splitWorkBytes)
				return -1;
			AmigaPlaybackCopy(player->splitWorkBuf[index][0],
				player->splitBuf[index][0], frames);
			AmigaPlaybackCopy(player->splitWorkBuf[index][1],
				player->splitBuf[index][1], frames);
		} else if (buf) {
			for (i = 0; i < frames; i++) {
				player->splitBuf[index][0][i] = buf[2UL * i];
				player->splitBuf[index][1][i] = buf[2UL * i + 1UL];
			}
		} else if (!player->splitBuf[index][0] || !player->splitBuf[index][1]) {
			return -1;
		}
		if (player->debugPlay)
			printf("debug-play: planar stereo layout buffer %s combinedLen=%lu perChannelLen=%lu leftBase=%p rightBase=%p expectedRight=%p rightMatchesExpected=%d ioa_Length(per-channel)=%lu source=%s\n",
				PlaybackBufferName(index), len, frames,
				(void *)player->splitBuf[index][0], (void *)player->splitBuf[index][1],
				(void *)(player->splitBuf[index][0] + frames),
				player->splitBuf[index][1] == player->splitBuf[index][0] + frames ? 1 : 0,
				frames,
				player->splitWorkBuf[index][0] ? "planar-work" : (buf ? "interleaved-copy" : "chip-planar"));
		AmigaAudioPrepareOne(player, index, 1, player->splitBuf[index][1], frames);
		AmigaAudioPrepareOne(player, index, 0, player->splitBuf[index][0], frames);
	} else {
		if (!buf || len > player->splitBytes)
			return -1;
		if (player->splitBuf[index][0] && buf != player->splitBuf[index][0]) {
			AmigaPlaybackCopy(buf, player->splitBuf[index][0], len);
			buf = player->splitBuf[index][0];
		}
		AmigaAudioPrepareOne(player, index, 0, buf, len);
	}
	player->prepared[index] = 1;
	return 0;
}

static void AmigaAudioRefreshRequestedVolume(AmigaAudioPlayer *player)
{
	unsigned long seq;
	unsigned short percent;

	if (!player || player->stopping || player->cleanupStarted)
		return;
	seq = gMiniAmp3VolumeSequence;
	percent = gMiniAmp3RequestedVolume;
	if (percent > 100)
		percent = 100;
	if (seq != player->lastVolumeSequence) {
		player->lastVolumeSequence = seq;
		player->lastVolumePercent = percent;
		player->requestVolume = VolumePercentToAudioDevice(percent);
	}
}

static void AmigaAudioApplyPreparedVolume(AmigaAudioPlayer *player, int index)
{
	if (!player || !player->prepared[index])
		return;
	if (player->req[index][0])
		player->req[index][0]->ioa_Volume =
#ifdef MINTAMP_SKIN
            VolumePercentToAudioDevice(skin_balance_volume(player->lastVolumePercent,gSkinAudio.balance,0));
#else
            player->requestVolume;
#endif
	if (player->stereo && player->req[index][1])
		player->req[index][1]->ioa_Volume =
#ifdef MINTAMP_SKIN
            VolumePercentToAudioDevice(skin_balance_volume(player->lastVolumePercent,gSkinAudio.balance,1));
#else
            player->requestVolume;
#endif
}

#ifdef MINTAMP_SKIN
/* Count output samples once per completed paired slot, including the
 * slots drained for Pause. This handles MPEG-2/2.5 and variable-size decoder
 * blocks without the native UI's historical frames*1152 assumption. */
static void AmigaAudioSkinComplete(AmigaAudioPlayer *player,int index)
{
    if (player->skin_counted[index]) return;
    player->skin_counted[index]=1;
    skin_clock_advance(&player->skin_clock,player->req[index][0]->ioa_Length,
                       gGuiPlaybackStatus.effectiveRate);
    gSkinAudio.position_ms=player->skin_clock.milliseconds;
}
/* Finish queued DMA without reaping requests here. The regular ring still
 * owns/reaps every completion after resume; decoder and prepared PCM survive. */
static int AmigaAudioSkinPause(AmigaAudioPlayer *player)
{
    int i,ch;
    if (!gSkinAudio.pause_requested) return 0;
    ++gSkinAudio.pause_epoch;
    for (i=0;i<AMIGA_AUDIO_PLAYBACK_SLOTS;++i)
        for (ch=0;ch<(player->stereo ? 2 : 1);++ch)
            if (player->sent[i][ch]) {
                struct IORequest *r=(struct IORequest *)player->req[i][ch];
                while (!CheckIO(r)) {
                    if (!gSkinAudio.pause_requested) return 0;
                    if (Wait((1UL<<player->port->mp_SigBit)|SIGBREAKF_CTRL_C|SIGBREAKF_CTRL_D)&SIGBREAKF_CTRL_C) {
                        gPlaybackInterrupted=1; return -1;
                    }
                }
                if (ch==0) AmigaAudioSkinComplete(player,i);
            }
    if (!gSkinAudio.pause_requested) return 0;
    gSkinAudio.paused=1;
    while (gSkinAudio.pause_requested && !gPlaybackInterrupted)
        if (Wait(SIGBREAKF_CTRL_C|SIGBREAKF_CTRL_D)&SIGBREAKF_CTRL_C)
            gPlaybackInterrupted=1;
    gSkinAudio.paused=0;
    return gPlaybackInterrupted ? -1 : 0;
}
static void AmigaAudioSkinSnapshot(AmigaAudioPlayer *player,int index)
{
    int n;
    unsigned long request=gSkinAudio.visual_request;
    const signed char *left=(const signed char *)player->req[index][0]->ioa_Data;
    const signed char *right=player->stereo ? (const signed char *)player->req[index][1]->ioa_Data : left;
    gSkinAudio.output_stereo=player->stereo;
    gSkinAudio.output_rate=gGuiPlaybackStatus.effectiveRate;
    if (request==gSkinAudio.visual_sequence || player->req[index][0]->ioa_Length<SKIN_VIS_SAMPLES) return;
    for (n=0;n<SKIN_VIS_SAMPLES;++n) gSkinAudio.visual_pcm[n]=(signed char)(((int)left[n]+right[n])/2);
    gSkinAudio.visual_sequence=request;
}
#endif
static int AmigaAudioCommit(AmigaAudioPlayer *player, int index)
{
	if (AmigaPlaybackStopRequested(NULL, "before first buffer submission"))
		return -1;
	if (gPlaybackInterrupted || player->stopping)
		return -1;
	if (!player->prepared[index])
		return -1;
#ifdef MINTAMP_SKIN
    if (AmigaAudioSkinPause(player)!=0) return -1;
    if (player->skin_seek_sequence!=gSkinAudio.seek_sequence) {
        int slot;
        player->skin_seek_sequence=gSkinAudio.seek_sequence;
        player->skin_clock.milliseconds=(unsigned long)gSkinAudio.seek_seconds*1000UL;
        player->skin_clock.remainder=0;
        for (slot=0;slot<3;++slot) player->skin_counted[slot]=1;
        gSkinAudio.position_ms=player->skin_clock.milliseconds;
    }
    player->skin_counted[index]=0; gSkinAudio.position_valid=1;
    AmigaAudioSkinSnapshot(player,index);
#endif
	AmigaAudioRefreshRequestedVolume(player);
	AmigaAudioApplyPreparedVolume(player, index);
	AmigaAudioPrintStartupVolumeDebug(player, index);
	if (player->stereo) {
#if defined(AMIGA_M68K)
		Forbid();
#endif
		AmigaAudioCommitOne(player, index, 1);
		AmigaAudioCommitOne(player, index, 0);
#if defined(AMIGA_M68K)
		Permit();
#endif
	} else {
		AmigaAudioCommitOne(player, index, 0);
	}
	player->prepared[index] = 0;
	return 0;
}

static int AmigaAudioDone(AmigaAudioPlayer *player, int index)
{
	if (player->stereo) {
		if (!player->sent[index][0] || !player->sent[index][1])
			return 0;
		return CheckIO((struct IORequest *)player->req[index][0]) != 0 &&
			CheckIO((struct IORequest *)player->req[index][1]) != 0;
	}
	if (!player->sent[index][0])
		return 0;
	return CheckIO((struct IORequest *)player->req[index][0]) != 0;
}

static int AmigaAudioAbortOutstanding(AmigaAudioPlayer *player);

static int AmigaAudioWaitOne(AmigaAudioPlayer *player, int index, int ch)
{
	struct IORequest *req;
	int err;

	req = (struct IORequest *)player->req[index][ch];
#if defined(AMIGA_M68K)
	while (!CheckIO(req)) {
		ULONG sigs = (1UL << player->port->mp_SigBit) | SIGBREAKF_CTRL_C;
		ULONG got = Wait(sigs);
		if (got & SIGBREAKF_CTRL_C) {
			gPlaybackInterrupted = 1;
			player->stopping = 1;
			/* Abort and reap the WHOLE ring, not just this request: the
			 * old inline path aborted only req, then closed the device
			 * and forgot every other in-flight write -- audio.device kept
			 * replying into what cleanup later freed (the AN_FreeTwice/
			 * AN_BadFreeAddr alerts at app exit). */
			AmigaAudioAbortOutstanding(player);
			return -1;
		}
	}
#endif
	err = WaitIO(req);
	if (!err)
		err = player->req[index][ch]->ioa_Request.io_Error;
	if (player->debugPlay)
		printf("debug-play: WaitIO buffer=%s ch=%d result=%d io_Error=%d CheckIOAfter=%ld\n",
			PlaybackBufferName(index), ch, err,
			(int)player->req[index][ch]->ioa_Request.io_Error, (long)CheckIO(req));
#ifdef MINTAMP_SKIN
    if (!err && ch==0) AmigaAudioSkinComplete(player,index);
#endif
	player->sent[index][ch] = 0;
	return err;
}

static int AmigaAudioAbortOutstanding(AmigaAudioPlayer *player)
{
	int i;
	int ch;

	if (!player)
		return 0;
	player->stopping = 1;
	/* Cancel every queued request before waiting for any one request. */
	for (i = 0; i < AMIGA_AUDIO_PLAYBACK_SLOTS; i++) {
		for (ch = 0; ch < 2; ch++) {
			struct IORequest *req = (struct IORequest *)player->req[i][ch];
			if (req && player->sent[i][ch] && !CheckIO(req))
				AbortIO(req);
		}
	}
	/* Reap the aborted requests before CloseDevice(): an aborted CMD_WRITE
	 * still completes and replies, and closing/deleting under it is what
	 * produced the AN_FreeTwice/AN_BadFreeAddr alerts at app exit.  See
	 * AmigaAudioReapOutstanding(). */
	AmigaAudioReapOutstanding(player);
	for (ch = 0; ch < 2; ch++) {
		if (player->deviceOpen[ch] && player->closeReq[ch]) {
			CloseDevice((struct IORequest *)player->closeReq[ch]);
			player->deviceOpen[ch] = 0;
		}
	}
	AmigaAudioDrainReplies(player);
	return -1;
}

static int AmigaAudioWait(AmigaAudioPlayer *player, int index)
{
	int err;

	/* Stop can arrive while high-rate stereo has multiple Paula writes queued.
	 * Abort and reap the whole ring before returning so cleanup never closes an
	 * audio.device unit or frees a chip buffer that its paired channel may still
	 * be DMA-reading. */
	if (gPlaybackInterrupted)
		return AmigaAudioAbortOutstanding(player);
	err = 0;
	if (player->sent[index][0])
		err = AmigaAudioWaitOne(player, index, 0);
	if (gPlaybackInterrupted) {
		int err2 = AmigaAudioAbortOutstanding(player);
		if (!err)
			err = err2;
	#ifdef MINTAMP_SKIN
    if (!err && !gPlaybackInterrupted && gSkinAudio.pause_requested)
        err=AmigaAudioSkinPause(player);
#endif
	return err;
	}
	if (player->stereo && player->sent[index][1]) {
		int err2 = AmigaAudioWaitOne(player, index, 1);
		if (!err)
			err = err2;
	}
#ifdef MINTAMP_SKIN
    if (!err && !gPlaybackInterrupted && gSkinAudio.pause_requested)
        err=AmigaAudioSkinPause(player);
#endif
	return err;
}

static int AmigaAudioAllocWorkBuffers(AmigaAudioPlayer *player, int stereo,
	unsigned long bytes)
{
	int i;

	if (stereo) {
		player->splitWorkBytes = bytes / 2UL;
		if (player->splitWorkBytes == 0)
			player->splitWorkBytes = 1;
		for (i = 0; i < AMIGA_AUDIO_PLAYBACK_SLOTS; i++) {
			int ch;
			for (ch = 0; ch < 2; ch++) {
				player->splitWorkBuf[i][ch] =
					AmigaAllocGuarded(player->splitWorkBytes, 0,
						&player->splitWorkBase[i][ch],
						&player->splitWorkTotalBytes[i][ch]);
				if (!player->splitWorkBuf[i][ch])
					return -1;
				AmigaAudioLogBufferEvent(player, "alloc", "work-buffer-split",
					"AmigaAudioAllocWorkBuffers", i, ch,
					player->splitWorkBase[i][ch], player->splitWorkBytes);
			}
		}
	} else {
		player->workBytes = bytes;
		player->workChip = 0;
		for (i = 0; i < AMIGA_AUDIO_PLAYBACK_SLOTS; i++) {
			player->workBuf[i] = AmigaAllocGuarded(bytes, player->workChip,
				&player->workBase[i], &player->workTotalBytes[i]);
			if (!player->workBuf[i])
				return -1;
			AmigaAudioLogBufferEvent(player, "alloc", "work-buffer-mono",
				"AmigaAudioAllocWorkBuffers", i, -1,
				player->workBase[i], bytes);
		}
	}
	return 0;
}
#else
typedef struct AmigaAudioPlayer {
	int stereo;
	int sent[3][2];
	int prepared[3];
	signed char *splitBuf[3][2];
	unsigned long splitBytes;
	signed char *splitWorkBuf[3][2];
	unsigned long splitWorkBytes;
	signed char *workBuf[3];
	unsigned long workBytes;
	int debugCleanup;
	int stopping;
	int outputStride;
	int debugPlay;
	int startupVolumeDebugPrinted;
	UWORD requestVolume;
	unsigned short lastVolumePercent;
	unsigned long lastVolumeSequence;
} AmigaAudioPlayer;
static void AmigaAudioClose(AmigaAudioPlayer *player,
	PlaybackCleanupStatus *status)
{
	int i;
	gGuiPlaybackStatus.cleanupStage = GUIPLAY_CLEANUP_ABORT_REAP;
	for (i = 0; i < AMIGA_AUDIO_PLAYBACK_SLOTS; i++) {
		if (player->workBuf[i]) {
			free(player->workBuf[i]);
			player->workBuf[i] = NULL;
			if (status)
				status->workBuffersFreed++;
		}
	}
	gGuiPlaybackStatus.cleanupStage = GUIPLAY_CLEANUP_COMPLETE;
	gGuiPlaybackStatus.cleanupComplete = 1;
}
static int AmigaAudioOpen(AmigaAudioPlayer *player, unsigned int period,
	int stereo, unsigned long maxBytes, int initialVolumePercent)
{
	(void)player;
	(void)period;
	(void)stereo;
	(void)maxBytes;
	(void)initialVolumePercent;
	fprintf(stderr, "--play requires an AmigaOS audio.device build\n");
	return -1;
}
static int AmigaAudioPrepare(AmigaAudioPlayer *player, int index,
	signed char *buf, unsigned long len)
{
	(void)buf;
	if (len == 0 || (player->stereo && (len & 1UL)))
		return -1;
	player->prepared[index] = 1;
	return 0;
}
static void AmigaAudioPrintStartupVolumeDebug(AmigaAudioPlayer *player,
	int index)
{ (void)player; (void)index; }
static int AmigaAudioCommit(AmigaAudioPlayer *player, int index)
{
	if (!player->prepared[index])
		return -1;
	player->sent[index][0] = 1;
	if (player->stereo)
		player->sent[index][1] = 1;
	player->prepared[index] = 0;
	return 0;
}
static int AmigaAudioDone(AmigaAudioPlayer *player, int index)
{ (void)player; (void)index; return 1; }
static int AmigaAudioWait(AmigaAudioPlayer *player, int index)
{ player->sent[index][0] = 0; player->sent[index][1] = 0; return 0; }
static int AmigaAudioAllocWorkBuffers(AmigaAudioPlayer *player, int stereo,
	unsigned long bytes)
{
	int i;
	(void)stereo;
	player->workBytes = bytes;
	for (i = 0; i < AMIGA_AUDIO_PLAYBACK_SLOTS; i++) {
		player->workBuf[i] = (signed char *)malloc(bytes);
		if (!player->workBuf[i])
			return -1;
	}
	return 0;
}
#endif

static unsigned long PlaybackMaxChunkBytes(int stereo)
{
	return AMIGA_AUDIO_MAX_CHANNEL_BYTES * (stereo ? 2UL : 1UL);
}

static unsigned long AlignPlaybackChunkBytes(unsigned long bytes, int stereo)
{
	unsigned long maxBytes;

	/*
	 * Both streaming playback and --decode-then-play eventually submit these
	 * chunks through audio.device.  Keep every per-channel CMD_WRITE length below
	 * Paula's 16-bit DMA length boundary; otherwise a 22050 Hz multi-second
	 * buffer can play only its wrapped/truncated beginning while the decoder has
	 * already advanced to the next much-later chunk.
	 */
	maxBytes = PlaybackMaxChunkBytes(stereo);
	if (bytes > maxBytes)
		bytes = maxBytes;
	if (stereo && (bytes & 1UL))
		bytes--;
	if (bytes == 0)
		bytes = stereo ? 2UL : 1UL;
	return bytes;
}

static unsigned long PlaybackRequestedChunkBytes(const DecodeOptions *opt,
	int playbackRate)
{
	if (playbackRate <= 0)
		playbackRate = opt->outputRate > 0 ? opt->outputRate : 8287;
	return (unsigned long)playbackRate *
		(unsigned long)opt->bufferSeconds * (opt->stereo ? 2UL : 1UL);
}

static unsigned long PlaybackMaxHalfBufferMilliseconds(const DecodeOptions *opt,
	int playbackRate)
{
	unsigned long channels;
	unsigned long maxBytes;

	if (playbackRate <= 0)
		return 0;
	channels = opt->stereo ? 2UL : 1UL;
	maxBytes = PlaybackMaxChunkBytes(opt->stereo);
	return ((maxBytes / channels) * 1000UL) / (unsigned long)playbackRate;
}

static int PlaybackHalfBufferSamples(const DecodeOptions *opt,
	unsigned long chunkBytes)
{
	unsigned long channels;

	channels = opt->stereo ? 2UL : 1UL;
	if (chunkBytes == 0)
		return 0;
	return (int)(chunkBytes / channels);
}

static unsigned long PlaybackBufferDurationMilliseconds(const DecodeOptions *opt,
	unsigned long bytes, int playbackRate)
{
	unsigned long channels;
	unsigned long samples;

	channels = opt->stereo ? 2UL : 1UL;
	if (playbackRate <= 0 || bytes == 0)
		return 0;
	samples = bytes / channels;
	return (samples * 1000UL) / (unsigned long)playbackRate;
}

/* NOTE: clock() on AmigaOS/libnix uses CLOCKS_PER_SEC = 50 (VBL-based),
 * giving ~20ms resolution.  Reported spare times are therefore quantised
 * in 20ms steps; true underruns may appear as spareMs == 0 even when a
 * few ms of genuine headroom existed.  False-positive underrun counts at
 * low spare values (< 20ms) are expected and not indicative of audible
 * glitches. */
static unsigned long PlaybackElapsedMilliseconds(clock_t start, clock_t end)
{
	if (start == (clock_t)-1 || end == (clock_t)-1 || end <= start)
		return 0;
	return ClocksToMilliseconds((clock_t)((unsigned long)end - (unsigned long)start));
}

static const char *PlaybackBufferName(int index)
{
	return index == 0 ? "A" : (index == 1 ? "B" : "C");
}

static void PrintPlaybackFillDebug(const DecodeOptions *opt, int index,
	unsigned long bytes)
{
	unsigned long channels;

	if (!opt->debugPlay)
		return;
	channels = opt->stereo ? 2UL : 1UL;
	printf("debug-play: buffer %s fill samples/bytes: %lu/%lu\n",
		PlaybackBufferName(index), bytes / channels, bytes);
}

static int PlaybackBufferPeakS8(const signed char *buf, unsigned long len)
{
	int peak;
	unsigned long i;

	peak = 0;
	for (i = 0; i < len; i++) {
		int v = buf[i];
		if (v < 0)
			v = -v;
		if (v > peak)
			peak = v;
	}
	return peak;
}

/*
 * Honour a pending GUI seek request.  Estimates the byte offset of the target
 * second from the source bitrate (exact for CBR, close enough for VBR) and
 * repositions the input there, then flushes all decode-side buffers so the loop
 * re-syncs to the next frame.  The progress counter is re-anchored to the new
 * position (plus the pipeline half-buffer the GUI subtracts back off) so the
 * time read-out jumps immediately.  A couple of ERR_MP3_MAINDATA_UNDERFLOW
 * frames right after the jump are expected and handled by the decode loop.
 */
static void DecodeStreamApplySeek(DecodeStream *stream, const DecodeOptions *opt)
{
	InputSource *input = stream->input;
	long targetSecs;
	int sourceRate;
	int bitrate;
	unsigned long bytesPerSec;
	unsigned long baseOffset;
	unsigned long byteOffset;
	unsigned long frames;
	unsigned long halfMs;
	unsigned long compSecs;

	gSeekRequest = 0;
	if (!input || input->radio)
		return;
	sourceRate = stream->stats ? stream->stats->sampleRate : 0;
	bitrate    = stream->stats ? stream->stats->bitrate : 0;
	if (sourceRate <= 0 || bitrate <= 0)
		return;                 /* nothing decoded yet -- can't place the seek */

	targetSecs = gSeekTargetSecs;
	if (targetSecs < 0)
		targetSecs = 0;

	bytesPerSec = (unsigned long)bitrate / 8UL;
	baseOffset = input->info.firstFrameFound ?
		input->info.firstFrameOffset : input->info.id3v2SkipBytes;
	byteOffset = baseOffset + (unsigned long)targetSecs * bytesPerSec;
	if (input->memory && byteOffset > input->memorySize)
		byteOffset = input->memorySize;

	InputSourceSeek(input, byteOffset);
	/* The prefix buffer holds the first bytes of the file; after a seek those
	 * bytes are at the wrong position, so mark it fully consumed. */
	input->prefixPos = input->prefixSize;

	stream->readPtr = stream->readBuf;
	stream->bytesLeft = 0;
	stream->eofReached = 0;
	stream->outOfData = 0;
	stream->decodeError = 0;
	stream->spillPos = 0;
	stream->spillCount = 0;
	stream->planarSpillPos = 0;
	stream->planarSpillCount = 0;
	stream->rateState.phase = 0;

	frames = (unsigned long)targetSecs * (unsigned long)sourceRate / 1152UL;
	halfMs = gGuiPlaybackStatus.halfBufferMs;
	compSecs = halfMs ? (halfMs + 999UL) / 1000UL : (unsigned long)opt->bufferSeconds;
	frames += compSecs * (unsigned long)sourceRate / 1152UL;
#ifdef MINTAMP_SKIN
    gSkinAudio.seek_seconds=(int)targetSecs; ++gSkinAudio.seek_sequence;
#endif
	stream->stats->decodedFrames = frames;
	gGuiPlaybackStatus.decodedFrames = frames;
}

static unsigned long DecodeStreamFillPlaybackBuffer(DecodeStream *stream,
	const DecodeOptions *opt, AmigaAudioPlayer *player, int index,
	signed char *buf, unsigned long maxBytes)
{
	if (gPlaybackInterrupted)
		return 0;
	if (gSeekRequest)
		DecodeStreamApplySeek(stream, opt);
	if (opt->stereo) {
		signed char *left = player->splitWorkBuf[index][0] ?
			player->splitWorkBuf[index][0] : player->splitBuf[index][0];
		signed char *right = player->splitWorkBuf[index][1] ?
			player->splitWorkBuf[index][1] : player->splitBuf[index][1];
		int frames;

		if (!left || !right)
			return 0;
		frames = DecodeStreamFillPlanarS8(stream, opt, left, right,
			(int)(maxBytes / 2UL));
		if (gPlaybackInterrupted)
			return 0;
		return (unsigned long)frames * 2UL;
	}
	{
		int bytes = DecodeStreamFillS8(stream, opt, buf, (int)maxBytes);
		if (gPlaybackInterrupted || bytes < 0)
			return 0;
		return (unsigned long)bytes;
	}
}


static int AmigaAudioCopyStereoDecodeAheadToSlot(AmigaAudioPlayer *player,
	int dest, int src, unsigned long len)
{
	unsigned long frames = len / 2UL;

	if (!player->stereo || (len & 1UL) || frames > player->splitWorkBytes ||
		!player->splitWorkBuf[src][0] || !player->splitWorkBuf[src][1] ||
		!player->splitWorkBuf[dest][0] || !player->splitWorkBuf[dest][1])
		return -1;
	memcpy(player->splitWorkBuf[dest][0], player->splitWorkBuf[src][0],
		(size_t)frames);
	memcpy(player->splitWorkBuf[dest][1], player->splitWorkBuf[src][1],
		(size_t)frames);
	return 0;
}

static int AmigaAudioPreparePlaybackBuffer(AmigaAudioPlayer *player, int index,
	signed char *buf, unsigned long len)
{
	return AmigaAudioPrepare(player, index, buf, len);
}

static void FillDebugToneBuffer(const DecodeOptions *opt,
	AmigaAudioPlayer *player, int index, signed char *buf,
	unsigned long len)
{
	unsigned long i;

	if (opt->stereo) {
		unsigned long frames = len / 2UL;
		signed char *left = player->splitWorkBuf[index][0] ?
			player->splitWorkBuf[index][0] : player->splitBuf[index][0];
		signed char *right = player->splitWorkBuf[index][1] ?
			player->splitWorkBuf[index][1] : player->splitBuf[index][1];

		for (i = 0; i < frames; i++) {
			signed char v = (i & 32UL) ? 96 : -96;
			left[i] = v;
			right[i] = (signed char)-v;
		}
		printf("debug-play: debug tone filled planar signed-8 buffer %s leftBase=%p rightBase=%p perChannelLen=%lu\n",
			PlaybackBufferName(index), (void *)left, (void *)right, frames);
	} else {
		for (i = 0; i < len; i++)
			buf[i] = (i & 32UL) ? 96 : -96;
		printf("debug-play: debug tone filled mono signed-8 buffer %s base=%p len=%lu\n",
			PlaybackBufferName(index), (void *)buf, len);
	}
}

static int AmigaAudioCommitPlaybackBuffer(AmigaAudioPlayer *player, int index)
{
	return AmigaAudioCommit(player, index);
}

static int PlaybackBufferPeak(const DecodeOptions *opt,
	const AmigaAudioPlayer *player, int index, const signed char *buf,
	unsigned long len)
{
	if (opt->stereo) {
		unsigned long frames = len / 2UL;
		const signed char *left = player->splitWorkBuf[index][0] ?
			player->splitWorkBuf[index][0] : player->splitBuf[index][0];
		const signed char *right = player->splitWorkBuf[index][1] ?
			player->splitWorkBuf[index][1] : player->splitBuf[index][1];
		int leftPeak = PlaybackBufferPeakS8(left, frames);
		int rightPeak = PlaybackBufferPeakS8(right, frames);
		return leftPeak > rightPeak ? leftPeak : rightPeak;
	}
	return PlaybackBufferPeakS8(buf, len);
}

static unsigned long DecodeStreamFillPlaybackPrefill(DecodeStream *stream,
	const DecodeOptions *opt, signed char *dest, unsigned long maxBytes,
	unsigned long minSamples)
{
	unsigned long channels;
	unsigned long produced;
	int attempts;

	channels = opt->stereo ? 2UL : 1UL;
	if (channels == 0)
		channels = 1UL;
	if (minSamples == 0 || minSamples * channels > maxBytes)
		minSamples = maxBytes / channels;
	produced = 0;
	attempts = 0;
	while (produced < maxBytes && produced / channels < minSamples &&
		attempts < 8 && !stream->outOfData && !gPlaybackInterrupted) {
		int n;

		if (gPlaybackInterrupted)
			break;
		n = DecodeStreamFillS8(stream, opt, dest + produced,
			(int)(maxBytes - produced));
		if (gPlaybackInterrupted)
			break;
		if (n < 0)
			break;
		if (n == 0) {
			attempts++;
			if (stream->eofReached || stream->outOfData)
				break;
		} else {
			produced += (unsigned long)n;
			attempts = 0;
		}
	}
	return produced;
}

static int ProbeInputSampleRate(InputSource *input, HMP3Decoder decoder,
	DecodeStats *stats)
{
	unsigned char probe[READBUF_SIZE];
	HMP3Decoder probeDecoder;
	unsigned long pos;
	size_t nRead;
	int offset;
	int err;
	MP3FrameInfo info;

	(void)decoder;
	pos = InputSourceTell(input);
	nRead = InputSourceRead(input, probe, sizeof(probe));
	InputSourceSeek(input, pos);
	if (nRead == 0)
		return 0;
	offset = FindValidatedMpegSync(probe, (int)nRead);
	if (offset < 0)
		return 0;
	probeDecoder = MP3InitDecoder();
	if (!probeDecoder)
		return 0;
	err = MP3GetNextFrameInfo(probeDecoder, &info, probe + offset);
	MP3FreeDecoder(probeDecoder);
	if (err != ERR_MP3_NONE)
		return 0;
	UpdateFirstFrameStats(stats, &info);
	return info.samprate;
}

static void PrintPlaybackDebugStartup(const DecodeOptions *opt,
	int playbackRate, unsigned int period, unsigned long requestedBytes,
	unsigned long chunkBytes, const AmigaAudioPlayer *player,
	signed char *buf[3])
{
	if (!opt->debugPlay)
		return;
	printf("debug-play: actual output rate: %d Hz\n", playbackRate);
	printf("debug-play: PAL period: %u\n", period);
	printf("debug-play: requested buffer seconds: %d\n", opt->bufferSeconds);
	printf("debug-play: requested volume percent: %d\n", opt->volumePercent);
	printf("debug-play: mapped audio.device volume: %u (range 0-%u)\n", (unsigned int)VolumePercentToAudioDevice(opt->volumePercent), (unsigned int)AMIGA_AUDIO_DEVICE_MAX_VOLUME);
	printf("debug-play: initial request volume: %u\n", (unsigned int)player->requestVolume);
	printf("debug-play: live volume update method: next CMD_WRITE buffer ioa_Volume; no active writes aborted\n");
	printf("debug-play: volume update sequence count: %lu\n", gMiniAmp3VolumeSequence);
	printf("debug-play: requested half-buffer bytes: %lu\n", requestedBytes);
	printf("debug-play: selected half-buffer samples: %d\n",
		PlaybackHalfBufferSamples(opt, chunkBytes));
	printf("debug-play: selected half-buffer bytes: %lu\n", chunkBytes);
	if (opt->stereo) {
		printf("debug-play: chip buffer A left/right: %p/%p size %lu\n",
			(void *)player->splitBuf[0][0], (void *)player->splitBuf[0][1],
			player->splitBytes);
		printf("debug-play: chip buffer B left/right: %p/%p size %lu\n",
			(void *)player->splitBuf[1][0], (void *)player->splitBuf[1][1],
			player->splitBytes);
		printf("debug-play: fast planar work A left/right: %p/%p size %lu\n",
			(void *)player->splitWorkBuf[0][0],
			(void *)player->splitWorkBuf[0][1], player->splitWorkBytes);
		printf("debug-play: fast planar work B left/right: %p/%p size %lu\n",
			(void *)player->splitWorkBuf[1][0],
			(void *)player->splitWorkBuf[1][1], player->splitWorkBytes);
		printf("debug-play: fast planar work C left/right: %p/%p size %lu\n",
			(void *)player->splitWorkBuf[2][0],
			(void *)player->splitWorkBuf[2][1], player->splitWorkBytes);
	} else {
		printf("debug-play: chip submit buffer A: %p size %lu\n",
			(void *)player->splitBuf[0][0], player->splitBytes);
		printf("debug-play: chip submit buffer B: %p size %lu\n",
			(void *)player->splitBuf[1][0], player->splitBytes);
		printf("debug-play: fast conversion buffer A/B/C: %p/%p/%p size %lu\n",
			(void *)buf[0], (void *)buf[1], (void *)buf[2], chunkBytes);
	}
}

static int AmigaSetupPlaybackBuffers(AmigaAudioPlayer *player,
	const DecodeOptions *opt, unsigned int period, unsigned long requestedBytes,
	unsigned long minBytes, int directPlanar, signed char *buf[3],
	unsigned long *chunkBytes, PlaybackCleanupStatus *status)
{
	unsigned long tryBytes;

	buf[0] = NULL;
	buf[1] = NULL;
	buf[2] = NULL;
	tryBytes = AlignPlaybackChunkBytes(requestedBytes, opt->stereo);
	minBytes = AlignPlaybackChunkBytes(minBytes, opt->stereo);
	if (minBytes == 0)
		minBytes = opt->stereo ? 2UL : 1UL;
	if (tryBytes < minBytes)
		tryBytes = minBytes;

	while (tryBytes >= minBytes) {
		gGuiPlaybackStatus.tryBytes = tryBytes;
		GuiPublishStartupStage(GUISTART_AUDIO_SETUP);
		gMiniAmp3DebugPlayRequested = opt->debugPlay;
		if (AmigaAudioOpen(player, period, opt->stereo, tryBytes, opt->volumePercent) == 0) {
			int workReady;

			player->debugCleanup = opt->debugCleanup;
			player->debugPlay = opt->debugPlay;
			player->outputStride = opt->fastLowrate ?
				FastLowrateStrideForOutputRate(opt->outputRate) : 1;
			workReady = 0;
			if (!directPlanar)
				GuiPublishStartupStage(GUISTART_ALLOC_WORK_BUFFERS);
			if (!directPlanar &&
				AmigaAudioAllocWorkBuffers(player, opt->stereo, tryBytes) == 0) {
				if (opt->stereo) {
					workReady =
						player->splitWorkBuf[0][0] && player->splitWorkBuf[0][1] &&
						player->splitWorkBuf[1][0] && player->splitWorkBuf[1][1] &&
						player->splitWorkBuf[2][0] && player->splitWorkBuf[2][1];
				} else {
					buf[0] = player->workBuf[0];
					buf[1] = player->workBuf[1];
					buf[2] = player->workBuf[2];
					workReady = buf[0] && buf[1] && buf[2];
				}
			}
			if (directPlanar || workReady) {
				GuiPublishStartupStage(GUISTART_AUDIO_SETUP_DONE);
				*chunkBytes = tryBytes;
				if (tryBytes != requestedBytes)
					printf("play buffer reduced to %lu bytes per half-buffer\n",
						tryBytes);
				return 0;
			}
			{
				int wasInterrupted = gPlaybackInterrupted;
				AmigaAudioClose(player, status);
				if (!wasInterrupted)
					gPlaybackInterrupted = 0;
			}
			buf[0] = NULL;
			buf[1] = NULL;
			buf[2] = NULL;
		}

		if (tryBytes <= minBytes)
			break;
		tryBytes = AlignPlaybackChunkBytes(tryBytes / 2UL, opt->stereo);
		if (tryBytes < minBytes)
			tryBytes = minBytes;
	}

	fprintf(stderr, "cannot allocate audio buffers (requested %lu bytes per half-buffer)\n",
		requestedBytes);
	return -1;
}

static int AmigaAudioOpenSilentSelftest(const DecodeOptions *opt)
{
	AmigaAudioPlayer player;
	PlaybackCleanupStatus cleanupStatus;
	signed char *buf[3];
	unsigned long chunkBytes;
	unsigned int period;
	int err;

	memset(&player, 0, sizeof(player));
	PlaybackCleanupStatusInit(&cleanupStatus);
	buf[0] = NULL;
	buf[1] = NULL;
	buf[2] = NULL;
	gGuiPlaybackStatus.requestedRate = opt->outputRate;
	gGuiPlaybackStatus.effectiveRate = opt->outputRate;
	period = AmigaPalAudioPeriod(opt->outputRate);
	gGuiPlaybackStatus.paulaPeriod = period;
	gGuiPlaybackStatus.requestedBytes = 256UL;
	err = AmigaSetupPlaybackBuffers(&player, opt, period, 256UL, 1UL, 0,
		buf, &chunkBytes, &cleanupStatus);
	if (err == 0) {
		memset(buf[0], 0, (size_t)chunkBytes);
		if (AmigaAudioPreparePlaybackBuffer(&player, 0, buf[0], chunkBytes) != 0 ||
			AmigaAudioCommitPlaybackBuffer(&player, 0) != 0 ||
			AmigaAudioWait(&player, 0) != 0)
			err = -1;
	}
	printf("audio-open-silent-test: rate=%d period=%u bytes=%lu result=%d openDevice=%d\n",
		opt->outputRate, period, chunkBytes, err, gGuiPlaybackStatus.openDeviceResult);
	AmigaAudioClose(&player, &cleanupStatus);
	return err;
}

static void AmigaPlaybackCopyInterleavedToWork(AmigaAudioPlayer *player,
	int index, const signed char *pcm, unsigned long len)
{
	if (player->stereo) {
		unsigned long frames = len / 2UL;
		unsigned long i;

		for (i = 0; i < frames; i++) {
			player->splitWorkBuf[index][0][i] = pcm[2UL * i];
			player->splitWorkBuf[index][1][i] = pcm[2UL * i + 1UL];
		}
	} else {
		memcpy(player->workBuf[index], pcm, len);
	}
}

static int AmigaPlayWholeBuffer(const signed char *pcm, unsigned long totalBytes,
	const DecodeOptions *opt, DecodeStats *stats)
{
	AmigaAudioPlayer player;
	PlaybackCleanupStatus cleanupStatus;
	unsigned int period;
	unsigned long pos;
	unsigned long chunkBytes;
	signed char *buf[3];
	unsigned long len[3];
	int cur;
	int pending;
	int first;
	int err;

	memset(&player, 0, sizeof(player));
	PlaybackCleanupStatusInit(&cleanupStatus);
	err = -1;
	if (totalBytes == 0) {
		fprintf(stderr, "no decoded samples; audio.device playback not started\n");
		goto cleanup;
	}
	{
		int playbackRate = PlaybackOutputSampleRate(opt, stats);
		period = AmigaPalAudioPeriod(playbackRate);
		PrintFastLowrateOutputRateDifference(opt, playbackRate);
		printf("play output rate: %d Hz\n", playbackRate);
	}
	printf("PAL audio period: %u\n", period);
	chunkBytes = PlaybackRequestedChunkBytes(opt, PlaybackOutputSampleRate(opt, stats));
	if (chunkBytes > PlaybackMaxChunkBytes(opt->stereo))
		printf("requested %d second half-buffer exceeds audio.device per-write limit; maximum at this rate is %lu ms\n",
			opt->bufferSeconds, PlaybackMaxHalfBufferMilliseconds(opt,
				PlaybackOutputSampleRate(opt, stats)));
	if (AmigaSetupPlaybackBuffers(&player, opt, period, chunkBytes, 1UL, 0,
		buf, &chunkBytes, &cleanupStatus) != 0) {
		goto cleanup;
	}
	printf("playback half-buffer: %lu ms, %lu bytes\n",
		PlaybackBufferDurationMilliseconds(opt, chunkBytes,
			PlaybackOutputSampleRate(opt, stats)), chunkBytes);
	pos = 0;
	for (cur = 0; cur < 2; cur++) {
		len[cur] = totalBytes - pos;
		if (len[cur] > chunkBytes)
			len[cur] = chunkBytes;
		if (opt->stereo && (len[cur] & 1UL))
			len[cur]--;
		if (len[cur] > 0)
			AmigaPlaybackCopyInterleavedToWork(&player, cur, pcm + pos,
				len[cur]);
		pos += len[cur];
	}
	cur = 0;
	pending = 0;
	first = 1;
	while (!gPlaybackInterrupted && len[cur] > 0) {
		if (AmigaAudioPreparePlaybackBuffer(&player, cur, opt->stereo ? NULL : buf[cur],
			len[cur]) != 0 || AmigaAudioCommitPlaybackBuffer(&player, cur) != 0) {
			fprintf(stderr, "playback buffer %s CMD_WRITE byte length is invalid\n",
				PlaybackBufferName(cur));
			goto cleanup;
		}
		pending = 1;
		if (!first) {
			if (AmigaAudioWait(&player, 1 - cur) != 0) {
				fprintf(stderr, "audio.device write failed\n");
				goto cleanup;
			}
			len[1 - cur] = totalBytes - pos;
			if (len[1 - cur] > chunkBytes)
				len[1 - cur] = chunkBytes;
			if (opt->stereo && (len[1 - cur] & 1UL))
				len[1 - cur]--;
			if (len[1 - cur] > 0) {
				AmigaPlaybackCopyInterleavedToWork(&player, 1 - cur,
					pcm + pos, len[1 - cur]);
				pos += len[1 - cur];
			}
		} else {
			first = 0;
		}
		cur = 1 - cur;
	}
	if (gPlaybackInterrupted) {
		fprintf(stderr, "playback interrupted\n");
		goto cleanup;
	}
	if (pending) {
		if (AmigaAudioWait(&player, 1 - cur) != 0) {
			fprintf(stderr, "audio.device write failed\n");
			goto cleanup;
		}
	}
	err = 0;
cleanup:
#ifdef MINTAMP_SKIN
    gSkinAudio.paused=0; gSkinAudio.pause_requested=0;
    gSkinAudio.completed_ok=(err==0 && !gPlaybackInterrupted);
#endif
	GuiPublishStartupStage(err == 0 ? GUISTART_CLEANUP : GUISTART_FAILED);
	gGuiPlaybackStatus.phase = GUIPLAY_PHASE_STOPPING;
	gGuiPlaybackStatus.cleanupComplete = 0;
	AmigaAudioClose(&player, &cleanupStatus);
	gGuiPlaybackStatus.phase = GUIPLAY_PHASE_DONE;
	if (cleanupStatus.canaryErrors)
		err = -1;
	PrintPlaybackCleanupStatus(opt, &cleanupStatus);
	(void)stats;
	return err;
}

static int AmigaPlayDecodeThenPlay(InputSource *input, HMP3Decoder decoder,
	const DecodeOptions *opt, DecodeStats *stats, TimingStats *timing)
{
	DecodeStream stream;
	signed char temp[4096];
	signed char *all;
	unsigned long used;
	unsigned long cap;
	int n;
	int err;

	DecodeStreamInit(&stream, input, decoder, stats, timing);
	all = NULL;
	used = 0;
	cap = 0;
	err = -1;
	while (!gPlaybackInterrupted &&
		(n = DecodeStreamFillS8(&stream, opt, temp, sizeof(temp))) > 0) {
		if (used + (unsigned long)n > cap) {
			unsigned long newCap = cap ? cap * 2UL : 65536UL;
			signed char *newAll;
			while (newCap < used + (unsigned long)n)
				newCap *= 2UL;
			newAll = (signed char *)realloc(all, newCap);
			if (!newAll) {
				fprintf(stderr, "cannot allocate decode-then-play RAM\n");
				goto cleanup;
			}
			all = newAll;
			cap = newCap;
		}
		memcpy(all + used, temp, n);
		used += (unsigned long)n;
	}
	if (stream.decodeError)
		goto cleanup;
	if (gPlaybackInterrupted) {
		fprintf(stderr, "playback interrupted\n");
		goto cleanup;
	}
	printf("decode-then-play bytes: %lu\n", used);
	err = AmigaPlayWholeBuffer(all, used, opt, stats);
cleanup:
#ifdef MINTAMP_SKIN
    gSkinAudio.paused=0; gSkinAudio.pause_requested=0;
    gSkinAudio.completed_ok=(err==0 && !gPlaybackInterrupted);
#endif
	free(all);
	all = NULL;
	if (!gGuiPlaybackStatus.cleanupComplete) {
		gGuiPlaybackStatus.phase = GUIPLAY_PHASE_STOPPING;
		gGuiPlaybackStatus.cleanupStage = GUIPLAY_CLEANUP_COMPLETE;
		gGuiPlaybackStatus.cleanupComplete = 1;
		gGuiPlaybackStatus.phase = GUIPLAY_PHASE_DONE;
	}
	return err;
}


/* =========================================================================
 * Generic decoder stream — bridges DecoderOps module vtable into the same
 * Paula playback infrastructure used by the MP3 path.  Handles stereo and
 * mono output, optional rate downsampling, and fake-stereo widening.
 * ========================================================================= */

#ifdef HAVE_AMIGA_AUDIO_DEVICE

#define GENERIC_STALL_LIMIT 64
#define GENERIC_STARTUP_TIMEOUT_ITERATIONS 64
#define GENERIC_STARTUP_TIMEOUT_MS 5000UL
#define AAC_RADIO_STARTUP_TIMEOUT_MS 15000UL

#define GENERIC_STREAM_MAGIC 0x47445354UL
#define GENERIC_STREAM_GUARD 0x6d704143UL
typedef struct GenericDecodeStream {
	unsigned long             magic;
	unsigned long             preGuard;
	const struct DecoderOps *ops;
	DecHandle                handle;
	int                      channels;    /* as reported by ops->open()          */
	int                      sampleRate;  /* native rate reported by the module  */
	int                      bitsPerSample;
	short                    decodeBuf[OUTBUF_SAMPS]; /* module output (S16 IL)  */
	short                    writeBuf[OUTBUF_SAMPS];  /* post-processed S16      */
	short                    rateBuf[OUTBUF_SAMPS];   /* rate-converted S16      */
	union {
		signed char interleaved[OUTBUF_SAMPS];
		signed char planar[2][OUTBUF_SAMPS / 2];
	} spill;
	int                      spillPos;
	int                      spillCount;
	int                      planarSpillPos;
	int                      planarSpillCount;
	int                      outOfData;
	int                      decodeError;
	int                      consecutiveZeroOutput;
	DecodeStats             *stats;
	TimingStats             *timing;
	RateState                rateState;
	FakeStereo               fakeStereo;
	int                      firstFillDebugPrinted;
	int                      firstDecodeDebugPrinted;
	unsigned long             postGuard;
} GenericDecodeStream;

static void GenericDecodeStreamInit(GenericDecodeStream *gs,
	const struct DecoderOps *ops, DecHandle handle,
	int channels, int sampleRate, int bitsPerSample,
	DecodeStats *stats, TimingStats *timing)
{
	memset(gs, 0, sizeof(*gs));
	gs->magic      = GENERIC_STREAM_MAGIC;
	gs->preGuard   = GENERIC_STREAM_GUARD;
	gs->postGuard  = GENERIC_STREAM_GUARD;
	gs->ops        = ops;
	gs->handle     = handle;
	gs->channels   = channels > 2 ? 2 : (channels < 1 ? 1 : channels);
	gs->sampleRate = sampleRate;
	gs->bitsPerSample = bitsPerSample;
	gs->stats      = stats;
	gs->timing     = timing;
}


static int GenericDecodeStreamGuardOk(GenericDecodeStream *gs, const char *where)
{
	if (!gs || gs->magic != GENERIC_STREAM_MAGIC ||
		gs->preGuard != GENERIC_STREAM_GUARD ||
		gs->postGuard != GENERIC_STREAM_GUARD) {
		fprintf(stderr, "AAC_OUT: CORRUPTED buffer=AAC decode context session=%lu where=%s magic=%08lx pre=%08lx post=%08lx\n",
			(unsigned long)gGuiPlaybackStatus.runId, where ? where : "",
			gs ? gs->magic : 0UL, gs ? gs->preGuard : 0UL, gs ? gs->postGuard : 0UL);
		if (gs) { gs->decodeError = 1; gs->outOfData = 1; }
		gPlaybackInterrupted = 1;
		return 0;
	}
	return 1;
}

/* Max samples per channel to request from the module in one call.
 * OUTBUF_SAMPS is sized for an MP3 frame (2 ch * 2 gran * 576 = 2304).
 * We halve it so the interleaved output always fits in decodeBuf. */
#define GENERIC_DECODE_CHUNK (OUTBUF_SAMPS / 2)
#define GENERIC_BYTES_PER_SAMPLE 2UL

static DecULong GenericDecodeChunkForRateConvert(const GenericDecodeStream *gs,
	const DecodeOptions *opt, int outputChannels)
{
	unsigned long chunk = GENERIC_DECODE_CHUNK;
	unsigned long maxOutFrames;
	unsigned long safe;
	if (gs && opt && outputChannels > 0 && opt->outputRate > gs->sampleRate &&
		gs->sampleRate > 0) {
		maxOutFrames = OUTBUF_SAMPS / (unsigned long)outputChannels;
		safe = (maxOutFrames * (unsigned long)gs->sampleRate) /
			(unsigned long)opt->outputRate;
		if (safe > 1UL)
			safe--;
		if (safe > 0UL && safe < chunk)
			chunk = safe;
	}
	return (DecULong)chunk;
}
#ifndef GENERIC_STARTUP_DECODE_CALL_GUARD
#define GENERIC_STARTUP_DECODE_CALL_GUARD 128
#endif
#ifndef GENERIC_FLAC_TEST_HALF_BUFFER_BYTES
#define GENERIC_FLAC_TEST_HALF_BUFFER_BYTES 0UL
#endif

/* --- Mono (interleaved S8) fill ----------------------------------------- */

static int GenericDecodeStreamCopySpill(GenericDecodeStream *gs,
	signed char *dest, int maxBytes, int *outBytes)
{
	int n;

	if (gs->spillPos >= gs->spillCount) {
		gs->spillPos   = 0;
		gs->spillCount = 0;
		return 0;
	}
	n = gs->spillCount - gs->spillPos;
	if (n > maxBytes)
		n = maxBytes;
	if (n < 0 || *outBytes < 0 || n > maxBytes - *outBytes) {
		fprintf(stderr, "Stream failed: AAC output overflow prevented\n");
		gs->decodeError = 1; return 0;
	}
	memcpy(dest + *outBytes, gs->spill.interleaved + gs->spillPos, n);
	gs->spillPos += n;
	*outBytes    += n;
	if (gs->spillPos >= gs->spillCount) {
		gs->spillPos   = 0;
		gs->spillCount = 0;
	}
	return n;
}


static int GenericRateConvertFrame(RateState *rate, const short *in, short *out,
	int nSamps, int inRate, int outRate, int channels, int outCapacity)
{
	unsigned long inFrames;
	unsigned long produced;
	unsigned long consume;

	if (outRate <= 0 || outRate == inRate || channels <= 0) {
		if (nSamps > outCapacity)
			nSamps = outCapacity;
		if (out != in)
			memmove(out, in, (size_t)nSamps * sizeof(short));
		return nSamps;
	}

	if (rate->inRate != inRate || rate->outRate != outRate ||
		rate->channels != channels) {
		rate->inRate = inRate;
		rate->outRate = outRate;
		rate->channels = channels;
		rate->phase = 0;
	}

	inFrames = (unsigned long)(nSamps / channels);
	produced = 0;
	while (rate->phase / (unsigned long)outRate < inFrames &&
		(produced + 1UL) * (unsigned long)channels <= (unsigned long)outCapacity) {
		unsigned long srcFrame = rate->phase / (unsigned long)outRate;
		int ch;
		for (ch = 0; ch < channels; ch++)
			out[produced * (unsigned long)channels + (unsigned long)ch] =
				in[srcFrame * (unsigned long)channels + (unsigned long)ch];
		produced++;
		rate->phase += (unsigned long)inRate;
	}
	consume = inFrames * (unsigned long)outRate;
	if (rate->phase >= consume)
		rate->phase -= consume;
	else
		rate->phase = 0;

	return (int)(produced * (unsigned long)channels);
}



static int GenericIsAacDecoder(const GenericDecodeStream *gs)
{
	return gs && gs->ops && gs->ops->info && gs->ops->info->extensions &&
		StrCaseCmp(gs->ops->info->extensions, "aac") == 0;
}

static const char *GenericCodecName(const GenericDecodeStream *gs)
{
	return GenericIsAacDecoder(gs) ? "AAC/AAC+" : "generic";
}

#ifdef HAVE_AMISSL
static void *GenericAmiSSLMasterSnapshot(void)
{
	{ void *socket_base = 0, *amissl_base = 0, *amissl_master_base = 0; Radio_GetNetworkBases(&socket_base, &amissl_base, &amissl_master_base); (void)socket_base; (void)amissl_base; return amissl_master_base; }
}
#else
static void *GenericAmiSSLMasterSnapshot(void)
{
	return NULL;
}
#endif

static int GenericValidateDecodedPcm(GenericDecodeStream *gs,
	const DecodeOptions *opt, DecLong nDecoded, unsigned long destFrames,
	unsigned long freeFrames, const char *where, void *masterBefore)
{
	unsigned long channels;
	unsigned long frames;
	unsigned long samples;
	unsigned long bytes;
	unsigned long destCap;
	void *masterAfter;
	int overflow;

	(void)opt;
	channels = (unsigned long)(gs && gs->channels > 0 ? gs->channels : 0);
	frames = (unsigned long)(nDecoded > 0 ? nDecoded : 0);
	samples = frames * channels;
	bytes = samples * GENERIC_BYTES_PER_SAMPLE;
	destCap = destFrames * channels * GENERIC_BYTES_PER_SAMPLE;
	overflow = (channels == 0 || frames > destFrames || samples > (unsigned long)OUTBUF_SAMPS);
	masterAfter = GenericAmiSSLMasterSnapshot();

	if (GenericIsAacDecoder(gs) || overflow) {
		fprintf(stderr,
			"AAC_OUT: codec=%s rate=%ld ch=%lu samplesPerFrame=%lu outputSamples=%lu outputBytes=%lu destCapacity=%lu ringFree=%lu writeOffset=%lu %s section=%s session=%lu station=\"%s\" url=\"%s\" masterBefore=%p masterAfter=%p\n",
			GenericCodecName(gs), (long)(gs ? gs->sampleRate : 0), channels,
			frames, samples, bytes, destCap,
			freeFrames * channels * GENERIC_BYTES_PER_SAMPLE,
			(destFrames - freeFrames) * channels * GENERIC_BYTES_PER_SAMPLE,
			overflow ? "OVERFLOW_PREVENTED" : "OK", where ? where : "decode",
			(unsigned long)gGuiPlaybackStatus.runId,
			(const char *)gGuiPlaybackStatus.radioStationName,
			(const char *)gGuiPlaybackStatus.radioStreamUrl,
			masterBefore, masterAfter);
	}
	if (masterBefore != masterAfter) {
		fprintf(stderr,
			"MEMORY CORRUPTION: TLS master base changed during AAC output handling section=%s before=%p after=%p\n",
			where ? where : "decode", masterBefore, masterAfter);
		return 0;
	}
	if (overflow) {
		fprintf(stderr, "Stream failed: AAC output overflow prevented\n");
		fprintf(stderr,
			"generic decoder: refusing oversized decoded PCM frame codec=%s frames=%lu channels=%lu samples=%lu bytes=%lu destCap=%lu\n",
			GenericCodecName(gs), frames, channels, samples, bytes, destCap);
		gs->decodeError = 1;
		gs->outOfData = 1;
		return 0;
	}
	return 1;
}

static void GenericPrintFirstDecodePcmDebug(const GenericDecodeStream *gs,
	const DecodeOptions *opt, DecLong moduleFrames)
{
	long frames;
	long totalSamples;
	long calculatedBytes;
	long i;
	short minSample = 32767;
	short maxSample = -32768;

	if (!opt->debugDecoder || !gs || moduleFrames <= 0)
		return;

	frames = (long)moduleFrames;
	totalSamples = frames * (long)gs->channels;
	calculatedBytes = totalSamples * (long)sizeof(short);
	if (totalSamples <= 0) {
		minSample = 0;
		maxSample = 0;
	} else {
		for (i = 0; i < totalSamples; i++) {
			short sample = gs->decodeBuf[i];
			if (sample < minSample) minSample = sample;
			if (sample > maxSample) maxSample = sample;
		}
	}

	fprintf(stderr,
		"generic-debug: first decoded PCM channels=%ld bitsPerSample=%ld moduleFrames=%ld moduleTotalSamples=%ld moduleBytes=%ld calculatedFrames=%ld calculatedBytes=%ld first16=",
		(long)gs->channels, (long)gs->bitsPerSample, frames, totalSamples, calculatedBytes,
		frames, calculatedBytes);
	for (i = 0; i < 16 && i < totalSamples; i++)
		fprintf(stderr, "%s%ld", i ? "," : "", (long)gs->decodeBuf[i]);
	fprintf(stderr, " minSample=%ld maxSample=%ld\n",
		(long)minSample, (long)maxSample);
}

static int GenericDecodeStreamFillS8(GenericDecodeStream *gs,
	const DecodeOptions *opt, signed char *dest, int maxBytes)
{
	int produced = 0;

	GenericDecodeStreamCopySpill(gs, dest, maxBytes, &produced);

	while (produced < maxBytes && !gs->outOfData && !gPlaybackInterrupted) {
		DecLong nDecoded;
		void   *masterBeforeDecode;
		int     outSamps;
		int     direct;
		int     spill;
		int     i;

		if (AmigaPlaybackStopRequested(opt, "inside generic mono decode loop"))
			break;

		if (opt->debugDecoder && !gs->firstDecodeDebugPrinted)
			fprintf(stderr, "generic-debug: first decode call entered maxSamplesPerChan=%lu\n",
				(unsigned long)GENERIC_DECODE_CHUNK);
		if (opt->debugDecoder && !gs->firstDecodeDebugPrinted && gs->ops && gs->ops->info &&
			gs->ops->info->extensions && StrCaseCmp(gs->ops->info->extensions, "aac") == 0)
			fprintf(stderr, "AAC: before first decode\n");
		if (!GenericDecodeStreamGuardOk(gs, "before mono decode"))
			break;
		masterBeforeDecode = GenericAmiSSLMasterSnapshot();
		nDecoded = gs->ops->decode(gs->handle, gs->decodeBuf,
			GenericDecodeChunkForRateConvert(gs, opt, 1));
		if (!GenericDecodeStreamGuardOk(gs, "after mono decode"))
			break;
		/* Temporary diagnostic for the new WMA module -- unconditional
		 * (not gated behind opt->debugDecoder, which the ReAction GUI
		 * never sets) so the first several decode() calls are visible
		 * from a Shell window regardless. Safe to remove once WMA
		 * playback is confirmed working. */
		if (gs->ops && gs->ops->info && gs->ops->info->extensions &&
			StrCaseCmp(gs->ops->info->extensions, "wma") == 0) {
			static unsigned long wmaMonoDebugCount = 0;
			if (wmaMonoDebugCount < 20) {
				fprintf(stderr, "wma-debug: mono decode() call #%lu rc=%ld\n",
					wmaMonoDebugCount, (long)nDecoded);
				wmaMonoDebugCount++;
			}
		}
		if (opt->debugDecoder && !gs->firstDecodeDebugPrinted && gs->ops && gs->ops->info &&
			gs->ops->info->extensions && StrCaseCmp(gs->ops->info->extensions, "aac") == 0)
			fprintf(stderr, "AAC: after first decode rc=%ld\n", (long)nDecoded);
		if (!gs->firstDecodeDebugPrinted && gs->ops && gs->ops->info &&
			gs->ops->info->extensions && StrCaseCmp(gs->ops->info->extensions, "aac") == 0)
			fprintf(stderr, "radio-aac-startup: AAC decoder return code=%ld decoded sample count=%ld decoded sample rate=%ld decoded channel count=%ld\n",
				(long)nDecoded, (long)(nDecoded > 0 ? nDecoded * (DecLong)gs->channels : 0),
				(long)gs->sampleRate, (long)gs->channels);
		if (opt->debugDecoder && !gs->firstDecodeDebugPrinted) {
			fprintf(stderr, "generic-debug: first decode call result rc=%ld moduleFrames=%ld totalSamples=%ld sampleRate=%ld channels=%ld\n",
				(long)nDecoded, (long)(nDecoded > 0 ? nDecoded : 0),
				(long)(nDecoded > 0 ? nDecoded * (DecLong)gs->channels : 0),
				(long)gs->sampleRate, (long)gs->channels);
			GenericPrintFirstDecodePcmDebug(gs, opt, nDecoded);
			gs->firstDecodeDebugPrinted = 1;
		}

#if defined(AMIGA_M68K)
		if (SetSignal(0, 0) & SIGBREAKF_CTRL_C)
			gPlaybackInterrupted = 1;
#endif
		if (gPlaybackInterrupted)
			break;

		if (opt->debugDecoder) printf("generic-debug: decode rc=%ld moduleFrames=%ld totalSamples=%ld stopRequested=%d zeroOutput=%d eof=%d error=%d\n",
			(long)nDecoded, (long)(nDecoded > 0 ? nDecoded : 0),
			(long)(nDecoded > 0 ? nDecoded * (DecLong)gs->channels : 0),
			gPlaybackInterrupted ? 1 : 0, gs->consecutiveZeroOutput,
			gs->outOfData, gs->decodeError);

		if (nDecoded > 0 && !GenericValidateDecodedPcm(gs, opt, nDecoded,
			GENERIC_DECODE_CHUNK, GENERIC_DECODE_CHUNK, "mono decode/copy",
			masterBeforeDecode))
			break;


		if (nDecoded < 0) {
			gs->decodeError = 1;
			gs->outOfData   = 1;
			break;
		}
		if (nDecoded == 0) {
			gs->consecutiveZeroOutput++;
			if (opt->debugDecoder) printf("generic-debug: zero-output decode count=%d stopRequested=%d\n",
				gs->consecutiveZeroOutput, gPlaybackInterrupted ? 1 : 0);
			if (AmigaPlaybackStopRequested(opt, "after generic mono zero-output decode"))
				break;
			if (gs->consecutiveZeroOutput > GENERIC_STALL_LIMIT) {
				fprintf(stderr, "generic decoder-stalled: rc=0 pcmSamples=0 zeroOutput=%d\n",
					gs->consecutiveZeroOutput);
				gs->decodeError = 1;
			}
			gs->outOfData = 1;
			break;
		}
		gs->consecutiveZeroOutput = 0;

		/* Mix stereo → mono if necessary, or pass through mono */
		if (gs->channels > 1) {
			outSamps = MixFrame(gs->decodeBuf, gs->writeBuf,
				(int)nDecoded * gs->channels, gs->channels, 1);
		} else {
			memcpy(gs->writeBuf, gs->decodeBuf,
				(size_t)((int)nDecoded * (int)sizeof(short)));
			outSamps = (int)nDecoded;
		}

		/* Convert source-rate frames to output-rate frames before S8 output. */
		if (opt->outputRate > 0 && gs->sampleRate != opt->outputRate) {
			outSamps = GenericRateConvertFrame(&gs->rateState,
				gs->writeBuf, gs->rateBuf, outSamps,
				gs->sampleRate, opt->outputRate, 1, OUTBUF_SAMPS);
			memmove(gs->writeBuf, gs->rateBuf,
				(size_t)outSamps * sizeof(short));
		}

		if (gs->stats)
			gs->stats->outputSamples += (unsigned long)outSamps;
		if (gs->stats)
			gs->stats->decodedFrames++;

		/* Convert S16 → S8 directly into dest[], tail goes to spill */
		direct = outSamps;
		if (direct > maxBytes - produced)
			direct = maxBytes - produced;
		i = 0;
		if (direct >= 4) {
			int d4 = direct & ~3;
			for (; i < d4; i += 4) {
				dest[produced + i]     = Sample16ToS8(gs->writeBuf[i]);
				dest[produced + i + 1] = Sample16ToS8(gs->writeBuf[i + 1]);
				dest[produced + i + 2] = Sample16ToS8(gs->writeBuf[i + 2]);
				dest[produced + i + 3] = Sample16ToS8(gs->writeBuf[i + 3]);
			}
		}
		for (; i < direct; i++)
			dest[produced + i] = Sample16ToS8(gs->writeBuf[i]);
		produced += direct;

		spill = outSamps - direct;
		if (spill > 0) {
			gs->spillPos   = 0;
			gs->spillCount = spill;
			for (i = 0; i < spill; i++)
				gs->spill.interleaved[i] =
					Sample16ToS8(gs->writeBuf[direct + i]);
		}
		if (opt->debugDecoder)
			printf("generic-debug: mono summary sourceRate=%ld outputRate=%ld sourceFramesDecoded=%ld outputFramesWritten=%ld bytesWrittenThisIteration=%ld totalBytesWritten=%ld ratio=%ld/%ld phase=%lu\n",
				(long)gs->sampleRate, (long)(opt->outputRate > 0 ? opt->outputRate : gs->sampleRate),
				(long)nDecoded, (long)direct, (long)direct, (long)produced,
				(long)gs->sampleRate, (long)(opt->outputRate > 0 ? opt->outputRate : gs->sampleRate),
				gs->rateState.phase);
	}
	return produced;
}

/* --- Stereo (planar S8) fill -------------------------------------------- */

static int GenericDecodeStreamCopyPlanarSpill(GenericDecodeStream *gs,
	signed char *left, signed char *right, int maxFrames, int *outFrames)
{
	int n;

	if (gs->planarSpillPos >= gs->planarSpillCount) {
		gs->planarSpillPos   = 0;
		gs->planarSpillCount = 0;
		return 0;
	}
	n = gs->planarSpillCount - gs->planarSpillPos;
	if (n > maxFrames)
		n = maxFrames;
	if (n < 0 || *outFrames < 0 || n > maxFrames - *outFrames) {
		fprintf(stderr, "Stream failed: AAC output overflow prevented\n");
		gs->decodeError = 1; return 0;
	}
	memcpy(left  + *outFrames, gs->spill.planar[0] + gs->planarSpillPos, (size_t)n);
	memcpy(right + *outFrames, gs->spill.planar[1] + gs->planarSpillPos, (size_t)n);
	gs->planarSpillPos += n;
	*outFrames         += n;
	if (gs->planarSpillPos >= gs->planarSpillCount) {
		gs->planarSpillPos   = 0;
		gs->planarSpillCount = 0;
	}
	return n;
}

static int GenericDecodeStreamFillPlanarS8(GenericDecodeStream *gs,
	const DecodeOptions *opt, signed char *left, signed char *right, int maxFrames)
{
	int produced = 0;
	int decodeCalls = 0;
	int firstFill = !gs->firstFillDebugPrinted;
	short min16 = 32767;
	short max16 = -32768;
	signed char min8 = 127;
	signed char max8 = -128;
	unsigned long nonzero8 = 0;
	int haveDiag = 0;
	int startupGuard = GENERIC_STARTUP_DECODE_CALL_GUARD;

	/* When downsampling (source rate >> output rate), each decode call produces
	 * fewer output frames, so more calls are needed to fill the startup buffer.
	 * Scale the guard to cover the worst-case legitimate fill count. */
	if (opt->outputRate > 0 && gs->sampleRate > opt->outputRate) {
		long framesPerCall = ((long)GENERIC_DECODE_CHUNK * opt->outputRate) / gs->sampleRate;
		if (framesPerCall > 0) {
			int needed = (maxFrames + (int)framesPerCall - 1) / (int)framesPerCall;
			if (needed + GENERIC_STARTUP_DECODE_CALL_GUARD > startupGuard)
				startupGuard = needed + GENERIC_STARTUP_DECODE_CALL_GUARD;
		}
	}

	if (!gs->fakeStereo.configured)
		FakeStereoInit(&gs->fakeStereo, opt->fakeStereo,
			opt->fakeStereoDelay, opt->fakeStereoShift);

	GenericDecodeStreamCopyPlanarSpill(gs, left, right, maxFrames, &produced);

	while (produced < maxFrames && !gs->outOfData && !gPlaybackInterrupted) {
		DecLong     nDecoded;
		void       *masterBeforeDecode;
		const short *pcm;
		int          frames;
		int          channels;
		int          i;
		int          direct;

		if (AmigaPlaybackStopRequested(opt, "inside generic stereo decode loop"))
			break;
		if (firstFill && decodeCalls >= startupGuard) {
			fprintf(stderr, "generic decoder: startup fill exceeded decode-call guard\n");
			gs->decodeError = 1;
			break;
		}

		if (opt->debugDecoder && !gs->firstDecodeDebugPrinted)
			fprintf(stderr, "generic-debug: first decode call entered maxSamplesPerChan=%lu\n",
				(unsigned long)GENERIC_DECODE_CHUNK);
		if (opt->debugDecoder && !gs->firstDecodeDebugPrinted && gs->ops && gs->ops->info &&
			gs->ops->info->extensions && StrCaseCmp(gs->ops->info->extensions, "aac") == 0)
			fprintf(stderr, "AAC: before first decode\n");
		if (!GenericDecodeStreamGuardOk(gs, "before stereo decode"))
			break;
		masterBeforeDecode = GenericAmiSSLMasterSnapshot();
		nDecoded = gs->ops->decode(gs->handle, gs->decodeBuf,
			GenericDecodeChunkForRateConvert(gs, opt, 2));
		if (!GenericDecodeStreamGuardOk(gs, "after stereo decode"))
			break;
		/* Temporary diagnostic for the new WMA module -- unconditional
		 * (not gated behind opt->debugDecoder, which the ReAction GUI
		 * never sets) so the first several decode() calls are visible
		 * from a Shell window regardless. Safe to remove once WMA
		 * playback is confirmed working. */
		if (gs->ops && gs->ops->info && gs->ops->info->extensions &&
			StrCaseCmp(gs->ops->info->extensions, "wma") == 0) {
			static unsigned long wmaStereoDebugCount = 0;
			if (wmaStereoDebugCount < 20) {
				fprintf(stderr, "wma-debug: stereo decode() call #%lu rc=%ld\n",
					wmaStereoDebugCount, (long)nDecoded);
				wmaStereoDebugCount++;
			}
		}
		if (opt->debugDecoder && !gs->firstDecodeDebugPrinted && gs->ops && gs->ops->info &&
			gs->ops->info->extensions && StrCaseCmp(gs->ops->info->extensions, "aac") == 0)
			fprintf(stderr, "AAC: after first decode rc=%ld\n", (long)nDecoded);
		if (!gs->firstDecodeDebugPrinted && gs->ops && gs->ops->info &&
			gs->ops->info->extensions && StrCaseCmp(gs->ops->info->extensions, "aac") == 0)
			fprintf(stderr, "radio-aac-startup: AAC decoder return code=%ld decoded sample count=%ld decoded sample rate=%ld decoded channel count=%ld\n",
				(long)nDecoded, (long)(nDecoded > 0 ? nDecoded * (DecLong)gs->channels : 0),
				(long)gs->sampleRate, (long)gs->channels);
		if (opt->debugDecoder && !gs->firstDecodeDebugPrinted) {
			fprintf(stderr, "generic-debug: first decode call result rc=%ld moduleFrames=%ld totalSamples=%ld sampleRate=%ld channels=%ld\n",
				(long)nDecoded, (long)(nDecoded > 0 ? nDecoded : 0),
				(long)(nDecoded > 0 ? nDecoded * (DecLong)gs->channels : 0),
				(long)gs->sampleRate, (long)gs->channels);
			GenericPrintFirstDecodePcmDebug(gs, opt, nDecoded);
			gs->firstDecodeDebugPrinted = 1;
		}
		decodeCalls++;

#if defined(AMIGA_M68K)
		if (SetSignal(0, 0) & SIGBREAKF_CTRL_C)
			gPlaybackInterrupted = 1;
#endif
		if (gPlaybackInterrupted)
			break;

		if (opt->debugDecoder) printf("generic-debug: decode rc=%ld moduleFrames=%ld totalSamples=%ld stopRequested=%d zeroOutput=%d eof=%d error=%d\n",
			(long)nDecoded, (long)(nDecoded > 0 ? nDecoded : 0),
			(long)(nDecoded > 0 ? nDecoded * (DecLong)gs->channels : 0),
			gPlaybackInterrupted ? 1 : 0, gs->consecutiveZeroOutput,
			gs->outOfData, gs->decodeError);

		if (nDecoded > 0 && !GenericValidateDecodedPcm(gs, opt, nDecoded,
			GENERIC_DECODE_CHUNK, (unsigned long)(maxFrames - produced),
			"stereo decode/copy", masterBeforeDecode))
			break;

		if (nDecoded < 0) {
			gs->decodeError = 1;
			gs->outOfData   = 1;
			break;
		}
		if (nDecoded == 0) {
			gs->consecutiveZeroOutput++;
			if (opt->debugDecoder) printf("generic-debug: zero-output decode count=%d stopRequested=%d\n",
				gs->consecutiveZeroOutput, gPlaybackInterrupted ? 1 : 0);
			if (AmigaPlaybackStopRequested(opt, "after generic stereo zero-output decode"))
				break;
			if (gs->consecutiveZeroOutput > GENERIC_STALL_LIMIT) {
				fprintf(stderr, "generic decoder-stalled: rc=0 pcmSamples=0 zeroOutput=%d\n",
					gs->consecutiveZeroOutput);
				gs->decodeError = 1;
			}
			gs->outOfData = 1;
			break;
		}
		gs->consecutiveZeroOutput = 0;

		channels = gs->channels;
		frames   = (int)nDecoded;  /* samples per channel */
		pcm      = gs->decodeBuf;

		/* Convert source-rate frames to output-rate frames before planar S8 output. */
		if (opt->outputRate > 0 && gs->sampleRate != opt->outputRate) {
			if (channels == 1) {
				/* Expand mono to interleaved-stereo for the resampler */
				for (i = frames - 1; i >= 0; i--) {
					gs->writeBuf[2 * i]     = gs->decodeBuf[i];
					gs->writeBuf[2 * i + 1] = gs->decodeBuf[i];
				}
				pcm = gs->writeBuf;
			}
			frames = GenericRateConvertFrame(&gs->rateState, pcm, gs->rateBuf,
				frames * (channels == 1 ? 2 : channels),
				gs->sampleRate, opt->outputRate, 2, OUTBUF_SAMPS) / 2;
			pcm      = gs->rateBuf;
			channels = 2;
		}

		if (gs->stats)
			gs->stats->decodedFrames++;
		if (gs->stats)
			gs->stats->outputSamples += (unsigned long)frames * 2UL;

		/* De-interleave → planar S8 with FakeStereo support for mono sources */
		direct = frames;
		if (direct > maxFrames - produced)
			direct = maxFrames - produced;

		for (i = 0; i < direct; i++) {
			short wl, wr;
			signed char sl, sr;
			if (channels >= 2) {
				wl = pcm[2 * i];
				wr = pcm[2 * i + 1];
			} else if (gs->fakeStereo.enabled) {
				FakeStereoProcess(&gs->fakeStereo, pcm[i], &wl, &wr);
			} else {
				wl = pcm[i];
				wr = pcm[i];
			}
			if (firstFill) {
				if (wl < min16) min16 = wl;
				if (wl > max16) max16 = wl;
				if (wr < min16) min16 = wr;
				if (wr > max16) max16 = wr;
			}
			sl = Sample16ToS8(wl);
			sr = Sample16ToS8(wr);
			left[produced  + i] = sl;
			right[produced + i] = sr;
			if (firstFill) {
				if (sl < min8) min8 = sl;
				if (sl > max8) max8 = sl;
				if (sr < min8) min8 = sr;
				if (sr > max8) max8 = sr;
				if (sl != 0) nonzero8++;
				if (sr != 0) nonzero8++;
				haveDiag = 1;
			}
		}
		if (opt->debugDecoder)
			printf("generic-debug: planar summary sourceRate=%ld outputRate=%ld sourceFramesDecoded=%ld outputFramesWritten=%ld bytesWrittenThisIteration=%ld totalBytesWritten=%ld targetBufBytes=%ld ratio=%ld/%ld phase=%lu enough=%d\n",
				(long)gs->sampleRate, (long)(opt->outputRate > 0 ? opt->outputRate : gs->sampleRate),
				(long)nDecoded, (long)direct, (long)direct * 2L,
				(long)(produced + direct) * 2L, (long)maxFrames * 2L,
				(long)gs->sampleRate, (long)(opt->outputRate > 0 ? opt->outputRate : gs->sampleRate),
				gs->rateState.phase, produced + direct >= maxFrames ? 1 : 0);

		gs->planarSpillPos   = 0;
		gs->planarSpillCount = frames - direct;
		for (i = direct; i < frames; i++) {
			int s = i - direct;
			if (channels >= 2) {
				gs->spill.planar[0][s] = Sample16ToS8(pcm[2 * i]);
				gs->spill.planar[1][s] = Sample16ToS8(pcm[2 * i + 1]);
			} else if (gs->fakeStereo.enabled) {
				short wl, wr;
				FakeStereoProcess(&gs->fakeStereo, pcm[i], &wl, &wr);
				gs->spill.planar[0][s] = Sample16ToS8(wl);
				gs->spill.planar[1][s] = Sample16ToS8(wr);
			} else {
				gs->spill.planar[0][s] = Sample16ToS8(pcm[i]);
				gs->spill.planar[1][s] = gs->spill.planar[0][s];
			}
		}
		produced += direct;
	}
	if (firstFill) {
		int i;
		gs->firstFillDebugPrinted = 1;
		if (!haveDiag) {
			min16 = max16 = 0;
			min8 = max8 = 0;
		}
		if (opt->debugDecoder) printf("generic-debug: first buffer diagnostics s16Min=%ld s16Max=%ld s8Min=%ld s8Max=%ld nonzeroBytes=%lu first16=",
			(long)min16, (long)max16, (long)min8, (long)max8, nonzero8);
		for (i = 0; i < 16 && i < produced; i++)
			if (opt->debugDecoder) printf("%s%ld", i ? "," : "", (long)left[i]);
		if (opt->debugDecoder) printf("\n");
	}
	return produced;
}

/* Mirrors DecodeStreamFillPlaybackBuffer() but uses GenericDecodeStream */
static unsigned long GenericDecodeStreamFillPlaybackBuffer(
	GenericDecodeStream *gs, const DecodeOptions *opt,
	AmigaAudioPlayer *player, int index,
	signed char *buf, unsigned long maxBytes)
{
	const char *fillPath = opt->stereo ? "planar-s8-stereo" : "interleaved-s8-mono";
	if (!GenericDecodeStreamGuardOk(gs, "fill entry"))
		return 0;
	if (opt->debugDecoder) printf("generic-debug: fill entry bufBytes=%lu sourceChannels=%ld outputStereo=%d outputRate=%ld sourceRate=%ld path=%s\n",
		maxBytes, (long)gs->channels, opt->stereo ? 1 : 0,
		(long)opt->outputRate, (long)gs->sampleRate, fillPath);
	if (gPlaybackInterrupted)
		return 0;
	if (opt->stereo) {
		signed char *lbuf = player->splitWorkBuf[index][0] ?
			player->splitWorkBuf[index][0] : player->splitBuf[index][0];
		signed char *rbuf = player->splitWorkBuf[index][1] ?
			player->splitWorkBuf[index][1] : player->splitBuf[index][1];
		int frames;

		if (!lbuf || !rbuf)
			return 0;
		frames = GenericDecodeStreamFillPlanarS8(gs, opt, lbuf, rbuf,
			(int)(maxBytes / 2UL));
		if (gPlaybackInterrupted)
			return 0;
		if (opt->debugDecoder) printf("generic-debug: fill return combinedBytes=%lu perChannelBytes=%lu signedness=signed-8 center=0 matches-mp3-Sample16ToS8\n",
			(unsigned long)frames * 2UL, (unsigned long)frames);
		return (unsigned long)frames * 2UL;
	}
	{
		int bytes = GenericDecodeStreamFillS8(gs, opt, buf, (int)maxBytes);
		if (gPlaybackInterrupted || bytes < 0)
			return 0;
		return (unsigned long)bytes;
	}
}

/* --- Module loading ------------------------------------------------------ */

typedef struct LoadedDecoderModule {
	BPTR                     segment;
	const struct DecoderOps *ops;
	char                     path[600];
} LoadedDecoderModule;

static int ValidateDecoderModuleOps(const struct DecoderOps *ops,
	const char *path, int debugDecoder)
{
	if (!ops) {
		fprintf(stderr, "decoder module validation failed: %s returned null ops\n",
			path ? path : "(unknown)");
		return 0;
	}
	if (!ops->info) {
		fprintf(stderr, "decoder module validation failed: %s has null info\n",
			path ? path : "(unknown)");
		return 0;
	}
	if (ops->info->magic != DECODER_MODULE_MAGIC ||
		ops->info->version > DECODER_MODULE_VERSION) {
		fprintf(stderr, "decoder module validation failed: %s ABI mismatch (magic=%08lx version=%lu expectedMagic=%08lx maxVersion=%lu)\n",
			path ? path : "(unknown)", (unsigned long)ops->info->magic,
			(unsigned long)ops->info->version,
			(unsigned long)DECODER_MODULE_MAGIC,
			(unsigned long)DECODER_MODULE_VERSION);
		return 0;
	}
	if (!ops->open || !ops->decode || !ops->close) {
		fprintf(stderr, "decoder module validation failed: %s missing required callbacks (open=%p decode=%p close=%p)\n",
			path ? path : "(unknown)", (void *)ops->open,
			(void *)ops->decode, (void *)ops->close);
		return 0;
	}
	if (!ops->info->name || !ops->info->extensions || !ops->info->extensions[0]) {
		fprintf(stderr, "decoder module validation failed: %s missing name/extensions\n",
			path ? path : "(unknown)");
		return 0;
	}
	if (debugDecoder)
		fprintf(stderr, "decoder module validation: %s ops OK (name=%s abi=%u revision=%u)\n",
			path ? path : "(unknown)", ops->info->name,
			(unsigned int)ops->info->version,
			(unsigned int)ops->info->revision);
	return 1;
}

/* Scan gDecoderModulesPath for a module whose extension list matches ext.
 * Returns non-zero and fills *out on success; caller must call
 * UnloadDecoderModule() when done. */
static int LoadDecoderModuleForExt(const char *ext,
	LoadedDecoderModule *out, int debugDecoder)
{
	char path[600];
	BPTR lock;
	struct FileInfoBlock *fib;
	BPTR seg;
	DecoderModuleEntryFn entry;
	const struct DecoderOps *ops;
	const char *exts;

	InitDecoderModulesPath();

	if (!gDecoderModulesPath[0] || !ext) {
		if (debugDecoder)
			fprintf(stderr, "decoder module discovery: no decoder directory configured\n");
		return 0;
	}

	if (debugDecoder)
		fprintf(stderr, "decoder module discovery: searching %s for .%s\n",
			gDecoderModulesPath, ext);

	lock = Lock((STRPTR)gDecoderModulesPath, ACCESS_READ);
	if (!lock) {
		if (debugDecoder)
			fprintf(stderr, "decoder module discovery: cannot lock %s (IoErr=%ld)\n",
				gDecoderModulesPath, (long)IoErr());
		return 0;
	}

	fib = (struct FileInfoBlock *)AllocMem(sizeof(*fib), MEMF_CLEAR);
	if (!fib) {
		UnLock(lock);
		return 0;
	}

	if (!Examine(lock, fib)) {
		FreeMem(fib, sizeof(*fib));
		UnLock(lock);
		return 0;
	}

	while (ExNext(lock, fib)) {
		const char *fname = fib->fib_FileName;
		const char *dot;
		/* We only care about *.decoder files */
		dot = NULL;
		{
			const char *p;
			for (p = fname; *p; p++)
				if (*p == '.') dot = p;
		}
		if (!dot || StrCaseCmp(dot, ".decoder") != 0)
			continue;

		/* Build full path */
		{
			int dlen = (int)strlen(gDecoderModulesPath);
			int flen = (int)strlen(fname);
			if (dlen + flen + 1 >= (int)sizeof(path))
				continue;
			memcpy(path, gDecoderModulesPath, (size_t)dlen);
			memcpy(path + dlen, fname, (size_t)(flen + 1));
		}

		if (debugDecoder)
			fprintf(stderr, "decoder module discovery: trying %s\n", path);
		if (debugDecoder && ext && StrCaseCmp(ext, "aac") == 0)
			fprintf(stderr, "AAC: before LoadSeg\n");
		/* Flushed stdout breadcrumbs (RADIO_DBG) as well as the stderr
		 * debugDecoder ones: the halt logs only capture stdout, and this
		 * LoadSeg + entry() jump is one of the few wild-jump candidates in
		 * the silent stretch where two hard halts have landed. */
		RADIO_DBG(printf("radio-startup: decoder module LoadSeg \"%s\"\n", path);)
		seg = LoadSeg((STRPTR)path);
		RADIO_DBG(printf("radio-startup: decoder module LoadSeg done seg=%p\n", (void *)seg);)
		if (debugDecoder && ext && StrCaseCmp(ext, "aac") == 0)
			fprintf(stderr, "AAC: after LoadSeg segment=%p\n", (void *)seg);
		if (!seg) {
			if (debugDecoder)
				fprintf(stderr, "decoder module discovery: LoadSeg failed for %s (IoErr=%ld)\n",
					path, (long)IoErr());
			continue;
		}

		entry = (DecoderModuleEntryFn)((unsigned char *)BADDR(seg) + 4);
		if (!entry) {
			fprintf(stderr, "decoder module discovery: %s has null entry pointer\n", path);
			UnLoadSeg(seg);
			continue;
		}
		if (debugDecoder)
			fprintf(stderr, "decoder module discovery: entry pointer=%p\n", (void *)entry);
		if (debugDecoder && ext && StrCaseCmp(ext, "aac") == 0)
			fprintf(stderr, "AAC: before entry\n");
		RADIO_DBG(printf("radio-startup: decoder module entry call entry=%p\n", (void *)entry);)
		ops   = entry();
		RADIO_DBG(printf("radio-startup: decoder module entry returned ops=%p\n", (void *)ops);)
		if (debugDecoder && ext && StrCaseCmp(ext, "aac") == 0)
			fprintf(stderr, "AAC: after entry ops=%p\n", (void *)ops);
		if (debugDecoder)
			fprintf(stderr, "decoder module discovery: ops pointer=%p\n", (void *)ops);
		if (debugDecoder && ext && StrCaseCmp(ext, "aac") == 0)
			fprintf(stderr, "AAC: before ops validation\n");
		if (!ValidateDecoderModuleOps(ops, path, debugDecoder)) {
			UnLoadSeg(seg);
			continue;
		}
		if (debugDecoder && ext && StrCaseCmp(ext, "aac") == 0)
			fprintf(stderr, "AAC: after ops validation\n");

		if (debugDecoder)
			fprintf(stderr, "decoder module discovery: loaded %s name=\"%s\" abi=%u revision=%u flags=%lu\n",
			path, ops->info->name ? ops->info->name : "(null)",
			(unsigned int)ops->info->version,
			(unsigned int)ops->info->revision,
			(unsigned long)ops->info->flags);

		/* Walk the extension list: "flac\0fla\0\0" */
		if (debugDecoder)
			fprintf(stderr, "decoder module discovery: %s registers extensions:", path);
		for (exts = ops->info->extensions; exts && *exts;
			 exts += strlen(exts) + 1) {
			if (debugDecoder)
				fprintf(stderr, " %s", exts);
			if (StrCaseCmp(exts, ext) == 0) {
				if (debugDecoder)
					fprintf(stderr, "\n");
				if (StrCaseCmp(ext, "flac") == 0 || StrCaseCmp(ext, "fla") == 0) {
					if (strcmp(path, EXPECTED_FLAC_DECODER_PATH) == 0) {
						if (debugDecoder)
							fprintf(stderr, "decoder module discovery: FLAC loader path verified: %s\n",
								path);
					} else {
						if (debugDecoder)
							fprintf(stderr, "decoder module discovery: FLAC loader path WARNING: loaded %s, expected %s\n",
								path, EXPECTED_FLAC_DECODER_PATH);
					}
				}
				out->segment = seg;
				out->ops     = ops;
				strncpy(out->path, path, sizeof(out->path) - 1);
				out->path[sizeof(out->path) - 1] = '\0';
				FreeMem(fib, sizeof(*fib));
				UnLock(lock);
				return 1;
			}
		}
		if (debugDecoder)
			fprintf(stderr, "\n");

		UnLoadSeg(seg);
	}

	FreeMem(fib, sizeof(*fib));
	UnLock(lock);
	return 0;
}

static void UnloadDecoderModule(LoadedDecoderModule *mod)
{
	if (mod && mod->segment) {
		UnLoadSeg(mod->segment);
		mod->segment = (BPTR)0;
		mod->ops     = NULL;
		mod->path[0] = '\0';
	}
}

/* I/O callbacks backed by the existing InputSource ----------------------- */

static DecLong DecModReadCb(void *userData, unsigned char *buf, DecULong maxBytes)
{
	return (DecLong)InputSourceRead((InputSource *)userData, buf, (size_t)maxBytes);
}

static DecLong DecModSeekCb(void *userData, DecLong offset, int whence)
{
	InputSource *src = (InputSource *)userData;
	unsigned long pos;

	if (whence == 0) {              /* SEEK_SET */
		if (offset < 0) return -1;
		pos = (unsigned long)offset;
	} else if (whence == 1) {       /* SEEK_CUR */
		unsigned long cur = InputSourceTell(src);
		if (offset < 0 && (unsigned long)(-(long)offset) > cur) return -1;
		pos = (unsigned long)((long)cur + offset);
	} else {
		return -1;                  /* SEEK_END not supported */
	}
	if (src->radio)
		return -1;
	InputSourceSeek(src, pos);
	return 0;
}

/*
 * Host-side read-ahead buffer.  The decoder's readFn is pointed at
 * DecModPrefetchReadCb; requests are served from a RAM buffer that is
 * refilled in large chunks to amortise hard-drive seek latency.
 * On seek the buffer is invalidated so stale data is never returned.
 */
typedef struct DecModPrefetchState {
	InputSource   *src;
	unsigned char *buf;
	unsigned long  capacity;    /* total buffer allocation              */
	unsigned long  readChunk;   /* bytes per underlying disk read       */
	unsigned long  fill;        /* valid bytes starting at buf[0]       */
	unsigned long  pos;         /* read cursor within buf               */
} DecModPrefetchState;

static DecLong DecModPrefetchReadCb(void *userData, unsigned char *out, DecULong maxBytes)
{
	DecModPrefetchState *ps = (DecModPrefetchState *)userData;
	DecULong             produced = 0;

	while (produced < maxBytes) {
		unsigned long avail = ps->fill - ps->pos;
		unsigned long want  = (unsigned long)(maxBytes - produced);

		if (avail == 0) {
			/* Compact: shift remaining bytes (if any) to front — normally
			 * avail==0 here so this is a no-op, but guard anyway. */
			ps->fill = 0;
			ps->pos  = 0;

			/* Read one large chunk from the underlying source. */
			{
				unsigned long chunkCap = ps->capacity;
				unsigned long toRead   = ps->readChunk > chunkCap ? chunkCap : ps->readChunk;
				size_t        got      = InputSourceRead(ps->src, ps->buf, (size_t)toRead);
				if (got == 0)
					break;
				ps->fill = (unsigned long)got;
				ps->pos  = 0;
				avail    = ps->fill;
			}
		}

		{
			unsigned long take = avail < want ? avail : want;
			memcpy(out + produced, ps->buf + ps->pos, (size_t)take);
			ps->pos  += take;
			produced += (DecULong)take;
		}
	}

	return (DecLong)produced;
}

static DecLong DecModPrefetchSeekCb(void *userData, DecLong offset, int whence)
{
	DecModPrefetchState *ps  = (DecModPrefetchState *)userData;
	InputSource         *src = ps->src;
	unsigned long        pos;

	if (whence == 0) {
		if (offset < 0) return -1;
		pos = (unsigned long)offset;
	} else if (whence == 1) {
		/* Current file position is (underlying_pos - unread_buffered_bytes).
		 * Reconstruct it from InputSourceTell which reflects the real cursor. */
		unsigned long cur  = InputSourceTell(src);
		unsigned long unread = ps->fill - ps->pos;
		unsigned long apparent = cur > unread ? cur - unread : 0;
		if (offset < 0 && (unsigned long)(-(long)offset) > apparent) return -1;
		pos = (unsigned long)((long)apparent + offset);
	} else {
		return -1;
	}

	if (src->radio)
		return -1;
	InputSourceSeek(src, pos);
	ps->fill = 0;
	ps->pos  = 0;
	return 0;
}

/*
 * Allocate a prefetch buffer based on hints from the decoder module.
 * Returns 1 on success (ps->buf is set), 0 if unavailable (caller falls
 * back to direct DecModReadCb).
 */
static int DecModPrefetchInit(DecModPrefetchState *ps, InputSource *src,
	const struct DecoderIoHints *hints)
{
	unsigned long cap;

	memset(ps, 0, sizeof(*ps));
	ps->src = src;

	if (!hints || hints->prefetch_bytes == 0)
		return 0;

	cap = hints->prefetch_bytes;
#ifdef HAVE_AMIGA_AUDIO_DEVICE
	ps->buf = (unsigned char *)AllocMem((LONG)cap, MEMF_FAST | MEMF_CLEAR);
	if (!ps->buf)
		ps->buf = (unsigned char *)AllocMem((LONG)cap, MEMF_ANY | MEMF_CLEAR);
#else
	ps->buf = (unsigned char *)malloc((size_t)cap);
#endif
	if (!ps->buf)
		return 0;

	ps->capacity  = cap;
	ps->readChunk = (hints->preferred_read_bytes > 0 && hints->preferred_read_bytes <= cap)
	                ? hints->preferred_read_bytes : cap;
	return 1;
}

static void DecModPrefetchFree(DecModPrefetchState *ps)
{
	if (!ps->buf) return;
#ifdef HAVE_AMIGA_AUDIO_DEVICE
	FreeMem(ps->buf, (LONG)ps->capacity);
#else
	free(ps->buf);
#endif
	ps->buf = NULL;
}


static int FindAdtsSyncLocal(const unsigned char *buf, size_t n)
{
	size_t i;
	if (!buf || n < 2)
		return -1;
	for (i = 0; i + 1 < n; i++) {
		if (buf[i] == 0xff && (buf[i + 1] & 0xf0) == 0xf0 &&
			(buf[i + 1] & 0x06) == 0)
			return (int)i;
	}
	return -1;
}

/* Length of the ADTS frame whose plausible header is at b, else 0. */
static int AdtsFrameLenLocal(const unsigned char *b, size_t n)
{
	int len;
	if (n < 7 || b[0] != 0xff || (b[1] & 0xf6) != 0xf0)
		return 0;
	if (((b[2] >> 2) & 0x0f) > 12)
		return 0;
	len = ((b[3] & 0x03) << 11) | (b[4] << 3) | (b[5] >> 5);
	return len >= ((b[1] & 1) ? 7 : 9) ? len : 0;
}

/* Where AAC radio audio really starts.  A stream joins mid-frame, and two
 * bytes of frame data look like an ADTS sync about once in 4 KB, so a header
 * only counts once the next frame starts with a matching one (*confirmed).
 * Failing that, the first header whose next frame is not yet buffered. */
static int FindAdtsStartLocal(const unsigned char *buf, size_t n, int *confirmed)
{
	size_t i;
	int first = -1;
	*confirmed = 0;
	for (i = 0; i + 7 <= n; i++) {
		const unsigned char *q;
		int len = AdtsFrameLenLocal(buf + i, n - i);
		if (!len)
			continue;
		if (i + (size_t)len + 7 > n) {
			if (first < 0)
				first = (int)i;
			continue;
		}
		q = buf + i + len;
		if (AdtsFrameLenLocal(q, n - i - (size_t)len) &&
			(q[1] & 0x08) == (buf[i + 1] & 0x08) &&
			(q[2] & 0xfc) == (buf[i + 2] & 0xfc)) {
			*confirmed = 1;
			return (int)i;
		}
	}
	return first;
}

static int ValidateAacAdtsInput(InputSource *input, int debugDecoder)
{
	unsigned char probe[16];
	unsigned long pos;
	size_t nRead;
	int i;
	int sync;
	int profile = -1;
	int sfIndex = -1;
	int chanCfg = -1;
	int frameLen = -1;

	pos = InputSourceTell(input);
	nRead = InputSourceRead(input, probe, sizeof(probe));
	InputSourceSeek(input, pos);
	sync = FindAdtsSyncLocal(probe, nRead);
	if (sync >= 0 && nRead >= (size_t)sync + 7U) {
		const unsigned char *h = probe + sync;
		profile = (h[2] >> 6) & 0x03;
		sfIndex = (h[2] >> 2) & 0x0f;
		chanCfg = ((h[2] & 0x01) << 2) | ((h[3] >> 6) & 0x03);
		frameLen = ((h[3] & 0x03) << 11) | (h[4] << 3) | ((h[5] >> 5) & 0x07);
	}

	if (debugDecoder) {
		fprintf(stderr, "generic-debug: AAC module selected\n");
		fprintf(stderr, "generic-debug: AAC first 16 bytes available=%lu first16=", (unsigned long)nRead);
		for (i = 0; i < (int)nRead; i++)
			fprintf(stderr, "%s%02lx", i ? " " : "", (unsigned long)probe[i]);
		fprintf(stderr, "\n");
		fprintf(stderr, "generic-debug: AACFindSyncWord result=%d profile=%d sampleRateIndex=%d channels=%d frameLength=%d\n",
			sync, profile, sfIndex, chanCfg, frameLen);
	}

	if (nRead >= 8 && probe[4] == 'f' && probe[5] == 't' &&
		probe[6] == 'y' && probe[7] == 'p') {
		fprintf(stderr, "Unsupported AAC container - ADTS .aac only\n");
		return 0;
	}
	if (sync != 0 || nRead < 7 || profile != 1 || sfIndex == 0x0f ||
		chanCfg < 1 || chanCfg > 2 || frameLen < 7) {
		fprintf(stderr, "Unsupported AAC stream - expected ADTS AAC LC mono/stereo\n");
		return 0;
	}
	return 1;
}

static int PrimeRadioAacAdtsInput(InputSource *input, int debugDecoder)
{
	unsigned long total = 0;
	clock_t startedAt;
	int sync = -1;
	int syncConfirmed = 0;
	int pump;
	int i;

	if (!input || !input->radio)
		return 0;

	fprintf(stderr, "radio-aac-startup: transport read start max=%lu\n",
		(unsigned long)sizeof(input->prefix));
	startedAt = clock();
	for (pump = 0; pump < 64 && total < sizeof(input->prefix); pump++) {
		size_t got = (size_t)Radio_ReadStartupAudio(input->radio, input->prefix + total,
			(int)(sizeof(input->prefix) - total), AAC_RADIO_STARTUP_TIMEOUT_MS);
		fprintf(stderr, "radio-aac-startup: transport read bytes returned=%lu total=%lu status=%d\n",
			(unsigned long)got, total + (unsigned long)got,
			(int)Radio_GetStatus(input->radio));
		if (got == 0)
			break;
		total += (unsigned long)got;
		sync = FindAdtsStartLocal(input->prefix, (size_t)total, &syncConfirmed);
		if (sync >= 0 && syncConfirmed)
			break;
		if (PlaybackElapsedMilliseconds(startedAt, clock()) >=
			AAC_RADIO_STARTUP_TIMEOUT_MS) {
			fprintf(stderr, "radio-aac-startup: AAC stream start timeout while searching ADTS sync buffered=%lu\n",
				total);
			break;
		}
	}
	input->prefixSize = total;
	input->prefixPos = 0;

	fprintf(stderr, "radio-aac-startup: first 16 audio bytes after ICY stripping=");
	for (i = 0; i < 16 && (unsigned long)i < total; i++)
		fprintf(stderr, "%s%02lx", i ? " " : "", (unsigned long)input->prefix[i]);
	fprintf(stderr, "\n");
	fprintf(stderr, "radio-aac-startup: first bytes available=%lu first32=",
		total < 32UL ? total : 32UL);
	for (i = 0; i < 32 && (unsigned long)i < total; i++)
		fprintf(stderr, "%s%02lx", i ? " " : "", (unsigned long)input->prefix[i]);
	fprintf(stderr, "\n");
	fprintf(stderr, "radio-aac-startup: AAC sync detection start buffered=%lu\n", total);
	if (sync < 0) {
		fprintf(stderr, "radio-aac-startup: ADTS sync not found reason=\"Unsupported AAC stream format or no ADTS sync\" bytesSkipped=0 searchWindow=%lu\n",
			total);
		fprintf(stderr, "Unsupported AAC stream format or no ADTS sync\n");
		Radio_FailStartup(input->radio,
			total == 0 ? "AAC stream start timeout" :
			"Unsupported AAC stream format or no ADTS sync");
		GuiSetPlaybackPhase(GUIPLAY_PHASE_ERROR);
		gGuiPlaybackStatus.startupStage = GUISTART_FAILED;
		GuiMarkRadioErrorText(Radio_GetError(input->radio));
		fprintf(stderr, "radio-aac-startup: early exit reason=no_adts_sync cleanup called=yes global state reset called=yes final phase=%d radioStatus=%d buffered=%lu\n",
			(int)gGuiPlaybackStatus.phase,
			(int)Radio_GetStatus(input->radio), total);
		return 0;
	}
	fprintf(stderr, "radio-aac-startup: ADTS sync found offset=%d buffered=%lu bytesSkipped=%d confirmed=%d\n",
		sync, total, sync, syncConfirmed);
	if (sync > 0) {
		memmove(input->prefix, input->prefix + sync, (size_t)(total - (unsigned long)sync));
		input->prefixSize = total - (unsigned long)sync;
		input->prefixPos = 0;
		fprintf(stderr, "radio-aac-startup: skipped %d non-ADTS prefix bytes before decoder\n", sync);
	}
	fprintf(stderr, "radio-aac-startup: AAC decoder init start\n");
	return 1;
}

/* --- AmigaPlayStreamingGeneric ------------------------------------------ */

/*
 * Streaming playback path for non-MP3 formats.  Mirrors AmigaPlayStreaming()
 * but uses a GenericDecodeStream backed by a DecoderOps vtable.  The module
 * has already been opened (ops->open called) before this is invoked.
 */
static int AmigaPlayStreamingGeneric(InputSource *input,
	const struct DecoderOps *ops, DecHandle handle,
	const struct DecoderStreamInfo *sinfo,
	const DecodeOptions *opt, DecodeStats *stats, TimingStats *timing)
{
	GenericDecodeStream      stream;
	AmigaAudioPlayer         player;
	PlaybackCleanupStatus    cleanupStatus;
	unsigned int             period;
	unsigned long            bufBytes;
	unsigned long            requestedBytes;
	signed char             *buf[3];
	unsigned long            len[3];
	unsigned long            playbackChannels;
	unsigned long            halfMilliseconds;
	int                      playbackRate;
	int                      active;
	int                      decodeAhead;
	int                      initialDecodeSlots;
	int                      liveSlots;
	int                      refill;
	int                      startupFillAttempts;
	clock_t                  startupStartedAt;
	int                      err;

	(void)input;  /* module handles its own I/O via callbacks */

	memset(&player, 0, sizeof(player));
	PlaybackCleanupStatusInit(&cleanupStatus);
	GuiSetPlaybackPhase(GUIPLAY_PHASE_BUFFERING);
	buf[0] = NULL; buf[1] = NULL; buf[2] = NULL;
	len[0] = 0;    len[1] = 0;    len[2] = 0;
	err = -1;

	playbackRate = opt->outputRate > 0 ? opt->outputRate : (int)sinfo->sampleRate;
	if (sinfo->sampleRate == 0 || sinfo->channels == 0 || sinfo->channels > 2 ||
		sinfo->bitsPerSample == 0 || sinfo->bitsPerSample > 32) {
		fprintf(stderr, "generic decoder: invalid stream format %lu Hz %u ch %u-bit\n",
			sinfo->sampleRate, sinfo->channels, sinfo->bitsPerSample);
		GuiSetPlaybackPhase(GUIPLAY_PHASE_ERROR);
		gGuiPlaybackStatus.startupStage = GUISTART_FAILED;
		return -1;
	}
	if (playbackRate <= 0)
		playbackRate = 8287;

#ifdef MINTAMP_SKIN
    gSkinAudio.eq_supported=0; gSkinAudio.channels=(int)sinfo->channels;
#endif
	stats->sampleRate      = (int)sinfo->sampleRate;
	stats->channels        = (int)sinfo->channels;
	stats->outputSampleRate = playbackRate;
	/* Publish the SOURCE sample rate for the progress clock: decodedFrames
	 * counts source frames of 1152 samples each, so the GUI computes elapsed
	 * as frames*1152/sampleRate.  Using the (possibly downsampled) Paula output
	 * rate here made the progress bar run fast -- effectiveRate carries the
	 * output rate for anything that needs it. */
	gGuiPlaybackStatus.sampleRate  = (int)sinfo->sampleRate;
	gGuiPlaybackStatus.effectiveRate = playbackRate;
	gGuiPlaybackStatus.requestedRate = opt->outputRate;

	GuiPublishStartupStage(GUISTART_STREAM_INIT);
	if (AmigaPlaybackStopRequested(opt, "before generic stream init"))
		goto cleanup;

	GenericDecodeStreamInit(&stream, ops, handle,
		(int)sinfo->channels, (int)sinfo->sampleRate,
		(int)sinfo->bitsPerSample, stats, timing);

	period = AmigaPalAudioPeriod(playbackRate);
	gGuiPlaybackStatus.paulaPeriod = period;
	printf("play output rate: %d Hz (source: %lu Hz, %u ch)\n",
		playbackRate, sinfo->sampleRate, sinfo->channels);

	requestedBytes = PlaybackRequestedChunkBytes(opt, playbackRate);
	if (GENERIC_FLAC_TEST_HALF_BUFFER_BYTES > 0UL &&
		requestedBytes > GENERIC_FLAC_TEST_HALF_BUFFER_BYTES) {
		if (opt->debugDecoder) printf("generic-debug: forcing generic test half-buffer bytes from %lu to %lu\n",
			requestedBytes, (unsigned long)GENERIC_FLAC_TEST_HALF_BUFFER_BYTES);
		requestedBytes = GENERIC_FLAC_TEST_HALF_BUFFER_BYTES;
	}
	gGuiPlaybackStatus.requestedBytes = requestedBytes;

	GuiPublishStartupStage(GUISTART_AUDIO_SETUP);
	if (AmigaPlaybackStopRequested(opt, "before generic audio setup"))
		goto cleanup;

	if (ops && ops->info && ops->info->extensions &&
		StrCaseCmp(ops->info->extensions, "aac") == 0)
		fprintf(stderr, "radio-aac-startup: audio output init start rate=%d sourceRate=%lu channels=%u requestedBytes=%lu\n",
			playbackRate, sinfo->sampleRate, sinfo->channels, requestedBytes);
	if (AmigaSetupPlaybackBuffers(&player, opt, period, requestedBytes,
		opt->stereo ? 2UL : 1UL, 0, buf, &bufBytes, &cleanupStatus) != 0) {
		if (ops && ops->info && ops->info->extensions &&
			StrCaseCmp(ops->info->extensions, "aac") == 0)
			fprintf(stderr, "radio-aac-startup: audio output init result=failure\n");
		goto cleanup;
	}
	if (ops && ops->info && ops->info->extensions &&
		StrCaseCmp(ops->info->extensions, "aac") == 0)
		fprintf(stderr, "radio-aac-startup: audio output init result=success bufBytes=%lu halfSlots=%lu\n",
			bufBytes, (unsigned long)AmigaAudioLiveSlots(opt->stereo));

	halfMilliseconds = PlaybackBufferDurationMilliseconds(opt, bufBytes, playbackRate);
	gGuiPlaybackStatus.halfBufferMs = halfMilliseconds;

	if (AmigaPlaybackStopRequested(opt, "after generic audio setup"))
		goto cleanup;

	if (opt->debugTone) {
		int toneSlot;
		int toneSlots;

		printf("debug-play: debug tone mode active; decoder PCM is bypassed after audio buffer allocation\n");
		toneSlots = AmigaAudioLiveSlots(opt->stereo);
		for (toneSlot = 0; toneSlot < toneSlots; toneSlot++) {
			FillDebugToneBuffer(opt, &player, toneSlot, buf[toneSlot], bufBytes);
			if (AmigaAudioPreparePlaybackBuffer(&player, toneSlot,
				opt->stereo ? NULL : buf[toneSlot], bufBytes) != 0 ||
				AmigaAudioCommitPlaybackBuffer(&player, toneSlot) != 0)
				goto cleanup;
		}
		for (toneSlot = 0; toneSlot < toneSlots; toneSlot++) {
			if (AmigaAudioWait(&player, toneSlot) != 0)
				goto cleanup;
		}
		err = 0;
		goto cleanup;
	}

	GuiSetPlaybackPhase(GUIPLAY_PHASE_BUFFERING);
	playbackChannels   = opt->stereo ? 2UL : 1UL;
	liveSlots          = AmigaAudioLiveSlots(opt->stereo);
	decodeAhead        = opt->stereo ? 2 : -1;
	initialDecodeSlots = opt->stereo ? AMIGA_STEREO_DECODE_SLOTS : liveSlots;
	startupFillAttempts = 0;
	startupStartedAt = clock();

	for (active = 0; active < initialDecodeSlots; active++) {
		GuiPublishStartupStage(active == 0 ? GUISTART_FILL_BUFFER_A :
			GUISTART_FILL_BUFFER_B);
		if (gPlaybackInterrupted)
			goto cleanup;

		len[active] = GenericDecodeStreamFillPlaybackBuffer(&stream, opt,
			&player, active, buf[active], bufBytes);
		#if defined(MINIAMP_AUDIO_WORKBUF_CANARY_DEBUG)
		AmigaAudioCheckWorkBufferCanary(&player, active, len[active], "generic startup fill");
		#endif
		if (active == 0)
			if (opt->debugDecoder) printf("generic-debug: startup buffer 0 filled len=%lu\n", len[active]);
		startupFillAttempts++;
		if (len[active] == 0 && !stream.outOfData && !stream.decodeError &&
			(startupFillAttempts >= GENERIC_STARTUP_TIMEOUT_ITERATIONS ||
			PlaybackElapsedMilliseconds(startupStartedAt, clock()) >=
			GENERIC_STARTUP_TIMEOUT_MS)) {
			fprintf(stderr, "generic decoder: startup timed out before PCM output\n");
			if (ops && ops->info && ops->info->extensions &&
				StrCaseCmp(ops->info->extensions, "aac") == 0) {
				/* A fatal SSL/TLS fault (see radio_ssl_error_is_fatal()) already
				 * fails the stream and sets a specific error ("TLS read/write/
				 * connect failed") well before this generic startup-timeout
				 * budget runs out. Radio_FailStartup() unconditionally
				 * overwrites rs->error with the generic timeout message,
				 * clobbering that diagnosis and reporting a misleading
				 * timeout for what was actually an immediate fatal TLS
				 * failure. Only call it for a genuine timeout: no data,
				 * still no error already recorded. */
				if (input->radio && Radio_GetStatus(input->radio) == RADIO_STATUS_ERROR) {
					fprintf(stderr, "radio-aac-startup: stream already failed (%s), not overwriting with timeout\n",
						Radio_GetError(input->radio));
				} else {
					fprintf(stderr, "radio-aac-startup: AAC stream start timeout before first decoded frame\n");
					Radio_FailStartup(input->radio, "AAC stream start timeout");
				}
			}
			stream.decodeError = 1;
			GuiSetPlaybackPhase(GUIPLAY_PHASE_ERROR);
			gGuiPlaybackStatus.startupStage = GUISTART_FAILED;
			gPlaybackInterrupted = 1;
		}

		if (gPlaybackInterrupted)
			goto cleanup;
		GuiPublishStartupStage(active == 0 ? GUISTART_FILL_BUFFER_A_DONE :
			GUISTART_FILL_BUFFER_B_DONE);

		if (stream.decodeError) {
			GuiSetPlaybackPhase(GUIPLAY_PHASE_ERROR);
			gGuiPlaybackStatus.startupStage = GUISTART_FAILED;
			goto cleanup;
		}
		if (active == 0 && len[0] > 0 && len[0] / playbackChannels > 0 &&
			ops && ops->info && ops->info->extensions &&
			StrCaseCmp(ops->info->extensions, "aac") == 0)
			fprintf(stderr, "radio-aac-startup: first decoded frame ready bytes=%lu samplesPerChannel=%lu\n",
				len[0], len[0] / playbackChannels);
		if (active == 0 && (len[0] == 0 || len[0] / playbackChannels == 0)) {
			fprintf(stderr, "generic decoder: first buffer fill produced zero bytes\n");
			GuiSetPlaybackPhase(GUIPLAY_PHASE_ERROR);
			gGuiPlaybackStatus.startupStage = GUISTART_FAILED;
			goto cleanup;
		}
		if (len[active] == 0)
			break;
		if (active < liveSlots) {
			GuiPublishStartupStage(active == 0 ? GUISTART_PREPARE_A :
				GUISTART_PREPARE_B);
			if (gPlaybackInterrupted)
				goto cleanup;
			if (AmigaAudioPreparePlaybackBuffer(&player, active,
				buf[active], len[active]) != 0)
				goto cleanup;
		}
	}

	if (active == 0)
		goto cleanup;
	if (opt->debugDecoder) printf("generic-debug: starting audio playback\n");
	for (refill = 0; refill < active && refill < liveSlots; refill++) {
		if (gPlaybackInterrupted)
			goto cleanup;
		if (refill == 0)
			GuiPublishStartupStage(GUISTART_COMMIT_A);
		if (AmigaAudioCommitPlaybackBuffer(&player, refill) != 0)
			goto cleanup;
	}
	GuiPublishStartupStage(GUISTART_PLAYING);
	GuiSetPlaybackPhase(GUIPLAY_PHASE_PLAYING);
	if (ops && ops->info && ops->info->extensions &&
		StrCaseCmp(ops->info->extensions, "aac") == 0)
		fprintf(stderr, "radio-aac-startup: UI state transition to PLAYING phase=%d\n",
			(int)gGuiPlaybackStatus.phase);
	err = 0;

	active = 0;
	while (err == 0 && !gPlaybackInterrupted && player.sent[active][0]) {
		clock_t       waitStartedAt;
		clock_t       refillFinishedAt;
		unsigned long elapsedMilliseconds;
		unsigned long activeMilliseconds;
		long          spareMilliseconds;
		int           justFreed;
		int           underrun;
		int           late;

#if defined(AMIGA_M68K)
		if (SetSignal(0, 0) & SIGBREAKF_CTRL_C) {
			gPlaybackInterrupted = 1;
			break;
		}
#endif
#ifdef MINTAMP_SKIN
        unsigned long skinPauseEpoch=gSkinAudio.pause_epoch;
#endif
		waitStartedAt = clock();
		if (gPlaybackInterrupted)
			break;
		underrun = AmigaAudioDone(&player, active);
		if (AmigaAudioWait(&player, active) != 0) {
			err = -1;
			break;
		}
#if defined(AMIGA_M68K)
		if (SetSignal(0, 0) & SIGBREAKF_CTRL_C) {
			gPlaybackInterrupted = 1;
			break;
		}
#endif
		justFreed = active;
		if (opt->stereo) {
			activeMilliseconds = PlaybackBufferDurationMilliseconds(opt,
				len[decodeAhead], playbackRate);
			if (len[decodeAhead] == 0) {
				active = (active + 1) % liveSlots;
				break;
			}
			if (AmigaAudioCopyStereoDecodeAheadToSlot(&player, justFreed,
				decodeAhead, len[decodeAhead]) != 0) {
				err = -1;
				break;
			}
			#if defined(MINIAMP_AUDIO_WORKBUF_CANARY_DEBUG)
			AmigaAudioCheckWorkBufferCanary(&player, justFreed, len[decodeAhead], "generic decode-ahead copy");
			#endif
			len[justFreed] = len[decodeAhead];
		} else {
			activeMilliseconds = PlaybackBufferDurationMilliseconds(opt,
				len[justFreed], playbackRate);
			if (gPlaybackInterrupted)
				break;
			len[justFreed] = GenericDecodeStreamFillPlaybackBuffer(&stream, opt,
				&player, justFreed, buf[justFreed], bufBytes);
			#if defined(MINIAMP_AUDIO_WORKBUF_CANARY_DEBUG)
			AmigaAudioCheckWorkBufferCanary(&player, justFreed, len[justFreed], "generic refill justFreed");
			#endif
			if (stream.decodeError) {
				if (ops && ops->info && ops->info->extensions && StrCaseCmp(ops->info->extensions, "aac") == 0 && input->radio)
					Radio_FailStartup(input->radio, "AAC output overflow prevented");
				err = -1;
				break;
			}
			if (len[justFreed] == 0) {
				active = (active + 1) % liveSlots;
				break;
			}
		}

		if (gPlaybackInterrupted)
			break;
		if (AmigaAudioPreparePlaybackBuffer(&player, justFreed, buf[justFreed],
			len[justFreed]) != 0 ||
			AmigaAudioCommitPlaybackBuffer(&player, justFreed) != 0) {
			err = -1;
			break;
		}
		if (opt->stereo) {
			if (gPlaybackInterrupted)
				break;
			len[decodeAhead] = GenericDecodeStreamFillPlaybackBuffer(&stream, opt,
				&player, decodeAhead, buf[decodeAhead], bufBytes);
			#if defined(MINIAMP_AUDIO_WORKBUF_CANARY_DEBUG)
			AmigaAudioCheckWorkBufferCanary(&player, decodeAhead, len[decodeAhead], "generic refill decodeAhead");
			#endif
			if (stream.decodeError) {
				if (ops && ops->info && ops->info->extensions && StrCaseCmp(ops->info->extensions, "aac") == 0 && input->radio)
					Radio_FailStartup(input->radio, "AAC output overflow prevented");
				err = -1;
				break;
			}
		}
		refillFinishedAt = clock();

		active = (active + 1) % liveSlots;
		elapsedMilliseconds = PlaybackElapsedMilliseconds(waitStartedAt, refillFinishedAt);
#ifdef MINTAMP_SKIN
        if (gSkinAudio.pause_epoch!=skinPauseEpoch || gSkinAudio.pause_catchup>0) {
            elapsedMilliseconds=0; underrun=0;
            if (gSkinAudio.pause_epoch!=skinPauseEpoch) gSkinAudio.pause_catchup=liveSlots;
            else --gSkinAudio.pause_catchup;
        }
#endif
		spareMilliseconds   = (long)activeMilliseconds - (long)elapsedMilliseconds;
		late = (spareMilliseconds < 0) || underrun;
		if (!stats->spareTimeMeasured || spareMilliseconds < stats->minimumSpareMilliseconds) {
			stats->minimumSpareMilliseconds = spareMilliseconds;
			stats->spareTimeMeasured = 1;
		}
		if (late)
			stats->lateBuffers++;
		if (underrun) {
			stats->underruns++;
			stats->underrunBuffers[justFreed]++;
		}
		gGuiPlaybackStatus.spareMs       = spareMilliseconds;
		gGuiPlaybackStatus.underruns     = stats->underruns;
		gGuiPlaybackStatus.decodedFrames = stats->decodedFrames;
		if (underrun)
			GuiSetPlaybackPhase(GUIPLAY_PHASE_UNDERRUN);
		else if (gGuiPlaybackStatus.phase == GUIPLAY_PHASE_UNDERRUN)
			GuiSetPlaybackPhase(GUIPLAY_PHASE_PLAYING);
	}

cleanup:
#ifdef MINTAMP_SKIN
    gSkinAudio.paused=0; gSkinAudio.pause_requested=0;
    gSkinAudio.completed_ok=(err==0 && !gPlaybackInterrupted);
#endif
	if (err != 0 && ops && ops->info && ops->info->extensions &&
		StrCaseCmp(ops->info->extensions, "aac") == 0) {
		GuiSetPlaybackPhase(GUIPLAY_PHASE_ERROR);
		gGuiPlaybackStatus.startupStage = GUISTART_FAILED;
		fprintf(stderr, "radio-aac-startup: early exit reason=playback_start_failed cleanup called=yes global state reset called=yes final phase=%d interrupted=%d\n",
			(int)gGuiPlaybackStatus.phase, gPlaybackInterrupted ? 1 : 0);
	}
	AmigaAudioClose(&player, &cleanupStatus);
	return err;
}

/*
 * AmigaGenericFormatPlay — self-contained non-MP3 playback path.
 *
 * Opens the file, finds a matching decoder module from gDecoderModulesPath,
 * probes the stream, runs AmigaPlayStreamingGeneric(), then cleans up.
 * Called from HelixAmp3CliMain() when the input extension is not ".mp3".
 */
static int AmigaGenericInputPlay(const char *sourceName, InputSource *input, const char *ext,
	const DecodeOptions *opt, DecodeStats *stats, int closeInput)
{
	LoadedDecoderModule   mod;
	struct DecoderStreamInfo sinfo;
	DecHandle             handle = NULL;
	DecModPrefetchState   prefetch;
	int                   hasPrefetch = 0;
	int                   ret = 1;

	memset(&mod,     0, sizeof(mod));
	memset(&sinfo,   0, sizeof(sinfo));
	memset(&prefetch, 0, sizeof(prefetch));

	if (!input)
		return 1;

	if (opt->debugDecoder)
		fprintf(stderr, "generic-debug: selected source extension=.%s decoder type=%s\n",
			ext ? ext : "(null)", (ext && StrCaseCmp(ext, "mp3") == 0) ? "internal-mp3" : "external-module");
	if (opt->debugDecoder && ext && StrCaseCmp(ext, "aac") == 0)
		fprintf(stderr, "AAC: selected\n");
	/* Local modular formats used to branch here before main() reached the
	 * common MP3 Fast RAM preload, so --fast-mem was silently ignored for AAC,
	 * FLAC, Ogg, WMA, WAV and every other decoder module.  Preload the shared
	 * InputSource before validation/module callbacks; those callbacks already
	 * understand memory-backed input.  Live radio remains deliberately excluded
	 * because it is neither finite nor seekable. */
	if (opt->fastMem && !input->radio && !input->memory) {
		int preloadResult;

		GuiSetPlaybackPhase(GUIPLAY_PHASE_BUFFERING);
		GuiPublishStartupStage(GUISTART_INPUT_PRELOAD_FASTMEM);
		preloadResult = InputSourcePreloadFastMemory(input);
		if (preloadResult != 0) {
			if (preloadResult < 0)
				fprintf(stderr, "cannot preload input into Fast RAM: %s\n",
					sourceName ? sourceName : "(unknown)");
			goto done_input;
		}
		gGuiPlaybackStatus.fastInputBytes = input->memorySize;
	}
	if (!input->radio && ext && StrCaseCmp(ext, "aac") == 0 && !ValidateAacAdtsInput(input, opt->debugDecoder)) {
		GuiSetPlaybackPhase(GUIPLAY_PHASE_ERROR);
		gGuiPlaybackStatus.startupStage = GUISTART_FAILED;
		goto done_input;
	}
	if (input->radio && ext && StrCaseCmp(ext, "aac") == 0 &&
		!PrimeRadioAacAdtsInput(input, opt->debugDecoder))
		goto done_input;

	if (!LoadDecoderModuleForExt(ext, &mod, opt->debugDecoder)) {
		fprintf(stderr, "no decoder module found for .%s streams/files\n", ext ? ext : "(unknown)");
		if (input->radio) {
			char failMsg[96];
			sprintf(failMsg, "no .%s decoder module in decoders/", ext ? ext : "?");
			Radio_FailStartup(input->radio, failMsg);
		}
		goto done_input;
	}

	if (opt->debugDecoder && ext && StrCaseCmp(ext, "aac") == 0)
		fprintf(stderr, "generic-debug: AAC module loaded\n");
	if (opt->debugDecoder)
		fprintf(stderr, "generic-debug: decoder module path/name=%s/%s load=success entry ops=%p\n",
			mod.path[0] ? mod.path : "(unknown)",
			(mod.ops && mod.ops->info && mod.ops->info->name) ? mod.ops->info->name : "(null)",
			(void *)mod.ops);

	if (!input->radio && mod.ops->get_io_hints) {
		struct DecoderIoHints hints;
		memset(&hints, 0, sizeof(hints));
		if (mod.ops->get_io_hints(NULL, &hints) == 0) {
			hasPrefetch = DecModPrefetchInit(&prefetch, input, &hints);
			if (opt->debugDecoder)
				fprintf(stderr, "generic-debug: prefetch %s preferred=%lu prefetch=%lu\n",
					hasPrefetch ? "active" : "unavailable (alloc failed)",
					(unsigned long)hints.preferred_read_bytes,
					(unsigned long)hints.prefetch_bytes);
		}
	}

	GuiPublishStartupStage(GUISTART_INPUT_PREPARE);
	if (opt->debugDecoder)
		fprintf(stderr, "generic-debug: decoder open init entering inputPos=%lu\n", InputSourceTell(input));
	if (opt->debugDecoder && ext && StrCaseCmp(ext, "aac") == 0)
		fprintf(stderr, "AAC: before open\n");
	if (input->radio && ext && StrCaseCmp(ext, "aac") == 0)
		fprintf(stderr, "radio-aac-startup: bytes passed to AAC decoder initially=%lu\n",
			input->prefixSize - input->prefixPos);
	if (hasPrefetch)
		handle = mod.ops->open(DecModPrefetchReadCb, DecModPrefetchSeekCb, &prefetch, &sinfo);
	else
		handle = mod.ops->open(DecModReadCb, DecModSeekCb, input, &sinfo);
	if (opt->debugDecoder && ext && StrCaseCmp(ext, "aac") == 0)
		fprintf(stderr, "AAC: after open handle=%p\n", (void *)handle);
	if (input->radio && ext && StrCaseCmp(ext, "aac") == 0)
		fprintf(stderr, "radio-aac-startup: AAC decoder init result handle=%p sampleRate=%lu channels=%u bits=%u\n",
			(void *)handle, sinfo.sampleRate, sinfo.channels, sinfo.bitsPerSample);
	if (opt->debugDecoder)
		fprintf(stderr, "generic-debug: decoder open result handle=%p sampleRate=%lu channels=%u bits=%u\n",
			(void *)handle, sinfo.sampleRate, sinfo.channels, sinfo.bitsPerSample);
	if (!handle) {
		fprintf(stderr, "decoder module failed to open: %s\n", sourceName ? sourceName : "(unknown)");
		GuiSetPlaybackPhase(GUIPLAY_PHASE_ERROR);
		gGuiPlaybackStatus.startupStage = GUISTART_FAILED;
		if (input->radio && ext && StrCaseCmp(ext, "aac") == 0) {
			GuiMarkRadioErrorText("Unsupported AAC stream format or no ADTS sync");
			fprintf(stderr, "radio-aac-startup: early exit reason=decoder_init_failed cleanup called=yes global state reset called=yes final phase=%d radioStatus=%d\n",
				(int)gGuiPlaybackStatus.phase, (int)Radio_GetStatus(input->radio));
		}
		goto done_module;
	}

	if (AmigaPlaybackStopRequested(opt, "after generic stream open"))
		goto done_handle;

	printf("generic decoder: %s  %lu Hz  %u ch  %u-bit totalSamples=%lu\n",
		mod.ops->info->name,
		sinfo.sampleRate, sinfo.channels, sinfo.bitsPerSample, sinfo.totalSamples);

	GuiPublishStartupStage(GUISTART_DECODER_ALLOC);
	gMiniAmp3RequestedVolume = (unsigned short)opt->volumePercent;
	gMiniAmp3VolumeSequence++;

	ret = AmigaPlayStreamingGeneric(input, mod.ops, handle, &sinfo,
		opt, stats, NULL);

done_handle:
	if (input && input->radio) printf("radio-teardown: closing decoder handle=%p\n", (void *)handle);
	mod.ops->close(handle);
done_module:
	if (input && input->radio) printf("radio-teardown: unloading decoder module\n");
	UnloadDecoderModule(&mod);
	if (hasPrefetch)
		DecModPrefetchFree(&prefetch);
done_input:
	if (input && input->radio) printf("radio-teardown: InputSourceClose (radio close) start\n");
	if (closeInput)
		InputSourceClose(input);
	if (input && input->radio) printf("radio-teardown: AmigaGenericInputPlay finished ret=%d\n", ret);
	return ret ? 1 : 0;
}

static int AmigaGenericFormatPlay(const char *filename, const char *ext,
	const DecodeOptions *opt, DecodeStats *stats)
{
	BPTR        amigaFile = (BPTR)0;
	InputSource input;

	GuiPublishStartupStage(GUISTART_INPUT_OPEN);
	if (AmigaPlaybackStopRequested(opt, "before generic input open"))
		return 1;

	GuiPublishStartupStage(GUISTART_INPUT_FOPEN_BEFORE);
	amigaFile = Open((STRPTR)filename, MODE_OLDFILE);
	GuiPublishStartupStage(GUISTART_INPUT_FOPEN_AFTER);
	if (!amigaFile) {
		fprintf(stderr, "cannot open input: %s\n", filename);
		return 1;
	}
	InputSourceInitAmigaDos(&input, amigaFile);
	amigaFile = (BPTR)0;

	if (AmigaPlaybackStopRequested(opt, "after generic input open")) {
		InputSourceClose(&input);
		return 1;
	}

	return AmigaGenericInputPlay(filename, &input, ext, opt, stats, 1);
}

static int AmigaAacSmokeTest(const char *filename, const DecodeOptions *opt)
{
	BPTR amigaFile = (BPTR)0;
	InputSource input;
	LoadedDecoderModule mod;
	struct DecoderStreamInfo sinfo;
	DecHandle handle = NULL;
	short *pcm = NULL;
	DecLong nDecoded;
	int ret = 1;

	(void)opt;
	memset(&mod, 0, sizeof(mod));
	memset(&sinfo, 0, sizeof(sinfo));
	if (!filename || !GetFileExtension(filename) ||
		StrCaseCmp(GetFileExtension(filename), "aac") != 0) {
		fprintf(stderr, "AAC test: input must have .aac extension\n");
		return 1;
	}
	printf("AAC test: open file\n");
	amigaFile = Open((STRPTR)filename, MODE_OLDFILE);
	if (!amigaFile) {
		fprintf(stderr, "AAC test: cannot open input: %s\n", filename);
		return 1;
	}
	InputSourceInitAmigaDos(&input, amigaFile);
	amigaFile = (BPTR)0;

	printf("AAC test: first bytes\n");
	if (!ValidateAacAdtsInput(&input, 1))
		goto done_input;

	printf("AAC test: load module\n");
	printf("AAC: selected\n");
	if (!LoadDecoderModuleForExt("aac", &mod, 1)) {
		fprintf(stderr, "AAC test: no aac.decoder module found\n");
		goto done_input;
	}

	printf("AAC test: module entry\n");
	printf("AAC test: validate ops\n");
	if (!ValidateDecoderModuleOps(mod.ops, mod.path, 1))
		goto done_module;

	printf("AAC test: open/init\n");
	printf("AAC: before open\n");
	handle = mod.ops->open(DecModReadCb, DecModSeekCb, &input, &sinfo);
	printf("AAC: after open handle=%p\n", (void *)handle);
	if (!handle) {
		fprintf(stderr, "AAC test: decoder open/init failed\n");
		goto done_module;
	}
	printf("AAC test: stream %lu Hz %u ch %u-bit\n",
		sinfo.sampleRate, sinfo.channels, sinfo.bitsPerSample);

	printf("AAC test: decode one frame\n");
	pcm = (short *)AllocMem(4096UL * sizeof(short), MEMF_FAST);
	if (!pcm)
		pcm = (short *)AllocMem(4096UL * sizeof(short), MEMF_PUBLIC);
	if (!pcm) {
		fprintf(stderr, "AAC test: cannot allocate PCM smoke-test buffer\n");
		goto done_handle;
	}
	printf("AAC: before first decode\n");
	nDecoded = mod.ops->decode(handle, pcm, 1024UL);
	printf("AAC: after first decode rc=%ld\n", (long)nDecoded);
	if (nDecoded <= 0) {
		fprintf(stderr, "AAC test: decode one frame failed rc=%ld\n", (long)nDecoded);
		goto done_handle;
	}
	printf("AAC test: decoded %ld sample frames (%ld total int16 samples, %ld bytes)\n",
		(long)nDecoded, (long)nDecoded * (long)sinfo.channels,
		(long)nDecoded * (long)sinfo.channels * (long)sizeof(short));
	ret = 0;

done_handle:
	if (pcm)
		FreeMem(pcm, 4096UL * sizeof(short));
	printf("AAC test: close\n");
	mod.ops->close(handle);
done_module:
	UnloadDecoderModule(&mod);
done_input:
	InputSourceClose(&input);
	printf("AAC test: done\n");
	return ret;
}

#endif /* HAVE_AMIGA_AUDIO_DEVICE */
static int AmigaPlayStreaming(InputSource *input, HMP3Decoder decoder,
	const DecodeOptions *opt, DecodeStats *stats, TimingStats *timing)
{
	DecodeStream stream;
	AmigaAudioPlayer player;
	PlaybackCleanupStatus cleanupStatus;
	unsigned int period;
	unsigned long bufBytes;
	unsigned long requestedBytes;
	signed char *buf[3];
	signed char startupBuf[OUTBUF_SAMPS];
	unsigned long startupLen;
	unsigned long len[3];
	unsigned long playbackChannels;
	unsigned long halfMilliseconds;
	int playbackRate;
	int inputSampleRate;
	int active;
	int decodeAhead;
	int initialDecodeSlots;
	int liveSlots;
	int refill;
	int err;

	memset(&player, 0, sizeof(player));
	PlaybackCleanupStatusInit(&cleanupStatus);
	/* Publish an immediate child-side state before any probing or
	 * audio.device setup can block.  This keeps the GUI from sitting on
	 * its optimistic launch message and proves the new playback process
	 * accepted the start request. */
	GuiSetPlaybackPhase(GUIPLAY_PHASE_BUFFERING);
	buf[0] = NULL;
	buf[1] = NULL;
	buf[2] = NULL;
	len[0] = 0;
	len[1] = 0;
	len[2] = 0;
	err = -1;
	GuiPublishStartupStage(GUISTART_PROBE_RATE);
	if (AmigaPlaybackStopRequested(opt, "before input rate probe"))
		goto cleanup;
	inputSampleRate = ProbeInputSampleRate(input, decoder, stats);
	GuiPublishStartupStage(GUISTART_PROBE_RATE_DONE);
	if (AmigaPlaybackStopRequested(opt, "after input rate probe"))
		goto cleanup;
	playbackRate = EffectiveOutputSampleRate(opt, inputSampleRate);
	if (playbackRate <= 0)
		playbackRate = opt->outputRate > 0 ? opt->outputRate : 8287;
	stats->outputSampleRate = playbackRate;
	/* Publish the SOURCE (decoder input) rate for the progress clock, not the
	 * downsampled Paula output rate -- see note in the generic path above.
	 * effectiveRate keeps the actual output rate. */
	gGuiPlaybackStatus.sampleRate = inputSampleRate > 0 ? inputSampleRate : playbackRate;
	gGuiPlaybackStatus.effectiveRate = playbackRate;
	GuiPublishStartupStage(GUISTART_STREAM_INIT);
	if (AmigaPlaybackStopRequested(opt, "before stream init"))
		goto cleanup;
	DecodeStreamInit(&stream, input, decoder, stats, timing);
	if (AmigaPlaybackStopRequested(opt, "after stream init"))
		goto cleanup;
	period = AmigaPalAudioPeriod(playbackRate);
	gGuiPlaybackStatus.paulaPeriod = period;
	PrintFastLowrateOutputRateDifference(opt, playbackRate);
	printf("play output rate: %d Hz\n", playbackRate);
	requestedBytes = PlaybackRequestedChunkBytes(opt, playbackRate);
	if (requestedBytes > PlaybackMaxChunkBytes(opt->stereo))
		printf("requested %d second half-buffer exceeds audio.device per-write limit; maximum at this rate is %lu ms\n",
			opt->bufferSeconds, PlaybackMaxHalfBufferMilliseconds(opt, playbackRate));
	printf("PAL audio period: %u\n", period);
	/* Mono validates a decoded frame before allocating playback buffers. */
	startupLen = 0;
	if (!opt->stereo) {
		GuiPublishStartupStage(GUISTART_PREFILL);
		if (AmigaPlaybackStopRequested(opt, "before prefill"))
			goto cleanup;
		startupLen = DecodeStreamFillPlaybackPrefill(&stream, opt, startupBuf,
			OUTBUF_SAMPS, 1UL);
		GuiPublishStartupStage(GUISTART_PREFILL_DONE);
		if (AmigaPlaybackStopRequested(opt, "after prefill"))
			goto cleanup;
		if (stream.decodeError || startupLen == 0) {
			fprintf(stderr, "no decoded samples; audio.device playback not started\n");
			goto cleanup;
		}
	}
	GuiPublishStartupStage(GUISTART_AUDIO_SETUP);
	if (AmigaPlaybackStopRequested(opt, "before audio setup"))
		goto cleanup;
	gGuiPlaybackStatus.requestedBytes = requestedBytes;
	if (AmigaSetupPlaybackBuffers(&player, opt, period, requestedBytes,
		opt->stereo ? 2UL : startupLen, 0, buf, &bufBytes,
		&cleanupStatus) != 0) {
		goto cleanup;
	}
	halfMilliseconds = PlaybackBufferDurationMilliseconds(opt, bufBytes,
		playbackRate);
	gGuiPlaybackStatus.halfBufferMs = halfMilliseconds;
	if (AmigaPlaybackStopRequested(opt, "after audio setup"))
		goto cleanup;
	printf("playback half-buffer: %lu ms, %lu bytes\n", halfMilliseconds,
		bufBytes);
	PrintPlaybackDebugStartup(opt, playbackRate, period, requestedBytes,
		bufBytes, &player, buf);

	/* Fill decode buffers before the first CMD_WRITE starts playback.  Mono
	 * remains a true three-request audio.device ring.  Stereo queues only two
	 * live DMA pairs (A/B) and keeps C as a Fast RAM decode-ahead buffer; C is
	 * copied into whichever A/B chip pair has been WaitIO-reaped. */
	GuiSetPlaybackPhase(GUIPLAY_PHASE_BUFFERING);
	playbackChannels = opt->stereo ? 2UL : 1UL;
	liveSlots = AmigaAudioLiveSlots(opt->stereo);
	decodeAhead = opt->stereo ? 2 : -1;
	initialDecodeSlots = opt->stereo ? AMIGA_STEREO_DECODE_SLOTS : liveSlots;
	for (active = 0; active < initialDecodeSlots; active++) {
		GuiPublishStartupStage(active == 0 ? GUISTART_FILL_BUFFER_A :
			(active == 1 ? GUISTART_FILL_BUFFER_B : GUISTART_FILL_BUFFER_B));
		if (gPlaybackInterrupted)
			goto cleanup;
		if (active == 0 && !opt->stereo) {
			memcpy(buf[0], startupBuf, (size_t)startupLen);
			len[0] = startupLen;
			if (gPlaybackInterrupted)
				goto cleanup;
			if (len[0] < bufBytes)
				len[0] += (unsigned long)DecodeStreamFillS8(&stream, opt,
					buf[0] + len[0], (int)(bufBytes - len[0]));
		} else {
			len[active] = DecodeStreamFillPlaybackBuffer(&stream, opt, &player,
				active, buf[active], bufBytes);
			#if defined(MINIAMP_AUDIO_WORKBUF_CANARY_DEBUG)
			AmigaAudioCheckWorkBufferCanary(&player, active, len[active], "mp3 startup fill");
			#endif
		}
		if (gPlaybackInterrupted)
			goto cleanup;
		GuiPublishStartupStage(active == 0 ? GUISTART_FILL_BUFFER_A_DONE :
			(active == 1 ? GUISTART_FILL_BUFFER_B_DONE : GUISTART_FILL_BUFFER_B_DONE));
		PrintPlaybackFillDebug(opt, active, len[active]);
		if (stream.decodeError)
			goto cleanup;
		if (active == 0 && len[0] > 0 && opt->debugPlay &&
			PlaybackBufferPeak(opt, &player, 0, buf[0], len[0]) == 0)
			printf("first playback buffer is silent/near-silent\n");
		if (active == 0 && (len[0] == 0 || len[0] / playbackChannels == 0)) {
			fprintf(stderr, "first playback buffer fill produced zero CMD_WRITE bytes\n");
			goto cleanup;
		}
		if (len[active] == 0)
			break;
		if (active < liveSlots) {
			GuiPublishStartupStage(active == 0 ? GUISTART_PREPARE_A :
				(active == 1 ? GUISTART_PREPARE_B : GUISTART_PREPARE_B));
			if (gPlaybackInterrupted)
				goto cleanup;
			if (AmigaAudioPreparePlaybackBuffer(&player, active, buf[active],
				len[active]) != 0) {
				fprintf(stderr, "playback buffer %s CMD_WRITE byte length is invalid\n",
					PlaybackBufferName(active));
				goto cleanup;
			}
		}
	}

	if (active == 0)
		goto cleanup;
	for (refill = 0; refill < active && refill < liveSlots; refill++) {
		if (gPlaybackInterrupted)
			goto cleanup;
		if (refill == 0)
			GuiPublishStartupStage(GUISTART_COMMIT_A);
		if (AmigaAudioCommitPlaybackBuffer(&player, refill) != 0) {
			fprintf(stderr, "playback buffer %s CMD_WRITE byte length is invalid\n",
				PlaybackBufferName(refill));
			goto cleanup;
		}
		if (refill == 0)
			RADIO_DBG(printf("radio-audio: first audio.device CMD_WRITE queued slot=%s bytes=%lu live_slots=%d stereo=%d\n",
				PlaybackBufferName(refill), len[refill], liveSlots, opt->stereo ? 1 : 0);)
	}
	GuiPublishStartupStage(GUISTART_PLAYING);
	GuiSetPlaybackPhase(GUIPLAY_PHASE_PLAYING);
	if (opt->debugPlay) {
		printf("debug-play: CMD_WRITE queued initial ring depth %d\n",
			active < liveSlots ? active : liveSlots);
		if (opt->stereo)
			printf("debug-play: stereo decode-ahead buffer C prepared: %lu bytes\n",
				len[decodeAhead]);
	}
	err = 0;

	active = 0;
	while (err == 0 && !gPlaybackInterrupted &&
		player.sent[active][0]) {
		clock_t waitStartedAt;
		clock_t refillFinishedAt;
		unsigned long elapsedMilliseconds;
		unsigned long activeMilliseconds;
		long spareMilliseconds;
		int justFreed;
		int underrun;
		int late;

#if defined(AMIGA_M68K)
		if (SetSignal(0, 0) & SIGBREAKF_CTRL_C) {
			gPlaybackInterrupted = 1;
			break;
		}
#endif

		/* Wait for the oldest queued live slot before reusing any buffers.
		 * Mono reuses the completed slot in its three-request ring.  Stereo first
		 * WaitIO-reaps both channels in the completed A/B pair, then copies the
		 * prepared Fast RAM C decode-ahead block into that chip pair before
		 * resubmitting it and decoding the next block into C. */
#ifdef MINTAMP_SKIN
        unsigned long skinPauseEpoch=gSkinAudio.pause_epoch;
#endif
		waitStartedAt = clock();
		if (gPlaybackInterrupted)
			break;
		underrun = AmigaAudioDone(&player, active);
		if (AmigaAudioWait(&player, active) != 0) {
			fprintf(stderr, "audio.device write failed\n");
			err = -1;
			break;
		}
#if defined(AMIGA_M68K)
		if (SetSignal(0, 0) & SIGBREAKF_CTRL_C) {
			gPlaybackInterrupted = 1;
			break;
		}
#endif
		if (opt->debugPlay)
			printf("debug-play: CMD_WRITE completed %s\n",
				PlaybackBufferName(active));

		justFreed = active;
		if (opt->stereo) {
			activeMilliseconds = PlaybackBufferDurationMilliseconds(opt,
				len[decodeAhead], playbackRate);
			if (len[decodeAhead] == 0) {
				active = (active + 1) % liveSlots;
				break;
			}
			if (AmigaAudioCopyStereoDecodeAheadToSlot(&player, justFreed,
				decodeAhead, len[decodeAhead]) != 0) {
				fprintf(stderr, "playback buffer %s CMD_WRITE byte length is invalid\n",
					PlaybackBufferName(justFreed));
				err = -1;
				break;
			}
			#if defined(MINIAMP_AUDIO_WORKBUF_CANARY_DEBUG)
			AmigaAudioCheckWorkBufferCanary(&player, justFreed, len[decodeAhead], "mp3 decode-ahead copy");
			#endif
			len[justFreed] = len[decodeAhead];
		} else {
			activeMilliseconds = PlaybackBufferDurationMilliseconds(opt,
				len[justFreed], playbackRate);
			if (gPlaybackInterrupted)
				break;
			len[justFreed] = DecodeStreamFillPlaybackBuffer(&stream, opt, &player,
				justFreed, buf[justFreed], bufBytes);
			#if defined(MINIAMP_AUDIO_WORKBUF_CANARY_DEBUG)
			AmigaAudioCheckWorkBufferCanary(&player, justFreed, len[justFreed], "mp3 refill justFreed");
			#endif
			PrintPlaybackFillDebug(opt, justFreed, len[justFreed]);
			if (stream.decodeError) {
				err = -1;
				break;
			}
			if (len[justFreed] == 0) {
				active = (active + 1) % liveSlots;
				break;
			}
		}

		if (gPlaybackInterrupted)
			break;
		if (AmigaAudioPreparePlaybackBuffer(&player, justFreed, buf[justFreed],
			len[justFreed]) != 0 ||
			AmigaAudioCommitPlaybackBuffer(&player, justFreed) != 0) {
			fprintf(stderr, "playback buffer %s CMD_WRITE byte length is invalid\n",
				PlaybackBufferName(justFreed));
			err = -1;
			break;
		}
		if (opt->stereo) {
			if (gPlaybackInterrupted)
				break;
			len[decodeAhead] = DecodeStreamFillPlaybackBuffer(&stream, opt, &player,
				decodeAhead, buf[decodeAhead], bufBytes);
			#if defined(MINIAMP_AUDIO_WORKBUF_CANARY_DEBUG)
			AmigaAudioCheckWorkBufferCanary(&player, decodeAhead, len[decodeAhead], "mp3 refill decodeAhead");
			#endif
			PrintPlaybackFillDebug(opt, decodeAhead, len[decodeAhead]);
			if (stream.decodeError) {
				err = -1;
				break;
			}
		}
		refillFinishedAt = clock();
		if (opt->debugPlay)
			printf("debug-play: CMD_WRITE resubmitted %s: %lu bytes\n",
				PlaybackBufferName(justFreed), len[justFreed]);

		active = (active + 1) % liveSlots;
		elapsedMilliseconds = PlaybackElapsedMilliseconds(waitStartedAt,
			refillFinishedAt);
#ifdef MINTAMP_SKIN
        if (gSkinAudio.pause_epoch!=skinPauseEpoch || gSkinAudio.pause_catchup>0) {
            elapsedMilliseconds=0; underrun=0;
            if (gSkinAudio.pause_epoch!=skinPauseEpoch) gSkinAudio.pause_catchup=liveSlots;
            else --gSkinAudio.pause_catchup;
        }
#endif
		spareMilliseconds = (long)activeMilliseconds - (long)elapsedMilliseconds;
		late = (spareMilliseconds < 0) || underrun;
		if (!stats->spareTimeMeasured || spareMilliseconds < stats->minimumSpareMilliseconds) {
			stats->minimumSpareMilliseconds = spareMilliseconds;
			stats->spareTimeMeasured = 1;
		}
		if (late)
			stats->lateBuffers++;
		if (underrun) {
			stats->underruns++;
			stats->underrunBuffers[justFreed]++;
			if (opt->debugPlay)
				printf("debug-play: underrun detected before buffer %s refill wait\n",
					PlaybackBufferName(justFreed));
		}
		gGuiPlaybackStatus.spareMs = spareMilliseconds;
		gGuiPlaybackStatus.underruns = stats->underruns;
		gGuiPlaybackStatus.decodedFrames = stats->decodedFrames;
		/* Progress clock wants the source rate (frames*1152 are source
		 * samples); fall back to the effective output rate until the first
		 * frame reports the stream's real rate. */
		if (stream.stats && stream.stats->sampleRate)
			gGuiPlaybackStatus.sampleRate = stream.stats->sampleRate;
		else if (stream.effectiveRate)
			gGuiPlaybackStatus.sampleRate = stream.effectiveRate;
		if (underrun)
			GuiSetPlaybackPhase(GUIPLAY_PHASE_UNDERRUN);
		else if (gGuiPlaybackStatus.phase == GUIPLAY_PHASE_UNDERRUN)
			GuiSetPlaybackPhase(GUIPLAY_PHASE_PLAYING);
	}

	if (err == 0 && !gPlaybackInterrupted) {
		int drain;
		for (drain = 0; drain < liveSlots; drain++) {
			if (player.sent[drain][0]) {
				if (AmigaAudioWait(&player, drain) != 0) {
					fprintf(stderr, "audio.device write failed\n");
					err = -1;
					break;
				} else if (opt->debugPlay) {
					printf("debug-play: CMD_WRITE completed %s\n",
						PlaybackBufferName(drain));
				}
			}
		}
	}

	if (gPlaybackInterrupted) {
		fprintf(stderr, "playback interrupted\n");
		err = -1;
	}
cleanup:
#ifdef MINTAMP_SKIN
    gSkinAudio.paused=0; gSkinAudio.pause_requested=0;
    gSkinAudio.completed_ok=(err==0 && !gPlaybackInterrupted);
#endif
	gGuiPlaybackStatus.phase = GUIPLAY_PHASE_STOPPING;
	gGuiPlaybackStatus.cleanupComplete = 0;
	AmigaAudioClose(&player, &cleanupStatus);
	RADIO_DBG(printf("radio-audio: AmigaAudioClose returned session=%lu\n",
		(input && input->radio) ? Radio_GetSessionId(input->radio) : 0UL));
	RADIO_DBG(printf("radio-audio: entering post-audio cleanup session=%lu\n",
		(input && input->radio) ? Radio_GetSessionId(input->radio) : 0UL));
	gGuiPlaybackStatus.phase = GUIPLAY_PHASE_DONE;
	if (cleanupStatus.canaryErrors)
		err = -1;
	PrintPlaybackCleanupStatus(opt, &cleanupStatus);
	RADIO_DBG(printf("radio-audio: leaving post-audio cleanup session=%lu\n",
		(input && input->radio) ? Radio_GetSessionId(input->radio) : 0UL));
	return err;
}

static int AmigaPlayLifecycleTest(const DecodeOptions *opt)
{
	AmigaAudioPlayer player;
	PlaybackCleanupStatus cleanupStatus;
	unsigned int period;
	unsigned long requestedBytes;
	unsigned long chunkBytes;
	signed char *buf[3];
	int playbackRate;
	int pass;
	int err;

	playbackRate = opt->outputRate > 0 ? opt->outputRate : (opt->stereo ? 8820 : 8287);
	period = AmigaPalAudioPeriod(playbackRate);
	requestedBytes = PlaybackRequestedChunkBytes(opt, playbackRate);
	err = 0;
	for (pass = 0; pass < 5 && err == 0 && !gPlaybackInterrupted; pass++) {
		unsigned long len;

		memset(&player, 0, sizeof(player));
		PlaybackCleanupStatusInit(&cleanupStatus);
		buf[0] = NULL;
		buf[1] = NULL;
		printf("play cleanup self-test pass %d/5\n", pass + 1);
		GuiPublishStartupStage(GUISTART_AUDIO_SETUP);
	gGuiPlaybackStatus.requestedBytes = requestedBytes;
	if (AmigaSetupPlaybackBuffers(&player, opt, period, requestedBytes,
			opt->stereo ? 2UL : 1UL, 0, buf, &chunkBytes, &cleanupStatus) != 0) {
			PrintPlaybackCleanupStatus(opt, &cleanupStatus);
			err = -1;
			break;
		}
		len = (unsigned long)playbackRate / 20UL;
		if (len < 1UL)
			len = 1UL;
		if (opt->stereo)
			len *= 2UL;
		if (len > chunkBytes)
			len = chunkBytes;
		if (opt->stereo) {
			if (!player.splitWorkBuf[0][0] || !player.splitWorkBuf[0][1]) {
				fprintf(stderr, "play lifecycle test work buffer missing\n");
				err = -1;
			} else {
				memset(player.splitWorkBuf[0][0], 0, len / 2UL);
				memset(player.splitWorkBuf[0][1], 0, len / 2UL);
			}
		} else {
			memset(buf[0], 0, len);
		}
		if (err != 0) {
			AmigaAudioClose(&player, &cleanupStatus);
			PrintPlaybackCleanupStatus(opt, &cleanupStatus);
			break;
		}
		if (AmigaAudioPreparePlaybackBuffer(&player, 0, opt->stereo ? NULL : buf[0],
			len) != 0 || AmigaAudioCommitPlaybackBuffer(&player, 0) != 0) {
			fprintf(stderr, "play lifecycle test CMD_WRITE byte length is invalid\n");
			err = -1;
		}
		AmigaAudioClose(&player, &cleanupStatus);
		if (cleanupStatus.canaryErrors)
			err = -1;
		PrintPlaybackCleanupStatus(opt, &cleanupStatus);
	}
	if (gPlaybackInterrupted) {
		fprintf(stderr, "playback interrupted\n");
		err = -1;
	}
	return err;
}

int main(int argc, char **argv)
{
	DecodeOptions opt;
	DecodeStats stats;
	unsigned char readBuf[READBUF_SIZE];
	unsigned char *readPtr;
	short decodeBuf[OUTBUF_SAMPS];
	short writeBuf[OUTBUF_SAMPS];
	short rateBuf[OUTBUF_SAMPS];
	FILE *infile;
#ifdef HAVE_AMIGA_AUDIO_DEVICE
	BPTR amigaInputFile;
#endif
	InputSource input;
	FILE *outfile;
	HMP3Decoder decoder;
	MP3FrameInfo info;
	SvxWriter svx;
	TimingStats timing;
	RateState rateState;
	int bytesLeft;
	int eofReached;
	int outOfData;
	int svxOpen;
	int verifyError;
	clock_t startClock;
	clock_t endClock;
	NormalizedArgs normalized;
	int debugArgv;
	int effectiveRate;
	char *resolvedOutName;

#if defined(AMIGA_M68K) && !defined(RADIO_CONSOLE_LOCK_INIT_ELSEWHERE)
	/* Standalone build only (fast030/amiga_mp3dec targets): this main() is
	 * the true program entry point, so it is safe to InitSemaphore() here,
	 * once, before anything else runs. When this file is #included into
	 * minimp3r.c instead, main() is renamed to HelixAmp3CliMain and becomes
	 * the per-playback-child entry point -- RADIO_CONSOLE_LOCK_INIT_ELSEWHERE
	 * is defined there so this block is skipped; minimp3r.c's own real
	 * main() does the one-time init instead, since re-running InitSemaphore()
	 * on every child spawn would race any other task already using the
	 * semaphore. */
	InitSemaphore(&radio_console_lock);
#endif
	resolvedOutName = NULL;
	infile = NULL;
	outfile = NULL;
#ifdef HAVE_AMIGA_AUDIO_DEVICE
	amigaInputFile = (BPTR)0;
#endif

	if (AmigaNormalizeArgs(argc, argv, &normalized) != 0) {
		fprintf(stderr, "cannot normalize command arguments\n");
		return 1;
	}
	argc = normalized.argc;
	argv = normalized.argv;

	debugArgv = 0;
	{
		int i;
		for (i = 1; i < argc; i++) {
			if (!strcmp(argv[i], "--debug-argv") ||
				!strcmp(argv[i], "--show-argv")) {
				debugArgv = 1;
				break;
			}
		}
	}
	if (debugArgv)
		PrintArgvDebug(argc, argv);

	if (ParseOptions(argc, argv, &opt) != 0) {
		PrintUsage(argv && argv[0] ? argv[0] : "amiga_mp3dec");
		AmigaFreeNormalizedArgs(&normalized);
		return 1;
	}
	if (opt.help) {
		PrintUsage(argv && argv[0] ? argv[0] : "amiga_mp3dec");
		AmigaFreeNormalizedArgs(&normalized);
		return 0;
	}
	if (opt.selftestMulshift) {
		int selftestErr = SelftestMulshift();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestClz) {
		int selftestErr = SelftestClz();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestFdct32) {
		int selftestErr;
		gSelftestVerbose = opt.selftestVerbose;
		selftestErr = SelftestFdct32();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestFdct32Half) {
		int selftestErr;
		gSelftestVerbose = opt.selftestVerbose || opt.selftestFdct32HalfDebug;
		gSelftestFdct32HalfDebug = opt.selftestFdct32HalfDebug;
		selftestErr = SelftestFdct32Half();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestImdct) {
		int selftestErr = SelftestImdct();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestImdctThin) {
		int selftestErr = AMIGA_IMDCT_THIN_SELFTEST();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestSubbandCap) {
		int selftestErr = AMIGA_IMDCT_SUBBAND_CAP_SELFTEST();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestAntialiasSubbandCap) {
		int selftestErr = AMIGA_ANTIALIAS_SUBBAND_CAP_SELFTEST();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestImdct36AsmGeneralPath) {
		int selftestErr = AMIGA_IMDCT36_ASM_GENERAL_PATH_SELFTEST();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestDequantSubbandCap) {
		int selftestErr = AMIGA_DEQUANT_SUBBAND_CAP_SELFTEST();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestCollapseStereoToMono) {
		int selftestErr = AMIGA_COLLAPSE_STEREO_TO_MONO_SELFTEST();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestMidSideSubbandCap) {
		int selftestErr = AMIGA_MIDSIDE_SUBBAND_CAP_SELFTEST();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestCollapseStereoToMonoSubbandCap) {
		int selftestErr = AMIGA_COLLAPSE_STEREO_TO_MONO_SUBBAND_CAP_SELFTEST();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestIntensitySubbandCap) {
		int selftestErr = AMIGA_INTENSITY_SUBBAND_CAP_SELFTEST();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestFdct32HalfSparse16) {
		int selftestErr = AMIGA_FDCT32_HALF_SPARSE16_SELFTEST();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestAntialias) {
		int selftestErr = SelftestAntialias();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestPolyphase) {
		int selftestErr = SelftestPolyphase();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestPolyphaseStride2) {
		int selftestErr = SelftestPolyphaseStride2();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestPolyphaseStride2Reduced) {
		int selftestErr = SelftestPolyphaseStride2Reduced();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestPolyphaseStride4) {
		int selftestErr = SelftestPolyphaseStride4();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestPolyphaseStride4Stereo) {
		int selftestErr = SelftestPolyphaseStride4Stereo();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestPolyphaseStride4StereoReduced) {
		int selftestErr = SelftestPolyphaseStride4StereoReduced();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestPlanarS8TrueStereo) {
		int selftestErr = SelftestPlanarS8TrueStereo();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestPolyphaseStride2Stereo) {
		int selftestErr = SelftestPolyphaseStride2Stereo();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestPolyphaseStride2StereoReduced) {
		int selftestErr = SelftestPolyphaseStride2StereoReduced();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestPolyphaseStride5Stereo) {
		int selftestErr = SelftestPolyphaseStride5Stereo();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestPolyphaseStride3) {
		int selftestErr = SelftestPolyphaseStride3();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestPolyphaseStride3Stereo) {
		int selftestErr = SelftestPolyphaseStride3Stereo();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestPolyphaseStride5) {
		int selftestErr = SelftestPolyphaseStride5();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestPolyphaseStride4AllPhases) {
		int selftestErr = SelftestPolyphaseStride4AllPhases();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestFastLowrate) {
		int selftestErr = SelftestFastLowrate();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestReducedTaps) {
		int selftestErr = SelftestReducedTaps();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestFdct32Quarter) {
		int selftestErr = SelftestFdct32Quarter();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestFdct32QuarterStereo) {
		int selftestErr = SelftestFdct32QuarterStereo();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestHuffman) {
		int selftestErr = SelftestHuffman(&opt);
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestDequant) {
		int selftestErr = SelftestDequant();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestIntensity) {
		int selftestErr = SelftestIntensity();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestBitstream) {
		int selftestErr = AMIGA_BITSTREAM_REFILL_SELFTEST();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestMonoFastLowrateStereo) {
		int selftestErr = SelftestMonoFastLowrateStereo();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestQuality) {
		int selftestErr = SelftestQuality();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.selftestFakeStereo) {
		int selftestErr = SelftestFakeStereo();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr;
	}
	if (opt.startupVolumeSelftest) {
		int selftestErr = SelftestStartupVolume();
		AmigaFreeNormalizedArgs(&normalized);
		return selftestErr == 0 ? 0 : 1;
	}
#ifdef HAVE_AMIGA_AUDIO_DEVICE
	if (opt.testAac) {
		int aacTestErr;
		if (!opt.inName) {
			fprintf(stderr, "--test-aac requires an input .aac file\n");
			AmigaFreeNormalizedArgs(&normalized);
			return 1;
		}
		aacTestErr = AmigaAacSmokeTest(opt.inName, &opt);
		AmigaFreeNormalizedArgs(&normalized);
		return aacTestErr == 0 ? 0 : 1;
	}
#else
	if (opt.testAac) {
		if (!opt.inName)
			fprintf(stderr, "--test-aac requires an input .aac file\n");
		fprintf(stderr, "--test-aac requires an AmigaOS LoadSeg decoder-module build\n");
		AmigaFreeNormalizedArgs(&normalized);
		return 1;
	}
#endif
	if (opt.audioOpenSilentTest) {
		int audioTestErr = AmigaAudioOpenSilentSelftest(&opt);
		AmigaFreeNormalizedArgs(&normalized);
		return audioTestErr == 0 ? 0 : 1;
	}
	if (opt.playLifecycleTest) {
		int playTestErr;
		/* Preserve any GUI stop request that may have arrived before the
		 * lifecycle test reaches its playback loop. */
#ifndef AMIGA_M68K
		signal(SIGINT, PlaybackSignalHandler);
#endif
		playTestErr = AmigaPlayLifecycleTest(&opt);
#ifndef AMIGA_M68K
		signal(SIGINT, SIG_DFL);
#endif
		AmigaFreeNormalizedArgs(&normalized);
		return playTestErr == 0 ? 0 : 1;
	}

	GuiPublishStartupStage(GUISTART_ARGS_READY);
	if (opt.inName && !strncmp(opt.inName, "http://", 7))
		opt.radioStream = 1;
#if defined(HAVE_AMISSL)
	if (opt.inName && !strncmp(opt.inName, "https://", 8))
		opt.radioStream = 1;
#endif
	if (opt.outName && OutputNameIsDirectory(opt.outName)) {
		resolvedOutName = BuildDirectoryOutputName(opt.outName, opt.inName, &opt);
		if (!resolvedOutName) {
			fprintf(stderr, "cannot build output path\n");
			AmigaFreeNormalizedArgs(&normalized);
			return 1;
		}
		opt.outName = resolvedOutName;
	}

	memset(&stats, 0, sizeof(stats));
	if (opt.checksum)
		stats.pcmChecksum = 2166136261UL;
	memset(&timing, 0, sizeof(timing));
	memset(&rateState, 0, sizeof(rateState));
	memset(&info, 0, sizeof(info));

	GuiPublishStartupStage(GUISTART_INPUT_OPEN);
	if (opt.play && AmigaPlaybackStopRequested(&opt, "before input open")) {
		free(resolvedOutName);
		AmigaFreeNormalizedArgs(&normalized);
		return 1;
	}
	gGuiPlaybackStatus.requestedRate = opt.outputRate;
#ifdef HAVE_AMIGA_AUDIO_DEVICE
	if (opt.play && opt.radioStream) {
		RadioStream *radio;
#if ENABLE_RADIO && !defined(main)
		CliResolveRadioUrl(&opt);
#endif
		GuiPublishStartupStage(GUISTART_INPUT_FOPEN_BEFORE);
		radio = Radio_OpenWithHostAddr(opt.inName, opt.haveRadioHostAddr, opt.radioHostAddrBe);
		GuiPublishStartupStage(GUISTART_INPUT_FOPEN_AFTER);
		if (!radio || Radio_GetStatus(radio) == RADIO_STATUS_ERROR) {
			fprintf(stderr, "cannot open radio stream: %s\n", radio ? Radio_GetError(radio) : "out of memory");
			GuiMarkRadioErrorText(radio ? Radio_GetError(radio) : "out of memory");
			if (radio) Radio_Close(radio);
			free(resolvedOutName);
			AmigaFreeNormalizedArgs(&normalized);
			return 1;
		}
		InputSourceInitRadio(&input, radio);
		{
			int probePump;
			for (probePump = 0; probePump < 200 && !Radio_GetContentType(radio)[0] &&
				Radio_GetStatus(radio) != RADIO_STATUS_ERROR; probePump++)
				Radio_Pump(radio);
		}
		GuiPublishRadioMetadata(radio);
		{
			const char *radioExt;
			fprintf(stderr, "radio-codec: Radio Browser codec=%s URL codec hint=%s HTTP content-type=%s\n",
				opt.radioCodecHint ? opt.radioCodecHint : "(none)",
				RadioUrlHasMp3Hint(opt.inName) ? "MP3" : "none", Radio_GetContentType(radio));
			if (RadioStreamLooksLikeOpus(opt.inName, Radio_GetContentType(radio), opt.radioCodecHint)) {
				fprintf(stderr, "cannot open radio stream: Opus-in-Ogg is not supported by this decoder\n");
				GuiMarkRadioErrorText("Unsupported stream codec: OPUS");
				Radio_Close(radio);
				free(resolvedOutName);
				AmigaFreeNormalizedArgs(&normalized);
				return 1;
			}
			radioExt = RadioDecoderExtFromUrlOrTypeHint(opt.inName, Radio_GetContentType(radio), opt.radioCodecHint);
			/* Lossless stations send FLAC inside Ogg under the same
			 * "audio/ogg" type as Vorbis, so look at the stream itself. */
			if (radioExt && (StrCaseCmp(radioExt, "ogg") == 0 || StrCaseCmp(radioExt, "flac") == 0)) {
				int sniff = RadioSniffFlac(&input);
				if (sniff == RADIO_SNIFF_OGG_FLAC) {
					if (!InputSourceStartOggFlac(&input)) {
						fprintf(stderr, "cannot open radio stream: out of memory for Ogg FLAC\n");
						GuiMarkRadioErrorText("out of memory");
						Radio_Close(radio);
						free(resolvedOutName);
						AmigaFreeNormalizedArgs(&normalized);
						return 1;
					}
					radioExt = "flac";
				} else if (sniff == RADIO_SNIFF_NATIVE_FLAC) {
					radioExt = "flac";
				} else if (sniff == RADIO_SNIFF_OGG_VORBIS) {
					radioExt = "ogg";
				}
				fprintf(stderr, "radio-codec: stream sniff=%s\n",
					sniff == RADIO_SNIFF_OGG_FLAC ? "Ogg FLAC" :
					sniff == RADIO_SNIFF_NATIVE_FLAC ? "FLAC" :
					sniff == RADIO_SNIFF_OGG_VORBIS ? "Ogg Vorbis" : "unknown");
			}
			fprintf(stderr, "radio-codec: final selected decoder=%s\n", radioExt ? radioExt : "mp3");
			if (radioExt && StrCaseCmp(radioExt, "mp3") != 0) {
				int gret = AmigaGenericInputPlay(opt.inName, &input, radioExt, &opt, &stats, 1);
				printf("radio-teardown: generic(AAC/FLAC) play returned, freeing resolvedOutName=%p\n", (void *)resolvedOutName);
				free(resolvedOutName);
				printf("radio-teardown: resolvedOutName freed, freeing normalized args\n");
				AmigaFreeNormalizedArgs(&normalized);
				printf("radio-teardown: normalized args freed, returning gret=%d from main\n", gret);
				return gret;
			}
		}
	} else if (opt.play) {
		/* If the file extension is not .mp3, try a generic decoder module. */
		{
			const char *ext = GetFileExtension(opt.inName);
			if (ext && StrCaseCmp(ext, "mp3") != 0) {
				int gret = AmigaGenericFormatPlay(opt.inName, ext, &opt, &stats);
				free(resolvedOutName);
				AmigaFreeNormalizedArgs(&normalized);
				return gret;
			}
		}
		GuiPublishStartupStage(GUISTART_INPUT_FOPEN_BEFORE);
		amigaInputFile = Open((STRPTR)opt.inName, MODE_OLDFILE);
		GuiPublishStartupStage(GUISTART_INPUT_FOPEN_AFTER);
		if (!amigaInputFile) {
			fprintf(stderr, "cannot open input: %s\n", opt.inName);
			free(resolvedOutName);
			AmigaFreeNormalizedArgs(&normalized);
			return 1;
		}
		InputSourceInitAmigaDos(&input, amigaInputFile);
		amigaInputFile = (BPTR)0;
	} else
#endif
	{
		if (opt.radioStream) {
			RadioStream *radio;
			GuiPublishStartupStage(GUISTART_INPUT_FOPEN_BEFORE);
			radio = Radio_OpenWithHostAddr(opt.inName, opt.haveRadioHostAddr, opt.radioHostAddrBe);
			GuiPublishStartupStage(GUISTART_INPUT_FOPEN_AFTER);
			if (!radio || Radio_GetStatus(radio) == RADIO_STATUS_ERROR) {
				fprintf(stderr, "cannot open radio stream: %s\n", radio ? Radio_GetError(radio) : "out of memory");
				GuiMarkRadioErrorText(radio ? Radio_GetError(radio) : "out of memory");
				if (radio) Radio_Close(radio);
				free(resolvedOutName);
				AmigaFreeNormalizedArgs(&normalized);
				return 1;
			}
			InputSourceInitRadio(&input, radio);
			GuiPublishRadioMetadata(radio);
		} else {
		GuiPublishStartupStage(GUISTART_INPUT_FOPEN_BEFORE);
		infile = fopen(opt.inName, "rb");
		GuiPublishStartupStage(GUISTART_INPUT_FOPEN_AFTER);
		if (!infile) {
			fprintf(stderr, "cannot open input: %s\n", opt.inName);
			free(resolvedOutName);
			AmigaFreeNormalizedArgs(&normalized);
			return 1;
		}
		InputSourceInit(&input, infile);
		}
	}
	if (opt.info && infile) {
		PrintMp3Info(infile, opt.inName);
		if (!opt.play && !opt.outName) {
			CloseInputFile(&infile, opt.debugCleanup);
			free(resolvedOutName);
			AmigaFreeNormalizedArgs(&normalized);
			return 0;
		}
	}
	if (opt.play)
		GuiSetPlaybackPhase(GUIPLAY_PHASE_BUFFERING);
	if (opt.play && AmigaPlaybackStopRequested(&opt, "after input open")) {
		InputSourceClose(&input);
		CloseInputFile(&infile, opt.debugCleanup);
		free(resolvedOutName);
		AmigaFreeNormalizedArgs(&normalized);
		return 1;
	}
	if (opt.fastMem) {
		int preloadResult;

		GuiPublishStartupStage(GUISTART_INPUT_PRELOAD_FASTMEM);
		preloadResult = InputSourcePreloadFastMemory(&input);
		if (preloadResult != 0) {
			if (preloadResult < 0)
				fprintf(stderr, "cannot preload input into Fast RAM: %s\n", opt.inName);
			InputSourceClose(&input);
			CloseInputFile(&infile, opt.debugCleanup);
			free(resolvedOutName);
			AmigaFreeNormalizedArgs(&normalized);
			return 1;
		}
		gGuiPlaybackStatus.fastInputBytes = input.memorySize;
	}
	if (opt.play && AmigaPlaybackStopRequested(&opt, "after input preload")) {
		InputSourceClose(&input);
		CloseInputFile(&infile, opt.debugCleanup);
		free(resolvedOutName);
		AmigaFreeNormalizedArgs(&normalized);
		return 1;
	}
	GuiPublishStartupStage(GUISTART_INPUT_PREPARE);
	if (opt.play && AmigaPlaybackStopRequested(&opt, "before input prepare")) {
		InputSourceClose(&input);
		CloseInputFile(&infile, opt.debugCleanup);
		free(resolvedOutName);
		AmigaFreeNormalizedArgs(&normalized);
		return 1;
	}
	if (InputSourcePrepareMp3(&input) != 0) {
		fprintf(stderr, "cannot inspect MP3 input: %s\n", opt.inName);
		InputSourceClose(&input);
		CloseInputFile(&infile, opt.debugCleanup);
		free(resolvedOutName);
		AmigaFreeNormalizedArgs(&normalized);
		return 1;
	}

	outfile = NULL;
	if (!opt.noOutput) {
		outfile = fopen(opt.outName, opt.outFormat == OUT_8SVX ? "wb+" : "wb");
		if (!outfile) {
			fprintf(stderr, "cannot open output: %s\n", opt.outName);
			InputSourceClose(&input);
			CloseInputFile(&infile, opt.debugCleanup);
			free(resolvedOutName);
			AmigaFreeNormalizedArgs(&normalized);
			return 1;
		}
	}

	if (opt.play && AmigaPlaybackStopRequested(&opt, "after input prepare")) {
		InputSourceClose(&input);
		CloseInputFile(&infile, opt.debugCleanup);
		free(resolvedOutName);
		AmigaFreeNormalizedArgs(&normalized);
		return 1;
	}
	GuiPublishStartupStage(GUISTART_DECODER_ALLOC);
	if (opt.play && AmigaPlaybackStopRequested(&opt, "before decoder alloc")) {
		InputSourceClose(&input);
		CloseInputFile(&infile, opt.debugCleanup);
		free(resolvedOutName);
		AmigaFreeNormalizedArgs(&normalized);
		return 1;
	}
	decoder = MP3InitDecoder();
	if (!decoder) {
		fprintf(stderr, "MP3InitDecoder failed\n");
		InputSourceClose(&input);
		CloseInputFile(&infile, opt.debugCleanup);
		if (outfile)
			fclose(outfile);
		free(resolvedOutName);
		AmigaFreeNormalizedArgs(&normalized);
		return 1;
	}
	Radio_CheckMiniMem("after decoder init");

	if (opt.play && opt.stereo)
		fprintf(stderr, "Stereo playback needs significantly more CPU and may underrun on 030.\n");
	GuiPublishStartupStage(GUISTART_DECODER_CONFIG);
	if (opt.play && AmigaPlaybackStopRequested(&opt, "before decoder config")) {
		MP3FreeDecoder(decoder);
		InputSourceClose(&input);
		CloseInputFile(&infile, opt.debugCleanup);
		if (outfile)
			fclose(outfile);
		free(resolvedOutName);
		AmigaFreeNormalizedArgs(&normalized);
		return 1;
	}

	MP3SetOutputMono(decoder, opt.mono && !opt.stereo && !opt.noMonoMSSideSkip);
	MP3SetMonoMSSideSkip(decoder, !opt.noMonoMSSideSkip);
	if (opt.expPoly) {
#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE) && defined(AMIGA_M68K_ASM_POLYPHASE)
		fprintf(stderr, "warning: --exp-poly enables experimental 68030 asm "
			"mono polyphase when real/amiga_m68k_polyphase.S is linked; "
			"otherwise it falls back to the existing fast path\n");
#else
		fprintf(stderr, "warning: --exp-poly requested, but this build has no 68030 asm polyphase; using existing polyphase\n");
#endif
	}
	MP3SetExperimentalPolyphase(opt.expPoly);
	MP3SetForceStereoStride2PolyphaseC(opt.forceCPolyphaseStride2Stereo);
	MP3ResetStereoStride2PolyphaseCounters();
	MP3ResetStereoStride4PolyphaseCounters();
	MP3ResetMonoStride2PolyphaseCounters();
	MP3SetExperimentalHuffman(opt.expHuff);
	MP3SetExperimentalIMDCTThin(decoder, opt.expImdctThin);
	MP3SetExperimentalReducedTaps(opt.expReducedTaps);
	MP3SetExperimentalFDCT32Quarter(opt.expFdct32Quarter ||
		(opt.superfastLowrate && opt.outputRate == 11025));
	if (opt.fastLowrate) {
		int stride = FastLowrateStrideForOutputRate(opt.outputRate);
		if (opt.expReducedTaps && stride != 2 && stride != 4)
			fprintf(stderr, "warning: --exp-reduced-taps only affects 22050/11025 Hz stride-2/stride-4 fast-lowrate output\n");
		if (opt.expFdct32Quarter && stride != 4)
			fprintf(stderr, "warning: --exp-fdct32-quarter only affects 11025 Hz stride-4 fast-lowrate output\n");
		MP3SetFastLowrate(decoder, stride);
		if (opt.superfastLowrate)
			MP3SetSuperfastLowrate(decoder, 1);
		GuiPublishStartupStage(GUISTART_FASTLOWRATE_WARN_BEFORE);
		if (!gMiniAmp3EmbeddedPlayback && opt.outputRate == 22050)
			fprintf(stderr,
				"22050 requires significantly more CPU and may underrun on 030 systems.\n");
		GuiPublishStartupStage(GUISTART_FASTLOWRATE_WARN_AFTER);
#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE)
		if (opt.expReducedTaps) {
#if defined(AMIGA_FAST_REDUCED_TAPS)
			fprintf(stderr, "warning: --exp-reduced-taps enables lossy reduced-tap fast-lowrate dewindowing\n");
#else
			fprintf(stderr, "warning: --exp-reduced-taps requested, but this build lacks AMIGA_FAST_REDUCED_TAPS\n");
#endif
		}
		if (opt.expFdct32Quarter) {
#if defined(AMIGA_FAST_FDCT32_QUARTER)
			fprintf(stderr, "warning: --exp-fdct32-quarter enables lossy stride-4 quarter-rate FDCT32 synthesis\n");
#else
			fprintf(stderr, "warning: --exp-fdct32-quarter requested, but this build lacks AMIGA_FAST_FDCT32_QUARTER\n");
#endif
		}
		if (opt.expImdctThin) {
#if defined(AMIGA_M68K_IMDCT_THIN_OUTPUT)
			fprintf(stderr, "warning: --exp-imdct-thin is disabled because stride-4 playback needs every IMDCT subband for full FDCT32 synthesis\n");
#else
			fprintf(stderr, "warning: --exp-imdct-thin requested, but this build lacks AMIGA_M68K_IMDCT_THIN_OUTPUT\n");
#endif
		}
		fprintf(stderr, "warning: --fast-lowrate is experimental, lower quality, "
			"and only skips polyphase output samples%s\n",
			opt.expFdct32Quarter ? "; FDCT32 uses the requested lossy quarter-rate path" :
			"; IMDCT/DCT32 still run full-rate");
#else
		fprintf(stderr, "warning: --fast-lowrate is experimental and lower quality; "
			"this build still generates full polyphase output before decimation\n");
#endif
	}
	if (opt.subbandCap > 0)
		MP3SetSubbandCap(decoder, opt.subbandCap);
	if (opt.debugSubbandPrecondition)
		MP3EnableFdct32HalfSparse16PreconditionCheck(1);
	if (opt.play && opt.outputRate == 28600)
		fprintf(stderr,
			"28600 PAL-top playback uses normal post-decode decimation and may underrun on 030 systems.\n");

	if (opt.play) {
		int playErr;
		TimingStats *playTiming;

		GuiPublishStartupStage(GUISTART_STREAM_INIT);
		if (AmigaPlaybackStopRequested(&opt, "immediately before playback")) {
			MP3FreeDecoder(decoder);
			InputSourceClose(&input);
			CloseInputFile(&infile, opt.debugCleanup);
			if (outfile)
				fclose(outfile);
			free(resolvedOutName);
			AmigaFreeNormalizedArgs(&normalized);
			return 1;
		}
		playTiming = opt.bench ? &timing : NULL;
		/* Do not clear gPlaybackInterrupted here.  The MintAMP GUI can
		 * signal Stop after PlaybackEntry() resets decoder statics but before
		 * this play block is reached; clearing the flag at this late point
		 * loses that stop request and leaves the old child holding audio.device
		 * while the GUI waits to start the next song/rate.  Fresh CLI starts and
		 * GUI launches already reset the flag before entering main(). */
#ifndef AMIGA_M68K
		signal(SIGINT, PlaybackSignalHandler);
#endif
		gMiniAmp3RequestedVolume = (unsigned short)opt.volumePercent;
		gMiniAmp3VolumeSequence++;
		gTiming = playTiming;
		MP3SetDecodeCoreProfileEnabled(opt.bench);
		if (opt.bench) {
			MP3ResetDecodeCoreProfile();
			startClock = clock();
		}
		if (opt.decodeThenPlay)
			playErr = AmigaPlayDecodeThenPlay(&input, decoder, &opt, &stats, playTiming);
		else
			playErr = AmigaPlayStreaming(&input, decoder, &opt, &stats, playTiming);
		if (opt.bench)
			endClock = clock();
		if (!stats.outputSampleRate)
			stats.outputSampleRate = PlaybackOutputSampleRate(&opt, &stats);
		printf("input sample rate: %d Hz\n", stats.sampleRate);
		PrintFastLowrateOutputRateDifference(&opt, stats.outputSampleRate);
		printf("output sample rate: %d Hz\n", stats.outputSampleRate);
		printf("channels: %d (%s output)\n", stats.channels,
			opt.stereo ? "stereo" : "mono");
		printf("bitrate: %d bps\n", stats.bitrate);
		printf("decoded frames: %lu\n", stats.decodedFrames);
		printf("output samples: %lu\n", stats.outputSamples);
		PrintOutputStats(&opt, &stats);
		if (opt.checksum)
			printf("playback PCM checksum: %08lx\n", stats.pcmChecksum);
		printf("playback underruns: %lu\n", stats.underruns);
		printf("playback underruns buffer 0: %lu\n", stats.underrunBuffers[0]);
		printf("playback underruns buffer 1: %lu\n", stats.underrunBuffers[1]);
		printf("playback underruns buffer 2: %lu\n", stats.underrunBuffers[2]);
		printf("playback late buffers: %lu\n", stats.lateBuffers);
		if (opt.debugSubbandPrecondition)
			printf("FDCT32HalfSparse16 precondition checks: %lu, violations: %lu\n",
				gFdct32HalfSparse16PreconditionChecks,
				gFdct32HalfSparse16PreconditionViolations);
		if (stats.spareTimeMeasured)
			printf("playback minimum spare before buffer end: %ld ms\n",
				stats.minimumSpareMilliseconds);
		else
			printf("playback minimum spare before buffer end: n/a\n");
		if (MP3SuperfastLowrateEnabled(decoder))
			printf("fast-lowrate stride: %d (superfast: IMDCT/overlap capped to %d of %d subbands; %s)\n",
				MP3GetFastLowrateStride(decoder),
				MP3GetFastLowrateActiveSubbands(decoder), 32,
				(MP3GetFastLowrateStride(decoder) == 4 &&
				 MP3ExperimentalFDCT32QuarterEnabled()) ?
					"FDCT32Quarter" : "FDCT32 full-rate");
		else
			printf("fast-lowrate stride: %d (fast-lowrate: IMDCT/DCT32 full-rate)\n",
				MP3GetFastLowrateStride(decoder));
		if (opt.bench) {
			PrintBenchmarkSeconds(&opt, &stats, startClock, endClock);
			printf("playback underruns: %lu\n", stats.underruns);
			printf("playback underruns buffer 0: %lu\n", stats.underrunBuffers[0]);
			printf("playback underruns buffer 1: %lu\n", stats.underrunBuffers[1]);
			printf("playback underruns buffer 2: %lu\n", stats.underrunBuffers[2]);
			printf("playback late buffers: %lu\n", stats.lateBuffers);
			if (stats.spareTimeMeasured)
				printf("playback minimum spare before buffer end: %ld ms\n",
					stats.minimumSpareMilliseconds);
			else
				printf("playback minimum spare before buffer end: n/a\n");
			PrintClockSeconds("timing frame decode: ", timing.frameDecode);
			PrintClockSeconds("timing PCM conversion: ", timing.pcmConvert);
		}
#ifndef AMIGA_M68K
		signal(SIGINT, SIG_DFL);
#endif
		{
			unsigned long teardownSessionId = (input.radio) ? Radio_GetSessionId(input.radio) : 0UL;
			int teardownFatal = (input.radio) ? Radio_IsSessionFatal(input.radio) : 0;

			RADIO_DBG(printf("radio-teardown: post-audio cleanup begin session=%lu fatal=%d\n", teardownSessionId, teardownFatal));
			if (AmigaPostAudioLeakTestEnabled()) {
				/* Test mode (MP3_POST_AUDIO_LEAK_TEST=1): deliberately skip
				 * MP3FreeDecoder()/InputSourceClose() (which frees the
				 * decoder, the radio ring buffer, and the RadioStream
				 * struct/socket/SSL context via Radio_Close()) and leak them
				 * instead.  If a reboot that used to happen right here stops
				 * happening with this set, the corruptor is a use-after-free
				 * or double-free in that free/close path, not in
				 * AmigaAudioClose() (already fixed) or upstream of it. */
				RADIO_DBG(printf("radio-teardown: MP3_POST_AUDIO_LEAK_TEST active -- leaking decoder/ring/stream session=%lu\n",
					teardownSessionId));
			} else {
				Radio_DebugCheckExecMem("before decoder free/skip");
				if (teardownFatal) {
					/* Fatal TLS teardown quarantine (see
					 * Radio_IsSessionFatal()): the decoder context is part of
					 * the same child-owned object graph the fatal fault may
					 * have damaged by corrupting exec memory around it --
					 * leak it instead of risking MP3FreeDecoder() writing
					 * into/through already-damaged heap state. */
					RADIO_DBG(printf("radio-teardown: decoder free skipped (fatal TLS quarantine) session=%lu -- leaking\n",
						teardownSessionId));
				} else {
					RADIO_DBG(printf("radio-teardown: before decoder free session=%lu\n", teardownSessionId));
					MP3FreeDecoder(decoder);
					RADIO_DBG(printf("radio-teardown: after decoder free session=%lu\n", teardownSessionId));
				}
				Radio_DebugCheckExecMem("after decoder free/skip");
				Radio_CheckMiniMem("after decoder cleanup");
				RADIO_DBG(printf("radio-teardown: before Radio_Close second stop phase (InputSourceClose) session=%lu\n",
					teardownSessionId));
				InputSourceClose(&input);
				RADIO_DBG(printf("radio-teardown: after InputSourceClose/Radio_Close session=%lu\n", teardownSessionId));
				CloseInputFile(&infile, opt.debugCleanup);
			}
			gTiming = NULL;
			MP3SetDecodeCoreProfileEnabled(0);
			RADIO_DBG(printf("radio-teardown: freeing resolvedOutName=%p session=%lu\n",
				(void *)resolvedOutName, teardownSessionId));
			free(resolvedOutName);
			RADIO_DBG(printf("radio-teardown: freeing normalized args session=%lu\n", teardownSessionId));
			AmigaFreeNormalizedArgs(&normalized);
			RADIO_DBG(printf("radio-teardown: post-audio cleanup end, returning from main playErr=%d session=%lu\n",
				playErr, teardownSessionId));
		}
		return playErr == 0 ? 0 : 1;
	}

	bytesLeft = 0;
	eofReached = 0;
	outOfData = 0;
	svxOpen = 0;
	verifyError = 0;
	readPtr = readBuf;
	gTiming = opt.bench ? &timing : NULL;
	MP3SetDecodeCoreProfileEnabled(opt.bench);
	if (opt.bench)
		MP3ResetDecodeCoreProfile();
	effectiveRate = 0;
	if (opt.bench)
		startClock = clock();

	while (!outOfData) {
		int nRead;
		int offset;
		int err;
		unsigned char *frameStart;
		int frameBytes;

		if (bytesLeft < 2 * MAINBUF_SIZE && !eofReached) {
			nRead = FillReadBuffer(readBuf, readPtr, READBUF_SIZE,
				bytesLeft, &input);
			bytesLeft += nRead;
			readPtr = readBuf;
			if (nRead == 0)
				eofReached = 1;
		}

		offset = FindValidatedMpegSync(readPtr, bytesLeft);
		if (offset < 0) {
			if (eofReached)
				break;
			if (bytesLeft > 3) {
				readPtr += bytesLeft - 3;
				bytesLeft = 3;
			}
			continue;
		}

		readPtr += offset;
		bytesLeft -= offset;
		InputSourceAlignDecodePointer(readBuf, &readPtr, &bytesLeft);
		frameStart = readPtr;
		frameBytes = bytesLeft;

		if (opt.bench) {
			clock_t t0 = clock();
			err = MP3Decode(decoder, &readPtr, &bytesLeft, decodeBuf, 0);
			timing.frameDecode += clock() - t0;
		} else {
			err = MP3Decode(decoder, &readPtr, &bytesLeft, decodeBuf, 0);
		}
		if (err) {
			if (err == ERR_MP3_INDATA_UNDERFLOW &&
				stats.decodedFrames == 0 && frameBytes > 1) {
				readPtr = frameStart + 1;
				bytesLeft = frameBytes - 1;
			} else if (err == ERR_MP3_INDATA_UNDERFLOW) {
				outOfData = 1;
			} else if (err == ERR_MP3_MAINDATA_UNDERFLOW) {
				/* Need more main data from later frames; keep decoding. */
			} else if (stats.decodedFrames == 0 && frameBytes > 1) {
				/* Rescan after a bad first candidate before giving up. */
				readPtr = frameStart + 1;
				bytesLeft = frameBytes - 1;
			} else {
				fprintf(stderr, "decode error %d after %lu frames\n",
					err, stats.decodedFrames);
				outOfData = 1;
			}
			continue;
		}

		MP3GetLastFrameInfo(decoder, &info);
		if (opt.debugFastLowrate) {
			MP3FastLowrateGranuleDebug fastDbg[MAX_NGRAN];
			int dbgCount = MP3GetFastLowrateDebug(decoder, fastDbg, MAX_NGRAN);
			int dbgIndex;
			for (dbgIndex = 0; dbgIndex < dbgCount && dbgIndex < MAX_NGRAN; dbgIndex++) {
				fprintf(stderr,
					"fast-lowrate frame=%lu granule=%d stride=%d "
					"phase=%d..%d full-rate-samps=%d lowrate-samps=%d "
					"cumulative-lowrate-samps=%d dest-offset=%d..%d\n",
					stats.decodedFrames, fastDbg[dbgIndex].granule,
					fastDbg[dbgIndex].stride, fastDbg[dbgIndex].phaseStart,
					fastDbg[dbgIndex].phaseEnd,
					fastDbg[dbgIndex].fullRateSamps,
					fastDbg[dbgIndex].lowrateSamps,
					fastDbg[dbgIndex].cumulativeLowrateSamps,
					fastDbg[dbgIndex].destOffsetStart,
					fastDbg[dbgIndex].destOffsetEnd);
			}
		}
		UpdateFirstFrameStats(&stats, &info);
		if (opt.checksum && !opt.fastLowrate)
			stats.pcmChecksum = UpdatePcmChecksum(stats.pcmChecksum, decodeBuf,
				info.outputSamps);
		if (!effectiveRate) {
			effectiveRate = EffectiveOutputSampleRate(&opt, info.samprate);
			stats.outputSampleRate = effectiveRate;
		}
		if (!stats.outputChannels)
			stats.outputChannels = (opt.mono || info.nChans <= 1) ? 1 : info.nChans;

		if (!opt.decodeOnly && opt.outFormat == OUT_8SVX && !svxOpen) {
			if (!info.samprate) {
				fprintf(stderr, "cannot write 8SVX before sample rate is known\n");
				outOfData = 1;
				break;
			}
			if (opt.noOutput) {
				InitNoOutputSvx(&svx, opt.compression);
			} else {
				int beginErr;
				if (opt.bench) {
					clock_t t0 = clock();
					beginErr = SvxBegin(&svx, outfile, effectiveRate, opt.compression);
					timing.svxWrite += clock() - t0;
				} else {
					beginErr = SvxBegin(&svx, outfile, effectiveRate, opt.compression);
				}
				if (beginErr != 0) {
					fprintf(stderr, "cannot write 8SVX header\n");
					outOfData = 1;
					break;
				}
			}
			svxOpen = 1;
		}

		if (opt.decodeOnly) {
			const short *accountBuf;
			int accountSamps;
			int decoderOutputChannels;

			accountBuf = decodeBuf;
			accountSamps = info.outputSamps;
			decoderOutputChannels = MP3GetOutputChannels(decoder);
			if (opt.mono && info.nChans > 1 && decoderOutputChannels != 1) {
				accountSamps = MixFrame(decodeBuf, writeBuf, info.outputSamps,
					info.nChans, 1);
				accountBuf = writeBuf;
			}
			if (opt.checksum && opt.fastLowrate)
				stats.pcmChecksum = UpdatePcmChecksum(stats.pcmChecksum, accountBuf,
					accountSamps);
			stats.outputSamples += (unsigned long)accountSamps;
		} else {
			int outSamps;
			int outChannels;
			int writeErr;
			clock_t t0;

			if (opt.bench)
				t0 = clock();
			outChannels = MP3GetOutputChannels(decoder);
			if (opt.mono && info.nChans > 1 && outChannels == 1) {
				if (writeBuf != decodeBuf)
					memmove(writeBuf, decodeBuf, info.outputSamps * sizeof(short));
				outSamps = info.outputSamps;
			} else {
				outSamps = MixFrame(decodeBuf, writeBuf, info.outputSamps,
					info.nChans, opt.mono);
				outChannels = (opt.mono || info.nChans <= 1) ? 1 : info.nChans;
			}
			stats.outputChannels = outChannels;
			if (!opt.fastLowrate && opt.outputRate && info.samprate > opt.outputRate) {
				outSamps = DownsampleFrame(&rateState, writeBuf, rateBuf, outSamps,
					info.samprate, opt.outputRate, outChannels);
				memmove(writeBuf, rateBuf, outSamps * sizeof(short));
			}
			if (opt.checksum && opt.fastLowrate)
				stats.pcmChecksum = UpdatePcmChecksum(stats.pcmChecksum, writeBuf,
					outSamps);
			if (opt.bench)
				timing.pcmConvert += clock() - t0;

			if (opt.outFormat == OUT_8SVX) {
				if (opt.bench) {
					t0 = clock();
					writeErr = SvxWriteSamples(&svx, writeBuf, outSamps);
					timing.svxWrite += clock() - t0;
				} else {
					writeErr = SvxWriteSamples(&svx, writeBuf, outSamps);
				}
			} else {
				writeErr = WriteRawSamples(opt.noOutput ? NULL : outfile, writeBuf,
					outSamps, opt.outFormat);
			}

			if (writeErr != 0) {
				fprintf(stderr, "output write error\n");
				outOfData = 1;
				break;
			}
			stats.outputSamples += (unsigned long)outSamps;
		}

		stats.decodedFrames++;
	}

	if (svxOpen) {
		clock_t t0;
		if (opt.bench)
			t0 = clock();
		if (SvxEnd(&svx) != 0) {
			fprintf(stderr, "error finalizing 8SVX file\n");
			verifyError = 1;
		}
		if (svx.sourceSamples != stats.outputSamples) {
			fprintf(stderr,
				"8SVX VHDR sample count mismatch: vhdr=%lu output=%lu\n",
				svx.sourceSamples, stats.outputSamples);
			verifyError = 1;
		}
		if (svx.compression == SVX_COMP_NONE && svx.bodyBytes != svx.sourceSamples) {
			fprintf(stderr,
				"8SVX BODY/sample count mismatch: body=%lu samples=%lu\n",
				svx.bodyBytes, svx.sourceSamples);
			verifyError = 1;
		}
		if (opt.bench)
			timing.svxWrite += clock() - t0;
	}

	if (opt.bench)
		endClock = clock();

	if (!stats.outputSampleRate)
		stats.outputSampleRate = effectiveRate ? effectiveRate : stats.sampleRate;
	printf("input sample rate: %d Hz\n", stats.sampleRate);
	PrintFastLowrateOutputRateDifference(&opt, stats.outputSampleRate);
	if (stats.outputSampleRate && stats.outputSampleRate != stats.sampleRate)
		printf("output sample rate: %d Hz\n", stats.outputSampleRate);
	printf("channels: %d%s\n", stats.channels, opt.mono ? " (mono output)" : "");
	printf("bitrate: %d bps\n", stats.bitrate);
	printf("decoded frames: %lu\n", stats.decodedFrames);
	printf("output samples: %lu\n", stats.outputSamples);
	PrintOutputStats(&opt, &stats);
	if (opt.checksum)
		printf("%s PCM checksum: %08lx\n",
			opt.fastLowrate ? "fast-lowrate output" : "decoded",
			stats.pcmChecksum);
	if (opt.fastLowrate) {
		if (MP3SuperfastLowrateEnabled(decoder))
			printf("fast-lowrate stride: %d (superfast: IMDCT/overlap capped to %d of %d subbands; %s)\n",
				MP3GetFastLowrateStride(decoder),
				MP3GetFastLowrateActiveSubbands(decoder), 32,
				(MP3GetFastLowrateStride(decoder) == 4 &&
				 MP3ExperimentalFDCT32QuarterEnabled()) ?
					"FDCT32Quarter" : "FDCT32 full-rate");
		else
			printf("fast-lowrate stride: %d (fast-lowrate: IMDCT/DCT32 full-rate)\n",
				MP3GetFastLowrateStride(decoder));
	}

	if (opt.bench) {
		PrintBenchmarkSeconds(&opt, &stats, startClock, endClock);
		{
			MP3DecodeCoreProfile coreProfile;

			MP3GetDecodeCoreProfile(&coreProfile);
			printf("decode-core profiling: %s\n",
				MP3DecodeCoreProfileIsEnabled() ? "enabled" : "disabled");
			if (MP3DecodeCoreProfileIsEnabled()) {
				PrintClockSeconds("timing core bitstream/frame parsing: ", coreProfile.bitstreamFrameParsing);
				PrintClockSeconds("timing core huffman: ", coreProfile.huffman);
				PrintClockSeconds("timing core dequant: ", coreProfile.dequant);
				PrintClockSeconds("timing core stereo/post: ", coreProfile.stereoPost);
				PrintClockSeconds("timing core imdct: ", coreProfile.imdct);
				PrintClockSeconds("timing core subband/dct32: ", coreProfile.subbandDct32);
				PrintClockSeconds("timing core polyphase: ", coreProfile.polyphase);
				{
					unsigned long m2Asm = 0, m2C = 0, m2Reduced = 0;
					MP3GetMonoStride2PolyphaseCounters(&m2Asm, &m2C, &m2Reduced);
					printf("mono stride-2 polyphase: %s\n",
						m2Reduced ? "reduced" : (m2Asm ? "asm" : "C"));
					printf("mono stride-2 polyphase calls: asm=%lu C=%lu reduced=%lu\n",
						m2Asm, m2C, m2Reduced);
				}
				{
					unsigned long s2Asm = 0, s2C = 0, s2Reduced = 0;
					MP3GetStereoStride2PolyphaseCounters(&s2Asm, &s2C, &s2Reduced);
					printf("stereo stride-2 polyphase: %s\n",
						s2Reduced ? "reduced" : (s2Asm ? "asm" : "C"));
					printf("stereo stride-2 polyphase calls: asm=%lu C=%lu reduced=%lu\n",
						s2Asm, s2C, s2Reduced);
				}
				{
					unsigned long s4Asm = 0, s4C = 0, s4Reduced = 0;
					MP3GetStereoStride4PolyphaseCounters(&s4Asm, &s4C, &s4Reduced);
					printf("stereo stride-4 polyphase: %s\n",
						s4Reduced ? "reduced" : (s4Asm ? "asm" : "C"));
					printf("stereo stride-4 polyphase calls: asm=%lu C=%lu reduced=%lu\n",
						s4Asm, s4C, s4Reduced);
				}
				printf("core IMDCT subbands: executed=%lu skipped=%lu\n",
					coreProfile.imdctSubbandsExecuted, coreProfile.imdctSubbandsSkipped);
				{
					unsigned long imdctTotalBlocks = coreProfile.imdct36BlockCount + coreProfile.imdct12x3BlockCount;
					printf("imdct block kind: IMDCT36(long)=%lu IMDCT12x3(short)=%lu short-block%%=",
						coreProfile.imdct36BlockCount, coreProfile.imdct12x3BlockCount);
					PrintIntegerRatio("", IntegerScale(coreProfile.imdct12x3BlockCount, imdctTotalBlocks, 1000, 1), 10, 1, "\n");
				}
				printf("mono M/S side-channel skip: eligible=%lu huffman=%lu dequant=%lu imdct=%lu synthesis=%lu\n",
					coreProfile.monoMSSideSkipEligible,
					coreProfile.monoMSSideHuffmanSkipped,
					coreProfile.monoMSSideDequantSkipped,
					coreProfile.monoMSSideIMDCTSkipped,
					coreProfile.monoMSSideSynthesisSkipped);
				printf("mono M/S fallback: not-stereo-source=%lu output-stereo=%lu not-joint=%lu no-ms=%lu intensity=%lu disabled=%lu malformed=%lu\n",
					coreProfile.monoMSSideFallbackNotStereoSource,
					coreProfile.monoMSSideFallbackOutputStereo,
					coreProfile.monoMSSideFallbackNotJointStereo,
					coreProfile.monoMSSideFallbackNoMS,
					coreProfile.monoMSSideFallbackIntensity,
					coreProfile.monoMSSideFallbackDisabled,
					coreProfile.monoMSSideFallbackMalformed);
				printf("intensity stereo usage: MPEG1=%lu MPEG2=%lu with-M/S=%lu intensity-only=%lu\n",
					coreProfile.intensityMPEG1Count,
					coreProfile.intensityMPEG2Count,
					coreProfile.intensityWithMidSideCount,
					coreProfile.intensityOnlyCount);
				if (opt.fastLowrate)
					printf("sparse low-rate: stride=%d active-subbands=%d fdct=%s\n",
						FastLowrateStrideForOutputRate(opt.outputRate),
						FastLowrateStrideForOutputRate(opt.outputRate) == 2 ? 16 :
						(FastLowrateStrideForOutputRate(opt.outputRate) == 4 ? 8 : 32),
						FastLowrateStrideForOutputRate(opt.outputRate) == 4 && opt.superfastLowrate ?
						"FDCT32Quarter" : (FastLowrateStrideForOutputRate(opt.outputRate) == 2 ?
						"FDCT32Half" : "FDCT32"));
			}
		}
		PrintClockSeconds("timing frame decode: ", timing.frameDecode);
		PrintClockSeconds("timing PCM conversion: ", timing.pcmConvert);
		PrintClockSeconds("timing 8SVX write: ", timing.svxWrite);
		PrintClockSeconds("timing Fibonacci compression: ", timing.fibCompress);
		PrintClockSeconds("timing file writing: ", timing.fileWrite);
	}

	MP3FreeDecoder(decoder);
	Radio_CheckMiniMem("after decoder cleanup");
	InputSourceClose(&input);
	CloseInputFile(&infile, opt.debugCleanup);
	if (outfile)
		fclose(outfile);
	gTiming = NULL;
	MP3SetDecodeCoreProfileEnabled(0);
	free(resolvedOutName);
	AmigaFreeNormalizedArgs(&normalized);

	return verifyError ? 1 : 0;
}
