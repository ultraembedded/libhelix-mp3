/* Shared skin playlist operations. Borrow the native arrays, parser and add
 * callback; utility windows remain the owner of the underlying playlist. */
typedef struct SkinPlaylistOps {
    SkinList list;
    void *ctx;
    int (*add)(void *ctx,const char *path,const char *title);
    void (*files)(void *ctx);
    void (*save)(void *ctx);
    void (*refresh)(void *ctx);
    char drawer[512];
    int added,last_added;
} SkinPlaylistOps;
static int SkinPlaylistImport(void *ctx,const char *location,const char *title)
{
    SkinPlaylistOps *o=(SkinPlaylistOps *)ctx;
    char path[512],shown[128];
    o->last_added=-1;
    if (*o->list.count>=SKIN_LIST_MAX) return 0;
    AmigaUtf8ToDisplay(shown,sizeof(shown),title);
    if (strchr(location,':') || location[0]=='/') {
        if (o->add(o->ctx,location,shown)) { ++o->added; o->last_added=*o->list.count-1; }
    } else {
        SafeCopy(path,sizeof(path),o->drawer);
        if (AddPart(path,(STRPTR)location,sizeof(path)) && o->add(o->ctx,path,shown)) { ++o->added; o->last_added=*o->list.count-1; }
    }
    return 1;
}
static void SkinPlaylistDuration(void *ctx,int seconds)
{ SkinPlaylistOps *o=(SkinPlaylistOps *)ctx; if (o->last_added>=0 && o->list.durations) o->list.durations[o->last_added]=seconds; }
static int SkinPlaylistCount(void *ctx,const char *location,const char *title)
{ (void)ctx; (void)location; (void)title; return 1; }
static int SkinPlaylistRequest(SkinPlaylistOps *o,char *path,int directory)
{
    struct FileRequester *fr;
    int ok=0;
    fr=(struct FileRequester *)AllocAslRequestTags(ASL_FileRequest,
        ASLFR_TitleText,(ULONG)(directory ? "Add directory (one level)" : "Load or append M3U/M3U8/PLS"),
        ASLFR_DrawersOnly,(ULONG)directory,ASLFR_DoPatterns,(ULONG)!directory,
        ASLFR_InitialPattern,(ULONG)"#?.(m3u|m3u8|pls)",TAG_DONE);
    if (!fr) return 0;
    if (AslRequestTags(fr,ASLFR_Window,(ULONG)skin_player_window(gSkinPlayer),ASLFR_SleepWindow,TRUE,TAG_DONE)) {
        SafeCopy(path,512,(const char *)fr->fr_Drawer);
        SafeCopy(o->drawer,sizeof(o->drawer),(const char *)fr->fr_Drawer);
        ok=directory || (fr->fr_File[0] && AddPart(path,fr->fr_File,512));
    }
    FreeAslRequest(fr); return ok;
}
static void SkinPlaylistLoad(SkinPlaylistOps *o,int append)
{
    char path[512],*text; SkinListSnapshot *backup=NULL; BPTR fh; LONG len=0,got; const LONG max=96L*1024L;
    if (!SkinPlaylistRequest(o,path,0)) return;
    fh=Open(path,MODE_OLDFILE);
    if (!fh) { skin_player_notice(gSkinPlayer,"COULD NOT OPEN PLAYLIST"); return; }
    text=(char *)malloc(max+1);
    if (!text) { Close(fh); skin_player_notice(gSkinPlayer,"NOT ENOUGH MEMORY FOR PLAYLIST"); return; }
    while (len<max+1 && (got=Read(fh,text+len,max+1-len))>0) len+=got;
    Close(fh);
    if (got<0 || len>max || !playlist_parse(text,(size_t)len,SkinPlaylistCount,NULL)) {
        free(text); skin_player_notice(gSkinPlayer,"PLAYLIST INVALID EMPTY OR TOO LARGE - LIST RETAINED"); return;
    }
    if (!append) {
        backup=skin_list_snapshot(&o->list);
        if (!backup) { free(text); skin_player_notice(gSkinPlayer,"NOT ENOUGH MEMORY - LIST RETAINED"); return; }
        *o->list.count=0; *o->list.current=*o->list.selected=-1;
        memset(o->list.selection,0,sizeof(*o->list.selection)); o->list.selection->focus=-1;
    }
    playlist_parse_ex(text,(size_t)len,SkinPlaylistImport,o,SkinPlaylistDuration); free(text);
    if (backup) {
        if (!o->added) { skin_list_restore(&o->list,backup); skin_player_notice(gSkinPlayer,"NO VALID ENTRIES - LIST RETAINED"); }
        skin_list_snapshot_free(backup);
    }
    if (*o->list.selected<0 && *o->list.count) *o->list.selected=0;
}
static void SkinPlaylistDirectory(SkinPlaylistOps *o)
{
    struct FileInfoBlock *fib; BPTR lock; char drawer[512],path[512]; LONG pattern;
    char compiled[256]; int examined=0;
    if (!SkinPlaylistRequest(o,drawer,1)) return;
    fib=(struct FileInfoBlock *)AllocDosObject(DOS_FIB,TAG_DONE);
    if (!fib) { skin_player_notice(gSkinPlayer,"NOT ENOUGH MEMORY FOR DIRECTORY"); return; }
    lock=Lock(drawer,ACCESS_READ);
    pattern=ParsePatternNoCase((STRPTR)gSupportedExtPattern,compiled,sizeof(compiled));
    if (lock && Examine(lock,fib) && fib->fib_DirEntryType>0) {
        while (*o->list.count<SKIN_LIST_MAX && examined++<4096 && ExNext(lock,fib)) {
            if (fib->fib_DirEntryType>=0) continue;
            SafeCopy(path,sizeof(path),drawer);
            if (pattern>=0 && MatchPatternNoCase(compiled,fib->fib_FileName) &&
                AddPart(path,fib->fib_FileName,sizeof(path)) && o->add(o->ctx,path,NULL)) { ++o->added; o->last_added=*o->list.count-1; }
        }
    }
    if (lock) UnLock(lock);
    FreeDosObject(DOS_FIB,fib);
    skin_player_notice(gSkinPlayer,o->added ? "DIRECTORY ADDED - SUBDIRECTORIES ARE NOT RECURSED" : "NO SUPPORTED FILES ADDED");
}
static void SkinPlaylistAction(SkinPlaylistOps *o,const SkinEvent *e)
{
    int choice=0,i,changed=0;
    if (e->action==SKIN_TRACK_MOVE) { changed=e->selection ? skin_list_move_selected(&o->list,e->value,e->target) : skin_list_move(&o->list,e->value,e->target); }
    else if (e->action==SKIN_TRACK_REMOVE) choice=1;
    else if (e->action==SKIN_LIST_ADD) {
        choice=skin_player_choice(gSkinPlayer,"Add to playlist","Files|Drawer|List|Cancel");
        if (choice==1) o->files(o->ctx);
        else if (choice==2) SkinPlaylistDirectory(o);
        else if (choice==3) SkinPlaylistLoad(o,1);
        changed=choice>=1 && choice<=3; choice=0;
    } else if (e->action==SKIN_LIST_REM) choice=skin_player_choice(gSkinPlayer,"Remove from playlist","Selected|Missing|Clear|Cancel");
    else if (e->action==SKIN_LIST_SEL) {
        choice=skin_player_choice(gSkinPlayer,"Select entries","All|None|Invert|Cancel");
        if (choice>=1 && choice<=3) skin_selection_all(o->list.selection,choice==1 ? 1 : choice==2 ? 0 : 2);
        choice=0;
    } else if (e->action==SKIN_LIST_MISC) {
        choice=skin_player_choice(gSkinPlayer,"Playlist operations","Title|Path|Up|Down|Cancel");
        if (choice==1 || choice==2) { skin_list_sort(&o->list,choice==2); changed=1; }
        else if (choice==3 || choice==4) changed=skin_list_move(&o->list,*o->list.selected,*o->list.selected+(choice==3 ? -1 : 1));
        choice=0;
    } else if (e->action==SKIN_LIST_FILE) {
        choice=skin_player_choice(gSkinPlayer,"M3U/M3U8/PLS playlists","Load|Save|Append|Cancel");
        if (choice==1 || choice==3) { SkinPlaylistLoad(o,choice==3); changed=1; }
        else if (choice==2) o->save(o->ctx);
        choice=0;
    }
    if (choice>=1 && choice<=3 && (e->action==SKIN_LIST_REM || e->action==SKIN_TRACK_REMOVE)) {
        for (i=*o->list.count-1;i>=0;--i) {
            int remove=choice==3 || (choice==1 && o->list.selection->bits[i]);
            if (choice==2) {
                const char *path=o->list.paths+i*o->list.path_size;
                if (!strstr(path,"://")) {
                    BPTR lock=Lock((STRPTR)path,ACCESS_READ);
                    if (lock) UnLock(lock);
                    else { LONG err=IoErr(); remove=err==ERROR_OBJECT_NOT_FOUND || err==ERROR_DIR_NOT_FOUND; }
                }
            }
            if (remove) { skin_list_remove(&o->list,i); changed=1; }
        }
    }
    if (changed) skin_player_order_reset(gSkinPlayer,*o->list.count,*o->list.current);
    o->refresh(o->ctx);
}
