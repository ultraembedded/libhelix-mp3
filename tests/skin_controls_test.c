#include "skin_controls.h"
#include "winamp_skin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); exit(1); } } while (0)
static void order_tests(void)
{
    SkinOrder o; int i,current=0,visited[128]={0};
    skin_order_init(&o,42); skin_order_reset(&o,128,0); o.shuffle=1; visited[0]=1;
    for (i=1;i<128;++i) { current=skin_order_next(&o,128,current,1); CHECK(current>=0 && current<128 && !visited[current]); visited[current]=1; }
    CHECK(skin_order_next(&o,128,current,1)==-1);
    { int last=current,previous=skin_order_previous(&o,128,last);
      CHECK(previous>=0 && previous!=last); CHECK(skin_order_next(&o,128,previous,0)==last); }
    o.repeat=SKIN_REPEAT_LIST; CHECK(skin_order_next(&o,128,current,1)!=current);
    o.repeat=SKIN_REPEAT_TRACK; CHECK(skin_order_next(&o,128,23,1)==23);
    o.shuffle=0; o.repeat=SKIN_REPEAT_OFF; skin_order_reset(&o,3,0);
    CHECK(skin_order_next(&o,3,0,1)==1); CHECK(skin_order_next(&o,3,2,1)==-1);
    o.repeat=SKIN_REPEAT_LIST; CHECK(skin_order_next(&o,3,2,1)==0);
    o.repeat=SKIN_REPEAT_TRACK; CHECK(skin_order_next(&o,3,1,1)==1); CHECK(skin_order_next(&o,3,1,0)==2);
    CHECK(skin_order_previous(&o,3,0)==-1); CHECK(skin_order_next(&o,0,-1,1)==-1);
}
static void pending_order_tests(void)
{
    SkinOrderQueue q; int first,second;
    skin_order_queue_init(&q,42); skin_order_queue_reset(&q,5,0); q.order.shuffle=1;
    first=skin_order_queue_request(&q,5,0,1,0); CHECK(first>0 && q.order.last==0);
    skin_order_queue_observe(&q,5,0); /* old child still stopping */
    CHECK(q.pending==first && q.order.used==1 && !q.order.seen[first]);
    skin_order_queue_observe(&q,5,first); CHECK(q.pending==-1 && q.order.used==2);
    CHECK(skin_order_queue_request(&q,5,first,-1,0)==0);
    skin_order_queue_observe(&q,5,first); /* Previous pending */
    skin_order_queue_observe(&q,5,0); CHECK(q.order.cursor==0);
    CHECK(skin_order_queue_request(&q,5,0,1,0)==first);
    skin_order_queue_cancel(&q); CHECK(q.pending==-1 && q.order.cursor==0);
    CHECK(skin_order_queue_request(&q,5,0,1,0)==first);
    second=skin_order_queue_request(&q,5,0,1,0); CHECK(second>0 && second!=first);
    skin_order_queue_observe(&q,5,second); CHECK(q.order.last==second);
}
static void list_tests(void)
{
    char paths[128][512]={{0}},titles[128][80]={{0}},names[128][80]={{0}};
    int duration[128]={10,20,30,40},count=4,current=1,selected=2;
    SkinSelection selection; SkinList l;
    memset(&selection,0,sizeof(selection)); selection.focus=-1;
    skin_selection_sync(&selection,count,selected); skin_selection_set(&selection,0,SKIN_SELECT_TOGGLE);
    CHECK(selection.bits[0] && selection.bits[2]);
    skin_selection_set(&selection,3,SKIN_SELECT_RANGE); CHECK(selection.bits[0] && selection.bits[1] && selection.bits[2] && selection.bits[3]);
    skin_selection_all(&selection,2); CHECK(!selection.bits[0] && !selection.bits[3]);
    skin_selection_all(&selection,1); selected=selection.focus;
    strcpy(paths[0],"Work:z.mp3"); strcpy(paths[1],"Work:a.mp3"); strcpy(paths[2],"Work:b.mp3"); strcpy(paths[3],"Work:c.mp3");
    strcpy(titles[1],"Alpha"); strcpy(names[1],"Alpha");
    memset(&l,0,sizeof(l)); l.paths=&paths[0][0]; l.path_size=512; l.titles=&titles[0][0]; l.title_size=80;
    l.names=&names[0][0]; l.name_size=80; l.count=&count; l.current=&current; l.selected=&selected; l.selection=&selection; l.durations=duration;
    { SkinListSnapshot *backup=skin_list_snapshot(&l);
      CHECK(backup!=NULL); skin_list_remove(&l,1); skin_list_sort(&l,0);
      skin_list_restore(&l,backup); skin_list_snapshot_free(backup);
      CHECK(count==4 && current==1 && selected==3 && !strcmp(paths[1],"Work:a.mp3") && duration[1]==20); }
    CHECK(skin_list_move(&l,1,3)); CHECK(current==3 && selected==2 && !strcmp(paths[3],"Work:a.mp3") && duration[3]==20 && !strcmp(names[3],"Alpha"));
    skin_list_sort(&l,0); CHECK(!strcmp(titles[0],"Alpha") && current==0);
    CHECK(skin_list_remove(&l,0)); CHECK(count==3 && current==-1 && !strcmp(paths[0],"Work:b.mp3"));
    CHECK(!skin_list_move(&l,-1,0) && !skin_list_move(&l,0,128));
    while (count) CHECK(skin_list_remove(&l,0));
    CHECK(selected==-1 && current==-1);
    skin_selection_sync(&selection,128,127); CHECK(selection.bits[127]); skin_selection_sync(&selection,0,-1); CHECK(!selection.bits[127]);
}
static void block_move_tests(void)
{
    int mask,from,to;
    for (mask=1;mask<256;++mask) for (from=0;from<8;++from) for (to=0;to<8;++to) {
        char paths[8][16],titles[8][16],names[8][16];
        int durations[8],count=8,current=3,selected=from,expected[8],i,n=0,insert=0,boundary;
        SkinList l; SkinSelection s;
        if (!(mask&(1<<from))) continue;
        memset(&s,0,sizeof(s)); memset(&l,0,sizeof(l)); s.count=8; s.focus=s.anchor=from;
        for (i=0;i<8;++i) {
            sprintf(paths[i],"%d",i); sprintf(titles[i],"title%d",i); sprintf(names[i],"name%d",i);
            durations[i]=i*10; s.bits[i]=(mask>>i)&1;
        }
        boundary=to>from ? to+1 : to;
        for (i=0;i<boundary;++i) if (!s.bits[i]) ++insert;
        for (i=0;i<8;++i) if (!s.bits[i]) expected[n++]=i;
        memmove(expected+insert+8-n,expected+insert,(n-insert)*sizeof(int));
        n=insert; for (i=0;i<8;++i) if (s.bits[i]) expected[n++]=i;
        if (from==to) for (i=0;i<8;++i) expected[i]=i;
        l.paths=&paths[0][0]; l.path_size=16; l.titles=&titles[0][0]; l.title_size=16;
        l.names=&names[0][0]; l.name_size=16; l.count=&count; l.current=&current; l.selected=&selected; l.durations=durations; l.selection=&s;
        skin_list_move_selected(&l,from,to);
        for (i=0;i<8;++i) {
            CHECK(atoi(paths[i])==expected[i] && durations[i]==expected[i]*10);
            CHECK(s.bits[i]==((mask>>expected[i])&1));
            CHECK(atoi(titles[i]+5)==expected[i] && atoi(names[i]+4)==expected[i]);
            if (expected[i]==3) CHECK(current==i);
            if (expected[i]==from) CHECK(selected==i && s.focus==i);
        }
    }
    { unsigned char levels[16]={12},peaks[16]={0},hold[16]={0};
      skin_visual_peaks(levels,peaks,hold); CHECK(peaks[0]==12);
      levels[0]=0; skin_visual_peaks(levels,peaks,hold); CHECK(peaks[0]==12);
      skin_visual_peaks(levels,peaks,hold); skin_visual_peaks(levels,peaks,hold); CHECK(peaks[0]==11);
      for (mask=0;mask<30;++mask) skin_visual_peaks(levels,peaks,hold);
      CHECK(!peaks[0]); }
}
static void audio_tests(void)
{
    int spectrum[576],copy[576],i; signed char pcm[64]; unsigned char levels[16];
    CHECK(skin_balance_volume(100,100,0)==0 && skin_balance_volume(100,100,1)==100);
    CHECK(skin_balance_volume(100,-100,0)==100 && skin_balance_volume(100,-100,1)==0);
    CHECK(skin_balance_volume(50,50,0)==25 && skin_balance_volume(50,50,1)==50);
    CHECK(skin_eq_gain(INT_MAX,16384)==INT_MAX && skin_eq_gain(INT_MIN,16384)==INT_MIN);
    for (i=0;i<576;++i) spectrum[i]=copy[i]=(i-288)*32768;
    gSkinAudio.eq_enabled=0; CHECK(!skin_eq_apply(spectrum,576,44100) && !memcmp(spectrum,copy,sizeof(copy)));
    gSkinAudio.eq_enabled=1; skin_eq_flat(); CHECK(!skin_eq_apply(spectrum,576,44100) && !memcmp(spectrum,copy,sizeof(copy)));
    gSkinAudio.eq_bands[3]=-12; ++gSkinAudio.eq_sequence; CHECK(skin_eq_apply(spectrum,576,44100));
    CHECK(abs(spectrum[12])<abs(copy[12])/2 && spectrum[200]==copy[200]);
    CHECK(!skin_eq_apply(spectrum,577,44100));
    memset(pcm,0,64); skin_visual_analyse(pcm,levels); for (i=0;i<16;++i) CHECK(levels[i]==0);
    for (i=0;i<64;++i) pcm[i]=(signed char)((i/4)%2 ? -127 : 127);
    skin_visual_analyse(pcm,levels); CHECK(levels[7]>levels[0] && levels[7]>0);
    for (i=0;i<16;++i) CHECK(levels[i]<=15);
    CHECK(skin_hit_test(65,90)==SKIN_PAUSE && skin_hit_test(190,60)==SKIN_BALANCE_SET);
    CHECK(skin_hit_test(170,95)==SKIN_SHUFFLE && skin_hit_test(215,95)==SKIN_REPEAT);
    CHECK(skin_hit_test(55,30)==SKIN_TIMER && skin_hit_test(50,50)==SKIN_VISUAL);
    CHECK(skin_slider_value(SKIN_BALANCE_SET,184)==-100 && skin_slider_value(SKIN_BALANCE_SET,196)==0 && skin_slider_value(SKIN_BALANCE_SET,208)==100);
}
static void clock_tests(void)
{
    SkinClock c={0,0,0}; int i;
    for (i=0;i<100;++i) skin_clock_advance(&c,576,11025);
    CHECK(c.milliseconds==5224); /* MPEG-2.5, not frames*1152 */
    memset(&c,0,sizeof(c)); for (i=0;i<100;++i) skin_clock_advance(&c,1024,44100);
    CHECK(c.milliseconds==2321); /* AAC-sized blocks */
    memset(&c,0,sizeof(c)); skin_clock_advance(&c,11025,11025); CHECK(c.milliseconds==1000);
    skin_clock_advance(&c,22050,22050); CHECK(c.milliseconds==2000);
}
static void window_tests(void)
{
    SkinRect r[3]={{20,20,275,116},{20,136,275,232},{295,20,275,116}};
    int dx,dy;
    CHECK(skin_window_group(r,3,0,2)==7);
    r[1].y=142; r[2].x=400;
    CHECK(skin_window_group(r,3,0,2)==1);
    skin_window_snap(r,3,0,1,8,&dx,&dy); CHECK(dx==0 && dy==6);
    r[1].w=0; skin_window_snap(r,3,0,1,8,&dx,&dy); CHECK(dx==0 && dy==0);
    { /* Artwork docks beside the main window as the fourth group member. */
      SkinRect a[4]={{20,20,275,116},{20,136,275,232},{0,0,0,0},{295,20,74,74}};
      CHECK(skin_window_group(a,4,0,2)==11); CHECK(skin_window_group(a,4,3,2)==11);
      a[3].x=301; CHECK(skin_window_group(a,4,0,2)==3);
      skin_window_snap(a,4,3,8,8,&dx,&dy); CHECK(dx==-6 && dy==0);
      CHECK(skin_window_group(a,SKIN_WINDOW_MAX+1,0,2)==0); }
}
static void settings_tests(void)
{
    SkinSettingsRow rows[SKIN_SETTINGS_MAX]; int n=0,i;
    /* Choices wrap both ways; any non-negative step moves forward. */
    CHECK(skin_settings_cycle(0,1,0,4)==1); CHECK(skin_settings_cycle(4,1,0,4)==0);
    CHECK(skin_settings_cycle(0,-1,0,4)==4); CHECK(skin_settings_cycle(10,1,1,10)==1);
    CHECK(skin_settings_cycle(1,-1,1,10)==10); CHECK(skin_settings_cycle(2,0,0,3)==3);
    skin_settings_row(rows,&n,SKIN_ROW_HEADER,0,"PLAYBACK",NULL);
    skin_settings_row(rows,&n,SKIN_ROW_VALUE,7,"A label longer than sixteen","A value much longer than the row can hold");
    CHECK(n==2 && rows[0].kind==SKIN_ROW_HEADER && !rows[0].value[0]);
    CHECK(rows[1].id==7 && strlen(rows[1].label)==sizeof(rows[1].label)-1 && strlen(rows[1].value)==sizeof(rows[1].value)-1);
    for (i=n;i<SKIN_SETTINGS_MAX+4;++i) skin_settings_row(rows,&n,SKIN_ROW_VALUE,i,"x","y");
    CHECK(n==SKIN_SETTINGS_MAX);
    skin_settings_row(NULL,&n,SKIN_ROW_VALUE,0,"x","y"); CHECK(n==SKIN_SETTINGS_MAX);
}
int main(void) { order_tests(); block_move_tests(); pending_order_tests(); list_tests(); audio_tests(); window_tests(); clock_tests(); settings_tests(); puts("Skin controls: order/history, shared list/selection, balance, EQ bypass/headroom, spectrum, mapping and settings rows passed"); return 0; }
