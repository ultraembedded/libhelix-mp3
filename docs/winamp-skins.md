# MintAMP classic Winamp skin editions

MintAMP 2.0 opens **skinned first**. A ticked menu item, **Classic interface**,
switches the running player to the classic MintAMP window and back again,
without stopping playback. MintAMP-SGT uses GadTools utility windows;
MintAMP-SR uses ReAction/ClassAct utility windows. Both contain the skinned
Intuition player, playlist, EQ and artwork windows *and* the complete classic
MintAMP window. Playback, decoders, Internet radio, favourites, artwork and
settings remain owned by the established frontend/backend. MintAMP and
MintAMP-GT remain supported classic-only builds (smaller, no skin code).
This is partial classic Winamp 2.x parity, not a Windows audio engine or
plug-in host.

## Build and install

```sh
make -f Makefile.amiga skins RADIO=1 SSL=1 CPU=30
make -f Makefile.amiga skins RADIO=1 SSL=1 CPU=60 ASM60_GROUPS='lowrate060 huffman midside planars8'
```

Individual targets are `sgt` and `sr`. Use separate build directories or clean
when changing CPU/compiler/flags; keep CPU-matched AAC, FLAC, Ogg, WMA, WAV and
IFF decoder modules with the executable. SSL requires the existing AmiSSL SDK
and runtime. New audio/UI calculations use integer arithmetic. For an explicitly
soft-float application build, pass `EXTRA_CFLAGS=-msoft-float`; also validate
external modules and installed libraries on the intended FPU-less machine.
The existing compiler/CPU optimisation recipes are unchanged.

Application playback timing, output-duration/benchmark reports and decoder
self-test RMS diagnostics now use integer arithmetic, with no runtime
`float`/`double` calculations or floating-point printf arguments. Millisecond
conversion preserves the Amiga clock's 20 ms resolution and guards overflow.
Decimal reports keep their usual precision; benchmark speed uses integer
milliseconds, and self-test RMS is reported in whole counts (rounded down).
Rare oversized diagnostic sums report overflow explicitly. Explicit 32-bit
limbs avoid introducing emulated 64-bit multiply/divide instructions. External
libraries are outside this application-source check.

The supplied mint-leaf skin is bundled at `PROGDIR:Skins/MintAMP-mintleaf.wsz`
and opens by default in both editions on a fresh installation. Keep the `Skins`
drawer beside the executables; `make release` and CI artifacts include it.
Saved skin choices, the `SKIN` tooltype and `--skin` override the default.
Other classic `.wsz` skins remain selectable through the player menu. If the
chosen skin is unavailable, the skin requester opens; cancelling or failure to
open the skin window retains the native frontend. Valid skins open directly,
without first flashing the native main window. Workbench menu colours/fonts
are used.

```text
MintAMP-SGT --skin Work:Skins/MintAMP-mintleaf.wsz
MintAMP-SR Music:track.mp3 --skin Work:Skins/another.wsz --double
```

Workbench tooltypes are `SKIN=Work:Skins/MintAMP-mintleaf.wsz` and `DOUBLESIZE=YES`.
Normal/double sizes are 275x116 and 550x232. Scaling covers artwork, hit testing,
EQ, playlist and text. Oversized windows/scales are refused or fall back.

## Skin or Classic: one player, a tick in the menu

Start `MintAMP-Launch` (or the MintAMP-SR / MintAMP-SGT icon). The skin opens
first by default. To use the classic MintAMP window instead:

- **Skin → Classic:** right-click the skin, *MintAMP ▸ Classic interface*.
  The skin windows close and the classic MintAMP window becomes the main
  interface (its close gadget now quits).
- **Classic → Skin:** untick *Project ▸ Classic interface* in the classic
  window. The skin reopens with its saved skin, size, positions, playlist,
  EQ and artwork windows, and the classic window hides.
- *Settings* (T) opens a skinned **settings window**, not the classic
  player. It holds only the saved settings (see [Settings window](#settings-window));
  its last row, *Classic mode: Switch now*, is a second way to the classic
  interface. Only a skin without PLEDIT.BMP, or a screen too small for even
  four rows, falls back to the classic window as a settings window.

Playback, the playlist, the radio browser and native playlist windows carry
on across a switch. Pending pause is released when moving to Classic, which
has no pause control. Balance and MP3 EQ chosen in the skin stay in effect in
Classic (and are restored at a Classic start), matching Winamp's behaviour of
keeping DSP independent of the skin. The choice is remembered for the next
launch (ENV immediately, ENVARC on clean exit after audio retirement), and a
failed skin load or window open keeps the interface that is already visible.

### Launcher, ToolTypes and precedence

`MintAMP-Launch` dispatches to MintAMP-SR (ReAction) or MintAMP-SGT (GadTools)
for both appearances, always passing `--skinned` or `--classic`. It loads the
sibling executable and creates a CLI process with copied arguments, inherited
installation PROGDIR and owned NIL IO handles. It does not evaluate a shell
command or stay resident.

| GUI | MODE | Edition | Opens with |
| --- | --- | --- | --- |
| REACTION | SKINNED | MintAMP-SR | Skin (default) |
| REACTION | CLASSIC | MintAMP-SR | Classic MintAMP window |
| GADTOOLS | SKINNED | MintAMP-SGT | Skin |
| GADTOOLS | CLASSIC | MintAMP-SGT | Classic MintAMP window |

`MODE=NATIVE`, the 2.0-dev spelling, is accepted as `CLASSIC`. MintAMP and
MintAMP-GT are not launched by MintAMP-Launch; start them directly for the
smallest classic-only footprint.

Workbench ToolTypes: `GUI=REACTION`/`GADTOOLS`, `MODE=SKINNED`/`CLASSIC`,
`SKIN=PROGDIR:Skins/MintAMP-mintleaf.wsz`, `DOUBLESIZE=YES`/`NO`.
The main launcher icon contains **disabled** examples in parentheses; remove
parentheses only to force an option. Values are case-insensitive.

```text
MintAMP-Launch --gui GADTOOLS --classic
MintAMP-Launch Music:track.mp3 GUI=REACTION MODE=SKINNED DOUBLESIZE=YES
MintAMP-SR Music:track.mp3 --classic
MintAMP-Launch --prefs
```

Field-by-field precedence is **CLI > ToolTypes > shared MintAMP2 preferences >
defaults** (ReAction, skinned, normal size, bundled mint-leaf). A direct
MintAMP-SR/SGT start reads its own icon's `MODE` ToolType and accepts
`--skinned`, `--classic`, `--mode VALUE` or `MODE=VALUE`. Skin/scale options
apply to the first skin opened in a run, even after a Classic start; later
Classic → Skin switches reuse the per-edition saved skin so a skin loaded in
the meantime is kept. Workbench shifted audio/playlist paths resolve from
their own directory lock. Only one startup file is forwarded.

Every frontend has **Interface preferences...** in its Project/player menu to
choose the framework, appearance and size for the next launch. Explicit
icon/CLI options still win. The standalone `--prefs` command saves
immediately. A forced exit can lose the latest disk preferences.

## Main controls

| Control | Behaviour |
| --- | --- |
| Play / X | Start playback; with no loaded input, play the selected/first playlist entry; resume a paused decoder; no restart while already playing |
| Pause / C / Space | Toggle buffer-boundary pause/resume |
| Stop / V | Existing interrupt, IO retirement and child shutdown path |
| Previous / Z; Next / B | Play a playlist entry after the old child exits; shuffle uses previous/forward history |
| Shuffle / J | Visit every entry once per cycle; avoid immediate repeats between cycles |
| Repeat / L | Cycle off, playlist and track; track repeat applies to natural completion, not explicit Next |
| Volume | Drag; existing volume setting/save path |
| Balance | Drag left/right attenuation using Paula channel volumes; mono output reports unavailable |
| Position | Drag a preview, seek on release; only an unpaused local MP3 with known duration |
| Timer | Click to toggle elapsed/remaining; remaining requires a known duration |
| Eject / O | Existing native audio/playlist requester |
| U / menu Open stream URL | HTTP/HTTPS URL requester; stops the current input, then plays the stream |
| I / menu File info | Title, artist, album, track, genre and format; station, ICY title, bitrate and URL for radio |
| Left / Right | Seek 5 s (same unpaused local MP3 rule as the position bar) |
| Up / Down | Volume up/down 5% |
| A / menu Artwork | Toggle the artwork window |
| Menu Audio filter | Tick toggles the Amiga low-pass filter/power LED, shared with the classic FLT control |
| Minimise button / menu Iconify | Close all skin windows to a Workbench AppIcon; double-click it to restore them. Playback continues |
| Menu Classic interface | Switch live to the classic MintAMP window (see above) |
| EQ / E | Toggle the skinned equaliser |
| PL / P | Toggle the compact playlist, with native fallback |
| T / menu Settings | Toggle the skinned settings window (classic window only as a fallback) |
| R / menu Internet Radio | Existing browser, favourites and network options |
| Menu About | Existing frontend version and decoder credits requester |
| Visualisation area / menu | Cycle off, spectrum and oscilloscope |
| Menu Visual rate | 1, 2 or 4 analysis updates per second |
| Menu Playback statistics | Buffer length, spare time, underruns, radio buffered bytes and source/output details; optional title-line overlay |
| S / menu Load skin | Transactional skin replacement |
| D / menu Double size | Normal/double size (ticked while doubled) |
| W / shade button | Main windowshade; graphics and visual analysis stop while shaded |
| Close / menu Quit / Ctrl-C | Stop and quit |

Right-click any skin window except the EQ for three menus: **MintAMP**
(open audio/stream, radio, file info, settings, interface preferences,
Classic interface, iconify, about, quit), **Windows** (playlist, full
playlist options, equaliser, artwork, load skin, double size; visible windows
are ticked) and **Options** (visualisation, visual rate, audio filter,
playback statistics). The main title shows `Artist - Title` when both tags
are known.

The GadTools radio browser opens on the player's public screen and shortens
its results list to fit 256-line screens. It requires at least 548x200; screen
or allocation failures are reported in the player instead of silently doing
nothing. About remains available without opening the native settings window.

Play/pause/stopped indicators follow backend state. Pause is acknowledged only
when the queued audio has finished: latency can be several configured buffers.
The decoder and any prepared/decode-ahead PCM are retained; resume continues
from that state. Refilling a mono buffer can add resume latency.
Stop still interrupts and retires the whole IO ring. Intentional pause and ring
catch-up are excluded from underrun accounting. A long radio pause can exhaust
network buffers or encounter a server timeout; no reconnect/resume is faked.

Elapsed time counts completed output samples, rather than assuming every MP3,
AAC or generic decode block contains 1152 samples. Updates are **buffer-granular**,
not a sample-accurate moving cursor. MPEG-2.5 and variable-sized blocks are
handled; pause accounts for all drained slots once. The classic four-digit
counter caps at 99:59. MP3 seeking remains the backend's bitrate-based estimate,
particularly approximate for VBR, with existing queued-buffer latency. Other
formats and live radio explicitly reject seeking.

Bitrate is updated from MP3 frame headers (or the radio bitrate hint where the
module supplies none); sample rate and mono/stereo describe the source, while
statistics identify mono/stereo Paula output separately. Unknown bitrate stays
unknown/zero. Scrolling titles use live ICY metadata with station-name fallback.
Both editions use the same bounded GUI animation rate.

## Playlist editor

PLEDIT.BMP supplies the frame; PLEDIT.TXT [Text] supplies Normal, Current,
NormalBG and SelectedBG colours. Missing/invalid colours use classic defaults.
ROM Topaz 8 supplies labels; Windows fonts are not loaded. This is another view
of the **same native arrays**, with shared move/remove/sort operations.

| Button | Operations |
| --- | --- |
| ADD | Files via the existing multi-file requester; one directory level of supported audio files; append playlist |
| REM | Selected rows; confirmed missing local files; clear |
| SEL | All, none, invert |
| MISC | Stable title/filename or full-path sorting; focused row up/down |
| LIST | Replace, save, append M3U/M3U8/PLS using the existing parser/writers |
| List options / menu Full playlist options | Complete native playlist, including URLs and radio favourites |

Menus use small native requesters so they remain readable on AGA screens.
Click selects; Ctrl-click toggles; Shift-click extends an anchored range.
Double-click plays on release, so beginning a drag does not start playback.
Return/Space plays; Up/Down navigates; Shift extends selection; Ctrl-Up/Down
moves the focused row. Keypad 7/1 goes first/last and keypad 9/3 pages up/down;
Delete removes selected rows, Ctrl-A selects all. Escape/P closes the playlist.
Drag a selected row vertically to move the selected block, preserving its
order even for noncontiguous selection. Clicking an already selected row keeps
the block until a plain click is released without dragging. Holding at the
upper/lower list edge autoscrolls one row per quarter-second, including when
the pointer is stationary or the main window is shaded. Release commits once;
losing focus cancels the drag. The scrollbar supports clicking and dragging
and its pressed artwork is used.

Current, focused and selected entries are remapped with array edits. A native
list replacement invalidates obsolete shuffle history and pending row starts.
Skin-originated sorting/reordering retains the selection mask. The native view
has its existing single-focus selection UI; the extra multi-selection mask is
used by the skin. Cancelled, unreadable, empty, oversized or wholly unusable
loads retain the previous list. Replace uses a temporary rollback snapshot,
not a second persistent playlist. Imports remain bounded to 128 entries/96 KiB.
Directory addition is nonrecursive and bounded to 4096 examined entries.
Missing removal preserves URLs and permission/other ambiguous IO failures.

EXTINF/PLS Length values and durations learned for the current local track are
shown and preserved on save. The bottom total sums known lengths and appends
`+` when any lengths are unknown. Tracks are not decoded/probed en masse simply
to populate durations, and URLs are not contacted for missing-file checks.

The compact playlist remains fixed at 275x232 (17 rows), scaled at double size.
Its shade button now toggles a 14-pixel view; the EQ has the same shade toggle.
EQ_EX.BMP supplies shaded EQ artwork, with a labelled fallback when absent.
Sprite-based resizing and miniature shade sliders are not implemented. Missing artwork, insufficient memory or a small
screen uses the native playlist. Native settings/favourites remain accessible.

## Settings window

The skin edition's settings live in their own skinned window: the
PLEDIT.BMP title, side and scrollbar sprites with PLEDIT.TXT colours and
ROM Topaz rows, like the playlist. It docks and snaps with the other skin
windows (Ctrl-drag detaches), has the same right-button menus, and closes
with its close button, Escape or T. It opens beside the main window and
remembers its place for the rest of the session.

| Section | Rows |
| --- | --- |
| Playback | Speed mode, quality, subbands, output rate, channels, fake-stereo width/delay, buffer, decode first, Fast RAM preload, audio filter, HTTPS wait |
| Artwork | Artwork on/off, colour artwork, artwork cache, clear cache |
| Interface | Skin (load another), double size, next-launch preferences, Classic mode (switch now) |

Click a row (or Return/Space, Right) for the next value; Shift-click (or
Left) for the previous one. Up/Down move between rows, the scrollbar or
cursor keys scroll when the screen shows fewer than 19 rows (double size
on a 256-line screen). Values use exactly the classic window's handlers:
the same limits, status messages and saved ENV/ENVARC variables, so a
later switch to Classic shows the same choices. Decoder settings (speed,
quality, subbands, rate, channels, fake stereo on/off, buffer, Fast RAM) are
refused with a notice while a track plays, as the GadTools classic window
does; they apply to the next playback. Star ratings, artwork refresh/reload,
bench mode and the progress bar remain classic-window features.

## Equaliser

EQMAIN.BMP supplies a separate 275x116 window, on/off, preamp, ten sliders and
presets. The classic band labels are 60, 170, 310, 600 Hz, 1, 3, 6, 12, 14 and
16 kHz. Gains/preamp are integer dB in -12..+12; combined gain is limited to
-24..+12 dB, with saturation/guard-bit recomputation before Helix IMDCT.

This is an **optional MP3-only spectral EQ**, default off. Cached integer gains
are applied to existing dequantised/alias-processed coefficients; no second
engine, sample-by-sample floating-point filters or FPU requirement is added.
An enabled flat EQ is a bit-identical bypass. The response is coarse,
particularly at low frequencies/short blocks, and does not reproduce Winamp's
biquad filter curves. Source/output Nyquist limits and the existing fast-lowrate
subband cap still apply. Bands wholly outside output bandwidth reject dragging;
partly retained bands have a reduced usable range. AAC, FLAC, Ogg, WMA, WAV and
IFF bypass EQ and report unsupported enable requests.

Flat and Bass cut presets plus save/load use the bounded `MintAMP-EQ-1` text
format (`.eq`), with preamp followed by ten gains. This is not Winamp binary
EQF compatibility. Invalid preset values leave settings unchanged. Automatic
per-track presets report unsupported. The skin graph is decorative; there is
no computed response curve. Missing EQMAIN reports unavailable. EQ settings
persist separately per skin edition. Test audio headroom and CPU spare time
before leaving EQ on for radio or a 68030 configuration.

## Visualisation and performance

The main area offers a coarse 16-bin spectrum or 64-point oscilloscope, default
off. A GUI request causes the audio-buffer submission path to copy only 64
signed 8-bit samples from the already prepared planar PCM, averaging stereo.
The request/acknowledgement handshake makes the snapshot stable while GUI code
reads it. There is no full-buffer copy, allocation, DFT or drawing inside audio
submission. Spectrum analysis uses a bounded 64-point integer DFT on the GUI
task; scope uses the same snapshot. VISCOLOR.TXT supplies the classic 24 RGB rows
(background, gradient, scope colours and peak colour), with bounded parsing and
defaults for missing/invalid data. A cached three-pixel gradient strip needs
one clipped blit per spectrum bin. Integer peaks hold for two visual updates,
then decay one pixel per update; no extra analysis cadence or audio-child
work is introduced. Logarithmic binning is not implemented.

Analysis is limited to the selected 1/2/4 Hz and stops when disabled, paused,
stopped or main-shaded. Snapshot freshness is **limited by buffer submissions**:
large audio buffers can make even a 4 Hz visualisation refresh look static for
several seconds. A GUI-only quarter-second timer also scrolls long titles and
expires notices; it does not poll the network or alter the native playback timer.
The initial analysis rate is 2 Hz; existing saved choices take precedence.
Mode/rate changes show a confirmation in the title strip. Enabling a visualiser
requests the next available PCM snapshot immediately, avoiding an extra initial
analysis interval. The 4 Hz option improves drawing cadence but cannot make
the backend submit samples more often.

Cached native bitmaps and incremental rendering remain in use. Stationary
states produce no draw calls. Palette/bitmap memory is bounded, but optional EQ
artwork adds a cached sheet; double size increases bitmap memory fourfold.
New controls compile without floating-point or emulated 64-bit multiply
instructions in the audited 68060 object. This does not prove real-time timing
on an Amiga: leave EQ/visualisation off until measured on the target system.
No claim of stutter-free enabled DSP/visualisation on a 68060 is made from host
tests alone. Native editions do not compile these new audio hooks.

## Artwork window

Album art and radio station logos decoded by the frontend appear in their
own window. A skin with **MB.BMP** (the classic mini-browser sheet) frames it
with that artwork: title bar, sides, close button and the bottom bar, whose
text box shows the track title in PLEDIT.TXT `MbBG`/`MbFG` colours (black and
green by default). The window is 150 pixels wide with a 128x128 art square,
or 64x64 when the doubled 190-pixel frame would not fit the screen. Skins
without MB.BMP keep the plain 74x74 box in the playlist colours. An
undersized MB.BMP (under 178x119) is ignored rather than failing the skin.

The frontends still decode a 64x64 RGB thumbnail (shared with the classic
window and the artwork cache); the 128 square is a 2x enlargement smoothed by
integer averaging, not a higher-resolution decode. Each new image is mapped
to pens once into a cached bitmap and then only blitted, so refreshes and
window moves cost one blit. The window docks and snaps with the main,
playlist, EQ and settings windows, drags from anywhere except the close
button, and has the same right-button menus. Missing art shows `NO ART`, a
station without a usable logo `RADIO / NO LOGO`, and a decode in progress
`LOADING` (GadTools). The thumbnail is only hashed while the window is open,
at the GUI tick rate; pens come from the skin's shared pen cache and are
released with the skin. Artwork/colour artwork options and
cache controls are rows in the skinned settings window. With artwork
switched off the window shows `ART OFF` and points to Settings (T).
Double-click the artwork for the file/stream information.

## Windows and persistence

Nearby main/EQ/playlist/artwork edges snap on release (eight scaled pixels). Docked
windows move together; Ctrl-drag moves one window to detach it. Moves are
bounded to the screen. Positions, visibility (including artwork), main shade, shuffle/repeat,
balance, time mode, EQ, visual rate/mode and debug overlay persist in each
edition's separate `ENV:/ENVARC:` State variable, alongside Skin/Scale. Native
audio settings and favourites keep their established storage. Restored
positions are clamped when screen/scale changes.

Native refresh handling, pressed playback/shuffle/repeat sprites, optional
artwork bounds, owned menu structures and cleanup remain shared. Classic skins
have no general hover sprite set. EQ on/off, auto, presets and close controls now animate pressed sprites
and act on release within their original hit rectangle. Shade does not compact the remaining docked windows.

## Skin compatibility

Classic `.wsz`/`.zip`: stored/deflated entries, case-insensitive names, nested
paths, bounded ZIP/CRC checks. BMP: 1/4/8-bit palette, 24/32-bit RGB, RLE4/RLE8
and uncompressed top-down. MAIN.BMP, CBUTTONS.BMP and TEXT.BMP are required.
Missing optional player artwork uses background/font/neutral fallback controls;
malformed or undersized supplied sheets are rejected. Optional PLEDIT.BMP is
at least 276x110; EQMAIN.BMP at least 275x315; MB.BMP (artwork frame) at
least 178x119, otherwise only the frame falls back. Failed replacements retain the
current resources. No files are extracted to disk.

V39+ uses obtained shared pens with disposal; V37/V38 uses the desktop palette.
AGA remapping depends on available colours; RTG/P96 improves fidelity.
REGION.TXT shaped windows, custom cursors, modern `.wal` skins and
Winamp DLL plug-ins are not supported.

## Validation

```sh
make -f Makefile.amiga skin-format-test SKIN_TEST_FILE=/path/to/base-2.91.wsz
make -f Makefile.amiga skin-controls-test skin-eq-decode-test playlist-format-test
```

Tests cover generated BMP/ZIP encodings, corrupt/truncated/duplicate/undersized
assets, optional EQ/playlist/EQ_EX sheets, VISCOLOR, failed replacement preservation, control
mapping, full/incremental player/playlist/EQ equality and stationary rendering;
shuffle cycles/history/repeat, selection/remapping/sorting/rollback/selected-block moves, balance,
output-sample clock, pending track-switch transactions, EQ arithmetic/bypass, spectral analysis and docking.
An actual MPEG-2.5 tone decode verifies disabled/flat EQ sample equality and
cut/boost output energy. Playlist tests verify timed M3U/PLS round trips.
Existing decoder arithmetic, radio metadata/conversion and favourites tests
remain part of validation. CI builds skin and native editions for both CPUs.

Real Amiga tests are still required: pause/resume/stop and ring retirement,
seek/clock latency, mono/stereo balance, AGA/P96 refreshing/palettes, skin changes,
window grouping, memory recovery, preset/settings IO, decoder transitions and
long-running MP3/AAC/HTTPS radio with underrun measurements. Test 68030, 68060
with and without FPU, and PiStorm. Host sanitizers and cross-compiles cannot
substitute for audio.device/Intuition runtime tests. GCC 16.2-rc13 is the local
validation compiler; a new GCC 13.2.0 run is not yet available here.

Sprite reference: [Webamp](https://github.com/captbaritone/webamp),
`packages/webamp/js/skinSprites.ts` (MIT). Rendering and integration are native C.

Full remaining-differences table and per-machine release gates:
[MintAMP 2.0 acceptance](mintamp-2.0-acceptance.md). This includes A1200 68030,
FPU-less 68060 and PiStorm RTG, with measured underrun/spare-time comparisons.
