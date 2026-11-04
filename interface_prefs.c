#include "interface_prefs.h"
#include <string.h>
#include <stdio.h>
#ifdef MINTAMP_PREFS_TEST
#include "tests/amiga_prefs_stubs.h"
#else
#include <intuition/intuition.h>
#include <workbench/startup.h>
#include <workbench/workbench.h>
#include <proto/dos.h>
#include <proto/icon.h>
#include <proto/intuition.h>
#endif
static int preferences_dirty;
static const char * const keys[]={"GUI","MODE","SKIN","DOUBLESIZE"};
void mint_interface_load(MintLaunchOptions *o)
{
    int i; char key[64],v[512];
    for (i=0;i<4;++i) {
        LONG n; snprintf(key,sizeof(key),"MintAMP2/%s",keys[i]);
        n=GetVar(key,v,sizeof(v),GVF_GLOBAL_ONLY);
        if (n>0 && n<(LONG)sizeof(v)-1) { v[n]=0; mint_launch_option(o,keys[i],v); }
    }
}
void mint_interface_tooltypes(MintLaunchOptions *o,int argc,char **argv)
{
    struct WBStartup *wb; struct DiskObject *icon; BPTR old; int i;
    if (argc!=0 || !argv) return;
    wb=(struct WBStartup *)argv;
    if (wb->sm_NumArgs<1 || !wb->sm_ArgList) return;
    old=CurrentDir(wb->sm_ArgList[0].wa_Lock);
    icon=GetDiskObject(wb->sm_ArgList[0].wa_Name); CurrentDir(old);
    if (!icon) return;
    for (i=0;i<4;++i) {
        STRPTR v=FindToolType(icon->do_ToolTypes,(STRPTR)keys[i]);
        if (v) mint_launch_option(o,keys[i],(const char *)v);
    }
    FreeDiskObject(icon);
}
static int choice(struct Window *w,const char *text,const char *buttons)
{
    struct EasyStruct es; memset(&es,0,sizeof(es));
    es.es_StructSize=sizeof(es); es.es_Title=(STRPTR)"MintAMP 2.0 interface preferences";
    es.es_TextFormat=(STRPTR)"%s"; es.es_GadgetFormat=(STRPTR)buttons;
    return (int)EasyRequestArgs(w,&es,NULL,(APTR)&text);
}
void mint_interface_preferences(struct Window *w)
{
    MintLaunchOptions o; int n,i,ok=1; char text[192],key[64]; const char *values[4];
    mint_launch_defaults(&o); mint_interface_load(&o);
    snprintf(text,sizeof(text),"Preferred GUI: %s\nFor subsequent launches through MintAMP-Launch.",o.gadtools ? "GadTools" : "ReAction");
    n=choice(w,text,"ReAction|GadTools|Cancel"); if (!n) return; o.gadtools=n==2;
    n=choice(w,"Preferred appearance for subsequent launches.\nThe menu tick 'Classic interface' also switches live.","Skin|Classic MintAMP|Cancel"); if (!n) return; o.skinned=n==1;
    n=choice(w,"Preferred skin scale (when skinned)","Normal|Double|Cancel"); if (!n) return; o.doublesize=n==2;
    values[0]=o.gadtools ? "GADTOOLS" : "REACTION"; values[1]=o.skinned ? "SKINNED" : "CLASSIC";
    /* Preserve a custom saved path; Load skin in the skinned edition remains
     * the way to change it. No heavyweight skin loading in this requester. */
    values[2]=o.skin; values[3]=o.doublesize ? "YES" : "NO";
    for (i=0;i<4;++i) { snprintf(key,sizeof(key),"MintAMP2/%s",keys[i]); if (!SetVar(key,(STRPTR)values[i],-1,GVF_GLOBAL_ONLY)) ok=0; }
    preferences_dirty=1;
    choice(w,ok ? "Saved for next MintAMP-Launch.\nActive playback continues.\nExplicit ToolTypes/CLI arguments override saved preferences." : "Could not save all interface preferences.\nCheck ENV: and ENVARC: are writable.","OK");
}

int mint_interface_start_skinned(int argc,char **argv)
{
    MintLaunchOptions o; int mode;
    mint_launch_defaults(&o); mint_interface_load(&o); mint_interface_tooltypes(&o,argc,argv);
    mode=argc>0 && argv ? mint_launch_mode_arg(argc,argv) : -1;
    return mode>=0 ? mode : o.skinned;
}
void mint_interface_set_mode(int skinned)
{
    if (SetVar("MintAMP2/MODE",(STRPTR)(skinned ? "SKINNED" : "CLASSIC"),-1,GVF_GLOBAL_ONLY)) preferences_dirty=1;
}
static void persist_options(int first)
{
    int i; char key[64],value[512];
    for (i=first;i<4;++i) {
        LONG n; snprintf(key,sizeof(key),"MintAMP2/%s",keys[i]);
        n=GetVar(key,value,sizeof(value),GVF_GLOBAL_ONLY);
        if (n>0 && n<(LONG)sizeof(value)-1) {
            value[n]=0; SetVar(key,value,-1,GVF_GLOBAL_ONLY|GVF_SAVE_VAR);
        }
    }
}
void mint_interface_flush(void)
{
    if (preferences_dirty) persist_options(0);
    preferences_dirty=0;
}
void mint_interface_flush_skin(void)
{
    /* Read the current shared ENV values: a newer next-launch preference
     * must not be overwritten by the running player's older size/path. */
    persist_options(2);
}
