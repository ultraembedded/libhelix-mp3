#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
M68K_CC=${M68K_CC:-m68k-linux-gnu-gcc}
QEMU_M68K=${QEMU_M68K:-qemu-m68k}
M68K_NM=${M68K_NM:-m68k-linux-gnu-nm}
M68K_OBJDUMP=${M68K_OBJDUMP:-m68k-linux-gnu-objdump}
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/mintamp-aac-060-XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM
# This build deliberately does NOT set MINTAMP_AAC_TEST_68060: it executes
# the actual inline assembly, checked against 64-bit C arithmetic.
"$M68K_CC" -O2 -static -m68060 -std=gnu99 -DMINTAMP_AAC_NATIVE_68060 \
    -DMINTAMP_AAC_060_REG5 tests/aac_mulshift60_ref_test.c -o "$build_dir/arithmetic"
"$QEMU_M68K" "$build_dir/arithmetic"
cat > "$build_dir/probe.c" <<'PROBE'
#include "aac_m68k_aac_optimized.h"
int probe_high(int x,int y) {return AAC_M68K_MULSHIFT32(x,y);}
long long probe_mac(long long s,int x,int y) {return AAC_M68K_MADD64(s,x,y);}
long long probe_q20(long long s,int x) {return AAC_M68K_MADD64(s,x,1<<20);}
PROBE
"$M68K_CC" -O3 -m68060 -DMINTAMP_AAC_060_REG5 -Idecoders \
    -c "$build_dir/probe.c" -o "$build_dir/probe.o"
"$M68K_NM" -u "$build_dir/probe.o" > "$build_dir/probe.nm"
"$M68K_OBJDUMP" -d "$build_dir/probe.o" > "$build_dir/probe.dis"
python3 tests/check_aac_060_object.py "$build_dir/probe.nm" "$build_dir/probe.dis"
for mode in reference scheduled; do
    mkdir -p "$build_dir/$mode"
    objects=''
    for file in decoders/aac/*.c; do
        base=${file##*/}; base=${base%.c}
        case "$base" in sbr*) continue;; esac
        extra=''
        if [ "$mode" = scheduled ]; then
            case "$base" in dct4|imdct|tns) extra=-DMINTAMP_AAC_060_REG5;; esac
        fi
        "$M68K_CC" -O3 -g -static -m68060 -std=gnu89 -DAMIGA_M68K \
            -DARDUINO -DESP8266 -DAAC_ENABLE_DECIM -Idecoders -Idecoders/aac \
            -Idecoders/aac-arduino-shim -include decoders/aac_m68k_aac_optimized.h \
            -DAMIGA_M68K_ASM_AAC_HUFFMAN -DAMIGA_M68K_ASM_AAC_DEQUANT \
            -DAMIGA_M68K_ASM_AAC_STEREO -DAMIGA_M68K_ASM_AAC_IMDCT \
            -DAMIGA_M68K_ASM_AAC_PNS -DAMIGA_M68K_ASM_AAC_TNS $extra \
            -c "$file" -o "$build_dir/$mode/$base.o"
        objects="$objects $build_dir/$mode/$base.o"
    done
    "$M68K_CC" -O2 -g -static -m68060 -std=c99 -DARDUINO -DESP8266 \
        -DAAC_ENABLE_DECIM -Idecoders/aac -Idecoders/aac-arduino-shim \
        tests/aac_decode_hash_test.c $objects -o "$build_dir/$mode/check"
    for rate in 22 48; do
        case "$rate" in 22) fixture=tests/fixtures/aac22k-tns-pns.aac;;
                        48) fixture=tests/fixtures/aac48k-transients.aac;; esac
        for factor in 1 2 4; do
            "$QEMU_M68K" "$build_dir/$mode/check" "$fixture" \
                "$build_dir/$mode/$rate-$factor.pcm" "$factor" \
                2> "$build_dir/$mode/$rate-$factor.meta"
            cat "$build_dir/$mode/$rate-$factor.meta"
        done
    done
done
for rate in 22 48; do
    for factor in 1 2 4; do
        cmp "$build_dir/reference/$rate-$factor.pcm" "$build_dir/scheduled/$rate-$factor.pcm"
        cmp "$build_dir/reference/$rate-$factor.meta" "$build_dir/scheduled/$rate-$factor.meta"
    done
done
# Prove the fixtures exercised all four window sequences and the tools whose
# multiply implementation is under test, rather than just decoding silence.
grep -q 'windows=f' "$build_dir/scheduled/48-1.meta"
grep -Eq 'tns=[1-9][0-9]*' "$build_dir/scheduled/22-1.meta"
grep -Eq 'pns=[1-9][0-9]*' "$build_dir/scheduled/22-1.meta"
echo 'AAC 060: six full-decoder PCM/metadata comparisons passed (22/48 kHz, factors 1/2/4)'
