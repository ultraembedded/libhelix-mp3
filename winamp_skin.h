/* Classic Winamp bitmap skins. No Windows or toolkit dependencies. */
#ifndef MINTAMP_WINAMP_SKIN_H
#define MINTAMP_WINAMP_SKIN_H
#include <stddef.h>

#define SKIN_WIDTH 275
#define SKIN_HEIGHT 116
#define SKIN_MAX_FILE (8UL * 1024UL * 1024UL)
#define SKIN_MAX_BITMAP (2UL * 1024UL * 1024UL)
#define SKIN_MAX_TOTAL (4UL * 1024UL * 1024UL)

enum SkinAssetId {
    SKIN_MAIN, SKIN_BUTTONS, SKIN_TITLEBAR, SKIN_NUMBERS, SKIN_TEXT,
    SKIN_VOLUME, SKIN_BALANCE, SKIN_POSITION, SKIN_PLAYPAUS,
    SKIN_MONOSTER, SKIN_SHUFREP, SKIN_PLEDIT, SKIN_EQMAIN, SKIN_EQ_EX, SKIN_VISBAR, SKIN_ASSET_COUNT
};
typedef struct SkinBitmap {
    unsigned width, height;
    unsigned char *rgb;
} SkinBitmap;
typedef struct WinampSkin {
    SkinBitmap assets[SKIN_ASSET_COUNT];
    unsigned visual_colors[24];
    unsigned playlist_normal, playlist_current, playlist_background, playlist_selected;
} WinampSkin;

/* Destination must be zero-initialised. Failure leaves it unchanged. */
int skin_load_memory(WinampSkin *skin, const unsigned char *zip, size_t size,
                     char *error, size_t error_size);
int skin_load_file(WinampSkin *skin, const char *path, char *error, size_t error_size);
int skin_decode_bmp(SkinBitmap *bmp, const unsigned char *data, size_t size,
                    char *error, size_t error_size);
void skin_free(WinampSkin *skin);

enum SkinAction {
    SKIN_NONE, SKIN_PLAY, SKIN_STOP, SKIN_PREVIOUS, SKIN_NEXT,
    SKIN_BROWSE, SKIN_SETTINGS, SKIN_PLAYLIST, SKIN_RADIO,
    SKIN_QUIT, SKIN_SELECT, SKIN_SIZE, SKIN_VOLUME_SET, SKIN_SEEK,
    SKIN_UNSUPPORTED, SKIN_DRAG, SKIN_TRACK_SELECT, SKIN_TRACK_PLAY,
    SKIN_PLAYLIST_OPTIONS, SKIN_PLAYLIST_CLOSE, SKIN_PLAYLIST_SCROLL,
    SKIN_PAUSE, SKIN_BALANCE_SET, SKIN_SHUFFLE, SKIN_REPEAT, SKIN_TIMER,
    SKIN_VISUAL, SKIN_EQ, SKIN_DEBUG, SKIN_VIS_RATE, SKIN_SHADE,
    SKIN_LIST_ADD, SKIN_LIST_REM, SKIN_LIST_SEL, SKIN_LIST_MISC, SKIN_LIST_FILE,
    SKIN_TRACK_MOVE, SKIN_TRACK_REMOVE, SKIN_ABOUT, SKIN_INTERFACE, SKIN_PLAYLIST_SHADE,
    /* 2.0: Classic/skin switching and parity with the classic window. */
    SKIN_CLASSIC, SKIN_FILTER, SKIN_INFO, SKIN_STREAM, SKIN_SEEK_STEP,
    SKIN_ICONIFY, SKIN_ART,
    /* Skinned settings window: value = frontend row id, target = +1/-1. */
    SKIN_SETTING_CHANGE
};
typedef struct SkinState {
    char title[256],debug_text[80];
    int elapsed, total, volume, playing, mono, bitrate, rate;
    int pressed, scroll, playlist_visible, active;
    int paused, balance, shuffle, repeat, remaining, shaded, eq_visible;
    int visual_mode, filter;
    unsigned char levels[16], peaks[16];
    signed char scope[64];
} SkinState;
typedef void (*SkinBlit)(void *ctx, int asset, int sx, int sy,
                          int width, int height, int x, int y);
typedef void (*SkinFill)(void *ctx, unsigned rgb, int x, int y, int w, int h);

#define SKIN_PLAYLIST_HEIGHT 232
#define SKIN_PLAYLIST_ROWS 17
typedef struct SkinPlaylistState {
    int count, selected, current, top, total_seconds, unknown_durations;
    int shaded, active, pressed;
    unsigned char selection[128];
    char rows[SKIN_PLAYLIST_ROWS][80];
    int durations[SKIN_PLAYLIST_ROWS];
} SkinPlaylistState;
typedef void (*SkinLabel)(void *ctx, const char *text, unsigned rgb,
                         unsigned background, int x, int y, int width);
int skin_playlist_top(int top, int count);
int skin_playlist_hit_test(int x, int y);
void skin_playlist_render(const WinampSkin *skin, const SkinPlaylistState *now,
                          const SkinPlaylistState *old, SkinBlit blit,
                          SkinFill fill, SkinLabel label, void *ctx);

typedef struct SkinEqState {
    int enabled, preamp, bands[10], pressed, shaded, active;
} SkinEqState;
void skin_eq_render(const WinampSkin *skin,const SkinEqState *now,
                    const SkinEqState *old,SkinBlit blit,SkinFill fill,void *ctx);
/* Coordinates use the unscaled 275x116 layout. Unsupported optional sheets
 * use neutral controls; missing mandatory background/buttons/text fail load. */
int skin_hit_test(int x, int y);
int skin_slider_value(int action, int x);
void skin_render(const WinampSkin *skin, const SkinState *now,
                 const SkinState *old, SkinBlit blit, SkinFill fill, void *ctx);
#endif
