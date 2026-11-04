#ifndef MINTAMP_SKIN_CONTROLS_H
#define MINTAMP_SKIN_CONTROLS_H

#define SKIN_LIST_MAX 128
#define SKIN_EQ_BANDS 10
#define SKIN_VIS_SAMPLES 64

enum { SKIN_REPEAT_OFF, SKIN_REPEAT_LIST, SKIN_REPEAT_TRACK };
enum { SKIN_SELECT_REPLACE, SKIN_SELECT_TOGGLE, SKIN_SELECT_RANGE, SKIN_SELECT_FOCUS };
typedef struct SkinOrder {
    int shuffle, repeat, count, cursor, used, last;
    int history[SKIN_LIST_MAX];
    unsigned char seen[SKIN_LIST_MAX];
    unsigned long seed;
} SkinOrder;
void skin_order_init(SkinOrder *order, unsigned long seed);
void skin_order_reset(SkinOrder *order, int count, int current);
void skin_order_observe(SkinOrder *order, int count, int current);
int skin_order_next(SkinOrder *order, int count, int current, int natural);
int skin_order_previous(SkinOrder *order, int count, int current);

/* A proposed switch is committed only after the backend changes track.
 * Stop/cancel cannot consume a shuffle entry or corrupt Previous history. */
typedef struct SkinOrderQueue { SkinOrder order,proposed; int pending,actual; } SkinOrderQueue;
void skin_order_queue_init(SkinOrderQueue *queue,unsigned long seed);
void skin_order_queue_reset(SkinOrderQueue *queue,int count,int current);
void skin_order_queue_observe(SkinOrderQueue *queue,int count,int current);
int skin_order_queue_request(SkinOrderQueue *queue,int count,int current,int direction,int natural);
void skin_order_queue_cancel(SkinOrderQueue *queue);

typedef struct SkinSelection {
    unsigned char bits[SKIN_LIST_MAX];
    int anchor, focus, count;
} SkinSelection;
void skin_selection_sync(SkinSelection *selection, int count, int focus);
void skin_selection_set(SkinSelection *selection, int index, int mode);
void skin_selection_all(SkinSelection *selection, int mode); /* 0 none, 1 all, 2 invert */

/* A borrowed view of each frontend's existing arrays. No second playlist. */
typedef struct SkinList {
    char *paths, *titles, *names;
    unsigned path_size, title_size, name_size;
    int *count, *current, *selected, *durations;
    SkinSelection *selection;
} SkinList;
typedef struct SkinListSnapshot SkinListSnapshot;
SkinListSnapshot *skin_list_snapshot(const SkinList *list);
void skin_list_restore(SkinList *list,const SkinListSnapshot *snapshot);
void skin_list_snapshot_free(SkinListSnapshot *snapshot);
int skin_list_move(SkinList *list, int from, int to);
int skin_list_move_selected(SkinList *list,int from,int to);
void skin_visual_peaks(const unsigned char *levels,unsigned char *peaks,unsigned char *hold);
int skin_list_remove(SkinList *list, int index);
void skin_list_sort(SkinList *list, int by_path);
int skin_balance_volume(int volume, int balance, int channel);

/* Only skin binaries link these controls. Audio/device work stays in the
 * established playback child. GUI analysis uses the small snapshot below. */
typedef struct SkinAudioState {
    volatile int pause_requested, paused, balance, output_stereo, completed_ok, pause_catchup;
    volatile unsigned long pause_epoch,position_ms,seek_sequence;
    volatile int position_valid,seek_seconds;
    volatile int eq_enabled, eq_preamp, eq_bands[SKIN_EQ_BANDS];
    volatile unsigned long eq_sequence;
    volatile int eq_supported, channels, bitrate, output_rate;
    volatile unsigned long visual_request, visual_sequence;
    volatile signed char visual_pcm[SKIN_VIS_SAMPLES];
} SkinAudioState;
extern SkinAudioState gSkinAudio;
void skin_eq_flat(void);
int skin_eq_apply(int *spectrum, int count, int sample_rate);
int skin_eq_gain(int value, int gain);
void skin_visual_analyse(const signed char *pcm, unsigned char *levels);

typedef struct SkinClock { unsigned long milliseconds,remainder; int rate; } SkinClock;
void skin_clock_advance(SkinClock *clock,unsigned long frames,int rate);
typedef struct SkinRect { int x,y,w,h; } SkinRect;
/* Main, playlist, EQ, artwork and settings windows. */
#define SKIN_WINDOW_MAX 5
unsigned skin_window_group(const SkinRect *rects,int count,int root,int tolerance);
void skin_window_snap(const SkinRect *rects,int count,int root,unsigned moving,int tolerance,int *dx,int *dy);
/* Rows of the skinned settings window (see skin_player.h). */
#define SKIN_SETTINGS_MAX 32
enum { SKIN_ROW_HEADER, SKIN_ROW_VALUE, SKIN_ROW_ACTION };
typedef struct SkinSettingsRow {
    short kind, id, action;
    char label[16], value[24];
} SkinSettingsRow;
/* Append a row (bounded and NUL-terminated; value may be NULL). */
void skin_settings_row(SkinSettingsRow *rows,int *count,int kind,int id,const char *label,const char *value);
/* Step a cyclic choice: +1/-1 with wrap-around in min..max. */
int skin_settings_cycle(int value,int direction,int min,int max);
#endif
