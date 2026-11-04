# MintAMP 2.0 acceptance: PR #521

This checklist is an **unexecuted hardware release gate**. No Amiga emulator,
Workbench session or physical audio.device target is available in the current
workspace. Cross-compilation and host tests cannot verify uninterrupted sound,
actual icon launching, installed OS libraries or Paula/RTG timing.

## Changes reviewed

- **Skin first, Classic on a tick:** MintAMP-SR/SGT open the skin by default.
  *Classic interface* (ticked menu item in both interfaces) switches the
  running player between the skin and the classic MintAMP window without
  stopping playback, and is remembered for the next launch. A live switch
  defers ENVARC writes to clean shutdown.
- Skin parity with the classic window: stream URL requester, file/stream
  info, artwork window (docking, persisted), Workbench AppIcon iconify from
  the minimise button, audio filter tick, Left/Right seek and Up/Down volume.
  Menus are split into MintAMP/Windows/Options with live check marks.
- Artwork colour pens are now released before the classic window closes
  (Settings hide/iconify) and rebuilt when it reopens, instead of leaking.
- A lightweight `MintAMP-Launch` dispatches to MintAMP-SR or MintAMP-SGT
  for both appearances; MintAMP and MintAMP-GT stay directly launchable.
  It starts a CLI process even from Workbench, copies arguments via DOS, uses
  the installation's PROGDIR, and transfers segment/IO ownership on success.
  There is no shell command evaluation or resident launcher task.
- CLI > launcher ToolTypes > shared `MintAMP2` preferences > skinned ReAction
  defaults. The skin editions retain legacy Skin/Scale and direct icon syntax.
- Late skin shutdown persists the newest shared ENV preferences, so an older
  live size cannot overwrite a newer next-launch choice. A mocked DOS/icon
  regression exercises this ordering, cancellation and Workbench lock handling.
- Next-launch interface preferences in all four menus; optional launcher icon
  examples are disabled so they do not defeat saved preferences.
- Active/inactive titlebars, remaining-time sign artwork, pressed EQ controls,
  correct EQ presets destination, playlist scrollbar pressed sprite.
- Playlist/EQ shade, bounded selected-block drag and 4 Hz edge autoscroll.
  Current track, focus, selection, titles and durations follow the permutation.
- Bounded VISCOLOR.TXT and EQ_EX.BMP loading, cached visualiser colour strip,
  integer peak hold/decay. No per-frame allocation or audio-child analysis.
- Known playlist durations occupy a separate right-aligned column; long
  titles cannot hide them. Duration labels no longer invalidate a second playlist click. Failed skin
  replacement restores the EQ as well as the playlist. Unshading clamps to
  the screen and skin/scale changes preserve shaded window heights.
- Ordinary skin mouse releases write ENV (RAM); ENVARC persistence and shared
  interface preferences flush at clean shutdown after the decoder is retired.
  Forced termination may lose the latest disk preferences.
- Application version is 2.0-dev on this development branch; CPU drawers carry
  the launcher, mint-leaf skin, legacy methods, matched modules and documentation.

## Automated evidence

Run the commands in BUILD-RELEASE.txt. Existing arithmetic, MPEG-2.5 decode,
actual MP3 disabled/flat EQ equivalence, playlist/radio regressions and skin
loader tests remain in use. Additional checks cover:

- four-way dispatch, field-by-field precedence, lower-case/invalid values,
  missing/unknown CLI options, URL arguments, bounded AmigaDOS quoting;
- preserved classic and colour Workbench icon bytes and serialized ToolTypes;
- every nonempty 8-row selection mask, selected origin and destination,
  with exact identity/selection/current-track/focus/duration remapping;
- peak hold/decay, malformed/duplicate/oversized VISCOLOR and EQ_EX entries;
- shade/unshade clipping, focus/pressed state rendering, stationary rendering;
- host CPU timings for 100,000 integer spectrum+peak updates; 100,000 stationary
  renders require zero graphics calls; changing all bars/peaks needs at most
  33 calls, using cached gradients.

Host timings describe the host process only. They are **not** 68030/68060 spare
CPU, radio buffering or underrun measurements. Common skin checks include the
bundled MintAMP mint-leaf and the supplied classic Winamp 2.91 skin. There is
no claim of testing the entire Internet's classic skin collection.

## Remaining classic Winamp differences

| Area | Remaining difference |
| --- | --- |
| EQ audio | Coarse MP3-only integer spectral shaping; not Winamp biquad curves/all-format EQ. Flat and disabled bypass remain bit-identical. No binary EQF/automatic per-track presets or computed graph. |
| Visualiser | 16 linear bins, 64 sampled PCM points, 1/2/4 Hz, submission-limited freshness. VISCOLOR rows and peak hold/decay are supported, but the high-rate Winamp visualiser response/grid/log bins are not reproduced. |
| Playlist | Fixed 275x232/17 rows, Topaz rather than Windows font; native requesters for popup actions. No free resizing, embedded transport/visualiser or cross-application drop target. Shade displays the first visible row. |
| Shaded windows | EQ_EX title artwork supported; its miniature volume/balance sliders and main miniature seek/time controls are not implemented. Shade does not rearrange the docked group. Missing EQ_EX uses a labelled fallback. |
| Main controls | Minimise iconifies to an AppIcon; double size is on D and the Windows menu. The options button opens Settings rather than a popup menu (Intuition menus are right-button). No pause blink/easter-egg/clutterbar parity. |
| Timing/seeking | Buffer-boundary pause and output clock, 99:59 cap, local MP3 bitrate/VBR approximation; other formats/live radio do not acquire new seek support. |
| Artwork | Shared screen pen mapping can quantise colours on ECS/AGA/RTG. No REGION.TXT shaped/translucent windows, cursor skins, extended NUMS_EX glyphs, WAL skins or Windows DLL plugins. |
| Native integration | Settings use a skinned settings window (classic window only as a fallback); the radio browser remains the established native utility window. Artwork options are settings rows. Star ratings, artwork refresh/reload, bench mode and the progress bar are classic-window only. |

## Setup and record sheet

Use a separate clean installation/preferences backup. Match every decoder to
its CPU drawer. Record build SHA, GCC version, CPU/clock/FPU, OS/Kickstart,
Chip/Fast RAM, screen width/height/depth, TCP/IP/AmiSSL versions, buffer length,
subband cap and playback quality. Start with spectrum and EQ **off**.
Record before/after underruns, minimum spare time, perceived glitches and
available Chip/Fast RAM. Keep station/format/bitrate and network constant for
native-versus-skinned comparisons. Do not count intentional stops as underruns.

| Target | Required focus |
| --- | --- |
| A1200 68030 | AGA 640x256/low-colour Workbench, normal-size mint-leaf and 2.91; memory recovery and radio headroom. Also check a supported ECS public screen. Use GadTools where ReAction is absent. |
| FPU-less 68060 | Dedicated 68060 drawer and lowrate060/huffman/midside/planars8 path; no FPU required by the application or installed decoders/libraries. Check actual CPU exception/emulation behaviour and spare time with AAC+. |
| PiStorm RTG | P96 8-bit and true-colour screens; pen mapping, refresh, window grouping, 2x scaling, skin switching, metadata/artwork and long radio sessions. Use the CPU-compatible drawer. |

For **each** target:

1. **Workbench/CLI:** double-click MintAMP-Launch, test all four GUI/MODE
   combinations, lower-case YES/NO and SKIN paths containing spaces. Shift-open
   one track/playlist. Run equivalent Shell arguments and launch each old icon.
   Change preferences, quit/reboot, verify persistence; enable/disable a
   ToolType and confirm the override. Try missing frontend/skin and cancel the
   requester. Confirm PROGDIR follows the installation, not the music drawer.
2. **Formats:** play representative MP3 (CBR/VBR/MPEG-2.5), AAC and AAC+, FLAC,
   Ogg Vorbis, WMA, WAV and IFF/8SVX. Use supported source bitrates. Play a mixed
   playlist twice; repeated next/previous/stop must not corrupt child/module
   state. Confirm EQ unsupported decoders bypass cleanly.
3. **Long audio measurement:** play the same MP3 and AAC+ radio station for
   30 minutes in native and skinned mode, with HTTP and HTTPS samples. Record
   underrun deltas/minimum spare time. Repeat skin run at 1/2/4 Hz, then MP3 EQ
   separately, then both if headroom permits. A new repeatable underrun/glitch
   regression with identical settings fails acceptance; reduce optional load
   only when documenting a hardware limit, not to conceal a default-mode bug.
4. **Controls:** pause/resume at least 20 times, short and long radio pauses,
   stop during buffering and shutdown, seek local CBR/VBR MP3, elapsed/remaining
   time, shuffle without immediate repeats, history, repeat list/track, balance.
   Verify the acknowledged buffer latency and that new input replaces old audio.
5. **Utility windows:** open/close radio, artwork, settings, native and skinned
   playlist/EQ repeatedly while playing. Check ICY titles/station fallback and
   artwork arrive and update without silent menu failures or increased underruns.
6. **Skin/Classic switching:** while playing MP3 and HTTPS radio, switch
   Skin → Classic → Skin 20 times from both menus, including with the skin
   shaded, iconified-then-restored, paused (must resume in Classic), with
   playlist/EQ/artwork open and with Settings already visible. No audio gap
   beyond the measured baseline, no stale windows/AppIcons, and the next
   launch opens the last interface. Start with `--classic`, switch to the skin
   and confirm the CLI `--skin` and saved EQ/balance apply.
   Iconify from the minimise button and the menu; restore by double-click.
   Check stream URL entry, file info, artwork updates and arrow keys.
   **Skinned settings:** T opens the settings window and never the classic
   window; change every row in both directions, stopped and while playing
   (decoder rows refuse with a notice), then switch to Classic and confirm
   the classic gadgets/menus show the same values, and that they persist
   after a relaunch. Use *Classic mode: Switch now* from the window itself.
7. **Skin/UI:** mint-leaf, 2.91 and at least two independently sourced classic
   skins; compare screenshots at normal and double size. Press/release outside
   controls, focus/unfocus each window, shade/unshade near screen bottom,
   grouped dragging/snapping and Ctrl-detach. Refuse 2x on screens too small.
   Confirm playlist/EQ remain visible through skin changes and failed loads.
8. **Playlist:** Ctrl/Shift selection, double-click rows with known durations,
   contiguous/noncontiguous block drag in both directions, hold at both edges
   with the mouse stationary, scrollbar, Delete, sort, rollback of empty/bad
   imports. Track sound/current identity must stay correct after array edits.
9. **Cleanup:** alternate skins and scales 20 times, toggle windows 50 times,
   quit/relaunch five times. Chip/Fast memory and obtained pens must return to
   the expected baseline with no cumulative loss, stale tasks or stuck IO.
   Save/load presets and verify enabled-flat/disabled MP3 sound is unchanged.

Record results and any failing reproducer on PR #521. Keep it unmerged until
these hardware gates have an owner and recorded outcomes.
