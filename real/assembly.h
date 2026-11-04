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
 * assembly.h - assembly language functions and prototypes for supported platforms
 *
 * - inline rountines with access to 64-bit multiply results 
 * - x86 (_WIN32) and ARM (ARM_ADS, _WIN32_WCE) versions included
 * - some inline functions are mix of asm and C for speed
 * - some functions are in native asm files, so only the prototype is given here
 *
 * MULSHIFT32(x, y)    signed multiply of two 32-bit integers (x and y), returns top 32 bits of 64-bit result
 * FASTABS(x)          branchless absolute value of signed integer x
 * CLZ(x)              count leading zeros in x
 * MADD64(sum, x, y)   (Windows only) sum [64-bit] += x [32-bit] * y [32-bit]
 * SHL64(sum, x, y)    (Windows only) 64-bit left shift using __int64
 * SAR64(sum, x, y)    (Windows only) 64-bit right shift using __int64
 */

#ifndef _ASSEMBLY_H
#define _ASSEMBLY_H

#if defined(AMIGA_M68K) && defined(AMIGA_FAST_POLYPHASE) && defined(AMIGA_M68K_ASM_POLYPHASE) && defined(__GNUC__)
extern void MonoFastPolyphaseStride4_Amiga_m68k(short *pcm, int *vbuf,
	const int *coefBase) __asm__("MonoFastPolyphaseStride4_Amiga_m68k")
	__attribute__((weak));
extern void StereoFastPolyphaseStride4Half_Amiga_m68k(short *pcm, int *vbuf,
	const int *coefBase, int phase)
	__asm__("StereoFastPolyphaseStride4Half_Amiga_m68k")
	__attribute__((weak));
extern void StereoFastPolyphaseStride4HalfReduced_Amiga_m68k(short *pcm, int *vbuf,
	const int *coefBase, int phase)
	__asm__("StereoFastPolyphaseStride4HalfReduced_Amiga_m68k")
	__attribute__((weak));
extern int StereoFastPolyphaseStride5_Amiga_m68k(short *pcm, int *vbuf,
	const int *coefBase, int phase)
	__asm__("StereoFastPolyphaseStride5_Amiga_m68k")
	__attribute__((weak));
#endif


#if (defined _WIN32 && !defined _WIN32_WCE) || (defined __WINS__ && defined _SYMBIAN) || defined(_OPENWAVE_SIMULATOR) || defined(WINCE_EMULATOR)    /* Symbian emulator for Ix86 */

#pragma warning( disable : 4035 )	/* complains about inline asm not returning a value */

static __inline int MULSHIFT32(int x, int y)	
{
	__asm {
		mov		eax, x
		imul	y
		mov		eax, edx
	}
}

static __inline int FASTABS(int x) 
{
	int sign;

	sign = x >> (sizeof(int) * 8 - 1);
	x ^= sign;
	x -= sign;

	return x;
}

static __inline int CLZ(int x)
{
	int numZeros;

	if (!x)
		return (sizeof(int) * 8);

	numZeros = 0;
	while (!(x & 0x80000000)) {
		numZeros++;
		x <<= 1;
	} 

	return numZeros;
}

/* MADD64, SHL64, SAR64:
 * write in assembly to avoid dependency on run-time lib for 64-bit shifts, muls
 *  (sometimes compiler thunks to function calls instead of code generating)
 * required for Symbian emulator
 */
#ifdef __CW32__
typedef long long Word64;
#else
typedef __int64 Word64;
#endif

static __inline Word64 MADD64(Word64 sum, int x, int y)
{
	unsigned int sumLo = ((unsigned int *)&sum)[0];
	int sumHi = ((int *)&sum)[1];

	__asm {
		mov		eax, x
		imul	y
		add		eax, sumLo
		adc		edx, sumHi
	}

	/* equivalent to return (sum + ((__int64)x * y)); */
}

static __inline Word64 SHL64(Word64 x, int n)
{
	unsigned int xLo = ((unsigned int *)&x)[0];
	int xHi = ((int *)&x)[1];
	unsigned char nb = (unsigned char)n;

	if (n < 32) {
		__asm {
			mov		edx, xHi
			mov		eax, xLo
			mov		cl, nb
			shld    edx, eax, cl
			shl     eax, cl
		}
	} else if (n < 64) {
		/* shl masks cl to 0x1f */
		__asm {
			mov		edx, xLo
			mov		cl, nb
			xor     eax, eax
			shl     edx, cl
		}
	} else {
		__asm {
			xor		edx, edx
			xor		eax, eax
		}
	}
}

static __inline Word64 SAR64(Word64 x, int n)
{
	unsigned int xLo = ((unsigned int *)&x)[0];
	int xHi = ((int *)&x)[1];
	unsigned char nb = (unsigned char)n;

	if (n < 32) {
		__asm {
			mov		edx, xHi
			mov		eax, xLo
			mov		cl, nb
			shrd	eax, edx, cl
			sar		edx, cl
		}
	} else if (n < 64) {
		/* sar masks cl to 0x1f */
		__asm {
			mov		edx, xHi
			mov		eax, xHi
			mov		cl, nb
			sar		edx, 31
			sar		eax, cl
		}
	} else {
		__asm {
			sar		xHi, 31
			mov		eax, xHi
			mov		edx, xHi
		}
	}
}

#elif (defined _WIN32) && (defined _WIN32_WCE)

/* use asm function for now (EVC++ 3.0 does horrible job compiling __int64 version) */
#define MULSHIFT32	xmp3_MULSHIFT32
int MULSHIFT32(int x, int y);

static __inline int FASTABS(int x) 
{
	int sign;

	sign = x >> (sizeof(int) * 8 - 1);
	x ^= sign;
	x -= sign;

	return x;
}

static __inline int CLZ(int x)
{
	int numZeros;

	if (!x)
		return (sizeof(int) * 8);

	numZeros = 0;
	while (!(x & 0x80000000)) {
		numZeros++;
		x <<= 1;
	} 

	return numZeros;
}


#elif defined(AMIGA_M68K) || \
	(defined(__GNUC__) && (defined(__mc68000__) || defined(__mc68020__) || defined(mc68000)))

/*
 * Correctness-first AmigaOS/m68k backend.
 *
 * m68k-amigaos-gcc targets 32-bit int/long and supports long long, which is
 * enough for the fixed-point helpers used by the portable RealNetworks decoder.
 * Keep this block free of ARM/x86/RISC-V assembly so the initial Amiga port can
 * build cleanly; 68020-specific inline assembly can be added later behind a
 * dedicated opt-in guard while retaining these C fallbacks.
 */
#ifndef AMIGA_M68K
#define AMIGA_M68K 1
#endif

typedef long long Word64;

/*
 * Keep the portable C implementation named and callable so Amiga benchmark
 * builds can verify every optimized helper against the reference semantics.
 */
static __inline int MULSHIFT32_C_REFERENCE(int x, int y)
{
	return (int)(((Word64)x * (Word64)y) >> 32);
}

#if defined(AMIGA_M68K_ASM) && defined(__GNUC__) && \
	(defined(__mc68020__) || defined(__mc68030__) || defined(__mc68040__) || \
	 defined(__mc68060__) || defined(mc68020))
/*
 * Optional 68020+ signed 32x32 -> 64 multiply.  The two-operand m68k GNU as
 * form writes the high 32 bits to the first output register and the low 32
 * bits to the second output register.  MADD64/SHL64/SAR64 deliberately
 * remain C below.
 */
static __inline int MULSHIFT32_AMIGA_M68K_ASM(int x, int y)
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
#define MULSHIFT32_HAS_AMIGA_M68K_ASM 1
#define MULSHIFT32(x, y) MULSHIFT32_AMIGA_M68K_ASM((x), (y))
#elif defined(AMIGA_M68K) && defined(AMIGA_M68K_POLYPHASE_68060)
/*
 * 68060 C builds: the 32x32->64 product above compiles to a call to libgcc's
 * ___muldi3 per multiply (the 060 has no hardware 64-bit multiply), which
 * made the IMDCT, anti-alias and stereo stages the largest remaining decode
 * cost. Compute the identical high word with hardware 32-bit multiplies
 * (Warren "mulhs", as MulShift68060 in polyphase_68060.h). Bit-for-bit equal
 * to MULSHIFT32_C_REFERENCE; tests/mp3_decode_hash_test.c checks whole-file
 * PCM against the 64-bit build.
 */
static __inline int MULSHIFT32_68060(int x, int y)
{
	unsigned int x0 = (unsigned int)x & 0xFFFFu;
	int x1 = x >> 16;
	unsigned int y0 = (unsigned int)y & 0xFFFFu;
	int y1 = y >> 16;
	unsigned int w0 = x0 * y0;
	int t = x1 * (int)y0 + (int)(w0 >> 16);
	int w1 = t & 0xFFFF;
	int w2 = t >> 16;

	w1 = (int)x0 * y1 + w1;
	return x1 * y1 + w2 + (w1 >> 16);
}
#define MULSHIFT32_HAS_AMIGA_M68K_ASM 0
#define MULSHIFT32(x, y) MULSHIFT32_68060((x), (y))
#else
#define MULSHIFT32_HAS_AMIGA_M68K_ASM 0
#define MULSHIFT32(x, y) MULSHIFT32_C_REFERENCE((x), (y))
#endif

static __inline int FASTABS(int x)
{
	int sign;

	sign = x >> (sizeof(int) * 8 - 1);
	x ^= sign;
	x -= sign;

	return x;
}

static __inline int CLZ_C_REFERENCE(int x)
{
	unsigned int ux;
	int numZeros;

	ux = (unsigned int)x;
	if (!ux)
		return 32;

	/* Fixed-depth search avoids up to 31 loop/back-branch iterations on m68k. */
	numZeros = 0;
	if (!(ux & 0xffff0000UL)) { numZeros += 16; ux <<= 16; }
	if (!(ux & 0xff000000UL)) { numZeros += 8;  ux <<= 8;  }
	if (!(ux & 0xf0000000UL)) { numZeros += 4;  ux <<= 4;  }
	if (!(ux & 0xc0000000UL)) { numZeros += 2;  ux <<= 2;  }
	if (!(ux & 0x80000000UL)) { numZeros += 1; }

	return numZeros;
}

#if defined(AMIGA_M68K_ASM) && defined(__GNUC__) && \
	(defined(__mc68020__) || defined(__mc68030__) || defined(__mc68040__) || \
	 defined(__mc68060__) || defined(mc68020))
static __inline int CLZ_AMIGA_M68K_ASM(int x)
{
	int result;

	if (!x)
		return 32;
	__asm__("bfffo %1{#0:#32},%0" : "=d" (result) : "d" (x) : "cc");
	return result;
}
#define CLZ_HAS_AMIGA_M68K_ASM 1
#define CLZ(x) CLZ_AMIGA_M68K_ASM((x))
#else
#define CLZ_HAS_AMIGA_M68K_ASM 0
#define CLZ(x) CLZ_C_REFERENCE((x))
#endif

static __inline Word64 MADD64(Word64 sum, int x, int y)
{
	return sum + (Word64)x * (Word64)y;
}

static __inline Word64 SHL64(Word64 x, int n)
{
	return x << n;
}

static __inline Word64 SAR64(Word64 x, int n)
{
	return x >> n;
}

#elif defined ARM_ADS

static __inline int MULSHIFT32(int x, int y)
{
	/* important rules for smull RdLo, RdHi, Rm, Rs:
	 *     RdHi and Rm can't be the same register
	 *     RdLo and Rm can't be the same register
	 *     RdHi and RdLo can't be the same register
	 * Note: Rs determines early termination (leading sign bits) so if you want to specify
	 *   which operand is Rs, put it in the SECOND argument (y)
	 * For inline assembly, x and y are not assumed to be R0, R1 so it shouldn't matter
	 *   which one is returned. (If this were a function call, returning y (R1) would 
	 *   require an extra "mov r0, r1")
	 */
	int zlow;
	__asm {
		smull zlow,y,x,y
	}

	return y;
}

static __inline int FASTABS(int x) 
{
	int t=0; /*Really is not necessary to initialiaze only to avoid warning*/

	__asm {
		eor	t, x, x, asr #31
		sub	t, t, x, asr #31
	}

	return t;
}

static __inline int CLZ(int x)
{
	int numZeros;

	if (!x)
		return (sizeof(int) * 8);

	numZeros = 0;
	while (!(x & 0x80000000)) {
		numZeros++;
		x <<= 1;
	} 

	return numZeros;
}

#elif defined(__GNUC__) && defined(ARM)

#if defined(ARM7DI)
	
typedef long long Word64;

static __inline int MULSHIFT32(int x, int y) {
	return x * y;
}

static __inline Word64 SAR64(Word64 x, int n) {
	return x >>= n;
}


typedef union _U64 {
	Word64 w64;
	struct {
		/* x86 = little endian */
		unsigned int lo32;
		signed int   hi32;
	} r;
} U64;

static __inline Word64 MADD64(Word64 sum64, int x, int y)
{
	sum64 += (Word64)x * (Word64)y;

	return sum64;
}
	
#else

static __inline int MULSHIFT32(int x, int y)
{
	/* important rules for smull RdLo, RdHi, Rm, Rs:
	 *     RdHi and Rm can't be the same register
	 *     RdLo and Rm can't be the same register
	 *     RdHi and RdLo can't be the same register
	 * Note: Rs determines early termination (leading sign bits) so if you want to specify
	 *   which operand is Rs, put it in the SECOND argument (y)
	 * For inline assembly, x and y are not assumed to be R0, R1 so it shouldn't matter
	 *   which one is returned. (If this were a function call, returning y (R1) would 
	 *   require an extra "mov r0, r1")
	 */
	int zlow;
	__asm__ volatile ("smull %0,%1,%2,%3" : "=&r" (zlow), "=r" (y) : "r" (x), "1" (y)) ;

	return y;
}

#endif

static __inline int FASTABS(int x) 
{
	int t=0; /*Really is not necessary to initialiaze only to avoid warning*/

	__asm__ volatile (
		"eor %0,%2,%2, asr #31;"
		"sub %0,%1,%2, asr #31;"
		: "=&r" (t) 
		: "0" (t), "r" (x)
	 );

	return t;
}

static __inline int CLZ(int x)
{
	int numZeros;

	if (!x)
		return (sizeof(int) * 8);

	numZeros = 0;
	while (!(x & 0x80000000)) {
		numZeros++;
		x <<= 1;
	} 

	return numZeros;
}

#else

#ifdef __riscv

typedef long long Word64;

static __inline int MULSHIFT32(int x, int y)
{
	unsigned int result = 0;
	asm volatile ("mulh %0, %1, %2" : "=r"(result): "r"(x), "r"(y));
	return result;
}

static __inline int FASTABS(int x) 
{
	int sign;

	sign = x >> (sizeof(int) * 8 - 1);
	x ^= sign;
	x -= sign;

	return x;
}

static __inline int CLZ(int x)
{
	int numZeros;

	if (!x)
		return (sizeof(int) * 8);

	numZeros = 0;
	while (!(x & 0x80000000)) {
		numZeros++;
		x <<= 1;
	} 

	return numZeros;
}

static __inline Word64 MADD64(Word64 sum, int a, int b)
{
	unsigned int result_hi = 0;
	unsigned int result_lo = 0;
	asm volatile ("mulh %0, %1, %2" : "=r"(result_hi): "r"(a), "r"(b));
	asm volatile ("mul  %0, %1, %2" : "=r"(result_lo): "r"(a), "r"(b));

	Word64 result = result_hi;
	result <<= 32;
	result += result_lo;
	result += sum;
	return result;
}

static __inline Word64 SHL64(Word64 x, int n)
{
	return (x<<n);
}

static __inline Word64 SAR64(Word64 x, int n)
{
	return (x >> n);
}

#else

#error Unsupported platform in assembly.h

#endif

#endif	/* platforms */

/*
 * Read two consecutive bitstream bytes as one big-endian 16-bit value.
 *
 * A REV16 after a uint16_t load is useful on little-endian targets.  Native
 * m68k is already big-endian, so the equivalent (and cheaper) operation is a
 * single word load with no byte swap.  Restrict the potentially unaligned
 * word load to 68020+; the portable byte loads remain safe for 68000/68010 and
 * for non-m68k builds that define AMIGA_M68K while exercising the C backend.
 */
#if defined(__GNUC__) && \
	(defined(__mc68020__) || defined(__mc68030__) || defined(__mc68040__) || \
	 defined(__mc68060__) || defined(mc68020))
static __inline unsigned int LOADBE16(const unsigned char *buf)
{
	unsigned int value;

	__asm__ volatile ("moveq #0,%0\n\tmove.w (%1),%0"
		: "=&d" (value)
		: "a" (buf)
		: "memory");
	return value;
}
#define LOADBE16_HAS_M68K_WORD_LOAD 1
#else
static __inline unsigned int LOADBE16(const unsigned char *buf)
{
	return ((unsigned int)buf[0] << 8) | (unsigned int)buf[1];
}
#define LOADBE16_HAS_M68K_WORD_LOAD 0
#endif

#endif /* _ASSEMBLY_H */
