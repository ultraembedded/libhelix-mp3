#include "winamp_skin.h"
#include "lodepng.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Canvas {
    const WinampSkin *skin;
    unsigned char pixels[275*232*3];
    unsigned draws;
    int height;
} Canvas;
static void blit(void *ctx,int id,int sx,int sy,int w,int h,int x,int y)
{
    Canvas *c=ctx; const SkinBitmap *b=&c->skin->assets[id]; int row;
    assert(x>=0 && y>=0 && x+w<=275 && y+h<=c->height);
    assert(sx>=0 && sy>=0 && sx+w<=(int)b->width && sy+h<=(int)b->height);
    for (row=0;row<h;++row)
        memcpy(c->pixels+((y+row)*275+x)*3,b->rgb+((sy+row)*b->width+sx)*3,w*3);
    ++c->draws;
}
static void fill(void *ctx,unsigned rgb,int x,int y,int w,int h)
{
    Canvas *c=ctx; int i,j;
    assert(x>=0 && y>=0 && x+w<=275 && y+h<=c->height);
    for (j=y;j<y+h;++j) for (i=x;i<x+w;++i) {
        unsigned char *p=c->pixels+(j*275+i)*3;
        p[0]=rgb>>16; p[1]=rgb>>8; p[2]=rgb;
    }
    ++c->draws;
}
static void label(void *ctx,const char *text,unsigned rgb,unsigned bg,int x,int y,int width)
{
    int i; (void)bg;
    /* Deterministic glyph marks test clipping, colours and redraws. Native
     * runtime uses Topaz; these marks are not a font-fidelity preview. */
    for (i=0;text[i] && i*8+7<=width;++i)
        fill(ctx,rgb,x+i*8,y+(text[i]&3),6,4);
}
static unsigned char *read_file(const char *name,size_t *size)
{
    FILE *f=fopen(name,"rb"); long len; unsigned char *data;
    assert(f); assert(!fseek(f,0,SEEK_END)); len=ftell(f); assert(len>=0); rewind(f);
    data=malloc(len ? (size_t)len : 1); assert(data);
    assert(fread(data,1,(size_t)len,f)==(size_t)len); fclose(f); *size=(size_t)len; return data;
}
int main(int argc,char **argv)
{
    WinampSkin skin={0}; char error[160]; int i;
    if (argc>2 && !strcmp(argv[1],"--bmp")) {
        size_t len; unsigned char *data=read_file(argv[2],&len); SkinBitmap b={0};
        int ok=skin_decode_bmp(&b,data,len,error,sizeof(error)); free(data);
        if (!ok) { fprintf(stderr,"%s\n",error); return 1; }
        printf("%u %u %08x\n",b.width,b.height,lodepng_crc32(b.rgb,(size_t)b.width*b.height*3));
        free(b.rgb); return 0;
    }
    assert(argc>=2);
    if (!skin_load_file(&skin,argc>2 && (!strcmp(argv[1],"--colours") || !strcmp(argv[1],"--visual-colours")) ? argv[2] : argv[1],error,sizeof(error))) {
        fprintf(stderr,"%s\n",error); return 1;
    }
    if (argc>2 && !strcmp(argv[1],"--visual-colours")) {
        for (i=0;i<24;++i) printf("%06x%s",skin.visual_colors[i],i==23 ? "\n" : " ");
        skin_free(&skin); return 0;
    }
    if (argc>2 && !strcmp(argv[1],"--colours")) {
        printf("%06x %06x %06x %06x %06x %06x\n",skin.playlist_normal,skin.playlist_current,
               skin.playlist_background,skin.playlist_selected,skin.mb_background,skin.mb_text);
        {
        Canvas *c=calloc(1,sizeof(*c)); SkinEqState state={0},old;
        unsigned char *full=malloc(sizeof(c->pixels)); unsigned draws;
        assert(c && full); c->skin=&skin; c->height=116;
        skin_eq_render(&skin,&state,NULL,blit,fill,c); draws=c->draws; old=state;
        skin_eq_render(&skin,&state,&old,blit,fill,c); assert(c->draws==draws);
        state.preamp=12; state.enabled=1; state.pressed=3;
        for (i=0;i<10;++i) state.bands[i]=i%2 ? -12 : 12;
        skin_eq_render(&skin,&state,&old,blit,fill,c); memcpy(full,c->pixels,sizeof(c->pixels));
        skin_eq_render(&skin,&state,NULL,blit,fill,c); assert(!memcmp(full,c->pixels,sizeof(c->pixels)));
        old=state; state.shaded=1; state.active=1; c->height=14;
        skin_eq_render(&skin,&state,&old,blit,fill,c); memcpy(full,c->pixels,275*14*3);
        skin_eq_render(&skin,&state,NULL,blit,fill,c); assert(!memcmp(full,c->pixels,275*14*3));
        old=state; draws=c->draws; skin_eq_render(&skin,&state,&old,blit,fill,c); assert(c->draws==draws);
        state.shaded=0; state.pressed=-4; c->height=116;
        skin_eq_render(&skin,&state,&old,blit,fill,c); memcpy(full,c->pixels,275*116*3);
        skin_eq_render(&skin,&state,NULL,blit,fill,c); assert(!memcmp(full,c->pixels,275*116*3));
        free(full); free(c);
    }
    skin_free(&skin); return 0;
    }
    for (i=0;i<SKIN_ASSET_COUNT;++i) if (skin.assets[i].rgb)
        printf("%d %u %u %08x\n",i,skin.assets[i].width,skin.assets[i].height,
               lodepng_crc32(skin.assets[i].rgb,(size_t)skin.assets[i].width*skin.assets[i].height*3));
    /* A failed replacement must leave the loaded skin and its pixels alive. */
    {
        unsigned char invalid[22]={0}; unsigned char *original=skin.assets[0].rgb;
        assert(!skin_load_memory(&skin,invalid,sizeof(invalid),error,sizeof(error)));
        assert(skin.assets[0].rgb==original);
    }
    {
        Canvas *c=calloc(1,sizeof(*c)); SkinState state={0},old;
        unsigned char *full=malloc(sizeof(c->pixels)); unsigned draws;
        assert(c && full); c->skin=&skin; c->height=116;
        strcpy(state.title,"MINTAMP CLASSIC SKIN TEST - LIVE RADIO METADATA");
        state.elapsed=125; state.total=300; state.volume=75; state.playing=1;
        state.rate=44100; state.bitrate=128;
        skin_render(&skin,&state,NULL,blit,fill,c); assert(c->draws);
        draws=c->draws; old=state;
        skin_render(&skin,&state,&old,blit,fill,c); assert(c->draws==draws);
        /* Incremental and full renders must produce identical pixels. */
        state.elapsed=5999; state.total=6000; state.volume=0; state.scroll=7;
        state.pressed=SKIN_VOLUME_SET; state.playing=0; state.mono=1;
        state.balance=-100; state.shuffle=1; state.repeat=2; state.remaining=1; state.paused=1;
        state.visual_mode=1; for (i=0;i<16;++i) state.levels[i]=i;
        skin_render(&skin,&state,&old,blit,fill,c);
        memcpy(full,c->pixels,sizeof(c->pixels));
        skin_render(&skin,&state,NULL,blit,fill,c);
        assert(!memcmp(full,c->pixels,sizeof(c->pixels)));
        old=state; state.visual_mode=2; for (i=0;i<64;++i) state.scope[i]=(signed char)(i*4-128);
        skin_render(&skin,&state,&old,blit,fill,c); memcpy(full,c->pixels,sizeof(c->pixels));
        skin_render(&skin,&state,NULL,blit,fill,c); assert(!memcmp(full,c->pixels,sizeof(c->pixels)));
        assert(skin_hit_test(39,88)==SKIN_PLAY);
        assert(skin_hit_test(85,88)==SKIN_STOP);
        assert(skin_hit_test(136,89)==SKIN_BROWSE);
        assert(skin_hit_test(264,3)==SKIN_QUIT);
        assert(skin_hit_test(242,58)==SKIN_PLAYLIST);
        assert(skin_hit_test(-1,0)==SKIN_NONE && skin_hit_test(275,116)==SKIN_NONE);
        assert(skin_slider_value(SKIN_VOLUME_SET,-1000)==0);
        assert(skin_slider_value(SKIN_VOLUME_SET,1000)==100);
        assert(skin_slider_value(SKIN_SEEK,30)==0 && skin_slider_value(SKIN_SEEK,249)==100);
        if (argc>2) {
            FILE *f=fopen(argv[2],"wb"); assert(f);
            state=old; skin_render(&skin,&state,NULL,blit,fill,c);
            fprintf(f,"P6\n275 116\n255\n"); fwrite(c->pixels,1,275*116*3,f); fclose(f);
        }
        free(full); free(c);
    }
    if (skin.assets[SKIN_PLEDIT].rgb) {
        Canvas *c=calloc(1,sizeof(*c)); SkinPlaylistState state={0},old;
        unsigned char *full=malloc(sizeof(c->pixels)); unsigned draws;
        assert(c && full); c->skin=&skin; c->height=232;
        state.count=30; state.selected=2; state.current=1; state.selection[2]=state.selection[4]=1;
        state.total_seconds=600; state.unknown_durations=1;
        for (i=0;i<SKIN_PLAYLIST_ROWS;++i) snprintf(state.rows[i],80,"Track %d",i+1);
        skin_playlist_render(&skin,&state,NULL,blit,fill,label,c);
        draws=c->draws; old=state;
        skin_playlist_render(&skin,&state,&old,blit,fill,label,c); assert(c->draws==draws);
        state.top=13; state.selected=29; state.current=20; memset(state.selection,0,128); state.selection[29]=1;
        state.total_seconds=800; state.unknown_durations=0;
        for (i=0;i<SKIN_PLAYLIST_ROWS;++i) snprintf(state.rows[i],80,"Track %d",i+14);
        for (i=0;i<SKIN_PLAYLIST_ROWS;++i) state.durations[i]=i%2 ? -1 : 120+i;
        skin_playlist_render(&skin,&state,&old,blit,fill,label,c);
        memcpy(full,c->pixels,sizeof(c->pixels));
        skin_playlist_render(&skin,&state,NULL,blit,fill,label,c);
        assert(!memcmp(full,c->pixels,sizeof(c->pixels)));
        old=state; state.count=0; state.top=0; state.selected=state.current=-1;
        memset(state.rows,0,sizeof(state.rows));
        skin_playlist_render(&skin,&state,&old,blit,fill,label,c);
        memcpy(full,c->pixels,sizeof(c->pixels));
        skin_playlist_render(&skin,&state,NULL,blit,fill,label,c);
        assert(!memcmp(full,c->pixels,sizeof(c->pixels)));
        assert(skin_playlist_top(-99,30)==0 && skin_playlist_top(99,30)==13);
        assert(skin_playlist_top(99,0)==0 && skin_playlist_top(99,17)==0);
        assert(skin_playlist_hit_test(14,22)==SKIN_TRACK_SELECT);
        assert(skin_playlist_hit_test(258,20)==SKIN_PLAYLIST_SCROLL);
        assert(skin_playlist_hit_test(230,202)==SKIN_PLAYLIST_OPTIONS);
        assert(skin_playlist_hit_test(265,4)==SKIN_PLAYLIST_CLOSE);
        assert(skin_playlist_hit_test(275,0)==SKIN_NONE);
        old=state; state.shaded=1; state.active=1; c->height=14;
        skin_playlist_render(&skin,&state,&old,blit,fill,label,c); memcpy(full,c->pixels,275*14*3);
        skin_playlist_render(&skin,&state,NULL,blit,fill,label,c); assert(!memcmp(full,c->pixels,275*14*3));
        old=state; draws=c->draws; skin_playlist_render(&skin,&state,&old,blit,fill,label,c); assert(c->draws==draws);
        old=state; state.shaded=0; state.pressed=SKIN_PLAYLIST_SCROLL; c->height=232;
        skin_playlist_render(&skin,&state,&old,blit,fill,label,c); memcpy(full,c->pixels,sizeof(c->pixels));
        skin_playlist_render(&skin,&state,NULL,blit,fill,label,c); assert(!memcmp(full,c->pixels,sizeof(c->pixels)));
        free(full); free(c);
    }
    skin_free(&skin); return 0;
}
