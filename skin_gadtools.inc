#include "skin_playlist_actions.inc"
static int GuiSkinAdd(void *ctx,const char *path,const char *title)
{ return PlaylistAddEntry(&((HelixAmp3Gui *)ctx)->playlist,path,title); }
static void GuiSkinFiles(void *ctx) { PlaylistAddFiles((HelixAmp3Gui *)ctx); }
static void GuiSkinSave(void *ctx)
{ HelixAmp3Gui *gui=(HelixAmp3Gui *)ctx; PlaylistSaveM3U(gui); skin_player_notice(gSkinPlayer,gui->statusText); }
static void GuiSkinRefresh(void *ctx) { RefreshPlaylistView((HelixAmp3Gui *)ctx); }
static SkinPlaylistOps GuiSkinOps(HelixAmp3Gui *gui)
{
    SkinPlaylistOps o;
    memset(&o,0,sizeof(o)); o.ctx=gui;
    o.add=GuiSkinAdd; o.files=GuiSkinFiles; o.save=GuiSkinSave; o.refresh=GuiSkinRefresh;
    o.list.paths=&gui->playlist.paths[0][0]; o.list.path_size=sizeof(gui->playlist.paths[0]);
    o.list.titles=&gui->playlist.titles[0][0]; o.list.title_size=sizeof(gui->playlist.titles[0]);
    o.list.names=&gui->playlist.names[0][0]; o.list.name_size=sizeof(gui->playlist.names[0]);
    o.list.durations=gui->playlist.durations;
    o.list.count=&gui->playlist.count; o.list.current=&gui->playlist.current; o.list.selected=&gui->playlist.selected;
    o.list.selection=skin_player_selection(gSkinPlayer,gui->playlist.count,gui->playlist.selected);
    return o;
}
/* Included only by the MintAMP-SGT frontend. Reuse its established actions. */
static void GuiSkinHideSettings(HelixAmp3Gui *gui)
{
    if (!gui->win) return;
    ReleaseArtColorPens(gui);
    gui->iconifyLeft=gui->win->LeftEdge; gui->iconifyTop=gui->win->TopEdge;
    if (gui->menuStrip) ClearMenuStrip(gui->win);
    if (gui->gadgets) RemoveGList(gui->win,gui->gadgets,-1);
    DrainWindowMessages(gui); CloseWindow(gui->win); gui->win=NULL;
    /* Reopening uses the existing AppIcon restore path, without requiring
     * an AppIcon or a standard edition's .info file just to hide settings. */
    gui->iconified=1;
}
/* Live switch from the skin to the classic MintAMP-SGT window. Playback,
 * the playlist and the utility windows continue untouched. */
static void GuiSkinToClassic(HelixAmp3Gui *gui)
{
    if (!gSkinPlayer) return;
    /* The classic window has no Pause control: never leave audio held. */
    if (gSkinAudio.pause_requested || gSkinAudio.paused) skin_player_wake("MintAMP-GT playback",0);
    skin_player_order_cancel(gSkinPlayer); gSkinPendingTrack=-1; gSkinPendingPlay=0;
    skin_player_suspend(gSkinPlayer); gSkinPlayer=NULL;
    if (gui->win) SetWindowTitles(gui->win,(UBYTE *)"MintAMP-SGT",(UBYTE *)~0);
    else GuiRestoreFromAppIcon(gui);
    if (!gui->win && !gui->appIcon) {
        /* No way back to a visible interface: return to the skin. */
        gSkinPlayer=skin_player_open("MintAMP-SGT",gSkinArgc,gSkinArgv);
        if (gSkinPlayer) { skin_player_notice(gSkinPlayer,"COULD NOT OPEN THE CLASSIC WINDOW"); return; }
        gui->closeRequested=1; return;
    }
    mint_interface_set_mode(0);
    SyncMenuChecks(gui);
    if (gui->win) { WindowToFront(gui->win); ActivateWindow(gui->win); SetStatus(gui,"Classic interface. Untick Project/Classic interface for the skin."); }
}
static void GuiSkinUpdate(HelixAmp3Gui *gui,int tick);
static void GuiClassicToSkin(HelixAmp3Gui *gui)
{
    if (gSkinPlayer) return;
    gSkinPlayer=skin_player_open("MintAMP-SGT",gSkinArgc,gSkinArgv);
    if (!gSkinPlayer) {
        SyncMenuChecks(gui);
        SetStatus(gui,"Could not open the skin; staying with the classic interface.");
        return;
    }
    mint_interface_set_mode(1);
    SyncMenuChecks(gui);
    if (gui->win) GuiSkinHideSettings(gui);
    GuiSkinUpdate(gui,0);
}
static void GuiSkinInfo(HelixAmp3Gui *gui)
{
    char text[640],station[128],stream[128];
    if (IsRadioInputName(gui->inputName)) {
        CopyVolatileGuiString(station,sizeof(station),gGuiPlaybackStatus.radioStationName);
        CopyVolatileGuiString(stream,sizeof(stream),gGuiPlaybackStatus.radioTitle);
        snprintf(text,sizeof(text),"Station: %s\nNow playing: %s\nBitrate: %d kbit/s\nURL: %.200s",
            station[0] ? station : "-",stream[0] ? stream : "-",
            gSkinAudio.bitrate ? gSkinAudio.bitrate : (int)gGuiPlaybackStatus.radioBitrateKbps,gui->inputName);
    } else if (gui->inputName[0]) {
        FormatFileInfo(gui);
        snprintf(text,sizeof(text),"Title: %s\nArtist: %s\nAlbum: %s\nTrack: %s\nGenre: %s\n%s\nFile: %.200s",
            gui->tags.title[0] ? gui->tags.title : "-",gui->tags.artist[0] ? gui->tags.artist : "-",
            gui->tags.album[0] ? gui->tags.album : "-",gui->tags.track[0] ? gui->tags.track : "-",
            gui->tags.genre[0] ? gui->tags.genre : "-",gui->fileInfoText,gui->inputName);
    } else SafeCopy(text,sizeof(text),"No audio loaded.\nOpen a file (O) or a stream URL (U).");
    skin_player_choice(gSkinPlayer,text,"OK");
}
static void GuiSkinStream(HelixAmp3Gui *gui)
{
    char url[HELIXAMP3_MAX_PATH];
    if (!gui->hasNetwork) { skin_player_notice(gSkinPlayer,"NO TCP/IP STACK AVAILABLE"); return; }
    SafeCopy(url,sizeof(url),IsRadioInputName(gui->inputName) ? gui->inputName : "http://");
    if (!skin_player_request_text(gSkinPlayer,"Open HTTP/HTTPS stream (Return plays, close cancels)",url,(int)sizeof(url))) return;
    if (!is_url_path(url)) { skin_player_notice(gSkinPlayer,"STREAM URL MUST START WITH HTTP:// OR HTTPS://"); return; }
    skin_player_order_cancel(gSkinPlayer); gSkinPendingTrack=-1;
    if (gui->playbackActive || gui->playbackDonePending) StopPlayback(gui);
    gui->queuedInputName[0]=0; gui->queuedPlayPending=gui->playlistNextPending=0;
    /* Typed URLs have no known station favicon. */
    gui->currentRadioFavicon[0]=0;
    SelectInternetStream(gui,url);
    gSkinPendingPlay=1;
}
static const char *GuiSkinPlaylistName(void *ctx,int index)
{
    HelixAmp3Gui *gui=(HelixAmp3Gui *)ctx;
    return gui->playlist.names[index];
}
static void GuiSkinPlaylistUpdate(HelixAmp3Gui *gui)
{
    if (gui->playlist.current>=0 && gui->playlist.current<gui->playlist.count && gui->totalSecs>0 && !IsRadioInputName(gui->inputName)) gui->playlist.durations[gui->playlist.current]=gui->totalSecs;
    skin_player_playlist_durations(gSkinPlayer,gui->playlist.durations);
    skin_player_playlist_update(gSkinPlayer,gui->playlist.count,gui->playlist.selected,
        gui->playlist.current,GuiSkinPlaylistName,gui);
}
/* ---- Skinned settings page ----------------------------------------------
 * The skin's Settings window edits MintAMP-GT's saved settings through the
 * classic window's own handlers, so validation, status text and the
 * ENVARC deferral while playing stay identical. The classic gadgets are
 * kept in step for a later switch to the classic interface. */
enum { GSET_SPEED=1, GSET_QUALITY, GSET_SUBBANDS, GSET_RATE, GSET_CHANNELS, GSET_WIDTH,
    GSET_DELAY, GSET_BUFFER, GSET_FASTMEM, GSET_DECODE_FIRST, GSET_FILTER, GSET_HTTPS,
    GSET_ART, GSET_ART_COLOUR, GSET_ART_CACHE, GSET_ART_CLEAN };
static const char *GuiSkinOnOff(int on) { return on ? "On" : "Off"; }
static const char *GuiSkinLabel(const STRPTR *labels,int index,int count)
{ return (const char *)labels[index>=0 && index<count ? index : 0]; }
static void GuiSkinSettingsRows(HelixAmp3Gui *gui)
{
    SkinSettingsRow rows[SKIN_SETTINGS_MAX]; int n=0,speed=SpeedModeIndex(gui); char text[24];
    if (!gSkinPlayer) return;
    skin_settings_row(rows,&n,SKIN_ROW_HEADER,0,"PLAYBACK",NULL);
    skin_settings_row(rows,&n,SKIN_ROW_VALUE,GSET_SPEED,"Speed mode",
        speed==4 ? "22050 mono ultra" : GuiSkinLabel(kSpeedModeLabels,speed,5));
    skin_settings_row(rows,&n,SKIN_ROW_VALUE,GSET_QUALITY,"Quality",GuiSkinLabel(kQualityLabels,gui->qualityIndex,4));
    skin_settings_row(rows,&n,SKIN_ROW_VALUE,GSET_SUBBANDS,"Subbands",
        GuiSkinLabel(kSubbandCapLabels,gui->subbandCapIndex,(int)SUBBAND_CAP_COUNT));
    snprintf(text,sizeof(text),"%s Hz",GuiSkinLabel(kRateLabels,gui->rateIndex,6));
    skin_settings_row(rows,&n,SKIN_ROW_VALUE,GSET_RATE,"Output rate",text);
    skin_settings_row(rows,&n,SKIN_ROW_VALUE,GSET_CHANNELS,"Channels",GuiSkinLabel(kChannelModeLabels,ChannelModeIndex(gui),3));
    skin_settings_row(rows,&n,SKIN_ROW_VALUE,GSET_WIDTH,"Stereo width",
        gui->fakeStereo ? GuiSkinLabel(kFakeStereoWidthLabels,gui->fakeStereoWidthIndex,5) : "-");
    snprintf(text,sizeof(text),"%s samples",GuiSkinLabel(kFakeStereoDelayLabels,gui->fakeStereoDelayIndex,5));
    skin_settings_row(rows,&n,SKIN_ROW_VALUE,GSET_DELAY,"Stereo delay",gui->fakeStereo ? text : "-");
    snprintf(text,sizeof(text),"%d s",gui->bufferSeconds);
    skin_settings_row(rows,&n,SKIN_ROW_VALUE,GSET_BUFFER,"Buffer",gui->decodeThenPlay ? "Decode first" : text);
    skin_settings_row(rows,&n,SKIN_ROW_VALUE,GSET_DECODE_FIRST,"Decode first",GuiSkinOnOff(gui->decodeThenPlay));
    skin_settings_row(rows,&n,SKIN_ROW_VALUE,GSET_FASTMEM,"Fast RAM",GuiSkinOnOff(gui->fastMem));
    skin_settings_row(rows,&n,SKIN_ROW_VALUE,GSET_FILTER,"Audio filter",GuiSkinOnOff(gui->hardwareFilter));
    skin_settings_row(rows,&n,SKIN_ROW_VALUE,GSET_HTTPS,"HTTPS wait",
        kHttpsWaitDesc[gui->httpsWaitIndex>=0 && gui->httpsWaitIndex<HTTPS_WAIT_COUNT ? gui->httpsWaitIndex : 0]);
    skin_settings_row(rows,&n,SKIN_ROW_HEADER,0,"ARTWORK",NULL);
    skin_settings_row(rows,&n,SKIN_ROW_VALUE,GSET_ART,"Artwork",GuiSkinOnOff(gui->artEnabled));
    skin_settings_row(rows,&n,SKIN_ROW_VALUE,GSET_ART_COLOUR,"Colour art",GuiSkinOnOff(gui->artColorEnabled));
    skin_settings_row(rows,&n,SKIN_ROW_VALUE,GSET_ART_CACHE,"Art cache",GuiSkinOnOff(gui->artCacheEnabled));
    skin_settings_row(rows,&n,SKIN_ROW_ACTION,GSET_ART_CLEAN,"Clear cache","Clean now");
    skin_player_settings_update(gSkinPlayer,rows,n);
}
static struct Gadget *GuiSkinFindGadget(HelixAmp3Gui *gui,UWORD id)
{
    struct Gadget *g;
    for (g=gui->gadgets;g;g=g->NextGadget) if (g->GadgetID==id) return g;
    return NULL;
}
static void GuiSkinToggleFilter(HelixAmp3Gui *gui)
{
    gui->hardwareFilter=!gui->hardwareFilter;
    ApplyHardwareAudioFilter(gui);
    if (gui->gadHardwareFilter)
        GT_SetGadgetAttrs(gui->gadHardwareFilter,gui->win,NULL,GTCB_Checked,(ULONG)gui->hardwareFilter,TAG_DONE);
    skin_player_notice(gSkinPlayer,gui->hardwareFilter ? "AUDIO FILTER ON" : "AUDIO FILTER OFF");
    SaveGuiSettings(gui);
}
static void GuiSkinSettingChange(HelixAmp3Gui *gui,int id,int dir)
{
    struct Gadget fake, *real=NULL; UWORD gid=0; int code=0,shown=-1;
    int busy=gui->playbackActive || gui->playbackDonePending;
    char msg[64];
    if ((id==GSET_SPEED || id==GSET_QUALITY || id==GSET_SUBBANDS || id==GSET_RATE ||
         id==GSET_CHANNELS || id==GSET_BUFFER || id==GSET_FASTMEM) && busy) {
        skin_player_notice(gSkinPlayer,"STOP PLAYBACK TO CHANGE THIS SETTING"); return;
    }
    switch (id) {
    case GSET_SPEED: gid=GID_SPEED_MODE; real=gui->gadSpeedMode; code=skin_settings_cycle(SpeedModeIndex(gui),dir,0,4); break;
    case GSET_QUALITY: gid=GID_QUALITY; code=skin_settings_cycle(gui->qualityIndex,dir,HELIXAMP3_QUALITY_MIN,HELIXAMP3_QUALITY_MAX); break;
    case GSET_SUBBANDS: gid=GID_SUBBAND_CAP; code=skin_settings_cycle(gui->subbandCapIndex,dir,0,(int)SUBBAND_CAP_COUNT-1); break;
    case GSET_RATE: gid=GID_RATE; real=gui->gadRate; code=skin_settings_cycle(gui->rateIndex,dir,0,5); break;
    case GSET_CHANNELS:
        if (gui->cd32Ultrafast) { skin_player_notice(gSkinPlayer,"22050 MONO ULTRAFAST IS MONO ONLY"); return; }
        gid=GID_CHANNEL_MODE; real=gui->gadChannelMode; code=skin_settings_cycle(ChannelModeIndex(gui),dir,0,2); break;
    case GSET_WIDTH: case GSET_DELAY:
        if (!gui->fakeStereo) { skin_player_notice(gSkinPlayer,"SET CHANNELS TO FAKE STEREO FIRST"); return; }
        gid=id==GSET_WIDTH ? GID_FAKE_STEREO_WIDTH : GID_FAKE_STEREO_DELAY;
        real=id==GSET_WIDTH ? gui->gadFakeStereoWidth : gui->gadFakeStereoDelay;
        code=skin_settings_cycle(id==GSET_WIDTH ? gui->fakeStereoWidthIndex : gui->fakeStereoDelayIndex,dir,0,4); break;
    case GSET_BUFFER:
        if (gui->decodeThenPlay) { skin_player_notice(gSkinPlayer,"BUFFER IS NOT USED WITH DECODE FIRST"); return; }
        SetGuiBuffer(gui,skin_settings_cycle(gui->bufferSeconds,dir,1,10),TRUE); break;
    case GSET_FASTMEM:
        /* Its handler always updates the real checkbox. */
        if (!gui->gadFastMem) { skin_player_notice(gSkinPlayer,"FAST RAM OPTION UNAVAILABLE"); return; }
        gid=GID_FAST_MEM; real=gui->gadFastMem; code=!gui->fastMem; break;
    case GSET_DECODE_FIRST: SetDecodeThenPlay(gui,!gui->decodeThenPlay); break;
    case GSET_FILTER: GuiSkinToggleFilter(gui); GuiSkinSettingsRows(gui); return;
    case GSET_HTTPS:
        gui->httpsWaitIndex=skin_settings_cycle(gui->httpsWaitIndex,dir,0,HTTPS_WAIT_COUNT-1);
        SyncMenuChecks(gui);
        sprintf(msg,"HTTPS stream wait set to %s.",kHttpsWaitDesc[gui->httpsWaitIndex]);
        SetStatus(gui,msg); SaveGuiSettings(gui); break;
    case GSET_ART: SetArtworkEnabled(gui,!gui->artEnabled); break;
    case GSET_ART_COLOUR:
        gui->artColorEnabled=!gui->artColorEnabled;
        SetMenuItemChecked(gui,MENUNUM_PLAYBACK,ITEMNUM_ARTCOLOR,gui->artColorEnabled);
        /* Rebuild the 64x64 buffer the skin shows in colour or grey. */
        UpdateArtDisplay(gui);
        if (gui->artDecode.active) SendTimerRequest(gui,ART_TIMER_MICROS);
        SetStatus(gui,gui->artColorEnabled ? "Colour artwork enabled." : "Black and white artwork enabled.");
        SaveGuiSettings(gui); break;
    case GSET_ART_CACHE:
        gui->artCacheEnabled=!gui->artCacheEnabled;
        SetMenuItemChecked(gui,MENUNUM_PLAYBACK,ITEMNUM_ARTCACHE,gui->artCacheEnabled);
        SetStatus(gui,gui->artCacheEnabled ? "Artwork cache enabled." : "Artwork cache disabled.");
        SaveGuiSettings(gui); break;
    case GSET_ART_CLEAN: CleanArtworkCache(gui); break;
    default: return;
    }
    if (gid) {
        if (!real) real=GuiSkinFindGadget(gui,gid);
        if (!real) {
            /* Not busy (checked above), so these handlers never touch gad. */
            memset(&fake,0,sizeof(fake)); fake.GadgetID=gid; real=&fake;
        }
        HandleGuiAction(gui,real,(UWORD)code,IDCMP_GADGETUP,TRUE);
        switch (id) {
        case GSET_SPEED: shown=SpeedModeIndex(gui); break;
        case GSET_QUALITY: shown=gui->qualityIndex; break;
        case GSET_SUBBANDS: shown=gui->subbandCapIndex; break;
        case GSET_RATE: shown=gui->rateIndex; break;
        case GSET_CHANNELS: shown=ChannelModeIndex(gui); break;
        case GSET_WIDTH: shown=gui->fakeStereoWidthIndex; break;
        case GSET_DELAY: shown=gui->fakeStereoDelayIndex; break;
        }
        /* A click on the classic cycle moves it itself; a skin change must. */
        if (shown>=0 && real!=&fake) GT_SetGadgetAttrs(real,gui->win,NULL,GTCY_Active,(ULONG)shown,TAG_DONE);
        UpdateChannelGadgetState(gui);
    }
    if (gui->statusText[0]) skin_player_notice(gSkinPlayer,gui->statusText);
    GuiSkinSettingsRows(gui);
}
static void GuiSkinSettings(HelixAmp3Gui *gui)
{
    if (skin_player_settings_visible(gSkinPlayer)) { skin_player_settings_close(gSkinPlayer); return; }
    GuiSkinSettingsRows(gui);
    if (skin_player_settings_open(gSkinPlayer)) return;
    /* No PLEDIT artwork or too small a screen: the classic window serves. */
    skin_player_notice(gSkinPlayer,"SKIN CANNOT SHOW SETTINGS - USING CLASSIC WINDOW");
    if (gui->win) GuiSkinHideSettings(gui);
    else GuiRestoreFromAppIcon(gui);
}
static void GuiSkinPoll(HelixAmp3Gui *gui)
{
    SkinEvent e;
    while (skin_player_poll(gSkinPlayer,&e)) {
        struct Gadget gad;
        memset(&gad,0,sizeof(gad));
        switch (e.action) {
        case SKIN_QUIT: gui->closeRequested=1; return;
        case SKIN_ABOUT: ShowAbout(gui); break;
        case SKIN_SETTINGS: GuiSkinSettings(gui); break;
        case SKIN_SETTING_CHANGE: GuiSkinSettingChange(gui,e.value,e.target); break;
        case SKIN_BROWSE:
            skin_player_order_cancel(gSkinPlayer);
            gSkinPendingTrack=-1; ChooseMp3(gui); break;
        case SKIN_PLAY:
            if (gSkinAudio.pause_requested || gSkinAudio.paused) { skin_player_wake("MintAMP-GT playback",0); break; }
            if (gui->playbackActive) break;
            gSkinPendingTrack=-1;
            if (!gui->inputName[0] && !gui->queuedInputName[0]) {
                if (gui->playlist.count>0) {
                    skin_player_order_cancel(gSkinPlayer);
                    gSkinPendingTrack=gui->playlist.selected>=0 && gui->playlist.selected<gui->playlist.count ? gui->playlist.selected : 0;
                    gui->queuedPlayPending=gui->playlistNextPending=0;
                    break;
                }
                skin_player_notice(gSkinPlayer,"OPEN AUDIO OR CHOOSE INTERNET RADIO"); break;
            }
            gad.GadgetID=GID_PLAY; HandleGuiAction(gui,&gad,0,IDCMP_GADGETUP,TRUE); break;
        case SKIN_PAUSE:
            if (gui->playbackActive && !gui->playbackDonePending &&
                (gGuiPlaybackStatus.phase==GUIPLAY_PHASE_PLAYING || gGuiPlaybackStatus.phase==GUIPLAY_PHASE_UNDERRUN)) {
                skin_player_wake("MintAMP-GT playback",!gSkinAudio.pause_requested);
                skin_player_notice(gSkinPlayer,gSkinAudio.pause_requested ? "PAUSE PENDING - QUEUED AUDIO WILL FINISH" : "RESUMING");
            } else skin_player_notice(gSkinPlayer,"PAUSE REQUIRES PLAYBACK");
            break;
        case SKIN_BALANCE_SET:
            if (gui->playbackActive && !gSkinAudio.output_stereo) skin_player_notice(gSkinPlayer,"BALANCE REQUIRES STEREO PAULA OUTPUT");
            else { gSkinAudio.balance=e.value; if (e.released) skin_player_save(gSkinPlayer); }
            break;
        case SKIN_DEBUG: {
            char station[128]; CopyVolatileGuiString(station,sizeof(station),gGuiPlaybackStatus.radioStationName);
            skin_player_debug(gSkinPlayer,gGuiPlaybackStatus.halfBufferMs,gGuiPlaybackStatus.spareMs,
                gGuiPlaybackStatus.underruns,gGuiPlaybackStatus.radioBufferedBytes,station); break;
        }
        case SKIN_LIST_ADD: case SKIN_LIST_REM: case SKIN_LIST_SEL: case SKIN_LIST_MISC: case SKIN_LIST_FILE:
        case SKIN_TRACK_MOVE: case SKIN_TRACK_REMOVE: {
            SkinPlaylistOps o=GuiSkinOps(gui);
            gSkinPendingTrack=-1; SkinPlaylistAction(&o,&e); GuiSkinPlaylistUpdate(gui); break;
        }
        case SKIN_STOP:
            skin_player_order_cancel(gSkinPlayer);
            gSkinPendingTrack=-1; StopPlayback(gui); break;
        case SKIN_TRACK_SELECT:
            if (e.value>=0 && e.value<gui->playlist.count) {
                skin_selection_set(skin_player_selection(gSkinPlayer,gui->playlist.count,gui->playlist.selected),e.value,e.selection);
                gui->playlist.selected=e.value; RefreshPlaylistView(gui); GuiSkinPlaylistUpdate(gui);
            }
            break;
        case SKIN_TRACK_PLAY: case SKIN_PREVIOUS: case SKIN_NEXT: {
            int index=e.action==SKIN_TRACK_PLAY ? e.value :
                skin_player_order(gSkinPlayer,gui->playlist.count,gui->playlist.current,e.action==SKIN_NEXT ? 1 : -1,0);
            if (index<0 || index>=gui->playlist.count) {
                skin_player_notice(gSkinPlayer,"NO PLAYLIST TRACK IN THAT DIRECTION"); break;
            }
            if (e.action==SKIN_TRACK_PLAY) skin_player_order_cancel(gSkinPlayer);
            gSkinPendingTrack=index;
            gui->queuedInputName[0]=0; gui->queuedPlayPending=0;
            gui->playlistNextPending=0;
            if (gui->playbackActive || gui->playbackDonePending) StopPlayback(gui);
            break;
        }
        case SKIN_RADIO:
            skin_player_order_cancel(gSkinPlayer);
            gSkinPendingTrack=-1;
            if (gui->hasNetwork) {
                OpenRadioWindow(gui);
                if (!gui->rbWin) skin_player_notice(gSkinPlayer,gui->statusText);
            }
            else skin_player_notice(gSkinPlayer,"NO TCP/IP STACK AVAILABLE");
            break;
        case SKIN_PLAYLIST:
            GuiSkinPlaylistUpdate(gui);
            if (skin_player_playlist_toggle(gSkinPlayer)) break;
            /* Missing artwork or a small screen uses the full native view. */
            if (gui->plWin) ClosePlaylistWindow(gui); else OpenPlaylistWindow(gui); break;
        case SKIN_PLAYLIST_OPTIONS:
            if (!gui->plWin) OpenPlaylistWindow(gui);
            if (gui->plWin) { WindowToFront(gui->plWin); ActivateWindow(gui->plWin); }
            break;
        case SKIN_VOLUME_SET:
            SetGuiVolume(gui,e.value,e.released,IDCMP_MOUSEMOVE,0); break;
        case SKIN_SEEK:
            if (gSkinAudio.pause_requested || !gSkinAudio.eq_supported || IsRadioInputName(gui->inputName) || gui->totalSecs<=0 || !gui->playbackActive)
                skin_player_notice(gSkinPlayer,"SEEK REQUIRES AN UNPAUSED LOCAL MP3");
            else GuiSeekRelative(gui,(int)((long)gui->totalSecs*e.value/100)-
                                      (gui->elapsedSecs-gui->launchBufferSecs));
            break;
        case SKIN_UNSUPPORTED:
            skin_player_notice(gSkinPlayer,"CONTROL NOT AVAILABLE"); break;
        case SKIN_CLASSIC:
            GuiSkinToClassic(gui); return;
        case SKIN_FILTER: GuiSkinToggleFilter(gui); break;
        case SKIN_INFO: GuiSkinInfo(gui); break;
        case SKIN_STREAM: GuiSkinStream(gui); break;
        case SKIN_SEEK_STEP:
            if (gSkinAudio.pause_requested || !gSkinAudio.eq_supported || IsRadioInputName(gui->inputName) || gui->totalSecs<=0 || !gui->playbackActive)
                skin_player_notice(gSkinPlayer,"SEEK REQUIRES AN UNPAUSED LOCAL MP3");
            else GuiSeekRelative(gui,e.value);
            break;
        }
    }
    if (!gui->closeRequested && gSkinPendingPlay && gSkinPlayer && !gui->playbackActive && !gui->playbackDonePending) {
        struct Gadget gad;
        gSkinPendingPlay=0; memset(&gad,0,sizeof(gad));
        gad.GadgetID=GID_PLAY; HandleGuiAction(gui,&gad,0,IDCMP_GADGETUP,TRUE);
    }
    if (!gui->closeRequested && gSkinPendingTrack>=0 && !gui->playbackActive && !gui->playbackDonePending) {
        int index=gSkinPendingTrack; gSkinPendingTrack=-1;
        if (index<gui->playlist.count) { PlaylistLoadAndShow(gui,index); PlaylistStartCurrent(gui); }
    }
}
static void GuiSkinUpdate(HelixAmp3Gui *gui,int tick)
{
    SkinState state;
    if (!gSkinPlayer) return;
    memset(&state,0,sizeof(state));
    if (IsRadioInputName(gui->inputName))
        CopyVolatileGuiString(state.title,sizeof(state.title),gGuiPlaybackStatus.radioTitle);
    if (gGuiPlaybackStatus.phase==GUIPLAY_PHASE_ERROR)
        SafeCopy(state.title,sizeof(state.title),gui->statusText);
    if (!state.title[0] && IsRadioInputName(gui->inputName))
        CopyVolatileGuiString(state.title,sizeof(state.title),gGuiPlaybackStatus.radioStationName);
    if (!state.title[0] && gui->tags.title[0] && strcmp(gui->tags.title,"-")) {
        if (gui->tags.artist[0] && strcmp(gui->tags.artist,"-"))
            snprintf(state.title,sizeof(state.title),"%s - %s",gui->tags.artist,gui->tags.title);
        else SafeCopy(state.title,sizeof(state.title),gui->tags.title);
    }
    if (!state.title[0]) SafeCopy(state.title,sizeof(state.title),
        gui->inputName[0] ? gui->inputName : "MINTAMP - RIGHT CLICK FOR MENU");
    state.elapsed=gui->elapsedSecs-gui->launchBufferSecs;
    if (gSkinAudio.position_valid && gui->playbackActive) state.elapsed=(int)(gSkinAudio.position_ms/1000UL);
    if (!gui->playbackActive && !gSkinAudio.completed_ok) state.elapsed=0;
    state.total=gui->totalSecs; state.volume=gui->volumePercent;
    state.playing=gui->playbackActive && (gGuiPlaybackStatus.phase==GUIPLAY_PHASE_PLAYING || gGuiPlaybackStatus.phase==GUIPLAY_PHASE_UNDERRUN);
    state.paused=gSkinAudio.paused;
    state.mono=gSkinAudio.channels==1; state.rate=gGuiPlaybackStatus.sampleRate;
    state.bitrate=gSkinAudio.bitrate ? gSkinAudio.bitrate : gGuiPlaybackStatus.radioBitrateKbps;
    state.filter=gui->hardwareFilter;
    snprintf(state.debug_text,sizeof(state.debug_text),"BUF %luMS SPARE %ldMS UNDERRUN %lu",gGuiPlaybackStatus.halfBufferMs,gGuiPlaybackStatus.spareMs,gGuiPlaybackStatus.underruns);
    skin_player_update(gSkinPlayer,&state,tick);
    GuiSkinPlaylistUpdate(gui);
    skin_player_artwork_enabled(gSkinPlayer,gui->artEnabled);
    skin_player_artwork(gSkinPlayer,gui->artRGBBuf,gui->artEnabled && gui->artValid,
        gui->artLoading || gui->artDecode.active,IsRadioInputName(gui->inputName));
    if (skin_player_settings_visible(gSkinPlayer)) GuiSkinSettingsRows(gui);
}
