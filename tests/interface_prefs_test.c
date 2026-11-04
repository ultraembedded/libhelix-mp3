#include "interface_prefs.h"
#include "amiga_prefs_stubs.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static const char *keys[]={"MintAMP2/GUI","MintAMP2/MODE","MintAMP2/SKIN","MintAMP2/DOUBLESIZE"};
static char env[4][512],disk[4][512];
static int answers[8],answer_index,icon_reads,frees;
static BPTR directory=7;
static char *types[]={"GUI=GADTOOLS","MODE=SKINNED","SKIN=Work:old skin.wsz","DOUBLESIZE=YES",NULL};
static struct DiskObject icon={types};
static int key_index(const char *key)
{ int i; for (i=0;i<4;++i) if (!strcmp(key,keys[i])) return i; return -1; }
LONG GetVar(const char *key,char *value,LONG size,LONG flags)
{
    int i=key_index(key); size_t n; (void)flags;
    if (i<0 || !env[i][0]) return -1;
    n=strlen(env[i]); if (n>=(size_t)size) return -1;
    memcpy(value,env[i],n+1); return (LONG)n;
}
int SetVar(const char *key,const char *value,LONG size,LONG flags)
{
    int i=key_index(key); (void)size; assert(i>=0 && strlen(value)<512);
    strcpy(env[i],value); if (flags&GVF_SAVE_VAR) strcpy(disk[i],value); return 1;
}
BPTR CurrentDir(BPTR lock) { BPTR old=directory; directory=lock; return old; }
struct DiskObject *GetDiskObject(const char *name)
{ assert(directory==42 && !strcmp(name,"MintAMP-Launch")); ++icon_reads; return &icon; }
STRPTR FindToolType(STRPTR *tt,STRPTR key)
{ int i; size_t n=strlen(key); for (i=0;tt[i];++i) if (!strncmp(tt[i],key,n) && tt[i][n]=='=') return tt[i]+n+1; return NULL; }
void FreeDiskObject(struct DiskObject *object) { assert(object==&icon); ++frees; }
LONG EasyRequestArgs(struct Window *w,struct EasyStruct *es,APTR idcmp,APTR args)
{ (void)w;(void)es;(void)idcmp;(void)args; assert(answer_index<8); return answers[answer_index++]; }
int main(void)
{
    MintLaunchOptions o; struct WBArg arg={42,"MintAMP-Launch"}; struct WBStartup wb={1,&arg};
    const char *audio; char *cli[]={"launch","--gui","REACTION","--normal"};
    SetVar(keys[0],"REACTION",-1,GVF_GLOBAL_ONLY); SetVar(keys[1],"NATIVE",-1,GVF_GLOBAL_ONLY);
    mint_launch_defaults(&o); mint_interface_load(&o); assert(!o.gadtools && !o.skinned);
    mint_interface_tooltypes(&o,0,(char **)&wb);
    assert(o.gadtools && o.skinned && o.doublesize && !strcmp(o.skin,"Work:old skin.wsz"));
    assert(icon_reads==1 && frees==1 && directory==7);
    assert(mint_launch_args(&o,4,cli,&audio)); assert(!o.gadtools && !o.doublesize && o.skinned);
    mint_interface_tooltypes(&o,1,cli); assert(icon_reads==1); /* CLI does not read WB memory */
    /* Reproducer: live size changes to double, then next-launch prefs choose
     * normal. Late skin shutdown must persist the newer shared NO. */
    SetVar(keys[2],"Work:mintleaf.wsz",-1,GVF_GLOBAL_ONLY);
    SetVar(keys[3],"YES",-1,GVF_GLOBAL_ONLY);
    answers[0]=2; answers[1]=2; answers[2]=1; answers[3]=1;
    mint_interface_preferences(NULL);
    assert(!strcmp(env[3],"NO") && !disk[3][0]); /* no live disk IO */
    mint_interface_flush_skin(); assert(!strcmp(disk[3],"NO"));
    mint_interface_flush(); assert(!strcmp(disk[0],"GADTOOLS") && !strcmp(disk[1],"CLASSIC"));
    assert(!strcmp(disk[2],"Work:mintleaf.wsz") && !strcmp(disk[3],"NO"));
    /* Live menu tick: ENV changes immediately, disk only on clean shutdown. */
    mint_interface_set_mode(1); assert(!strcmp(env[1],"SKINNED") && !strcmp(disk[1],"CLASSIC"));
    mint_interface_flush(); assert(!strcmp(disk[1],"SKINNED"));
    /* Start mode: CLI > own icon > shared ENV > skinned default. */
    { char *classic[]={"MintAMP-SR","--classic"}; char *plain[]={"MintAMP-SR","a.mp3"};
      env[1][0]=0; assert(mint_interface_start_skinned(2,plain)==1);
      SetVar(keys[1],"CLASSIC",-1,GVF_GLOBAL_ONLY); assert(mint_interface_start_skinned(2,plain)==0);
      SetVar(keys[1],"SKINNED",-1,GVF_GLOBAL_ONLY); assert(mint_interface_start_skinned(2,classic)==0);
      types[1]="MODE=CLASSIC"; assert(mint_interface_start_skinned(0,(char **)&wb)==0); types[1]="MODE=SKINNED"; }
    /* Cancel performs no edits, including a partially entered choice. */
    answer_index=0; answers[0]=1; answers[1]=0;
    mint_interface_preferences(NULL); assert(!strcmp(env[0],"GADTOOLS"));
    puts("Workbench lock/icon lifecycle, precedence, cancel and late persistence ordering passed"); return 0;
}
