#ifndef MINTAMP_INTEGER_STATS_H
#define MINTAMP_INTEGER_STATS_H

/* Timing/reporting only. Explicit 32-bit limbs avoid FPU and 68060-trapped
 * 64-bit multiply/divide instructions, including diagnostic RMS calculations. */
#include <stdint.h>
#include <stdio.h>

typedef struct IntegerWide { uint32_t hi, lo; } IntegerWide;
typedef struct IntegerRms { IntegerWide squares; uint32_t count; int overflow; } IntegerRms;

static __inline__ IntegerWide IntegerMultiply(uint32_t a, uint32_t b)
{
    uint32_t low, cross1, cross2, old;
    IntegerWide result;
    low=(a&65535U)*(b&65535U);
    cross1=(a&65535U)*(b>>16);
    cross2=(a>>16)*(b&65535U);
    result.lo=low+(cross1<<16);
    result.hi=(a>>16)*(b>>16)+(cross1>>16)+(result.lo<low);
    old=result.lo; result.lo+=cross2<<16;
    result.hi+=(cross2>>16)+(result.lo<old);
    return result;
}

static __inline__ IntegerWide IntegerDivide(IntegerWide value, uint32_t divisor,
                                           uint32_t *remainder)
{
    IntegerWide result;
    uint32_t rem, bit, carry;
    if (!divisor) { result.hi=result.lo=0; *remainder=0; return result; }
    result.hi=value.hi/divisor; rem=value.hi%divisor; result.lo=0;
    for (bit=0x80000000U; bit; bit>>=1) {
        carry=rem>>31; rem=(rem<<1)|((value.lo&bit)!=0);
        if (carry || rem>=divisor) { rem-=divisor; result.lo|=bit; }
    }
    *remainder=rem; return result;
}

/* floor(numerator*scale/divisor), optionally rounded; saturate only when
 * the final answer cannot fit 32 bits. The common millisecond path is a
 * single ordinary 32-bit multiply/divide, not a wide division loop. */
static __inline__ uint32_t IntegerScale(uint32_t numerator, uint32_t divisor,
                                      uint32_t scale, int round_nearest)
{
    uint32_t quotient, remainder;
    IntegerWide product, result;
    if (!divisor || !scale) return 0;
    if (numerator<=UINT32_MAX/scale) {
        uint32_t value=numerator*scale;
        quotient=value/divisor; remainder=value%divisor;
    } else {
        product=IntegerMultiply(numerator,scale);
        result=IntegerDivide(product,divisor,&remainder);
        if (result.hi) return UINT32_MAX;
        quotient=result.lo;
    }
    if (round_nearest && remainder>=divisor/2+(divisor&1U) && quotient<UINT32_MAX)
        ++quotient;
    return quotient;
}

static __inline__ void IntegerRatioText(char *text, size_t size,
                                        uint32_t numerator, uint32_t divisor,
                                        int digits)
{
    uint32_t whole=0, fraction=0, scale=1;
    int i;
    if (digits<1) digits=1;
    if (digits>6) digits=6;
    for (i=0;i<digits;++i) scale*=10;
    if (divisor) {
        whole=numerator/divisor;
        fraction=IntegerScale(numerator%divisor,divisor,scale,1);
        if (fraction==scale) { ++whole; fraction=0; }
    }
    snprintf(text,size,"%lu.%0*lu",(unsigned long)whole,digits,(unsigned long)fraction);
}

static __inline__ void IntegerRmsAdd(IntegerRms *stats, int32_t a, int32_t b)
{
    uint32_t difference, old, carry;
    IntegerWide square;
    if (stats->overflow) return;
    difference=a>=b ? (uint32_t)a-(uint32_t)b : (uint32_t)b-(uint32_t)a;
    square=IntegerMultiply(difference,difference);
    old=stats->squares.lo; stats->squares.lo+=square.lo;
    carry=stats->squares.lo<old;
    old=stats->squares.hi; stats->squares.hi+=square.hi;
    stats->overflow=stats->squares.hi<old;
    old=stats->squares.hi; stats->squares.hi+=carry;
    stats->overflow|=stats->squares.hi<old || stats->count==UINT32_MAX;
    if (!stats->overflow) ++stats->count;
}

/* Integer sqrt(floor(mean square)); diagnostics report whole sample counts.
 * Overflow is explicitly reported instead of silently wrapping the sum. */
static __inline__ void IntegerRmsText(char *text, size_t size, const IntegerRms *stats)
{
    IntegerWide mean, square;
    uint32_t root=0, bit, candidate, remainder;
    if (stats->overflow) { snprintf(text,size,"unavailable (overflow)"); return; }
    mean=IntegerDivide(stats->squares,stats->count,&remainder);
    for (bit=0x80000000U; bit; bit>>=1) {
        candidate=root|bit; square=IntegerMultiply(candidate,candidate);
        if (square.hi<mean.hi || (square.hi==mean.hi && square.lo<=mean.lo)) root=candidate;
    }
    snprintf(text,size,"%lu",(unsigned long)root);
}

#endif
