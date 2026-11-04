#ifndef PLAYLIST_FORMAT_H
#define PLAYLIST_FORMAT_H

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* Reading and writing the two playlist formats people share stations and
 * track lists in, for both GUI editions:
 *
 *   M3U   "#EXTM3U", then per entry an optional "#EXTINF:<secs>,<title>"
 *         line followed by the location line.
 *   PLS   "[playlist]", then "FileN=<location>" with optional "TitleN=" and
 *         "LengthN=", and "NumberOfEntries=" / "Version=2".
 *
 * A location is a URL or an Amiga path, exactly as written; resolving a
 * relative path against the playlist's drawer is left to the caller.  The
 * parser works on the whole file in memory and never writes to it. */

#define PLAYLIST_FORMAT_M3U 0
#define PLAYLIST_FORMAT_PLS 1

#define PLAYLIST_PLS_MAX_ENTRIES 256  /* highest FileN read from a .pls */
#define PLAYLIST_LOCATION_MAX    512
#define PLAYLIST_TITLE_MAX       128

/* Called once per entry, in playlist order; return 0 to stop early. */
typedef int (*PlaylistEntryFn)(void *ctx, const char *location, const char *title);
/* Optional metadata delivered immediately after its entry callback. Unknown
 * or invalid durations are -1; bounded to seven days per entry. */
typedef void (*PlaylistDurationFn)(void *ctx,int seconds);
static int playlist_duration(const char *text,size_t len)
{
    int seconds=0; size_t i=0;
    while (i<len && (text[i]==' ' || text[i]=='\t')) ++i;
    if (i==len || text[i]<'0' || text[i]>'9') return -1;
    while (i<len && text[i]>='0' && text[i]<='9') {
        if (seconds>604800/10) return -1;
        seconds=seconds*10+text[i++]-'0'; if (seconds>604800) return -1;
    }
    while (i<len && (text[i]==' ' || text[i]=='\t')) ++i;
    return i==len ? seconds : -1;
}

static int playlist_ascii_starts_nocase(const char *s, size_t len, const char *prefix)
{
    size_t n = strlen(prefix);
    size_t i;

    if (len < n) return 0;
    for (i = 0; i < n; i++) {
        char a = s[i];
        char b = prefix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return 0;
    }
    return 1;
}

/* PLAYLIST_FORMAT_PLS for a name ending ".pls" (any case), else M3U. */
static int playlist_format_from_name(const char *name)
{
    size_t n = name ? strlen(name) : 0;
    return n >= 4 && playlist_ascii_starts_nocase(name + n - 4, 4, ".pls") ?
        PLAYLIST_FORMAT_PLS : PLAYLIST_FORMAT_M3U;
}

/* True for a name ending ".m3u", ".m3u8" or ".pls" (any case). */
static int playlist_is_playlist_name(const char *name)
{
    size_t n = name ? strlen(name) : 0;
    return (n >= 4 && (playlist_ascii_starts_nocase(name + n - 4, 4, ".m3u") ||
                       playlist_ascii_starts_nocase(name + n - 4, 4, ".pls"))) ||
           (n >= 5 && playlist_ascii_starts_nocase(name + n - 5, 5, ".m3u8"));
}

/* Copies text[0..len) with surrounding blanks removed into out. */
static void playlist_copy_trimmed(char *out, size_t outSize, const char *text, size_t len)
{
    while (len > 0 && (*text == ' ' || *text == '\t')) { text++; len--; }
    while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t')) len--;
    if (len >= outSize) len = outSize - 1;
    memcpy(out, text, len);
    out[len] = '\0';
}

/* Advances *pos past the next line of text; returns its start and length
 * (without CR/LF), or NULL at the end. */
static const char *playlist_next_line(const char *text, size_t len, size_t *pos, size_t *lineLen)
{
    size_t start = *pos;
    size_t end;

    if (start >= len) return NULL;
    end = start;
    while (end < len && text[end] != '\n' && text[end] != '\r') end++;
    *lineLen = end - start;
    while (end < len && (text[end] == '\n' || text[end] == '\r')) end++;
    *pos = end;
    return text + start;
}

static int playlist_parse_pls(const char *text, size_t len, PlaylistEntryFn fn, void *ctx,PlaylistDurationFn duration)
{
    /* Static: kept off the small GUI task stack; parsing is never nested. */
    static const char *file[PLAYLIST_PLS_MAX_ENTRIES + 1];
    static size_t fileLen[PLAYLIST_PLS_MAX_ENTRIES + 1];
    static const char *title[PLAYLIST_PLS_MAX_ENTRIES + 1];
    static size_t titleLen[PLAYLIST_PLS_MAX_ENTRIES + 1];
    static int lengths[PLAYLIST_PLS_MAX_ENTRIES + 1];
    char location[PLAYLIST_LOCATION_MAX];
    char name[PLAYLIST_TITLE_MAX];
    const char *line;
    size_t pos = 0, lineLen;
    int maxN = 0, count = 0, n;

    memset(file, 0, sizeof(file));
    memset(title, 0, sizeof(title));
    for (n=0;n<=PLAYLIST_PLS_MAX_ENTRIES;++n) lengths[n]=-1;
    while ((line = playlist_next_line(text, len, &pos, &lineLen)) != NULL) {
        int isFile = playlist_ascii_starts_nocase(line, lineLen, "File");
        int isTitle = playlist_ascii_starts_nocase(line, lineLen, "Title");
        int isLength = playlist_ascii_starts_nocase(line,lineLen,"Length");
        size_t k;
        if (!isFile && !isTitle && !isLength) continue;
        k = isFile ? 4 : isTitle ? 5 : 6;
        n = 0;
        while (k < lineLen && line[k] >= '0' && line[k] <= '9' && n <= PLAYLIST_PLS_MAX_ENTRIES)
            n = n * 10 + (line[k++] - '0');
        if (n < 1 || n > PLAYLIST_PLS_MAX_ENTRIES || k >= lineLen || line[k] != '=') continue;
        k++;
        if (isFile) { file[n] = line + k; fileLen[n] = lineLen - k; if (n > maxN) maxN = n; }
        else if (isTitle) { title[n] = line + k; titleLen[n] = lineLen - k; }
        else lengths[n]=playlist_duration(line+k,lineLen-k);
    }
    for (n = 1; n <= maxN; n++) {
        if (!file[n]) continue;
        playlist_copy_trimmed(location, sizeof(location), file[n], fileLen[n]);
        if (!location[0]) continue;
        name[0] = '\0';
        if (title[n]) playlist_copy_trimmed(name, sizeof(name), title[n], titleLen[n]);
        count++;
        { int proceed=fn(ctx,location,name);
          if (duration) duration(ctx,lengths[n]);
          if (!proceed) break; }
    }
    return count;
}

static int playlist_parse_m3u(const char *text, size_t len, PlaylistEntryFn fn, void *ctx,PlaylistDurationFn duration)
{
    char location[PLAYLIST_LOCATION_MAX];
    char name[PLAYLIST_TITLE_MAX];
    const char *line;
    size_t pos = 0, lineLen;
    int count = 0,seconds=-1;

    name[0] = '\0';
    while ((line = playlist_next_line(text, len, &pos, &lineLen)) != NULL) {
        playlist_copy_trimmed(location, sizeof(location), line, lineLen);
        if (!location[0]) continue;
        if (location[0] == '#') {
            /* "#EXTINF:<seconds>,<title>" names the entry that follows. */
            if (playlist_ascii_starts_nocase(location, strlen(location), "#EXTINF:")) {
                const char *comma = strchr(location, ',');
                name[0] = '\0';
                seconds=comma ? playlist_duration(location+8,(size_t)(comma-location-8)) : -1;
                if (comma) playlist_copy_trimmed(name, sizeof(name), comma + 1, strlen(comma + 1));
            }
            continue;
        }
        count++;
        { int proceed=fn(ctx,location,name);
          if (duration) duration(ctx,seconds);
          if (!proceed) break; }
        seconds=-1;
        name[0] = '\0';
    }
    return count;
}

/* Parses an M3U/M3U8 or PLS playlist held in text[0..len), telling the two
 * apart by content. Returns the number of entries passed to fn. */
static int playlist_parse_ex(const char *text, size_t len, PlaylistEntryFn fn, void *ctx,PlaylistDurationFn duration)
{
    const char *line;
    size_t pos = 0, lineLen;

    if (!text || !fn) return 0;
    if (len >= 3 && (unsigned char)text[0] == 0xef && (unsigned char)text[1] == 0xbb &&
        (unsigned char)text[2] == 0xbf) {
        text += 3;
        len -= 3;
    }
    while ((line = playlist_next_line(text, len, &pos, &lineLen)) != NULL) {
        size_t k = 0;
        while (k < lineLen && (line[k] == ' ' || line[k] == '\t')) k++;
        if (k == lineLen) continue;
        if (playlist_ascii_starts_nocase(line + k, lineLen - k, "[playlist]"))
            return playlist_parse_pls(text, len, fn, ctx,duration);
        break;
    }
    return playlist_parse_m3u(text, len, fn, ctx,duration);
}

static int playlist_parse(const char *text,size_t len,PlaylistEntryFn fn,void *ctx)
{ return playlist_parse_ex(text,len,fn,ctx,NULL); }

/* Writers: each formats one piece of the file into out (always
 * NUL-terminated) and returns its length, or 0 if it does not fit. A file is
 * header, then entries numbered from 1, then footer. */
static size_t playlist_fit(char *out, size_t outSize, int n)
{
    if (n < 0 || (size_t)n >= outSize) {
        if (outSize) out[0] = '\0';
        return 0;
    }
    return (size_t)n;
}

static size_t playlist_write_header(char *out, size_t outSize, int format)
{
    return playlist_fit(out, outSize, snprintf(out, outSize, "%s\n",
        format == PLAYLIST_FORMAT_PLS ? "[playlist]" : "#EXTM3U"));
}

/* Line breaks inside a title would start a bogus entry; titles here come
 * from station names and file names, so they are simply cut there. */
static size_t playlist_write_entry_ex(char *out, size_t outSize, int format, int number,
                                   const char *location, const char *title,int seconds)
{
    char safeTitle[PLAYLIST_TITLE_MAX];
    if (seconds<0 || seconds>604800) seconds=-1;

    playlist_copy_trimmed(safeTitle, sizeof(safeTitle), title ? title : "",
        title ? strcspn(title, "\r\n") : 0);
    if (format == PLAYLIST_FORMAT_PLS) {
        if (safeTitle[0])
            return playlist_fit(out, outSize, snprintf(out, outSize,
                "File%d=%s\nTitle%d=%s\nLength%d=%d\n", number, location, number, safeTitle, number,seconds));
        return playlist_fit(out, outSize, snprintf(out, outSize,
            "File%d=%s\nLength%d=%d\n", number, location, number,seconds));
    }
    if (safeTitle[0] || seconds>=0)
        return playlist_fit(out, outSize, snprintf(out, outSize, "#EXTINF:%d,%s\n%s\n", seconds,safeTitle, location));
    return playlist_fit(out, outSize, snprintf(out, outSize, "%s\n", location));
}

static size_t playlist_write_entry(char *out,size_t size,int format,int number,const char *location,const char *title)
{ return playlist_write_entry_ex(out,size,format,number,location,title,-1); }
static size_t playlist_write_footer(char *out, size_t outSize, int format, int count)
{
    if (format != PLAYLIST_FORMAT_PLS) {
        if (outSize) out[0] = '\0';
        return 0;
    }
    return playlist_fit(out, outSize, snprintf(out, outSize,
        "NumberOfEntries=%d\nVersion=2\n", count));
}

/* True for a location that is a URL ("scheme://...") rather than a path. */
static int playlist_location_is_url(const char *location)
{
    const char *p = location;
    if (!p || !((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z'))) return 0;
    while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
           (*p >= '0' && *p <= '9') || *p == '+' || *p == '-' || *p == '.')
        p++;
    return p[0] == ':' && p[1] == '/' && p[2] == '/';
}

#endif
