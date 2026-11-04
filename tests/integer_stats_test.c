#include "integer_stats.h"
#include <assert.h>
#include <string.h>
#include <limits.h>
#include <stdlib.h>

/* Independent wide host maths is a test oracle, never linked into Amiga apps. */
static uint32_t random32(uint32_t *seed)
{ *seed=*seed*1664525U+1013904223U; return *seed; }

static void ratio(uint32_t n, uint32_t d, int digits, const char *expected)
{
    char text[32];
    IntegerRatioText(text,sizeof(text),n,d,digits);
    assert(!strcmp(text,expected));
}

int main(void)
{
    uint32_t seed=123, i;
    IntegerRms rms;
    char text[32];
    assert(IntegerScale(1,50,1000,0)==20);
    assert(IntegerScale(51,50,1000,0)==1020);
    assert(IntegerScale(UINT32_MAX,1000000,1000,0)==4294967);
    assert(IntegerScale(UINT32_MAX,1,1000,0)==UINT32_MAX);
    assert(IntegerScale(10,0,1000,0)==0);
    assert(IntegerScale(1,3,1000,0)==333);
    assert(IntegerScale(2,3,1000,1)==667);
    ratio(1,44100,6,"0.000023");
    ratio(1999999,1000000,3,"2.000");
    ratio(UINT32_MAX,1,6,"4294967295.000000");
    ratio(UINT32_MAX-1,UINT32_MAX,6,"1.000000");
    ratio(1,UINT32_MAX,6,"0.000000");
    ratio(0,0,3,"0.000");
    ratio(IntegerScale(17,32,1000,1),10,1,"53.1");
    for (i=0;i<100000;++i) {
        uint32_t a=random32(&seed), b=random32(&seed), d=random32(&seed)|1U;
        uint32_t rem, got, scale=random32(&seed), round_nearest=i&1U;
        uint64_t product=(uint64_t)a*b, reference=(uint64_t)a*scale;
        IntegerWide wide=IntegerMultiply(a,b), quotient;
        assert(wide.hi==(uint32_t)(product>>32) && wide.lo==(uint32_t)product);
        quotient=IntegerDivide(wide,d,&rem);
        assert((((uint64_t)quotient.hi<<32)|quotient.lo)==product/d);
        assert(rem==product%d);
        got=IntegerScale(a,d,scale,round_nearest);
        {
            uint64_t q=reference/d;
            if (round_nearest && reference%d>=d/2+(d&1U)) ++q;
            if (q>UINT32_MAX) q=UINT32_MAX;
            assert(got==(uint32_t)q);
        }
    }
    memset(&rms,0,sizeof(rms));
    IntegerRmsText(text,sizeof(text),&rms); assert(!strcmp(text,"0"));
    IntegerRmsAdd(&rms,3,0); IntegerRmsAdd(&rms,4,0);
    IntegerRmsText(text,sizeof(text),&rms); assert(!strcmp(text,"3"));
    memset(&rms,0,sizeof(rms));
    IntegerRmsAdd(&rms,INT32_MIN,INT32_MAX);
    IntegerRmsText(text,sizeof(text),&rms); assert(!strcmp(text,"4294967295"));
    IntegerRmsAdd(&rms,INT32_MIN,INT32_MAX);
    IntegerRmsText(text,sizeof(text),&rms); assert(!strcmp(text,"unavailable (overflow)"));
    memset(&rms,0,sizeof(rms));
    {
        uint64_t sum=0, mean;
        uint32_t root=0, bit;
        for (i=0;i<10000;++i) {
            int32_t a=(int32_t)(random32(&seed)&65535U)-32768;
            int32_t b=(int32_t)(random32(&seed)&65535U)-32768;
            int64_t difference=(int64_t)a-b;
            IntegerRmsAdd(&rms,a,b); sum+=(uint64_t)(difference*difference);
        }
        assert(!rms.overflow && rms.count==10000);
        assert((((uint64_t)rms.squares.hi<<32)|rms.squares.lo)==sum);
        mean=sum/10000;
        for (bit=0x80000000U;bit;bit>>=1) {
            uint32_t candidate=root|bit;
            if ((uint64_t)candidate*candidate<=mean) root=candidate;
        }
        IntegerRmsText(text,sizeof(text),&rms);
        assert((uint32_t)strtoul(text,NULL,10)==root);
    }
    puts("Integer statistics: 100000 wide/scaled cases, timing, decimal rounding and RMS passed");
    return 0;
}
