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
 * polyphase.c - final stage of subband transform (polyphase synthesis filter)
 *
 * This is the C reference version using __int64
 * Look in the appropriate subdirectories for optimized asm implementations 
 *   (e.g. arm/asmpoly.s)
 **************************************************************************************/

#include "coder.h"
#include "assembly.h"

/* input to Polyphase = Q(DQ_FRACBITS_OUT-2), gain 2 bits in convolution
 *  we also have the implicit bias of 2^15 to add back, so net fraction bits = 
 *    DQ_FRACBITS_OUT - 2 - 2 - 15
 *  (see comment on Dequantize() for more info)
 */
#define DEF_NFRACBITS	(DQ_FRACBITS_OUT - 2 - 2 - 15)	
#define CSHIFT	12	/* coefficients have 12 leading sign bits for early-terminating mulitplies */

#if defined(__GNUC__) && defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE)
#define POLYPHASE_REF_UNUSED __attribute__((unused))
#else
#define POLYPHASE_REF_UNUSED
#endif

#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE)
static int gExperimentalPolyphaseEnabled;
static int gForceStereoStride2C;
static unsigned long gStereoStride2AsmCalls;
static unsigned long gStereoStride2CCalls;
static unsigned long gStereoStride2ReducedCalls;
static unsigned long gMonoStride2AsmCalls;
static unsigned long gMonoStride2CCalls;
static unsigned long gMonoStride2ReducedCalls;
static unsigned long gStereoStride4AsmCalls;
static unsigned long gStereoStride4CCalls;
static unsigned long gStereoStride4ReducedCalls;
#if defined(AMIGA_FAST_REDUCED_TAPS)
static int gExperimentalReducedTapsEnabled;
#endif
#if defined(AMIGA_FAST_FDCT32_QUARTER)
static int gExperimentalFDCT32QuarterEnabled;
#endif

void MP3SetExperimentalPolyphase(int enabled)
{
	gExperimentalPolyphaseEnabled = enabled ? 1 : 0;
}

int MP3ExperimentalPolyphaseEnabled(void)
{
	return gExperimentalPolyphaseEnabled;
}

void MP3SetForceStereoStride2PolyphaseC(int enabled)
{
	gForceStereoStride2C = enabled ? 1 : 0;
}

int MP3ForceStereoStride2PolyphaseC(void)
{
	return gForceStereoStride2C;
}

void MP3GetStereoStride2PolyphaseCounters(unsigned long *asmCalls, unsigned long *cCalls,
	unsigned long *reducedCalls)
{
	if (asmCalls)
		*asmCalls = gStereoStride2AsmCalls;
	if (cCalls)
		*cCalls = gStereoStride2CCalls;
	if (reducedCalls)
		*reducedCalls = gStereoStride2ReducedCalls;
}

void MP3ResetStereoStride2PolyphaseCounters(void)
{
	gStereoStride2AsmCalls = 0;
	gStereoStride2CCalls = 0;
	gStereoStride2ReducedCalls = 0;
}

void MP3GetMonoStride2PolyphaseCounters(unsigned long *asmCalls, unsigned long *cCalls,
	unsigned long *reducedCalls)
{
	if (asmCalls)
		*asmCalls = gMonoStride2AsmCalls;
	if (cCalls)
		*cCalls = gMonoStride2CCalls;
	if (reducedCalls)
		*reducedCalls = gMonoStride2ReducedCalls;
}

void MP3ResetMonoStride2PolyphaseCounters(void)
{
	gMonoStride2AsmCalls = 0;
	gMonoStride2CCalls = 0;
	gMonoStride2ReducedCalls = 0;
}

void MP3GetStereoStride4PolyphaseCounters(unsigned long *asmCalls, unsigned long *cCalls,
	unsigned long *reducedCalls)
{
	if (asmCalls)
		*asmCalls = gStereoStride4AsmCalls;
	if (cCalls)
		*cCalls = gStereoStride4CCalls;
	if (reducedCalls)
		*reducedCalls = gStereoStride4ReducedCalls;
}

void MP3ResetStereoStride4PolyphaseCounters(void)
{
	gStereoStride4AsmCalls = 0;
	gStereoStride4CCalls = 0;
	gStereoStride4ReducedCalls = 0;
}

void MP3SetExperimentalReducedTaps(int enabled)
{
#if defined(AMIGA_FAST_REDUCED_TAPS)
	gExperimentalReducedTapsEnabled = enabled ? 1 : 0;
#else
	(void)enabled;
#endif
}

int MP3ExperimentalReducedTapsEnabled(void)
{
#if defined(AMIGA_FAST_REDUCED_TAPS)
	return gExperimentalReducedTapsEnabled;
#else
	return 0;
#endif
}

void MP3SetExperimentalFDCT32Quarter(int enabled)
{
#if defined(AMIGA_FAST_FDCT32_QUARTER)
	gExperimentalFDCT32QuarterEnabled = enabled ? 1 : 0;
#else
	(void)enabled;
#endif
}

int MP3ExperimentalFDCT32QuarterEnabled(void)
{
#if defined(AMIGA_FAST_FDCT32_QUARTER)
	return gExperimentalFDCT32QuarterEnabled;
#else
	return 0;
#endif
}

void AmigaResetPolyphaseStatics(void)
{
	/* The synthesis FIFO lives in SubbandInfo and is freshly allocated by
	 * MP3InitDecoder().  Reset the file-scope runtime controls too so an
	 * in-process GUI playback starts from the same polyphase state each time.
	 */
	gExperimentalPolyphaseEnabled = 0;
	gForceStereoStride2C = 0;
	MP3ResetStereoStride2PolyphaseCounters();
	MP3ResetStereoStride4PolyphaseCounters();
	MP3ResetMonoStride2PolyphaseCounters();
#if defined(AMIGA_FAST_REDUCED_TAPS)
	gExperimentalReducedTapsEnabled = 0;
#endif
#if defined(AMIGA_FAST_FDCT32_QUARTER)
	gExperimentalFDCT32QuarterEnabled = 0;
#endif
}
#else
void MP3SetExperimentalPolyphase(int enabled)
{
	(void)enabled;
}

int MP3ExperimentalPolyphaseEnabled(void)
{
	return 0;
}

void MP3SetForceStereoStride2PolyphaseC(int enabled)
{
	(void)enabled;
}

int MP3ForceStereoStride2PolyphaseC(void)
{
	return 0;
}

void MP3GetStereoStride2PolyphaseCounters(unsigned long *asmCalls, unsigned long *cCalls,
	unsigned long *reducedCalls)
{
	if (asmCalls)
		*asmCalls = 0;
	if (cCalls)
		*cCalls = 0;
	if (reducedCalls)
		*reducedCalls = 0;
}

void MP3ResetStereoStride2PolyphaseCounters(void)
{
}

void MP3GetMonoStride2PolyphaseCounters(unsigned long *asmCalls, unsigned long *cCalls,
	unsigned long *reducedCalls)
{
	if (asmCalls)
		*asmCalls = 0;
	if (cCalls)
		*cCalls = 0;
	if (reducedCalls)
		*reducedCalls = 0;
}

void MP3ResetMonoStride2PolyphaseCounters(void)
{
}

void MP3GetStereoStride4PolyphaseCounters(unsigned long *asmCalls, unsigned long *cCalls,
	unsigned long *reducedCalls)
{
	if (asmCalls)
		*asmCalls = 0;
	if (cCalls)
		*cCalls = 0;
	if (reducedCalls)
		*reducedCalls = 0;
}

void MP3ResetStereoStride4PolyphaseCounters(void)
{
}

void MP3SetExperimentalReducedTaps(int enabled)
{
	(void)enabled;
}

int MP3ExperimentalReducedTapsEnabled(void)
{
	return 0;
}

void MP3SetExperimentalFDCT32Quarter(int enabled)
{
	(void)enabled;
}

int MP3ExperimentalFDCT32QuarterEnabled(void)
{
	return 0;
}

void AmigaResetPolyphaseStatics(void)
{
}
#endif

#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE) && defined(AMIGA_M68K_ASM_POLYPHASE)
/*
 * Keep this a weak reference so command lines that define
 * AMIGA_M68K_ASM_POLYPHASE but still use the real C sources without the
 * optional .S file
 * link cleanly and fall back to the existing C fast path.
 */
extern void AmigaM68KPolyphaseMonoFast(short *pcm, int *vbuf,
	const int *coefBase) __asm__("AmigaM68KPolyphaseMonoFast")
	__attribute__((weak));
extern void AmigaM68KPolyphaseMonoFastStride2(short *pcm, int *vbuf,
	const int *coefBase) __asm__("AmigaM68KPolyphaseMonoFastStride2")
	__attribute__((weak));
extern void AmigaM68KPolyphaseMonoFastStride2Reduced(short *pcm, int *vbuf,
	const int *coefBase) __asm__("AmigaM68KPolyphaseMonoFastStride2Reduced")
	__attribute__((weak));
extern void MonoFastPolyphaseStride4_Amiga_m68k(short *pcm, int *vbuf,
	const int *coefBase) __asm__("MonoFastPolyphaseStride4_Amiga_m68k")
	__attribute__((weak));
extern void MonoFastPolyphaseStride4Phase1_Amiga_m68k(short *pcm, int *vbuf,
	const int *coefBase) __asm__("MonoFastPolyphaseStride4Phase1_Amiga_m68k")
	__attribute__((weak));
extern void MonoFastPolyphaseStride4Phase2_Amiga_m68k(short *pcm, int *vbuf,
	const int *coefBase) __asm__("MonoFastPolyphaseStride4Phase2_Amiga_m68k")
	__attribute__((weak));
extern void MonoFastPolyphaseStride4Phase3_Amiga_m68k(short *pcm, int *vbuf,
	const int *coefBase) __asm__("MonoFastPolyphaseStride4Phase3_Amiga_m68k")
	__attribute__((weak));
extern void MonoFastPolyphaseStride3Phase0_Amiga_m68k(short *pcm, int *vbuf,
	const int *coefBase) __asm__("MonoFastPolyphaseStride3Phase0_Amiga_m68k")
	__attribute__((weak));
extern void MonoFastPolyphaseStride3Phase1_Amiga_m68k(short *pcm, int *vbuf,
	const int *coefBase) __asm__("MonoFastPolyphaseStride3Phase1_Amiga_m68k")
	__attribute__((weak));
extern void MonoFastPolyphaseStride3Phase2_Amiga_m68k(short *pcm, int *vbuf,
	const int *coefBase) __asm__("MonoFastPolyphaseStride3Phase2_Amiga_m68k")
	__attribute__((weak));
extern void MonoFastPolyphaseStride5Phase0_Amiga_m68k(short *pcm, int *vbuf,
	const int *coefBase) __asm__("MonoFastPolyphaseStride5Phase0_Amiga_m68k")
	__attribute__((weak));
extern void MonoFastPolyphaseStride5Phase1_Amiga_m68k(short *pcm, int *vbuf,
	const int *coefBase) __asm__("MonoFastPolyphaseStride5Phase1_Amiga_m68k")
	__attribute__((weak));
extern void MonoFastPolyphaseStride5Phase2_Amiga_m68k(short *pcm, int *vbuf,
	const int *coefBase) __asm__("MonoFastPolyphaseStride5Phase2_Amiga_m68k")
	__attribute__((weak));
extern void MonoFastPolyphaseStride5Phase3_Amiga_m68k(short *pcm, int *vbuf,
	const int *coefBase) __asm__("MonoFastPolyphaseStride5Phase3_Amiga_m68k")
	__attribute__((weak));
extern void MonoFastPolyphaseStride5Phase4_Amiga_m68k(short *pcm, int *vbuf,
	const int *coefBase) __asm__("MonoFastPolyphaseStride5Phase4_Amiga_m68k")
	__attribute__((weak));
extern void StereoFastPolyphaseStride3Phase0_Amiga_m68k(short *pcm, int *vbuf,
	const int *coefBase) __asm__("StereoFastPolyphaseStride3Phase0_Amiga_m68k")
	__attribute__((weak));
extern void StereoFastPolyphaseStride3Phase1_Amiga_m68k(short *pcm, int *vbuf,
	const int *coefBase) __asm__("StereoFastPolyphaseStride3Phase1_Amiga_m68k")
	__attribute__((weak));
extern void StereoFastPolyphaseStride3Phase2_Amiga_m68k(short *pcm, int *vbuf,
	const int *coefBase) __asm__("StereoFastPolyphaseStride3Phase2_Amiga_m68k")
	__attribute__((weak));
extern int StereoFastPolyphaseStride2Phase0_Amiga_m68k(short *pcm, int *vbuf,
	const int *coefBase) __asm__("StereoFastPolyphaseStride2Phase0_Amiga_m68k")
	__attribute__((weak));
extern int StereoFastPolyphaseStride2Phase0Reduced_Amiga_m68k(short *pcm, int *vbuf,
	const int *coefBase) __asm__("StereoFastPolyphaseStride2Phase0Reduced_Amiga_m68k")
	__attribute__((weak));
extern int StereoFastPolyphaseStride4Phase0_Amiga_m68k(short *pcm, int *vbuf,
	const int *coefBase) __asm__("StereoFastPolyphaseStride4Phase0_Amiga_m68k")
	__attribute__((weak));
extern int StereoFastPolyphaseStride4Phase1_Amiga_m68k(short *pcm, int *vbuf,
	const int *coefBase) __asm__("StereoFastPolyphaseStride4Phase1_Amiga_m68k")
	__attribute__((weak));
extern int StereoFastPolyphaseStride4Phase2_Amiga_m68k(short *pcm, int *vbuf,
	const int *coefBase) __asm__("StereoFastPolyphaseStride4Phase2_Amiga_m68k")
	__attribute__((weak));
extern int StereoFastPolyphaseStride4Phase3_Amiga_m68k(short *pcm, int *vbuf,
	const int *coefBase) __asm__("StereoFastPolyphaseStride4Phase3_Amiga_m68k")
	__attribute__((weak));
extern void AmigaM68KPolyphaseFullRateStereo(short *pcm, int *vbuf,
	const int *coefBase) __asm__("AmigaM68KPolyphaseFullRateStereo")
	__attribute__((weak));

/*
 * The m68k assembly polyphase kernels previously shifted every coefficient left
 * by DEF_NFRACBITS (6) with an `asl.l #6` immediately before each `muls.l`.  The
 * coefficient table is small and constant, so pre-shift it once into a side
 * table and hand that pointer to the assembly kernels instead.  This removes one
 * shift per filter tap from the hottest synthesis loop on the 68030.  The C
 * reference path is unchanged: it still receives the original coefBase and
 * applies the shift itself, so PolyAsmCoef() only feeds the assembly path.
 * polyCoef magnitudes are well within range: max |coef| << 6 stays far below
 * 2^31, so the pre-shift never overflows.
 */
#define POLY_ASM_COEF_COUNT 264
static int gPolyAsmCoef[POLY_ASM_COEF_COUNT];
static int gPolyAsmCoefReady;

static const int *PolyAsmCoef(const int *coefBase)
{
	int i;

	if (!gPolyAsmCoefReady) {
		for (i = 0; i < POLY_ASM_COEF_COUNT; i++)
			gPolyAsmCoef[i] = coefBase[i] << DEF_NFRACBITS;
		gPolyAsmCoefReady = 1;
	}
	return gPolyAsmCoef;
}
#endif

static __inline short ClipToShort(int x, int fracBits)
{
	int sign;

	/* assumes you've already rounded (x += (1 << (fracBits-1))) */
	x >>= fracBits;
	
	/* Ken's trick: clips to [-32768, 32767] */
	sign = x >> 31;
	if (sign != (x >> 15))
		x = sign ^ ((1 << 15) - 1);

	return (short)x;
}

#define MC0M(x)	{ \
	c1 = *coef;		coef++;		c2 = *coef;		coef++; \
	vLo = *(vb1+(x));			vHi = *(vb1+(23-(x))); \
	sum1L = MADD64(sum1L, vLo,  c1);	sum1L = MADD64(sum1L, vHi, -c2); \
}

#define MC1M(x)	{ \
	c1 = *coef;		coef++; \
	vLo = *(vb1+(x)); \
	sum1L = MADD64(sum1L, vLo,  c1); \
}

#define MC2M(x)	{ \
		c1 = *coef;		coef++;		c2 = *coef;		coef++; \
		vLo = *(vb1+(x));	vHi = *(vb1+(23-(x))); \
		sum1L = MADD64(sum1L, vLo,  c1);	sum2L = MADD64(sum2L, vLo,  c2); \
		sum1L = MADD64(sum1L, vHi, -c2);	sum2L = MADD64(sum2L, vHi,  c1); \
}

/**************************************************************************************
 * Function:    PolyphaseMono
 *
 * Description: filter one subband and produce 32 output PCM samples for one channel
 *
 * Inputs:      pointer to PCM output buffer
 *              number of "extra shifts" (vbuf format = Q(DQ_FRACBITS_OUT-2))
 *              pointer to start of vbuf (preserved from last call)
 *              start of filter coefficient table (in proper, shuffled order)
 *              no minimum number of guard bits is required for input vbuf 
 *                (see additional scaling comments below)
 *
 * Outputs:     32 samples of one channel of decoded PCM data, (i.e. Q16.0)
 *
 * Return:      none
 *
 * TODO:        add 32-bit version for platforms where 64-bit mul-acc is not supported
 *                (note max filter gain - see polyCoef[] comments)
 **************************************************************************************/
static POLYPHASE_REF_UNUSED void PolyphaseMonoReference(short *pcm, int *vbuf, const int *coefBase)
{	
	int i;
	const int *coef;
	int *vb1;
	int vLo, vHi, c1, c2;
	Word64 sum1L, sum2L, rndVal;

	rndVal = (Word64)( 1 << (DEF_NFRACBITS - 1 + (32 - CSHIFT)) );

	/* special case, output sample 0 */
	coef = coefBase;
	vb1 = vbuf;
	sum1L = rndVal;

	MC0M(0)
	MC0M(1)
	MC0M(2)
	MC0M(3)
	MC0M(4)
	MC0M(5)
	MC0M(6)
	MC0M(7)

	*(pcm + 0) = ClipToShort((int)SAR64(sum1L, (32-CSHIFT)), DEF_NFRACBITS);

	/* special case, output sample 16 */
	coef = coefBase + 256;
	vb1 = vbuf + 64*16;
	sum1L = rndVal;

	MC1M(0)
	MC1M(1)
	MC1M(2)
	MC1M(3)
	MC1M(4)
	MC1M(5)
	MC1M(6)
	MC1M(7)

	*(pcm + 16) = ClipToShort((int)SAR64(sum1L, (32-CSHIFT)), DEF_NFRACBITS);

	/* main convolution loop: sum1L = samples 1, 2, 3, ... 15   sum2L = samples 31, 30, ... 17 */
	coef = coefBase + 16;
	vb1 = vbuf + 64;
	pcm++;

	/* right now, the compiler creates bad asm from this... */
	for (i = 15; i > 0; i--) {
		sum1L = sum2L = rndVal;

		MC2M(0)
		MC2M(1)
		MC2M(2)
		MC2M(3)
		MC2M(4)
		MC2M(5)
		MC2M(6)
		MC2M(7)

		vb1 += 64;
		*(pcm)       = ClipToShort((int)SAR64(sum1L, (32-CSHIFT)), DEF_NFRACBITS);
		*(pcm + 2*i) = ClipToShort((int)SAR64(sum2L, (32-CSHIFT)), DEF_NFRACBITS);
		pcm++;
	}
}

#define MC0S(x)	{ \
	c1 = *coef;		coef++;		c2 = *coef;		coef++; \
	vLo = *(vb1+(x));		vHi = *(vb1+(23-(x))); \
	sum1L = MADD64(sum1L, vLo,  c1);	sum1L = MADD64(sum1L, vHi, -c2); \
	vLo = *(vb1+32+(x));	vHi = *(vb1+32+(23-(x))); \
	sum1R = MADD64(sum1R, vLo,  c1);	sum1R = MADD64(sum1R, vHi, -c2); \
}

#define MC1S(x)	{ \
	c1 = *coef;		coef++; \
	vLo = *(vb1+(x)); \
	sum1L = MADD64(sum1L, vLo,  c1); \
	vLo = *(vb1+32+(x)); \
	sum1R = MADD64(sum1R, vLo,  c1); \
}

#define MC2S(x)	{ \
		c1 = *coef;		coef++;		c2 = *coef;		coef++; \
		vLo = *(vb1+(x));	vHi = *(vb1+(23-(x))); \
		sum1L = MADD64(sum1L, vLo,  c1);	sum2L = MADD64(sum2L, vLo,  c2); \
		sum1L = MADD64(sum1L, vHi, -c2);	sum2L = MADD64(sum2L, vHi,  c1); \
		vLo = *(vb1+32+(x));	vHi = *(vb1+32+(23-(x))); \
		sum1R = MADD64(sum1R, vLo,  c1);	sum2R = MADD64(sum2R, vLo,  c2); \
		sum1R = MADD64(sum1R, vHi, -c2);	sum2R = MADD64(sum2R, vHi,  c1); \
}

/**************************************************************************************
 * Function:    PolyphaseStereo
 *
 * Description: filter one subband and produce 32 output PCM samples for each channel
 *
 * Inputs:      pointer to PCM output buffer
 *              number of "extra shifts" (vbuf format = Q(DQ_FRACBITS_OUT-2))
 *              pointer to start of vbuf (preserved from last call)
 *              start of filter coefficient table (in proper, shuffled order)
 *              no minimum number of guard bits is required for input vbuf 
 *                (see additional scaling comments below)
 *
 * Outputs:     32 samples of two channels of decoded PCM data, (i.e. Q16.0)
 *
 * Return:      none
 *
 * Notes:       interleaves PCM samples LRLRLR...
 *
 * TODO:        add 32-bit version for platforms where 64-bit mul-acc is not supported
 **************************************************************************************/
static POLYPHASE_REF_UNUSED void PolyphaseStereoReference(short *pcm, int *vbuf, const int *coefBase)
{
	int i;
	const int *coef;
	int *vb1;
	int vLo, vHi, c1, c2;
	Word64 sum1L, sum2L, sum1R, sum2R, rndVal;

	rndVal = (Word64)( 1 << (DEF_NFRACBITS - 1 + (32 - CSHIFT)) );

	/* special case, output sample 0 */
	coef = coefBase;
	vb1 = vbuf;
	sum1L = sum1R = rndVal;

	MC0S(0)
	MC0S(1)
	MC0S(2)
	MC0S(3)
	MC0S(4)
	MC0S(5)
	MC0S(6)
	MC0S(7)

	*(pcm + 0) = ClipToShort((int)SAR64(sum1L, (32-CSHIFT)), DEF_NFRACBITS);
	*(pcm + 1) = ClipToShort((int)SAR64(sum1R, (32-CSHIFT)), DEF_NFRACBITS);

	/* special case, output sample 16 */
	coef = coefBase + 256;
	vb1 = vbuf + 64*16;
	sum1L = sum1R = rndVal;

	MC1S(0)
	MC1S(1)
	MC1S(2)
	MC1S(3)
	MC1S(4)
	MC1S(5)
	MC1S(6)
	MC1S(7)

	*(pcm + 2*16 + 0) = ClipToShort((int)SAR64(sum1L, (32-CSHIFT)), DEF_NFRACBITS);
	*(pcm + 2*16 + 1) = ClipToShort((int)SAR64(sum1R, (32-CSHIFT)), DEF_NFRACBITS);

	/* main convolution loop: sum1L = samples 1, 2, 3, ... 15   sum2L = samples 31, 30, ... 17 */
	coef = coefBase + 16;
	vb1 = vbuf + 64;
	pcm += 2;

	/* right now, the compiler creates bad asm from this... */
	for (i = 15; i > 0; i--) {
		sum1L = sum2L = rndVal;
		sum1R = sum2R = rndVal;

		MC2S(0)
		MC2S(1)
		MC2S(2)
		MC2S(3)
		MC2S(4)
		MC2S(5)
		MC2S(6)
		MC2S(7)

		vb1 += 64;
		*(pcm + 0)         = ClipToShort((int)SAR64(sum1L, (32-CSHIFT)), DEF_NFRACBITS);
		*(pcm + 1)         = ClipToShort((int)SAR64(sum1R, (32-CSHIFT)), DEF_NFRACBITS);
		*(pcm + 2*2*i + 0) = ClipToShort((int)SAR64(sum2L, (32-CSHIFT)), DEF_NFRACBITS);
		*(pcm + 2*2*i + 1) = ClipToShort((int)SAR64(sum2R, (32-CSHIFT)), DEF_NFRACBITS);
		pcm += 2;
	}
}

#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE)

#if defined(AMIGA_M68K_POLYPHASE_68060)
/* Pull in MulShift68060 so the fast path's per-tap multiply can be made
 * 68060-clean below. This is the "decouple": AMIGA_FAST_POLYPHASE supplies the
 * downsampling work reductions (stride, reduced taps, subband cap, fdct32
 * quarter, imdct thin), and poly060 swaps their emulated muls.l for hardware
 * multiplies, so the whole fast/lowrate path runs without emulation on a 060. */
#include "polyphase_68060.h"
#endif

/*
 * Optional Amiga/m68k fast polyphase path.
 *
 * The reference path above keeps a 64-bit accumulator and rounds once at the
 * end of each convolution.  That is exact, but very expensive on 68030 C
 * builds because every MADD64/SAR64 can become libgcc helper code.  This path
 * trades a small, deterministic rounding difference for a much cheaper 32-bit
 * accumulator: each product is scaled directly to the final PCM integer domain
 * with a signed 32x32 high multiply using the existing coefficient headroom
 * (polyCoef values are Q30 pre-shifted by CSHIFT).  Accumulation and clipping
 * are then plain 32-bit operations.
 *
 * Build with -DAMIGA_FAST_POLYPHASE to opt in.  Omit it to retain the original
 * bit-exact polyphase implementation.
 */
static __inline short ClipIntToShort(int x)
{
	int sign;

	sign = x >> 31;
	if (sign != (x >> 15))
		x = sign ^ ((1 << 15) - 1);

	return (short)x;
}

static __inline int PolyphaseMulShift26(int x, int coef)
{
	/* Need (x * coef) >> (32 - CSHIFT + DEF_NFRACBITS) == >> 26.
	 * Shift the coefficient left by DEF_NFRACBITS and take the high word.
	 */
#if defined(AMIGA_M68K_POLYPHASE_68060)
	/* 68060: the register-pair muls.l below is emulated in software. Compute
	 * the identical value with hardware-only multiplies. Bit-for-bit equal to
	 * the muls.l result, so the fast/lowrate PCM output is unchanged. */
	return MulShift68060(x, coef << DEF_NFRACBITS);
#elif defined(__GNUC__) && \
	(defined(__mc68020__) || defined(__mc68030__) || defined(__mc68040__) || \
	 defined(__mc68060__) || defined(mc68020))
	int hi;
	int lo;
	lo = x;
	__asm__ volatile ("muls.l %2,%0:%1"
		: "=d" (hi), "+d" (lo)
		: "dmi" (coef << DEF_NFRACBITS));
	(void)lo;
	return hi;
#else
	return MULSHIFT32(x, coef << DEF_NFRACBITS);
#endif
}

#define FAST_MC0(acc, v, c) do { \
	const int *vLoP = (v); \
	const int *vHiP = (v) + 23; \
	const int *cP = (c); \
	(acc) += PolyphaseMulShift26(vLoP[0], cP[0]); \
	(acc) -= PolyphaseMulShift26(vHiP[0], cP[1]); \
	(acc) += PolyphaseMulShift26(vLoP[1], cP[2]); \
	(acc) -= PolyphaseMulShift26(vHiP[-1], cP[3]); \
	(acc) += PolyphaseMulShift26(vLoP[2], cP[4]); \
	(acc) -= PolyphaseMulShift26(vHiP[-2], cP[5]); \
	(acc) += PolyphaseMulShift26(vLoP[3], cP[6]); \
	(acc) -= PolyphaseMulShift26(vHiP[-3], cP[7]); \
	(acc) += PolyphaseMulShift26(vLoP[4], cP[8]); \
	(acc) -= PolyphaseMulShift26(vHiP[-4], cP[9]); \
	(acc) += PolyphaseMulShift26(vLoP[5], cP[10]); \
	(acc) -= PolyphaseMulShift26(vHiP[-5], cP[11]); \
	(acc) += PolyphaseMulShift26(vLoP[6], cP[12]); \
	(acc) -= PolyphaseMulShift26(vHiP[-6], cP[13]); \
	(acc) += PolyphaseMulShift26(vLoP[7], cP[14]); \
	(acc) -= PolyphaseMulShift26(vHiP[-7], cP[15]); \
} while (0)

#define FAST_MC1(acc, v, c) do { \
	const int *vLoP = (v); \
	const int *cP = (c); \
	(acc) += PolyphaseMulShift26(vLoP[0], cP[0]); \
	(acc) += PolyphaseMulShift26(vLoP[1], cP[1]); \
	(acc) += PolyphaseMulShift26(vLoP[2], cP[2]); \
	(acc) += PolyphaseMulShift26(vLoP[3], cP[3]); \
	(acc) += PolyphaseMulShift26(vLoP[4], cP[4]); \
	(acc) += PolyphaseMulShift26(vLoP[5], cP[5]); \
	(acc) += PolyphaseMulShift26(vLoP[6], cP[6]); \
	(acc) += PolyphaseMulShift26(vLoP[7], cP[7]); \
} while (0)

#define FAST_MC2(accLo, accHi, v, c) do { \
	const int *vLoP = (v); \
	const int *vHiP = (v) + 23; \
	const int *cP = (c); \
	int c1_, c2_; \
	int lo_, hi_; \
	c1_ = cP[0]; c2_ = cP[1]; lo_ = vLoP[0]; hi_ = vHiP[0]; \
	(accLo) += PolyphaseMulShift26(lo_, c1_) - PolyphaseMulShift26(hi_, c2_); \
	(accHi) += PolyphaseMulShift26(lo_, c2_) + PolyphaseMulShift26(hi_, c1_); \
	c1_ = cP[2]; c2_ = cP[3]; lo_ = vLoP[1]; hi_ = vHiP[-1]; \
	(accLo) += PolyphaseMulShift26(lo_, c1_) - PolyphaseMulShift26(hi_, c2_); \
	(accHi) += PolyphaseMulShift26(lo_, c2_) + PolyphaseMulShift26(hi_, c1_); \
	c1_ = cP[4]; c2_ = cP[5]; lo_ = vLoP[2]; hi_ = vHiP[-2]; \
	(accLo) += PolyphaseMulShift26(lo_, c1_) - PolyphaseMulShift26(hi_, c2_); \
	(accHi) += PolyphaseMulShift26(lo_, c2_) + PolyphaseMulShift26(hi_, c1_); \
	c1_ = cP[6]; c2_ = cP[7]; lo_ = vLoP[3]; hi_ = vHiP[-3]; \
	(accLo) += PolyphaseMulShift26(lo_, c1_) - PolyphaseMulShift26(hi_, c2_); \
	(accHi) += PolyphaseMulShift26(lo_, c2_) + PolyphaseMulShift26(hi_, c1_); \
	c1_ = cP[8]; c2_ = cP[9]; lo_ = vLoP[4]; hi_ = vHiP[-4]; \
	(accLo) += PolyphaseMulShift26(lo_, c1_) - PolyphaseMulShift26(hi_, c2_); \
	(accHi) += PolyphaseMulShift26(lo_, c2_) + PolyphaseMulShift26(hi_, c1_); \
	c1_ = cP[10]; c2_ = cP[11]; lo_ = vLoP[5]; hi_ = vHiP[-5]; \
	(accLo) += PolyphaseMulShift26(lo_, c1_) - PolyphaseMulShift26(hi_, c2_); \
	(accHi) += PolyphaseMulShift26(lo_, c2_) + PolyphaseMulShift26(hi_, c1_); \
	c1_ = cP[12]; c2_ = cP[13]; lo_ = vLoP[6]; hi_ = vHiP[-6]; \
	(accLo) += PolyphaseMulShift26(lo_, c1_) - PolyphaseMulShift26(hi_, c2_); \
	(accHi) += PolyphaseMulShift26(lo_, c2_) + PolyphaseMulShift26(hi_, c1_); \
	c1_ = cP[14]; c2_ = cP[15]; lo_ = vLoP[7]; hi_ = vHiP[-7]; \
	(accLo) += PolyphaseMulShift26(lo_, c1_) - PolyphaseMulShift26(hi_, c2_); \
	(accHi) += PolyphaseMulShift26(lo_, c2_) + PolyphaseMulShift26(hi_, c1_); \
} while (0)

#define FAST_MC2_LO(accLo, v, c) do { \
	const int *vLoP = (v); \
	const int *vHiP = (v) + 23; \
	const int *cP = (c); \
	int c1_, c2_; \
	int lo_, hi_; \
	c1_ = cP[0]; c2_ = cP[1]; lo_ = vLoP[0]; hi_ = vHiP[0]; \
	(accLo) += PolyphaseMulShift26(lo_, c1_) - PolyphaseMulShift26(hi_, c2_); \
	c1_ = cP[2]; c2_ = cP[3]; lo_ = vLoP[1]; hi_ = vHiP[-1]; \
	(accLo) += PolyphaseMulShift26(lo_, c1_) - PolyphaseMulShift26(hi_, c2_); \
	c1_ = cP[4]; c2_ = cP[5]; lo_ = vLoP[2]; hi_ = vHiP[-2]; \
	(accLo) += PolyphaseMulShift26(lo_, c1_) - PolyphaseMulShift26(hi_, c2_); \
	c1_ = cP[6]; c2_ = cP[7]; lo_ = vLoP[3]; hi_ = vHiP[-3]; \
	(accLo) += PolyphaseMulShift26(lo_, c1_) - PolyphaseMulShift26(hi_, c2_); \
	c1_ = cP[8]; c2_ = cP[9]; lo_ = vLoP[4]; hi_ = vHiP[-4]; \
	(accLo) += PolyphaseMulShift26(lo_, c1_) - PolyphaseMulShift26(hi_, c2_); \
	c1_ = cP[10]; c2_ = cP[11]; lo_ = vLoP[5]; hi_ = vHiP[-5]; \
	(accLo) += PolyphaseMulShift26(lo_, c1_) - PolyphaseMulShift26(hi_, c2_); \
	c1_ = cP[12]; c2_ = cP[13]; lo_ = vLoP[6]; hi_ = vHiP[-6]; \
	(accLo) += PolyphaseMulShift26(lo_, c1_) - PolyphaseMulShift26(hi_, c2_); \
	c1_ = cP[14]; c2_ = cP[15]; lo_ = vLoP[7]; hi_ = vHiP[-7]; \
	(accLo) += PolyphaseMulShift26(lo_, c1_) - PolyphaseMulShift26(hi_, c2_); \
} while (0)

#define FAST_MC2_HI(accHi, v, c) do { \
	const int *vLoP = (v); \
	const int *vHiP = (v) + 23; \
	const int *cP = (c); \
	int c1_, c2_; \
	int lo_, hi_; \
	c1_ = cP[0]; c2_ = cP[1]; lo_ = vLoP[0]; hi_ = vHiP[0]; \
	(accHi) += PolyphaseMulShift26(lo_, c2_) + PolyphaseMulShift26(hi_, c1_); \
	c1_ = cP[2]; c2_ = cP[3]; lo_ = vLoP[1]; hi_ = vHiP[-1]; \
	(accHi) += PolyphaseMulShift26(lo_, c2_) + PolyphaseMulShift26(hi_, c1_); \
	c1_ = cP[4]; c2_ = cP[5]; lo_ = vLoP[2]; hi_ = vHiP[-2]; \
	(accHi) += PolyphaseMulShift26(lo_, c2_) + PolyphaseMulShift26(hi_, c1_); \
	c1_ = cP[6]; c2_ = cP[7]; lo_ = vLoP[3]; hi_ = vHiP[-3]; \
	(accHi) += PolyphaseMulShift26(lo_, c2_) + PolyphaseMulShift26(hi_, c1_); \
	c1_ = cP[8]; c2_ = cP[9]; lo_ = vLoP[4]; hi_ = vHiP[-4]; \
	(accHi) += PolyphaseMulShift26(lo_, c2_) + PolyphaseMulShift26(hi_, c1_); \
	c1_ = cP[10]; c2_ = cP[11]; lo_ = vLoP[5]; hi_ = vHiP[-5]; \
	(accHi) += PolyphaseMulShift26(lo_, c2_) + PolyphaseMulShift26(hi_, c1_); \
	c1_ = cP[12]; c2_ = cP[13]; lo_ = vLoP[6]; hi_ = vHiP[-6]; \
	(accHi) += PolyphaseMulShift26(lo_, c2_) + PolyphaseMulShift26(hi_, c1_); \
	c1_ = cP[14]; c2_ = cP[15]; lo_ = vLoP[7]; hi_ = vHiP[-7]; \
	(accHi) += PolyphaseMulShift26(lo_, c2_) + PolyphaseMulShift26(hi_, c1_); \
} while (0)

static void PolyphaseMonoFast(short *pcm, int *vbuf, const int *coefBase)
{
	int i;
	const int *coef;
	int *vb1;
	int sum1;
	int sum2;

	coef = coefBase;
	vb1 = vbuf;
	sum1 = 0;
	FAST_MC0(sum1, vb1, coef);
	pcm[0] = ClipIntToShort(sum1);

	coef = coefBase + 256;
	vb1 = vbuf + 64 * 16;
	sum1 = 0;
	FAST_MC1(sum1, vb1, coef);
	pcm[16] = ClipIntToShort(sum1);

	coef = coefBase + 16;
	vb1 = vbuf + 64;
	pcm++;
	for (i = 15; i > 0; i--) {
		sum1 = 0;
		sum2 = 0;
		FAST_MC2(sum1, sum2, vb1, coef);
		pcm[0] = ClipIntToShort(sum1);
		pcm[2 * i] = ClipIntToShort(sum2);
		vb1 += 64;
		coef += 16;
		pcm++;
	}
}

static void PolyphaseStereoFast(short *pcm, int *vbuf, const int *coefBase)
{
	int i;
	const int *coef;
	int *vb1;
	int sum1L;
	int sum2L;
	int sum1R;
	int sum2R;

	coef = coefBase;
	vb1 = vbuf;
	sum1L = 0;
	sum1R = 0;
	FAST_MC0(sum1L, vb1, coef);
	FAST_MC0(sum1R, vb1 + 32, coef);
	pcm[0] = ClipIntToShort(sum1L);
	pcm[1] = ClipIntToShort(sum1R);

	coef = coefBase + 256;
	vb1 = vbuf + 64 * 16;
	sum1L = 0;
	sum1R = 0;
	FAST_MC1(sum1L, vb1, coef);
	FAST_MC1(sum1R, vb1 + 32, coef);
	pcm[2 * 16 + 0] = ClipIntToShort(sum1L);
	pcm[2 * 16 + 1] = ClipIntToShort(sum1R);

	coef = coefBase + 16;
	vb1 = vbuf + 64;
	pcm += 2;
	for (i = 15; i > 0; i--) {
		sum1L = 0;
		sum2L = 0;
		sum1R = 0;
		sum2R = 0;
		FAST_MC2(sum1L, sum2L, vb1, coef);
		FAST_MC2(sum1R, sum2R, vb1 + 32, coef);
		pcm[0] = ClipIntToShort(sum1L);
		pcm[1] = ClipIntToShort(sum1R);
		pcm[2 * 2 * i + 0] = ClipIntToShort(sum2L);
		pcm[2 * 2 * i + 1] = ClipIntToShort(sum2R);
		vb1 += 64;
		coef += 16;
		pcm += 2;
	}
}


static short PolyphaseMonoFastSample0(int *vbuf, const int *coefBase)
{
	int sum;

	sum = 0;
	FAST_MC0(sum, vbuf, coefBase);
	return ClipIntToShort(sum);
}

static short PolyphaseMonoFastSample16(int *vbuf, const int *coefBase)
{
	int sum;

	sum = 0;
	FAST_MC1(sum, vbuf + 64 * 16, coefBase + 256);
	return ClipIntToShort(sum);
}

static __inline short PolyphaseMonoFastSampleLo(int pair, int *vbuf, const int *coefBase)
{
	int sum;

	sum = 0;
	FAST_MC2_LO(sum, vbuf + 64 * pair, coefBase + 16 * pair);
	return ClipIntToShort(sum);
}

static __inline short PolyphaseMonoFastSampleHi(int pair, int *vbuf, const int *coefBase)
{
	int sum;

	sum = 0;
	FAST_MC2_HI(sum, vbuf + 64 * pair, coefBase + 16 * pair);
	return ClipIntToShort(sum);
}

static __inline void PolyphaseMonoFastSample(short *pcm, int sample, int *vbuf, const int *coefBase)
{
	int pair;

	if (sample == 0)
		pcm[0] = PolyphaseMonoFastSample0(vbuf, coefBase);
	else if (sample == 16)
		pcm[0] = PolyphaseMonoFastSample16(vbuf, coefBase);
	else {
		pair = sample < 16 ? sample : 32 - sample;
		if (sample < 16)
			pcm[0] = PolyphaseMonoFastSampleLo(pair, vbuf, coefBase);
		else
			pcm[0] = PolyphaseMonoFastSampleHi(pair, vbuf, coefBase);
	}
}

static __inline void PolyphaseStereoFastSample(short *pcm, int sample, int *vbuf, const int *coefBase)
{
	const int *coef;
	int *vb1;
	int sum1L;
	int sum2L;
	int sum1R;
	int sum2R;
	int pair;

	if (sample == 0) {
		coef = coefBase;
		vb1 = vbuf;
		sum1L = 0;
		sum1R = 0;
		FAST_MC0(sum1L, vb1, coef);
		FAST_MC0(sum1R, vb1 + 32, coef);
		pcm[0] = ClipIntToShort(sum1L);
		pcm[1] = ClipIntToShort(sum1R);
	} else if (sample == 16) {
		coef = coefBase + 256;
		vb1 = vbuf + 64 * 16;
		sum1L = 0;
		sum1R = 0;
		FAST_MC1(sum1L, vb1, coef);
		FAST_MC1(sum1R, vb1 + 32, coef);
		pcm[0] = ClipIntToShort(sum1L);
		pcm[1] = ClipIntToShort(sum1R);
	} else {
		pair = sample < 16 ? sample : 32 - sample;
		coef = coefBase + 16 * pair;
		vb1 = vbuf + 64 * pair;
		if (sample < 16) {
			sum1L = 0;
			sum1R = 0;
			FAST_MC2_LO(sum1L, vb1, coef);
			FAST_MC2_LO(sum1R, vb1 + 32, coef);
			pcm[0] = ClipIntToShort(sum1L);
			pcm[1] = ClipIntToShort(sum1R);
		} else {
			sum2L = 0;
			sum2R = 0;
			FAST_MC2_HI(sum2L, vb1, coef);
			FAST_MC2_HI(sum2R, vb1 + 32, coef);
			pcm[0] = ClipIntToShort(sum2L);
			pcm[1] = ClipIntToShort(sum2R);
		}
	}
}


/*
 * Compact fixed-stride mono kernels for the two 44100 Hz low-rate modes.
 * Keep the eight-tap convolution as a loop so the 68030 instruction cache is
 * not filled by a separate unrolled call sequence for every phase.  Only the
 * requested side of a paired sample is accumulated.
 */
static short PolyphaseMonoFastCompactSample(int sample, int *vbuf,
	const int *coefBase)
{
	const int *coef;
	const int *vLo;
	const int *vHi;
	int pair;
	int sum;
	int tap;

	sum = 0;
	if (sample == 0) {
		coef = coefBase;
		vLo = vbuf;
		vHi = vbuf + 23;
		for (tap = 0; tap < 8; tap++) {
			sum += PolyphaseMulShift26(*vLo++, *coef++);
			sum -= PolyphaseMulShift26(*vHi--, *coef++);
		}
	} else if (sample == 16) {
		coef = coefBase + 256;
		vLo = vbuf + 64 * 16;
		for (tap = 0; tap < 8; tap++)
			sum += PolyphaseMulShift26(*vLo++, *coef++);
	} else {
		pair = sample < 16 ? sample : 32 - sample;
		coef = coefBase + 16 * pair;
		vLo = vbuf + 64 * pair;
		vHi = vLo + 23;
		if (sample < 16) {
			for (tap = 0; tap < 8; tap++) {
				sum += PolyphaseMulShift26(*vLo++, coef[0]) -
					PolyphaseMulShift26(*vHi--, coef[1]);
				coef += 2;
			}
		} else {
			for (tap = 0; tap < 8; tap++) {
				sum += PolyphaseMulShift26(*vLo++, coef[1]) +
					PolyphaseMulShift26(*vHi--, coef[0]);
				coef += 2;
			}
		}
	}
	return ClipIntToShort(sum);
}


#if defined(AMIGA_FAST_REDUCED_TAPS)
/* Keep only dewindow segments 4..11.  polyCoef is stored in paired
 * segment order (0/15, 2/13, 4/11, 6/9, 8/7, 10/5, 12/3, 14/1),
 * so segments 4..11 are the middle four paired multiply slots.
 */
#define REDUCED_TAP_FIRST_PAIR 2
#define REDUCED_TAP_PAIR_COUNT 4

static short PolyphaseMonoFastCompactSampleReduced(int sample, int *vbuf,
	const int *coefBase)
{
	const int *coef;
	const int *vLo;
	const int *vHi;
	int pair;
	int sum;
	int tap;

	sum = 0;
	if (sample == 0) {
		coef = coefBase + REDUCED_TAP_FIRST_PAIR * 2;
		vLo = vbuf + REDUCED_TAP_FIRST_PAIR;
		vHi = vbuf + 23 - REDUCED_TAP_FIRST_PAIR;
		for (tap = 0; tap < REDUCED_TAP_PAIR_COUNT; tap++) {
			sum += PolyphaseMulShift26(*vLo++, *coef++);
			sum -= PolyphaseMulShift26(*vHi--, *coef++);
		}
	} else if (sample == 16) {
		coef = coefBase + 256 + REDUCED_TAP_FIRST_PAIR;
		vLo = vbuf + 64 * 16 + REDUCED_TAP_FIRST_PAIR;
		for (tap = 0; tap < REDUCED_TAP_PAIR_COUNT; tap++)
			sum += PolyphaseMulShift26(*vLo++, *coef++);
	} else {
		pair = sample < 16 ? sample : 32 - sample;
		coef = coefBase + 16 * pair + REDUCED_TAP_FIRST_PAIR * 2;
		vLo = vbuf + 64 * pair + REDUCED_TAP_FIRST_PAIR;
		vHi = vbuf + 64 * pair + 23 - REDUCED_TAP_FIRST_PAIR;
		if (sample < 16) {
			for (tap = 0; tap < REDUCED_TAP_PAIR_COUNT; tap++) {
				sum += PolyphaseMulShift26(*vLo++, coef[0]) -
					PolyphaseMulShift26(*vHi--, coef[1]);
				coef += 2;
			}
		} else {
			for (tap = 0; tap < REDUCED_TAP_PAIR_COUNT; tap++) {
				sum += PolyphaseMulShift26(*vLo++, coef[1]) +
					PolyphaseMulShift26(*vHi--, coef[0]);
				coef += 2;
			}
		}
	}
	return ClipIntToShort(sum);
}

static void PolyphaseStereoFastCompactSampleReduced(short *pcm, int sample,
	int *vbuf, const int *coefBase)
{
	pcm[0] = PolyphaseMonoFastCompactSampleReduced(sample, vbuf, coefBase);
	pcm[1] = PolyphaseMonoFastCompactSampleReduced(sample, vbuf + 32, coefBase);
}
#endif

static const unsigned char fastLowrateStride2Samples[16] = {
	0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30
};

static const unsigned char fastLowrateStride4Samples[4][8] = {
	{ 0, 4, 8, 12, 16, 20, 24, 28 },
	{ 3, 7, 11, 15, 19, 23, 27, 31 },
	{ 2, 6, 10, 14, 18, 22, 26, 30 },
	{ 1, 5, 9, 13, 17, 21, 25, 29 }
};

static const unsigned char fastLowrateStride5Samples[5][7] = {
	{ 0, 5, 10, 15, 20, 25, 30 },
	{ 4, 9, 14, 19, 24, 29, 0 },
	{ 3, 8, 13, 18, 23, 28, 0 },
	{ 2, 7, 12, 17, 22, 27, 0 },
	{ 1, 6, 11, 16, 21, 26, 31 }
};

static const unsigned char fastLowrateStride5Count[5] = { 7, 6, 6, 6, 7 };

#if defined(AMIGA_FAST_REDUCED_TAPS)
/* Every 3rd sample, phase p starting at (3 - p) % 3, in the same order as
 * PolyphaseMonoFastLowrateStride3(). */
static const unsigned char fastLowrateStride3Samples[3][11] = {
	{ 0, 3, 6, 9, 12, 15, 18, 21, 24, 27, 30 },
	{ 2, 5, 8, 11, 14, 17, 20, 23, 26, 29, 0 },
	{ 1, 4, 7, 10, 13, 16, 19, 22, 25, 28, 31 }
};

static const unsigned char fastLowrateStride3Count[3] = { 11, 10, 11 };
#endif

static int PolyphaseMonoFastLowrateCompact(short *pcm, int *vbuf,
	const int *coefBase, const unsigned char *samples, int count)
{
	int remaining;

	remaining = count;
	while (remaining-- > 0)
		*pcm++ = PolyphaseMonoFastCompactSample(*samples++, vbuf, coefBase);
	return count;
}

static int PolyphaseMonoFastLowrateStride2(short *pcm, int *vbuf,
	const int *coefBase)
{
	pcm[0] = PolyphaseMonoFastSample0(vbuf, coefBase);
	pcm[1] = PolyphaseMonoFastSampleLo(2, vbuf, coefBase);
	pcm[2] = PolyphaseMonoFastSampleLo(4, vbuf, coefBase);
	pcm[3] = PolyphaseMonoFastSampleLo(6, vbuf, coefBase);
	pcm[4] = PolyphaseMonoFastSampleLo(8, vbuf, coefBase);
	pcm[5] = PolyphaseMonoFastSampleLo(10, vbuf, coefBase);
	pcm[6] = PolyphaseMonoFastSampleLo(12, vbuf, coefBase);
	pcm[7] = PolyphaseMonoFastSampleLo(14, vbuf, coefBase);
	pcm[8] = PolyphaseMonoFastSample16(vbuf, coefBase);
	pcm[9] = PolyphaseMonoFastSampleHi(14, vbuf, coefBase);
	pcm[10] = PolyphaseMonoFastSampleHi(12, vbuf, coefBase);
	pcm[11] = PolyphaseMonoFastSampleHi(10, vbuf, coefBase);
	pcm[12] = PolyphaseMonoFastSampleHi(8, vbuf, coefBase);
	pcm[13] = PolyphaseMonoFastSampleHi(6, vbuf, coefBase);
	pcm[14] = PolyphaseMonoFastSampleHi(4, vbuf, coefBase);
	pcm[15] = PolyphaseMonoFastSampleHi(2, vbuf, coefBase);
	return 16;
}

static int PolyphaseMonoFastLowrateStride4(short *pcm, int *vbuf,
	const int *coefBase, int phase)
{
	switch (phase) {
	case 0:
		pcm[0] = PolyphaseMonoFastSample0(vbuf, coefBase);
		pcm[1] = PolyphaseMonoFastSampleLo(4, vbuf, coefBase);
		pcm[2] = PolyphaseMonoFastSampleLo(8, vbuf, coefBase);
		pcm[3] = PolyphaseMonoFastSampleLo(12, vbuf, coefBase);
		pcm[4] = PolyphaseMonoFastSample16(vbuf, coefBase);
		pcm[5] = PolyphaseMonoFastSampleHi(12, vbuf, coefBase);
		pcm[6] = PolyphaseMonoFastSampleHi(8, vbuf, coefBase);
		pcm[7] = PolyphaseMonoFastSampleHi(4, vbuf, coefBase);
		return 8;
	case 1:
		pcm[0] = PolyphaseMonoFastSampleLo(3, vbuf, coefBase);
		pcm[1] = PolyphaseMonoFastSampleLo(7, vbuf, coefBase);
		pcm[2] = PolyphaseMonoFastSampleLo(11, vbuf, coefBase);
		pcm[3] = PolyphaseMonoFastSampleLo(15, vbuf, coefBase);
		pcm[4] = PolyphaseMonoFastSampleHi(13, vbuf, coefBase);
		pcm[5] = PolyphaseMonoFastSampleHi(9, vbuf, coefBase);
		pcm[6] = PolyphaseMonoFastSampleHi(5, vbuf, coefBase);
		pcm[7] = PolyphaseMonoFastSampleHi(1, vbuf, coefBase);
		return 8;
	case 2:
		pcm[0] = PolyphaseMonoFastSampleLo(2, vbuf, coefBase);
		pcm[1] = PolyphaseMonoFastSampleLo(6, vbuf, coefBase);
		pcm[2] = PolyphaseMonoFastSampleLo(10, vbuf, coefBase);
		pcm[3] = PolyphaseMonoFastSampleLo(14, vbuf, coefBase);
		pcm[4] = PolyphaseMonoFastSampleHi(14, vbuf, coefBase);
		pcm[5] = PolyphaseMonoFastSampleHi(10, vbuf, coefBase);
		pcm[6] = PolyphaseMonoFastSampleHi(6, vbuf, coefBase);
		pcm[7] = PolyphaseMonoFastSampleHi(2, vbuf, coefBase);
		return 8;
	case 3:
		pcm[0] = PolyphaseMonoFastSampleLo(1, vbuf, coefBase);
		pcm[1] = PolyphaseMonoFastSampleLo(5, vbuf, coefBase);
		pcm[2] = PolyphaseMonoFastSampleLo(9, vbuf, coefBase);
		pcm[3] = PolyphaseMonoFastSampleLo(13, vbuf, coefBase);
		pcm[4] = PolyphaseMonoFastSampleHi(15, vbuf, coefBase);
		pcm[5] = PolyphaseMonoFastSampleHi(11, vbuf, coefBase);
		pcm[6] = PolyphaseMonoFastSampleHi(7, vbuf, coefBase);
		pcm[7] = PolyphaseMonoFastSampleHi(3, vbuf, coefBase);
		return 8;
	default:
		return 0;
	}
}

#if defined(AMIGA_FAST_REDUCED_TAPS)
static int PolyphaseMonoFastLowrateStride2Reduced(short *pcm, int *vbuf,
	const int *coefBase)
{
	const unsigned char *samples;
	int remaining;

	samples = fastLowrateStride2Samples;
	remaining = 16;
	while (remaining-- > 0)
		*pcm++ = PolyphaseMonoFastCompactSampleReduced(*samples++, vbuf, coefBase);
	return 16;
}

static int PolyphaseMonoFastLowrateStride4Reduced(short *pcm, int *vbuf,
	const int *coefBase, int phase)
{
	const unsigned char *samples;
	int remaining;

	samples = fastLowrateStride4Samples[phase];
	remaining = 8;
	while (remaining-- > 0)
		*pcm++ = PolyphaseMonoFastCompactSampleReduced(*samples++, vbuf, coefBase);
	return 8;
}

/* 14700 Hz from 44.1 kHz: the same middle-eight-tap dewindow as 22050/11025,
 * halving the per-sample filter work for "Faster"/"Fast" quality. */
static int PolyphaseMonoFastLowrateStride3Reduced(short *pcm, int *vbuf,
	const int *coefBase, int phase)
{
	const unsigned char *samples;
	int count, remaining;

	samples = fastLowrateStride3Samples[phase];
	count = remaining = fastLowrateStride3Count[phase];
	while (remaining-- > 0)
		*pcm++ = PolyphaseMonoFastCompactSampleReduced(*samples++, vbuf, coefBase);
	return count;
}
#endif

static int PolyphaseMonoFastLowrateStride5(short *pcm, int *vbuf,
	const int *coefBase, int phase)
{
	switch (phase) {
	case 0:
		pcm[0] = PolyphaseMonoFastSample0(vbuf, coefBase);
		pcm[1] = PolyphaseMonoFastSampleLo(5, vbuf, coefBase);
		pcm[2] = PolyphaseMonoFastSampleLo(10, vbuf, coefBase);
		pcm[3] = PolyphaseMonoFastSampleLo(15, vbuf, coefBase);
		pcm[4] = PolyphaseMonoFastSampleHi(12, vbuf, coefBase);
		pcm[5] = PolyphaseMonoFastSampleHi(7, vbuf, coefBase);
		pcm[6] = PolyphaseMonoFastSampleHi(2, vbuf, coefBase);
		return 7;
	case 1:
		pcm[0] = PolyphaseMonoFastSampleLo(4, vbuf, coefBase);
		pcm[1] = PolyphaseMonoFastSampleLo(9, vbuf, coefBase);
		pcm[2] = PolyphaseMonoFastSampleLo(14, vbuf, coefBase);
		pcm[3] = PolyphaseMonoFastSampleHi(13, vbuf, coefBase);
		pcm[4] = PolyphaseMonoFastSampleHi(8, vbuf, coefBase);
		pcm[5] = PolyphaseMonoFastSampleHi(3, vbuf, coefBase);
		return 6;
	case 2:
		pcm[0] = PolyphaseMonoFastSampleLo(3, vbuf, coefBase);
		pcm[1] = PolyphaseMonoFastSampleLo(8, vbuf, coefBase);
		pcm[2] = PolyphaseMonoFastSampleLo(13, vbuf, coefBase);
		pcm[3] = PolyphaseMonoFastSampleHi(14, vbuf, coefBase);
		pcm[4] = PolyphaseMonoFastSampleHi(9, vbuf, coefBase);
		pcm[5] = PolyphaseMonoFastSampleHi(4, vbuf, coefBase);
		return 6;
	case 3:
		pcm[0] = PolyphaseMonoFastSampleLo(2, vbuf, coefBase);
		pcm[1] = PolyphaseMonoFastSampleLo(7, vbuf, coefBase);
		pcm[2] = PolyphaseMonoFastSampleLo(12, vbuf, coefBase);
		pcm[3] = PolyphaseMonoFastSampleHi(15, vbuf, coefBase);
		pcm[4] = PolyphaseMonoFastSampleHi(10, vbuf, coefBase);
		pcm[5] = PolyphaseMonoFastSampleHi(5, vbuf, coefBase);
		return 6;
	case 4:
		pcm[0] = PolyphaseMonoFastSampleLo(1, vbuf, coefBase);
		pcm[1] = PolyphaseMonoFastSampleLo(6, vbuf, coefBase);
		pcm[2] = PolyphaseMonoFastSampleLo(11, vbuf, coefBase);
		pcm[3] = PolyphaseMonoFastSample16(vbuf, coefBase);
		pcm[4] = PolyphaseMonoFastSampleHi(11, vbuf, coefBase);
		pcm[5] = PolyphaseMonoFastSampleHi(6, vbuf, coefBase);
		pcm[6] = PolyphaseMonoFastSampleHi(1, vbuf, coefBase);
		return 7;
	default:
		return 0;
	}
}

static int PolyphaseAdvanceLowratePhase(int phase, int stride)
{
	if (stride > 1) {
		phase += NBANDS % stride;
		while (phase >= stride)
			phase -= stride;
	}
	return phase;
}

static int PolyphaseMonoFastLowrateList(short *pcm, int *vbuf,
	const int *coefBase, const unsigned char *samples, int count)
{
	short *out;
	int remaining;

	out = pcm;
	remaining = count;
	while (remaining-- > 0)
		PolyphaseMonoFastSample(out++, *samples++, vbuf, coefBase);
	return count;
}

static int PolyphaseStereoFastLowrateList(short *pcm, int *vbuf, const int *coefBase, const unsigned char *samples, int count)
{
	short *out;
	const unsigned char *samplePtr;
	int remaining;

	out = pcm;
	samplePtr = samples;
	remaining = count;
	while (remaining-- > 0) {
		PolyphaseStereoFastSample(out, *samplePtr++, vbuf, coefBase);
		out += 2;
	}
	return count * 2;
}

static __inline void PolyphaseStereoFastSample0(short *pcm, int *vbuf,
	const int *coefBase)
{
	int sumL;
	int sumR;

	sumL = 0;
	sumR = 0;
	FAST_MC0(sumL, vbuf, coefBase);
	FAST_MC0(sumR, vbuf + 32, coefBase);
	pcm[0] = ClipIntToShort(sumL);
	pcm[1] = ClipIntToShort(sumR);
}

static __inline void PolyphaseStereoFastSample16(short *pcm, int *vbuf,
	const int *coefBase)
{
	int sumL;
	int sumR;

	sumL = 0;
	sumR = 0;
	FAST_MC1(sumL, vbuf + 64 * 16, coefBase + 256);
	FAST_MC1(sumR, vbuf + 64 * 16 + 32, coefBase + 256);
	pcm[0] = ClipIntToShort(sumL);
	pcm[1] = ClipIntToShort(sumR);
}

static __inline void PolyphaseStereoFastSampleLo(short *pcm, int pair,
	int *vbuf, const int *coefBase)
{
	int sumL;
	int sumR;
	int *vb1;
	const int *coef;

	vb1 = vbuf + 64 * pair;
	coef = coefBase + 16 * pair;
	sumL = 0;
	sumR = 0;
	FAST_MC2_LO(sumL, vb1, coef);
	FAST_MC2_LO(sumR, vb1 + 32, coef);
	pcm[0] = ClipIntToShort(sumL);
	pcm[1] = ClipIntToShort(sumR);
}

static __inline void PolyphaseStereoFastSampleHi(short *pcm, int pair,
	int *vbuf, const int *coefBase)
{
	int sumL;
	int sumR;
	int *vb1;
	const int *coef;

	vb1 = vbuf + 64 * pair;
	coef = coefBase + 16 * pair;
	sumL = 0;
	sumR = 0;
	FAST_MC2_HI(sumL, vb1, coef);
	FAST_MC2_HI(sumR, vb1 + 32, coef);
	pcm[0] = ClipIntToShort(sumL);
	pcm[1] = ClipIntToShort(sumR);
}

static __inline void PolyphaseStereoFastCompactSample(short *pcm, int sample,
	int *vbuf, const int *coefBase)
{
	int pair;

	if (sample == 0)
		PolyphaseStereoFastSample0(pcm, vbuf, coefBase);
	else if (sample == 16)
		PolyphaseStereoFastSample16(pcm, vbuf, coefBase);
	else {
		pair = sample < 16 ? sample : 32 - sample;
		if (sample < 16)
			PolyphaseStereoFastSampleLo(pcm, pair, vbuf, coefBase);
		else
			PolyphaseStereoFastSampleHi(pcm, pair, vbuf, coefBase);
	}
}

static int PolyphaseStereoFastLowrateCompact(short *pcm, int *vbuf,
	const int *coefBase, const unsigned char *samples, int count)
{
	int remaining;

	remaining = count;
	while (remaining-- > 0) {
		PolyphaseStereoFastCompactSample(pcm, *samples++, vbuf, coefBase);
		pcm += 2;
	}
	return count * 2;
}

static int PolyphaseStereoFastLowrateStride2_C(short *pcm, int *vbuf,
	const int *coefBase)
{
	return PolyphaseStereoFastLowrateCompact(pcm, vbuf, coefBase,
		fastLowrateStride2Samples, 16);
}

#if defined(AMIGA_FAST_REDUCED_TAPS)
static int PolyphaseStereoFastLowrateStride2Reduced(short *pcm, int *vbuf,
	const int *coefBase)
{
	const unsigned char *samples;
	int remaining;

	samples = fastLowrateStride2Samples;
	remaining = 16;
	while (remaining-- > 0) {
		PolyphaseStereoFastCompactSampleReduced(pcm, *samples++, vbuf, coefBase);
		pcm += 2;
	}
	return 32;
}
#endif

static int PolyphaseStereoFastLowrateStride4(short *pcm, int *vbuf,
	const int *coefBase, int phase)
{
	return PolyphaseStereoFastLowrateCompact(pcm, vbuf, coefBase,
		fastLowrateStride4Samples[phase], 8);
}

#if defined(AMIGA_FAST_REDUCED_TAPS)
static int PolyphaseStereoFastLowrateStride4Reduced(short *pcm, int *vbuf,
	const int *coefBase, int phase)
{
	const unsigned char *samples;
	int remaining;

	samples = fastLowrateStride4Samples[phase];
	remaining = 8;
	while (remaining-- > 0) {
		PolyphaseStereoFastCompactSampleReduced(pcm, *samples++, vbuf, coefBase);
		pcm += 2;
	}
	return 16;
}

static int PolyphaseStereoFastLowrateStride3Reduced(short *pcm, int *vbuf,
	const int *coefBase, int phase)
{
	const unsigned char *samples;
	int count, remaining;

	samples = fastLowrateStride3Samples[phase];
	count = remaining = fastLowrateStride3Count[phase];
	while (remaining-- > 0) {
		PolyphaseStereoFastCompactSampleReduced(pcm, *samples++, vbuf, coefBase);
		pcm += 2;
	}
	return count * 2;
}
#endif

static int PolyphaseStereoFastLowrateStride5(short *pcm, int *vbuf,
	const int *coefBase, int phase)
{
	switch (phase) {
	case 0:
		PolyphaseStereoFastSample0(pcm + 0, vbuf, coefBase);
		PolyphaseStereoFastSampleLo(pcm + 2, 5, vbuf, coefBase);
		PolyphaseStereoFastSampleLo(pcm + 4, 10, vbuf, coefBase);
		PolyphaseStereoFastSampleLo(pcm + 6, 15, vbuf, coefBase);
		PolyphaseStereoFastSampleHi(pcm + 8, 12, vbuf, coefBase);
		PolyphaseStereoFastSampleHi(pcm + 10, 7, vbuf, coefBase);
		PolyphaseStereoFastSampleHi(pcm + 12, 2, vbuf, coefBase);
		return 14;
	case 1:
		PolyphaseStereoFastSampleLo(pcm + 0, 4, vbuf, coefBase);
		PolyphaseStereoFastSampleLo(pcm + 2, 9, vbuf, coefBase);
		PolyphaseStereoFastSampleLo(pcm + 4, 14, vbuf, coefBase);
		PolyphaseStereoFastSampleHi(pcm + 6, 13, vbuf, coefBase);
		PolyphaseStereoFastSampleHi(pcm + 8, 8, vbuf, coefBase);
		PolyphaseStereoFastSampleHi(pcm + 10, 3, vbuf, coefBase);
		return 12;
	case 2:
		PolyphaseStereoFastSampleLo(pcm + 0, 3, vbuf, coefBase);
		PolyphaseStereoFastSampleLo(pcm + 2, 8, vbuf, coefBase);
		PolyphaseStereoFastSampleLo(pcm + 4, 13, vbuf, coefBase);
		PolyphaseStereoFastSampleHi(pcm + 6, 14, vbuf, coefBase);
		PolyphaseStereoFastSampleHi(pcm + 8, 9, vbuf, coefBase);
		PolyphaseStereoFastSampleHi(pcm + 10, 4, vbuf, coefBase);
		return 12;
	case 3:
		PolyphaseStereoFastSampleLo(pcm + 0, 2, vbuf, coefBase);
		PolyphaseStereoFastSampleLo(pcm + 2, 7, vbuf, coefBase);
		PolyphaseStereoFastSampleLo(pcm + 4, 12, vbuf, coefBase);
		PolyphaseStereoFastSampleHi(pcm + 6, 15, vbuf, coefBase);
		PolyphaseStereoFastSampleHi(pcm + 8, 10, vbuf, coefBase);
		PolyphaseStereoFastSampleHi(pcm + 10, 5, vbuf, coefBase);
		return 12;
	case 4:
		PolyphaseStereoFastSampleLo(pcm + 0, 1, vbuf, coefBase);
		PolyphaseStereoFastSampleLo(pcm + 2, 6, vbuf, coefBase);
		PolyphaseStereoFastSampleLo(pcm + 4, 11, vbuf, coefBase);
		PolyphaseStereoFastSample16(pcm + 6, vbuf, coefBase);
		PolyphaseStereoFastSampleHi(pcm + 8, 11, vbuf, coefBase);
		PolyphaseStereoFastSampleHi(pcm + 10, 6, vbuf, coefBase);
		PolyphaseStereoFastSampleHi(pcm + 12, 1, vbuf, coefBase);
		return 14;
	default:
		return 0;
	}
}

/*
 * Stride 3 (14700 Hz output from 44100 Hz): hand-unrolled C, mirroring the
 * stride 4/5 style above -- no per-sample dispatch branch or loop overhead,
 * just the same PolyphaseMonoFastSampleLo/Hi/0/16 primitives called
 * directly. incoming phase p selects synthesis indices i with
 * (p + i) % 3 == 0, matching PolyphaseAdvanceLowratePhase()'s contract:
 * phase 0 keeps {0,3,...,30} (11), phase 1 keeps {2,5,...,29} (10), phase 2
 * keeps {1,4,...,31} (11), 32 samples total over the 3-call cycle. Proven
 * against the full-band reference by --selftest-polyphase-stride3[-stereo].
 */
static int PolyphaseMonoFastLowrateStride3(short *pcm, int *vbuf,
	const int *coefBase, int phase)
{
	switch (phase) {
	case 0:
		pcm[0] = PolyphaseMonoFastSample0(vbuf, coefBase);
		pcm[1] = PolyphaseMonoFastSampleLo(3, vbuf, coefBase);
		pcm[2] = PolyphaseMonoFastSampleLo(6, vbuf, coefBase);
		pcm[3] = PolyphaseMonoFastSampleLo(9, vbuf, coefBase);
		pcm[4] = PolyphaseMonoFastSampleLo(12, vbuf, coefBase);
		pcm[5] = PolyphaseMonoFastSampleLo(15, vbuf, coefBase);
		pcm[6] = PolyphaseMonoFastSampleHi(14, vbuf, coefBase);
		pcm[7] = PolyphaseMonoFastSampleHi(11, vbuf, coefBase);
		pcm[8] = PolyphaseMonoFastSampleHi(8, vbuf, coefBase);
		pcm[9] = PolyphaseMonoFastSampleHi(5, vbuf, coefBase);
		pcm[10] = PolyphaseMonoFastSampleHi(2, vbuf, coefBase);
		return 11;
	case 1:
		pcm[0] = PolyphaseMonoFastSampleLo(2, vbuf, coefBase);
		pcm[1] = PolyphaseMonoFastSampleLo(5, vbuf, coefBase);
		pcm[2] = PolyphaseMonoFastSampleLo(8, vbuf, coefBase);
		pcm[3] = PolyphaseMonoFastSampleLo(11, vbuf, coefBase);
		pcm[4] = PolyphaseMonoFastSampleLo(14, vbuf, coefBase);
		pcm[5] = PolyphaseMonoFastSampleHi(15, vbuf, coefBase);
		pcm[6] = PolyphaseMonoFastSampleHi(12, vbuf, coefBase);
		pcm[7] = PolyphaseMonoFastSampleHi(9, vbuf, coefBase);
		pcm[8] = PolyphaseMonoFastSampleHi(6, vbuf, coefBase);
		pcm[9] = PolyphaseMonoFastSampleHi(3, vbuf, coefBase);
		return 10;
	case 2:
		pcm[0] = PolyphaseMonoFastSampleLo(1, vbuf, coefBase);
		pcm[1] = PolyphaseMonoFastSampleLo(4, vbuf, coefBase);
		pcm[2] = PolyphaseMonoFastSampleLo(7, vbuf, coefBase);
		pcm[3] = PolyphaseMonoFastSampleLo(10, vbuf, coefBase);
		pcm[4] = PolyphaseMonoFastSampleLo(13, vbuf, coefBase);
		pcm[5] = PolyphaseMonoFastSample16(vbuf, coefBase);
		pcm[6] = PolyphaseMonoFastSampleHi(13, vbuf, coefBase);
		pcm[7] = PolyphaseMonoFastSampleHi(10, vbuf, coefBase);
		pcm[8] = PolyphaseMonoFastSampleHi(7, vbuf, coefBase);
		pcm[9] = PolyphaseMonoFastSampleHi(4, vbuf, coefBase);
		pcm[10] = PolyphaseMonoFastSampleHi(1, vbuf, coefBase);
		return 11;
	default:
		return 0;
	}
}

static int PolyphaseStereoFastLowrateStride3(short *pcm, int *vbuf,
	const int *coefBase, int phase)
{
	switch (phase) {
	case 0:
		PolyphaseStereoFastSample0(pcm + 0, vbuf, coefBase);
		PolyphaseStereoFastSampleLo(pcm + 2, 3, vbuf, coefBase);
		PolyphaseStereoFastSampleLo(pcm + 4, 6, vbuf, coefBase);
		PolyphaseStereoFastSampleLo(pcm + 6, 9, vbuf, coefBase);
		PolyphaseStereoFastSampleLo(pcm + 8, 12, vbuf, coefBase);
		PolyphaseStereoFastSampleLo(pcm + 10, 15, vbuf, coefBase);
		PolyphaseStereoFastSampleHi(pcm + 12, 14, vbuf, coefBase);
		PolyphaseStereoFastSampleHi(pcm + 14, 11, vbuf, coefBase);
		PolyphaseStereoFastSampleHi(pcm + 16, 8, vbuf, coefBase);
		PolyphaseStereoFastSampleHi(pcm + 18, 5, vbuf, coefBase);
		PolyphaseStereoFastSampleHi(pcm + 20, 2, vbuf, coefBase);
		return 22;
	case 1:
		PolyphaseStereoFastSampleLo(pcm + 0, 2, vbuf, coefBase);
		PolyphaseStereoFastSampleLo(pcm + 2, 5, vbuf, coefBase);
		PolyphaseStereoFastSampleLo(pcm + 4, 8, vbuf, coefBase);
		PolyphaseStereoFastSampleLo(pcm + 6, 11, vbuf, coefBase);
		PolyphaseStereoFastSampleLo(pcm + 8, 14, vbuf, coefBase);
		PolyphaseStereoFastSampleHi(pcm + 10, 15, vbuf, coefBase);
		PolyphaseStereoFastSampleHi(pcm + 12, 12, vbuf, coefBase);
		PolyphaseStereoFastSampleHi(pcm + 14, 9, vbuf, coefBase);
		PolyphaseStereoFastSampleHi(pcm + 16, 6, vbuf, coefBase);
		PolyphaseStereoFastSampleHi(pcm + 18, 3, vbuf, coefBase);
		return 20;
	case 2:
		PolyphaseStereoFastSampleLo(pcm + 0, 1, vbuf, coefBase);
		PolyphaseStereoFastSampleLo(pcm + 2, 4, vbuf, coefBase);
		PolyphaseStereoFastSampleLo(pcm + 4, 7, vbuf, coefBase);
		PolyphaseStereoFastSampleLo(pcm + 6, 10, vbuf, coefBase);
		PolyphaseStereoFastSampleLo(pcm + 8, 13, vbuf, coefBase);
		PolyphaseStereoFastSample16(pcm + 10, vbuf, coefBase);
		PolyphaseStereoFastSampleHi(pcm + 12, 13, vbuf, coefBase);
		PolyphaseStereoFastSampleHi(pcm + 14, 10, vbuf, coefBase);
		PolyphaseStereoFastSampleHi(pcm + 16, 7, vbuf, coefBase);
		PolyphaseStereoFastSampleHi(pcm + 18, 4, vbuf, coefBase);
		PolyphaseStereoFastSampleHi(pcm + 20, 1, vbuf, coefBase);
		return 22;
	default:
		return 0;
	}
}

#endif /* AMIGA_M68K && AMIGA_FAST_POLYPHASE */

void PolyphaseMonoFast_C_REFERENCE(short *pcm, int *vbuf, const int *coefBase)
{
#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE)
	PolyphaseMonoFast(pcm, vbuf, coefBase);
#else
	PolyphaseMonoReference(pcm, vbuf, coefBase);
#endif
}

int AmigaM68KPolyphaseMonoFast_IsActive(void)
{
#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE) && defined(AMIGA_M68K_ASM_POLYPHASE)
	return AmigaM68KPolyphaseMonoFast ? 1 : 0;
#else
	return 0;
#endif
}

int AmigaM68KPolyphaseMonoFastStride2_IsActive(void)
{
#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE) && defined(AMIGA_M68K_ASM_POLYPHASE)
	return AmigaM68KPolyphaseMonoFastStride2 ? 1 : 0;
#else
	return 0;
#endif
}

int AmigaM68KPolyphaseMonoFastStride2Reduced_IsActive(void)
{
#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE) && defined(AMIGA_M68K_ASM_POLYPHASE)
	return AmigaM68KPolyphaseMonoFastStride2Reduced ? 1 : 0;
#else
	return 0;
#endif
}

int MonoFastPolyphaseStride4_Amiga_m68k_IsActive(void)
{
#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE) && defined(AMIGA_M68K_ASM_POLYPHASE)
	return MonoFastPolyphaseStride4_Amiga_m68k ? 1 : 0;
#else
	return 0;
#endif
}

/*
 * Phase 0 keeps its own MonoFastPolyphaseStride4_Amiga_m68k_IsActive()
 * above (used by the original --selftest-polyphase-stride4, which only
 * ever exercises phase 0). This one additionally requires phases 1-3 --
 * added later, closing the gap where 3 of every 4 mono stride-4 frames
 * fell back to C -- and is what the runtime dispatch below actually
 * checks before using any of the asm phase kernels.
 */
int MonoFastPolyphaseStride4AllPhases_Amiga_m68k_IsActive(void)
{
#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE) && defined(AMIGA_M68K_ASM_POLYPHASE)
	return (MonoFastPolyphaseStride4_Amiga_m68k &&
		MonoFastPolyphaseStride4Phase1_Amiga_m68k &&
		MonoFastPolyphaseStride4Phase2_Amiga_m68k &&
		MonoFastPolyphaseStride4Phase3_Amiga_m68k) ? 1 : 0;
#else
	return 0;
#endif
}

int MonoFastPolyphaseStride3_Amiga_m68k_IsActive(void)
{
#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE) && defined(AMIGA_M68K_ASM_POLYPHASE)
	return (MonoFastPolyphaseStride3Phase0_Amiga_m68k &&
		MonoFastPolyphaseStride3Phase1_Amiga_m68k &&
		MonoFastPolyphaseStride3Phase2_Amiga_m68k) ? 1 : 0;
#else
	return 0;
#endif
}

int MonoFastPolyphaseStride5_Amiga_m68k_IsActive(void)
{
#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE) && defined(AMIGA_M68K_ASM_POLYPHASE)
	return (MonoFastPolyphaseStride5Phase0_Amiga_m68k &&
		MonoFastPolyphaseStride5Phase1_Amiga_m68k &&
		MonoFastPolyphaseStride5Phase2_Amiga_m68k &&
		MonoFastPolyphaseStride5Phase3_Amiga_m68k &&
		MonoFastPolyphaseStride5Phase4_Amiga_m68k) ? 1 : 0;
#else
	return 0;
#endif
}

int StereoFastPolyphaseStride3_Amiga_m68k_IsActive(void)
{
#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE) && defined(AMIGA_M68K_ASM_POLYPHASE)
	return (StereoFastPolyphaseStride3Phase0_Amiga_m68k &&
		StereoFastPolyphaseStride3Phase1_Amiga_m68k &&
		StereoFastPolyphaseStride3Phase2_Amiga_m68k) ? 1 : 0;
#else
	return 0;
#endif
}

int StereoFastPolyphaseStride2Phase0_Amiga_m68k_IsActive(void)
{
#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE) && defined(AMIGA_M68K_ASM_POLYPHASE)
	return StereoFastPolyphaseStride2Phase0_Amiga_m68k ? 1 : 0;
#else
	return 0;
#endif
}

int StereoFastPolyphaseStride2Phase0Reduced_Amiga_m68k_IsActive(void)
{
#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE) && defined(AMIGA_M68K_ASM_POLYPHASE)
	return StereoFastPolyphaseStride2Phase0Reduced_Amiga_m68k ? 1 : 0;
#else
	return 0;
#endif
}

int StereoFastPolyphaseStride2_HAS_AMIGA_M68K_ASM_RUNTIME(void)
{
	return StereoFastPolyphaseStride2Phase0_Amiga_m68k_IsActive();
}

int PolyphaseStereoFastLowrateStride2Reduced_HAS_AMIGA_M68K_ASM_RUNTIME(void)
{
	return StereoFastPolyphaseStride2Phase0Reduced_Amiga_m68k_IsActive();
}

int StereoFastPolyphaseStride4_HAS_AMIGA_M68K_ASM_RUNTIME(void)
{
	return StereoFastPolyphaseStride4Half_Amiga_m68k_IsActive();
}

int PolyphaseStereoFastLowrateStride4Reduced_HAS_AMIGA_M68K_ASM_RUNTIME(void)
{
	return StereoFastPolyphaseStride4HalfReduced_Amiga_m68k_IsActive();
}

int PolyphaseStereoFastLowrateStride2_C_REFERENCE(short *pcm, int *vbuf, const int *coefBase)
{
#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE)
	return PolyphaseStereoFastLowrateCompact(pcm, vbuf, coefBase,
		fastLowrateStride2Samples, 16);
#else
	int phase = 0;
	return PolyphaseStereoFastLowrate(pcm, vbuf, coefBase, 2, &phase);
#endif
}

int PolyphaseStereoFastLowrateStride2_TEST_ACTIVE(short *pcm, int *vbuf, const int *coefBase)
{
#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE) && defined(AMIGA_FAST_REDUCED_TAPS)
	if (MP3ExperimentalReducedTapsEnabled()) {
#if defined(AMIGA_M68K_ASM_POLYPHASE)
		if (StereoFastPolyphaseStride2Phase0Reduced_Amiga_m68k_IsActive()) {
			StereoFastPolyphaseStride2Phase0Reduced_Amiga_m68k(pcm, vbuf, PolyAsmCoef(coefBase));
			return 32;
		}
#endif
		return PolyphaseStereoFastLowrateStride2Reduced(pcm, vbuf, coefBase);
	}
#endif
#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE) && defined(AMIGA_M68K_ASM_POLYPHASE)
	if (!MP3ForceStereoStride2PolyphaseC() &&
		StereoFastPolyphaseStride2Phase0_Amiga_m68k_IsActive()) {
		StereoFastPolyphaseStride2Phase0_Amiga_m68k(pcm, vbuf, PolyAsmCoef(coefBase));
		return 32;
	}
#endif
	return PolyphaseStereoFastLowrateStride2_C_REFERENCE(pcm, vbuf, coefBase);
}

int StereoFastPolyphaseStride4Half_Amiga_m68k_IsActive(void)
{
#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE) && defined(AMIGA_M68K_ASM_POLYPHASE)
	return StereoFastPolyphaseStride4Half_Amiga_m68k ? 1 : 0;
#else
	return 0;
#endif
}

int StereoFastPolyphaseStride4HalfReduced_Amiga_m68k_IsActive(void)
{
#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE) && defined(AMIGA_M68K_ASM_POLYPHASE)
	return StereoFastPolyphaseStride4HalfReduced_Amiga_m68k ? 1 : 0;
#else
	return 0;
#endif
}

int StereoFastPolyphaseStride4Phase0_Amiga_m68k_IsActive(void)
{
#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE) && defined(AMIGA_M68K_ASM_POLYPHASE)
	return StereoFastPolyphaseStride4Half_Amiga_m68k_IsActive();
#else
	return 0;
#endif
}

int StereoFastPolyphaseStride4Phase1_Amiga_m68k_IsActive(void)
{
#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE) && defined(AMIGA_M68K_ASM_POLYPHASE)
	return StereoFastPolyphaseStride4Phase1_Amiga_m68k ? 1 : 0;
#else
	return 0;
#endif
}

int StereoFastPolyphaseStride4Phase2_Amiga_m68k_IsActive(void)
{
#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE) && defined(AMIGA_M68K_ASM_POLYPHASE)
	return StereoFastPolyphaseStride4Phase2_Amiga_m68k ? 1 : 0;
#else
	return 0;
#endif
}

int StereoFastPolyphaseStride4Phase3_Amiga_m68k_IsActive(void)
{
#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE) && defined(AMIGA_M68K_ASM_POLYPHASE)
	return StereoFastPolyphaseStride4Phase3_Amiga_m68k ? 1 : 0;
#else
	return 0;
#endif
}

int StereoFastPolyphaseStride5_Amiga_m68k_IsActive(void)
{
#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE) && defined(AMIGA_M68K_ASM_POLYPHASE)
	return StereoFastPolyphaseStride5_Amiga_m68k ? 1 : 0;
#else
	return 0;
#endif
}

int PolyphaseMonoFast_HAS_AMIGA_M68K_ASM_RUNTIME(void)
{
	return AmigaM68KPolyphaseMonoFast_IsActive();
}

int PolyphaseMonoFastLowrateStride2_HAS_AMIGA_M68K_ASM_RUNTIME(void)
{
	return AmigaM68KPolyphaseMonoFastStride2_IsActive();
}

int PolyphaseMonoFastLowrateStride2Reduced_HAS_AMIGA_M68K_ASM_RUNTIME(void)
{
	return AmigaM68KPolyphaseMonoFastStride2Reduced_IsActive();
}

int PolyphaseMonoFastLowrateStride4_HAS_AMIGA_M68K_ASM_RUNTIME(void)
{
	return MonoFastPolyphaseStride4_Amiga_m68k_IsActive();
}

void PolyphaseMonoFast_TEST_ACTIVE(short *pcm, int *vbuf, const int *coefBase)
{
#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE) && defined(AMIGA_M68K_ASM_POLYPHASE)
	if (AmigaM68KPolyphaseMonoFast_IsActive()) {
		AmigaM68KPolyphaseMonoFast(pcm, vbuf, PolyAsmCoef(coefBase));
		return;
	}
#endif
	PolyphaseMonoFast_C_REFERENCE(pcm, vbuf, coefBase);
}


int PolyphaseMonoFastLowrateStride2_C_REFERENCE(short *pcm, int *vbuf, const int *coefBase)
{
#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE)
	return PolyphaseMonoFastLowrateList(pcm, vbuf, coefBase,
		fastLowrateStride2Samples, 16);
#else
	int phase;
	phase = 0;
	return PolyphaseMonoFastLowrate(pcm, vbuf, coefBase, 2, &phase);
#endif
}

int PolyphaseMonoFastLowrateStride2_TEST_ACTIVE(short *pcm, int *vbuf, const int *coefBase)
{
#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE) && defined(AMIGA_FAST_REDUCED_TAPS)
	if (MP3ExperimentalReducedTapsEnabled()) {
#if defined(AMIGA_M68K_ASM_POLYPHASE)
		if (AmigaM68KPolyphaseMonoFastStride2Reduced_IsActive()) {
			AmigaM68KPolyphaseMonoFastStride2Reduced(pcm, vbuf, PolyAsmCoef(coefBase));
			return 16;
		}
#endif
		return PolyphaseMonoFastLowrateStride2Reduced(pcm, vbuf, coefBase);
	}
#endif
#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE) && defined(AMIGA_M68K_ASM_POLYPHASE)
	if (AmigaM68KPolyphaseMonoFastStride2_IsActive()) {
		AmigaM68KPolyphaseMonoFastStride2(pcm, vbuf, PolyAsmCoef(coefBase));
		return 16;
	}
#endif
	return PolyphaseMonoFastLowrateStride2_C_REFERENCE(pcm, vbuf, coefBase);
}

int PolyphaseMonoFastLowrateStride4_C_REFERENCE(short *pcm, int *vbuf, const int *coefBase)
{
#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE)
	return PolyphaseMonoFastLowrateStride4(pcm, vbuf, coefBase, 0);
#else
	int phase;
	phase = 0;
	return PolyphaseMonoFastLowrate(pcm, vbuf, coefBase, 4, &phase);
#endif
}

int PolyphaseMonoFastLowrateStride4_TEST_ACTIVE(short *pcm, int *vbuf, const int *coefBase)
{
#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE) && defined(AMIGA_M68K_ASM_POLYPHASE)
	if (MonoFastPolyphaseStride4_Amiga_m68k_IsActive()) {
		MonoFastPolyphaseStride4_Amiga_m68k(pcm, vbuf, PolyAsmCoef(coefBase));
		return 8;
	}
#endif
	return PolyphaseMonoFastLowrateStride4_C_REFERENCE(pcm, vbuf, coefBase);
}


int PolyphaseStereoFastLowrateStride4_C_REFERENCE(short *pcm, int *vbuf,
	const int *coefBase, int phase)
{
#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE)
	return PolyphaseStereoFastLowrateList(pcm, vbuf, coefBase,
		fastLowrateStride4Samples[phase], 8);
#else
	return PolyphaseStereoFastLowrate(pcm, vbuf, coefBase, 4, &phase);
#endif
}

int PolyphaseStereoFastLowrateStride4_TEST_ACTIVE(short *pcm, int *vbuf,
	const int *coefBase, int phase)
{
#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE) && defined(AMIGA_M68K_ASM_POLYPHASE)
	if (StereoFastPolyphaseStride4Half_Amiga_m68k_IsActive()) {
		StereoFastPolyphaseStride4Half_Amiga_m68k(pcm, vbuf, PolyAsmCoef(coefBase), phase);
		return 16;
	}
#endif
	return PolyphaseStereoFastLowrateStride4_C_REFERENCE(pcm, vbuf, coefBase, phase);
}

int PolyphaseStereoFastLowrateStride4Reduced_C_REFERENCE(short *pcm, int *vbuf,
	const int *coefBase, int phase)
{
#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE) && defined(AMIGA_FAST_REDUCED_TAPS)
	return PolyphaseStereoFastLowrateStride4Reduced(pcm, vbuf, coefBase, phase);
#else
	(void)pcm;
	(void)vbuf;
	(void)coefBase;
	(void)phase;
	return 0;
#endif
}


int PolyphaseStereoFastLowrateStride5_C_REFERENCE(short *pcm, int *vbuf,
	const int *coefBase, int phase)
{
#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE)
	return PolyphaseStereoFastLowrateStride5(pcm, vbuf, coefBase, phase);
#else
	return PolyphaseStereoFastLowrate(pcm, vbuf, coefBase, 5, &phase);
#endif
}

int PolyphaseStereoFastLowrateStride5_TEST_ACTIVE(short *pcm, int *vbuf,
	const int *coefBase, int phase)
{
#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE) && defined(AMIGA_M68K_ASM_POLYPHASE)
	if (StereoFastPolyphaseStride5_Amiga_m68k_IsActive())
		return StereoFastPolyphaseStride5_Amiga_m68k(pcm, vbuf, PolyAsmCoef(coefBase), phase);
#endif
	return PolyphaseStereoFastLowrateStride5_C_REFERENCE(pcm, vbuf, coefBase, phase);
}

int PolyphaseMonoFastLowrateStride2Reduced_C_REFERENCE(short *pcm, int *vbuf,
	const int *coefBase)
{
#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE) && defined(AMIGA_FAST_REDUCED_TAPS)
	return PolyphaseMonoFastLowrateStride2Reduced(pcm, vbuf, coefBase);
#else
	(void)pcm;
	(void)vbuf;
	(void)coefBase;
	return 0;
#endif
}

int PolyphaseMonoFastLowrateStride2Reduced_TEST_ACTIVE(short *pcm, int *vbuf,
	const int *coefBase)
{
#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE) && defined(AMIGA_FAST_REDUCED_TAPS)
#if defined(AMIGA_M68K_ASM_POLYPHASE)
	if (AmigaM68KPolyphaseMonoFastStride2Reduced_IsActive()) {
		AmigaM68KPolyphaseMonoFastStride2Reduced(pcm, vbuf, PolyAsmCoef(coefBase));
		return 16;
	}
#endif
	return PolyphaseMonoFastLowrateStride2Reduced(pcm, vbuf, coefBase);
#else
	(void)pcm;
	(void)vbuf;
	(void)coefBase;
	return 0;
#endif
}

int PolyphaseMonoFastLowrateStride4Reduced_TEST_ACTIVE(short *pcm, int *vbuf,
	const int *coefBase, int phase)
{
#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE) && defined(AMIGA_FAST_REDUCED_TAPS)
	return PolyphaseMonoFastLowrateStride4Reduced(pcm, vbuf, coefBase, phase);
#else
	(void)pcm;
	(void)vbuf;
	(void)coefBase;
	(void)phase;
	return 0;
#endif
}

int PolyphaseStereoFastLowrateStride2Reduced_C_REFERENCE(short *pcm, int *vbuf,
	const int *coefBase)
{
#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE) && defined(AMIGA_FAST_REDUCED_TAPS)
	return PolyphaseStereoFastLowrateStride2Reduced(pcm, vbuf, coefBase);
#else
	(void)pcm;
	(void)vbuf;
	(void)coefBase;
	return 0;
#endif
}

int PolyphaseStereoFastLowrateStride2Reduced_TEST_ACTIVE(short *pcm, int *vbuf,
	const int *coefBase)
{
#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE) && defined(AMIGA_FAST_REDUCED_TAPS)
	#if defined(AMIGA_M68K_ASM_POLYPHASE)
	if (StereoFastPolyphaseStride2Phase0Reduced_Amiga_m68k_IsActive()) {
		StereoFastPolyphaseStride2Phase0Reduced_Amiga_m68k(pcm, vbuf, PolyAsmCoef(coefBase));
		return 32;
	}
#endif
	return PolyphaseStereoFastLowrateStride2Reduced(pcm, vbuf, coefBase);
#else
	(void)pcm;
	(void)vbuf;
	(void)coefBase;
	return 0;
#endif
}

int PolyphaseStereoFastLowrateStride4Reduced_TEST_ACTIVE(short *pcm, int *vbuf,
	const int *coefBase, int phase)
{
#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE) && defined(AMIGA_FAST_REDUCED_TAPS)
#if defined(AMIGA_M68K_ASM_POLYPHASE)
	if (StereoFastPolyphaseStride4HalfReduced_Amiga_m68k_IsActive()) {
		StereoFastPolyphaseStride4HalfReduced_Amiga_m68k(pcm, vbuf,
			PolyAsmCoef(coefBase), phase);
		return 16;
	}
#endif
	return PolyphaseStereoFastLowrateStride4Reduced(pcm, vbuf, coefBase, phase);
#else
	(void)pcm;
	(void)vbuf;
	(void)coefBase;
	(void)phase;
	return 0;
#endif
}

int PolyphaseMonoFastLowrate(short *pcm, int *vbuf, const int *coefBase, int stride, int *phase)
{
#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE)
	int sample;
	int produced;
	int localPhase;
	short *out;

	if (stride < 2) {
		PolyphaseMono(pcm, vbuf, coefBase);
		return NBANDS;
	}

	localPhase = *phase;
	if (stride == 2 && localPhase == 0) {
		*phase = PolyphaseAdvanceLowratePhase(localPhase, stride);
#if defined(AMIGA_FAST_REDUCED_TAPS)
		if (MP3ExperimentalReducedTapsEnabled()) {
			gMonoStride2ReducedCalls++;
#if defined(AMIGA_M68K_ASM_POLYPHASE)
			if (AmigaM68KPolyphaseMonoFastStride2Reduced_IsActive()) {
				AmigaM68KPolyphaseMonoFastStride2Reduced(pcm, vbuf, PolyAsmCoef(coefBase));
				return 16;
			}
#endif
			return PolyphaseMonoFastLowrateStride2Reduced(pcm, vbuf, coefBase);
		}
#endif
#if defined(AMIGA_M68K_ASM_POLYPHASE)
		if (AmigaM68KPolyphaseMonoFastStride2_IsActive()) {
			gMonoStride2AsmCalls++;
			AmigaM68KPolyphaseMonoFastStride2(pcm, vbuf, PolyAsmCoef(coefBase));
			return 16;
		}
#endif
		gMonoStride2CCalls++;
		return PolyphaseMonoFastLowrateStride2(pcm, vbuf, coefBase);
	}
	if (stride == 4) {
		*phase = PolyphaseAdvanceLowratePhase(localPhase, stride);
#if defined(AMIGA_FAST_REDUCED_TAPS)
		if (MP3ExperimentalReducedTapsEnabled())
			return PolyphaseMonoFastLowrateStride4Reduced(pcm, vbuf, coefBase,
				localPhase);
#endif
#if defined(AMIGA_M68K_ASM_POLYPHASE)
		if (MonoFastPolyphaseStride4AllPhases_Amiga_m68k_IsActive()) {
			switch (localPhase) {
			case 0:
				MonoFastPolyphaseStride4_Amiga_m68k(pcm, vbuf, PolyAsmCoef(coefBase));
				return 8;
			case 1:
				MonoFastPolyphaseStride4Phase1_Amiga_m68k(pcm, vbuf, PolyAsmCoef(coefBase));
				return 8;
			case 2:
				MonoFastPolyphaseStride4Phase2_Amiga_m68k(pcm, vbuf, PolyAsmCoef(coefBase));
				return 8;
			default:
				MonoFastPolyphaseStride4Phase3_Amiga_m68k(pcm, vbuf, PolyAsmCoef(coefBase));
				return 8;
			}
		}
		if (localPhase == 0 && MonoFastPolyphaseStride4_Amiga_m68k_IsActive()) {
			MonoFastPolyphaseStride4_Amiga_m68k(pcm, vbuf, PolyAsmCoef(coefBase));
			return 8;
		}
#endif
		return PolyphaseMonoFastLowrateStride4(pcm, vbuf, coefBase, localPhase);
	}
	if (stride == 5) {
		int endPhase = localPhase + 2;
		if (endPhase >= 5)
			endPhase -= 5;
#if defined(AMIGA_M68K_ASM_POLYPHASE)
		if (MonoFastPolyphaseStride5_Amiga_m68k_IsActive()) {
			*phase = endPhase;
			switch (localPhase) {
			case 0:
				MonoFastPolyphaseStride5Phase0_Amiga_m68k(pcm, vbuf, PolyAsmCoef(coefBase));
				return 7;
			case 1:
				MonoFastPolyphaseStride5Phase1_Amiga_m68k(pcm, vbuf, PolyAsmCoef(coefBase));
				return 6;
			case 2:
				MonoFastPolyphaseStride5Phase2_Amiga_m68k(pcm, vbuf, PolyAsmCoef(coefBase));
				return 6;
			case 3:
				MonoFastPolyphaseStride5Phase3_Amiga_m68k(pcm, vbuf, PolyAsmCoef(coefBase));
				return 6;
			default:
				MonoFastPolyphaseStride5Phase4_Amiga_m68k(pcm, vbuf, PolyAsmCoef(coefBase));
				return 7;
			}
		}
#endif
		produced = PolyphaseMonoFastLowrateStride5(pcm, vbuf, coefBase, localPhase);
		*phase = endPhase;
		return produced;
	}
	if (stride == 3) {
		*phase = PolyphaseAdvanceLowratePhase(localPhase, stride);
#if defined(AMIGA_FAST_REDUCED_TAPS)
		if (MP3ExperimentalReducedTapsEnabled())
			return PolyphaseMonoFastLowrateStride3Reduced(pcm, vbuf, coefBase,
				localPhase);
#endif
#if defined(AMIGA_M68K_ASM_POLYPHASE)
		if (MonoFastPolyphaseStride3_Amiga_m68k_IsActive()) {
			switch (localPhase) {
			case 0:
				MonoFastPolyphaseStride3Phase0_Amiga_m68k(pcm, vbuf, PolyAsmCoef(coefBase));
				return 11;
			case 1:
				MonoFastPolyphaseStride3Phase1_Amiga_m68k(pcm, vbuf, PolyAsmCoef(coefBase));
				return 10;
			default:
				MonoFastPolyphaseStride3Phase2_Amiga_m68k(pcm, vbuf, PolyAsmCoef(coefBase));
				return 11;
			}
		}
#endif
		return PolyphaseMonoFastLowrateStride3(pcm, vbuf, coefBase, localPhase);
	}

	produced = 0;
	out = pcm;
	for (sample = 0; sample < NBANDS; sample++) {
		if (localPhase == 0) {
			PolyphaseMonoFastSample(out, sample, vbuf, coefBase);
			out++;
			produced++;
		}
		localPhase++;
		if (localPhase >= stride)
			localPhase = 0;
	}
	*phase = localPhase;
	return produced;
#else
	int fullPhase;
	int sample;
	int produced;
	short full[NBANDS];

	if (stride < 2) {
		PolyphaseMono(pcm, vbuf, coefBase);
		return NBANDS;
	}
	fullPhase = *phase;
	PolyphaseMono(full, vbuf, coefBase);
	produced = 0;
	for (sample = 0; sample < NBANDS; sample++) {
		if (fullPhase == 0)
			pcm[produced++] = full[sample];
		fullPhase++;
		if (fullPhase >= stride)
			fullPhase = 0;
	}
	*phase = fullPhase;
	return produced;
#endif
}

int PolyphaseStereoFastLowrate(short *pcm, int *vbuf, const int *coefBase, int stride, int *phase)
{
#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE)
	int sample;
	int produced;
	int localPhase;
	short *out;

	if (stride < 2) {
		PolyphaseStereo(pcm, vbuf, coefBase);
		return NBANDS * 2;
	}

	localPhase = *phase;
	if (stride == 2 && localPhase == 0) {
		*phase = PolyphaseAdvanceLowratePhase(localPhase, stride);
#if defined(AMIGA_FAST_REDUCED_TAPS)
		if (MP3ExperimentalReducedTapsEnabled()) {
			gStereoStride2ReducedCalls++;
#if defined(AMIGA_M68K_ASM_POLYPHASE)
			if (StereoFastPolyphaseStride2Phase0Reduced_Amiga_m68k_IsActive()) {
				StereoFastPolyphaseStride2Phase0Reduced_Amiga_m68k(pcm, vbuf, PolyAsmCoef(coefBase));
				return 32;
			}
#endif
			return PolyphaseStereoFastLowrateStride2Reduced(pcm, vbuf, coefBase);
		}
#endif
#if defined(AMIGA_M68K_ASM_POLYPHASE)
		if (!MP3ForceStereoStride2PolyphaseC() &&
			StereoFastPolyphaseStride2Phase0_Amiga_m68k_IsActive()) {
			gStereoStride2AsmCalls++;
			StereoFastPolyphaseStride2Phase0_Amiga_m68k(pcm, vbuf, PolyAsmCoef(coefBase));
			return 32;
		}
#endif
		gStereoStride2CCalls++;
		return PolyphaseStereoFastLowrateStride2_C(pcm, vbuf, coefBase);
	}
	if (stride == 4) {
		*phase = PolyphaseAdvanceLowratePhase(localPhase, stride);
#if defined(AMIGA_FAST_REDUCED_TAPS)
		if (MP3ExperimentalReducedTapsEnabled()) {
			gStereoStride4ReducedCalls++;
#if defined(AMIGA_M68K_ASM_POLYPHASE)
			if (StereoFastPolyphaseStride4HalfReduced_Amiga_m68k_IsActive()) {
				StereoFastPolyphaseStride4HalfReduced_Amiga_m68k(pcm, vbuf,
					PolyAsmCoef(coefBase), localPhase);
				return 16;
			}
#endif
			return PolyphaseStereoFastLowrateStride4Reduced(pcm, vbuf, coefBase,
				localPhase);
		}
#endif
#if defined(AMIGA_M68K_ASM_POLYPHASE)
		if (StereoFastPolyphaseStride4Half_Amiga_m68k_IsActive()) {
			gStereoStride4AsmCalls++;
			StereoFastPolyphaseStride4Half_Amiga_m68k(pcm, vbuf, PolyAsmCoef(coefBase),
				localPhase);
			return 16;
		}
#endif
		gStereoStride4CCalls++;
		return PolyphaseStereoFastLowrateStride4(pcm, vbuf, coefBase, localPhase);
	}
	if (stride == 5) {
#if defined(AMIGA_M68K_ASM_POLYPHASE)
		if (StereoFastPolyphaseStride5_Amiga_m68k_IsActive())
			produced = StereoFastPolyphaseStride5_Amiga_m68k(pcm, vbuf,
				PolyAsmCoef(coefBase), localPhase);
		else
#endif
			produced = PolyphaseStereoFastLowrateStride5(pcm, vbuf,
				coefBase, localPhase);
		localPhase += 2;
		if (localPhase >= 5)
			localPhase -= 5;
		*phase = localPhase;
		return produced;
	}
	if (stride == 3) {
		*phase = PolyphaseAdvanceLowratePhase(localPhase, stride);
#if defined(AMIGA_FAST_REDUCED_TAPS)
		if (MP3ExperimentalReducedTapsEnabled())
			return PolyphaseStereoFastLowrateStride3Reduced(pcm, vbuf, coefBase,
				localPhase);
#endif
#if defined(AMIGA_M68K_ASM_POLYPHASE)
		if (StereoFastPolyphaseStride3_Amiga_m68k_IsActive()) {
			switch (localPhase) {
			case 0:
				StereoFastPolyphaseStride3Phase0_Amiga_m68k(pcm, vbuf, PolyAsmCoef(coefBase));
				return 22;
			case 1:
				StereoFastPolyphaseStride3Phase1_Amiga_m68k(pcm, vbuf, PolyAsmCoef(coefBase));
				return 20;
			default:
				StereoFastPolyphaseStride3Phase2_Amiga_m68k(pcm, vbuf, PolyAsmCoef(coefBase));
				return 22;
			}
		}
#endif
		return PolyphaseStereoFastLowrateStride3(pcm, vbuf, coefBase, localPhase);
	}

	produced = 0;
	out = pcm;
	for (sample = 0; sample < NBANDS; sample++) {
		if (localPhase == 0) {
			PolyphaseStereoFastSample(out, sample, vbuf, coefBase);
			out += 2;
			produced++;
		}
		localPhase++;
		if (localPhase >= stride)
			localPhase = 0;
	}
	*phase = localPhase;
	return produced * 2;
#else
	int fullPhase;
	int sample;
	int produced;
	short full[NBANDS * 2];

	if (stride < 2) {
		PolyphaseStereo(pcm, vbuf, coefBase);
		return NBANDS * 2;
	}
	fullPhase = *phase;
	PolyphaseStereo(full, vbuf, coefBase);
	produced = 0;
	for (sample = 0; sample < NBANDS; sample++) {
		if (fullPhase == 0) {
			pcm[produced * 2] = full[sample * 2];
			pcm[produced * 2 + 1] = full[sample * 2 + 1];
			produced++;
		}
		fullPhase++;
		if (fullPhase >= stride)
			fullPhase = 0;
	}
	*phase = fullPhase;
	return produced * 2;
#endif
}

#if defined(AMIGA_M68K) && defined(AMIGA_M68K_POLYPHASE_68060)
#include "polyphase_68060.h"
#endif

void PolyphaseMono(short *pcm, int *vbuf, const int *coefBase)
{
#if defined(AMIGA_M68K) && defined(AMIGA_M68K_POLYPHASE_68060) && !defined(AMIGA_FAST_POLYPHASE)
	/* poly060 alone: dedicated 68060 full-rate kernel (hardware-only multiplies).
	 * When AMIGA_FAST_POLYPHASE is also enabled the fast/lowrate path below is
	 * used instead -- it handles stride downsampling and reduced taps and is now
	 * 68060-clean too (PolyphaseMulShift26 uses MulShift68060). Selected first so
	 * every established 030 branch is left byte-for-byte unchanged when off. */
	PolyphaseMono68060(pcm, vbuf, coefBase);
#elif defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE) && defined(AMIGA_M68K_ASM_POLYPHASE)
	if (AmigaM68KPolyphaseMonoFast_IsActive()) {
		AmigaM68KPolyphaseMonoFast(pcm, vbuf, PolyAsmCoef(coefBase));
		return;
	}
	PolyphaseMonoFast_C_REFERENCE(pcm, vbuf, coefBase);
#elif defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE)
	PolyphaseMonoFast_C_REFERENCE(pcm, vbuf, coefBase);
#else
	PolyphaseMonoReference(pcm, vbuf, coefBase);
#endif
}

void PolyphaseStereo(short *pcm, int *vbuf, const int *coefBase)
{
#if defined(AMIGA_M68K) && defined(AMIGA_M68K_POLYPHASE_68060) && !defined(AMIGA_FAST_POLYPHASE)
	PolyphaseStereo68060(pcm, vbuf, coefBase);
	return;
#endif
#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE) && defined(AMIGA_M68K_ASM_POLYPHASE)
	if (AmigaM68KPolyphaseFullRateStereo) {
		AmigaM68KPolyphaseFullRateStereo(pcm, vbuf, PolyAsmCoef(coefBase));
		return;
	}
#endif
#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE)
	PolyphaseStereoFast(pcm, vbuf, coefBase);
#else
	PolyphaseStereoReference(pcm, vbuf, coefBase);
#endif
}
