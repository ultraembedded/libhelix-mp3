/* Minimal DOS/icon/requester surface for exercising the real prefs module. */
#ifndef MINTAMP_PREFS_STUBS_H
#define MINTAMP_PREFS_STUBS_H
#include <stddef.h>
typedef long LONG; typedef unsigned long BPTR; typedef char *STRPTR; typedef void *APTR;
#define GVF_GLOBAL_ONLY 1
#define GVF_SAVE_VAR 2
struct Window { int unused; };
struct EasyStruct { unsigned long es_StructSize; STRPTR es_Title,es_TextFormat,es_GadgetFormat; };
struct WBArg { BPTR wa_Lock; STRPTR wa_Name; };
struct WBStartup { int sm_NumArgs; struct WBArg *sm_ArgList; };
struct DiskObject { STRPTR *do_ToolTypes; };
LONG GetVar(const char *key,char *value,LONG size,LONG flags);
int SetVar(const char *key,const char *value,LONG size,LONG flags);
BPTR CurrentDir(BPTR lock);
struct DiskObject *GetDiskObject(const char *name);
STRPTR FindToolType(STRPTR *types,STRPTR key);
void FreeDiskObject(struct DiskObject *icon);
LONG EasyRequestArgs(struct Window *w,struct EasyStruct *es,APTR idcmp,APTR args);
#endif
