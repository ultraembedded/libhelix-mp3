/* FDCT32LowBands must equal FDCT32_C_REFERENCE whenever bands 16..31 are
 * zero (the fast low-rate subband cap), for every offset/oddBlock/guard-bit
 * case. The program also prints a hash of the full, half, sparse-half,
 * quarter and low-band transforms: the Makefile builds it with and without
 * AMIGA_M68K_POLYPHASE_68060 and requires equal hashes, proving the 68060
 * hardware-multiply DCT is bit-identical to the 64-bit reference. */
#include "coder.h"
#include "mp3dec.h"
#include <stdio.h>
#include <string.h>

#define DEST_INTS (VBUF_LENGTH * 2)
static unsigned long rng = 0x2468aceUL;
static unsigned long next(void) { rng = rng * 1664525UL + 1013904223UL; return rng & 0xffffffffUL; }
static unsigned long hash = 2166136261UL;
static void mix(const int *d)
{
	int i;
	for (i = 0; i < DEST_INTS; i++) {
		unsigned long v = (unsigned long)d[i] & 0xffffffffUL;
		int b;
		for (b = 0; b < 4; b++) hash = ((hash ^ ((v >> (8 * b)) & 0xff)) * 16777619UL) & 0xffffffffUL;
	}
}

int main(void)
{
	static int in[32], b1[32], b2[32], d1[DEST_INTS], d2[DEST_INTS];
	static const int nonzero[] = { 6, 8, 10, 12, 16 };
	static const int bits[] = { 16, 22, 24, 26, 27 };
	int trial, failures = 0, cases = 0;

	MP3SetExperimentalFDCT32Quarter(1);
	for (trial = 0; trial < 6000; trial++) {
		int n = nonzero[trial % 5], m = bits[(trial / 5) % 5];
		int offset = (int)(next() & 7), odd = (int)(next() & 1);
		int gb = 31 - m, i;
		for (i = 0; i < 32; i++) {
			long v = (long)(next() & ((1UL << m) - 1)) - (1L << (m - 1));
			in[i] = i < n ? (int)v : 0;
		}
		if (trial % 7 == 0) in[n - 1] = 0; /* a zero inside the live band */

		/* Low-band vs full reference. */
		memcpy(b1, in, sizeof(in)); memcpy(b2, in, sizeof(in));
		memset(d1, 0, sizeof(d1)); memset(d2, 0, sizeof(d2));
		FDCT32_C_REFERENCE(b1, d1, offset, odd, gb);
		FDCT32LowBands(b2, d2, offset, odd, gb);
		++cases;
		if (memcmp(d1, d2, sizeof(d1))) {
			if (failures++ < 5) printf("mismatch trial %d n=%d bits=%d offset=%d odd=%d\n", trial, n, m, offset, odd);
		}
		mix(d1);

		/* Hash the other C transforms for the 68060 multiply comparison. */
		memcpy(b1, in, sizeof(in)); memset(d1, 0, sizeof(d1));
		FDCT32Half(b1, d1, offset, odd, gb); mix(d1);
		memcpy(b1, in, sizeof(in)); memset(d1, 0, sizeof(d1));
		FDCT32HalfSparse16(b1, d1, offset, odd, gb); mix(d1);
		memcpy(b1, in, sizeof(in)); memset(d1, 0, sizeof(d1));
		FDCT32Quarter(b1, d1, offset, odd, gb, trial & 3, 4); mix(d1);
	}
	/* Nonzero upper band: LowBands must fall back to the full transform. */
	memcpy(b1, in, sizeof(in)); b1[20] = 12345; memcpy(b2, b1, sizeof(b1));
	memset(d1, 0, sizeof(d1)); memset(d2, 0, sizeof(d2));
	FDCT32_C_REFERENCE(b1, d1, 3, 1, 7); FDCT32LowBands(b2, d2, 3, 1, 7);
	if (memcmp(d1, d2, sizeof(d1))) { ++failures; puts("fallback mismatch"); }

	printf("FDCT32 low-band: %d cases, %d failures; transform hash %08lx\n", cases, failures, hash);
	return failures ? 1 : 0;
}
