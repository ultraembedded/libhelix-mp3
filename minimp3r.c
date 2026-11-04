#include "interface_prefs.h"
/*
 * MintAMP - Mini Internet Amiga Media Player ReAction/ClassAct frontend for the Helix fixed-point
 * MP3 decoder, aimed at AmigaOS 3.3/3.5/3.9 (and 68k boards with ReAction or
 * the older ClassAct distribution installed).
 *
 * Like the GadTools frontend (amiga_mp3gui.c) this wraps the existing
 * amiga_mp3dec playback engine: the decoder source is compiled straight into
 * this translation unit with main() renamed to HelixAmp3CliMain(), and a small
 * child process feeds it the same --play/--rate/--buffer-seconds argument set
 * the Shell command would use.  All of the decode/Paula-streaming code is the
 * proven path; only the user interface differs.
 *
 * Build it from the Makefile with:  make -f Makefile.amiga guir
 *
 * The window is assembled entirely from BOOPSI gadget classes (window.class,
 * layout.gadget, getfile.gadget, chooser.gadget, slider.gadget,
 * checkbox.gadget, fuelgauge.gadget, string.gadget and label.image) so it gets
 * a native ReAction look and resizes cleanly on a 3.x/3.9 Workbench.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "radio_debug.h"
#include "radio_runtime_flags.h"
#include "amiga_display_text.h"
#include "miniamp_memguard.h"
#if defined(MINTAMP_SKIN) && defined(AMIGA_M68K)
#include "skin_player.h"
static SkinPlayer *gSkinPlayer;
static int gSkinPendingTrack = -1;
/* Startup arguments for the first skin opened, possibly after Classic. */
static int gSkinArgc;
static char **gSkinArgv;
static char gSkinPendingUrl[512];
#endif

#if defined(AMIGA_M68K)

/* Pull the whole decoder/playback engine into this object, with its command
 * line entry point renamed.  Mirrors the trick used by amiga_mp3gui.c so we
 * share gGuiPlaybackStatus, gMiniAmp3EmbeddedPlayback and gPlaybackInterrupted
 * with the playback child without any extra glue. */
/* Tell amiga_mp3dec.c's main() (renamed to HelixAmp3CliMain just below, and
 * used as the per-playback-child entry point in this build) not to
 * InitSemaphore() radio_console_lock itself -- this file's own real main()
 * does that exactly once, before any child/worker task exists to race it. */
#define RADIO_CONSOLE_LOCK_INIT_ELSEWHERE 1
#define MINTAMP_EMBEDDED_FRONTEND 1
#define main HelixAmp3CliMain
#include "amiga_mp3dec.c"
#undef main
#undef MINTAMP_EMBEDDED_FRONTEND
#undef printf
#undef fprintf
#undef fputs
#undef puts
#undef putchar
#undef fflush
#undef fwrite

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/tasks.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <devices/timer.h>
#include <intuition/intuition.h>
#include <intuition/gadgetclass.h>
#include <intuition/icclass.h>
#include <workbench/workbench.h>
#include <hardware/cia.h>
#include "picojpeg.h"
#include "lodepng.h"
#include "webpdec.h"
#include "svgdec.h"
#include "radio_stream.h"
#include "radio_browser_controller.h"
#include "radio_browser_http.h"
#include "playlist_format.h"
#include "radio_favourites.h"

/* See the matching comment in amiga_mp3gui.c's GUI_ENV_PREFIX: bare name,
 * no explicit device prefix -- GVF_SAVE_VAR already constructs the
 * persistent "ENVARC:" + name path itself, and an explicit "ENVARC:" baked
 * in here doubled up into a malformed path that silently failed to persist
 * across reboots while the plain ENV: (RAM) write kept working. */
#define MR_ENV_PREFIX "MintAMP"
#define MINTAMP_VERSION "2.0-dev"
#define MR_SETTINGS_VERSION 1
#define MR_RADIO_FAV_MAX 50

/* AmigaOS Version command metadata; unrelated to MR_SETTINGS_VERSION. */
static const char gMintAmpVersionTag[] __attribute__((used)) =
#ifdef MINTAMP_SKIN
	"\0$VER: MintAMP-SR " MINTAMP_VERSION " (10.10.2026)";
#else
	"\0$VER: MintAMP " MINTAMP_VERSION " (08.10.2026)";
#endif
#if !defined(__AROS__) && !defined(MR_DISABLE_CIA_FILTER)
#define MR_ENABLE_CIA_FILTER 1
#endif


#include <classes/window.h>
#include <gadgets/layout.h>
#include <gadgets/button.h>
#include <gadgets/getfile.h>
#include <gadgets/chooser.h>
#include <gadgets/listbrowser.h>
#include <gadgets/slider.h>
#include <gadgets/checkbox.h>
#include <gadgets/fuelgauge.h>
#include <gadgets/string.h>
#include <images/label.h>

#include <reaction/reaction.h>
#include <reaction/reaction_macros.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <workbench/startup.h>
#include <proto/intuition.h>
#include <proto/utility.h>
#include <proto/asl.h>
#include <proto/window.h>
#include <proto/layout.h>
#include <proto/button.h>
#include <proto/getfile.h>
#include <proto/chooser.h>
#include <proto/listbrowser.h>
#include <proto/slider.h>
#include <proto/checkbox.h>
#include <proto/fuelgauge.h>
#include <proto/string.h>
#include <proto/label.h>
#include <proto/graphics.h>
#include <proto/icon.h>

/* ------------------------------------------------------------------------- */
/* Tunables                                                                  */
/* ------------------------------------------------------------------------- */

/* OpenLibrary version for the ReAction classes.  V44 is the OS 3.5/3.9
 * ReAction baseline; users running the older ClassAct 2.x distribution on
 * OS 3.1/3.3 can drop this to the version their classes report (typically
 * 41-43) and rebuild. */
#ifndef MINIMP3R_CLASS_VERSION
#define MINIMP3R_CLASS_VERSION 44
#endif

#define MR_MAX_PATH      256
#define MR_ARGC_MAX      40
#define MR_PLAYLIST_MAX  128
#define MR_ART_W        64
#define MR_ART_H        64
#define MR_ART_COLOR_CACHE 64
#define MR_MAX_JPEG_DIM 1024
#define MR_QUALITY_MIN   0
#define MR_QUALITY_MAX   3
#define MR_STARTUP_STACK_SIZE 262144UL

/* Optional Radio Browser station-favicon artwork.  Disable by building with
 * -DENABLE_RADIO_ARTWORK=0; it never touches stream playback either way. */
#ifndef ENABLE_RADIO_ARTWORK
#define ENABLE_RADIO_ARTWORK 1
#endif
/* PNG favicon support via lodepng (most Radio Browser favicons are PNG).
 * Disable by building with -DENABLE_PNG_ARTWORK=0 -- note this only skips
 * the decode calls; lodepng.c must also be dropped from the Makefile's
 * source list to actually shrink the binary. */
#ifndef ENABLE_PNG_ARTWORK
#define ENABLE_PNG_ARTWORK 1
#endif
/* SVG favicon support via svgdec.c (a small from-scratch, fixed-point
 * subset decoder -- see svgdec.h).  Disable by building with
 * -DENABLE_SVG_ARTWORK=0 and dropping svgdec.c from the Makefile's
 * source list to shrink the binary. */
#ifndef ENABLE_SVG_ARTWORK
#define ENABLE_SVG_ARTWORK 1
#endif
/* WebP favicon support via webpdec.c (a small from-scratch VP8/VP8L decoder).
 * Disable by building with -DENABLE_WEBP_ARTWORK=0 and dropping webpdec.c from
 * the Makefile's source list to shrink the binary. */
#ifndef ENABLE_WEBP_ARTWORK
#define ENABLE_WEBP_ARTWORK 1
#endif
#define MR_FAVICON_MAX_BYTES (256L * 1024L)

/* ------------------------------------------------------------------------- */
/* Recoverable-free diagnostics (main/GUI-task FreeMem alert hunt).           */
/*                                                                            */
/* The last hardware run finished without heap corruption or a crash, but     */
/* AmigaOS still raised two recoverable Exec alerts, both from ONE task:       */
/*     01000009  AN_FreeTwice    -- the same block handed to FreeMem twice     */
/*     0100000F  AN_BadFreeAddr  -- FreeMem given an address Exec's allocator  */
/*                                  has no memory header for                   */
/* The alerting task was NOT the radio net worker (that task's pointer is      */
/* logged separately by radio_stream.c), so the GUI/main application task is   */
/* the prime suspect.  A stale-but-non-NULL owner pointer freed a second time  */
/* looks identical to these two alerts, which is why guarding a free with      */
/* "if (ptr) FreeMem(ptr)" is not enough: the pointer has to be proven live.   */
/*                                                                            */
/* These numbered BEGIN/END records log FindTask(NULL) at every main-task      */
/* free site together with the pointer, its owner/type, the session or         */
/* generation it belongs to, and whether the owner field was already cleared   */
/* (cleared=1 on a BEGIN is exactly the stale-owner double-free the alert       */
/* describes).  Pairing every FreeMem/free with a BEGIN before and an END      */
/* after also brackets the exact call whose FreeMem raises the alert on the    */
/* next run, so the log's last un-paired BEGIN names the culprit.  RADIO_DEBUG- */
/* only and serialized through radio_console_lock like every other RADIO_DBG.  */
#ifdef RADIO_DEBUG
static unsigned long gMrFreeAuditSeq;

static void MrFreeAuditLog(const char *phase, const char *site,
	const char *owner, const void *ptr, unsigned long generation)
{
	RADIO_DBG_PRINTF(("free-audit[%lu] %s site=%s owner=%s task=%p ptr=%p gen=%lu cleared=%d\n",
		gMrFreeAuditSeq, phase, site ? site : "?", owner ? owner : "?",
		(void *)FindTask(NULL), ptr, generation, ptr ? 0 : 1));
}

/* Log the identity of the running task at a life-cycle checkpoint, so the log
 * ties the FindTask(NULL) pointer in the free records above to a named phase
 * (startup, event loop, artwork, stream-done, audio cleanup, shutdown). */
static void MrTaskIdentityLog(const char *phase)
{
	RADIO_DBG_PRINTF(("free-audit-task: phase=%s task=%p\n",
		phase ? phase : "?", (void *)FindTask(NULL)));
}

#define MR_FREE_BEGIN(site, owner, ptr, gen) \
	do { ++gMrFreeAuditSeq; MrFreeAuditLog("BEGIN", (site), (owner), (const void *)(ptr), (unsigned long)(gen)); } while (0)
#define MR_FREE_END(site, owner, ptr, gen) \
	MrFreeAuditLog("END", (site), (owner), (const void *)(ptr), (unsigned long)(gen))
#define MR_TASK_IDENTITY(phase) MrTaskIdentityLog((phase))
#else
#define MR_FREE_BEGIN(site, owner, ptr, gen) do { } while (0)
#define MR_FREE_END(site, owner, ptr, gen) do { } while (0)
#define MR_TASK_IDENTITY(phase) do { } while (0)
#endif

/* How often we poll the shared playback status block while a track plays.
 * Keep the heartbeat responsive, but throttle expensive text redraws below. */
#define MR_TICK_MICROS   250000UL
#define MR_METADATA_TICKS 4
#define MR_TIME_TICKS     2
/* How long Stop is allowed to sit outstanding (child signalled but never
 * confirmed gone) before the GUI gives up waiting silently and says so.
 * See the matching define/comment in amiga_mp3gui.c. */
#define STOP_WATCHDOG_TIMEOUT_MICROS (20UL * 1000000UL)
/* Bound on AppCloseShutdown()'s re-signal loop: each iteration delays 5
 * ticks (Delay(5), ~1/10s at the usual 50Hz jiffy rate), so this is roughly
 * a minute total before giving up on a wedged playback child and letting
 * the app close anyway rather than hang the whole task forever. */
#define APP_CLOSE_WEDGED_CHILD_MAX_TICKS 600

#ifdef REACTION_POLL_DEBUG
#define MR_POLL_DBG(args) do { printf args; } while (0)
#else
#define MR_POLL_DBG(args) do { } while (0)
#endif

/* Mirror the phase/startup constants the decoder publishes.  They are defined
 * inside amiga_mp3dec.c only for non-AMIGA builds, so re-declare the few we use
 * here for the m68k path. */
#ifndef GUIPLAY_PHASE_IDLE
#define GUIPLAY_PHASE_IDLE      0
#define GUIPLAY_PHASE_BUFFERING 1
#define GUIPLAY_PHASE_PLAYING   2
#define GUIPLAY_PHASE_UNDERRUN  3
#define GUIPLAY_PHASE_DONE      4
#define GUIPLAY_PHASE_STOPPING  5
#define GUIPLAY_PHASE_ERROR     6
#endif

/* ------------------------------------------------------------------------- */
/* Gadget IDs                                                                */
/* ------------------------------------------------------------------------- */

enum {
	GID_FILE = 1,
	GID_RATE,
	GID_QUALITY,
	GID_SUBBAND_CAP,
	GID_CHANNEL,
	GID_VOLUME,
	GID_BUFFER,
	GID_FASTMEM,
	GID_SPEED,
	GID_WIDTH,
	GID_DELAY,
	GID_PLAY,
	GID_NEXT,
	GID_STOP,
	GID_REW,
	GID_FFWD,
	GID_FILTER,
	GID_PLAYLIST,
	GID_RADIO,
	GID_TIME,
	GID_FILEINFO,
	GID_TITLE,
	GID_ARTIST,
	GID_ALBUM,
	GID_TRACK,
	GID_GENRE,
	GID_RATING,
	GID_STAR1,
	GID_STAR2,
	GID_STAR3,
	GID_STAR4,
	GID_STAR5,
	GID_LAST
};

/* ------------------------------------------------------------------------- */
/* Option tables (shared with the CLI flag set the decoder understands)      */
/* ------------------------------------------------------------------------- */

/* 8287 Hz removed: it failed often enough in practice to not be worth
 * offering, and 8820 Hz already covers the same "lowest available rate"
 * role.  14700 Hz (stride 3) sits between 11025 and 22050 as a middle-ground
 * rate -- see the matching entry in amiga_mp3gui.c's kRates.
 * kRates[MR_RATE_22050_INDEX] must stay "22050" -- several speed/
 * ultrafast/CD32 code paths below key off that specific rate. */
static const char * const kRates[] = {
	"8820", "11025", "14700", "22050", "28600"
};
#define MR_RATE_COUNT  ((int)(sizeof(kRates) / sizeof(kRates[0])))
#define MR_RATE_22050_INDEX 3

static const STRPTR kRateLabels[] = {
	(STRPTR)"8820 Hz",
	(STRPTR)"11025 Hz",
	(STRPTR)"14700 Hz",
	(STRPTR)"22050 Hz",
	(STRPTR)"28600 Hz",
	NULL
};

static const STRPTR kQualityLabels[] = {
	(STRPTR)"Faster",
	(STRPTR)"Fast",
	(STRPTR)"Normal",
	(STRPTR)"Best",
	NULL
};

/* Manual override for --subband-cap N -- see the matching comment in
 * amiga_mp3gui.c's kSubbandCapLabels. "Auto" (index 0) leaves whatever the
 * active fast-lowrate/ultrafast preset already picked untouched. */
static const STRPTR kSubbandCapLabels[] = {
	(STRPTR)"Auto",
	(STRPTR)"26",
	(STRPTR)"20",
	(STRPTR)"16",
	(STRPTR)"12",
	(STRPTR)"10",
	(STRPTR)"8",
	NULL
};
static const int kSubbandCapValues[] = { 0, 26, 20, 16, 12, 10, 8 };
#define SUBBAND_CAP_COUNT (sizeof(kSubbandCapValues) / sizeof(kSubbandCapValues[0]))

static const STRPTR kChannelLabels[] = {
	(STRPTR)"Stereo",
	(STRPTR)"Mono",
	NULL
};
static const STRPTR kSpeedLabels[] = {
	(STRPTR)"Normal",
	(STRPTR)"Superfast low-rate",
	(STRPTR)"Ultrafast",
	(STRPTR)"22050 Mono Ultrafast",
	NULL
};

static const STRPTR kSpeedLabelsNo22050[] = {
	(STRPTR)"Normal",
	(STRPTR)"Superfast low-rate",
	(STRPTR)"Ultrafast",
	NULL
};

static const STRPTR kWidthLabels[] = {
	(STRPTR)"Normal M/S",
	(STRPTR)"Fake stereo 1",
	(STRPTR)"Fake stereo 2",
	(STRPTR)"Fake stereo 3",
	(STRPTR)"Fake stereo 4",
	(STRPTR)"Fake stereo 5",
	NULL
};

static const int kFakeStereoShifts[] = { 1, 2, 3, 4, 5 };

static const STRPTR kDelayLabels[] = {
	(STRPTR)"48", (STRPTR)"64", (STRPTR)"96", (STRPTR)"128", (STRPTR)"192", NULL
};

static const int kFakeStereoDelays[] = { 48, 64, 96, 128, 192 };

#define MENUNUM_PROJECT   0
#define MENUNUM_PLAYBACK  1
#define ITEMNUM_ABOUT     0
#define ITEMNUM_RADIO     1
#define ITEMNUM_ICONIFY   2
#define ITEMNUM_QUIT      3
#define ITEMNUM_DTP       0
#define ITEMNUM_BENCH     1
#define ITEMNUM_ARTWORK   2
#define ITEMNUM_ARTCACHE  3
#define ITEMNUM_ARTCOLOR  4
#define ITEMNUM_ARTREFRESH 5
#define ITEMNUM_ARTRELOAD  6
#define ITEMNUM_ARTCLEAN   7
#define ITEMNUM_PROGRESS   8
/* Mutually-exclusive "HTTPS stream wait" choices occupy the next block of
 * Playback items. The wait is the settle gap inserted before the next HTTPS
 * radio stream's decoder child is launched, giving the previous stream's
 * AmiSSL/socket teardown time to finish; slow links need a longer gap. */
#define ITEMNUM_HTTPSWAIT_BASE 9
#define HTTPS_WAIT_COUNT       5
#define PLAYBACK_ITEM_COUNT    (ITEMNUM_HTTPSWAIT_BASE + HTTPS_WAIT_COUNT)

/* Amiga Delay() ticks (50 ticks = 1 second). Index 0 keeps the historical
 * 4-tick (~80 ms) default so behaviour is unchanged unless the user opts in. */
static const int kHttpsWaitTicks[HTTPS_WAIT_COUNT] = { 4, 25, 50, 100, 200 };
static const char * const kHttpsWaitDesc[HTTPS_WAIT_COUNT] = {
	"default (~0.1s)", "0.5s", "1s", "2s", "4s"
};

#ifdef MINTAMP_SKIN
#define ITEMNUM_CLASSIC   5
#define PROJECT_ITEM_COUNT 6
#else
#define PROJECT_ITEM_COUNT 5
#endif
static struct Menu kMenus[2];
static struct MenuItem kProjectItems[PROJECT_ITEM_COUNT];
static struct MenuItem kPlaybackItems[PLAYBACK_ITEM_COUNT];
static struct IntuiText kProjectText[PROJECT_ITEM_COUNT];
static struct IntuiText kPlaybackText[PLAYBACK_ITEM_COUNT];
static const char * const kProjectLabels[PROJECT_ITEM_COUNT] = {
	"About MintAMP...", "Internet Radio", "Iconify", "Quit", "Interface preferences..."
#ifdef MINTAMP_SKIN
	/* Ticked while this window is the main interface; untick for the skin. */
	, "Classic interface"
#endif
};
static const char * const kPlaybackLabels[PLAYBACK_ITEM_COUNT] = {
	"Decode-then-play", "Bench mode", "Artwork", "Artwork Cache",
	"Colour Artwork", "Refresh Artwork", "Reload Art from File",
	"Clear Artwork Cache", "Progress Bar",
	"HTTPS wait: Default", "HTTPS wait: 0.5 sec", "HTTPS wait: 1 sec",
	"HTTPS wait: 2 sec", "HTTPS wait: 4 sec"
};

static void MrInitMenuStrip(void)
{
	int i;
	memset(kMenus, 0, sizeof(kMenus));
	memset(kProjectItems, 0, sizeof(kProjectItems));
	memset(kPlaybackItems, 0, sizeof(kPlaybackItems));
	memset(kProjectText, 0, sizeof(kProjectText));
	memset(kPlaybackText, 0, sizeof(kPlaybackText));
	kMenus[0].NextMenu = &kMenus[1];
	kMenus[0].LeftEdge = 0;
	kMenus[0].TopEdge = 0;
	kMenus[0].Width = 72;
	kMenus[0].Height = 10;
	kMenus[0].Flags = MENUENABLED;
	kMenus[0].MenuName = (STRPTR)"Project";
	kMenus[0].FirstItem = &kProjectItems[0];
	kMenus[1].LeftEdge = 72;
	kMenus[1].TopEdge = 0;
	kMenus[1].Width = 80;
	kMenus[1].Height = 10;
	kMenus[1].Flags = MENUENABLED;
	kMenus[1].MenuName = (STRPTR)"Playback";
	kMenus[1].FirstItem = &kPlaybackItems[0];
	for (i = 0; i < PROJECT_ITEM_COUNT; i++) {
		/* Pen 0 is the screen's light/background pen, pen 1 is black on
		 * the standard 4-colour Workbench palette -- these were swapped,
		 * so JAM1 (which only uses FrontPen) drew the item text in the
		 * pale pen instead of black. */
		kProjectText[i].FrontPen = 1;
		kProjectText[i].BackPen = 0;
		kProjectText[i].DrawMode = JAM1;
		kProjectText[i].LeftEdge = 2;
		kProjectText[i].TopEdge = 1;
		kProjectText[i].IText = (STRPTR)kProjectLabels[i];
		kProjectItems[i].NextItem = (i < PROJECT_ITEM_COUNT - 1) ? &kProjectItems[i + 1] : NULL;
		kProjectItems[i].LeftEdge = 0;
		kProjectItems[i].TopEdge = i * 10;
		kProjectItems[i].Width = 200;
		kProjectItems[i].Height = 10;
		kProjectItems[i].Flags = ITEMTEXT | ITEMENABLED | HIGHCOMP;
#ifdef MINTAMP_SKIN
		if (i == ITEMNUM_CLASSIC) {
			kProjectItems[i].Flags |= CHECKIT | MENUTOGGLE;
			kProjectText[i].LeftEdge = 14;
		}
#endif
		kProjectItems[i].ItemFill = (APTR)&kProjectText[i];
		kProjectItems[i].NextSelect = MENUNULL;
	}
	for (i = 0; i < PLAYBACK_ITEM_COUNT; i++) {
		/* Same FrontPen/BackPen swap as kProjectText above. */
		kPlaybackText[i].FrontPen = 1;
		kPlaybackText[i].BackPen = 0;
		kPlaybackText[i].DrawMode = JAM1;
		kPlaybackText[i].LeftEdge = 14;
		kPlaybackText[i].TopEdge = 1;
		kPlaybackText[i].IText = (STRPTR)kPlaybackLabels[i];
		kPlaybackItems[i].NextItem = (i < PLAYBACK_ITEM_COUNT - 1) ? &kPlaybackItems[i + 1] : NULL;
		kPlaybackItems[i].LeftEdge = 0;
		kPlaybackItems[i].TopEdge = i * 10;
		kPlaybackItems[i].Width = 200;
		kPlaybackItems[i].Height = 10;
		kPlaybackItems[i].Flags = ITEMTEXT | ITEMENABLED | HIGHCOMP;
		if (i == ITEMNUM_DTP || i == ITEMNUM_BENCH || i == ITEMNUM_ARTWORK ||
			i == ITEMNUM_ARTCACHE || i == ITEMNUM_ARTCOLOR || i == ITEMNUM_PROGRESS)
			kPlaybackItems[i].Flags |= CHECKIT | MENUTOGGLE;
		if (i >= ITEMNUM_HTTPSWAIT_BASE &&
			i < ITEMNUM_HTTPSWAIT_BASE + HTTPS_WAIT_COUNT) {
			/* Radio-style group: CHECKIT (no MENUTOGGLE) plus a MutualExclude
			 * mask of the other four HTTPS-wait items, so picking one clears
			 * the rest. SyncMenuChecks() also enforces the persisted choice. */
			int j;
			LONG mx = 0;
			for (j = ITEMNUM_HTTPSWAIT_BASE;
				j < ITEMNUM_HTTPSWAIT_BASE + HTTPS_WAIT_COUNT; j++)
				if (j != i)
					mx |= (1L << j);
			kPlaybackItems[i].Flags |= CHECKIT;
			kPlaybackItems[i].MutualExclude = mx;
		}
		kPlaybackItems[i].ItemFill = (APTR)&kPlaybackText[i];
		kPlaybackItems[i].NextSelect = MENUNULL;
	}
}


/* ------------------------------------------------------------------------- */
/* Library / class bases                                                     */
/* ------------------------------------------------------------------------- */

struct IntuitionBase *IntuitionBase;
struct Library *UtilityBase;
struct Library *AslBase;
struct Library *IconBase;
struct Library *WindowBase;
struct Library *LayoutBase;
struct Library *ButtonBase;
struct Library *GetFileBase;
struct Library *ChooserBase;
struct Library *ListBrowserBase;
struct Library *SliderBase;
struct Library *CheckBoxBase;
struct Library *FuelGaugeBase;
struct Library *StringBase;
struct Library *LabelBase;

/* ------------------------------------------------------------------------- */
/* Playback child plumbing (a trimmed-down copy of the amiga_mp3gui logic)   */
/* ------------------------------------------------------------------------- */

typedef struct MrPlayArgs {
	int   argc;
	char *argv[MR_ARGC_MAX];
	char  storage[MR_ARGC_MAX][MR_MAX_PATH];
} MrPlayArgs;

typedef enum MrStreamState {
	MR_STREAM_IDLE = 0,
	MR_STREAM_STARTING,
	MR_STREAM_PLAYING,
	MR_STREAM_STOP_REQUESTED,
	MR_STREAM_STOPPING,
	MR_STREAM_EXITED,
	MR_STREAM_ERROR,
	MR_STREAM_STOP_TIMEOUT
} MrStreamState;

typedef struct MrPlayer {
	volatile int    stopRequested;
	int             argc;
	char          **argv;
	struct Process *process;
	struct Task    *task;
	unsigned long   sessionId;
	char            url[MR_MAX_PATH];
	char            codec[16];
	volatile const char *stage;
	volatile const char *startupStage;
	volatile const char *cleanupStage;
	volatile const char *lastIoState;
	volatile int    donePosted;
} MrPlayer;

static MrPlayer        gPlayer;
static MrPlayArgs      gArgs;
typedef struct MrDoneMessage {
	struct Message msg;
	unsigned long magic;
	unsigned long runId;
	int posted;
} MrDoneMessage;

static MrDoneMessage gDoneMsg;
static struct MsgPort *gDonePort;
static volatile unsigned long gRunCounter;
static volatile unsigned long gEntryRunId;
static volatile unsigned long gDoneRunId;
#define MR_APP_MAGIC 0x4d523047UL
#define MR_DONE_MAGIC 0x4d52444fUL
#ifdef MINTAMP_SKIN
#define MR_WINDOW_TITLE (gSkinPlayer ? "MintAMP-SR Settings" : "MintAMP-SR")
#else
#define MR_WINDOW_TITLE "MintAMP"
#endif

/* ------------------------------------------------------------------------- */
/* Application state                                                         */
/* ------------------------------------------------------------------------- */

typedef struct MrApp {
	unsigned long magic;
	Object         *winObj;
	struct Window  *win;
	struct Menu    *menuStrip;

	Object         *fileGad;
	Object         *rateGad;
	Object         *qualityGad;
	Object         *subbandCapGad;
	Object         *channelGad;
	Object         *volumeGad;
	Object         *bufferGad;
	Object         *fastMemGad;
	Object         *speedGad;
	Object         *widthGad;
	Object         *delayGad;
	Object         *playGad;
	Object         *nextGad;
	Object         *stopGad;
	Object         *rewGad;
	Object         *ffwdGad;
	Object         *filterGad;
	Object         *playlistGad;
	Object         *radioGad;
	Object         *timeGad;
	Object         *fileInfoGad;
	Object         *titleGad;
	Object         *artistGad;
	Object         *albumGad;
	Object         *trackGad;
	Object         *genreGad;
	Object         *ratingGad;
	Object         *starGad[5];
	Object         *gaugeGad;
	Object         *statusGad;
	Object         *artGad;

	Object         *plWinObj;
	struct Window  *plWin;
	Object         *plListGad;
	Object         *plAddGad;
	Object         *plRemoveGad;
	Object         *plClearGad;
	Object         *plPlayGad;
	Object         *plLoadGad;
	Object         *plSaveGad;
	Object         *plCloseGad;
	Object         *plUrlGad;
	Object         *plAddUrlGad;
	Object         *plFromFavsGad;
	Object         *plToFavsGad;
	struct List     plList;
	struct Node    *plNodes[MR_PLAYLIST_MAX];
	char            plNames[MR_PLAYLIST_MAX][80];

	Object         *rbWinObj;
	struct Window  *rbWin;
	Object         *rbSearchGad;
	Object         *rbCodecGad;
	Object         *rbCountryGad;
	Object         *rbCountryCodeGad;
	Object         *rbSchemeGad;
	Object         *rbLimitGad;
	Object         *rbBitrateGad;
	Object         *rbListGad;
	Object         *rbStatusGad;
	Object         *rbDoSearchGad;
	Object         *rbPlayGad;
	Object         *rbAddFavGad;
	Object         *rbFavouritesGad;
	Object         *rbUpGad;
	Object         *rbDownGad;
	Object         *rbCloseGad;
	/* Favourites edit row, enabled while the favourites are shown. */
	Object         *rbFavNameGad;
	Object         *rbFavRenameGad;
	Object         *rbFavRemoveGad;
	Object         *rbFavMoveUpGad;
	Object         *rbFavMoveDownGad;
	struct List     rbList;
	struct Node    *rbNodes[RB_CONTROLLER_MAX_STATIONS];
	char            rbNames[RB_CONTROLLER_MAX_STATIONS][96];
	int             rbVisibleToController[RB_CONTROLLER_MAX_STATIONS];
	int             rbVisibleCount;
	int             rbShowHttps;
	int             rbSchemeMode;
	int             hasNetwork;
	int             hasHttps;
	unsigned long   stopWatchdogMicros;
	int             stopWatchdogFired;
	int             rbCountryMode;
	int             rbShowingFavourites;
	int             rbFavouriteCount;
	int             rbSelectedFavourite;
	int             rbSearchInProgress;
	char            rbFavouriteNames[MR_RADIO_FAV_MAX][RB_MAX_NAME];
	char            rbFavouriteUrls[MR_RADIO_FAV_MAX][RB_MAX_URL];
	char            currentRadioStationName[RB_MAX_NAME];
	/* URL the current stream was started from, before redirects or a
	 * .pls/.m3u were followed: what Add Fav bookmarks. */
	char            currentRadioSourceUrl[RB_MAX_URL];
	char            currentRadioFavicon[RB_MAX_FAVICON];
	/* Final URL the current station name/favicon belong to, so a Play/replay
	 * (which passes no station name) can tell "same stream, keep the artwork"
	 * from "a different URL, reset it". */
	char            currentRadioArtUrl[MR_MAX_PATH];
	RadioBrowserController rbController;

	struct MsgPort   *timerPort;
	struct timerequest *timerReq;
	int               timerRunning;
	struct MsgPort   *donePort;
	struct MsgPort   *appPort;

	char  inputName[MR_MAX_PATH];
	char  lastDrawer[MR_MAX_PATH];
	char  playlist[MR_PLAYLIST_MAX][MR_MAX_PATH];
	/* Name to show for each entry (station name or #EXTINF/TitleN title);
	 * empty means show the file name or URL. */
	char  playlistTitles[MR_PLAYLIST_MAX][80];
	int   rateIndex;
	int   qualityIndex;
	int   subbandCapIndex;
	int   mono;
	int   fastMem;
	int   fastLowrate;
	int   superfastLowrate;
	int   ultrafast;
	int   cd32Ultrafast;
	int   fakeStereo;
	int   fakeStereoWidthIndex;
	int   fakeStereoDelayIndex;
	int   hardwareFilter;
	int   decodeThenPlay;
	int   bench;
	int   haveRadioHostAddr;
	unsigned long radioHostAddrBe;
	int   httpsWaitIndex;   /* index into kHttpsWaitTicks[] */
	int   artEnabled;
	int   artCacheEnabled;
	int   artColorEnabled;
	/* Random tint for the drawn no-artwork radio fallback icon.  Rolled once
	 * per station/track (keyed on inputName) so it stays stable across the
	 * many redraws a single station triggers, and changes when you tune away. */
	unsigned long artFallbackKey;
	int           artFallbackHasColor;
	unsigned char artFallbackR;
	unsigned char artFallbackG;
	unsigned char artFallbackB;
	int   progressEnabled;
	int   artCacheBypass;
	int   artPensBuilt;
	int   artPenCacheUsed;
	struct { unsigned long key; long pen; } artPenCache[MR_ART_COLOR_CACHE];
	unsigned char artRGBBuf[MR_ART_W * MR_ART_H * 3];
	unsigned char artPenIdx[MR_ART_W * MR_ART_H];
	int   playlistCount;
#ifdef MINTAMP_SKIN
    int playlistDurations[MR_PLAYLIST_MAX];
#endif
	int   playlistCurrent;
	int   playlistSelected;
	int   playlistNextPending;
	int   volumePercent;
	int   bufferSeconds;
	int   rating;
	int   totalSecs;
	int   elapsedSecs;
	unsigned long lastFrames;

	unsigned long playbackRunId;
	int   playbackActive;
	int   playbackDonePending;
	MrStreamState streamState;
	char  currentStreamUrl[MR_MAX_PATH];
	char  queuedStreamUrl[MR_MAX_PATH];
	int   lastCompletedWasHttps;
	char  currentStreamCodec[16];
	unsigned long activeChildCount;
	unsigned long activeStreamSessions;
	unsigned long activeStreamTasks;
	int   stoppedByUser;
	char  lastChildExitReason[32];
	char  lastChildError[128];
	char  lastRadioError[256];
	int   parentDoneHandled;
	int   lastChildEverPlayed;
	int   lastChildFirstData;
	int   lastPhaseShown;
	unsigned char artGreyBuf[MR_ART_W * MR_ART_H];
	int artValid;

	char  shownTitle[128];
	char  shownArtist[128];
	char  shownAlbum[128];
	char  fullAlbum[128];
	char  shownTrack[32];
	char  shownGenre[64];
	char  shownFileInfo[128];
	char  shownStatus[128];
	int   albumHover;
	int   albumScrollPos;
	unsigned long pollTick;
	unsigned long lastRadioMetaTick;
	unsigned long lastRadioStatusTick;
	unsigned long lastTimeTick;
	int   shownGaugeLevel;
	int   shownChannelDisabled;
	int   shownWidthDisabled;
	int   shownNextDisabled;
	int   lastRadioStatusShown;
	int   shuttingDown;
} MrApp;

static void UpdateTimeDisplay(MrApp *app);
static void RefreshFileInfoAndTags(MrApp *app);
static void SaveSettings(MrApp *app);
static void ApplyHardwareAudioFilter(MrApp *app);
static void UpdateChannelGadgetState(MrApp *app);
static void UpdateSpeedGadgetChoices(MrApp *app);
static void UpdateNextButtonState(MrApp *app);
static void DrawArtPanel(MrApp *app);
static void SaveSettings(MrApp *app);
static void RefreshPlaylistView(MrApp *app);
static void ClosePlaylistWindow(MrApp *app);
static void OpenPlaylistWindow(MrApp *app);
static void CloseRadioWindow(MrApp *app);
static void OpenRadioWindow(MrApp *app);
static void HandleRadioWindow(MrApp *app);
static void HandleDoneSignal(MrApp *app);
static void FinalizePlayback(MrApp *app);
static void RadioSetStatus(MrApp *app, const char *text);
static int SetStatusIfChanged(MrApp *app, const char *text);
static void SetStatus(MrApp *app, const char *text);
static void RadioDoProbeAndPlay(MrApp *app);
static void RadioProbeUrlAndStart(MrApp *app, const char *url, const char *stationName);
static void LoadPlaylistPath(MrApp *app, const char *m3uPath, const char *drawer);
static void PlaylistLoadCurrent(MrApp *app, int index, int startPlayback);
static void RadioSelectResult(MrApp *app, ULONG eventSelected);
static void PlaylistStartCurrent(MrApp *app);

static void SyncMenuChecks(MrApp *app);
static void SetDecodeThenPlay(MrApp *app, int enabled);

static const char *MrStreamStateName(MrStreamState state)
{
	switch (state) {
	case MR_STREAM_IDLE: return "IDLE";
	case MR_STREAM_STARTING: return "STARTING";
	case MR_STREAM_PLAYING: return "PLAYING";
	case MR_STREAM_STOP_REQUESTED: return "STOP_REQUESTED";
	case MR_STREAM_STOPPING: return "STOPPING";
	case MR_STREAM_EXITED: return "EXITED";
	case MR_STREAM_ERROR: return "ERROR";
	case MR_STREAM_STOP_TIMEOUT: return "STOP_TIMEOUT";
	}
	return "UNKNOWN";
}

static void MrDebugSession(const char *event, const MrApp *app)
{
	RADIO_DBG(printf("radio-session: %s session=%lu state=%s childTask=%p childProc=%p url=\"%s\" codec=%s stop=%d done=%d active_child_count=%lu active_stream_sessions=%lu active_stream_tasks=%lu stage=\"%s\" startup=\"%s\" cleanup=\"%s\" io=\"%s\"\n",
		event ? event : "event",
		gPlayer.sessionId,
		app ? MrStreamStateName(app->streamState) : "(no-app)",
		gPlayer.task, gPlayer.process,
		gPlayer.url[0] ? gPlayer.url : (app ? app->inputName : ""),
		gPlayer.codec[0] ? gPlayer.codec : "unknown",
		gPlayer.stopRequested, gPlayer.donePosted,
		app ? app->activeChildCount : 0,
		app ? app->activeStreamSessions : 0,
		app ? app->activeStreamTasks : 0,
		gPlayer.stage ? (const char *)gPlayer.stage : "",
		gPlayer.startupStage ? (const char *)gPlayer.startupStage : "",
		gPlayer.cleanupStage ? (const char *)gPlayer.cleanupStage : "",
		gPlayer.lastIoState ? (const char *)gPlayer.lastIoState : "");)
}

static int PlaybackProcessStillExists(void);
static int StopPlaybackAndWait(MrApp *app, int ticks, const char *timeoutStatus);
static void HandleDoneSignal(MrApp *app);

/* ------------------------------------------------------------------------- */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------- */


#ifdef RADIO_DEBUG
static void AppCloseDebug(const char *stage, const MrApp *app)
{
	long active_stream_sessions = 0;
	long active_stream_tasks = 0;
	long open_socket_count = 0;
	long active_ssl_count = 0;
	long active_ssl_ctx_count = 0;
	void *socket_base = 0;
	void *amissl_base = 0;
	void *amissl_master_base = 0;
	Radio_GetNetworkStats(&active_stream_sessions, &active_stream_tasks,
		&open_socket_count, &active_ssl_count, &active_ssl_ctx_count);
	Radio_GetNetworkBases(&socket_base, &amissl_base, &amissl_master_base);
	printf("APP_CLOSE: %s streamState=%s playbackActive=%d playbackDonePending=%d activeChildCount=%lu gPlayer.process=%p gPlayer.task=%p gPlayer.stopRequested=%d active_stream_sessions=%ld active_stream_tasks=%ld open_socket_count=%ld active_ssl_count=%ld active_ssl_ctx_count=%ld browser_probe_socket_counts=unavailable net_base=%p tls_base=%p tls_master=%p\n",
		stage ? stage : "stage",
		app ? MrStreamStateName(app->streamState) : "(no-app)",
		app ? app->playbackActive : 0,
		app ? app->playbackDonePending : 0,
		app ? app->activeChildCount : 0,
		gPlayer.process, gPlayer.task, gPlayer.stopRequested,
		active_stream_sessions, active_stream_tasks, open_socket_count,
		active_ssl_count, active_ssl_ctx_count,
		socket_base, amissl_base, amissl_master_base
	);
}
#else
#define AppCloseDebug(stage, app) do { (void)(stage); (void)(app); } while (0)
#endif

static int AppHasActivePlaybackChild(const MrApp *app)
{
	return app && app->activeChildCount > 0 &&
		(app->playbackActive || app->playbackDonePending ||
		 gPlayer.process || gPlayer.task || PlaybackProcessStillExists());
}

/* Returns 1 if the playback child is confirmed gone (safe to dispose the
 * shared browser/network/GUI objects it can touch), 0 if it is still active
 * or wedged -- the caller must then leak those objects and exit without
 * touching them rather than free/close something the child still holds a
 * reference to (Radio_NetworkShutdown() closing bsdsocket.library/AmiSSL out
 * from under a child that still owns an open socket/SSL context was seen
 * interleaving with in-flight ring/ICY writes right before an exec-heap
 * corruption). */
static int AppCloseShutdown(MrApp *app)
{
	if (!app)
		return 1;
	app->shuttingDown = 1;
	MR_TASK_IDENTITY("final-application-shutdown");
	AppCloseDebug("begin", app);
	AppCloseDebug("playback state", app);
	if (AppHasActivePlaybackChild(app)) {
		RADIO_DBG(printf("APP_CLOSE: stopping playback\n");)
		AppCloseDebug("stop active playback if needed", app);
		if (!StopPlaybackAndWait(app, 500, "Failed to stop previous stream")) {
			/* The child missed the first stop request (e.g. it was stalled
			 * inside a radio pump loop).  Keep re-asserting the shared stop
			 * flags and re-signalling CTRL_C while waiting -- a single Signal
			 * sent before the child sampled its signal mask can be consumed
			 * without effect, and the old bare Delay() wait here left the app
			 * frozen forever when that happened.  Matches the GadTools
			 * front-end's WaitForPlaybackShutdown() loop.
			 *
			 * But if the child is genuinely wedged inside a blocking call
			 * that never observes SIGBREAKF_CTRL_C at all (not just a missed
			 * signal), re-signalling forever never helps either -- give up
			 * after a bound so the app can still close instead of hanging
			 * the whole task, at the cost of leaking the wedged child (no
			 * safe way to force-kill a task stuck inside a library call). */
			int wedgedTicks;
			RADIO_DBG(printf("APP_CLOSE: waiting child\n");)
			for (wedgedTicks = 0; wedgedTicks < APP_CLOSE_WEDGED_CHILD_MAX_TICKS &&
				PlaybackProcessStillExists(); wedgedTicks++) {
				struct Task *child;
				gPlayer.stopRequested = 1;
				gPlaybackInterrupted = 1;
				Forbid();
				child = FindTask((STRPTR)"MintAMP playback");
				if (child)
					Signal(child, SIGBREAKF_CTRL_C);
				Permit();
				HandleDoneSignal(app);
				Delay(5);
			}
			if (PlaybackProcessStillExists()) {
				RADIO_DBG(printf("app-close: giving up on wedged playback child after %d ticks, leaking it\n", wedgedTicks);)
				RADIO_DBG(printf("APP_CLOSE: child still active after timeout -- leaking shared browser/network/GUI objects, skipping disposal\n");)
				return 0;
			}
			HandleDoneSignal(app);
		}
	} else {
		AppCloseDebug("playback already idle, skip stop", app);
		AppCloseDebug("playback idle, no stop needed", app);
	}
	RADIO_DBG(printf("APP_CLOSE: child done\n");)
	/* HandleDoneSignal() -> FinalizePlayback() should already have run by
	 * now (that's what clears playbackActive/playbackDonePending in the
	 * loops above); this is a belt-and-suspenders catch for the case where
	 * the child task exited (PlaybackProcessStillExists() false) without the
	 * done message ever being drained here, so a wedged/lost message cannot
	 * leave playbackActive stuck true and this function still returns "safe
	 * to dispose" regardless. */
	if (app->playbackActive || app->playbackDonePending) {
		HandleDoneSignal(app);
		if (app->playbackActive || app->playbackDonePending)
			FinalizePlayback(app);
	}
	RADIO_DBG(printf("APP_CLOSE: finalize done\n");)
	/* Let audio.device/DOS settle after the child task's exit before this
	 * task starts touching the shared objects it used. */
	Delay(10);
	RADIO_DBG(printf("APP_CLOSE: now disposing GUI/network\n");)
	AppCloseDebug("dispose radio browser controller", app);
	AppCloseDebug("free favourites/search results", app);
	AppCloseDebug("dispose GUI objects", app);
	return 1;
}

static void SafeCopy(char *dst, size_t size, const char *src)
{
	if (!size)
		return;
	if (!src)
		src = "";
	strncpy(dst, src, size - 1);
	dst[size - 1] = '\0';
}

static int MrVerifyAppMagic(MrApp *app, const char *where)
{
	if (!app || app->magic != MR_APP_MAGIC) {
		RADIO_DBG(printf("radio-guard: WARNING app magic corrupt before %s app=%p magic=%08lx expected=%08lx\n",
			where ? where : "operation", app, app ? app->magic : 0UL, (unsigned long)MR_APP_MAGIC);)
		return 0;
	}
	return 1;
}


static int ClampInt(int value, int minValue, int maxValue)
{
	if (value < minValue)
		return minValue;
	if (value > maxValue)
		return maxValue;
	return value;
}

static void FormatTime(int secs, char *buf)
{
	if (secs < 0) {
		SafeCopy(buf, 8, "--:--");
		return;
	}
	sprintf(buf, "%02d:%02d", secs / 60, secs % 60);
}


static void CopyDrawerFromPath(char *drawer, size_t drawerSize, const char *path)
{
	char *q;
	if (!drawer || drawerSize == 0) return;
	drawer[0] = '\0';
	if (!path || !path[0]) return;
	SafeCopy(drawer, drawerSize, path);
	q = drawer + strlen(drawer);
	while (q > drawer && *q != '/' && *q != ':') q--;
	if (*q == '/' || *q == ':') *(q + 1) = '\0';
	else drawer[0] = '\0';
}

static void EnvName(char *dst, size_t dstSize, const char *key)
{
	SafeCopy(dst, dstSize, MR_ENV_PREFIX);
	strncat(dst, "/", dstSize - strlen(dst) - 1);
	strncat(dst, key, dstSize - strlen(dst) - 1);
}

static int LoadEnvIntMaybe(const char *key, int *outValue, int minValue, int maxValue)
{
	char name[64], value[32]; long n; int v;
	if (!outValue) return 0;
	EnvName(name, sizeof(name), key);
	n = GetVar((STRPTR)name, (STRPTR)value, sizeof(value) - 1, 0);
	if (n <= 0) return 0;
	value[n] = '\0';
	v = atoi(value);
	*outValue = ClampInt(v, minValue, maxValue);
	return 1;
}

static int LoadEnvInt(const char *key, int fallback, int minValue, int maxValue)
{
	int v;
	return LoadEnvIntMaybe(key, &v, minValue, maxValue) ? v : fallback;
}

static void LoadEnvString(const char *key, char *dst, size_t dstSize)
{
	char name[64]; long n;
	if (!dst || dstSize == 0) return;
	EnvName(name, sizeof(name), key);
	n = GetVar((STRPTR)name, (STRPTR)dst, dstSize - 1, 0);
	if (n > 0) dst[n] = '\0'; else dst[0] = '\0';
}

static void SaveEnvString(const char *key, const char *value)
{
	static char current[1024];
	char name[64];
	LONG len;
	size_t n;
	EnvName(name, sizeof(name), key);
	if (!value) value = "";
	/* Each GVF_SAVE_VAR write is a file in ENVARC: on disk, and a settings
	 * save covers every favourite slot (130-odd variables), which took up to
	 * a minute on a slow drive.  ENV: holds the same values (copied from
	 * ENVARC: at boot, then kept in step here), so write only what changed. */
	len = GetVar((STRPTR)name, (STRPTR)current, sizeof(current),
		GVF_GLOBAL_ONLY | GVF_BINARY_VAR);
	n = strlen(value);
	if (len < 0 ? n == 0 :
		(n + 1 < sizeof(current) && (size_t)len == n && !memcmp(current, value, n)))
		return;
	SetVar((STRPTR)name, (STRPTR)value, strlen(value), GVF_GLOBAL_ONLY);
	SetVar((STRPTR)name, (STRPTR)value, strlen(value), GVF_SAVE_VAR);
}

static void SaveEnvInt(const char *key, int value)
{
	char text[16]; sprintf(text, "%d", value); SaveEnvString(key, text);
}

/* The radio favourites live in one variable (see radio_favourites.h); a
 * variable per slot meant ~100 ENVARC: files rewritten on every edit. */
static void LoadRadioFavourites(MrApp *app)
{
	size_t cap = RADIO_FAV_TEXT_MAX(MR_RADIO_FAV_MAX);
	char *text = (char *)malloc(cap);
	char name[64];
	char key[32];
	LONG len;
	int count = -1, i;

	if (text) {
		EnvName(name, sizeof(name), "RadioFavourites");
		len = GetVar((STRPTR)name, (STRPTR)text, (LONG)cap, GVF_GLOBAL_ONLY | GVF_BINARY_VAR);
		if (len >= 0)
			count = radio_fav_parse(text, (size_t)len, app->rbFavouriteNames,
				app->rbFavouriteUrls, MR_RADIO_FAV_MAX);
		free(text);
	}
	if (count >= 0) {
		app->rbFavouriteCount = count;
		return;
	}
	/* Saved by MintAMP 1.4.0 or earlier: a name and a URL variable per slot. */
	app->rbFavouriteCount = LoadEnvInt("RadioFavCount", app->rbFavouriteCount, 0, MR_RADIO_FAV_MAX);
	for (i = 0; i < app->rbFavouriteCount; i++) {
		sprintf(key, "RadioFavName%d", i);
		LoadEnvString(key, app->rbFavouriteNames[i], sizeof(app->rbFavouriteNames[i]));
		sprintf(key, "RadioFavUrl%d", i);
		LoadEnvString(key, app->rbFavouriteUrls[i], sizeof(app->rbFavouriteUrls[i]));
	}
}

/* Writes the favourites variable, only when it changed. */
static void SaveRadioFavourites(MrApp *app)
{
	size_t cap = RADIO_FAV_TEXT_MAX(MR_RADIO_FAV_MAX);
	char *text = (char *)malloc(cap * 2);
	char name[64];
	size_t n;
	LONG len;

	if (!text)
		return;
	n = radio_fav_format(text, cap, app->rbFavouriteNames, app->rbFavouriteUrls,
		ClampInt(app->rbFavouriteCount, 0, MR_RADIO_FAV_MAX));
	if (n) {
		EnvName(name, sizeof(name), "RadioFavourites");
		len = GetVar((STRPTR)name, (STRPTR)(text + cap), (LONG)cap, GVF_GLOBAL_ONLY | GVF_BINARY_VAR);
		if (len < 0 || (size_t)len != n || memcmp(text + cap, text, n) != 0) {
			SetVar((STRPTR)name, (STRPTR)text, (LONG)n, GVF_GLOBAL_ONLY);
			SetVar((STRPTR)name, (STRPTR)text, (LONG)n, GVF_SAVE_VAR);
		}
	}
	free(text);
}

static void LoadSettings(MrApp *app)
{
	app->fastLowrate = LoadEnvInt("FastLowrate", app->fastLowrate, 0, 1);
	app->superfastLowrate = LoadEnvInt("SuperfastLowrate", app->superfastLowrate, 0, 1);
	app->ultrafast = LoadEnvInt("Ultrafast", app->ultrafast, 0, 1);
	app->cd32Ultrafast = LoadEnvInt("CD32Ultrafast", app->cd32Ultrafast, 0, 1);
	if (app->cd32Ultrafast) {
		app->ultrafast = 0;
		app->fastLowrate = 1;
		app->superfastLowrate = 1;
	} else if (app->ultrafast) {
		app->fastLowrate = 0;
		app->superfastLowrate = 0;
	}
	app->fastMem = LoadEnvInt("FastMem", app->fastMem, 0, 1);
	app->mono = LoadEnvInt("Mono", app->mono, 0, 1);
	app->fakeStereo = LoadEnvInt("FakeStereo", app->fakeStereo, 0, 1);
	app->fakeStereoWidthIndex = LoadEnvInt("FakeStereoWidthIndex", app->fakeStereoWidthIndex, 0, 4);
	app->fakeStereoDelayIndex = LoadEnvInt("FakeStereoDelayIndex", app->fakeStereoDelayIndex, 0, 4);
	app->hardwareFilter = LoadEnvInt("HardwareFilter", app->hardwareFilter, 0, 1);
	app->rateIndex = LoadEnvInt("RateIndex", app->rateIndex, 0, MR_RATE_COUNT - 1);
	if (app->cd32Ultrafast) {
		/* Mono-only: drop any stale saved fake-stereo so the greyed Mode/width
		 * chooser comes up parked on "Normal M/S" rather than a fake-stereo
		 * width the output can never actually use. */
		app->mono = 1;
		app->fakeStereo = 0;
		app->rateIndex = MR_RATE_22050_INDEX;
	}
	app->bufferSeconds = LoadEnvInt("BufferSeconds", app->bufferSeconds, 1, 10);
	app->volumePercent = LoadEnvInt("Volume", app->volumePercent, 0, 100);
	app->qualityIndex = LoadEnvInt("QualityIndex", app->qualityIndex, 0, 3);
	app->subbandCapIndex = LoadEnvInt("SubbandCapIndex", app->subbandCapIndex, 0, SUBBAND_CAP_COUNT - 1);
	app->decodeThenPlay = LoadEnvInt("DecodeThenPlay", app->decodeThenPlay, 0, 1);
	app->httpsWaitIndex = LoadEnvInt("HttpsWaitIndex", app->httpsWaitIndex, 0, HTTPS_WAIT_COUNT - 1);
	app->bench = LoadEnvInt("Bench", app->bench, 0, 1);
	app->artEnabled = LoadEnvInt("Artwork", app->artEnabled, 0, 1);
	app->artCacheEnabled = LoadEnvInt("ArtworkCache", app->artCacheEnabled, 0, 1);
	app->artColorEnabled = LoadEnvInt("ArtworkColour", app->artColorEnabled, 0, 1);
	app->progressEnabled = LoadEnvInt("ProgressBar", app->progressEnabled, 0, 1);
	LoadEnvString("LastDrawer", app->lastDrawer, sizeof(app->lastDrawer));
	LoadRadioFavourites(app);
}

static void SaveSettings(MrApp *app)
{
	SaveEnvInt("FastLowrate", app->fastLowrate);
	SaveEnvInt("SuperfastLowrate", app->superfastLowrate);
	SaveEnvInt("Ultrafast", app->ultrafast);
	SaveEnvInt("CD32Ultrafast", app->cd32Ultrafast);
	SaveEnvInt("FastMem", app->fastMem);
	SaveEnvInt("Mono", app->mono);
	SaveEnvInt("FakeStereo", app->fakeStereo);
	SaveEnvInt("FakeStereoWidthIndex", app->fakeStereoWidthIndex);
	SaveEnvInt("FakeStereoDelayIndex", app->fakeStereoDelayIndex);
	SaveEnvInt("HardwareFilter", app->hardwareFilter);
	SaveEnvInt("RateIndex", app->rateIndex);
	SaveEnvInt("BufferSeconds", ClampInt(app->bufferSeconds, 1, 10));
	SaveEnvInt("Volume", ClampInt(app->volumePercent, 0, 100));
	SaveEnvInt("QualityIndex", app->qualityIndex);
	SaveEnvInt("SubbandCapIndex", app->subbandCapIndex);
	SaveEnvInt("SettingsVersion", MR_SETTINGS_VERSION);
	SaveEnvInt("DecodeThenPlay", app->decodeThenPlay);
	SaveEnvInt("HttpsWaitIndex", app->httpsWaitIndex);
	SaveEnvInt("Bench", app->bench);
	SaveEnvInt("Artwork", app->artEnabled);
	SaveEnvInt("ArtworkCache", app->artCacheEnabled);
	SaveEnvInt("ArtworkColour", app->artColorEnabled);
	SaveEnvInt("ProgressBar", app->progressEnabled);
	SaveEnvString("LastDrawer", app->lastDrawer);
	SaveRadioFavourites(app);
}

/* Sets one gadget attribute.  While iconified there is no window, but the
 * object must still change: the callers cache what they last set, so an
 * update skipped then is never repeated, and the window would reopen
 * showing the old title, artist, buttons or progress. */
static void MrSetGadgetAttr(Object *gad, struct Window *win, ULONG tag, ULONG value)
{
	if (!gad)
		return;
	if (win)
		SetGadgetAttrs((struct Gadget *)gad, win, NULL, tag, value, TAG_DONE);
	else
		SetAttrs(gad, tag, value, TAG_DONE);
}

static int SetReadonlyString(Object *gad, struct Window *win, char *cache, size_t cacheSize, const char *text)
{
	if (!text)
		text = "";
	if (cache && cacheSize > 0 && !strcmp(cache, text))
		return 0;
	if (cache && cacheSize > 0)
		SafeCopy(cache, cacheSize, text);
	if (gad) {
		MrSetGadgetAttr(gad, win, STRINGA_TextVal, (ULONG)text);
		/* STRINGA_TextVal parks the cursor/view at the END of the new string, so
		 * a long value (e.g. the album/station name) renders with its left edge
		 * clipped off.  Reset the view to the start in a SEPARATE SetGadgetAttrs
		 * pass - doing it in the same tag list as STRINGA_TextVal gets overridden
		 * when the gadget re-derives its display offset from the cursor. */
		MrSetGadgetAttr(gad, win, STRINGA_BufferPos, 0);
		MrSetGadgetAttr(gad, win, STRINGA_DispPos, 0);
	}
	return 1;
}

static int SetStatusIfChanged(MrApp *app, const char *text)
{
	const char *safeText = text ? text : "";
	if (!app)
		return 0;
	SafeCopy(app->lastRadioError, sizeof(app->lastRadioError), safeText);
	return SetReadonlyString(app->statusGad, app->win, app->shownStatus,
		sizeof(app->shownStatus), app->lastRadioError);
}

static void SetStatus(MrApp *app, const char *text)
{
	(void)SetStatusIfChanged(app, text);
}

static int PointInGadget(struct Gadget *gad, int x, int y)
{
	return gad && x >= gad->LeftEdge && y >= gad->TopEdge &&
		x < gad->LeftEdge + gad->Width && y < gad->TopEdge + gad->Height;
}

/* How many characters of the album/station name fit in the gadget at the
 * current font.  A ReAction read-only string gadget that overflows renders its
 * tail (clipping the left), so we truncate the text to fit and it then always
 * shows the start, left-aligned.  The full name is still available on hover via
 * the status line. */
static int AlbumVisibleChars(MrApp *app)
{
	int txw, px, chars;
	if (!app || !app->win || !app->albumGad)
		return 0;
	txw = app->win->RPort ? app->win->RPort->TxWidth : 8;
	if (txw <= 0)
		txw = 8;
	/* Gadget Width includes the recessed border; leave a couple of chars of
	 * slack so the last glyph never spills past the right edge. */
	px = ((struct Gadget *)app->albumGad)->Width - 8;
	if (px < txw)
		return 1;
	chars = px / txw - 1;
	return chars > 0 ? chars : 1;
}

/* Set the album gadget to a left-aligned, fit-to-width copy, keeping the full
 * string in app->fullAlbum for the hover status hint. */
static int SetAlbumDisplay(MrApp *app, const char *full)
{
	char shown[128];
	int fit;
	if (!app)
		return 0;
	if (!full)
		full = "";
	SafeCopy(app->fullAlbum, sizeof(app->fullAlbum), full);
	fit = AlbumVisibleChars(app);
	if (fit > (int)sizeof(shown) - 1)
		fit = (int)sizeof(shown) - 1;
	if ((int)strlen(full) > fit) {
		memcpy(shown, full, (size_t)fit);
		shown[fit] = 0;
	} else {
		SafeCopy(shown, sizeof(shown), full);
	}
	return SetReadonlyString(app->albumGad, app->win, app->shownAlbum,
		sizeof(app->shownAlbum), shown);
}

static void UpdateAlbumHover(MrApp *app)
{
	int over;
	if (!app || !app->win || !app->albumGad)
		return;
	over = PointInGadget((struct Gadget *)app->albumGad, app->win->MouseX, app->win->MouseY);
	if (over == app->albumHover)
		return;
	app->albumHover = over;
	/* On hover show the full album/station name in the status line (the gadget
	 * itself only has room for the left part). */
	if (over && app->fullAlbum[0] && strcmp(app->fullAlbum, "-")) {
		char buf[160];
		SafeCopy(buf, sizeof(buf), "Album: ");
		strncat(buf, app->fullAlbum, sizeof(buf) - strlen(buf) - 1);
		SetStatus(app, buf);
	}
}

/* The album text is truncated to fit, so there is nothing to scroll; kept as a
 * no-op so the main loop call site stays simple. */
static void ScrollAlbumHover(MrApp *app)
{
	(void)app;
}


static int MrIsRadioInput(const char *name)
{
	return name && (!strncmp(name, "http://", 7) ||
		!strncmp(name, "https://", 8));
}

static void UpdateFastMemGadgetState(MrApp *app)
{
	if (app->win && app->fastMemGad)
		SetGadgetAttrs((struct Gadget *)app->fastMemGad, app->win, NULL,
			GA_Selected, (ULONG)(app->fastMem ? TRUE : FALSE),
			GA_Disabled, (ULONG)(MrIsRadioInput(app->inputName) ||
				app->playbackActive || app->playbackDonePending), TAG_DONE);
}

/* A local .m3u/.m3u8/.pls file (URLs ending so are station links, which the
 * stream probe follows instead). */
static int MrIsPlaylistFile(const char *name)
{
	return !MrIsRadioInput(name) && playlist_is_playlist_name(name);
}


static void MrCopyVolatileString(char *dst, unsigned long dstSize, volatile const char *src)
{
	unsigned long i;
	char raw[256];
	if (!dst || dstSize == 0) return;
	if (!src) { dst[0] = 0; return; }
	for (i = 0; i + 1 < sizeof(raw) && src[i]; i++) raw[i] = (char)src[i];
	raw[i] = 0;
	AmigaUtf8ToDisplay(dst, dstSize, raw);
}

static void MrSplitStreamTitle(const char *streamTitle, char *artist, unsigned long artistSize, char *title, unsigned long titleSize)
{
	const char *sep; char tmp[128];
	if (artist && artistSize) artist[0] = 0;
	if (title && titleSize) title[0] = 0;
	if (!streamTitle || !streamTitle[0]) return;
	sep = strstr(streamTitle, " - ");
	if (!sep) { SafeCopy(title, titleSize, streamTitle); return; }
	SafeCopy(tmp, sizeof(tmp), streamTitle);
	sep = strstr(tmp, " - ");
	if (sep) { ((char *)sep)[0] = 0; SafeCopy(artist, artistSize, tmp); SafeCopy(title, titleSize, sep + 3); }
}

static const char *MrRadioCodecName(const char *contentType)
{
	if (!contentType) return "";
	if (strstr(contentType, "flac") || strstr(contentType, "FLAC"))
		return "FLAC";
	if (strstr(contentType, "aac") || strstr(contentType, "AAC") ||
		strstr(contentType, "aach") || strstr(contentType, "AACH"))
		return "AAC+";
	if (strstr(contentType, "mpeg") || strstr(contentType, "MPEG") ||
		strstr(contentType, "mp3") || strstr(contentType, "MP3"))
		return "MP3";
	return "";
}


static int MrRadioPlaybackHasStarted(void)
{
	return gGuiPlaybackStatus.phase == GUIPLAY_PHASE_PLAYING ||
		gGuiPlaybackStatus.decodedFrames > 0;
}

static void MrFormatRadioStreamingStatus(MrApp *app, const char *station, char *status, unsigned long statusSize)
{
	const char *name = station;
	char streamUrl[128];

	if (!status || statusSize == 0)
		return;
	if (!name || !name[0]) {
		MrCopyVolatileString(streamUrl, sizeof(streamUrl), gGuiPlaybackStatus.radioStreamUrl);
		name = (app && app->currentRadioStationName[0]) ? app->currentRadioStationName :
			(streamUrl[0] ? streamUrl : (app && app->inputName[0] ? app->inputName : "Internet Radio"));
	}
	sprintf(status, "Streaming %.100s", name);
}

static int MrSetRadioMetadata(MrApp *app, int updateStatus)
{
	char streamTitle[128], station[128], genre[64], contentType[64], radioError[128], artist[64], title[64], fileInfo[128], status[128];
	const char *codec;
	int bitrate;
	int updates = 0;
	int radioStatus;

	MrCopyVolatileString(streamTitle, sizeof(streamTitle), gGuiPlaybackStatus.radioTitle);
	MrCopyVolatileString(station, sizeof(station), gGuiPlaybackStatus.radioStationName);
	MrCopyVolatileString(genre, sizeof(genre), gGuiPlaybackStatus.radioGenre);
	MrCopyVolatileString(contentType, sizeof(contentType), gGuiPlaybackStatus.radioContentType);
	MrCopyVolatileString(radioError, sizeof(radioError), gGuiPlaybackStatus.radioError);
	radioStatus = gGuiPlaybackStatus.radioStatus;

	MrSplitStreamTitle(streamTitle, artist, sizeof(artist), title, sizeof(title));
	codec = MrRadioCodecName(contentType);
	bitrate = gGuiPlaybackStatus.radioBitrateKbps;
	if (codec[0] && bitrate > 0)
		sprintf(fileInfo, "Internet Stream - %s %dkbps", codec, bitrate);
	else if (codec[0])
		sprintf(fileInfo, "Internet Stream - %s", codec);
	else if (bitrate > 0)
		sprintf(fileInfo, "Internet Stream - %dkbps", bitrate);
	else
		sprintf(fileInfo, "Internet Stream");
	updates += SetReadonlyString(app->titleGad, app->win, app->shownTitle, sizeof(app->shownTitle), title[0] ? title : "-");
	updates += SetReadonlyString(app->artistGad, app->win, app->shownArtist, sizeof(app->shownArtist), artist[0] ? artist : "-");
	updates += SetAlbumDisplay(app, station[0] ? station : "Internet Radio");
	updates += SetReadonlyString(app->trackGad, app->win, app->shownTrack, sizeof(app->shownTrack), "Live");
	updates += SetReadonlyString(app->genreGad, app->win, app->shownGenre, sizeof(app->shownGenre), genre[0] ? genre : "-");
	updates += SetReadonlyString(app->fileInfoGad, app->win, app->shownFileInfo, sizeof(app->shownFileInfo), fileInfo);
	if (updateStatus) {
		if (radioStatus == RADIO_STATUS_ERROR)
			sprintf(status, "Stream failed: %s", radioError[0] ? radioError : "radio error");
		else if (radioStatus == RADIO_STATUS_RECONNECTING)
			sprintf(status, "Stream dropped - reconnecting");
		else if (radioStatus == RADIO_STATUS_CONNECTING)
			sprintf(status, "Connecting stream...");
		else if (radioStatus == RADIO_STATUS_BUFFERING && !MrRadioPlaybackHasStarted())
			sprintf(status, "Buffering - %.100s", station[0] ? station :
				(app->currentRadioStationName[0] ? app->currentRadioStationName : "Internet Radio"));
		else if (radioStatus == RADIO_STATUS_PLAYING ||
			(radioStatus == RADIO_STATUS_BUFFERING && MrRadioPlaybackHasStarted())) {
			if (gGuiPlaybackStatus.decodedFrames > 0)
				RADIO_DBG(printf("radio-ui: UI state set to PLAYING after first audio frame\n");)
			MrFormatRadioStreamingStatus(app, station, status, sizeof(status));
		}
		else
			sprintf(status, "Connecting stream...");
		updates += SetReadonlyString(app->statusGad, app->win, app->shownStatus,
			sizeof(app->shownStatus), status);
	}
	return updates;
}

static void SetGauge(MrApp *app, int level)
{
	if (level < 0)
		level = 0;
	if (level > 100)
		level = 100;
	if (level == app->shownGaugeLevel)
		return;
	app->shownGaugeLevel = level;
	MrSetGadgetAttr(app->gaugeGad, app->win, FUELGAUGE_Level, (ULONG)level);
}

static int SpeedChoiceFromApp(const MrApp *app)
{
	if (app->rateIndex == MR_RATE_22050_INDEX && app->cd32Ultrafast)
		return 3;
	if (app->ultrafast)
		return 2;
	if (app->superfastLowrate)
		return 1;
	return 0;
}

static void UpdateSpeedGadgetChoices(MrApp *app)
{
	if (app->rateIndex != MR_RATE_22050_INDEX && app->cd32Ultrafast) {
		/* "22050 Mono Ultrafast" only exists at 22050 Hz; leaving that rate
		 * must fall back to plain "Ultrafast" (index 2, same as picking it
		 * by hand -- see the v==2 case in SyncFromGadgets()), not "Normal". */
		app->cd32Ultrafast = 0;
		app->ultrafast = 1;
		app->superfastLowrate = 0;
		app->fastLowrate = 0;
	}
	if (app->win && app->speedGad)
		SetGadgetAttrs((struct Gadget *)app->speedGad, app->win, NULL,
			CHOOSER_LabelArray, (ULONG)(app->rateIndex == MR_RATE_22050_INDEX ? kSpeedLabels : kSpeedLabelsNo22050),
			CHOOSER_Selected, (ULONG)SpeedChoiceFromApp(app),
			TAG_DONE);
}

static void UpdateChannelGadgetState(MrApp *app)
{
	/* The output channel count is forced -- so the Mono/Stereo chooser must be
	 * greyed -- whenever fake stereo is engaged or 22050 Mono Ultrafast
	 * (cd32Ultrafast) is selected.  The latter is mono-only, so the fake-stereo
	 * Mode/width and Delay choosers are meaningless there and get greyed too. */
	int channelDisabled = (app->fakeStereo || app->cd32Ultrafast) ? TRUE : FALSE;
	int widthDisabled = app->cd32Ultrafast ? TRUE : FALSE;
	if (channelDisabled != app->shownChannelDisabled) {
		app->shownChannelDisabled = channelDisabled;
		if (app->win && app->channelGad)
			SetGadgetAttrs((struct Gadget *)app->channelGad, app->win, NULL,
				GA_Disabled, (ULONG)channelDisabled, TAG_DONE);
	}
	if (widthDisabled != app->shownWidthDisabled) {
		app->shownWidthDisabled = widthDisabled;
		if (app->win && app->widthGad)
			SetGadgetAttrs((struct Gadget *)app->widthGad, app->win, NULL,
				GA_Disabled, (ULONG)widthDisabled, TAG_DONE);
		if (app->win && app->delayGad)
			SetGadgetAttrs((struct Gadget *)app->delayGad, app->win, NULL,
				GA_Disabled, (ULONG)widthDisabled, TAG_DONE);
	}
}

static void UpdateNextButtonState(MrApp *app)
{
	int enabled = app->playlistCount > 0 && app->playlistCurrent >= 0 &&
		app->playlistCurrent + 1 < app->playlistCount &&
		!app->playbackDonePending && !gPlayer.stopRequested;
	int disabled = enabled ? FALSE : TRUE;
	if (disabled == app->shownNextDisabled)
		return;
	app->shownNextDisabled = disabled;
	MrSetGadgetAttr(app->nextGad, app->win, GA_Disabled, (ULONG)disabled);
}

static void EnablePlayStop(MrApp *app, int playing)
{
	MrSetGadgetAttr(app->playGad, app->win, GA_Disabled, (ULONG)(playing ? TRUE : FALSE));
	MrSetGadgetAttr(app->stopGad, app->win, GA_Disabled, (ULONG)(playing ? FALSE : TRUE));
	UpdateNextButtonState(app);
	UpdateFastMemGadgetState(app);
}

/* ------------------------------------------------------------------------- */
/* Build the playback argument vector (same flags as the Shell command)      */
/* ------------------------------------------------------------------------- */

static void AddArg(MrPlayArgs *args, const char *text)
{
	if (args->argc >= MR_ARGC_MAX)
		return;
	SafeCopy(args->storage[args->argc], MR_MAX_PATH, text);
	args->argv[args->argc] = args->storage[args->argc];
	args->argc++;
}

static void BuildPlaybackArgs(MrApp *app, MrPlayArgs *args)
{
	char num[16];
	int isRadio;
	int useCd32Ultrafast;
	int useUltrafast;
	int useSuperfast;
	int useFastLowrate;
	int useMono;
	int useFakeStereo;
	int rateIndex;

	memset(args, 0, sizeof(*args));
	isRadio = MrIsRadioInput(app->inputName);
	useCd32Ultrafast = app->cd32Ultrafast;
	useUltrafast = app->ultrafast;
	useSuperfast = app->superfastLowrate;
	useFastLowrate = app->fastLowrate;
	useMono = app->mono;
	useFakeStereo = app->fakeStereo;
	rateIndex = app->rateIndex;
	if (isRadio && (useCd32Ultrafast || (useUltrafast && useMono && rateIndex == MR_RATE_22050_INDEX))) {
		useCd32Ultrafast = 0;
		useUltrafast = 0;
		useSuperfast = 0;
		useFastLowrate = 0;
	}
	AddArg(args, "MintAMP");
	AddArg(args, "--play");
	if (isRadio) {
		AddArg(args, "--radio-stream");
		if (app->haveRadioHostAddr) {
			AddArg(args, "--radio-host-addr-be");
			sprintf(num, "%lu", app->radioHostAddrBe);
			AddArg(args, num);
		}
		if (!app->rbShowingFavourites && app->rbController.selected_index >= 0) {
			const RadioBrowserStation *st = rb_controller_get_station(&app->rbController, app->rbController.selected_index);
			if (st && st->codec[0]) {
				AddArg(args, "--radio-codec-hint");
				AddArg(args, st->codec);
			}
		}
	}
	/* --fast-mem preloads the *whole input* into Fast RAM up front
	 * (InputSourcePreloadFastMemory() in amiga_mp3dec.c does an
	 * end-of-file Seek()/ftell() to size the preload buffer) -- that
	 * assumes a finite, seekable local file.  A radio stream is neither;
	 * passing --fast-mem through for radio input pointed that preload
	 * logic at a live stream socket/handle it was never designed for. */
	if (app->fastMem && !isRadio)
		AddArg(args, "--fast-mem");
	if (useCd32Ultrafast) {
		AddArg(args, "--fast-lowrate");
		AddArg(args, "--superfast-lowrate");
		AddArg(args, "--exp-reduced-taps");
		AddArg(args, "--subband-cap");
		AddArg(args, "12");
	} else if (useSuperfast ||
		(useUltrafast && strcmp(kRates[rateIndex], "28600") != 0)) {
		AddArg(args, "--fast-lowrate");
		AddArg(args, "--superfast-lowrate");
	} else if (useFastLowrate && strcmp(kRates[rateIndex], "28600")) {
		AddArg(args, "--fast-lowrate");
	}
	if (useUltrafast && strcmp(kRates[rateIndex], "28600") == 0)
		AddArg(args, "--ultrafast");
	/* The ASM polyphase (--exp-poly) and reduced-tap dewindowing
	 * (--exp-reduced-taps) are no longer toggled from the GUI: the Quality
	 * level (--quality below) selects them via ApplyQualityOptions() in
	 * amiga_mp3dec.c ("Faster" enables both, plus ASM Huffman and quarter-rate
	 * FDCT32).  CD32 Ultrafast still adds its own --exp-reduced-taps above as
	 * part of its fixed preset. */
	/* Manual subband cap always comes last so it overrides whatever default
	 * a fast-lowrate/ultrafast preset above already picked (e.g. CD32
	 * Ultrafast's hardcoded --subband-cap 12) -- --subband-cap N just does
	 * a plain last-flag-wins atoi() assignment in amiga_mp3dec.c. */
	if (app->subbandCapIndex > 0) {
		AddArg(args, "--subband-cap");
		sprintf(num, "%d", kSubbandCapValues[app->subbandCapIndex]);
		AddArg(args, num);
	}
	if (useFakeStereo) {
		AddArg(args, "--fake-stereo");
		AddArg(args, "--fake-stereo-delay");
		sprintf(num, "%d", kFakeStereoDelays[app->fakeStereoDelayIndex]);
		AddArg(args, num);
		AddArg(args, "--fake-stereo-shift");
		sprintf(num, "%d", kFakeStereoShifts[app->fakeStereoWidthIndex]);
		AddArg(args, num);
	} else if (useMono)
		AddArg(args, "--mono");
	else
		AddArg(args, "--stereo");
	AddArg(args, "--rate");
	AddArg(args, kRates[rateIndex]);
	AddArg(args, "--buffer-seconds");
	sprintf(num, "%d", ClampInt(app->bufferSeconds, 1, 10));
	AddArg(args, num);
	AddArg(args, "--volume");
	sprintf(num, "%d", ClampInt(app->volumePercent, 0, 100));
	AddArg(args, num);
	AddArg(args, "--quality");
	sprintf(num, "%d", app->qualityIndex);
	AddArg(args, num);
	if (app->decodeThenPlay)
		AddArg(args, "--decode-then-play");
	if (app->bench)
		AddArg(args, "--bench");
	AddArg(args, app->inputName);
	args->argv[args->argc] = NULL;
}

/* ------------------------------------------------------------------------- */
/* CLI parser reset (the C runtime getopt state is process-global)           */
/* ------------------------------------------------------------------------- */

extern int optind;
extern int opterr;
extern int optopt;
extern char *optarg;

static void ResetCliParser(void)
{
	optind = 1;
	opterr = 0;
	optopt = 0;
	optarg = NULL;
}

/* ------------------------------------------------------------------------- */
/* The playback child process                                                */
/* ------------------------------------------------------------------------- */

static int PlaybackProcessStillExists(void)
{
	struct Task *task;

	Forbid();
	task = FindTask((STRPTR)"MintAMP playback");
	Permit();
	return task != NULL;
}

static void PlaybackEntry(void)
{
	struct MsgPort *donePort;
	ULONG pending;
	int earlyStop;
	int ranDecoder = 0;
	int postedDone = 0;
	const char *childExitReason;

	gPlayer.task = FindTask(NULL);
	gPlayer.stage = "STARTING";
	gPlayer.startupStage = "child entered";
	RADIO_DBG(printf("radio-session: child entered session=%lu childTask=%p childProc=%p url=\"%s\" codec=%s state=STARTING\n", gPlayer.sessionId, gPlayer.task, gPlayer.process, gPlayer.url, gPlayer.codec);)
	pending = SetSignal(0, 0);
	earlyStop = gPlayer.stopRequested || gPlaybackInterrupted ||
		(pending & SIGBREAKF_CTRL_C);
	if (earlyStop)
		gPlaybackInterrupted = 1;

	ResetCliParser();
	if (!earlyStop) {
		MP3ResetStatics();
		ResetCliParser();
	}

	gGuiPlaybackStatus.runId = gEntryRunId;

	if (!earlyStop && !gPlayer.stopRequested && !gPlaybackInterrupted) {
		ranDecoder = 1;
		gMiniAmp3EmbeddedPlayback = 1;
		gPlayer.stage = "PLAYING";
		gPlayer.startupStage = "decoder main";
		RADIO_DBG(printf("radio-teardown: child entering HelixAmp3CliMain session=%lu\n", gPlayer.sessionId);)
		HelixAmp3CliMain(gPlayer.argc, gPlayer.argv);
		RADIO_DBG(printf("radio-teardown: child returned from HelixAmp3CliMain session=%lu stop=%d\n", gPlayer.sessionId, gPlayer.stopRequested);)
		gMiniAmp3EmbeddedPlayback = 0;
	}

	gPlayer.stage = "EXITING";
	gPlayer.cleanupStage = "cleanup enter";
	childExitReason = gPlayer.stopRequested ? "stop" :
		(strstr((const char *)gGuiPlaybackStatus.radioError, "stalled") ? "radio-stalled" :
			(gGuiPlaybackStatus.radioStatus == RADIO_STATUS_ERROR || gGuiPlaybackStatus.radioError[0] ? "radio-error" : "normal"));
	RADIO_DBG(printf("radio-session: child begins cleanup session=%lu reason=%s error=\"%s\"\n",
		gPlayer.sessionId,
		childExitReason,
		(const char *)gGuiPlaybackStatus.radioError);)
	gGuiPlaybackStatus.phase = GUIPLAY_PHASE_DONE;
	gGuiPlaybackStatus.cleanupComplete = 1;
	gPlayer.cleanupStage = "cleanup complete";
	RADIO_DBG(printf("radio-session: child cleanup complete session=%lu\n", gPlayer.sessionId);)
	(void)ranDecoder;

	gDoneRunId = gGuiPlaybackStatus.runId;
	donePort = gDonePort;
	if (donePort && !gPlayer.donePosted && !gDoneMsg.posted) {
		gDoneMsg.msg.mn_Node.ln_Type = NT_MESSAGE;
		gDoneMsg.magic = MR_DONE_MAGIC;
		gDoneMsg.runId = gGuiPlaybackStatus.runId;
		gDoneMsg.posted = 1;
		RADIO_DBG(printf("radio-teardown: child posting done message session=%lu done_magic=%08lx error_ptr=%p\n", gPlayer.sessionId, gDoneMsg.magic, (void *)gGuiPlaybackStatus.radioError);)
		gPlayer.donePosted = 1;
		postedDone = 1;
		PutMsg(donePort, &gDoneMsg.msg);
	} else if (donePort) {
		RADIO_DBG(printf("radio-teardown: duplicate child done suppressed session=%lu donePosted=%d msgPosted=%d\n", gPlayer.sessionId, gPlayer.donePosted, gDoneMsg.posted);)
	}
	if (!postedDone) RADIO_DBG(printf("radio-session: child no done port session=%lu done message not posted\n", gPlayer.sessionId);)
	gPlayer.stage = "EXITED";
	RADIO_DBG(printf("child exiting session=%lu reason=%s\n", gPlayer.sessionId, childExitReason);)
	RADIO_DBG(printf("radio-teardown: child PlaybackEntry exiting (task will terminate) session=%lu\n", gPlayer.sessionId);)
}

static void StartPlayback(MrApp *app)
{
	struct Process *thisProc;
	BPTR dirLock;
	BPTR nilOut;
	struct Message *stale;

	if (!MrVerifyAppMagic(app, "StartPlayback"))
		return;
	if (Radio_IsMemoryPoisoned()) {
		SetStatus(app, "Memory corruption detected. Save log and reboot before using MintAMP again.");
		RADIO_DBG(printf("radio-memory: refusing StartPlayback after MiniMem/ring corruption url=\"%s\"\n", app->inputName);)
		return;
	}
	if (!app->inputName[0]) {
		SetStatus(app, "Pick an audio file first.");
		return;
	}
	if (app->streamState == MR_STREAM_STARTING) {
		SetStatus(app, "Playback is already starting.");
		return;
	}
	if (app->playbackActive || app->playbackDonePending) {
		SetStatus(app, "Already playing - press Stop first.");
		return;
	}
	if (PlaybackProcessStillExists()) {
		SetStatus(app, "Previous playback is still exiting.");
		return;
	}
	if (!app->donePort) {
		SetStatus(app, "No reply port; cannot start playback.");
		return;
	}

	/* Drain any stale done message and re-arm the static message node. */
	while ((stale = GetMsg(app->donePort)) != NULL)
		;
	memset(&gDoneMsg, 0, sizeof(gDoneMsg));
	gDoneMsg.msg.mn_Length = sizeof(gDoneMsg);
	gDoneMsg.msg.mn_Node.ln_Type = NT_MESSAGE;
	gDoneMsg.magic = MR_DONE_MAGIC;

	memset((void *)&gGuiPlaybackStatus, 0, sizeof(gGuiPlaybackStatus));
	app->streamState = MR_STREAM_STARTING;
	app->playbackRunId = ++gRunCounter;
	gGuiPlaybackStatus.runId = app->playbackRunId;
	gEntryRunId = app->playbackRunId;

	BuildPlaybackArgs(app, &gArgs);
	gPlayer.argc = gArgs.argc;
	gPlayer.argv = gArgs.argv;
	gPlayer.stopRequested = 0;
	gPlayer.sessionId = app->playbackRunId;
	SafeCopy(gPlayer.url, sizeof(gPlayer.url), app->inputName);
	SafeCopy(gPlayer.codec, sizeof(gPlayer.codec), MrIsRadioInput(app->inputName) ? "radio" : "file");
	gPlayer.stage = "STARTING";
	gPlayer.startupStage = "CreateNewProc";
	gPlayer.cleanupStage = "";
	gPlayer.lastIoState = "";
	gPlayer.donePosted = 0;
	app->lastChildExitReason[0] = '\0';
	app->lastChildError[0] = '\0';
	app->lastRadioError[0] = '\0';
	app->parentDoneHandled = 0;
	app->lastChildEverPlayed = 0;
	app->lastChildFirstData = 0;
	gPlaybackInterrupted = 0;
	gDonePort = app->donePort;
	gDoneRunId = 0;

	thisProc = (struct Process *)FindTask(NULL);
	dirLock = DupLock(thisProc ? thisProc->pr_CurrentDir : (BPTR)0);
	nilOut = Open((STRPTR)"NIL:", MODE_NEWFILE);
	if (!nilOut) {
		if (dirLock)
			UnLock(dirLock);
		SetStatus(app, "Could not open NIL: for the player.");
		return;
	}

	gPlayer.process = CreateNewProcTags(
		NP_Entry,      (ULONG)PlaybackEntry,
		NP_Name,       (ULONG)"MintAMP playback",
		NP_Priority,   AMIGA_PLAYBACK_TASK_PRIORITY,
		NP_StackSize,  262144,
		NP_CurrentDir, dirLock,
		NP_Output,     nilOut,
		NP_CloseOutput, TRUE,
		NP_CopyVars,   FALSE,
		TAG_DONE);

	if (!gPlayer.process) {
		Close(nilOut);
		if (dirLock)
			UnLock(dirLock);
		gDonePort = NULL;
		SetStatus(app, "Could not launch the playback process.");
		return;
	}

	app->playbackActive = 1;
	app->playbackDonePending = 0;
	app->streamState = MR_STREAM_PLAYING;
	SafeCopy(app->currentStreamUrl, sizeof(app->currentStreamUrl), app->inputName);
	SafeCopy(app->currentStreamCodec, sizeof(app->currentStreamCodec), gPlayer.codec);
	gPlayer.task = (struct Task *)gPlayer.process;
	app->activeChildCount = 1; app->activeStreamSessions = 1; app->activeStreamTasks = 1;
	MrDebugSession("parent starts stream", app);
	app->stoppedByUser = 0;
	app->lastPhaseShown = -1;
	app->elapsedSecs = 0;
	app->lastFrames = 0;
	app->lastRadioMetaTick = 0;
	app->lastRadioStatusTick = 0;
	app->lastTimeTick = 0;
	app->lastRadioStatusShown = -1;
	UpdateTimeDisplay(app);
	EnablePlayStop(app, 1);
	if (MrIsRadioInput(app->inputName)) {
		char status[160];
		sprintf(status, "Buffering - %.140s", app->currentRadioStationName[0] ? app->currentRadioStationName : "Internet Radio");
		SetStatus(app, status);
		RadioSetStatus(app, status);
	} else
		SetStatus(app, "Starting playback...");
	SetGauge(app, 0);
}

static void StopPlayback(MrApp *app)
{
	struct Task *child;
	int canStop;

	if (!MrVerifyAppMagic(app, "StopPlayback"))
		return;
	RADIO_DBG(printf("radio-guard: before StopPlayback\n");)
	RADIO_DBG(printf("radio-stop: button streamState=%s playbackActive=%d playbackDonePending=%d activeChildCount=%lu process=%p task=%p stopRequested=%d queuedStreamUrl=\"%s\" playlistNextPending=%d session=%lu lastExit=\"%s\" lastError=\"%s\"\n",
		MrStreamStateName(app->streamState), app->playbackActive,
		app->playbackDonePending, app->activeChildCount, gPlayer.process,
		gPlayer.task, gPlayer.stopRequested, app->queuedStreamUrl,
		app->playlistNextPending, gPlayer.sessionId, app->lastChildExitReason,
		app->lastChildError);)
	canStop = app->playbackActive &&
		app->streamState != MR_STREAM_IDLE &&
		app->streamState != MR_STREAM_EXITED &&
		app->streamState != MR_STREAM_ERROR &&
		app->activeChildCount > 0 &&
		(gPlayer.process || gPlayer.task || PlaybackProcessStillExists());
	if (!canStop) {
		RADIO_DBG(printf("Stop ignored: no active playback child\n");)
		SetStatus(app, "Stopped.");
		RADIO_DBG(printf("radio-guard: after StopPlayback\n");)
		return;
	}
	if (gPlayer.stopRequested) {
		SetStatus(app, "Stopping...");
		RADIO_DBG(printf("radio-guard: after StopPlayback\n");)
		return;
	}
	app->stoppedByUser = 1;
	app->streamState = MR_STREAM_STOP_REQUESTED;
	gPlayer.stage = "STOP_REQUESTED";
	gPlayer.stopRequested = 1;
	app->stopWatchdogMicros = 0;
	app->stopWatchdogFired = 0;
	MrDebugSession("parent stop request sent", app);
	gPlaybackInterrupted = 1;

	/* Radio teardown safety (parity with the GadTools front-end): mark the
	 * shared radio status as stopping and clear the active flag straight away.
	 * Otherwise the timer poll below keeps calling MrSetRadioMetadata() against
	 * a stream the playback child is in the middle of closing, refreshing the
	 * GUI from radio state that is being torn down. */
	if (MrIsRadioInput(app->inputName)) {
		gGuiPlaybackStatus.radioStatus = (int)RADIO_STATUS_STOPPING;
		gGuiPlaybackStatus.radioActive = 0;
		gGuiPlaybackStatus.radioBufferedBytes = 0;
	}

	/* Wake the child immediately so it does not sit in WaitIO for the rest of
	 * a multi-second audio buffer.  Forbid()/FindTask() guards against the
	 * child already being torn down by DOS. */
	Forbid();
	child = FindTask((STRPTR)"MintAMP playback");
	if (child)
		Signal(child, SIGBREAKF_CTRL_C);
	Permit();

	app->streamState = MR_STREAM_STOPPING;
	gPlayer.stage = "STOPPING";
	SetStatus(app, "Stopping...");
	UpdateNextButtonState(app);
	RADIO_DBG(printf("radio-guard: after StopPlayback\n");)
}

static int StopPlaybackAndWait(MrApp *app, int ticks, const char *timeoutStatus)
{
	int waited;

	if (!app)
		return 1;
	if (!app->playbackActive && !app->playbackDonePending && !PlaybackProcessStillExists())
		return 1;
	StopPlayback(app);
	for (waited = 0; waited < ticks; waited++) {
		HandleDoneSignal(app);
		if (!app->playbackActive && !app->playbackDonePending && !PlaybackProcessStillExists())
			return 1;
		Delay(1);
	}
	app->streamState = MR_STREAM_STOP_TIMEOUT;
	if (timeoutStatus && timeoutStatus[0])
		SetStatus(app, timeoutStatus);
	MrDebugSession("parent stop timeout - queued stream not started", app);
	RADIO_DBG(printf("radio-session: stop timeout details childTask=%p session=%lu currentStage=\"%s\" lastStartup=\"%s\" lastCleanup=\"%s\" currentUrl=\"%s\" lastReadDecode=\"%s\" stopFlag=%d donePosted=%d\n", gPlayer.task, gPlayer.sessionId, gPlayer.stage ? (const char *)gPlayer.stage : "", gPlayer.startupStage ? (const char *)gPlayer.startupStage : "", gPlayer.cleanupStage ? (const char *)gPlayer.cleanupStage : "", gPlayer.url, gPlayer.lastIoState ? (const char *)gPlayer.lastIoState : "", gPlayer.stopRequested, gPlayer.donePosted);)
	return 0;
}
/* Test mode (MP3_NO_QUEUED_STREAM_SWITCH=1 in the environment): a station
 * selected while another stream is active/stopping is never remembered as a
 * queued switch.  FinalizePlayback() just returns to idle and the user has
 * to press Play again.  If reboots stop happening with this set, the
 * queued-switch state machine (not the single-session teardown path) is the
 * corruptor. */
static int MrNoQueuedStreamSwitchEnabled(void)
{
	static int cached = -1;
	if (cached < 0) {
		const char *v = getenv("MP3_NO_QUEUED_STREAM_SWITCH");
		cached = (v && *v && *v != '0') ? 1 : 0;
	}
	return cached;
}

/* Diagnostic-only: logs whether every resource counter from the just-torn-down
 * session has actually reached zero before a queued stream is allowed to
 * start.  Does not block -- these are expected to already be zero by the
 * time FinalizePlayback() runs (Radio_Close() drops them before the child
 * ever posts its done message), so this only ever fires as a warning if that
 * invariant is somehow violated. */
static void MrLogQueuedStreamReadiness(MrApp *app, const char *where)
{
	long sessions = 0, tasks = 0, sockets = 0, playbackSockets = 0;
	long decoders = 0, audioBuffers = 0, streamBuffers = 0;
	int stillExists = PlaybackProcessStillExists();

	Radio_GetTeardownStats(&sessions, &tasks, &sockets, &playbackSockets,
		&decoders, &audioBuffers, &streamBuffers);
	RADIO_DBG(printf("radio-teardown: queued-switch readiness (%s) activeChildCount=%lu processStillExists=%d "
		"active_stream_sessions=%ld active_stream_tasks=%ld open_socket_count=%ld playback_open_socket_count=%ld "
		"active_decoder_count=%ld active_audio_buffer_count=%ld active_stream_buffer_count=%ld\n",
		where, app->activeChildCount, stillExists, sessions, tasks, sockets, playbackSockets,
		decoders, audioBuffers, streamBuffers));
	if (stillExists || app->activeChildCount != 0 || sessions != 0 || tasks != 0 ||
		sockets != 0 || playbackSockets != 0 || decoders != 0 || audioBuffers != 0 || streamBuffers != 0)
		RADIO_DBG(printf("radio-teardown: WARNING queued-switch readiness (%s) found a nonzero counter -- "
			"old session's resources may not be fully released\n", where));
}

static void FinalizePlayback(MrApp *app)
{
	int stoppedByUser = app->stoppedByUser;
	char radioError[128];
	int failedStart;
	const char *finalStatus;

	if (!MrVerifyAppMagic(app, "FinalizePlayback"))
		return;
	RADIO_DBG(printf("radio-guard: before FinalizePlayback\n");)
	/* Stream completion handling runs on the GUI/main task (the playback
	 * child has already posted its done message and is being reaped): confirm
	 * the task identity so any parent-side RadioStream/message cleanup that
	 * frees here is attributed to the same task as the alerts. */
	MR_TASK_IDENTITY("stream-completion-handling");
	MrCopyVolatileString(radioError, sizeof(radioError), gGuiPlaybackStatus.radioError);
	failedStart = (!stoppedByUser && MrIsRadioInput(gPlayer.url) && gGuiPlaybackStatus.decodedFrames == 0 &&
		(gGuiPlaybackStatus.radioStatus == RADIO_STATUS_ERROR ||
		 gGuiPlaybackStatus.radioStatus == RADIO_STATUS_CONNECTING ||
		 gGuiPlaybackStatus.radioStatus == RADIO_STATUS_BUFFERING ||
		 gGuiPlaybackStatus.radioStatus == RADIO_STATUS_CLOSED));
	SafeCopy(app->lastChildExitReason, sizeof(app->lastChildExitReason),
		stoppedByUser ? "stop" : (failedStart ? "error" : "normal"));
	SafeCopy(app->lastChildError, sizeof(app->lastChildError),
		failedStart ? (radioError[0] ? radioError : "radio stream failed") : "");
	app->lastChildEverPlayed = gGuiPlaybackStatus.decodedFrames > 0 ? 1 : 0;
	app->lastChildFirstData = gGuiPlaybackStatus.radioBufferedBytes > 0 ? 1 : 0;
	app->lastCompletedWasHttps = (gPlayer.url[0] && strncmp(gPlayer.url, "https://", 8) == 0);
	app->playbackActive = 0;
	app->playbackDonePending = 0;
	app->stoppedByUser = 0;
	app->streamState = failedStart ? MR_STREAM_ERROR : MR_STREAM_EXITED;
	MrDebugSession("parent receives done and clears child pointer", app);
	RADIO_DBG(printf("radio-done: parent done received childExit=\"%s\" childError=\"%s\" everPlayed=%d firstData=%d streamStateBeforeFinalize=%s\n",
		app->lastChildExitReason, app->lastChildError,
		app->lastChildEverPlayed, app->lastChildFirstData,
		failedStart ? "ERROR" : "EXITED");)
	gPlayer.process = NULL;
	gPlayer.task = NULL;
	gPlayer.stopRequested = 0;
	app->activeChildCount = 0; app->activeStreamSessions = 0; app->activeStreamTasks = 0;
	app->stopWatchdogMicros = 0;
	app->stopWatchdogFired = 0;
	gPlaybackInterrupted = 0;
	gDonePort = NULL;
	ResetCliParser();

	EnablePlayStop(app, 0);
	app->elapsedSecs = (stoppedByUser || app->totalSecs <= 0) ? 0 : app->totalSecs;
	UpdateTimeDisplay(app);
	SetGauge(app, app->progressEnabled && !stoppedByUser ? 100 : 0);
	if (failedStart) {
		char status[160];
		sprintf(status, "Stream failed: %s", app->lastChildError[0] ? app->lastChildError : "radio stream failed");
		SafeCopy(app->lastRadioError, sizeof(app->lastRadioError), status);
		finalStatus = app->lastRadioError;
		SetStatus(app, app->lastRadioError);
	} else {
		finalStatus = stoppedByUser ? "Stopped." : "Finished.";
		SetStatus(app, finalStatus);
		app->streamState = MR_STREAM_IDLE;
	}
	RADIO_DBG(printf("radio-done: streamStateAfterFinalize=%s guiStatus=\"%s\"\n",
		MrStreamStateName(app->streamState), finalStatus);)
	RADIO_DBG(printf("radio-guard: after FinalizePlayback\n");)
	if (Radio_IsMemoryPoisoned()) {
		if (app->queuedStreamUrl[0] || app->playlistNextPending)
			SetStatus(app, "Memory corruption detected. Save log and reboot before using MintAMP again.");
		app->queuedStreamUrl[0] = '\0';
		app->playlistNextPending = 0;
		RADIO_DBG(printf("radio-memory: queued/next stream suppressed after memory poison session=%lu\n", gPlayer.sessionId);)
		return;
	}
	if (app->shuttingDown) {
		/* AppCloseShutdown() is reaping the child on the way out.  Starting
		 * a queued stream or playlist-next now would launch a fresh playback
		 * child that the imminent GUI teardown then pulls the rug from
		 * under.  Reachable since Stop can complete from mid-buffering
		 * states (Radio_SetStopCheck) that previously never finalized. */
		app->queuedStreamUrl[0] = '\0';
		app->playlistNextPending = 0;
		return;
	}
	if (app->queuedStreamUrl[0]) {
		app->queuedStreamUrl[0] = '\0';
		if (MrNoQueuedStreamSwitchEnabled()) {
			RADIO_DBG(printf("radio-teardown: MP3_NO_QUEUED_STREAM_SWITCH active -- discarding queued stream session=%lu\n",
				gPlayer.sessionId));
			SetStatus(app, "Stopped - press Play to start the next stream.");
			return;
		}
		MrLogQueuedStreamReadiness(app, "before queued stream start");
		RadioSetStatus(app, "Starting queued stream...");
		/* Give audio.device/DOS a moment to settle after the old child's
		 * task exit and resource teardown before the new probe/child starts. */
		Delay(10);
		RadioDoProbeAndPlay(app);
		return;
	}
	if (app->playlistNextPending) {
		app->playlistNextPending = 0;
		/* As for a queued stream above: let the old child's teardown
		 * settle before probing the next entry's stream. */
		if (MrIsRadioInput(app->inputName))
			Delay(10);
		PlaylistStartCurrent(app);
    }
#if defined(MINTAMP_SKIN) && defined(AMIGA_M68K)
    else if (gSkinPlayer && !stoppedByUser && !failedStart && gSkinAudio.completed_ok)
        gSkinPendingTrack=skin_player_order(gSkinPlayer,app->playlistCount,app->playlistCurrent,1,1);
#endif
}

static void HandleDoneSignal(MrApp *app)
{
	struct Message *msg;
	int gotDone = 0;
	char doneError[128];
	const char *doneReason;
	int doneEverPlayed;
	int doneFirstData;

	if (!app->donePort)
		return;
	while ((msg = GetMsg(app->donePort)) != NULL) {
		gotDone = 1;
		if (((MrDoneMessage *)msg)->magic != MR_DONE_MAGIC)
			RADIO_DBG(printf("radio-done: bad done message magic msg=%p magic=%08lx\n", (void *)msg, ((MrDoneMessage *)msg)->magic);)
	}
	if (!gotDone)
		return;
	if (app->parentDoneHandled) {
		RADIO_DBG(printf("radio-done: duplicate parent done ignored session=%lu handled=%d pending=%d active=%d\n", gPlayer.sessionId, app->parentDoneHandled, app->playbackDonePending, app->playbackActive);)
		return;
	}
	app->parentDoneHandled = 1;

	MrCopyVolatileString(doneError, sizeof(doneError), gGuiPlaybackStatus.radioError);
	doneReason = gPlayer.stopRequested ? "stop" :
		(strstr(doneError, "stalled") ? "radio-stalled" :
			(gGuiPlaybackStatus.radioStatus == RADIO_STATUS_ERROR || doneError[0] ? "error" : "normal"));
	doneEverPlayed = gGuiPlaybackStatus.decodedFrames > 0 ? 1 : 0;
	doneFirstData = gGuiPlaybackStatus.radioBufferedBytes > 0 ? 1 : 0;
	RADIO_DBG(printf("radio-session: parent receives done message session=%lu doneRun=%lu active=%d pending=%d childExit=\"%s\" childError=\"%s\" everPlayed=%d firstData=%d streamStateBefore=%s\n",
		gPlayer.sessionId, gDoneRunId, app->playbackActive,
		app->playbackDonePending, doneReason, doneError,
		doneEverPlayed, doneFirstData, MrStreamStateName(app->streamState));)
	app->playbackDonePending = 1;
	/* The child posts its done message just before returning from
	 * PlaybackEntry(); wait for DOS to actually reap the task before we let a
	 * new decoder start. */
	if (!PlaybackProcessStillExists())
		FinalizePlayback(app);
}

/* ------------------------------------------------------------------------- */
/* Status polling (driven by the timer tick)                                 */
/* ------------------------------------------------------------------------- */

static void PollPlaybackStatus(MrApp *app)
{
	int phase;
	unsigned long frames;
	int rate;
	long spareMs;
	unsigned long halfMs;
	unsigned long fastInputBytes;
	char buf[96];
	long audioSecs;
	int updates = 0;
	int radioStatus;
	int radioActive;
	unsigned long tick;

	if (!app)
		return;
	tick = ++app->pollTick;
	MR_POLL_DBG(("minimp3r: poll %lu start\n", tick));

	/* A late done where the child had already vanished before we drained the
	 * port: finalize now.  This is only a quick task lookup and never asks the
	 * playback child to respond synchronously. */
	if (app->playbackDonePending && !PlaybackProcessStillExists()) {
		FinalizePlayback(app);
		MR_POLL_DBG(("minimp3r: poll %lu finalized done, updates=%d\n", tick, updates));
		return;
	}
	if (!app->playbackActive) {
		MR_POLL_DBG(("minimp3r: poll %lu idle, updates=%d\n", tick, updates));
		return;
	}

	/* Last-resort watchdog: Stop has been outstanding for a long time and the
	 * child still has not been confirmed gone -- most likely wedged inside a
	 * blocking bsdsocket/AmiSSL call that never observes SIGBREAKF_CTRL_C.
	 * There is no safe way to force-kill an AmigaOS task stuck inside a
	 * library call, and starting a new playback child while this one might
	 * still be alive would let two children race on the same shared decoder/
	 * IPC globals. So this does not try to recover playback; it only
	 * replaces the indefinite silent "Stopping..." with an honest, one-shot
	 * status so the user knows a restart is needed. */
	if (!app->playbackDonePending && gPlayer.stopRequested) {
		app->stopWatchdogMicros += MR_TICK_MICROS;
		if (!app->stopWatchdogFired && app->stopWatchdogMicros >= STOP_WATCHDOG_TIMEOUT_MICROS) {
			app->stopWatchdogFired = 1;
			RADIO_DBG(printf("radio-stop: watchdog timeout after %luus, stream not responding to Stop\n", app->stopWatchdogMicros);)
			SetStatus(app, "Stream isn't responding to Stop - restart the app to recover.");
		}
	} else {
		app->stopWatchdogMicros = 0;
		app->stopWatchdogFired = 0;
	}

	radioStatus = gGuiPlaybackStatus.radioStatus;
	radioActive = gGuiPlaybackStatus.radioActive;
	if (MrIsRadioInput(app->inputName)) {
		if (radioStatus == RADIO_STATUS_ERROR) {
			char radioError[128], status[160];
			MrCopyVolatileString(radioError, sizeof(radioError), gGuiPlaybackStatus.radioError);
			gGuiPlaybackStatus.radioActive = 0;
			gGuiPlaybackStatus.radioBufferedBytes = 0;
			sprintf(status, "Stream failed: %s", radioError[0] ? radioError : "radio error");
			SafeCopy(app->lastRadioError, sizeof(app->lastRadioError), status);
			updates += SetStatusIfChanged(app, app->lastRadioError);
			RadioSetStatus(app, app->lastRadioError);
		} else if (radioActive &&
			radioStatus != RADIO_STATUS_STOPPING &&
			radioStatus != RADIO_STATUS_CLOSED) {
			int updateMeta = (app->lastRadioMetaTick == 0) ||
				(tick - app->lastRadioMetaTick >= MR_METADATA_TICKS);
			int updateStatus = (app->lastRadioStatusTick == 0) ||
				radioStatus != app->lastRadioStatusShown ||
				((radioStatus == RADIO_STATUS_BUFFERING || radioStatus == RADIO_STATUS_PLAYING) &&
				 MrRadioPlaybackHasStarted() && strncmp(app->shownStatus, "Streaming ", 10));
			if (updateMeta || updateStatus) {
				updates += MrSetRadioMetadata(app, updateStatus);
				if (updateMeta) app->lastRadioMetaTick = tick;
				if (updateStatus) { app->lastRadioStatusTick = tick; app->lastRadioStatusShown = radioStatus; }
			}
		}
	}

	phase   = gGuiPlaybackStatus.phase;
	frames  = gGuiPlaybackStatus.decodedFrames;
	rate    = gGuiPlaybackStatus.sampleRate;
	spareMs = gGuiPlaybackStatus.spareMs;
	halfMs  = gGuiPlaybackStatus.halfBufferMs;
	fastInputBytes = gGuiPlaybackStatus.fastInputBytes;

	if (!MrIsRadioInput(app->inputName) && rate > 0 && frames > 0 &&
		frames != app->lastFrames &&
		(app->lastTimeTick == 0 || tick - app->lastTimeTick >= MR_TIME_TICKS)) {
		app->lastFrames = frames;
		app->lastTimeTick = tick;
		audioSecs = (long)((frames * 1152UL) / (unsigned long)rate);
		audioSecs -= halfMs ? (long)((halfMs + 999UL) / 1000UL) : app->bufferSeconds;
		if (audioSecs < 0) audioSecs = 0;
		if (app->totalSecs > 0 && audioSecs > app->totalSecs) audioSecs = app->totalSecs;
		if (app->elapsedSecs != (int)audioSecs) {
			app->elapsedSecs = (int)audioSecs;
			UpdateTimeDisplay(app);
			updates++;
		}
		SetGauge(app, (app->progressEnabled && app->totalSecs > 0) ? (app->elapsedSecs * 100) / app->totalSecs : 0);
	}
	(void)spareMs;
	(void)halfMs;

	if (phase != app->lastPhaseShown) {
		int isRadioInput = MrIsRadioInput(app->inputName);
		app->lastPhaseShown = phase;
		switch (phase) {
		case GUIPLAY_PHASE_BUFFERING:
			if (!isRadioInput) {
				SetStatus(app, gGuiPlaybackStatus.startupStage == GUISTART_INPUT_PRELOAD_FASTMEM ?
					"Copying file to Fast RAM..." : "Buffering...");
				updates++;
			}
			break;
		case GUIPLAY_PHASE_PLAYING:
			if (!isRadioInput) {
				if (fastInputBytes)
					sprintf(buf, "Playing - Fast RAM: %luK, buffer: %lu.%03lu sec",
						(fastInputBytes + 1023UL) / 1024UL,
						halfMs / 1000UL, halfMs % 1000UL);
				else
					sprintf(buf, "Playing - buffer: %lu.%03lu sec",
						halfMs / 1000UL, halfMs % 1000UL);
				SetStatus(app, buf);
				updates++;
			}
			break;
		case GUIPLAY_PHASE_UNDERRUN:
			if (!isRadioInput) { SetStatus(app, "Playing (buffer low)..."); updates++; }
			break;
		case GUIPLAY_PHASE_STOPPING:
			SetStatus(app, "Stopping..."); updates++;
			break;
		case GUIPLAY_PHASE_ERROR:
			SetStatus(app, "Playback error."); updates++;
			break;
		default:
			break;
		}
	}
	MR_POLL_DBG(("minimp3r: poll %lu end updates=%d phase=%d radio=%d/%d fullrefresh=0\n",
		tick, updates, phase, radioActive, radioStatus));
}

/* ------------------------------------------------------------------------- */
/* Timer device                                                              */
/* ------------------------------------------------------------------------- */

static void ArmTimer(MrApp *app, ULONG micros)
{
	if (!app->timerReq)
		return;
	if (app->timerRunning) {
		AbortIO((struct IORequest *)app->timerReq);
		WaitIO((struct IORequest *)app->timerReq);
	}
	app->timerReq->tr_node.io_Command = TR_ADDREQUEST;
	app->timerReq->tr_time.tv_secs  = micros / 1000000UL;
	app->timerReq->tr_time.tv_micro = micros % 1000000UL;
	SendIO((struct IORequest *)app->timerReq);
	app->timerRunning = 1;
}

static int OpenTimer(MrApp *app)
{
	app->timerPort = CreateMsgPort();
	if (!app->timerPort)
		return 0;
	app->timerReq = (struct timerequest *)CreateIORequest(app->timerPort,
		sizeof(struct timerequest));
	if (!app->timerReq)
		return 0;
	if (OpenDevice((STRPTR)TIMERNAME, UNIT_VBLANK,
			(struct IORequest *)app->timerReq, 0) != 0)
		return 0;
	ArmTimer(app, MR_TICK_MICROS);
	return 1;
}

static void CloseTimer(MrApp *app)
{
	if (app->timerReq) {
		if (app->timerRunning) {
			RADIO_DBG(printf("app-dispose: before AbortIO timerReq=%p\n", app->timerReq);)
			AbortIO((struct IORequest *)app->timerReq);
			WaitIO((struct IORequest *)app->timerReq);
			app->timerRunning = 0;
			RADIO_DBG(printf("app-dispose: after AbortIO/WaitIO timerReq\n");)
			Radio_CheckMiniMem("after timer AbortIO/WaitIO");
		}
		if (app->timerReq->tr_node.io_Device)
			CloseDevice((struct IORequest *)app->timerReq);
		DeleteIORequest((struct IORequest *)app->timerReq);
		app->timerReq = NULL;
	}
	if (app->timerPort) {
		DeleteMsgPort(app->timerPort);
		app->timerPort = NULL;
	}
}

static long CountListNodes(const struct List *list)
{
	long count = 0;
	struct Node *node;
	if (!list || !list->lh_Head)
		return 0;
	for (node = list->lh_Head; node && node->ln_Succ; node = node->ln_Succ)
		count++;
	return count;
}

static long DrainMsgPortForClose(struct MsgPort *port, const char *reason)
{
	long drained = 0;
	struct Message *msg;
	if (!port) {
		RADIO_DBG(printf("app-close-drain: reason=\"%s\" port=%p drained=%ld\n",
			reason ? reason : "", port, drained);)
		return 0;
	}
	while ((msg = GetMsg(port)) != NULL)
		drained++;
	RADIO_DBG(printf("app-close-drain: reason=\"%s\" port=%p drained=%ld\n",
		reason ? reason : "", port, drained);)
	return drained;
}

/* Workbench owns AppMessages and requires every one to be replied before the
 * AppPort is deleted.  Unlike the private done/timer ports above, simply
 * removing these messages would leave Workbench waiting on stale storage. */
static long DrainReplyMsgPortForClose(struct MsgPort *port, const char *reason)
{
	long drained = 0;
	struct Message *msg;
	if (!port)
		return 0;
	while ((msg = GetMsg(port)) != NULL) {
		ReplyMsg(msg);
		drained++;
	}
	RADIO_DBG(printf("app-close-drain: reason=\"%s\" appPort=%p replied=%ld\n",
		reason ? reason : "", port, drained);)
	return drained;
}

static long DrainWindowUserPortForClose(struct Window *win, const char *reason)
{
	long drained = 0;
	struct IntuiMessage *imsg;
	if (!win || !win->UserPort) {
		RADIO_DBG(printf("app-close-drain: reason=\"%s\" window=%p port=%p drained=%ld\n",
			reason ? reason : "", win, win ? win->UserPort : NULL, drained);)
		return 0;
	}
	while ((imsg = (struct IntuiMessage *)GetMsg(win->UserPort)) != NULL) {
		ReplyMsg((struct Message *)imsg);
		drained++;
	}
	RADIO_DBG(printf("app-close-drain: reason=\"%s\" window=%p port=%p drained=%ld\n",
		reason ? reason : "", win, win->UserPort, drained);)
	return drained;
}

static long DrainGuiPorts(MrApp *app, const char *reason)
{
	long drained = 0;
	if (!app)
		return 0;
	drained += DrainWindowUserPortForClose(app->win, reason);
	drained += DrainWindowUserPortForClose(app->rbWin, reason);
	drained += DrainWindowUserPortForClose(app->plWin, reason);
	drained += DrainMsgPortForClose(app->donePort, reason);
	drained += DrainMsgPortForClose(gDonePort, reason);
	drained += DrainMsgPortForClose(app->timerPort, reason);
	return drained;
}

static void AuditLiveGuiResource(const char *prefix, const char *name, const void *ptr)
{
	if (ptr)
		RADIO_DBG(printf("%s: ERROR live gui resource name=%s ptr=%p\n",
			prefix ? prefix : "app-close-audit", name ? name : "unknown", ptr);)
}

static void PreCloseLibsAudit(MrApp *app)
{
	long stationNodes;
	long playlistNodes;
	long chooserCount;
	long imageObjects;
	long hooks;
	if (!app)
		return;
	stationNodes = CountListNodes(&app->rbList);
	playlistNodes = CountListNodes(&app->plList);
	chooserCount =
		(app->rbCodecGad ? 1 : 0) + (app->rbCountryCodeGad ? 1 : 0) +
		(app->rbSchemeGad ? 1 : 0) + (app->rbLimitGad ? 1 : 0) +
		(app->rbBitrateGad ? 1 : 0) + (app->qualityGad ? 1 : 0) +
		(app->subbandCapGad ? 1 : 0) +
		(app->channelGad ? 1 : 0) + (app->rateGad ? 1 : 0);
	imageObjects = app->artGad ? 1 : 0;
	hooks = 0;
	RADIO_DBG(printf("app-close-audit: rbWin=%p plWin=%p mainWinObj=%p playerWinObj=%p menuStrip=%p appPort=%p donePort=%p timerPort=%p timerReq=%p\n",
		app->rbWin, app->plWin, app->winObj, app->plWinObj, app->menuStrip,
		(void *)0, app->donePort, app->timerPort, app->timerReq);)
	RADIO_DBG(printf("app-close-audit: stationList=%p stationNodes=%ld playlistNodes=%ld chooserLabels=%p chooserCount=%ld getFileReq=%p imageObjects=%ld hooks=%ld\n",
		&app->rbList, stationNodes, playlistNodes, (void *)0, chooserCount,
		app->fileGad, imageObjects, hooks);)
	RADIO_DBG(printf("app-close-audit: currentArtwork=%p artworkBitmap=%p artworkDTObj=%p listBrowserLabels=%p listBrowserNodes=%p\n",
		app->artValid ? app->artRGBBuf : NULL, app->artValid ? app->artPenIdx : NULL,
		(void *)0, &app->rbList, stationNodes ? app->rbList.lh_Head : NULL);)
	RADIO_DBG(printf("app-close-audit: rbBrowserObj=%p rbListObj=%p rbChooserObj=%p rbGetFileObj=%p playerLayoutObj=%p\n",
		app->rbWinObj, app->rbListGad, app->rbCodecGad, (void *)0, app->winObj);)
	AuditLiveGuiResource("app-close-audit", "rbWin", app->rbWin);
	AuditLiveGuiResource("app-close-audit", "plWin", app->plWin);
	AuditLiveGuiResource("app-close-audit", "mainWinObj", app->winObj);
	AuditLiveGuiResource("app-close-audit", "playerWinObj", app->plWinObj);
	AuditLiveGuiResource("app-close-audit", "radioWinObj", app->rbWinObj);
	AuditLiveGuiResource("app-close-audit", "menuStrip", app->menuStrip);
	AuditLiveGuiResource("app-close-audit", "timerReq", app->timerReq);
	AuditLiveGuiResource("app-close-audit", "timerPort", app->timerPort);
}

static int GuiPointersNull(MrApp *app)
{
	if (!app)
		return 1;
	return !app->win && !app->rbWin && !app->plWin && !app->winObj &&
		!app->rbWinObj && !app->plWinObj && !app->menuStrip &&
		!app->timerPort && !app->timerReq &&
		CountListNodes(&app->rbList) == 0 && CountListNodes(&app->plList) == 0 &&
		!app->rbListGad && !app->plListGad;
}

static void PostCloseLibsAudit(MrApp *app, long portsDrained)
{
	int ptrsNull;
	if (!app)
		return;
	ptrsNull = GuiPointersNull(app);
	RADIO_DBG(printf("app-close-audit-final: rbWin=%p plWin=%p mainWinObj=%p playerWinObj=%p menuStrip=%p appPort=%p donePort=%p timerPort=%p timerReq=%p\n",
		app->rbWin, app->plWin, app->winObj, app->plWinObj, app->menuStrip,
		(void *)0, app->donePort, app->timerPort, app->timerReq);)
	RADIO_DBG(printf("app-close-audit-final: gui_ptrs_null=%d windowsClosed=%d objectsDisposed=%d listsFreed=%d chooserFreed=%d hooksFreed=%d portsDrained=%d\n",
		ptrsNull, !app->win && !app->rbWin && !app->plWin,
		!app->winObj && !app->rbWinObj && !app->plWinObj,
		CountListNodes(&app->rbList) == 0 && CountListNodes(&app->plList) == 0,
		!app->rbCodecGad && !app->rbCountryCodeGad && !app->rbSchemeGad &&
			!app->rbLimitGad && !app->rbBitrateGad,
		1, portsDrained >= 0);)
	if (!ptrsNull) {
		AuditLiveGuiResource("app-close-audit-final", "rbWin", app->rbWin);
		AuditLiveGuiResource("app-close-audit-final", "plWin", app->plWin);
		AuditLiveGuiResource("app-close-audit-final", "mainWinObj", app->winObj);
		AuditLiveGuiResource("app-close-audit-final", "radioWinObj", app->rbWinObj);
		AuditLiveGuiResource("app-close-audit-final", "playlistWinObj", app->plWinObj);
		AuditLiveGuiResource("app-close-audit-final", "menuStrip", app->menuStrip);
		AuditLiveGuiResource("app-close-audit-final", "timerPort", app->timerPort);
		AuditLiveGuiResource("app-close-audit-final", "timerReq", app->timerReq);
	}
}

static void MrCloseWindow(MrApp *app);

static void DisposeGuiObjectsBeforeCloseLibs(MrApp *app)
{
	if (!app)
		return;
	RADIO_DBG(printf("app-dispose: begin\n");)
	DrainGuiPorts(app, "before dispose");
	CloseTimer(app);
	RADIO_DBG(printf("app-dispose: CloseTimer done\n");)
	RADIO_DBG(printf("app-dispose: before DisposeObject mainWinObj=%p\n", app->winObj);)
	MrCloseWindow(app);
	RADIO_DBG(printf("app-dispose: after DisposeObject mainWinObj\n");)
	Radio_CheckMiniMem("after DisposeObject mainWinObj");
	if (app->appPort) {
		DrainReplyMsgPortForClose(app->appPort, "before DeleteMsgPort appPort");
		DeleteMsgPort(app->appPort);
		app->appPort = NULL;
	}
	if (app->donePort) {
		DrainMsgPortForClose(app->donePort, "before DeleteMsgPort donePort");
		DeleteMsgPort(app->donePort);
		app->donePort = NULL;
	}
	gDonePort = NULL;
	RADIO_DBG(printf("app-dispose: end\n");)
}

/* ------------------------------------------------------------------------- */
/* Library / class open / close                                              */
/* ------------------------------------------------------------------------- */

static void CloseLibs(void)
{
	int skipReactionClasses = radio_runtime_flag_enabled("MP3_SKIP_CLOSE_REACTION_CLASSES");
	RADIO_DBG(printf("app-close: CloseLibs enter Label=%p String=%p FuelGauge=%p CheckBox=%p Slider=%p ListBrowser=%p Chooser=%p GetFile=%p Button=%p Layout=%p Window=%p Utility=%p Intuition=%p\n",
		LabelBase, StringBase, FuelGaugeBase, CheckBoxBase, SliderBase,
		ListBrowserBase, ChooserBase, GetFileBase, ButtonBase, LayoutBase, WindowBase, UtilityBase, IntuitionBase);)
	if (skipReactionClasses) {
		RADIO_DBG(printf("app-close: CloseLibs diagnostic skip MP3_SKIP_CLOSE_REACTION_CLASSES=1 Label=%p String=%p FuelGauge=%p CheckBox=%p Slider=%p ListBrowser=%p Chooser=%p GetFile=%p Button=%p Layout=%p Window=%p\n",
			LabelBase, StringBase, FuelGaugeBase, CheckBoxBase, SliderBase,
			ListBrowserBase, ChooserBase, GetFileBase, ButtonBase, LayoutBase, WindowBase);)
		LabelBase = StringBase = FuelGaugeBase = CheckBoxBase = SliderBase = NULL;
		ListBrowserBase = ChooserBase = GetFileBase = ButtonBase = LayoutBase = WindowBase = NULL;
	} else {
		if (LabelBase)     { CloseLibrary(LabelBase);     LabelBase = NULL; RADIO_DBG(printf("app-close: CloseLibs Label done\n");) Radio_CheckMiniMem("after CloseLibrary Label"); }
		if (StringBase)    { CloseLibrary(StringBase);    StringBase = NULL; RADIO_DBG(printf("app-close: CloseLibs String done\n");) Radio_CheckMiniMem("after CloseLibrary String"); }
		if (FuelGaugeBase) { CloseLibrary(FuelGaugeBase); FuelGaugeBase = NULL; RADIO_DBG(printf("app-close: CloseLibs FuelGauge done\n");) Radio_CheckMiniMem("after CloseLibrary FuelGauge"); }
		if (CheckBoxBase)  { CloseLibrary(CheckBoxBase);  CheckBoxBase = NULL; RADIO_DBG(printf("app-close: CloseLibs CheckBox done\n");) Radio_CheckMiniMem("after CloseLibrary CheckBox"); }
		if (SliderBase)    { CloseLibrary(SliderBase);    SliderBase = NULL; RADIO_DBG(printf("app-close: CloseLibs Slider done\n");) Radio_CheckMiniMem("after CloseLibrary Slider"); }
		if (!radio_runtime_flag_enabled("MP3_SKIP_CLOSE_LISTBROWSER") && ListBrowserBase) {
			CloseLibrary(ListBrowserBase); ListBrowserBase = NULL; RADIO_DBG(printf("app-close: CloseLibs ListBrowser done\n");) Radio_CheckMiniMem("after CloseLibrary ListBrowser");
		} else if (ListBrowserBase) {
			RADIO_DBG(printf("app-close: CloseLibs diagnostic skip MP3_SKIP_CLOSE_LISTBROWSER=1 base=%p\n", ListBrowserBase);)
			ListBrowserBase = NULL;
		}
		if (!radio_runtime_flag_enabled("MP3_SKIP_CLOSE_CHOOSER") && ChooserBase) {
			CloseLibrary(ChooserBase); ChooserBase = NULL; RADIO_DBG(printf("app-close: CloseLibs Chooser done\n");) Radio_CheckMiniMem("after CloseLibrary Chooser");
		} else if (ChooserBase) {
			RADIO_DBG(printf("app-close: CloseLibs diagnostic skip MP3_SKIP_CLOSE_CHOOSER=1 base=%p\n", ChooserBase);)
			ChooserBase = NULL;
		}
		if (!radio_runtime_flag_enabled("MP3_SKIP_CLOSE_GETFILE") && GetFileBase) {
			CloseLibrary(GetFileBase); GetFileBase = NULL; RADIO_DBG(printf("app-close: CloseLibs GetFile done\n");) Radio_CheckMiniMem("after CloseLibrary GetFile");
		} else if (GetFileBase) {
			RADIO_DBG(printf("app-close: CloseLibs diagnostic skip MP3_SKIP_CLOSE_GETFILE=1 base=%p\n", GetFileBase);)
			GetFileBase = NULL;
		}
		if (ButtonBase)    { CloseLibrary(ButtonBase);    ButtonBase = NULL; RADIO_DBG(printf("app-close: CloseLibs Button done\n");) Radio_CheckMiniMem("after CloseLibrary Button"); }
		if (LayoutBase)    { CloseLibrary(LayoutBase);    LayoutBase = NULL; RADIO_DBG(printf("app-close: CloseLibs Layout done\n");) Radio_CheckMiniMem("after CloseLibrary Layout"); }
		if (WindowBase)    { CloseLibrary(WindowBase);    WindowBase = NULL; RADIO_DBG(printf("app-close: CloseLibs Window done\n");) Radio_CheckMiniMem("after CloseLibrary Window"); }
	}
	if (AslBase)       { CloseLibrary(AslBase);       AslBase = NULL; RADIO_DBG(printf("app-close: CloseLibs ASL done\n");) Radio_CheckMiniMem("after CloseLibrary ASL"); }
	if (IconBase)      { CloseLibrary(IconBase);      IconBase = NULL; RADIO_DBG(printf("app-close: CloseLibs Icon done\n");) Radio_CheckMiniMem("after CloseLibrary Icon"); }
	if (UtilityBase)   { CloseLibrary(UtilityBase);   UtilityBase = NULL; RADIO_DBG(printf("app-close: CloseLibs Utility done\n");) Radio_CheckMiniMem("after CloseLibrary Utility"); }
	if (IntuitionBase) { CloseLibrary((struct Library *)IntuitionBase); IntuitionBase = NULL; RADIO_DBG(printf("app-close: CloseLibs Intuition done\n");) Radio_CheckMiniMem("after CloseLibrary Intuition"); }
	RADIO_DBG(printf("app-close: CloseLibs exit\n");)
}

static int OpenLibs(void)
{
	IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 39);
	if (!IntuitionBase) {
		fprintf(stderr, "MintAMP needs intuition.library V39+.\n");
		return 0;
	}
	UtilityBase = OpenLibrary("utility.library", 39);
	if (!UtilityBase) {
		fprintf(stderr, "MintAMP needs utility.library V39+.\n");
		return 0;
	}
	AslBase = OpenLibrary("asl.library", 39);
	if (!AslBase) {
		fprintf(stderr, "MintAMP needs asl.library V39+.\n");
		return 0;
	}
	IconBase = OpenLibrary("icon.library", 39);

	WindowBase    = OpenLibrary("window.class",            MINIMP3R_CLASS_VERSION);
	LayoutBase    = OpenLibrary("gadgets/layout.gadget",   MINIMP3R_CLASS_VERSION);
	ButtonBase    = OpenLibrary("gadgets/button.gadget",   MINIMP3R_CLASS_VERSION);
	GetFileBase   = OpenLibrary("gadgets/getfile.gadget",  MINIMP3R_CLASS_VERSION);
	ChooserBase   = OpenLibrary("gadgets/chooser.gadget",  MINIMP3R_CLASS_VERSION);
	ListBrowserBase = OpenLibrary("gadgets/listbrowser.gadget", MINIMP3R_CLASS_VERSION);
	SliderBase    = OpenLibrary("gadgets/slider.gadget",   MINIMP3R_CLASS_VERSION);
	CheckBoxBase  = OpenLibrary("gadgets/checkbox.gadget", MINIMP3R_CLASS_VERSION);
	FuelGaugeBase = OpenLibrary("gadgets/fuelgauge.gadget",MINIMP3R_CLASS_VERSION);
	StringBase    = OpenLibrary("gadgets/string.gadget",   MINIMP3R_CLASS_VERSION);
	LabelBase     = OpenLibrary("images/label.image",      MINIMP3R_CLASS_VERSION);

	if (!WindowBase || !LayoutBase || !ButtonBase || !GetFileBase ||
		!ChooserBase || !ListBrowserBase || !SliderBase || !CheckBoxBase || !FuelGaugeBase ||
		!StringBase || !LabelBase) {
		fprintf(stderr,
			"MintAMP needs the ReAction (or ClassAct) classes V%d+ installed.\n",
			MINIMP3R_CLASS_VERSION);
		return 0;
	}
	return 1;
}

/* ------------------------------------------------------------------------- */
/* Window build / teardown                                                   */
/* ------------------------------------------------------------------------- */

/* A labelled child gadget, added to a (vertical) layout group. */
#define ADD_LABELLED(gadget, labeltext) \
        LAYOUT_AddChild, (ULONG)(gadget), \
        CHILD_Label, (ULONG)NewObject(LABEL_GetClass(), NULL, LABEL_Text, (ULONG)(labeltext), TAG_DONE)

static Object *ReadonlyString(ULONG id, const char *text, ULONG max)
{
	return (Object *)NewObject(STRING_GetClass(), NULL,
		GA_ID, id, GA_ReadOnly, TRUE, STRINGA_TextVal, (ULONG)text,
		STRINGA_BufferPos, 0,
		STRINGA_DispPos, 0,
		STRINGA_MaxChars, max, TAG_DONE);
}

static int CheckGadget(Object *obj, const char *name)
{
	if (obj)
		return 1;
	fprintf(stderr, "MintAMP: could not create %s gadget.\n", name);
	return 0;
}

static int MrOpenWindow(MrApp *app)
{
	Object *root;
	struct DiskObject *windowIcon = NULL;

	app->fileGad = (Object *)NewObject(GETFILE_GetClass(), NULL,
		GA_ID, GID_FILE,
		GA_RelVerify, TRUE,
		GETFILE_TitleText, (ULONG)"Choose an audio file",
		GETFILE_Pattern, (ULONG)"#?.(mp3|flac|aac|ogg|oga|wav|wma|8svx|iff|svx|aif|aiff)",
		GETFILE_DoPatterns, TRUE,
		GETFILE_FullFile, (ULONG)(app->inputName[0] ? app->inputName : ""),
		TAG_DONE);

	app->rateGad = (Object *)NewObject(CHOOSER_GetClass(), NULL,
	                GA_ID, GID_RATE,
	                GA_RelVerify, TRUE,
	                CHOOSER_LabelArray, (ULONG)kRateLabels,
	                CHOOSER_Selected, (ULONG)app->rateIndex,
	                TAG_DONE);

	app->qualityGad = (Object *)NewObject(CHOOSER_GetClass(), NULL,
	                GA_ID, GID_QUALITY,
	                GA_RelVerify, TRUE,
	                CHOOSER_LabelArray, (ULONG)kQualityLabels,
	                CHOOSER_Selected, (ULONG)app->qualityIndex,
	                TAG_DONE);

	app->subbandCapGad = (Object *)NewObject(CHOOSER_GetClass(), NULL,
	                GA_ID, GID_SUBBAND_CAP,
	                GA_RelVerify, TRUE,
	                CHOOSER_LabelArray, (ULONG)kSubbandCapLabels,
	                CHOOSER_Selected, (ULONG)app->subbandCapIndex,
	                TAG_DONE);

	app->channelGad = (Object *)NewObject(CHOOSER_GetClass(), NULL,
	                GA_ID, GID_CHANNEL,
	                GA_RelVerify, TRUE,
	                CHOOSER_LabelArray, (ULONG)kChannelLabels,
	                CHOOSER_Selected, (ULONG)(app->mono ? 1 : 0),
	                GA_Disabled, (ULONG)(app->fakeStereo ? TRUE : FALSE),
	                TAG_DONE);

	app->volumeGad = (Object *)NewObject(SLIDER_GetClass(), NULL,
	                GA_ID, GID_VOLUME,
	                GA_RelVerify, TRUE,
	                SLIDER_Min, 0,
	                SLIDER_Max, 100,
	                SLIDER_Level, (ULONG)app->volumePercent,
	                SLIDER_Orientation, SORIENT_HORIZ,
	                SLIDER_LevelFormat, (ULONG)"%ld%%",
	                TAG_DONE);

	app->bufferGad = (Object *)NewObject(SLIDER_GetClass(), NULL,
	                GA_ID, GID_BUFFER,
	                GA_RelVerify, TRUE,
	                SLIDER_Min, 1,
	                SLIDER_Max, 10,
	                SLIDER_Level, (ULONG)app->bufferSeconds,
	                SLIDER_Orientation, SORIENT_HORIZ,
	                SLIDER_LevelFormat, (ULONG)"%ld s",
	                TAG_DONE);

	/* The decoder optimisation stack is now driven entirely by the Quality
	 * chooser (see ApplyQualityOptions() in amiga_mp3dec.c): "Faster" enables
	 * the ASM polyphase, ASM Huffman, reduced-tap dewindowing and quarter-rate
	 * FDCT32 paths, with fewer of them at each higher-quality step.  The old
	 * "Fast Polyphase", "Reduced-tap dewindowing" and "Fast low-rate decode"
	 * checkboxes duplicated that (and were silently overridden at "Faster"), so
	 * only the independent whole-file preload toggle remains. */
	app->fastMemGad = (Object *)NewObject(CHECKBOX_GetClass(), NULL,
	                GA_ID, GID_FASTMEM,
	                GA_RelVerify, TRUE,
	                GA_Text, (ULONG)"Preload file into Fast RAM",
	                GA_Selected, (ULONG)(app->fastMem ? TRUE : FALSE),
	                TAG_DONE);

	/* Build with the FULL label list so the layout sizes the chooser box to
	 * its longest entry, "22050 Mono Ultrafast".  Sizing it to the shorter
	 * kSpeedLabelsNo22050 set (as happens whenever the app starts at a non-
	 * 22050 rate) left the box too narrow, so once the rate was switched to
	 * 22050 the swapped-in long label spilled past the frame ("...ast"
	 * overlapping the border).  The rate-appropriate (possibly narrower) list
	 * is applied by UpdateSpeedGadgetChoices() right after the window opens;
	 * swapping to a shorter list never needs a wider box, so the frame stays
	 * correctly sized for the long label. */
	app->speedGad = (Object *)NewObject(CHOOSER_GetClass(), NULL,
	                GA_ID, GID_SPEED,
	                GA_RelVerify, TRUE,
	                CHOOSER_LabelArray, (ULONG)kSpeedLabels,
	                CHOOSER_Selected, (ULONG)SpeedChoiceFromApp(app),
	                TAG_DONE);

	app->widthGad = (Object *)NewObject(CHOOSER_GetClass(), NULL,
	                GA_ID, GID_WIDTH,
	                GA_RelVerify, TRUE,
	                CHOOSER_LabelArray, (ULONG)kWidthLabels,
	                CHOOSER_Selected, (ULONG)(app->fakeStereo ? app->fakeStereoWidthIndex + 1 : 0),
	                TAG_DONE);

	app->delayGad = (Object *)NewObject(CHOOSER_GetClass(), NULL,
	                GA_ID, GID_DELAY,
	                GA_RelVerify, TRUE,
	                CHOOSER_LabelArray, (ULONG)kDelayLabels,
	                CHOOSER_Selected, (ULONG)app->fakeStereoDelayIndex,
	                TAG_DONE);

	app->playGad = (Object *)NewObject(BUTTON_GetClass(), NULL,
	                GA_ID, GID_PLAY,
	                GA_RelVerify, TRUE,
	                GA_Text, (ULONG)"_Play",
	                TAG_DONE);

	app->stopGad = (Object *)NewObject(BUTTON_GetClass(), NULL,
	                GA_ID, GID_STOP,
	                GA_RelVerify, TRUE,
	                GA_Disabled, TRUE,
	                GA_Text, (ULONG)"_Stop",
	                TAG_DONE);

	app->nextGad = (Object *)NewObject(BUTTON_GetClass(), NULL,
	                GA_ID, GID_NEXT, GA_RelVerify, TRUE, GA_Disabled, TRUE, GA_Text, (ULONG)"_Next", TAG_DONE);
	app->rewGad = (Object *)NewObject(BUTTON_GetClass(), NULL,
	                GA_ID, GID_REW, GA_RelVerify, TRUE, GA_Text, (ULONG)"<<", TAG_DONE);
	app->ffwdGad = (Object *)NewObject(BUTTON_GetClass(), NULL,
	                GA_ID, GID_FFWD, GA_RelVerify, TRUE, GA_Text, (ULONG)">>", TAG_DONE);
	app->filterGad = (Object *)NewObject(BUTTON_GetClass(), NULL,
	                GA_ID, GID_FILTER, GA_RelVerify, TRUE, GA_Text, (ULONG)"FLT", TAG_DONE);
	app->playlistGad = (Object *)NewObject(BUTTON_GetClass(), NULL,
	                GA_ID, GID_PLAYLIST, GA_RelVerify, TRUE, GA_Text, (ULONG)"Playlist", TAG_DONE);
	app->radioGad = (Object *)NewObject(BUTTON_GetClass(), NULL,
	                GA_ID, GID_RADIO, GA_RelVerify, TRUE, GA_Disabled, !app->hasNetwork, GA_Text, (ULONG)"Radio", TAG_DONE);

	app->gaugeGad = (Object *)NewObject(FUELGAUGE_GetClass(), NULL,
	                FUELGAUGE_Min, 0,
	                FUELGAUGE_Max, 100,
	                FUELGAUGE_Level, 0,
	                FUELGAUGE_Percent, TRUE,
	                FUELGAUGE_Justification, FGJ_CENTER,
	                TAG_DONE);

	app->statusGad = (Object *)NewObject(STRING_GetClass(), NULL,
	                GA_ReadOnly, TRUE,
	                STRINGA_TextVal, (ULONG)"Ready.",
	                STRINGA_MaxChars, 128,
	                TAG_DONE);

	app->timeGad = ReadonlyString(GID_TIME, "00:00 / --:--", 32);
	app->fileInfoGad = ReadonlyString(GID_FILEINFO, "No file info", 128);
	app->titleGad = ReadonlyString(GID_TITLE, "-", 96);
	app->artistGad = ReadonlyString(GID_ARTIST, "-", 96);
	app->albumGad = ReadonlyString(GID_ALBUM, "-", 128);
	app->trackGad = ReadonlyString(GID_TRACK, "-", 32);
	app->genreGad = ReadonlyString(GID_GENRE, "-", 64);
	app->ratingGad = ReadonlyString(GID_RATING, "0/5", 16);
	/* ReAction artwork render target: a fixed right-hand box.  The picojpeg
	 * decode/dither path (shared with the GadTools frontend) renders the 64x64
	 * thumbnail straight into this gadget's rectangle via DrawArtPanel(), which
	 * is re-stamped on every window refresh/resize. */
	app->artGad = (Object *)NewObject(BUTTON_GetClass(), NULL,
	                GA_ID, GID_LAST,
	                GA_ReadOnly, TRUE,
	                GA_Text, (ULONG)"",
	                TAG_DONE);
	{ int i; for (i = 0; i < 5; i++) app->starGad[i] = (Object *)NewObject(BUTTON_GetClass(), NULL, GA_ID, GID_STAR1 + i, GA_RelVerify, TRUE, GA_Text, (ULONG)"-", TAG_DONE); }

	if (!CheckGadget(app->fileGad, "file") || !CheckGadget(app->rateGad, "rate") ||
		!CheckGadget(app->qualityGad, "quality") || !CheckGadget(app->subbandCapGad, "subband cap") ||
		!CheckGadget(app->channelGad, "channel") ||
		!CheckGadget(app->volumeGad, "volume") || !CheckGadget(app->bufferGad, "buffer") ||
		!CheckGadget(app->fastMemGad, "fast memory") ||
		!CheckGadget(app->speedGad, "speed") || !CheckGadget(app->widthGad, "width") ||
		!CheckGadget(app->delayGad, "delay") || !CheckGadget(app->playGad, "play") ||
		!CheckGadget(app->nextGad, "next") || !CheckGadget(app->stopGad, "stop") ||
		!CheckGadget(app->rewGad, "rewind") || !CheckGadget(app->ffwdGad, "fast-forward") ||
		!CheckGadget(app->filterGad, "filter") || !CheckGadget(app->playlistGad, "playlist") ||
		!CheckGadget(app->radioGad, "internet radio") ||
		!CheckGadget(app->gaugeGad, "progress") || !CheckGadget(app->statusGad, "status") ||
		!CheckGadget(app->timeGad, "time") || !CheckGadget(app->fileInfoGad, "file info") ||
		!CheckGadget(app->titleGad, "title") || !CheckGadget(app->artistGad, "artist") ||
		!CheckGadget(app->albumGad, "album") || !CheckGadget(app->trackGad, "track") ||
		!CheckGadget(app->genreGad, "genre") || !CheckGadget(app->ratingGad, "rating") ||
		!CheckGadget(app->starGad[0], "star1") || !CheckGadget(app->starGad[1], "star2") ||
		!CheckGadget(app->starGad[2], "star3") || !CheckGadget(app->starGad[3], "star4") ||
		!CheckGadget(app->starGad[4], "star5") || !CheckGadget(app->artGad, "artwork placeholder"))
		return 0;

	root = (Object *)NewObject(LAYOUT_GetClass(), NULL,
		LAYOUT_Orientation, LAYOUT_ORIENT_VERT,
		LAYOUT_SpaceOuter, TRUE,
		LAYOUT_DeferLayout, TRUE,

		/* File chooser spans the full width at the top. */
		LAYOUT_AddChild, (ULONG)NewObject(LAYOUT_GetClass(), NULL,
			LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
			ADD_LABELLED(app->fileGad, "File"),
			TAG_DONE),
		CHILD_WeightedHeight, 0,

		/* Track metadata stacked on the left, artwork box on the right. */
		LAYOUT_AddChild, (ULONG)NewObject(LAYOUT_GetClass(), NULL,
			LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
			LAYOUT_SpaceInner, TRUE,

			LAYOUT_AddChild, (ULONG)NewObject(LAYOUT_GetClass(), NULL,
				LAYOUT_Orientation, LAYOUT_ORIENT_VERT,
				LAYOUT_BevelStyle, BVS_GROUP,
				LAYOUT_Label, (ULONG)"Track",
				LAYOUT_SpaceInner, TRUE,

				ADD_LABELLED(app->titleGad, "Title"),
				CHILD_WeightedHeight, 0,
				ADD_LABELLED(app->artistGad, "Artist"),
				CHILD_WeightedHeight, 0,
				ADD_LABELLED(app->albumGad, "Album"),
				CHILD_WeightedHeight, 0,
				ADD_LABELLED(app->genreGad, "Genre"),
				CHILD_WeightedHeight, 0,

				/* Compact rating row: read-out, five small stars, track no. */
				LAYOUT_AddChild, (ULONG)NewObject(LAYOUT_GetClass(), NULL,
					LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
					ADD_LABELLED(app->ratingGad, "Rating"),
					CHILD_MaxWidth, 44,
					CHILD_WeightedWidth, 0,
					LAYOUT_AddChild, (ULONG)app->starGad[0], CHILD_MaxWidth, 18, CHILD_WeightedWidth, 0,
					LAYOUT_AddChild, (ULONG)app->starGad[1], CHILD_MaxWidth, 18, CHILD_WeightedWidth, 0,
					LAYOUT_AddChild, (ULONG)app->starGad[2], CHILD_MaxWidth, 18, CHILD_WeightedWidth, 0,
					LAYOUT_AddChild, (ULONG)app->starGad[3], CHILD_MaxWidth, 18, CHILD_WeightedWidth, 0,
					LAYOUT_AddChild, (ULONG)app->starGad[4], CHILD_MaxWidth, 18, CHILD_WeightedWidth, 0,
					ADD_LABELLED(app->trackGad, "Track"),
					CHILD_MaxWidth, 60,
					TAG_DONE),
				CHILD_WeightedHeight, 0,
				TAG_DONE),

			LAYOUT_AddChild, (ULONG)app->artGad,
			CHILD_MinWidth, 72,
			CHILD_MinHeight, 72,
			CHILD_MaxWidth, 72,
			CHILD_WeightedWidth, 0,
			CHILD_WeightedHeight, 0,
			TAG_DONE),
		CHILD_WeightedHeight, 0,

		/* All decoder/playback controls grouped in one tidy block. */
		LAYOUT_AddChild, (ULONG)NewObject(LAYOUT_GetClass(), NULL,
			LAYOUT_Orientation, LAYOUT_ORIENT_VERT,
			LAYOUT_BevelStyle, BVS_GROUP,
			LAYOUT_Label, (ULONG)"Decoder",
			LAYOUT_SpaceInner, TRUE,

			LAYOUT_AddChild, (ULONG)NewObject(LAYOUT_GetClass(), NULL,
				LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
				LAYOUT_EvenSize, TRUE,
				/* Left-packed, size-to-content row, matching the layout the
				 * Rating/stars/Track row below already uses successfully:
				 * every item except the LAST gets CHILD_WeightedWidth 0 (no
				 * stretch), and the last item is left as the row's one
				 * flex child but capped with a tight CHILD_MaxWidth so it
				 * can't visibly stretch either -- giving every item in the
				 * row weight 0 made the layout fall back to dividing the
				 * row into equal shares again instead of packing left. */
				ADD_LABELLED(app->rateGad, "Rate"),
				CHILD_WeightedWidth, 0,
				ADD_LABELLED(app->qualityGad, "Quality"),
				CHILD_WeightedWidth, 0,
				ADD_LABELLED(app->subbandCapGad, "Subbands"),
				CHILD_WeightedWidth, 0,
				/* Widest label is "Stereo" -- this is the row's one flex
				 * child (see comment above), capped tight so it still
				 * reads as left-packed. */
				ADD_LABELLED(app->channelGad, "Mono/Stereo"),
				CHILD_WeightedWidth, 0,
				CHILD_MaxWidth, 100,
				TAG_DONE),
			CHILD_WeightedHeight, 0,

			LAYOUT_AddChild, (ULONG)NewObject(LAYOUT_GetClass(), NULL,
				LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
				/* Same left-packed treatment as the Rate row above: every
				 * item but the last is CHILD_WeightedWidth 0 with no
				 * explicit Min/Max, so it sizes to CHOOSER_GetClass's own
				 * natural width for its widest label -- exactly how Rate
				 * fits "28600 Hz" above. A forced CHILD_MinWidth was tried
				 * here to stop "22050 Mono Ultrafast" overflowing the
				 * gadget, but that only mattered while this row was still
				 * being stretched to an equal share of the row width
				 * (narrower than the chooser's own natural size); now that
				 * it isn't, the forced minimum was wider than the chooser
				 * actually needs and left dead space inside its own box --
				 * so drop it and let the gadget size itself, same as Rate. */
				ADD_LABELLED(app->speedGad, "Speed"),
				CHILD_WeightedWidth, 0,
				ADD_LABELLED(app->widthGad, "Mode/width"),
				CHILD_WeightedWidth, 0,
				/* Widest label is "192" -- a few pixels is plenty, instead
				 * of an equal share of the row. This is the row's one flex
				 * child (see comment above). */
				ADD_LABELLED(app->delayGad, "Delay"),
				CHILD_MaxWidth, 50,
				TAG_DONE),
			CHILD_WeightedHeight, 0,

			LAYOUT_AddChild, (ULONG)NewObject(LAYOUT_GetClass(), NULL,
				LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
				ADD_LABELLED(app->bufferGad, "Buffer"),
				ADD_LABELLED(app->volumeGad, "Volume"),
				TAG_DONE),
			CHILD_WeightedHeight, 0,

			LAYOUT_AddChild, (ULONG)NewObject(LAYOUT_GetClass(), NULL,
				LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
				LAYOUT_AddChild, (ULONG)app->fastMemGad,
				TAG_DONE),
			CHILD_WeightedHeight, 0,
			TAG_DONE),
		CHILD_WeightedHeight, 0,

		/* File info readout, with the track time pinned to the right. */
		LAYOUT_AddChild, (ULONG)NewObject(LAYOUT_GetClass(), NULL,
			LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
			ADD_LABELLED(app->fileInfoGad, "File info"),
			ADD_LABELLED(app->timeGad, "Time"),
			/* Wide enough for the full "00:00 / 00:00" read-out; the previous
			 * 96px clipped the elapsed half off the left. */
			CHILD_MinWidth, 120,
			CHILD_MaxWidth, 132,
			CHILD_WeightedWidth, 0,
			TAG_DONE),
		CHILD_WeightedHeight, 0,

		/* Seek controls flank the progress gauge: [<<] [====gauge====] [>>].
		 * Kept out of the even-size transport row below so they stay narrow
		 * and the button row does not grow past the screen edge. */
		LAYOUT_AddChild, (ULONG)NewObject(LAYOUT_GetClass(), NULL,
			LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
			LAYOUT_AddChild, (ULONG)app->rewGad,
			CHILD_MinWidth, 32, CHILD_MaxWidth, 40, CHILD_WeightedWidth, 0,
			LAYOUT_AddChild, (ULONG)app->gaugeGad,
			LAYOUT_AddChild, (ULONG)app->ffwdGad,
			CHILD_MinWidth, 32, CHILD_MaxWidth, 40, CHILD_WeightedWidth, 0,
			TAG_DONE),
		CHILD_WeightedHeight, 0,
		LAYOUT_AddChild, (ULONG)app->statusGad,
		CHILD_WeightedHeight, 0,

		LAYOUT_AddChild, (ULONG)NewObject(LAYOUT_GetClass(), NULL,
			LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
			LAYOUT_EvenSize, TRUE,
			LAYOUT_AddChild, (ULONG)app->playGad,
			LAYOUT_AddChild, (ULONG)app->nextGad,
			LAYOUT_AddChild, (ULONG)app->stopGad,
			LAYOUT_AddChild, (ULONG)app->filterGad,
			LAYOUT_AddChild, (ULONG)app->playlistGad,
			LAYOUT_AddChild, (ULONG)app->radioGad,
			TAG_DONE),
		CHILD_WeightedHeight, 0,
		TAG_DONE);

        if (!root) {
		fprintf(stderr, "MintAMP: could not build the gadget layout.\n");
		return 0;
	}

	MrInitMenuStrip();
	app->menuStrip = &kMenus[0];
	SyncMenuChecks(app);
	if (IconBase)
		windowIcon = GetDiskObject((STRPTR)"PROGDIR:MintAMP");
	app->winObj = (Object *)NewObject(WINDOW_GetClass(), NULL,
		WA_Title, (ULONG)MR_WINDOW_TITLE,
		WA_ScreenTitle, (ULONG)MR_WINDOW_TITLE,
		WA_Activate, TRUE,
		WA_DepthGadget, TRUE,
		WA_DragBar, TRUE,
		WA_CloseGadget, TRUE,
		WA_SizeGadget, TRUE,
		WINDOW_AppPort, (ULONG)app->appPort,
		WINDOW_IconifyGadget, TRUE,
		WINDOW_IconTitle, (ULONG)MR_WINDOW_TITLE,
		windowIcon ? WINDOW_Icon : TAG_IGNORE, (ULONG)windowIcon,
		WINDOW_MenuStrip, (ULONG)app->menuStrip,
		/* SMART_REFRESH so Intuition preserves the hand-drawn artwork panel when
		 * another window (the Internet Radio browser especially) overlaps it.
		 * Without this the window is simple-refresh: overlapping deletes the art
		 * and it never comes back, because RA_HandleInput consumes
		 * IDCMP_REFRESHWINDOW itself and nothing repaints the custom drawing.
		 * The GadTools frontend already uses WFLG_SMART_REFRESH for the same
		 * reason. Costs a per-window backing store but no CPU on the hot path. */
		WA_SmartRefresh, TRUE,
		WA_IDCMP, IDCMP_GADGETUP | IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW |
			IDCMP_IDCMPUPDATE | IDCMP_MENUPICK | IDCMP_NEWSIZE | IDCMP_MOUSEMOVE,
		WA_NewLookMenus, TRUE,
		/* Match the GadTools frontend's larger default footprint now that
		 * ReAction window sizing is stable, so long metadata and file-info
		 * strings get the same breathing room. */
		WA_Width, 560,
		WA_Height, 340,
		WINDOW_Position, WPOS_CENTERSCREEN,
		WINDOW_ParentGroup, (ULONG)root,
                TAG_DONE);

        if (!app->winObj) {
		fprintf(stderr, "MintAMP: could not create the window object.\n");
		return 0;
	}

#ifdef MINTAMP_SKIN
	/* The BOOPSI tree retains settings while closed. Do not open its native
	 * window merely to close it again once the skin is visible. */
	if (gSkinPlayer) {
		MrSetGadgetAttr(app->bufferGad,NULL,GA_Disabled,(ULONG)app->decodeThenPlay);
		if (!app->hasNetwork) {
			struct MenuItem *item=ItemAddress(app->menuStrip,
				FULLMENUNUM(MENUNUM_PROJECT,ITEMNUM_RADIO,NOSUB));
			if (item) item->Flags &= ~ITEMENABLED;
		}
		return 1;
	}
#endif
	app->win = (struct Window *)RA_OpenWindow(app->winObj);
	if (!app->win) {
		fprintf(stderr, "MintAMP: could not open the window.\n");
		return 0;
	}
	if (app->bufferGad)
		SetGadgetAttrs((struct Gadget *)app->bufferGad, app->win, NULL,
			GA_Disabled, app->decodeThenPlay,
			TAG_DONE);
	if (!app->hasNetwork)
		OffMenu(app->win, FULLMENUNUM(MENUNUM_PROJECT, ITEMNUM_RADIO, NOSUB));
	return 1;
}

static void MrCloseWindow(MrApp *app)
{
#ifdef MINTAMP_SKIN
	/* Also release the player if creating the native settings tree failed. */
	skin_player_close(gSkinPlayer); gSkinPlayer=NULL;
#endif
	RADIO_DBG(printf("app-close: MrCloseWindow enter rbWin=%p plWin=%p menuStrip=%p winObj=%p\n",
		app->rbWin, app->plWin, app->menuStrip, app->winObj);)
	CloseRadioWindow(app);
	RADIO_DBG(printf("app-close: CloseRadioWindow done\n");)
	ClosePlaylistWindow(app);
	RADIO_DBG(printf("app-close: ClosePlaylistWindow done\n");)
	if (app->winObj) {
		RADIO_DBG(printf("app-dispose: before DisposeObject mainWinObj=%p\n", app->winObj);)
		DisposeObject(app->winObj);	/* disposes the whole gadget tree too */
		app->winObj = NULL;
		app->win = NULL;
		app->fileGad = app->rateGad = app->qualityGad = app->subbandCapGad = app->channelGad = NULL;
		app->volumeGad = app->bufferGad = app->fastMemGad = NULL;
		app->speedGad = app->widthGad = app->delayGad = app->playGad = NULL;
		app->nextGad = app->stopGad = app->filterGad = app->playlistGad = NULL;
		app->radioGad = app->timeGad = app->fileInfoGad = app->titleGad = NULL;
		app->artistGad = app->albumGad = app->trackGad = app->genreGad = NULL;
		app->ratingGad = app->gaugeGad = app->statusGad = app->artGad = NULL;
		memset(app->starGad, 0, sizeof(app->starGad));
		RADIO_DBG(printf("app-dispose: after DisposeObject mainWinObj\n");)
		Radio_CheckMiniMem("after DisposeObject mainWinObj");
	}
	app->menuStrip = NULL;
	RADIO_DBG(printf("app-close: MrCloseWindow exit\n");)
}


typedef struct MrMp3Info {
	char title[64], artist[64], album[64], track[16], genre[32];
	int bitrateKbps, sampleRate, channels, channelMode, modeExtension, durationSecs, rating;
	unsigned long fileSize;
	unsigned char *artData;
	unsigned long artBytes;
	int artIsPng;
} MrMp3Info;

static void CopyTrim(char *dst, size_t size, const unsigned char *src, int n)
{
	int end = n;
	while (end > 0 && (src[end - 1] == ' ' || src[end - 1] == 0)) end--;
	if ((size_t)end >= size) end = (int)size - 1;
	memcpy(dst, src, end); dst[end] = 0;
}


static int Id3Synchsafe(const unsigned char *p)
{
	return ((p[0] & 0x7f) << 21) | ((p[1] & 0x7f) << 14) | ((p[2] & 0x7f) << 7) | (p[3] & 0x7f);
}

static long Id3BigEndian32(const unsigned char *p)
{
	return ((long)p[0] << 24) | ((long)p[1] << 16) | ((long)p[2] << 8) | (long)p[3];
}

static void CopyId3Text(char *dst, size_t size, const unsigned char *p, long n)
{
	unsigned char enc;
	long i, out;
	int bigEndian;

	if (!dst || size == 0) return;
	dst[0] = 0;
	if (!p || n <= 0) return;

	enc = p[0];
	p++;
	n--;
	if (enc == 0 || enc == 3) {
		CopyTrim(dst, size, p, (int)n);
		return;
	}
	if (enc != 1 && enc != 2) {
		p--;
		n++;
		CopyTrim(dst, size, p, (int)n);
		return;
	}

	bigEndian = (enc == 2);
	if (n >= 2 && p[0] == 0xfe && p[1] == 0xff) {
		bigEndian = 1;
		p += 2;
		n -= 2;
	} else if (n >= 2 && p[0] == 0xff && p[1] == 0xfe) {
		bigEndian = 0;
		p += 2;
		n -= 2;
	}

	out = 0;
	for (i = 0; i + 1 < n && out + 1 < (long)size; i += 2) {
		unsigned int cp = bigEndian ?
			(((unsigned int)p[i] << 8) | p[i + 1]) :
			(((unsigned int)p[i + 1] << 8) | p[i]);

		if (cp == 0)
			break;
		if (cp >= 0xd800 && cp <= 0xdbff) {
			if (i + 3 < n) {
				unsigned int lo = bigEndian ?
					(((unsigned int)p[i + 2] << 8) | p[i + 3]) :
					(((unsigned int)p[i + 3] << 8) | p[i + 2]);
				if (lo >= 0xdc00 && lo <= 0xdfff)
					i += 2;
			}
			dst[out++] = '?';
		} else if (cp >= 0xdc00 && cp <= 0xdfff) {
			dst[out++] = '?';
		} else if (cp < 0x20 || cp == 0x7f) {
			dst[out++] = '?';
		} else if (cp <= 0x00ff) {
			dst[out++] = (char)(cp & 0xff);
		} else {
			dst[out++] = '?';
		}
	}
	dst[out] = 0;
	while (out > 0 && (dst[out - 1] == ' ' || dst[out - 1] == 0))
		dst[--out] = 0;
}

static int RatingFromPopmByte(unsigned char b)
{
	if (b == 0) return 0;
	if (b < 32) return 1;
	if (b < 96) return 2;
	if (b < 160) return 3;
	if (b < 224) return 4;
	return 5;
}

static int ContainsTextNoCase(const char *s, const char *needle)
{
	int i, j;
	if (!s || !needle || !needle[0]) return 0;
	for (i = 0; s[i]; i++) {
		for (j = 0; needle[j]; j++) {
			char a = s[i + j], b = needle[j];
			if (!a) return 0;
			if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
			if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
			if (a != b) break;
		}
		if (!needle[j]) return 1;
	}
	return 0;
}

static unsigned long ApicImageOffset(const unsigned char *p, unsigned long n)
{
	unsigned long i = 1;
	if (!p || n < 4) return n;
	while (i < n && p[i]) i++; /* MIME */
	if (i < n) i++;
	if (i < n) i++;          /* picture type */
	while (i < n && p[i]) i++; /* description (ISO-8859-1/UTF-8 common case) */
	if (i < n) i++;
	return i;
}

static unsigned long PicImageOffset(const unsigned char *p, unsigned long n)
{
	unsigned long i = 5;
	if (!p || n < 6) return n;
	while (i < n && p[i]) i++;
	if (i < n) i++;
	return i;
}

static void DetectPictureMime(const unsigned char *payload,
	unsigned long payloadBytes, int version, int *isJpeg, int *isPng)
{
	char mime[40];
	unsigned long i;
	*isJpeg = 0; *isPng = 0;
	if (!payload || payloadBytes < 4) return;
	memset(mime, 0, sizeof(mime));
	if (version == 2) {
		for (i = 0; i < 3 && i + 1 < payloadBytes; i++) mime[i] = (char)payload[i + 1];
	} else {
		for (i = 1; i < payloadBytes && i < sizeof(mime); i++) {
			if (!payload[i]) break;
			mime[i - 1] = (char)payload[i];
		}
	}
	if (ContainsTextNoCase(mime, "jpeg") || ContainsTextNoCase(mime, "jpg")) *isJpeg = 1;
	else if (ContainsTextNoCase(mime, "png")) *isPng = 1;
}

static void FreeMp3Info(MrMp3Info *info)
{
	if (info && info->artData) {
		/* info->artData is AllocMem()'d by ReadId3v2()'s APIC branch or by
		 * TryFolderArt(); it lives only on this stack-local MrMp3Info, is
		 * never published to another task, and is decoded (not adopted) by
		 * UpdateArtwork().  So the single owner is this struct and the single
		 * free is here -- clearing artData/artBytes immediately below makes a
		 * repeat call a proven no-op instead of a stale double free. */
		MR_FREE_BEGIN("FreeMp3Info", "id3/folder-artData", info->artData, info->artBytes);
		FreeMem(info->artData, info->artBytes);
		info->artData = NULL;
		info->artBytes = 0;
		MR_FREE_END("FreeMp3Info", "id3/folder-artData", info->artData, 0);
	}
}

static void ReadId3v2(FILE *f, MrMp3Info *info)
{
	unsigned char hdr[10], fh[10]; long end; int version;
	if (fseek(f, 0, SEEK_SET) != 0 || fread(hdr, 1, 10, f) != 10 || memcmp(hdr, "ID3", 3)) {
		/* No ID3v2 tag: leave the file rewound so the MPEG scan starts at
		 * the top of the file. */
		fseek(f, 0, SEEK_SET);
		return;
	}
	version = hdr[3];
	end = 10 + Id3Synchsafe(hdr + 6);
	while (ftell(f) + 10 <= end && fread(fh, 1, 10, f) == 10) {
		long sz = version == 4 ? Id3Synchsafe(fh + 4) : Id3BigEndian32(fh + 4); unsigned char *payload;
		if (fh[0] == 0 || sz <= 0 || ftell(f) + sz > end) break;
		payload = (unsigned char *)malloc((size_t)sz);
		if (!payload) { fseek(f, sz, SEEK_CUR); continue; }
		if (fread(payload, 1, (size_t)sz, f) != (size_t)sz) { free(payload); break; }
		if (!memcmp(fh, "TIT2", 4) && !info->title[0]) CopyId3Text(info->title, sizeof(info->title), payload, sz);
		else if (!memcmp(fh, "TPE1", 4) && !info->artist[0]) CopyId3Text(info->artist, sizeof(info->artist), payload, sz);
		else if (!memcmp(fh, "TALB", 4) && !info->album[0]) CopyId3Text(info->album, sizeof(info->album), payload, sz);
		else if (!memcmp(fh, "TRCK", 4) && !info->track[0]) CopyId3Text(info->track, sizeof(info->track), payload, sz);
		else if (!memcmp(fh, "TCON", 4) && !info->genre[0]) CopyId3Text(info->genre, sizeof(info->genre), payload, sz);
		else if (!memcmp(fh, "POPM", 4)) { long i; for (i = 0; i + 1 < sz; i++) if (payload[i] == 0) { info->rating = RatingFromPopmByte(payload[i + 1]); break; } }
		else if (!info->artData && (!memcmp(fh, "APIC", 4)) && sz > 4 && sz <= 512L * 1024L) {
			unsigned long off, bytes; int isJpeg, isPng;
			DetectPictureMime(payload, (unsigned long)sz, version, &isJpeg, &isPng);
			off = ApicImageOffset(payload, (unsigned long)sz);
			bytes = (unsigned long)sz - off;
			if (off < (unsigned long)sz && bytes > 4) {
				info->artData = (unsigned char *)AllocMem(bytes, MEMF_ANY);
				if (info->artData) {
					memcpy(info->artData, payload + off, bytes);
					info->artBytes = bytes;
					info->artIsPng = isPng || (!isJpeg && !isPng);
				}
			}
		}
		free(payload);
	}
	/* Position the file pointer just past the ID3v2 tag so the MPEG frame
	 * scan that follows does not mistake bytes inside the tag (e.g. an
	 * embedded JPEG's FFE0/FFD8 markers) for an MPEG frame sync. */
	fseek(f, end, SEEK_SET);
}

static void ReadId3v1(FILE *f, MrMp3Info *info)
{
	unsigned char b[128];
	static const char *genres[] = { "Blues","Classic Rock","Country","Dance","Disco","Funk","Grunge","Hip-Hop","Jazz","Metal","New Age","Oldies","Other","Pop","R&B","Rap","Reggae","Rock" };
	if (fseek(f, -128, SEEK_END) != 0 || fread(b, 1, 128, f) != 128 || memcmp(b, "TAG", 3)) return;
	CopyTrim(info->title, sizeof(info->title), b + 3, 30);
	CopyTrim(info->artist, sizeof(info->artist), b + 33, 30);
	CopyTrim(info->album, sizeof(info->album), b + 63, 30);
	if (b[125] == 0 && b[126] != 0) sprintf(info->track, "%u", (unsigned)b[126]);
	if (b[127] < sizeof(genres) / sizeof(genres[0])) SafeCopy(info->genre, sizeof(info->genre), genres[b[127]]);
}

/* Strict MPEG-1/2 Layer III frame-sync test, matching the GadTools frontend.
 * The loose "(h[1] & 0xe0) == 0xe0" test used previously matched JPEG APPn
 * markers (0xFFEn) and Layer I/II frames, so embedded cover art or a stray
 * byte run produced bogus bitrate/sample-rate readouts. */
static int IsMpegSyncHeaderMr(const unsigned char *h)
{
	return h[0] == 0xff && (h[1] == 0xfb || h[1] == 0xfa ||
		h[1] == 0xf3 || h[1] == 0xf2 || h[1] == 0xe3 || h[1] == 0xe2);
}

static void ReadMpegHeader(FILE *f, MrMp3Info *info, long *firstFrame)
{
	static const int br[16] = {0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0};
	static const int sr[4] = {44100,48000,32000,0};
	unsigned char h[4];
	int b;
	if (firstFrame) *firstFrame = -1;
	if (!f || !info) return;
	/* Scan forward from the current position (already advanced past any
	 * ID3v2 tag by ReadId3v2) using a 4-byte sliding window. */
	h[0] = h[1] = h[2] = h[3] = 0;
	while ((b = fgetc(f)) != EOF) {
		h[0] = h[1]; h[1] = h[2]; h[2] = h[3]; h[3] = (unsigned char)b;
		if (IsMpegSyncHeaderMr(h)) {
			long pos = ftell(f);
			int bi = (h[2] >> 4) & 15, si = (h[2] >> 2) & 3;
			if (firstFrame && pos >= 4) *firstFrame = pos - 4;
			info->bitrateKbps = br[bi];
			info->sampleRate = sr[si];
			info->channelMode = (h[3] >> 6) & 3;
			info->modeExtension = (h[3] >> 4) & 3;
			info->channels = info->channelMode == 3 ? 1 : 2;
			return;
		}
	}
}

static void TryFolderArt(const char *inputName, MrMp3Info *info)
{
	static const char *names[] = { "folder.jpg", "cover.jpg", "album.jpg", "front.jpg", NULL };
	char dirPath[MR_MAX_PATH], artPath[MR_MAX_PATH];
	int i;
	if (!inputName || !info || info->artData) return;
	SafeCopy(dirPath, sizeof(dirPath), inputName);
	{
		char *q = dirPath + strlen(dirPath);
		while (q > dirPath && *q != '/' && *q != ':') q--;
		if (*q == '/' || *q == ':') *(q + 1) = '\0';
		else dirPath[0] = '\0';
	}
	for (i = 0; names[i] && !info->artData; i++) {
		FILE *af;
		SafeCopy(artPath, sizeof(artPath), dirPath);
		strncat(artPath, names[i], sizeof(artPath) - strlen(artPath) - 1);
		af = fopen(artPath, "rb");
		if (af) {
			long sz;
			fseek(af, 0, SEEK_END); sz = ftell(af); fseek(af, 0, SEEK_SET);
			if (sz > 4 && sz <= 512L * 1024L) {
				info->artData = (unsigned char *)AllocMem((unsigned long)sz, MEMF_ANY);
				if (info->artData) {
					if (fread(info->artData, 1, (size_t)sz, af) == (size_t)sz) {
						info->artBytes = (unsigned long)sz;
						info->artIsPng = 0;
					} else {
						/* Short read: free the just-allocated buffer and
						 * clear artData so neither the loop's own
						 * !info->artData guard nor a later FreeMp3Info() can
						 * see (and re-free) this now-dead pointer. */
						MR_FREE_BEGIN("TryFolderArt", "folder-artData(short-read)", info->artData, (unsigned long)sz);
						FreeMem(info->artData, (unsigned long)sz);
						info->artData = NULL;
						MR_FREE_END("TryFolderArt", "folder-artData(short-read)", info->artData, 0);
					}
				}
			}
			fclose(af);
		}
	}
}

static void ReadMp3Info(const char *path, MrMp3Info *info)
{
	FILE *f; long first = -1, sz;
	memset(info, 0, sizeof(*info));
	f = fopen(path, "rb"); if (!f) return;
	ReadId3v2(f, info);
	ReadMpegHeader(f, info, &first);
	if (fseek(f, 0, SEEK_END) == 0) { sz = ftell(f); if (sz > 0) info->fileSize = (unsigned long)sz; }
	if (info->bitrateKbps > 0 && first >= 0 && info->fileSize > (unsigned long)first)
		info->durationSecs = (int)(((info->fileSize - (unsigned long)first) * 8UL) / ((unsigned long)info->bitrateKbps * 1000UL));
	ReadId3v1(f, info);
	fclose(f);
	TryFolderArt(path, info);
}

typedef struct MrPjpegSrc {
	const unsigned char *data;
	unsigned long pos;
	unsigned long size;
} MrPjpegSrc;

static unsigned char MrPjpegCb(unsigned char *buf, unsigned char bufSize,
	unsigned char *bytesRead, void *ud)
{
	MrPjpegSrc *src = (MrPjpegSrc *)ud;
	unsigned long left = src->size - src->pos;
	unsigned char n = (unsigned char)(left < (unsigned long)bufSize ? left : (unsigned long)bufSize);
	if (n) {
		memcpy(buf, src->data + src->pos, n);
		src->pos += n;
	}
	*bytesRead = n;
	return 0;
}

static int MrJpegGreySample(const pjpeg_image_info_t *info, int off)
{
	unsigned long r, g, b;
	if (info->m_comps == 1) return info->m_pMCUBufR[off];
	r = info->m_pMCUBufR[off]; g = info->m_pMCUBufG[off]; b = info->m_pMCUBufB[off];
	return (int)((77UL * r + 150UL * g + 29UL * b + 128UL) >> 8);
}

static int MrJpegRgbSample(const pjpeg_image_info_t *info, int off,
	unsigned char *r, unsigned char *g, unsigned char *b)
{
	if (info->m_comps == 1) {
		*r = *g = *b = info->m_pMCUBufR[off];
		return (int)*r;
	}
	*r = info->m_pMCUBufR[off];
	*g = info->m_pMCUBufG[off];
	*b = info->m_pMCUBufB[off];
	return MrJpegGreySample(info, off);
}

static int MrMcuSampleOffset(const pjpeg_image_info_t *info, int x, int y)
{
	int blockX = x / 8, blockY = y / 8, blocksPerRow = info->m_MCUWidth / 8;
	return (blockY * blocksPerRow + blockX) * 64 + (y & 7) * 8 + (x & 7);
}

static const char *PjpegStatusName(int status)
{
	switch (status) {
	case 0: return "OK";
	case PJPG_NO_MORE_BLOCKS: return "NO_MORE_BLOCKS";
	case PJPG_BAD_DHT_COUNTS: return "BAD_DHT_COUNTS";
	case PJPG_BAD_DHT_INDEX: return "BAD_DHT_INDEX";
	case PJPG_BAD_DHT_MARKER: return "BAD_DHT_MARKER";
	case PJPG_BAD_DQT_MARKER: return "BAD_DQT_MARKER";
	case PJPG_BAD_DQT_TABLE: return "BAD_DQT_TABLE";
	case PJPG_BAD_PRECISION: return "BAD_PRECISION";
	case PJPG_BAD_HEIGHT: return "BAD_HEIGHT";
	case PJPG_BAD_WIDTH: return "BAD_WIDTH";
	case PJPG_TOO_MANY_COMPONENTS: return "TOO_MANY_COMPONENTS";
	case PJPG_BAD_SOF_LENGTH: return "BAD_SOF_LENGTH";
	case PJPG_BAD_VARIABLE_MARKER: return "BAD_VARIABLE_MARKER";
	case PJPG_BAD_DRI_LENGTH: return "BAD_DRI_LENGTH";
	case PJPG_BAD_SOS_LENGTH: return "BAD_SOS_LENGTH";
	case PJPG_BAD_SOS_COMP_ID: return "BAD_SOS_COMP_ID";
	case PJPG_W_EXTRA_BYTES_BEFORE_MARKER: return "EXTRA_BYTES_BEFORE_MARKER";
	case PJPG_NO_ARITHMITIC_SUPPORT: return "NO_ARITHMETIC_SUPPORT";
	case PJPG_UNEXPECTED_MARKER: return "UNEXPECTED_MARKER";
	case PJPG_NOT_JPEG: return "NOT_JPEG";
	case PJPG_UNSUPPORTED_MARKER: return "UNSUPPORTED_MARKER";
	case PJPG_BAD_DQT_LENGTH: return "BAD_DQT_LENGTH";
	case PJPG_TOO_MANY_BLOCKS: return "TOO_MANY_BLOCKS";
	case PJPG_UNDEFINED_QUANT_TABLE: return "UNDEFINED_QUANT_TABLE";
	case PJPG_UNDEFINED_HUFF_TABLE: return "UNDEFINED_HUFF_TABLE";
	case PJPG_NOT_SINGLE_SCAN: return "NOT_SINGLE_SCAN (progressive JPEG unsupported)";
	case PJPG_UNSUPPORTED_COLORSPACE: return "UNSUPPORTED_COLORSPACE";
	case PJPG_UNSUPPORTED_SAMP_FACTORS: return "UNSUPPORTED_SAMP_FACTORS";
	case PJPG_DECODE_ERROR: return "DECODE_ERROR";
	case PJPG_BAD_RESTART_MARKER: return "BAD_RESTART_MARKER";
	case PJPG_ASSERTION_ERROR: return "ASSERTION_ERROR";
	case PJPG_BAD_SOS_SPECTRAL: return "BAD_SOS_SPECTRAL";
	case PJPG_BAD_SOS_SUCCESSIVE: return "BAD_SOS_SUCCESSIVE";
	case PJPG_STREAM_READ_ERROR: return "STREAM_READ_ERROR";
	case PJPG_NOTENOUGHMEM: return "NOTENOUGHMEM";
	case PJPG_UNSUPPORTED_COMP_IDENT: return "UNSUPPORTED_COMP_IDENT";
	case PJPG_UNSUPPORTED_QUANT_TABLE: return "UNSUPPORTED_QUANT_TABLE";
	case PJPG_UNSUPPORTED_MODE: return "UNSUPPORTED_MODE";
	default: return "UNKNOWN";
	}
}


static int MrJpegHasSof2(const unsigned char *data, unsigned long bytes)
{
	unsigned long p = 2;
	if (!data || bytes < 4 || data[0] != 0xff || data[1] != 0xd8) return 0;
	while (p + 3 < bytes) {
		unsigned int m, len;
		while (p < bytes && data[p] != 0xff) p++;
		while (p < bytes && data[p] == 0xff) p++;
		if (p >= bytes) break;
		m = data[p++];
		if (m == 0xc2) return 1;
		if (m == 0xd9 || m == 0xda) break;
		if (m >= 0xd0 && m <= 0xd7) continue;
		if (p + 1 >= bytes) break;
		len = ((unsigned int)data[p] << 8) | data[p + 1];
		if (len < 2 || p + len > bytes) break;
		p += len;
	}
	return 0;
}

typedef struct MrJpegHuffTab {
	unsigned char valid;
	unsigned char size[256];
	unsigned short code[256];
	unsigned char sym[256];
	int count;
} MrJpegHuffTab;

typedef struct MrProgComp {
	unsigned char id, h, v, tq, td;
	int bw, bh, prevDc;
	unsigned char haveDc;
} MrProgComp;

typedef struct MrProgBits {
	const unsigned char *p, *end;
	unsigned int bits;
	int bitcnt;
} MrProgBits;

static int MrJpegReadBits(MrProgBits *br, int n)
{
	int v = 0;
	while (n-- > 0) {
		if (br->bitcnt <= 0) {
			int c;
			if (br->p >= br->end) return -1;
			c = *br->p++;
			if (c == 0xff) {
				if (br->p < br->end && *br->p == 0x00) br->p++;
				else return -1;
			}
			br->bits = (unsigned int)c;
			br->bitcnt = 8;
		}
		v = (v << 1) | (int)((br->bits >> 7) & 1);
		br->bits <<= 1;
		br->bitcnt--;
	}
	return v;
}

static int MrJpegDecodeHuff(MrProgBits *br, const MrJpegHuffTab *ht)
{
	unsigned short code = 0;
	int len, i;
	if (!ht || !ht->valid) return -1;
	for (len = 1; len <= 16; len++) {
		int b = MrJpegReadBits(br, 1);
		if (b < 0) return -1;
		code = (unsigned short)((code << 1) | b);
		for (i = 0; i < ht->count; i++)
			if (ht->size[i] == len && ht->code[i] == code)
				return ht->sym[i];
	}
	return -1;
}

static int MrJpegReceiveExtend(MrProgBits *br, int s)
{
	int v;
	if (s == 0) return 0;
	if (s < 0 || s > 11) return -40999;
	v = MrJpegReadBits(br, s);
	if (v < 0) return -40999;
	if (v < (1 << (s - 1))) v += ((-1) << s) + 1;
	return v;
}

static int MrJpegFindEntropyEnd(const unsigned char *p, const unsigned char *end)
{
	const unsigned char *s = p;
	while (s + 1 < end) {
		if (s[0] == 0xff && s[1] != 0x00) {
			if (s[1] >= 0xd0 && s[1] <= 0xd7) { s += 2; continue; }
			break;
		}
		s++;
	}
	return (int)(s - p);
}

static int DecodeProgressiveJpegDcPreviewToGrey(const unsigned char *jpg, unsigned long bytes,
	unsigned char *greyOut, unsigned char *rgbOut, int outW, int outH)
{
	static short dc[4][(MR_MAX_JPEG_DIM / 8) * (MR_MAX_JPEG_DIM / 8)];
	static unsigned long greyAccum[MR_ART_W * MR_ART_H], rAccum[MR_ART_W * MR_ART_H], gAccum[MR_ART_W * MR_ART_H], bAccum[MR_ART_W * MR_ART_H];
	static unsigned short greyCount[MR_ART_W * MR_ART_H];
	MrJpegHuffTab hdc[4]; MrProgComp comp[4];
	unsigned short qdc[4];
	unsigned long p = 2; int width = 0, height = 0, comps = 0, maxh = 1, maxv = 1, sof2 = 0, anyDc = 0, anyDcScan = 0, lumaDcReady = 0, laterScanFailed = 0, i;
	const char *failReason = NULL;
	memset(hdc, 0, sizeof(hdc)); memset(qdc, 0, sizeof(qdc)); memset(comp, 0, sizeof(comp)); memset(dc, 0, sizeof(dc));
	if (!jpg || bytes < 4 || !greyOut || outW <= 0 || outW > MR_ART_W || outH <= 0 || outH > MR_ART_H || jpg[0] != 0xff || jpg[1] != 0xd8) { failReason = "bad entropy decode"; goto fail; }
	while (p + 3 < bytes) {
		unsigned int m, len;
		while (p < bytes && jpg[p] != 0xff) p++;
		while (p < bytes && jpg[p] == 0xff) p++;
		if (p >= bytes) break;
		m = jpg[p++];
		if (m == 0xd9) break;
		if (m >= 0xd0 && m <= 0xd7) continue;
		if (p + 1 >= bytes) { failReason = "bad entropy decode"; goto fail; }
		len = ((unsigned int)jpg[p] << 8) | jpg[p + 1];
		if (len < 2 || p + len > bytes) { failReason = "bad entropy decode"; goto fail; }
		if (m == 0xdb) {
			unsigned long e = p + len; p += 2;
			while (p < e) { unsigned int t = jpg[p++]; if (t >> 4) { failReason = "unsupported quant precision"; goto fail; } if ((t & 15) > 3 || p + 64 > e) { failReason = "bad entropy decode"; goto fail; } qdc[t & 15] = jpg[p]; p += 64; }
			continue;
		}
		if (m == 0xc2) {
			unsigned long s = p + 2; int c;
			sof2 = 1; if (len < 8 || jpg[s] != 8) { failReason = (len >= 8 && jpg[s] != 8) ? "unsupported quant precision" : "bad entropy decode"; goto fail; }
			height = ((int)jpg[s+1] << 8) | jpg[s+2]; width = ((int)jpg[s+3] << 8) | jpg[s+4]; comps = jpg[s+5];
			RADIO_DBG(printf("radio-art: SOF2 width=%d height=%d components=%d\n", width, height, comps);)
			if (width <= 0 || height <= 0 || width > MR_MAX_JPEG_DIM || height > MR_MAX_JPEG_DIM || (comps != 1 && comps != 3 && comps != 4) || len != (unsigned)(8 + comps * 3)) { failReason = "bad entropy decode"; goto fail; }
			for (c = 0, s += 6; c < comps; c++, s += 3) { comp[c].id = jpg[s]; comp[c].h = jpg[s+1] >> 4; comp[c].v = jpg[s+1] & 15; comp[c].tq = jpg[s+2]; RADIO_DBG(printf("radio-art: SOF2 component id=%u h=%u v=%u tq=%u\n", comp[c].id, comp[c].h, comp[c].v, comp[c].tq);) if (!comp[c].h || !comp[c].v || comp[c].tq > 3) { failReason = "bad entropy decode"; goto fail; } if (comp[c].h > maxh) maxh = comp[c].h; if (comp[c].v > maxv) maxv = comp[c].v; }
			for (c = 0; c < comps; c++) { comp[c].bw = (width * comp[c].h + maxh * 8 - 1) / (maxh * 8); comp[c].bh = (height * comp[c].v + maxv * 8 - 1) / (maxv * 8); if (comp[c].bw * comp[c].bh > (MR_MAX_JPEG_DIM / 8) * (MR_MAX_JPEG_DIM / 8)) { failReason = "bad entropy decode"; goto fail; } }
		} else if (m == 0xc4) {
			unsigned long e = p + len; p += 2;
			while (p < e) { unsigned int tc, th, n = 0, k; unsigned short code = 0; const unsigned char *counts; if (p + 17 > e) { failReason = "bad entropy decode"; goto fail; } tc = jpg[p] >> 4; th = jpg[p] & 15; p++; counts = jpg + p; for (i = 0; i < 16; i++) n += jpg[p+i]; RADIO_DBG(printf("radio-art: DHT class=%u table=%u symbols=%u\n", tc, th, n);) if (th > 3 || tc > 1 || p + 16 + n > e || n > 256) { failReason = "bad entropy decode"; goto fail; } p += 16; if (tc == 1) { RADIO_DBG(printf("radio-art: skipped AC DHT table=%u symbols=%u\n", th, n);) p += n; continue; } hdc[th].count = 0; for (i = 1; i <= 16; i++) { int cnt = counts[i - 1]; for (k = 0; k < (unsigned)cnt; k++) { int idx = hdc[th].count++; hdc[th].size[idx] = i; hdc[th].code[idx] = code++; hdc[th].sym[idx] = jpg[p++]; } code <<= 1; } hdc[th].valid = 1; }
			continue;
		} else if (m == 0xda) {
			unsigned long s = p + 2; int ns, Ss, Se, Ah, Al, scanComp[4], scanOk = 1, scanHasLuma = 0, c, mi, mx, my, by, bx; const unsigned char *ep; MrProgBits br;
			if (!sof2 || len < 6) { failReason = !sof2 ? "no SOF2" : "bad entropy decode"; goto fail; } ns = jpg[s++]; if (ns < 1 || ns > comps || len != (unsigned)(6 + ns * 2)) { failReason = "bad entropy decode"; goto fail; }
			for (i = 0; i < ns; i++, s += 2) { for (c = 0; c < comps && comp[c].id != jpg[s]; c++); if (c >= comps || (jpg[s+1] & 15)) { failReason = "bad entropy decode"; goto fail; } comp[c].td = jpg[s+1] >> 4; scanComp[i] = c; if (c == 0) scanHasLuma = 1; RADIO_DBG(printf("radio-art: SOS component id=%u td=%u\n", comp[c].id, comp[c].td);) }
			Ss = jpg[s++]; Se = jpg[s++]; Ah = jpg[s] >> 4; Al = jpg[s] & 15;
			RADIO_DBG(printf("radio-art: SOS ns=%d Ss=%d Se=%d Ah=%d Al=%d\n", ns, Ss, Se, Ah, Al);)
			ep = jpg + p + len; i = MrJpegFindEntropyEnd(ep, jpg + bytes); br.p = ep; br.end = ep + i; br.bits = 0; br.bitcnt = 0; p += len + i;
			if (Ss == 0 && Se == 0 && Ah == 0) {
				anyDcScan = 1;
				mx = (width + maxh * 8 - 1) / (maxh * 8); my = (height + maxv * 8 - 1) / (maxv * 8);
				for (mi = 0; scanOk && mi < mx * my; mi++) for (i = 0; scanOk && i < ns; i++) { c = scanComp[i]; for (by = 0; scanOk && by < comp[c].v; by++) for (bx = 0; bx < comp[c].h; bx++) { int x = (mi % mx) * comp[c].h + bx, y = (mi / mx) * comp[c].v + by, sym, diff; if (x >= comp[c].bw || y >= comp[c].bh) continue; if (!hdc[comp[c].td].valid) { failReason = "no DC Huffman table"; scanOk = 0; break; } sym = MrJpegDecodeHuff(&br, &hdc[comp[c].td]); diff = MrJpegReceiveExtend(&br, sym); if (sym < 0 || diff == -40999) { failReason = "bad entropy decode"; scanOk = 0; break; } comp[c].prevDc += diff; dc[c][y * comp[c].bw + x] = (short)(comp[c].prevDc << Al); comp[c].haveDc = 1; anyDc = 1; } }
				if (scanHasLuma && comp[0].haveDc && !lumaDcReady) { lumaDcReady = 1; RADIO_DBG(printf("radio-art: progressive JPEG luma DC ready\n");) }
				if (!scanOk) {
					if (comp[0].haveDc) { laterScanFailed = 1; goto render_preview; }
					goto fail;
				}
			} else {
				/* AC scans and refinement scans are not needed for the DC preview. */
			}
			continue;
		}
		p += len;
	}
	if (!sof2) { failReason = "no SOF2"; goto fail; }
	if (!anyDcScan) { failReason = "no DC scan found"; goto fail; }
	if (!anyDc) { failReason = "no DC Huffman table"; goto fail; }
	if (!lumaDcReady || !comp[0].haveDc) { failReason = "no luma DC"; goto fail; }

render_preview:
	if (!comp[0].haveDc) { failReason = failReason ? failReason : "no luma DC"; goto fail; }
	memset(greyOut, 0x80, (size_t)(outW * outH)); if (rgbOut) memset(rgbOut, 0x80, (size_t)(outW * outH * 3)); memset(greyAccum,0,sizeof(greyAccum)); memset(rAccum,0,sizeof(rAccum)); memset(gAccum,0,sizeof(gAccum)); memset(bAccum,0,sizeof(bAccum)); memset(greyCount,0,sizeof(greyCount));
	for (i = 0; i < width * height; i++) { int x = i % width, y = i / width, dst = (y * outH / height) * outW + (x * outW / width); int yb = (y * comp[0].v * comp[0].bh) / height / comp[0].v, xb = (x * comp[0].h * comp[0].bw) / width / comp[0].h; int Y = 128 + (dc[0][yb * comp[0].bw + xb] * (int)qdc[comp[0].tq] + 4) / 8; int R = Y, G = Y, B = Y; if (Y < 0) Y = 0; else if (Y > 255) Y = 255; if (comps == 3 && comp[1].haveDc && comp[2].haveDc) { int cbx = x * comp[1].bw / width, cby = y * comp[1].bh / height, crx = x * comp[2].bw / width, cry = y * comp[2].bh / height; int Cb = 128 + (dc[1][cby * comp[1].bw + cbx] * (int)qdc[comp[1].tq] + 4) / 8; int Cr = 128 + (dc[2][cry * comp[2].bw + crx] * (int)qdc[comp[2].tq] + 4) / 8; R = Y + ((359 * (Cr - 128)) >> 8); G = Y - ((88 * (Cb - 128) + 183 * (Cr - 128)) >> 8); B = Y + ((454 * (Cb - 128)) >> 8); if (R<0)R=0; else if(R>255)R=255; if(G<0)G=0; else if(G>255)G=255; if(B<0)B=0; else if(B>255)B=255; }
		greyAccum[dst] += (77UL * R + 150UL * G + 29UL * B + 128UL) >> 8; rAccum[dst] += R; gAccum[dst] += G; bAccum[dst] += B; if (greyCount[dst] != 0xffff) greyCount[dst]++; }
	for (i = 0; i < outW * outH; i++) if (greyCount[i]) { unsigned short c = greyCount[i]; greyOut[i] = (unsigned char)((greyAccum[i] + c / 2) / c); if (rgbOut) { rgbOut[i*3] = (unsigned char)((rAccum[i] + c / 2) / c); rgbOut[i*3+1] = (unsigned char)((gAccum[i] + c / 2) / c); rgbOut[i*3+2] = (unsigned char)((bAccum[i] + c / 2) / c); } }
	if (laterScanFailed) RADIO_DBG(printf("radio-art: progressive JPEG later scan failed, keeping luma preview\n");)
	if (comps == 3 && comp[1].haveDc && comp[2].haveDc) RADIO_DBG(printf("radio-art: progressive JPEG colour DC preview used\n");)
	else if (comps == 4) RADIO_DBG(printf("radio-art: progressive JPEG 4-component DC preview using component 0 greyscale\n");)
	else RADIO_DBG(printf("radio-art: progressive JPEG greyscale DC preview used\n");)
	return 0;

fail:
	if (comp[0].haveDc && failReason && !strcmp(failReason, "bad entropy decode")) { laterScanFailed = 1; goto render_preview; }
	RADIO_DBG(printf("radio-art: progressive JPEG DC preview unsupported: %s\n", failReason ? failReason : "bad entropy decode");)
	return -1;
}

static int DecodeJpegToGrey(const unsigned char *jpegData, unsigned long jpegBytes,
	unsigned char *greyOut, unsigned char *rgbOut, int outW, int outH, int isPng)
{
	pjpeg_image_info_t info;
	MrPjpegSrc src;
	unsigned char status;
	static unsigned char xMap[MR_MAX_JPEG_DIM], yMap[MR_MAX_JPEG_DIM];
	static unsigned long greyAccum[MR_ART_W * MR_ART_H];
	static unsigned long rAccum[MR_ART_W * MR_ART_H];
	static unsigned long gAccum[MR_ART_W * MR_ART_H];
	static unsigned long bAccum[MR_ART_W * MR_ART_H];
	static unsigned short greyCount[MR_ART_W * MR_ART_H];
	int mcuIndex, i;
	if (isPng || !jpegData || jpegBytes <= 4 || !greyOut ||
		outW <= 0 || outW > MR_ART_W || outH <= 0 || outH > MR_ART_H)
		return -1;
	src.data = jpegData; src.pos = 0; src.size = jpegBytes;
	memset(greyOut, 0x80, (size_t)(outW * outH));
	if (rgbOut)
		memset(rgbOut, 0x80, (size_t)(outW * outH * 3));
	memset(greyAccum, 0, sizeof(greyAccum));
	memset(rAccum, 0, sizeof(rAccum));
	memset(gAccum, 0, sizeof(gAccum));
	memset(bAccum, 0, sizeof(bAccum));
	memset(greyCount, 0, sizeof(greyCount));
	status = pjpeg_decode_init(&info, MrPjpegCb, &src, 0);
	if (status != 0) {
		pjpeg_decode_free();
		if (MrJpegHasSof2(jpegData, jpegBytes)) {
			RADIO_DBG(printf("radio-art: progressive JPEG detected\n");)
			if (DecodeProgressiveJpegDcPreviewToGrey(jpegData, jpegBytes, greyOut, rgbOut, outW, outH) == 0)
				return 0;
			RADIO_DBG(printf("radio-art: progressive JPEG DC preview unsupported\n");)
		}
		RADIO_DBG(printf("radio-art: pjpeg_decode_init failed status=%d (%s)\n",
			(int)status, PjpegStatusName((int)status));)
		return -1;
	}
	if (info.m_width <= 0 || info.m_height <= 0 ||
		info.m_width > MR_MAX_JPEG_DIM || info.m_height > MR_MAX_JPEG_DIM) {
		pjpeg_decode_free();
		RADIO_DBG(printf("radio-art: jpeg dimensions out of range %dx%d (max %d)\n",
			(int)info.m_width, (int)info.m_height, MR_MAX_JPEG_DIM);)
		return -1;
	}
	RADIO_DBG(printf("radio-art: jpeg %dx%d comps=%d scan-type=%d\n",
		(int)info.m_width, (int)info.m_height, (int)info.m_comps, (int)info.m_scanType);)
	for (i = 0; i < info.m_width; i++) xMap[i] = (unsigned char)((i * outW) / info.m_width);
	for (i = 0; i < info.m_height; i++) yMap[i] = (unsigned char)((i * outH) / info.m_height);
	for (mcuIndex = 0; mcuIndex < info.m_MCUSPerRow * info.m_MCUSPerCol; mcuIndex++) {
		int mcuX, mcuY, y;
		status = pjpeg_decode_mcu();
		if (status == PJPG_NO_MORE_BLOCKS) break;
		if (status != 0) {
			pjpeg_decode_free();
			RADIO_DBG(printf("radio-art: pjpeg_decode_mcu failed at mcu=%d status=%d (%s)\n",
				mcuIndex, (int)status, PjpegStatusName((int)status));)
			return -1;
		}
		mcuX = (mcuIndex % info.m_MCUSPerRow) * info.m_MCUWidth;
		mcuY = (mcuIndex / info.m_MCUSPerRow) * info.m_MCUHeight;
		for (y = 0; y < info.m_MCUHeight; y++) {
			int srcY = mcuY + y, dstY, x;
			if (srcY >= info.m_height) continue;
			dstY = yMap[srcY];
			for (x = 0; x < info.m_MCUWidth; x++) {
				int srcX = mcuX + x, dst;
				unsigned char r, g, b;
				if (srcX >= info.m_width) continue;
				dst = dstY * outW + xMap[srcX];
				greyAccum[dst] += (unsigned long)MrJpegRgbSample(&info, MrMcuSampleOffset(&info, x, y), &r, &g, &b);
				rAccum[dst] += r; gAccum[dst] += g; bAccum[dst] += b;
				if (greyCount[dst] != 0xffff) greyCount[dst]++;
			}
		}
	}
	for (i = 0; i < outW * outH; i++)
		if (greyCount[i]) {
			unsigned short c = greyCount[i];
			greyOut[i] = (unsigned char)((greyAccum[i] + (c / 2)) / c);
			if (rgbOut) {
				rgbOut[i * 3    ] = (unsigned char)((rAccum[i] + (c / 2)) / c);
				rgbOut[i * 3 + 1] = (unsigned char)((gAccum[i] + (c / 2)) / c);
				rgbOut[i * 3 + 2] = (unsigned char)((bAccum[i] + (c / 2)) / c);
			}
		}
	pjpeg_decode_free();
	return 0;
}

static void ReleaseArtColorPens(MrApp *app)
{
	if (app && app->artPensBuilt && app->win) {
		struct ColorMap *cm = app->win->WScreen->ViewPort.ColorMap;
		int i;
		MR_TASK_IDENTITY("artwork-release-pens");
		if (cm)
			for (i = 0; i < app->artPenCacheUsed; i++)
				if (app->artPenCache[i].pen >= 0) {
					/* Ownership field invalidation: a released pen index is
					 * stale exactly like a freed pointer, so mark the slot -1
					 * the instant we hand it back.  Without this, a slot left
					 * holding its old (now-released) index could be released a
					 * second time -- the pen equivalent of the AN_FreeTwice
					 * double free this build is chasing -- if this cache were
					 * ever walked again before BuildArtColorPens() repopulated
					 * it.  Clearing it makes the guard above self-correct. */
					MR_FREE_BEGIN("ReleaseArtColorPens", "art-color-pen",
						(void *)(long)app->artPenCache[i].pen, (unsigned long)i);
					ReleasePen(cm, app->artPenCache[i].pen);
					app->artPenCache[i].pen = -1;
					MR_FREE_END("ReleaseArtColorPens", "art-color-pen",
						(void *)(long)app->artPenCache[i].pen, (unsigned long)i);
				}
	}
	if (app) {
		app->artPensBuilt = 0;
		app->artPenCacheUsed = 0;
	}
}

static void BuildArtColorPens(MrApp *app)
{
	struct ColorMap *cm;
	int i;
	static const unsigned char bayer[8][8] = {
		{0,32,8,40,2,34,10,42},{48,16,56,24,50,18,58,26},{12,44,4,36,14,46,6,38},{60,28,52,20,62,30,54,22},
		{3,35,11,43,1,33,9,41},{51,19,59,27,49,17,57,25},{15,47,7,39,13,45,5,37},{63,31,55,23,61,29,53,21}
	};
	ReleaseArtColorPens(app);
	if (!app || !app->win || !app->artValid)
		return;
	cm = app->win->WScreen->ViewPort.ColorMap;
	if (!cm)
		return;

	/* Pass 1: cache the first unique source colours we can obtain pens for.
	 * The artwork usually has many more than 64 colours, so later pixels must
	 * be matched to the nearest cached colour instead of falling back to grey. */
	for (i = 0; i < MR_ART_W * MR_ART_H; i++) {
		const unsigned char *p = &app->artRGBBuf[i * 3];
		unsigned long key = ((unsigned long)p[0] << 16) | ((unsigned long)p[1] << 8) | p[2];
		int j;
		for (j = 0; j < app->artPenCacheUsed; j++)
			if (app->artPenCache[j].key == key)
				break;
		if (j == app->artPenCacheUsed && app->artPenCacheUsed < MR_ART_COLOR_CACHE) {
			ULONG r32 = (ULONG)p[0] | ((ULONG)p[0] << 8) | ((ULONG)p[0] << 16) | ((ULONG)p[0] << 24);
			ULONG g32 = (ULONG)p[1] | ((ULONG)p[1] << 8) | ((ULONG)p[1] << 16) | ((ULONG)p[1] << 24);
			ULONG b32 = (ULONG)p[2] | ((ULONG)p[2] << 8) | ((ULONG)p[2] << 16) | ((ULONG)p[2] << 24);
			app->artPenCache[j].key = key;
			app->artPenCache[j].pen = ObtainBestPen(cm, r32, g32, b32,
				OBP_FailIfBad, (Tag)FALSE, TAG_DONE);
			app->artPenCacheUsed++;
		}
	}
	if (!app->artPenCacheUsed)
		return;

	/* Pass 2: match every pixel to the closest cached colour after a small
	 * Bayer dither offset, mirroring the GadTools colour-art path. */
	for (i = 0; i < MR_ART_W * MR_ART_H; i++) {
		const unsigned char *p = &app->artRGBBuf[i * 3];
		int dv = (int)bayer[(i / MR_ART_W) & 7][i & 7] - 32;
		int dscale = dv * 3 / 4;
		int rd = (int)p[0] + dscale;
		int gd = (int)p[1] + dscale;
		int bd = (int)p[2] + dscale;
		int bestj = 0;
		unsigned long bestDist = 0xffffffffUL;
		int j;
		if (rd < 0) rd = 0; else if (rd > 255) rd = 255;
		if (gd < 0) gd = 0; else if (gd > 255) gd = 255;
		if (bd < 0) bd = 0; else if (bd > 255) bd = 255;
		for (j = 0; j < app->artPenCacheUsed; j++) {
			unsigned long k = app->artPenCache[j].key;
			int dr = (int)((k >> 16) & 0xff) - rd;
			int dg = (int)((k >>  8) & 0xff) - gd;
			int db = (int)( k        & 0xff) - bd;
			unsigned long dist = (unsigned long)(dr * dr + dg * dg + db * db);
			if (dist < bestDist) { bestDist = dist; bestj = j; }
		}
		app->artPenIdx[i] = (unsigned char)bestj;
	}
	app->artPensBuilt = 1;
}

static void ArtworkCacheName(MrApp *app, char *dst, size_t dstSize)
{
	const char *source, *base, *end;
	char safe[80];
	int i, j;
	/* Artwork cache lives beside the executable (PROGDIR:ArtCache), not under
	 * ENV:/ENVARC: -- these are 16-20 KB .grey64 image files, and PROGDIR: is
	 * persistent and launch-location independent (ENVARC: is for small config
	 * vars, not image blobs). */
	SafeCopy(dst, dstSize, "PROGDIR:ArtCache");
	source = (MrIsRadioInput(app->inputName) && app->currentRadioFavicon[0]) ?
		app->currentRadioFavicon : app->inputName;
	end = strchr(source, '?');
	if (!end) end = source + strlen(source);
	base = end;
	while (base > source && base[-1] != '/' && base[-1] != ':')
		base--;
	for (i = 0, j = 0; base + i < end && base[i] && j < (int)sizeof(safe) - 1; i++) {
		unsigned char c = (unsigned char)base[i];
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
			safe[j++] = (char)c;
		else if (c == '.')
			safe[j++] = '_';
	}
	safe[j] = '\0';
	if (!safe[0])
		SafeCopy(safe, sizeof(safe), "art");
	strncat(dst, "/", dstSize - strlen(dst) - 1);
	strncat(dst, safe, dstSize - strlen(dst) - 1);
	strncat(dst, ".grey64", dstSize - strlen(dst) - 1);
}

static int LoadArtworkCache(MrApp *app)
{
	char path[MR_MAX_PATH], hdr[8];
	FILE *f;
	if (!app->artCacheEnabled || app->artCacheBypass || !app->inputName[0])
		return 0;
	ArtworkCacheName(app, path, sizeof(path));
	f = fopen(path, "rb");
	if (!f)
		return 0;
	if (fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr) &&
		memcmp(hdr, "M3AG64\0", 7) == 0 && (hdr[7] == 1 || hdr[7] == 2) &&
		fread(app->artGreyBuf, 1, MR_ART_W * MR_ART_H, f) == MR_ART_W * MR_ART_H) {
		int i;
		if (hdr[7] != 2 ||
			fread(app->artRGBBuf, 1, MR_ART_W * MR_ART_H * 3, f) != MR_ART_W * MR_ART_H * 3)
			for (i = 0; i < MR_ART_W * MR_ART_H; i++)
				app->artRGBBuf[i * 3] = app->artRGBBuf[i * 3 + 1] = app->artRGBBuf[i * 3 + 2] = app->artGreyBuf[i];
		fclose(f);
		app->artValid = 1;
		return 1;
	}
	fclose(f);
	return 0;
}

static void SaveArtworkCache(MrApp *app)
{
	char dir[64], path[MR_MAX_PATH];
	FILE *f;
	static const unsigned char hdr[8] = { 'M','3','A','G','6','4','\0', 2 };
	if (!app->artCacheEnabled || !app->inputName[0] || !app->artValid)
		return;
	SafeCopy(dir, sizeof(dir), "PROGDIR:ArtCache");
	CreateDir((STRPTR)dir);
	ArtworkCacheName(app, path, sizeof(path));
	f = fopen(path, "wb");
	if (!f)
		return;
	fwrite(hdr, 1, sizeof(hdr), f);
	fwrite(app->artGreyBuf, 1, MR_ART_W * MR_ART_H, f);
	fwrite(app->artRGBBuf, 1, MR_ART_W * MR_ART_H * 3, f);
	fclose(f);
}

static void CleanArtworkCache(MrApp *app)
{
	char dir[64];
	BPTR lock;
	struct FileInfoBlock *fib;
	int removed = 0;
	SafeCopy(dir, sizeof(dir), "PROGDIR:ArtCache");
	lock = Lock((STRPTR)dir, ACCESS_READ);
	if (!lock) { SetStatus(app, "Artwork cache is empty."); return; }
	fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
	if (fib && Examine(lock, fib)) {
		while (ExNext(lock, fib)) {
			char path[MR_MAX_PATH];
			int len = strlen(fib->fib_FileName);
			if (fib->fib_DirEntryType >= 0 || len < 7 || strcmp(fib->fib_FileName + len - 7, ".grey64") != 0)
				continue;
			SafeCopy(path, sizeof(path), dir);
			strncat(path, "/", sizeof(path) - strlen(path) - 1);
			strncat(path, fib->fib_FileName, sizeof(path) - strlen(path) - 1);
			if (DeleteFile((STRPTR)path))
				removed++;
		}
	}
	if (fib) FreeDosObject(DOS_FIB, fib);
	UnLock(lock);
	if (removed) {
		char msg[64];
		sprintf(msg, "Removed %d cached artwork file(s).", removed);
		SetStatus(app, msg);
	} else
		SetStatus(app, "No cached artwork files to remove.");
}


/* Uppercased file extension (without the dot) of a URL's path, e.g. "PNG"
 * for ".../icon.png?x=1".  The last dot in the final path segment wins,
 * so thumbnail URLs such as ".../Logo.svg.png" are labelled as PNG rather
 * than SVG.  Empty if there's no recognizable extension.
 * Used only to label the artwork placeholder so it's visible at a glance
 * which favicon type was rejected (".webp", ".svg", ...). */
static void MrUrlExtensionUpper(const char *url, char *out, int outSize)
{
	const char *q, *hash, *end, *segment, *dot, *p;
	int len, i, j;
	if (!out || outSize <= 0) return;
	out[0] = '\0';
	if (!url || !url[0]) return;
	q = strchr(url, '?');
	hash = strchr(url, '#');
	end = url + strlen(url);
	if (q && q < end) end = q;
	if (hash && hash < end) end = hash;
	len = (int)(end - url);
	segment = url + len;
	while (segment > url && segment[-1] != '/' && segment[-1] != ':') segment--;
	dot = (const char *)0;
	for (p = segment; p < end; p++) {
		if (*p == '.')
			dot = p;
	}
	if (!dot || dot + 1 >= end) return;
	dot++;
	for (i = 0, j = 0; dot + i < url + len && dot[i] && j < outSize - 1; i++) {
		unsigned char c = (unsigned char)dot[i];
		if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 'a' + 'A');
		if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
			out[j++] = (char)c;
		else
			break;
	}
	out[j] = '\0';
}

#if ENABLE_RADIO_ARTWORK
static int MrIsJpegMagic(const unsigned char *data, int bytes)
{
	return bytes >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF;
}

/* Shared downsample scratch space for the PNG and ICO favicon decoders
 * below.  They're never active concurrently -- LoadRadioFaviconImage runs
 * at most one decode path per favicon -- so sharing these buffers instead
 * of each decoder declaring its own saves ~72KB of BSS that duplicating
 * them would otherwise cost. */
static unsigned char sArtXMap[MR_MAX_JPEG_DIM], sArtYMap[MR_MAX_JPEG_DIM];
static unsigned long sArtGreyAccum[MR_ART_W * MR_ART_H];
static unsigned long sArtRAccum[MR_ART_W * MR_ART_H];
static unsigned long sArtGAccum[MR_ART_W * MR_ART_H];
static unsigned long sArtBAccum[MR_ART_W * MR_ART_H];
static unsigned short sArtGreyCount[MR_ART_W * MR_ART_H];

#if ENABLE_PNG_ARTWORK
static int MrIsPngMagic(const unsigned char *data, int bytes)
{
	static const unsigned char sig[8] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
	return bytes >= 8 && memcmp(data, sig, sizeof(sig)) == 0;
}

/* Decodes a PNG favicon via lodepng into the same downsampled grey/RGB
 * thumbnail buffers DecodeJpegToGrey() produces, so the rest of the
 * artwork pipeline (cache, dithered/colour rendering) doesn't need to
 * know which decoder ran.  lodepng_inspect() reads just the IHDR header
 * first so an oversized PNG is rejected before lodepng_decode32() would
 * malloc width*height*4 bytes for it -- the 256KB download cap bounds the
 * compressed input, but a small PNG can still declare huge dimensions. */
static int DecodePngToGrey(const unsigned char *pngData, unsigned long pngBytes,
	unsigned char *greyOut, unsigned char *rgbOut, int outW, int outH)
{
	LodePNGState state;
	unsigned pw = 0, ph = 0, err;
	unsigned char *image;
	unsigned char *xMap = sArtXMap, *yMap = sArtYMap;
	unsigned long *greyAccum = sArtGreyAccum;
	unsigned long *rAccum = sArtRAccum;
	unsigned long *gAccum = sArtGAccum;
	unsigned long *bAccum = sArtBAccum;
	unsigned short *greyCount = sArtGreyCount;
	unsigned x, y;
	int i;

	if (!pngData || pngBytes <= 8 || !greyOut ||
		outW <= 0 || outW > MR_ART_W || outH <= 0 || outH > MR_ART_H)
		return -1;

	lodepng_state_init(&state);
	err = lodepng_inspect(&pw, &ph, &state, pngData, (size_t)pngBytes);
	lodepng_state_cleanup(&state);
	if (err) {
		RADIO_DBG(printf("radio-art: lodepng_inspect failed err=%u (%s)\n",
			err, lodepng_error_text(err));)
		return -1;
	}
	if (pw == 0 || ph == 0 || pw > MR_MAX_JPEG_DIM || ph > MR_MAX_JPEG_DIM) {
		RADIO_DBG(printf("radio-art: png dimensions out of range %ux%u (max %d)\n",
			pw, ph, MR_MAX_JPEG_DIM);)
		return -1;
	}

	image = NULL;
	err = lodepng_decode32(&image, &pw, &ph, pngData, (size_t)pngBytes);
	if (err || !image) {
		RADIO_DBG(printf("radio-art: lodepng_decode32 failed err=%u (%s)\n",
			err, lodepng_error_text(err));)
		if (image) {
			/* image is malloc()'d inside lodepng.c (a separate translation
			 * unit) and released with the matching free(); it is a private
			 * decode buffer owned only here.  Clear it after the free so the
			 * pointer cannot be reused on any later edit of this path. */
			MR_FREE_BEGIN("DecodePngToGrey", "lodepng-image(err)", image, (unsigned long)pngBytes);
			free(image);
			image = NULL;
			MR_FREE_END("DecodePngToGrey", "lodepng-image(err)", image, 0);
		}
		return -1;
	}
	RADIO_DBG(printf("radio-art: png %ux%u decoded ok\n", pw, ph);)

	memset(greyOut, 0x80, (size_t)(outW * outH));
	if (rgbOut)
		memset(rgbOut, 0x80, (size_t)(outW * outH * 3));
	memset(greyAccum, 0, sizeof(sArtGreyAccum));
	memset(rAccum, 0, sizeof(sArtRAccum));
	memset(gAccum, 0, sizeof(sArtGAccum));
	memset(bAccum, 0, sizeof(sArtBAccum));
	memset(greyCount, 0, sizeof(sArtGreyCount));

	for (x = 0; x < pw; x++) xMap[x] = (unsigned char)(((unsigned long)x * (unsigned long)outW) / pw);
	for (y = 0; y < ph; y++) yMap[y] = (unsigned char)(((unsigned long)y * (unsigned long)outH) / ph);

	/* Transparent pixels are composited over the panel's neutral grey (see
	 * the per-pixel note below) so transparent favicons blend into the panel
	 * instead of showing a raw RGB box. */
	for (y = 0; y < ph; y++) {
		const unsigned char *row = image + 4UL * (unsigned long)y * (unsigned long)pw;
		int dstY = yMap[y];
		for (x = 0; x < pw; x++) {
			const unsigned char *px = row + 4 * x;
			unsigned char r = px[0], g = px[1], b = px[2];
			unsigned char a = px[3];
			int dst = dstY * outW + xMap[x];
			/* Composite over the panel's neutral grey so transparent favicon
			 * areas blend into the recessed panel instead of showing raw RGB
			 * (usually a black or white box).  0x80 matches the buffer fill and
			 * the mid "background" dither band.  Opaque pixels (a==255) are left
			 * untouched, so fully opaque images decode exactly as before. */
			if (a != 255) {
				r = (unsigned char)((r * a + 0x80 * (255 - a) + 127) / 255);
				g = (unsigned char)((g * a + 0x80 * (255 - a) + 127) / 255);
				b = (unsigned char)((b * a + 0x80 * (255 - a) + 127) / 255);
			}
			greyAccum[dst] += (77UL * r + 150UL * g + 29UL * b + 128UL) >> 8;
			rAccum[dst] += r; gAccum[dst] += g; bAccum[dst] += b;
			if (greyCount[dst] != 0xffff) greyCount[dst]++;
		}
	}
	MR_FREE_BEGIN("DecodePngToGrey", "lodepng-image", image, (unsigned long)pngBytes);
	free(image);
	image = NULL;
	MR_FREE_END("DecodePngToGrey", "lodepng-image", image, 0);

	for (i = 0; i < outW * outH; i++)
		if (greyCount[i]) {
			unsigned short c = greyCount[i];
			greyOut[i] = (unsigned char)((greyAccum[i] + (c / 2)) / c);
			if (rgbOut) {
				rgbOut[i * 3    ] = (unsigned char)((rAccum[i] + (c / 2)) / c);
				rgbOut[i * 3 + 1] = (unsigned char)((gAccum[i] + (c / 2)) / c);
				rgbOut[i * 3 + 2] = (unsigned char)((bAccum[i] + (c / 2)) / c);
			}
		}
	return 0;
}
#endif /* ENABLE_PNG_ARTWORK */

#if ENABLE_WEBP_ARTWORK
/* Decodes a WebP favicon via webpdec into the same downsampled grey/RGB
 * thumbnail buffers DecodeJpegToGrey()/DecodePngToGrey() produce.
 * webp_get_info() bounds the declared dimensions before webp_decode_rgb()
 * allocates width*height*3; both the lossless (VP8L) and lossy (VP8)
 * bitstreams are handled, returning a packed 24-bit RGB buffer we free(). */
static int DecodeWebpToGrey(const unsigned char *webpData, unsigned long webpBytes,
	unsigned char *greyOut, unsigned char *rgbOut, int outW, int outH)
{
	unsigned char *image = NULL;
	unsigned pw = 0, ph = 0;
	unsigned char *xMap = sArtXMap, *yMap = sArtYMap;
	unsigned long *greyAccum = sArtGreyAccum;
	unsigned long *rAccum = sArtRAccum;
	unsigned long *gAccum = sArtGAccum;
	unsigned long *bAccum = sArtBAccum;
	unsigned short *greyCount = sArtGreyCount;
	unsigned x, y;
	int i, rc;

	if (!webpData || webpBytes <= 12 || !greyOut ||
		outW <= 0 || outW > MR_ART_W || outH <= 0 || outH > MR_ART_H)
		return -1;

	if (webp_get_info(webpData, webpBytes, &pw, &ph) != WEBP_OK)
		return -1;
	if (pw == 0 || ph == 0 || pw > MR_MAX_JPEG_DIM || ph > MR_MAX_JPEG_DIM) {
		RADIO_DBG(printf("radio-art: webp dimensions out of range %ux%u (max %d)\n",
			pw, ph, MR_MAX_JPEG_DIM);)
		return -1;
	}

	rc = webp_decode_rgb(webpData, webpBytes, MR_MAX_JPEG_DIM, &image, &pw, &ph);
	if (rc != WEBP_OK || !image) {
		RADIO_DBG(printf("radio-art: webp decode failed rc=%d\n", rc);)
		if (image) {
			MR_FREE_BEGIN("DecodeWebpToGrey", "webp-image(err)", image, (unsigned long)webpBytes);
			free(image);
			image = NULL;
			MR_FREE_END("DecodeWebpToGrey", "webp-image(err)", image, 0);
		}
		return -1;
	}
	RADIO_DBG(printf("radio-art: webp %ux%u decoded ok\n", pw, ph);)

	memset(greyOut, 0x80, (size_t)(outW * outH));
	if (rgbOut)
		memset(rgbOut, 0x80, (size_t)(outW * outH * 3));
	memset(greyAccum, 0, sizeof(sArtGreyAccum));
	memset(rAccum, 0, sizeof(sArtRAccum));
	memset(gAccum, 0, sizeof(sArtGAccum));
	memset(bAccum, 0, sizeof(sArtBAccum));
	memset(greyCount, 0, sizeof(sArtGreyCount));

	for (x = 0; x < pw; x++) xMap[x] = (unsigned char)(((unsigned long)x * (unsigned long)outW) / pw);
	for (y = 0; y < ph; y++) yMap[y] = (unsigned char)(((unsigned long)y * (unsigned long)outH) / ph);

	for (y = 0; y < ph; y++) {
		const unsigned char *row = image + 3UL * (unsigned long)y * (unsigned long)pw;
		int dstY = yMap[y];
		for (x = 0; x < pw; x++) {
			const unsigned char *px = row + 3 * x;
			unsigned char r = px[0], g = px[1], b = px[2];
			int dst = dstY * outW + xMap[x];
			greyAccum[dst] += (77UL * r + 150UL * g + 29UL * b + 128UL) >> 8;
			rAccum[dst] += r; gAccum[dst] += g; bAccum[dst] += b;
			if (greyCount[dst] != 0xffff) greyCount[dst]++;
		}
	}
	MR_FREE_BEGIN("DecodeWebpToGrey", "webp-image", image, (unsigned long)webpBytes);
	free(image);
	image = NULL;
	MR_FREE_END("DecodeWebpToGrey", "webp-image", image, 0);

	for (i = 0; i < outW * outH; i++)
		if (greyCount[i]) {
			unsigned short c = greyCount[i];
			greyOut[i] = (unsigned char)((greyAccum[i] + (c / 2)) / c);
			if (rgbOut) {
				rgbOut[i * 3    ] = (unsigned char)((rAccum[i] + (c / 2)) / c);
				rgbOut[i * 3 + 1] = (unsigned char)((gAccum[i] + (c / 2)) / c);
				rgbOut[i * 3 + 2] = (unsigned char)((bAccum[i] + (c / 2)) / c);
			}
		}
	return 0;
}
#endif /* ENABLE_WEBP_ARTWORK */

static int MrIsIcoMagic(const unsigned char *data, int bytes)
{
	/* ICONDIR: 2 bytes reserved (0), 2 bytes type (1 = icon, 2 = cursor). */
	return bytes >= 6 && data[0] == 0 && data[1] == 0 &&
		data[2] == 1 && data[3] == 0;
}

static unsigned IcoLE16(const unsigned char *p)
{
	return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static unsigned long IcoLE32(const unsigned char *p)
{
	return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
		((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

typedef struct IcoEntry {
	unsigned width, height, bitCount;
	unsigned long size, offset;
} IcoEntry;

/* Decodes the raw BITMAPINFOHEADER-style DIB embedded in a legacy
 * (non-PNG) ICO entry into the same downsampled grey/RGB thumbnail
 * buffers DecodePngToGrey()/DecodeJpegToGrey() produce.  Only the depths
 * real-world icon tools actually emit are supported (32/24/8bpp,
 * uncompressed); 4bpp/1bpp and RLE-compressed DIBs are rejected.  The AND
 * (transparency) mask that follows the pixel data is ignored, the same
 * tradeoff DecodePngToGrey makes with alpha -- favicons are rarely
 * meaningfully transparent. */
static int DecodeIcoDibToGrey(const unsigned char *dib, unsigned long dibBytes,
	unsigned char *greyOut, unsigned char *rgbOut, int outW, int outH)
{
	unsigned long headerSize;
	long width, height;
	unsigned bitCount;
	unsigned long compression, paletteEntries, rowBytes;
	const unsigned char *palette;
	const unsigned char *pixels;
	unsigned char *xMap = sArtXMap, *yMap = sArtYMap;
	unsigned long *greyAccum = sArtGreyAccum;
	unsigned long *rAccum = sArtRAccum;
	unsigned long *gAccum = sArtGAccum;
	unsigned long *bAccum = sArtBAccum;
	unsigned short *greyCount = sArtGreyCount;
	long x, y;
	int i;

	if (!dib || dibBytes < 40 || !greyOut || outW <= 0 || outW > MR_ART_W ||
		outH <= 0 || outH > MR_ART_H)
		return -1;
	headerSize = IcoLE32(dib);
	if (headerSize < 40 || headerSize > dibBytes)
		return -1;
	width = (long)IcoLE32(dib + 4);
	/* Height counts the XOR colour rows plus the AND mask rows stacked
	 * together, so the actual image height is half the field's value. */
	height = (long)IcoLE32(dib + 8) / 2;
	bitCount = IcoLE16(dib + 14);
	compression = IcoLE32(dib + 16);
	if (width <= 0 || height <= 0 || width > 256 || height > 256 ||
		compression != 0 /* BI_RGB, uncompressed */)
		return -1;
	if (bitCount != 32 && bitCount != 24 && bitCount != 8)
		return -1;

	paletteEntries = (bitCount == 8) ? 256UL : 0UL;
	palette = dib + headerSize;
	pixels = palette + paletteEntries * 4UL;
	rowBytes = (((unsigned long)width * bitCount + 31UL) / 32UL) * 4UL;
	if ((unsigned long)(pixels - dib) + rowBytes * (unsigned long)height > dibBytes)
		return -1;

	memset(greyOut, 0x80, (size_t)(outW * outH));
	if (rgbOut)
		memset(rgbOut, 0x80, (size_t)(outW * outH * 3));
	memset(greyAccum, 0, sizeof(sArtGreyAccum));
	memset(rAccum, 0, sizeof(sArtRAccum));
	memset(gAccum, 0, sizeof(sArtGAccum));
	memset(bAccum, 0, sizeof(sArtBAccum));
	memset(greyCount, 0, sizeof(sArtGreyCount));
	for (x = 0; x < width; x++)
		xMap[x] = (unsigned char)((x * outW) / width);
	for (y = 0; y < height; y++)
		yMap[y] = (unsigned char)((y * outH) / height);

	/* DIB pixel rows are stored bottom-up. */
	for (y = 0; y < height; y++) {
		const unsigned char *row = pixels +
			(unsigned long)(height - 1 - y) * rowBytes;
		int dstY = yMap[y];
		for (x = 0; x < width; x++) {
			unsigned char r, g, b;
			int dst;

			if (bitCount == 8) {
				const unsigned char *pal = palette + 4 * row[x];
				b = pal[0]; g = pal[1]; r = pal[2];
			} else {
				const unsigned char *px = row + (bitCount == 32 ? 4 : 3) * x;
				b = px[0]; g = px[1]; r = px[2];
			}
			dst = dstY * outW + xMap[x];
			greyAccum[dst] += (77UL * r + 150UL * g + 29UL * b + 128UL) >> 8;
			rAccum[dst] += r; gAccum[dst] += g; bAccum[dst] += b;
			if (greyCount[dst] != 0xffff) greyCount[dst]++;
		}
	}
	for (i = 0; i < outW * outH; i++)
		if (greyCount[i]) {
			unsigned short c = greyCount[i];
			greyOut[i] = (unsigned char)((greyAccum[i] + (c / 2)) / c);
			if (rgbOut) {
				rgbOut[i * 3    ] = (unsigned char)((rAccum[i] + (c / 2)) / c);
				rgbOut[i * 3 + 1] = (unsigned char)((gAccum[i] + (c / 2)) / c);
				rgbOut[i * 3 + 2] = (unsigned char)((bAccum[i] + (c / 2)) / c);
			}
		}
	return 0;
}

static int IcoEntryCompare(const void *pa, const void *pb)
{
	const IcoEntry *a = (const IcoEntry *)pa;
	const IcoEntry *b = (const IcoEntry *)pb;
	unsigned long areaA = (unsigned long)a->width * (unsigned long)a->height;
	unsigned long areaB = (unsigned long)b->width * (unsigned long)b->height;

	if (areaA != areaB) return areaA > areaB ? -1 : 1;
	return (int)b->bitCount - (int)a->bitCount;
}

/* ICO files are a directory of one or more embedded images at different
 * sizes/depths; each entry's payload is either a full PNG (the 256px
 * entry in modern multi-res icons almost always is) or a raw
 * BITMAPINFOHEADER DIB with no BITMAPFILEHEADER.  Entries are tried
 * largest-first, since that gives the best quality once downsampled to
 * the ART_W x ART_H thumbnail, falling through to smaller/other entries
 * if the chosen one can't be decoded (e.g. a 4bpp/1bpp legacy DIB). */
static int DecodeIcoToGrey(const unsigned char *icoData, unsigned long icoBytes,
	unsigned char *greyOut, unsigned char *rgbOut, int outW, int outH)
{
	unsigned count, i, n;
	IcoEntry entries[64];

	if (!icoData || icoBytes < 6 || !MrIsIcoMagic(icoData, (int)icoBytes))
		return -1;
	count = IcoLE16(icoData + 4);
	if (count == 0)
		return -1;
	if (count > 64)
		count = 64;
	n = 0;
	for (i = 0; i < count; i++) {
		const unsigned char *e = icoData + 6 + (unsigned long)i * 16UL;
		unsigned w, h;
		unsigned long size, offset;

		if (6UL + (unsigned long)(i + 1) * 16UL > icoBytes)
			break;
		w = e[0] ? e[0] : 256;
		h = e[1] ? e[1] : 256;
		size = IcoLE32(e + 8);
		offset = IcoLE32(e + 12);
		if (size < 8 || offset > icoBytes || size > icoBytes - offset)
			continue;
		entries[n].width = w;
		entries[n].height = h;
		entries[n].bitCount = IcoLE16(e + 6);
		entries[n].size = size;
		entries[n].offset = offset;
		n++;
	}
	if (n == 0)
		return -1;
	qsort(entries, n, sizeof(entries[0]), IcoEntryCompare);
	for (i = 0; i < n; i++) {
		const unsigned char *payload = icoData + entries[i].offset;
		unsigned long payloadBytes = entries[i].size;

#if ENABLE_PNG_ARTWORK
		if (MrIsPngMagic(payload, (int)payloadBytes)) {
			if (DecodePngToGrey(payload, payloadBytes, greyOut, rgbOut,
				outW, outH) == 0)
				return 0;
			continue;
		}
#endif
		if (DecodeIcoDibToGrey(payload, payloadBytes, greyOut, rgbOut,
			outW, outH) == 0)
			return 0;
	}
	return -1;
}

/* Favicon artwork is fetched from the Radio Browser station's "favicon"
 * field only, never from the MP3/ICY stream, and only once playback has
 * already started picking a station (see RadioDoProbeAndPlay).  The fetch
 * goes through rb_probe_fetch_binary(), which shares its HTTP/HTTPS/AmiSSL
 * connection handling with the stream probe used to start playback but is
 * a separate code path that the probe's stream-playback logic never calls
 * into -- so this never touches the MP3 stream's SSL read loop.  Any
 * failure here (bad URL, unsupported TLS, oversized body, unsupported
 * format, broken decode) just leaves artValid 0 and never affects
 * playback.  JPEG, PNG, ICO and SVG are tried in that order, each
 * independently confirmed via magic bytes (or, for SVG, a bounded scan
 * for a "<svg" root tag) before decoding, regardless of URL extension or
 * Content-Type; anything else (WebP, ...) is silently rejected. */
static int LoadRadioFaviconImage(MrApp *app)
{
	char contentType[64];
	static unsigned char response[MR_FAVICON_MAX_BYTES];
	int bytes = 0;
	int rc;
	int artworkDisabled;
	if (!app || !app->currentRadioFavicon[0]) {
		RADIO_DBG(printf("radio-art: no favicon URL for current station\n");)
		return 0;
	}
	artworkDisabled = rb_probe_artwork_disabled();
	RADIO_DBG(printf("radio-art: flag check MP3_NO_ARTWORK enabled=%d testEnable=%d before favicon/artwork fetch\n", artworkDisabled, rb_probe_artwork_test_enabled());)
	if (artworkDisabled) {
		if (radio_runtime_flag_enabled("MP3_NO_ARTWORK"))
			RADIO_DBG(printf("radio-art: skipped by MP3_NO_ARTWORK\n");)
		else
			RADIO_DBG(printf("radio-art: disabled for run after fatal TLS/artwork transport fault\n");)
		return 0;
	}
	if (Radio_PlaybackOwnsNetwork()) {
		RADIO_DBG(printf("radio-art: skipped favicon fetch while radio playback child owns networking\n");)
		return 0;
	}
	RADIO_DBG(printf("radio-art: fetching favicon url=%s\n", app->currentRadioFavicon);)
	rc = rb_probe_fetch_binary(app->currentRadioFavicon, response, (int)sizeof(response),
		&bytes, contentType, (int)sizeof(contentType));
	if (rc != RB_STREAM_PROBE_OK) {
		RADIO_DBG(printf("radio-art: fetch failed rc=%d (%s)\n", rc, rb_probe_error_text(rc));)
		return 0;
	}
	RADIO_DBG(printf("radio-art: fetched %d bytes content-type=\"%s\"\n", bytes, contentType);)
	if (bytes <= 8)
		return 0;

	/* Dispatch purely on the actual bytes fetched, not the URL extension or
	 * declared Content-Type: plenty of real sites serve a PNG (or other
	 * format) under a "favicon.ico" URL and/or an "image/vnd.microsoft.icon"
	 * Content-Type, so trusting either would reject content we can in fact
	 * decode (and can't be trusted to guard against content we can't). */
	if (MrIsJpegMagic(response, bytes)) {
		if (DecodeJpegToGrey(response, (unsigned long)bytes, app->artGreyBuf, app->artRGBBuf,
			MR_ART_W, MR_ART_H, 0) != 0) {
			RADIO_DBG(printf("radio-art: picojpeg decode failed\n");)
			return 0;
		}
		app->artValid = 1;
		return 1;
	}
#if ENABLE_PNG_ARTWORK
	if (MrIsPngMagic(response, bytes)) {
		if (DecodePngToGrey(response, (unsigned long)bytes, app->artGreyBuf, app->artRGBBuf,
			MR_ART_W, MR_ART_H) != 0) {
			RADIO_DBG(printf("radio-art: lodepng decode failed\n");)
			return 0;
		}
		app->artValid = 1;
		return 1;
	}
#endif
	if (MrIsIcoMagic(response, bytes)) {
		if (DecodeIcoToGrey(response, (unsigned long)bytes, app->artGreyBuf, app->artRGBBuf,
			MR_ART_W, MR_ART_H) != 0) {
			RADIO_DBG(printf("radio-art: ico decode failed\n");)
			return 0;
		}
		app->artValid = 1;
		return 1;
	}
#if ENABLE_WEBP_ARTWORK
	if (webp_is_webp(response, (unsigned long)bytes)) {
		if (DecodeWebpToGrey(response, (unsigned long)bytes, app->artGreyBuf, app->artRGBBuf,
			MR_ART_W, MR_ART_H) != 0) {
			RADIO_DBG(printf("radio-art: webp decode failed\n");)
			return 0;
		}
		app->artValid = 1;
		return 1;
	}
#endif
#if ENABLE_SVG_ARTWORK
	if (SvgLooksLikeSvg(response, bytes)) {
		if (SvgDecodeToGrey(response, (unsigned long)bytes, app->artGreyBuf, app->artRGBBuf,
			MR_ART_W, MR_ART_H) != 0) {
			RADIO_DBG(printf("radio-art: svgdec decode failed\n");)
			return 0;
		}
		app->artValid = 1;
		return 1;
	}
#endif
	RADIO_DBG(printf("radio-art: rejected, unsupported favicon format (first bytes %02X %02X %02X %02X don't match JPEG/PNG/ICO/SVG)\n",
		response[0], response[1], response[2], response[3]);)
	return 0;
}
#endif /* ENABLE_RADIO_ARTWORK */

static void UpdateArtwork(MrApp *app, MrMp3Info *info)
{
	ReleaseArtColorPens(app);
	app->artValid = 0;
	if (app->artEnabled && LoadArtworkCache(app)) {
		/* cache hit */
	} else if (app->artEnabled && info && info->artData && info->artBytes > 4 &&
		DecodeJpegToGrey(info->artData, info->artBytes, app->artGreyBuf, app->artRGBBuf, MR_ART_W, MR_ART_H, info->artIsPng) == 0) {
		app->artValid = 1;
		SaveArtworkCache(app);
#if ENABLE_RADIO_ARTWORK
	} else if (app->artEnabled && MrIsRadioInput(app->inputName) && LoadRadioFaviconImage(app)) {
		SaveArtworkCache(app);
#endif
	}
	if (app->artColorEnabled && app->artValid)
		BuildArtColorPens(app);
	Radio_CheckMiniMem("after artwork fetch/decode");
	/* The panel (frame + thumbnail or "No art") is hand-drawn over the
	 * placeholder gadget rather than via the button's own text. */
	DrawArtPanel(app);
}

/* djb2 hash of a C string, used to key the random fallback-icon tint to the
 * current station/track name so the colour only re-rolls when it changes. */
static unsigned long ArtFallbackHash(const char *s)
{
	unsigned long h = 5381;
	if (s)
		while (*s)
			h = ((h << 5) + h) + (unsigned char)*s++;
	return h ? h : 1; /* never 0: 0 means "no colour rolled yet" */
}

/* Rolls a fresh, vivid random colour.  Hue is fully random; saturation and
 * value are pinned to the top so the icon always reads clearly against the
 * grey/blue Workbench regardless of which hue comes up.  A tiny self-seeding
 * xorshift PRNG keeps this dependency-free (no <stdlib.h> rand, no math). */
static void ArtRollFallbackColor(unsigned long salt,
	unsigned char *r, unsigned char *g, unsigned char *b)
{
	static unsigned long state = 2463534242UL;
	static unsigned long bump = 0;
	unsigned long hue, region, rem, q, t;

	state ^= salt + 0x9E3779B9UL + (bump++ << 6);
	state ^= state << 13;
	state ^= state >> 17;
	state ^= state << 5;

	hue = state % 360UL;
	region = hue / 60UL;
	rem = ((hue % 60UL) * 255UL) / 60UL;
	q = 255UL - rem;
	t = rem;
	switch (region) {
	case 0:  *r = 255;        *g = (unsigned char)t; *b = 0;            break;
	case 1:  *r = (unsigned char)q; *g = 255;        *b = 0;            break;
	case 2:  *r = 0;          *g = 255;        *b = (unsigned char)t;   break;
	case 3:  *r = 0;          *g = (unsigned char)q; *b = 255;          break;
	case 4:  *r = (unsigned char)t; *g = 0;          *b = 255;          break;
	default: *r = 255;        *g = 0;          *b = (unsigned char)q;   break;
	}
}

/* Returns the fallback-icon tint for the current input, rolling a new colour
 * only when the station/track (keyed on inputName) has changed since last time.
 * Stable across the frequent redraws a single station triggers. */
static void ArtFallbackColor(MrApp *app,
	unsigned char *r, unsigned char *g, unsigned char *b)
{
	unsigned long key = ArtFallbackHash(app->inputName);
	if (!app->artFallbackHasColor || key != app->artFallbackKey) {
		ArtRollFallbackColor(key, &app->artFallbackR,
			&app->artFallbackG, &app->artFallbackB);
		app->artFallbackKey = key;
		app->artFallbackHasColor = 1;
	}
	*r = app->artFallbackR;
	*g = app->artFallbackG;
	*b = app->artFallbackB;
}

/* Integer Newton's-method sqrt for plotting filled circles without <math.h>. */
static int ArtIconIntSqrt(int n)
{
	int x, y;
	if (n <= 0)
		return 0;
	x = n;
	y = (x + 1) / 2;
	while (y < x) {
		x = y;
		y = (x + n / x) / 2;
	}
	return x;
}

static void ArtIconFillCircle(struct RastPort *rp, int cx, int cy, int r)
{
	int dy;
	for (dy = -r; dy <= r; dy++) {
		int dx = ArtIconIntSqrt(r * r - dy * dy);
		RectFill(rp, cx - dx, cy + dy, cx + dx, cy + dy);
	}
}

/* Obtains a pen in the current fallback tint (rolled per station/track) for a
 * drawn no-artwork icon.  Returns the pen to draw with -- the obtained best
 * pen, or system pen 1 if none is available -- and reports via *obtained the
 * pen the caller must ReleasePen() afterwards (-1 when there is nothing to
 * free).  *cmOut receives the colour map used for the release. */
static UWORD ArtFallbackPen(MrApp *app, struct ColorMap **cmOut, LONG *obtained)
{
	struct ColorMap *cm = app->win ? app->win->WScreen->ViewPort.ColorMap : NULL;
	unsigned char rr, gg, bb;

	*cmOut = cm;
	*obtained = -1;
	ArtFallbackColor(app, &rr, &gg, &bb);
	if (cm) {
		ULONG r32 = (ULONG)rr | ((ULONG)rr << 8) | ((ULONG)rr << 16) | ((ULONG)rr << 24);
		ULONG g32 = (ULONG)gg | ((ULONG)gg << 8) | ((ULONG)gg << 16) | ((ULONG)gg << 24);
		ULONG b32 = (ULONG)bb | ((ULONG)bb << 8) | ((ULONG)bb << 16) | ((ULONG)bb << 24);
		LONG pen = ObtainBestPen(cm, r32, g32, b32,
			OBP_FailIfBad, (Tag)FALSE, TAG_DONE);
		if (pen >= 0) {
			*obtained = pen;
			return (UWORD)pen;
		}
	}
	return 1;
}

/* Boombox silhouette drawn entirely from RectFill/Move/Draw (no bitmap asset),
 * shown for a radio stream that has no favicon.  Tinted with the per-station
 * random colour; parity with the GadTools frontend's DrawRadioIcon.  Assumes a
 * full 64x64 panel — callers fall back to a text label when the panel is
 * smaller than the icon's fixed layout. */
static void DrawRadioIcon(MrApp *app, struct RastPort *rp,
	int originX, int originY)
{
	int bx0 = originX + 10, by0 = originY + 34;
	int bx1 = originX + 54, by1 = originY + 58;
	struct ColorMap *cm;
	LONG obtained;
	UWORD fgPen = ArtFallbackPen(app, &cm, &obtained);

	SetAPen(rp, fgPen);

	Move(rp, bx0 + 10, by0);
	Draw(rp, bx0 + 10, by0 - 12);
	Draw(rp, bx1 - 10, by0 - 12);
	Draw(rp, bx1 - 10, by0);

	Move(rp, bx1 - 8, by0);
	Draw(rp, bx1 + 4, by0 - 16);
	RectFill(rp, bx1 + 2, by0 - 18, bx1 + 6, by0 - 14);

	Move(rp, bx0, by0);
	Draw(rp, bx1, by0);
	Draw(rp, bx1, by1);
	Draw(rp, bx0, by1);
	Draw(rp, bx0, by0);

	ArtIconFillCircle(rp, bx0 + 12, by0 + 12, 7);
	SetAPen(rp, 0);
	ArtIconFillCircle(rp, bx0 + 12, by0 + 12, 4);

	SetAPen(rp, (UWORD)fgPen);
	ArtIconFillCircle(rp, bx1 - 10, by0 + 12, 4);

	if (obtained >= 0)
		ReleasePen(cm, obtained);
}

/* Drawn when a local/offline file has no embedded artwork: a simple eighth
 * note (filled head, stem, flag), tinted with the per-track random colour.
 * Parity with the GadTools frontend's DrawMusicNoteIcon; assumes a full
 * 64x64 panel like DrawRadioIcon. */
static void DrawMusicNoteIcon(MrApp *app, struct RastPort *rp,
	int originX, int originY)
{
	int headCx = originX + 24;
	int headCy = originY + 46;
	int headR = 8;
	int stemX = headCx + headR - 1;
	int stemTopY = originY + 12;
	struct ColorMap *cm;
	LONG obtained;
	UWORD fgPen = ArtFallbackPen(app, &cm, &obtained);

	SetAPen(rp, fgPen);
	ArtIconFillCircle(rp, headCx, headCy, headR);

	Move(rp, stemX, headCy);
	Draw(rp, stemX, stemTopY);

	Draw(rp, stemX + 12, stemTopY + 8);
	Draw(rp, stemX, stemTopY + 16);
	Draw(rp, stemX, stemTopY);

	if (obtained >= 0)
		ReleasePen(cm, obtained);
}

static void DrawArtPanel(MrApp *app)
{
	struct Gadget *gad;
	struct RastPort *rp;
	int availW, availH, w, h, ox, oy, x, y;
	static const unsigned char bayer[8][8] = {
		{0,32,8,40,2,34,10,42},{48,16,56,24,50,18,58,26},{12,44,4,36,14,46,6,38},{60,28,52,20,62,30,54,22},
		{3,35,11,43,1,33,9,41},{51,19,59,27,49,17,57,25},{15,47,7,39,13,45,5,37},{63,31,55,23,61,29,53,21}
	};
	if (!app || !app->win || !app->artGad) return;
	gad = (struct Gadget *)app->artGad;
	rp = app->win->RPort;
	/* The artwork placeholder button reserves the rectangle in the layout;
	 * its laid-out bounds tell us where to paint.  Bail out until the layout
	 * has actually sized it (early refreshes can fire with a zero rect). */
	if (gad->Width <= 8 || gad->Height <= 8) return;
	availW = gad->Width - 8; availH = gad->Height - 8;
	w = availW < MR_ART_W ? availW : MR_ART_W;
	h = availH < MR_ART_H ? availH : MR_ART_H;
	if (w <= 0 || h <= 0) return;
	ox = gad->LeftEdge + 4 + (availW - w) / 2;
	oy = gad->TopEdge + 4 + (availH - h) / 2;

	/* Recessed frame drawn without GadTools so the panel remains visible even
	 * before any artwork has decoded. */
	{
		int frameShinePen = 1, frameShadowPen = 2, frameBackPen = 0;
		struct DrawInfo *dri = GetScreenDrawInfo(app->win->WScreen);
		if (dri) {
			frameShinePen = dri->dri_Pens[SHINEPEN];
			frameShadowPen = dri->dri_Pens[SHADOWPEN];
			frameBackPen = dri->dri_Pens[BACKGROUNDPEN];
			FreeScreenDrawInfo(app->win->WScreen, dri);
		}
		SetAPen(rp, (UWORD)frameBackPen);
		RectFill(rp, ox - 2, oy - 2, ox + w + 1, oy + h + 1);
		SetAPen(rp, (UWORD)frameShadowPen);
		Move(rp, ox - 2, oy - 2);
		Draw(rp, ox + w + 1, oy - 2);
		Move(rp, ox - 2, oy - 2);
		Draw(rp, ox - 2, oy + h + 1);
		SetAPen(rp, (UWORD)frameShinePen);
		Move(rp, ox - 1, oy + h + 1);
		Draw(rp, ox + w + 1, oy + h + 1);
		Move(rp, ox + w + 1, oy - 1);
		Draw(rp, ox + w + 1, oy + h + 1);
	}

	if (app->artValid) {
		int pens[3] = {0, 1, 1};
		struct DrawInfo *dri = GetScreenDrawInfo(app->win->WScreen);
		if (dri) {
			pens[0] = dri->dri_Pens[SHADOWPEN];
			pens[1] = dri->dri_Pens[BACKGROUNDPEN];
			pens[2] = dri->dri_Pens[SHINEPEN];
			FreeScreenDrawInfo(app->win->WScreen, dri);
		}
		for (y = 0; y < h; y++) {
			int yy = (y * MR_ART_H) / h;
			for (x = 0; x < w; x++) {
				int xx = (x * MR_ART_W) / w;
				int idx = yy * MR_ART_W + xx;
				if (app->artColorEnabled && app->artPensBuilt &&
					app->artPenIdx[idx] < app->artPenCacheUsed &&
					app->artPenCache[app->artPenIdx[idx]].pen >= 0) {
					SetAPen(rp, (UWORD)app->artPenCache[app->artPenIdx[idx]].pen);
				} else {
					int g = app->artGreyBuf[idx] + (((int)bayer[yy & 7][xx & 7] - 32) * 3 / 4);
					SetAPen(rp, (UWORD)pens[g >= 171 ? 2 : (g >= 85 ? 1 : 0)]);
				}
				WritePixel(rp, ox + x, oy + y);
			}
		}
	} else {
		/* Paint our own placeholder panel rather than relying on the button's
		 * text, which the layout can repaint over (leaving it blank).  For
		 * radio input, label it with what we actually tried so it's visible
		 * at a glance whether the favicon pipeline ran at all: "Blank" when
		 * the station has no favicon URL, or "No art" plus the rejected file
		 * type, so a station icon swap is visibly noticed even when nothing
		 * renders.  The panel is only MR_ART_W (64px) wide, too narrow for
		 * "No art (WEBP)" on one line, so fall back to a second line for the
		 * extension rather than silently dropping the label when it
		 * wouldn't fit. */
		char line1[16], line2[16];
		int line1Len, line2Len, line1W, line2W;
		int iconKind = 0; /* 0=text label, 1=radio boombox, 2=music note */
		line2[0] = '\0';
		if (MrIsRadioInput(app->inputName)) {
			/* Any radio stream with no usable artwork gets the hand-drawn
			 * boombox placeholder (parity with the GadTools frontend) --
			 * whether the station advertised no favicon at all or one was
			 * fetched and failed to load (404, unsupported/broken format).
			 * line1/line2 are still filled in so a panel too small for the
			 * fixed-size icon degrades to a text label instead. */
			iconKind = 1;
			if (!app->currentRadioFavicon[0]) {
				SafeCopy(line1, sizeof(line1), "Blank");
			} else {
				char ext[16];
				MrUrlExtensionUpper(app->currentRadioFavicon, ext, sizeof(ext));
				SafeCopy(line1, sizeof(line1), "No art");
				if (ext[0]) sprintf(line2, "(%s)", ext);
			}
		} else {
			/* Local/offline file with no embedded artwork: music-note
			 * placeholder (parity with the GadTools frontend), degrading to
			 * the text label when the panel is too small for the icon. */
			iconKind = 2;
			SafeCopy(line1, sizeof(line1), "No art");
		}
		SetAPen(rp, 0);
		RectFill(rp, ox, oy, ox + w - 1, oy + h - 1);
		/* The icon uses a fixed 64x64 layout; only draw it when the panel is
		 * at least that big, otherwise fall through to the text label. */
		if (iconKind && w >= MR_ART_W && h >= MR_ART_H) {
			if (iconKind == 1)
				DrawRadioIcon(app, rp, ox, oy);
			else
				DrawMusicNoteIcon(app, rp, ox, oy);
			return;
		}
		line1Len = (int)strlen(line1);
		line2Len = (int)strlen(line2);
		line1W = line1Len > 0 ? TextLength(rp, line1, line1Len) : 0;
		line2W = line2Len > 0 ? TextLength(rp, line2, line2Len) : 0;
		SetAPen(rp, 1);
		if (line2Len > 0 && h >= 18) {
			/* Two centered lines: label on top, extension underneath. */
			if (line1W <= w) {
				Move(rp, ox + (w - line1W) / 2, oy + h / 2 - 2);
				Text(rp, line1, line1Len);
			}
			if (line2W <= w) {
				Move(rp, ox + (w - line2W) / 2, oy + h / 2 + 9);
				Text(rp, line2, line2Len);
			}
		} else if (line1Len > 0 && h >= 8 && line1W <= w) {
			Move(rp, ox + (w - line1W) / 2, oy + h / 2 + 2);
			Text(rp, line1, line1Len);
		}
	}
}

static void UpdateTimeDisplay(MrApp *app)
{
	char e[8], t[8], buf[24];
	if (MrIsRadioInput(app->inputName)) {
		sprintf(buf, "Live");
	} else {
		FormatTime(app->elapsedSecs, e); FormatTime(app->totalSecs > 0 ? app->totalSecs : -1, t);
		sprintf(buf, "%s / %s", e, t);
	}
	if (app->timeGad && app->win) SetGadgetAttrs((struct Gadget *)app->timeGad, app->win, NULL, STRINGA_TextVal, (ULONG)buf, TAG_DONE);
}

#define MR_SEEK_STEP_SECS 10

/*
 * Fast-forward / rewind for the ReAction frontend.  Hands a target position to
 * the playback child through the shared gSeek* channel (defined in
 * amiga_mp3dec.c, which this file #includes).  Only local tracks of known
 * length can seek; live radio and unknown-duration inputs are rejected.  The
 * read-out is nudged at once and the next status tick re-derives the exact
 * position from the decoder's frame count.
 */
static void MrSeekRelative(MrApp *app, int deltaSecs)
{
	int target;
	char buf[48];

	if (!app->playbackActive || app->playbackDonePending) {
		SetStatus(app, "Nothing playing to seek.");
		return;
	}
	if (MrIsRadioInput(app->inputName)) {
		SetStatus(app, "Cannot seek a live radio stream.");
		return;
	}
	if (app->totalSecs <= 0) {
		SetStatus(app, "Track length unknown - cannot seek.");
		return;
	}

	target = app->elapsedSecs + deltaSecs;
	if (target < 0)
		target = 0;
	if (target > app->totalSecs)
		target = app->totalSecs;

	gSeekTargetSecs = target;
	gSeekRequest = 1;

	app->elapsedSecs = target;
	UpdateTimeDisplay(app);
	SetGauge(app, (app->progressEnabled && app->totalSecs > 0) ?
		(target * 100) / app->totalSecs : 0);

	sprintf(buf, "%s to %02d:%02d",
		deltaSecs < 0 ? "Rewind" : "Fast-forward", target / 60, target % 60);
	SetStatus(app, buf);
}

static void UpdateRatingDisplay(MrApp *app)
{
	char buf[16]; int i;
	sprintf(buf, "%d/5", app->rating);
	if (app->ratingGad && app->win) SetGadgetAttrs((struct Gadget *)app->ratingGad, app->win, NULL, STRINGA_TextVal, (ULONG)buf, TAG_DONE);
	for (i = 0; i < 5; i++) if (app->starGad[i] && app->win) SetGadgetAttrs((struct Gadget *)app->starGad[i], app->win, NULL, GA_Text, (ULONG)(i < app->rating ? "*" : "-"), TAG_DONE);
}

static const char *MrChannelModeName(const MrMp3Info *info)
{
	if (!info || info->channels <= 0) return "-";
	if (info->channels == 1) return "mono";
	if (info->channelMode == 1) return (info->modeExtension & 0x02) ? "M/S" : "joint-stereo";
	return "stereo";
}

static void RefreshFileInfoAndTags(MrApp *app)
{
	MrMp3Info info; char fileInfo[128]; const char *ch; unsigned long kb;
	UpdateFastMemGadgetState(app);
	if (!app->inputName[0]) {
		SetReadonlyString(app->fileInfoGad, app->win, app->shownFileInfo, sizeof(app->shownFileInfo), "No file info");
		return;
	}
	if (MrIsRadioInput(app->inputName)) {
		app->rating = 0; app->totalSecs = 0; app->elapsedSecs = 0; app->lastFrames = 0;
		SetReadonlyString(app->titleGad, app->win, app->shownTitle, sizeof(app->shownTitle), "Internet Radio");
		SetReadonlyString(app->artistGad, app->win, app->shownArtist, sizeof(app->shownArtist), "-");
		SetAlbumDisplay(app, "Internet Radio");
		SetReadonlyString(app->trackGad, app->win, app->shownTrack, sizeof(app->shownTrack), "Live");
		SetReadonlyString(app->genreGad, app->win, app->shownGenre, sizeof(app->shownGenre), "-");
		SetReadonlyString(app->fileInfoGad, app->win, app->shownFileInfo, sizeof(app->shownFileInfo), "Internet Stream");
		UpdateRatingDisplay(app); UpdateTimeDisplay(app); SetGauge(app, 0);
		/* Radio has no local MP3/ID3 art, only an optional station favicon,
		 * so this is the only place that drives LoadRadioFaviconImage() for
		 * radio input -- without it, switching stations (or "Reload Art
		 * from File") never re-attempts the favicon fetch, and any layout
		 * repaint of the placeholder gadget is never redrawn. */
		UpdateArtwork(app, NULL);
		SetStatus(app, app->artValid ? "Internet Radio ready." : "Internet Radio ready (No art).");
		return;
	}
	ReadMp3Info(app->inputName, &info);
	app->rating = info.rating; app->totalSecs = info.durationSecs; app->elapsedSecs = 0; app->lastFrames = 0;
	ch = MrChannelModeName(&info);
	kb = (info.fileSize + 1023UL) / 1024UL;
	/* Same one-line summary the GadTools frontend shows; the track length is
	 * presented separately in the Time field rather than crammed in here. */
	fileInfo[0] = '\0';
	if (info.bitrateKbps > 0 || info.sampleRate > 0 || info.fileSize > 0)
		sprintf(fileInfo, "%d kbps, %s, %d Hz, %lu KB",
			info.bitrateKbps, ch, info.sampleRate, kb);
	else
		SafeCopy(fileInfo, sizeof(fileInfo), "-");
	SetReadonlyString(app->titleGad, app->win, app->shownTitle, sizeof(app->shownTitle), info.title[0] ? info.title : "-");
	SetReadonlyString(app->artistGad, app->win, app->shownArtist, sizeof(app->shownArtist), info.artist[0] ? info.artist : "-");
	SetAlbumDisplay(app, info.album[0] ? info.album : "-");
	SetReadonlyString(app->trackGad, app->win, app->shownTrack, sizeof(app->shownTrack), info.track[0] ? info.track : "-");
	SetReadonlyString(app->genreGad, app->win, app->shownGenre, sizeof(app->shownGenre), info.genre[0] ? info.genre : "-");
	SetReadonlyString(app->fileInfoGad, app->win, app->shownFileInfo, sizeof(app->shownFileInfo), fileInfo);
	UpdateRatingDisplay(app); UpdateTimeDisplay(app); SetGauge(app, 0); UpdateArtwork(app, &info); SetStatus(app, app->artValid ? "File ready." : "File ready (No art).");
	FreeMp3Info(&info);
}

static void ApplyHardwareAudioFilter(MrApp *app)
{
#ifdef MR_ENABLE_CIA_FILTER
	volatile UBYTE *ciapra = (volatile UBYTE *)0xbfe001;

	if (app && app->hardwareFilter)
		*ciapra &= (UBYTE)~CIAF_LED;
	else
		*ciapra |= (UBYTE)CIAF_LED;
#else
	(void)app;
#endif
}

/* ------------------------------------------------------------------------- */
/* Reading the current gadget values back into app state                     */
/* ------------------------------------------------------------------------- */


static void BrowseForFile(MrApp *app)
{
struct FileRequester *fr;
char path[MR_MAX_PATH];

fr = (struct FileRequester *)AllocAslRequestTags(ASL_FileRequest,
ASLFR_TitleText, (ULONG)"Choose an audio file or playlist",
ASLFR_DoPatterns, TRUE,
ASLFR_InitialPattern, (ULONG)"#?.(mp3|flac|aac|ogg|oga|wav|wma|8svx|iff|svx|aif|aiff|m3u|m3u8|pls)",
ASLFR_InitialDrawer, (ULONG)(app->lastDrawer[0] ? app->lastDrawer : NULL),
TAG_DONE);

if (!fr) {
SetStatus(app, "Could not allocate file requester.");
return;
}

if (AslRequestTags(fr,
ASLFR_Window, (ULONG)app->win,
ASLFR_SleepWindow, TRUE,
TAG_DONE)) {
path[0] = '\0';

if (fr->fr_Drawer && fr->fr_Drawer[0])
SafeCopy(path, sizeof(path), (const char *)fr->fr_Drawer);

if (fr->fr_File && fr->fr_File[0]) {
if (!AddPart((STRPTR)path, fr->fr_File, sizeof(path))) {
SetStatus(app, "Selected path is too long.");
FreeAslRequest(fr);
return;
}

/* A playlist opens in the playlist, with its first entry selected. */
if (MrIsPlaylistFile(path)) {
CopyDrawerFromPath(app->lastDrawer, sizeof(app->lastDrawer), path);
SaveSettings(app);
LoadPlaylistPath(app, path, (const char *)fr->fr_Drawer);
FreeAslRequest(fr);
return;
}

SafeCopy(app->inputName, sizeof(app->inputName), path);
CopyDrawerFromPath(app->lastDrawer, sizeof(app->lastDrawer), app->inputName);
SaveSettings(app);

MrSetGadgetAttr(app->fileGad, app->win, GETFILE_FullFile, (ULONG)app->inputName);

app->playlistCount = 0;
app->playlistCurrent = -1;
RefreshPlaylistView(app);
UpdateNextButtonState(app);
RefreshFileInfoAndTags(app);
}
}

FreeAslRequest(fr);
}


static void UpdateFileGadget(MrApp *app)
{
	MrSetGadgetAttr(app->fileGad, app->win, GETFILE_FullFile, (ULONG)app->inputName);
}

static void PlaylistNext(MrApp *app)
{
	int wasPlaying = app->playbackActive;
	if (app->playbackDonePending || gPlayer.stopRequested) {
		SetStatus(app, "Previous playback is still exiting.");
		UpdateNextButtonState(app);
		return;
	}
	if (app->playlistCount <= 0 || app->playlistCurrent + 1 >= app->playlistCount) {
		SetStatus(app, "No next playlist item.");
		return;
	}
	app->playlistCurrent++;
	SafeCopy(app->inputName, sizeof(app->inputName), app->playlist[app->playlistCurrent]);
	UpdateFileGadget(app);
	RefreshFileInfoAndTags(app);
	if (wasPlaying) {
		app->playlistNextPending = 1;
		StopPlayback(app);
	} else {
		SetStatus(app, "Playlist item selected.");
		PlaylistStartCurrent(app);
	}
}

static void TrimLine(char *s)
{
	char *e;
	while (*s == ' ' || *s == '\t')
		memmove(s, s + 1, strlen(s));
	e = s + strlen(s);
	while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t'))
		*--e = '\0';
}

/* Appends one entry; a location that does not fit is skipped rather than
 * cut short (a truncated URL or path would only fail later). */
static int PlaylistAddEntry(MrApp *app, const char *location, const char *title)
{
	int n = app->playlistCount;
	if (n >= MR_PLAYLIST_MAX || !location || !location[0] || strlen(location) >= MR_MAX_PATH)
		return 0;
	SafeCopy(app->playlist[n], MR_MAX_PATH, location);
	SafeCopy(app->playlistTitles[n], sizeof(app->playlistTitles[n]), title ? title : "");
#ifdef MINTAMP_SKIN
    app->playlistDurations[n]=-1;
#endif
	app->playlistCount++;
	return 1;
}

typedef struct MrPlaylistLoad {
	MrApp *app;
	const char *drawer;
	int skipped;
#ifdef MINTAMP_SKIN
    int before;
#endif
} MrPlaylistLoad;

static int MrPlaylistLoadEntry(void *ctx, const char *location, const char *title)
{
	MrPlaylistLoad *load = (MrPlaylistLoad *)ctx;
	char full[MR_MAX_PATH];
	char shown[PLAYLIST_TITLE_MAX];
#ifdef MINTAMP_SKIN
    load->before=load->app->playlistCount;
#endif
	if (load->app->playlistCount >= MR_PLAYLIST_MAX)
		return 0;
	/* Downloaded playlists are usually UTF-8; the location stays as is. */
	AmigaUtf8ToDisplay(shown, sizeof(shown), title);
	title = shown;
	/* URLs and absolute Amiga paths as written; anything else is relative
	 * to the playlist's drawer. */
	if (strchr(location, ':') || location[0] == '/') {
		if (!PlaylistAddEntry(load->app, location, title))
			load->skipped++;
		return 1;
	}
	SafeCopy(full, sizeof(full), load->drawer ? load->drawer : "");
	if (!AddPart((STRPTR)full, (STRPTR)location, sizeof(full)) ||
		!PlaylistAddEntry(load->app, full, title))
		load->skipped++;
	return 1;
}

#ifdef MINTAMP_SKIN
static void MrPlaylistDuration(void *ctx,int seconds)
{ MrPlaylistLoad *load=(MrPlaylistLoad *)ctx; if (load->app->playlistCount>load->before) load->app->playlistDurations[load->app->playlistCount-1]=seconds; }
#endif
/* Reads a whole small text file into a NUL-terminated malloc()ed buffer. */
static char *MrReadTextFile(const char *path, long maxBytes, long *lenOut)
{
	BPTR fh;
	char *buf;
	long len = 0, got;
	*lenOut = 0;
	fh = Open((STRPTR)path, MODE_OLDFILE);
	if (!fh)
		return NULL;
	buf = (char *)malloc((size_t)maxBytes + 1);
	if (buf) {
		while (len < maxBytes && (got = Read(fh, buf + len, maxBytes - len)) > 0)
			len += got;
		buf[len] = '\0';
		*lenOut = len;
	}
	Close(fh);
	return buf;
}

/* Loads an M3U or PLS playlist, replacing the current one. */
static void LoadPlaylistPath(MrApp *app, const char *m3uPath, const char *drawer)
{
	MrPlaylistLoad load;
	char *text;
	long len;
	app->playlistCount = 0;
	app->playlistCurrent = -1;
	app->playlistSelected = -1;
	text = MrReadTextFile(m3uPath, 96L * 1024L, &len);
	if (!text) {
		SetStatus(app, "Could not open playlist.");
		return;
	}
	load.app = app;
	load.drawer = drawer;
	load.skipped = 0;
	#ifdef MINTAMP_SKIN
    playlist_parse_ex(text,(size_t)len,MrPlaylistLoadEntry,&load,MrPlaylistDuration);
#else
    playlist_parse(text, (size_t)len, MrPlaylistLoadEntry, &load);
#endif
	free(text);
	if (app->playlistCount > 0) {
		app->playlistCurrent = 0;
		app->playlistSelected = 0;
		SafeCopy(app->inputName, sizeof(app->inputName), app->playlist[0]);
		UpdateFileGadget(app);
		RefreshFileInfoAndTags(app);
		RefreshPlaylistView(app);
		UpdateNextButtonState(app);
		if (load.skipped > 0)
			SetStatus(app, "Playlist loaded; some entries were too long or did not fit.");
		else
			SetStatus(app, app->artValid ? "Playlist loaded." : "Playlist loaded (No art).");
	} else {
		app->playlistSelected = -1;
		RefreshPlaylistView(app);
		UpdateNextButtonState(app);
		SetStatus(app, "Playlist had no playable entries.");
	}
}

enum {
	PL_GID_LIST = 200,
	PL_GID_ADD,
	PL_GID_REMOVE,
	PL_GID_CLEAR,
	PL_GID_PLAY,
	PL_GID_LOAD_M3U,
	PL_GID_SAVE_M3U,
	PL_GID_CLOSE,
	PL_GID_URL,
	PL_GID_ADD_URL,
	PL_GID_FROM_FAVS,
	PL_GID_TO_FAVS
};

enum {
	RB_GID_SEARCH_TEXT = 300,
	RB_GID_CODEC,
	RB_GID_COUNTRY,
	RB_GID_COUNTRY_CODE,
	RB_GID_RADIO_RESULTS,
	RB_GID_SEARCH,
	RB_GID_PROBE,
	RB_GID_ADD_FAV,
	RB_GID_FAVOURITES,
	RB_GID_CLOSE,
	RB_GID_SCHEME,
	RB_GID_LIMIT,
	RB_GID_BITRATE,
	RB_GID_UP,
	RB_GID_DOWN,
	RB_GID_STATUS,
	RB_GID_FAV_NAME,
	RB_GID_FAV_RENAME,
	RB_GID_FAV_REMOVE,
	RB_GID_FAV_MOVE_UP,
	RB_GID_FAV_MOVE_DOWN
};


static const int kRadioSearchLimits[] = { 10, 25, 50, 100 };
static STRPTR kRadioSearchLimitLabels[] = { (STRPTR)"10", (STRPTR)"25", (STRPTR)"50", (STRPTR)"100", NULL };
#define MR_RADIO_SEARCH_LIMIT_COUNT ((int)(sizeof(kRadioSearchLimits) / sizeof(kRadioSearchLimits[0])))

/* Maps a limit value back to its kRadioSearchLimits[] cycle-gadget index;
 * picks the closest entry rather than the first/last bucket so adding more
 * limit choices doesn't need a hand-maintained chain of >=/<= comparisons. */
static int RadioSearchLimitIndex(int limit)
{
	int best = 0, bestDist, i;
	bestDist = limit > kRadioSearchLimits[0] ? limit - kRadioSearchLimits[0] : kRadioSearchLimits[0] - limit;
	for (i = 1; i < MR_RADIO_SEARCH_LIMIT_COUNT; i++) {
		int dist = limit > kRadioSearchLimits[i] ? limit - kRadioSearchLimits[i] : kRadioSearchLimits[i] - limit;
		if (dist < bestDist) { bestDist = dist; best = i; }
	}
	return best;
}
static const int kRadioBitrateMax[] = { -1, 56, 64, 96, 128 };
static STRPTR kRadioBitrateLabels[] = { (STRPTR)"Any", (STRPTR)"<=56", (STRPTR)"<=64", (STRPTR)"<=96", (STRPTR)"<=128", NULL };
static STRPTR kRadioSchemeLabels[] = { (STRPTR)"HTTP", (STRPTR)"HTTPS", (STRPTR)"All", NULL };
static STRPTR kRadioCountryLabels[] = { (STRPTR)"All", (STRPTR)"GB", (STRPTR)"US", (STRPTR)"FR", (STRPTR)"ZA", (STRPTR)"DE", (STRPTR)"NL", NULL };

static const char *RadioCountryFromIndex(int idx)
{
	switch (idx) {
	case 1: return "GB";
	case 2: return "US";
	case 3: return "FR";
	case 4: return "ZA";
	case 5: return "DE";
	case 6: return "NL";
	default: return "";
	}
}

static int RadioCountryToIndex(const char *countrycode)
{
	int i;
	if (!countrycode || !countrycode[0]) return 0;
	for (i = 1; kRadioCountryLabels[i]; i++)
		if (!strcmp(countrycode, (const char *)kRadioCountryLabels[i])) return i;
	return 0;
}

static const char *RadioBitrateFilterLabel(int max_bitrate)
{
	static char label[16];

	if (max_bitrate <= 0) return "Any";
	sprintf(label, "<=%d", max_bitrate);
	return label;
}

static const char *RadioCodecFromIndex(int idx)
{
	switch (idx) {
	case 1: return "MP3";
	case 2: return "AAC";
	case 3: return "AAC+";
	case 4: return "FLAC";
	default: return "";
	}
}

static int RadioCodecToIndex(const char *codec)
{
	if (!codec || !codec[0]) return 0;
	if (!strcmp(codec, "MP3")) return 1;
	if (!strcmp(codec, "AAC")) return 2;
	if (!strcmp(codec, "AAC+")) return 3;
	if (!strcmp(codec, "FLAC")) return 4;
	return 0;
}

static const char *ProbeCodecName(RbStreamCodec codec)
{
	if (codec == RB_STREAM_CODEC_MP3) return "MP3";
	if (codec == RB_STREAM_CODEC_AAC) return "AAC";
	if (codec == RB_STREAM_CODEC_OGG) return "OGG";
	if (codec == RB_STREAM_CODEC_FLAC) return "FLAC";
	return "unknown";
}

static void RadioSetStatus(MrApp *app, const char *text)
{
	if (!app) return;
	SafeCopy(app->lastRadioError, sizeof(app->lastRadioError), text ? text : "");
	if (app->rbWin && app->rbStatusGad)
		SetGadgetAttrs((struct Gadget *)app->rbStatusGad, app->rbWin, NULL,
			STRINGA_TextVal, (ULONG)app->lastRadioError, TAG_DONE);
}

static void RadioProbeUrlAndStart(MrApp *app, const char *url, const char *stationName)
{
	static unsigned char peek[RB_PROBE_PEEK_SIZE];
	static char unwrapped[512];
	RbStreamInfo info;
	int peekLen = 0;
	int rc;
	char msg[256];
	const char *name;
	const char *err;

	if (!app) return;
	if (!url || !url[0]) {
		RadioSetStatus(app, "No stream URL to play.");
		SetStatus(app, "No stream URL to play.");
		return;
	}
	if (rb_probe_unwrap_playlist_generator(url, unwrapped, (int)sizeof(unwrapped)))
		url = unwrapped;
	if (!MrIsRadioInput(url)) {
		RadioSetStatus(app, "Stream URL must start with HTTP or HTTPS.");
		SetStatus(app, "Stream URL must start with HTTP or HTTPS.");
		return;
	}
	if (!app->hasHttps && !strncmp(url, "https://", 8)) {
		RadioSetStatus(app, "HTTPS not supported in this build");
		SetStatus(app, "HTTPS not supported in this build");
		return;
	}
	if (rb_probe_url_looks_hls(url)) {
		RadioSetStatus(app, "HLS stream not supported");
		SetStatus(app, "HLS stream not supported");
		return;
	}
	if (Radio_PlaybackOwnsNetwork()) {
		RADIO_DBG(printf("radio-ui: direct/favourite probe skipped while radio playback child owns networking url=\"%s\"\n", url);)
		RadioSetStatus(app, "Radio playback owns networking; stop before probing another stream.");
		SetStatus(app, "Radio playback owns networking; stop before probing another stream.");
		return;
	}
	if (Radio_IsMemoryPoisoned()) {
		RadioSetStatus(app, "Memory corruption detected. Save log and reboot before using MintAMP again.");
		SetStatus(app, "Memory corruption detected. Save log and reboot before using MintAMP again.");
		RADIO_DBG(printf("radio-memory: refusing direct/favourite probe after MiniMem/ring corruption url=\"%s\"\n", url);)
		return;
	}

	app->haveRadioHostAddr = 0;
	app->radioHostAddrBe = 0;
	memset(&info, 0, sizeof(info));
	RadioSetStatus(app, "Connecting...");
	SetStatus(app, "Connecting...");
	Radio_LogTestModeSummary();
	RADIO_DBG(printf("radio-ui: direct/favourite probe start url=\"%s\" name=\"%s\"\n", url, stationName ? stationName : "");)
	rc = rb_probe_stream_url(url, &info, peek, (int)sizeof(peek), &peekLen);
	RADIO_DBG(printf("radio-ui: direct/favourite probe result rc=%d final=\"%s\" content=\"%s\" codec=%d redirects=%d\n",
		rc, info.final_url, info.content_type, (int)info.codec, info.redirect_count);)
	if (rc < 0) {
		char emsg[256];
		err = rb_probe_error_text(rc);
		if (info.error_detail[0]) {
			sprintf(emsg, "%.150s [%.90s]", err, info.error_detail);
			RadioSetStatus(app, emsg);
			SetStatus(app, emsg);
		} else {
			RadioSetStatus(app, err);
			SetStatus(app, err);
		}
		return;
	}
	if (info.codec != RB_STREAM_CODEC_MP3 && info.codec != RB_STREAM_CODEC_AAC &&
		info.codec != RB_STREAM_CODEC_OGG && info.codec != RB_STREAM_CODEC_FLAC) {
		sprintf(msg, "Unsupported stream codec: %s (%.48s)", ProbeCodecName(info.codec), info.content_type);
		RadioSetStatus(app, msg);
		SetStatus(app, msg);
		return;
	}
	if (!info.final_url[0]) {
		RadioSetStatus(app, "Stream probe did not return a playable URL.");
		SetStatus(app, "Stream probe did not return a playable URL.");
		return;
	}
#if defined(AMIGA_M68K)
	if (app->lastCompletedWasHttps && !strncmp(info.final_url, "https://", 8) &&
		!app->playbackActive && !app->playbackDonePending && !PlaybackProcessStillExists()) {
		/* Settle gap before the next HTTPS stream so the previous session's
		 * AmiSSL/socket teardown can finish; user-tunable via the Playback
		 * "HTTPS wait" menu (index 0 keeps the historical ~80 ms default). */
		int waitTicks = kHttpsWaitTicks[(app->httpsWaitIndex >= 0 &&
			app->httpsWaitIndex < HTTPS_WAIT_COUNT) ? app->httpsWaitIndex : 0];
		RADIO_DBG(printf("radio-done: Delay(%d) between fully completed HTTPS sessions before starting next HTTPS URL\n", waitTicks);)
		RadioSetStatus(app, "Waiting briefly before next HTTPS stream...");
		Delay(waitTicks);
	}
#endif
	if (url != app->currentRadioSourceUrl)
		SafeCopy(app->currentRadioSourceUrl, sizeof(app->currentRadioSourceUrl), url);
	SafeCopy(app->inputName, sizeof(app->inputName), info.final_url);
	app->haveRadioHostAddr = info.have_host_addr;
	app->radioHostAddrBe = info.host_addr_be;
	/* Preserve the station name and favicon when this is a Play/replay of the
	 * same stream (no station name supplied and the probed URL matches the one
	 * the current artwork belongs to). Otherwise -- a named start, or a
	 * different URL typed in -- establish a fresh art context. Without this,
	 * hitting Play after Stop reset the name to "Internet Radio" and cleared the
	 * favicon, so the artwork never reloaded (cache key no longer matched and
	 * there was no favicon URL to re-fetch). */
	if ((stationName && stationName[0]) ||
	    !app->currentRadioStationName[0] ||
	    !app->currentRadioArtUrl[0] ||
	    strcmp(info.final_url, app->currentRadioArtUrl) != 0) {
		app->currentRadioFavicon[0] = '\0';
		SafeCopy(app->currentRadioStationName,
			sizeof(app->currentRadioStationName),
			(stationName && stationName[0]) ? stationName : "Internet Radio");
		SafeCopy(app->currentRadioArtUrl,
			sizeof(app->currentRadioArtUrl), info.final_url);
	}
	name = app->currentRadioStationName[0] ?
		app->currentRadioStationName : "Internet Radio";
	UpdateFileGadget(app);
	RefreshFileInfoAndTags(app);
	sprintf(msg, "Buffering - %.140s", name);
	RadioSetStatus(app, msg);
	SetStatus(app, msg);
	RADIO_DBG(printf("radio-ui: direct/favourite stream start url=\"%s\" hostCached=%d\n", info.final_url, info.have_host_addr);)
	StartPlayback(app);
	Radio_CheckMiniMem("after direct/favourite station switch");
}


static int RadioStationMatchesScheme(MrApp *app, const RadioBrowserStation *st)
{
	const char *url = rb_station_play_url(st);
	int isHttp, isHttps;
	if (!url) return 0;
	isHttp = strncmp(url, "http://", 7) == 0;
	isHttps = strncmp(url, "https://", 8) == 0;
	if (app && app->rbSchemeMode == 1) {
#if defined(HAVE_AMISSL)
		return isHttps;
#else
		return 0;
#endif
	}
	if (app && app->rbSchemeMode == 2) {
#if defined(HAVE_AMISSL)
		return isHttp || isHttps;
#else
		return isHttp;
#endif
	}
	return isHttp;
}

static void RadioRefreshResultsList(MrApp *app)
{
	int i, row;
	int selectedRow = -1;
	int wantedController = -1;
	int wantedFavourite = -1;
	char display[RB_MAX_NAME];
	const RadioBrowserStation *st;
	const char *url;
	const char *reason;
	if (!app) return;
	if (app->rbShowingFavourites)
		wantedFavourite = app->rbSelectedFavourite;
	else
		wantedController = app->rbController.selected_index;
	if (app->rbWin && app->rbListGad)
		SetGadgetAttrs((struct Gadget *)app->rbListGad, app->rbWin, NULL,
			LISTBROWSER_Labels, (ULONG)~0,
			LISTBROWSER_Selected, (ULONG)~0,
			TAG_DONE);
	if (app->rbList.lh_Head) {
		/* The list must be detached from the gadget (LISTBROWSER_Labels ~0,
		 * done just above) before its nodes are freed here, then rebuilt.
		 * rbNodes[] is the only other reference to these nodes, so it is
		 * memset to 0 immediately after the free below -- a stale rbNodes
		 * entry pointing at a freed node is precisely the dangling owner that
		 * turns the next AddTail()/free into an AN_BadFreeAddr. */
		MR_FREE_BEGIN("RadioRefreshResults", "listbrowser-nodes",
			app->rbList.lh_Head, (unsigned long)app->rbVisibleCount);
		FreeListBrowserList(&app->rbList);
		MR_FREE_END("RadioRefreshResults", "listbrowser-nodes", (void *)0, 0);
	}
	NewList(&app->rbList);
	memset(app->rbNodes, 0, sizeof(app->rbNodes));
	app->rbVisibleCount = 0;
	app->rbSelectedFavourite = -1;
	if (app->rbShowingFavourites) {
		for (i = 0; i < app->rbFavouriteCount && app->rbVisibleCount < RB_CONTROLLER_MAX_STATIONS; i++) {
			if (!app->rbFavouriteNames[i][0] || !app->rbFavouriteUrls[i][0]) continue;
			row = app->rbVisibleCount++;
			app->rbVisibleToController[row] = i;
			sprintf(app->rbNames[row], "%.48s | favourite", app->rbFavouriteNames[i]);
			if (i == wantedFavourite) selectedRow = row;
			app->rbNodes[row] = AllocListBrowserNode(1,
				LBNA_Column, 0,
					LBNCA_Text, (ULONG)app->rbNames[row],
				TAG_DONE);
			if (app->rbNodes[row])
				AddTail(&app->rbList, app->rbNodes[row]);
		}
	} else {
		for (i = 0; i < app->rbController.station_count; i++) {
			st = rb_controller_get_station(&app->rbController, i);
			if (!st) continue;
			url = rb_station_play_url(st);
			reason = "show";
			if (!RadioStationMatchesScheme(app, st)) { reason = "hidden_scheme"; }
			else if (st->hls) { reason = "hidden_hls"; }
			else if (st->lastcheckok == 0) { reason = "hidden_offline"; }
			else if (st->ssl_error != 0) { reason = "hidden_ssl_error"; }
			else if (app->rbController.max_bitrate > 0 && st->bitrate == 0) { reason = "hidden_bitrate_unknown"; }
			else if (app->rbController.max_bitrate > 0 && st->bitrate > app->rbController.max_bitrate) { reason = "hidden_bitrate"; }
#ifdef MINIAMP3_DEBUG
			RADIO_DBG(printf("Radio Browser filter: name=\"%s\" scheme=%s codec=%s bitrate=%d max=%d country=%s hls=%d lastcheckok=%d ssl_error=%d reason=%s\n",
				st->name, url && strncmp(url, "https://", 8) == 0 ? "https" : (url && strncmp(url, "http://", 7) == 0 ? "http" : "other"),
				st->codec, st->bitrate, app->rbController.max_bitrate, st->countrycode,
				st->hls, st->lastcheckok, st->ssl_error, reason);)
#endif
			if (reason[0] != 's') continue;
			row = app->rbVisibleCount++;
			app->rbVisibleToController[row] = i;
			if (i == wantedController) selectedRow = row;
			rb_station_display_name(st, display, (int)sizeof(display));
			sprintf(app->rbNames[row], "%.48s | %s | %d | %s",
				display, st->codec, st->bitrate, st->countrycode);
			app->rbNodes[row] = AllocListBrowserNode(1,
				LBNA_Column, 0,
					LBNCA_Text, (ULONG)app->rbNames[row],
				TAG_DONE);
			if (app->rbNodes[row])
				AddTail(&app->rbList, app->rbNodes[row]);
		}
	}
	if (app->rbVisibleCount <= 0) {
		app->rbController.selected_index = -1;
		app->rbSelectedFavourite = -1;
		selectedRow = -1;
	} else if (selectedRow < 0) {
		selectedRow = 0;
		if (app->rbShowingFavourites)
			app->rbSelectedFavourite = app->rbVisibleToController[0];
		else
			rb_controller_set_selected(&app->rbController, app->rbVisibleToController[0]);
	} else if (app->rbShowingFavourites) {
		app->rbSelectedFavourite = app->rbVisibleToController[selectedRow];
	} else {
		rb_controller_set_selected(&app->rbController, app->rbVisibleToController[selectedRow]);
	}
	if (app->rbWin && app->rbListGad) {
		SetGadgetAttrs((struct Gadget *)app->rbListGad, app->rbWin, NULL,
			LISTBROWSER_Labels, (ULONG)&app->rbList,
			LISTBROWSER_Selected, selectedRow >= 0 ? (ULONG)selectedRow : (ULONG)~0,
			LISTBROWSER_ShowSelected, TRUE,
			LISTBROWSER_MakeVisible, selectedRow >= 0 ? (ULONG)selectedRow : 0,
			GA_Disabled, app->rbVisibleCount <= 0 ? TRUE : FALSE,
			TAG_DONE);
	}
}

static void RadioFavEditSync(MrApp *app);

static void RadioRefreshResults(MrApp *app)
{
	RadioRefreshResultsList(app);
	RadioFavEditSync(app);
}

static void RadioDoSearch(MrApp *app)
{
	STRPTR text = NULL;
	ULONG v = 0;
	int rc;
	char filterMsg[192];
	if (app->rbSearchInProgress) { RadioSetStatus(app, "Search already running."); return; }
	if (Radio_PlaybackOwnsNetwork()) {
		RADIO_DBG(printf("radio-browser: search skipped while radio playback child owns networking\n");)
		RadioSetStatus(app, "Radio playback owns networking; search after stopping.");
		return;
	}
	app->rbSearchInProgress = 1;
	RadioSetStatus(app, "Searching Radio Browser...");
	if (app->rbSearchGad) GetAttr(STRINGA_TextVal, app->rbSearchGad, (ULONG *)(void *)&text);
	SafeCopy(app->rbController.name, sizeof(app->rbController.name), text ? (const char *)text : "");
	v = 0; if (app->rbCodecGad) GetAttr(CHOOSER_Selected, app->rbCodecGad, &v);
	SafeCopy(app->rbController.codec, sizeof(app->rbController.codec), RadioCodecFromIndex((int)v));
	text = NULL; if (app->rbCountryGad) GetAttr(STRINGA_TextVal, app->rbCountryGad, (ULONG *)(void *)&text);
	SafeCopy(app->rbController.countrycode, sizeof(app->rbController.countrycode), text ? (const char *)text : "");
	v = 0; if (app->rbCountryCodeGad) GetAttr(CHOOSER_Selected, app->rbCountryCodeGad, &v);
	app->rbCountryMode = ClampInt((int)v, 0, 6);
	if (app->rbCountryMode > 0) SafeCopy(app->rbController.countrycode, sizeof(app->rbController.countrycode), RadioCountryFromIndex(app->rbCountryMode));
	v = 1; if (app->rbLimitGad) GetAttr(CHOOSER_Selected, app->rbLimitGad, &v);
	app->rbController.limit = kRadioSearchLimits[ClampInt((int)v, 0, MR_RADIO_SEARCH_LIMIT_COUNT - 1)];
	v = 0; if (app->rbBitrateGad) GetAttr(CHOOSER_Selected, app->rbBitrateGad, &v);
	app->rbController.max_bitrate = kRadioBitrateMax[ClampInt((int)v, 0, 4)];
	sprintf(filterMsg, "Search filters: name=\"%.40s\" codec=%s country=%s max bitrate=%s limit=%d", app->rbController.name[0] ? app->rbController.name : "Any", app->rbController.codec[0] ? app->rbController.codec : "Any", app->rbController.countrycode[0] ? app->rbController.countrycode : "Any", RadioBitrateFilterLabel(app->rbController.max_bitrate), app->rbController.limit);
	RadioSetStatus(app, filterMsg);
	rc = rb_controller_search(&app->rbController);
	Radio_CheckMiniMem("after radio browser JSON parse");
	app->rbSearchInProgress = 0; app->rbShowingFavourites = FALSE; RadioRefreshResults(app);
	if (rc < 0) RadioSetStatus(app, app->rbController.last_error);
	else { char msg[128]; int hidden = app->rbController.raw_station_count - app->rbVisibleCount; if (app->rbVisibleCount == 0 && app->rbController.raw_station_count > 0) sprintf(msg, "No stations found after filters"); else if (app->rbVisibleCount == 0 && app->rbController.raw_station_count == 0) sprintf(msg, "No stations found"); else sprintf(msg, "Found %d stations, showing %d playable (%d hidden)", app->rbController.raw_station_count, app->rbVisibleCount, hidden < 0 ? 0 : hidden); RadioSetStatus(app, msg); }
}


static int RadioCurrentSelectedRow(MrApp *app)
{
	int i, wanted;
	if (!app || app->rbVisibleCount <= 0) return -1;
	wanted = app->rbShowingFavourites ? app->rbSelectedFavourite : app->rbController.selected_index;
	for (i = 0; i < app->rbVisibleCount; i++)
		if (app->rbVisibleToController[i] == wanted)
			return i;
	return -1;
}

static void RadioMoveSelection(MrApp *app, int delta)
{
	int row;
	if (!app || !app->rbWin || !app->rbListGad) return;
	if (app->rbVisibleCount <= 0) {
		RadioSetStatus(app, "No stations to select.");
		return;
	}
	row = RadioCurrentSelectedRow(app);
	if (row < 0) row = 0;
	else row += delta;
	if (row < 0) row = 0;
	if (row >= app->rbVisibleCount) row = app->rbVisibleCount - 1;
	RadioSelectResult(app, (ULONG)row);
}

static void RadioSelectResult(MrApp *app, ULONG eventSelected)
{
	ULONG selected = eventSelected;
	ULONG row;
	const RadioBrowserStation *st;
	char display[RB_MAX_NAME];
	char msg[RB_MAX_NAME + 16];

	if (!app->rbWin || !app->rbListGad) return;
	if (selected == (ULONG)~0)
		GetAttr(LISTBROWSER_Selected, app->rbListGad, &selected);
#ifdef MINIAMP3_DEBUG
	RADIO_DBG(printf("radio results selection event row/index: %ld\n", (long)selected);)
#endif
	if (selected == (ULONG)~0 || selected >= (ULONG)app->rbVisibleCount) {
		app->rbSelectedFavourite = -1;
		rb_controller_set_selected(&app->rbController, -1);
#ifdef MINIAMP3_DEBUG
		RADIO_DBG(printf("radio results controller selected_index: %d\n", app->rbController.selected_index);)
#endif
		RadioSetStatus(app, "Select a station first.");
		return;
	}
	row = selected;
	selected = (ULONG)app->rbVisibleToController[row];
	if (app->rbShowingFavourites) {
		app->rbSelectedFavourite = (int)selected;
		SetGadgetAttrs((struct Gadget *)app->rbListGad, app->rbWin, NULL,
			LISTBROWSER_Selected, (ULONG)row,
			LISTBROWSER_ShowSelected, TRUE,
			LISTBROWSER_MakeVisible, (ULONG)row, TAG_DONE);
		sprintf(msg, "Selected favourite: %.120s", app->rbFavouriteNames[app->rbSelectedFavourite]);
		RadioSetStatus(app, msg);
		RadioFavEditSync(app);
		return;
	}
	app->rbSelectedFavourite = -1;
	if (rb_controller_set_selected(&app->rbController, (int)selected) < 0) {
		RadioSetStatus(app, app->rbController.last_error);
		return;
	}
	SetGadgetAttrs((struct Gadget *)app->rbListGad, app->rbWin, NULL,
		LISTBROWSER_Selected, (ULONG)row,
		LISTBROWSER_ShowSelected, TRUE,
		LISTBROWSER_MakeVisible, (ULONG)row, TAG_DONE);
	st = rb_controller_get_station(&app->rbController, app->rbController.selected_index);
	if (!st) {
		RadioSetStatus(app, "Select a station first.");
		return;
	}
	rb_station_display_name(st, display, (int)sizeof(display));
#ifdef MINIAMP3_DEBUG
	RADIO_DBG(printf("radio results controller selected_index: %d\n", app->rbController.selected_index);)
	RADIO_DBG(printf("radio results station display name: %s\n", display);)
#endif
	sprintf(msg, "Selected: %.120s", display);
	RadioSetStatus(app, msg);
}

static void RadioFavEditSync(MrApp *app);
static void RadioAddCurrentStreamFavourite(MrApp *app);

static void RadioAddFavourite(MrApp *app)
{
	const RadioBrowserStation *st;
	const char *url;
	char display[RB_MAX_NAME];
	char msg[160];
	int i;
	/* With the favourites shown, or no search result selected, Add Fav
	 * bookmarks the stream that is playing (or was last started) instead. */
	if (app->rbShowingFavourites || app->rbController.selected_index < 0) {
		RadioAddCurrentStreamFavourite(app);
		return;
	}
	st = rb_controller_get_station(&app->rbController, app->rbController.selected_index);
	if (!st) {
		RadioSetStatus(app, "Select a search result to favourite.");
		return;
	}
	url = rb_station_play_url(st);
	if (!url || !url[0]) {
		RadioSetStatus(app, "Selected station has no URL.");
		return;
	}
	rb_station_display_name(st, display, (int)sizeof(display));
	for (i = 0; i < app->rbFavouriteCount; i++) {
		if (!strcmp(app->rbFavouriteUrls[i], url)) {
			SafeCopy(app->rbFavouriteNames[i], sizeof(app->rbFavouriteNames[i]), display);
			SaveSettings(app);
			RadioSetStatus(app, "Favourite updated.");
			return;
		}
	}
	if (app->rbFavouriteCount >= MR_RADIO_FAV_MAX) {
		RadioSetStatus(app, "Radio favourites are full.");
		return;
	}
	i = app->rbFavouriteCount++;
	SafeCopy(app->rbFavouriteNames[i], sizeof(app->rbFavouriteNames[i]), display);
	SafeCopy(app->rbFavouriteUrls[i], sizeof(app->rbFavouriteUrls[i]), url);
	SaveSettings(app);
	sprintf(msg, "Added favourite: %.120s", display);
	RadioSetStatus(app, msg);
}

/* Name for a bookmark of the current stream: the station name it was
 * started with, else the name the stream announces (icy-name), else the
 * URL. */
static void RadioCurrentStreamName(MrApp *app, const char *url, char *out, size_t outSize)
{
	char live[128];
	if (app->currentRadioStationName[0] && strcmp(app->currentRadioStationName, "Internet Radio") != 0) {
		SafeCopy(out, outSize, app->currentRadioStationName);
		return;
	}
	MrCopyVolatileString(live, sizeof(live), gGuiPlaybackStatus.radioStationName);
	SafeCopy(out, outSize, live[0] ? live : url);
}

static void RadioAddCurrentStreamFavourite(MrApp *app)
{
	const char *url = app->currentRadioSourceUrl[0] ? app->currentRadioSourceUrl :
		(MrIsRadioInput(app->inputName) ? app->inputName : "");
	char name[RB_MAX_NAME];
	char msg[160];
	int i;
	if (!url[0]) {
		RadioSetStatus(app, "Nothing to bookmark: play a stream, or select a search result.");
		return;
	}
	for (i = 0; i < app->rbFavouriteCount; i++) {
		if (!strcmp(app->rbFavouriteUrls[i], url)) {
			sprintf(msg, "Already a favourite: %.120s", app->rbFavouriteNames[i]);
			RadioSetStatus(app, msg);
			return;
		}
	}
	if (app->rbFavouriteCount >= MR_RADIO_FAV_MAX) {
		RadioSetStatus(app, "Radio favourites are full.");
		return;
	}
	if (strlen(url) >= sizeof(app->rbFavouriteUrls[0])) {
		RadioSetStatus(app, "Stream URL is too long to bookmark.");
		return;
	}
	RadioCurrentStreamName(app, url, name, sizeof(name));
	i = app->rbFavouriteCount++;
	SafeCopy(app->rbFavouriteNames[i], sizeof(app->rbFavouriteNames[i]), name);
	SafeCopy(app->rbFavouriteUrls[i], sizeof(app->rbFavouriteUrls[i]), url);
	SaveSettings(app);
	if (app->rbShowingFavourites) {
		app->rbSelectedFavourite = i;
		RadioRefreshResults(app);
	}
	sprintf(msg, "Added favourite: %.120s", name);
	RadioSetStatus(app, msg);
}

static int RadioSelectedFavouriteForEdit(MrApp *app)
{
	if (!app->rbShowingFavourites) {
		RadioSetStatus(app, "Press Favourites first to edit them.");
		return -1;
	}
	if (app->rbSelectedFavourite < 0 || app->rbSelectedFavourite >= app->rbFavouriteCount) {
		RadioSetStatus(app, "Select a favourite first.");
		return -1;
	}
	return app->rbSelectedFavourite;
}

static void RadioRenameFavourite(MrApp *app)
{
	STRPTR text = NULL;
	char name[RB_MAX_NAME];
	char *p;
	int i = RadioSelectedFavouriteForEdit(app);
	if (i < 0) return;
	if (app->rbFavNameGad)
		GetAttr(STRINGA_TextVal, app->rbFavNameGad, (ULONG *)(void *)&text);
	SafeCopy(name, sizeof(name), text ? (const char *)text : "");
	for (p = name; *p == ' ' || *p == '\t'; p++) ;
	memmove(name, p, strlen(p) + 1);
	p = name + strlen(name);
	while (p > name && (p[-1] == ' ' || p[-1] == '\t')) *--p = '\0';
	if (!name[0]) {
		RadioSetStatus(app, "Type the new name first.");
		return;
	}
	SafeCopy(app->rbFavouriteNames[i], sizeof(app->rbFavouriteNames[i]), name);
	SaveSettings(app);
	RadioRefreshResults(app);
	RadioSetStatus(app, "Favourite renamed.");
}

static void RadioRemoveFavourite(MrApp *app)
{
	char msg[160];
	int j, i = RadioSelectedFavouriteForEdit(app);
	if (i < 0) return;
	sprintf(msg, "Removed favourite: %.120s", app->rbFavouriteNames[i]);
	for (j = i; j < app->rbFavouriteCount - 1; j++) {
		SafeCopy(app->rbFavouriteNames[j], sizeof(app->rbFavouriteNames[j]), app->rbFavouriteNames[j + 1]);
		SafeCopy(app->rbFavouriteUrls[j], sizeof(app->rbFavouriteUrls[j]), app->rbFavouriteUrls[j + 1]);
	}
	app->rbFavouriteCount--;
	app->rbFavouriteNames[app->rbFavouriteCount][0] = '\0';
	app->rbFavouriteUrls[app->rbFavouriteCount][0] = '\0';
	SaveSettings(app);
	app->rbSelectedFavourite = i < app->rbFavouriteCount ? i : app->rbFavouriteCount - 1;
	RadioRefreshResults(app);
	RadioSetStatus(app, msg);
}

static void RadioMoveFavourite(MrApp *app, int delta)
{
	char name[RB_MAX_NAME];
	char url[RB_MAX_URL];
	int i = RadioSelectedFavouriteForEdit(app);
	int j = i + delta;
	if (i < 0) return;
	if (j < 0 || j >= app->rbFavouriteCount) {
		RadioSetStatus(app, delta < 0 ? "Already at the top." : "Already at the bottom.");
		return;
	}
	SafeCopy(name, sizeof(name), app->rbFavouriteNames[i]);
	SafeCopy(url, sizeof(url), app->rbFavouriteUrls[i]);
	SafeCopy(app->rbFavouriteNames[i], sizeof(app->rbFavouriteNames[i]), app->rbFavouriteNames[j]);
	SafeCopy(app->rbFavouriteUrls[i], sizeof(app->rbFavouriteUrls[i]), app->rbFavouriteUrls[j]);
	SafeCopy(app->rbFavouriteNames[j], sizeof(app->rbFavouriteNames[j]), name);
	SafeCopy(app->rbFavouriteUrls[j], sizeof(app->rbFavouriteUrls[j]), url);
	SaveSettings(app);
	app->rbSelectedFavourite = j;
	RadioRefreshResults(app);
	RadioSetStatus(app, delta < 0 ? "Favourite moved up." : "Favourite moved down.");
}

/* Enables the favourites edit row only while the favourites are shown, and
 * fills the name field with the selected favourite's name. */
static void RadioFavEditSync(MrApp *app)
{
	int editing = app->rbShowingFavourites && app->rbSelectedFavourite >= 0 &&
		app->rbSelectedFavourite < app->rbFavouriteCount;
	Object *gads[5];
	int k;
	if (!app->rbWin)
		return;
	gads[0] = app->rbFavNameGad;
	gads[1] = app->rbFavRenameGad;
	gads[2] = app->rbFavRemoveGad;
	gads[3] = app->rbFavMoveUpGad;
	gads[4] = app->rbFavMoveDownGad;
	for (k = 0; k < 5; k++)
		if (gads[k])
			SetGadgetAttrs((struct Gadget *)gads[k], app->rbWin, NULL,
				GA_Disabled, editing ? FALSE : TRUE, TAG_DONE);
	if (app->rbFavNameGad)
		SetGadgetAttrs((struct Gadget *)app->rbFavNameGad, app->rbWin, NULL,
			STRINGA_TextVal, (ULONG)(editing ? app->rbFavouriteNames[app->rbSelectedFavourite] : ""),
			TAG_DONE);
}

static void RadioToggleFavourites(MrApp *app)
{
	app->rbShowingFavourites = app->rbShowingFavourites ? FALSE : TRUE;
	RadioRefreshResults(app);
	RadioSetStatus(app, app->rbShowingFavourites ?
		"Favourites: Add Fav saves the stream that is playing." : "Showing search results.");
}

static void RadioDoProbeAndPlay(MrApp *app)
{
	static unsigned char peek[RB_PROBE_PEEK_SIZE];
	RbStreamInfo info;
	int peekLen = 0;
	int rc;
	const RadioBrowserStation *st;
	char msg[512];
	RADIO_DBG(printf("radio-ui: play requested currentActive=%d donePending=%d stopRequested=%d state=%s input=\"%s\"\n",
		app->playbackActive, app->playbackDonePending, gPlayer.stopRequested, MrStreamStateName(app->streamState), app->inputName);)
	if (app->streamState == MR_STREAM_STARTING) { RadioSetStatus(app, "Still starting previous stream"); return; }
	if (app->streamState == MR_STREAM_STOP_REQUESTED || app->streamState == MR_STREAM_STOPPING || app->streamState == MR_STREAM_STOP_TIMEOUT) {
		SafeCopy(app->queuedStreamUrl, sizeof(app->queuedStreamUrl), "radio-selection");
		RadioSetStatus(app, "Queued stream; waiting for previous stream to stop...");
		return;
	}
	if (app->rbShowingFavourites) {
		if (app->rbSelectedFavourite >= 0 && app->rbSelectedFavourite < app->rbFavouriteCount &&
			app->playbackActive && MrIsRadioInput(app->inputName) &&
			!strcmp(app->inputName, app->rbFavouriteUrls[app->rbSelectedFavourite])) {
			RadioSetStatus(app, "Selected stream is already playing.");
			MrDebugSession("parent play same URL ignored", app);
			return;
		}
	} else if (app->rbController.selected_index >= 0) {
		st = rb_controller_get_station(&app->rbController, app->rbController.selected_index);
		if (st && rb_station_play_url(st) && app->playbackActive && MrIsRadioInput(app->inputName) &&
			!strcmp(app->inputName, rb_station_play_url(st))) {
			RadioSetStatus(app, "Selected stream is already playing.");
			MrDebugSession("parent play same URL ignored", app);
			return;
		}
	}
	if (app->playbackActive || app->playbackDonePending || PlaybackProcessStillExists()) {
		/* Something is already playing -- a radio stream OR a local file.  Queue
		 * this station and stop the current playback; FinalizePlayback() drains
		 * the "radio-selection" marker and re-enters here once the child is gone,
		 * so pressing Play on a station swaps out whatever was playing instead of
		 * rejecting with "Already playing - press Stop first."  Previously this
		 * branch was gated on MrIsRadioInput(app->inputName), so a local file
		 * playing fell through to StartPlayback()'s active-playback guard. */
		SafeCopy(app->queuedStreamUrl, sizeof(app->queuedStreamUrl), "radio-selection");
		RadioSetStatus(app, "Queued stream; stopping previous stream...");
		RADIO_DBG(printf("radio-ui: queued stream while stopping old stream\n");)
		if (!gPlayer.stopRequested)
			StopPlayback(app);
		return;
	}
	if (Radio_PlaybackOwnsNetwork()) {
		RADIO_DBG(printf("radio-probe: play/probe skipped while radio playback child owns networking\n");)
		RadioSetStatus(app, "Radio playback owns networking; stop before probing another stream.");
		return;
	}
	if (app->rbShowingFavourites) {
		if (app->rbSelectedFavourite < 0 || app->rbSelectedFavourite >= app->rbFavouriteCount) {
			RadioSetStatus(app, "Select a favourite first.");
			return;
		}
		RADIO_DBG(printf("radio-ui: favourite probe requested url=\"%s\" name=\"%s\"\n",
			app->rbFavouriteUrls[app->rbSelectedFavourite], app->rbFavouriteNames[app->rbSelectedFavourite]);)
		RadioProbeUrlAndStart(app, app->rbFavouriteUrls[app->rbSelectedFavourite],
			app->rbFavouriteNames[app->rbSelectedFavourite]);
		return;
	}
	if (app->rbController.selected_index < 0) {
		RadioSetStatus(app, "Select a station first.");
		return;
	}
	st = rb_controller_get_station(&app->rbController, app->rbController.selected_index);
	if (!st) {
		RadioSetStatus(app, "Select a station first.");
		return;
	}
	if (!app->hasHttps && rb_station_play_url(st) && strncmp(rb_station_play_url(st), "https://", 8) == 0) {
		RadioSetStatus(app, "HTTPS not supported in this build");
		return;
	}
	memset(&info, 0, sizeof(info));
	RadioSetStatus(app, "Connecting...");
	Radio_LogTestModeSummary();
	{
		int probeDisabled = rb_probe_stream_probe_disabled();
		RADIO_DBG(printf("radio-probe: flag check MP3_NO_STREAM_PROBE enabled=%d testEnable=%d before selected probe\n", probeDisabled, rb_probe_stream_probe_test_enabled());)
		if (!probeDisabled) {
			RADIO_DBG(printf("radio-ui: new stream probe start url=\"%s\"\n", rb_station_play_url(st));)
		}
	}
	rc = rb_controller_probe_selected(&app->rbController, &info, peek, (int)sizeof(peek), &peekLen);
	RADIO_DBG(printf("radio-ui: new stream probe result rc=%d final=\"%s\" content=\"%s\" codec=%d redirects=%d\n",
		rc, info.final_url, info.content_type, (int)info.codec, info.redirect_count);)
	if (rc < 0) {
		RadioSetStatus(app, app->rbController.last_error);
		return;
	}
	if (info.codec != RB_STREAM_CODEC_MP3 && info.codec != RB_STREAM_CODEC_AAC &&
		info.codec != RB_STREAM_CODEC_OGG && info.codec != RB_STREAM_CODEC_FLAC) {
		sprintf(msg, "Unsupported stream codec: %s (%.48s)", ProbeCodecName(info.codec), info.content_type);
		RadioSetStatus(app, msg);
		return;
	}
	if (!info.final_url[0]) {
		RadioSetStatus(app, "Stream probe did not return a playable URL.");
		return;
	}
#if defined(AMIGA_M68K)
	if (app->lastCompletedWasHttps && strncmp(info.final_url, "https://", 8) == 0 &&
		!app->playbackActive && !app->playbackDonePending && !PlaybackProcessStillExists()) {
		/* Settle gap before the next HTTPS stream so the previous session's
		 * AmiSSL/socket teardown can finish; user-tunable via the Playback
		 * "HTTPS wait" menu (index 0 keeps the historical ~80 ms default). */
		int waitTicks = kHttpsWaitTicks[(app->httpsWaitIndex >= 0 &&
			app->httpsWaitIndex < HTTPS_WAIT_COUNT) ? app->httpsWaitIndex : 0];
		RADIO_DBG(printf("radio-done: Delay(%d) between fully completed HTTPS sessions before starting next HTTPS URL\n", waitTicks);)
		RadioSetStatus(app, "Waiting briefly before next HTTPS stream...");
		Delay(waitTicks);
	}
#endif
	SafeCopy(app->currentRadioSourceUrl, sizeof(app->currentRadioSourceUrl),
		rb_station_play_url(st) ? rb_station_play_url(st) : info.final_url);
	SafeCopy(app->inputName, sizeof(app->inputName), info.final_url);
	/* This station's name/favicon (set just below) belong to this URL, so a
	 * later Play/replay can recognise it and keep the artwork. */
	SafeCopy(app->currentRadioArtUrl, sizeof(app->currentRadioArtUrl), info.final_url);
	{
		int artworkDisabled = rb_probe_artwork_disabled();
		RADIO_DBG(printf("radio-art: flag check MP3_NO_ARTWORK enabled=%d testEnable=%d before favicon/artwork fetch\n", artworkDisabled, rb_probe_artwork_test_enabled());)
		if (artworkDisabled) {
			app->currentRadioFavicon[0] = '\0';
			if (radio_runtime_flag_enabled("MP3_NO_ARTWORK"))
				RADIO_DBG(printf("radio-art: skipped by MP3_NO_ARTWORK\n");)
			else
				RADIO_DBG(printf("radio-art: disabled for run after fatal TLS/artwork transport fault\n");)
		} else {
			SafeCopy(app->currentRadioFavicon, sizeof(app->currentRadioFavicon), st->favicon);
			RADIO_DBG(printf("radio-art: station favicon=\"%s\"\n", app->currentRadioFavicon);)
		}
	}
	app->haveRadioHostAddr = info.have_host_addr;
	app->radioHostAddrBe = info.host_addr_be;
	UpdateFileGadget(app);
	RefreshFileInfoAndTags(app);
	rb_station_display_name(st, msg, (int)sizeof(msg));
	SafeCopy(app->currentRadioStationName, sizeof(app->currentRadioStationName), msg);
	sprintf(msg, "Buffering - %.140s", app->currentRadioStationName[0] ? app->currentRadioStationName : "Internet Radio");
	RadioSetStatus(app, msg);
	RADIO_DBG(printf("radio-ui: new stream start url=\"%s\"\n", info.final_url);)
	if (Radio_IsMemoryPoisoned()) {
		RadioSetStatus(app, "Memory corruption detected. Save log and reboot before using MintAMP again.");
		RADIO_DBG(printf("radio-memory: refusing station switch after MiniMem/ring corruption url=\"%s\"\n", info.final_url);)
		return;
	}
	StartPlayback(app);
	Radio_CheckMiniMem("after station switch");
}

static Object *RadioButton(ULONG id, const char *text)
{
	return (Object *)NewObject(BUTTON_GetClass(), NULL, GA_ID, id, GA_RelVerify, TRUE, GA_Text, (ULONG)text, TAG_DONE);
}

static void CloseRadioWindow(MrApp *app)
{
	if (app->rbWinObj) {
		if (app->rbWin && app->rbListGad) {
			RADIO_DBG(printf("app-dispose: before SetGadgetAttrs detach listbrowser listObj=%p list=%p\n",
				app->rbListGad, &app->rbList);)
			SetGadgetAttrs((struct Gadget *)app->rbListGad, app->rbWin, NULL, LISTBROWSER_Labels, (ULONG)~0, TAG_DONE);
			RADIO_DBG(printf("app-dispose: after detach listbrowser\n");)
			Radio_CheckMiniMem("after detach radio listbrowser");
		}
		RADIO_DBG(printf("app-dispose: before RA_CloseWindow rbWinObj=%p rbWin=%p\n", app->rbWinObj, app->rbWin);)
		RA_CloseWindow(app->rbWinObj);
		app->rbWin = NULL;
		RADIO_DBG(printf("app-dispose: after RA_CloseWindow rbWinObj\n");)
		RADIO_DBG(printf("app-dispose: before DisposeObject rbWinObj=%p\n", app->rbWinObj);)
		DisposeObject(app->rbWinObj);
		app->rbWinObj = NULL;
		RADIO_DBG(printf("app-dispose: after DisposeObject rbWinObj\n");)
		Radio_CheckMiniMem("after DisposeObject rbWinObj");
	}
	if (app->rbList.lh_Head) {
		RADIO_DBG(printf("app-dispose: before FreeListBrowserList list=%p count=%ld\n",
			&app->rbList, CountListNodes(&app->rbList));)
		FreeListBrowserList(&app->rbList);
		RADIO_DBG(printf("app-dispose: after FreeListBrowserList\n");)
		Radio_CheckMiniMem("after FreeListBrowserList radio");
	}
	NewList(&app->rbList);
	app->rbSearchGad = app->rbCodecGad = app->rbCountryGad = app->rbCountryCodeGad = NULL;
	app->rbSchemeGad = app->rbLimitGad = app->rbBitrateGad = app->rbListGad = NULL;
	app->rbStatusGad = app->rbDoSearchGad = app->rbPlayGad = app->rbAddFavGad = NULL;
	app->rbFavouritesGad = app->rbUpGad = app->rbDownGad = app->rbCloseGad = NULL;
	app->rbFavNameGad = app->rbFavRenameGad = app->rbFavRemoveGad = NULL;
	app->rbFavMoveUpGad = app->rbFavMoveDownGad = NULL;
}

static void OpenRadioWindow(MrApp *app)
{
	struct Window *parent = app->win;
#ifdef MINTAMP_SKIN
	if (!parent) parent = skin_player_window(gSkinPlayer);
#endif

	Object *root = NULL;
	static STRPTR codecs[] = { (STRPTR)"All", (STRPTR)"MP3", (STRPTR)"AAC", (STRPTR)"AAC+", (STRPTR)"FLAC", NULL };
	/* Window geometry, fitted to the actual screen.  The radio window's natural
	 * size is 540x340; on a standard PAL/NTSC Workbench screen (256/200 px tall)
	 * a 340-tall window centred with WPOS_CENTERSCREEN puts its title/drag bar
	 * ABOVE the top of the screen, so it can't be dragged.  Cap the size to the
	 * screen and anchor the top below the screen title bar so the drag bar is
	 * always reachable. */
	struct Screen *rbScreen = parent ? parent->WScreen : NULL;
	LONG rbScrW = rbScreen ? (LONG)rbScreen->Width : 640;
	LONG rbScrH = rbScreen ? (LONG)rbScreen->Height : 256;
	LONG rbBarH = rbScreen ? (LONG)rbScreen->BarHeight + 1 : 11;
	LONG rbWinW = 540;
	LONG rbWinH = 340;
	LONG rbWinLeft;
	LONG rbWinTop;
	if (app->rbWinObj) {
		struct Window *rbWin = NULL;

		GetAttr(WINDOW_Window, app->rbWinObj, (ULONG *)&rbWin);
		if (rbWin) {
			WindowToFront(rbWin);
			ActivateWindow(rbWin);
		}
		return;
	}
	if (!parent || !app->hasNetwork)
		return;
	if (app->rbController.limit <= 0) { rb_controller_init(&app->rbController); app->rbShowHttps = FALSE; app->rbSchemeMode = 0; app->rbCountryMode = 0; }
	app->rbCountryMode = RadioCountryToIndex(app->rbController.countrycode); app->rbShowingFavourites = FALSE; app->rbSelectedFavourite = -1; NewList(&app->rbList);
	app->rbSearchGad = (Object *)NewObject(STRING_GetClass(), NULL, GA_ID, RB_GID_SEARCH_TEXT, GA_RelVerify, TRUE, STRINGA_TextVal, (ULONG)app->rbController.name, STRINGA_MaxChars, RB_MAX_NAME, TAG_DONE);
	app->rbCodecGad = (Object *)NewObject(CHOOSER_GetClass(), NULL, GA_ID, RB_GID_CODEC, GA_RelVerify, TRUE, CHOOSER_LabelArray, (ULONG)codecs, CHOOSER_Selected, RadioCodecToIndex(app->rbController.codec), TAG_DONE);
	app->rbCountryGad = (Object *)NewObject(STRING_GetClass(), NULL, GA_ID, RB_GID_COUNTRY, GA_RelVerify, TRUE, STRINGA_TextVal, (ULONG)app->rbController.countrycode, STRINGA_MaxChars, RB_MAX_COUNTRY, TAG_DONE);
	app->rbCountryCodeGad = (Object *)NewObject(CHOOSER_GetClass(), NULL, GA_ID, RB_GID_COUNTRY_CODE, GA_RelVerify, TRUE, CHOOSER_LabelArray, (ULONG)kRadioCountryLabels, CHOOSER_Selected, app->rbCountryMode, TAG_DONE);
	app->rbSchemeGad = (Object *)NewObject(CHOOSER_GetClass(), NULL, GA_ID, RB_GID_SCHEME, GA_RelVerify, TRUE, CHOOSER_LabelArray, (ULONG)kRadioSchemeLabels, CHOOSER_Selected, app->rbSchemeMode, GA_Disabled, !app->hasHttps, TAG_DONE);
	app->rbLimitGad = (Object *)NewObject(CHOOSER_GetClass(), NULL, GA_ID, RB_GID_LIMIT, GA_RelVerify, TRUE, CHOOSER_LabelArray, (ULONG)kRadioSearchLimitLabels, CHOOSER_Selected, RadioSearchLimitIndex(app->rbController.limit), TAG_DONE);
	app->rbBitrateGad = (Object *)NewObject(CHOOSER_GetClass(), NULL, GA_ID, RB_GID_BITRATE, GA_RelVerify, TRUE, CHOOSER_LabelArray, (ULONG)kRadioBitrateLabels, CHOOSER_Selected, app->rbController.max_bitrate <= 0 ? 0 : (app->rbController.max_bitrate <= 56 ? 1 : (app->rbController.max_bitrate <= 64 ? 2 : (app->rbController.max_bitrate <= 96 ? 3 : 4))), TAG_DONE);
	app->rbListGad = (Object *)NewObject(LISTBROWSER_GetClass(), NULL, GA_ID, RB_GID_RADIO_RESULTS, GA_RelVerify, TRUE, LISTBROWSER_Labels, (ULONG)&app->rbList, LISTBROWSER_Selected, (ULONG)~0, LISTBROWSER_ShowSelected, TRUE, LISTBROWSER_AutoFit, TRUE, LISTBROWSER_Separators, TRUE, TAG_DONE);
	app->rbStatusGad = (Object *)NewObject(STRING_GetClass(), NULL, GA_ID, RB_GID_STATUS, GA_ReadOnly, TRUE, STRINGA_TextVal, (ULONG)(app->lastRadioError[0] ? app->lastRadioError : "Ready."), STRINGA_MaxChars, 512, TAG_DONE);
	app->rbDoSearchGad = RadioButton(RB_GID_SEARCH, "Search"); app->rbPlayGad = RadioButton(RB_GID_PROBE, "Play"); app->rbAddFavGad = RadioButton(RB_GID_ADD_FAV, "Add Fav"); app->rbFavouritesGad = RadioButton(RB_GID_FAVOURITES, "Favourites"); app->rbUpGad = RadioButton(RB_GID_UP, "Up"); app->rbDownGad = RadioButton(RB_GID_DOWN, "Down"); app->rbCloseGad = RadioButton(RB_GID_CLOSE, "Close");
	app->rbFavNameGad = (Object *)NewObject(STRING_GetClass(), NULL, GA_ID, RB_GID_FAV_NAME, GA_RelVerify, TRUE, GA_Disabled, TRUE, STRINGA_TextVal, (ULONG)"", STRINGA_MaxChars, RB_MAX_NAME, TAG_DONE);
	app->rbFavRenameGad = RadioButton(RB_GID_FAV_RENAME, "Rename"); app->rbFavRemoveGad = RadioButton(RB_GID_FAV_REMOVE, "Remove"); app->rbFavMoveUpGad = RadioButton(RB_GID_FAV_MOVE_UP, "Move Up"); app->rbFavMoveDownGad = RadioButton(RB_GID_FAV_MOVE_DOWN, "Move Down");
	if (!app->rbSearchGad || !app->rbCodecGad || !app->rbCountryGad || !app->rbCountryCodeGad || !app->rbSchemeGad || !app->rbLimitGad || !app->rbBitrateGad || !app->rbListGad || !app->rbStatusGad || !app->rbDoSearchGad || !app->rbPlayGad || !app->rbAddFavGad || !app->rbFavouritesGad || !app->rbUpGad || !app->rbDownGad || !app->rbCloseGad ||
		!app->rbFavNameGad || !app->rbFavRenameGad || !app->rbFavRemoveGad || !app->rbFavMoveUpGad || !app->rbFavMoveDownGad) goto fail;
	root = (Object *)NewObject(LAYOUT_GetClass(), NULL, LAYOUT_Orientation, LAYOUT_ORIENT_VERT, LAYOUT_SpaceOuter, TRUE, LAYOUT_SpaceInner, TRUE, LAYOUT_DeferLayout, TRUE,
		LAYOUT_AddChild, (ULONG)NewObject(LAYOUT_GetClass(), NULL, LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ, ADD_LABELLED(app->rbSearchGad, "Search"), ADD_LABELLED(app->rbCodecGad, "Codec"), TAG_DONE), CHILD_WeightedHeight, 0,
		LAYOUT_AddChild, (ULONG)NewObject(LAYOUT_GetClass(), NULL, LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ, ADD_LABELLED(app->rbCountryGad, "Country"), ADD_LABELLED(app->rbCountryCodeGad, "Code"), ADD_LABELLED(app->rbSchemeGad, "URL"), TAG_DONE), CHILD_WeightedHeight, 0,
		LAYOUT_AddChild, (ULONG)NewObject(LAYOUT_GetClass(), NULL, LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ, ADD_LABELLED(app->rbLimitGad, "Limit"), ADD_LABELLED(app->rbBitrateGad, "Max kbps"), TAG_DONE), CHILD_WeightedHeight, 0,
		/* Minimum list height trimmed by the favourites row below, so the
		 * screen-fitted window does not need to grow. */
		LAYOUT_AddChild, (ULONG)app->rbListGad, CHILD_MinHeight, 100,
		LAYOUT_AddChild, (ULONG)NewObject(LAYOUT_GetClass(), NULL, LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
			ADD_LABELLED(app->rbFavNameGad, "Name"),
			LAYOUT_AddChild, (ULONG)app->rbFavRenameGad, CHILD_WeightedWidth, 0,
			LAYOUT_AddChild, (ULONG)app->rbFavRemoveGad, CHILD_WeightedWidth, 0,
			LAYOUT_AddChild, (ULONG)app->rbFavMoveUpGad, CHILD_WeightedWidth, 0,
			LAYOUT_AddChild, (ULONG)app->rbFavMoveDownGad, CHILD_WeightedWidth, 0,
			TAG_DONE), CHILD_WeightedHeight, 0,
		LAYOUT_AddChild, (ULONG)NewObject(LAYOUT_GetClass(), NULL, LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ, LAYOUT_EvenSize, TRUE, LAYOUT_AddChild, (ULONG)app->rbDoSearchGad, LAYOUT_AddChild, (ULONG)app->rbPlayGad, LAYOUT_AddChild, (ULONG)app->rbAddFavGad, LAYOUT_AddChild, (ULONG)app->rbFavouritesGad, LAYOUT_AddChild, (ULONG)app->rbUpGad, LAYOUT_AddChild, (ULONG)app->rbDownGad, LAYOUT_AddChild, (ULONG)app->rbCloseGad, TAG_DONE), CHILD_WeightedHeight, 0,
		LAYOUT_AddChild, (ULONG)app->rbStatusGad, CHILD_WeightedHeight, 0, TAG_DONE);
	if (!root) goto fail;
	if (rbWinW > rbScrW) rbWinW = rbScrW;
	if (rbWinH > rbScrH - rbBarH) rbWinH = rbScrH - rbBarH;
	/* Open offset from the main window, NOT centred.  The main window is itself
	 * centred, so a centred radio window lands with its title bar directly over
	 * the main window's -- the overlapping drag bars fight and the drag grabs
	 * the window behind, until the user moves the main window out from under it.
	 * Offsetting keeps the two title bars apart (the GadTools frontend already
	 * opens its radio window at main + 30 for this reason).  Clamp so the whole
	 * window still lands on-screen. */
	rbWinLeft = (parent ? (LONG)parent->LeftEdge : 0) + 30;
	rbWinTop  = (parent ? (LONG)parent->TopEdge : rbBarH) + 30;
	if (rbWinLeft + rbWinW > rbScrW) rbWinLeft = rbScrW - rbWinW;
	if (rbWinLeft < 0) rbWinLeft = 0;
	if (rbWinTop + rbWinH > rbScrH) rbWinTop = rbScrH - rbWinH;
	if (rbWinTop < rbBarH) rbWinTop = rbBarH;
	/* WA_IDCMP must include IDCMP_NEWSIZE for a size-gadget window: without it
	 * window.class never sees the resize that commits its border/drag regions,
	 * so the title bar stays inert until the user manually resizes the window
	 * (the "only draggable after I resize it down" symptom).  The main window,
	 * which lists IDCMP_NEWSIZE, drags fine.  IDCMP_VANILLAKEY lets Enter in the
	 * search string submit, matching the main window. */
	app->rbWinObj = (Object *)NewObject(WINDOW_GetClass(), NULL, WA_Title, (ULONG)"Internet Radio", WA_Activate, TRUE, WA_DepthGadget, TRUE, WA_DragBar, TRUE, WA_CloseGadget, TRUE, WA_SizeGadget, TRUE, WA_IDCMP, IDCMP_GADGETUP | IDCMP_CLOSEWINDOW | IDCMP_IDCMPUPDATE | IDCMP_REFRESHWINDOW | IDCMP_NEWSIZE | IDCMP_VANILLAKEY, WA_Left, rbWinLeft, WA_Top, rbWinTop, WA_Width, rbWinW, WA_Height, rbWinH, WINDOW_ParentGroup, (ULONG)root, TAG_DONE);
	if (!app->rbWinObj) goto fail; app->rbWin = (struct Window *)RA_OpenWindow(app->rbWinObj); if (!app->rbWin) goto fail; WindowToFront(app->rbWin); ActivateWindow(app->rbWin); RadioRefreshResults(app); return;
fail:
	if (!app->rbWinObj && root) DisposeObject(root); CloseRadioWindow(app);
}

/* Double-clicking a station plays it, the same as pressing Play. This mirrors
 * MintVID's ReAction IPTV browser, which works: the row comes from the list's
 * own LISTBROWSER_Selected, not from RA_HandleInput()'s code (a listbrowser
 * inside a layout reports through window.class notification, so that code is
 * not a dependable row number), and the clicks are timed with DateStamp(),
 * about 600 ms at dos.library's 50 ticks a second. The pair is forgotten once
 * it fires, so a third click does not start the station again. */
#define RB_DOUBLE_CLICK_TICKS 30UL

static ULONG RadioDateStampTicks(const struct DateStamp *ds)
{
	return ((ULONG)ds->ds_Days * 24UL * 60UL + (ULONG)ds->ds_Minute) * 3000UL +
		(ULONG)ds->ds_Tick;
}

static int RadioResultDoubleClicked(ULONG row)
{
	static struct DateStamp last;
	static ULONG lastRow = (ULONG)~0;
	struct DateStamp now;
	int dbl;

	DateStamp(&now);
	dbl = row != (ULONG)~0 && row == lastRow &&
		RadioDateStampTicks(&now) - RadioDateStampTicks(&last) <= RB_DOUBLE_CLICK_TICKS;
	last = now;
	lastRow = dbl ? (ULONG)~0 : row;
	return dbl;
}

static void HandleRadioWindow(MrApp *app)
{
	ULONG result; UWORD code = 0; if (!app->rbWinObj) return;
	while ((result = RA_HandleInput(app->rbWinObj, &code)) != WMHI_LASTMSG) {
		switch (result & WMHI_CLASSMASK) {
		case WMHI_CLOSEWINDOW: CloseRadioWindow(app); return;
		case WMHI_GADGETUP:
			/* Memory corruption detected: refuse every radio-browser action
			 * (search/play/add-fav/favourites/up/down/scheme/country) that
			 * could probe, fetch, or start playback against a heap already
			 * known to be damaged -- only Close is still allowed, since the
			 * user must still be able to get out of this window. No queued
			 * stream may survive this either. */
			if (Radio_IsMemoryPoisoned() && (result & WMHI_GADGETMASK) != RB_GID_CLOSE) {
				RadioSetStatus(app, "Memory corruption detected. Save log and reboot before using MintAMP again.");
				app->queuedStreamUrl[0] = '\0';
				app->playlistNextPending = 0;
				break;
			}
			switch (result & WMHI_GADGETMASK) {
			case RB_GID_SEARCH_TEXT: case RB_GID_SEARCH: RadioDoSearch(app); break;
			case RB_GID_RADIO_RESULTS: {
				/* Read the clicked row from the list itself, before
				 * RadioSelectResult() sets the selection from code. */
				ULONG row = (ULONG)~0;
				if (app->rbListGad)
					GetAttr(LISTBROWSER_Selected, app->rbListGad, &row);
				RadioSelectResult(app, (ULONG)code);
				if (RadioResultDoubleClicked(row) &&
				    row < (ULONG)app->rbVisibleCount)
					RadioDoProbeAndPlay(app);
				break;
			}
			case RB_GID_PROBE: RadioDoProbeAndPlay(app); break;
			case RB_GID_ADD_FAV: RadioAddFavourite(app); break;
			case RB_GID_FAVOURITES: RadioToggleFavourites(app); break;
			case RB_GID_UP: RadioMoveSelection(app, -1); break;
			case RB_GID_DOWN: RadioMoveSelection(app, 1); break;
			case RB_GID_FAV_NAME: case RB_GID_FAV_RENAME: RadioRenameFavourite(app); break;
			case RB_GID_FAV_REMOVE: RadioRemoveFavourite(app); break;
			case RB_GID_FAV_MOVE_UP: RadioMoveFavourite(app, -1); break;
			case RB_GID_FAV_MOVE_DOWN: RadioMoveFavourite(app, 1); break;
			case RB_GID_SCHEME: { ULONG active = 0; GetAttr(CHOOSER_Selected, app->rbSchemeGad, &active); app->rbSchemeMode = ClampInt((int)active, 0, 2); app->rbShowHttps = (app->rbSchemeMode != 0); RadioRefreshResults(app); break; }
			case RB_GID_COUNTRY_CODE: { ULONG active = 0; GetAttr(CHOOSER_Selected, app->rbCountryCodeGad, &active); app->rbCountryMode = ClampInt((int)active, 0, 6); if (app->rbCountryGad) SetGadgetAttrs((struct Gadget *)app->rbCountryGad, app->rbWin, NULL, STRINGA_TextVal, (ULONG)RadioCountryFromIndex(app->rbCountryMode), TAG_DONE); break; }
			case RB_GID_CLOSE: CloseRadioWindow(app); return;
			}
			break;
		}
	}
}


static const char *PlaylistBaseName(const char *path)
{
	const char *p = path;
	const char *last = path;
	while (*p) {
		if (*p == '/' || *p == ':')
			last = p + 1;
		p++;
	}
	return last;
}

static void RefreshPlaylistView(MrApp *app)
{
#if defined(MINTAMP_SKIN) && defined(AMIGA_M68K)
    if (skin_player_paths(gSkinPlayer,&app->playlist[0][0],sizeof(app->playlist[0]),
        app->playlistCount,app->playlistCurrent,app->playlistSelected)) gSkinPendingTrack=-1;
#endif
	int i;
	int sel = app->playlistSelected >= 0 ? app->playlistSelected : app->playlistCurrent;
	if (app->plWin && app->plListGad) {
		SetGadgetAttrs((struct Gadget *)app->plListGad, app->plWin, NULL,
			LISTBROWSER_Labels, (ULONG)~0,
			TAG_DONE);
	}
	if (app->plList.lh_Head)
		FreeListBrowserList(&app->plList);
	NewList(&app->plList);
	memset(app->plNodes, 0, sizeof(app->plNodes));
	for (i = 0; i < app->playlistCount; i++) {
		/* A title if the entry has one; else the file name, or the whole
		 * URL for a stream (its last path part is often "stream" or
		 * "listen.pls?sid=1", which says nothing). */
		SafeCopy(app->plNames[i], sizeof(app->plNames[i]),
			app->playlistTitles[i][0] ? app->playlistTitles[i] :
			(MrIsRadioInput(app->playlist[i]) ? app->playlist[i] : PlaylistBaseName(app->playlist[i])));
		app->plNodes[i] = AllocListBrowserNode(1,
			LBNA_Column, 0,
				LBNCA_Text, (ULONG)app->plNames[i],
			TAG_DONE);
		if (app->plNodes[i])
			AddTail(&app->plList, app->plNodes[i]);
	}
	if (app->plWin && app->plListGad) {
		SetGadgetAttrs((struct Gadget *)app->plListGad, app->plWin, NULL,
			LISTBROWSER_Labels, (ULONG)&app->plList,
			LISTBROWSER_Selected, app->playlistCount > 0 && sel >= 0 ? (ULONG)sel : (ULONG)~0,
			LISTBROWSER_ShowSelected, TRUE,
			LISTBROWSER_MakeVisible, app->playlistCount > 0 && sel >= 0 ? (ULONG)sel : 0,
			GA_Disabled, app->playlistCount == 0 ? TRUE : FALSE,
			TAG_DONE);
	}
}

/* Append one or more files (ASL multi-select) to the playlist. */
static void PlaylistAddFiles(MrApp *app)
{
	struct FileRequester *fr;
	char path[MR_MAX_PATH];
	int added = 0;

	fr = (struct FileRequester *)AllocAslRequestTags(ASL_FileRequest,
		ASLFR_TitleText, (ULONG)"Add to playlist",
		ASLFR_DoMultiSelect, TRUE,
		ASLFR_DoPatterns, TRUE,
		ASLFR_InitialPattern, (ULONG)"#?.(mp3|flac|aac|ogg|oga|wav|wma|8svx|iff|svx|aif|aiff)",
		ASLFR_InitialDrawer, (ULONG)(app->lastDrawer[0] ? app->lastDrawer : NULL),
		TAG_DONE);
	if (!fr) {
		SetStatus(app, "Could not allocate file requester.");
		return;
	}
	if (AslRequestTags(fr, ASLFR_Window, (ULONG)(app->plWin ? app->plWin : app->win),
		ASLFR_SleepWindow, TRUE, TAG_DONE)) {
		if (fr->fr_Drawer && fr->fr_Drawer[0])
			SafeCopy(app->lastDrawer, sizeof(app->lastDrawer), (const char *)fr->fr_Drawer);
		if (fr->fr_NumArgs > 0 && fr->fr_ArgList) {
			int i;
			for (i = 0; i < (int)fr->fr_NumArgs && app->playlistCount < MR_PLAYLIST_MAX; i++) {
				path[0] = '\0';
				if (fr->fr_Drawer && fr->fr_Drawer[0]) {
					SafeCopy(path, sizeof(path), (const char *)fr->fr_Drawer);
					AddPart((STRPTR)path, fr->fr_ArgList[i].wa_Name, sizeof(path));
				} else {
					SafeCopy(path, sizeof(path), (const char *)fr->fr_ArgList[i].wa_Name);
				}
				if (!path[0]) continue;
				if (PlaylistAddEntry(app, path, NULL))
					added++;
			}
		} else if (fr->fr_File && fr->fr_File[0] && app->playlistCount < MR_PLAYLIST_MAX) {
			path[0] = '\0';
			if (fr->fr_Drawer && fr->fr_Drawer[0])
				SafeCopy(path, sizeof(path), (const char *)fr->fr_Drawer);
			if (AddPart((STRPTR)path, fr->fr_File, sizeof(path)) &&
				PlaylistAddEntry(app, path, NULL))
				added++;
		}
	}
	FreeAslRequest(fr);
	if (added > 0) {
		char msg[48];
		if (app->playlistSelected < 0)
			app->playlistSelected = app->playlistCount - added;
		RefreshPlaylistView(app);
		UpdateNextButtonState(app);
		sprintf(msg, "Added %d track%s to playlist.", added, added == 1 ? "" : "s");
		SetStatus(app, msg);
	} else {
		SetStatus(app, app->playlistCount >= MR_PLAYLIST_MAX ?
			"Playlist is full." : "No tracks added.");
	}
}

/* Remove the currently selected entry from the playlist. */
static void PlaylistRemoveSelected(MrApp *app)
{
	int sel = app->playlistSelected;
	int i;
	if (sel < 0 || sel >= app->playlistCount) {
		SetStatus(app, "Select a track to remove first.");
		return;
	}
	for (i = sel; i < app->playlistCount - 1; i++) {
#ifdef MINTAMP_SKIN
        app->playlistDurations[i]=app->playlistDurations[i+1];
#endif
		SafeCopy(app->playlist[i], MR_MAX_PATH, app->playlist[i + 1]);
		SafeCopy(app->playlistTitles[i], sizeof(app->playlistTitles[i]), app->playlistTitles[i + 1]);
	}
	app->playlistCount--;
	if (app->playlistCurrent > sel) app->playlistCurrent--;
	else if (app->playlistCurrent == sel) app->playlistCurrent = -1;
	if (app->playlistSelected >= app->playlistCount)
		app->playlistSelected = app->playlistCount - 1;
	RefreshPlaylistView(app);
	UpdateNextButtonState(app);
	SetStatus(app, "Track removed from playlist.");
}

static void PlaylistClearAll(MrApp *app)
{
	app->playlistCount = 0;
	app->playlistCurrent = -1;
	app->playlistSelected = -1;
	RefreshPlaylistView(app);
	UpdateNextButtonState(app);
	SetStatus(app, "Playlist cleared.");
}

/* Writes a buffer to an open file; 0 on a short write. */
static int MrWriteAll(BPTR fh, const char *text, size_t len)
{
	return len == 0 || Write(fh, (APTR)text, (LONG)len) == (LONG)len;
}

/* Saves the playlist as PLS when the chosen name ends in .pls, else as an
 * extended M3U, with each entry's title where it has one. */
static void PlaylistSaveM3U(MrApp *app)
{
	struct FileRequester *fr;
	char m3uPath[MR_MAX_PATH];
	char text[1024];
	BPTR fh;
	int i, format, ok;

	if (app->playlistCount <= 0) {
		SetStatus(app, "Playlist is empty - nothing to save.");
		return;
	}
	fr = (struct FileRequester *)AllocAslRequestTags(ASL_FileRequest,
		ASLFR_TitleText, (ULONG)"Save playlist (name it .m3u or .pls)",
		ASLFR_DoSaveMode, TRUE,
		ASLFR_InitialFile, (ULONG)"playlist.m3u",
		ASLFR_InitialDrawer, (ULONG)(app->lastDrawer[0] ? app->lastDrawer : NULL),
		TAG_DONE);
	if (!fr) {
		SetStatus(app, "Could not allocate file requester.");
		return;
	}
	m3uPath[0] = '\0';
	if (AslRequestTags(fr, ASLFR_Window, (ULONG)(app->plWin ? app->plWin : app->win),
		ASLFR_SleepWindow, TRUE, TAG_DONE)) {
		if (fr->fr_Drawer && fr->fr_Drawer[0])
			SafeCopy(m3uPath, sizeof(m3uPath), (const char *)fr->fr_Drawer);
		if (fr->fr_File && fr->fr_File[0])
			AddPart((STRPTR)m3uPath, fr->fr_File, sizeof(m3uPath));
	}
	FreeAslRequest(fr);
	if (!m3uPath[0])
		return;
	format = playlist_format_from_name(m3uPath);
	fh = Open((STRPTR)m3uPath, MODE_NEWFILE);
	if (!fh) {
		SetStatus(app, "Cannot create playlist file.");
		return;
	}
	ok = MrWriteAll(fh, text, playlist_write_header(text, sizeof(text), format));
	for (i = 0; ok && i < app->playlistCount; i++)
		ok = MrWriteAll(fh,text,
#ifdef MINTAMP_SKIN
            playlist_write_entry_ex(text,sizeof(text),format,i+1,app->playlist[i],app->playlistTitles[i],app->playlistDurations[i])
#else
            playlist_write_entry(text,sizeof(text),format,i+1,app->playlist[i],app->playlistTitles[i])
#endif
        );
	if (ok)
		ok = MrWriteAll(fh, text, playlist_write_footer(text, sizeof(text), format, app->playlistCount));
	Close(fh);
	SetStatus(app, !ok ? "Error writing playlist file." :
		format == PLAYLIST_FORMAT_PLS ? "Playlist saved as PLS." : "Playlist saved as M3U.");
}

/* Adds the URL typed in the playlist window's URL field. */
static void PlaylistAddUrl(MrApp *app)
{
	STRPTR text = NULL;
	char url[MR_MAX_PATH];
	if (app->plUrlGad)
		GetAttr(STRINGA_TextVal, app->plUrlGad, (ULONG *)(void *)&text);
	if (!text || !text[0] || !strcmp((const char *)text, "http://")) {
		SetStatus(app, "Type a stream URL first.");
		return;
	}
	SafeCopy(url, sizeof(url), (const char *)text);
	TrimLine(url);
	if (!MrIsRadioInput(url)) {
		SetStatus(app, "Stream URL must start with http:// or https://");
		return;
	}
	if (!PlaylistAddEntry(app, url, NULL)) {
		SetStatus(app, app->playlistCount >= MR_PLAYLIST_MAX ? "Playlist is full." : "URL is too long.");
		return;
	}
	app->playlistSelected = app->playlistCount - 1;
	RefreshPlaylistView(app);
	UpdateNextButtonState(app);
	if (app->plUrlGad && app->plWin)
		SetGadgetAttrs((struct Gadget *)app->plUrlGad, app->plWin, NULL,
			STRINGA_TextVal, (ULONG)"http://", TAG_DONE);
	SetStatus(app, "Stream added to playlist.");
}

/* Appends every radio favourite to the playlist (then Save exports them). */
static void PlaylistAddFavourites(MrApp *app)
{
	int i, added = 0;
	for (i = 0; i < app->rbFavouriteCount; i++)
		if (PlaylistAddEntry(app, app->rbFavouriteUrls[i], app->rbFavouriteNames[i]))
			added++;
	if (added > 0) {
		char msg[64];
		if (app->playlistSelected < 0)
			app->playlistSelected = app->playlistCount - added;
		RefreshPlaylistView(app);
		UpdateNextButtonState(app);
		sprintf(msg, "Added %d favourite%s to playlist.", added, added == 1 ? "" : "s");
		SetStatus(app, msg);
	} else {
		SetStatus(app, app->rbFavouriteCount == 0 ? "No radio favourites yet." :
			"Playlist is full.");
	}
}

/* Adds every stream in the playlist to the radio favourites (so a loaded
 * .pls/.m3u imports them), updating the name of ones already there. */
static void PlaylistToFavourites(MrApp *app)
{
	int i, j, added = 0, full = 0;
	char msg[80];
	for (i = 0; i < app->playlistCount; i++) {
		const char *url = app->playlist[i];
		const char *name = app->playlistTitles[i][0] ? app->playlistTitles[i] : url;
		if (!MrIsRadioInput(url))
			continue;
		for (j = 0; j < app->rbFavouriteCount; j++)
			if (!strcmp(app->rbFavouriteUrls[j], url))
				break;
		if (j == app->rbFavouriteCount) {
			if (app->rbFavouriteCount >= MR_RADIO_FAV_MAX) {
				full = 1;
				continue;
			}
			app->rbFavouriteCount++;
			SafeCopy(app->rbFavouriteUrls[j], sizeof(app->rbFavouriteUrls[j]), url);
			added++;
		} else if (!app->playlistTitles[i][0]) {
			continue;   /* keep the existing favourite's name */
		}
		SafeCopy(app->rbFavouriteNames[j], sizeof(app->rbFavouriteNames[j]), name);
	}
	SaveSettings(app);
	if (app->rbWinObj && app->rbShowingFavourites)
		RadioRefreshResults(app);
	sprintf(msg, "Added %d stream%s to favourites%s", added, added == 1 ? "" : "s",
		full ? " (favourites full)." : ".");
	SetStatus(app, msg);
}

static void ClosePlaylistWindow(MrApp *app)
{
	if (app->plWinObj) {
		if (app->plWin && app->plListGad) {
			RADIO_DBG(printf("app-dispose: before SetGadgetAttrs detach playlist listObj=%p list=%p\n",
				app->plListGad, &app->plList);)
			SetGadgetAttrs((struct Gadget *)app->plListGad, app->plWin, NULL,
				LISTBROWSER_Labels, (ULONG)~0,
				TAG_DONE);
			RADIO_DBG(printf("app-dispose: after detach playlist listbrowser\n");)
			Radio_CheckMiniMem("after detach playlist listbrowser");
		}
		RADIO_DBG(printf("app-dispose: before RA_CloseWindow plWinObj=%p plWin=%p\n", app->plWinObj, app->plWin);)
		RA_CloseWindow(app->plWinObj);
		app->plWin = NULL;
		RADIO_DBG(printf("app-dispose: after RA_CloseWindow plWinObj\n");)
		RADIO_DBG(printf("app-dispose: before DisposeObject playerWinObj=%p\n", app->plWinObj);)
		DisposeObject(app->plWinObj);
		app->plWinObj = NULL;
		RADIO_DBG(printf("app-dispose: after DisposeObject playerWinObj\n");)
		Radio_CheckMiniMem("after DisposeObject playerWinObj");
	}
	if (app->plList.lh_Head) {
		RADIO_DBG(printf("app-dispose: before FreeListBrowserList list=%p count=%ld\n",
			&app->plList, CountListNodes(&app->plList));)
		FreeListBrowserList(&app->plList);
		RADIO_DBG(printf("app-dispose: after FreeListBrowserList\n");)
		Radio_CheckMiniMem("after FreeListBrowserList playlist");
	}
	NewList(&app->plList);
	app->plListGad = NULL;
	app->plAddGad = NULL;
	app->plRemoveGad = NULL;
	app->plClearGad = NULL;
	app->plPlayGad = NULL;
	app->plLoadGad = NULL;
	app->plSaveGad = NULL;
	app->plCloseGad = NULL;
	app->plUrlGad = NULL;
	app->plAddUrlGad = NULL;
	app->plFromFavsGad = NULL;
	app->plToFavsGad = NULL;
}

static Object *PlaylistButton(ULONG id, const char *text)
{
	return (Object *)NewObject(BUTTON_GetClass(), NULL,
		GA_ID, id,
		GA_RelVerify, TRUE,
		GA_Text, (ULONG)text,
		TAG_DONE);
}

static void OpenPlaylistWindow(MrApp *app)
{
	struct Window *parent = app->win;
#ifdef MINTAMP_SKIN
	if (!parent) parent = skin_player_window(gSkinPlayer);
#endif

	Object *root = NULL;
	int sel;

	if (app->plWinObj || !parent)
		return;

	RefreshPlaylistView(app);
	sel = app->playlistSelected >= 0 ? app->playlistSelected : app->playlistCurrent;
	if (sel < 0 || sel >= app->playlistCount)
		sel = 0;

	app->plListGad = (Object *)NewObject(LISTBROWSER_GetClass(), NULL,
		GA_ID, PL_GID_LIST,
		GA_RelVerify, TRUE,
		GA_Disabled, app->playlistCount == 0 ? TRUE : FALSE,
		LISTBROWSER_Labels, (ULONG)&app->plList,
		LISTBROWSER_Selected, app->playlistCount > 0 ? (ULONG)sel : (ULONG)~0,
		LISTBROWSER_MakeVisible, (ULONG)sel,
		LISTBROWSER_ShowSelected, TRUE,
		LISTBROWSER_AutoFit, TRUE,
		LISTBROWSER_Separators, TRUE,
		TAG_DONE);
	app->plAddGad = PlaylistButton(PL_GID_ADD, "Add Files");
	app->plRemoveGad = PlaylistButton(PL_GID_REMOVE, "Remove");
	app->plClearGad = PlaylistButton(PL_GID_CLEAR, "Clear");
	app->plPlayGad = PlaylistButton(PL_GID_PLAY, "Play");
	app->plLoadGad = PlaylistButton(PL_GID_LOAD_M3U, "Load...");
	app->plSaveGad = PlaylistButton(PL_GID_SAVE_M3U, "Save...");
	app->plCloseGad = PlaylistButton(PL_GID_CLOSE, "Close");
	app->plUrlGad = (Object *)NewObject(STRING_GetClass(), NULL,
		GA_ID, PL_GID_URL,
		GA_RelVerify, TRUE,
		STRINGA_TextVal, (ULONG)"http://",
		STRINGA_MaxChars, MR_MAX_PATH,
		TAG_DONE);
	app->plAddUrlGad = PlaylistButton(PL_GID_ADD_URL, "Add URL");
	app->plFromFavsGad = PlaylistButton(PL_GID_FROM_FAVS, "From Favs");
	app->plToFavsGad = PlaylistButton(PL_GID_TO_FAVS, "To Favs");

	if (!app->plListGad || !app->plAddGad || !app->plRemoveGad ||
		!app->plClearGad || !app->plPlayGad || !app->plLoadGad ||
		!app->plSaveGad || !app->plCloseGad || !app->plUrlGad ||
		!app->plAddUrlGad || !app->plFromFavsGad || !app->plToFavsGad)
		goto fail;

	root = (Object *)NewObject(LAYOUT_GetClass(), NULL,
		LAYOUT_Orientation, LAYOUT_ORIENT_VERT,
		LAYOUT_SpaceOuter, TRUE,
		LAYOUT_SpaceInner, TRUE,
		LAYOUT_DeferLayout, TRUE,
		LAYOUT_AddChild, (ULONG)app->plListGad,
		CHILD_MinHeight, 120,
		LAYOUT_AddChild, (ULONG)NewObject(LAYOUT_GetClass(), NULL,
			LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
			LAYOUT_EvenSize, TRUE,
			LAYOUT_AddChild, (ULONG)app->plAddGad,
			LAYOUT_AddChild, (ULONG)app->plRemoveGad,
			LAYOUT_AddChild, (ULONG)app->plClearGad,
			LAYOUT_AddChild, (ULONG)app->plPlayGad,
			TAG_DONE),
		CHILD_WeightedHeight, 0,
		LAYOUT_AddChild, (ULONG)NewObject(LAYOUT_GetClass(), NULL,
			LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
			ADD_LABELLED(app->plUrlGad, "URL"),
			LAYOUT_AddChild, (ULONG)app->plAddUrlGad,
			CHILD_WeightedWidth, 0,
			TAG_DONE),
		CHILD_WeightedHeight, 0,
		LAYOUT_AddChild, (ULONG)NewObject(LAYOUT_GetClass(), NULL,
			LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
			LAYOUT_EvenSize, TRUE,
			LAYOUT_AddChild, (ULONG)app->plLoadGad,
			LAYOUT_AddChild, (ULONG)app->plSaveGad,
			LAYOUT_AddChild, (ULONG)app->plFromFavsGad,
			LAYOUT_AddChild, (ULONG)app->plToFavsGad,
			LAYOUT_AddChild, (ULONG)app->plCloseGad,
			TAG_DONE),
		CHILD_WeightedHeight, 0,
		TAG_DONE);
	if (!root)
		goto fail;

	app->plWinObj = (Object *)NewObject(WINDOW_GetClass(), NULL,
		WA_Title, (ULONG)"MintAMP Playlist",
		WA_Activate, TRUE,
		WA_DepthGadget, TRUE,
		WA_DragBar, TRUE,
		WA_CloseGadget, TRUE,
		WA_SizeGadget, TRUE,
		WA_IDCMP, IDCMP_GADGETUP | IDCMP_CLOSEWINDOW | IDCMP_IDCMPUPDATE |
			IDCMP_REFRESHWINDOW | IDCMP_VANILLAKEY,
		WA_Width, 420,
		WA_Height, 120,
		WINDOW_Position, WPOS_CENTERSCREEN,
		WINDOW_ParentGroup, (ULONG)root,
		TAG_DONE);
	if (!app->plWinObj)
		goto fail;

	app->plWin = (struct Window *)RA_OpenWindow(app->plWinObj);
	if (!app->plWin)
		goto fail;
	WindowToFront(app->plWin);
	ActivateWindow(app->plWin);
	RefreshPlaylistView(app);
	return;

fail:
	if (!app->plWinObj && root)
		DisposeObject(root);
	ClosePlaylistWindow(app);
}

static void BrowseForPlaylist(MrApp *app)
{
	struct FileRequester *fr;
	char path[MR_MAX_PATH];
	fr = (struct FileRequester *)AllocAslRequestTags(ASL_FileRequest,
		ASLFR_TitleText, (ULONG)"Choose an M3U or PLS playlist",
		ASLFR_DoPatterns, TRUE,
		ASLFR_InitialPattern, (ULONG)"#?.(m3u|m3u8|pls)",
		ASLFR_InitialDrawer, (ULONG)(app->lastDrawer[0] ? app->lastDrawer : NULL),
		TAG_DONE);
	if (!fr) {
		SetStatus(app, "Could not allocate playlist requester.");
		return;
	}
	if (AslRequestTags(fr, ASLFR_Window, (ULONG)app->win,
		ASLFR_SleepWindow, TRUE, TAG_DONE)) {
		path[0] = '\0';
		if (fr->fr_Drawer && fr->fr_Drawer[0])
			SafeCopy(path, sizeof(path), (const char *)fr->fr_Drawer);
		if (fr->fr_File && fr->fr_File[0] && AddPart((STRPTR)path, fr->fr_File, sizeof(path))) {
			CopyDrawerFromPath(app->lastDrawer, sizeof(app->lastDrawer), path);
			SaveSettings(app);
			LoadPlaylistPath(app, path, (const char *)fr->fr_Drawer);
		}
	}
	FreeAslRequest(fr);
}

static void PlaylistLoadCurrent(MrApp *app, int index, int startPlayback)
{
	if (index < 0 || index >= app->playlistCount)
		return;
	app->playlistCurrent = index;
	app->playlistSelected = index;
	SafeCopy(app->inputName, sizeof(app->inputName), app->playlist[index]);
	UpdateFileGadget(app);
	RefreshFileInfoAndTags(app);
	RefreshPlaylistView(app);
	UpdateNextButtonState(app);
	SetStatus(app, "Playlist item selected.");
	if (startPlayback)
		PlaylistStartCurrent(app);
}

/* Starts app->inputName, the current playlist entry.  A stream goes through
 * the same probe as Play and favourites (redirects, .pls/.m3u links, codec
 * check); StartPlayback() on a bare URL skips all of that. */
static void PlaylistStartCurrent(MrApp *app)
{
	int i = app->playlistCurrent;
	if (app->playbackActive || app->playbackDonePending) {
		SetStatus(app, "Already playing - press Stop first.");
		return;
	}
	if (MrIsRadioInput(app->inputName))
		RadioProbeUrlAndStart(app, app->inputName,
			(i >= 0 && i < app->playlistCount && app->playlistTitles[i][0]) ?
			app->playlistTitles[i] : NULL);
	else
		StartPlayback(app);
}

static void HandlePlaylistWindow(MrApp *app)
{
	ULONG result;
	UWORD code = 0;
	if (!app->plWinObj)
		return;
	while ((result = RA_HandleInput(app->plWinObj, &code)) != WMHI_LASTMSG) {
		switch (result & WMHI_CLASSMASK) {
		case WMHI_CLOSEWINDOW:
			ClosePlaylistWindow(app);
			return;
		case WMHI_GADGETUP:
			switch (result & WMHI_GADGETMASK) {
			case PL_GID_LIST:
				if (app->playlistCount > 0 && app->plListGad) {
					ULONG selected = (ULONG)code;
					if (selected == (ULONG)~0)
						GetAttr(LISTBROWSER_Selected, app->plListGad, &selected);
					if ((int)selected < app->playlistCount) {
						app->playlistSelected = (int)selected;
						SetGadgetAttrs((struct Gadget *)app->plListGad, app->plWin, NULL,
							LISTBROWSER_Selected, selected,
							LISTBROWSER_ShowSelected, TRUE,
							LISTBROWSER_MakeVisible, selected, TAG_DONE);
					}
				}
				break;
			case PL_GID_ADD:
				PlaylistAddFiles(app);
				break;
			case PL_GID_REMOVE:
				PlaylistRemoveSelected(app);
				break;
			case PL_GID_CLEAR:
				PlaylistClearAll(app);
				break;
			case PL_GID_PLAY: {
				int idx = app->playlistSelected >= 0 ?
					app->playlistSelected : app->playlistCurrent;
				if (idx >= 0 && idx < app->playlistCount)
					PlaylistLoadCurrent(app, idx, 1);
				else
					SetStatus(app, "Select a track to play first.");
				break;
			}
			case PL_GID_LOAD_M3U:
				BrowseForPlaylist(app);
				break;
			case PL_GID_SAVE_M3U:
				PlaylistSaveM3U(app);
				break;
			case PL_GID_URL:
			case PL_GID_ADD_URL:
				PlaylistAddUrl(app);
				break;
			case PL_GID_FROM_FAVS:
				PlaylistAddFavourites(app);
				break;
			case PL_GID_TO_FAVS:
				PlaylistToFavourites(app);
				break;
			case PL_GID_CLOSE:
				ClosePlaylistWindow(app);
				return;
			}
			break;
		}
	}
}


static void SetMenuItemChecked(MrApp *app, int menuNum, int itemNum, int checked)
{
	struct MenuItem *item;
	if (!app->menuStrip)
		return;
	item = ItemAddress(app->menuStrip, FULLMENUNUM(menuNum, itemNum, NOSUB));
	if (!item)
		return;
	if (checked)
		item->Flags |= CHECKED;
	else
		item->Flags &= ~CHECKED;
}

static void SyncMenuChecks(MrApp *app)
{
	SetMenuItemChecked(app, MENUNUM_PLAYBACK, ITEMNUM_DTP,
		app->decodeThenPlay);
	SetMenuItemChecked(app, MENUNUM_PLAYBACK, ITEMNUM_BENCH, app->bench);
	SetMenuItemChecked(app, MENUNUM_PLAYBACK, ITEMNUM_ARTWORK,
		app->artEnabled);
	SetMenuItemChecked(app, MENUNUM_PLAYBACK, ITEMNUM_ARTCACHE,
		app->artCacheEnabled);
	SetMenuItemChecked(app, MENUNUM_PLAYBACK, ITEMNUM_ARTCOLOR,
		app->artColorEnabled);
	SetMenuItemChecked(app, MENUNUM_PLAYBACK, ITEMNUM_PROGRESS,
		app->progressEnabled);
	{
		int i;
		for (i = 0; i < HTTPS_WAIT_COUNT; i++)
			SetMenuItemChecked(app, MENUNUM_PLAYBACK,
				ITEMNUM_HTTPSWAIT_BASE + i, app->httpsWaitIndex == i);
	}
#ifdef MINTAMP_SKIN
	SetMenuItemChecked(app, MENUNUM_PROJECT, ITEMNUM_CLASSIC, gSkinPlayer == NULL);
#endif
}

static void MrIconify(MrApp *app)
{
	if (!app || !app->winObj || !app->win)
		return;
	/* Do not leave utility windows floating after the player becomes an
	 * AppIcon.  Playback/timer/done ports remain alive and audio continues. */
	CloseRadioWindow(app);
	ClosePlaylistWindow(app);
	/* Shared pens can only be released while their screen is known. */
	ReleaseArtColorPens(app);
	if (RA_Iconify(app->winObj))
		app->win = NULL;
}

static void MrUniconify(MrApp *app)
{
	if (!app || !app->winObj || app->win)
		return;
	app->win = (struct Window *)RA_OpenWindow(app->winObj);
	if (!app->win)
		return;
#ifdef MINTAMP_SKIN
	/* On the first open the full chooser label list reserves its width, then
	 * narrow it as in the standard startup path. Reapply current enablement
	 * after the hidden period, including a saved mono-only speed mode. */
	UpdateSpeedGadgetChoices(app);
	app->shownChannelDisabled=app->shownWidthDisabled=-1;
	UpdateChannelGadgetState(app);
	MrSetGadgetAttr(app->bufferGad,app->win,GA_Disabled,(ULONG)app->decodeThenPlay);
#endif
	UpdateFastMemGadgetState(app);
	if (app->artValid && app->artColorEnabled && !app->artPensBuilt)
		BuildArtColorPens(app);
	DrawArtPanel(app);
	WindowToFront(app->win);
	ActivateWindow(app->win);
}

static void SetDecodeThenPlay(MrApp *app, int enabled)
{
	app->decodeThenPlay = enabled ? 1 : 0;
	if (app->win && app->bufferGad) {
		SetGadgetAttrs((struct Gadget *)app->bufferGad, app->win, NULL,
			GA_Disabled, app->decodeThenPlay,
			TAG_DONE);
	}
	SyncMenuChecks(app);
	SetStatus(app, app->decodeThenPlay ?
		"Decode-then-play enabled; Buffer slider disabled." :
		"Streaming playback mode enabled.");
	SaveSettings(app);
}

static void ShowAbout(MrApp *app)
{
	struct Window *parent=app->win;
	struct EasyStruct es;
#ifdef MINTAMP_SKIN
	if (!parent) parent=skin_player_window(gSkinPlayer);
#endif
	es.es_StructSize = sizeof(es);
	es.es_Flags = 0;
	es.es_Title = (UBYTE *)"About MintAMP";
	es.es_TextFormat = (UBYTE *)"MintAMP\nMini Internet Amiga Media Player\nReAction Edition\nMade by boingball\n(C)2026 - v" MINTAMP_VERSION "\nTo support this application visit:\nhttps://buymeacoffee.com/boingball\n-----\nMade with decoders from\nHelix MP3 / AAC\nby Real Networks\nlibfoxenflac\nby astoeckel\n\nESP8266Audio\nby earlephilhower\n-----\nAI Used\nClaude and Codex\nLate Nights\nMany";
	es.es_GadgetFormat = (UBYTE *)"OK";
	EasyRequest(parent, &es, NULL, TAG_DONE);
}

#ifdef MINTAMP_SKIN
static void MrSkinToClassic(MrApp *app);
static void MrClassicToSkin(MrApp *app);
#endif

static void HandleMenu(MrApp *app, UWORD code, int *done)
{
	while (code != MENUNULL) {
		struct MenuItem *item = ItemAddress(app->menuStrip, code);
		if (item) {
			int mn = (int)MENUNUM(code), it = (int)ITEMNUM(code);
			if (mn == MENUNUM_PROJECT && it == ITEMNUM_QUIT) *done = 1;
			else if (mn == MENUNUM_PROJECT && it == ITEMNUM_ABOUT) {
				ShowAbout(app);
			}
			else if (mn == MENUNUM_PROJECT && it == 4)
				mint_interface_preferences(app->win);
#ifdef MINTAMP_SKIN
			else if (mn == MENUNUM_PROJECT && it == ITEMNUM_CLASSIC) {
				if (gSkinPlayer) MrSkinToClassic(app); else MrClassicToSkin(app);
				SyncMenuChecks(app);
				break;
			}
#endif
			else if (mn == MENUNUM_PROJECT && it == ITEMNUM_RADIO)
				OpenRadioWindow(app);
			else if (mn == MENUNUM_PROJECT && it == ITEMNUM_ICONIFY)
				MrIconify(app);
			else if (mn == MENUNUM_PLAYBACK && it == ITEMNUM_DTP)
				SetDecodeThenPlay(app, !app->decodeThenPlay);
			else if (mn == MENUNUM_PLAYBACK && it == ITEMNUM_BENCH) {
				app->bench = !app->bench;
				SetStatus(app, app->bench ? "Bench mode enabled." : "Bench mode disabled.");
				SaveSettings(app);
			} else if (mn == MENUNUM_PLAYBACK && it == ITEMNUM_ARTWORK) {
				app->artEnabled = !app->artEnabled;
				app->artCacheBypass = 0;
				RefreshFileInfoAndTags(app);
				SetStatus(app, app->artEnabled ? (app->artValid ? "Artwork enabled." : "No artwork.") : "Artwork disabled.");
				SaveSettings(app);
			} else if (mn == MENUNUM_PLAYBACK && it == ITEMNUM_ARTCACHE) {
				app->artCacheEnabled = !app->artCacheEnabled;
				SetStatus(app, app->artCacheEnabled ? "Artwork cache enabled." : "Artwork cache disabled.");
				SaveSettings(app);
			} else if (mn == MENUNUM_PLAYBACK && it == ITEMNUM_ARTCOLOR) {
				app->artColorEnabled = !app->artColorEnabled;
				if (app->artColorEnabled && app->artValid) BuildArtColorPens(app);
				else ReleaseArtColorPens(app);
				DrawArtPanel(app);
				SetStatus(app, app->artColorEnabled ? "Colour artwork enabled." : "Colour artwork disabled.");
				SaveSettings(app);
			} else if (mn == MENUNUM_PLAYBACK && it == ITEMNUM_PROGRESS) {
				app->progressEnabled = !app->progressEnabled;
				SetGauge(app, app->progressEnabled && app->totalSecs > 0 ? (app->elapsedSecs * 100) / app->totalSecs : 0);
				SetStatus(app, app->progressEnabled ? "Progress bar enabled." : "Progress bar disabled.");
				SaveSettings(app);
			} else if (mn == MENUNUM_PLAYBACK && it == ITEMNUM_ARTREFRESH) {
				DrawArtPanel(app);
				SetStatus(app, app->artValid ? "Artwork refreshed." : "No artwork.");
			} else if (mn == MENUNUM_PLAYBACK && it == ITEMNUM_ARTRELOAD) {
				app->artCacheBypass = 1;
				RefreshFileInfoAndTags(app);
				app->artCacheBypass = 0;
				SetStatus(app, app->artValid ? "Artwork refreshed." : "No artwork.");
			} else if (mn == MENUNUM_PLAYBACK && it == ITEMNUM_ARTCLEAN) {
				CleanArtworkCache(app);
			} else if (mn == MENUNUM_PLAYBACK &&
				it >= ITEMNUM_HTTPSWAIT_BASE &&
				it < ITEMNUM_HTTPSWAIT_BASE + HTTPS_WAIT_COUNT) {
				char msg[64];
				app->httpsWaitIndex = it - ITEMNUM_HTTPSWAIT_BASE;
				sprintf(msg, "HTTPS stream wait set to %s.",
					kHttpsWaitDesc[app->httpsWaitIndex]);
				SetStatus(app, msg);
				SaveSettings(app);
			}
			SyncMenuChecks(app);
			code = item->NextSelect;
		} else code = MENUNULL;
	}
}

static void SyncFromGadgets(MrApp *app)
{
	ULONG v;
	STRPTR path = NULL;

	if (app->fileGad) {
		GetAttr(GETFILE_FullFile, app->fileGad, (ULONG *)(void *)&path);
		if (path)
			SafeCopy(app->inputName, sizeof(app->inputName), (const char *)path);
	}
	if (app->rateGad && GetAttr(CHOOSER_Selected, app->rateGad, &v)) {
		if ((int)v >= 0 && (int)v < MR_RATE_COUNT && app->rateIndex != (int)v) {
			app->rateIndex = (int)v;
			UpdateSpeedGadgetChoices(app);
		}
	}
	if (app->qualityGad && GetAttr(CHOOSER_Selected, app->qualityGad, &v))
		app->qualityIndex = (int)v;
	if (app->subbandCapGad && GetAttr(CHOOSER_Selected, app->subbandCapGad, &v)) {
		if ((int)v >= 0 && (int)v < (int)SUBBAND_CAP_COUNT)
			app->subbandCapIndex = (int)v;
	}
	if (app->channelGad && GetAttr(CHOOSER_Selected, app->channelGad, &v))
		app->mono = ((int)v == 1);
	if (app->volumeGad && GetAttr(SLIDER_Level, app->volumeGad, &v)) {
		int oldPercent = app->volumePercent;
		app->volumePercent = ClampInt((int)v, 0, 100);
		if (app->volumePercent != oldPercent) {
			gMiniAmp3RequestedVolume = (unsigned short)app->volumePercent;
			gMiniAmp3VolumeSequence++;
		}
	}
	if (app->bufferGad && GetAttr(SLIDER_Level, app->bufferGad, &v))
		app->bufferSeconds = ClampInt((int)v, 1, 10);
	if (app->fastMemGad && GetAttr(GA_Selected, app->fastMemGad, &v))
		app->fastMem = (v != 0);
	UpdateFastMemGadgetState(app);
	if (app->speedGad && GetAttr(CHOOSER_Selected, app->speedGad, &v)) {
		app->cd32Ultrafast = (app->rateIndex == MR_RATE_22050_INDEX && (int)v == 3);
		app->ultrafast = ((int)v == 2);
		app->superfastLowrate = ((int)v == 1 || app->cd32Ultrafast);
		if (app->cd32Ultrafast) {
			app->fastLowrate = 1;
			app->mono = 1;
			app->rateIndex = MR_RATE_22050_INDEX;
			/* Reflect the forced mono output in the (about-to-be-greyed)
			 * Mono/Stereo chooser so it doesn't keep showing "Stereo". */
			if (app->win && app->channelGad)
				SetGadgetAttrs((struct Gadget *)app->channelGad, app->win, NULL,
					CHOOSER_Selected, (ULONG)1, TAG_DONE);
		} else if (app->ultrafast)
			app->fastLowrate = 0;
		else
			/* The Speed chooser is now the sole owner of fast-lowrate (the
			 * "Fast low-rate decode" checkbox was removed): Normal=off,
			 * Superfast low-rate=on. */
			app->fastLowrate = app->superfastLowrate;
	}
	if (app->widthGad && GetAttr(CHOOSER_Selected, app->widthGad, &v)) {
		if (app->cd32Ultrafast) {
			/* 22050 Mono Ultrafast is mono-only, so fake stereo can't apply:
			 * force it off and keep the greyed Mode/width chooser parked on the
			 * "Normal M/S" entry rather than leaving a stale width selected. */
			app->fakeStereo = 0;
			if (app->win && app->widthGad)
				SetGadgetAttrs((struct Gadget *)app->widthGad, app->win, NULL,
					CHOOSER_Selected, (ULONG)0, TAG_DONE);
		} else {
			app->fakeStereo = ((int)v > 0);
			app->fakeStereoWidthIndex = app->fakeStereo ? (int)v - 1 : 0;
		}
		UpdateChannelGadgetState(app);
	}
	if (app->delayGad && GetAttr(CHOOSER_Selected, app->delayGad, &v)) {
		if ((int)v >= 0 && (int)v < 5)
			app->fakeStereoDelayIndex = (int)v;
	}
}

/* ------------------------------------------------------------------------- */
/* Main                                                                      */
/* ------------------------------------------------------------------------- */

static struct StackSwapStruct gMrNewStack;
static struct StackSwapStruct gMrOldStack;
static APTR gMrAllocatedStack;
static ULONG gMrDetectedStackLower;
static ULONG gMrDetectedStackUpper;
static ULONG gMrDetectedStackSize;
static ULONG gMrEffectiveStackSize;

/* Opens and plays a file, stream URL or .pls/.m3u playlist given on the
 * command line ("MintAMP Work:Radio/jazz.pls") or as a Workbench project
 * icon whose Default Tool is MintAMP. */
static void MrOpenStartupArg(MrApp *app, int argc, char **argv)
{
	char path[MR_MAX_PATH];
	path[0] = '\0';
#ifdef MINTAMP_SKIN
	if (argc > 0 && skin_player_audio_arg(argc, argv)) {
		SafeCopy(path, sizeof(path), skin_player_audio_arg(argc, argv));
#else
	if (argc >= 2 && argv[1] && argv[1][0] && argv[1][0] != '-') {
		SafeCopy(path, sizeof(path), argv[1]);
#endif
	} else if (argc == 0 && argv) {
		struct WBStartup *wb = (struct WBStartup *)argv;
		if (wb->sm_NumArgs >= 2 && wb->sm_ArgList[1].wa_Lock &&
			(!NameFromLock(wb->sm_ArgList[1].wa_Lock, (STRPTR)path, sizeof(path)) ||
			 !AddPart((STRPTR)path, wb->sm_ArgList[1].wa_Name, sizeof(path))))
			path[0] = '\0';
	}
	if (!path[0])
		return;
#ifdef MINTAMP_SKIN
	if (skin_player_is_skin(path)) return;
#endif
	if (MrIsPlaylistFile(path)) {
		char drawer[MR_MAX_PATH];
		CopyDrawerFromPath(drawer, sizeof(drawer), path);
		LoadPlaylistPath(app, path, drawer);
		if (app->playlistCount > 0)
			PlaylistLoadCurrent(app, 0, 1);
	} else if (MrIsRadioInput(path)) {
		RadioProbeUrlAndStart(app, path, NULL);
	} else {
		SafeCopy(app->inputName, sizeof(app->inputName), path);
		UpdateFileGadget(app);
		RefreshFileInfoAndTags(app);
		StartPlayback(app);
	}
}

#ifdef MINTAMP_SKIN
#include "skin_reaction.inc"
#endif

static int MrMainReal(int argc, char **argv)
{
	static MrApp app;
	ULONG winSig = 0;
	ULONG appSig = 0;
	ULONG timerSig = 0;
	ULONG doneSig = 0;
	int done = 0;

	(void)argc;
	(void)argv;

	/* Record the GUI/main application task identity up front: every free
	 * bracketed by MR_FREE_BEGIN/END below logs FindTask(NULL), and this is
	 * the pointer they must match for the recoverable AN_FreeTwice/
	 * AN_BadFreeAddr alerts to be pinned on the GUI task rather than the net
	 * worker or a playback child. */
	MR_TASK_IDENTITY("application-startup-main-task");

	/* First-run defaults (used until the user saves settings): the fastest
	 * out-of-the-box preset so RC1 plays smoothly on a stock 020/030 --
	 * 11025 Hz, mono, Ultrafast speed, "Faster" quality, decode from Fast RAM,
	 * with artwork and colour artwork on. */
	app.magic = MR_APP_MAGIC;
	app.rateIndex = 1;		/* 11025 Hz */
	app.qualityIndex = 0;		/* Faster (fastest quality preset) */
	app.mono = 1;
	app.fastMem = 1;
	app.fastLowrate = 0;
	app.ultrafast = 1;		/* Ultrafast speed preset */
	app.cd32Ultrafast = 0;
	app.volumePercent = 100;
	app.bufferSeconds = 10;
	app.fakeStereoDelayIndex = 0;
	app.artEnabled = 1;
	app.artCacheEnabled = 1;
	app.artColorEnabled = 1;
	app.progressEnabled = 0;
	app.playlistCurrent = -1;
	app.playlistSelected = -1;
	app.lastPhaseShown = -1;
	app.shownGaugeLevel = -1;
	app.shownChannelDisabled = -1;
	app.shownWidthDisabled = -1;
	app.shownNextDisabled = -1;
	app.lastRadioStatusShown = -1;

	/* Let the playback child find any installed *.decoder modules, exactly as
	 * the GadTools frontend does. */
	if (!gDecoderModulesPath[0])
		SafeCopy(gDecoderModulesPath, sizeof(gDecoderModulesPath),
			"PROGDIR:decoders/");

	if (!OpenLibs()) {
		CloseLibs();
		return 1;
	}
	/* Open bsdsocket.library/AmiSSL once for the whole app run; every probe,
	 * favicon fetch, browser search and stream still opens/closes its own
	 * socket and, for HTTPS, its own SSL/SSL_CTX -- only the shared libraries
	 * stay open until Radio_NetworkShutdown() at final app exit. */
	Radio_NetworkInit();
	/* Probe the result up front so the menu/gadgets below can be greyed out
	 * for offline users instead of failing on first use. */
	app.hasNetwork = Radio_HasNetwork();
	app.hasHttps = Radio_HasHttps();

	LoadSettings(&app);

	app.donePort = CreateMsgPort();
	if (!app.donePort) {
		fprintf(stderr, "MintAMP: could not create the reply port.\n");
		CloseLibs();
		return 1;
	}

	if (!OpenTimer(&app)) {
		fprintf(stderr, "MintAMP: could not open timer.device.\n");
		CloseTimer(&app);
		DeleteMsgPort(app.donePort);
		CloseLibs();
		return 1;
	}

	app.appPort = CreateMsgPort();
	if (!app.appPort) {
		fprintf(stderr, "MintAMP: could not create the Workbench AppPort.\n");
		CloseTimer(&app);
		DeleteMsgPort(app.donePort);
		CloseLibs();
		return 1;
	}

#ifdef MINTAMP_SKIN
	gSkinArgc=argc; gSkinArgv=argv;
	if (mint_interface_start_skinned(argc,argv))
		gSkinPlayer=skin_player_open("MintAMP-SR",argc,argv);
	else
		skin_player_restore_audio("MintAMP-SR");
#endif
	if (!MrOpenWindow(&app)) {
		SyncFromGadgets(&app);
		SaveSettings(&app);
		MrCloseWindow(&app);
		CloseTimer(&app);
		DrainReplyMsgPortForClose(app.appPort, "open failure");
		DeleteMsgPort(app.appPort);
		app.appPort = NULL;
		DeleteMsgPort(app.donePort);
		CloseLibs();
		return 1;
	}

	/* The Speed chooser was built with the full label list so the layout
	 * reserved room for the longest option; now that the window is open and
	 * sized, narrow it to the rate-appropriate set (the box keeps its width).
	 * Also apply the initial greyed state for the Mono/Stereo and Mode/width
	 * choosers in case a saved 22050-Mono-Ultrafast / fake-stereo setting is
	 * in effect. */
	UpdateSpeedGadgetChoices(&app);
	UpdateChannelGadgetState(&app);
	UpdateFastMemGadgetState(&app);

	GetAttr(WINDOW_SigMask, app.winObj, &winSig);
	appSig   = 1UL << app.appPort->mp_SigBit;
	timerSig = 1UL << app.timerPort->mp_SigBit;
	doneSig  = 1UL << app.donePort->mp_SigBit;

	/* Paint the (empty) artwork panel once now that the layout has sized the
	 * placeholder, so the recessed box is shown before the first file loads. */
	DrawArtPanel(&app);

	MrOpenStartupArg(&app, argc, argv);
#ifdef MINTAMP_SKIN
	if (gSkinPlayer) MrSkinUpdate(&app,0);
#endif

	MR_TASK_IDENTITY("gui-event-loop");
	while (!done) {
		ULONG plSig = 0;
		ULONG rbSig = 0;
		ULONG sigs;
		if (app.plWinObj)
			GetAttr(WINDOW_SigMask, app.plWinObj, &plSig);
		if (app.rbWinObj)
			GetAttr(WINDOW_SigMask, app.rbWinObj, &rbSig);
#ifdef MINTAMP_SKIN
		GetAttr(WINDOW_SigMask, app.winObj, &winSig);
		sigs = Wait(skin_player_signal(gSkinPlayer) | winSig | appSig | timerSig | doneSig | plSig | rbSig |
#else
		sigs = Wait(winSig | appSig | timerSig | doneSig | plSig | rbSig |
#endif
			SIGBREAKF_CTRL_C);

		if (sigs & SIGBREAKF_CTRL_C)
			done = 1;

		if (sigs & doneSig)
			HandleDoneSignal(&app);
#ifdef MINTAMP_SKIN
		MrSkinPoll(&app,&done);
#endif

		if (sigs & timerSig) {
			struct Message *tmsg;
			while ((tmsg = GetMsg(app.timerPort)) != NULL)
				;
			app.timerRunning = 0;
			PollPlaybackStatus(&app);
			UpdateAlbumHover(&app);
			ScrollAlbumHover(&app);
			ArmTimer(&app, MR_TICK_MICROS);
		}

		if (plSig && (sigs & plSig))
			HandlePlaylistWindow(&app);
		if (rbSig && (sigs & rbSig))
			HandleRadioWindow(&app);

		if (sigs & (winSig | appSig)) {
			ULONG result;
			UWORD code = 0;
			while ((result = RA_HandleInput(app.winObj, &code)) != WMHI_LASTMSG) {
				switch (result & WMHI_CLASSMASK) {
				case WMHI_CLOSEWINDOW:
#ifdef MINTAMP_SKIN
					if (gSkinPlayer) MrSkinHideSettings(&app); else
#endif
					done = 1;
					break;
				case WMHI_ICONIFY:
					MrIconify(&app);
					break;
				case WMHI_UNICONIFY:
					MrUniconify(&app);
					GetAttr(WINDOW_SigMask, app.winObj, &winSig);
					break;
				case WMHI_MENUPICK:
					if (app.menuStrip)
						HandleMenu(&app, code, &done);
					break;
				case WMHI_GADGETUP:
					switch (result & WMHI_GADGETMASK) {
					case GID_FILE:
						BrowseForFile(&app);
						break;
					case GID_PLAY:
						SyncFromGadgets(&app);
						if (MrIsRadioInput(app.inputName))
							RadioProbeUrlAndStart(&app, app.inputName, NULL);
						else
							StartPlayback(&app);
						break;
					case GID_NEXT:
						SyncFromGadgets(&app);
						PlaylistNext(&app);
						break;
					case GID_STOP:
						StopPlayback(&app);
						break;
					case GID_REW:
						MrSeekRelative(&app, -MR_SEEK_STEP_SECS);
						break;
					case GID_FFWD:
						MrSeekRelative(&app, MR_SEEK_STEP_SECS);
						break;
					case GID_FILTER:
						app.hardwareFilter = !app.hardwareFilter;
						ApplyHardwareAudioFilter(&app);
						SetGadgetAttrs((struct Gadget *)app.filterGad, app.win, NULL, GA_Text, (ULONG)(app.hardwareFilter ? "FLT*" : "FLT"), TAG_DONE);
						SetStatus(&app, app.hardwareFilter ? "Hardware filter enabled." : "Hardware filter disabled.");
						break;
					case GID_PLAYLIST:
						if (app.plWin)
							ClosePlaylistWindow(&app);
						else
							OpenPlaylistWindow(&app);
						break;
					case GID_RADIO:
						OpenRadioWindow(&app);
						break;
					case GID_STAR1: case GID_STAR2: case GID_STAR3: case GID_STAR4: case GID_STAR5:
						app.rating = (int)(result & WMHI_GADGETMASK) - GID_STAR1 + 1;
						UpdateRatingDisplay(&app);
						SetStatus(&app, "Rating updated for this file only.");
						break;
					default:
						/* Keep app state current for the other controls. */
						SyncFromGadgets(&app);
						break;
					}
					break;
				default:
					break;
				}
				if (done)
					break;
			}
			/* Mouse movement only updates the Album hover state.  The timer handles
			 * the slow text scrolling so normal pointer motion cannot repaint the
			 * metadata gadgets continuously. */
			UpdateAlbumHover(&app);
		}
#ifdef MINTAMP_SKIN
		MrSkinUpdate(&app,(sigs & timerSig)!=0);
#endif
	}

	/* Ordered, idempotent app-close teardown. Only proceed to dispose the
	 * GUI/browser/network objects the playback child can touch if it is
	 * confirmed gone -- a wedged child means those objects are leaked
	 * (and CloseLibs()/Radio_NetworkShutdown() skipped) rather than freed
	 * or closed out from under a task that might still reference them. */
	if (!AppCloseShutdown(&app)) {
		RADIO_DBG(printf("app-close: wedged playback child, exiting without disposing GUI/network objects\n");)
		return 0;
	}
	if (Radio_IsMemoryPoisoned()) {
		/* Corrupt exec heap: SaveSettings()/MrCloseWindow()'s DisposeObject()/
		 * Radio_NetworkShutdown()/CloseLibs() all allocate or free memory
		 * (GadTools teardown, bsdsocket.library/AmiSSL closure) that could
		 * write through the same already-damaged allocator state that
		 * poisoned the session. Skip every further disposal step and exit
		 * as directly as possible -- a leak here is recoverable with a
		 * reboot; another corrupting free is not. */
		RADIO_DBG(printf("APP_CLOSE: memory corruption detected -- skipping SaveSettings/GUI dispose/Radio_NetworkShutdown/CloseLibs, exiting directly\n");)
		return 0;
	}

#ifdef MINTAMP_SKIN
	skin_player_close(gSkinPlayer); gSkinPlayer=NULL;
	skin_player_shutdown();
#endif
	SyncFromGadgets(&app);
	SaveSettings(&app);
	mint_interface_flush();
	RADIO_DBG(printf("app-close: SaveSettings done\n");)
	DrainGuiPorts(&app, "before dispose");
	DisposeGuiObjectsBeforeCloseLibs(&app);
	DrainGuiPorts(&app, "after DisposeGuiObjectsBeforeCloseLibs");
	Delay(2);
	DrainGuiPorts(&app, "after dispose delay");
	RADIO_DBG(printf("app-close: GUI dispose/drain done\n");)
	/* Every playback child has been stopped and reaped above, so it is now safe
	 * to release the shared network libraries (AmiSSL master + bsdsocket.library)
	 * that the probe/search/streams opened.  Without this the app left
	 * bsdsocket.library open on exit and the next launch could not open a working
	 * socket ("Search failed" with the network otherwise up). */
	AppCloseDebug("close network base fallback", &app);
	AppCloseDebug("AmiSSL shutdown fallback", &app);
	Radio_NetworkShutdown();
	RADIO_DBG(printf("app-close: Radio_NetworkShutdown done\n");)
	AppCloseDebug("end", &app);
	PreCloseLibsAudit(&app);
	CloseLibs();
	PostCloseLibsAudit(&app, 1);
	RADIO_DBG(printf("app-close: CloseLibs done, returning from main\n");)
	Radio_CheckMiniMem("before app exit");
	MiniMem_ReportLeaks();
	return 0;
}


#if defined(AMIGA_M68K)
extern void LibnixFreeAllCompat_Install(void);
#endif

int main(int argc, char **argv)
{
	struct Task *task = FindTask(NULL);
	int rc;

	/*
	 * Workbench launches have no normal Shell console. Route incidental
	 * stdout/stderr output to NIL: before startup code can print or flush.
	 */
	if (argc == 0) {
		(void)freopen("NIL:", "w", stdout);
		(void)freopen("NIL:", "w", stderr);
	}

	/* True program entry point: init the cross-task stdout lock exactly
	 * once here, before any playback child or the net worker task can be
	 * spawned. See amiga_mp3dec.c's radio_console_lock definition and
	 * RADIO_CONSOLE_LOCK_INIT_ELSEWHERE above. */
	InitSemaphore(&radio_console_lock);
#if defined(AMIGA_M68K)
	LibnixFreeAllCompat_Install();
#endif


	gMrDetectedStackLower = (ULONG)task->tc_SPLower;
	gMrDetectedStackUpper = (ULONG)task->tc_SPUpper;
	gMrDetectedStackSize = gMrDetectedStackUpper - gMrDetectedStackLower;
	gMrEffectiveStackSize = gMrDetectedStackSize;

	if (gMrDetectedStackSize >= MR_STARTUP_STACK_SIZE) {
#if defined(DEBUG) || defined(RADIO_DEBUG)
		printf("MintAMP: startup stack lower=%lu upper=%lu size=%lu, no swap needed\n",
			gMrDetectedStackLower, gMrDetectedStackUpper, gMrDetectedStackSize);
#endif
		return MrMainReal(argc, argv);
	}

	gMrAllocatedStack = AllocMem(MR_STARTUP_STACK_SIZE, MEMF_PUBLIC);
	if (!gMrAllocatedStack)
		return 1;

	gMrNewStack.stk_Lower = gMrAllocatedStack;
	gMrNewStack.stk_Upper = (ULONG)((UBYTE *)gMrAllocatedStack + MR_STARTUP_STACK_SIZE);
	gMrNewStack.stk_Pointer = (APTR)gMrNewStack.stk_Upper;
	gMrEffectiveStackSize = MR_STARTUP_STACK_SIZE;

	StackSwap(&gMrNewStack);
	gMrOldStack = gMrNewStack;

#if defined(DEBUG) || defined(RADIO_DEBUG)
	printf("MintAMP: startup stack lower=%lu upper=%lu size=%lu, swapped to %lu bytes\n",
		gMrDetectedStackLower, gMrDetectedStackUpper, gMrDetectedStackSize,
		gMrEffectiveStackSize);
#endif
	rc = MrMainReal(argc, argv);

	StackSwap(&gMrOldStack);
	MR_TASK_IDENTITY("shutdown-free-startup-stack");
	MR_FREE_BEGIN("MrMain", "startup-stack", gMrAllocatedStack, MR_STARTUP_STACK_SIZE);
	FreeMem(gMrAllocatedStack, MR_STARTUP_STACK_SIZE);
	gMrAllocatedStack = NULL;
	MR_FREE_END("MrMain", "startup-stack", gMrAllocatedStack, 0);
	return rc;
}

#else	/* !AMIGA_M68K */

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	fprintf(stderr,
		"MintAMP is an AmigaOS ReAction/ClassAct frontend and needs an "
		"AMIGA_M68K build.\n");
	return 1;
}

#endif
