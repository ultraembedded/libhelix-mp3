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
 * August 2003
 *
 * dqchan.c - dequantization of transform coefficients
 **************************************************************************************/

#include "coder.h"
#include "assembly.h"
#include <stdio.h>
#include <string.h>

typedef int ARRAY3[3];	/* for short-block reordering */

#if defined(AMIGA_M68K_ASM) && defined(__GNUC__) && \
	(defined(__mc68000__) || defined(__mc68020__) || defined(__mc68030__) || \
	 defined(__mc68040__) || defined(__mc68060__) || defined(mc68000) || \
	 defined(mc68020))
#define REORDER_SHORT_BLOCK_HAS_AMIGA_M68K_ASM 1
#else
#define REORDER_SHORT_BLOCK_HAS_AMIGA_M68K_ASM 0
#endif

static __inline void ReorderShortBlock(ARRAY3 *buf, int *workBuf, int nSamps)
{
#if REORDER_SHORT_BLOCK_HAS_AMIGA_M68K_ASM
	int *src0;
	int *src1;
	int *src2;
	int *dst;
	unsigned int count;

	if (nSamps <= 0)
		return;

	src0 = workBuf;
	src1 = workBuf + nSamps;
	src2 = workBuf + 2*nSamps;
	dst = (int *)buf;
	count = (unsigned int)(nSamps - 1);

	__asm__ volatile (
		"1:\n\t"
		"move.l (%[src0])+,(%[dst])+\n\t"
		"move.l (%[src1])+,(%[dst])+\n\t"
		"move.l (%[src2])+,(%[dst])+\n\t"
		"dbf %[count],1b"
		: [src0] "+a" (src0),
		  [src1] "+a" (src1),
		  [src2] "+a" (src2),
		  [dst] "+a" (dst),
		  [count] "+d" (count)
		:
		: "memory", "cc");
#else
	int j;

	for (j = 0; j < nSamps; j++) {
		buf[j][0] = workBuf[0*nSamps + j];
		buf[j][1] = workBuf[1*nSamps + j];
		buf[j][2] = workBuf[2*nSamps + j];
	}
#endif
}

/* optional pre-emphasis for high-frequency scale factor bands */
static const char preTab[22] = { 0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,2,2,3,3,3,2,0 };

/* pow(2,-i/4) for i=0..3, Q31 format */
int pow14[4] = { 
	0x7fffffff, 0x6ba27e65, 0x5a82799a, 0x4c1bf829
};

/* pow(2,-i/4) * pow(j,4/3) for i=0..3 j=0..15, Q25 format */
int pow43_14[4][16] = {
{	0x00000000, 0x10000000, 0x285145f3, 0x453a5cdb, /* Q28 */
	0x0cb2ff53, 0x111989d6, 0x15ce31c8, 0x1ac7f203, 
	0x20000000, 0x257106b9, 0x2b16b4a3, 0x30ed74b4, 
	0x36f23fa5, 0x3d227bd3, 0x437be656, 0x49fc823c, },

{	0x00000000, 0x0d744fcd, 0x21e71f26, 0x3a36abd9, 
	0x0aadc084, 0x0e610e6e, 0x12560c1d, 0x168523cf, 
	0x1ae89f99, 0x1f7c03a4, 0x243bae49, 0x29249c67, 
	0x2e34420f, 0x33686f85, 0x38bf3dff, 0x3e370182, },

{	0x00000000, 0x0b504f33, 0x1c823e07, 0x30f39a55, 
	0x08facd62, 0x0c176319, 0x0f6b3522, 0x12efe2ad, 
	0x16a09e66, 0x1a79a317, 0x1e77e301, 0x2298d5b4, 
	0x26da56fc, 0x2b3a902a, 0x2fb7e7e7, 0x3450f650, },

{	0x00000000, 0x09837f05, 0x17f910d7, 0x2929c7a9, 
	0x078d0dfa, 0x0a2ae661, 0x0cf73154, 0x0fec91cb, 
	0x1306fe0a, 0x16434a6c, 0x199ee595, 0x1d17ae3d, 
	0x20abd76a, 0x2459d551, 0x28204fbb, 0x2bfe1808, },
};

/* pow(j,4/3) for j=16..63, Q23 format */
int pow43[] = {
	0x1428a2fa, 0x15db1bd6, 0x1796302c, 0x19598d85, 
	0x1b24e8bb, 0x1cf7fcfa, 0x1ed28af2, 0x20b4582a, 
	0x229d2e6e, 0x248cdb55, 0x26832fda, 0x28800000, 
	0x2a832287, 0x2c8c70a8, 0x2e9bc5d8, 0x30b0ff99, 
	0x32cbfd4a, 0x34eca001, 0x3712ca62, 0x393e6088, 
	0x3b6f47e0, 0x3da56717, 0x3fe0a5fc, 0x4220ed72, 
	0x44662758, 0x46b03e7c, 0x48ff1e87, 0x4b52b3f3, 
	0x4daaebfd, 0x5007b497, 0x5268fc62, 0x54ceb29c, 
	0x5738c721, 0x59a72a59, 0x5c19cd35, 0x5e90a129, 
	0x610b9821, 0x638aa47f, 0x660db90f, 0x6894c90b, 
	0x6b1fc80c, 0x6daeaa0d, 0x70416360, 0x72d7e8b0, 
	0x75722ef9, 0x78102b85, 0x7ab1d3ec, 0x7d571e09, 
};

/* sqrt(0.5) in Q31 format */
#define SQRTHALF 0x5a82799a

/*
 * Minimax polynomial approximation to pow(x, 4/3), over the range
 *  poly43lo: x = [0.5, 0.7071]
 *  poly43hi: x = [0.7071, 1.0]
 *
 * Relative error < 1E-7
 * Coefs are scaled by 4, 2, 1, 0.5, 0.25
 */
int poly43lo[5] = { 0x29a0bda9, 0xb02e4828, 0x5957aa1b, 0x236c498d, 0xff581859 };
int poly43hi[5] = { 0x10852163, 0xd333f6a4, 0x46e9408b, 0x27c2cef0, 0xfef577b4 };

/* pow(2, i*4/3) as exp and frac */
int pow2exp[8]  = { 14, 13, 11, 10, 9, 7, 6, 5 };

int pow2frac[8] = {
	0x6597fa94, 0x50a28be6, 0x7fffffff, 0x6597fa94, 
	0x50a28be6, 0x7fffffff, 0x6597fa94, 0x50a28be6
};

/**************************************************************************************
 * Function:    DequantBlock
 *
 * Description: Ken's highly-optimized, low memory dequantizer performing the operation
 *              y = pow(x, 4.0/3.0) * pow(2, 25 - scale/4.0)
 *
 * Inputs:      input buffer of decode Huffman codewords (signed-magnitude)
 *              output buffer of same length (in-place (outbuf = inbuf) is allowed)
 *              number of samples
 *              
 * Outputs:     dequantized samples in Q25 format
 *
 * Return:      bitwise-OR of the unsigned outputs (for guard bit calculations)
 **************************************************************************************/
#if defined(AMIGA_M68K) && defined(AMIGA_M68K_ASM_DEQUANT) && defined(__GNUC__) && \
	(defined(__mc68020__) || defined(__mc68030__) || defined(__mc68040__) || \
	 defined(__mc68060__) || defined(mc68020))
#define DEQUANTBLOCK_HAS_AMIGA_M68K_ASM 1
#else
#define DEQUANTBLOCK_HAS_AMIGA_M68K_ASM 0
#endif

static __inline int DequantBlock_MULSHIFT32_C_REFERENCE(int x, int y)
{
#if defined(AMIGA_M68K) && defined(AMIGA_M68K_POLYPHASE_68060)
	/* Same value without a ___muldi3 call per dequantised line on the 060. */
	return MULSHIFT32_68060(x, y);
#else
	return (int)(((long long)x * (long long)y) >> 32);
#endif
}

int DequantBlock_C_REFERENCE(int *inbuf, int *outbuf, int num, int scale)
{
	int tab4[4];
	int scalef, scalei, shift;
	int sx, x, y;
	int mask = 0;
	const int *tab16, *coef;

	tab16 = pow43_14[scale & 0x3];
	scalef = pow14[scale & 0x3];
	scalei = MIN(scale >> 2, 31);	/* smallest input scale = -47, so smallest scalei = -12 */

	/* cache first 4 values */
	shift = MIN(scalei + 3, 31);
	shift = MAX(shift, 0);
	tab4[0] = 0;
	tab4[1] = tab16[1] >> shift;
	tab4[2] = tab16[2] >> shift;
	tab4[3] = tab16[3] >> shift;

	do {

		sx = *inbuf++;
		x = sx & 0x7fffffff;	/* sx = sign|mag */

		/* Zero is the dominant sparse-spectrum case; avoid scale/sign work. */
		if (!x) {
			*outbuf++ = 0;
			continue;
		}

		if (x < 4) {

			y = tab4[x];

		} else if (x < 16) {

			y = tab16[x];
			/* scalei can legitimately reach -12 (see comment above: "smallest
			 * input scale = -47, so smallest scalei = -12"), and tab16[]
			 * entries are large enough that left-shifting by that much can
			 * overflow into the sign bit. The x>=64 branch below already
			 * clips its analogous shift<0 case; this branch didn't, so it
			 * could silently emit a negative "dequantized" magnitude for an
			 * ordinary small Huffman coefficient. */
			if (scalei < 0) {
				shift = -scalei;
				if (y > (0x7fffffff >> shift))
					y = 0x7fffffff;
				else
					y <<= shift;
			} else {
				y >>= scalei;
			}

		} else {

			if (x < 64) {

				y = pow43[x-16];

				/* fractional scale */
				y = DequantBlock_MULSHIFT32_C_REFERENCE(y, scalef);
				shift = scalei - 3;

			} else {

				/* normalize to [0x40000000, 0x7fffffff] */
				x <<= 17;
				shift = 0;
				if (x < 0x08000000)
					x <<= 4, shift += 4;
				if (x < 0x20000000)
					x <<= 2, shift += 2;
				if (x < 0x40000000)
					x <<= 1, shift += 1;

				coef = (x < SQRTHALF) ? poly43lo : poly43hi;

				/* polynomial */
				y = coef[0];
				y = DequantBlock_MULSHIFT32_C_REFERENCE(y, x) + coef[1];
				y = DequantBlock_MULSHIFT32_C_REFERENCE(y, x) + coef[2];
				y = DequantBlock_MULSHIFT32_C_REFERENCE(y, x) + coef[3];
				y = DequantBlock_MULSHIFT32_C_REFERENCE(y, x) + coef[4];
				y = DequantBlock_MULSHIFT32_C_REFERENCE(y, pow2frac[shift]) << 3;

				/* fractional scale */
				y = DequantBlock_MULSHIFT32_C_REFERENCE(y, scalef);
				shift = scalei - pow2exp[shift];
			}

			/* integer scale */
			if (shift < 0) {
				shift = -shift;
				if (y > (0x7fffffff >> shift))
					y = 0x7fffffff;		/* clip */
				else
					y <<= shift;
			} else {
				y >>= shift;
			}
		}

		/* sign and store */
		mask |= y;
		*outbuf++ = (sx < 0) ? -y : y;

	} while (--num);

	return mask;
}

#if DEQUANTBLOCK_HAS_AMIGA_M68K_ASM
static __inline void DequantBlock_AmigaM68K_ReadInput(int **pinbuf, int *sx, int *x)
{
	int *p;
	int sxReg;
	int xReg;

	p = *pinbuf;
	__asm__ volatile (
		"move.l (%[p])+,%[sx]\n\t"
		"move.l %[sx],%[x]\n\t"
		"bclr #31,%[x]"
		: [p] "+a" (p), [sx] "=&d" (sxReg), [x] "=&d" (xReg)
		:
		: "cc", "memory");
	*pinbuf = p;
	*sx = sxReg;
	*x = xReg;
}

static __inline void DequantBlock_AmigaM68K_WriteOutput(int **poutbuf, int y)
{
	int *p;

	p = *poutbuf;
	__asm__ volatile (
		"move.l %[y],(%[p])+"
		: [p] "+a" (p)
		: [y] "d" (y)
		: "memory");
	*poutbuf = p;
}

static __inline int DequantBlock_AmigaM68K_ApplySign(int y, int sx)
{
	return (sx < 0) ? -y : y;
}

static int DequantBlock_AmigaM68KAsm(int *inbuf, int *outbuf, int num, int scale)
{
	int tab4[4];
	int scalef, scalei, shift;
	int sx, x, y;
	int mask = 0;
	const int *tab16, *coef;
	register const int *tab4Reg __asm__("a2");
	register int scalefReg __asm__("d5");
	register int scaleiReg __asm__("d6");

	tab16 = pow43_14[scale & 0x3];
	scalef = pow14[scale & 0x3];
	scalei = MIN(scale >> 2, 31);

	/* Cache first 4 values pre-shifted, matching the C reference hot path. */
	shift = MIN(scalei + 3, 31);
	shift = MAX(shift, 0);
	tab4[0] = 0;
	tab4[1] = tab16[1] >> shift;
	tab4[2] = tab16[2] >> shift;
	tab4[3] = tab16[3] >> shift;
	tab4Reg = tab4;
	scalefReg = scalef;
	scaleiReg = scalei;

	do {

		/* Read signed-magnitude input exactly once using post-increment.
		 * Never write back through inbuf; dequantization permits in-place
		 * output only via outbuf stores below.
		 */
		DequantBlock_AmigaM68K_ReadInput(&inbuf, &sx, &x);

		/* Zero is the dominant sparse-spectrum case; avoid scale/sign work. */
		if (!x) {
			DequantBlock_AmigaM68K_WriteOutput(&outbuf, 0);
			continue;
		}

		if (x < 4) {
			/* 68020+ indexed load from the pre-shifted small-coefficient table. */
			__asm__ volatile (
				"move.l (%[tab],%[idx].l*4),%[out]\n\t"
				"or.l %[out],%[mask]"
				: [out] "=&d" (y), [mask] "+d" (mask)
				: [tab] "a" (tab4Reg), [idx] "d" (x)
				: "cc");
			DequantBlock_AmigaM68K_WriteOutput(&outbuf,
				DequantBlock_AmigaM68K_ApplySign(y, sx));
			continue;

		} else if (x < 16) {

			y = tab16[x];
			/* See the matching comment in DequantBlock_C_REFERENCE: scaleiReg
			 * can legitimately reach -12, and this left-shift was missing the
			 * clip its x>=64-branch sibling already has. */
			if (scaleiReg < 0) {
				shift = -scaleiReg;
				if (y > (0x7fffffff >> shift))
					y = 0x7fffffff;
				else
					y <<= shift;
			} else {
				y >>= scaleiReg;
			}

		} else {

			if (x < 64) {

				y = pow43[x-16];

				/* fractional scale */
				y = MULSHIFT32(y, scalefReg);
				shift = scaleiReg - 3;

			} else {

				/* normalize to [0x40000000, 0x7fffffff] */
				x <<= 17;
				shift = 0;
				if (x < 0x08000000)
					x <<= 4, shift += 4;
				if (x < 0x20000000)
					x <<= 2, shift += 2;
				if (x < 0x40000000)
					x <<= 1, shift += 1;

				coef = (x < SQRTHALF) ? poly43lo : poly43hi;

				/* polynomial and fractional scale in one block, so GCC cannot spill between muls.l ops. */
				{
					int hi, lo;
					__asm__ volatile (
						"move.l (%[coef]),%[out]\n\t"
						"move.l %[x],%[lo]\n\t"
						"muls.l %[out],%[hi]:%[lo]\n\t"
						"move.l %[hi],%[out]\n\t"
						"add.l 4(%[coef]),%[out]\n\t"
						"move.l %[x],%[lo]\n\t"
						"muls.l %[out],%[hi]:%[lo]\n\t"
						"move.l %[hi],%[out]\n\t"
						"add.l 8(%[coef]),%[out]\n\t"
						"move.l %[x],%[lo]\n\t"
						"muls.l %[out],%[hi]:%[lo]\n\t"
						"move.l %[hi],%[out]\n\t"
						"add.l 12(%[coef]),%[out]\n\t"
						"move.l %[x],%[lo]\n\t"
						"muls.l %[out],%[hi]:%[lo]\n\t"
						"move.l %[hi],%[out]\n\t"
						"add.l 16(%[coef]),%[out]\n\t"
						"move.l %[powfrac],%[lo]\n\t"
						"muls.l %[out],%[hi]:%[lo]\n\t"
						"move.l %[hi],%[out]\n\t"
						"lsl.l #3,%[out]\n\t"
						"move.l %[scalef],%[lo]\n\t"
						"muls.l %[out],%[hi]:%[lo]\n\t"
						"move.l %[hi],%[out]"
						: [out] "=&d" (y), [hi] "=&d" (hi), [lo] "=&d" (lo)
						: [x] "d" (x), [coef] "a" (coef),
						  [powfrac] "d" (pow2frac[shift]), [scalef] "d" (scalefReg)
						: "cc");
				}
				shift = scaleiReg - pow2exp[shift];
			}

			/* integer scale */
			if (shift < 0) {
				shift = -shift;
				if (y > (0x7fffffff >> shift))
					y = 0x7fffffff;		/* clip */
				else
					y <<= shift;
			} else {
				y >>= shift;
			}
		}

		/* sign and store */
		mask |= y;
		DequantBlock_AmigaM68K_WriteOutput(&outbuf,
			DequantBlock_AmigaM68K_ApplySign(y, sx));

	} while (--num);

	return mask;
}
#endif

int DequantBlock_TEST_ACTIVE(int *inbuf, int *outbuf, int num, int scale)
{
#if DEQUANTBLOCK_HAS_AMIGA_M68K_ASM
	return DequantBlock_AmigaM68KAsm(inbuf, outbuf, num, scale);
#else
	return DequantBlock_C_REFERENCE(inbuf, outbuf, num, scale);
#endif
}

int DequantBlock_HAS_AMIGA_M68K_ASM_RUNTIME(void)
{
	return DEQUANTBLOCK_HAS_AMIGA_M68K_ASM;
}

static int DequantBlock(int *inbuf, int *outbuf, int num, int scale)
{
	return DequantBlock_TEST_ACTIVE(inbuf, outbuf, num, scale);
}

/**************************************************************************************
 * Function:    DequantChannel
 *
 * Description: dequantize one granule, one channel worth of decoded Huffman codewords
 *
 * Inputs:      sample buffer (decoded Huffman codewords), length = MAX_NSAMP samples
 *              work buffer for reordering short-block, length = MAX_REORDER_SAMPS
 *                samples (3 * width of largest short-block critical band)
 *              non-zero bound for this channel/granule
 *              valid FrameHeader, SideInfoSub, ScaleFactorInfoSub, and CriticalBandInfo
 *                structures for this channel/granule
 *
 * Outputs:     MAX_NSAMP dequantized samples in sampleBuf
 *              updated non-zero bound (indicating which samples are != 0 after DQ)
 *              filled-in cbi structure indicating start and end critical bands
 *
 * Return:      minimum number of guard bits in dequantized sampleBuf
 *
 * Notes:       dequantized samples in Q(DQ_FRACBITS_OUT) format
 *
 *              subbandCapSampleLimit: sample-index cutoff (activeSubbands*18,
 *              matching MP3FastLowrateEffectiveActiveSubbands()'s convention;
 *              <= 0 means "no cap active") beyond which the fast-lowrate
 *              IMDCT subband cap guarantees this content is never read
 *              downstream (bc->nBlocksLong is capped before HybridTransform's
 *              loop even starts -- see IMDCTApplySubbandCap()). Long-block
 *              critical bands entirely beyond this cutoff skip the expensive
 *              per-coefficient DequantBlock() call; see the loop below for
 *              how cbMax[0] (-> cbi->cbEndL, which IntensityProcMPEG1/2 use
 *              as the intensity-stereo start boundary) and gbMask (->
 *              guard-bit count, this function's return value) still get
 *              tracked exactly.
 **************************************************************************************/
int DequantChannel(int *sampleBuf, int *workBuf, int *nonZeroBound, FrameHeader *fh, SideInfoSub *sis,
					ScaleFactorInfoSub *sfis, CriticalBandInfo *cbi, int subbandCapSampleLimit)
{
	int i, w, cb;
	int cbEndL, cbStartS, cbEndS;
	int nSamps, nonZero, sfactMultiplier, gbMask;
	int globalGain, gainI;
	int cbMax[3];
	ARRAY3 *buf;    /* short block reorder */
	
	/* set default start/end points for short/long blocks - will update with non-zero cb info */
	if (sis->blockType == 2) {
		if (sis->mixedBlock) { 
			cbEndL = (fh->ver == MPEG1 ? 8 : 6); 
			cbStartS = 3; 
		} else {
			cbEndL = 0; 
			cbStartS = 0;
		}
		cbEndS = 13;
	} else {
		/* long block */
		cbEndL =   22;
		cbStartS = 13;
		cbEndS =   13;
	}
	cbMax[2] = cbMax[1] = cbMax[0] = 0;
	gbMask = 0;
	i = 0;

	/* sfactScale = 0 --> quantizer step size = 2
	 * sfactScale = 1 --> quantizer step size = sqrt(2)
	 *   so sfactMultiplier = 2 or 4 (jump through globalGain by powers of 2 or sqrt(2))
	 */
	sfactMultiplier = 2 * (sis->sfactScale + 1);

	/* offset globalGain by -2 if midSide enabled, for 1/sqrt(2) used in MidSideProc()
	 *  (DequantBlock() does 0.25 * gainI so knocking it down by two is the same as 
	 *   dividing every sample by sqrt(2) = multiplying by 2^-.5)
	 */
	globalGain = sis->globalGain;
	if (fh->modeExt >> 1)
		 globalGain -= 2;
	globalGain += IMDCT_SCALE;		/* scale everything by sqrt(2), for fast IMDCT36 */

	/* long blocks */
	for (cb = 0; cb < cbEndL; cb++) {

		nSamps = fh->sfBand->l[cb + 1] - fh->sfBand->l[cb];
		gainI = 210 - globalGain + sfactMultiplier * (sfis->l[cb] + (sis->preFlag ? (int)preTab[cb] : 0));

		if (subbandCapSampleLimit > 0 && i >= subbandCapSampleLimit) {
			/* Entirely beyond the fast-lowrate subband cap: skip the
			 * expensive per-coefficient dequant, but still scan the raw
			 * (undequantized) magnitudes so cbMax[0] tracking stays exact --
			 * nonzero-ness is identical whether checked before or after
			 * dequantization (DequantBlock never turns a nonzero magnitude
			 * into a zero output or vice versa). gbMask safety is preserved
			 * via a single DequantBlock() call on the band's peak raw
			 * magnitude: y = pow(x,4/3)*scale is monotonic in x for a fixed
			 * scale, so dequantizing the peak is a guaranteed upper bound on
			 * every coefficient's true y in this band, which can only ever
			 * *reduce* (never increase) the reported guard-bit count
			 * relative to full computation -- the safety direction that
			 * matters for IMDCT's downstream guard-bit-dependent scaling.
			 * sampleBuf[i..i+nSamps) is intentionally left un-dequantized;
			 * it's never read once the subband cap is active. */
			int k, xAbs, xPeak, yPeakDiscard;

			nonZero = 0;
			xPeak = 0;
			for (k = 0; k < nSamps; k++) {
				xAbs = sampleBuf[i + k] & 0x7fffffff;
				if (xAbs) {
					nonZero = 1;
					if (xAbs > xPeak)
						xPeak = xAbs;
				}
			}
			if (xPeak) {
				/* Return value (mask) is what we actually need; outbuf must
				 * still point somewhere valid, so it writes the same value
				 * into a local we don't otherwise use. */
				gbMask |= DequantBlock(&xPeak, &yPeakDiscard, 1, gainI);
			}
		} else {
			nonZero = DequantBlock(sampleBuf + i, sampleBuf + i, nSamps, gainI);
			gbMask |= nonZero;
		}

		i += nSamps;

		/* update highest non-zero critical band */
		if (nonZero)
			cbMax[0] = cb;

		if (i >= *nonZeroBound)
			break;
	}

	/* set cbi (Type, EndS[], EndSMax will be overwritten if we proceed to do short blocks) */
	cbi->cbType = 0;			/* long only */
	cbi->cbEndL  = cbMax[0];
	cbi->cbEndS[0] = cbi->cbEndS[1] = cbi->cbEndS[2] = 0;
	cbi->cbEndSMax = 0;

	/* early exit if no short blocks */
	if (cbStartS >= 12) 
		return CLZ(gbMask) - 1;
	
	/* short blocks */
	cbMax[2] = cbMax[1] = cbMax[0] = cbStartS;
	for (cb = cbStartS; cb < cbEndS; cb++) {

		nSamps = fh->sfBand->s[cb + 1] - fh->sfBand->s[cb];
		for (w = 0; w < 3; w++) {
			nonZero =  0;
			gainI = 210 - globalGain + 8*sis->subBlockGain[w] + sfactMultiplier*(sfis->s[cb][w]);

			nonZero |= DequantBlock(sampleBuf + i + nSamps*w, workBuf + nSamps*w, nSamps, gainI);

			/* update highest non-zero critical band */
			if (nonZero)
				cbMax[w] = cb;
			gbMask |= nonZero;
		}

		/* reorder blocks */
		buf = (ARRAY3 *)(sampleBuf + i);
		i += 3*nSamps;
		ReorderShortBlock(buf, workBuf, nSamps);

		ASSERT(3*nSamps <= MAX_REORDER_SAMPS);

		if (i >= *nonZeroBound) 
			break;
	}

	/* i = last non-zero INPUT sample processed, which corresponds to highest possible non-zero 
	 *     OUTPUT sample (after reorder)
	 * however, the original nzb is no longer necessarily true
	 *   for each cb, buf[][] is updated with 3*nSamps samples (i increases 3*nSamps each time)
	 *   (buf[j + 1][0] = 3 (input) samples ahead of buf[j][0])
     * so update nonZeroBound to i
	 */
	*nonZeroBound = i;

	ASSERT(*nonZeroBound <= MAX_NSAMP);

	cbi->cbType = (sis->mixedBlock ? 2 : 1);	/* 2 = mixed short/long, 1 = short only */

	cbi->cbEndS[0] = cbMax[0];
	cbi->cbEndS[1] = cbMax[1];
	cbi->cbEndS[2] = cbMax[2];

	cbi->cbEndSMax = cbMax[0];
	cbi->cbEndSMax = MAX(cbi->cbEndSMax, cbMax[1]);
	cbi->cbEndSMax = MAX(cbi->cbEndSMax, cbMax[2]);

	return CLZ(gbMask) - 1;
}

/*
 * Verifies the DequantChannel() subbandCapSampleLimit long-block skip
 * against the uncapped computation, calling the real production function
 * (not a host stand-in) with subbandCapSampleLimit=0 (uncapped) and every
 * active-subbands value that can actually reach this function: the four
 * values fast-lowrate derives automatically from stride (16 -> stride 2,
 * 10 -> stride 3, 8 -> stride 4, 6 -> stride 5), which apply in both plain
 * fast-lowrate and superfast mode since MP3FastLowrateEffectiveActiveSubbands()
 * uses the same stride-derived default either way, plus the manual overrides
 * the GUI's "Subbands" cycle gadget exposes whenever Superfast is enabled
 * (26, 20, 12 -- see amiga_mp3gui.c's kSubbandCapValues, which
 * MP3SetSubbandCap() can only ever narrow further). Checks, per the same
 * reasoning in DequantChannel()'s comment:
 *   1. kept-region samples (index < cutoff) bit-identical
 *   2. cbi->cbEndL bit-identical regardless of whether it falls above or
 *      below the cutoff (intensity stereo's cbStartL = cbi[1].cbEndL + 1
 *      boundary depends on this staying exact)
 *   3. return value (guard-bit count) identical -- proven exact (not just
 *      safe-but-conservative) once DequantBlock's x<16 branch overflow was
 *      fixed (see that fix's commit message); this selftest would catch a
 *      regression of either fix.
 * Long blocks only (mirrors the optimization's scope); short/mixed blocks
 * are untouched by subbandCapSampleLimit and not exercised here.
 */
int DequantSubbandCapSelftest(void)
{
	static const int kActiveSubbandsCases[] = { 26, 20, 16, 12, 10, 8, 6 };
	unsigned int rngState = 0x5EED1234U;
	int trial, caseIdx, failures, total;

	failures = 0;
	total = 0;

	for (trial = 0; trial < 20000; trial++) {
		int sampleBufRef[MAX_NSAMP], sampleBufCap[MAX_NSAMP];
		int workBufRef[MAX_REORDER_SAMPS], workBufCap[MAX_REORDER_SAMPS];
		FrameHeader fh;
		SideInfoSub sis;
		ScaleFactorInfoSub sfis;
		CriticalBandInfo cbiRef, cbiCap;
		int nonZeroBoundRef, nonZeroBoundCap, nonZeroBoundIn;
		int gbRef, gbCap;
		int i, cb, mismatch;

		for (i = 0; i < MAX_NSAMP; i++) {
			unsigned int r;
			int mag, roll;

			rngState ^= rngState << 13; rngState ^= rngState >> 17; rngState ^= rngState << 5;
			r = rngState;
			roll = (int)(r % 10);
			if (roll < 4) mag = 0;
			else if (roll < 7) mag = (int)(r % 4);
			else if (roll < 9) mag = (int)(r % 60) + 4;
			else mag = (int)(r % 8000) + 64;

			rngState ^= rngState << 13; rngState ^= rngState >> 17; rngState ^= rngState << 5;
			sampleBufRef[i] = (int)(((rngState & 1U) << 31) | (unsigned int)mag);
			sampleBufCap[i] = sampleBufRef[i];
		}
		memset(workBufRef, 0, sizeof(workBufRef));
		memset(workBufCap, 0, sizeof(workBufCap));

		memset(&fh, 0, sizeof(fh));
		fh.ver = MPEG1;
		fh.sfBand = &sfBandTable[MPEG1][0];

		memset(&sis, 0, sizeof(sis));
		sis.blockType = 0;		/* long block only -- matches this optimization's scope */
		sis.mixedBlock = 0;

		rngState ^= rngState << 13; rngState ^= rngState >> 17; rngState ^= rngState << 5;
		sis.globalGain = (int)(rngState % 256);
		rngState ^= rngState << 13; rngState ^= rngState >> 17; rngState ^= rngState << 5;
		sis.sfactScale = (int)(rngState & 1);
		rngState ^= rngState << 13; rngState ^= rngState >> 17; rngState ^= rngState << 5;
		sis.preFlag = (int)(rngState & 1);

		memset(&sfis, 0, sizeof(sfis));
		for (cb = 0; cb < 22; cb++) {
			rngState ^= rngState << 13; rngState ^= rngState >> 17; rngState ^= rngState << 5;
			sfis.l[cb] = (char)(rngState % 32);
		}

		rngState ^= rngState << 13; rngState ^= rngState >> 17; rngState ^= rngState << 5;
		nonZeroBoundIn = (rngState % 3 == 0) ? (int)(rngState % 576) : 576;
		if (nonZeroBoundIn <= 0) nonZeroBoundIn = 576;

		memset(&cbiRef, 0, sizeof(cbiRef));
		memset(&cbiCap, 0, sizeof(cbiCap));
		nonZeroBoundRef = nonZeroBoundIn;
		nonZeroBoundCap = nonZeroBoundIn;

		gbRef = DequantChannel(sampleBufRef, workBufRef, &nonZeroBoundRef, &fh, &sis, &sfis, &cbiRef, 0);

		caseIdx = trial % (int)(sizeof(kActiveSubbandsCases) / sizeof(kActiveSubbandsCases[0]));
		gbCap = DequantChannel(sampleBufCap, workBufCap, &nonZeroBoundCap, &fh, &sis, &sfis, &cbiCap,
			kActiveSubbandsCases[caseIdx] * 18);
		total++;

		mismatch = 0;
		{
			int keptLimit = kActiveSubbandsCases[caseIdx] * 18;
			if (keptLimit > nonZeroBoundRef) keptLimit = nonZeroBoundRef;
			for (i = 0; i < keptLimit; i++) {
				if (sampleBufRef[i] != sampleBufCap[i]) {
					printf("dequant subband cap selftest MISMATCH trial=%d kept sample=%d ref=%d cap=%d activeSubbands=%d\n",
						trial, i, sampleBufRef[i], sampleBufCap[i], kActiveSubbandsCases[caseIdx]);
					mismatch = 1;
					break;
				}
			}
		}
		if (cbiRef.cbEndL != cbiCap.cbEndL) {
			printf("dequant subband cap selftest MISMATCH trial=%d cbEndL ref=%d cap=%d activeSubbands=%d\n",
				trial, cbiRef.cbEndL, cbiCap.cbEndL, kActiveSubbandsCases[caseIdx]);
			mismatch = 1;
		}
		if (gbRef != gbCap) {
			printf("dequant subband cap selftest MISMATCH trial=%d gb ref=%d cap=%d activeSubbands=%d\n",
				trial, gbRef, gbCap, kActiveSubbandsCases[caseIdx]);
			mismatch = 1;
		}
		if (mismatch)
			failures++;
	}

	printf("dequant subband cap selftest: %d trials, %s\n", total, failures ? "FAIL" : "PASS");
	return failures ? -1 : 0;
}
