/* Exercise the actual IMDCT hook against the existing encoded tone fixture. */
#define main existing_mpeg25_test_main
#include "mpeg25_decode_test.c"
#undef main
#include "skin_controls.h"
#include <stdlib.h>
static long decode_tone(int enabled,int db,unsigned long *checksum)
{
    HMP3Decoder decoder; MP3FrameInfo info; unsigned char *at=(unsigned char *)mpeg25_stream;
    int left=sizeof(mpeg25_stream),rc,n; short pcm[2304]; long energy=0;
    skin_eq_flat(); gSkinAudio.eq_bands[3]=db; ++gSkinAudio.eq_sequence; gSkinAudio.eq_enabled=enabled;
    *checksum=0; decoder=MP3InitDecoder(); if (!decoder) exit(1);
    while (left>0) {
        rc=MP3Decode(decoder,&at,&left,pcm,0);
        if (rc) { fprintf(stderr,"EQ tone decode failed %d\n",rc); exit(1); }
        MP3GetLastFrameInfo(decoder,&info);
        for (n=0;n<info.outputSamps;++n) { energy+=abs(pcm[n]); *checksum=(*checksum*33UL+(unsigned short)pcm[n])&0xffffffffUL; }
    }
    MP3FreeDecoder(decoder); return energy;
}
int main(void)
{
    unsigned long base_crc,flat_crc,unused; long base,flat,cut,boost;
    if (existing_mpeg25_test_main()) return 1;
    base=decode_tone(0,12,&base_crc); flat=decode_tone(1,0,&flat_crc);
    cut=decode_tone(1,-12,&unused); boost=decode_tone(1,6,&unused);
    if (base!=flat || base_crc!=flat_crc || cut>=base*3/4 || boost<=base) {
        fprintf(stderr,"EQ energy: base=%ld flat=%ld cut=%ld boost=%ld\n",base,flat,cut,boost); return 1;
    }
    printf("Actual MP3 EQ: disabled/flat identical; 440 Hz energy base=%ld cut=%ld boost=%ld\n",base,cut,boost); return 0;
}
