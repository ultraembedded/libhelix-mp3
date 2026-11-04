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
 * coder.h - private, implementation-specific header file
 **************************************************************************************/

#ifndef _CODER_H
#define _CODER_H

#include "mp3common.h"

#if defined(ASSERT)
#undef ASSERT
#endif
#if defined(_WIN32) && defined(_M_IX86) && (defined (_DEBUG) || defined (REL_ENABLE_ASSERTS))
#define ASSERT(x) if (!(x)) __asm int 3;
#else
#define ASSERT(x) /* do nothing */
#endif

#ifndef MAX
#define MAX(a,b)	((a) > (b) ? (a) : (b))
#endif

#ifndef MIN
#define MIN(a,b)	((a) < (b) ? (a) : (b))
#endif

/* clip to range [-2^n, 2^n - 1] */
#define CLIP_2N(y, n) { \
	int sign = (y) >> 31;  \
	if (sign != (y) >> (n))  { \
		(y) = sign ^ ((1 << (n)) - 1); \
	} \
}

#define SIBYTES_MPEG1_MONO		17
#define SIBYTES_MPEG1_STEREO	32
#define SIBYTES_MPEG2_MONO		 9
#define SIBYTES_MPEG2_STEREO	17

/* number of fraction bits for pow43Tab (see comments there) */
#define POW43_FRACBITS_LOW		22
#define POW43_FRACBITS_HIGH		12

#define DQ_FRACBITS_OUT			25	/* number of fraction bits in output of dequant */
#define	IMDCT_SCALE				2	/* additional scaling (by sqrt(2)) for fast IMDCT36 */

#define	HUFF_PAIRTABS			32
#define BLOCK_SIZE				18
#define	NBANDS					32
#define MAX_REORDER_SAMPS		((192-126)*3)		/* largest critical band for short blocks (see sfBandTable) */
#define VBUF_LENGTH				(17 * 2 * NBANDS)	/* for double-sized vbuf FIFO */

/* additional external symbols to name-mangle for static linking */
#define	SetBitstreamPointer	STATNAME(SetBitstreamPointer)
#define BitstreamRefillSelftest STATNAME(BitstreamRefillSelftest)
#define	CalcBitsUsed		STATNAME(CalcBitsUsed)
#define	DequantChannel		STATNAME(DequantChannel)
#define DequantBlock_C_REFERENCE STATNAME(DequantBlock_C_REFERENCE)
#define DequantBlock_TEST_ACTIVE STATNAME(DequantBlock_TEST_ACTIVE)
#define DequantBlock_HAS_AMIGA_M68K_ASM_RUNTIME STATNAME(DequantBlock_HAS_AMIGA_M68K_ASM_RUNTIME)
#define	MidSideProc			STATNAME(MidSideProc)
#define	IntensityProcMPEG1	STATNAME(IntensityProcMPEG1)
#define	IntensityProcMPEG2	STATNAME(IntensityProcMPEG2)
#define IntensityScaleRun_C_REFERENCE STATNAME(IntensityScaleRun_C_REFERENCE)
#define IntensityScaleRun1_TEST_ACTIVE STATNAME(IntensityScaleRun1_TEST_ACTIVE)
#define IntensityScaleRun3_TEST_ACTIVE STATNAME(IntensityScaleRun3_TEST_ACTIVE)
#define IntensityScaleRun_HAS_AMIGA_M68K_ASM_RUNTIME STATNAME(IntensityScaleRun_HAS_AMIGA_M68K_ASM_RUNTIME)
#define PolyphaseMono		STATNAME(PolyphaseMono)
#define PolyphaseStereo		STATNAME(PolyphaseStereo)
#define PolyphaseMonoFast_C_REFERENCE STATNAME(PolyphaseMonoFast_C_REFERENCE)
#define PolyphaseMonoFast_TEST_ACTIVE STATNAME(PolyphaseMonoFast_TEST_ACTIVE)
#define PolyphaseMonoFast_HAS_AMIGA_M68K_ASM_RUNTIME STATNAME(PolyphaseMonoFast_HAS_AMIGA_M68K_ASM_RUNTIME)
#define PolyphaseMonoFastLowrateStride2_C_REFERENCE STATNAME(PolyphaseMonoFastLowrateStride2_C_REFERENCE)
#define PolyphaseMonoFastLowrateStride2_TEST_ACTIVE STATNAME(PolyphaseMonoFastLowrateStride2_TEST_ACTIVE)
#define PolyphaseMonoFastLowrateStride2_HAS_AMIGA_M68K_ASM_RUNTIME STATNAME(PolyphaseMonoFastLowrateStride2_HAS_AMIGA_M68K_ASM_RUNTIME)
#define PolyphaseMonoFastLowrateStride4_C_REFERENCE STATNAME(PolyphaseMonoFastLowrateStride4_C_REFERENCE)
#define PolyphaseMonoFastLowrateStride4_TEST_ACTIVE STATNAME(PolyphaseMonoFastLowrateStride4_TEST_ACTIVE)
#define PolyphaseMonoFastLowrateStride4_HAS_AMIGA_M68K_ASM_RUNTIME STATNAME(PolyphaseMonoFastLowrateStride4_HAS_AMIGA_M68K_ASM_RUNTIME)
#define PolyphaseStereoFastLowrateStride2_C_REFERENCE STATNAME(PolyphaseStereoFastLowrateStride2_C_REFERENCE)
#define PolyphaseStereoFastLowrateStride2_TEST_ACTIVE STATNAME(PolyphaseStereoFastLowrateStride2_TEST_ACTIVE)
#define PolyphaseStereoFastLowrateStride2Reduced_C_REFERENCE STATNAME(PolyphaseStereoFastLowrateStride2Reduced_C_REFERENCE)
#define PolyphaseStereoFastLowrateStride2Reduced_TEST_ACTIVE STATNAME(PolyphaseStereoFastLowrateStride2Reduced_TEST_ACTIVE)
#define PolyphaseStereoFastLowrateStride2Reduced_HAS_AMIGA_M68K_ASM_RUNTIME STATNAME(PolyphaseStereoFastLowrateStride2Reduced_HAS_AMIGA_M68K_ASM_RUNTIME)
#define PolyphaseMonoFastLowrateStride2Reduced_C_REFERENCE STATNAME(PolyphaseMonoFastLowrateStride2Reduced_C_REFERENCE)
#define PolyphaseMonoFastLowrateStride2Reduced_TEST_ACTIVE STATNAME(PolyphaseMonoFastLowrateStride2Reduced_TEST_ACTIVE)
#define PolyphaseMonoFastLowrateStride2Reduced_HAS_AMIGA_M68K_ASM_RUNTIME STATNAME(PolyphaseMonoFastLowrateStride2Reduced_HAS_AMIGA_M68K_ASM_RUNTIME)
#define StereoFastPolyphaseStride2_HAS_AMIGA_M68K_ASM_RUNTIME STATNAME(StereoFastPolyphaseStride2_HAS_AMIGA_M68K_ASM_RUNTIME)
#define StereoFastPolyphaseStride4_HAS_AMIGA_M68K_ASM_RUNTIME STATNAME(StereoFastPolyphaseStride4_HAS_AMIGA_M68K_ASM_RUNTIME)
#define PolyphaseStereoFastLowrateStride4_C_REFERENCE STATNAME(PolyphaseStereoFastLowrateStride4_C_REFERENCE)
#define PolyphaseStereoFastLowrateStride4_TEST_ACTIVE STATNAME(PolyphaseStereoFastLowrateStride4_TEST_ACTIVE)
#define PolyphaseStereoFastLowrateStride5_C_REFERENCE STATNAME(PolyphaseStereoFastLowrateStride5_C_REFERENCE)
#define PolyphaseStereoFastLowrateStride5_TEST_ACTIVE STATNAME(PolyphaseStereoFastLowrateStride5_TEST_ACTIVE)
#define StereoFastPolyphaseStride5_Amiga_m68k_IsActive STATNAME(StereoFastPolyphaseStride5_Amiga_m68k_IsActive)
#define PolyphaseMonoFastLowrateStride4Reduced_TEST_ACTIVE STATNAME(PolyphaseMonoFastLowrateStride4Reduced_TEST_ACTIVE)
#define PolyphaseStereoFastLowrateStride4Reduced_TEST_ACTIVE STATNAME(PolyphaseStereoFastLowrateStride4Reduced_TEST_ACTIVE)
#define PolyphaseStereoFastLowrateStride4Reduced_C_REFERENCE STATNAME(PolyphaseStereoFastLowrateStride4Reduced_C_REFERENCE)
#define PolyphaseStereoFastLowrateStride4Reduced_HAS_AMIGA_M68K_ASM_RUNTIME STATNAME(PolyphaseStereoFastLowrateStride4Reduced_HAS_AMIGA_M68K_ASM_RUNTIME)
#define StereoFastPolyphaseStride4Half_Amiga_m68k_IsActive STATNAME(StereoFastPolyphaseStride4Half_Amiga_m68k_IsActive)
#define StereoFastPolyphaseStride4HalfReduced_Amiga_m68k_IsActive STATNAME(StereoFastPolyphaseStride4HalfReduced_Amiga_m68k_IsActive)
#define AmigaM68KPolyphaseMonoFast_IsActive STATNAME(AmigaM68KPolyphaseMonoFast_IsActive)
#define AmigaM68KPolyphaseMonoFastStride2_IsActive STATNAME(AmigaM68KPolyphaseMonoFastStride2_IsActive)
#define AmigaM68KPolyphaseMonoFastStride2Reduced_IsActive STATNAME(AmigaM68KPolyphaseMonoFastStride2Reduced_IsActive)
#define MonoFastPolyphaseStride4_Amiga_m68k_IsActive STATNAME(MonoFastPolyphaseStride4_Amiga_m68k_IsActive)
#define MonoFastPolyphaseStride4AllPhases_Amiga_m68k_IsActive STATNAME(MonoFastPolyphaseStride4AllPhases_Amiga_m68k_IsActive)
#define MonoFastPolyphaseStride3_Amiga_m68k_IsActive STATNAME(MonoFastPolyphaseStride3_Amiga_m68k_IsActive)
#define StereoFastPolyphaseStride3_Amiga_m68k_IsActive STATNAME(StereoFastPolyphaseStride3_Amiga_m68k_IsActive)
#define MonoFastPolyphaseStride5_Amiga_m68k_IsActive STATNAME(MonoFastPolyphaseStride5_Amiga_m68k_IsActive)
#define StereoFastPolyphaseStride2Phase0Reduced_Amiga_m68k_IsActive STATNAME(StereoFastPolyphaseStride2Phase0Reduced_Amiga_m68k_IsActive)
#define FDCT32				STATNAME(FDCT32)
#define FDCT32Half			STATNAME(FDCT32Half)
#define FDCT32HalfSparse16		STATNAME(FDCT32HalfSparse16)
#define FDCT32LowBands			STATNAME(FDCT32LowBands)
#define FDCT32Quarter			STATNAME(FDCT32Quarter)
#define FDCT32_C_REFERENCE	STATNAME(FDCT32_C_REFERENCE)
#define FDCT32_HAS_AMIGA_M68K_ASM_RUNTIME STATNAME(FDCT32_HAS_AMIGA_M68K_ASM_RUNTIME)
#define FDCT32Half_TEST_ACTIVE STATNAME(FDCT32Half_TEST_ACTIVE)
#define FDCT32Half_AMIGA_M68K_ASM_RUNTIME STATNAME(FDCT32Half_AMIGA_M68K_ASM_RUNTIME)
#define FDCT32HalfSparse16Selftest STATNAME(FDCT32HalfSparse16Selftest)
#define AntiAlias_C_REFERENCE STATNAME(AntiAlias_C_REFERENCE)
#define AntiAlias_TEST_ACTIVE STATNAME(AntiAlias_TEST_ACTIVE)
#define AntiAlias_HAS_AMIGA_M68K_ASM_RUNTIME STATNAME(AntiAlias_HAS_AMIGA_M68K_ASM_RUNTIME)
#define IMDCT36_C_REFERENCE STATNAME(IMDCT36_C_REFERENCE)
#define IMDCT36_TEST_ACTIVE STATNAME(IMDCT36_TEST_ACTIVE)
#define IMDCT36_HAS_AMIGA_M68K_ASM_RUNTIME STATNAME(IMDCT36_HAS_AMIGA_M68K_ASM_RUNTIME)
#define IMDCTThinOutputSelftest STATNAME(IMDCTThinOutputSelftest)
#define IMDCTSubbandCapSelftest STATNAME(IMDCTSubbandCapSelftest)
#define AntiAliasSubbandCapSelftest STATNAME(AntiAliasSubbandCapSelftest)
#define IMDCT36AsmGeneralPathSelftest STATNAME(IMDCT36AsmGeneralPathSelftest)
#define MP3FastLowrateEffectiveActiveSubbands STATNAME(MP3FastLowrateEffectiveActiveSubbands)
#define DequantSubbandCapSelftest STATNAME(DequantSubbandCapSelftest)
#define CollapseStereoToMonoSelftest STATNAME(CollapseStereoToMonoSelftest)
#define MidSideProcSubbandCapSelftest STATNAME(MidSideProcSubbandCapSelftest)
#define CollapseStereoToMonoSubbandCapSelftest STATNAME(CollapseStereoToMonoSubbandCapSelftest)
#define IntensityProcSubbandCapSelftest STATNAME(IntensityProcSubbandCapSelftest)
#define DecodeHuffmanPairs_C_REFERENCE STATNAME(DecodeHuffmanPairs_C_REFERENCE)
#define DecodeHuffmanPairs_TEST_ACTIVE STATNAME(DecodeHuffmanPairs_TEST_ACTIVE)
#define DecodeHuffmanPairs_HAS_AMIGA_M68K_ASM_RUNTIME STATNAME(DecodeHuffmanPairs_HAS_AMIGA_M68K_ASM_RUNTIME)
#define DecodeHuffmanPairs_AMIGA_M68K_ASM_NOTE STATNAME(DecodeHuffmanPairs_AMIGA_M68K_ASM_NOTE)

#define	ISFMpeg1			STATNAME(ISFMpeg1)
#define	ISFMpeg2			STATNAME(ISFMpeg2)
#define	ISFIIP				STATNAME(ISFIIP)
#define uniqueIDTab			STATNAME(uniqueIDTab)
#define	coef32				STATNAME(coef32)
#define	polyCoef			STATNAME(polyCoef)
#define	csa					STATNAME(csa)
#define	imdctWin			STATNAME(imdctWin)

#define	huffTable			STATNAME(huffTable)
#define	huffTabOffset		STATNAME(huffTabOffset)
#define	huffTabLookup		STATNAME(huffTabLookup)
#define	quadTable			STATNAME(quadTable)
#define	quadTabOffset		STATNAME(quadTabOffset)
#define	quadTabMaxBits		STATNAME(quadTabMaxBits)

/* map these to the corresponding 2-bit values in the frame header */
typedef enum {
	Stereo = 0x00,	/* two independent channels, but L and R frames might have different # of bits */
	Joint = 0x01,	/* coupled channels - layer III: mix of M-S and intensity, Layers I/II: intensity and direct coding only */
	Dual = 0x02,	/* two independent channels, L and R always have exactly 1/2 the total bitrate */
	Mono = 0x03		/* one channel */
} StereoMode;

typedef struct _BitStreamInfo {
	unsigned char *bytePtr;
	unsigned int iCache;
	int cachedBits;
	int nBytes;
} BitStreamInfo;

/**************************************************************************************
 * RefillBitstreamCache / GetBits live here (rather than bitstream.c) as
 * static __inline so every translation unit that reads bits (bitstream.c
 * itself for header/side-info fields, scalfact.c for scale factors) inlines
 * the bit-cache logic at the call site instead of paying a real call/return
 * per bit read - GetBits alone has dozens of call sites across those two
 * files. Semantics are unchanged from the original bitstream.c
 * implementation; BitstreamRefillSelftest() in bitstream.c still exercises
 * RefillBitstreamCache4_C_REFERENCE/_TEST_ACTIVE directly to compare them.
 **************************************************************************************/
#if defined(AMIGA_M68K) && defined(AMIGA_M68K_ASM) && defined(__GNUC__) \
	&& (defined(__mc68020__) || defined(__mc68030__) || defined(__mc68040__) || defined(__mc68060__))
#define AMIGA_M68K_BITSTREAM_MOVEL 1
#endif

static __inline void RefillBitstreamCache4_C_REFERENCE(BitStreamInfo *bsi)
{
	bsi->iCache  = ((unsigned int)(*bsi->bytePtr++)) << 24;
	bsi->iCache |= ((unsigned int)(*bsi->bytePtr++)) << 16;
	bsi->iCache |= ((unsigned int)(*bsi->bytePtr++)) <<  8;
	bsi->iCache |=  (unsigned int)(*bsi->bytePtr++);
	bsi->cachedBits = 32;
	bsi->nBytes -= 4;
}

static __inline void RefillBitstreamCache4_TEST_ACTIVE(BitStreamInfo *bsi)
{
#ifdef AMIGA_M68K_BITSTREAM_MOVEL
	__asm__ volatile (
		"move.l (%0)+,%1"
		: "+a" (bsi->bytePtr), "=d" (bsi->iCache)
		: );
	bsi->cachedBits = 32;
	bsi->nBytes -= 4;
#else
	RefillBitstreamCache4_C_REFERENCE(bsi);
#endif
}

static __inline void RefillBitstreamCache(BitStreamInfo *bsi)
{
	int nBytes = bsi->nBytes;

	/* optimize for common case, independent of machine endian-ness */
	if (nBytes >= 4) {
		RefillBitstreamCache4_TEST_ACTIVE(bsi);
	} else {
		bsi->iCache = 0;
		while (nBytes--) {
			bsi->iCache |= (*bsi->bytePtr++);
			bsi->iCache <<= 8;
		}
		bsi->iCache <<= ((3 - bsi->nBytes)*8);
		bsi->cachedBits = 8*bsi->nBytes;
		bsi->nBytes = 0;
	}
}

/* nBits must be in range [0, 31], nBits outside this range masked by 0x1f
 * for speed, does not indicate error if you overrun bit buffer
 * if nBits = 0, returns 0 (useful for scalefactor unpacking)
 */
static __inline unsigned int GetBits(BitStreamInfo *bsi, int nBits)
{
	unsigned int data, lowBits;

	nBits &= 0x1f;							/* nBits mod 32 to avoid unpredictable results like >> by negative amount */
	data = bsi->iCache >> (31 - nBits);		/* unsigned >> so zero-extend */
	data >>= 1;								/* do as >> 31, >> 1 so that nBits = 0 works okay (returns 0) */
	bsi->iCache <<= nBits;					/* left-justify cache */
	bsi->cachedBits -= nBits;				/* how many bits have we drawn from the cache so far */

	/* if we cross an int boundary, refill the cache */
	if (bsi->cachedBits < 0) {
		lowBits = -bsi->cachedBits;
		RefillBitstreamCache(bsi);
		data |= bsi->iCache >> (32 - lowBits);		/* get the low-order bits */

		bsi->cachedBits -= lowBits;			/* how many bits have we drawn from the cache so far */
		bsi->iCache <<= lowBits;			/* left-justify cache */
	}

	return data;
}

typedef struct _FrameHeader {
    MPEGVersion ver;	/* version ID */
    int layer;			/* layer index (1, 2, or 3) */
    int crc;			/* CRC flag: 0 = disabled, 1 = enabled */
    int brIdx;			/* bitrate index (0 - 15) */
    int srIdx;			/* sample rate index (0 - 2) */
    int paddingBit;		/* padding flag: 0 = no padding, 1 = single pad byte */
    int privateBit;		/* unused */
    StereoMode sMode;	/* mono/stereo mode */
    int modeExt;		/* used to decipher joint stereo mode */
    int copyFlag;		/* copyright flag: 0 = no, 1 = yes */
    int origFlag;		/* original flag: 0 = copy, 1 = original */
    int emphasis;		/* deemphasis mode */
    int CRCWord;		/* CRC word (16 bits, 0 if crc not enabled) */

	const SFBandTable *sfBand;
} FrameHeader;

typedef struct _SideInfoSub {
    int part23Length;		/* number of bits in main data */ 
    int nBigvals;			/* 2x this = first set of Huffman cw's (maximum amplitude can be > 1) */
    int globalGain;			/* overall gain for dequantizer */
    int sfCompress;			/* unpacked to figure out number of bits in scale factors */
    int winSwitchFlag;		/* window switching flag */
    int blockType;			/* block type */
    int mixedBlock;			/* 0 = regular block (all short or long), 1 = mixed block */
    int tableSelect[3];		/* index of Huffman tables for the big values regions */
    int subBlockGain[3];	/* subblock gain offset, relative to global gain */
    int region0Count;		/* 1+region0Count = num scale factor bands in first region of bigvals */
    int region1Count;		/* 1+region1Count = num scale factor bands in second region of bigvals */
    int preFlag;			/* for optional high frequency boost */
    int sfactScale;			/* scaling of the scalefactors */
    int count1TableSelect;	/* index of Huffman table for quad codewords */
} SideInfoSub;

typedef struct _SideInfo {
	int mainDataBegin;
	int privateBits;
	int scfsi[MAX_NCHAN][MAX_SCFBD];				/* 4 scalefactor bands per channel */
	
	SideInfoSub	sis[MAX_NGRAN][MAX_NCHAN];
} SideInfo;

typedef struct {
    int cbType;		/* pure long = 0, pure short = 1, mixed = 2 */
    int cbEndS[3];	/* number nonzero short cb's, per subbblock */
	int cbEndSMax;	/* max of cbEndS[] */
    int cbEndL;		/* number nonzero long cb's  */
} CriticalBandInfo;

typedef struct _DequantInfo {
	int workBuf[MAX_REORDER_SAMPS];		/* workbuf for reordering short blocks */
	CriticalBandInfo cbi[MAX_NCHAN];	/* filled in dequantizer, used in joint stereo reconstruction */
} DequantInfo;

typedef struct _HuffmanInfo {
	int huffDecBuf[MAX_NCHAN][MAX_NSAMP];		/* used both for decoded Huffman values and dequantized coefficients */
	int nonZeroBound[MAX_NCHAN];				/* number of coeffs in huffDecBuf[ch] which can be > 0 */
	int gb[MAX_NCHAN];							/* minimum number of guard bits in huffDecBuf[ch] */
} HuffmanInfo;

typedef enum _HuffTabType {
	noBits,
	oneShot,
	loopNoLinbits,
	loopLinbits,
	quadA,
	quadB,
	invalidTab
} HuffTabType;

typedef struct _HuffTabLookup {
	int	linBits;
	HuffTabType tabType;
} HuffTabLookup;

typedef struct _IMDCTInfo {
	int outBuf[MAX_NCHAN][BLOCK_SIZE][NBANDS];	/* output of IMDCT */	
	int overBuf[MAX_NCHAN][MAX_NSAMP / 2];		/* overlap-add buffer (by symmetry, only need 1/2 size) */
	int numPrevIMDCT[MAX_NCHAN];				/* how many IMDCT's calculated in this channel on prev. granule */
	int prevType[MAX_NCHAN];
	int prevWinSwitch[MAX_NCHAN];
	int gb[MAX_NCHAN];
} IMDCTInfo;

typedef struct _BlockCount {
	int nBlocksLong;
	int nBlocksTotal;
	int nBlocksPrev; 
	int prevType;
	int prevWinSwitch;
	int currWinSwitch;
	int gbIn;
	int gbOut;
	int imdctThinActive;
	int imdctThinStride;
	int imdctThinPhase;
	int subbandCapActive;
	int activeSubbands;
} BlockCount;

/* max bits in scalefactors = 5, so use char's to save space */
typedef struct _ScaleFactorInfoSub {
	char l[23];            /* [band] */
	char s[13][3];         /* [band][window] */
} ScaleFactorInfoSub;  

/* used in MPEG 2, 2.5 intensity (joint) stereo only */
typedef struct _ScaleFactorJS {
	int intensityScale;		
	int	slen[4];
	int	nr[4];
} ScaleFactorJS;

typedef struct _ScaleFactorInfo {
	ScaleFactorInfoSub sfis[MAX_NGRAN][MAX_NCHAN];
	ScaleFactorJS sfjs;
} ScaleFactorInfo;

/* NOTE - could get by with smaller vbuf if memory is more important than speed
 *  (in Subband, instead of replicating each block in FDCT32 you would do a memmove on the
 *   last 15 blocks to shift them down one, a hardware style FIFO)
 */ 
typedef struct _SubbandInfo {
	int vbuf[MAX_NCHAN * VBUF_LENGTH];		/* vbuf for fast DCT-based synthesis PQMF - double size for speed (no modulo indexing) */
	int vindex;								/* internal index for tracking position in vbuf */
} SubbandInfo;

/* bitstream.c */
void SetBitstreamPointer(BitStreamInfo *bsi, int nBytes, unsigned char *buf);
int BitstreamRefillSelftest(void);
int CalcBitsUsed(BitStreamInfo *bsi, unsigned char *startBuf, int startOffset);

/* dequant.c, dqchan.c, stproc.c */
int DequantChannel(int *sampleBuf, int *workBuf, int *nonZeroBound, FrameHeader *fh, SideInfoSub *sis,
					ScaleFactorInfoSub *sfis, CriticalBandInfo *cbi, int subbandCapSampleLimit);
int DequantBlock_C_REFERENCE(int *inbuf, int *outbuf, int num, int scale);
int DequantBlock_TEST_ACTIVE(int *inbuf, int *outbuf, int num, int scale);
int DequantBlock_HAS_AMIGA_M68K_ASM_RUNTIME(void);
void MidSideProc(int x[MAX_NCHAN][MAX_NSAMP], int nSamps, int mOut[2]);
void IntensityProcMPEG1(int x[MAX_NCHAN][MAX_NSAMP], int nSamps, FrameHeader *fh, ScaleFactorInfoSub *sfis, 
						CriticalBandInfo *cbi, int midSideFlag, int mixFlag, int mOut[2]);
void IntensityProcMPEG2(int x[MAX_NCHAN][MAX_NSAMP], int nSamps, FrameHeader *fh, ScaleFactorInfoSub *sfis, 
						CriticalBandInfo *cbi, ScaleFactorJS *sfjs, int midSideFlag, int mixFlag, int mOut[2]);
void IntensityScaleRun_C_REFERENCE(int *xL, int *xR, int fl, int fr,
	int count, int stride, int *mOutL, int *mOutR);
void IntensityScaleRun1_TEST_ACTIVE(int *xL, int *xR, int fl, int fr,
	int count, int *mOutL, int *mOutR);
void IntensityScaleRun3_TEST_ACTIVE(int *xL, int *xR, int fl, int fr,
	int count, int *mOutL, int *mOutR);
int IntensityScaleRun_HAS_AMIGA_M68K_ASM_RUNTIME(void);

/* dct32.c */
// about 1 ms faster in RAM, but very large
void FDCT32(int *x, int *d, int offset, int oddBlock, int gb);
void FDCT32Half(int *x, int *d, int offset, int oddBlock, int gb);
void FDCT32HalfSparse16(int *x, int *d, int offset, int oddBlock, int gb);
void FDCT32LowBands(int *x, int *d, int offset, int oddBlock, int gb);
void FDCT32Quarter(int *x, int *d, int offset, int oddBlock, int gb, int phase, int stride);
void FDCT32FastLowrate(int *x, int *d, int offset, int oddBlock, int gb,
	int stride, int phase);
void FDCT32_C_REFERENCE(int *x, int *d, int offset, int oddBlock, int gb);
int FDCT32_HAS_AMIGA_M68K_ASM_RUNTIME(void);
void FDCT32Half_TEST_ACTIVE(int *x, int *d, int offset, int oddBlock, int gb);
int FDCT32Half_AMIGA_M68K_ASM_RUNTIME(void);
int FDCT32HalfSparse16Selftest(void);

/* imdct.c selftest hooks */
int IMDCT36_C_REFERENCE(int *xCurr, int *xPrev, int *y, int btCurr, int btPrev, int blockIdx, int gb);
int IMDCT36_TEST_ACTIVE(int *xCurr, int *xPrev, int *y, int btCurr, int btPrev, int blockIdx, int gb);
int IMDCT36_HAS_AMIGA_M68K_ASM_RUNTIME(void);
int IMDCTThinOutputSelftest(void);
int IMDCTSubbandCapSelftest(void);
int AntiAliasSubbandCapSelftest(void);
int IMDCT36AsmGeneralPathSelftest(void);
int MP3FastLowrateEffectiveActiveSubbands(const MP3DecInfo *mp3DecInfo);
int DequantSubbandCapSelftest(void);
int CollapseStereoToMonoSelftest(void);
int MidSideProcSubbandCapSelftest(void);
int CollapseStereoToMonoSubbandCapSelftest(void);
int IntensityProcSubbandCapSelftest(void);

/* huffman.c selftest hooks */
int DecodeHuffmanPairs_C_REFERENCE(int *xy, int nVals, int tabIdx, int bitsLeft, unsigned char *buf, int bitOffset);
int DecodeHuffmanPairs_TEST_ACTIVE(int *xy, int nVals, int tabIdx, int bitsLeft, unsigned char *buf, int bitOffset);
int DecodeHuffmanPairs_HAS_AMIGA_M68K_ASM_RUNTIME(void);
const char *DecodeHuffmanPairs_AMIGA_M68K_ASM_NOTE(void);

/* polyphase.c selftest hooks */
void PolyphaseMonoFast_C_REFERENCE(short *pcm, int *vbuf, const int *coefBase);
void PolyphaseMonoFast_TEST_ACTIVE(short *pcm, int *vbuf, const int *coefBase);
int PolyphaseMonoFast_HAS_AMIGA_M68K_ASM_RUNTIME(void);
int PolyphaseMonoFastLowrateStride2_C_REFERENCE(short *pcm, int *vbuf, const int *coefBase);
int PolyphaseMonoFastLowrateStride2_TEST_ACTIVE(short *pcm, int *vbuf, const int *coefBase);
int PolyphaseMonoFastLowrateStride2_HAS_AMIGA_M68K_ASM_RUNTIME(void);
int PolyphaseMonoFastLowrateStride4_C_REFERENCE(short *pcm, int *vbuf, const int *coefBase);
int PolyphaseMonoFastLowrateStride4_TEST_ACTIVE(short *pcm, int *vbuf, const int *coefBase);
int PolyphaseMonoFastLowrateStride4_HAS_AMIGA_M68K_ASM_RUNTIME(void);
int PolyphaseStereoFastLowrateStride2_C_REFERENCE(short *pcm, int *vbuf, const int *coefBase);
int PolyphaseStereoFastLowrateStride2_TEST_ACTIVE(short *pcm, int *vbuf, const int *coefBase);
int PolyphaseStereoFastLowrateStride2Reduced_C_REFERENCE(short *pcm, int *vbuf, const int *coefBase);
int PolyphaseStereoFastLowrateStride2Reduced_TEST_ACTIVE(short *pcm, int *vbuf, const int *coefBase);
int PolyphaseStereoFastLowrateStride2Reduced_HAS_AMIGA_M68K_ASM_RUNTIME(void);
int PolyphaseMonoFastLowrateStride2Reduced_C_REFERENCE(short *pcm, int *vbuf, const int *coefBase);
int PolyphaseMonoFastLowrateStride2Reduced_TEST_ACTIVE(short *pcm, int *vbuf, const int *coefBase);
int PolyphaseMonoFastLowrateStride2Reduced_HAS_AMIGA_M68K_ASM_RUNTIME(void);
int StereoFastPolyphaseStride2_HAS_AMIGA_M68K_ASM_RUNTIME(void);
int StereoFastPolyphaseStride4_HAS_AMIGA_M68K_ASM_RUNTIME(void);
void MP3SetForceStereoStride2PolyphaseC(int enabled);
int MP3ForceStereoStride2PolyphaseC(void);
void MP3GetStereoStride2PolyphaseCounters(unsigned long *asmCalls, unsigned long *cCalls,
	unsigned long *reducedCalls);
void MP3ResetStereoStride2PolyphaseCounters(void);
void MP3GetMonoStride2PolyphaseCounters(unsigned long *asmCalls, unsigned long *cCalls,
	unsigned long *reducedCalls);
void MP3ResetMonoStride2PolyphaseCounters(void);
void MP3GetStereoStride4PolyphaseCounters(unsigned long *asmCalls, unsigned long *cCalls,
	unsigned long *reducedCalls);
void MP3ResetStereoStride4PolyphaseCounters(void);
int PolyphaseStereoFastLowrateStride4_C_REFERENCE(short *pcm, int *vbuf, const int *coefBase, int phase);
int PolyphaseStereoFastLowrateStride4_TEST_ACTIVE(short *pcm, int *vbuf, const int *coefBase, int phase);
int PolyphaseStereoFastLowrateStride5_C_REFERENCE(short *pcm, int *vbuf, const int *coefBase, int phase);
int PolyphaseStereoFastLowrateStride5_TEST_ACTIVE(short *pcm, int *vbuf, const int *coefBase, int phase);
int StereoFastPolyphaseStride5_Amiga_m68k_IsActive(void);
int PolyphaseMonoFastLowrateStride4Reduced_TEST_ACTIVE(short *pcm, int *vbuf, const int *coefBase, int phase);
int PolyphaseStereoFastLowrateStride4Reduced_TEST_ACTIVE(short *pcm, int *vbuf, const int *coefBase, int phase);
int PolyphaseStereoFastLowrateStride4Reduced_C_REFERENCE(short *pcm, int *vbuf, const int *coefBase, int phase);
int PolyphaseStereoFastLowrateStride4Reduced_HAS_AMIGA_M68K_ASM_RUNTIME(void);
int StereoFastPolyphaseStride4Half_Amiga_m68k_IsActive(void);
int StereoFastPolyphaseStride4HalfReduced_Amiga_m68k_IsActive(void);
int AmigaM68KPolyphaseMonoFast_IsActive(void);
int AmigaM68KPolyphaseMonoFastStride2_IsActive(void);
int AmigaM68KPolyphaseMonoFastStride2Reduced_IsActive(void);
int MonoFastPolyphaseStride4_Amiga_m68k_IsActive(void);
int MonoFastPolyphaseStride4AllPhases_Amiga_m68k_IsActive(void);
int MonoFastPolyphaseStride3_Amiga_m68k_IsActive(void);
int StereoFastPolyphaseStride3_Amiga_m68k_IsActive(void);
int MonoFastPolyphaseStride5_Amiga_m68k_IsActive(void);
int StereoFastPolyphaseStride2Phase0Reduced_Amiga_m68k_IsActive(void);

/* hufftabs.c */
extern const HuffTabLookup huffTabLookup[HUFF_PAIRTABS];
extern const int huffTabOffset[HUFF_PAIRTABS];
extern const unsigned short huffTable[];
extern const unsigned char quadTable[64+16];
extern const int quadTabOffset[2];
extern const int quadTabMaxBits[2];

/* polyphase.c (or asmpoly.s)
 * some platforms require a C++ compile of all source files,
 * so if we're compiling C as C++ and using native assembly
 * for these functions we need to prevent C++ name mangling.
 */
#ifdef __cplusplus
extern "C" {
#endif
void PolyphaseMono(short *pcm, int *vbuf, const int *coefBase);
void PolyphaseStereo(short *pcm, int *vbuf, const int *coefBase);
int PolyphaseMonoFastLowrate(short *pcm, int *vbuf, const int *coefBase, int stride, int *phase);
int PolyphaseStereoFastLowrate(short *pcm, int *vbuf, const int *coefBase, int stride, int *phase);
#ifdef __cplusplus
}
#endif

/* trigtabs.c */
extern const int imdctWin[4][36];
extern const int ISFMpeg1[2][7];
extern const int ISFMpeg2[2][2][16];
extern const int ISFIIP[2][2];
extern const int csa[8][2];
extern const int coef32[31];
extern const int polyCoef[264];

#endif	/* _CODER_H */
