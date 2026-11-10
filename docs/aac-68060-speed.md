# AAC-LC 68060 multiply scheduling

`CPU=60 AACASM=1` selects a five-register, four-partial-product high-word
multiply in dct4.c, imdct.c and tns.c. It uses only single-result MULS.L/
MULU.L instructions, SWAP/EXT for signed halves, and early-clobber operands.
The result is identical to the existing Warren mulhs reconstruction.
FFT, PNS, dequant and stereo retain the existing C-scheduled helper: an
all-files experiment increased their instruction counts/register pressure.
The 68000/020/030/040 branches and PCM/sample rates are unchanged.

A known-power-of-two multiply-accumulate (TNS initial y[n] * 2^20) becomes an
unsigned shift and wrapping addition instead of five partial multiplies.
This remains mathematically exact for negative samples and accumulator wrap.

`AAC060_REG5=0` disables the per-stage scheduling for a hardware comparison;
the known-power-of-two simplification remains enabled. The decoder module
configuration stamp invalidates stale objects when CPU/AACASM/REG5 changes.
The selected source objects depend on the helper and module Makefile too.

Validation:

* `make -f Makefile.amiga aac-mulshift60-ref-test`: one million random pairs,
  edges, full multiply-accumulate and power-of-two accumulation, on the host.
* `make -f Makefile.amiga aac-060-m68k-test`: executes the real assembly under
  qemu-m68k, scans its instruction encodings/undefined helpers, then compares
  complete decoder PCM and metadata with/without scheduling for 22.05/48 kHz
  stereo at full, half and quarter output rates. Fixtures cover all four
  window sequences, PNS and TNS. Requires gcc-m68k-linux-gnu, the cross libc,
  binutils-m68k-linux-gnu, qemu-user and Python 3.

Exploratory instruction profiling used GCC 12, -m68060, QEMU 7.2 and a plugin
counting executed instructions on the first 24 frames of the 22.05 kHz
fixture. Scheduling every file barely helped; selecting the transforms and
TNS reduced total instructions by about 3% against the original helper.
These are instruction counts, not real-68060 cycles, and the Amiga compiler
may allocate registers differently. Real-hardware speed remains unverified.
Compare identical files/settings with MintVID's summary-only --benchmark;
keep frequency, stereo, frame policy and display path unchanged.

The synthetic fixtures derive from MintVID/player/tests/gen_audio_assets.sh:
48 kHz stereo chirps and noise bursts, encoded with FFmpeg AAC. The 22.05 kHz
fixture was resampled/re-encoded at 48 kbps with PNS/TNS enabled. They contain
no third-party programme audio. Both require a nonzero decode count; the
runner checks tool/window coverage rather than accepting a silence-only run.
