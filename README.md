# MintAMP — Mini Internet Amiga Media Player

[![Build](https://github.com/boingball/MintAMP/actions/workflows/build.yml/badge.svg)](https://github.com/boingball/MintAMP/actions/workflows/build.yml)
![Version](https://img.shields.io/badge/version-1.4.0-brightgreen)
![Status](https://img.shields.io/badge/status-stable-brightgreen)
![AmigaOS](https://img.shields.io/badge/AmigaOS-3.0%2B-F28C28)
![CPU](https://img.shields.io/badge/CPU-68030%20%7C%2068040%20%7C%2068060-2F74C0)

![Formats](https://img.shields.io/badge/audio-MP3%20%7C%20AAC%20%7C%20FLAC%20%7C%20Ogg%20%7C%20WMA%20%7C%20WAV%20%7C%20IFF-7B68EE)
![Streaming](https://img.shields.io/badge/radio-HTTP%20%7C%20HTTPS-22A699)
![Output](https://img.shields.io/badge/output-Paula%20audio.device-CB4B16)
![GUI](https://img.shields.io/badge/GUI-ReAction%20%7C%20GadTools-8A2BE2)
[![GitHub stars](https://img.shields.io/github/stars/boingball/MintAMP)](https://github.com/boingball/MintAMP/stargazers)
[![GitHub last commit](https://img.shields.io/github/last-commit/boingball/MintAMP/master)](https://github.com/boingball/MintAMP/commits/master)
[![Support](https://img.shields.io/badge/Support-Buy%20Me%20a%20Coffee-FFDD00?logo=buymeacoffee&logoColor=000000)](https://buymeacoffee.com/boingball)
![AI](https://img.shields.io/badge/AI-assisted%20coding-6e7781)

**A complete multiformat player and internet-radio client for classic AmigaOS.** MintAMP plays local audio, searches and streams internet radio, displays live metadata and station artwork, and outputs directly through Paula using `audio.device` — no AHI or sound card required.

MintAMP ships with ReAction/ClassAct, GadTools and command-line editions. Its fixed-point playback engine and modular decoders have separate 68030/68040 and dedicated 68060 builds for real classic hardware and emulators.

[Build guide](docs/building-amiga.md) · [Optimization status](docs/optimization-status.md) · [Release recipe](BUILD-RELEASE.txt) · [CI builds](https://github.com/boingball/MintAMP/actions/workflows/build.yml) · [Decoder details](#decoder-modules)

<img width="713" height="692" alt="MintAMP running on Amiga Workbench" src="https://github.com/user-attachments/assets/fcc2acc5-2900-47e7-b5aa-273fd0c4520f" />

## Highlights

- **Multiformat playback:** MP3, AAC-LC/AAC+ ADTS, FLAC, Ogg Vorbis, classic WMA, PCM WAV and Amiga IFF-8SVX.
- **Internet radio:** HTTP/HTTPS MP3, AAC/AAC+, Ogg Vorbis and lossless FLAC streams, `.pls`/`.m3u` station links with mirror fallback, Radio Browser search, editable favourites, ICY title/artist updates and resilient reconnect handling. Station names and titles in any language are converted for the Amiga's Latin-1 fonts.
- **Workbench friendly:** ReAction/ClassAct and GadTools interfaces, M3U/PLS playlists mixing files and radio stations, track information, ratings, artwork and iconification while playback continues.
- **Classic output:** direct Paula `audio.device` playback with configurable rate, quality, channel mode, buffering and reduced-work modes for slower systems.
- **CPU-aware decoders:** dedicated 68060-safe multiply and hot-loop paths avoid instructions that the 68060 handles through software emulation.
- **Modular distribution:** matching AAC, FLAC, Ogg, WMA, WAV and IFF decoder modules are bundled with every release edition and CI artifact.

MintAMP 2.0 opens with a classic Winamp-compatible skin and switches live to
the classic MintAMP window with a ticked **Classic interface** menu item
(**MintAMP-SR** for ReAction, **MintAMP-SGT** for GadTools, both started by
**MintAMP-Launch**). See the
[skin editions guide](docs/winamp-skins.md) for builds, skin selection and
compatibility limits.

## Choose your edition

| Installed CPU | Release drawer | Notes |
|---|---|---|
| 68030 or 68040 | `MintAMP-v1.4.0-68030` | Established full-result m68k assembly paths. |
| 68060 | `MintAMP-v1.4.0-68060` | Dedicated 68060-safe and 68060-optimised player and decoder paths. |

Each drawer contains all three front ends:

| Program | Interface | Best for |
|---|---|---|
| `MintAMP` | ReAction/ClassAct | The full Workbench experience. |
| `MintAMP-GT` | GadTools | Systems without ReAction/ClassAct. |
| `amiga_mp3dec.fastexp` | Shell | Scripts, direct playback, testing and lower overhead. |

Keep the player and the decoder modules from the same CPU drawer together. In particular, do not use the 68030 decoder set on a real 68060: its full-result `MULS.L` instructions are software-emulated and can be dramatically slower.

## Project status

> [!IMPORTANT]
> **MintAMP 1.4 is a stable, complete application.** It is no longer just a Helix MP3 port or decoder demonstration: local playback, both GUI editions, the CLI, modular codecs and direct internet radio are integrated release features.

> [!NOTE]
> Some compatibility/debug names still contain `exp` or `Experimental` because they were named during development, including `--exp-huff`, `--exp-poly`, `MP3SetExperimentalHuffman()` and `MP3SetExperimentalPolyphase()`. Those names are retained to avoid breaking callers; they do **not** mean the release-enabled paths are untested. See [Optimization and feature status](docs/optimization-status.md).

| Area | Status | Notes |
|---|---:|---|
| ReAction and GadTools applications | Stable | Local playback, playlists, radio search, metadata, artwork and iconification. |
| MP3, AAC, FLAC, Ogg, WAV and IFF | Stable | CPU-matched modules are included with the application. |
| HTTP/HTTPS internet radio | Stable | HTTPS requires AmiSSL; certificate verification is optional at build time. |
| 68030/68040 edition | Stable | Uses the established m68k assembly path. |
| 68060 edition | Stable, hardware tested | Dedicated MP3 and decoder optimisations tested on a real 68060 Amiga. |
| Classic WMA | Compatibility-limited | WMAv1/WMAv2 only; WMA Pro, Lossless and Voice are not supported. |
| HLS/M3U8 | Out of scope | MintAMP plays direct stream URLs and `.pls`/`.m3u` station links. |

Development continues around performance, codec compatibility and UI polish, but those are improvements to a functioning player rather than missing foundations. Older development builds were named MiniAMP3/minimp3r; compatibility make targets remain where practical.

## Build both release editions

MintAMP ships as separate 68030/68040 and 68060 editions. A plain
`make clean && make release` builds only the default 68030 edition; use the
following complete recipe to build both release drawers:

```sh
make -f Makefile.amiga release-clean
make -f Makefile.amiga clean

make -f Makefile.amiga release \
  CPU=30 \
  RADIO=1 \
  SSL=1 \
  SSLCERTS=1 \
  RELEASE_NAME=MintAMP-v1.4.0-68030

# Keep the first release drawer, but remove CPU-specific objects.
make -f Makefile.amiga clean

make -f Makefile.amiga release \
  CPU=60 \
  ASM60_GROUPS="lowrate060 huffman midside planars8" \
  RADIO=1 \
  SSL=1 \
  SSLCERTS=1 \
  RELEASE_NAME=MintAMP-v1.4.0-68060
```

The clean between builds is required so 68030 objects are not reused in the
68060 edition. Do not run `release-clean` between them because it removes the
first completed drawer. The 68030 edition uses the established full assembly
path and also supports 68040; the 68060 edition uses only the selected
68060-safe optimisation groups. The same `CPU` value is passed to every
external decoder module. The recipe is also kept in
[`BUILD-RELEASE.txt`](BUILD-RELEASE.txt).

## CLI / `fast030` edition

MintAMP also includes a command-line edition named `amiga_mp3dec.fastexp`. It uses the same Paula playback engine and external decoder modules as the GUI editions, without the ReAction/GadTools interface, station browser or artwork code.

The CLI edition is useful for Shell scripts, direct file/URL playback, decoder testing and lower-overhead setups.

Build the local-media CLI:

```sh
make -f Makefile.amiga fast030
```

Build it with HTTP radio support:

```sh
make -f Makefile.amiga radio030
```

Build it with HTTP and HTTPS/AmiSSL radio support:

```sh
make -f Makefile.amiga sslradio030
```

Example playback:

```text
amiga_mp3dec.fastexp --play music.mp3
amiga_mp3dec.fastexp --play music.flac
amiga_mp3dec.fastexp --play music.ogg
amiga_mp3dec.fastexp --play sample.wav
amiga_mp3dec.fastexp --play sample.iff
amiga_mp3dec.fastexp --play "https://example.com/direct-stream"
```

Useful playback controls include `--rate`, `--quality`, `--subband-cap`, `--mono`, `--stereo`, `--fake-stereo`, `--buffer-seconds`, `--volume` and `--fast-mem`. Run the binary without arguments to display the complete option list.

The `fast030` target name is retained for compatibility. The actual target CPU is selected with `CPU=00`, `20`, `30`, `40` or `60`. MintAMP v1.4.0 release drawers are produced for the established 68030 path and a separately tuned 68060 path.

## Supported formats

| Format | Status | Notes |
|---|---:|---|
| MP3 | Working | Main supported format. Helix fixed-point decoder with m68k optimisation. Layer III in all three versions: MPEG-1 (32/44.1/48 kHz), MPEG-2 (16/22.05/24 kHz) and MPEG-2.5 (8/11.025/12 kHz). |
| AAC-LC ADTS | Working | External `aac.decoder` module. ADTS `.aac` streams/files only. |
| FLAC | Working | External `flac.decoder` module. Performance depends heavily on CPU, output rate and file complexity. |
| Ogg Vorbis | Working | External `ogg.decoder` using the fixed-point Tremor decoder and libogg. |
| WMA | Working (classic) | External `wma.decoder` for classic WMAv1/WMAv2 audio in ASF containers. WMA Pro, Lossless and Voice are not supported. |
| WAV | Working | External `wav.decoder` for uncompressed integer PCM WAV files with 8, 16, 24 or 32-bit samples, mono or stereo. |
| IFF-8SVX | Working | External `iff.decoder` for mono 8-bit samples, raw or Fibonacci-delta compressed. |
| HTTP MP3/AAC/Ogg radio | Working | `http://` MP3, ADTS AAC/AAC+ and Ogg Vorbis streams. `.pls`/`.m3u` station links and redirects are followed, trying a playlist's next server if one is down. ICY metadata supported where provided. |
| FLAC radio | Working | Lossless stations sending FLAC in Ogg (the usual Icecast form) or plain FLAC, through `flac.decoder`. Needs roughly 1 Mbit/s for 16-bit/44.1 kHz; 24-bit/high-rate streams are reduced to 16-bit and the output rate. |
| HTTPS MP3/AAC radio | Working with AmiSSL | Build with `RADIO=1 SSL=1`. Uses AmiSSL and classic-Amiga-specific teardown quarantine for stability. |
| HTTPS certificate verification | Optional | Add `SSLCERTS=1` to use AmiSSL's installed CA certificates for peer/hostname verification. |
| Radio Browser search | Working | Used by the GUI radio search. |
| JPEG artwork | Working | picojpeg-compatible baseline decoder. |
| PNG artwork | Working | LodePNG decoder, compiled into both GUI editions. |
| WebP artwork | Working | Compact VP8/VP8L decoder for station artwork and favicons. |
| ICO artwork | Working | PNG-backed ICO and simple DIB-backed favicon entries. |
| SVG artwork | Working (subset) | Fixed-point `svgdec.c` renderer; see "Artwork notes" below. |
| Playlists and favourites | Working | M3U and PLS playlists mixing files and stations, opened from the file requester, command line or a Workbench icon. Favourites can bookmark the playing stream and be renamed, removed and reordered. |
| HLS / M3U8 | Not supported | Out of scope currently. Direct stream URLs and `.pls`/`.m3u` station links only. |

## Internet radio

Radio support is enabled at build time with `RADIO=1`.

Plain HTTP radio:

```sh
make -f Makefile.amiga radio030
```

HTTPS radio through AmiSSL:

```sh
make -f Makefile.amiga sslradio030
```

ReAction/ClassAct GUI with HTTPS radio:

```sh
make -f Makefile.amiga sslguir
```

GadTools GUI with HTTPS radio:

```sh
make -f Makefile.amiga sslgui
```

### AmiSSL certificates and `SSLCERTS=1`

By default, HTTPS radio uses TLS transport but does not verify the remote certificate chain or hostname. This keeps HTTPS playback compatible with older or incomplete classic Amiga AmiSSL setups.

Add `SSLCERTS=1` to enable certificate verification:

```sh
make -f Makefile.amiga sslguir SSLCERTS=1
```

`SSLCERTS=1` defines the certificate-verification path and tells MintAMP to use AmiSSL's installed CA certificate bundle. The Amiga-side AmiSSL install must have its current root certificates available, and the system clock must be sane, otherwise valid HTTPS streams may fail verification.

Useful certificate-verifying builds:

```sh
make -f Makefile.amiga sslradio030 SSLCERTS=1
make -f Makefile.amiga sslgui SSLCERTS=1
make -f Makefile.amiga sslguir SSLCERTS=1
```

Without `SSLCERTS=1`, HTTPS radio still uses TLS transport, but certificate verification is disabled for compatibility with classic Amiga setups.

### Debug/development build

The current heavy debug build used for radio, artwork, AmiSSL and heap-guard testing is:

```sh
make -f Makefile.amiga guir RADIO=1 SSL=1 SSLCERTS=1 DEBUG=1 HEAPGUARD=1
```

This enables:

- ReAction/ClassAct GUI build (`guir`)
- internet radio (`RADIO=1`)
- HTTPS/AmiSSL transport (`SSL=1`)
- AmiSSL certificate verification (`SSLCERTS=1`)
- verbose radio/AmiSSL/artwork diagnostics (`DEBUG=1`)
- MiniAMP heap guard instrumentation (`HEAPGUARD=1`)

Use this for debugging only. It is intentionally noisier and heavier than a normal release build.

### Known working radio streams

Plain HTTP MP3:

```text
http://ice1.somafm.com/groovesalad-128-mp3
```

HTTPS MP3:

```text
https://icecast.walmradio.com:8443/classic
```

Expected metadata includes station name, genre, bitrate, content type and live ICY `StreamTitle` updates where the station provides them.

## AmiSSL stability note

Classic AmigaOS/AmiSSL teardown can be fragile when rapidly switching HTTPS streams from short-lived playback child tasks.

MintAMP therefore uses a conservative stability pattern for HTTPS radio:

- the probe path reuses a shared probe `SSL_CTX` rather than creating/freeing one for every station probe
- probe SSL objects are quarantined on dangerous close/EOF paths
- playback child tasks skip/quarantine `SSL_free()`, `SSL_CTX_free()` and per-task `CleanupAmiSSL()` during stream teardown

This intentionally leaks small per-session AmiSSL objects during the app run. That is a trade-off for runtime stability on classic AmigaOS: repeated station switching previously reproduced delayed `Software Failure #80000008` crashes, while the quarantine path stopped the crash during repeated manual testing.

If you are changing the HTTPS radio code, do not remove this quarantine behaviour unless you have stress-tested repeated HTTPS station switching on the target AmigaOS/AmiSSL setup.

## Build requirements

- Bebbo m68k Amiga GCC toolchain
- `m68k-amigaos-gcc`
- `m68k-amigaos-nm`
- GNU Make
- Git with submodule support
- AmiSSL SDK headers for HTTPS radio builds
- AmiSSL root certificates for `SSLCERTS=1` certificate verification
- ReAction/ClassAct runtime classes for `MintAMP`
- `bsdsocket.library` at runtime for radio

Check the toolchain:

```sh
which m68k-amigaos-gcc
which m68k-amigaos-nm
m68k-amigaos-gcc --version | head -3
```

## Full build instructions

Full AmigaOS build instructions are in:

```text
docs/building-amiga.md
```

That document covers:

- clean repo sync
- submodule sync
- FLAC decoder build and CPU-specific LPC restoration with `FLACASM=1`
- AAC decoder build and CPU-aware m68k helpers (`AACASM=0` selects the portable-C fallback)
- Ogg Vorbis/Tremor decoder build and CPU-aware m68k helpers (`OGGASM=0` selects the portable-C fallback)
- WAV, IFF-8SVX and WMA decoder builds
- radio builds with `RADIO=1`
- HTTPS/AmiSSL builds with `SSL=1`
- certificate-verifying HTTPS builds with `SSLCERTS=1`
- debug/heap-guard builds with `DEBUG=1 HEAPGUARD=1`
- decoder entrypoint checks
- copying files to Amiga/WinUAE
- runtime tests
- Git hygiene

Each GitHub Actions CPU artifact contains `amiga_mp3dec.fastexp` together with
all six matching `*.decoder` modules, so a CI build can be tested on hardware
without rebuilding or mixing modules from another CPU edition.

## Quick build

From a clean checkout:

```sh
cd ~/Amiga-Programs/libhelix-mp3

git fetch origin --prune
git checkout master
git reset --hard origin/master
git clean -fd

git submodule sync --recursive
git submodule update --init --recursive
git submodule foreach --recursive 'git reset --hard && git clean -fd'

make -C decoders clean || true
find . -name "*.o" -delete
rm -f amiga_mp3dec.fastexp MintAMP MintAMP-GT
rm -f decoders/*.decoder decoders/*.decoder.map

make -C decoders flac
make -C decoders aac AACASM=1
make -C decoders ogg
make -C decoders wav
make -C decoders iff
make -C decoders wma
make -f Makefile.amiga sslguir
```

Useful alternative builds:

```sh
make -f Makefile.amiga fast030       # CLI/local playback focused build
make -f Makefile.amiga radio030      # CLI/radio build without HTTPS
make -f Makefile.amiga sslradio030   # CLI/radio build with HTTPS/AmiSSL
make -f Makefile.amiga gui RADIO=1   # GadTools GUI with radio
make -f Makefile.amiga guir RADIO=1  # ReAction GUI with radio
make -f Makefile.amiga sslgui        # GadTools GUI with HTTPS radio
make -f Makefile.amiga sslguir       # ReAction GUI with HTTPS radio/artwork
make -f Makefile.amiga sslguir SSLCERTS=1  # ReAction HTTPS radio with AmiSSL cert verification
make -f Makefile.amiga guir RADIO=1 SSL=1 SSLCERTS=1 DEBUG=1 HEAPGUARD=1  # heavy debug build
```

Verify decoder module entrypoints:

```sh
for module in aac flac ogg wav iff wma; do
  m68k-amigaos-nm -n "decoders/$module.decoder" | head -1
done
```

Every module should start with:

```text
00000000 T _DecoderModuleEntry
```

If anything appears before `_DecoderModuleEntry`, the module may crash when loaded.

## Runtime layout

Keep the player and decoder modules together.

Typical Amiga-side layout:

```text
MintAMP/
  MintAMP
  MintAMP-GT
  amiga_mp3dec.fastexp
  decoders/
    aac.decoder
    flac.decoder
    ogg.decoder
    wma.decoder
    wav.decoder
    iff.decoder
```

Depending on the build target/front-end, the executable may be:

```text
amiga_mp3dec.fastexp   CLI/local and radio playback
MintAMP-GT             GadTools GUI
MintAMP                ReAction/ClassAct GUI
```

## Runtime tests

Local MP3:

```text
amiga_mp3dec.fastexp --play test.mp3
```

AAC:

```text
amiga_mp3dec.fastexp --play test.aac
```

FLAC:

```text
amiga_mp3dec.fastexp --play test.flac
```

Ogg Vorbis:

```text
amiga_mp3dec.fastexp --play test.ogg
```

Classic WMA in ASF:

```text
amiga_mp3dec.fastexp --play test.wma
```

PCM WAV:

```text
amiga_mp3dec.fastexp --play test.wav
```

Amiga IFF-8SVX:

```text
amiga_mp3dec.fastexp --play test.iff
```

HTTP MP3 radio:

```text
amiga_mp3dec.fastexp --play "http://ice1.somafm.com/groovesalad-128-mp3"
```

HTTPS MP3 radio:

```text
amiga_mp3dec.fastexp --play "https://icecast.walmradio.com:8443/classic"
```

## Decoder modules

MintAMP's external decoder modules are:

| Module | Format / implementation |
|---|---|
| `aac.decoder` | Helix AAC through ESP8266Audio |
| `flac.decoder` | libfoxenflac |
| `ogg.decoder` | Xiph.Org Tremor and libogg |
| `wma.decoder` | Rockbox fixed-point libwma plus MintAMP's ASF demuxer |
| `wav.decoder` | MintAMP RIFF/PCM decoder |
| `iff.decoder` | MintAMP IFF-8SVX decoder |

Every external decoder module must export `DecoderModuleEntry` as its first real text symbol.

Required check:

```sh
for module in aac flac ogg wav iff wma; do
  m68k-amigaos-nm -n "decoders/$module.decoder" | head -1
done
```

Expected for every module:

```text
00000000 T _DecoderModuleEntry
```

This is important because the Amiga module loader expects to enter the decoder module at the correct offset. If compiler helper or library code appears before `DecoderModuleEntry`, the module can jump into the wrong code and crash.

## AAC notes

AAC support currently targets AAC-LC ADTS files and streams, with or without
the ADTS CRC.

Radio streams are told apart by their bytes, not only the server's
Content-Type: the probe reads the first 4 KB and looks for a chain of matching
ADTS or MP3 frame headers anywhere in it, since a stream rarely starts on a
frame boundary and some servers send the wrong type or none. The radio AAC
start-up likewise waits for an ADTS header whose next frame also matches
before decoding.

The AAC decoder uses the `decoders/esp8266audio` submodule, with the AAC source under:

```text
decoders/esp8266audio/src/libhelix-aac
```

The expected local symlink is:

```text
decoders/aac -> esp8266audio/src/libhelix-aac
```

Build AAC normally:

```sh
make -C decoders aac
```

Build AAC with the m68k assembly helpers explicitly selected (they are enabled by default):

```sh
make -C decoders aac AACASM=1
```

`AACASM=1` selects m68k helper paths for:

```text
AMIGA_M68K_ASM_AAC_HUFFMAN
AMIGA_M68K_ASM_AAC_DEQUANT
AMIGA_M68K_ASM_AAC_STEREO
AMIGA_M68K_ASM_AAC_IMDCT
```

The helpers are enabled by default. The 68020/030/040 path uses full-result
`MULS.L`; CPU=60 selects a bit-exact implementation made from hardware
two-operand `MULS.L`/`MULU.L` partial products instead, avoiding the emulated
register-pair form. The plain C fallback remains available with `AACASM=0`.

## FLAC notes

FLAC support is provided by `flac.decoder`.

Build:

```sh
make -C decoders flac
```

The existing `AMIGA_M68K_ASM` flag now selects a real LPC restoration helper.
CPU=20/30/40 uses hardware register-pair `MULS.L` and `ADD/ADDX` for each
predictor tap. CPU=60 uses exact hardware-only partial products and avoids the
emulated register-pair form. Use `FLACASM=0` for the original portable-C path.

FLAC is heavier than MP3 and performance depends on CPU, file complexity,
predictor order, output rate and playback settings.

## Ogg Vorbis notes

Ogg Vorbis support is provided by `ogg.decoder`, using Xiph.Org's fixed-point Tremor decoder through Phil Schatzmann's Arduino port together with Xiph.Org libogg.

Build:

```sh
make -C decoders ogg
```

The default build enables CPU-aware m68k fixed-point helpers. The 68020/68030/68040 path keeps Tremor's fast full-result register-pair `MULS.L`; the 68060 path uses exact hardware-only partial products so it does not fall into the 68060 software-emulation trap. Use `OGGASM=0` to build the portable C path.

## WAV, IFF-8SVX and WMA notes

The WAV and IFF modules are compact decoders written for MintAMP:

- `wav.decoder` accepts uncompressed integer PCM WAV files with 8, 16, 24 or 32-bit samples, mono or stereo
- `iff.decoder` accepts classic mono IFF-8SVX samples, both raw and Fibonacci-delta compressed

The WMA module accepts classic WMAv1 and WMAv2 codec streams in ASF containers. Its fixed-point codec core comes from Rockbox's libwma and is derived from FFmpeg; the ASF demuxer and MintAMP module integration are project-specific. WMA Pro, Lossless, Voice and other ASF-contained codecs are out of scope.

WMA's CPU-aware fixed-point helpers are enabled by default. CPU=20/30/40 uses the hardware full-result register-pair `MULS.L`; CPU=60 reconstructs the exact result with four hardware-only partial products. These primitives feed WMA's FFT, MDCT, complex multiplication and unrolled window loops. Use `WMAASM=0` for the portable 64-bit-C fallback and A/B testing.

Build individually:

```sh
make -C decoders wav
make -C decoders iff
make -C decoders wma
```

## Artwork notes

Both GUI editions can fetch and display station artwork where the station or Radio Browser metadata provides a usable image URL.

Supported artwork decode paths:

- JPEG through a small picojpeg-compatible baseline decoder; the original picojpeg API/design is by Rich Geldreich
- PNG through vendored LodePNG by Lode Vandevenne
- WebP through `webpdec.c`, a compact VP8/VP8L implementation using portions of Google's libwebp reference decoder under its BSD licence
- ICO favicon files, including PNG-backed entries and simple DIB-backed icons
- SVG through `svgdec.c`, a fixed-point subset renderer designed for small classic-Amiga artwork

The SVG renderer supports the common paths, shapes, transforms, solid fills, simple strokes and limited gradient handling needed by station logos. It intentionally omits heavyweight browser features such as filters, external images, text layout, CSS stylesheets, masks and full animation. See `svgdec.h` and the source comments for the exact supported subset.

Artwork support is available in both GUI editions. The CLI build does not include or need the artwork decoders.

## Development notes

Do not commit generated files:

```text
*.o
*.decoder
*.decoder.map
*.fastexp
MintAMP-GT
MintAMP
test audio files
*:Zone.Identifier
```

Before committing:

```sh
git status --short
```

Recommended final test checklist before pushing decoder/player changes:

```text
MP3 local playback
AAC local playback
AAC TNS-heavy file
FLAC local playback
Ogg Vorbis local playback
WMA local playback
WAV local playback
IFF-8SVX local playback
HTTP MP3 radio playback
HTTPS MP3 radio playback
HTTPS certificate verification with SSLCERTS=1
Radio Browser search
ICY title/artist metadata updates
Station artwork display where provided
Stop on radio stream
Menus after stopping radio
Restart radio after stopping
Repeated manual HTTPS station switching
DecoderModuleEntry still at offset 0
```

## Credits

MintAMP's Amiga port, GUI front-ends, decoder-module integration, internet radio, artwork integration and m68k optimisation are by Darren Banfi (boingball).

Audio decoder components:

- MP3: Helix fixed-point decoder from RealNetworks
- AAC: Helix AAC through [ESP8266Audio](https://github.com/earlephilhower/ESP8266Audio) by Earle F. Philhower III
- FLAC: [libfoxenflac](https://github.com/astoeckel/libfoxenflac) by Alexander Stoeckel
- Ogg Vorbis: Xiph.Org [Tremor](https://gitlab.xiph.org/xiph/tremor) through [Phil Schatzmann's Arduino port](https://github.com/pschatzmann/arduino-libvorbis-tremor), together with Xiph.Org [libogg](https://github.com/xiph/ogg)
- WMA: Rockbox fixed-point libwma, derived from FFmpeg's WMA decoder

Artwork decoder components:

- JPEG: picojpeg-compatible decoder; [picojpeg](https://github.com/richgel999/picojpeg) was created by Rich Geldreich
- PNG: [LodePNG](https://github.com/lvandeve/lodepng) by Lode Vandevenne
- WebP: compact MintAMP implementation using portions of Google's libwebp reference decoder

The MintAMP-specific ASF/WMA integration, WAV/PCM and IFF-8SVX decoders, and ICO, SVG and compact WebP artwork handlers were developed with assistance from Anthropic Claude.

## Licence

MintAMP combines components under several licences, including the Helix RPSL, LGPL, BSD-style and LodePNG terms. See the repository licence files and the included/submodule source trees for the complete terms. Third-party copyright and licence notices must be retained when redistributing binaries or source.
