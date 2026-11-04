# AmigaOS 3 / m68k-amigaos-gcc port notes

This tree now has a correctness-first Amiga m68k backend in `real/assembly.h`.
Define `AMIGA_M68K` when building for AmigaOS 3, or rely on the automatic
GNU m68k target detection used by m68k-amigaos-gcc.
The backend deliberately uses portable C `long long` fixed-point helpers first;
68020 inline assembly optimizations should be added only after comparing their
output against this fallback.

> **Current status:** the m68k fast paths enabled by MintAMP's release recipes
> are validated and hardware-tested. Some CLI/API names still contain `exp` or
> `Experimental` because those names were introduced during development and are
> retained for compatibility. Do not interpret the old spelling as a current
> stability warning. See `docs/optimization-status.md` for the authoritative
> release status and the separate 68060 safety rules.

## Minimal decoder build

Build the command-line decoder with the public decoder files, the common table
file, every portable RealNetworks backend C file, and the `pub`/`real` include
paths:

```sh
m68k-amigaos-gcc -m68020 -std=gnu89 -O2 -Ipub -Ireal \
  -o amiga_mp3dec amiga_mp3dec.c mp3dec.c mp3tabs.c real/*.c
```

Or use the checked-in convenience makefile so the include paths and `-D` flags
stay separated correctly:

```sh
make -f Makefile.amiga
```

For the established, release-tested 68030 fast build, use:

```sh
make -f Makefile.amiga fast030
```

The equivalent expanded command is:

```sh
m68k-amigaos-gcc -m68030 -std=gnu89 -O3 -fomit-frame-pointer \
  -Ipub -Ireal \
  -DAMIGA_M68K -DAMIGA_M68K_ASM -DAMIGA_M68K_ASM_FDCT32 \
  -DAMIGA_FAST_POLYPHASE -DAMIGA_M68K_ASM_POLYPHASE \
  -DAMIGA_M68K_ASM_IMDCT -DAMIGA_M68K_ASM_MIDSIDE \
  -DAMIGA_M68K_ASM_INTENSITY -DAMIGA_M68K_ASM_HUFFMAN \
  -o amiga_mp3dec.fastexp amiga_mp3dec.c mp3dec.c mp3tabs.c real/*.c \
  real/amiga_m68k_polyphase.S
python3 tools/amiga_fast_preferred_hunks.py amiga_mp3dec.fastexp
```

The validated Huffman m68k ASM refill path is runtime-gated. It is included in
the quality 0 preset and remains available under the legacy `--exp-huff` name
for targeted A/B testing and profiling. The `exp` spelling is retained for
compatibility; it is not a current stability label. Builds without
`AMIGA_M68K_ASM_HUFFMAN` silently stay on the portable C Huffman decoder.

Keep a space between every `-D...` define and every `-I...` include path.  For
example, `-DAMIGA_M68K_ASM_MIDSIDE-Ipub` is parsed as one malformed macro
definition, so the compiler never receives the `pub` include path and then fails
with missing `mp3dec.h`/`mp3common.h` errors.  Likewise,
`DAMIGA_M68K_ASM_IMDCT` without the leading `-D` is treated as an input file name
instead of a preprocessor define.

After linking, `Makefile.amiga` runs `tools/amiga_fast_preferred_hunks.py` on the
Amiga executable.  The tool clears explicit chip-only or fast-only allocation
bits in the load-file hunk table, leaving the normal AmigaDOS "any memory, prefer
Fast RAM" setting.  This makes the program code/data load into Fast RAM when Fast
RAM is available while preserving compatibility with chip-only systems.


`--exp-poly` is the legacy compatibility name for the validated 68030 mono
polyphase assembly path in `AMIGA_M68K_ASM_POLYPHASE` builds when
`real/amiga_m68k_polyphase.S` is linked. If the macro is set but the optional
assembly source is omitted, the weak asm reference resolves as unavailable and
the decoder falls back to the existing `AMIGA_FAST_POLYPHASE` C path instead of
failing at link time. The option name is retained for A/B and self-test flows;
it does not mean the assembly path is untested.

The command-line decoder embeds an AmigaOS `$STACK:262144` cookie, requesting a
minimum 262,144-byte stack without requiring users to run the `Stack` command
first.

For a native smoke build on a non-Amiga host, use a portable backend define such
as `-D__riscv` only if your compiler supports the helper assembly for that
architecture; otherwise build with `-DAMIGA_M68K` to exercise the plain-C
fallback.

On a real 68020+ target, the Huffman cache's full 16-bit refill uses one
potentially unaligned `move.w` through `LOADBE16`.  No `REV16` byte swap is
needed because m68k is already big-endian.  68000/68010 and non-m68k smoke
builds retain the safe two-byte C fallback.

### Playback cleanup diagnostics

Playback resources are owned by one audio-player lifecycle and released through a
single cleanup path. `--debug-cleanup` reports reaped/aborted writes, device and
Exec object deletion, Chip/work-buffer release, debug-build canary checks, and
input-file closure. `MINIAMP3_DEBUG` builds also emit AmigaDOS `Printf()`
cleanup trace lines for each submitted slot/channel, including `CheckIO`,
`AbortIO`, `WaitIO`, device close, request deletion, buffer frees, and cleanup
invocation count. `--selftest-play-cleanup` repeats a tiny silent
`audio.device` submission and complete teardown five times; the older
`--play-lifecycle-test` spelling remains as an alias.

The playback implementation does not call `CurrentDir()`, `Lock()`, `Forbid()`,
`Permit()`, `Disable()`, or `Enable()`, so playback cleanup does not own a current
directory lock or interrupt/task-switch nesting state.


## Pluggable decoder modules and FLAC/AAC playback

MintAMP can now play non-MP3 formats through loadable Amiga hunk decoder
modules.  The host scans the configured decoder-module directory for
`*.decoder` files, calls the module ABI entry point, and matches the selected
file extension against the module extension list.  MP3 remains built in; other
formats use the same Paula streaming path once the module has produced signed
16-bit PCM blocks.

FLAC support is provided by `decoders/flac.decoder`, a standalone module built
from `decoders/flac_entry.c`, `decoders/flac_module.c`, the allocator shim in
`decoders/flac_alloc.c`, and the `libfoxenflac` submodule.  Initialise the FLAC
submodule, then build the modules with either the top-level Amiga makefile or
the decoder makefile directly:

```sh
git submodule update --init decoders/flac
make -f Makefile.amiga decoder-modules
# or
make -C decoders flac
make -C decoders aac
```

AAC m68k asm helpers are enabled by default for these builds.  Pass `AACASM=0`
to either the top-level Amiga makefile or `make -C decoders aac` only when you
need the plain C fallback.

OGG/Vorbis support is provided by `decoders/ogg.decoder`, built from
`decoders/ogg_entry.c`/`ogg_alloc.c`/`ogg_module.c` plus the vendored Tremor
(fixed-point Vorbis) and libogg submodules:

```sh
git submodule update --init decoders/tremor decoders/libogg
make -C decoders ogg
```

Like AAC, Tremor's m68k asm helpers (`decoders/tremor/src/tremor/asm_m68k.h`,
covering `MULT32`/`MULT31`/`MULT31_SHIFT15`/`CLIP_TO_15`) are enabled by
default. Pass `OGGASM=0` to build Tremor with its plain C fallback instead.

`make -f Makefile.amiga`, `fast030`, `gui`, and `guir` all depend on
`decoder-modules`; the module makefile now builds both `flac.decoder` and `aac.decoder` by default.  The FLAC module is linked with `-nostartfiles`, keeps its allocation
inside the module through an Exec `AllocMem`/`FreeMem` shim, and audits for
forbidden libc/startup symbols before producing `flac.decoder`.

At runtime, keep the decoder modules next to MintAMP or in the discovered
module directory.  Use `--debug-decoder` when diagnosing module discovery, ABI
revision checks, extension matching, stream format probing, or generic decoder
startup.  Current FLAC support targets subset FLAC streams up to 48 kHz stereo;
the playback engine downmixes/resamples through the same mono, stereo,
fast-lowrate, fake-stereo, volume, and buffer handling used for MP3 playback.

## GUI builds

Two native Amiga frontends are available.  `MintAMP-GT` is the Workbench
2.x/3.x GadTools frontend; `MintAMP` is the ReAction/ClassAct frontend for
newer 3.x systems.  Both reuse the command-line decoder/playback core and both
participate in the decoder-module build, so MP3 stays built in while FLAC, AAC, and
future formats are supplied by `*.decoder` modules.

```sh
make -f Makefile.amiga gui      # GadTools: MintAMP-GT
make -f Makefile.amiga guir     # ReAction/ClassAct: MintAMP
# or the explicit target names
make -f Makefile.amiga miniamp3  # compatibility alias for MintAMP-GT
make -f Makefile.amiga minimp3r  # compatibility alias for MintAMP
```

## AmiSSL HTTPS smoke test

Build `amissl_https_get` to test the AmiSSL HTTPS GET path without involving
the player, audio decoders, GUI, or Radio Browser integration:

```sh
make -f Makefile.amiga amissl-get-test
```

Run it on AmigaOS to fetch only the response headers from
`https://ice1.somafm.com/groovesalad-128-mp3`.  A successful transport smoke
test prints the HTTP/ICY status line and the `Content-Type` header, which should
be `audio/mpeg` for the Groove Salad MP3 stream.

`MintAMP-GT` opens the MintAMP-GT window with an ASL file requester whose pattern is
expanded from built-in MP3 plus discovered decoder-module extensions such as
FLAC and AAC.  Internet radio URLs can be entered from
Project/Internet Stream.  The main window includes a speed-mode control; an
output selector for Stereo, Mono, or Fake stereo; fast-mem, sample-rate,
quality, buffer, and volume controls;
metadata/artwork fields; progress and time displays; transport buttons for Play,
Next, Stop, the Paula hardware filter, and the playlist window; a status bar;
and a file-info row.  The GUI uses `gadtools.library` through the AmigaOS m68k
pragmas, so no `-lauto` linker flag is needed.

The quality cycle maps directly to the same decoder quality levels used by
`amiga_mp3dec`, and GUI playback always passes an explicit `--quality N` value:

- **Faster** passes `--quality 0`: maximum decoder speed and the most aggressive
  optimisations.
- **Fast** passes `--quality 1`.
- **Normal** passes `--quality 2`.
- **Best** passes `--quality 3`: least aggressive optimisation.

The Fast-mem checkbox remains independent of decoder quality, so unticking
Fast-mem is always obeyed.  When an internet stream is selected the GadTools
frontend clears Fast-mem and omits `--fast-mem`, matching the ReAction frontend,
because stream inputs are live sockets rather than finite seekable files.

The buffer slider chooses the requested `--buffer-seconds` value from 1 to 10
seconds. Paula's per-write limit can shorten it at higher sample rates; once
playback begins both GUIs show the actual selected half-buffer duration rather
than implying the full request was possible. The Volume slider stores
`ENVARC:MintAMP/Volume` as 0-100% and maps it to `audio.device` `ioa_Volume`
0-64, so 0% is silent and 100% preserves the previous full-volume request
value. Volume changes are shared with the embedded playback subprocess and
applied to the next safe `CMD_WRITE` submission without changing PCM samples.
The GUI rate selector cycles through 8287, 8820, 11025, 14700, 22050, and
28600 Hz.
Superfast is a fast-lowrate variant rather than a separate exclusive mode; when
Superfast is ticked the rate selector narrows to its supported 11025 and 22050 Hz
choices. Playback still
uses the Paula streaming implementation inside `amiga_mp3dec`. The GUI launches
the playback subprocess at normal priority so CPU-bound decoding does not starve
the Workbench/GadTools event loop. Stop requests set the same interrupt flag
used by Shell playback, signal the child,
and the audio wait path can abort/reap outstanding writes so the GUI remains
responsive. During local-file playback the status bar keeps change-only
playback text visible instead of redrawing generic `Playing` on each timer tick.
The GadTools Internet Radio dialog now exposes the same practical Radio Browser
controls as the ReAction version: name/codec/country search, preset country
codes, HTTP/HTTPS/all URL filtering, 10/25/50/100 result limits, maximum bitrate
filtering, favourites, playable-result selection, and Up/Down selection buttons.
The Up/Down buttons follow the selected result and keep it visible, while the
listview's own scrollbar remains independent.  Closing and reopening Internet
Radio preserves the last search, filters, results and selection for the current
MintAMP session.  Project/Iconify hides the GadTools window as a Workbench
AppIcon; double-click the AppIcon to restore the player while playback continues.
The ReAction edition provides the native title-bar iconify gadget, uses the
program's own icon, and offers the same Project/Iconify menu action.
For Internet radio streams, both GUI frontends show a stable `Streaming` status
while audio is flowing and only change it for connection, dropped-stream
reconnect, stop, or error states. The GUI file-info row reports the first MPEG frame channel mode
as `mono`, `stereo`, `joint-stereo`, or `M/S` when the Layer III joint-stereo
mode-extension bit marks mid/side stereo. Artwork greyscale conversion uses a
68030-friendly m68k inline path in fast builds that replaces the previous
per-pixel divide with an 8-bit luma approximation.

Speed mode is now a single cycle gadget:

- **Normal**: no fast-lowrate flags.
- **Fast**: adds `--fast-lowrate` for supported non-28600 Hz playback rates.
- **Superfast**: adds `--fast-lowrate --superfast-lowrate`, capping sparse
  low-rate synthesis work for 8287, 8820, 11025, 14700, or 22050 Hz output.
- **Ultrafast**: uses Superfast for low-rate playback, or `--ultrafast` at
  28600 Hz to cap IMDCT work to 26 subbands.
- **22050 Mono Ultrafast**: a CD32/030-oriented preset that forces 22050 Hz
  mono-style cost, enables `--fast-lowrate --superfast-lowrate`, adds reduced
  taps, and applies `--subband-cap 12` for maximum throughput.

The playlist window supports Add, Remove, Clear, and Play actions, ASL
multi-select where available, `Next`/automatic end-of-track advance, and M3U
Load/Save.  M3U loading accepts `#EXTM3U`/comment lines, keeps absolute Amiga
volume paths as-is, resolves relative entries against the playlist drawer, and
adds entries up to the internal playlist limit.  Saving writes `#EXTM3U` plus
one path per playlist entry.

A separate library can still be added later if multiple frontends need to share
higher-level application code. For now, `amiga_mp3gui.c` includes the existing
CLI frontend with its `main` renamed internally, keeping a single compiled GUI
source while reusing the public decoder and Paula playback implementation.

## Usage

```sh
amiga_mp3dec [options] infile.mp3 outfile
amiga_mp3dec --info infile.mp3
amiga_mp3dec --play [--stereo|--fake-stereo] [--rate 8287|8820|11025|14700|22050|28600] [--quality 0|1|2|3] [--buffer-seconds N] [--volume N] [--fast-mem] infile.mp3
amiga_mp3dec --play [playback options] infile.flac
amiga_mp3dec --play [playback options] infile.aac
amiga_mp3dec --selftest-play-cleanup [--debug-cleanup] [--buffer-seconds N]
```

Default output is raw signed 16-bit big-endian PCM. If `outfile` names an
Amiga volume or directory and ends in `:`, `/`, or `\`, the decoder creates
an output file there from the input basename, using `.pcm`, `.s8`, or `.8svx`
for the selected output format.  For example, `RAM:` with `song.mp3` writes
`RAM:song.pcm`. Options:

- `--mono` mixes stereo input down to mono before writing.
- `--s8` writes raw signed 8-bit PCM.
- `--8svx` writes mono Amiga IFF-8SVX signed 8-bit output.
- `--fibdelta` writes 8SVX with Fibonacci Delta compression. The compressed
  `BODY` contains the two D1 predictor bytes plus one packed delta nibble per
  `oneShotHiSamples` output sample, so odd sample counts end with a padded low
  nibble.
- `--bench` prints elapsed time, realtime decode speed, and timing buckets for
  frame decode, PCM conversion/downsampling, 8SVX writing, Fibonacci
  compression, and low-level file writes. In `--play` mode it also prints the
  audio-device underrun count. Playback mode always prints underruns at exit so
  target runs can show whether streaming kept up.
- `--info` prints the input file size, ID3v2 text metadata and embedded-artwork
  size, first MPEG audio frame details, and ID3v1 metadata when present. Used as
  `--info infile.mp3`, it exits after inspection without decoding. It can also
  be combined with `--play` (or a normal decode command) to print the metadata
  before playback or decoding begins.
- Non-MP3 playback is handled by generic decoder modules.  If the input
  extension is not `.mp3`, `--play` opens the file through a matching
  `*.decoder` module, currently including `.flac`/`.fla` when
  `decoders/flac.decoder` is installed and `.aac` when
  `decoders/aac.decoder` is installed.  The module reports sample rate,
  channel count, bit depth, and total samples, then feeds the common Paula
  playback pipeline.  Use `--debug-decoder` for module-directory scanning,
  LoadSeg, ABI, extension-list, and stream-format diagnostics.
- `--play` is the supported AmigaOS Paula streaming mode used by the CLI and GUI
  editions. It opens `audio.device`, decodes to mono signed 8-bit PCM into
  Fast RAM work buffers, and bulk-copies each completed half-buffer into the
  chip-memory buffers submitted to `audio.device`. The default playback rate is
  8287 Hz for 030 safety; `--rate 8820`, `--rate 11025`, and `--rate 14700`
  are accepted, `--rate 22050` is a supported/high-CPU mono-first mode that may
  underrun on slower 030 systems, and `--rate 28600` selects the PAL-top Paula
  mode. Playback rates up to 22050 Hz imply `--fast-lowrate`; 28600 Hz uses normal
  post-decode decimation because it is not an integer fast-lowrate stride. 22050 Hz fast-lowrate
  playback prints `22050 requires significantly more CPU and may underrun on
  030 systems.`
  Playback prints the requested and actual output rates when fixed stride output
  differs, and calculates the PAL audio period from the actual output rate using
  rounded `3546895 / actual_output_rate` ticks, so 22050 Hz uses period 161, 14700 Hz uses period 241, and 28600 Hz uses period 124.
  Playback automatically uses the reduced-
  overhead fast path: checksums run only with `--checksum`, timing buckets and
  decode-core profiling run only with `--bench`, and export/8SVX/Fibonacci state
  is not touched while streaming to `audio.device`.
- `--stereo` is opt-in only and applies only with `--play`. It keeps the default
  mono path unchanged, writes signed 8-bit samples per channel, preserves decoded
  left/right channels for stereo MP3 input where possible, and duplicates mono
  MP3 input to both output channels. Streaming stereo playback opens separate
  `audio.device` allocations for one left Paula channel and one right Paula
  channel, converts decoded interleaved signed 16-bit PCM into planar signed
  8-bit Fast RAM work buffers, and uses one bulk copy per channel into the
  Paula chip-memory submission buffers when each half-buffer is submitted. Stereo
  supports `--rate 8820`, `--rate 11025`, and `--rate 14700` first; `--rate
  22050` is supported as a high-CPU stereo mode, and `--rate 28600` is available
  as the PAL-top Paula mode. `--rate 8287` is mono-only.
  Enabling stereo prints `Stereo playback needs significantly more CPU and may
  underrun on 030.`
- `--fake-stereo` is an opt-in `--play` alternative to true `--stereo` for systems
  that cannot decode real stereo in real time. It decodes a single (mono) channel
  — so it costs about the same CPU as mono playback — then synthesises a stereo
  impression with an energy-symmetric cross-delay widener:
  `L = mono + (delayed >> shift)`, `R = delayed + (mono >> shift)`. Because the
  two channels are symmetric in `mono`/`delayed`, `E[L^2] == E[R^2]` for any
  stationary input, so neither channel is louder than the other (a plain
  `L=mono+w`/`R=mono-w` comb instead leans correlated bass into one channel,
  making the other sound quieter). It is mutually exclusive with `--stereo`.
  `--fake-stereo-delay N` sets the delay line length in output samples (1-256,
  default 96 ≈ 11 ms at 8820 Hz); `--fake-stereo-shift K` sets the cross-bleed
  `>>K` (0-8, default 2; higher K = wider, 0 = mono). It is an effect, not the
  real left/right mix, so hard-panned material is not reproduced faithfully.
  In the GUI it is the "Fake-st" checkbox and applies when Mono is also selected.
- `--selftest-fake-stereo` verifies the widener's mono-compatibility invariant
  (`L + R == 2*mono`), the delayed-difference width term, and the zeroed warm-up
  region, without playing audio.
- `--play-fast-path` is accepted as an explicit alias for `--play`; the normal
  `--play` mode already uses this reduced-overhead streaming path.
- `--volume N` sets the `audio.device` request volume for playback, from 0 to 100 percent (default 100). The implementation maps this integer range to `ioa_Volume` 0-64 with rounded integer arithmetic; both stereo channel requests receive the exact same value. Live GUI changes are observed by the playback task through a volatile percent plus sequence counter and are applied to the next submitted buffer, so latency is bounded by the queued-buffer duration and active writes are not aborted just to change volume.
- `--selftest-startup-volume` verifies the startup volume mapping used for the first playback requests without playing audio: 0% maps to `ioa_Volume` 0, 50% to 32, and 100% to 64 for both mono and stereo request setup.
- `--buffer-seconds N` chooses the requested playback depth for each half of the
  `--play` double buffer; the default is 4 seconds for safer 030 playback. Values
  must be positive integers; values above 10 seconds are clamped to 10 seconds.
  Playback now keeps separate Fast RAM conversion buffers and chip-memory
  `audio.device` submission buffers; mono copies one completed half-buffer at
  submit time, while stereo copies the completed left and right planar buffers.
  If the requested 22050 Hz or stereo buffer set is too large for available
  memory, playback automatically retries with smaller
  half-buffers and prints the reduced byte count instead of failing immediately.
  Playback prints the selected half-buffer duration and byte size at startup.
  Each Paula channel submission is capped at 65534 bytes, so high-rate settings
  such as 22050 Hz or 28600 Hz automatically use shorter half-buffers in both
  streaming playback and `--decode-then-play` instead of decoding far more audio
  than one `CMD_WRITE` can play. When this cap shortens the requested depth,
  playback prints the maximum half-buffer duration for the selected rate; for
  example, mono 28600 Hz is limited to about 2291 ms per half-buffer. Playback
  reports total underruns,
  per-buffer underruns, late-buffer count, and the
  minimum measured spare time before a playing buffer ended at exit.
- `--fast-mem` preloads the complete compressed local input into Fast RAM before
  decoding or playback starts. It applies to the built-in MP3 decoder and to
  modular AAC, FLAC, Ogg, WMA, WAV and other local decoder formats. On AmigaOS
  builds it requests `MEMF_FAST`, so the input
  does not consume chip RAM needed by Paula buffers. MintAMP disables the
  checkbox when the selected file will not fit in available Fast RAM. During
  startup it reports that it is copying the input to Fast RAM. This removes
  filesystem and slow-HDD reads from the realtime decode/refill loop and prints
  the allocated byte count at startup. The option fails before playback if the complete input
  cannot be sized, read, or allocated; it never silently falls back to disk.
  Unlike `--decode-then-play`, it stores only the compressed MP3 and continues to
  decode into the normal two-slot playback queue while Stop cleanup is
  being validated on real hardware, so it normally needs less RAM than
  `--decode-then-play`. It intentionally preloads synchronously: background HDD I/O on a
  CPU-limited 68030 could still steal decode time at unpredictable points.
  For slow disks, start with `--play --fast-mem`; if decode-time spikes can still
  exhaust the queued audio, also increase `--buffer-seconds` toward 10.
- MintAMP GUI startup diagnostics are compiled out by default so release builds
  do not write `T:MintAMP-startup.log` or show internal startup stages. Developers
  can enable the detailed GUI startup log, watchdog text, task/process pointers,
  and embedded audio-open diagnostics by building with `-DMINIAMP3_DEBUG`, for
  example `make -f Makefile.amiga gui EXTRA_CFLAGS=-DMINIAMP3_DEBUG`.
- `--debug-play` prints startup diagnostics for Paula streaming, including the requested volume percent, mapped `ioa_Volume`, initial request volume, selected live-update method, volume sequence count, and the
  actual output rate, PAL period, requested buffer depth, selected half-buffer
  samples/bytes, chip submission buffer addresses/sizes, optional stereo work
  buffer addresses/sizes, buffer A/B fill samples/bytes, queued A/B `CMD_WRITE` startup, every A/B `CMD_WRITE`
  submit/complete milestone, underrun detections, and final cleanup counts for
  completed/aborted outstanding I/O, freed buffers, and closed audio devices. The
  streaming startup path
  allocates the playback buffers before pre-filling A, B, and C by decoded
  sample count (not amplitude). Mono remains a true three-request
  `audio.device` ring: A, B, and C can all be submitted as live DMA requests.
  Stereo uses only two submitted DMA pairs, A and B, plus one Fast RAM
  decode-ahead buffer C; C is never submitted to `audio.device`, and its prepared
  left/right data is copied into whichever A/B chip pair has completed and been
  WaitIO-reaped. This preserves stereo decode-ahead slack without keeping six
  outstanding stereo I/O requests alive. A silent first
  playback buffer is accepted so valid MP3 encoder delay, padding, or fade-ins
  can play normally; with `--debug-play`, an all-zero first buffer prints
  `first playback buffer is silent/near-silent`. Playback does not skip leading
  silence by default.
- `--decode-then-play` is a `--play` debug mode that decodes the whole MP3 to
  RAM as signed 8-bit PCM first (mono by default, or stereo with `--stereo`), then plays the resulting buffer via
  `audio.device`, which helps separate decoder/streaming issues from playback
  issues.
- `--play-lifecycle-test` opens playback, allocates the playback buffers, submits
  a short silent `CMD_WRITE`, cleans up, and repeats the sequence three times.
  Use it on real hardware with `--debug-play` to verify that repeated
  `audio.device` open/submit/abort-or-wait/close and buffer free cycles leave no
  stale requests or reply messages before testing longer MP3 streams.
- `--decode-only` decodes MP3 frames and skips PCM conversion plus all output.
  The output path argument is optional in this mode.
- `--no-output` runs PCM conversion/downsampling and 8SVX/Fibonacci compression
  paths but discards bytes instead of touching an output file.  The output path
  argument is optional in this mode.
- `--rate 28600`, `--rate 22050`, `--rate 14700`, `--rate 11025`, `--rate 8820`,
  or `--rate 8287` post-decode downsamples the output with a lightweight
  nearest-sample decimator when the MP3 sample rate is higher than the
  requested output rate.
- `--fast-lowrate` is a lower-quality Amiga conversion mode for speed-oriented
  playback and conversion. It requires one of the `--rate` values
  above. In `AMIGA_M68K` + `AMIGA_FAST_POLYPHASE` builds it writes only every
  second polyphase output sample for 22050 Hz, every third for 14700 Hz,
  every fourth for 11025 Hz, and every fifth for 8820/8287 Hz. This skips
  discarded polyphase sample work, appends emitted samples through one
  cumulative low-rate output counter, and keeps the low-rate phase/stride
  state alive across granules and MP3 frames.
  For exact integer strides such as 44100 -> 11025, fast-lowrate selects the
  same source positions as normal `--rate` decimation. The 8287 Hz mode uses a
  fixed stride of 5 for Amiga-rate compatibility, so 44100 Hz input emits at
  8820 Hz and reports/plays/writes metadata at that actual emitted rate instead
  of labeling it as the requested 8287 Hz. For stride-2 22050 Hz output, the
  fast-polyphase build computes only the 16 even synthesis rows: a half-width
  FDCT path skips the unused factored-DCT half and polyphase evaluates only those
  16 output samples. The emitted PCM remains bit-identical to selecting every
  second sample from the full synthesis path. Huffman/dequant and IMDCT still
  run at full MP3 rate; stride-3/4/5 output still uses full FDCT32 (stride 3
  has a hand-unrolled C kernel and a full three-phase m68k asm kernel
  covering mono and stereo; stride 5 mono now also has a full five-phase
  asm kernel matching its already-complete stereo coverage -- see
  `--selftest-polyphase-stride3`/`--selftest-polyphase-stride5`). Stereo input
  with `--mono` is collapsed in the decoder after required MPEG stereo
  reconstruction, so the
  right-channel IMDCT/FDCT32/polyphase work and full stereo PCM copy are skipped.
  Pure mid/side joint-stereo mono output also advances over the unused coded
  side-channel Huffman payload, removing a bitrate-sensitive decode cost.
  `--bench` reports the huffman, dequant, stereo/post, imdct, subband/dct32,
  and polyphase buckets used to profile that path, plus stereo stride-2 and
  stride-4 polyphase ASM/C call counters when decode-core profiling is enabled.
- `--superfast-lowrate` is the sparse low-rate mode used by the GUI Superfast
  speed setting.  It implies fast-lowrate, defaults to 11025 Hz if no rate is
  supplied, and supports 8287, 8820, 11025, 14700, and 22050 Hz output.  It
  caps active subbands to the output bandwidth so skipped high-frequency
  IMDCT/overlap work is not generated merely to be discarded by the low-rate
  selector.
- `--rate 14700` (stride 3) costs more than its sample count suggests: the
  half/quarter DCT tricks need a power-of-two stride, so it runs the full
  32-point FDCT32 for every subband block. Three things keep that affordable:
  on 68060 builds (`AMIGA_M68K_POLYPHASE_68060`) every C DCT multiply uses
  hardware-only 32-bit multiplies instead of a libgcc `___muldi3` call per
  multiply (bit-identical); `FDCT32LowBands()` skips the reads, adds and
  zero-input multiplies of the capped, all-zero upper subbands in the first
  butterfly pass (bit-identical, used for strides 3 and 5 when no hand-written
  68030 FDCT32 is linked); and with "Faster"/"Fast" quality the same
  middle-eight-tap reduced dewindow used at 22050/11025 Hz replaces the full
  16-tap filter (`PolyphaseMono/StereoFastLowrateStride3Reduced`). `make -f
  Makefile.amiga fdct32-lowbands-test polyphase-stride3-reduced-test` checks
  them on the host.
- `--rate 14700` (stride 3) is a middle ground between 11025 Hz (stride 4)
  and 22050 Hz (stride 2). It has its own hand-unrolled C polyphase kernel
  (`PolyphaseMonoFastLowrateStride3`/`PolyphaseStereoFastLowrateStride3` in
  `real/polyphase.c`), matching the stride 4/5 style: no per-sample dispatch
  branch or loop overhead, just direct calls to the same
  `PolyphaseMonoFastSampleLo/Hi/0/16` primitives the other strides use. In
  `AMIGA_M68K_ASM_POLYPHASE` builds it also has a full asm kernel covering
  all three phases for both mono and stereo
  (`MonoFastPolyphaseStride3Phase0/1/2_Amiga_m68k` and
  `StereoFastPolyphaseStride3Phase0/1/2_Amiga_m68k` in
  `real/amiga_m68k_polyphase.S`) -- unlike stride 4, which only has an asm
  kernel for mono phase 0. `MonoFastPolyphaseStride3_Amiga_m68k_IsActive()`/
  `StereoFastPolyphaseStride3_Amiga_m68k_IsActive()` report whether the asm
  is linked in and active; `--selftest-polyphase-stride3[-stereo]` print
  both the "asm requested" (compile-time) and "asm active" (runtime) status
  alongside the correctness check. Both plain `--fast-lowrate` and
  `--superfast-lowrate` default the active-subbands cap to 10 (output
  Nyquist ~7.35 kHz) at this rate -- `MP3FastLowrateEffectiveActiveSubbands()`
  used to hardcode 16 for every stride >= 2 outside superfast mode (a
  leftover from when stride 2 was the only fast-lowrate rate), which silently
  widened stride 3/4/5's cap back out to 16 in plain fast mode and made a
  manual `--subband-cap N` a no-op unless Superfast was also ticked; it now
  reuses the same stride-derived default (and manual override) either way.
  Unlike stride 2 (`FDCT32Half`/`FDCT32HalfSparse16`) and stride 4
  (`FDCT32Quarter`), stride 3 has no equivalent `FDCT32FastLowrate()` fast
  path and still runs the full 32-row FDCT32 every call -- see the note in
  `real/subband.c`'s `FDCT32FastLowrate()` dispatch. This is a real
  algorithmic gap, not just a missing kernel: `FDCT32Half`/`FDCT32Quarter`
  prune the DCT's shared radix-2 butterfly network by skipping entire
  recursive sub-blocks whose outputs are never read, which only works
  because stride 2 and stride 4 are powers of 2 that align with that
  network's even/odd and quarter splits. Stride 3's kept-row residue class
  (every 3rd of 32 rows) is coprime to 2, so it is spread evenly across
  every sub-block at every recursion depth -- no sub-block can be skipped,
  and computing the ~11 needed rows directly (bypassing the shared
  butterfly reuse) costs more multiplies than the existing full FDCT32
  (which does all 32 outputs in ~80 muls total). A stride-3 "FDCT32Third"
  analogous to Quarter is therefore not available via this codebase's DCT
  algorithm; the subband-cap fix above (dequant/IMDCT skip inactive bands)
  and stride 3's existing full polyphase asm coverage are the safe wins at
  this rate, not FDCT32 pruning.
- `--rate 8820`/`--rate 8287` (stride 5) mono now also has a full
  five-phase m68k asm kernel (`MonoFastPolyphaseStride5Phase0..4_Amiga_m68k`
  in `real/amiga_m68k_polyphase.S`), matching the stereo coverage this
  stride already had. `MonoFastPolyphaseStride5_Amiga_m68k_IsActive()`
  reports whether it's linked in and active; `--selftest-polyphase-stride5`
  checks its output against full-band synthesis the same way the stride 3
  selftests do.
- `--rate 11025` (stride 4) mono now has asm kernels for all 4 phases
  (`MonoFastPolyphaseStride4_Amiga_m68k` for phase 0, plus
  `MonoFastPolyphaseStride4Phase1/2/3_Amiga_m68k` in
  `real/amiga_m68k_polyphase.S`) -- previously only phase 0 had an asm
  kernel, so 3 of every 4 mono stride-4 frames fell back to C. Stereo
  stride 4 already had full 4-phase asm coverage and is unaffected.
  `MonoFastPolyphaseStride4AllPhases_Amiga_m68k_IsActive()` gates the
  runtime dispatch (falling back to the original phase-0-only asm path,
  then to C, if only some phases are linked); the original
  `--selftest-polyphase-stride4` still only exercises phase 0 as before,
  and the new `--selftest-polyphase-stride4-allphases` checks all 4
  phases against full-band synthesis.
- The shared stereo tap macros in `real/amiga_m68k_polyphase.S`
  (`STEREO_SAMPLE0`/`STEREO_SAMPLE16`/`STEREO_PAIR_LO`/`STEREO_PAIR_HI`,
  used by the stride 2/3/4/5 stereo asm kernels) seed their L/R
  accumulators from the first tap's product instead of `clr.l`-ing
  `%d3`/`%d5` and adding into them, via a new `STEREO_PROD_ADD_FIRST`
  macro. This removes two instructions per sample with no change to
  the arithmetic (0 + x == x) or output. `--selftest-polyphase-stride2-stereo`,
  `-stride3-stereo`, `-stride4-stereo`, and `-stride5-stereo` all compare
  the asm kernels against `PolyphaseStereoFastLowrateCompact`/full-band C
  reference output and cover every macro touched by this change.
  `MP3GetStereoStride2PolyphaseCounters()`/`MP3GetStereoStride4PolyphaseCounters()`
  (printed at playback exit alongside the mono equivalents) report whether
  asm or C actually ran, so a regression would show up as a call-count or
  correctness change, not just a missing symbol.
- `--ultrafast` is a full-rate/high-rate speed option that applies
  `--subband-cap 26`, capping IMDCT work to roughly the first 18 kHz of a
  44.1 kHz source.  The GUI's 22050 Mono Ultrafast preset goes further by
  combining 22050 Hz Superfast mono-style playback with reduced taps and
  `--subband-cap 12` for the lowest CPU cost.
- `--debug-fastlowrate` prints one line per decoded frame/granule with the
  full-rate sample count, low-rate samples emitted, cumulative low-rate samples,
  and destination offset range used for contiguous placement.

### Quality/speed tradeoff

`--quality N` selects one of the verified fast-path combinations while leaving
the individual legacy-named `--exp-*` flags available for fine-grained override.
If `--quality` is omitted, `--fast-lowrate --rate 11025`, `--fast-lowrate --rate
14700`, and `--fast-lowrate --rate 22050` default to quality 1; all other
invocations default to quality 3.

- `--quality 0` (fastest) enables reduced taps, quarter-rate FDCT32, fast
  polyphase, and the validated Huffman ASM refill path. In builds without the
  Huffman m68k ASM path it silently falls back to the normal C Huffman decoder.
- `--quality 1` (fast) enables reduced taps and fast polyphase. This is the
  automatic default for `--fast-lowrate --rate 11025`, `--fast-lowrate --rate
  14700`, and `--fast-lowrate --rate 22050`. Reduced taps only affects
  stride 2/4 (22050/11025 Hz) output; stride 3 (14700 Hz) prints a warning
  and otherwise ignores it. The IMDCT subband cap (16 subbands) is applied at
  stride 2 and above, skipping subbands above the output Nyquist -- Superfast
  mode narrows this further per-stride (10 subbands at 14700 Hz).
- `--quality 2` (balanced) enables fast polyphase without reduced taps.
- `--quality 3` (accurate) enables no approximations and is equivalent to the
  original decoder behavior.

For 68030 realtime playback of 192 kbps and higher content, start with:

```sh
amiga_mp3dec --quality 0 --play --fast-mem --fast-lowrate --rate 11025 --mono song.mp3
```

- `--selftest-mulshift` compares the portable C `MULSHIFT32` reference with the
  optional 68020+ assembly helper over edge cases and 100,000 pseudo-random
  input pairs.
- `--selftest-clz` compares the portable C `CLZ` reference with the active
  helper over zero, one, every power of two, `0x7fffffff`, `0xffffffff`, and
  10,000 pseudo-random inputs.  In `AMIGA_M68K_ASM` 68020+ builds this proves
  the inline `bfffo` helper against the C reference.
- `--selftest-fdct32` compares `FDCT32_C_REFERENCE` against the normal `FDCT32`
  entry point, so `AMIGA_M68K_ASM_FDCT32` builds can prove the optional asm
  multiply path preserves the C operation order and fixed-point outputs.
- `--selftest-fdct32half` compares `FDCT32Half`'s stride-2 even-row stores
  against the full `FDCT32` output across every offset, odd/even block, and
  guard-bit value.  In normal optimized 68020+/68030 `AMIGA_M68K_ASM_FDCT32`
  builds, the validated m68k `FDCT32Half` asm path is requested and active when
  the target supports it and guard bits are safe; lower guard-bit cases still
  fall back to `FDCT32Half_C_REFERENCE`.  Add `--selftest-verbose` to print every
  mismatch instead of the first mismatch per failing case.
- `--selftest-imdct` compares the C IMDCT36 reference with the active IMDCT
  entry point over zero, random, edge-value, common long-window, and fallback
  window cases.
- `--selftest-polyphase` compares the C fast mono polyphase path with the active
  mono polyphase entry point, so `AMIGA_M68K_ASM_POLYPHASE` builds can prove the
  optional 68030 assembly kernel.  `--exp-poly` no longer runs this selftest
  automatically; use this standalone flag when you want to re-verify the asm
  kernel.
- `--selftest-polyphase-stride2` compares the C stride-2 mono fast-lowrate
  polyphase selection with the optional contiguous-output m68k assembly kernel.
- `--selftest-polyphase-stride4` compares the C stride-4 mono fast-lowrate
  polyphase selection with the optional 11025 Hz m68k assembly kernel over 500
  pseudo-random vbuf states.
- `--selftest-polyphase-stride4-stereo` compares the stereo stride-4 compact
  fast-lowrate polyphase kernel with the generic stereo sample selection over
  every phase and 500 pseudo-random vbuf states.
- `--selftest-polyphase-stride2-stereo` compares the stereo stride-2 compact
  fast-lowrate polyphase kernel with the generic stereo sample selection over
  every phase and 500 pseudo-random vbuf states.
- `--selftest-fastlowrate` compares a synthetic ramp/impulse-like PCM sequence
  through normal 44100 -> 11025 `--rate` decimation and the stride-4
  fast-lowrate selector across chunk boundaries.
- `--selftest-huffman` runs 1000 pseudo-random Huffman pair decode cases
  through the portable C reference and active test path to verify stable bit
  counts and coefficients.  The legacy `--exp-huff` name explicitly selects
  the validated `AMIGA_M68K_ASM_HUFFMAN` decode path at runtime; it keeps the C
  Huffman state machine and bit extraction logic, but uses inline m68k refill
  code in the hot pair loops when compiled for 68020+.
- `--selftest-dequant` compares the C dequant block reference with the active
  optional 68030 dequant inner loop for scale values -47..0 and coefficient
  magnitudes 0..8206.
- `--selftest-quality` verifies the `--quality 0` through `--quality 3` flag
  combinations, explicit `--exp-*` override preservation, the automatic
  `--fast-lowrate --rate 11025` quality 1 default, and the fact that IMDCT
  thinning stays explicitly opt-in.
- `--checksum` prints a 32-bit checksum of the decoded 16-bit PCM stream before
  optional mixing, downsampling, or output-format conversion. With
  `--fast-lowrate`, it instead covers the low-rate output samples so A/B checks
  can verify deterministic fast-lowrate output. Use it to compare the default
  polyphase path with optional optimized builds.
- `--debug-argv` or `--show-argv` prints `argc` and `argv` after Amiga
  command-tail normalization.  The normalizer also handles Amiga C runtimes
  that pass the whole command tail as one argument, including CR/LF
  whitespace and quoted paths.

The program prints the first decoded frame's input sample rate, output sample
rate when it differs, channel count, and bitrate when available, followed by
decoded frame count and output sample count.

## Optimization/reference notes

This section preserves the individual build and A/B controls used while the
m68k paths were developed. The current 68020/030/040 release bundle and the
68060 release combination documented in `docs/optimization-status.md` are
already validated; "opt-in" below means a path can still be selected separately
for reference, self-test or profiling, not that the release-enabled feature is
experimental.

1. Keep the C helper block as the reference backend for 68000 and non-GCC builds.
2. Benchmark with `--bench --decode-only`, `--bench --no-output`, and normal
   output on target hardware to separate frame decode, conversion/compression,
   and filesystem cost.
3. `AMIGA_M68K_ASM` currently optimizes `MULSHIFT32` with optional 68020+
   `muls.l` inline assembly, `CLZ` with optional 68020+ `bfffo` inline
   assembly, and the hot 4-byte bitstream refill with an optional 68020+
   `move.l (%a0)+,%d0` inline load.  Build and prove these helpers before
   enabling deeper asm paths:

   ```sh
   m68k-amigaos-gcc -m68020 -std=gnu89 -O2 -DAMIGA_M68K_ASM -Ipub -Ireal \
     -o amiga_mp3dec.asm amiga_mp3dec.c mp3dec.c mp3tabs.c real/*.c
   amiga_mp3dec.asm --selftest-mulshift
   amiga_mp3dec.asm --selftest-clz
   amiga_mp3dec.asm --selftest-bitstream
   ```

   Upstream GCC's m68k backend has a `clzsi2` pattern that emits `bfffo` when
   targeting 68020+ bit-field instructions, and marks CLZ-at-zero as 32 because
   `bfffo`/ColdFire `ff1` return 32 for zero.  This workspace did not include
   `m68k-amigaos-gcc`, so the local command-line selftest keeps the inline asm
   explicit until the exact Amiga cross-compiler build is disassembled.  Verify
   the installed compiler with:

   ```sh
   printf 'int f(unsigned x){return x?__builtin_clz(x):32;}\n' | \
     m68k-amigaos-gcc -m68020 -O2 -S -xc - -o - | grep bfffo
   ```

   If that command emits `bfffo`, a future pass can remove the handwritten
   `CLZ_AMIGA_M68K_ASM` wrapper and rely on the builtin guarded by the same zero
   check.

4. `AMIGA_M68K_ASM_FDCT32` is an exact full `FDCT32` arithmetic path for
   68020+ GNU m68k builds, tuned for the 68030. It is part of the established
   68020/030/040 release bundle and also enables the validated `FDCT32Half`
   assembly implementation for stride-2 paths when guard bits are safe. The C
   fallbacks remain callable for regression testing and unsupported/low-guard
   cases. On a 68060, do not enable the old `fdct32` group: its register-pair
   multiply form is software-emulated there; use the dedicated 68060 release
   configuration instead.

   ```sh
   m68k-amigaos-gcc -m68020 -std=gnu89 -O2 -DAMIGA_M68K_ASM_FDCT32 -Ipub -Ireal \
     -o amiga_mp3dec.fdct32asm amiga_mp3dec.c mp3dec.c mp3tabs.c real/*.c
   amiga_mp3dec.fdct32asm --selftest-fdct32
   ```


5. `AMIGA_M68K_ASM_IMDCT` is the exact long-block IMDCT36 path used by the
   established 68020/030/040 release bundle. The C IMDCT remains the reference;
   the active entry point uses a compact nine-iteration asm window/overlap
   kernel for the common long-window case and falls back to C for short blocks,
   mixed/transition windows, start/stop windows, and other uncommon cases. On a
   68060, the old `imdct` group is intentionally excluded because its
   register-pair multiply form is software-emulated; use the dedicated 68060
   release configuration instead.

   ```sh
   m68k-amigaos-gcc -m68030 -std=gnu89 -O3 -fomit-frame-pointer \
     -DAMIGA_M68K -DAMIGA_M68K_ASM -DAMIGA_FAST_POLYPHASE \
     -DAMIGA_M68K_ASM_FDCT32 -DAMIGA_M68K_ASM_IMDCT -Ipub -Ireal \
     -o amiga_mp3dec.imdctasm amiga_mp3dec.c mp3dec.c mp3tabs.c real/*.c
   amiga_mp3dec.imdctasm --selftest-imdct
   ```

6. `AMIGA_M68K_ASM_POLYPHASE` is the validated 68030 mono fast polyphase
   kernel used by the established 68020/030/040 path. The C fast mono path
   remains callable as the reference; `--selftest-polyphase` compares it
   against the active asm hook. The legacy `--exp-poly` spelling is retained
   for compatibility and A/B testing; it does not indicate experimental status.

   ```sh
   m68k-amigaos-gcc -m68030 -std=gnu89 -O3 -fomit-frame-pointer \
     -DAMIGA_M68K -DAMIGA_M68K_ASM -DAMIGA_FAST_POLYPHASE \
     -DAMIGA_M68K_ASM_POLYPHASE -Ipub -Ireal \
     -o amiga_mp3dec.polyasm amiga_mp3dec.c mp3dec.c mp3tabs.c real/*.c \
     real/amiga_m68k_polyphase.S
   amiga_mp3dec.polyasm --selftest-polyphase
   ```

7. `AMIGA_M68K_ASM_MIDSIDE` is a validated 68020+ GNU m68k implementation of
   the joint-stereo mid/side reconstruction loop. It is part of the established
   68020/030/040 bundle and the validated 68060 release combination. It keeps
   both channel pointers and guard-bit masks in registers, uses `dbf` for the
   bounded sample loop, and performs each absolute value with a branchless
   `asr`/`eor`/`sub` sequence. Keep the checksum/selftests when changing it so
   both release paths remain regression-protected.

   ```sh
   m68k-amigaos-gcc -m68030 -std=gnu89 -O3 -fomit-frame-pointer \
     -DAMIGA_M68K_ASM_MIDSIDE -Ipub -Ireal \
     -o amiga_mp3dec.midsideasm amiga_mp3dec.c mp3dec.c mp3tabs.c real/*.c
   amiga_mp3dec.midsideasm --bench --decode-only --checksum stereo-joint.mp3
   ```

   `real/dequant.c`'s `CollapseStereoToMono()` (plain LR-encoded stereo
   source decoding to mono output -- e.g. 22050 Mono Ultrafast on a
   non-joint-stereo 128 kbps file) shares this same `AMIGA_M68K_ASM_MIDSIDE`
   flag for its own near-identical asm loop, since pure mid/side joint-stereo
   sources skip this function entirely via the `monoMSSideSkipGranule` fast
   path in `mp3dec.c` (mid = (L+R)/2 is already the coded mid channel, so
   there's nothing to collapse) -- only non-MS-eligible stereo sources
   (plain "Stereo" mode, or joint-stereo with intensity) reach
   `CollapseStereoToMono()` at all. Verify with
   `--selftest-collapse-stereo-mono`, which cross-checks the asm against a
   C reference across edge-case magnitude patterns (including `INT_MIN`,
   which the branchless `FASTABS` trick wraps back to `INT_MIN` -- verified
   the asm reproduces that exact quirk rather than diverging on it) before
   trusting it on a real file.

   `MidSideProc()`'s caller in `Dequantize()` (`real/dequant.c`) also caps
   its `nSamps` to the same `dequantSubbandCapSampleLimit` (activeSubbands+1
   guard subband, matching the antialias guard-band fix above) whenever a
   fast-lowrate subband cap is active, instead of always scanning the whole
   spectrum. Samples at/beyond that boundary were already left un-dequantized
   by `DequantChannel()`'s own cap, and neither IMDCT's subband cap nor
   `AntiAlias()`'s guard-subband read ever reach that far into
   `hi->huffDecBuf` -- so reconstructing sum/difference values there was
   wasted work, more of it the wider a file's real bandwidth (i.e. more
   noticeable on higher-bitrate joint-stereo files, which was the original
   motivation: 22050 Hz Ultrafast Mono headroom on a 192 kbps joint-stereo
   file). `--selftest-midside-subband-cap` proves this is exactly equivalent
   to running the uncapped scan against the same input with the discarded
   region zeroed (the same assumption IMDCT/AntiAlias already make about it),
   both for the reconstructed samples and for `mOut`/the guard-bit count it
   derives -- plus a direction check that capping never reports a *smaller*
   (less safe) guard-bit count than today's uncapped-against-leftover-data
   call would. Checksummed real 128/192 kbps joint-stereo decodes at 22050 Hz
   (stereo and mono output, default and manual subband caps) bit-identical
   before/after. `IntensityProc()` was deliberately left unchanged -- it's
   the smaller-magnitude case in practice at these bitrates, and safely
   capping it needs separate scrutiny of its `sfBand`/`cbi[]` boundary math.


8. `AMIGA_M68K_ASM_HUFFMAN` is the validated Huffman-pair refill shortcut for
   68020+ GNU m68k builds, tuned for 68030 and included in the supported
   release configurations where appropriate. It deliberately does not add a
   `.S` Huffman decoder and does not use `bfextu`; the C Huffman state machine
   remains authoritative while the hot pair-loop refill can use inline
   `move.l` on buffers with at least four safe bytes remaining, falling back to
   the existing 16-bit refill near the tail. The legacy `--exp-huff` spelling is
   retained for explicit A/B selection and profiling; it is not a stability
   label. Validate modifications with `--selftest-huffman` and `--checksum`.

   ```sh
   m68k-amigaos-gcc -m68030 -std=gnu89 -O3 -fomit-frame-pointer \
     -DAMIGA_M68K -DAMIGA_M68K_ASM -DAMIGA_M68K_ASM_HUFFMAN -Ipub -Ireal \
     -o amiga_mp3dec.huffasm amiga_mp3dec.c mp3dec.c mp3tabs.c real/*.c
   amiga_mp3dec.huffasm --selftest-huffman
   amiga_mp3dec.huffasm --exp-huff --bench --decode-only --checksum song.mp3
   ```

9. Checksum the C and ASM FDCT32 builds with identical inputs and output modes
   before enabling the ASM binary in a release or local deployment.  The
   required regression set is: mono 56 kbps, stereo 160 kbps, stereo 256 kbps,
   fast-lowrate 11025 Hz, and fast-lowrate 8820 Hz.

   ```sh
   m68k-amigaos-gcc -m68020 -std=gnu89 -O2 -Ipub -Ireal \
     -o amiga_mp3dec.c-ref amiga_mp3dec.c mp3dec.c mp3tabs.c real/*.c
   m68k-amigaos-gcc -m68020 -std=gnu89 -O2 -DAMIGA_M68K_ASM_FDCT32 -Ipub -Ireal \
     -o amiga_mp3dec.fdct32asm amiga_mp3dec.c mp3dec.c mp3tabs.c real/*.c

   amiga_mp3dec.c-ref --bench --decode-only --checksum mono-56.mp3
   amiga_mp3dec.fdct32asm --bench --decode-only --checksum mono-56.mp3
   amiga_mp3dec.c-ref --bench --decode-only --checksum stereo-160.mp3
   amiga_mp3dec.fdct32asm --bench --decode-only --checksum stereo-160.mp3
   amiga_mp3dec.c-ref --bench --decode-only --checksum stereo-256.mp3
   amiga_mp3dec.fdct32asm --bench --decode-only --checksum stereo-256.mp3
   amiga_mp3dec.c-ref --bench --no-output --fast-lowrate --rate 11025 --checksum stereo-160.mp3
   amiga_mp3dec.fdct32asm --bench --no-output --fast-lowrate --rate 11025 --checksum stereo-160.mp3
   amiga_mp3dec.c-ref --bench --no-output --fast-lowrate --rate 8820 --checksum stereo-160.mp3
   amiga_mp3dec.fdct32asm --bench --no-output --fast-lowrate --rate 8820 --checksum stereo-160.mp3
   ```

   On a 68030, compare `elapsed seconds`, `decode speed`, and, when built with
   `AMIGA_PROFILE_DECODE`, `timing core subband/dct32` between the C reference
   and ASM binaries.  If any PCM checksum differs, do not define
   `AMIGA_M68K_ASM_FDCT32` for the default build.

   Define `AMIGA_FORCE_FDCT32_HALF_C` when you want to keep the full `FDCT32` asm
   path but force `FDCT32Half()` back to `FDCT32Half_C_REFERENCE` for regression
   testing or benchmarks.  A normal optimized build should report
   `FDCT32Half asm requested: yes`, `FDCT32Half asm active: yes`, and
   `FDCT32Half selftest failures: 0` from `--selftest-fdct32half`; the forced-C
   comparison build should keep failures at zero while reporting the half asm
   inactive.

   ```sh
   make -f Makefile.amiga clean
   make -f Makefile.amiga fast030
   amiga_mp3dec.fastexp --selftest-fdct32half --selftest-verbose
   amiga_mp3dec.fastexp --bench --no-output --fast-lowrate --rate 22050 --checksum song.mp3

   make -f Makefile.amiga clean
   make -f Makefile.amiga fast030 EXTRA_CFLAGS=-DAMIGA_FORCE_FDCT32_HALF_C
   amiga_mp3dec.fastexp --selftest-fdct32half --selftest-verbose
   amiga_mp3dec.fastexp --bench --no-output --fast-lowrate --rate 22050 --checksum song.mp3
   ```

10. `AMIGA_FAST_POLYPHASE` is a selectable Amiga/m68k polyphase synthesis path
   for 68020+ builds. It keeps the original implementation available when the
   flag is omitted, but replaces the 64-bit polyphase accumulator with 32-bit
   fixed-point high-multiply terms to reduce 68030 inner-loop overhead:

   ```sh
   m68k-amigaos-gcc -m68020 -std=gnu89 -O2 -DAMIGA_FAST_POLYPHASE -Ipub -Ireal \
     -o amiga_mp3dec.fastpoly amiga_mp3dec.c mp3dec.c mp3tabs.c real/*.c
   ```

11. Compare default and `AMIGA_FAST_POLYPHASE` builds with identical inputs,
   output modes, and checksum reporting:

   ```sh
   m68k-amigaos-gcc -m68020 -std=gnu89 -O2 -Ipub -Ireal \
     -o amiga_mp3dec.c-ref amiga_mp3dec.c mp3dec.c mp3tabs.c real/*.c
   m68k-amigaos-gcc -m68020 -std=gnu89 -O2 -DAMIGA_FAST_POLYPHASE -Ipub -Ireal \
     -o amiga_mp3dec.fastpoly amiga_mp3dec.c mp3dec.c mp3tabs.c real/*.c
   amiga_mp3dec.c-ref --bench --decode-only --checksum song.mp3
   amiga_mp3dec.fastpoly --bench --decode-only --checksum song.mp3
   amiga_mp3dec.c-ref --bench --no-output --fibdelta --rate 22050 --checksum song.mp3
   amiga_mp3dec.fastpoly --bench --no-output --fibdelta --rate 22050 --checksum song.mp3
   ```

12. Check final Amiga binaries for libgcc 64-bit helper calls before measuring:

   ```sh
   m68k-amigaos-nm -u amiga_mp3dec.asm | egrep '__muldi3|__ashrdi3|__lshrdi3'
   ```

   `MULSHIFT32` should not add those calls when `AMIGA_M68K_ASM` is enabled.
   `AMIGA_FAST_POLYPHASE` should remove the polyphase dependency on `MADD64` and
   `SAR64`; any remaining helper symbols should be attributed to non-polyphase
   paths before investigating them one at a time.