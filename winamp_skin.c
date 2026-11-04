#include "winamp_skin.h"
#include "lodepng.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static unsigned u16(const unsigned char *p) { return p[0] | ((unsigned)p[1] << 8); }
static unsigned long u32(const unsigned char *p)
{ return (unsigned long)p[0] | ((unsigned long)p[1]<<8) |
         ((unsigned long)p[2]<<16) | ((unsigned long)p[3]<<24); }
static int fail(char *error, size_t n, const char *message)
{ if (error && n) { strncpy(error, message, n-1); error[n-1]=0; } return 0; }
static int span(size_t at, size_t len, size_t size)
{ return at <= size && len <= size-at; }

void skin_free(WinampSkin *skin)
{
    int i;
    for (i=0; i<SKIN_ASSET_COUNT; ++i) free(skin->assets[i].rgb);
    memset(skin, 0, sizeof(*skin));
}

int skin_decode_bmp(SkinBitmap *bmp, const unsigned char *d, size_t n,
                    char *error, size_t en)
{
    unsigned long header, offset, raw_w, raw_h, compression, colors;
    unsigned w, h, bits, x, y, index, topdown;
    size_t stride, at, palette_at;
    unsigned char *rgb, *indices = NULL;
    const unsigned char *palette;
    if (n<54 || d[0]!='B' || d[1]!='M') return fail(error,en,"Not a Windows BMP");
    header=u32(d+14); offset=u32(d+10); raw_w=u32(d+18); raw_h=u32(d+22);
    bits=u16(d+28); compression=u32(d+30); colors=u32(d+46);
    topdown=(raw_h & 0x80000000UL)!=0;
    w=(unsigned)raw_w; h=(unsigned)(topdown ? ((~raw_h+1)&0xffffffffUL) : raw_h);
    if (header<40 || header>124 || !span(14,header,n) || u16(d+26)!=1 ||
        !raw_w || (raw_w & 0x80000000UL) || !h || w>1024 || h>1024 ||
        (unsigned long)w*h*3>SKIN_MAX_BITMAP || offset>n)
        return fail(error,en,"Invalid or oversized BMP dimensions/header");
    if (!(bits==1 || bits==4 || bits==8 || bits==24 || bits==32) ||
        !(compression==0 || (compression==1 && bits==8 && !topdown) ||
          (compression==2 && bits==4 && !topdown)))
        return fail(error,en,"Unsupported BMP encoding (need RGB, RLE8 or RLE4)");
    palette_at=14+header; palette=d+palette_at;
    if (bits<=8) {
        if (!colors) colors=1UL<<bits;
        if (colors>(1UL<<bits) || !span(palette_at,colors*4,n) ||
            offset<palette_at+colors*4) return fail(error,en,"Invalid BMP palette");
    } else if (offset<palette_at) return fail(error,en,"Invalid BMP pixel offset");
    rgb=(unsigned char *)calloc((size_t)w*h,3);
    if (!rgb) return fail(error,en,"Not enough memory for skin bitmap");
    if (compression) {
        int ended=0;
        indices=(unsigned char *)calloc((size_t)w*h,1);
        if (!indices) { free(rgb); return fail(error,en,"Not enough memory for BMP RLE"); }
        at=offset; x=0; y=0;
        while (span(at,2,n)) {
            unsigned count=d[at++], value=d[at++], i;
            if (count) {
                if (y>=h || count>w-x) goto bad_rle;
                for (i=0;i<count;++i)
                    indices[(size_t)y*w+x++]=(unsigned char)(bits==8 ? value :
                        ((i&1) ? value&15 : value>>4));
            } else if (value==0) { x=0; if (++y>h) goto bad_rle; }
            else if (value==1) { ended=1; break; }
            else if (value==2) {
                if (!span(at,2,n) || d[at]>w-x || d[at+1]>=h-y) goto bad_rle;
                x+=d[at++]; y+=d[at++];
            } else {
                size_t bytes=(bits==8 ? value : (value+1)/2), padded=(bytes+1)&~(size_t)1;
                if (y>=h || value>w-x || !span(at,padded,n)) goto bad_rle;
                for (i=0;i<value;++i)
                    indices[(size_t)y*w+x++]=(unsigned char)(bits==8 ? d[at+i] :
                        ((i&1) ? d[at+i/2]&15 : d[at+i/2]>>4));
                at+=padded;
            }
        }
        if (!ended) goto bad_rle;
    }
    stride=(((size_t)w*bits+31)/32)*4;
    if (!compression && !span(offset,stride*h,n)) {
        free(rgb); return fail(error,en,"Truncated BMP pixels");
    }
    for (y=0;y<h;++y) for (x=0;x<w;++x) {
        unsigned source_y=topdown ? y : h-1-y;
        unsigned char *out=rgb+((size_t)y*w+x)*3;
        const unsigned char *row=d+offset;
        if (!compression) row+=stride*source_y;
        if (bits<=8) {
            index=compression ? indices[(size_t)source_y*w+x] :
                (bits==8 ? row[x] : bits==4 ? ((x&1) ? row[x/2]&15 : row[x/2]>>4) :
                 (row[x/8]>>(7-x%8))&1);
            if (index>=colors) {
                free(indices); free(rgb); return fail(error,en,"BMP palette index out of range");
            }
            out[0]=palette[index*4+2]; out[1]=palette[index*4+1]; out[2]=palette[index*4];
        } else {
            row+=x*(bits/8); out[0]=row[2]; out[1]=row[1]; out[2]=row[0];
        }
    }
    free(indices); free(bmp->rgb); bmp->rgb=rgb; bmp->width=w; bmp->height=h;
    return 1;
bad_rle:
    free(indices); free(rgb); return fail(error,en,"Truncated or invalid BMP RLE stream");
}

static const char * const names[SKIN_VISBAR+2] = {
    "main.bmp", "cbuttons.bmp", "titlebar.bmp", "numbers.bmp", "text.bmp",
    "volume.bmp", "balance.bmp", "posbar.bmp", "playpaus.bmp", "monoster.bmp", "shufrep.bmp",
    "pledit.bmp", "eqmain.bmp", "eq_ex.bmp", "mb.bmp", "pledit.txt", "viscolor.txt"
};
static const unsigned min_w[SKIN_VISBAR]={275,136,302,90,155,68,47,277,27,56,46,276,275,275,178};
static const unsigned min_h[SKIN_VISBAR]={116,36,29,13,12,433,433,10,9,24,73,110,315,29,119};

static int asset_id(const unsigned char *name, unsigned len)
{
    unsigned start=0, k; int i;
    for (k=0;k<len;++k) if (name[k]=='/' || name[k]=='\\') start=k+1;
    for (i=0;i<SKIN_VISBAR+2;++i) {
        size_t n=strlen(names[i]);
        if (len-start!=n) continue;
        for (k=0;k<n;++k) if (tolower((unsigned char)name[start+k])!=names[i][k]) break;
        if (k==n) return i;
    }
    return -1;
}

/* Read only bounded colour values in [Text]. Windows font names are not
 * filesystem paths; the native view uses its small Amiga font instead. */
static void playlist_colors(WinampSkin *skin,const unsigned char *data,size_t size)
{
    size_t at=0; int text_section=0;
    while (at<size) {
        char line[128]; size_t n=0; char *key,*value,*end; unsigned rgb=0; int i;
        while (at<size && data[at]!='\n') {
            if (n<sizeof(line)-1) line[n++]=(char)tolower(data[at]);
            ++at;
        }
        if (at<size) ++at;
        line[n]=0; key=line;
        while (*key && isspace((unsigned char)*key)) ++key;
        end=key+strlen(key);
        while (end>key && isspace((unsigned char)end[-1])) *--end=0;
        if (*key=='[') { text_section=!strcmp(key,"[text]"); continue; }
        if (!text_section || !(value=strchr(key,'='))) continue;
        *value++=0; end=key+strlen(key);
        while (end>key && isspace((unsigned char)end[-1])) *--end=0;
        while (*value && isspace((unsigned char)*value)) ++value;
        if (*value++!='#' || strlen(value)!=6) continue;
        for (i=0;i<6;++i) {
            int c=(unsigned char)value[i];
            if (!isxdigit(c)) break;
            rgb=(rgb<<4)|(unsigned)(c<='9' ? c-'0' : c-'a'+10);
        }
        if (i!=6) continue;
        if (!strcmp(key,"normal")) skin->playlist_normal=rgb;
        else if (!strcmp(key,"current")) skin->playlist_current=rgb;
        else if (!strcmp(key,"normalbg")) skin->playlist_background=rgb;
        else if (!strcmp(key,"selectedbg")) skin->playlist_selected=rgb;
        else if (!strcmp(key,"mbbg")) skin->mb_background=rgb;
        else if (!strcmp(key,"mbfg")) skin->mb_text=rgb;
    }
}

/* Classic 24 RGB rows, optional comments. Malformed palettes keep defaults. */
static void visual_colors(WinampSkin *skin,const unsigned char *data,size_t size)
{
    unsigned colors[24]; size_t at=0; int count=0;
    while (at<size && count<24) {
        char line[160],*p,*end; size_t n=0; int i; unsigned rgb=0;
        while (at<size && data[at]!='\n') { if (n<sizeof(line)-1) line[n++]=(char)data[at]; ++at; }
        if (at<size) ++at;
        line[n]=0; p=line;
        while (isspace((unsigned char)*p)) ++p;
        if (!*p || *p==';' || *p=='/' || *p=='#') continue;
        for (i=0;i<3;++i) {
            long v=strtol(p,&end,10);
            if (end==p || v<0 || v>255) return;
            rgb=(rgb<<8)|(unsigned)v; p=end;
            while (isspace((unsigned char)*p)) ++p;
            if (i<2) { if (*p++!=',') return; }
        }
        colors[count++]=rgb;
    }
    if (count==24) memcpy(skin->visual_colors,colors,sizeof(colors));
}
int skin_load_memory(WinampSkin *skin, const unsigned char *z, size_t size,
                     char *error, size_t en)
{
    WinampSkin tmp; size_t eocd, cd, cd_end, at; unsigned entries, i;
    int found=0, have_colors[2]={0,0}; unsigned long cd_len, total_rgb=0;
    memset(&tmp,0,sizeof(tmp));
    tmp.playlist_normal=0x00ff00; tmp.playlist_current=0xffffff;
    tmp.playlist_selected=0x0000c6;
    tmp.mb_background=0x000000; tmp.mb_text=0x00d000;
    for (i=0;i<16;++i) tmp.visual_colors[i+2]=((unsigned)(i*12)<<16)|((unsigned)(255-i*8)<<8);
    for (i=18;i<23;++i) tmp.visual_colors[i]=0x00cc00;
    tmp.visual_colors[23]=0xffffff;
    if (!z || size<22 || size>SKIN_MAX_FILE) return fail(error,en,"Invalid or oversized skin archive");
    eocd=size-22;
    for (;;) {
        if (u32(z+eocd)==0x06054b50UL && span(eocd,22,size) &&
            eocd+22+u16(z+eocd+20)==size) { found=1; break; }
        if (!eocd || size-eocd>65557) break;
        --eocd;
    }
    if (!found) return fail(error,en,"Not a classic WSZ/ZIP skin archive");
    entries=u16(z+eocd+10); cd_len=u32(z+eocd+12); cd=u32(z+eocd+16);
    if (u16(z+eocd+4) || u16(z+eocd+6) || entries!=u16(z+eocd+8) ||
        entries>512 || !span(cd,cd_len,eocd)) return fail(error,en,"Unsupported multi-disk/ZIP64 or invalid archive");
    cd_end=cd+cd_len; at=cd;
    for (i=0;i<entries;++i) {
        unsigned len, extra, comment, method, flags; int id;
        size_t local, payload, packed, unpacked, out_size=0;
        unsigned char *out=NULL; const unsigned char *bmp;
        unsigned long crc;
        if (!span(at,46,cd_end) || u32(z+at)!=0x02014b50UL) goto invalid_zip;
        len=u16(z+at+28); extra=u16(z+at+30); comment=u16(z+at+32);
        if (!span(at+46,(size_t)len+extra+comment,cd_end)) goto invalid_zip;
        id=asset_id(z+at+46,len);
        if (id<0) { at+=46+len+extra+comment; continue; }
        if (id>=SKIN_VISBAR ? have_colors[id-SKIN_VISBAR] : tmp.assets[id].rgb!=NULL) {
            fail(error,en,"Duplicate skin asset names"); goto failed;
        }
        flags=u16(z+at+8); method=u16(z+at+10); crc=u32(z+at+16);
        packed=u32(z+at+20); unpacked=u32(z+at+24); local=u32(z+at+42);
        if ((flags&1) || (method!=0 && method!=8) || !unpacked ||
            unpacked>(id>=SKIN_VISBAR ? 8192UL : SKIN_MAX_BITMAP) || !span(local,30,cd) || u32(z+local)!=0x04034b50UL ||
            u16(z+local+8)!=method || u16(z+local+6)!=flags) goto invalid_zip;
        payload=local+30+u16(z+local+26)+u16(z+local+28);
        if (!span(payload,packed,cd)) goto invalid_zip;
        if (method==8) {
            LodePNGDecompressSettings settings;
            unsigned rc;
            lodepng_decompress_settings_init(&settings); settings.max_output_size=unpacked;
            rc=lodepng_inflate(&out,&out_size,z+payload,packed,&settings);
            if (rc || out_size!=unpacked) {
                free(out); fail(error,en,"Invalid or oversized deflated skin bitmap"); goto failed;
            }
            bmp=out;
        } else {
            if (packed!=unpacked) goto invalid_zip;
            bmp=z+payload;
        }
        if (lodepng_crc32(bmp,unpacked)!=crc) {
            free(out); fail(error,en,"Skin bitmap CRC check failed"); goto failed;
        }
        if (id>=SKIN_VISBAR) {
            if (id==SKIN_VISBAR) playlist_colors(&tmp,bmp,unpacked);
            else visual_colors(&tmp,bmp,unpacked);
            have_colors[id-SKIN_VISBAR]=1; free(out);
            at+=46+len+extra+comment; continue;
        }
        if (!skin_decode_bmp(&tmp.assets[id],bmp,unpacked,error,en)) { free(out); goto failed; }
        free(out);
        total_rgb+=(unsigned long)tmp.assets[id].width*tmp.assets[id].height*3;
        if (total_rgb>SKIN_MAX_TOTAL) {
            fail(error,en,"Skin bitmaps exceed the total memory limit"); goto failed;
        }
        if (id==SKIN_MB && (tmp.assets[id].width<min_w[id] || tmp.assets[id].height<min_h[id])) {
            /* Rarely used by classic skins: an odd sheet only loses the frame. */
            total_rgb-=(unsigned long)tmp.assets[id].width*tmp.assets[id].height*3;
            free(tmp.assets[id].rgb); memset(&tmp.assets[id],0,sizeof(tmp.assets[id]));
        } else if (tmp.assets[id].width<min_w[id] || tmp.assets[id].height<min_h[id]) {
            if (error && en) snprintf(error,en,"Skin bitmap too small: %s",names[id]);
            goto failed;
        }
        at+=46+len+extra+comment;
    }
    if (!tmp.assets[SKIN_MAIN].rgb || !tmp.assets[SKIN_BUTTONS].rgb || !tmp.assets[SKIN_TEXT].rgb) {
        fail(error,en,"Skin needs MAIN.BMP, CBUTTONS.BMP and TEXT.BMP"); goto failed;
    }
    /* Cache one three-pixel gradient strip with the other native bitmaps.
     * Each spectrum bin needs one clipped blit, not sixteen colour fills. */
    if (total_rgb>SKIN_MAX_TOTAL-144) { fail(error,en,"Skin bitmaps exceed the total memory limit"); goto failed; }
    tmp.assets[SKIN_VISBAR].rgb=(unsigned char *)malloc(3*16*3);
    if (!tmp.assets[SKIN_VISBAR].rgb) { fail(error,en,"Not enough memory for visualiser colours"); goto failed; }
    tmp.assets[SKIN_VISBAR].width=3; tmp.assets[SKIN_VISBAR].height=16;
    for (i=0;i<16;++i) {
        int x; unsigned rgb=tmp.visual_colors[2+15-i];
        for (x=0;x<3;++x) {
            unsigned char *pixel=tmp.assets[SKIN_VISBAR].rgb+(i*3+x)*3;
            pixel[0]=(unsigned char)(rgb>>16); pixel[1]=(unsigned char)(rgb>>8); pixel[2]=(unsigned char)rgb;
        }
    }
    skin_free(skin); *skin=tmp; if (error && en) error[0]=0; return 1;
invalid_zip:
    fail(error,en,"Invalid or unsupported ZIP entry");
failed:
    skin_free(&tmp); return 0;
}

int skin_load_file(WinampSkin *skin, const char *path, char *error, size_t en)
{
    FILE *f=fopen(path,"rb"); long length; unsigned char *data; int ok;
    if (!f) return fail(error,en,"Cannot open skin file");
    if (fseek(f,0,SEEK_END) || (length=ftell(f))<22 || (unsigned long)length>SKIN_MAX_FILE ||
        fseek(f,0,SEEK_SET)) { fclose(f); return fail(error,en,"Invalid or oversized skin file"); }
    data=(unsigned char *)malloc((size_t)length);
    if (!data) { fclose(f); return fail(error,en,"Not enough memory for skin archive"); }
    if (fread(data,1,(size_t)length,f)!=(size_t)length) {
        free(data); fclose(f); return fail(error,en,"Could not read skin archive");
    }
    fclose(f); ok=skin_load_memory(skin,data,(size_t)length,error,en); free(data); return ok;
}
