#include "launch_options.h"
#include <string.h>
#include <ctype.h>
static int equal(const char *a,const char *b)
{
    while (*a && *b && toupper((unsigned char)*a)==toupper((unsigned char)*b)) { ++a; ++b; }
    return !*a && !*b;
}
static int equal_prefix(const char *a,const char *prefix)
{
    while (*prefix && toupper((unsigned char)*a)==toupper((unsigned char)*prefix)) { ++a; ++prefix; }
    return !*prefix;
}
void mint_launch_defaults(MintLaunchOptions *o)
{ memset(o,0,sizeof(*o)); o->skinned=1; strcpy(o->skin,MINTAMP_DEFAULT_SKIN); }
int mint_launch_option(MintLaunchOptions *o,const char *key,const char *v)
{
    if (!v) return 0;
    if (equal(key,"GUI")) {
        if (!equal(v,"REACTION") && !equal(v,"GADTOOLS")) return 0;
        o->gadtools=equal(v,"GADTOOLS");
    } else if (equal(key,"MODE")) {
        /* NATIVE is the 2.0-dev spelling of CLASSIC; keep saved values valid. */
        if (!equal(v,"CLASSIC") && !equal(v,"NATIVE") && !equal(v,"SKINNED") && !equal(v,"SKIN")) return 0;
        o->skinned=equal(v,"SKINNED") || equal(v,"SKIN");
    } else if (equal(key,"DOUBLESIZE")) {
        if (!equal(v,"YES") && !equal(v,"NO") && !equal(v,"TRUE") && !equal(v,"FALSE") && !equal(v,"1") && !equal(v,"0") && *v) return 0;
        o->doublesize=!*v || equal(v,"YES") || equal(v,"TRUE") || equal(v,"1"); o->size_set=1;
    } else if (equal(key,"SKIN")) {
        size_t i,n=strlen(v);
        if (!n || n>=sizeof(o->skin)) return 0;
        for (i=0;i<n;++i) if ((unsigned char)v[i]<32 || (unsigned char)v[i]==127) return 0;
        strcpy(o->skin,v); o->skin_set=1;
    } else return 0;
    return 1;
}
/* Both appearances use the skin-capable edition so the menu tick can switch
 * live. MintAMP and MintAMP-GT remain directly launchable classic-only builds. */
const char *mint_launch_binary(const MintLaunchOptions *o)
{ return o->gadtools ? "MintAMP-SGT" : "MintAMP-SR"; }
int mint_launch_args(MintLaunchOptions *o,int argc,char **argv,const char **audio)
{
    int i; *audio=NULL;
    for (i=1;i<argc;++i) {
        const char *a=argv[i],*key=NULL,*value=NULL;
        char k[16]; const char *eq=strchr(a,'=');
        if (!strcmp(a,"--double") || !strcmp(a,"--normal")) {
            mint_launch_option(o,"DOUBLESIZE",!strcmp(a,"--double") ? "YES" : "NO"); continue;
        }
        if (!strcmp(a,"--classic") || !strcmp(a,"--skinned")) {
            mint_launch_option(o,"MODE",!strcmp(a,"--classic") ? "CLASSIC" : "SKINNED"); continue;
        }
        if (!strcmp(a,"--gui")) key="GUI";
        else if (!strcmp(a,"--mode")) key="MODE";
        else if (!strcmp(a,"--skin")) key="SKIN";
        else if (!strcmp(a,"--doublesize")) key="DOUBLESIZE";
        if (key) { if (++i>=argc) return 0; value=argv[i]; }
        else if (eq && eq-a>0 && eq-a<(int)sizeof(k)) {
            memcpy(k,a,(size_t)(eq-a)); k[eq-a]=0;
            if (equal(k,"GUI") || equal(k,"MODE") || equal(k,"SKIN") || equal(k,"DOUBLESIZE")) { key=k; value=eq+1; }
        }
        if (key) { if (!mint_launch_option(o,key,value)) return 0; }
        else if (a[0]=='-' || *audio) return 0;
        else *audio=a;
    }
    return 1;
}
int mint_launch_quote(char *out,size_t size,const char *a)
{
    size_t n=0; if (size<3) return 0; out[n++]='"';
    while (*a) {
        unsigned char c=(unsigned char)*a++;
        if (c<32 || c==127 || n+3>=size) return 0;
        if (c=='*' || c=='"') out[n++]='*';
        out[n++]=(char)c;
    }
    out[n++]='"'; out[n]=0; return 1;
}
static int mode_value(const char *v)
{
    MintLaunchOptions o; mint_launch_defaults(&o);
    return mint_launch_option(&o,"MODE",v) ? o.skinned : -1;
}
int mint_launch_option_span(int argc,char **argv,int i)
{
    const char *a=argv[i];
    if (!strcmp(a,"--classic") || !strcmp(a,"--skinned")) return 1;
    if (!strcmp(a,"--mode") || !strcmp(a,"--gui")) return i+1<argc ? 2 : 1;
    if (strlen(a)>5 && equal_prefix(a,"MODE=")) return 1;
    return 0;
}
int mint_launch_mode_arg(int argc,char **argv)
{
    int i,mode=-1,v;
    for (i=1;i<argc;++i) {
        const char *a=argv[i];
        if (!strcmp(a,"--classic")) mode=0;
        else if (!strcmp(a,"--skinned")) mode=1;
        else if (!strcmp(a,"--mode") && i+1<argc) { if ((v=mode_value(argv[++i]))>=0) mode=v; }
        else if (strlen(a)>5 && equal_prefix(a,"MODE=")) { if ((v=mode_value(a+5))>=0) mode=v; }
    }
    return mode;
}
