/* The reduced (middle eight taps) 14700 Hz filter must produce, for every
 * phase, exactly the samples 0/3/6.. (phase 0), 2/5/8.. (1), 1/4/7.. (2) of the
 * trusted reduced 22050 (even samples) and 11025 (odd samples) filters run on
 * the same synthesis buffer, mono and stereo, with the right count and next
 * phase. It must also differ from the full sixteen-tap result. */
#include "coder.h"
#include "mp3dec.h"
#include <stdio.h>
#include <string.h>

static unsigned long rng = 0x13579bdUL;
static int rnd(void) { rng = rng * 1664525UL + 1013904223UL; return (int)((rng >> 8) & 0x1fffff) - 0x100000; }

/* Reference sample s (stereo channel ch) from the stride-2/4 reduced filters. */
static short ref_sample(int stereo, int s, int ch, int *vbuf)
{
	short out[64];
	int phase, n;
	if (!(s & 1)) {
		phase = 0;
		n = stereo ? PolyphaseStereoFastLowrate(out, vbuf, polyCoef, 2, &phase)
		           : PolyphaseMonoFastLowrate(out, vbuf, polyCoef, 2, &phase);
		(void)n;
		return stereo ? out[(s / 2) * 2 + ch] : out[s / 2];
	}
	/* Odd s: stride-4 phase p keeps samples (4 - p) % 4, +4, ... */
	phase = (4 - (s & 3)) & 3;
	{
		int p0 = phase, idx = (s - ((4 - p0) & 3)) / 4;
		n = stereo ? PolyphaseStereoFastLowrate(out, vbuf, polyCoef, 4, &phase)
		           : PolyphaseMonoFastLowrate(out, vbuf, polyCoef, 4, &phase);
		(void)n;
		return stereo ? out[idx * 2 + ch] : out[idx];
	}
}

int main(void)
{
	static int vbuf[MAX_NCHAN * VBUF_LENGTH];
	short out[64], full[64];
	int stereo, phase, trial, i, failures = 0, differs = 0, cases = 0;

	MP3SetExperimentalReducedTaps(1);
	for (trial = 0; trial < 200; trial++) {
		for (i = 0; i < MAX_NCHAN * VBUF_LENGTH; i++) vbuf[i] = rnd();
		for (stereo = 0; stereo < 2; stereo++)
			for (phase = 0; phase < 3; phase++) {
				int p = phase, first = (3 - phase) % 3, count = 0, s, n, k;
				for (s = first; s < 32; s += 3) ++count;
				n = stereo ? PolyphaseStereoFastLowrate(out, vbuf, polyCoef, 3, &p)
				           : PolyphaseMonoFastLowrate(out, vbuf, polyCoef, 3, &p);
				++cases;
				if (n != count * (stereo + 1) || p != (phase + 2) % 3) {
					if (failures++ < 5) printf("count/phase: stereo=%d phase=%d n=%d next=%d\n", stereo, phase, n, p);
					continue;
				}
				for (k = 0, s = first; s < 32; s += 3, ++k) {
					int ch;
					for (ch = 0; ch <= stereo; ++ch) {
						short want = ref_sample(stereo, s, ch, vbuf);
						short got = stereo ? out[k * 2 + ch] : out[k];
						if (got != want && failures++ < 5)
							printf("sample %d stereo=%d phase=%d ch=%d: got %d want %d\n", s, stereo, phase, ch, got, want);
					}
				}
				/* The full filter is a different (longer) response. */
				MP3SetExperimentalReducedTaps(0);
				p = phase;
				if (stereo) PolyphaseStereoFastLowrate(full, vbuf, polyCoef, 3, &p);
				else PolyphaseMonoFastLowrate(full, vbuf, polyCoef, 3, &p);
				MP3SetExperimentalReducedTaps(1);
				if (memcmp(full, out, (size_t)n * sizeof(short))) ++differs;
			}
	}
	if (!differs) { ++failures; puts("reduced filter never differed from the full filter"); }
	printf("Polyphase stride-3 reduced: %d cases, %d failures\n", cases, failures);
	return failures ? 1 : 0;
}
