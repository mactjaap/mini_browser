#include "badgevms/wifi.h"
#include "curl/curl.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>

/* ---------- Limits & layout ---------- */
#define MAX_BYTES     (16 * 1024)
#define TIMEOUT_S     10
#define URL_MAX       256
#define PAD_LR        10
#define PAD_TOP       34
#define PAD_BOTTOM    10
#define VIEW_W        716
#define VIEW_H        645
#define URLBAR_H      24
#define LINE_SPACING  2
#define MAX_LINKS     128

/* ---------- 5x7 bitmap font (ASCII 32..127) ---------- */
static const unsigned char font5x7[96][5] = {
/* SP */ {0,0,0,0,0},      {/*!*/0x00,0x00,0x5F,0x00,0x00},   {/*"*/0x00,0x07,0x00,0x07,0x00},
/* #  */ {0x14,0x7F,0x14,0x7F,0x14},                           /* $ */ {0x24,0x2A,0x7F,0x2A,0x12},
/* %  */ {0x23,0x13,0x08,0x64,0x62},                           /* & */ {0x36,0x49,0x55,0x22,0x50},
/* '  */ {0x00,0x05,0x03,0x00,0x00},                           /* ( */ {0x00,0x1C,0x22,0x41,0x00},
/* )  */ {0x00,0x41,0x22,0x1C,0x00},                           /* * */ {0x14,0x08,0x3E,0x08,0x14},
/* +  */ {0x08,0x08,0x3E,0x08,0x08},                           /* , */ {0x00,0x50,0x30,0x00,0x00},
/* -  */ {0x08,0x08,0x08,0x08,0x08},                           /* . */ {0x00,0x60,0x60,0x00,0x00},
/* /  */ {0x20,0x10,0x08,0x04,0x02},                           /* 0 */ {0x3E,0x51,0x49,0x45,0x3E},
/* 1  */ {0x00,0x42,0x7F,0x40,0x00},                           /* 2 */ {0x42,0x61,0x51,0x49,0x46},
/* 3  */ {0x21,0x41,0x45,0x4B,0x31},                           /* 4 */ {0x18,0x14,0x12,0x7F,0x10},
/* 5  */ {0x27,0x45,0x45,0x45,0x39},                           /* 6 */ {0x3C,0x4A,0x49,0x49,0x30},
/* 7  */ {0x01,0x71,0x09,0x05,0x03},                           /* 8 */ {0x36,0x49,0x49,0x49,0x36},
/* 9  */ {0x06,0x49,0x49,0x29,0x1E},                           /* : */ {0x00,0x36,0x36,0x00,0x00},
/* ;  */ {0x00,0x56,0x36,0x00,0x00},                           /* < */ {0x08,0x14,0x22,0x41,0x00},
/* =  */ {0x14,0x14,0x14,0x14,0x14},                           /* > */ {0x00,0x41,0x22,0x14,0x08},
/* ?  */ {0x02,0x01,0x51,0x09,0x06},                           /* @ */ {0x32,0x49,0x79,0x41,0x3E},
/* A  */ {0x7E,0x11,0x11,0x11,0x7E},                           /* B */ {0x7F,0x49,0x49,0x49,0x36},
/* C  */ {0x3E,0x41,0x41,0x41,0x22},                           /* D */ {0x7F,0x41,0x41,0x22,0x1C},
/* E  */ {0x7F,0x49,0x49,0x49,0x41},                           /* F */ {0x7F,0x09,0x09,0x09,0x01},
/* G  */ {0x3E,0x41,0x49,0x49,0x7A},                           /* H */ {0x7F,0x08,0x08,0x08,0x7F},
/* I  */ {0x00,0x41,0x7F,0x41,0x00},                           /* J */ {0x20,0x40,0x41,0x3F,0x01},
/* K  */ {0x7F,0x08,0x14,0x22,0x41},                           /* L */ {0x7F,0x40,0x40,0x40,0x40},
/* M  */ {0x7F,0x02,0x0C,0x02,0x7F},                           /* N */ {0x7F,0x04,0x08,0x10,0x7F},
/* O  */ {0x3E,0x41,0x41,0x41,0x3E},                           /* P */ {0x7F,0x09,0x09,0x09,0x06},
/* Q  */ {0x3E,0x41,0x51,0x21,0x5E},                           /* R */ {0x7F,0x09,0x19,0x29,0x46},
/* S  */ {0x46,0x49,0x49,0x49,0x31},                           /* T */ {0x01,0x01,0x7F,0x01,0x01},
/* U  */ {0x3F,0x40,0x40,0x40,0x3F},                           /* V */ {0x1F,0x20,0x40,0x20,0x1F},
/* W  */ {0x7F,0x20,0x18,0x20,0x7F},                           /* X */ {0x63,0x14,0x08,0x14,0x63},
/* Y  */ {0x07,0x08,0x70,0x08,0x07},                           /* Z */ {0x61,0x51,0x49,0x45,0x43},
/* [  */ {0x00,0x7F,0x41,0x41,0x00},                           /* \ */ {0x02,0x04,0x08,0x10,0x20},
/* ]  */ {0x00,0x41,0x41,0x7F,0x00},                           /* ^ */ {0x04,0x02,0x01,0x02,0x04},
/* _  */ {0x40,0x40,0x40,0x40,0x40},                           /* ` */ {0x00,0x01,0x02,0x04,0x00},
/* a  */ {0x20,0x54,0x54,0x54,0x78},                           /* b */ {0x7F,0x48,0x44,0x44,0x38},
/* c  */ {0x38,0x44,0x44,0x44,0x20},                           /* d */ {0x38,0x44,0x44,0x48,0x7F},
/* e  */ {0x38,0x54,0x54,0x54,0x18},                           /* f */ {0x08,0x7E,0x09,0x01,0x02},
/* g  */ {0x0C,0x52,0x52,0x52,0x3E},                           /* h */ {0x7F,0x08,0x04,0x04,0x78},
/* i  */ {0x00,0x44,0x7D,0x40,0x00},                           /* j */ {0x20,0x40,0x44,0x3D,0x00},
/* k  */ {0x7F,0x10,0x28,0x44,0x00},                           /* l */ {0x00,0x41,0x7F,0x40,0x00},
/* m  */ {0x7C,0x04,0x18,0x04,0x78},                           /* n */ {0x7C,0x08,0x04,0x04,0x78},
/* o  */ {0x38,0x44,0x44,0x44,0x38},                           /* p */ {0x7C,0x14,0x14,0x14,0x08},
/* q  */ {0x08,0x14,0x14,0x14,0x7C},                           /* r */ {0x7C,0x08,0x04,0x04,0x08},
/* s  */ {0x48,0x54,0x54,0x54,0x20},                           /* t */ {0x04,0x3F,0x44,0x40,0x20},
/* u  */ {0x3C,0x40,0x40,0x20,0x7C},                           /* v */ {0x1C,0x20,0x40,0x20,0x1C},
/* w  */ {0x3C,0x40,0x30,0x40,0x3C},                           /* x */ {0x44,0x28,0x10,0x28,0x44},
/* y  */ {0x0C,0x50,0x50,0x50,0x3C},                           /* z */ {0x44,0x64,0x54,0x4C,0x44},
/* {  */ {0x00,0x08,0x36,0x41,0x00},                           /* | */ {0x00,0x00,0x7F,0x00,0x00},
/* }  */ {0x00,0x41,0x36,0x08,0x00},                           /* ~ */ {0x08,0x04,0x08,0x10,0x08}
};

#define FONT_W_COLS 5
#define FONT_H_ROWS 7
#define FONT_COL_GAP 1
#define FONT_SCALE  2
#define CH_W ((FONT_W_COLS + FONT_COL_GAP) * FONT_SCALE)
#define CH_H ((FONT_H_ROWS) * FONT_SCALE)

/* --------- curl memory sink --------- */
typedef struct { char *buf; size_t len; } mem_t;
static size_t wr_cb(void *ptr, size_t sz, size_t nm, void *ud) {
    size_t n = sz * nm, keep = n;
    mem_t *m = (mem_t*)ud;
    if (m->len >= MAX_BYTES) return n;
    if (m->len + keep > MAX_BYTES) keep = MAX_BYTES - m->len;
    char *p = (char*)realloc(m->buf, m->len + keep + 1);
    if (!p) return 0;
    m->buf = p;
    memcpy(m->buf + m->len, ptr, keep);
    m->len += keep;
    m->buf[m->len] = 0;
    return n;
}

/* ---------- link + page model ---------- */
typedef struct { char href[URL_MAX]; } link_t;
typedef struct {
    char *text;            /* rendered text with [n] markers */
    link_t links[MAX_LINKS];
    int link_count;
} page_t;

/* ---------- tiny text renderer ---------- */
static void draw_char(SDL_Renderer *r, int x, int y, char c) {
    if (!r) return;
    if (c < 32 || c > 127) c = '?';
    const unsigned char *cols = font5x7[c - 32];
    for (int col = 0; col < FONT_W_COLS; col++) {
        unsigned char bits = cols[col];
        for (int row = 0; row < FONT_H_ROWS; row++) {
            if (bits & (1u << row)) {
                SDL_FRect px = { (float)(x + col*FONT_SCALE), (float)(y + row*FONT_SCALE),
                                 (float)FONT_SCALE, (float)FONT_SCALE };
                SDL_RenderFillRect(r, &px);
            }
        }
    }
}
static void draw_text(SDL_Renderer *r, int x, int y, const char *s, int max_w) {
    if (!r || !s) return;
    int cx = x, cy = y;
    while (*s) {
        if (*s == '\n') { cx = x; cy += (CH_H + LINE_SPACING); s++; continue; }
        if (cx + CH_W > x + max_w) { cx = x; cy += (CH_H + LINE_SPACING); }
        draw_char(r, cx, cy, *s++);
        cx += CH_W;
    }
}

/* ---------- helpers: base URL / resolution ---------- */
static void get_scheme_host(const char *url, char *out, size_t cap) {
    /* scheme://host[:port] */
    const char *p = strstr(url, "://");
    if (!p) { out[0]=0; return; }
    p += 3;
    const char *slash = strchr(p, '/');
    size_t n = slash ? (size_t)(slash - url) : strlen(url);
    if (n >= cap) n = cap - 1;
    memcpy(out, url, n); out[n]=0;
}
static void get_dir(const char *url, char *out, size_t cap) {
    const char *p = strrchr(url, '/');
    if (!p) { out[0]=0; return; }
    size_t n = (size_t)(p - url) + 1;
    if (n >= cap) n = cap - 1;
    memcpy(out, url, n); out[n]=0;
}
static void resolve_url(const char *base, const char *href, char *out, size_t cap) {
    if (!href || !*href) { out[0]=0; return; }
    if (strstr(href, "://")) { strncpy(out, href, cap); out[cap-1]=0; return; }
    if (href[0] == '/') {
        char origin[URL_MAX]; get_scheme_host(base, origin, sizeof origin);
        snprintf(out, cap, "%s%s", origin, href);
        return;
    }
    if (href[0] == '#' || href[0] == '?') {
        strncpy(out, base, cap); out[cap-1]=0; return;
    }
    if (href[0] == '.' && href[1] == '/') href += 2;
    char dir[URL_MAX]; get_dir(base, dir, sizeof dir);
    snprintf(out, cap, "%s%s", dir, href);
}

/* ---------- entity decode (minimal) ---------- */
static const char *emit_entity(const char *h, char *out, size_t *o, size_t cap) {
    if      (!strncmp(h,"&amp;",5))  { if(*o<cap) out[(*o)++]='&';  return h+5; }
    else if (!strncmp(h,"&lt;",4))   { if(*o<cap) out[(*o)++]='<';  return h+4; }
    else if (!strncmp(h,"&gt;",4))   { if(*o<cap) out[(*o)++]='>';  return h+4; }
    else if (!strncmp(h,"&quot;",6)) { if(*o<cap) out[(*o)++]='"';  return h+6; }
    else if (!strncmp(h,"&#39;",5))  { if(*o<cap) out[(*o)++]='\''; return h+5; }
    return NULL;
}

/* ---------- HTML -> page_t (skip head/script/style, collect links) ---------- */
static page_t *html_to_page(const char *html, const char *base_url) {
    if (!html) return NULL;
    size_t L = strlen(html);
    /* generous buffer: text <= html size + link markers */
    char *buf = (char*)malloc(L + 1 + MAX_LINKS*4);
    if (!buf) return NULL;

    page_t *pg = (page_t*)calloc(1, sizeof(page_t));
    if (!pg) { free(buf); return NULL; }

    bool in_tag=false, closing=false;
    bool in_head=false, in_script=false, in_style=false, in_anchor=false;
    char tag[16]; int taglen=0;
    char href_tmp[URL_MAX]; href_tmp[0]=0;

    size_t o=0;
    for (size_t i=0; i<L; ) {
        char c = html[i];

        if (!in_script && !in_style && !in_head && c=='&') {
            const char *adv = emit_entity(&html[i], buf, &o, L + MAX_LINKS*4);
            if (adv) { i = (size_t)(adv - html); continue; }
        }

        if (c == '<') {
            in_tag = true; closing = false; taglen=0; i++;
            if (i<L && html[i]=='/') { closing=true; i++; }
            /* read tag name */
            while (i<L && taglen < (int)sizeof(tag)-1 && isalpha((unsigned char)html[i])) {
                tag[taglen++] = (char)tolower((unsigned char)html[i]); i++;
            }
            tag[taglen]=0;

            /* skip attrs until '>' while collecting href for <a> */
            bool got_href=false;
            char attrname[16]; int an=0;
            char attrval[URL_MAX]; int av=0;
            bool in_name=true, in_val=false, quoted=false; char quotech=0;

            if (!closing) {
                while (i<L && html[i] != '>') {
                    char ch = html[i++];
                    if (in_val) {
                        if (quoted) {
                            if (ch==quotech) { in_val=false; quoted=false; attrval[av]=0;
                                if (!got_href && strcmp(tag,"a")==0 && strcasecmp(attrname,"href")==0) {
                                    strncpy(href_tmp, attrval, URL_MAX); href_tmp[URL_MAX-1]=0;
                                    got_href=true;
                                }
                                an=av=0; attrname[0]=attrval[0]=0;
                            } else if (av < (int)sizeof(attrval)-1) attrval[av++]=ch;
                        } else {
                            if (isspace((unsigned char)ch) || ch=='>') {
                                in_val=false; attrval[av]=0;
                                if (!got_href && strcmp(tag,"a")==0 && strcasecmp(attrname,"href")==0) {
                                    strncpy(href_tmp, attrval, URL_MAX); href_tmp[URL_MAX-1]=0;
                                    got_href=true;
                                }
                                an=av=0; attrname[0]=attrval[0]=0;
                                if (ch=='>') break;
                            } else if (av < (int)sizeof(attrval)-1) attrval[av++]=ch;
                        }
                    } else if (in_name) {
                        if (ch=='=' ) {
                            in_name=false; in_val=true; quoted=false; quotech=0;
                            attrname[an]=0;
                        } else if (isspace((unsigned char)ch)) {
                            in_name=false; attrname[an]=0;
                        } else if (an < (int)sizeof(attrname)-1) attrname[an++]=(char)tolower((unsigned char)ch);
                    } else { /* between attrs */
                        if (!isspace((unsigned char)ch)) {
                            in_name=true; in_val=false; an=0; av=0; attrname[an++]=(char)tolower((unsigned char)ch);
                            /* check if starts quoted value right after name? handle name='...' */
                            /* peek */
                            while (i<L && isalnum((unsigned char)html[i])) {
                                if (an < (int)sizeof(attrname)-1) attrname[an++]=(char)tolower((unsigned char)html[i]);
                                i++;
                            }
                            /* skip spaces */
                            while (i<L && isspace((unsigned char)html[i])) i++;
                            if (i<L && html[i]=='=') { i++; in_name=false; in_val=true; av=0; quoted=false; quotech=0;
                                while (i<L && isspace((unsigned char)html[i])) i++;
                                if (i<L && (html[i]=='"'||html[i]=='\'')) { quoted=true; quotech=html[i++]; }
                            } else {
                                in_name=false; attrname[an]=0;
                            }
                        } else {
                            /* noop */
                        }
                    }
                }
            } else {
                /* fast forward to '>' for close tag too */
                while (i<L && html[i] != '>') i++;
            }

            if (i<L && html[i]=='>') i++;

            /* tag state machines */
            if (!closing) {
                if (strcmp(tag,"head")==0) in_head=true;
                else if (strcmp(tag,"script")==0) in_script=true;
                else if (strcmp(tag,"style")==0) in_style=true;
                else if (strcmp(tag,"br")==0 || strcmp(tag,"p")==0) {
                    if (o && buf[o-1] != '\n') buf[o++] = '\n';
                } else if (strcmp(tag,"a")==0) {
                    in_anchor = true;
                    /* we’ll append the [n] marker at close tag time */
                }
            } else {
                if (strcmp(tag,"head")==0) in_head=false;
                else if (strcmp(tag,"script")==0) in_script=false;
                else if (strcmp(tag,"style")==0) in_style=false;
                else if (strcmp(tag,"a")==0) {
                    if (in_anchor) {
                        if (pg->link_count < MAX_LINKS) {
                            int idx = pg->link_count++;
                            char abs[URL_MAX]; resolve_url(base_url, href_tmp, abs, sizeof abs);
                            strncpy(pg->links[idx].href, abs, URL_MAX);
                            pg->links[idx].href[URL_MAX-1] = 0;
                            /* append marker [n] */
                            int n = idx + 1;
                            int wrote = snprintf(buf + o, L + MAX_LINKS*4 - o, "[%d]", n);
                            if (wrote > 0) o += (size_t)wrote;
                        }
                        href_tmp[0]=0;
                        in_anchor=false;
                    }
                }
            }
            continue;
        }

        if (in_script || in_style || in_head) { i++; continue; }

        /* normal text */
        if (c=='\r') { i++; continue; }
        if (c=='\n') { if (o && buf[o-1] != '\n') buf[o++]='\n'; i++; continue; }

        /* collapse double spaces */
        if (c==' ' && o && buf[o-1]==' ') { i++; continue; }

        if (o < L + MAX_LINKS*4 - 1) buf[o++] = c;
        i++;
    }

    buf[o]=0;
    pg->text = buf;
    return pg;
}

/* ---------- wrap text to columns ---------- */
static char *wrap_text(const char *in, int max_cols) {
    if (!in) return NULL;
    size_t n = strlen(in);
    char *out = (char*)malloc(n + n/(max_cols?max_cols:1) + 8);
    if (!out) return NULL;
    int col=0; size_t o=0;
    for (size_t i=0;i<n;i++){
        char c=in[i];
        if (c=='\r') continue;
        if (c=='\n'){ out[o++]='\n'; col=0; continue; }
        if (max_cols && col>=max_cols && c==' '){ out[o++]='\n'; col=0; continue; }
        if (max_cols && col>=max_cols){ out[o++]='\n'; col=0; }
        out[o++]=c; col++;
    }
    out[o]=0;
    return out;
}

/* ---------- curl fetch ---------- */
static int fetch_url(const char *url, mem_t *m) {
    if (!url || !m) return -1;
    CURL *curl = curl_easy_init(); if (!curl) return -2;
    m->buf = NULL; m->len = 0;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "BadgeVMS-mini-browser/1.1");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, wr_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, m);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, TIMEOUT_S);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, TIMEOUT_S);
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    return (res == CURLE_OK) ? 0 : (int)res;
}

/* ---------- UI ---------- */
static void draw_ui(SDL_Renderer *r, const char *url, const char *status_msg, int link_count, int sel_idx) {
    if (!r) return;
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_RenderClear(r);

    SDL_FRect top = {0, 0, VIEW_W, URLBAR_H};
    SDL_SetRenderDrawColor(r, 30, 30, 30, 255);
    SDL_RenderFillRect(r, &top);

    SDL_SetRenderDrawColor(r, 220, 220, 220, 255);
    draw_text(r, PAD_LR, 4, url ? url : "", VIEW_W - PAD_LR*2);

    SDL_FRect mid = {0, URLBAR_H + 2, VIEW_W, 6};
    SDL_SetRenderDrawColor(r, 80, 80, 80, 255);
    SDL_RenderFillRect(r, &mid);

    char hint[160];
    snprintf(hint, sizeof(hint),
             "Type URL, Enter=Go | Tab=Next link, Shift+Tab=Prev, Enter=Open [%d/%d] | Ctrl+R reload, Ctrl+Q quit",
             (link_count? sel_idx+1 : 0), link_count);

    SDL_SetRenderDrawColor(r, 200, 200, 40, 255);
    draw_text(r, PAD_LR, URLBAR_H + 12, status_msg && *status_msg ? status_msg : hint, VIEW_W - PAD_LR*2);
}

/* ---------- printable (ASCII) for URL input ---------- */
static int is_printable_ascii(const char *text) {
    if (!text || !*text) return 0;
    unsigned char c = (unsigned char)text[0];
    return (c >= 32 && c <= 126);
}

/* ---------- main ---------- */
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("[mini_browser] enter main\n");

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        printf("[mini_browser] SDL_Init failed: %s\n", SDL_GetError());
        return 0;
    }

    SDL_Window *win = SDL_CreateWindow("mini_browser", VIEW_W, VIEW_H, 0);
    if (!win) {
        printf("[mini_browser] CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 0;
    }
    SDL_Renderer *ren = SDL_CreateRenderer(win, NULL);
    if (!ren) {
        printf("[mini_browser] CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 0;
    }

    wifi_connect();
    curl_global_init(0);

    char url_buf[URL_MAX] = "https://why2025.org";
    char status[128] = "Type URL. Enter=Go  Tab/Shift+Tab=Select link  Enter=Open  Ctrl+R=Reload  Ctrl+Q=Quit";
    char *content_wrapped = NULL;
    page_t *page = NULL;
    int  scroll_lines = 0;
    int  need_fetch = 1;
    int  sel_link = -1;

    const int max_cols = (VIEW_W - 2*PAD_LR) / CH_W;
    const int lines_per_page = (VIEW_H - PAD_TOP - PAD_BOTTOM) / (CH_H + LINE_SPACING);

    SDL_StartTextInput(win);

    draw_ui(ren, url_buf, status, 0, -1);
    SDL_RenderPresent(ren);

    int running = 1;
    while (running) {

        if (need_fetch) {
            snprintf(status, sizeof(status), "Fetching...");
            draw_ui(ren, url_buf, status, 0, -1);
            SDL_RenderPresent(ren);

            mem_t m = {0};
            int rc = fetch_url(url_buf, &m);
            if (rc != 0) {
                snprintf(status, sizeof(status), "Fetch failed (%d)", rc);
                printf("[mini_browser] fetch error %d\n", rc);
                if (page) { free(page->text); free(page); page=NULL; }
                free(content_wrapped); content_wrapped=NULL;
                sel_link=-1;
            } else {
                page_t *pg = html_to_page(m.buf ? m.buf : "", url_buf);
                if (!pg) {
                    snprintf(status, sizeof(status), "OOM parsing");
                    if (page) { free(page->text); free(page); page=NULL; }
                    free(content_wrapped); content_wrapped=NULL;
                    sel_link=-1;
                } else {
                    char *wrapped = wrap_text(pg->text, max_cols);
                    free(content_wrapped); content_wrapped = wrapped;
                    if (page) { free(page->text); free(page); }
                    page = pg;
                    snprintf(status, sizeof(status), "OK (%zu bytes, %d links)", m.len, page->link_count);
                    scroll_lines = 0;
                    sel_link = (page->link_count>0) ? 0 : -1;
                    if (wrapped) {
                        printf("\n--- BEGIN CONTENT ---\n%s\n--- END CONTENT ---\n", wrapped);
                    }
                }
            }
            free(m.buf);
            need_fetch = 0;
        }

        /* Draw UI + visible text slice */
        draw_ui(ren, url_buf, status, page ? page->link_count : 0, (sel_link>=0?sel_link:0));
        if (content_wrapped) {
            int y = PAD_TOP;
            const char *p = content_wrapped;
            for (int s = 0; s < scroll_lines && p && *p; ) { if (*p++ == '\n') s++; }
            SDL_SetRenderDrawColor(ren, 220, 220, 220, 255);
            int drawn = 0;
            while (p && *p && drawn < lines_per_page) {
                const char *nl = strchr(p, '\n');
                int len = nl ? (int)(nl - p) : (int)strlen(p);
                char tmp[1024];
                if (len > (int)sizeof(tmp)-1) len = (int)sizeof(tmp)-1;
                memcpy(tmp, p, len); tmp[len] = 0;
                draw_text(ren, PAD_LR, y, tmp, VIEW_W - 2*PAD_LR);
                y += (CH_H + LINE_SPACING);
                drawn++;
                p = nl ? nl + 1 : NULL;
            }
        }
        SDL_RenderPresent(ren);

        /* Events */
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) { running = 0; break; }

            if (ev.type == SDL_EVENT_TEXT_INPUT) {
                const char *t = ev.text.text; /* UTF-8 subset */
                if (is_printable_ascii(t)) {
                    size_t curlen = strlen(url_buf);
                    if (curlen < URL_MAX - 1) {
                        url_buf[curlen] = t[0];
                        url_buf[curlen + 1] = 0;
                        snprintf(status, sizeof(status), "Editing URL...");
                    }
                }
            }

            if (ev.type == SDL_EVENT_KEY_DOWN) {
                SDL_Keymod mods = ev.key.mod;
                const bool ctrl = (mods & SDL_KMOD_CTRL) != 0;

                switch (ev.key.scancode) {
                    /* URL actions */
                    case SDL_SCANCODE_RETURN:
                    case SDL_SCANCODE_KP_ENTER:
                        if (page && sel_link >= 0 && sel_link < page->link_count) {
                            strncpy(url_buf, page->links[sel_link].href, URL_MAX);
                            url_buf[URL_MAX-1]=0; need_fetch = 1;
                        } else {
                            need_fetch = 1;
                        }
                        break;

                    case SDL_SCANCODE_BACKSPACE: {
                        size_t curlen = strlen(url_buf);
                        if (curlen) url_buf[curlen - 1] = 0;
                        snprintf(status, sizeof(status), "Editing URL...");
                        break;
                    }

                    /* Commands (Ctrl+X so plain letters can be typed in URL) */
                    case SDL_SCANCODE_Q: if (ctrl) running = 0; break;
                    case SDL_SCANCODE_R: if (ctrl) need_fetch = 1; break;
                    case SDL_SCANCODE_H:
                        if (ctrl) { strncpy(url_buf, "https://httpbin.org/html", URL_MAX); url_buf[URL_MAX-1]=0; need_fetch=1; }
                        break;
                    case SDL_SCANCODE_E:
                        if (ctrl) { strncpy(url_buf, "https://badge.why2025.org/", URL_MAX); url_buf[URL_MAX-1]=0; need_fetch=1; }
                        break;

                    /* Scrolling */
                    case SDL_SCANCODE_DOWN:
                    case SDL_SCANCODE_J: scroll_lines++; break;
                    case SDL_SCANCODE_UP:
                    case SDL_SCANCODE_K: if (scroll_lines>0) scroll_lines--; break;
                    case SDL_SCANCODE_HOME: scroll_lines = 0; break;

                    /* Link navigation */
                    case SDL_SCANCODE_TAB:
                        if (page && page->link_count>0) {
                            if (mods & SDL_KMOD_SHIFT) {
                                sel_link = (sel_link<=0) ? (page->link_count-1) : (sel_link-1);
                            } else {
                                sel_link = (sel_link+1) % page->link_count;
                            }
                            snprintf(status, sizeof(status), "Link [%d/%d]: %s",
                                     sel_link+1, page->link_count, page->links[sel_link].href);
                        }
                        break;

                    case SDL_SCANCODE_ESCAPE: running = 0; break;
                    default: break;
                }
            }
        }

        SDL_Delay(10);
    }

    SDL_StopTextInput(win);
    if (page) { free(page->text); free(page); }
    free(content_wrapped);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    SDL_Quit();
    curl_global_cleanup();
    printf("[mini_browser] exit main\n");
    return 0;
}
