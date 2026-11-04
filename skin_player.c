/* Shared Intuition window for the SGT and SR editions. Native utility
 * windows remain owned by the respective GadTools/ReAction frontend. */
#include "skin_player.h"
#include "interface_prefs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <exec/memory.h>
#include <graphics/gfxbase.h>
#include <graphics/rastport.h>
#include <graphics/view.h>
#include <graphics/scale.h>
#include <graphics/text.h>
#include <intuition/intuition.h>
#include <libraries/asl.h>
#include <devices/inputevent.h>
#include <devices/timer.h>
#include <workbench/startup.h>
#include <workbench/workbench.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/asl.h>
#include <proto/dos.h>
#include <proto/icon.h>
/* Private workbench.library base for the skin AppIcon: the GadTools edition
 * already owns a global WorkbenchBase and ReAction does not open one. */
#define __NOLIBBASE__
#define WB_BASE_NAME gSkinWorkbenchBase
#include <proto/wb.h>
#undef __NOLIBBASE__

extern struct GfxBase *GfxBase;
static struct Library *gSkinWorkbenchBase;
static int gSkinArgsUsed;

typedef struct SkinResources {
    WinampSkin skin;
    struct BitMap *bitmaps[SKIN_ASSET_COUNT];
    int modern[SKIN_ASSET_COUNT];
    LONG pens[4096];
    UWORD owned[128];
    int owned_count, scale;
} SkinResources;

/* Three right-button menus. Checkmarked items mirror live state and are
 * resynchronised after every action rather than trusting Intuition's toggle. */
typedef struct SkinMenuDef { unsigned char menu, check; int action; const char *label; } SkinMenuDef;
static const SkinMenuDef kMenuDefs[]={
    {0,0,SKIN_BROWSE,"Open audio..."},
    {0,0,SKIN_STREAM,"Open stream URL..."},
    {0,0,SKIN_RADIO,"Internet Radio..."},
    {0,0,SKIN_INFO,"File info..."},
    {0,1,SKIN_SETTINGS,"Settings"},
    {0,0,SKIN_INTERFACE,"Interface preferences..."},
    {0,1,SKIN_CLASSIC,"Classic interface"},
    {0,0,SKIN_ICONIFY,"Iconify"},
    {0,0,SKIN_ABOUT,"About..."},
    {0,0,SKIN_QUIT,"Quit"},
    {1,1,SKIN_PLAYLIST,"Playlist"},
    {1,0,SKIN_PLAYLIST_OPTIONS,"Full playlist options..."},
    {1,1,SKIN_EQ,"Equaliser"},
    {1,1,SKIN_ART,"Artwork"},
    {1,0,SKIN_SELECT,"Load skin..."},
    {1,1,SKIN_SIZE,"Double size"},
    {2,0,SKIN_VISUAL,"Visualisation"},
    {2,0,SKIN_VIS_RATE,"Visual rate..."},
    {2,1,SKIN_FILTER,"Audio filter"},
    {2,0,SKIN_DEBUG,"Playback statistics..."}
};
#define SKIN_MENU_ITEMS ((int)(sizeof(kMenuDefs)/sizeof(kMenuDefs[0])))
#define SKIN_MENU_MAX 20
#define SKIN_MENUS 3
typedef char SkinMenuFits[SKIN_MENU_ITEMS<=SKIN_MENU_MAX ? 1 : -1];
static const char * const kMenuTitles[SKIN_MENUS]={"MintAMP","Windows","Options"};
typedef struct SkinMenuStrip {
    struct Menu menus[SKIN_MENUS];
    struct MenuItem items[SKIN_MENU_MAX];
    struct IntuiText labels[SKIN_MENU_MAX];
    unsigned long checks;
} SkinMenuStrip;
enum { STRIP_TEMPLATE, STRIP_MAIN, STRIP_PLAYLIST, STRIP_ART, STRIP_SETTINGS, STRIP_COUNT };

/* Artwork window: 64x64 frontend artwork inside a skin-coloured frame. */
#define SKIN_ART_SIZE 64
#define SKIN_ART_BOX 74
enum { SKIN_ART_NONE, SKIN_ART_RADIO, SKIN_ART_LOADING, SKIN_ART_IMAGE, SKIN_ART_OFF };
/* Settings window: PLEDIT frame, 10-pixel Topaz rows, up to 19 visible. */
#define SKIN_SET_VISIBLE 19
enum { SET_PRESS_NONE, SET_PRESS_DRAG, SET_PRESS_CLOSE, SET_PRESS_ROW, SET_PRESS_SCROLL };

struct SkinPlayer {
    struct Screen *screen;
    struct Window *win, *plwin, *eqwin, *artwin, *setwin, *target;
    struct BitMap *text_source, *text_scaled;
    struct TextFont *playlist_font;
    struct MsgPort *visual_port;
    struct timerequest *visual_timer;
    int visual_device,visual_pending;
    SkinResources *resources;
    SkinMenuStrip strips[STRIP_COUNT];
    struct MsgPort *app_port;
    struct AppIcon *app_icon;
    struct DiskObject *app_object, app_icon_copy;
    int iconified, art_left, art_top, art_position, restore_art, art_drag_x, art_drag_y, art_dragging;
    int art_kind, art_drawn_kind, art_have_drawn, art_disabled;
    int art_mb, art_size, art_pixels, art_cached, art_pressed;
    struct BitMap *art_bitmap;
    SkinArtState art_frame;
    char art_title[32];
    SkinSettingsRow set_rows[SKIN_SETTINGS_MAX], set_drawn[SKIN_SETTINGS_MAX];
    int set_count, set_focus, set_top, set_visible, set_pressed, set_row, set_have_drawn;
    int set_drawn_count, set_drawn_focus, set_drawn_top, set_drawn_active, set_drawn_pressed;
    int set_left, set_top_edge, set_position, restore_settings, set_drag_x, set_drag_y;
    ULONG art_seconds, art_micros;
    unsigned long art_hash;
    unsigned char art_rgb[SKIN_ART_SIZE*SKIN_ART_SIZE*3];
    SkinState now, drawn;
    SkinOrderQueue sequence;
    SkinSelection selection;
    SkinEqState eq_drawn;
    int eq_have_drawn,eq_pressed,eq_drag_x,eq_drag_y;
    int remaining,shaded,visual_mode,visual_interval,visual_ticks,debug;
    int pl_left,pl_top,eq_left,eq_top,pl_position,eq_position;
    int move_row,move_target,move_active,move_edge,collapse_selection;
    int pl_shaded,eq_shaded,state_dirty,choice_dirty;
    unsigned char peak_hold[16];
    int saved_left,saved_top,restore_playlist,restore_eq;
    unsigned drag_group;
    /* Coalesced group drag: expected window positions and the not yet
     * applied pointer delta. Moves wait until Intuition has applied the
     * previous ones, so a fast drag cannot queue a backlog of layer work. */
    int drag_pdx,drag_pdy,drag_root,drag_waits;
    WORD drag_wx[SKIN_WINDOW_MAX],drag_wy[SKIN_WINDOW_MAX];
    int slider_pending,slider_action,slider_value;
    unsigned long playlist_identity;
    int preserve_selection;
    int have_drawn, pressed, drag_x, drag_y,seek_preview;
    int notice_ticks;
    SkinPlaylistState playlist, playlist_drawn;
    int playlist_have_drawn, playlist_pressed, playlist_drag_x, playlist_drag_y;
    int click_row, pending_options,pending_double;
    ULONG click_seconds, click_micros;
    const int *durations;
    SkinPlaylistName playlist_name;
    void *playlist_ctx;
    char notice[256], name[32], path[512];
};

static void save_state(SkinPlayer *p);
static void playlist_scroll(SkinPlayer *p,int top);
static void visual_arm(SkinPlayer *p);
static void menu_sync(SkinPlayer *p);
static void art_close(SkinPlayer *p);
static int art_open(SkinPlayer *p);
static void settings_close(SkinPlayer *p);
static int settings_open(SkinPlayer *p);

static int suffix(const char *path,const char *ext)
{
    size_t n=strlen(path), e=strlen(ext), i;
    if (n<e) return 0;
    for (i=0;i<e;++i) if (tolower((unsigned char)path[n-e+i])!=ext[i]) return 0;
    return 1;
}
int skin_player_is_skin(const char *path) { return suffix(path,".wsz") || suffix(path,".zip"); }
const char *skin_player_audio_arg(int argc,char **argv)
{
    int i;
    for (i=1;i<argc;++i) {
        int span=mint_launch_option_span(argc,argv,i);
        if (span) { i+=span-1; continue; }
        if (!strcmp(argv[i],"--skin")) { ++i; continue; }
        if (argv[i][0]!='-' && !skin_player_is_skin(argv[i])) return argv[i];
    }
    return NULL;
}
static void error_request(SkinPlayer *p,const char *message)
{
    struct EasyStruct es;
    memset(&es,0,sizeof(es)); es.es_StructSize=sizeof(es);
    es.es_Title=(STRPTR)p->name; es.es_TextFormat=(STRPTR)message;
    es.es_GadgetFormat=(STRPTR)"OK";
    /* Format is fixed: a skin error must never become a printf format. */
    es.es_TextFormat=(STRPTR)"%s";
    EasyRequestArgs(p->win,&es,NULL,(APTR)&message);
}
static void env_name(SkinPlayer *p,char *key,const char *option)
{ snprintf(key,64,"%s/%s",p->name,option); }
static void save_choice(SkinPlayer *p)
{
    char key[64], size[2];
    env_name(p,key,"Skin"); SetVar(key,p->path,-1,GVF_GLOBAL_ONLY);
    size[0]=(char)('0'+p->resources->scale); size[1]=0;
    env_name(p,key,"Scale"); SetVar(key,size,-1,GVF_GLOBAL_ONLY); if (!p->choice_dirty) p->choice_dirty=1;
}
static LONG color_pen(SkinPlayer *p,SkinResources *r,unsigned rgb)
{
    unsigned key=((rgb>>12)&0xf00)|((rgb>>8)&0xf0)|((rgb>>4)&15);
    unsigned red=(rgb>>16)&255, green=(rgb>>8)&255, blue=rgb&255;
    LONG pen;
    if (r->pens[key]>=0) return r->pens[key];
    if (GfxBase->LibNode.lib_Version>=39) {
        ULONG rr=red*0x01010101UL, gg=green*0x01010101UL, bb=blue*0x01010101UL;
        pen=r->owned_count<128 ? ObtainBestPen(p->screen->ViewPort.ColorMap,rr,gg,bb,
              OBP_Precision,PRECISION_IMAGE,TAG_DONE) : -1;
        if (pen>=0) r->owned[r->owned_count++]=(UWORD)pen;
        else pen=FindColor(p->screen->ViewPort.ColorMap,rr,gg,bb,-1);
    } else {
        unsigned count=p->screen->ViewPort.ColorMap->Count, i; unsigned long best=~0UL;
        pen=0;
        for (i=0;i<count;++i) {
            ULONG c=GetRGB4(p->screen->ViewPort.ColorMap,i);
            long dr=(long)red-((c>>8)&15)*17, dg=(long)green-((c>>4)&15)*17, db=(long)blue-(c&15)*17;
            unsigned long distance=(unsigned long)(dr*dr+dg*dg+db*db);
            if (distance<best) { best=distance; pen=(LONG)i; }
        }
    }
    if (pen<0) pen=0;
    r->pens[key]=pen; return pen;
}
static void free_resources(SkinPlayer *p,SkinResources *r)
{
    int i,j;
    if (!r) return;
    WaitBlit();
    for (i=0;i<SKIN_ASSET_COUNT;++i) if (r->bitmaps[i]) {
        if (r->modern[i]) FreeBitMap(r->bitmaps[i]);
        else {
            for (j=0;j<r->bitmaps[i]->Depth;++j) if (r->bitmaps[i]->Planes[j])
                FreeRaster(r->bitmaps[i]->Planes[j],r->skin.assets[i].width*r->scale,
                           r->skin.assets[i].height*r->scale);
            free(r->bitmaps[i]);
        }
    }
    for (i=0;i<r->owned_count;++i) ReleasePen(p->screen->ViewPort.ColorMap,r->owned[i]);
    skin_free(&r->skin); free(r);
}
static SkinResources *load_resources(SkinPlayer *p,const char *path,int scale,char *error)
{
    SkinResources *r=(SkinResources *)calloc(1,sizeof(*r));
    int i,j; unsigned x,y,depth=p->screen->RastPort.BitMap->Depth;
    if (!r) { strcpy(error,"Not enough memory for skin"); return NULL; }
    r->scale=scale;
    for (i=0;i<4096;++i) r->pens[i]=-1;
    if (!skin_load_file(&r->skin,path,error,160)) { free(r); return NULL; }
    for (i=0;i<SKIN_ASSET_COUNT;++i) if (r->skin.assets[i].rgb) {
        SkinBitmap *b=&r->skin.assets[i];
        struct RastPort rp;
        if (GfxBase->LibNode.lib_Version>=39) {
            r->modern[i]=1;
            r->bitmaps[i]=AllocBitMap(b->width*scale,b->height*scale,depth,BMF_CLEAR,
                                    p->screen->RastPort.BitMap);
        } else {
            r->bitmaps[i]=(struct BitMap *)calloc(1,sizeof(struct BitMap));
            if (r->bitmaps[i]) {
                InitBitMap(r->bitmaps[i],depth,b->width*scale,b->height*scale);
                for (j=0;j<(int)depth;++j) {
                    r->bitmaps[i]->Planes[j]=AllocRaster(b->width*scale,b->height*scale);
                    if (!r->bitmaps[i]->Planes[j]) break;
                }
                if (j!=(int)depth) goto no_memory;
            }
        }
        if (!r->bitmaps[i]) goto no_memory;
        InitRastPort(&rp); rp.BitMap=r->bitmaps[i]; SetDrMd(&rp,JAM1);
        /* Paint equal-colour runs once, rather than decoding/converting any
         * images in the playback loop. Blits later copy tiny changed areas. */
        for (y=0;y<b->height;++y) {
            x=0;
            while (x<b->width) {
                unsigned start=x, rgb; LONG pen;
                const unsigned char *pixel=b->rgb+((size_t)y*b->width+x)*3;
                rgb=((unsigned)pixel[0]<<16)|((unsigned)pixel[1]<<8)|pixel[2];
                pen=color_pen(p,r,rgb); ++x;
                while (x<b->width) {
                    pixel=b->rgb+((size_t)y*b->width+x)*3;
                    rgb=((unsigned)pixel[0]<<16)|((unsigned)pixel[1]<<8)|pixel[2];
                    if (color_pen(p,r,rgb)!=pen) break;
                    ++x;
                }
                SetAPen(&rp,(ULONG)pen);
                RectFill(&rp,start*scale,y*scale,x*scale-1,(y+1)*scale-1);
            }
        }
    }
    return r;
no_memory:
    strcpy(error,"Not enough graphics memory to cache skin"); free_resources(p,r); return NULL;
}
static void blit(void *ctx,int id,int sx,int sy,int w,int h,int x,int y)
{
    SkinPlayer *p=(SkinPlayer *)ctx; int s=p->resources->scale;
    if (p->target && p->resources->bitmaps[id])
        BltBitMapRastPort(p->resources->bitmaps[id],sx*s,sy*s,p->target->RPort,x*s,y*s,w*s,h*s,0xc0);
}
static void fill(void *ctx,unsigned rgb,int x,int y,int w,int h)
{
    SkinPlayer *p=(SkinPlayer *)ctx; int s=p->resources->scale;
    SetAPen(p->target->RPort,(ULONG)color_pen(p,p->resources,rgb));
    RectFill(p->target->RPort,x*s,y*s,(x+w)*s-1,(y+h)*s-1);
}
#define SKIN_WINDOWS SKIN_WINDOW_MAX
static void window_list(SkinPlayer *p,struct Window **windows)
{ windows[0]=p->win; windows[1]=p->plwin; windows[2]=p->eqwin; windows[3]=p->artwin; windows[4]=p->setwin; }
/* During a drag, members of the group report where they are going to be. */
static void window_rects(SkinPlayer *p,SkinRect *r,struct Window **windows)
{
    int i;
    window_list(p,windows);
    memset(r,0,SKIN_WINDOWS*sizeof(*r));
    for (i=0;i<SKIN_WINDOWS;++i) if (windows[i]) {
        int moving=(p->drag_group&(1U<<i))!=0;
        r[i].x=moving ? p->drag_wx[i] : windows[i]->LeftEdge; r[i].y=moving ? p->drag_wy[i] : windows[i]->TopEdge;
        r[i].w=windows[i]->Width; r[i].h=windows[i]->Height;
    }
}
/* Mouse movement is reported only while a button is held. Hovering over a
 * skin window then wakes neither the GUI nor, through it, the decoder's
 * share of the CPU. */
static void track_mouse(SkinPlayer *p,struct Window *w,ULONG type,UWORD code);
static void drag_ticks(SkinPlayer *p,int on)
{
    struct Window *windows[SKIN_WINDOWS]; struct Window *w;
    window_list(p,windows); w=p->drag_root>=0 && p->drag_root<SKIN_WINDOWS ? windows[p->drag_root] : NULL;
    if (!w) return;
    /* IntuiTicks flush a held-back move once the pointer stops. */
    if (on) ModifyIDCMP(w,w->IDCMPFlags|IDCMP_INTUITICKS);
    else if (w->IDCMPFlags&IDCMP_INTUITICKS) ModifyIDCMP(w,w->IDCMPFlags&~IDCMP_INTUITICKS);
}
static void drag_begin(SkinPlayer *p,int root,UWORD qualifier)
{
    SkinRect r[SKIN_WINDOWS]; struct Window *windows[SKIN_WINDOWS]; int i;
    p->drag_group=0; window_rects(p,r,windows);
    p->drag_group=(qualifier&IEQUALIFIER_CONTROL) ? 1U<<root : skin_window_group(r,SKIN_WINDOWS,root,2*p->resources->scale);
    for (i=0;i<SKIN_WINDOWS;++i) { p->drag_wx[i]=(WORD)r[i].x; p->drag_wy[i]=(WORD)r[i].y; }
    p->drag_pdx=p->drag_pdy=0; p->drag_root=root; p->drag_waits=0;
    if (p->drag_group) drag_ticks(p,1);
}
static int drag_landed(SkinPlayer *p)
{
    struct Window *windows[SKIN_WINDOWS]; int i;
    window_list(p,windows);
    for (i=0;i<SKIN_WINDOWS;++i) if ((p->drag_group&(1U<<i)) && windows[i] &&
        (windows[i]->LeftEdge!=p->drag_wx[i] || windows[i]->TopEdge!=p->drag_wy[i])) return 0;
    return 1;
}
static void drag_apply(SkinPlayer *p,int force)
{
    SkinRect r[SKIN_WINDOWS]; struct Window *windows[SKIN_WINDOWS];
    int i,j,dx=p->drag_pdx,dy=p->drag_pdy,done[SKIN_WINDOWS];
    if (!p->drag_group || (!dx && !dy)) return;
    if (!force && !drag_landed(p)) {
        /* Normally the next IntuiTick finds the move done. If a window
         * stays elsewhere (another program moved it), trust where it is. */
        if (++p->drag_waits<8) return;
        window_list(p,windows);
        for (i=0;i<SKIN_WINDOWS;++i) if (windows[i]) { p->drag_wx[i]=windows[i]->LeftEdge; p->drag_wy[i]=windows[i]->TopEdge; }
    }
    p->drag_waits=0;
    window_rects(p,r,windows);
    for (i=0;i<SKIN_WINDOWS;++i) if ((p->drag_group&(1U<<i)) && windows[i]) {
        if (r[i].x+dx<0) dx=-r[i].x;
        if (r[i].y+dy<0) dy=-r[i].y;
        if (r[i].x+r[i].w+dx>p->screen->Width) dx=p->screen->Width-r[i].x-r[i].w;
        if (r[i].y+r[i].h+dy>p->screen->Height) dy=p->screen->Height-r[i].y-r[i].h;
    }
    p->drag_pdx=p->drag_pdy=0;
    if (!dx && !dy) return;
    /* Move the leading window first so docked windows never cover each
     * other on the way; with smart refresh nothing then needs repainting. */
    for (i=0;i<SKIN_WINDOWS;++i) done[i]=!((p->drag_group&(1U<<i)) && windows[i]);
    for (;;) {
        int best=-1; long score=0;
        for (j=0;j<SKIN_WINDOWS;++j) if (!done[j]) {
            long v=(long)r[j].x*(dx>0 ? 1 : dx<0 ? -1 : 0)+(long)r[j].y*(dy>0 ? 1 : dy<0 ? -1 : 0);
            if (best<0 || v>score) { best=j; score=v; }
        }
        if (best<0) break;
        done[best]=1; MoveWindow(windows[best],dx,dy);
        p->drag_wx[best]=(WORD)(r[best].x+dx); p->drag_wy[best]=(WORD)(r[best].y+dy);
    }
}
static void drag_move(SkinPlayer *p,int dx,int dy)
{
    if (!p->drag_group) return;
    p->drag_pdx+=dx; p->drag_pdy+=dy; drag_apply(p,0);
}
static void drag_end(SkinPlayer *p,int root)
{
    SkinRect r[SKIN_WINDOWS]; struct Window *windows[SKIN_WINDOWS]; int dx,dy;
    if (!p->drag_group) return;
    drag_apply(p,1);
    window_rects(p,r,windows);
    skin_window_snap(r,SKIN_WINDOWS,root,p->drag_group,8*p->resources->scale,&dx,&dy);
    p->drag_pdx=dx; p->drag_pdy=dy; drag_apply(p,1);
    drag_ticks(p,0); p->drag_group=0;
}
/* Focus lost mid-drag: finish where the pointer was, without snapping. */
static void drag_cancel(SkinPlayer *p)
{
    if (!p->drag_group) return;
    drag_apply(p,1); drag_ticks(p,0); p->drag_group=0;
}
static void track_mouse(SkinPlayer *p,struct Window *w,ULONG type,UWORD code)
{
    if (type==IDCMP_MOUSEBUTTONS && code==SELECTDOWN) ReportMouse(TRUE,w);
    else if ((type==IDCMP_MOUSEBUTTONS && code==SELECTUP) || type==IDCMP_INACTIVEWINDOW) {
        ReportMouse(FALSE,w);
        if (type==IDCMP_INACTIVEWINDOW) drag_cancel(p);
    }
}
static void repaint(SkinPlayer *p,int all)
{
    SkinState display;
    if (!p->win || !p->resources) return;
    p->target=p->win;
    p->now.pressed=p->pressed;
    p->now.active=(p->win->Flags&WFLG_WINDOWACTIVE)!=0;
    p->now.playlist_visible=p->plwin!=NULL;
    p->now.eq_visible=p->eqwin!=NULL;
    display=p->now;
    if (p->pressed==SKIN_SEEK && display.total>0) display.elapsed=(int)((long)display.total*p->seek_preview/100);
    skin_render(&p->resources->skin,&display,(all || !p->have_drawn) ? NULL : &p->drawn,blit,fill,p);
    p->drawn=display; p->have_drawn=1;
}
static struct BitMap *text_bitmap(SkinPlayer *p,int width,int height)
{
    struct BitMap *bm; int i;
    if (GfxBase->LibNode.lib_Version>=39)
        return AllocBitMap(width,height,p->screen->RastPort.BitMap->Depth,
                           BMF_CLEAR,p->screen->RastPort.BitMap);
    bm=(struct BitMap *)calloc(1,sizeof(*bm));
    if (!bm) return NULL;
    InitBitMap(bm,p->screen->RastPort.BitMap->Depth,width,height);
    for (i=0;i<bm->Depth;++i) {
        bm->Planes[i]=AllocRaster(width,height);
        if (!bm->Planes[i]) {
            while (i--) FreeRaster(bm->Planes[i],width,height);
            free(bm); return NULL;
        }
    }
    return bm;
}
static void free_text_bitmap(struct BitMap *bm,int width,int height)
{
    int i;
    if (!bm) return;
    WaitBlit();
    if (GfxBase->LibNode.lib_Version>=39) { FreeBitMap(bm); return; }
    for (i=0;i<bm->Depth;++i) FreeRaster(bm->Planes[i],width,height);
    free(bm);
}
static void playlist_label(void *ctx,const char *text,unsigned rgb,unsigned bg,int x,int y,int width)
{
    SkinPlayer *p=(SkinPlayer *)ctx; struct RastPort rp;
    struct BitScaleArgs args; char line[96]; int i,n,scale=p->resources->scale;
    n=(int)strlen(text); if (n>width/8) n=width/8;
    for (i=0;i<n;++i) line[i]=(unsigned char)text[i]<32 ? ' ' : text[i];
    InitRastPort(&rp); rp.BitMap=p->text_source; SetFont(&rp,p->playlist_font);
    SetAPen(&rp,(ULONG)color_pen(p,p->resources,bg)); RectFill(&rp,0,0,242,7);
    SetDrMd(&rp,JAM1); SetAPen(&rp,(ULONG)color_pen(p,p->resources,rgb));
    Move(&rp,0,p->playlist_font->tf_Baseline); Text(&rp,line,n);
    if (scale==2) {
        memset(&args,0,sizeof(args));
        args.bsa_SrcWidth=width; args.bsa_SrcHeight=8;
        args.bsa_XSrcFactor=args.bsa_YSrcFactor=1;
        args.bsa_XDestFactor=args.bsa_YDestFactor=2;
        args.bsa_SrcBitMap=p->text_source; args.bsa_DestBitMap=p->text_scaled;
        WaitBlit(); BitMapScale(&args);
    }
    BltBitMapRastPort(scale==2 ? p->text_scaled : p->text_source,0,0,
                     p->target->RPort,x*scale,y*scale,width*scale,8*scale,0xc0);
    /* The next row reuses the source, and must not race a queued blit. */
    WaitBlit();
}
static void playlist_repaint(SkinPlayer *p,int all)
{
    if (!p->plwin) return;
    p->target=p->plwin;
    p->playlist.active=(p->plwin->Flags&WFLG_WINDOWACTIVE)!=0;
    p->playlist.shaded=p->pl_shaded; p->playlist.pressed=p->playlist_pressed;
    skin_playlist_render(&p->resources->skin,&p->playlist,
        all || !p->playlist_have_drawn ? NULL : &p->playlist_drawn,
        blit,fill,playlist_label,p);
    p->playlist_drawn=p->playlist; p->playlist_have_drawn=1;
}
static void playlist_close(SkinPlayer *p)
{
    struct Message *m;
    if (!p->plwin) return;
    ClearMenuStrip(p->plwin);
    while ((m=GetMsg(p->plwin->UserPort))!=NULL) ReplyMsg(m);
    p->pl_left=p->plwin->LeftEdge; p->pl_top=p->plwin->TopEdge; p->pl_position=1;
    CloseWindow(p->plwin); p->plwin=NULL;
    p->playlist_pressed=0; p->click_row=-1; p->playlist_have_drawn=0;
    p->move_active=p->move_edge=p->pending_double=p->collapse_selection=0; p->move_row=-1;
    repaint(p,0);
}
static int text_ready(SkinPlayer *p)
{
    struct TextAttr font={(STRPTR)"topaz.font",8,FS_NORMAL,FPF_ROMFONT};
    if (!p->playlist_font) p->playlist_font=OpenFont(&font);
    if (!p->text_source) p->text_source=text_bitmap(p,243,8);
    if (!p->text_scaled) p->text_scaled=text_bitmap(p,486,16);
    return p->playlist_font && p->text_source && p->text_scaled;
}
static void strip_attach(SkinPlayer *p,int index,struct Window *w);
static int playlist_open(SkinPlayer *p)
{
    int scale=p->resources->scale, left,top;
    int height=p->pl_shaded ? 14 : SKIN_PLAYLIST_HEIGHT;
    if (!p->win || !p->resources->skin.assets[SKIN_PLEDIT].rgb ||
        275*scale>p->screen->Width || height*scale>p->screen->Height) return 0;
    if (!text_ready(p)) return 0;
    left=p->pl_position ? p->pl_left : p->win->LeftEdge; top=p->pl_position ? p->pl_top : p->win->TopEdge+p->win->Height;
    if (left<0) left=0;
    if (top<0) top=0;
    if (left+275*scale>p->screen->Width) left=p->screen->Width-275*scale;
    if (top+height*scale>p->screen->Height) top=p->screen->Height-height*scale;
    p->plwin=OpenWindowTags(NULL,WA_CustomScreen,(ULONG)p->screen,
        WA_Left,left,WA_Top,top,WA_Width,275*scale,WA_Height,height*scale,
        WA_Borderless,TRUE,WA_Activate,TRUE,WA_RMBTrap,FALSE,
        WA_DetailPen,p->win->DetailPen,WA_BlockPen,p->win->BlockPen,
        WA_NewLookMenus,(ULONG)(GfxBase->LibNode.lib_Version>=39),
        WA_ReportMouse,FALSE,WA_SmartRefresh,TRUE,
        WA_IDCMP,IDCMP_MOUSEBUTTONS|IDCMP_MOUSEMOVE|IDCMP_REFRESHWINDOW|
                   IDCMP_MENUPICK|IDCMP_VANILLAKEY|IDCMP_RAWKEY|IDCMP_INACTIVEWINDOW|IDCMP_ACTIVEWINDOW,TAG_DONE);
    if (!p->plwin) return 0;
    strip_attach(p,STRIP_PLAYLIST,p->plwin); p->click_row=-1;
    playlist_repaint(p,1); repaint(p,0); menu_sync(p); return 1;
}
int skin_player_playlist_toggle(SkinPlayer *p)
{
    if (!p) return 0;
    if (p->plwin) { playlist_close(p); save_state(p); menu_sync(p); return 1; }
    { int ok=playlist_open(p); save_state(p); menu_sync(p); return ok; }
}
void skin_player_playlist_durations(SkinPlayer *p,const int *durations)
{ if (p) p->durations=durations; }
void skin_player_playlist_update(SkinPlayer *p,int count,int selected,int current,
                                SkinPlaylistName name,void *ctx)
{
    int i, top;
    if (!p) return;
    p->playlist_name=name; p->playlist_ctx=ctx;
    top=skin_playlist_top(p->playlist.top,count);
    if (selected!=p->playlist.selected && selected>=0 && selected<count) {
        if (selected<top) top=selected;
        if (selected>=top+SKIN_PLAYLIST_ROWS) top=selected-SKIN_PLAYLIST_ROWS+1;
    }
    if (count!=p->playlist.count || selected!=p->playlist.selected || current!=p->playlist.current)
        p->click_row=-1;
    skin_order_queue_observe(&p->sequence,count,current);
    skin_selection_sync(&p->selection,count,selected);
    memcpy(p->playlist.selection,p->selection.bits,sizeof(p->playlist.selection));
    p->playlist.count=count; p->playlist.selected=selected; p->playlist.current=current;
    p->playlist.top=skin_playlist_top(top,count);
    p->playlist.total_seconds=0; p->playlist.unknown_durations=0;
    for (i=0;i<count;++i) {
        if (p->durations && p->durations[i]>=0) p->playlist.total_seconds+=p->durations[i];
        else ++p->playlist.unknown_durations;
    }
    for (i=0;i<SKIN_PLAYLIST_ROWS;++i) {
        int index=p->playlist.top+i;
        const char *s=index<count && name ? name(ctx,index) : "";
        p->playlist.durations[i]=index<count && p->durations ? p->durations[index] : -1;
        strncpy(p->playlist.rows[i],s,79); p->playlist.rows[i][79]=0;
    }
    playlist_repaint(p,0);
}
static void close_window(SkinPlayer *p)
{
    struct Message *m;
    if (!p->win) return;
    ClearMenuStrip(p->win);
    while ((m=GetMsg(p->win->UserPort))!=NULL) ReplyMsg(m);
    CloseWindow(p->win); p->win=NULL;
}
static void build_menu(SkinPlayer *p,UWORD text_pen,UWORD background_pen)
{
    SkinMenuStrip *t=&p->strips[STRIP_TEMPLATE];
    int i,m,h=p->screen->Font->ta_YSize+4,left=0;
    memset(t,0,sizeof(*t));
    for (m=0;m<SKIN_MENUS;++m) {
        int width=0,count=0,check=0;
        struct MenuItem *last=NULL;
        for (i=0;i<SKIN_MENU_ITEMS;++i) if (kMenuDefs[i].menu==m && kMenuDefs[i].check) check=CHECKWIDTH;
        for (i=0;i<SKIN_MENU_ITEMS;++i) if (kMenuDefs[i].menu==m) {
            int w=TextLength(&p->screen->RastPort,(STRPTR)kMenuDefs[i].label,strlen(kMenuDefs[i].label))+check+16;
            if (w>width) width=w;
        }
        t->menus[m].Flags=MENUENABLED; t->menus[m].MenuName=(STRPTR)kMenuTitles[m];
        t->menus[m].LeftEdge=left;
        t->menus[m].Width=TextLength(&p->screen->RastPort,(STRPTR)kMenuTitles[m],strlen(kMenuTitles[m]))+16;
        t->menus[m].Height=h; left+=t->menus[m].Width;
        t->menus[m].NextMenu=m<SKIN_MENUS-1 ? &t->menus[m+1] : NULL;
        for (i=0;i<SKIN_MENU_ITEMS;++i) if (kMenuDefs[i].menu==m) {
            struct MenuItem *item=&t->items[i]; struct IntuiText *label=&t->labels[i];
            if (last) last->NextItem=item; else t->menus[m].FirstItem=item;
            last=item;
            item->TopEdge=count++*h; item->Width=width; item->Height=h;
            item->Flags=ITEMTEXT|ITEMENABLED|HIGHCOMP|(kMenuDefs[i].check ? CHECKIT|MENUTOGGLE : 0);
            item->ItemFill=label; item->NextSelect=MENUNULL;
            label->FrontPen=(UBYTE)text_pen; label->BackPen=(UBYTE)background_pen; label->DrawMode=JAM1;
            label->ITextFont=p->screen->Font;
            label->LeftEdge=4+check; label->TopEdge=2; label->IText=(UBYTE *)kMenuDefs[i].label;
        }
    }
    t->checks=~0UL;
}
/* Intuition may annotate menu structures, so each window owns a copy. */
static void strip_copy(SkinMenuStrip *d,const SkinMenuStrip *t)
{
    int i,m;
    *d=*t;
    for (m=0;m<SKIN_MENUS;++m) {
        d->menus[m].NextMenu=m<SKIN_MENUS-1 ? &d->menus[m+1] : NULL;
        d->menus[m].FirstItem=t->menus[m].FirstItem ? &d->items[t->menus[m].FirstItem-t->items] : NULL;
    }
    for (i=0;i<SKIN_MENU_ITEMS;++i) {
        d->items[i].NextItem=t->items[i].NextItem ? &d->items[t->items[i].NextItem-t->items] : NULL;
        d->items[i].ItemFill=&d->labels[i];
    }
}
static unsigned long menu_checks(SkinPlayer *p)
{
    unsigned long bits=0; int i;
    for (i=0;i<SKIN_MENU_ITEMS;++i) {
        int on=0;
        switch (kMenuDefs[i].action) {
        case SKIN_PLAYLIST: on=p->plwin!=NULL; break;
        case SKIN_EQ: on=p->eqwin!=NULL; break;
        case SKIN_ART: on=p->artwin!=NULL; break;
        case SKIN_SETTINGS: on=p->setwin!=NULL; break;
        case SKIN_SIZE: on=p->resources && p->resources->scale==2; break;
        case SKIN_FILTER: on=p->now.filter; break;
        default: break; /* Classic interface is never ticked while skinned. */
        }
        if (on) bits|=1UL<<i;
    }
    return bits;
}
static void strip_checks(SkinMenuStrip *strip,unsigned long bits)
{
    int i;
    for (i=0;i<SKIN_MENU_ITEMS;++i) if (kMenuDefs[i].check) {
        if (bits&(1UL<<i)) strip->items[i].Flags|=CHECKED; else strip->items[i].Flags&=~CHECKED;
    }
    strip->checks=bits;
}
static struct Window *strip_window(SkinPlayer *p,int index)
{ return index==STRIP_MAIN ? p->win : index==STRIP_PLAYLIST ? p->plwin : index==STRIP_ART ? p->artwin :
         index==STRIP_SETTINGS ? p->setwin : NULL; }
static void strip_attach(SkinPlayer *p,int index,struct Window *w)
{
    strip_copy(&p->strips[index],&p->strips[STRIP_TEMPLATE]);
    strip_checks(&p->strips[index],menu_checks(p));
    SetMenuStrip(w,&p->strips[index].menus[0]);
}
static void menu_sync(SkinPlayer *p)
{
    unsigned long bits=menu_checks(p); int i;
    for (i=STRIP_MAIN;i<STRIP_COUNT;++i) {
        struct Window *w=strip_window(p,i);
        if (!w || p->strips[i].checks==bits) continue;
        ClearMenuStrip(w); strip_checks(&p->strips[i],bits);
        ResetMenuStrip(w,&p->strips[i].menus[0]);
    }
}
static int open_window(SkinPlayer *p,WORD left,WORD top)
{
    int scale=p->resources->scale;
    int height=p->shaded ? 14 : 116;
    struct DrawInfo *dri;
    UWORD text_pen=p->screen->DetailPen, background_pen=p->screen->BlockPen;
    if (SKIN_WIDTH*scale>p->screen->Width || height*scale>p->screen->Height) return 0;
    if (left<0) left=0;
    if (top<0) top=0;
    if (left+275*scale>p->screen->Width) left=p->screen->Width-275*scale;
    if (top+height*scale>p->screen->Height) top=p->screen->Height-height*scale;
    /* Menus use window pens even on a borderless window. Zero defaults make
     * the menu title disappear. V39 adds dedicated menu-bar pens; V37/38
     * have the standard text/background pens. Never assume palette indices. */
    dri=GetScreenDrawInfo(p->screen);
    if (dri) {
        text_pen=dri->dri_Pens[dri->dri_NumPens>BARDETAILPEN ? BARDETAILPEN : TEXTPEN];
        background_pen=dri->dri_Pens[dri->dri_NumPens>BARBLOCKPEN ? BARBLOCKPEN : BACKGROUNDPEN];
        FreeScreenDrawInfo(p->screen,dri);
    }
    p->win=OpenWindowTags(NULL,WA_CustomScreen,(ULONG)p->screen,
        WA_Left,(ULONG)left,WA_Top,(ULONG)top,
        WA_Width,SKIN_WIDTH*scale,WA_Height,height*scale,
        WA_Borderless,TRUE,WA_Activate,TRUE,WA_RMBTrap,FALSE,
        WA_DetailPen,(ULONG)text_pen,WA_BlockPen,(ULONG)background_pen,
        WA_NewLookMenus,(ULONG)(GfxBase->LibNode.lib_Version>=39),
        WA_ReportMouse,FALSE,WA_SmartRefresh,TRUE,
        WA_IDCMP,IDCMP_MOUSEBUTTONS|IDCMP_MOUSEMOVE|IDCMP_REFRESHWINDOW|
                   IDCMP_MENUPICK|IDCMP_VANILLAKEY|IDCMP_RAWKEY|IDCMP_INACTIVEWINDOW|IDCMP_ACTIVEWINDOW,
        TAG_DONE);
    if (!p->win) return 0;
    build_menu(p,text_pen,background_pen); strip_attach(p,STRIP_MAIN,p->win);
    p->have_drawn=0; repaint(p,1); return 1;
}
static int request_path(SkinPlayer *p,char *path)
{
    struct FileRequester *fr=(struct FileRequester *)AllocAslRequestTags(ASL_FileRequest,
        ASLFR_TitleText,(ULONG)"Select a classic Winamp skin",
        ASLFR_DoPatterns,TRUE,ASLFR_InitialPattern,(ULONG)"#?.(wsz|zip)",
        ASLFR_InitialDrawer,(ULONG)"PROGDIR:Skins",TAG_DONE);
    int ok=0;
    if (!fr) return 0;
    if (AslRequestTags(fr,ASLFR_Window,(ULONG)p->win,ASLFR_SleepWindow,TRUE,TAG_DONE)) {
        strncpy(path,(const char *)fr->fr_Drawer,511); path[511]=0;
        ok=AddPart(path,fr->fr_File,512)!=0;
    }
    FreeAslRequest(fr); return ok;
}
static void save_state(SkinPlayer *p)
{
    char key[64],text[512]; int i,n;
    /* While iconified the windows are closed; the state saved on iconify
     * (including which windows to reopen) remains authoritative. */
    if (!p->win) return;
    if (p->plwin) { p->pl_left=p->plwin->LeftEdge; p->pl_top=p->plwin->TopEdge; p->pl_position=1; }
    if (p->eqwin) { p->eq_left=p->eqwin->LeftEdge; p->eq_top=p->eqwin->TopEdge; p->eq_position=1; }
    if (p->artwin) { p->art_left=p->artwin->LeftEdge; p->art_top=p->artwin->TopEdge; p->art_position=1; }
    n=snprintf(text,sizeof(text),"4 %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d",
        p->win->LeftEdge,p->win->TopEdge,p->pl_left,p->pl_top,p->eq_left,p->eq_top,
        p->sequence.order.shuffle,p->sequence.order.repeat,gSkinAudio.balance,p->remaining,p->shaded,
        p->visual_mode,p->visual_interval,p->debug,gSkinAudio.eq_enabled,gSkinAudio.eq_preamp);
    for (i=0;i<10 && n>0 && n<(int)sizeof(text)-16;++i)
        n+=snprintf(text+n,sizeof(text)-n," %d",gSkinAudio.eq_bands[i]);
    n+=snprintf(text+n,sizeof(text)-n," %d %d %d %d %d %d %d %d %d %d",p->plwin!=NULL,p->eqwin!=NULL,p->pl_position,p->eq_position,p->pl_shaded,p->eq_shaded,
        p->artwin!=NULL,p->art_position,p->art_left,p->art_top);
    env_name(p,key,"State"); SetVar(key,text,-1,GVF_GLOBAL_ONLY); p->state_dirty=1;
}
static void restore_state(SkinPlayer *p)
{
    char key[64],text[512],*at,*end; long v[27],extra[10]; int i,count;
    p->saved_left=40; p->saved_top=30; p->visual_interval=2;
    env_name(p,key,"State");
    if (GetVar(key,text,sizeof(text),GVF_GLOBAL_ONLY)<=0) return;
    text[sizeof(text)-1]=0; at=text;
    for (i=0;i<27;++i) { v[i]=strtol(at,&end,10); if (end==at || v[i]<-32768 || v[i]>32767) return; at=end; }
    if (v[0]<1 || v[0]>4 || v[7]<0 || v[7]>1 || v[8]<0 || v[8]>2 || v[9]<-100 || v[9]>100 ||
        v[12]<0 || v[12]>2 || v[13]<1 || v[13]>4 || v[16]<-12 || v[16]>12) return;
    for (i=17;i<27;++i) if (v[i]<-12 || v[i]>12) return;
    p->saved_left=(int)v[1]; p->saved_top=(int)v[2];
    p->pl_left=(int)v[3]; p->pl_top=(int)v[4]; p->eq_left=(int)v[5]; p->eq_top=(int)v[6];
    p->pl_position=p->eq_position=1; p->sequence.order.shuffle=(int)v[7]; p->sequence.order.repeat=(int)v[8];
    gSkinAudio.balance=(int)v[9]; p->remaining=v[10]!=0; p->shaded=v[11]!=0;
    p->visual_mode=(int)v[12]; p->visual_interval=(int)v[13]; p->debug=0; /* former title overlay; the song title always stays */
    gSkinAudio.eq_enabled=v[15]!=0; gSkinAudio.eq_preamp=(int)v[16];
    for (i=0;i<10;++i) gSkinAudio.eq_bands[i]=(int)v[17+i];
    /* Version 2 adds visibility/positions, 3 shade states, 4 artwork. */
    for (count=0;count<10;++count) {
        extra[count]=strtol(at,&end,10);
        if (end==at || extra[count]<-32768 || extra[count]>32767) break;
        at=end;
    }
    if (v[0]>=2) {
        if (count>0) p->restore_playlist=extra[0]==1;
        if (count>1) p->restore_eq=extra[1]==1;
        if (count>2) p->pl_position=extra[2]==1;
        if (count>3) p->eq_position=extra[3]==1;
    }
    if (v[0]>=3) {
        if (count>4) p->pl_shaded=extra[4]==1;
        if (count>5) p->eq_shaded=extra[5]==1;
    }
    if (v[0]>=4 && count>9) {
        p->restore_art=extra[6]==1; p->art_position=extra[7]==1;
        p->art_left=(int)extra[8]; p->art_top=(int)extra[9];
    }
    ++gSkinAudio.eq_sequence;
}
static void eq_repaint(SkinPlayer *p,int all)
{
    SkinEqState s; int i;
    if (!p->eqwin) return;
    memset(&s,0,sizeof(s)); s.enabled=gSkinAudio.eq_enabled && gSkinAudio.eq_supported;
    s.preamp=gSkinAudio.eq_preamp; s.pressed=p->eq_pressed;
    s.shaded=p->eq_shaded; s.active=(p->eqwin->Flags&WFLG_WINDOWACTIVE)!=0;
    for (i=0;i<10;++i) s.bands[i]=gSkinAudio.eq_bands[i];
    p->target=p->eqwin;
    skin_eq_render(&p->resources->skin,&s,(all || !p->eq_have_drawn) ? NULL : &p->eq_drawn,blit,fill,p);
    p->eq_drawn=s; p->eq_have_drawn=1;
}
static void eq_close(SkinPlayer *p)
{
    struct Message *m;
    if (!p->eqwin) return;
    p->eq_left=p->eqwin->LeftEdge; p->eq_top=p->eqwin->TopEdge; p->eq_position=1;
    while ((m=GetMsg(p->eqwin->UserPort))!=NULL) ReplyMsg(m);
    CloseWindow(p->eqwin); p->eqwin=NULL; p->eq_pressed=0; p->eq_have_drawn=0;
    repaint(p,0); menu_sync(p);
}
static void eq_toggle(SkinPlayer *p)
{
    int scale=p->resources->scale,left,top;
    int height=p->eq_shaded ? 14 : 116;
    if (p->eqwin) { eq_close(p); return; }
    if (!p->resources->skin.assets[SKIN_EQMAIN].rgb) { skin_player_notice(p,"THIS SKIN HAS NO EQMAIN ARTWORK"); return; }
    left=p->eq_position ? p->eq_left : p->win->LeftEdge;
    top=p->eq_position ? p->eq_top : p->win->TopEdge+p->win->Height;
    if (left<0) left=0;
    if (top<0) top=0;
    if (left+275*scale>p->screen->Width) left=p->screen->Width-275*scale;
    if (top+height*scale>p->screen->Height) top=p->screen->Height-height*scale;
    p->eqwin=OpenWindowTags(NULL,WA_CustomScreen,(ULONG)p->screen,WA_Left,left,WA_Top,top,
        WA_Width,275*scale,WA_Height,height*scale,WA_Borderless,TRUE,WA_Activate,TRUE,
        WA_RMBTrap,TRUE,WA_ReportMouse,FALSE,WA_SmartRefresh,TRUE,
        WA_IDCMP,IDCMP_MOUSEBUTTONS|IDCMP_MOUSEMOVE|IDCMP_REFRESHWINDOW|IDCMP_VANILLAKEY|IDCMP_INACTIVEWINDOW|IDCMP_ACTIVEWINDOW,TAG_DONE);
    if (!p->eqwin) { skin_player_notice(p,"COULD NOT OPEN EQUALISER"); return; }
    eq_repaint(p,1); repaint(p,0); menu_sync(p);
}
/* ---- Artwork window ------------------------------------------------------
 * With MB.BMP the window uses the classic mini-browser frame: a 128-pixel
 * square (64 when a doubled frame would not fit the screen), and the track
 * title in the bottom bar's text box (PLEDIT.TXT MbBG/MbFG colours).
 * Without it, a plain 74x74 box in the playlist colours. The frontend's
 * 64x64 RGB is smoothed to 128 with integer averaging, remapped to pens
 * once per new image into a cached bitmap, and blitted on every repaint. */
static void art_layout(SkinPlayer *p,int *x,int *y)
{
    if (p->art_mb) { *x=SKIN_ART_X(p->art_size); *y=SKIN_ART_Y; }
    else { *x=5; *y=5; }
}
static unsigned art_pixel(const unsigned char *rgb,int size,int x,int y)
{
    const unsigned char *a;
    if (size==SKIN_ART_SIZE) { a=rgb+((size_t)y*SKIN_ART_SIZE+x)*3; return ((unsigned)a[0]<<16)|((unsigned)a[1]<<8)|a[2]; }
    {
        /* 2x: average the (up to) four nearest source pixels. */
        int sx=x>>1,sy=y>>1,nx=sx+(x&1),ny=sy+(y&1),c; unsigned out=0;
        const unsigned char *b,*d,*e;
        if (nx>=SKIN_ART_SIZE) nx=SKIN_ART_SIZE-1;
        if (ny>=SKIN_ART_SIZE) ny=SKIN_ART_SIZE-1;
        a=rgb+((size_t)sy*SKIN_ART_SIZE+sx)*3; b=rgb+((size_t)sy*SKIN_ART_SIZE+nx)*3;
        d=rgb+((size_t)ny*SKIN_ART_SIZE+sx)*3; e=rgb+((size_t)ny*SKIN_ART_SIZE+nx)*3;
        for (c=0;c<3;++c) out=(out<<8)|(unsigned)((a[c]+b[c]+d[c]+e[c]+2)>>2);
        return out;
    }
}
/* Paint the art as equal-pen runs into rp at pixel offset (ox,oy). */
static void art_paint(SkinPlayer *p,struct RastPort *rp,int ox,int oy)
{
    int x,y,size=p->art_size,s=p->resources->scale;
    for (y=0;y<size;++y) {
        x=0;
        while (x<size) {
            int start=x; LONG pen=color_pen(p,p->resources,art_pixel(p->art_rgb,size,x,y));
            for (++x;x<size;++x) if (color_pen(p,p->resources,art_pixel(p->art_rgb,size,x,y))!=pen) break;
            SetAPen(rp,(ULONG)pen);
            RectFill(rp,ox+start*s,oy+y*s,ox+x*s-1,oy+(y+1)*s-1);
        }
    }
}
static void art_free_bitmap(SkinPlayer *p)
{
    if (p->art_bitmap) free_text_bitmap(p->art_bitmap,p->art_pixels,p->art_pixels);
    p->art_bitmap=NULL; p->art_pixels=0; p->art_cached=0;
}
static void art_label(SkinPlayer *p,const char *text,int y,unsigned rgb,unsigned bg)
{
    int ax,ay,n=(int)strlen(text);
    if (!n) return;
    if (n>p->art_size/8) n=p->art_size/8;
    art_layout(p,&ax,&ay);
    playlist_label(p,text,rgb,bg,ax+(p->art_size-n*8)/2,ay+y,n*8);
}
#define SKIN_IDLE_TITLE "MINTAMP - RIGHT CLICK FOR MENU"
static const char *art_status(SkinPlayer *p)
{
    int titled=p->art_title[0] && strncmp(p->art_title,SKIN_IDLE_TITLE,sizeof(p->art_title)-1);
    return p->art_kind==SKIN_ART_OFF ? "ART OFF - PRESS T" : p->art_kind==SKIN_ART_LOADING ? "LOADING ARTWORK" :
        titled ? p->art_title : p->art_kind==SKIN_ART_RADIO ? "NO STATION LOGO" : "NO ARTWORK";
}
static void art_repaint(SkinPlayer *p,int all)
{
    const WinampSkin *k; int ax,ay,s,content; unsigned fg,bg;
    if (!p->artwin || !p->resources || !text_ready(p)) return;
    k=&p->resources->skin; p->target=p->artwin; s=p->resources->scale;
    if (p->art_mb) {
        SkinArtState a;
        memset(&a,0,sizeof(a)); a.art=p->art_size; a.pressed=p->art_pressed;
        a.active=(p->artwin->Flags&WFLG_WINDOWACTIVE)!=0;
        strncpy(a.text,art_status(p),sizeof(a.text)-1);
        skin_art_render(k,&a,all || !p->art_have_drawn ? NULL : &p->art_frame,blit,fill,playlist_label,p);
        p->art_frame=a; fg=k->mb_text; bg=k->mb_background;
    } else {
        if (all || !p->art_have_drawn) fill(p,k->playlist_selected,0,0,SKIN_ART_BOX,SKIN_ART_BOX);
        fg=k->playlist_normal; bg=k->playlist_background;
    }
    content=all || !p->art_have_drawn || p->art_drawn_kind!=p->art_kind ||
        (p->art_kind==SKIN_ART_IMAGE && !p->art_cached);
    p->art_have_drawn=1;
    if (!content) return;
    art_layout(p,&ax,&ay);
    if (p->art_kind==SKIN_ART_IMAGE) {
        int pixels=p->art_size*s;
        if (p->art_bitmap && p->art_pixels!=pixels) art_free_bitmap(p);
        if (!p->art_bitmap && (p->art_bitmap=text_bitmap(p,pixels,pixels))!=NULL) p->art_pixels=pixels;
        if (p->art_bitmap) {
            if (!p->art_cached) {
                struct RastPort rp;
                InitRastPort(&rp); rp.BitMap=p->art_bitmap; art_paint(p,&rp,0,0); p->art_cached=1;
            }
            BltBitMapRastPort(p->art_bitmap,0,0,p->artwin->RPort,ax*s,ay*s,pixels,pixels,0xc0);
        } else art_paint(p,p->artwin->RPort,ax*s,ay*s); /* no chip RAM for a cache */
    } else {
        int mid=p->art_size/2;
        if (p->art_mb) fill(p,bg,ax,ay,p->art_size,p->art_size);
        else fill(p,bg,2,2,SKIN_ART_BOX-4,SKIN_ART_BOX-4);
        if (p->art_kind==SKIN_ART_OFF) { art_label(p,"ART OFF",mid-14,fg,bg); art_label(p,"T FOR",mid-4,fg,bg); art_label(p,"SETTINGS",mid+6,fg,bg); }
        else if (p->art_kind==SKIN_ART_LOADING) art_label(p,"LOADING",mid-4,fg,bg);
        else if (p->art_kind==SKIN_ART_RADIO) { art_label(p,"RADIO",mid-9,fg,bg); art_label(p,"NO LOGO",mid+1,fg,bg); }
        else art_label(p,"NO ART",mid-4,fg,bg);
    }
    p->art_drawn_kind=p->art_kind;
}
static void art_close(SkinPlayer *p)
{
    struct Message *m;
    if (!p->artwin) return;
    ClearMenuStrip(p->artwin);
    while ((m=GetMsg(p->artwin->UserPort))!=NULL) ReplyMsg(m);
    p->art_left=p->artwin->LeftEdge; p->art_top=p->artwin->TopEdge; p->art_position=1;
    CloseWindow(p->artwin); p->artwin=NULL; p->art_have_drawn=0; p->art_dragging=0; p->art_pressed=0;
    /* Pens belong to the skin; the next open remaps the image. */
    art_free_bitmap(p);
    menu_sync(p);
}
static int art_open(SkinPlayer *p)
{
    int scale=p->resources->scale,width,height,left,top;
    if (!p->win || !text_ready(p)) return 0;
    p->art_mb=p->resources->skin.assets[SKIN_MB].rgb!=NULL;
    if (p->art_mb) {
        p->art_size=SKIN_ART_HEIGHT(128)*scale<=p->screen->Height ? 128 : SKIN_ART_SIZE;
        width=SKIN_ART_FRAME_W*scale; height=SKIN_ART_HEIGHT(p->art_size)*scale;
    } else { p->art_size=SKIN_ART_SIZE; width=height=SKIN_ART_BOX*scale; }
    if (width>p->screen->Width || height>p->screen->Height) return 0;
    left=p->art_position ? p->art_left : p->win->LeftEdge+p->win->Width;
    top=p->art_position ? p->art_top : p->win->TopEdge;
    if (left+width>p->screen->Width) left=p->screen->Width-width;
    if (top+height>p->screen->Height) top=p->screen->Height-height;
    if (left<0) left=0;
    if (top<0) top=0;
    p->artwin=OpenWindowTags(NULL,WA_CustomScreen,(ULONG)p->screen,WA_Left,left,WA_Top,top,
        WA_Width,width,WA_Height,height,WA_Borderless,TRUE,WA_Activate,FALSE,WA_RMBTrap,FALSE,
        WA_DetailPen,p->win->DetailPen,WA_BlockPen,p->win->BlockPen,
        WA_NewLookMenus,(ULONG)(GfxBase->LibNode.lib_Version>=39),
        WA_ReportMouse,FALSE,WA_SmartRefresh,TRUE,
        WA_IDCMP,IDCMP_MOUSEBUTTONS|IDCMP_MOUSEMOVE|IDCMP_REFRESHWINDOW|IDCMP_MENUPICK|IDCMP_VANILLAKEY|
                   IDCMP_ACTIVEWINDOW|IDCMP_INACTIVEWINDOW,TAG_DONE);
    if (!p->artwin) return 0;
    strip_attach(p,STRIP_ART,p->artwin);
    p->art_have_drawn=0; p->art_cached=0; art_repaint(p,1); menu_sync(p); return 1;
}
static void art_toggle(SkinPlayer *p)
{
    if (p->artwin) art_close(p);
    else if (!art_open(p)) skin_player_notice(p,"ARTWORK WINDOW DOES NOT FIT THIS SCREEN");
}
void skin_player_artwork(SkinPlayer *p,const unsigned char *rgb,int valid,int loading,int radio)
{
    int kind; unsigned long hash=2166136261UL; size_t i;
    if (!p) return;
    kind=p->art_disabled ? SKIN_ART_OFF : valid && rgb ? SKIN_ART_IMAGE : loading ? SKIN_ART_LOADING :
        radio ? SKIN_ART_RADIO : SKIN_ART_NONE;
    /* Hash only while visible: a hidden artwork window costs nothing. */
    if (!p->artwin) { p->art_kind=-1; return; }
    if (kind==SKIN_ART_IMAGE) {
        /* Sampled: called on GUI updates, so keep it to ~1.8K bytes. */
        for (i=0;i<sizeof(p->art_rgb);i+=7) hash=((hash^rgb[i])*16777619UL)&0xffffffffUL;
        if (kind==p->art_kind && hash==p->art_hash) return;
        memcpy(p->art_rgb,rgb,sizeof(p->art_rgb)); p->art_hash=hash; p->art_cached=0;
    } else if (kind==p->art_kind) return;
    p->art_kind=kind; art_repaint(p,0);
}
int skin_player_artwork_visible(SkinPlayer *p) { return p && p->artwin; }
void skin_player_artwork_enabled(SkinPlayer *p,int enabled)
{
    if (!p || p->art_disabled==!enabled) return;
    p->art_disabled=!enabled; p->art_kind=-1; /* the next update redraws */
}
static int menu_action(SkinPlayer *p,UWORD code,SkinEvent *event);
static int key_action(SkinPlayer *p,UWORD code,SkinEvent *event);
static int art_poll(SkinPlayer *p,SkinEvent *event)
{
    struct IntuiMessage *m;
    while (p->artwin && (m=(struct IntuiMessage *)GetMsg(p->artwin->UserPort))!=NULL) {
        ULONG type=m->Class,seconds=m->Seconds,micros=m->Micros; UWORD code=m->Code,qualifier=m->Qualifier;
        int x=m->MouseX/p->resources->scale,y=m->MouseY/p->resources->scale;
        int sx=p->screen->MouseX,sy=p->screen->MouseY;
        int close=p->art_mb && skin_art_hit_test(p->art_size,x,y)==SKIN_PLAYLIST_CLOSE;
        ReplyMsg((struct Message *)m); memset(event,0,sizeof(*event)); track_mouse(p,p->artwin,type,code);
        if (type==IDCMP_REFRESHWINDOW) { BeginRefresh(p->artwin); art_repaint(p,1); EndRefresh(p->artwin,TRUE); }
        else if (type==IDCMP_ACTIVEWINDOW || type==IDCMP_INACTIVEWINDOW) {
            if (p->art_dragging) { p->art_dragging=0; drag_end(p,3); }
            p->art_pressed=0; art_repaint(p,0);
        }
        else if (type==IDCMP_MENUPICK) { if (menu_action(p,code,event)) return 1; }
        else if (type==IDCMP_MOUSEBUTTONS && code==SELECTDOWN) {
            if (close) { p->art_pressed=SKIN_PLAYLIST_CLOSE; art_repaint(p,0); continue; }
            /* Double-click the artwork for the file/stream information. */
            if (DoubleClick(p->art_seconds,p->art_micros,seconds,micros)) {
                p->art_seconds=p->art_micros=0; event->action=SKIN_INFO; event->released=1; return 1;
            }
            p->art_seconds=seconds; p->art_micros=micros;
            p->art_dragging=1; p->art_drag_x=sx; p->art_drag_y=sy; drag_begin(p,3,qualifier);
        } else if (type==IDCMP_MOUSEMOVE && p->art_dragging) {
            drag_move(p,sx-p->art_drag_x,sy-p->art_drag_y); p->art_drag_x=sx; p->art_drag_y=sy;
        } else if (type==IDCMP_MOUSEBUTTONS && code==SELECTUP) {
            if (p->art_pressed==SKIN_PLAYLIST_CLOSE) {
                p->art_pressed=0;
                if (close) { art_close(p); save_state(p); }
                else art_repaint(p,0);
            } else if (p->art_dragging) { p->art_dragging=0; drag_end(p,3); save_state(p); }
        } else if (type==IDCMP_VANILLAKEY) {
            if (code==27 || code=='a' || code=='A') { art_close(p); save_state(p); }
            else if (key_action(p,code,event)) return 1;
        }
    }
    return 0;
}
/* ---- Settings window ----------------------------------------------------
 * A skinned page for the player's settings only: the classic MintAMP
 * window is no longer shown just to change them. The frame reuses the
 * PLEDIT title, side and scrollbar sprites; rows use the PLEDIT.TXT
 * colours and ROM Topaz, like the playlist. Rows come from the frontend,
 * followed by the player's own Interface rows. */
enum { SET_ROW_SKIN=-1, SET_ROW_SIZE=-2, SET_ROW_PREFS=-3, SET_ROW_CLASSIC=-4 };
static int settings_rows_height(SkinPlayer *p) { return p->set_visible*10; }
static void settings_clamp(SkinPlayer *p)
{
    int maximum=p->set_count-p->set_visible;
    if (maximum<0) maximum=0;
    if (p->set_focus>=p->set_count) p->set_focus=p->set_count-1;
    if (p->set_focus<0) p->set_focus=0;
    if (p->set_focus<p->set_top) p->set_top=p->set_focus;
    if (p->set_focus>=p->set_top+p->set_visible) p->set_top=p->set_focus-p->set_visible+1;
    if (p->set_top>maximum) p->set_top=maximum;
    if (p->set_top<0) p->set_top=0;
}
static void settings_sprite_column(SkinPlayer *p,int sx,int w,int x,int bottom)
{
    int y;
    for (y=20;y<bottom;y+=29) blit(p,SKIN_PLEDIT,sx,42,w,bottom-y<29 ? bottom-y : 29,x,y);
}
static void settings_repaint(SkinPlayer *p,int all)
{
    const WinampSkin *k; int i,sy,active,bottom,maximum;
    if (!p->setwin || !p->resources || !text_ready(p)) return;
    k=&p->resources->skin; p->target=p->setwin;
    active=(p->setwin->Flags&WFLG_WINDOWACTIVE)!=0; sy=active ? 0 : 21;
    bottom=22+settings_rows_height(p);
    if (!p->set_have_drawn) all=1;
    if (all || active!=p->set_drawn_active || p->set_pressed!=p->set_drawn_pressed) {
        for (i=25;i<250;i+=25) blit(p,SKIN_PLEDIT,127,sy,25,20,i,0);
        blit(p,SKIN_PLEDIT,0,sy,25,20,0,0);
        blit(p,SKIN_PLEDIT,153,sy,25,20,250,0);
        if (p->set_pressed==SET_PRESS_CLOSE) blit(p,SKIN_PLEDIT,52,42,9,9,264,3);
        playlist_label(p,"MINTAMP 2.0 SETTINGS",k->playlist_normal,k->playlist_background,58,6,160);
    }
    if (all) {
        settings_sprite_column(p,0,12,0,bottom);
        settings_sprite_column(p,31,20,255,bottom);
        for (i=0;i<275;i+=25) blit(p,SKIN_PLEDIT,127,21,i+25>275 ? 275-i : 25,20,i,bottom);
        fill(p,k->playlist_background,12,20,243,bottom-20);
        playlist_label(p,"SHIFT-CLICK: PREVIOUS",k->playlist_normal,k->playlist_background,54,bottom+6,168);
    }
    for (i=0;i<p->set_visible;++i) {
        int index=p->set_top+i, old=p->set_drawn_top+i;
        const SkinSettingsRow *r=index<p->set_count ? &p->set_rows[index] : NULL;
        const SkinSettingsRow *o=old<p->set_drawn_count ? &p->set_drawn[old] : NULL;
        int focus=r && index==p->set_focus && r->kind!=SKIN_ROW_HEADER;
        int was=o && old==p->set_drawn_focus && o->kind!=SKIN_ROW_HEADER;
        unsigned bg=focus ? k->playlist_selected : k->playlist_background;
        if (!all && focus==was && ((!r && !o) || (r && o && r->kind==o->kind &&
            !strcmp(r->label,o->label) && !strcmp(r->value,o->value)))) continue;
        fill(p,bg,12,22+i*10,243,10);
        if (!r) continue;
        if (r->kind==SKIN_ROW_HEADER) {
            char line[40]; int n;
            n=snprintf(line,sizeof(line),"- %s -",r->label); if (n>29) n=29;
            playlist_label(p,line,k->playlist_current,bg,14+(232-n*8)/2,23+i*10,n*8);
        } else {
            playlist_label(p,r->label,k->playlist_normal,bg,14,23+i*10,96);
            playlist_label(p,r->value,k->playlist_current,bg,118,23+i*10,128);
        }
    }
    maximum=p->set_count-p->set_visible;
    if (all || p->set_top!=p->set_drawn_top || p->set_count!=p->set_drawn_count || p->set_pressed!=p->set_drawn_pressed) {
        settings_sprite_column(p,34,8,258,bottom);
        if (maximum>0)
            blit(p,SKIN_PLEDIT,p->set_pressed==SET_PRESS_SCROLL ? 61 : 52,53,8,18,258,
                 20+p->set_top*(bottom-38)/maximum);
    }
    memcpy(p->set_drawn,p->set_rows,sizeof(p->set_drawn));
    p->set_drawn_count=p->set_count; p->set_drawn_focus=p->set_focus; p->set_drawn_top=p->set_top;
    p->set_drawn_active=active; p->set_drawn_pressed=p->set_pressed; p->set_have_drawn=1;
}
void skin_player_settings_update(SkinPlayer *p,const SkinSettingsRow *rows,int count)
{
    SkinSettingsRow *r; const char *name;
    if (!p) return;
    if (count<0) count=0;
    if (count>SKIN_SETTINGS_MAX-5) count=SKIN_SETTINGS_MAX-5;
    memset(p->set_rows,0,sizeof(p->set_rows));
    if (count) memcpy(p->set_rows,rows,(size_t)count*sizeof(*rows));
    r=p->set_rows+count;
    r->kind=SKIN_ROW_HEADER; strcpy(r->label,"INTERFACE"); ++r;
    name=(const char *)FilePart((STRPTR)p->path);
    r->kind=SKIN_ROW_ACTION; r->id=SET_ROW_SKIN; r->action=SKIN_SELECT; strcpy(r->label,"Skin");
    strncpy(r->value,name && name[0] ? name : "Choose...",sizeof(r->value)-1); ++r;
    r->kind=SKIN_ROW_ACTION; r->id=SET_ROW_SIZE; r->action=SKIN_SIZE; strcpy(r->label,"Double size");
    strcpy(r->value,p->resources && p->resources->scale==2 ? "On" : "Off"); ++r;
    r->kind=SKIN_ROW_ACTION; r->id=SET_ROW_PREFS; r->action=SKIN_INTERFACE; strcpy(r->label,"Next launch");
    strcpy(r->value,"Preferences..."); ++r;
    r->kind=SKIN_ROW_ACTION; r->id=SET_ROW_CLASSIC; r->action=SKIN_CLASSIC; strcpy(r->label,"Classic mode");
    strcpy(r->value,"Switch now"); ++r;
    p->set_count=(int)(r-p->set_rows);
    if (p->set_focus>=0 && p->set_focus<p->set_count && p->set_rows[p->set_focus].kind==SKIN_ROW_HEADER) ++p->set_focus;
    settings_clamp(p);
    settings_repaint(p,0);
}
static void settings_close(SkinPlayer *p)
{
    struct Message *m;
    if (!p->setwin) return;
    ClearMenuStrip(p->setwin);
    while ((m=GetMsg(p->setwin->UserPort))!=NULL) ReplyMsg(m);
    p->set_left=p->setwin->LeftEdge; p->set_top_edge=p->setwin->TopEdge; p->set_position=1;
    CloseWindow(p->setwin); p->setwin=NULL; p->set_have_drawn=0; p->set_pressed=SET_PRESS_NONE;
    menu_sync(p);
}
static int settings_open(SkinPlayer *p)
{
    int scale,left,top,width,height,fit;
    if (!p || !p->win || !p->resources) return 0;
    if (p->setwin) { WindowToFront(p->setwin); ActivateWindow(p->setwin); return 1; }
    scale=p->resources->scale; width=275*scale;
    if (!p->resources->skin.assets[SKIN_PLEDIT].rgb || width>p->screen->Width || !text_ready(p)) return 0;
    fit=(p->screen->Height/scale-42)/10;
    p->set_visible=p->set_count<SKIN_SET_VISIBLE ? p->set_count : SKIN_SET_VISIBLE;
    if (p->set_visible>fit) p->set_visible=fit;
    if (p->set_visible<4) return 0;
    height=(42+p->set_visible*10)*scale;
    if (p->set_position) { left=p->set_left; top=p->set_top_edge; }
    else {
        /* Beside the player (or its artwork), else below it: the first
         * place on screen that covers no other skin window. */
        struct Window *w=p->win,*a=p->artwin;
        int cx[4],cy[4],i,j,n=0;
        cx[n]=w->LeftEdge+w->Width; cy[n++]=w->TopEdge;
        if (a) { cx[n]=a->LeftEdge+a->Width; cy[n++]=a->TopEdge; }
        cx[n]=w->LeftEdge-width; cy[n++]=w->TopEdge;
        cx[n]=w->LeftEdge; cy[n++]=w->TopEdge+w->Height;
        left=cx[0]; top=cy[0];
        for (i=0;i<n;++i) {
            SkinRect r[SKIN_WINDOWS]; struct Window *list[SKIN_WINDOWS]; int clear=1;
            if (cx[i]<0 || cy[i]<0 || cx[i]+width>p->screen->Width || cy[i]+height>p->screen->Height) continue;
            window_rects(p,r,list);
            for (j=0;j<SKIN_WINDOWS;++j) if (r[j].w && cx[i]<r[j].x+r[j].w && r[j].x<cx[i]+width &&
                cy[i]<r[j].y+r[j].h && r[j].y<cy[i]+height) clear=0;
            if (clear) { left=cx[i]; top=cy[i]; break; }
        }
    }
    if (left+width>p->screen->Width) left=p->screen->Width-width;
    if (top+height>p->screen->Height) top=p->screen->Height-height;
    if (left<0) left=0;
    if (top<0) top=0;
    settings_clamp(p);
    p->setwin=OpenWindowTags(NULL,WA_CustomScreen,(ULONG)p->screen,
        WA_Left,left,WA_Top,top,WA_Width,width,WA_Height,height,
        WA_Borderless,TRUE,WA_Activate,TRUE,WA_RMBTrap,FALSE,
        WA_DetailPen,p->win->DetailPen,WA_BlockPen,p->win->BlockPen,
        WA_NewLookMenus,(ULONG)(GfxBase->LibNode.lib_Version>=39),
        WA_ReportMouse,FALSE,WA_SmartRefresh,TRUE,
        WA_IDCMP,IDCMP_MOUSEBUTTONS|IDCMP_MOUSEMOVE|IDCMP_REFRESHWINDOW|IDCMP_MENUPICK|
                   IDCMP_VANILLAKEY|IDCMP_RAWKEY|IDCMP_INACTIVEWINDOW|IDCMP_ACTIVEWINDOW,TAG_DONE);
    if (!p->setwin) return 0;
    strip_attach(p,STRIP_SETTINGS,p->setwin);
    p->set_have_drawn=0; p->set_pressed=SET_PRESS_NONE; settings_repaint(p,1); menu_sync(p);
    return 1;
}
int skin_player_settings_open(SkinPlayer *p) { return settings_open(p); }
void skin_player_settings_close(SkinPlayer *p) { if (p) settings_close(p); }
int skin_player_settings_visible(SkinPlayer *p) { return p && p->setwin; }
static int run_action(SkinPlayer *p,int action,SkinEvent *event);
/* A click, Return or cursor key on the focused row. */
static int settings_activate(SkinPlayer *p,int direction,SkinEvent *event)
{
    const SkinSettingsRow *r;
    if (p->set_focus<0 || p->set_focus>=p->set_count) return 0;
    r=&p->set_rows[p->set_focus];
    if (r->kind==SKIN_ROW_HEADER) return 0;
    if (r->kind==SKIN_ROW_ACTION && r->action) return run_action(p,r->action,event);
    event->action=SKIN_SETTING_CHANGE; event->value=r->id; event->target=direction; event->released=1;
    return 1;
}
static int settings_hit(SkinPlayer *p,int x,int y)
{
    int bottom=22+settings_rows_height(p);
    if (y<20) return x>=264 && x<273 && y>=3 && y<12 ? SET_PRESS_CLOSE : SET_PRESS_DRAG;
    if (x>=258 && x<266 && y<bottom && p->set_count>p->set_visible) return SET_PRESS_SCROLL;
    if (x>=12 && x<255 && y>=22 && y<bottom) return SET_PRESS_ROW;
    return SET_PRESS_DRAG;
}
static void settings_scroll_to(SkinPlayer *p,int y)
{
    int bottom=22+settings_rows_height(p),maximum=p->set_count-p->set_visible;
    if (maximum<=0) return;
    p->set_top=(y-29)*maximum/(bottom-38>0 ? bottom-38 : 1);
    if (p->set_top<0) p->set_top=0;
    if (p->set_top>maximum) p->set_top=maximum;
    if (p->set_focus<p->set_top) p->set_focus=p->set_top;
    if (p->set_focus>=p->set_top+p->set_visible) p->set_focus=p->set_top+p->set_visible-1;
    settings_repaint(p,0);
}
static void settings_step(SkinPlayer *p,int direction)
{
    int i=p->set_focus;
    do { i+=direction; } while (i>=0 && i<p->set_count && p->set_rows[i].kind==SKIN_ROW_HEADER);
    if (i<0 || i>=p->set_count) return;
    p->set_focus=i; settings_clamp(p); settings_repaint(p,0);
}
static int settings_poll(SkinPlayer *p,SkinEvent *event)
{
    struct IntuiMessage *m;
    while (p->setwin && (m=(struct IntuiMessage *)GetMsg(p->setwin->UserPort))!=NULL) {
        ULONG type=m->Class; UWORD code=m->Code,qualifier=m->Qualifier;
        int x=m->MouseX/p->resources->scale,y=m->MouseY/p->resources->scale;
        int sx=p->screen->MouseX,sy=p->screen->MouseY;
        int back=(qualifier&(IEQUALIFIER_LSHIFT|IEQUALIFIER_RSHIFT))!=0;
        ReplyMsg((struct Message *)m); memset(event,0,sizeof(*event)); track_mouse(p,p->setwin,type,code);
        if (type==IDCMP_REFRESHWINDOW) { BeginRefresh(p->setwin); settings_repaint(p,1); EndRefresh(p->setwin,TRUE); }
        else if (type==IDCMP_INACTIVEWINDOW || type==IDCMP_ACTIVEWINDOW) {
            if (p->set_pressed==SET_PRESS_DRAG) drag_end(p,4);
            p->set_pressed=SET_PRESS_NONE; settings_repaint(p,0);
        }
        else if (type==IDCMP_MENUPICK) { if (menu_action(p,code,event)) return 1; }
        else if (type==IDCMP_VANILLAKEY) {
            if (code==27 || code=='t' || code=='T') settings_close(p);
            else if (code=='\r' || code==' ') { if (settings_activate(p,back ? -1 : 1,event)) return 1; }
        } else if (type==IDCMP_RAWKEY && code<0x80) {
            if (code==0x4c || code==0x4d) settings_step(p,code==0x4c ? -1 : 1);
            else if (code==0x4e || code==0x4f) {
                const SkinSettingsRow *r=p->set_focus>=0 && p->set_focus<p->set_count ? &p->set_rows[p->set_focus] : NULL;
                /* Cursor keys change values only; actions need Return/click. */
                if (r && r->kind==SKIN_ROW_VALUE && settings_activate(p,code==0x4e ? 1 : -1,event)) return 1;
            }
        } else if (type==IDCMP_MOUSEBUTTONS && code==SELECTDOWN) {
            p->set_pressed=settings_hit(p,x,y); p->set_drag_x=sx; p->set_drag_y=sy; p->set_row=-1;
            if (p->set_pressed==SET_PRESS_DRAG) drag_begin(p,4,qualifier);
            else if (p->set_pressed==SET_PRESS_SCROLL) settings_scroll_to(p,y);
            else if (p->set_pressed==SET_PRESS_ROW) {
                int index=p->set_top+(y-22)/10;
                if (index<p->set_count && p->set_rows[index].kind!=SKIN_ROW_HEADER) { p->set_focus=index; p->set_row=index; }
            }
            settings_repaint(p,0);
        } else if (type==IDCMP_MOUSEMOVE && p->set_pressed==SET_PRESS_DRAG) {
            drag_move(p,sx-p->set_drag_x,sy-p->set_drag_y); p->set_drag_x=sx; p->set_drag_y=sy;
        } else if (type==IDCMP_MOUSEMOVE && p->set_pressed==SET_PRESS_SCROLL) {
            settings_scroll_to(p,y);
        } else if (type==IDCMP_MOUSEBUTTONS && code==SELECTUP) {
            int pressed=p->set_pressed,row=p->set_row;
            p->set_pressed=SET_PRESS_NONE; p->set_row=-1;
            if (pressed==SET_PRESS_DRAG) drag_end(p,4);
            settings_repaint(p,0);
            if (pressed==SET_PRESS_CLOSE && settings_hit(p,x,y)==SET_PRESS_CLOSE) settings_close(p);
            else if (pressed==SET_PRESS_ROW && row>=0 && settings_hit(p,x,y)==SET_PRESS_ROW &&
                     p->set_top+(y-22)/10==row) {
                p->set_focus=row;
                if (settings_activate(p,back ? -1 : 1,event)) return 1;
            }
        }
    }
    return 0;
}
/* ---- Iconify through a Workbench AppIcon -------------------------------- */
static void app_icon_remove(SkinPlayer *p)
{
    struct Message *m;
    if (p->app_icon) { RemoveAppIcon(p->app_icon); p->app_icon=NULL; }
    if (p->app_port) {
        while ((m=GetMsg(p->app_port))!=NULL) ReplyMsg(m);
        DeleteMsgPort(p->app_port); p->app_port=NULL;
    }
    if (p->app_object) { FreeDiskObject(p->app_object); p->app_object=NULL; }
}
static int open_window(SkinPlayer *p,WORD left,WORD top);
static int internal_action(SkinPlayer *p,int action);
static void restore_windows(SkinPlayer *p)
{
    if (p->shaded) { p->shaded=0; internal_action(p,SKIN_SHADE); }
    if (p->restore_playlist && !playlist_open(p)) p->pending_options=1;
    if (p->restore_eq) eq_toggle(p);
    if (p->restore_art) art_open(p);
    if (p->restore_settings) settings_open(p);
    p->restore_playlist=p->restore_eq=p->restore_art=p->restore_settings=0;
}
static void iconify(SkinPlayer *p)
{
    char path[64]; struct DiskObject *icon;
    if (!IconBase || !gSkinWorkbenchBase) { skin_player_notice(p,"ICONIFY NEEDS WORKBENCH"); return; }
    snprintf(path,sizeof(path),"PROGDIR:%s",p->name);
    p->app_object=GetDiskObject((STRPTR)path);
    if (!p->app_object) p->app_object=GetDefDiskObject(WBTOOL);
    p->app_port=CreateMsgPort();
    if (!p->app_object || !p->app_port) { app_icon_remove(p); skin_player_notice(p,"COULD NOT CREATE APPICON"); return; }
    /* Borrow only the images; sanitise the copy to the AppIcon contract. */
    p->app_icon_copy=*p->app_object; icon=&p->app_icon_copy;
    icon->do_Magic=0; icon->do_Version=0; icon->do_Type=0; icon->do_DefaultTool=NULL;
    icon->do_ToolTypes=NULL; icon->do_CurrentX=icon->do_CurrentY=NO_ICON_POSITION;
    icon->do_DrawerData=NULL; icon->do_ToolWindow=NULL; icon->do_StackSize=0;
    icon->do_Gadget.NextGadget=NULL; icon->do_Gadget.LeftEdge=icon->do_Gadget.TopEdge=0;
    icon->do_Gadget.Flags&=GFLG_GADGHIMAGE; icon->do_Gadget.Activation=0; icon->do_Gadget.GadgetType=0;
    icon->do_Gadget.GadgetText=NULL; icon->do_Gadget.MutualExclude=0; icon->do_Gadget.SpecialInfo=NULL;
    icon->do_Gadget.GadgetID=0; icon->do_Gadget.UserData=NULL;
    p->app_icon=AddAppIconA(0,0,(STRPTR)"MintAMP",p->app_port,(BPTR)0,icon,NULL);
    if (!p->app_icon) { app_icon_remove(p); skin_player_notice(p,"COULD NOT CREATE APPICON"); return; }
    save_state(p);
    p->restore_playlist=p->plwin!=NULL; p->restore_eq=p->eqwin!=NULL; p->restore_art=p->artwin!=NULL;
    p->restore_settings=p->setwin!=NULL;
    p->saved_left=p->win->LeftEdge; p->saved_top=p->win->TopEdge;
    settings_close(p); eq_close(p); playlist_close(p); art_close(p); close_window(p);
    p->pressed=0; p->iconified=1;
}
static int uniconify(SkinPlayer *p)
{
    if (!open_window(p,p->saved_left,p->saved_top)) return 0;
    app_icon_remove(p); p->iconified=0;
    restore_windows(p); WindowToFront(p->win); ActivateWindow(p->win); visual_arm(p);
    return 1;
}
/* ---- Small modal text requester (no GadTools/ReAction dependency) -------- */
int skin_player_request_text(SkinPlayer *p,const char *title,char *buffer,int size)
{
    struct Window *w; struct Gadget g; struct StringInfo si; UBYTE undo[512];
    struct IntuiMessage *m; int done=0,ok=0,fh,width,height,top;
    if (!p || !buffer || size<2) return 0;
    if (size>(int)sizeof(undo)) size=sizeof(undo);
    fh=p->screen->Font->ta_YSize;
    width=p->screen->Width<480 ? p->screen->Width : 480;
    height=p->screen->WBorTop+fh+1+fh+14+p->screen->WBorBottom;
    top=p->win ? p->win->TopEdge : 20;
    if (top+height>p->screen->Height) top=p->screen->Height-height;
    if (top<0) top=0;
    w=OpenWindowTags(NULL,WA_CustomScreen,(ULONG)p->screen,WA_Title,(ULONG)title,
        WA_Left,(p->screen->Width-width)/2,WA_Top,top,
        WA_Width,width,WA_Height,height,WA_DragBar,TRUE,WA_DepthGadget,TRUE,WA_CloseGadget,TRUE,
        WA_Activate,TRUE,WA_RMBTrap,TRUE,WA_SmartRefresh,TRUE,
        WA_IDCMP,IDCMP_GADGETUP|IDCMP_CLOSEWINDOW|IDCMP_ACTIVEWINDOW|IDCMP_MOUSEBUTTONS,TAG_DONE);
    if (!w) return 0;
    memset(&g,0,sizeof(g)); memset(&si,0,sizeof(si)); memset(undo,0,sizeof(undo));
    si.Buffer=(UBYTE *)buffer; si.UndoBuffer=undo; si.MaxChars=(WORD)size;
    g.LeftEdge=w->BorderLeft+8; g.TopEdge=w->BorderTop+7;
    g.Width=w->Width-w->BorderLeft-w->BorderRight-16; g.Height=fh;
    g.Flags=GFLG_GADGHCOMP; g.Activation=GACT_RELVERIFY|GACT_STRINGLEFT;
    g.GadgetType=GTYP_STRGADGET; g.SpecialInfo=(APTR)&si;
    { struct RastPort *rp=w->RPort; int l=g.LeftEdge-3,t=g.TopEdge-3,r=g.LeftEdge+g.Width+2,b=g.TopEdge+g.Height+2;
      SetAPen(rp,1); Move(rp,l,t); Draw(rp,r,t); Draw(rp,r,b); Draw(rp,l,b); Draw(rp,l,t); }
    AddGadget(w,&g,0); RefreshGList(&g,w,NULL,1); ActivateGadget(&g,w,NULL);
    while (!done) {
        WaitPort(w->UserPort);
        while ((m=(struct IntuiMessage *)GetMsg(w->UserPort))!=NULL) {
            ULONG type=m->Class; ReplyMsg((struct Message *)m);
            if (type==IDCMP_CLOSEWINDOW) done=1;
            else if (type==IDCMP_GADGETUP) { ok=buffer[0]!=0; done=1; }
            else ActivateGadget(&g,w,NULL);
        }
    }
    RemoveGadget(w,&g);
    while ((m=(struct IntuiMessage *)GetMsg(w->UserPort))!=NULL) ReplyMsg((struct Message *)m);
    CloseWindow(w); buffer[size-1]=0; return ok;
}
int skin_player_choice(SkinPlayer *p,const char *text,const char *choices)
{
    struct EasyStruct es;
    memset(&es,0,sizeof(es)); es.es_StructSize=sizeof(es); es.es_Title=(STRPTR)p->name;
    es.es_TextFormat=(STRPTR)"%s"; es.es_GadgetFormat=(STRPTR)choices;
    return (int)EasyRequestArgs(p->win,&es,NULL,(APTR)&text);
}
static void eq_presets(SkinPlayer *p)
{
    int choice=skin_player_choice(p,"Equaliser presets (MP3 only)","Flat|Bass cut|Load|Save|Cancel"),i;
    struct FileRequester *fr; char path[512],text[256],*at,*end; BPTR fh; LONG len;
    long values[11]; int n;
    if (choice==1 || choice==2) { skin_eq_flat(); if (choice==2) { gSkinAudio.eq_bands[0]=-6; gSkinAudio.eq_bands[1]=-3; ++gSkinAudio.eq_sequence; } save_state(p); eq_repaint(p,0); return; }
    if (choice!=3 && choice!=4) return;
    fr=(struct FileRequester *)AllocAslRequestTags(ASL_FileRequest,ASLFR_TitleText,(ULONG)"MintAMP EQ preset",
        ASLFR_DoSaveMode,(ULONG)(choice==4),ASLFR_InitialFile,(ULONG)"mintamp.eq",TAG_DONE);
    if (!fr) return;
    path[0]=0;
    if (AslRequestTags(fr,ASLFR_Window,(ULONG)p->eqwin,ASLFR_SleepWindow,TRUE,TAG_DONE)) {
        strncpy(path,(const char *)fr->fr_Drawer,sizeof(path)-1); path[sizeof(path)-1]=0;
        if (!AddPart(path,fr->fr_File,sizeof(path))) path[0]=0;
    }
    FreeAslRequest(fr); if (!path[0]) return;
    if (choice==4) {
        n=snprintf(text,sizeof(text),"MintAMP-EQ-1 %d",gSkinAudio.eq_preamp);
        for (i=0;i<10;++i) n+=snprintf(text+n,sizeof(text)-n," %d",gSkinAudio.eq_bands[i]);
        text[n++]='\n'; fh=Open(path,MODE_NEWFILE);
        if (!fh) { error_request(p,"Could not create preset."); return; }
        len=Write(fh,text,n); Close(fh); if (len!=n) error_request(p,"Preset write failed.");
    } else {
        fh=Open(path,MODE_OLDFILE); if (!fh) { error_request(p,"Could not open preset."); return; }
        len=Read(fh,text,sizeof(text)-1); Close(fh);
        if (len<13 || len>=(LONG)sizeof(text)-1) { error_request(p,"Invalid MintAMP preset."); return; }
        text[len]=0; if (strncmp(text,"MintAMP-EQ-1 ",12)) { error_request(p,"Invalid MintAMP preset."); return; }
        at=text+12;
        for (i=0;i<11;++i) { values[i]=strtol(at,&end,10); if (at==end || values[i]<-12 || values[i]>12) break; at=end; }
        if (i!=11) { error_request(p,"Invalid MintAMP preset."); return; }
        gSkinAudio.eq_preamp=(int)values[0];
        for (i=0;i<10;++i) gSkinAudio.eq_bands[i]=(int)values[i+1];
        ++gSkinAudio.eq_sequence; eq_repaint(p,0); save_state(p);
    }
}
static void eq_drag_to(SkinPlayer *p,int y)
{
    int db=12-(y-43)*24/50;
    if (!p->eqwin || p->eq_pressed<=0) return;
    if (db<-12) db=-12;
    if (db>12) db=12;
    if (p->eq_pressed==1) gSkinAudio.eq_preamp=db; else gSkinAudio.eq_bands[p->eq_pressed-2]=db;
    ++gSkinAudio.eq_sequence; eq_repaint(p,0);
}
static int eq_poll(SkinPlayer *p)
{
    struct IntuiMessage *m; int pending=0,pending_y=0;
    while (p->eqwin && (m=(struct IntuiMessage *)GetMsg(p->eqwin->UserPort))!=NULL) {
        ULONG type=m->Class; UWORD code=m->Code;
        int x=m->MouseX/p->resources->scale,y=m->MouseY/p->resources->scale;
        int sx=p->screen->MouseX,sy=p->screen->MouseY,i=-1; UWORD qualifier=m->Qualifier;
        ReplyMsg((struct Message *)m); track_mouse(p,p->eqwin,type,code);
        /* A release (or anything else) lands the held-back slider first. */
        if (pending && type!=IDCMP_MOUSEMOVE) { eq_drag_to(p,pending_y); pending=0; }
        if (type==IDCMP_REFRESHWINDOW) { BeginRefresh(p->eqwin); eq_repaint(p,1); EndRefresh(p->eqwin,TRUE); }
        else if (type==IDCMP_INACTIVEWINDOW || type==IDCMP_ACTIVEWINDOW) { p->eq_pressed=0; eq_repaint(p,0); }
        else if (type==IDCMP_VANILLAKEY && (code==27 || code=='e' || code=='E')) { eq_close(p); save_state(p); }
        else if (type==IDCMP_MOUSEBUTTONS && code==SELECTDOWN) {
            if (x>=264 && x<273 && y>=3 && y<12) { p->eq_pressed=-2; eq_repaint(p,0); continue; }
            if (x>=254 && x<263 && y>=3 && y<12) { p->eq_pressed=-3; eq_repaint(p,0); continue; }
            if (y<14) { p->eq_pressed=-1; drag_begin(p,2,qualifier); p->eq_drag_x=sx; p->eq_drag_y=sy; continue; }
            if (x>=14 && x<40 && y>=18 && y<30) p->eq_pressed=-5;
            else if (x>=40 && x<72 && y>=18 && y<30) p->eq_pressed=-6;
            else if (x>=217 && x<261 && y>=18 && y<30) p->eq_pressed=-4;
            else if (y>=38 && y<101) {
                if (x>=20 && x<35) i=0;
                else if (x>=77 && x<253 && (x-77)%18<14) i=1+(x-77)/18;
                if (i>=0 && i<=10) {
                    static const int lower[10]={0,101,230,431,775,1732,4243,8485,12961,14967};
                    int rate=p->now.rate;
                    if (gSkinAudio.output_rate>0 && (!rate || gSkinAudio.output_rate<rate)) rate=gSkinAudio.output_rate;
                    if (i && rate>0 && lower[i-1]>=rate/2)
                        skin_player_notice(p,"EQ BAND OUTSIDE OUTPUT BANDWIDTH");
                    else p->eq_pressed=i+1;
                }
            }
        } else if (type==IDCMP_MOUSEMOVE && p->eq_pressed==-1) {
            drag_move(p,sx-p->eq_drag_x,sy-p->eq_drag_y); p->eq_drag_x=sx; p->eq_drag_y=sy;
        } else if (type==IDCMP_MOUSEBUTTONS && code==SELECTUP) {
            int pressed=p->eq_pressed; p->eq_pressed=0;
            if (pressed==-1) drag_end(p,2);
            else if (pressed==-2 && x>=264 && x<273 && y>=3 && y<12) eq_close(p);
            else if (pressed==-3 && x>=254 && x<263 && y>=3 && y<12) {
                int height=(p->eq_shaded ? 116 : 14)*p->resources->scale;
                if (height<=p->screen->Height) {
                    int top=p->eqwin->TopEdge; if (top+height>p->screen->Height) top=p->screen->Height-height;
                    p->eq_shaded=!p->eq_shaded; ChangeWindowBox(p->eqwin,p->eqwin->LeftEdge,top,p->eqwin->Width,height); p->eq_have_drawn=0;
                }
            } else if (pressed==-4 && x>=217 && x<261 && y>=18 && y<30) eq_presets(p);
            else if (pressed==-5 && x>=14 && x<40 && y>=18 && y<30) {
                if (gSkinAudio.eq_enabled) gSkinAudio.eq_enabled=0; /* bypass always accessible */
                else if (gSkinAudio.eq_supported) gSkinAudio.eq_enabled=1;
                else skin_player_notice(p,"EQ REQUIRES MP3 PLAYBACK - OTHER DECODERS BYPASS");
            } else if (pressed==-6 && x>=40 && x<72 && y>=18 && y<30)
                skin_player_notice(p,"AUTOMATIC PER-TRACK EQ PRESETS NOT SUPPORTED");
            eq_repaint(p,0); save_state(p);
        }
        if (type==IDCMP_MOUSEBUTTONS && code==SELECTDOWN) eq_repaint(p,0);
        /* Only the newest queued pointer position matters for a slider. */
        if ((type==IDCMP_MOUSEMOVE || (type==IDCMP_MOUSEBUTTONS && code==SELECTDOWN)) && p->eq_pressed>0) { pending=1; pending_y=y; }
    }
    if (pending) eq_drag_to(p,pending_y);
    return 0;
}
static void visual_arm(SkinPlayer *p)
{
    if (!p->visual_device || p->visual_pending || !p->win) return;
    if (p->shaded && !(p->move_active && p->move_edge)) return;
    if (!(p->move_active && p->move_edge) && !(strlen(p->now.title)>31 || p->notice_ticks || (p->visual_mode && p->now.playing && !p->now.paused))) return;
    p->visual_timer->tr_node.io_Command=TR_ADDREQUEST;
    p->visual_timer->tr_time.tv_secs=0;
    p->visual_timer->tr_time.tv_micro=250000;
    if (p->visual_timer->tr_time.tv_micro>=1000000) {
        p->visual_timer->tr_time.tv_secs=1; p->visual_timer->tr_time.tv_micro=0;
    }
    SendIO((struct IORequest *)p->visual_timer); p->visual_pending=1;
}
static void visual_tick(SkinPlayer *p)
{
    int n;
    if (p->plwin && p->move_active && p->move_edge) {
        playlist_scroll(p,p->playlist.top+p->move_edge);
        p->move_target=p->playlist.top+(p->move_edge<0 ? 0 : SKIN_PLAYLIST_ROWS-1);
        if (p->move_target>=p->playlist.count) p->move_target=p->playlist.count-1;
    }
    if (p->shaded) return;
    if (p->notice_ticks) --p->notice_ticks;
    else if (strlen(p->now.title)>31) p->now.scroll=(p->now.scroll+2)%((strlen(p->now.title)+5)*5);
    repaint(p,0);
    if (!p->visual_mode || ++p->visual_ticks<p->visual_interval) return;
    p->visual_ticks=0;
    if (!p->now.playing || p->now.paused) { memset(p->now.levels,0,16); memset(p->now.peaks,0,16); memset(p->peak_hold,0,16); memset(p->now.scope,0,64); repaint(p,0); return; }
    if (gSkinAudio.visual_request==gSkinAudio.visual_sequence) {
        for (n=0;n<64;++n) p->now.scope[n]=gSkinAudio.visual_pcm[n];
        if (p->visual_mode==1) skin_visual_analyse(p->now.scope,p->now.levels);
        if (p->now.playing && !p->now.paused) ++gSkinAudio.visual_request;
    }
    if (!p->now.playing || p->now.paused) { memset(p->now.levels,0,16); memset(p->now.peaks,0,16); memset(p->peak_hold,0,16); memset(p->now.scope,0,64); }
    skin_visual_peaks(p->now.levels,p->now.peaks,p->peak_hold);
    p->now.visual_mode=p->visual_mode; repaint(p,0);
}
static int internal_action(SkinPlayer *p,int action)
{
    if (action==SKIN_ICONIFY) { iconify(p); return 1; }
    if (action==SKIN_ART) { art_toggle(p); save_state(p); return 1; }
    if (action==SKIN_EQ) eq_toggle(p);
    else if (action==SKIN_TIMER) p->remaining=!p->remaining;
    else if (action==SKIN_SHUFFLE) { p->sequence.order.shuffle=!p->sequence.order.shuffle; skin_order_queue_reset(&p->sequence,p->playlist.count,p->playlist.current); skin_player_notice(p,p->sequence.order.shuffle ? "SHUFFLE ON" : "SHUFFLE OFF"); }
    else if (action==SKIN_REPEAT) { p->sequence.order.repeat=(p->sequence.order.repeat+1)%3; skin_player_notice(p,p->sequence.order.repeat==2 ? "REPEAT TRACK" : p->sequence.order.repeat==1 ? "REPEAT PLAYLIST" : "REPEAT OFF"); }
    else if (action==SKIN_VISUAL) {
        if (!p->visual_device) skin_player_notice(p,"VISUALISATION TIMER UNAVAILABLE");
        else {
            char notice[80];
            p->visual_mode=(p->visual_mode+1)%3; p->visual_ticks=0;
            if (p->visual_mode && p->now.playing && !p->now.paused &&
                gSkinAudio.visual_request==gSkinAudio.visual_sequence) ++gSkinAudio.visual_request;
            snprintf(notice,sizeof(notice),"VISUAL: %s - %d HZ",
                p->visual_mode==1 ? "SPECTRUM" : p->visual_mode==2 ? "OSCILLOSCOPE" : "OFF",
                4/p->visual_interval);
            skin_player_notice(p,notice);
        }
    }
    else if (action==SKIN_VIS_RATE) {
        int n=skin_player_choice(p,"Visualisation rate (GUI timer)","1 Hz|2 Hz|4 Hz|Cancel");
        if (n>=1 && n<=3) {
            char notice[80];
            p->visual_interval=n==1 ? 4 : n==2 ? 2 : 1; p->visual_ticks=0;
            snprintf(notice,sizeof(notice),"VISUAL RATE: %d HZ - AUDIO BUFFER LIMITED",4/p->visual_interval);
            skin_player_notice(p,notice);
        }
    }
    else if (action==SKIN_SHADE) {
        int height=(p->shaded ? 116 : 14)*p->resources->scale;
        int top=p->win->TopEdge;
        if (top+height>p->screen->Height) top=p->screen->Height-height;
        p->shaded=!p->shaded;
        ChangeWindowBox(p->win,p->win->LeftEdge,top,275*p->resources->scale,height);
        p->have_drawn=0;
    } else return 0;
    p->now.remaining=p->remaining; p->now.shaded=p->shaded;
    p->now.shuffle=p->sequence.order.shuffle; p->now.repeat=p->sequence.order.repeat;
    p->now.visual_mode=p->shaded ? 0 : p->visual_mode;
    visual_arm(p); repaint(p,0); save_state(p); return 1;
}
void skin_player_save(SkinPlayer *p) { if (p) save_state(p); }
void skin_player_wake(const char *name,int pause)
{
    struct Task *child;
    gSkinAudio.pause_requested=pause;
    if (pause) return;
    Forbid(); child=FindTask((STRPTR)name); if (child) Signal(child,SIGBREAKF_CTRL_D); Permit();
}
int skin_player_order(SkinPlayer *p,int count,int current,int direction,int natural)
{ return skin_order_queue_request(&p->sequence,count,current,direction,natural); }
int skin_player_paths(SkinPlayer *p,const char *paths,unsigned stride,int count,int current,int focus)
{
    unsigned long hash=2166136261UL; int i; unsigned n;
    if (!p) return 0;
    for (i=0;i<count;++i) {
        for (n=0;n<stride && paths[i*stride+n];++n)
            hash=((hash^(unsigned char)paths[i*stride+n])*16777619UL)&0xffffffffUL;
        hash=((hash^0xffUL)*16777619UL)&0xffffffffUL;
    }
    hash^=(unsigned long)count;
    if (hash==p->playlist_identity) { p->preserve_selection=0; return 0; }
    p->playlist_identity=hash; skin_order_queue_reset(&p->sequence,count,current);
    if (!p->preserve_selection) { memset(&p->selection,0,sizeof(p->selection)); p->selection.focus=-1; }
    p->preserve_selection=0;
    skin_selection_sync(&p->selection,count,focus); p->click_row=-1; p->pending_double=0;
    return 1;
}
void skin_player_order_cancel(SkinPlayer *p) { skin_order_queue_cancel(&p->sequence); }
void skin_player_order_reset(SkinPlayer *p,int count,int current)
{ skin_order_queue_reset(&p->sequence,count,current); p->preserve_selection=1; }
SkinSelection *skin_player_selection(SkinPlayer *p,int count,int focus)
{ skin_selection_sync(&p->selection,count,focus); return &p->selection; }
void skin_player_debug(SkinPlayer *p,long buffer_ms,long spare_ms,unsigned long underruns,unsigned long bytes,const char *station)
{
    char text[512];
    snprintf(text,sizeof(text),"Station: %.24s\nBuffer: %ld ms\nSpare: %ld ms\nUnderruns: %lu\nRadio buffered: %lu bytes\nSource: %d ch, %d kbit/s\nPaula output: %s\nPause: %s",station,buffer_ms,spare_ms,underruns,bytes,gSkinAudio.channels,gSkinAudio.bitrate,gSkinAudio.output_stereo ? "stereo" : "mono",gSkinAudio.paused ? "paused" : gSkinAudio.pause_requested ? "pending" : "running");
    skin_player_choice(p,text,"OK");
}
static void change_skin(SkinPlayer *p,int choose)
{
    char path[512], error[160]; SkinResources *next, *old; int scale;
    WORD left=p->win->LeftEdge,top=p->win->TopEdge;
    int had_playlist=p->plwin!=NULL,had_eq=p->eqwin!=NULL,had_art=p->artwin!=NULL,had_settings=p->setwin!=NULL;
    strcpy(path,p->path); scale=p->resources->scale;
    if (choose) { if (!request_path(p,path)) return; }
    else scale=scale==1 ? 2 : 1;
    if (275*scale>p->screen->Width || 116*scale>p->screen->Height) {
        error_request(p,"Double size does not fit this screen."); return;
    }
    next=load_resources(p,path,scale,error);
    if (!next) { error_request(p,error); return; }
    save_state(p); old=p->resources; settings_close(p); eq_close(p); playlist_close(p); art_close(p); close_window(p); p->resources=next;
    if (!open_window(p,left,top)) {
        p->resources=old; free_resources(p,next);
        if (!open_window(p,left,top)) p->win=NULL;
        if (had_playlist && p->win && !playlist_open(p)) p->pending_options=1;
        if (had_eq && p->win) eq_toggle(p);
        if (had_art && p->win) art_open(p);
        if (had_settings && p->win) settings_open(p);
        error_request(p,"Could not open the new skin window.");
        return;
    }
    free_resources(p,old); strcpy(p->path,path); save_choice(p);
    SetVar("MintAMP2/SKIN",p->path,-1,GVF_GLOBAL_ONLY);
    SetVar("MintAMP2/DOUBLESIZE",scale==2 ? "YES" : "NO",-1,GVF_GLOBAL_ONLY);
    p->choice_dirty=2;
    if (had_playlist && !playlist_open(p)) p->pending_options=1;
    if (had_eq) eq_toggle(p);
    if (had_art) art_open(p);
    if (had_settings && !settings_open(p)) skin_player_notice(p,"SETTINGS DO NOT FIT THIS SIZE - PRESS T");
    p->art_kind=-1; /* pens changed with the skin: redraw on next update */
}
SkinPlayer *skin_player_open(const char *name,int argc,char **argv)
{
    SkinPlayer *p=(SkinPlayer *)calloc(1,sizeof(*p));
    char key[64], value[512], error[160]; int i,scale=1;
    struct DiskObject *icon=NULL;
    if (!p) return NULL;
    /* CLI/icon skin options apply to the first skin opened in this run. A
     * later Classic -> skin switch reuses the saved per-edition choice, so a
     * skin loaded meanwhile is not replaced by the startup argument. */
    if (gSkinArgsUsed) { argc=1; argv=NULL; }
    gSkinArgsUsed=1;
    p->art_kind=-1;
    strncpy(p->name,name,sizeof(p->name)-1);
    { ULONG seconds,micros; CurrentTime(&seconds,&micros); skin_order_queue_init(&p->sequence,seconds^micros); }
    restore_state(p);
    p->selection.focus=p->selection.anchor=-1; p->move_row=-1;
    p->screen=LockPubScreen(NULL);
    if (!p->screen) { free(p); return NULL; }
    strcpy(p->path,"PROGDIR:Skins/MintAMP-mintleaf.wsz");
    env_name(p,key,"Skin");
    if (GetVar(key,value,sizeof(value),GVF_GLOBAL_ONLY)>0) {
        value[sizeof(value)-1]=0; strcpy(p->path,value);
    }
    env_name(p,key,"Scale");
    if (GetVar(key,value,sizeof(value),GVF_GLOBAL_ONLY)>0 && value[0]=='2') scale=2;
    if (argc==0 && argv) {
        struct WBStartup *wb=(struct WBStartup *)argv;
        BPTR previous=CurrentDir(wb->sm_ArgList[0].wa_Lock);
        icon=GetDiskObject(wb->sm_ArgList[0].wa_Name); CurrentDir(previous);
        if (icon) {
            STRPTR v=FindToolType(icon->do_ToolTypes,"SKIN");
            if (v) { strncpy(p->path,(const char *)v,sizeof(p->path)-1); p->path[sizeof(p->path)-1]=0; }
            v=FindToolType(icon->do_ToolTypes,"DOUBLESIZE");
            if (v) { MintLaunchOptions choice; mint_launch_defaults(&choice);
                if (mint_launch_option(&choice,"DOUBLESIZE",(const char *)v)) scale=choice.doublesize ? 2 : 1; }
            FreeDiskObject(icon);
        }
        if (wb->sm_NumArgs>1 && skin_player_is_skin((const char *)wb->sm_ArgList[1].wa_Name)) {
            if (NameFromLock(wb->sm_ArgList[1].wa_Lock,p->path,sizeof(p->path)))
                AddPart(p->path,wb->sm_ArgList[1].wa_Name,sizeof(p->path));
        }
    }
    for (i=1;i<argc;++i) {
        if (!strcmp(argv[i],"--double")) scale=2;
        else if (!strcmp(argv[i],"--normal")) scale=1;
        else if (!strcmp(argv[i],"--skin") && i+1<argc) {
            strncpy(p->path,argv[++i],sizeof(p->path)-1); p->path[sizeof(p->path)-1]=0;
        } else if (skin_player_is_skin(argv[i])) {
            strncpy(p->path,argv[i],sizeof(p->path)-1); p->path[sizeof(p->path)-1]=0;
        }
    }
    if (275*scale>p->screen->Width || 116*scale>p->screen->Height) scale=1;
    p->resources=load_resources(p,p->path,scale,error);
    if (!p->resources) {
        /* An unavailable saved/custom skin or incomplete installation can
         * still use the existing skin requester and native fallback. */
        if (!request_path(p,p->path)) { skin_player_suspend(p); return NULL; }
        p->resources=load_resources(p,p->path,scale,error);
        if (!p->resources) { error_request(p,error); skin_player_suspend(p); return NULL; }
    }
    strcpy(p->now.title,SKIN_IDLE_TITLE);
    if (!open_window(p,p->saved_left,p->saved_top)) { skin_player_suspend(p); return NULL; }
    p->visual_port=CreateMsgPort();
    if (p->visual_port) p->visual_timer=(struct timerequest *)CreateIORequest(p->visual_port,sizeof(struct timerequest));
    if (p->visual_timer && !OpenDevice((STRPTR)TIMERNAME,UNIT_VBLANK,(struct IORequest *)p->visual_timer,0)) p->visual_device=1;
    if (!p->visual_device) p->visual_mode=0;
    if (!gSkinWorkbenchBase) gSkinWorkbenchBase=OpenLibrary((STRPTR)"workbench.library",37);
    restore_windows(p);
    visual_arm(p); save_choice(p); return p;
}
/* ENVARC writes owed by suspended players, written at the next full close
 * or shutdown so a live Skin -> Classic switch does no disk IO mid-stream. */
static int gPendingPersist; static char gPendingName[32];
static void persist_choices(const char *name,int which)
{
    char key[64],value[512]; int i;
    static const char * const options[]={"State","Skin","Scale"};
    for (i=0;i<3;++i) if (which&(1<<i)) {
        snprintf(key,sizeof(key),"%s/%s",name,options[i]);
        if (GetVar(key,value,sizeof(value),GVF_GLOBAL_ONLY)>0) { value[sizeof(value)-1]=0; SetVar(key,value,-1,GVF_GLOBAL_ONLY|GVF_SAVE_VAR); }
    }
    if (which&8) mint_interface_flush_skin();
}
static void player_close(SkinPlayer *p,int defer)
{
    int which;
    if (!p) return;
    save_state(p);
    which=(p->state_dirty ? 1 : 0)|(p->choice_dirty ? 6 : 0)|(p->choice_dirty==2 ? 8 : 0);
    if (defer) { gPendingPersist|=which; strcpy(gPendingName,p->name); }
    else {
        /* Frontends call close only after stopping/retiring audio. Persist disk
         * choices here, not on every click while a radio decoder needs CPU/IO. */
        persist_choices(p->name,which|gPendingPersist); gPendingPersist=0;
    }
    if (p->visual_pending) { AbortIO((struct IORequest *)p->visual_timer); WaitIO((struct IORequest *)p->visual_timer); }
    if (p->visual_device) CloseDevice((struct IORequest *)p->visual_timer);
    if (p->visual_timer) DeleteIORequest((struct IORequest *)p->visual_timer);
    if (p->visual_port) DeleteMsgPort(p->visual_port);
    app_icon_remove(p);
    settings_close(p); eq_close(p); playlist_close(p); art_close(p); close_window(p); free_resources(p,p->resources);
    free_text_bitmap(p->text_source,243,8); free_text_bitmap(p->text_scaled,486,16);
    if (p->playlist_font) CloseFont(p->playlist_font);
    if (p->screen) UnlockPubScreen(NULL,p->screen);
    free(p);
}
void skin_player_close(SkinPlayer *p)
{
    if (p) player_close(p,0);
    else if (gPendingPersist) { persist_choices(gPendingName,gPendingPersist); gPendingPersist=0; }
}
void skin_player_suspend(SkinPlayer *p) { player_close(p,1); }
void skin_player_shutdown(void)
{
    if (gPendingPersist) { persist_choices(gPendingName,gPendingPersist); gPendingPersist=0; }
    if (gSkinWorkbenchBase) { CloseLibrary(gSkinWorkbenchBase); gSkinWorkbenchBase=NULL; }
}
void skin_player_restore_audio(const char *name)
{
    /* Classic start: apply the saved balance/EQ the skin would restore, so
     * audio does not depend on which interface opened first. */
    SkinPlayer *p=(SkinPlayer *)calloc(1,sizeof(*p));
    if (!p) return;
    strncpy(p->name,name,sizeof(p->name)-1); restore_state(p); free(p);
}
ULONG skin_player_signal(SkinPlayer *p)
{ return p ? (p->win ? 1UL<<p->win->UserPort->mp_SigBit : 0) |
            (p->plwin ? 1UL<<p->plwin->UserPort->mp_SigBit : 0) |
            (p->eqwin ? 1UL<<p->eqwin->UserPort->mp_SigBit : 0) |
            (p->artwin ? 1UL<<p->artwin->UserPort->mp_SigBit : 0) |
            (p->setwin ? 1UL<<p->setwin->UserPort->mp_SigBit : 0) |
            (p->app_port ? 1UL<<p->app_port->mp_SigBit : 0) |
            (p->visual_port ? 1UL<<p->visual_port->mp_SigBit : 0) : 0; }
struct Window *skin_player_window(SkinPlayer *p) { return p ? p->win : NULL; }
void skin_player_notice(SkinPlayer *p,const char *message)
{
    if (!p) return;
    strncpy(p->notice,message,sizeof(p->notice)-1); p->notice[sizeof(p->notice)-1]=0;
    p->notice_ticks=12; visual_arm(p);
}
void skin_player_update(SkinPlayer *p,const SkinState *s,int tick)
{
    int scroll;
    if (!p || !p->win) return;
    scroll=strcmp(p->now.title,s->title) ? 0 : p->now.scroll;
    if (tick && !p->visual_device && !p->notice_ticks) scroll=(scroll+2)%((strlen(s->title)+5)*5);
    { unsigned char levels[16],peaks[16]; signed char scope[64]; int filter=p->now.filter;
        memcpy(levels,p->now.levels,16); memcpy(peaks,p->now.peaks,16); memcpy(scope,p->now.scope,64);
        p->now=*s; memcpy(p->now.levels,levels,16); memcpy(p->now.peaks,peaks,16); memcpy(p->now.scope,scope,64);
        if (filter!=s->filter) menu_sync(p);
        p->now.remaining=p->remaining; p->now.shaded=p->shaded;
        p->now.shuffle=p->sequence.order.shuffle; p->now.repeat=p->sequence.order.repeat; p->now.balance=gSkinAudio.balance;
        p->now.visual_mode=p->shaded ? 0 : p->visual_mode;
        if (!s->playing || s->paused) { memset(p->now.levels,0,16); memset(p->now.peaks,0,16); memset(p->peak_hold,0,16); memset(p->now.scope,0,64); }
        eq_repaint(p,0); visual_arm(p);
    }
    p->now.scroll=scroll;
    if (strncmp(p->art_title,s->title,sizeof(p->art_title)-1)) {
        strncpy(p->art_title,s->title,sizeof(p->art_title)-1); p->art_title[sizeof(p->art_title)-1]=0;
        if (p->art_mb) art_repaint(p,0);
    }
    if (p->notice_ticks) {
        strcpy(p->now.title,p->notice); p->now.scroll=0;
        if (tick && !p->visual_device) --p->notice_ticks;
    }
    repaint(p,0);
}
static int menu_action(SkinPlayer *p,UWORD code,SkinEvent *event)
{
    int i,m,item,index=-1,action;
    if (code==MENUNULL) return 0;
    m=MENUNUM(code); item=ITEMNUM(code);
    for (i=0;i<SKIN_MENU_ITEMS;++i) if (kMenuDefs[i].menu==m && item--==0) { index=i; break; }
    if (index<0) return 0;
    action=kMenuDefs[index].action;
    /* Intuition toggled the tick itself; restore it from real state. */
    if (kMenuDefs[index].check) {
        unsigned long bits=menu_checks(p); int w;
        for (w=STRIP_MAIN;w<STRIP_COUNT;++w) p->strips[w].checks=~bits;
    }
    menu_sync(p);
    return run_action(p,action,event);
}
/* Shared by the menus and the settings window's Interface rows. */
static int run_action(SkinPlayer *p,int action,SkinEvent *event)
{
    if (action==SKIN_INTERFACE) { mint_interface_preferences(p->setwin ? p->setwin : p->win); return 0; }
    if (action==SKIN_SELECT || action==SKIN_SIZE) { change_skin(p,action==SKIN_SELECT); menu_sync(p); return 0; }
    if (internal_action(p,action)) { menu_sync(p); return 0; }
    event->action=action; event->released=1; return 1;
}
/* Keys shared by the main and artwork windows. Winamp-style transport keys,
 * plus I file info, U stream URL, A artwork, T settings, R radio. */
static int key_action(SkinPlayer *p,UWORD code,SkinEvent *event)
{
    int c=code>='A' && code<='Z' ? code-'A'+'a' : code;
    if (c=='s') { change_skin(p,1); menu_sync(p); return 0; }
    if (c=='d') { change_skin(p,0); menu_sync(p); return 0; }
    event->action=c==' ' || c=='c' ? SKIN_PAUSE : c=='x' ? SKIN_PLAY : c=='z' ? SKIN_PREVIOUS :
        c=='b' ? SKIN_NEXT : c=='e' ? SKIN_EQ : c=='v' ? SKIN_STOP : c=='j' ? SKIN_SHUFFLE :
        c=='l' ? SKIN_REPEAT : c=='w' ? SKIN_SHADE : c=='r' ? SKIN_RADIO : c=='o' ? SKIN_BROWSE :
        c=='p' ? SKIN_PLAYLIST : c=='t' ? SKIN_SETTINGS : c=='i' ? SKIN_INFO : c=='u' ? SKIN_STREAM :
        c=='a' ? SKIN_ART : SKIN_NONE;
    if (event->action && !internal_action(p,event->action)) { event->released=1; return 1; }
    event->action=SKIN_NONE; return 0;
}
static void playlist_scroll(SkinPlayer *p,int top)
{
    p->playlist.top=skin_playlist_top(top,p->playlist.count); p->click_row=-1;
    skin_player_playlist_update(p,p->playlist.count,p->playlist.selected,
        p->playlist.current,p->playlist_name,p->playlist_ctx);
}
static int playlist_poll(SkinPlayer *p,SkinEvent *event)
{
    struct IntuiMessage *msg; int scroll_pending=0,scroll_y=0;
    while (p->plwin && (msg=(struct IntuiMessage *)GetMsg(p->plwin->UserPort))!=NULL) {
        ULONG type=msg->Class,seconds=msg->Seconds,micros=msg->Micros;
        UWORD code=msg->Code;
        int x=msg->MouseX/p->resources->scale,y=msg->MouseY/p->resources->scale;
        int sx=p->screen->MouseX,sy=p->screen->MouseY;
        UWORD qualifier=msg->Qualifier;
        ReplyMsg((struct Message *)msg); memset(event,0,sizeof(*event)); track_mouse(p,p->plwin,type,code);
        if (scroll_pending && type!=IDCMP_MOUSEMOVE) {
            if (p->playlist_pressed==SKIN_PLAYLIST_SCROLL)
                playlist_scroll(p,(scroll_y-29)*skin_playlist_top(p->playlist.count,p->playlist.count)/156);
            scroll_pending=0;
        }
        if (type==IDCMP_REFRESHWINDOW) {
            BeginRefresh(p->plwin); playlist_repaint(p,1); EndRefresh(p->plwin,TRUE);
        } else if (type==IDCMP_INACTIVEWINDOW || type==IDCMP_ACTIVEWINDOW) {
            p->playlist_pressed=0; p->move_active=p->move_edge=p->pending_double=p->collapse_selection=0; p->move_row=-1;
            playlist_repaint(p,0);
        }
        else if (type==IDCMP_MENUPICK) {
            if (menu_action(p,code,event)) return 1;
        } else if (type==IDCMP_VANILLAKEY && (code=='\r' || code==' ')) {
            if (p->playlist.selected>=0 && p->playlist.selected<p->playlist.count) {
                event->action=SKIN_TRACK_PLAY; event->value=p->playlist.selected; return 1;
            }
        } else if (type==IDCMP_VANILLAKEY && (code=='p' || code=='P' || code==27)) {
            playlist_close(p);
        } else if (type==IDCMP_VANILLAKEY && (code=='o' || code=='O')) {
            event->action=SKIN_PLAYLIST_OPTIONS; return 1;
        } else if (type==IDCMP_RAWKEY && code==0x46) {
            event->action=SKIN_TRACK_REMOVE; return 1;
        } else if (type==IDCMP_VANILLAKEY && code==1) {
            skin_selection_all(&p->selection,1); playlist_scroll(p,p->playlist.top);
        } else if (type==IDCMP_RAWKEY && (code==0x4c || code==0x4d || code==0x3d || code==0x1d || code==0x3f || code==0x1f)) {
            int index=code==0x3d ? 0 : code==0x1d ? p->playlist.count-1 :
                p->playlist.selected+(code==0x4c ? -1 : code==0x4d ? 1 : code==0x3f ? -17 : 17);
            if ((qualifier&IEQUALIFIER_CONTROL) && (code==0x4c || code==0x4d)) {
                event->action=SKIN_TRACK_MOVE; event->value=p->playlist.selected; event->target=index; return 1;
            }
            if (index<0) index=0;
            if (index<p->playlist.count) {
                event->action=SKIN_TRACK_SELECT; event->value=index; p->click_row=-1;
                event->selection=(qualifier&(IEQUALIFIER_LSHIFT|IEQUALIFIER_RSHIFT)) ? SKIN_SELECT_RANGE : SKIN_SELECT_REPLACE; return 1;
            }
        } else if (type==IDCMP_MOUSEBUTTONS && code==SELECTDOWN) {
            p->playlist_pressed=p->pl_shaded && y>=14 ? SKIN_NONE : skin_playlist_hit_test(x,y);
            if (p->pl_shaded && p->playlist_pressed==SKIN_TRACK_SELECT) p->playlist_pressed=SKIN_NONE;
            playlist_repaint(p,0);
            if (p->playlist_pressed==SKIN_DRAG) drag_begin(p,1,qualifier);
            p->playlist_drag_x=sx; p->playlist_drag_y=sy;
            if (p->playlist_pressed==SKIN_TRACK_SELECT) {
                int index=p->playlist.top+(y-22)/10;
                if (index<p->playlist.count) {
                    int twice=index==p->click_row && DoubleClick(p->click_seconds,p->click_micros,seconds,micros);
                    /* Selection is mirrored immediately so the backend's next
                     * refresh does not invalidate the second click. */
                    p->playlist.selected=index; p->move_row=p->move_target=index; p->move_active=0;
                    p->click_row=twice ? -1 : index;
                    p->click_seconds=seconds; p->click_micros=micros;
                    p->pending_double=twice;
                    p->collapse_selection=!(qualifier&(IEQUALIFIER_CONTROL|IEQUALIFIER_LSHIFT|IEQUALIFIER_RSHIFT)) && p->selection.bits[index];
                    event->action=SKIN_TRACK_SELECT;
                    event->value=index; event->selection=(qualifier&IEQUALIFIER_CONTROL) ? SKIN_SELECT_TOGGLE : (qualifier&(IEQUALIFIER_LSHIFT|IEQUALIFIER_RSHIFT)) ? SKIN_SELECT_RANGE : p->collapse_selection ? SKIN_SELECT_FOCUS : SKIN_SELECT_REPLACE; return 1;
                }
            } else if (p->playlist_pressed==SKIN_PLAYLIST_SCROLL) {
                int maximum=skin_playlist_top(p->playlist.count,p->playlist.count);
                playlist_scroll(p,(y-29)*maximum/156);
            }
        } else if (type==IDCMP_MOUSEMOVE && p->playlist_pressed==SKIN_TRACK_SELECT && p->move_row>=0) {
            if (abs(sy-p->playlist_drag_y)>3*p->resources->scale) p->move_active=1;
            if (p->move_active) {
                p->move_edge=y<22 ? -1 : y>=192 ? 1 : 0;
                p->move_target=p->playlist.top+(y<22 ? 0 : y>=192 ? 16 : (y-22)/10);
                if (p->move_target>=p->playlist.count) p->move_target=p->playlist.count-1;
                p->click_row=-1; visual_arm(p);
            }
        } else if (type==IDCMP_MOUSEMOVE && p->playlist_pressed==SKIN_DRAG) {
            drag_move(p,sx-p->playlist_drag_x,sy-p->playlist_drag_y);
            p->playlist_drag_x=sx; p->playlist_drag_y=sy;
        } else if (type==IDCMP_MOUSEMOVE && p->playlist_pressed==SKIN_PLAYLIST_SCROLL) {
            scroll_pending=1; scroll_y=y; /* redraw rows once, at the newest position */
        } else if (type==IDCMP_MOUSEBUTTONS && code==SELECTUP) {
            int action=p->playlist_pressed; p->playlist_pressed=0;
            if (action==SKIN_DRAG) drag_end(p,1);
            save_state(p);
            if (action==SKIN_TRACK_SELECT && p->move_active) {
                event->action=SKIN_TRACK_MOVE; event->value=p->move_row; event->target=p->move_target; event->selection=1;
                p->move_row=-1; p->move_active=p->move_edge=p->collapse_selection=0; p->pending_double=0; return 1;
            }
            if (action==SKIN_TRACK_SELECT && p->pending_double && !p->move_active) {
                p->pending_double=0;
                if (p->move_row==p->playlist.top+(y-22)/10 && skin_playlist_hit_test(x,y)==SKIN_TRACK_SELECT) {
                    event->action=SKIN_TRACK_PLAY; event->value=p->move_row; return 1;
                }
            }
            if (action==SKIN_TRACK_SELECT && p->collapse_selection && skin_playlist_hit_test(x,y)==SKIN_TRACK_SELECT) {
                p->collapse_selection=0; event->action=SKIN_TRACK_SELECT; event->value=p->move_row;
                event->selection=SKIN_SELECT_REPLACE; return 1;
            }
            playlist_repaint(p,0);
            if (action==skin_playlist_hit_test(x,y)) {
                if (action==SKIN_PLAYLIST_CLOSE) { playlist_close(p); save_state(p); }
                else if (action==SKIN_PLAYLIST_SHADE) {
                    int height=(p->pl_shaded ? SKIN_PLAYLIST_HEIGHT : 14)*p->resources->scale;
                    if (height<=p->screen->Height) {
                        int top=p->plwin->TopEdge; if (top+height>p->screen->Height) top=p->screen->Height-height;
                        p->pl_shaded=!p->pl_shaded; ChangeWindowBox(p->plwin,p->plwin->LeftEdge,top,p->plwin->Width,height);
                        p->playlist_have_drawn=0; playlist_repaint(p,1); save_state(p);
                    } else skin_player_notice(p,"PLAYLIST DOES NOT FIT THIS SCREEN");
                }
                else if (action==SKIN_PLAYLIST_OPTIONS || (action>=SKIN_LIST_ADD && action<=SKIN_LIST_FILE)) { event->action=action; return 1; }
            }
        }
    }
    if (scroll_pending && p->plwin && p->playlist_pressed==SKIN_PLAYLIST_SCROLL) {
        int maximum=skin_playlist_top(p->playlist.count,p->playlist.count);
        playlist_scroll(p,(scroll_y-29)*maximum/156);
    }
    return 0;
}
int skin_player_poll(SkinPlayer *p,SkinEvent *event)
{
    struct IntuiMessage *msg;
    if (!p) return 0;
    if (p->iconified) {
        struct Message *m; int restore=0;
        while (p->app_port && (m=GetMsg(p->app_port))!=NULL) { ReplyMsg(m); restore=1; }
        if (restore && !uniconify(p)) skin_player_notice(p,"COULD NOT REOPEN THE SKIN");
        if (p->iconified) return 0;
    }
    if (!p->win) { event->action=SKIN_QUIT; return 1; }
    if (p->pending_options) {
        p->pending_options=0; memset(event,0,sizeof(*event));
        event->action=SKIN_PLAYLIST_OPTIONS; return 1;
    }
    if (p->visual_pending && CheckIO((struct IORequest *)p->visual_timer)) {
        WaitIO((struct IORequest *)p->visual_timer); p->visual_pending=0; visual_tick(p); visual_arm(p);
    }
    drag_apply(p,0);
    eq_poll(p);
    if (playlist_poll(p,event)) return 1;
    if (art_poll(p,event)) return 1;
    if (settings_poll(p,event)) return 1;
    if (!p->win) return 0; /* iconified from a utility window's menu */
    while (p->win && (msg=(struct IntuiMessage *)GetMsg(p->win->UserPort))!=NULL) {
        ULONG type=msg->Class; UWORD code=msg->Code;
        int x=msg->MouseX/p->resources->scale,y=msg->MouseY/p->resources->scale;
        int sx=p->screen->MouseX,sy=p->screen->MouseY;
        UWORD qualifier=msg->Qualifier;
        ReplyMsg((struct Message *)msg);
        memset(event,0,sizeof(*event)); track_mouse(p,p->win,type,code);
        if (type==IDCMP_REFRESHWINDOW) {
            BeginRefresh(p->win); repaint(p,1); EndRefresh(p->win,TRUE);
        } else if (type==IDCMP_INACTIVEWINDOW || type==IDCMP_ACTIVEWINDOW) { p->pressed=0; p->slider_pending=0; repaint(p,0); }
        else if (type==IDCMP_MENUPICK && code!=MENUNULL) {
            if (menu_action(p,code,event)) return 1;
        } else if (type==IDCMP_VANILLAKEY) {
            if (key_action(p,code,event)) return 1;
            if (!p->win) return 0;
        } else if (type==IDCMP_RAWKEY && code<0x80) {
            /* Cursor keys: Left/Right seek 5 s, Up/Down volume 5%. */
            if (code==0x4f || code==0x4e) { event->action=SKIN_SEEK_STEP; event->value=code==0x4e ? 5 : -5; event->released=1; return 1; }
            if (code==0x4c || code==0x4d) {
                int v=p->now.volume+(code==0x4c ? 5 : -5);
                event->action=SKIN_VOLUME_SET; event->value=v<0 ? 0 : v>100 ? 100 : v; event->released=1; return 1;
            }
        } else if (type==IDCMP_MOUSEBUTTONS && code==SELECTDOWN) {
            p->pressed=p->shaded && y>=14 ? SKIN_NONE : skin_hit_test(x,y); p->drag_x=sx; p->drag_y=sy;
            if (p->pressed==SKIN_DRAG) drag_begin(p,0,qualifier);
            if (p->pressed==SKIN_SEEK) p->seek_preview=skin_slider_value(SKIN_SEEK,x);
            repaint(p,0);
            if ((p->pressed==SKIN_VOLUME_SET || p->pressed==SKIN_BALANCE_SET)) {
                event->action=p->pressed; event->value=skin_slider_value(p->pressed,x); return 1;
            }
        } else if (type==IDCMP_MOUSEMOVE && p->pressed==SKIN_DRAG) {
            drag_move(p,sx-p->drag_x,sy-p->drag_y); p->drag_x=sx; p->drag_y=sy;
        } else if (type==IDCMP_MOUSEMOVE && (p->pressed==SKIN_SEEK || p->pressed==SKIN_VOLUME_SET || p->pressed==SKIN_BALANCE_SET)) {
            /* Coalesce: act once on the newest position after the queue drains. */
            p->slider_pending=1; p->slider_action=p->pressed; p->slider_value=skin_slider_value(p->pressed,x);
        } else if (type==IDCMP_MOUSEBUTTONS && code==SELECTUP) {
            int action=p->pressed; p->pressed=0; p->slider_pending=0;
            if (action==SKIN_DRAG) drag_end(p,0);
            repaint(p,0); save_state(p);
            if (action==SKIN_VOLUME_SET || action==SKIN_SEEK || action==SKIN_BALANCE_SET) {
                event->action=action; event->value=skin_slider_value(action,x); event->released=1; return 1;
            }
            if (action==skin_hit_test(x,y) && action!=SKIN_DRAG && action!=SKIN_NONE) {
                if (action==SKIN_SIZE) { change_skin(p,0); continue; }
                if (internal_action(p,action)) { if (!p->win) return 0; menu_sync(p); continue; }
                event->action=action; event->released=1; return 1;
            }
        }
    }
    if (p->win && p->slider_pending) {
        p->slider_pending=0;
        if (p->pressed==SKIN_SEEK) { p->seek_preview=p->slider_value; repaint(p,0); }
        else if (p->pressed==p->slider_action) {
            memset(event,0,sizeof(*event)); event->action=p->slider_action; event->value=p->slider_value; return 1;
        }
    }
    return 0;
}
