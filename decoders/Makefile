# decoders/Makefile — builds pluggable audio decoder modules for MintAMP
#
# Each module is a standalone Amiga hunk-format binary loaded at runtime with
# LoadSeg().  flac_entry.c is compiled and linked FIRST so that its first
# function lands at BADDR(seg)+4 (the decoder module ABI entry point).
#
# Prerequisites
# -------------
#   FLAC: git submodule add https://github.com/astoeckel/libfoxenflac flac
#   OGG:  git submodule add https://github.com/pschatzmann/arduino-libvorbis-tremor tremor
#         git submodule add https://github.com/xiph/ogg libogg
#
# Usage
# -----
#   make -C decoders          # build FLAC, AAC, OGG, WAV and IFF modules
#   make -C decoders flac     # build only the FLAC module
#   make -C decoders aac      # build only the AAC module
#   make -C decoders ogg      # build only the OGG/Vorbis (Tremor) module
#   make -C decoders ogg OGGASM=0  # build OGG/Tremor without m68k asm helpers
#   make -C decoders wav      # build only the WAV (RIFF/PCM) module
#   make -C decoders iff      # build only the IFF/8SVX module
#   make -C decoders wma      # build only the WMA (v1/v2) module

CC     := m68k-amigaos-gcc
NM     := m68k-amigaos-nm

# CPU selects the target processor: 00=68000, 20=68020, 30=68030 (default),
# 40=68040, 60=68060 -- same numeric convention as Makefile.amiga, which is
# the single source of truth when this is built via `make -f Makefile.amiga
# decoder-modules` (it forwards its own CPU= here). Run standalone (`make -C
# decoders`) it defaults to 30 to match this project's primary target.
CPU ?= 30
CPU_MFLAG_00 := -m68000
CPU_MFLAG_20 := -m68020
CPU_MFLAG_30 := -m68030
CPU_MFLAG_40 := -m68040
CPU_MFLAG_60 := -m68060
CPU_MFLAG := $(CPU_MFLAG_$(CPU))
ifeq ($(CPU_MFLAG),)
$(error Unknown CPU=$(CPU) -- expected one of: 00 20 30 40 60)
endif

OGGASM ?= 1
AACASM ?= 1
WMAASM ?= 1
FLACASM ?= 1
# override: these decoders link with -nostartfiles against a hand-written
# Exec-backed allocator shim (see ogg_alloc.c/aac_alloc.c/flac_alloc.c) that
# only works if GCC never recognizes/inlines a call as "the real libc
# malloc/free/memset" -- -fno-builtin-* is what prevents that. Previously
# Makefile.amiga's decoder-modules target passed its own CFLAGS="..." on
# the command line, which (per normal GNU Make precedence) silently
# replaced this entire line -- including those flags and -ffunction-
# sections/-fdata-sections needed for the --gc-sections link step -- every
# time this was built via `make -f Makefile.amiga gui/guir/fast030` rather
# than standalone `make -C decoders`. override makes this immune to that.
override CFLAGS := $(CPU_MFLAG) -O2 -fomit-frame-pointer -std=gnu89 -Wall -I. \
          -DHAVE_AMIGA_AUDIO_DEVICE -DAMIGA_M68K \
          -fno-builtin \
          -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc \
          -fno-builtin-free -fno-builtin-exit -fno-builtin-memset \
          -fno-tree-loop-distribute-patterns \
          -ffunction-sections -fdata-sections -DNDEBUG

# Post-link: mark hunks as "any RAM, prefer Fast" (same as main binary)
FIXHUNKS := python3 ../tools/amiga_fast_preferred_hunks.py

.PHONY: all flac aac ogg wav iff wma clean

# The normal decoder build must produce every supported external module.
all: flac aac ogg wav iff wma
	@echo "Decoder build complete."

# --------------------------------------------------------------------------
# FLAC — libfoxenflac (https://github.com/astoeckel/libfoxenflac)
# --------------------------------------------------------------------------
FLAC_SRCS := flac_entry.c flac_alloc.c flac_module.c flac/src/foxen-flac.c
FLAC_OBJS := $(FLAC_SRCS:.c=.o)

flac: flac.decoder

FLAC_CFLAGS := $(filter-out -std=gnu89 -std=gnu11,$(CFLAGS)) -std=gnu99 -I. -Iflac/src

FLAC_ASM_PATCH := flac-libfoxenflac-m68k-lpc.patch
FLAC_ASM_SOURCE := flac/src/foxen-flac.c
FLAC_ASM_MARKER := flac/.mintamp-m68k-lpc-v1

# libfoxenflac is a third-party submodule. Keep the CPU implementation in the
# parent repository and carry only the small dispatch hook as a patch.
$(FLAC_ASM_MARKER): $(FLAC_ASM_PATCH)
	@if grep -q 'MINTAMP_FLAC_LPC_ASM_HOOK' $(FLAC_ASM_SOURCE); then \
	    echo "libfoxenflac m68k LPC patch already applied, skipping."; \
	else \
	    echo "Applying libfoxenflac m68k LPC patch ($(FLAC_ASM_PATCH))..."; \
	    cd flac && patch -p1 < ../$(FLAC_ASM_PATCH); \
	fi
	@touch $@

ifneq ($(filter 1 yes true on,$(FLACASM)),)
FLAC_CFLAGS += -include flac_m68k_lpc.h -DAMIGA_M68K_ASM
endif

flac_entry.o flac_alloc.o flac_module.o: decoder_module.h
flac_alloc.o flac_module.o flac/src/foxen-flac.o: flac_alloc.h
flac/src/foxen-flac.o: flac_standalone.h flac_m68k_lpc.h $(FLAC_ASM_MARKER)
flac_module.o flac/src/foxen-flac.o: flac/src/foxen-flac.h
flac/src/foxen-flac.o: FLAC_CFLAGS += -include flac_standalone.h \
	-Dmalloc=FlacModuleMalloc -Dcalloc=FlacModuleCalloc \
	-Drealloc=FlacModuleRealloc -Dfree=FlacModuleFree \
	-Dexit=FlacModuleExit -DNDEBUG

%.o: %.c
	$(CC) $(FLAC_CFLAGS) -c -o $@ $<

flac.decoder: $(FLAC_OBJS) decoder_module.h flac/src/foxen-flac.h
	rm -f $@ $@.map
	@echo "FLAC decoder unresolved-symbol audit:"; \
	    $(NM) -u flac_entry.o flac_alloc.o flac_module.o flac/src/foxen-flac.o || true
	@if $(NM) -u flac_entry.o flac_module.o flac/src/foxen-flac.o 2>/dev/null | grep -iE '(^|[[:space:]])(_?malloc_r|_?calloc_r|_?realloc_r|_?free_r|malloc|calloc|realloc|free|exit|_exit|abort|assert|___assert_func|printf|fprintf|puts|putchar|fflush|stdio|vfprintf)$$'; then \
	    echo "ERROR: FLAC decoder objects reference forbidden libc/startup symbols outside flac_alloc.o" >&2; \
	    exit 1; \
	fi
	$(CC) $(FLAC_CFLAGS) -nostartfiles -Wl,--gc-sections -Wl,-Map,$@.map -o $@ $(FLAC_OBJS) -lgcc
	@if grep -iE "printf|fprintf|puts|putchar|fflush|stdio|vfprintf|malloc\.o|lib_a-exit|abort|assert|___assert_func|_exit|__EXIT_LIST__|DOSBase|SysBase|dummy\.o|raise" $@.map; then \
	    echo "ERROR: FLAC decoder linked forbidden libc/startup symbols" >&2; \
	    exit 1; \
	fi
	@if $(NM) -u $@ 2>/dev/null | grep -iE 'printf|fprintf|puts|putchar|fflush|stdio|vfprintf|malloc\.o|lib_a-exit|abort|assert|___assert_func|(^|[[:space:]])_?exit$$|__EXIT_LIST__|DOSBase|SysBase|dummy\.o|raise'; then \
	    echo "ERROR: FLAC decoder has forbidden unresolved libc/startup symbols" >&2; \
	    exit 1; \
	fi
	@if ! grep -q 'flac_module\.o' $@.map; then \
	    echo "ERROR: FLAC decoder map does not mention flac_module.o" >&2; \
	    exit 1; \
	fi
	@if ! grep -aqF 'FLAC MODULE BUILD MARKER 12345' $@; then \
	    echo "ERROR: FLAC decoder is missing build marker string" >&2; \
	    exit 1; \
	fi
	$(FIXHUNKS) $@
	@echo "Built $@"

# --------------------------------------------------------------------------
# AAC — libhelix-aac (ESP8266Audio/src/libhelix-aac or compatible)
#
# Prerequisites
# -------------
#   Place the libhelix-aac source files in decoders/aac/.  The simplest
#   approach is to add the ESP8266Audio repo as a submodule and symlink:
#
#     git submodule add https://github.com/earlephilhower/ESP8266Audio \
#                       decoders/esp8266audio
#     ln -s esp8266audio/src/libhelix-aac decoders/aac
#
#   Or clone a standalone libhelix-aac repo directly into decoders/aac/.
#
# SBR (HE-AAC)
# ------------
#   SBR is disabled by default (aac_standalone.h sets ESP8266 which
#   suppresses AAC_ENABLE_SBR).  To enable SBR: remove the ESP8266 define
#   from aac_standalone.h and include the sbr*.c files in AAC_LIB_SRCS.
# --------------------------------------------------------------------------

# Exclude SBR files — enabled by default to keep memory footprint small.
# Remove the filter-out to build with HE-AAC/SBR support.
# Exclude SBR files from the AAC module.
AAC_LIB_SRCS := $(filter-out aac/sbr%.c,$(wildcard aac/*.c))
AAC_SRCS     := aac_entry.c aac_alloc.c aac_module.c $(AAC_LIB_SRCS)
AAC_OBJS     := $(AAC_SRCS:.c=.o)

aac: aac.decoder

# Arduino/pgmspace shim directory — intercepted before any system headers.
AAC_SHIM_DIR := aac-arduino-shim

AAC_CFLAGS := $(CFLAGS) -I. -I$(AAC_SHIM_DIR) -Iaac -DARDUINO

# The Helix AAC sources have optional stage-level m68k helpers.  Keep the
# single full-result MULS.L on 68020/030/040; the forced-include MintAMP
# wrapper selects a 68060-safe, bit-exact partial-product implementation using
# only the hardware two-operand (low-result) multiply form when CPU=60.
# AACASM=0 remains the portable-C comparison/fallback build.
ifneq ($(filter 1 yes true on,$(AACASM)),)
AAC_CFLAGS += -include aac_m68k_aac_optimized.h \
	-DAMIGA_M68K_ASM_AAC_HUFFMAN \
	-DAMIGA_M68K_ASM_AAC_DEQUANT \
	-DAMIGA_M68K_ASM_AAC_STEREO \
	-DAMIGA_M68K_ASM_AAC_IMDCT \
	-DAMIGA_M68K_ASM_AAC_PNS \
	-DAMIGA_M68K_ASM_AAC_TNS
endif

# Measured per-stage scheduling: the five-register 060 helper improves the
# transforms/windowing and TNS, but increases FFT register spills. Keep the
# established helper in FFT, PNS, dequant and stereo. CPU 030/040 unchanged.
AAC060_REG5 ?= 1
ifeq ($(CPU),60)
ifneq ($(filter 1 yes true on,$(AACASM)),)
ifneq ($(filter 1 yes true on,$(AAC060_REG5)),)
aac/dct4.o aac/imdct.o aac/tns.o: private AAC_060_STAGE_FLAGS := -DMINTAMP_AAC_060_REG5
endif
endif
endif

# A/B switches must actually rebuild the decoder, rather than reuse objects
# from the previous CPU/AACASM/REG5 configuration.
.PHONY: force-aac-config
.aac-build-config: force-aac-config
	@printf '%s\n' '$(AAC_CFLAGS)' 'AAC060_REG5=$(AAC060_REG5)' > $@.tmp
	@cmp -s $@.tmp $@ || cp $@.tmp $@
	@rm -f $@.tmp
$(AAC_OBJS): .aac-build-config Makefile

aac_entry.o aac_alloc.o aac_module.o: decoder_module.h
aac_alloc.o aac_module.o: aac_alloc.h
aac_module.o: aac/aacdec.h
$(AAC_LIB_SRCS:.c=.o): aac_m68k_aac_optimized.h

# Compile our wrapper objects with base flags (no Arduino shim needed).
aac_entry.o aac_alloc.o aac_module.o: %.o: %.c
	$(CC) $(AAC_CFLAGS) -c -o $@ $<

# Compile every libhelix-aac .c with the forced-include shim so that
# malloc/calloc/realloc/free/exit are redirected and Arduino headers are
# resolved through aac-arduino-shim/.
aac/%.o: aac/%.c
	$(CC) $(AAC_CFLAGS) $(AAC_060_STAGE_FLAGS) \
	    -include aac_standalone.h \
	    -Dmalloc=AacModuleMalloc \
	    -Dcalloc=AacModuleCalloc \
	    -Drealloc=AacModuleRealloc \
	    -Dfree=AacModuleFree \
	    -Dexit=AacModuleExit \
	    -DNDEBUG \
	    -c -o $@ $<

aac.decoder: $(AAC_OBJS) decoder_module.h aac/aacdec.h
	rm -f $@ $@.map
	@echo "AAC decoder unresolved-symbol audit:"; \
	    $(NM) -u aac_entry.o aac_alloc.o aac_module.o || true
	@if $(NM) -u aac_entry.o aac_module.o $(AAC_LIB_SRCS:.c=.o) 2>/dev/null | \
	    grep -iE '(^|[[:space:]])(_?malloc_r|_?calloc_r|_?realloc_r|_?free_r|malloc|calloc|realloc|free|exit|_exit|abort|assert|___assert_func|printf|fprintf|puts|putchar|fflush|stdio|vfprintf)$$'; then \
	    echo "ERROR: AAC decoder objects reference forbidden libc/startup symbols outside aac_alloc.o" >&2; \
	    exit 1; \
	fi
	$(CC) $(AAC_CFLAGS) -nostartfiles -Wl,--gc-sections -Wl,-Map,$@.map -o $@ $(AAC_OBJS)
	@if grep -iE "printf|fprintf|puts|putchar|fflush|stdio|vfprintf|malloc\.o|lib_a-exit|abort|assert|___assert_func|_exit|__EXIT_LIST__|DOSBase|SysBase|dummy\.o|raise" $@.map; then \
	    echo "ERROR: AAC decoder linked forbidden libc/startup symbols" >&2; \
	    exit 1; \
	fi
	@if $(NM) -u $@ 2>/dev/null | \
	    grep -iE 'printf|fprintf|puts|putchar|fflush|stdio|vfprintf|malloc\.o|lib_a-exit|abort|assert|___assert_func|(^|[[:space:]])_?exit$$|__EXIT_LIST__|DOSBase|SysBase|dummy\.o|raise'; then \
	    echo "ERROR: AAC decoder has forbidden unresolved libc/startup symbols" >&2; \
	    exit 1; \
	fi
	@if ! grep -q 'aac_module\.o' $@.map; then \
	    echo "ERROR: AAC decoder map does not mention aac_module.o" >&2; \
	    exit 1; \
	fi
	@if ! grep -aqF 'AAC MODULE BUILD MARKER 12345' $@; then \
	    echo "ERROR: AAC decoder is missing build marker string" >&2; \
	    exit 1; \
	fi
	$(FIXHUNKS) $@
	@echo "Built $@"

# --------------------------------------------------------------------------
# OGG/Vorbis — Tremor (fixed-point libvorbis, pschatzmann/arduino-libvorbis-
# tremor, upstream https://gitlab.xiph.org/xiph/tremor) + libogg
# (https://github.com/xiph/ogg).
#
# Prerequisites
# -------------
#   git submodule add https://github.com/pschatzmann/arduino-libvorbis-tremor \
#                     decoders/tremor
#   git submodule add https://github.com/xiph/ogg decoders/libogg
#
# Path layout (verified against the checked-out pschatzmann/arduino-
# libvorbis-tremor submodule — note the extra tremor/ nesting under src/):
#   decoders/tremor/src/tremor/*.c, ivorbisfile.h, ivorbiscodec.h
#   decoders/libogg/src/*.c, decoders/libogg/include/ogg/ogg.h
#
# decoders/tremor/src/tremor/ also ships iseeking_example.c and
# ivorbisfile_example.c — standalone demo apps with their own main() and
# stdio use.  These are filtered out of TREMOR_LIB_SRCS below; they must
# not be linked into the module.
#
# Like AAC, all libogg/Tremor object files are compiled with the forced
# ogg_standalone.h include so malloc/calloc/realloc/free/exit resolve to the
# Exec-backed shim in ogg_alloc.c instead of pulling in libc/newlib startup.
#
# libogg/include/ogg/os_types.h pulls in <ogg/config_types.h>, which upstream
# is generated by libogg's own ./configure or CMake run (from
# config_types.h.in) and isn't checked into the repo.  Since this Makefile
# doesn't run libogg's native build system, ogg-config-shim/ogg/config_types.h
# provides a hand-written equivalent (plain stdint.h typedefs) ahead of
# libogg/include on the search path.
# --------------------------------------------------------------------------

# decoders/tremor is a third-party submodule (pschatzmann/arduino-libvorbis-
# tremor) we don't own, so the m68k asm additions can't live as commits inside
# it; they're carried as ogg-tremor-m68k-asm.patch and applied here. Tremor's
# own src/tremor/misc.h already includes asm_arm.h unconditionally for the
# equivalent ARM fast path, so this follows the same upstream-provided hook
# (see docs/ogg-tremor-m68k-asm.md). Using the new header itself as the
# make target means the patch is only (re-)applied when it isn't already
# present, so a repeat build or an already-patched checkout is a no-op.
OGG_ASM_PATCH  := ogg-tremor-m68k-asm.patch
OGG_ASM_MARKER := tremor/src/tremor/asm_m68k.h

$(OGG_ASM_MARKER): $(OGG_ASM_PATCH)
	@if [ -f $(OGG_ASM_MARKER) ]; then \
	    echo "Tremor m68k asm patch already applied, skipping."; \
	else \
	    echo "Applying Tremor m68k asm patch ($(OGG_ASM_PATCH))..."; \
	    cd tremor && patch -p1 < ../$(OGG_ASM_PATCH); \
	fi

TREMOR_LIB_SRCS := $(filter-out %_example.c,$(wildcard tremor/src/tremor/*.c))
LIBOGG_LIB_SRCS := $(wildcard libogg/src/*.c)
OGG_SRCS := ogg_entry.c ogg_alloc.c ogg_module.c $(TREMOR_LIB_SRCS) $(LIBOGG_LIB_SRCS)
OGG_OBJS := $(OGG_SRCS:.c=.o)

ogg: $(OGG_ASM_MARKER) ogg.decoder

OGG_CFLAGS := $(CFLAGS) -I. -Itremor/src -Itremor/src/tremor -Iogg-config-shim -Ilibogg/include

ifeq ($(OGGASM),1)
OGG_CFLAGS += -D_M68K_ASSEM_
endif

ogg_entry.o ogg_alloc.o ogg_module.o: decoder_module.h
ogg_alloc.o ogg_module.o: ogg_alloc.h
ogg_module.o: tremor/src/tremor/ivorbiscodec.h tremor/src/tremor/ivorbisfile.h

# Compile our wrapper objects with base flags (no forced include needed).
ogg_entry.o ogg_alloc.o ogg_module.o: %.o: %.c
	$(CC) $(OGG_CFLAGS) -c -o $@ $<

# Compile every Tremor / libogg .c with the forced-include shim so that
# malloc/calloc/realloc/free/exit are redirected to ogg_alloc.c.  Also
# redirect fopen/fclose/fread/fseek/ftell: Tremor's vorbisfile.c has a
# legacy FILE*-based ov_fopen()/ov_open() API we never call, but the real
# newlib fopen() would pull in dummy.o's SysBase-dependent syscall stubs
# regardless of whether it's ever reached at runtime.  Also redirect
# _impure_ptr (see ogg_alloc.c) for the same reason -- referenced from an
# errno check in that same dead code, and pulling in the real one cascades
# into the entire stdio implementation and DOSBase.
tremor/src/tremor/%.o: tremor/src/tremor/%.c $(OGG_ASM_MARKER) ogg_m68k_68060.h
	$(CC) $(OGG_CFLAGS) \
	    -include ogg_m68k_68060.h \
	    -include ogg_standalone.h \
	    -Dmalloc=OggModuleMalloc \
	    -Dcalloc=OggModuleCalloc \
	    -Drealloc=OggModuleRealloc \
	    -Dfree=OggModuleFree \
	    -Dexit=OggModuleExit \
	    -Dfopen=OggModuleFopen \
	    -Dfclose=OggModuleFclose \
	    -Dfread=OggModuleFread \
	    -Dfseek=OggModuleFseek \
	    -Dftell=OggModuleFtell \
	    -D_impure_ptr=OggModuleImpurePtr \
	    -DNDEBUG \
	    -c -o $@ $<

libogg/src/%.o: libogg/src/%.c
	$(CC) $(OGG_CFLAGS) \
	    -include ogg_standalone.h \
	    -Dmalloc=OggModuleMalloc \
	    -Dcalloc=OggModuleCalloc \
	    -Drealloc=OggModuleRealloc \
	    -Dfree=OggModuleFree \
	    -Dexit=OggModuleExit \
	    -Dfopen=OggModuleFopen \
	    -Dfclose=OggModuleFclose \
	    -Dfread=OggModuleFread \
	    -Dfseek=OggModuleFseek \
	    -Dftell=OggModuleFtell \
	    -D_impure_ptr=OggModuleImpurePtr \
	    -DNDEBUG \
	    -c -o $@ $<

ogg.decoder: $(OGG_OBJS) decoder_module.h tremor/src/tremor/ivorbisfile.h
	rm -f $@ $@.map
	@echo "OGG decoder unresolved-symbol audit:"; \
	    $(NM) -u ogg_entry.o ogg_alloc.o ogg_module.o || true
	@if $(NM) -u ogg_entry.o ogg_module.o $(TREMOR_LIB_SRCS:.c=.o) $(LIBOGG_LIB_SRCS:.c=.o) 2>/dev/null | \
	    grep -iE '(^|[[:space:]])(_?malloc_r|_?calloc_r|_?realloc_r|_?free_r|malloc|calloc|realloc|free|exit|_exit|abort|assert|___assert_func|printf|fprintf|puts|putchar|fflush|stdio|vfprintf|fopen|fclose|fread|fseek|ftell|getpid|_?sbrk|isatty|_impure_ptr|_?fstat|_?kill|_?lseek|_?unlink|ioctl)$$'; then \
	    echo "ERROR: OGG decoder objects reference forbidden libc/startup symbols outside ogg_alloc.o" >&2; \
	    exit 1; \
	fi
	$(CC) $(OGG_CFLAGS) -nostartfiles -Wl,--gc-sections -Wl,-Map,$@.map -o $@ $(OGG_OBJS) -lgcc
	@if grep -iE "printf|fprintf|puts|putchar|fflush|stdio|vfprintf|malloc\.o|lib_a-exit|abort|assert|___assert_func|_exit|__EXIT_LIST__|DOSBase|SysBase|dummy\.o|raise" $@.map; then \
	    echo "ERROR: OGG decoder linked forbidden libc/startup symbols" >&2; \
	    exit 1; \
	fi
	@if $(NM) -u $@ 2>/dev/null | \
	    grep -iE 'printf|fprintf|puts|putchar|fflush|stdio|vfprintf|malloc\.o|lib_a-exit|abort|assert|___assert_func|(^|[[:space:]])_?exit$$|__EXIT_LIST__|DOSBase|SysBase|dummy\.o|raise'; then \
	    echo "ERROR: OGG decoder has forbidden unresolved libc/startup symbols" >&2; \
	    exit 1; \
	fi
	@if ! grep -q 'ogg_module\.o' $@.map; then \
	    echo "ERROR: OGG decoder map does not mention ogg_module.o" >&2; \
	    exit 1; \
	fi
	@if ! grep -aqF 'OGG MODULE BUILD MARKER 12345' $@; then \
	    echo "ERROR: OGG decoder is missing build marker string" >&2; \
	    exit 1; \
	fi
	@first_sym=$$($(NM) -n $@ | grep -iE ' [TtWw] ' | head -1 | awk '{print $$3}'); \
	    if [ "$$first_sym" != "_DecoderModuleEntry" ] && [ "$$first_sym" != "DecoderModuleEntry" ]; then \
	    echo "ERROR: OGG decoder first text symbol is '$$first_sym', expected DecoderModuleEntry" >&2; \
	    exit 1; \
	fi
	$(FIXHUNKS) $@
	@echo "Built $@"

# --------------------------------------------------------------------------
# WAV — plain RIFF/PCM decoder, hand-written for MintAMP (no third-party
# library, so unlike FLAC/AAC/OGG above there is no forced malloc/calloc/
# free shim -- wav_alloc.c's WavModule* names are called directly).
# 8/16/24/32-bit integer PCM, mono or stereo, classic and
# WAVE_FORMAT_EXTENSIBLE 'fmt ' headers.
# --------------------------------------------------------------------------
WAV_SRCS := wav_entry.c wav_alloc.c wav_module.c
WAV_OBJS := $(WAV_SRCS:.c=.o)

wav: wav.decoder

wav_entry.o wav_alloc.o wav_module.o: decoder_module.h
wav_alloc.o wav_module.o: wav_alloc.h

wav_entry.o wav_alloc.o wav_module.o: %.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

wav.decoder: $(WAV_OBJS) decoder_module.h
	rm -f $@ $@.map
	@echo "WAV decoder unresolved-symbol audit:"; \
	    $(NM) -u $(WAV_OBJS) || true
	@if $(NM) -u $(WAV_OBJS) 2>/dev/null | \
	    grep -iE '(^|[[:space:]])(_?malloc_r|_?calloc_r|_?realloc_r|_?free_r|malloc|calloc|realloc|free|exit|_exit|abort|assert|___assert_func|printf|fprintf|puts|putchar|fflush|stdio|vfprintf)$$'; then \
	    echo "ERROR: WAV decoder objects reference forbidden libc/startup symbols" >&2; \
	    exit 1; \
	fi
	$(CC) $(CFLAGS) -nostartfiles -Wl,--gc-sections -Wl,-Map,$@.map -o $@ $(WAV_OBJS) -lgcc
	@if grep -iE "printf|fprintf|puts|putchar|fflush|stdio|vfprintf|malloc\.o|lib_a-exit|abort|assert|___assert_func|_exit|__EXIT_LIST__|DOSBase|SysBase|dummy\.o|raise" $@.map; then \
	    echo "ERROR: WAV decoder linked forbidden libc/startup symbols" >&2; \
	    exit 1; \
	fi
	@if $(NM) -u $@ 2>/dev/null | \
	    grep -iE 'printf|fprintf|puts|putchar|fflush|stdio|vfprintf|malloc\.o|lib_a-exit|abort|assert|___assert_func|(^|[[:space:]])_?exit$$|__EXIT_LIST__|DOSBase|SysBase|dummy\.o|raise'; then \
	    echo "ERROR: WAV decoder has forbidden unresolved libc/startup symbols" >&2; \
	    exit 1; \
	fi
	@if ! grep -q 'wav_module\.o' $@.map; then \
	    echo "ERROR: WAV decoder map does not mention wav_module.o" >&2; \
	    exit 1; \
	fi
	@if ! grep -aqF 'WAV MODULE BUILD MARKER 12345' $@; then \
	    echo "ERROR: WAV decoder is missing build marker string" >&2; \
	    exit 1; \
	fi
	$(FIXHUNKS) $@
	@echo "Built $@"

# --------------------------------------------------------------------------
# IFF/8SVX — Amiga "8-bit Voice" decoder, hand-written for MintAMP (no
# third-party library). Mono, 8-bit signed PCM, sCompression 0 (raw) or 1
# (Fibonacci-delta). IFF chunk sizes are big-endian, unlike WAV's RIFF.
# --------------------------------------------------------------------------
IFF_SRCS := iff_entry.c iff_alloc.c iff_module.c
IFF_OBJS := $(IFF_SRCS:.c=.o)

iff: iff.decoder

iff_entry.o iff_alloc.o iff_module.o: decoder_module.h
iff_alloc.o iff_module.o: iff_alloc.h

iff_entry.o iff_alloc.o iff_module.o: %.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

iff.decoder: $(IFF_OBJS) decoder_module.h
	rm -f $@ $@.map
	@echo "IFF decoder unresolved-symbol audit:"; \
	    $(NM) -u $(IFF_OBJS) || true
	@if $(NM) -u $(IFF_OBJS) 2>/dev/null | \
	    grep -iE '(^|[[:space:]])(_?malloc_r|_?calloc_r|_?realloc_r|_?free_r|malloc|calloc|realloc|free|exit|_exit|abort|assert|___assert_func|printf|fprintf|puts|putchar|fflush|stdio|vfprintf)$$'; then \
	    echo "ERROR: IFF decoder objects reference forbidden libc/startup symbols" >&2; \
	    exit 1; \
	fi
	$(CC) $(CFLAGS) -nostartfiles -Wl,--gc-sections -Wl,-Map,$@.map -o $@ $(IFF_OBJS) -lgcc
	@if grep -iE "printf|fprintf|puts|putchar|fflush|stdio|vfprintf|malloc\.o|lib_a-exit|abort|assert|___assert_func|_exit|__EXIT_LIST__|DOSBase|SysBase|dummy\.o|raise" $@.map; then \
	    echo "ERROR: IFF decoder linked forbidden libc/startup symbols" >&2; \
	    exit 1; \
	fi
	@if $(NM) -u $@ 2>/dev/null | \
	    grep -iE 'printf|fprintf|puts|putchar|fflush|stdio|vfprintf|malloc\.o|lib_a-exit|abort|assert|___assert_func|(^|[[:space:]])_?exit$$|__EXIT_LIST__|DOSBase|SysBase|dummy\.o|raise'; then \
	    echo "ERROR: IFF decoder has forbidden unresolved libc/startup symbols" >&2; \
	    exit 1; \
	fi
	@if ! grep -q 'iff_module\.o' $@.map; then \
	    echo "ERROR: IFF decoder map does not mention iff_module.o" >&2; \
	    exit 1; \
	fi
	@if ! grep -aqF 'IFF MODULE BUILD MARKER 12345' $@; then \
	    echo "ERROR: IFF decoder is missing build marker string" >&2; \
	    exit 1; \
	fi
	$(FIXHUNKS) $@
	@echo "Built $@"

# --------------------------------------------------------------------------
# WMA — classic WMA v1/v2, vendored from Rockbox's fixed-point libwma
# (lib/rbcodec/codecs/{libwma,lib}/*, LGPLv2.1+, derived from FFmpeg's WMA
# decoder). Chosen over extracting FFmpeg's own libavcodec/wmadec.c
# directly because Rockbox already did the work of decoupling it from
# FFmpeg's framework for embeddable, fixed-point, no-OS bare-metal
# playback (Coldfire/ARM/SH DAPs) -- the same constraints this project has
# on m68k, and the same reason FLAC/OGG here use libfoxenflac/Tremor
# instead of the reference libFLAC/full libvorbis. See decoders/wma/ for
# the vendored sources (platform.h/codecs.h/codeclib.h are this project's
# adaptation shims, not upstream Rockbox) and wma_asf.c for this project's
# own ASF container demuxer (WMA files are ASF containers; Rockbox's own
# ASF demux is split across its buffered-file/id3 metadata layer in a way
# that doesn't map onto this project's DecoderReadCb/DecoderSeekCb model).
#
# No decoder-supplied test WMA/ASF files were available to verify actual
# audio output in this build environment -- the ASF demux and decoder-core
# integration were checked with host-side unit tests (see decoders/wma/
# ReadMe or the project's development notes), but real playback on target
# hardware is the first real-world correctness check this module gets.
# --------------------------------------------------------------------------
WMA_LIB_SRCS := wma/wmadeci.c wma/wmafixed.c wma/mdct.c wma/mdct_lookup.c \
                wma/fft-ffmpeg.c wma/ffmpeg_bitstream.c
WMA_SRCS := wma_entry.c wma_alloc.c wma_asf.c $(WMA_LIB_SRCS) wma_module.c
WMA_OBJS := $(WMA_SRCS:.c=.o)

wma: wma.decoder

WMA_CFLAGS := $(CFLAGS) -I. -Iwma

# Rockbox's WMA core otherwise uses int64_t C for every fixed-point multiply.
# CPU=20/30/40 gets the hardware full-result MULS.L path; CPU=60 gets an exact
# four-part implementation using only hardware low-result multiplies.
# WMAASM=0 retains the portable C decoder for comparison and fallback.
ifneq ($(filter 1 yes true on,$(WMAASM)),)
WMA_CFLAGS += -DAMIGA_M68K_WMA_ASM
endif

wma_entry.o wma_alloc.o wma_asf.o wma_module.o: decoder_module.h
wma_alloc.o wma_asf.o wma_module.o: wma_alloc.h
wma_asf.o wma_module.o: wma_asf.h
wma_module.o: wma/wmadec.h

# Compile our wrapper objects with base flags (no forced include needed).
wma_entry.o wma_alloc.o wma_asf.o wma_module.o: %.o: %.c
	$(CC) $(WMA_CFLAGS) -c -o $@ $<

# Compile every vendored decoders/wma/ source with memcpy/memset/memmove/
# memcmp/qsort redirected to wma_alloc.c's freestanding implementations --
# same forced-rename pattern flac_module.c/aac_module.c/ogg_module.c use
# for malloc/calloc/free, needed here because no libc is linked into these
# -nostartfiles decoder-module hunks.
wma/%.o: wma/%.c wma/asm_m68k.h
	$(CC) $(WMA_CFLAGS) \
	    -Dmemcpy=WmaMemcpy \
	    -Dmemset=WmaMemset \
	    -Dmemmove=WmaMemmove \
	    -Dmemcmp=WmaMemcmp \
	    -Dqsort=WmaQsort \
	    -c -o $@ $<

wma.decoder: $(WMA_OBJS) decoder_module.h wma_alloc.h wma_asf.h wma/wmadec.h
	rm -f $@ $@.map
	@echo "WMA decoder unresolved-symbol audit:"; \
	    $(NM) -u wma_entry.o wma_alloc.o wma_asf.o wma_module.o || true
	@if $(NM) -u wma_entry.o wma_asf.o wma_module.o $(WMA_LIB_SRCS:.c=.o) 2>/dev/null | \
	    grep -iE '(^|[[:space:]])(_?malloc_r|_?calloc_r|_?realloc_r|_?free_r|malloc|calloc|realloc|free|exit|_exit|abort|assert|___assert_func|printf|fprintf|puts|putchar|fflush|stdio|vfprintf|memcpy|memset|memmove|memcmp|qsort)$$'; then \
	    echo "ERROR: WMA decoder objects reference forbidden libc/startup symbols outside wma_alloc.o" >&2; \
	    exit 1; \
	fi
	$(CC) $(WMA_CFLAGS) -nostartfiles -Wl,--gc-sections -Wl,-Map,$@.map -o $@ $(WMA_OBJS) -lgcc
	@if grep -iE "printf|fprintf|puts|putchar|fflush|stdio|vfprintf|malloc\.o|lib_a-exit|abort|assert|___assert_func|_exit|__EXIT_LIST__|DOSBase|SysBase|dummy\.o|raise" $@.map; then \
	    echo "ERROR: WMA decoder linked forbidden libc/startup symbols" >&2; \
	    exit 1; \
	fi
	@if $(NM) -u $@ 2>/dev/null | \
	    grep -iE 'printf|fprintf|puts|putchar|fflush|stdio|vfprintf|malloc\.o|lib_a-exit|abort|assert|___assert_func|(^|[[:space:]])_?exit$$|__EXIT_LIST__|DOSBase|SysBase|dummy\.o|raise'; then \
	    echo "ERROR: WMA decoder has forbidden unresolved libc/startup symbols" >&2; \
	    exit 1; \
	fi
	@if ! grep -q 'wma_module\.o' $@.map; then \
	    echo "ERROR: WMA decoder map does not mention wma_module.o" >&2; \
	    exit 1; \
	fi
	@if ! grep -aqF 'WMA MODULE BUILD MARKER 12345' $@; then \
	    echo "ERROR: WMA decoder is missing build marker string" >&2; \
	    exit 1; \
	fi
	@first_sym=$$($(NM) -n $@ | grep -iE ' [TtWw] ' | head -1 | awk '{print $$3}'); \
	    if [ "$$first_sym" != "_DecoderModuleEntry" ] && [ "$$first_sym" != "DecoderModuleEntry" ]; then \
	    echo "ERROR: WMA decoder first text symbol is '$$first_sym', expected DecoderModuleEntry" >&2; \
	    exit 1; \
	fi
	$(FIXHUNKS) $@
	@echo "Built $@"

# --------------------------------------------------------------------------

clean:
	rm -f flac.decoder flac.decoder.map $(FLAC_OBJS)
	rm -f aac.decoder  aac.decoder.map  $(AAC_OBJS)
	rm -f ogg.decoder  ogg.decoder.map  $(OGG_OBJS)
	rm -f wav.decoder  wav.decoder.map  $(WAV_OBJS)
	rm -f iff.decoder  iff.decoder.map  $(IFF_OBJS)
	rm -f wma.decoder  wma.decoder.map  $(WMA_OBJS)
