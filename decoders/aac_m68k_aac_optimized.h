/**************************************************************************************
    MintAMP CPU-aware m68k helpers for the Helix AAC decoder.

    The upstream AAC helper uses the 32x32->64 register-pair form of MULS.L.
    That is ideal on 68020/030/040, but it is not implemented in 68060
    hardware.  On a real 68060 it traps into the integer support package.

    This forced-include wrapper keeps that one-instruction path on 020/030/040
    and uses four hardware single-result multiplies to construct the exact high
    word on 060.  Defining the upstream include guard makes its later quoted
    include a no-op, so no modification of the ESP8266Audio submodule is
    required.
 **************************************************************************************/

#ifndef MINTAMP_AAC_M68K_OPTIMIZED_H
#define MINTAMP_AAC_M68K_OPTIMIZED_H

#ifndef _AMIGA_M68K_AAC_H
#define _AMIGA_M68K_AAC_H

#if defined(__GNUC__) && ( \
        defined(__mc68020__) || defined(__mc68030__) || \
        defined(__mc68040__) || defined(__mc68060__) || \
        defined(mc68020) || defined(mc68030) || \
        defined(mc68040) || defined(mc68060) || \
        defined(MINTAMP_AAC_TEST_68060) )

#define AAC_M68K_HAVE_ASM 1

#if defined(__mc68060__) || defined(mc68060) || \
        defined(MINTAMP_AAC_TEST_68060)

/* The register-pair form is emulated on 060; these two-operand, low-result
   forms are real hardware instructions. Test builds substitute native C. */
static __inline unsigned int AAC_M68K_MULU32_LO(unsigned int x,
                                                unsigned int y)
{
#if defined(MINTAMP_AAC_TEST_68060)
    return x * y;
#else
    __asm__ ("mulu.l %1,%0"
             : "+d" (x)
             : "d"  (y));
    return x;
#endif
}

static __inline int AAC_M68K_MULS32_LO(int x, int y)
{
#if defined(MINTAMP_AAC_TEST_68060)
    return x * y;
#else
    __asm__ ("muls.l %1,%0"
             : "+d" (x)
             : "d"  (y));
    return x;
#endif
}

/* Warren's signed mulhs construction (Hacker's Delight 8-2). Each operand of
   a partial product is a signed or unsigned 16-bit half held in a long, so the
   four single-result products are sufficient to recover the exact high word. */
static __inline int AAC_M68K_MULSHIFT32(int x, int y)
{
#if defined(MINTAMP_AAC_060_REG5) && !defined(MINTAMP_AAC_TEST_68060)
    unsigned int xl, yl, carry;
    /* Same signed mulhs decomposition as the reference below, scheduled in
       five data registers. SWAP/EXT extracts signed high halves without
       keeping a register containing 16 live across every multiply. All four
       multiplies have a single 32-bit destination: none traps on a 68060.
       Early-clobber is required: the inputs are consumed before the sequence
       finishes, including when the caller passes the same value twice. */
    __asm__ (
        "moveq #0,%2\n\t"
        "move.w %0,%2\n\t"       /* xl = unsigned low(x) */
        "moveq #0,%3\n\t"
        "move.w %1,%3\n\t"       /* yl = unsigned low(y) */
        "swap %0\n\t"
        "ext.l %0\n\t"           /* x = signed high(x) */
        "swap %1\n\t"
        "ext.l %1\n\t"           /* y = signed high(y) */
        "move.l %2,%4\n\t"
        "mulu.l %3,%4\n\t"       /* low product */
        "clr.w %4\n\t"
        "swap %4\n\t"            /* high16(low product) */
        "muls.l %0,%3\n\t"
        "add.l %4,%3\n\t"        /* t = xhi*ylo + carry */
        "move.l %3,%4\n\t"
        "swap %4\n\t"
        "ext.l %4\n\t"           /* signed high16(t) */
        "and.l #65535,%3\n\t"
        "muls.l %1,%2\n\t"
        "add.l %2,%3\n\t"        /* w1 = xlo*yhi + low16(t) */
        "swap %3\n\t"
        "ext.l %3\n\t"
        "muls.l %1,%0\n\t"
        "add.l %4,%0\n\t"
        "add.l %3,%0"
        : "+&d" (x), "+&d" (y), "=&d" (xl), "=&d" (yl), "=&d" (carry)
        :
        : "cc");
    return x;
#else
    unsigned int x0 = (unsigned int)x & 0xffffU;
    int x1 = x >> 16;
    unsigned int y0 = (unsigned int)y & 0xffffU;
    int y1 = y >> 16;
    unsigned int w0 = AAC_M68K_MULU32_LO(x0, y0);
    int t = AAC_M68K_MULS32_LO(x1, (int)y0) + (int)(w0 >> 16);
    int w1 = t & 0xffff;
    int w2 = t >> 16;

    w1 = AAC_M68K_MULS32_LO((int)x0, y1) + w1;
    return AAC_M68K_MULS32_LO(x1, y1) + w2 + (w1 >> 16);
#endif
}

static __inline long long AAC_M68K_MADD64(long long sum, int x, int y)
{
    /* TNS starts its accumulator with y[n] * 2^20. A known power of two
       needs no partial products; use unsigned shifting to define negative
       inputs and wrapping accumulation exactly like the general path. */
    if (__builtin_constant_p(y) && y > 0 && ((unsigned int)y & ((unsigned int)y - 1U)) == 0)
        return (long long)((unsigned long long)sum +
               ((unsigned long long)(long long)x << __builtin_ctz((unsigned int)y)));
    unsigned int lo;
    unsigned int hi;
    unsigned long long product;

    lo = AAC_M68K_MULU32_LO((unsigned int)x, (unsigned int)y);
    hi = (unsigned int)AAC_M68K_MULSHIFT32(x, y);
    product = ((unsigned long long)hi << 32) | lo;
    return (long long)((unsigned long long)sum + product);
}

#else /* 68020/030/040: hardware full-result MULS.L */

static __inline int AAC_M68K_MULSHIFT32(int x, int y)
{
    int hi, lo;

    __asm__ ("muls.l %3,%0:%1"
             : "=d" (hi), "=d" (lo)
             : "1"  (x),  "d"  (y));
    (void)lo;
    return hi;
}

static __inline long long AAC_M68K_MADD64(long long sum, int x, int y)
{
    union { long long w; struct { int hi; unsigned int lo; } r; } u;
    int phi;
    unsigned int plo;

    u.w = sum;
    __asm__ ("muls.l %3,%0:%1"
             : "=d" (phi), "=d" (plo)
             : "1"  (x),   "d"  (y));
    __asm__ ("add.l  %3,%1\n\t"
             "addx.l %2,%0"
             : "+d" (u.r.hi), "+d" (u.r.lo)
             : "d"  (phi),    "d"  (plo));
    return u.w;
}

#endif

static __inline unsigned int AAC_M68K_LOAD_BE32(const unsigned char *p)
{
    unsigned int v;

#if defined(MINTAMP_AAC_TEST_68060)
    v = ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) |
        ((unsigned int)p[2] << 8) | p[3];
#else
    __asm__ ("move.l (%1),%0"
             : "=d" (v)
             : "a"  (p));
#endif
    return v;
}

#define AAC_M68K_BIG_ENDIAN_LOAD 1

#endif /* GNU C 68020+ or reference test */
#endif /* _AMIGA_M68K_AAC_H */
#endif /* MINTAMP_AAC_M68K_OPTIMIZED_H */
