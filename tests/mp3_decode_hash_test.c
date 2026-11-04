/* Whole-file decode of a 44.1 kHz joint-stereo MP3 at full rate and at the
 * fast low-rate strides (22050/14700/11025 Hz), with and without the reduced
 * filter. Prints one PCM hash per configuration; the Makefile builds this
 * with and without AMIGA_M68K_POLYPHASE_68060 and requires identical output,
 * so the 68060 hardware-multiply MULSHIFT32/DCT/dequant are bit-exact. */
#include "mp3dec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char *load(const char *path, int *size)
{
	FILE *f = fopen(path, "rb"); long n; unsigned char *b;
	if (!f) return NULL;
	fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
	b = (unsigned char *)malloc((size_t)n);
	if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
	fclose(f); *size = (int)n; return b;
}

static int decode(const unsigned char *data, int size, int stride, int superfast,
	int reduced, unsigned long *hash, long *samples)
{
	HMP3Decoder d = MP3InitDecoder();
	unsigned char *at = (unsigned char *)data; int left = size, frames = 0;
	static short pcm[MAX_NCHAN * MAX_NGRAN * MAX_NSAMP];
	MP3FrameInfo info;
	if (!d) return -1;
	MP3SetExperimentalReducedTaps(reduced);
	if (stride > 1) { MP3SetFastLowrate(d, stride); MP3SetSuperfastLowrate(d, superfast); }
	*hash = 2166136261UL; *samples = 0;
	for (;;) {
		int off = MP3FindSyncWord(at, left), rc, i;
		if (off < 0) break;
		at += off; left -= off;
		rc = MP3Decode(d, &at, &left, pcm, 0);
		if (rc == ERR_MP3_INDATA_UNDERFLOW) break;
		if (rc == ERR_MP3_MAINDATA_UNDERFLOW) continue;
		if (rc) { if (left > 4) { ++at; --left; continue; } break; }
		MP3GetLastFrameInfo(d, &info);
		for (i = 0; i < info.outputSamps; i++)
			*hash = ((*hash ^ (unsigned short)pcm[i]) * 16777619UL) & 0xffffffffUL;
		*samples += info.outputSamps; ++frames;
	}
	MP3FreeDecoder(d);
	return frames;
}

int main(int argc, char **argv)
{
	static const struct { int stride, superfast, reduced; const char *name; } cfg[] = {
		{ 1, 0, 0, "44100 full" }, { 2, 0, 1, "22050 reduced" }, { 3, 0, 0, "14700 full filter" },
		{ 3, 1, 1, "14700 superfast reduced" }, { 3, 0, 1, "14700 reduced" }, { 4, 1, 1, "11025 superfast reduced" }
	};
	int size, i, failures = 0;
	unsigned char *data = load(argc > 1 ? argv[1] : "tests/fixtures/stereo44k.mp3", &size);
	if (!data) { puts("cannot read the MP3 fixture"); return 1; }
	for (i = 0; i < (int)(sizeof(cfg) / sizeof(cfg[0])); i++) {
		unsigned long h; long n; int frames = decode(data, size, cfg[i].stride, cfg[i].superfast, cfg[i].reduced, &h, &n);
		if (frames < 200 || n <= 0) ++failures;
		printf("%-26s frames=%d samples=%ld hash=%08lx\n", cfg[i].name, frames, n, h);
	}
	free(data);
	return failures ? 1 : 0;
}
