/* ***** BEGIN LICENSE BLOCK ***** 
 * Version: RCSL 1.0/RPSL 1.0 
 *  
 * Portions Copyright (c) 1995-2002 RealNetworks, Inc. All Rights Reserved. 
 *      
 * The contents of this file, and the files included with this file, are 
 * subject to the current version of the RealNetworks Public Source License 
 * Version 1.0 (the "RPSL") available at 
 * http://www.helixcommunity.org/content/rpsl unless you have licensed 
 * the file under the RealNetworks Community Source License Version 1.0 
 * (the "RCSL") available at http://www.helixcommunity.org/content/rcsl, 
 * in which case the RCSL will apply. You may also obtain the license terms 
 * directly from RealNetworks.  You may not use this file except in 
 * compliance with the RPSL or, if you have a valid RCSL with RealNetworks 
 * applicable to this file, the RCSL.  Please see the applicable RPSL or 
 * RCSL for the rights, obligations and limitations governing use of the 
 * contents of the file.  
 *  
 * This file is part of the Helix DNA Technology. RealNetworks is the 
 * developer of the Original Code and owns the copyrights in the portions 
 * it created. 
 *  
 * This file, and the files included with this file, is distributed and made 
 * available on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER 
 * EXPRESS OR IMPLIED, AND REALNETWORKS HEREBY DISCLAIMS ALL SUCH WARRANTIES, 
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY, FITNESS 
 * FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT. 
 * 
 * Technology Compatibility Kit Test Suite(s) Location: 
 *    http://www.helixcommunity.org/content/tck 
 * 
 * Contributor(s): 
 *  
 * ***** END LICENSE BLOCK ***** */ 

/**************************************************************************************
 * Fixed-point MP3 decoder
 * Jon Recker (jrecker@real.com), Ken Cooke (kenc@real.com)
 * June 2003
 *
 * subband.c - subband transform (synthesis filterbank implemented via 32-point DCT
 *               followed by polyphase filter)
 **************************************************************************************/

#include "coder.h"
#include "amiga_profile_decode.h"
#include "assembly.h"

/*
 * Diagnostic-only counter for FDCT32HalfSparse16()'s documented contract
 * (buf[16..31] == 0). Disabled unless
 * MP3EnableFdct32HalfSparse16PreconditionCheck(1) is called (wired to
 * --debug-subband-precondition). The stride-2 sparse fast path still performs
 * the high-half scan when compiled with AMIGA_FAST_SUBBAND_CAP so it can fall
 * back safely when a manual subband cap keeps bands 16+ alive; the debug flag
 * only controls whether those scans/violations are counted for diagnostics.
 */
static int gFdct32HalfSparse16CheckEnabled;
volatile unsigned long gFdct32HalfSparse16PreconditionViolations;
volatile unsigned long gFdct32HalfSparse16PreconditionChecks;

void MP3EnableFdct32HalfSparse16PreconditionCheck(int enabled)
{
	gFdct32HalfSparse16CheckEnabled = enabled ? 1 : 0;
	gFdct32HalfSparse16PreconditionViolations = 0;
	gFdct32HalfSparse16PreconditionChecks = 0;
}

/*
 * stride == 3 (14700 Hz) intentionally has no fast path here and falls
 * through to the full FDCT32() below. FDCT32Half (stride 2) and
 * FDCT32Quarter (stride 4) both work by skipping whole recursive
 * sub-blocks of the DCT's shared radix-2 butterfly network -- safe only
 * because 2 and 4 are powers of 2 that align with that network's
 * even/odd and quarter splits. Stride 3's kept-row residue class (every
 * 3rd of 32 rows) is coprime to 2, so it's spread evenly across every
 * sub-block at every recursion depth: no sub-block is ever fully unused,
 * and computing the ~11 needed rows directly (without the shared-butterfly
 * reuse) costs more multiplies than the ~80 the existing full FDCT32
 * already uses for all 32 rows. See README.amiga.md's "--rate 14700"
 * section for the full writeup; the safe win at this rate is the
 * dequant/IMDCT active-subbands cap (MP3FastLowrateEffectiveActiveSubbands),
 * not FDCT32 pruning. FDCT32LowBands() does take the input-side saving:
 * the capped upper bands are zero, so their reads, adds and zero-input
 * multiplies in the first butterfly pass are skipped (bit-exact).
 */
void FDCT32FastLowrate(int *x, int *d, int offset, int oddBlock, int gb,
	int stride, int phase)
{
	(void)phase;
#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE)
	if (stride == 2) {
#if defined(AMIGA_FAST_SUBBAND_CAP)
		{
			int k;
			int sparse16Ok;

			sparse16Ok = 1;
			if (gFdct32HalfSparse16CheckEnabled)
				gFdct32HalfSparse16PreconditionChecks++;
			for (k = 16; k < 32; k++) {
				if (x[k] != 0) {
					sparse16Ok = 0;
					if (gFdct32HalfSparse16CheckEnabled)
						gFdct32HalfSparse16PreconditionViolations++;
					break;
				}
			}
			if (sparse16Ok)
				FDCT32HalfSparse16(x, d, offset, oddBlock, gb);
			else
				FDCT32Half(x, d, offset, oddBlock, gb);
		}
#else
		FDCT32Half(x, d, offset, oddBlock, gb);
#endif
		return;
	}
	if (stride == 4 && MP3ExperimentalFDCT32QuarterEnabled()) {
		FDCT32Quarter(x, d, offset, oddBlock, gb, phase, stride);
		return;
	}
	/* Strides 3 and 5 (and 4 without the quarter transform) need every
	 * row; FDCT32LowBands still skips the capped, all-zero upper bands. */
	FDCT32LowBands(x, d, offset, oddBlock, gb);
	return;
#else
	(void)stride;
#endif
	FDCT32(x, d, offset, oddBlock, gb);
}

/**************************************************************************************
 * Function:    Subband
 *
 * Description: do subband transform on all the blocks in one granule, all channels
 *
 * Inputs:      filled MP3DecInfo structure, after calling IMDCT for all channels
 *              vbuf[ch] and vindex[ch] must be preserved between calls
 *
 * Outputs:     decoded PCM data, interleaved LRLRLR... if stereo
 *
 * Return:      0 on success,  -1 if null input pointers
 **************************************************************************************/
int Subband(MP3DecInfo *mp3DecInfo, short *pcmBuf)
{
	int b;
	HuffmanInfo *hi;
	IMDCTInfo *mi;
	SubbandInfo *sbi;
	clock_t amigaProfileStart;
	int vindex;
	int stride;
	int *vbuf;
	int outputMono;

	/* validate pointers */
	if (!mp3DecInfo || !mp3DecInfo->HuffmanInfoPS || !mp3DecInfo->IMDCTInfoPS || !mp3DecInfo->SubbandInfoPS)
		return -1;

	hi = (HuffmanInfo *)mp3DecInfo->HuffmanInfoPS;
	mi = (IMDCTInfo *)(mp3DecInfo->IMDCTInfoPS);
	sbi = (SubbandInfo*)(mp3DecInfo->SubbandInfoPS);
	(void)hi;

	vindex = sbi->vindex;
	vbuf = sbi->vbuf;
	stride = mp3DecInfo->fastLowrateStride;
	outputMono = (mp3DecInfo->outputMono && mp3DecInfo->nChans == 2);
	if (mp3DecInfo->nChans == 2 && !outputMono) {
		/* stereo */
		if (stride > 1) {
			int phase;
			int lowrateOutputSamps;

			phase = mp3DecInfo->fastLowratePhase;
			lowrateOutputSamps = mp3DecInfo->fastLowrateOutputSamps;
			for (b = 0; b < BLOCK_SIZE; b += 2) {
				int produced;
				int *vbase;

				AMIGA_PROFILE_START(amigaProfileStart);
				FDCT32FastLowrate(mi->outBuf[0][b], vbuf + 0*32, vindex, 0,
					mi->gb[0], stride, phase);
				FDCT32FastLowrate(mi->outBuf[1][b], vbuf + 1*32, vindex, 0,
					mi->gb[1], stride, phase);
				AMIGA_PROFILE_STOP(MP3_DECODE_CORE_PROFILE_SUBBAND_DCT32, amigaProfileStart);
				AMIGA_PROFILE_START(amigaProfileStart);
				vbase = vbuf + vindex;
				produced = PolyphaseStereoFastLowrate(pcmBuf, vbase, polyCoef,
					stride, &phase);
				lowrateOutputSamps += produced;
				pcmBuf += produced;
				AMIGA_PROFILE_STOP(MP3_DECODE_CORE_PROFILE_POLYPHASE, amigaProfileStart);

				AMIGA_PROFILE_START(amigaProfileStart);
				FDCT32FastLowrate(mi->outBuf[0][b + 1], vbuf + 0*32, vindex, 1,
					mi->gb[0], stride, phase);
				FDCT32FastLowrate(mi->outBuf[1][b + 1], vbuf + 1*32, vindex, 1,
					mi->gb[1], stride, phase);
				AMIGA_PROFILE_STOP(MP3_DECODE_CORE_PROFILE_SUBBAND_DCT32, amigaProfileStart);
				AMIGA_PROFILE_START(amigaProfileStart);
				vbase = vbuf + vindex + VBUF_LENGTH;
				produced = PolyphaseStereoFastLowrate(pcmBuf, vbase, polyCoef,
					stride, &phase);
				lowrateOutputSamps += produced;
				pcmBuf += produced;
				AMIGA_PROFILE_STOP(MP3_DECODE_CORE_PROFILE_POLYPHASE, amigaProfileStart);
				vindex = (vindex - 1) & 7;
			}
			mp3DecInfo->fastLowratePhase = phase;
			mp3DecInfo->fastLowrateOutputSamps = lowrateOutputSamps;
		} else {
			for (b = 0; b < BLOCK_SIZE; b += 2) {
				int *vbase;

				AMIGA_PROFILE_START(amigaProfileStart);
				FDCT32(mi->outBuf[0][b], vbuf + 0*32, vindex, 0, mi->gb[0]);
				FDCT32(mi->outBuf[1][b], vbuf + 1*32, vindex, 0, mi->gb[1]);
				AMIGA_PROFILE_STOP(MP3_DECODE_CORE_PROFILE_SUBBAND_DCT32, amigaProfileStart);
				AMIGA_PROFILE_START(amigaProfileStart);
				vbase = vbuf + vindex;
				PolyphaseStereo(pcmBuf, vbase, polyCoef);
				pcmBuf += (2 * NBANDS);
				AMIGA_PROFILE_STOP(MP3_DECODE_CORE_PROFILE_POLYPHASE, amigaProfileStart);

				AMIGA_PROFILE_START(amigaProfileStart);
				FDCT32(mi->outBuf[0][b + 1], vbuf + 0*32, vindex, 1, mi->gb[0]);
				FDCT32(mi->outBuf[1][b + 1], vbuf + 1*32, vindex, 1, mi->gb[1]);
				AMIGA_PROFILE_STOP(MP3_DECODE_CORE_PROFILE_SUBBAND_DCT32, amigaProfileStart);
				AMIGA_PROFILE_START(amigaProfileStart);
				vbase = vbuf + vindex + VBUF_LENGTH;
				PolyphaseStereo(pcmBuf, vbase, polyCoef);
				pcmBuf += (2 * NBANDS);
				AMIGA_PROFILE_STOP(MP3_DECODE_CORE_PROFILE_POLYPHASE, amigaProfileStart);
				vindex = (vindex - 1) & 7;
			}
		}
	} else {
		/* mono */
		if (stride > 1) {
			int phase;
			int lowrateOutputSamps;

			phase = mp3DecInfo->fastLowratePhase;
			lowrateOutputSamps = mp3DecInfo->fastLowrateOutputSamps;
			for (b = 0; b < BLOCK_SIZE; b += 2) {
				int produced;
				int *vbase;

				AMIGA_PROFILE_START(amigaProfileStart);
				FDCT32FastLowrate(mi->outBuf[0][b], vbuf + 0*32, vindex, 0,
					mi->gb[0], stride, phase);
				AMIGA_PROFILE_STOP(MP3_DECODE_CORE_PROFILE_SUBBAND_DCT32, amigaProfileStart);
				AMIGA_PROFILE_START(amigaProfileStart);
				vbase = vbuf + vindex;
				produced = PolyphaseMonoFastLowrate(pcmBuf, vbase, polyCoef,
					stride, &phase);
				lowrateOutputSamps += produced;
				pcmBuf += produced;
				AMIGA_PROFILE_STOP(MP3_DECODE_CORE_PROFILE_POLYPHASE, amigaProfileStart);

				AMIGA_PROFILE_START(amigaProfileStart);
				FDCT32FastLowrate(mi->outBuf[0][b + 1], vbuf + 0*32, vindex, 1,
					mi->gb[0], stride, phase);
				AMIGA_PROFILE_STOP(MP3_DECODE_CORE_PROFILE_SUBBAND_DCT32, amigaProfileStart);
				AMIGA_PROFILE_START(amigaProfileStart);
				vbase = vbuf + vindex + VBUF_LENGTH;
				produced = PolyphaseMonoFastLowrate(pcmBuf, vbase, polyCoef,
					stride, &phase);
				lowrateOutputSamps += produced;
				pcmBuf += produced;
				AMIGA_PROFILE_STOP(MP3_DECODE_CORE_PROFILE_POLYPHASE, amigaProfileStart);
				vindex = (vindex - 1) & 7;
			}
			mp3DecInfo->fastLowratePhase = phase;
			mp3DecInfo->fastLowrateOutputSamps = lowrateOutputSamps;
		} else {
			for (b = 0; b < BLOCK_SIZE; b += 2) {
				int *vbase;

				AMIGA_PROFILE_START(amigaProfileStart);
				FDCT32(mi->outBuf[0][b], vbuf + 0*32, vindex, 0, mi->gb[0]);
				AMIGA_PROFILE_STOP(MP3_DECODE_CORE_PROFILE_SUBBAND_DCT32, amigaProfileStart);
				AMIGA_PROFILE_START(amigaProfileStart);
				vbase = vbuf + vindex;
				PolyphaseMono(pcmBuf, vbase, polyCoef);
				pcmBuf += NBANDS;
				AMIGA_PROFILE_STOP(MP3_DECODE_CORE_PROFILE_POLYPHASE, amigaProfileStart);

				AMIGA_PROFILE_START(amigaProfileStart);
				FDCT32(mi->outBuf[0][b + 1], vbuf + 0*32, vindex, 1, mi->gb[0]);
				AMIGA_PROFILE_STOP(MP3_DECODE_CORE_PROFILE_SUBBAND_DCT32, amigaProfileStart);
				AMIGA_PROFILE_START(amigaProfileStart);
				vbase = vbuf + vindex + VBUF_LENGTH;
				PolyphaseMono(pcmBuf, vbase, polyCoef);
				pcmBuf += NBANDS;
				AMIGA_PROFILE_STOP(MP3_DECODE_CORE_PROFILE_POLYPHASE, amigaProfileStart);
				vindex = (vindex - 1) & 7;
			}
		}
	}

	sbi->vindex = vindex;
	return 0;
}
