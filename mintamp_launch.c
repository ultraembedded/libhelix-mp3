/* Lightweight dispatcher: no shell, decoder or resident launcher task.
 * A CLI process is necessary even when the dispatcher starts in Workbench. */
#include "interface_prefs.h"
#include <stdio.h>
#include <string.h>
#include <workbench/startup.h>
#include <dos/dostags.h>
#include <intuition/intuition.h>
#include <proto/dos.h>
#include <proto/intuition.h>
static int message(int wb,const char *text)
{
    if (wb) {
        struct EasyStruct es; memset(&es,0,sizeof(es)); es.es_StructSize=sizeof(es);
        es.es_Title=(STRPTR)"MintAMP 2.0"; es.es_TextFormat=(STRPTR)"%s"; es.es_GadgetFormat=(STRPTR)"OK";
        EasyRequestArgs(NULL,&es,NULL,(APTR)&text);
    } else fprintf(stderr,"%s\n",text);
    return 20;
}
int main(int argc,char **argv)
{
    MintLaunchOptions o; const char *audio=NULL; char binary[1024],args[3072],quoted[1100],path[1024];
    BPTR seg,input,output; struct Process *child; int wb=argc==0;
    mint_launch_defaults(&o); mint_interface_load(&o); mint_interface_tooltypes(&o,argc,argv);
    if (!wb && argc==2 && !strcmp(argv[1],"--prefs")) { mint_interface_preferences(NULL); mint_interface_flush(); return 0; }
    if (!wb && !mint_launch_args(&o,argc,argv,&audio))
        return message(wb,"Usage: MintAMP-Launch [audio/playlist] [--gui REACTION|GADTOOLS] [--mode SKINNED|CLASSIC] [--classic] [--skin path] [--double|--normal]\nUse --prefs to save the next-launch interface.");
    if (wb) {
        struct WBStartup *startup=(struct WBStartup *)argv;
        if (startup && startup->sm_NumArgs>1) {
            if (!NameFromLock(startup->sm_ArgList[1].wa_Lock,path,sizeof(path)) ||
                !AddPart(path,startup->sm_ArgList[1].wa_Name,sizeof(path))) return message(wb,"Workbench argument path is too long.");
            audio=path;
        }
    }
    args[0]=0;
    /* The file comes first. Both appearances run the skin-capable edition;
     * the mode is always explicit so a saved per-edition choice cannot
     * contradict the launcher. Explicit NO must override legacy Scale=2. */
    if (audio) { if (!mint_launch_quote(quoted,sizeof(quoted),audio)) return message(wb,"Invalid or oversized audio path."); strcat(args,quoted); strcat(args," "); }
    strcat(args,o.skinned ? "--skinned " : "--classic ");
    if (o.skin_set) {
        if (!mint_launch_quote(quoted,sizeof(quoted),o.skin)) return message(wb,"Invalid skin path.");
        strcat(args,"--skin "); strcat(args,quoted); strcat(args," ");
    }
    if (o.size_set) strcat(args,o.doublesize ? "--double " : "--normal ");
    strcat(args,"\n");
    if (!NameFromLock(GetProgramDir(),binary,sizeof(binary)) || !AddPart(binary,(STRPTR)mint_launch_binary(&o),sizeof(binary))) return message(wb,"Cannot resolve the MintAMP installation directory.");
    seg=LoadSeg(binary);
    if (!seg) return message(wb,"Could not load the selected MintAMP edition.\nKeep MintAMP-SR and MintAMP-SGT beside MintAMP-Launch.");
    input=Open("NIL:",MODE_OLDFILE); output=Open("NIL:",MODE_NEWFILE);
    if (!input || !output) {
        if (input) Close(input);
        if (output) Close(output);
        UnLoadSeg(seg); return message(wb,"Cannot open launcher IO handles.");
    }
    /* DOS copies Arguments/CommandName and duplicates CurrentDir/HomeDir.
     * The child owns the segment and IO handles only after successful creation. */
    child=CreateNewProcTags(NP_Seglist,(ULONG)seg,NP_FreeSeglist,TRUE,
        NP_Cli,TRUE,NP_CommandName,(ULONG)mint_launch_binary(&o),
        NP_Name,(ULONG)mint_launch_binary(&o),NP_Arguments,(ULONG)args,
        NP_Input,(ULONG)input,NP_Output,(ULONG)output,
        /* Match both frontends' existing 256 KiB startup requirement, avoiding
         * a second stack allocation/swap on low-memory machines. */
        NP_CloseInput,TRUE,NP_CloseOutput,TRUE,NP_StackSize,262144,TAG_DONE);
    if (!child) {
        Close(input); Close(output); UnLoadSeg(seg);
        return message(wb,"Could not start MintAMP: insufficient process memory.");
    }
    return 0;
}
