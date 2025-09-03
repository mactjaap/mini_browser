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
#define VIEW_H        716
#define URLBAR_H      24
#define LINE_SPACING  2
#define MAX_LINKS     128
#define HOME_URL      "https://urlvanish.com/47b2c6ec"  /* change if you prefer */

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
    char *text;                 /* rendered text with [n] markers */
    link_t links[MAX_LINKS];
    int link_count;
    char base[URL_MAX];         /* base URL for resolution */
} page_t;

/* ---------- UTF-8 decode + ASCII-only draw ---------- */
static void draw_char(SDL_Renderer *r, int x, int y, char c) {
    if (!r) return;
    if ((unsigned char)c < 32 || (unsigned char)c > 127) c = '?';
    const unsigned char *cols = font5x7[(unsigned char)c - 32];
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

static unsigned utf8_next(const char *s, size_t len, size_t *i) {
    if (*i >= len) return 0;
    unsigned char c = (unsigned char)s[*i];
    if (c < 0x80) { (*i)++; return c; }
    if ((c & 0xE0) == 0xC0 && *i+1 < len) { unsigned cp=((c&0x1F)<<6) | (s[*i+1]&0x3F); *i+=2; return cp; }
    if ((c & 0xF0) == 0xE0 && *i+2 < len) { unsigned cp=((c&0x0F)<<12)|((s[*i+1]&0x3F)<<6)|(s[*i+2]&0x3F); *i+=3; return cp; }
    if ((c & 0xF8) == 0xF0 && *i+3 < len) { unsigned cp=((c&0x07)<<18)|((s[*i+1]&0x3F)<<12)|((s[*i+2]&0x3F)<<6)|(s[*i+3]&0x3F); *i+=4; return cp; }
    (*i)++; return '?';
}

static void draw_text(SDL_Renderer *r, int x, int y, const char *s, int max_w) {
    if (!r || !s) return;
    int cx = x, cy = y;
    size_t i=0, L=strlen(s);
    while (i<L) {
        unsigned cp = utf8_next(s, L, &i);
        if (cp == 0) break;
        if (cp == '\n') { cx = x; cy += (CH_H + LINE_SPACING); continue; }
        if (cp == 0xA0) cp = ' ';                 /* NBSP -> space */
        if (cp < 32 || cp > 126) continue;        /* draw ASCII only */
        if (cx + CH_W > x + max_w) { cx = x; cy += (CH_H + LINE_SPACING); }
        draw_char(r, cx, cy, (char)cp);
        cx += CH_W;
    }
}

/* --- draw URL bar text in one line, clipping head with "..." if needed --- */
static void draw_bar(SDL_Renderer *r, const char *text) {
    int max_cols = (VIEW_W - 2*PAD_LR)/CH_W;
    size_t n = strlen(text);
    char tmp[URL_MAX + 32];
    if ((int)n > max_cols) {
        const char *start = text + (n - (size_t)max_cols + 3);
        snprintf(tmp, sizeof tmp, "...%s", start);
        draw_text(r, PAD_LR, 4, tmp, VIEW_W - PAD_LR*2);
    } else {
        draw_text(r, PAD_LR, 4, text, VIEW_W - PAD_LR*2);
    }
}

/* ---------- helpers: base URL / resolution ---------- */
static void get_scheme_host(const char *url, char *out, size_t cap) {
    const char *p = strstr(url, "://");
    if (!p) { out[0]=0; return; }
    p += 3;
    const char *slash = strchr(p, '/');
    size_t n = slash ? (size_t)(slash - url) : strlen(url);
    if (n >= cap) n = cap - 1;
    memcpy(out, url, n); out[n]=0;
}
static void get_dir(const char *url, char *out, size_t cap) {
    const char *q = url;
    const char *p = strrchr(q, '/');
    if (!p) { out[0]=0; return; }
    size_t n = (size_t)(p - q) + 1;
    if (n >= cap) n = cap - 1;
    memcpy(out, q, n); out[n]=0;
}
static void base_no_query_or_hash(const char *u, char *out, size_t cap) {
    size_t n = strlen(u), cut = n;
    for (size_t i=0;i<n;i++){ if (u[i]=='?' || u[i]=='#'){ cut=i; break; } }
    if (cut >= cap) cut = cap-1;
    memcpy(out, u, cut); out[cut]=0;
}
static void base_no_hash(const char *u, char *out, size_t cap) {
    size_t n = strlen(u), cut = n;
    for (size_t i=0;i<n;i++){ if (u[i]=='#'){ cut=i; break; } }
    if (cut >= cap) cut = cap-1;
    memcpy(out, u, cut); out[cut]=0;
}
static void scheme_from_url(const char *base, char *out, size_t cap) {
    if (!base) { strncpy(out, "https", cap); out[cap-1]=0; return; }
    const char *p = strstr(base, "://");
    if (!p) { strncpy(out, "https", cap); out[cap-1]=0; return; }
    size_t n = (size_t)(p - base);
    if (n >= cap) n = cap - 1;
    memcpy(out, base, n); out[n]=0;
}
static void resolve_url(const char *base, const char *href, char *out, size_t cap) {
    if (!href || !*href) { out[0]=0; return; }
    if (strstr(href, "://")) { strncpy(out, href, cap); out[cap-1]=0; return; }
    if (href[0]=='/' && href[1]=='/') {
        char sch[16]; scheme_from_url(base, sch, sizeof sch);
        snprintf(out, cap, "%s:%s", sch, href); return;
    }
    if (href[0]=='/') {
        char origin[URL_MAX]; get_scheme_host(base, origin, sizeof origin);
        snprintf(out, cap, "%s%s", origin, href); return;
    }
    if (href[0]=='?') {
        char base2[URL_MAX]; base_no_query_or_hash(base, base2, sizeof base2);
        snprintf(out, cap, "%s%s", base2, href); return;
    }
    if (href[0]=='#') {
        char base2[URL_MAX]; base_no_hash(base, base2, sizeof base2);
        snprintf(out, cap, "%s%s", base2, href); return;
    }
    if (href[0]=='.' && href[1]=='/') href += 2;
    char dir[URL_MAX]; get_dir(base, dir, sizeof dir);
    snprintf(out, cap, "%s%s", dir, href);
}

/* --- URL sanitation --- */
static void trim_inplace(char *s) {
    if (!s) return;
    size_t n = strlen(s);
    size_t i = 0; while (i < n && isspace((unsigned char)s[i])) i++;
    size_t j = n; while (j > i && isspace((unsigned char)s[j-1])) j--;
    if (i > 0 || j < n) { memmove(s, s + i, j - i); s[j - i] = 0; }
}
static int has_scheme(const char *u) { return u && strstr(u, "://") != NULL; }
static int is_http_scheme(const char *u) {
    return u && (strncmp(u, "http://", 7)==0 || strncmp(u, "https://", 8)==0);
}
static void normalize_typed_url(char *buf) {
    trim_inplace(buf);
    if (!buf[0]) return;
    if (!has_scheme(buf)) {
        char tmp[URL_MAX]; strncpy(tmp, buf, URL_MAX); tmp[URL_MAX-1]=0;
        snprintf(buf, URL_MAX, "https://%s", tmp);
    }
}

/* ---------- entity decode (minimal) ---------- */
static const char *emit_entity(const char *h, char *out, size_t *o, size_t cap) {
    if      (!strncmp(h,"&amp;",5))  { if(*o<cap) out[(*o)++]='&';  return h+5; }
    else if (!strncmp(h,"&lt;",4))   { if(*o<cap) out[(*o)++]='<';  return h+4; }
    else if (!strncmp(h,"&gt;",4))   { if(*o<cap) out[(*o)++]='>';  return h+4; }
    else if (!strncmp(h,"&quot;",6)) { if(*o<cap) out[(*o)++]='"';  return h+6; }
    else if (!strncmp(h,"&#39;",5))  { if(*o<cap) out[(*o)++]='\''; return h+5; }
    else if (!strncmp(h,"&nbsp;",6)) { if(*o<cap) out[(*o)++]=' ';  return h+6; }
    return NULL;
}

/* --------- href filter --------- */
static int is_supported_href(const char *h) {
    if (!h || !*h) return 0;
    if (h[0] == '#') return 0; /* in-page */
    if (!strncasecmp(h, "javascript:", 11)) return 0;
    if (!strncasecmp(h, "mailto:", 7)) return 0;
    if (!strncasecmp(h, "data:", 5)) return 0;
    /* allow http(s), scheme-relative //, ?, /, and relatives */
    return 1;
}

/* ---------- HTML -> page_t ---------- */
static page_t *html_to_page(const char *html, const char *base_url) {
    if (!html) return NULL;
    size_t L = strlen(html);
    char *buf = (char*)malloc(L + 1 + MAX_LINKS*6);
    if (!buf) return NULL;

    page_t *pg = (page_t*)calloc(1, sizeof(page_t));
    if (!pg) { free(buf); return NULL; }
    strncpy(pg->base, base_url ? base_url : "", URL_MAX);
    pg->base[URL_MAX-1] = 0;

    bool in_head=false, in_script=false, in_style=false;
    size_t o=0;

    for (size_t i=0; i<L; ) {
        char c = html[i];

        if (!in_script && !in_style && !in_head && c=='&') {
            const char *adv = emit_entity(&html[i], buf, &o, L + MAX_LINKS*6);
            if (adv) { i = (size_t)(adv - html); continue; }
        }

        if (c == '<') {
            i++;
            bool closing = false;
            if (i<L && html[i]=='/') { closing=true; i++; }

            char tname[16]; int tn=0;
            while (i<L && tn<(int)sizeof(tname)-1 && isalpha((unsigned char)html[i])) {
                tname[tn++] = (char)tolower((unsigned char)html[i]); i++;
            }
            tname[tn]=0;

            while (i<L && isspace((unsigned char)html[i])) i++;

            if (!closing) {
                if (!strcmp(tname,"head"))   in_head=true;
                else if (!strcmp(tname,"script")) in_script=true;
                else if (!strcmp(tname,"style"))  in_style=true;

                if (!strcmp(tname,"a")) {
                    char href_val[URL_MAX]; href_val[0]=0;

                    while (i<L && html[i] != '>') {
                        char aname[16]; int an=0;
                        while (i<L && isspace((unsigned char)html[i])) i++;
                        while (i<L && an<(int)sizeof(aname)-1 &&
                               (isalnum((unsigned char)html[i]) || html[i]=='-' || html[i]=='_')) {
                            aname[an++] = (char)tolower((unsigned char)html[i]); i++;
                        }
                        aname[an]=0;
                        while (i<L && isspace((unsigned char)html[i])) i++;

                        if (i<L && html[i]=='=') {
                            i++;
                            while (i<L && isspace((unsigned char)html[i])) i++;
                            char quote=0;
                            if (i<L && (html[i]=='"' || html[i]=='\'')) quote = html[i++];

                            char aval[URL_MAX]; int av=0;
                            while (i<L) {
                                if (quote) {
                                    if (html[i]==quote) { i++; break; }
                                } else {
                                    if (isspace((unsigned char)html[i]) || html[i]=='>') break;
                                }
                                if (av < (int)sizeof(aval)-1) aval[av++] = html[i];
                                i++;
                            }
                            aval[av]=0;

                            if (!strcmp(aname,"href") && !href_val[0]) {
                                strncpy(href_val, aval, URL_MAX);
                                href_val[URL_MAX-1]=0;
                            }
                        } else {
                            while (i<L && !isspace((unsigned char)html[i]) && html[i] != '>') i++;
                        }
                    }

                    if (is_supported_href(href_val) && pg->link_count < MAX_LINKS) {
                        char abs[URL_MAX]; resolve_url(pg->base, href_val, abs, sizeof abs);
                        if (is_supported_href(abs)) {
                            int idx = pg->link_count++;
                            strncpy(pg->links[idx].href, abs, URL_MAX);
                            pg->links[idx].href[URL_MAX-1]=0;
                            int wrote = snprintf(buf + o, L + MAX_LINKS*6 - o, "[%d]", idx+1);
                            if (wrote > 0) o += (size_t)wrote;
                        }
                    }

                    while (i<L && html[i] != '>') i++;
                } else if (!strcmp(tname,"br") || !strcmp(tname,"p")) {
                    if (o && buf[o-1] != '\n') buf[o++] = '\n';
                    while (i<L && html[i] != '>') i++;
                } else {
                    while (i<L && html[i] != '>') i++;
                }
            } else {
                if (!strcmp(tname,"head"))   in_head=false;
                else if (!strcmp(tname,"script")) in_script=false;
                else if (!strcmp(tname,"style"))  in_style=false;
                while (i<L && html[i] != '>') i++;
            }

            if (i<L && html[i]=='>') i++;
            continue;
        }

        if (in_script || in_style || in_head) { i++; continue; }

        if (c=='\r') { i++; continue; }
        if (c=='\n') { if (o && buf[o-1] != '\n') buf[o++]='\n'; i++; continue; }
        if (c==' ' && o && buf[o-1]==' ') { i++; continue; }

        if (o < L + MAX_LINKS*6 - 1) buf[o++] = c;
        i++;
    }

    buf[o]=0;
    pg->text = buf;
    return pg;
}

/* ---------- wrap text to columns (with whitespace collapse) ---------- */
static char *wrap_text(const char *in, int max_cols) {
    if (!in) return NULL;
    size_t n = strlen(in);
    char *out = (char*)malloc(n + n/(max_cols?max_cols:1) + 8);
    if (!out) return NULL;

    int col = 0;
    size_t o = 0;
    int blank_run = 0;

    for (size_t i = 0; i < n; i++) {
        char c = in[i];
        if (c == '\r') continue;

        if (c == '\n') {
            if (col == 0) {
                if (blank_run) continue;
                blank_run = 1;
            } else {
                blank_run = 0;
            }
            out[o++] = '\n';
            col = 0;
            continue;
        }

        if (c == ' ' && (col == 0 || out[o-1] == ' ')) continue;

        if (max_cols && col >= max_cols && c == ' ') {
            out[o++] = '\n'; col = 0; continue;
        }
        if (max_cols && col >= max_cols) {
            out[o++] = '\n'; col = 0;
        }

        out[o++] = c;
        col++;
        blank_run = 0;
    }
    out[o] = 0;
    return out;
}

/* ---------- curl fetch ---------- */
static int fetch_url(const char *url, mem_t *m) {
    if (!url || !m) return -1;
    CURL *curl = curl_easy_init(); if (!curl) return -2;
    m->buf = NULL; m->len = 0;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "BadgeVMS-mini-browser/2.0");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, wr_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, m);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, TIMEOUT_S);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, TIMEOUT_S);
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    return (res == CURLE_OK) ? 0 : (int)res;
}

/* ---------- UI ---------- */
static void draw_ui(SDL_Renderer *r, const char *bar_text) {
    if (!r) return;
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_RenderClear(r);

    SDL_FRect top = (SDL_FRect){0, 0, VIEW_W, URLBAR_H};
    SDL_SetRenderDrawColor(r, 30, 30, 30, 255);
    SDL_RenderFillRect(r, &top);

    SDL_SetRenderDrawColor(r, 220, 220, 220, 255);
    draw_bar(r, bar_text);

    SDL_FRect mid = (SDL_FRect){0, URLBAR_H + 1, VIEW_W, 2};
    SDL_SetRenderDrawColor(r, 60, 60, 60, 255);
    SDL_RenderFillRect(r, &mid);
}

/* ---------- printable (ASCII) for URL input ---------- */
static int is_printable_ascii(const char *text) {
    if (!text || !*text) return 0;
    unsigned char c = (unsigned char)text[0];
    return (c >= 32 && c <= 126);
}

/* ---------- main ---------- */
int main(void) {
    printf("[mini_browser] enter main\n");

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        printf("[mini_browser] SDL_Init failed: %s\n", SDL_GetError());
        return 0;
    }

    SDL_Window *win = SDL_CreateWindow("mini_browser", VIEW_W, VIEW_H, 0);
    if (!win) { printf("[mini_browser] CreateWindow failed: %s\n", SDL_GetError()); SDL_Quit(); return 0; }
    SDL_Renderer *ren = SDL_CreateRenderer(win, NULL);
    if (!ren) { printf("[mini_browser] CreateRenderer failed: %s\n", SDL_GetError()); SDL_DestroyWindow(win); SDL_Quit(); return 0; }

    wifi_connect();
    curl_global_init(0);

    char url_buf[URL_MAX] = HOME_URL;
    char barline[URL_MAX + 64];
    char *content_wrapped = NULL;
    page_t *page = NULL;
    int  scroll_lines = 0;
    int  need_fetch = 1;
    int  sel_link = -1;

    const int max_cols = (VIEW_W - 2*PAD_LR) / CH_W;
    const int lines_per_page = (VIEW_H - PAD_TOP - PAD_BOTTOM) / (CH_H + LINE_SPACING);

    SDL_StartTextInput(win);

    bool suppress_text_input = false;  /* <— NEW: skip the next TEXT_INPUT after an accelerator */

    int running = 1;
    while (running) {

        if (need_fetch) {
            trim_inplace(url_buf);
            if (!url_buf[0]) { need_fetch = 0; goto redraw; }
            if (!has_scheme(url_buf) && !(url_buf[0]=='/' && url_buf[1]=='/')) {
                normalize_typed_url(url_buf);
            }
            if (url_buf[0]=='/' && url_buf[1]=='/') {
                char sch[16]; scheme_from_url(page ? page->base : "https://example.org", sch, sizeof sch);
                char tmp[URL_MAX]; snprintf(tmp, sizeof tmp, "%s:%s", sch, url_buf);
                strncpy(url_buf, tmp, URL_MAX); url_buf[URL_MAX-1]=0;
            }
            if (is_http_scheme(url_buf)) {
                snprintf(barline, sizeof(barline), "%s", url_buf);
                draw_ui(ren, barline);
                SDL_RenderPresent(ren);

                mem_t m = {0};
                int rc = fetch_url(url_buf, &m);
                if (rc != 0) {
                    printf("[mini_browser] fetch error %d URL='%s'\n", rc, url_buf);
                    if (page) { free(page->text); free(page); page=NULL; }
                    free(content_wrapped); content_wrapped=NULL;
                    sel_link=-1;
                } else {
                    page_t *pg = html_to_page(m.buf ? m.buf : "", url_buf);
                    if (!pg) {
                        if (page) { free(page->text); free(page); page=NULL; }
                        free(content_wrapped); content_wrapped=NULL;
                        sel_link=-1;
                    } else {
                        char *wrapped = wrap_text(pg->text, max_cols);
                        free(content_wrapped); content_wrapped = wrapped;
                        if (page) { free(page->text); free(page); }
                        page = pg;
                        scroll_lines = 0;
                        sel_link = -1;
                        printf("[mini_browser] parsed %d links from %s\n", page->link_count, url_buf);
                        // print content in the serial output too.
                        if (content_wrapped && *content_wrapped) {
                            printf("\n--- CONTENT START ---\n%s\n--- CONTENT END ---\n", content_wrapped);
                            fflush(stdout);
                        }
                    }
                }
                free(m.buf);
            }
            need_fetch = 0;
        }

redraw:
        if (page && sel_link >= 0 && sel_link < page->link_count) {
            snprintf(barline, sizeof(barline), "[%d/%d]  %s",
                     sel_link+1, page->link_count, page->links[sel_link].href);
        } else {
            snprintf(barline, sizeof(barline), "%s", url_buf);
        }

        draw_ui(ren, barline);
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

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) { running = 0; break; }

            if (ev.type == SDL_EVENT_TEXT_INPUT) {
                if (suppress_text_input) { suppress_text_input = false; continue; }  /* <— NEW */
                const char *t = ev.text.text;
                if (is_printable_ascii(t)) {
                    size_t curlen = strlen(url_buf);
                    if (curlen < URL_MAX - 1) {
                        url_buf[curlen] = t[0];
                        url_buf[curlen + 1] = 0;
                        sel_link = -1;
                    }
                }
            }

            if (ev.type == SDL_EVENT_KEY_DOWN) {
                SDL_Keymod mods = ev.key.mod;
                const bool accel = (mods & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) != 0;  /* <— CHANGED */

                switch (ev.key.scancode) {
                    /* URL actions */
                    case SDL_SCANCODE_RETURN:
                    case SDL_SCANCODE_KP_ENTER:
                        if (page && sel_link >= 0 && sel_link < page->link_count) {
                            strncpy(url_buf, page->links[sel_link].href, URL_MAX);
                            url_buf[URL_MAX-1]=0;
                            need_fetch = 1;
                        } else {
                            need_fetch = 1;
                        }
                        break;

                    case SDL_SCANCODE_BACKSPACE: {
                        size_t curlen = strlen(url_buf);
                        if (curlen) url_buf[curlen - 1] = 0;
                        sel_link = -1;
                        break;
                    }

                    /* Commands (Ctrl/Cmd + key) */
                    case SDL_SCANCODE_Q: if (accel) running = 0; break;
                    case SDL_SCANCODE_R:
                        if (accel) { need_fetch = 1; suppress_text_input = true; }   /* <— NEW */
                        break;
                    case SDL_SCANCODE_E:
                        if (accel) {
                            strncpy(url_buf, "https://", URL_MAX);                   /* <— NEW */
                            url_buf[URL_MAX-1]=0;
                            sel_link = -1;
                            suppress_text_input = true;                              /* <— NEW */
                        }
                        break;
                    case SDL_SCANCODE_H:
                        if (accel) {
                            strncpy(url_buf, HOME_URL, URL_MAX);                     /* <— NEW */
                            url_buf[URL_MAX-1]=0;
                            sel_link = -1;
                            need_fetch = 1;
                            suppress_text_input = true;                              /* <— NEW */
                        }
                        break;

                    /* Scrolling */
                    case SDL_SCANCODE_DOWN:
                    case SDL_SCANCODE_J: scroll_lines++; break;
                    case SDL_SCANCODE_UP:
                    case SDL_SCANCODE_K: if (scroll_lines>0) scroll_lines--; break;
                    case SDL_SCANCODE_HOME: scroll_lines = 0; break;
                    case SDL_SCANCODE_PAGEDOWN: {
                        int lpp = (VIEW_H - PAD_TOP - PAD_BOTTOM) / (CH_H + LINE_SPACING);
                        scroll_lines += lpp > 2 ? lpp - 2 : 1;
                        break;
                    }
                    case SDL_SCANCODE_PAGEUP:
                        if (scroll_lines >= 5) scroll_lines -= 5; else scroll_lines = 0;
                        break;

                    /* Link navigation */
                    case SDL_SCANCODE_TAB: {
                        const bool shift = (mods & SDL_KMOD_SHIFT) != 0;
                        if (page && page->link_count>0) {
                            if (shift) {
                                sel_link = (sel_link<=0) ? (page->link_count-1) : (sel_link-1);
                            } else {
                                sel_link = (sel_link+1) % page->link_count;
                            }
                        }
                        break;
                    }

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
