/* Decode deterministic ADTS fixtures to PCM for cross-build comparisons.
 * The runner compares both PCM bytes and frame/rate/window/tool metadata.
 * No sample assertions are disabled, and decode errors fail the run. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "aacdec.h"
#include "coder.h"
int main(int argc, char **argv) {
    FILE *in, *out; long n; unsigned char *data, *p; int left, frames=0, limit=0;
    unsigned windows=0, tns=0, pns=0;
    short pcm[4096]; HAACDecoder dec; AACFrameInfo fi;
    int factor=argc>3?atoi(argv[3]):1;
    if(argc<3 || !(in=fopen(argv[1],"rb"))) return 2;
    if(argc>4) limit=atoi(argv[4]);
    fseek(in,0,SEEK_END); n=ftell(in); rewind(in);
    data=malloc(n); if(!data || fread(data,1,n,in)!=(size_t)n) return 3; fclose(in);
    out=fopen(argv[2],"wb"); if(!out) return 4;
    dec=AACInitDecoder(); if(!dec) return 5;
    if(AACSetOutputDecimation(dec,factor)) return 6;
    p=data;left=n;
    while(left>=7 && (!limit || frames<limit)) {
        int err=AACDecode(dec,&p,&left,pcm);
        if(err) { fprintf(stderr,"decode error %d frame %d left %d\n",err,frames,left);return 7; }
        AACGetLastFrameInfo(dec,&fi);
        windows |= 1U << ((PSInfoBase *)((AACDecInfo *)dec)->psInfoBase)->icsInfo[0].winSequence;
        tns += fi.tnsUsed != 0; pns += fi.pnsUsed != 0;
        if(fi.outputSamps<0 || fi.outputSamps>4096) return 8;
        if(fwrite(pcm,sizeof(short),fi.outputSamps,out)!=(size_t)fi.outputSamps)return 9;
        frames++;
    }
    fprintf(stderr,"frames=%d rate=%d channels=%d factor=%d windows=%x tns=%u pns=%u\n",frames,fi.sampRateOut,fi.nChans,factor,windows,tns,pns);
    fclose(out);AACFreeDecoder(dec);free(data);return frames?0:10;
}
