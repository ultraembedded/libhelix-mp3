#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../playlist_format.h"

typedef struct Collected {
    int count;
    char location[8][PLAYLIST_LOCATION_MAX];
    char title[8][PLAYLIST_TITLE_MAX];
    int stopAfter;
    int durations[8];
} Collected;

static int collect(void *ctx, const char *location, const char *title)
{
    Collected *c = (Collected *)ctx;
    assert(c->count < 8);
    strcpy(c->location[c->count], location);
    strcpy(c->title[c->count], title);
    c->count++;
    return c->stopAfter == 0 || c->count < c->stopAfter;
}

static void collect_duration(void *ctx,int seconds)
{ Collected *c=(Collected *)ctx; assert(c->count>0); c->durations[c->count-1]=seconds; }

static Collected parse(const char *text)
{
    Collected c;
    int n;
    memset(&c, 0, sizeof(c));
    n = playlist_parse(text, strlen(text), collect, &c);
    assert(n == c.count);
    return c;
}

int main(void)
{
    Collected c;
    char buf[1024];
    char file[4096];
    size_t len;
    int format;
    const char *timed="#EXTM3U\n#EXTINF:123,First\na.mp3\n#EXTINF:-1,Stream\nhttp://x\n#EXTINF:99999999999999999,Bad\nb.mp3\nc.mp3\n";
    memset(&c,0,sizeof(c));
    assert(playlist_parse_ex(timed,strlen(timed),collect,&c,collect_duration)==4);
    assert(c.durations[0]==123 && c.durations[1]==-1 && c.durations[2]==-1 && c.durations[3]==-1);
    timed="[playlist]\nLength2=99\nFile2=b\nFile1=a\nLength1=0\nLength3=oops\nFile3=c\n";
    memset(&c,0,sizeof(c)); assert(playlist_parse_ex(timed,strlen(timed),collect,&c,collect_duration)==3);
    assert(c.durations[0]==0 && c.durations[1]==99 && c.durations[2]==-1);
    for (format=0;format<=1;++format) {
        len=playlist_write_header(file,sizeof(file),format);
        len+=playlist_write_entry_ex(file+len,sizeof(file)-len,format,1,"a.mp3","",321);
        len+=playlist_write_footer(file+len,sizeof(file)-len,format,1);
        memset(&c,0,sizeof(c)); assert(playlist_parse_ex(file,len,collect,&c,collect_duration)==1);
        assert(c.durations[0]==321);
    }
    assert(playlist_duration("604800",6)==604800 && playlist_duration("604801",6)==-1);
    assert(playlist_duration("12foo",5)==-1);

    /* Shoutcast-style PLS: CRLF, keys in any order and case, gaps in N. */
    c = parse("[playlist]\r\nnumberofentries=3\r\nTitle2=Backup\r\nFile1=http://s6.reliastream.com:8008/stream\r\n"
              "Title1=(#1 - 12/500) Jazz FM\r\nLength1=-1\r\nfile2= http://backup.example.com/ \r\n"
              "File4=Work:Music/song.mp3\r\nVersion=2\r\n");
    assert(c.count == 3);
    assert(!strcmp(c.location[0], "http://s6.reliastream.com:8008/stream"));
    assert(!strcmp(c.title[0], "(#1 - 12/500) Jazz FM"));
    assert(!strcmp(c.location[1], "http://backup.example.com/"));
    assert(!strcmp(c.title[1], "Backup"));
    assert(!strcmp(c.location[2], "Work:Music/song.mp3"));
    assert(!strcmp(c.title[2], ""));

    /* Extended M3U with titles, comments, blank lines, relative paths, BOM. */
    c = parse("\xef\xbb\xbf#EXTM3U\n#EXTINF:-1,Radio Paradise FLAC\nhttp://stream.radioparadise.com/flac\n"
              "\n# just a comment\nsongs/a.mp3\n#EXTINF:183,Artist - Title\r\nDH0:b.flac\r\n");
    assert(c.count == 3);
    assert(!strcmp(c.location[0], "http://stream.radioparadise.com/flac"));
    assert(!strcmp(c.title[0], "Radio Paradise FLAC"));
    assert(!strcmp(c.location[1], "songs/a.mp3") && !c.title[1][0]);
    assert(!strcmp(c.location[2], "DH0:b.flac"));
    assert(!strcmp(c.title[2], "Artist - Title"));

    /* Plain M3U (the format MintAMP wrote before) still loads. */
    c = parse("#EXTM3U\nWork:a.mp3\nWork:b.mp3");
    assert(c.count == 2 && !strcmp(c.location[1], "Work:b.mp3"));

    /* Stopping early (playlist full). */
    {
        Collected s;
        const char *t = "a\nb\nc\n";
        memset(&s, 0, sizeof(s));
        s.stopAfter = 2;
        playlist_parse(t, strlen(t), collect, &s);
        assert(s.count == 2);
    }

    /* Out-of-range and malformed PLS keys are ignored. */
    c = parse("[playlist]\nFile0=x\nFile999=y\nFileX=z\nFile3=ok\n");
    assert(c.count == 1 && !strcmp(c.location[0], "ok"));

    assert(playlist_format_from_name("Work:Radio.PLS") == PLAYLIST_FORMAT_PLS);
    assert(playlist_format_from_name("list.m3u") == PLAYLIST_FORMAT_M3U);
    assert(playlist_format_from_name("pls") == PLAYLIST_FORMAT_M3U);
    assert(playlist_is_playlist_name("Work:Radio/Jazz.PLS"));
    assert(playlist_is_playlist_name("list.m3u") && playlist_is_playlist_name("live.M3U8"));
    assert(!playlist_is_playlist_name("song.mp3") && !playlist_is_playlist_name("m3u") &&
           !playlist_is_playlist_name(NULL));
    assert(playlist_location_is_url("https://x/y"));
    assert(!playlist_location_is_url("Work:Music/a.mp3"));
    assert(!playlist_location_is_url("DH0:http://x"));

    /* Round trip both formats, including a title with a line break. */
    for (format = PLAYLIST_FORMAT_M3U; format <= PLAYLIST_FORMAT_PLS; format++) {
        file[0] = '\0';
        len = playlist_write_header(buf, sizeof(buf), format);
        assert(len > 0); strcat(file, buf);
        len = playlist_write_entry(buf, sizeof(buf), format, 1, "http://a.example/live", "Station A\nInjected");
        assert(len > 0); strcat(file, buf);
        len = playlist_write_entry(buf, sizeof(buf), format, 2, "Work:Music/b.mp3", "");
        assert(len > 0); strcat(file, buf);
        playlist_write_footer(buf, sizeof(buf), format, 2);
        strcat(file, buf);
        c = parse(file);
        assert(c.count == 2);
        assert(!strcmp(c.location[0], "http://a.example/live"));
        assert(!strcmp(c.title[0], "Station A"));
        assert(!strcmp(c.location[1], "Work:Music/b.mp3") && !c.title[1][0]);
    }
    assert(strstr(file, "NumberOfEntries=2\nVersion=2\n"));

    /* An entry that does not fit reports 0 and leaves an empty string. */
    assert(playlist_write_entry(buf, 8, PLAYLIST_FORMAT_M3U, 1, "http://long.example/", "") == 0);
    assert(buf[0] == '\0');

    printf("playlist_format_test: ok\n");
    return 0;
}
