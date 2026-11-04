#include "launch_options.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
int main(void)
{
    MintLaunchOptions o,before; const char *audio; char q[64];
    char *args[]={"MintAMP-Launch","Work:my song.mp3","--gui","REACTION","MODE=SKINNED","--normal","--skin","Work:my skin.wsz"};
    char *bad[]={"launch","--skin"}; char *unknown[]={"launch","--bogus"};
    char *url[]={"launch","https://station/?token=value"}; char *many[]={"launch","one","two"};
    char *modes[]={"MintAMP-SR","--classic","x.mp3","MODE=skinned","--mode","CLASSIC","--skin","s.wsz","--double"};
    char *flip[]={"launch","--classic","t.mp3"};
    mint_launch_defaults(&o); assert(o.skinned && !strcmp(mint_launch_binary(&o),"MintAMP-SR") && !o.skin_set && !o.size_set);
    /* Classic also runs the skin-capable edition, so the menu tick can switch. */
    assert(mint_launch_option(&o,"MODE","Classic") && !o.skinned && !strcmp(mint_launch_binary(&o),"MintAMP-SR"));
    assert(mint_launch_option(&o,"MODE","SKINNED") && o.skinned);
    /* saved layer -> ToolTypes layer -> CLI */
    assert(mint_launch_option(&o,"GUI","GADTOOLS") && mint_launch_option(&o,"MODE","SKINNED"));
    assert(!strcmp(mint_launch_binary(&o),"MintAMP-SGT"));
    assert(mint_launch_option(&o,"DOUBLESIZE","yes"));
    assert(mint_launch_option(&o,"MODE","native") && !o.skinned); assert(!strcmp(mint_launch_binary(&o),"MintAMP-SGT"));
    assert(mint_launch_args(&o,8,args,&audio)); assert(!strcmp(audio,"Work:my song.mp3"));
    assert(!o.gadtools && o.skinned && !o.doublesize && o.size_set && o.skin_set);
    assert(!strcmp(mint_launch_binary(&o),"MintAMP-SR") && !strcmp(o.skin,"Work:my skin.wsz"));
    before=o; assert(!mint_launch_option(&o,"GUI","bad") && !memcmp(&o,&before,sizeof(o)));
    assert(!mint_launch_option(&o,"SKIN","a\nb") && !memcmp(&o,&before,sizeof(o)));
    assert(!mint_launch_args(&o,2,bad,&audio)); assert(!mint_launch_args(&o,2,unknown,&audio));
    assert(!mint_launch_args(&o,3,many,&audio)); assert(mint_launch_args(&o,2,url,&audio));
    assert(!strcmp(audio,url[1]));
    assert(mint_launch_quote(q,sizeof(q),"Work:skin *\"x\".wsz"));
    assert(!strcmp(q,"\"Work:skin ***\"x*\".wsz\""));
    assert(!mint_launch_quote(q,4,"abcdef") && !mint_launch_quote(q,sizeof(q),"a\nb"));
    assert(mint_launch_option(&o,"DOUBLESIZE","No") && !o.doublesize);
    /* Edition-side mode switches: later wins, values are validated. */
    assert(mint_launch_mode_arg(9,modes)==0 && mint_launch_mode_arg(4,modes)==1);
    assert(mint_launch_mode_arg(1,modes)==-1 && mint_launch_mode_arg(3,modes)==0);
    assert(mint_launch_option_span(9,modes,1)==1 && mint_launch_option_span(9,modes,2)==0);
    assert(mint_launch_option_span(9,modes,3)==1 && mint_launch_option_span(9,modes,4)==2);
    assert(mint_launch_option_span(5,modes,4)==1 && mint_launch_option_span(9,modes,6)==0);
    mint_launch_defaults(&o); assert(mint_launch_args(&o,3,flip,&audio) && !o.skinned && !strcmp(audio,"t.mp3"));
    puts("Launcher skin-first default, classic/skinned switches, precedence, argument validation/quoting passed"); return 0;
}
