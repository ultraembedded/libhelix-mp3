#ifdef MINTAMP_SKIN
#include "../skin_controls.h"
#endif
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
 * imdct.c - antialias, inverse transform (short/long/mixed), windowing, 
 *             overlap-add, frequency inversion
 **************************************************************************************/

#include "coder.h"
#include "assembly.h"
#include <stdio.h>
#include <string.h>

/**************************************************************************************
 * Function:    AntiAlias
 *
 * Description: smooth transition across DCT block boundaries (every 18 coefficients)
 *
 * Inputs:      vector of dequantized coefficients, length = (nBfly+1) * 18
 *              number of "butterflies" to perform (one butterfly means one
 *                inter-block smoothing operation)
 *
 * Outputs:     updated coefficient vector x
 *
 * Return:      none
 *
 * Notes:       weighted average of opposite bands (pairwise) from the 8 samples 
 *                before and after each block boundary
 *              nBlocks = (nonZeroBound + 7) / 18, since nZB is the first ZERO sample 
 *                above which all other samples are also zero
 *              max gain per sample = 1.372
 *                MAX(i) (abs(csa[i][0]) + abs(csa[i][1]))
 *              bits gained = 0
 *              assume at least 1 guard bit in x[] to avoid overflow
 *                (should be guaranteed from dequant, and max gain from stproc * max 
 *                 gain from AntiAlias < 2.0)
 **************************************************************************************/
#if defined(AMIGA_M68K) && defined(AMIGA_M68K_ASM_ANTIALIAS) && \
	defined(__GNUC__) && (defined(__mc68020__) || defined(__mc68030__) || \
	defined(__mc68040__) || defined(__mc68060__) || defined(mc68020))
#define ANTIALIAS_HAS_AMIGA_M68K_ASM 1
#else
#define ANTIALIAS_HAS_AMIGA_M68K_ASM 0
#endif

// a little bit faster in RAM (< 1 ms per block)
void AntiAlias_C_REFERENCE(int *x, int nBfly)
{
	int k, a0, b0, c0, c1;
	const int *c;

	/* csa = Q31 */
	for (k = nBfly; k > 0; k--) {
		c = csa[0];
		x += 18;

		a0 = x[-1];			c0 = *c;	c++;	b0 = x[0];		c1 = *c;	c++;
		x[-1] = (MULSHIFT32(c0, a0) - MULSHIFT32(c1, b0)) << 1;	
		x[0] =  (MULSHIFT32(c0, b0) + MULSHIFT32(c1, a0)) << 1;

		a0 = x[-2];			c0 = *c;	c++;	b0 = x[1];		c1 = *c;	c++;
		x[-2] = (MULSHIFT32(c0, a0) - MULSHIFT32(c1, b0)) << 1;	
		x[1] =  (MULSHIFT32(c0, b0) + MULSHIFT32(c1, a0)) << 1;
		
		a0 = x[-3];			c0 = *c;	c++;	b0 = x[2];		c1 = *c;	c++;
		x[-3] = (MULSHIFT32(c0, a0) - MULSHIFT32(c1, b0)) << 1;	
		x[2] =  (MULSHIFT32(c0, b0) + MULSHIFT32(c1, a0)) << 1;

		a0 = x[-4];			c0 = *c;	c++;	b0 = x[3];		c1 = *c;	c++;
		x[-4] = (MULSHIFT32(c0, a0) - MULSHIFT32(c1, b0)) << 1;	
		x[3] =  (MULSHIFT32(c0, b0) + MULSHIFT32(c1, a0)) << 1;

		a0 = x[-5];			c0 = *c;	c++;	b0 = x[4];		c1 = *c;	c++;
		x[-5] = (MULSHIFT32(c0, a0) - MULSHIFT32(c1, b0)) << 1;	
		x[4] =  (MULSHIFT32(c0, b0) + MULSHIFT32(c1, a0)) << 1;

		a0 = x[-6];			c0 = *c;	c++;	b0 = x[5];		c1 = *c;	c++;
		x[-6] = (MULSHIFT32(c0, a0) - MULSHIFT32(c1, b0)) << 1;	
		x[5] =  (MULSHIFT32(c0, b0) + MULSHIFT32(c1, a0)) << 1;

		a0 = x[-7];			c0 = *c;	c++;	b0 = x[6];		c1 = *c;	c++;
		x[-7] = (MULSHIFT32(c0, a0) - MULSHIFT32(c1, b0)) << 1;	
		x[6] =  (MULSHIFT32(c0, b0) + MULSHIFT32(c1, a0)) << 1;

		a0 = x[-8];			c0 = *c;	c++;	b0 = x[7];		c1 = *c;	c++;
		x[-8] = (MULSHIFT32(c0, a0) - MULSHIFT32(c1, b0)) << 1;	
		x[7] =  (MULSHIFT32(c0, b0) + MULSHIFT32(c1, a0)) << 1;
	}
}

#if ANTIALIAS_HAS_AMIGA_M68K_ASM
static void AntiAlias_AmigaM68KAsm(int *x, int nBfly)
{
	const int *cBase;
	const int *c;
	int *xLo;
	int *xHi;
	int outer;

	if (nBfly <= 0)
		return;

	cBase = csa[0];
	xLo = x;
	outer = nBfly - 1;
	__asm__ volatile (
		"1:\n\t"
		/* Advance to the next 18-sample block boundary. */
		"lea 72(%[xlo]),%[xlo]\n\t"
		"movea.l %[xlo],%[xhi]\n\t"
		"movea.l %[cbase],%[c]\n\t"
		"moveq #7,%%d6\n"
		"2:\n\t"
		/* Read a0 = x[-(i+1)] and b0 = x[i]. */
		"move.l -(%[xlo]),%%d0\n\t"
		"move.l (%[xhi])+,%%d1\n\t"
		/* newLo = (MULSHIFT32(c0, a0) - MULSHIFT32(c1, b0)) << 1. */
		"move.l %%d0,%%d2\n\t"
		"muls.l (%[c]),%%d3:%%d2\n\t"
		"move.l %%d1,%%d4\n\t"
		"muls.l 4(%[c]),%%d5:%%d4\n\t"
		"sub.l %%d5,%%d3\n\t"
		"add.l %%d3,%%d3\n\t"
		/* newHi = (MULSHIFT32(c0, b0) + MULSHIFT32(c1, a0)) << 1. */
		"move.l %%d1,%%d2\n\t"
		"muls.l (%[c]),%%d5:%%d2\n\t"
		"move.l %%d0,%%d4\n\t"
		"muls.l 4(%[c]),%%d2:%%d4\n\t"
		"add.l %%d2,%%d5\n\t"
		"add.l %%d5,%%d5\n\t"
		/* Store to the locations just read by pre-decrement/post-increment. */
		"move.l %%d3,(%[xlo])\n\t"
		"move.l %%d5,-4(%[xhi])\n\t"
		"addq.l #8,%[c]\n\t"
		"dbra %%d6,2b\n\t"
		/* Restore xlo to the boundary: the inner pre-decrement loop walked it
		 * 8 * 4 = 32 bytes backwards. Without this, the next outer iteration's
		 * lea 72(%[xlo]) would advance from boundary-32, placing the next
		 * boundary 32 bytes too early and corrupting every subsequent block. */
		"lea 32(%[xlo]),%[xlo]\n\t"
		"dbra %[outer],1b\n\t"
		: [outer] "+d" (outer), [xlo] "+a" (xLo), [xhi] "=&a" (xHi), [c] "=&a" (c)
		: [cbase] "a" (cBase)
		: "d0", "d1", "d2", "d3", "d4", "d5", "d6", "cc", "memory");
}
#endif

int AntiAlias_HAS_AMIGA_M68K_ASM_RUNTIME(void)
{
	return ANTIALIAS_HAS_AMIGA_M68K_ASM;
}

void AntiAlias_TEST_ACTIVE(int *x, int nBfly)
{
#if ANTIALIAS_HAS_AMIGA_M68K_ASM
	AntiAlias_AmigaM68KAsm(x, nBfly);
#else
	AntiAlias_C_REFERENCE(x, nBfly);
#endif
}

static void AntiAlias(int *x, int nBfly)
{
	AntiAlias_TEST_ACTIVE(x, nBfly);
}

/**************************************************************************************
 * Function:    WinPrevious
 *
 * Description: apply specified window to second half of previous IMDCT (overlap part)
 *
 * Inputs:      vector of 9 coefficients (xPrev)
 *
 * Outputs:     18 windowed output coefficients (gain 1 integer bit)
 *              window type (0, 1, 2, 3)
 *
 * Return:      none
 * 
 * Notes:       produces 9 output samples from 18 input samples via symmetry
 *              all blocks gain at least 1 guard bit via window (long blocks get extra
 *                sign bit, short blocks can have one addition but max gain < 1.0)
 **************************************************************************************/
static void WinPrevious(int *xPrev, int *xPrevWin, int btPrev)
{
	int i, x, *xp, *xpwLo, *xpwHi, wLo, wHi;
	const int *wpLo, *wpHi;

	xp = xPrev;
	/* mapping (see IMDCT12x3): xPrev[0-2] = sum[6-8], xPrev[3-8] = sum[12-17] */
	if (btPrev == 2) {
		/* this could be reordered for minimum loads/stores */
		wpLo = imdctWin[btPrev];
		xPrevWin[ 0] = MULSHIFT32(wpLo[ 6], xPrev[2]) + MULSHIFT32(wpLo[0], xPrev[6]);
		xPrevWin[ 1] = MULSHIFT32(wpLo[ 7], xPrev[1]) + MULSHIFT32(wpLo[1], xPrev[7]);
		xPrevWin[ 2] = MULSHIFT32(wpLo[ 8], xPrev[0]) + MULSHIFT32(wpLo[2], xPrev[8]);
		xPrevWin[ 3] = MULSHIFT32(wpLo[ 9], xPrev[0]) + MULSHIFT32(wpLo[3], xPrev[8]);
		xPrevWin[ 4] = MULSHIFT32(wpLo[10], xPrev[1]) + MULSHIFT32(wpLo[4], xPrev[7]);
		xPrevWin[ 5] = MULSHIFT32(wpLo[11], xPrev[2]) + MULSHIFT32(wpLo[5], xPrev[6]);
		xPrevWin[ 6] = MULSHIFT32(wpLo[ 6], xPrev[5]);
		xPrevWin[ 7] = MULSHIFT32(wpLo[ 7], xPrev[4]);
		xPrevWin[ 8] = MULSHIFT32(wpLo[ 8], xPrev[3]);
		xPrevWin[ 9] = MULSHIFT32(wpLo[ 9], xPrev[3]);
		xPrevWin[10] = MULSHIFT32(wpLo[10], xPrev[4]);
		xPrevWin[11] = MULSHIFT32(wpLo[11], xPrev[5]);
		xPrevWin[12] = xPrevWin[13] = xPrevWin[14] = xPrevWin[15] = xPrevWin[16] = xPrevWin[17] = 0;
	} else {
		/* use ARM-style pointers (*ptr++) so that ADS compiles well */
		wpLo = imdctWin[btPrev] + 18;
		wpHi = wpLo + 17;
		xpwLo = xPrevWin;
		xpwHi = xPrevWin + 17;
		for (i = 9; i > 0; i--) {
			x = *xp++;	wLo = *wpLo++;	wHi = *wpHi--;
			*xpwLo++ = MULSHIFT32(wLo, x);
			*xpwHi-- = MULSHIFT32(wHi, x);
		}
	}
}

/**************************************************************************************
 * Function:    FreqInvertRescale
 *
 * Description: do frequency inversion (odd samples of odd blocks) and rescale 
 *                if necessary (extra guard bits added before IMDCT)
 *
 * Inputs:      output vector y (18 new samples, spaced NBANDS apart)
 *              previous sample vector xPrev (9 samples)
 *              index of current block
 *              number of extra shifts added before IMDCT (usually 0)
 *
 * Outputs:     inverted and rescaled (as necessary) outputs
 *              rescaled (as necessary) previous samples
 *
 * Return:      updated mOut (from new outputs y)
 **************************************************************************************/
static __inline void FreqInvertOdd(int *y)
{
	int y0, y1, y2, y3, y4, y5, y6, y7, y8;

	y += NBANDS;
	y0 = *y;	y += 2*NBANDS;
	y1 = *y;	y += 2*NBANDS;
	y2 = *y;	y += 2*NBANDS;
	y3 = *y;	y += 2*NBANDS;
	y4 = *y;	y += 2*NBANDS;
	y5 = *y;	y += 2*NBANDS;
	y6 = *y;	y += 2*NBANDS;
	y7 = *y;	y += 2*NBANDS;
	y8 = *y;	y += 2*NBANDS;

	y -= 18*NBANDS;
	*y = -y0;	y += 2*NBANDS;
	*y = -y1;	y += 2*NBANDS;
	*y = -y2;	y += 2*NBANDS;
	*y = -y3;	y += 2*NBANDS;
	*y = -y4;	y += 2*NBANDS;
	*y = -y5;	y += 2*NBANDS;
	*y = -y6;	y += 2*NBANDS;
	*y = -y7;	y += 2*NBANDS;
	*y = -y8;
}


/*
 * Single source of truth for "how many of the 32 subbands does this decode
 * actually keep," shared by IMDCTApplySubbandCap() below (which acts on it),
 * IMDCT()'s AntiAlias() call site (which needs to know it *before*
 * IMDCTApplySubbandCap() normally runs, to size how much antialias work is
 * worth doing -- see the comment at that call site), and DequantChannel()
 * (real/dqchan.c, via Dequantize() in real/dequant.c), which uses the same
 * activeSubbands*18 sample-index convention to skip dequantizing critical
 * bands the IMDCT subband cap will never read. Returns NBANDS (32) as a
 * "no cap active" sentinel, matching IMDCTApplySubbandCap()'s original
 * return-0 behavior. Not static so real/dequant.c can call it too.
 */
int MP3FastLowrateEffectiveActiveSubbands(const MP3DecInfo *mp3DecInfo)
{
	int activeSubbands;

	if (!mp3DecInfo)
		return NBANDS;

	activeSubbands = mp3DecInfo->fastLowrateActiveSubbands;
	if (activeSubbands <= 0) activeSubbands = NBANDS;
	if (activeSubbands > NBANDS) activeSubbands = NBANDS;

	if (mp3DecInfo->superfastLowrate && mp3DecInfo->fastLowrateStride > 1)
		return activeSubbands;

#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE) && defined(AMIGA_FAST_SUBBAND_CAP)
	if (mp3DecInfo->fastLowrateStride >= 2) {
		/* MP3SetFastLowrate() already set fastLowrateActiveSubbands to the
		 * stride's own output-Nyquist-derived default (16/10/8/6 for
		 * stride 2/3/4/5), and MP3SetSubbandCap() can only ever lower it
		 * further. Previously this returned a hardcoded 16 for every
		 * stride >= 2 here (a leftover from when stride 2 was the only
		 * fast-lowrate rate), which silently widened stride 3/4/5's cap
		 * back out to 16 and made --subband-cap a no-op outside superfast
		 * mode. Just use the value already computed above. */
		return activeSubbands;
	}
#endif
	return NBANDS;
}

static int IMDCTApplySubbandCap(const MP3DecInfo *mp3DecInfo, BlockCount *bc)
{
	int activeSubbands;

	if (bc) {
		bc->subbandCapActive = 0;
		bc->activeSubbands = NBANDS;
	}
	if (!mp3DecInfo || !bc)
		return 0;

	activeSubbands = MP3FastLowrateEffectiveActiveSubbands(mp3DecInfo);
	if (activeSubbands >= NBANDS)
		return 0;

	if (bc->nBlocksTotal > activeSubbands) bc->nBlocksTotal = activeSubbands;
	if (bc->nBlocksLong  > activeSubbands) bc->nBlocksLong  = activeSubbands;
	if (bc->nBlocksPrev  > activeSubbands) bc->nBlocksPrev  = activeSubbands;
	bc->activeSubbands = activeSubbands;
	bc->subbandCapActive = 1;
	return 1;
}


static void IMDCTClearDiscardedSubbands(IMDCTInfo *mi, int ch, int activeSubbands)
{
	int row, band, i;

	if (!mi || activeSubbands >= NBANDS)
		return;
	for (row = 0; row < BLOCK_SIZE; row++)
		for (band = activeSubbands; band < NBANDS; band++)
			mi->outBuf[ch][row][band] = 0;
	for (band = activeSubbands; band < NBANDS; band++)
		for (i = 0; i < 9; i++)
			mi->overBuf[ch][band * 9 + i] = 0;
}

static int IMDCTThinOutputCanActivate(const MP3DecInfo *mp3DecInfo)
{
#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE) && defined(AMIGA_M68K_IMDCT_THIN_OUTPUT)
	/*
	 * Keep the IMDCT36 thinning experiment compiled for selftests only.  Stride-4
	 * fast-lowrate still feeds the normal FDCT32 synthesis, and each emitted PCM
	 * sample depends on all 32 IMDCT subbands for that time slot.  Skipping the
	 * non-phase subbands leaves stale/zero spectral inputs in FDCT32 and produces
	 * garbled playback, so runtime decode must remain on the full IMDCT path until
	 * a matching sparse FDCT32/polyphase path exists.
	 */
	(void)mp3DecInfo;
#else
	(void)mp3DecInfo;
#endif
	return 0;
}

static int FreqInvertRescale(int *y, int *xPrev, int blockIdx, int es)
{
	int i, d, mOut, clipBits, shifted;

	/* undo pre-IMDCT scaling, clipping if necessary */
	mOut = 0;
	clipBits = 31 - es;
	if (blockIdx & 0x01) {
		/* frequency invert */
		for (i = 9; i > 0; i--) {
			d = *y;		CLIP_2N(d, clipBits);	shifted = d << es;	*y = shifted;	mOut |= FASTABS(shifted);	y += NBANDS;
			d = -*y;	CLIP_2N(d, clipBits);	shifted = d << es;	*y = shifted;	mOut |= FASTABS(shifted);	y += NBANDS;
			d = *xPrev;	CLIP_2N(d, clipBits);	*xPrev++ = d << es;
		}
	} else {
		for (i = 9; i > 0; i--) {
			d = *y;		CLIP_2N(d, clipBits);	shifted = d << es;	*y = shifted;	mOut |= FASTABS(shifted);	y += NBANDS;
			d = *y;		CLIP_2N(d, clipBits);	shifted = d << es;	*y = shifted;	mOut |= FASTABS(shifted);	y += NBANDS;
			d = *xPrev;	CLIP_2N(d, clipBits);	*xPrev++ = d << es;
		}
	}
	return mOut;
}

/* format = Q31
 * #define M_PI 3.14159265358979323846
 * double u = 2.0 * M_PI / 9.0;
 * float c0 = sqrt(3.0) / 2.0; 
 * float c1 = cos(u);          
 * float c2 = cos(2*u);        
 * float c3 = sin(u);          
 * float c4 = sin(2*u);
 */

static const int c9_0 = 0x6ed9eba1;
static const int c9_1 = 0x620dbe8b;
static const int c9_2 = 0x163a1a7e;
static const int c9_3 = 0x5246dd49;
static const int c9_4 = 0x7e0e2e32;

/* format = Q31
 * cos(((0:8) + 0.5) * (pi/18)) 
 */
static const int c18[9] = {
	0x7f834ed0, 0x7ba3751d, 0x7401e4c1, 0x68d9f964, 0x5a82799a, 0x496af3e2, 0x36185aee, 0x2120fb83, 0x0b27eb5c, 
};

/* require at least 3 guard bits in x[] to ensure no overflow */
static __inline void idct9(int *x)
{
	int a1, a2, a3, a4, a5, a6, a7, a8, a9;
	int a10, a11, a12, a13, a14, a15, a16, a17, a18;
	int a19, a20, a21, a22, a23, a24, a25, a26, a27;
	int m1, m3, m5, m6, m7, m8, m9, m10, m11, m12;
	int x0, x1, x2, x3, x4, x5, x6, x7, x8;

	x0 = x[0]; x1 = x[1]; x2 = x[2]; x3 = x[3]; x4 = x[4];
	x5 = x[5]; x6 = x[6]; x7 = x[7]; x8 = x[8];

	a1 = x0 - x6;
	a2 = x1 - x5;
	a3 = x1 + x5;
	a4 = x2 - x4;
	a5 = x2 + x4;
	a6 = x2 + x8;
	a7 = x1 + x7;

	a8 = a6 - a5;		/* ie x[8] - x[4] */
	a9 = a3 - a7;		/* ie x[5] - x[7] */
	a10 = a2 - x7;		/* ie x[1] - x[5] - x[7] */
	a11 = a4 - x8;		/* ie x[2] - x[4] - x[8] */

	/* do the << 1 as constant shifts where mX is actually used (free, no stall or extra inst.) */
	m1 =  MULSHIFT32(c9_0, x3);
	m3 =  MULSHIFT32(c9_0, a10);
	m5 =  MULSHIFT32(c9_1, a5);
	m6 =  MULSHIFT32(c9_2, a6);
	m7 =  MULSHIFT32(c9_1, a8);
	m8 =  MULSHIFT32(c9_2, a5);
	m9 =  MULSHIFT32(c9_3, a9);
	m10 = MULSHIFT32(c9_4, a7);
	m11 = MULSHIFT32(c9_3, a3);
	m12 = MULSHIFT32(c9_4, a9);

	a12 = x[0] +  (x[6] >> 1);
	a13 = a12  +  (  m1 << 1);
	a14 = a12  -  (  m1 << 1);
	a15 = a1   +  ( a11 >> 1);
	a16 = ( m5 << 1) + (m6 << 1);
	a17 = ( m7 << 1) - (m8 << 1);
	a18 = a16 + a17;
	a19 = ( m9 << 1) + (m10 << 1);
	a20 = (m11 << 1) - (m12 << 1);

	a21 = a20 - a19;
	a22 = a13 + a16;
	a23 = a14 + a16;
	a24 = a14 + a17;
	a25 = a13 + a17;
	a26 = a14 - a18;
	a27 = a13 - a18;

	x0 = a22 + a19;			x[0] = x0;
	x1 = a15 + (m3 << 1);	x[1] = x1;
	x2 = a24 + a20;			x[2] = x2;
	x3 = a26 - a21;			x[3] = x3;
	x4 = a1 - a11;			x[4] = x4;
	x5 = a27 + a21;			x[5] = x5;
	x6 = a25 - a20;			x[6] = x6;
	x7 = a15 - (m3 << 1);	x[7] = x7;
	x8 = a23 - a19;			x[8] = x8;
}

/* let c(j) = cos(M_PI/36 * ((j)+0.5)), s(j) = sin(M_PI/36 * ((j)+0.5))
 * then fastWin[2*j+0] = c(j)*(s(j) + c(j)), j = [0, 8]
 *      fastWin[2*j+1] = c(j)*(s(j) - c(j))
 * format = Q30
 */
int fastWin36[18] = {
	0x42aace8b, 0xc2e92724, 0x47311c28, 0xc95f619a, 0x4a868feb, 0xd0859d8c,
	0x4c913b51, 0xd8243ea0, 0x4d413ccc, 0xe0000000, 0x4c913b51, 0xe7dbc161,
	0x4a868feb, 0xef7a6275, 0x47311c28, 0xf6a09e67, 0x42aace8b, 0xfd16d8dd,
};


#if defined(AMIGA_M68K) && defined(AMIGA_M68K_ASM_IMDCT) && defined(__GNUC__) && \
	(defined(__mc68020__) || defined(__mc68030__) || defined(__mc68040__) || \
	 defined(__mc68060__) || defined(mc68020))
#define IMDCT36_HAS_AMIGA_M68K_ASM 1
static __inline int IMDCT36_AMIGA_M68K_MULSHIFT32(int x, int y)
{
	int hi;
	int lo;

	lo = x;
	__asm__ volatile ("muls.l %2,%0:%1"
		: "=d" (hi), "+d" (lo)
		: "dmi" (y));
	(void)lo;
	return hi;
}
#else
#define IMDCT36_HAS_AMIGA_M68K_ASM 0
#endif

/**************************************************************************************
 * Function:    IMDCT36
 *
 * Description: 36-point modified DCT, with windowing and overlap-add (50% overlap)
 *
 * Inputs:      vector of 18 coefficients (N/2 inputs produces N outputs, by symmetry)
 *              overlap part of last IMDCT (9 samples - see output comments)
 *              window type (0,1,2,3) of current and previous block
 *              current block index (for deciding whether to do frequency inversion)
 *              number of guard bits in input vector
 *
 * Outputs:     18 output samples, after windowing and overlap-add with last frame
 *              second half of (unwindowed) 36-point IMDCT - save for next time
 *                only save 9 xPrev samples, using symmetry (see WinPrevious())
 *
 * Notes:       this is Ken's hyper-fast algorithm, including symmetric sin window
 *                optimization, if applicable
 *              total number of multiplies, general case: 
 *                2*10 (idct9) + 9 (last stage imdct) + 36 (for windowing) = 65
 *              total number of multiplies, btCurr == 0 && btPrev == 0:
 *                2*10 (idct9) + 9 (last stage imdct) + 18 (for windowing) = 47
 *
 *              blockType == 0 is by far the most common case, so it should be
 *                possible to use the fast path most of the time
 *              this is the fastest known algorithm for performing 
 *                long IMDCT + windowing + overlap-add in MP3
 *
 * Return:      mOut (OR of abs(y) for all y calculated here)
 *
 * TODO:        optimize for ARM (reorder window coefs, ARM-style pointers in C, 
 *                inline asm may or may not be helpful)
 **************************************************************************************/
// barely faster in RAM
/*
 * Windowing/overlap-add loop for the btCurr != 0 || btPrev != 0 case
 * (factored out of IMDCT36_C_REFERENCE's else branch below, unchanged, so
 * IMDCT36_AMIGA_M68K_ASM can share it after doing the xBuf accumulation +
 * idct9 stage in asm instead of falling back to the full C reference --
 * see that function's header comment for why this matters).
 */
static int IMDCT36_GeneralWindow(int *xp, const int *cp, int *xPrev,
	int btCurr, int btPrev, int *ypLo, int *ypHi)
{
	int i, xo, xe, c, d, mOut, yLo, yHi;
	int xPrevWin[18];
	int *xpwLo, *xpwHi;
	const int *wpLo, *wpHi;

	WinPrevious(xPrev, xPrevWin, btPrev);

	wpLo = imdctWin[btCurr];
	wpHi = wpLo + 17;
	xpwLo = xPrevWin;
	xpwHi = xPrevWin + 17;
	mOut = 0;
	for (i = 9; i > 0; i--) {
		c = *cp--;	xo = *(xp + 9);		xe = *xp--;
		/* gain 2 int bits here */
		xo = MULSHIFT32(c, xo);			/* 2*c18*xOdd (mul by 2 implicit in scaling)  */
		xe >>= 2;

		d = xe - xo;
		(*xPrev++) = xe + xo;	/* symmetry - xPrev[i] = xPrev[17-i] for long blocks */

		yLo = (*xpwLo++ + MULSHIFT32(d, *wpLo++)) << 2;
		yHi = (*xpwHi-- + MULSHIFT32(d, *wpHi--)) << 2;
		*ypLo = yLo;		ypLo += NBANDS;
		*ypHi = yHi;		ypHi -= NBANDS;
		mOut |= FASTABS(yLo);
		mOut |= FASTABS(yHi);
	}
	return mOut;
}

int IMDCT36_C_REFERENCE(int *xCurr, int *xPrev, int *y, int btCurr, int btPrev, int blockIdx, int gb)
{
	int i, es, xBuf[18];
	int acc1, acc2, s, d, t, mOut;
	int xo, xe, c, *xp, *ypLo, *ypHi, yLo, yHi;
	const int *cp, *wp;

	acc1 = acc2 = 0;
	xCurr += 17;

	/* 7 gb is always adequate for antialias + accumulator loop + idct9 */
	if (gb < 7) {
		/* rarely triggered - 5% to 10% of the time on normal clips (with Q25 input) */
		es = 7 - gb;
		for (i = 8; i >= 0; i--) {	
			acc1 = ((*xCurr--) >> es) - acc1;
			acc2 = acc1 - acc2;
			acc1 = ((*xCurr--) >> es) - acc1;
			xBuf[i+9] = acc2;	/* odd */
			xBuf[i+0] = acc1;	/* even */
			xPrev[i] >>= es;
		}
	} else {
		es = 0;
		/* max gain = 18, assume adequate guard bits */
		for (i = 8; i >= 0; i--) {	
			acc1 = (*xCurr--) - acc1;
			acc2 = acc1 - acc2;
			acc1 = (*xCurr--) - acc1;
			xBuf[i+9] = acc2;	/* odd */
			xBuf[i+0] = acc1;	/* even */
		}
	}
	/* xEven[0] and xOdd[0] scaled by 0.5 */
	xBuf[9] >>= 1;
	xBuf[0] >>= 1;

	/* do 9-point IDCT on even and odd */
	idct9(xBuf+0);	/* even */
	idct9(xBuf+9);	/* odd */

	xp = xBuf + 8;
	cp = c18 + 8;
	mOut = 0;
	if (btPrev == 0 && btCurr == 0) {
		/* fast path - use symmetry of sin window to reduce windowing multiplies to 18 (N/2) */
		wp = fastWin36;
		ypLo = y;
		ypHi = y + 17*NBANDS;
		for (i = 9; i > 0; i--) {
			c = *cp--;	xo = *(xp + 9);		xe = *xp--;
			/* gain 2 int bits here */
			xo = MULSHIFT32(c, xo);			/* 2*c18*xOdd (mul by 2 implicit in scaling)  */
			xe >>= 2;

			s = -(*xPrev);		/* sum from last block (always at least 2 guard bits) */
			d = -(xe - xo);		/* gain 2 int bits, don't shift xo (effective << 1 to eat sign bit, << 1 for mul by 2) */
			(*xPrev++) = xe + xo;			/* symmetry - xPrev[i] = xPrev[17-i] for long blocks */
			t = s - d;

			yLo = (d + (MULSHIFT32(t, *wp++) << 2));
			yHi = (s + (MULSHIFT32(t, *wp++) << 2));
			*ypLo = yLo;		ypLo += NBANDS;
			*ypHi = yHi;		ypHi -= NBANDS;
			mOut |= FASTABS(yLo);
			mOut |= FASTABS(yHi);
		}
		xPrev -= 9;	/* rewind the local advance from the loop above */
	} else {
		/* slower method - either prev or curr is using window type != 0 so do full 36-point window
		 * output xPrevWin has at least 3 guard bits (xPrev has 2, gain 1 in WinPrevious)
		 *
		 * NOTE: IMDCT36_GeneralWindow takes xPrev by value, so its internal
		 * (*xPrev++) advance during the write loop does not propagate back to
		 * this xPrev -- it is left pointing at the original (start) position,
		 * which is exactly what FreqInvertRescale/FreqInvertOdd below need, so
		 * unlike the fast path above, no rewind is needed here.
		 */
		ypLo = y;
		ypHi = y + 17*NBANDS;
		mOut = IMDCT36_GeneralWindow(xp, cp, xPrev, btCurr, btPrev, ypLo, ypHi);
	}

	if (es)
		mOut |= FreqInvertRescale(y, xPrev, blockIdx, es);
	else if (blockIdx & 0x01)
		FreqInvertOdd(y);

	return mOut;
}


#if IMDCT36_HAS_AMIGA_M68K_ASM
static __inline void idct9_amiga_m68k_asm(int *x)
{
	int a1, a2, a3, a4, a5, a6, a7, a8, a9;
	int a10, a11, a12, a13, a14, a15, a16, a17, a18;
	int a19, a20, a21, a22, a23, a24, a25, a26, a27;
	int m1, m3, m5, m6, m7, m8, m9, m10, m11, m12;
	int x0, x1, x2, x3, x4, x5, x6, x7, x8;

	x0 = x[0]; x1 = x[1]; x2 = x[2]; x3 = x[3]; x4 = x[4];
	x5 = x[5]; x6 = x[6]; x7 = x[7]; x8 = x[8];

	a1 = x0 - x6;
	a2 = x1 - x5;
	a3 = x1 + x5;
	a4 = x2 - x4;
	a5 = x2 + x4;
	a6 = x2 + x8;
	a7 = x1 + x7;

	a8 = a6 - a5;
	a9 = a3 - a7;
	a10 = a2 - x7;
	a11 = a4 - x8;

	m1 =  IMDCT36_AMIGA_M68K_MULSHIFT32(c9_0, x3);
	m3 =  IMDCT36_AMIGA_M68K_MULSHIFT32(c9_0, a10);
	m5 =  IMDCT36_AMIGA_M68K_MULSHIFT32(c9_1, a5);
	m6 =  IMDCT36_AMIGA_M68K_MULSHIFT32(c9_2, a6);
	m7 =  IMDCT36_AMIGA_M68K_MULSHIFT32(c9_1, a8);
	m8 =  IMDCT36_AMIGA_M68K_MULSHIFT32(c9_2, a5);
	m9 =  IMDCT36_AMIGA_M68K_MULSHIFT32(c9_3, a9);
	m10 = IMDCT36_AMIGA_M68K_MULSHIFT32(c9_4, a7);
	m11 = IMDCT36_AMIGA_M68K_MULSHIFT32(c9_3, a3);
	m12 = IMDCT36_AMIGA_M68K_MULSHIFT32(c9_4, a9);

	a12 = x[0] +  (x[6] >> 1);
	a13 = a12  +  (  m1 << 1);
	a14 = a12  -  (  m1 << 1);
	a15 = a1   +  ( a11 >> 1);
	a16 = ( m5 << 1) + (m6 << 1);
	a17 = ( m7 << 1) - (m8 << 1);
	a18 = a16 + a17;
	a19 = ( m9 << 1) + (m10 << 1);
	a20 = (m11 << 1) - (m12 << 1);

	a21 = a20 - a19;
	a22 = a13 + a16;
	a23 = a14 + a16;
	a24 = a14 + a17;
	a25 = a13 + a17;
	a26 = a14 - a18;
	a27 = a13 - a18;

	x0 = a22 + a19;			x[0] = x0;
	x1 = a15 + (m3 << 1);	x[1] = x1;
	x2 = a24 + a20;			x[2] = x2;
	x3 = a26 - a21;			x[3] = x3;
	x4 = a1 - a11;			x[4] = x4;
	x5 = a27 + a21;			x[5] = x5;
	x6 = a25 - a20;			x[6] = x6;
	x7 = a15 - (m3 << 1);	x[7] = x7;
	x8 = a23 - a19;			x[8] = x8;
}

/*
 * Compact common-long-window kernel.  Keep the nine iterations in one asm
 * region so the 68030 does not pay C spill/reload overhead around the three
 * high-word multiplies in every iteration.  The instruction order mirrors the
 * C reference exactly; in particular, stores happen before FASTABS so INT_MIN
 * retains the reference result.
 */
static __inline int IMDCT36_AMIGA_M68K_LONG_WINDOW(int *xp, const int *cp,
	const int *wp, int *xPrev, int *ypLo, int *ypHi)
{
	int mOut;

	/*
	 * The loop below is the btCurr == 0 && btPrev == 0 C fast path:
	 *
	 *   c = *cp--; xo = *(xp + 9); xe = *xp--;
	 *   xo = MULSHIFT32(c, xo); xe >>= 2;
	 *   s = -*xPrev; d = -(xe - xo); *xPrev++ = xe + xo;
	 *   t = s - d;
	 *   yLo = d + (MULSHIFT32(t, *wp++) << 2);
	 *   yHi = s + (MULSHIFT32(t, *wp++) << 2);
	 *
	 * Instruction-group map for each of the nine DBRA iterations:
	 *   - xo: load xp[9] into d1, load *cp into d0, cp -= 1 int,
	 *     then muls.l d0,d2:d1 so d2 is MULSHIFT32(*old_cp, xp[9]).
	 *   - xe: load *xp into d0, xp -= 1 int, then asr.l #2 to match
	 *     xe >>= 2 exactly.
	 *   - s/d/xPrev: d3 = -*xPrev, d4 = xe + xo stored to
	 *     *xPrev++, and d5 = xo - xe (the C d = -(xe - xo)).
	 *   - yLo: recompute t = s - d in d1, multiply by *wp++, shift
	 *     the high word left by 2, add d, store through ypLo, advance
	 *     ypLo by NBANDS ints (128 bytes), and OR FASTABS(yLo) into mOut.
	 *   - yHi: recompute the same t, multiply by the next *wp++, shift
	 *     left by 2, add s, store through ypHi, retreat ypHi by NBANDS
	 *     ints (128 bytes), and OR FASTABS(yHi) into mOut.
	 */
	__asm__ volatile (
		"\tmoveq #0,%0\n\t"
		"moveq #8,%%d7\n"
		"1:\n\t"
		/* xo = MULSHIFT32(*cp--, xp[9]); xe = *xp-- >> 2 */
		"move.l 36(%1),%%d1\n\t"
		"move.l (%2),%%d0\n\t"
		"subq.l #4,%2\n\t"
		"muls.l %%d0,%%d2:%%d1\n\t"
		"move.l (%1),%%d0\n\t"
		"subq.l #4,%1\n\t"
		"asr.l #2,%%d0\n\t"
		/* s = -*xPrev; d = xo - xe; *xPrev++ = xe + xo */
		"move.l (%4),%%d3\n\t"
		"neg.l %%d3\n\t"
		"move.l %%d0,%%d4\n\t"
		"add.l %%d2,%%d4\n\t"
		"move.l %%d4,(%4)+\n\t"
		"move.l %%d2,%%d5\n\t"
		"sub.l %%d0,%%d5\n\t"
		/* yLo = d + (MULSHIFT32(s - d, *wp++) << 2) */
		"move.l %%d3,%%d1\n\t"
		"sub.l %%d5,%%d1\n\t"
		"move.l (%3)+,%%d0\n\t"
		"muls.l %%d0,%%d4:%%d1\n\t"
		"lsl.l #2,%%d4\n\t"
		"add.l %%d5,%%d4\n\t"
		"move.l %%d4,(%5)\n\t"
		"adda.l #128,%5\n\t"
		/* 68k immediate shifts only encode counts 1..8; ADD/SUBX makes the sign mask. */
		"move.l %%d4,%%d0\n\t"
		"add.l %%d0,%%d0\n\t"
		"subx.l %%d0,%%d0\n\t"
		"eor.l %%d0,%%d4\n\t"
		"sub.l %%d0,%%d4\n\t"
		"or.l %%d4,%0\n\t"
		/* yHi = s + (MULSHIFT32(s - d, *wp++) << 2) */
		"move.l %%d3,%%d1\n\t"
		"sub.l %%d5,%%d1\n\t"
		"move.l (%3)+,%%d0\n\t"
		"muls.l %%d0,%%d4:%%d1\n\t"
		"lsl.l #2,%%d4\n\t"
		"add.l %%d3,%%d4\n\t"
		"move.l %%d4,(%6)\n\t"
		"suba.l #128,%6\n\t"
		/* 68k immediate shifts only encode counts 1..8; ADD/SUBX makes the sign mask. */
		"move.l %%d4,%%d0\n\t"
		"add.l %%d0,%%d0\n\t"
		"subx.l %%d0,%%d0\n\t"
		"eor.l %%d0,%%d4\n\t"
		"sub.l %%d0,%%d4\n\t"
		"or.l %%d4,%0\n\t"
		"dbra %%d7,1b"
		: "=&d" (mOut), "+a" (xp), "+a" (cp), "+a" (wp),
		  "+a" (xPrev), "+a" (ypLo), "+a" (ypHi)
		:
		: "d0", "d1", "d2", "d3", "d4", "d5", "d7", "cc", "memory");

	return mOut;
}

/*
 * Previously bailed out to the full IMDCT36_C_REFERENCE (redoing the
 * accumulation loop and BOTH idct9 calls in plain C) whenever btCurr != 0
 * || btPrev != 0 -- i.e. every block adjacent to a window-type transition
 * (start/stop/short), not just genuinely short blocks. That meant the
 * already-verified idct9_amiga_m68k_asm below got skipped for those
 * blocks too, not just the windowing step that actually differs.
 *
 * The accumulation loop and the two idct9 calls are IDENTICAL between the
 * btCurr==0&&btPrev==0 case and every other case -- only the final
 * windowing/overlap-add step differs (see IMDCT36_C_REFERENCE). So this
 * now always does the shared asm prep, then branches only the windowing:
 * IMDCT36_AMIGA_M68K_LONG_WINDOW (existing asm, symmetric-window fast
 * path) or IMDCT36_GeneralWindow (the C windowing loop factored out of
 * IMDCT36_C_REFERENCE's else branch, unchanged -- see that function's
 * comment). Verified via IMDCT36AsmGeneralPathSelftest() below to match
 * IMDCT36_C_REFERENCE bit-for-bit across randomized btCurr/btPrev/gb
 * combinations, including the btCurr==0&&btPrev==0 fast path (unchanged
 * from before this restructure).
 */
static int IMDCT36_AMIGA_M68K_ASM(int *xCurr, int *xPrev, int *y, int btCurr, int btPrev, int blockIdx, int gb)
{
	int i, es, xBuf[18];
	int acc1, acc2, mOut;
	int *xp, *ypLo, *ypHi;
	const int *cp, *wp;

	acc1 = acc2 = 0;
	xCurr += 17;
	if (gb < 7) {
		es = 7 - gb;
		for (i = 8; i >= 0; i--) {
			acc1 = ((*xCurr--) >> es) - acc1;
			acc2 = acc1 - acc2;
			acc1 = ((*xCurr--) >> es) - acc1;
			xBuf[i+9] = acc2;
			xBuf[i+0] = acc1;
			xPrev[i] >>= es;
		}
	} else {
		es = 0;
		for (i = 8; i >= 0; i--) {
			acc1 = (*xCurr--) - acc1;
			acc2 = acc1 - acc2;
			acc1 = (*xCurr--) - acc1;
			xBuf[i+9] = acc2;
			xBuf[i+0] = acc1;
		}
	}
	xBuf[9] >>= 1;
	xBuf[0] >>= 1;

	idct9_amiga_m68k_asm(xBuf+0);
	idct9_amiga_m68k_asm(xBuf+9);

	xp = xBuf + 8;
	cp = c18 + 8;
	ypLo = y;
	ypHi = y + 17*NBANDS;
	if (btCurr == 0 && btPrev == 0) {
		wp = fastWin36;
		mOut = IMDCT36_AMIGA_M68K_LONG_WINDOW(xp, cp, wp, xPrev, ypLo, ypHi);
	} else {
		mOut = IMDCT36_GeneralWindow(xp, cp, xPrev, btCurr, btPrev, ypLo, ypHi);
	}

	if (es)
		mOut |= FreqInvertRescale(y, xPrev, blockIdx, es);
	else if (blockIdx & 0x01)
		FreqInvertOdd(y);

	return mOut;
}
#endif

int IMDCT36_HAS_AMIGA_M68K_ASM_RUNTIME(void)
{
	return IMDCT36_HAS_AMIGA_M68K_ASM;
}

int IMDCT36_TEST_ACTIVE(int *xCurr, int *xPrev, int *y, int btCurr, int btPrev, int blockIdx, int gb)
{
#if IMDCT36_HAS_AMIGA_M68K_ASM
	return IMDCT36_AMIGA_M68K_ASM(xCurr, xPrev, y, btCurr, btPrev, blockIdx, gb);
#endif
	return IMDCT36_C_REFERENCE(xCurr, xPrev, y, btCurr, btPrev, blockIdx, gb);
}

static int IMDCT36(int *xCurr, int *xPrev, int *y, int btCurr, int btPrev, int blockIdx, int gb)
{
	return IMDCT36_TEST_ACTIVE(xCurr, xPrev, y, btCurr, btPrev, blockIdx, gb);
}

#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE) && defined(AMIGA_M68K_IMDCT_THIN_OUTPUT)
static __inline int IMDCTThinBlockSelected(const BlockCount *bc, int i)
{
	if (!bc->imdctThinActive)
		return 1;
	/* stride-4: polyphase reads every 4th subband starting at the phase offset */
	return ((i % bc->imdctThinStride) == (bc->imdctThinPhase % bc->imdctThinStride));
}

static int IMDCT36_ThinSkip(int *xCurr, int *xPrev, int gb)
{
	int i, es, xBuf[18];
	int acc1, acc2;
	int xo, xe, c, *xp;
	const int *cp;

	acc1 = acc2 = 0;
	xCurr += 17;

	/* 7 gb is always adequate for antialias + accumulator loop + idct9 */
	if (gb < 7) {
		/* preserve the reference guard-bit path's in-place xPrev scaling */
		es = 7 - gb;
		for (i = 8; i >= 0; i--) {
			acc1 = ((*xCurr--) >> es) - acc1;
			acc2 = acc1 - acc2;
			acc1 = ((*xCurr--) >> es) - acc1;
			xBuf[i+9] = acc2;	/* odd */
			xBuf[i+0] = acc1;	/* even */
			xPrev[i] >>= es;
		}
	} else {
		es = 0;
		(void)es;
		/* max gain = 18, assume adequate guard bits */
		for (i = 8; i >= 0; i--) {
			acc1 = (*xCurr--) - acc1;
			acc2 = acc1 - acc2;
			acc1 = (*xCurr--) - acc1;
			xBuf[i+9] = acc2;	/* odd */
			xBuf[i+0] = acc1;	/* even */
		}
	}
	/* xEven[0] and xOdd[0] scaled by 0.5 */
	xBuf[9] >>= 1;
	xBuf[0] >>= 1;

	/* always run both 9-point IDCTs so xPrev matches IMDCT36 */
	idct9(xBuf+0);	/* even */
	idct9(xBuf+9);	/* odd */

	xp = xBuf + 8;
	cp = c18 + 8;
	for (i = 9; i > 0; i--) {
		c = *cp--;	xo = *(xp + 9);		xe = *xp--;
		/* gain 2 int bits here */
		xo = MULSHIFT32(c, xo);			/* 2*c18*xOdd (mul by 2 implicit in scaling) */
		xe >>= 2;

		*xPrev++ = xe + xo;			/* symmetry - xPrev[i] = xPrev[17-i] for long blocks */
	}

	return 0;
}
#else
static __inline int IMDCTThinBlockSelected(const BlockCount *bc, int i)
{
	(void)bc; (void)i;
	return 1;
}
#endif

static int c3_0 = 0x6ed9eba1;	/* format = Q31, cos(pi/6) */
static int c6[3] = { 0x7ba3751d, 0x5a82799a, 0x2120fb83 };	/* format = Q31, cos(((0:2) + 0.5) * (pi/6)) */

/* 12-point inverse DCT, used in IMDCT12x3() 
 * 4 input guard bits will ensure no overflow
 */
static __inline void imdct12 (int *x, int *out)
{
	int a0, a1, a2;
	int x0, x1, x2, x3, x4, x5;

	x0 = *x;	x+=3;	x1 = *x;	x+=3;
	x2 = *x;	x+=3;	x3 = *x;	x+=3;
	x4 = *x;	x+=3;	x5 = *x;	x+=3;

	x4 -= x5;
	x3 -= x4;
	x2 -= x3;
	x3 -= x5;
	x1 -= x2;
	x0 -= x1;
	x1 -= x3;

	x0 >>= 1;
	x1 >>= 1;

	a0 = MULSHIFT32(c3_0, x2) << 1;
	a1 = x0 + (x4 >> 1);
	a2 = x0 - x4;
	x0 = a1 + a0;
	x2 = a2;
	x4 = a1 - a0;

	a0 = MULSHIFT32(c3_0, x3) << 1;
	a1 = x1 + (x5 >> 1);
	a2 = x1 - x5;

	/* cos window odd samples, mul by 2, eat sign bit */
	x1 = MULSHIFT32(c6[0], a1 + a0) << 2;			
	x3 = MULSHIFT32(c6[1], a2) << 2;
	x5 = MULSHIFT32(c6[2], a1 - a0) << 2;

	*out = x0 + x1;	out++;
	*out = x2 + x3;	out++;
	*out = x4 + x5;	out++;
	*out = x4 - x5;	out++;
	*out = x2 - x3;	out++;
	*out = x0 - x1;
}

/**************************************************************************************
 * Function:    IMDCT12x3
 *
 * Description: three 12-point modified DCT's for short blocks, with windowing,
 *                short block concatenation, and overlap-add
 *
 * Inputs:      3 interleaved vectors of 6 samples each 
 *                (block0[0], block1[0], block2[0], block0[1], block1[1]....)
 *              overlap part of last IMDCT (9 samples - see output comments)
 *              window type (0,1,2,3) of previous block
 *              current block index (for deciding whether to do frequency inversion)
 *              number of guard bits in input vector
 *
 * Outputs:     updated sample vector x, net gain of 1 integer bit
 *              second half of (unwindowed) IMDCT's - save for next time
 *                only save 9 xPrev samples, using symmetry (see WinPrevious())
 *
 * Return:      mOut (OR of abs(y) for all y calculated here)
 *
 * TODO:        optimize for ARM
 **************************************************************************************/
 // barely faster in RAM
static int IMDCT12x3(int *xCurr, int *xPrev, int *y, int btPrev, int blockIdx, int gb)
{
	int i, es, mOut, yLo, xBuf[18], xPrevWin[18];	/* need temp buffer for reordering short blocks */
	const int *wp;

	es = 0;
	/* 7 gb is always adequate for accumulator loop + idct12 + window + overlap */
	if (gb < 7) {
		es = 7 - gb;
		for (i = 0; i < 18; i+=2) {
			xCurr[i+0] >>= es;
			xCurr[i+1] >>= es;
			*xPrev++ >>= es;
		}
		xPrev -= 9;
	}

	/* requires 4 input guard bits for each imdct12 */
	imdct12(xCurr + 0, xBuf + 0);
	imdct12(xCurr + 1, xBuf + 6);
	imdct12(xCurr + 2, xBuf + 12);

	/* window previous from last time */
	WinPrevious(xPrev, xPrevWin, btPrev);

	/* could unroll this for speed, minimum loads (short blocks usually rare, so doesn't make much overall difference) 
	 * xPrevWin[i] << 2 still has 1 gb always, max gain of windowed xBuf stuff also < 1.0 and gain the sign bit
	 * so y calculations won't overflow
	 */
	wp = imdctWin[2];
	mOut = 0;
	for (i = 0; i < 3; i++) {
		yLo = (xPrevWin[ 0+i] << 2);
		mOut |= FASTABS(yLo);	y[( 0+i)*NBANDS] = yLo;
		yLo = (xPrevWin[ 3+i] << 2);
		mOut |= FASTABS(yLo);	y[( 3+i)*NBANDS] = yLo;
		yLo = (xPrevWin[ 6+i] << 2) + (MULSHIFT32(wp[0+i], xBuf[3+i]));	
		mOut |= FASTABS(yLo);	y[( 6+i)*NBANDS] = yLo;
		yLo = (xPrevWin[ 9+i] << 2) + (MULSHIFT32(wp[3+i], xBuf[5-i]));	
		mOut |= FASTABS(yLo);	y[( 9+i)*NBANDS] = yLo;
		yLo = (xPrevWin[12+i] << 2) + (MULSHIFT32(wp[6+i], xBuf[2-i]) + MULSHIFT32(wp[0+i], xBuf[(6+3)+i]));	
		mOut |= FASTABS(yLo);	y[(12+i)*NBANDS] = yLo;
		yLo = (xPrevWin[15+i] << 2) + (MULSHIFT32(wp[9+i], xBuf[0+i]) + MULSHIFT32(wp[3+i], xBuf[(6+5)-i]));	
		mOut |= FASTABS(yLo);	y[(15+i)*NBANDS] = yLo;
	}

	/* save previous (unwindowed) for overlap - only need samples 6-8, 12-17 */
	for (i = 6; i < 9; i++)
		*xPrev++ = xBuf[i] >> 2;
	for (i = 12; i < 18; i++)
		*xPrev++ = xBuf[i] >> 2;

	xPrev -= 9;
	if (es)
		mOut |= FreqInvertRescale(y, xPrev, blockIdx, es);
	else if (blockIdx & 0x01)
		FreqInvertOdd(y);

	return mOut;
}

/**************************************************************************************
 * Function:    HybridTransform
 *
 * Description: IMDCT's, windowing, and overlap-add on long/short/mixed blocks
 *
 * Inputs:      vector of input coefficients, length = nBlocksTotal * 18)
 *              vector of overlap samples from last time, length = nBlocksPrev * 9)
 *              buffer for output samples, length = MAXNSAMP
 *              SideInfoSub struct for this granule/channel
 *              BlockCount struct with necessary info
 *                number of non-zero input and overlap blocks
 *                number of long blocks in input vector (rest assumed to be short blocks)
 *                number of blocks which use long window (type) 0 in case of mixed block
 *                  (bc->currWinSwitch, 0 for non-mixed blocks)
 *
 * Outputs:     transformed, windowed, and overlapped sample buffer
 *              does frequency inversion on odd blocks
 *              updated buffer of samples for overlap
 *
 * Return:      number of non-zero IMDCT blocks calculated in this call
 *                (including overlap-add)
 *
 * TODO:        examine mixedBlock/winSwitch logic carefully (test he_mode.bit)
 **************************************************************************************/
static int HybridTransform(int *xCurr, int *xPrev, int y[BLOCK_SIZE][NBANDS], SideInfoSub *sis, BlockCount *bc)
{
	int xPrevWin[18], currWinIdx, prevWinIdx;
	int i, j, nBlocksOut, nonZero, mOut;
	int fiBit, xp;
	int blockType, mixedBlock, prevType, prevWinSwitch, currWinSwitch;

	ASSERT(bc->nBlocksLong  <= NBANDS);
	ASSERT(bc->nBlocksTotal <= NBANDS);
	ASSERT(bc->nBlocksPrev  <= NBANDS);

	mOut = 0;
	blockType = sis->blockType;
	mixedBlock = sis->mixedBlock;
	prevType = bc->prevType;
	prevWinSwitch = bc->prevWinSwitch;
	currWinSwitch = bc->currWinSwitch;

	/* do long blocks, if any */
	if (!mixedBlock && prevWinSwitch == 0) {
		currWinIdx = blockType;
		prevWinIdx = prevType;
		for(i = 0; i < bc->nBlocksLong; i++) {
			/* do 36-point IMDCT, including windowing and overlap-add */
#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE) && defined(AMIGA_M68K_IMDCT_THIN_OUTPUT)
			if (IMDCTThinBlockSelected(bc, i))
				mOut |= IMDCT36(xCurr, xPrev, &(y[0][i]), currWinIdx, prevWinIdx, i, bc->gbIn);
			else {
				int row;
				mOut |= IMDCT36_ThinSkip(xCurr, xPrev, bc->gbIn);
				for (row = 0; row < BLOCK_SIZE; row++)
					y[row][i] = 0;
			}
#else
			mOut |= IMDCT36(xCurr, xPrev, &(y[0][i]), currWinIdx, prevWinIdx, i, bc->gbIn);
#endif
			xCurr += 18;
			xPrev += 9;
		}
	} else {
		for(i = 0; i < bc->nBlocksLong; i++) {
			/* currWinIdx picks the right window for long blocks (if mixed, long blocks use window type 0) */
			currWinIdx = blockType;
			if (mixedBlock && i < currWinSwitch)
				currWinIdx = 0;

			prevWinIdx = prevType;
			if (i < prevWinSwitch)
				 prevWinIdx = 0;

			/* do 36-point IMDCT, including windowing and overlap-add.
			 * Mixed/transition-window long blocks now go through the same
			 * IMDCT36() dispatcher as the non-transition case above --
			 * IMDCT36_AMIGA_M68K_ASM shares the asm accumulation/idct9 stage
			 * for these blocks too and only branches C for the windowing step
			 * (see IMDCT36_AMIGA_M68K_ASM's header comment).
			 */
#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE) && defined(AMIGA_M68K_IMDCT_THIN_OUTPUT)
			if (IMDCTThinBlockSelected(bc, i))
				mOut |= IMDCT36(xCurr, xPrev, &(y[0][i]), currWinIdx, prevWinIdx, i, bc->gbIn);
			else {
				int row;
				mOut |= IMDCT36_ThinSkip(xCurr, xPrev, bc->gbIn);
				for (row = 0; row < BLOCK_SIZE; row++)
					y[row][i] = 0;
			}
#else
			mOut |= IMDCT36(xCurr, xPrev, &(y[0][i]), currWinIdx, prevWinIdx, i, bc->gbIn);
#endif
			xCurr += 18;
			xPrev += 9;
		}
	}

	/* do short blocks (if any) */
	for (   ; i < bc->nBlocksTotal; i++) {
		ASSERT(blockType == 2);

		prevWinIdx = prevType;
		if (i < prevWinSwitch)
			 prevWinIdx = 0;
		
		mOut |= IMDCT12x3(xCurr, xPrev, &(y[0][i]), prevWinIdx, i, bc->gbIn);
		xCurr += 18;
		xPrev += 9;
	}
	nBlocksOut = i;
	MP3AddDecodeCoreImdctBlockCounts((unsigned long)bc->nBlocksLong,
		(unsigned long)(nBlocksOut - bc->nBlocksLong));

	/* window and overlap prev if prev longer that current */
	for (   ; i < bc->nBlocksPrev; i++) {
		prevWinIdx = prevType;
		if (i < prevWinSwitch)
			 prevWinIdx = 0;
		WinPrevious(xPrev, xPrevWin, prevWinIdx);

		nonZero = 0;
		fiBit = i << 31;
		for (j = 0; j < 9; j++) {
			xp = xPrevWin[2*j+0] << 2;	/* << 2 temp for scaling */
			nonZero |= xp;
			y[2*j+0][i] = xp;
			mOut |= FASTABS(xp);

			/* frequency inversion on odd blocks/odd samples (flip sign if i odd, j odd) */
			xp = xPrevWin[2*j+1] << 2;
			xp = (xp ^ (fiBit >> 31)) + (i & 0x01);	
			nonZero |= xp;
			y[2*j+1][i] = xp;
			mOut |= FASTABS(xp);

			xPrev[j] = 0;
		}
		xPrev += 9;
		if (nonZero)
			nBlocksOut = i;
	}
	
	/* clear rest of blocks */
	for (   ; i < 32; i++) {
		int *yp;

		yp = &(y[0][i]);
		for (j = 18; j > 0; j--) {
			*yp = 0;
			yp += NBANDS;
		}
	}

	if (bc->subbandCapActive && bc->activeSubbands < NBANDS) {
		int row, band;
		for (row = 0; row < BLOCK_SIZE; row++)
			for (band = bc->activeSubbands; band < NBANDS; band++)
				y[row][band] = 0;
		MP3AddDecodeCoreIMDCTSubbands((unsigned long)nBlocksOut,
			(unsigned long)(NBANDS - bc->activeSubbands));
	} else {
		MP3AddDecodeCoreIMDCTSubbands((unsigned long)nBlocksOut, 0);
	}

	bc->gbOut = CLZ(mOut) - 1;

	return nBlocksOut;
}


static unsigned int IMDCTThinChecksumPCM(const int y[BLOCK_SIZE][NBANDS], int gb)
{
	int vbuf[MAX_NCHAN * VBUF_LENGTH];
	short pcm[BLOCK_SIZE * NBANDS];
	int tmp[NBANDS];
	int phase;
	int vindex;
	int out;
	int b;
	int i;
	unsigned int sum;

	memset(vbuf, 0, sizeof(vbuf));
	memset(pcm, 0, sizeof(pcm));
	phase = 0;
	vindex = 0;
	out = 0;
	for (b = 0; b < BLOCK_SIZE; b += 2) {
		memcpy(tmp, y[b], sizeof(tmp));
		FDCT32(tmp, vbuf, vindex, 0, gb);
		out += PolyphaseMonoFastLowrate(pcm + out, vbuf + vindex, polyCoef, 4, &phase);
		memcpy(tmp, y[b + 1], sizeof(tmp));
		FDCT32(tmp, vbuf, vindex, 1, gb);
		out += PolyphaseMonoFastLowrate(pcm + out, vbuf + vindex + VBUF_LENGTH, polyCoef, 4, &phase);
		vindex = (vindex - 1) & 7;
	}

	sum = 2166136261UL;
	for (i = 0; i < out; i++) {
		sum ^= (unsigned short)pcm[i];
		sum *= 16777619U;
	}
	return sum;
}

static int IMDCTCountHighSubbandNonZero(const int y[BLOCK_SIZE][NBANDS])
{
	int t;
	int sb;
	int count;

	count = 0;
	for (t = 0; t < BLOCK_SIZE; t++) {
		for (sb = 16; sb < NBANDS; sb++) {
			if (y[t][sb] != 0)
				count++;
		}
	}
	return count;
}

int IMDCTSubbandCapSelftest(void)
{
	int xUncap[MAX_NSAMP];
	int xCap[MAX_NSAMP];
	int prevUncap[MAX_NSAMP / 2];
	int prevCap[MAX_NSAMP / 2];
	int yUncap[BLOCK_SIZE][NBANDS];
	int yCap[BLOCK_SIZE][NBANDS];
	SideInfoSub sis;
	BlockCount baseBc;
	BlockCount uncapBc;
	BlockCount capBc;
	BlockCount helperBc;
	MP3DecInfo decInfo;
	int i;
	int outUncap;
	int outCap;
	int highNonZero;
	int helperApplied;
	unsigned int pcmUncap;
	unsigned int pcmCap;
	int failures;

	for (i = 0; i < MAX_NSAMP; i++) {
		xUncap[i] = ((i * 1103515245UL + 12345UL) & 0x001fffff) - 0x00100000;
		xCap[i] = xUncap[i];
	}
	for (i = 0; i < MAX_NSAMP / 2; i++) {
		prevUncap[i] = ((i * 69069UL + 1UL) & 0x0007ffff) - 0x00040000;
		prevCap[i] = prevUncap[i];
	}
	memset(yUncap, 0xa5, sizeof(yUncap));
	memset(yCap, 0xa5, sizeof(yCap));
	memset(&sis, 0, sizeof(sis));
	memset(&baseBc, 0, sizeof(baseBc));
	memset(&decInfo, 0, sizeof(decInfo));

	sis.blockType = 0;
	sis.mixedBlock = 0;
	baseBc.nBlocksLong = 32;
	baseBc.nBlocksTotal = 32;
	baseBc.nBlocksPrev = 32;
	baseBc.prevType = 0;
	baseBc.prevWinSwitch = 0;
	baseBc.currWinSwitch = 0;
	baseBc.gbIn = 8;
	uncapBc = baseBc;
	capBc = baseBc;
	capBc.nBlocksLong = 16;
	capBc.nBlocksTotal = 16;
	capBc.nBlocksPrev = 16;
	capBc.subbandCapActive = 1;

	outUncap = HybridTransform(xUncap, prevUncap, yUncap, &sis, &uncapBc);
	outCap = HybridTransform(xCap, prevCap, yCap, &sis, &capBc);
	pcmUncap = IMDCTThinChecksumPCM(yUncap, uncapBc.gbOut);
	pcmCap = IMDCTThinChecksumPCM(yCap, capBc.gbOut);
	highNonZero = IMDCTCountHighSubbandNonZero(yUncap);

	failures = 0;
	if (pcmUncap == pcmCap) {
		printf("subband cap selftest checksum did not change: uncapped=%lu capped=%lu\n",
			(unsigned long)pcmUncap, (unsigned long)pcmCap);
		failures++;
	}
	if (highNonZero == 0) {
		printf("subband cap selftest found no non-zero uncapped subband 16-31 samples\n");
		failures++;
	}
	if (outUncap <= outCap) {
		printf("subband cap selftest nBlocksOut unexpected: uncapped=%d capped=%d\n",
			outUncap, outCap);
		failures++;
	}

	decInfo.superfastLowrate = 1;
	decInfo.fastLowrateStride = 2;
	decInfo.fastLowrateActiveSubbands = 16;
	helperBc = baseBc;
	helperApplied = IMDCTApplySubbandCap(&decInfo, &helperBc);
	if (!helperApplied || helperBc.nBlocksTotal != 16 || helperBc.nBlocksLong != 16 ||
		helperBc.nBlocksPrev != 16 || helperBc.activeSubbands != 16 || !helperBc.subbandCapActive) {
		printf("subband cap selftest superfast stride-2 helper failed: applied=%d total=%d long=%d prev=%d activeBands=%d active=%d\n",
			helperApplied, helperBc.nBlocksTotal, helperBc.nBlocksLong, helperBc.nBlocksPrev,
			helperBc.activeSubbands, helperBc.subbandCapActive);
		failures++;
	}

	decInfo.fastLowrateStride = 4;
	decInfo.fastLowrateActiveSubbands = 8;
	decInfo.outputMono = 1;
	helperBc = baseBc;
	helperApplied = IMDCTApplySubbandCap(&decInfo, &helperBc);
	if (!helperApplied || helperBc.nBlocksTotal != 8 || helperBc.nBlocksLong != 8 ||
		helperBc.nBlocksPrev != 8 || helperBc.activeSubbands != 8 || !helperBc.subbandCapActive) {
		printf("subband cap selftest superfast stride-4 helper failed: applied=%d total=%d long=%d prev=%d activeBands=%d active=%d\n",
			helperApplied, helperBc.nBlocksTotal, helperBc.nBlocksLong, helperBc.nBlocksPrev,
			helperBc.activeSubbands, helperBc.subbandCapActive);
		failures++;
	}

	decInfo.fastLowrateStride = 5;
	decInfo.fastLowrateActiveSubbands = 6;
	decInfo.outputMono = 0;
	helperBc = baseBc;
	helperApplied = IMDCTApplySubbandCap(&decInfo, &helperBc);
	if (!helperApplied || helperBc.nBlocksTotal != 6 || helperBc.nBlocksLong != 6 ||
		helperBc.nBlocksPrev != 6 || helperBc.activeSubbands != 6 || !helperBc.subbandCapActive) {
		printf("subband cap selftest superfast stride-5 helper failed: applied=%d total=%d long=%d prev=%d activeBands=%d active=%d\n",
			helperApplied, helperBc.nBlocksTotal, helperBc.nBlocksLong, helperBc.nBlocksPrev,
			helperBc.activeSubbands, helperBc.subbandCapActive);
		failures++;
	}

	/*
	 * The GUI's manual "Subbands" cycle gadget lets users pick 26, 20, 16,
	 * 12, or 8 directly (see amiga_mp3gui.c's kSubbandCapValues) whenever
	 * Superfast is enabled, not just the two values (16, 8) that fast-lowrate
	 * itself derives automatically from stride. Prove the plumbing holds for
	 * every GUI-exposed value, not just the historically-tested ones.
	 */
	{
		static const int kManualCapCases[] = { 26, 20, 12 };
		int manualIdx;

		for (manualIdx = 0; manualIdx < (int)(sizeof(kManualCapCases) / sizeof(kManualCapCases[0])); manualIdx++) {
			int cap = kManualCapCases[manualIdx];

			decInfo.superfastLowrate = 1;
			decInfo.fastLowrateStride = 2;
			decInfo.fastLowrateActiveSubbands = cap;
			decInfo.outputMono = 1;
			helperBc = baseBc;
			helperApplied = IMDCTApplySubbandCap(&decInfo, &helperBc);
			if (!helperApplied || helperBc.nBlocksTotal != cap || helperBc.nBlocksLong != cap ||
				helperBc.nBlocksPrev != cap || helperBc.activeSubbands != cap || !helperBc.subbandCapActive) {
				printf("subband cap selftest manual-cap helper failed: cap=%d applied=%d total=%d long=%d prev=%d activeBands=%d active=%d\n",
					cap, helperApplied, helperBc.nBlocksTotal, helperBc.nBlocksLong, helperBc.nBlocksPrev,
					helperBc.activeSubbands, helperBc.subbandCapActive);
				failures++;
			}
		}
	}

	memset(&decInfo, 0, sizeof(decInfo));
	decInfo.fastLowrateStride = 4;
	decInfo.outputMono = 1;
	helperBc = baseBc;
	helperApplied = IMDCTApplySubbandCap(&decInfo, &helperBc);
#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE) && defined(AMIGA_FAST_SUBBAND_CAP)
	if (!helperApplied || helperBc.nBlocksTotal != 16 || helperBc.nBlocksLong != 16 ||
		helperBc.nBlocksPrev != 16 || !helperBc.subbandCapActive) {
		printf("subband cap selftest stride-4 helper failed: applied=%d total=%d long=%d prev=%d active=%d\n",
			helperApplied, helperBc.nBlocksTotal, helperBc.nBlocksLong, helperBc.nBlocksPrev,
			helperBc.subbandCapActive);
		failures++;
	}
#else
	if (helperApplied || helperBc.nBlocksTotal != 32 || helperBc.nBlocksLong != 32 ||
		helperBc.nBlocksPrev != 32) {
		printf("subband cap selftest compile-gated helper changed state while unavailable\n");
		failures++;
	}
#endif

	decInfo.fastLowrateStride = 1;
	decInfo.outputMono = 1;
	helperBc = baseBc;
	helperApplied = IMDCTApplySubbandCap(&decInfo, &helperBc);
	if (helperApplied || helperBc.nBlocksTotal != 32 || helperBc.nBlocksLong != 32 ||
		helperBc.nBlocksPrev != 32 || helperBc.subbandCapActive) {
		printf("subband cap selftest full-rate helper unexpectedly applied: applied=%d total=%d long=%d prev=%d active=%d\n",
			helperApplied, helperBc.nBlocksTotal, helperBc.nBlocksLong, helperBc.nBlocksPrev,
			helperBc.subbandCapActive);
		failures++;
	}

	printf("subband cap compile gate: %s\n",
#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE) && defined(AMIGA_FAST_SUBBAND_CAP)
		"available"
#else
		"unavailable"
#endif
	);
	printf("subband cap uncapped PCM checksum: %lu\n", (unsigned long)pcmUncap);
	printf("subband cap capped PCM checksum: %lu\n", (unsigned long)pcmCap);
	printf("subband cap uncapped non-zero y samples in subbands 16-31: %d\n", highNonZero);
	printf("subband cap full-rate stride-1 helper: not applied\n");
	printf("subband cap selftest: %s\n", failures ? "FAIL" : "PASS");
	return failures ? -1 : 0;
}

/*
 * Proves the IMDCT()'s antialiasNBfly capping is an exact simplification:
 * for a variety of (uncapped nBfly, activeSubbands) pairs, running
 * AntiAlias() with the full nBfly vs. with it capped to activeSubbands
 * must produce bit-identical output for every sample belonging to a KEPT
 * block (indices 0 .. activeSubbands*18 - 1). Anything at or beyond that
 * is either the discarded region (allowed to differ -- it gets zeroed by
 * the subband cap right after) or out of the buffer entirely.
 */
int AntiAliasSubbandCapSelftest(void)
{
	static const int kActiveSubbandsCases[] = { 26, 20, 16, 12, 8, 6, 3, 1 };
	int uncapped[MAX_NSAMP];
	int capped[MAX_NSAMP];
	int nBfly;
	int caseIdx;
	int i;
	int failures;

	failures = 0;
	for (nBfly = 1; nBfly <= 31; nBfly++) {
		int bufLen = (nBfly + 1) * 18;
		if (bufLen > MAX_NSAMP)
			bufLen = MAX_NSAMP;

		for (i = 0; i < bufLen; i++) {
			/* Deterministic pseudo-random fill, well within AntiAlias's
			 * documented "assume at least 1 guard bit" precondition. */
			int v = (int)(((unsigned int)(i * 2654435761U + (unsigned int)nBfly * 40503U)) & 0x000fffffU) - 0x00080000;
			uncapped[i] = v;
			capped[i] = v;
		}

		for (caseIdx = 0; caseIdx < (int)(sizeof(kActiveSubbandsCases) / sizeof(kActiveSubbandsCases[0])); caseIdx++) {
			int activeSubbands = kActiveSubbandsCases[caseIdx];
			int cappedNBfly = nBfly;
			int keptSamples;
			int j;

			if (cappedNBfly > activeSubbands)
				cappedNBfly = activeSubbands;
			keptSamples = activeSubbands * 18;
			if (keptSamples > bufLen)
				continue;	/* this nBfly doesn't even reach the cap; nothing to prove here */

			for (i = 0; i < bufLen; i++) {
				uncapped[i] = capped[i];	/* reset from the case above */
			}
			AntiAlias_C_REFERENCE(uncapped, nBfly);
			AntiAlias_C_REFERENCE(capped, cappedNBfly);

			for (j = 0; j < keptSamples; j++) {
				if (uncapped[j] != capped[j]) {
					printf("antialias subband cap selftest MISMATCH nBfly=%d cappedNBfly=%d activeSubbands=%d sample=%d uncapped=%d capped=%d\n",
						nBfly, cappedNBfly, activeSubbands, j, uncapped[j], capped[j]);
					failures++;
					break;
				}
			}
		}
	}

	printf("antialias subband cap selftest: %s\n", failures ? "FAIL" : "PASS");
	return failures ? -1 : 0;
}

/*
 * Verifies the IMDCT36_AMIGA_M68K_ASM restructure described at that
 * function's definition: it now always runs the shared xBuf accumulation +
 * idct9_amiga_m68k_asm stage and branches only at the windowing step
 * (IMDCT36_AMIGA_M68K_LONG_WINDOW vs IMDCT36_GeneralWindow), instead of
 * bailing out to the full C reference whenever btCurr != 0 || btPrev != 0.
 * Compares against IMDCT36_C_REFERENCE bit-for-bit (output y[], updated
 * xPrev[], and mOut) across randomized btCurr/btPrev/blockIdx/gb and input
 * combinations, including the btCurr==0&&btPrev==0 fast path (unchanged by
 * this restructure) so a regression there would also be caught here.
 */
int IMDCT36AsmGeneralPathSelftest(void)
{
#if IMDCT36_HAS_AMIGA_M68K_ASM
	int xCurrRef[18], xCurrAsm[18];
	int xPrevRef[9], xPrevAsm[9];
	int yRef[18][NBANDS], yAsm[18][NBANDS];
	unsigned int rngState = 0x9e3779b9U;
	int trial, btCurr, btPrev, gb, blockIdx, i;
	int mOutRef, mOutAsm;
	int failures = 0;
	int total = 0;

	for (trial = 0; trial < 20000; trial++) {
		rngState ^= rngState << 13; rngState ^= rngState >> 17; rngState ^= rngState << 5;
		btCurr = rngState % 4;
		rngState ^= rngState << 13; rngState ^= rngState >> 17; rngState ^= rngState << 5;
		btPrev = rngState % 4;
		rngState ^= rngState << 13; rngState ^= rngState >> 17; rngState ^= rngState << 5;
		blockIdx = rngState % 2;
		rngState ^= rngState << 13; rngState ^= rngState >> 17; rngState ^= rngState << 5;
		gb = rngState % 10;

		for (i = 0; i < 18; i++) {
			int v;
			rngState ^= rngState << 13; rngState ^= rngState >> 17; rngState ^= rngState << 5;
			v = (int)(rngState % 0x08000000U) - 0x04000000;
			xCurrRef[i] = v; xCurrAsm[i] = v;
		}
		for (i = 0; i < 9; i++) {
			int v;
			rngState ^= rngState << 13; rngState ^= rngState >> 17; rngState ^= rngState << 5;
			v = (int)(rngState % 0x08000000U) - 0x04000000;
			xPrevRef[i] = v; xPrevAsm[i] = v;
		}
		memset(yRef, 0, sizeof(yRef));
		memset(yAsm, 0, sizeof(yAsm));

		mOutRef = IMDCT36_C_REFERENCE(xCurrRef, xPrevRef, &yRef[0][0], btCurr, btPrev, blockIdx, gb);
		mOutAsm = IMDCT36_AMIGA_M68K_ASM(xCurrAsm, xPrevAsm, &yAsm[0][0], btCurr, btPrev, blockIdx, gb);
		total++;

		if (mOutRef != mOutAsm || memcmp(yRef, yAsm, sizeof(yRef)) != 0 ||
			memcmp(xPrevRef, xPrevAsm, sizeof(xPrevRef)) != 0) {
			printf("imdct36 asm general-path selftest MISMATCH trial=%d btCurr=%d btPrev=%d blockIdx=%d gb=%d mOutRef=%d mOutAsm=%d\n",
				trial, btCurr, btPrev, blockIdx, gb, mOutRef, mOutAsm);
			failures++;
		}
	}

	printf("imdct36 asm general-path selftest: %d trials, %s\n", total, failures ? "FAIL" : "PASS");
	return failures ? -1 : 0;
#else
	printf("imdct36 asm general-path selftest: SKIPPED (no m68k asm on this target)\n");
	return 0;
#endif
}

int IMDCTThinOutputSelftest(void)
{
	int xFull[MAX_NSAMP];
	int xThin[MAX_NSAMP];
	int prevFull[MAX_NSAMP / 2];
	int prevThin[MAX_NSAMP / 2];
	int yFull[BLOCK_SIZE][NBANDS];
	int yThin[BLOCK_SIZE][NBANDS];
	int yExpected[BLOCK_SIZE][NBANDS];
	SideInfoSub sis;
	BlockCount fullBc;
	BlockCount thinBc;
	int i;
	int outFull;
	int outThin;
	unsigned int pcmFull;
	unsigned int pcmThin;
	unsigned int pcmExpected;
	int failures;

	for (i = 0; i < MAX_NSAMP; i++) {
		xFull[i] = ((i * 1103515245UL + 12345UL) & 0x001fffff) - 0x00100000;
		xThin[i] = xFull[i];
	}
	for (i = 0; i < MAX_NSAMP / 2; i++) {
		prevFull[i] = ((i * 69069UL + 1UL) & 0x0007ffff) - 0x00040000;
		prevThin[i] = prevFull[i];
	}
	memset(yFull, 0xa5, sizeof(yFull));
	memset(yThin, 0xa5, sizeof(yThin));
	memset(yExpected, 0, sizeof(yExpected));
	memset(&sis, 0, sizeof(sis));
	memset(&fullBc, 0, sizeof(fullBc));
	memset(&thinBc, 0, sizeof(thinBc));

	sis.blockType = 0;
	sis.mixedBlock = 0;
	fullBc.nBlocksLong = 32;
	fullBc.nBlocksTotal = 32;
	fullBc.nBlocksPrev = 32;
	fullBc.prevType = 0;
	fullBc.prevWinSwitch = 0;
	fullBc.currWinSwitch = 0;
	fullBc.gbIn = 8;
	thinBc = fullBc;
	thinBc.imdctThinActive = 1;
	thinBc.imdctThinStride = 4;
	thinBc.imdctThinPhase = 0;

	outFull = HybridTransform(xFull, prevFull, yFull, &sis, &fullBc);
	outThin = HybridTransform(xThin, prevThin, yThin, &sis, &thinBc);
	for (i = 0; i < NBANDS; i++) {
		int t;
		if ((i % thinBc.imdctThinStride) == thinBc.imdctThinPhase)
			for (t = 0; t < BLOCK_SIZE; t++)
				yExpected[t][i] = yFull[t][i];
	}
	pcmFull = IMDCTThinChecksumPCM(yFull, fullBc.gbOut);
	pcmThin = IMDCTThinChecksumPCM(yThin, thinBc.gbOut);
	pcmExpected = IMDCTThinChecksumPCM(yExpected, fullBc.gbOut);

	failures = 0;
	if (outFull != outThin) {
		printf("IMDCT thin nBlocksOut mismatch: full=%d thin=%d\n", outFull, outThin);
		failures++;
	}
	if (memcmp(prevFull, prevThin, sizeof(prevFull)) != 0) {
		printf("IMDCT thin xPrev mismatch\n");
		failures++;
	}
	if (memcmp(yExpected, yThin, sizeof(yExpected)) != 0) {
		int t, sb;
		for (t = 0; t < BLOCK_SIZE; t++) {
			for (sb = 0; sb < NBANDS; sb++) {
				if (yExpected[t][sb] != yThin[t][sb]) {
					printf("IMDCT thin sparse y[] mismatch: index=%d subband=%d time=%d blockType=%d stride=%d full=%ld thin=%ld\n",
						t * NBANDS + sb, sb, t, sis.blockType, thinBc.imdctThinStride,
						(long)yExpected[t][sb], (long)yThin[t][sb]);
					t = BLOCK_SIZE;
					break;
				}
			}
		}
		failures++;
	}
	if (pcmThin != pcmExpected) {
		printf("IMDCT thin deterministic sparse PCM checksum mismatch: expected=%lu thin=%lu full=%lu\n",
			(unsigned long)pcmExpected, (unsigned long)pcmThin, (unsigned long)pcmFull);
		failures++;
	}

	printf("IMDCT thin compile gate: %s\n",
#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE) && defined(AMIGA_M68K_IMDCT_THIN_OUTPUT)
		"available"
#else
		"unavailable"
#endif
	);
	printf("IMDCT thin runtime activation: %s\n",
#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE) && defined(AMIGA_M68K_IMDCT_THIN_OUTPUT)
		"enabled for --exp-imdct-thin stride-4 mono fast-lowrate"
#else
		"disabled (compile gate unavailable)"
#endif
	);
	printf("IMDCT thin full PCM checksum: %lu\n", (unsigned long)pcmFull);
	printf("IMDCT thin deterministic sparse PCM checksum: %lu\n", (unsigned long)pcmExpected);
	printf("IMDCT thin selftest failures: %d\n", failures);
	return failures ? -1 : 0;
}

/**************************************************************************************
 * Function:    IMDCT
 *
 * Description: do alias reduction, inverse MDCT, overlap-add, and frequency inversion
 *
 * Inputs:      MP3DecInfo structure filled by UnpackFrameHeader(), UnpackSideInfo(),
 *                UnpackScaleFactors(), and DecodeHuffman() (for this granule, channel)
 *                includes PCM samples in overBuf (from last call to IMDCT) for OLA
 *              index of current granule and channel
 *
 * Outputs:     PCM samples in outBuf, for input to subband transform
 *              PCM samples in overBuf, for OLA next time
 *              updated hi->nonZeroBound index for this channel
 *
 * Return:      0 on success,  -1 if null input pointers
 **************************************************************************************/
 // a bit faster in RAM
int IMDCT(MP3DecInfo *mp3DecInfo, int gr, int ch)
{
	int nBfly, blockCutoff;
	int antialiasNBfly;
	int activeSubbandsForAntiAlias;
	FrameHeader *fh;
	SideInfo *si;
	HuffmanInfo *hi;
	IMDCTInfo *mi;
	BlockCount bc;

	/* validate pointers */
	if (!mp3DecInfo || !mp3DecInfo->FrameHeaderPS || !mp3DecInfo->SideInfoPS || 
		!mp3DecInfo->HuffmanInfoPS || !mp3DecInfo->IMDCTInfoPS)
		return -1;

	/* si is an array of up to 4 structs, stored as gr0ch0, gr0ch1, gr1ch0, gr1ch1 */
	fh = (FrameHeader *)(mp3DecInfo->FrameHeaderPS);
	si = (SideInfo *)(mp3DecInfo->SideInfoPS);
	hi = (HuffmanInfo*)(mp3DecInfo->HuffmanInfoPS);
	mi = (IMDCTInfo *)(mp3DecInfo->IMDCTInfoPS);

	/* anti-aliasing done on whole long blocks only
	 * for mixed blocks, nBfly always 1, except 3 for 8 kHz MPEG 2.5 (see sfBandTab) 
     *   nLongBlocks = number of blocks with (possibly) non-zero power 
	 *   nBfly = number of butterflies to do (nLongBlocks - 1, unless no long blocks)
	 */
	blockCutoff = fh->sfBand->l[(fh->ver == MPEG1 ? 8 : 6)] / 18;	/* same as 3* num short sfb's in spec */
	if (si->sis[gr][ch].blockType != 2) {
		/* all long transforms */
		bc.nBlocksLong = MIN((hi->nonZeroBound[ch] + 7) / 18 + 1, 32);	
		nBfly = bc.nBlocksLong - 1;
	} else if (si->sis[gr][ch].blockType == 2 && si->sis[gr][ch].mixedBlock) {
		/* mixed block - long transforms until cutoff, then short transforms */
		bc.nBlocksLong = blockCutoff;	
		nBfly = bc.nBlocksLong - 1;
	} else {
		/* all short transforms */
		bc.nBlocksLong = 0;
		nBfly = 0;
	}
 
	/*
	 * AntiAlias()'s inter-block butterflies are each strictly local to one
	 * block boundary (see AntiAlias_C_REFERENCE: butterfly k only reads/
	 * writes the 8 samples on either side of that one boundary, with no
	 * carry between iterations), so this is an *exact* simplification, not
	 * a new approximation on top of the existing subband cap: bands below
	 * the cap come out bit-identical whether or not the butterflies for the
	 * fully-discarded region run. The one butterfly that must still run is
	 * the boundary between the last KEPT block and the first discarded one
	 * -- it also touches the kept block's high edge -- which is why this
	 * caps at activeSubbands (the boundary count for activeSubbands kept
	 * blocks), not activeSubbands - 1. See AntiAliasSubbandCapSelftest()
	 * for a randomized proof that kept-band output is unaffected.
	 * This only ever fires for fast-lowrate stride>=2 decodes -- full-rate
	 * decode always gets activeSubbandsForAntiAlias == NBANDS here, so
	 * antialiasNBfly == nBfly and this is a no-op for the primary path.
	 */
	activeSubbandsForAntiAlias = MP3FastLowrateEffectiveActiveSubbands(mp3DecInfo);
	antialiasNBfly = nBfly;
	if (activeSubbandsForAntiAlias < NBANDS && antialiasNBfly > activeSubbandsForAntiAlias)
		antialiasNBfly = activeSubbandsForAntiAlias;

	AntiAlias(hi->huffDecBuf[ch], antialiasNBfly);
#ifdef MINTAMP_SKIN
    if (skin_eq_apply(hi->huffDecBuf[ch],MAX_NSAMP,mp3DecInfo->samprate)) {
        int k,mask=0;
        for (k=0;k<MAX_NSAMP;++k) mask|=FASTABS(hi->huffDecBuf[ch][k]);
        hi->gb[ch]=CLZ(mask)-1;
    }
#endif
	hi->nonZeroBound[ch] = MAX(hi->nonZeroBound[ch], (nBfly * 18) + 8);

	ASSERT(hi->nonZeroBound[ch] <= MAX_NSAMP);

	/* for readability, use a struct instead of passing a million parameters to HybridTransform() */
	bc.nBlocksTotal = (hi->nonZeroBound[ch] + 17) / 18;
	bc.nBlocksPrev = mi->numPrevIMDCT[ch];
	IMDCTApplySubbandCap(mp3DecInfo, &bc);
	bc.prevType = mi->prevType[ch];
	bc.prevWinSwitch = mi->prevWinSwitch[ch];
	bc.currWinSwitch = (si->sis[gr][ch].mixedBlock ? blockCutoff : 0);	/* where WINDOW switches (not nec. transform) */
	bc.gbIn = hi->gb[ch];
	bc.imdctThinStride = mp3DecInfo->fastLowrateStride;
	bc.imdctThinPhase = mp3DecInfo->fastLowratePhase;
	bc.imdctThinActive = IMDCTThinOutputCanActivate(mp3DecInfo);
	mp3DecInfo->imdctThinActive = bc.imdctThinActive;

	mi->numPrevIMDCT[ch] = HybridTransform(hi->huffDecBuf[ch], mi->overBuf[ch], mi->outBuf[ch], &si->sis[gr][ch], &bc);
	if (bc.subbandCapActive) {
		IMDCTClearDiscardedSubbands(mi, ch, bc.activeSubbands);
		if (mi->numPrevIMDCT[ch] > bc.activeSubbands)
			mi->numPrevIMDCT[ch] = bc.activeSubbands;
	}
	mi->prevType[ch] = si->sis[gr][ch].blockType;
	mi->prevWinSwitch[ch] = bc.currWinSwitch;		/* 0 means not a mixed block (either all short or all long) */
	mi->gb[ch] = bc.gbOut;

	ASSERT(mi->numPrevIMDCT[ch] <= NBANDS);

	/* output has gained 2 int bits */
	return 0;
}
