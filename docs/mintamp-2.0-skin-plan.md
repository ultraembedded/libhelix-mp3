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

## Phase 2: artwork in the skin (done, please test)

- **MB.BMP frame.** MB.BMP is the artwork window's frame (your choice). The
  loader reads it as an optional sheet, plus `MbBG`/`MbFG` from PLEDIT.TXT.
  Skins without it keep the plain box.
- **Bigger art.** 128x128 square (64 when a doubled frame does not fit),
  smoothed 2x from the frontends' 64x64 thumbnail. The track title shows in
  the frame's bottom text box.
- **Cheaper drawing.** Each new image is mapped to pens once into a cached
  bitmap; repaints and moves are one blit.
- **MintAMP 2.0 branding.** The bundled mint-leaf skin's main title bar and
  MB.BMP title now read "MintAMP 2.0" (the MB title used to say "Internet
  Radio"); the settings window title reads "MINTAMP 2.0 SETTINGS".

Still possible later: a true 128x128 decode. The 64x64 thumbnail is shared
with the classic window and the artwork cache, and the GadTools decoder
accumulates directly into a 64x64 grid, so a sharper image needs a second
skin-only decode path in both frontends. Worth doing only if the smoothed
enlargement looks too soft on real hardware.

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
9. Press A: the artwork window opens in the MB.BMP frame titled "MintAMP
   2.0", art 128x128 with the track title in the bottom box. Its close
   button closes it; it docks with the others. At double size on a
   256-line screen it uses the 64 square. Check colours/pens on AGA and RTG,
   and that changing tracks updates art and title without flicker.
10. The main window's title bar reads "MintAMP 2.0" (active and inactive).
