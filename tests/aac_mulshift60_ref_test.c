#include <limits.h>
#include <stdio.h>

#ifndef MINTAMP_AAC_NATIVE_68060
#define MINTAMP_AAC_TEST_68060 1
#endif
#include "../decoders/aac_m68k_aac_optimized.h"

static unsigned int rng_state = 0x4d696e74U;

static unsigned int next_random(void)
{
    rng_state = rng_state * 1664525U + 1013904223U;
    return rng_state;
}

static int check_pair(int x, int y)
{
    long long product = (long long)x * (long long)y;
    int expected = (int)(product >> 32);
    int actual = AAC_M68K_MULSHIFT32(x, y);
    unsigned long long base = ((unsigned long long)(unsigned int)x << 17) ^
                              (unsigned int)y;
    unsigned long long madd_expected = base + (unsigned long long)product;
    unsigned long long madd_actual = (unsigned long long)
                                    AAC_M68K_MADD64((long long)base, x, y);

    unsigned long long scaled_expected = base + (unsigned long long)((long long)x * (1 << 20));
    unsigned long long scaled_actual = (unsigned long long)AAC_M68K_MADD64((long long)base, x, 1 << 20);
    if (actual != expected || madd_actual != madd_expected || scaled_actual != scaled_expected) {
        fprintf(stderr,
                "FAIL x=%d y=%d high=%d expected=%d madd=%llu expected=%llu\n",
                x, y, actual, expected, madd_actual, madd_expected);
        return 0;
    }
    return 1;
}

int main(void)
{
    static const int edges[] = {
        0, 1, -1, 2, -2, 0x7fff, -0x8000,
        0x10000, -0x10000, INT_MAX, INT_MIN
    };
    unsigned int i, j;

    for (i = 0; i < sizeof(edges) / sizeof(edges[0]); ++i)
        for (j = 0; j < sizeof(edges) / sizeof(edges[0]); ++j)
            if (!check_pair(edges[i], edges[j]))
                return 1;

    for (i = 0; i < 1000000U; ++i)
        if (!check_pair((int)next_random(), (int)next_random()))
            return 1;

    puts("AAC 68060 partial-product multiply: 1000000 random pairs plus edges passed");
    return 0;
}
