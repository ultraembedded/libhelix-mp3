# MintAMP 2.0 skin edition: getting back on track

Status of the skin editions (MintAMP-SR, MintAMP-SGT), the model they should
follow, and the work in order. Classic MintAMP / MintAMP-GT and `master` are
not touched by any of this: the skin code compiles only with `MINTAMP_SKIN`.

## Where things are

- The `2.0` branch is identical to `master`. All skin work is on
  `codex/winamp-skin-frontends` (draft PR #521, base `2.0`). This plan's
  commits build on top of that branch.
- That branch builds (68030 `sgt sr`, full `release`) and passes the host
  skin, controls, launcher, preferences, EQ and playlist tests. Nothing has
  run on real hardware yet (see `mintamp-2.0-acceptance.md`).
- Before this plan, **Settings** in the skin re-opened the whole classic
  player window. That is the part that did not fit the skin edition.

## The model: two worlds, one player

| | Classic edition (MintAMP, MintAMP-GT) | Skin edition (MintAMP-SR, MintAMP-SGT) |
| --- | --- | --- |
| Main interface | Classic window | Winamp skin windows (main, playlist, EQ, artwork) |
| Settings | Inside the classic window | **Skinned settings window**: settings only |
| Utility windows | Native radio browser, playlist | Same native radio browser; skinned playlist |
| Other world | n/a | *Classic mode* switches the running player to the classic window and back |

Rules that keep it understandable:

1. In the skin edition the classic window is only shown when you choose
   *Classic interface/Classic mode*, never as a side effect of Settings.
2. Settings is a page of settings, not a player: no transport, tags or
   artwork there. It edits the same saved settings as the classic window,
   with the same code, so switching worlds never loses or reverts a choice.
3. There is always a visible way back: Classic mode in the skin menu and in
   the settings window; *Project ▸ Classic interface* (untick) in Classic.
4. If the skin cannot host a window (no PLEDIT.BMP, tiny screen), fall back
   to the classic window rather than failing.

## Phase 1: done in this update (please test)

- **Skinned settings window** (`skin_player.c`, shared by SR and SGT). PLEDIT
  frame and colours, Topaz rows, docks/snaps, scrolls when the screen is
  short, right-button menus. Rows: Playback (speed, quality, subbands, rate,
  channels, fake stereo, buffer, decode first, Fast RAM, filter, HTTPS
  wait), Artwork (on, colour, cache, clear cache), Interface (skin, double
  size, next-launch preferences, **Classic mode: Switch now**).
- Each frontend applies changes through its classic handlers
  (`skin_gadtools.inc`, `skin_reaction.inc`) and keeps the classic gadgets in
  step, so Classic shows the same values. Decoder settings are refused with
  a notice while playing (they apply to the next track), and no ENVARC write
  happens mid-stream.
- **Artwork**: `ART OFF / T FOR SETTINGS` when artwork is disabled (it used
  to say `NO ART`), colour-off shows grey art in SR as in SGT, double-click
  the artwork for file/stream info. Settings is ticked in the Windows menu.

## Phase 2: artwork in the skin (next)

The artwork window works but is small and plain (64x64 in a 74x74 box,
filled pixel runs, pens shared with the skin). Proposed, in order:

1. **Frame it with the skin.** Your mint-leaf skin ships `MB.BMP` (titled
   "Internet Radio") and `AVS.BMP`; neither is used yet. Option A: use the
   PLEDIT frame like the settings window (works for every classic skin).
   Option B: use MB.BMP when present (classic Winamp mini-browser frame).
2. **Bigger art.** The frontends decode to 64x64 (`ART_W/ART_H`, shared with
   Classic). Add a 128x128 skin-only buffer, decoded only while the skin's
   artwork window is open, so Classic and its cache stay unchanged.
3. **Cheaper drawing.** Remap the image once into a cached bitmap (as skin
   sheets are) and blit it, instead of RectFill runs on every change.
4. Optional: show art in the main window's visualiser area (76x16 is too
   small for a cover, so only as a "tiny art" visual mode).

Question for you: should the artwork window use MB.BMP (your "Internet
Radio" frame) or keep the playlist-style frame? Was MB.BMP meant for the
radio browser instead?

## Phase 3: remaining parity / polish

- Remember the settings window position/visibility across launches (needs a
  State v5 field; positions are session-only for now).
- Classic-only features to consider adding as settings rows: bench mode,
  progress bar, artwork refresh/reload. Star ratings stay classic-only.
- Skin the radio browser (MB.BMP frame) once the artwork decision is made.
- Hardware acceptance from `mintamp-2.0-acceptance.md`, including the new
  settings window gate below.

## Test checklist for this update

On MintAMP-SR and MintAMP-SGT, with the bundled mint-leaf skin:

1. Press **T** (or right-click ▸ MintAMP ▸ Settings): the skinned settings
   window opens beside the player; the classic window does **not** appear.
2. Click a row: next value; Shift-click: previous; Up/Down/Left/Right/Return
   and Escape/T to close. The notice shows in the main window's title.
3. While a track plays, change Speed mode: refused with a notice. Stop, then
   change it; play: the new mode is used. Change Audio filter while playing:
   the power LED/filter follows immediately.
4. Change several settings, then **Classic mode ▸ Switch now**: the classic
   window shows the same values. Untick *Classic interface*: back to the
   skin with the same values. Quit and relaunch: values persisted.
5. Artwork off in Settings: artwork window shows `ART OFF`; on again: art
   returns. Colour art off: grey art. Double-click the art: file info.
6. Double size (from the settings Interface row): the settings window
   reopens at double size; on a 256-line screen it scrolls.
7. Load a skin without PLEDIT.BMP: Settings falls back to the classic window.
8. Dock the settings window to the player and drag the player: they move
   together; Ctrl-drag detaches. Iconify/restore reopens it.
