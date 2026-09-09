#if defined(ESP_PLATFORM)
#include "esp_log.h"
#endif

#include "badgevms/wifi.h"
#include "curl/curl.h"
#include <SDL3/SDL.h>
#include <ctype.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* for strncasecmp */

/* --- Yield macro for BadgeVMS/ESP-IDF, no-op on desktop --- */
#if defined(ESP_PLATFORM)
# include "freertos/FreeRTOS.h"
# include "freertos/task.h"
# define YIELD_NET() vTaskDelay(pdMS_TO_TICKS(2))
#else
# define YIELD_NET() ((void)0)
#endif

/* ---------- Limits & layout ---------- */
#define MAX_BYTES     (64 * 1024)
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
#define MAX_ACTIONS   160
#define MAX_FORMS       4
#define MAX_FORM_FIELDS 8
#define FORM_VALUE_MAX 128

/* --- Scroll repeat constants for hold-to-scroll --- */
#define SCROLL_REPEAT_DELAY_MS 300
#define SCROLL_REPEAT_INTERVAL_MS 55

/* --- Icon placement inside URL bar --- */
#define ICON_LEFT   2
#define ICON_TOP    2
#define ICON_SIZE   20
#define ICON_GAP    8                 /* gap between icon and URL text */
#define URL_TEXT_X  (PAD_LR + ICON_SIZE + ICON_GAP)  /* start X for URL text */

/* ---------- Home + special-key targets ---------- */
#define HOME_URL          "https://minibrowser.macip.net"
#define SPECIAL_URL_124   "https://text.npr.org"
#define SPECIAL_URL_125   "https://news.ycombinator.com/"
#define SPECIAL_URL_126   "http://www.textfiles.com/"
#define SPECIAL_URL_127   "https://ifconfig.co"
#define SPECIAL_URL_128   "https://ohmeadhbh.github.io/bobcat/"
#define SPECIAL_URL_129   "https://curl.se/"

/* ---------- Accelerator + special scancodes ---------- */
#define SC_ACCELERATOR    ((SDL_Scancode)0xE3)  /* WHY2025 key */
#define SC_SPECIAL_124    ((SDL_Scancode)0x124)
#define SC_SPECIAL_125    ((SDL_Scancode)0x125)
#define SC_SPECIAL_126    ((SDL_Scancode)0x126)
#define SC_SPECIAL_127    ((SDL_Scancode)0x127)
#define SC_SPECIAL_128    ((SDL_Scancode)0x128)
#define SC_SPECIAL_129    ((SDL_Scancode)0x129)

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
typedef struct {
    char href[URL_MAX];
} link_t;

typedef struct {
    char name[64];
    char value[FORM_VALUE_MAX];
    char label[64];
    char type[16];
    bool disabled;
} form_field_t;

typedef struct {
    char action[URL_MAX];
    char method[8];
    form_field_t fields[MAX_FORM_FIELDS];
    int field_count;
} form_t;

typedef enum {
    ACTION_LINK,
    ACTION_FORM_FIELD,
    ACTION_FORM_SUBMIT
} action_type_t;

typedef struct {
    action_type_t type;
    int link_index;
    int form_index;
    int field_index;
} page_action_t;

typedef struct {
    char *text;
    char *text_template;
    link_t links[MAX_LINKS];
    int link_count;
    page_action_t actions[MAX_ACTIONS];
    int action_count;
    char base[URL_MAX];         /* base URL for resolution */
    char title[128];            /* page <title> */
    form_t forms[MAX_FORMS];
    int form_count;
} page_t;

/* ---------- URL helpers ---------- */
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

/* ---------- entity decode ---------- */
static const char *emit_entity(const char *h, char *out, size_t *o, size_t cap) {
    if (!strncmp(h, "&amp;", 5)) { if (*o < cap) out[(*o)++] = '&'; return h + 5; }
    if (!strncmp(h, "&lt;", 4)) { if (*o < cap) out[(*o)++] = '<'; return h + 4; }
    if (!strncmp(h, "&gt;", 4)) { if (*o < cap) out[(*o)++] = '>'; return h + 4; }
    if (!strncmp(h, "&quot;", 6)) { if (*o < cap) out[(*o)++] = '"'; return h + 6; }
    if (!strncmp(h, "&#39;", 5)) { if (*o < cap) out[(*o)++] = '\''; return h + 5; }
    if (!strncmp(h, "&apos;", 6)) { if (*o < cap) out[(*o)++] = '\''; return h + 6; }
    if (!strncmp(h, "&nbsp;", 6)) { if (*o < cap) out[(*o)++] = ' '; return h + 6; }
    return NULL;
}

/* support UTF-8 */

static int emit_utf8_codepoint(unsigned long cp, char *out, size_t *o, size_t cap) {
    if (cp > 0x10FFFFUL || (cp >= 0xD800UL && cp <= 0xDFFFUL)) {
        cp = 0xFFFD;
    }

    if (cp <= 0x7F) {
        if (*o + 1 >= cap) return 0;
        out[(*o)++] = (char)cp;
    } else if (cp <= 0x7FF) {
        if (*o + 2 >= cap) return 0;
        out[(*o)++] = (char)(0xC0 | (cp >> 6));
        out[(*o)++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp <= 0xFFFF) {
        if (*o + 3 >= cap) return 0;
        out[(*o)++] = (char)(0xE0 | (cp >> 12));
        out[(*o)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[(*o)++] = (char)(0x80 | (cp & 0x3F));
    } else {
        if (*o + 4 >= cap) return 0;
        out[(*o)++] = (char)(0xF0 | (cp >> 18));
        out[(*o)++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[(*o)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[(*o)++] = (char)(0x80 | (cp & 0x3F));
    }

    return 1;
}


static const char *emit_numeric_entity(const char *h, char *out, size_t *o, size_t cap) {
    int base = 10;
    const char *p = h + 2;
    if (*p == 'x' || *p == 'X') { base = 16; p++; }
    unsigned long value = 0;
    const char *digits = p;
    while (*p && *p != ';') {
        int digit;
        if (*p >= '0' && *p <= '9') digit = *p - '0';
        else if (base == 16 && *p >= 'a' && *p <= 'f') digit = 10 + *p - 'a';
        else if (base == 16 && *p >= 'A' && *p <= 'F') digit = 10 + *p - 'A';
        else return NULL;
        if (digit >= base || value > (ULONG_MAX - (unsigned long)digit) / (unsigned long)base) return NULL;
        value = value * (unsigned long)base + (unsigned long)digit;
        p++;
    }

    if (p == digits || *p != ';') return NULL;

    if (!emit_utf8_codepoint(value, out, o, cap)) {
        return NULL;
    }

    return p + 1;

}

/* --------- href filter --------- */
static int is_supported_href(const char *h) {
    if (!h || !*h || h[0] == '#') return 0;
    if (!strncasecmp(h, "javascript:", 11)) return 0;
    if (!strncasecmp(h, "mailto:", 7)) return 0;
    if (!strncasecmp(h, "data:", 5)) return 0;
    return 1;
}

static const char *find_case_insensitive(const char *haystack, const char *needle) {
    if (!haystack || !needle || !*needle) return haystack;
    size_t needle_len = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        if (!strncasecmp(p, needle, needle_len)) return p;
    }
    return NULL;
}

static const char *find_tag_end(const char *start, const char *end) {
    char quote = 0;
    for (const char *p = start; p < end; p++) {
        if (quote) {
            if (*p == quote) quote = 0;
        } else if (*p == '"' || *p == '\'') {
            quote = *p;
        } else if (*p == '>') {
            return p;
        }
    }
    return NULL;
}

static int tag_attribute(const char *start, const char *end, const char *wanted,
                         char *out, size_t out_cap) {
    const char *p = start;
    size_t wanted_len = strlen(wanted);

    while (p < end) {
        while (p < end && (isspace((unsigned char)*p) || *p == '/')) p++;
        const char *name = p;
        while (p < end && (isalnum((unsigned char)*p) || *p == '-' || *p == '_')) p++;
        size_t name_len = (size_t)(p - name);
        if (!name_len) { p++; continue; }
        while (p < end && isspace((unsigned char)*p)) p++;

        const char *value = NULL;
        size_t value_len = 0;
        if (p < end && *p == '=') {
            p++;
            while (p < end && isspace((unsigned char)*p)) p++;
            if (p < end && (*p == '"' || *p == '\'')) {
                char quote = *p++;
                value = p;
                while (p < end && *p != quote) p++;
                value_len = (size_t)(p - value);
                if (p < end) p++;
            } else {
                value = p;
                while (p < end && !isspace((unsigned char)*p) && *p != '>') p++;
                value_len = (size_t)(p - value);
            }
        }

        if (name_len == wanted_len && !strncasecmp(name, wanted, wanted_len)) {
            if (out && out_cap && value) {
                if (value_len >= out_cap) value_len = out_cap - 1;
                memcpy(out, value, value_len);
                out[value_len] = 0;
            }
            return 1;
        }
    }
    return 0;
}

static void lower_ascii(char *s) {
    for (; s && *s; s++) *s = (char)tolower((unsigned char)*s);
}

static int append_bytes(char *out, size_t cap, size_t *used, const char *text, size_t len) {
    if (*used + len >= cap) return 0;
    memcpy(out + *used, text, len);
    *used += len;
    out[*used] = 0;
    return 1;
}

static int append_text(char *out, size_t cap, size_t *used, const char *text) {
    return append_bytes(out, cap, used, text, strlen(text));
}

static void append_line_break(char *out, size_t cap, size_t *used) {
    if (*used && out[*used - 1] != '\n') append_text(out, cap, used, "\n");
}

static int append_action_marker(char *out, size_t cap, size_t *used, int action_index) {
    char marker[8];
    int n = snprintf(marker, sizeof(marker), "\001%03d\002", action_index);
    return n > 0 && (size_t)n < sizeof(marker) && append_bytes(out, cap, used, marker, (size_t)n);
}

static int add_action(page_t *page, action_type_t type, int link_index,
                      int form_index, int field_index) {
    if (page->action_count >= MAX_ACTIONS) return -1;
    int index = page->action_count++;
    page->actions[index].type = type;
    page->actions[index].link_index = link_index;
    page->actions[index].form_index = form_index;
    page->actions[index].field_index = field_index;
    return index;
}

static int refresh_page_text(page_t *page) {
    if (!page || !page->text_template) return 0;
    size_t cap = strlen(page->text_template) +
                 (size_t)page->action_count * (FORM_VALUE_MAX + 96) + 1;
    char *rendered = (char*)malloc(cap);
    if (!rendered) return 0;

    size_t used = 0;
    const char *p = page->text_template;
    while (*p) {
        if ((unsigned char)p[0] == 1 && isdigit((unsigned char)p[1]) &&
            isdigit((unsigned char)p[2]) && isdigit((unsigned char)p[3]) &&
            (unsigned char)p[4] == 2) {
            int action_index = (p[1] - '0') * 100 + (p[2] - '0') * 10 + (p[3] - '0');
            if (action_index >= 0 && action_index < page->action_count) {
                const page_action_t *action = &page->actions[action_index];
                char line[FORM_VALUE_MAX + 96];
                line[0] = 0;
                if (action->type == ACTION_FORM_FIELD &&
                    action->form_index >= 0 && action->form_index < page->form_count) {
                    const form_t *form = &page->forms[action->form_index];
                    if (action->field_index >= 0 && action->field_index < form->field_count) {
                        const form_field_t *field = &form->fields[action->field_index];
                        snprintf(line, sizeof(line), "[%d] %s: %s\n",
                                 action_index + 1, field->name, field->value);
                    }
                } else if (action->type == ACTION_FORM_SUBMIT &&
                           action->form_index >= 0 && action->form_index < page->form_count) {
                    const form_t *form = &page->forms[action->form_index];
                    if (action->field_index >= 0 && action->field_index < form->field_count) {
                        const form_field_t *field = &form->fields[action->field_index];
                        snprintf(line, sizeof(line), "[%d] [%s]\n",
                                 action_index + 1, field->label[0] ? field->label : "Submit");
                    }
                }
                if (!append_text(rendered, cap, &used, line)) { free(rendered); return 0; }
            }
            p += 5;
            continue;
        }
        if (!append_bytes(rendered, cap, &used, p, 1)) { free(rendered); return 0; }
        p++;
    }

    free(page->text);
    page->text = rendered;
    return 1;
}

static void extract_html_title(const char *html, char *out, size_t cap) {
    if (!out || !cap) return;
    out[0] = 0;
    if (!html) return;
    const char *start = find_case_insensitive(html, "<title");
    if (!start || !(start = strchr(start, '>'))) return;
    start++;
    const char *end = find_case_insensitive(start, "</title>");
    if (!end) return;
    size_t n = (size_t)(end - start);
    if (n >= cap) n = cap - 1;
    memcpy(out, start, n);
    out[n] = 0;
    trim_inplace(out);
}

static void extract_button_label(const char *start, const char *end, char *out, size_t cap) {
    size_t used = 0;
    bool spacing = false;
    if (!cap) return;
    for (const char *p = start; p < end && used + 1 < cap; p++) {
        if (*p == '<') {
            const char *tag_end = find_tag_end(p + 1, end);
            if (!tag_end) break;
            p = tag_end;
        } else if (isspace((unsigned char)*p)) {
            spacing = used > 0;
        } else {
            if (spacing && used + 1 < cap) out[used++] = ' ';
            spacing = false;
            out[used++] = *p;
        }
    }
    out[used] = 0;
    trim_inplace(out);
}

static page_t *html_to_page(const char *html, const char *base_url) {
    if (!html) return NULL;
    size_t length = strlen(html);
    size_t template_cap = length + (size_t)MAX_ACTIONS * 8 + 1;
    char *template_text = (char*)calloc(1, template_cap);
    page_t *page = (page_t*)calloc(1, sizeof(page_t));
    if (!template_text || !page) { free(template_text); free(page); return NULL; }

    strncpy(page->base, base_url ? base_url : "", URL_MAX);
    page->base[URL_MAX - 1] = 0;
    extract_html_title(html, page->title, sizeof(page->title));

    const char *html_end = html + length;
    size_t used = 0;
    bool in_head = false, in_script = false, in_style = false, in_pre = false;
    int current_form = -1;
    int ordered_depth = 0, ordered_item = 0;

    for (const char *cursor = html; cursor < html_end && *cursor; ) {
        if (*cursor != '<') {
            if (!in_head && !in_script && !in_style) {
                if (*cursor == '&') {
                    const char *next = NULL;
                    if (cursor + 1 < html_end && cursor[1] == '#')
                        next = emit_numeric_entity(cursor, template_text, &used, template_cap - 1);
                    if (!next) next = emit_entity(cursor, template_text, &used, template_cap - 1);
                    if (next) { cursor = next; continue; }
                }
                if (in_pre) {
                    if (*cursor != '\r') append_bytes(template_text, template_cap, &used, cursor, 1);
                } else if (isspace((unsigned char)*cursor)) {
                    if (used && template_text[used - 1] != ' ' && template_text[used - 1] != '\n')
                        append_text(template_text, template_cap, &used, " ");
                } else {
                    append_bytes(template_text, template_cap, &used, cursor, 1);
                }
            }
            cursor++;
            continue;
        }

        if (cursor + 4 <= html_end && !strncmp(cursor, "<!--", 4)) {
            const char *comment_end = strstr(cursor + 4, "-->");
            cursor = comment_end ? comment_end + 3 : html_end;
            continue;
        }
        if (cursor + 1 < html_end && (cursor[1] == '!' || cursor[1] == '?')) {
            const char *tag_end = find_tag_end(cursor + 2, html_end);
            cursor = tag_end ? tag_end + 1 : html_end;
            continue;
        }

        const char *p = cursor + 1;
        bool closing = false;
        if (p < html_end && *p == '/') { closing = true; p++; }
        while (p < html_end && isspace((unsigned char)*p)) p++;
        char tag[16]; size_t tag_len = 0;
        while (p < html_end && tag_len + 1 < sizeof(tag) && isalpha((unsigned char)*p))
            tag[tag_len++] = (char)tolower((unsigned char)*p++);
        tag[tag_len] = 0;
        const char *attributes = p;
        const char *tag_end = find_tag_end(attributes, html_end);
        if (!tag_end) break;
        const char *after_tag = tag_end + 1;

        if (closing) {
            if (!strcmp(tag, "head")) in_head = false;
            else if (!strcmp(tag, "script")) in_script = false;
            else if (!strcmp(tag, "style")) in_style = false;
            else if (!strcmp(tag, "form")) { current_form = -1; append_line_break(template_text, template_cap, &used); }
            else if (!strcmp(tag, "pre")) { in_pre = false; append_line_break(template_text, template_cap, &used); }
            else if (!strcmp(tag, "ol")) { if (ordered_depth > 0) ordered_depth--; }
            else if (!strcmp(tag, "code")) append_text(template_text, template_cap, &used, "`");
            else if (!strcmp(tag, "strong") || !strcmp(tag, "b")) append_text(template_text, template_cap, &used, "**");
            else if (!strcmp(tag, "em") || !strcmp(tag, "i")) append_text(template_text, template_cap, &used, "_");
            else if (!strcmp(tag, "p") || !strcmp(tag, "div") || !strcmp(tag, "section") ||
                     !strcmp(tag, "article") || !strcmp(tag, "main") || !strcmp(tag, "header") ||
                     !strcmp(tag, "footer") || !strcmp(tag, "nav") || !strcmp(tag, "aside") ||
                     !strcmp(tag, "blockquote") || !strcmp(tag, "address") || !strcmp(tag, "li") ||
                     !strcmp(tag, "h1") || !strcmp(tag, "h2") || !strcmp(tag, "h3") ||
                     !strcmp(tag, "h4") || !strcmp(tag, "h5") || !strcmp(tag, "h6") ||
                     !strcmp(tag, "tr") || !strcmp(tag, "table"))
                append_line_break(template_text, template_cap, &used);
            cursor = after_tag;
            continue;
        }

        if (!strcmp(tag, "head")) in_head = true;
        else if (!strcmp(tag, "script")) in_script = true;
        else if (!strcmp(tag, "style")) in_style = true;
        else if (!strcmp(tag, "pre")) { append_line_break(template_text, template_cap, &used); in_pre = true; }
        else if (!strcmp(tag, "br")) append_line_break(template_text, template_cap, &used);
        else if (!strcmp(tag, "p") || !strcmp(tag, "div") || !strcmp(tag, "section") ||
                 !strcmp(tag, "article") || !strcmp(tag, "main") || !strcmp(tag, "header") ||
                 !strcmp(tag, "footer") || !strcmp(tag, "nav") || !strcmp(tag, "aside") ||
                 !strcmp(tag, "blockquote") || !strcmp(tag, "address") || !strcmp(tag, "tr"))
            append_line_break(template_text, template_cap, &used);
        else if (!strcmp(tag, "h1") || !strcmp(tag, "h2") || !strcmp(tag, "h3") ||
                 !strcmp(tag, "h4") || !strcmp(tag, "h5") || !strcmp(tag, "h6")) {
            append_line_break(template_text, template_cap, &used);
            append_text(template_text, template_cap, &used, "= ");
        } else if (!strcmp(tag, "ul")) {
            ordered_depth = 0;
        } else if (!strcmp(tag, "ol")) {
            ordered_depth++;
            ordered_item = 0;
        } else if (!strcmp(tag, "li")) {
            append_line_break(template_text, template_cap, &used);
            if (ordered_depth) {
                char number[16];
                snprintf(number, sizeof(number), "%d. ", ++ordered_item);
                append_text(template_text, template_cap, &used, number);
            } else append_text(template_text, template_cap, &used, "* ");
        } else if (!strcmp(tag, "code")) append_text(template_text, template_cap, &used, "`");
        else if (!strcmp(tag, "strong") || !strcmp(tag, "b")) append_text(template_text, template_cap, &used, "**");
        else if (!strcmp(tag, "em") || !strcmp(tag, "i")) append_text(template_text, template_cap, &used, "_");
        else if (!strcmp(tag, "hr")) {
            append_line_break(template_text, template_cap, &used);
            append_text(template_text, template_cap, &used, "--------------------------------\n");
        } else if (!strcmp(tag, "td") || !strcmp(tag, "th")) {
            if (used && template_text[used - 1] != '\n' && template_text[used - 1] != ' ')
                append_text(template_text, template_cap, &used, " | ");
        } else if (!strcmp(tag, "form")) {
            append_line_break(template_text, template_cap, &used);
            if (page->form_count < MAX_FORMS) {
                current_form = page->form_count++;
                form_t *form = &page->forms[current_form];
                memset(form, 0, sizeof(*form));
                strncpy(form->method, "get", sizeof(form->method));
                tag_attribute(attributes, tag_end, "action", form->action, sizeof(form->action));
                if (tag_attribute(attributes, tag_end, "method", form->method, sizeof(form->method)))
                    lower_ascii(form->method);
            } else current_form = -1;
        } else if (!strcmp(tag, "input") && current_form >= 0) {
            form_t *form = &page->forms[current_form];
            if (form->field_count < MAX_FORM_FIELDS) {
                form_field_t field;
                memset(&field, 0, sizeof(field));
                strncpy(field.type, "text", sizeof(field.type));
                tag_attribute(attributes, tag_end, "name", field.name, sizeof(field.name));
                tag_attribute(attributes, tag_end, "value", field.value, sizeof(field.value));
                if (tag_attribute(attributes, tag_end, "type", field.type, sizeof(field.type))) lower_ascii(field.type);
                field.disabled = tag_attribute(attributes, tag_end, "disabled", NULL, 0);

                bool editable = !strcmp(field.type, "text") || !strcmp(field.type, "search") || !strcmp(field.type, "url");
                bool hidden = !strcmp(field.type, "hidden");
                bool submit = !strcmp(field.type, "submit");
                if ((editable && field.name[0]) || (hidden && field.name[0]) || submit) {
                    int field_index = form->field_count++;
                    form->fields[field_index] = field;
                    if (submit) {
                        strncpy(form->fields[field_index].label,
                                field.value[0] ? field.value : "Submit",
                                sizeof(form->fields[field_index].label));
                        if (!field.disabled) {
                            int action = add_action(page, ACTION_FORM_SUBMIT, -1, current_form, field_index);
                            if (action >= 0) append_action_marker(template_text, template_cap, &used, action);
                        }
                    } else if (editable && !field.disabled) {
                        int action = add_action(page, ACTION_FORM_FIELD, -1, current_form, field_index);
                        if (action >= 0) append_action_marker(template_text, template_cap, &used, action);
                    }
                }
            }
        } else if (!strcmp(tag, "button") && current_form >= 0) {
            form_t *form = &page->forms[current_form];
            char type[16] = "submit";
            tag_attribute(attributes, tag_end, "type", type, sizeof(type));
            lower_ascii(type);
            const char *close = find_case_insensitive(after_tag, "</button>");
            if (!strcmp(type, "submit") && form->field_count < MAX_FORM_FIELDS) {
                int field_index = form->field_count++;
                form_field_t *field = &form->fields[field_index];
                memset(field, 0, sizeof(*field));
                strncpy(field->type, "submit", sizeof(field->type));
                tag_attribute(attributes, tag_end, "name", field->name, sizeof(field->name));
                tag_attribute(attributes, tag_end, "value", field->value, sizeof(field->value));
                field->disabled = tag_attribute(attributes, tag_end, "disabled", NULL, 0);
                if (close) extract_button_label(after_tag, close, field->label, sizeof(field->label));
                if (!field->label[0]) strncpy(field->label, field->value[0] ? field->value : "Submit", sizeof(field->label));
                if (!field->disabled) {
                    int action = add_action(page, ACTION_FORM_SUBMIT, -1, current_form, field_index);
                    if (action >= 0) append_action_marker(template_text, template_cap, &used, action);
                }
            }
            if (close) {
                const char *close_end = strchr(close, '>');
                cursor = close_end ? close_end + 1 : html_end;
                continue;
            }
        } else if (!strcmp(tag, "a")) {
            char href[URL_MAX] = "";
            if (tag_attribute(attributes, tag_end, "href", href, sizeof(href)) &&
                is_supported_href(href) && page->link_count < MAX_LINKS) {
                char absolute[URL_MAX];
                resolve_url(page->base, href, absolute, sizeof(absolute));
                if (is_supported_href(absolute)) {
                    int link_index = page->link_count++;
                    strncpy(page->links[link_index].href, absolute, URL_MAX);
                    page->links[link_index].href[URL_MAX - 1] = 0;
                    int action = add_action(page, ACTION_LINK, link_index, -1, -1);
                    if (action >= 0) {
                        char number[16];
                        snprintf(number, sizeof(number), "[%d]", action + 1);
                        append_text(template_text, template_cap, &used, number);
                    }
                }
            }
        }

        cursor = after_tag;
    }

    template_text[used] = 0;
    page->text_template = template_text;
    if (!refresh_page_text(page)) {
        free(page->text_template);
        free(page);
        return NULL;
    }
    return page;
}

static int form_urlencode(const char *src, char *out, size_t out_cap) {
    static const char hex[] = "0123456789ABCDEF";
    size_t used = 0;
    if (!out_cap) return 0;
    for (size_t i = 0; src && src[i]; i++) {
        unsigned char c = (unsigned char)src[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            if (used + 1 >= out_cap) return 0;
            out[used++] = (char)c;
        } else if (c == ' ') {
            if (used + 1 >= out_cap) return 0;
            out[used++] = '+';
        } else {
            if (used + 3 >= out_cap) return 0;
            out[used++] = '%';
            out[used++] = hex[c >> 4];
            out[used++] = hex[c & 15];
        }
    }
    out[used] = 0;
    return 1;
}

static int append_url_part(char *out, size_t out_cap, const char *part) {
    size_t used = strlen(out), len = strlen(part);
    if (used + len >= out_cap) return 0;
    memcpy(out + used, part, len + 1);
    return 1;
}

static int build_get_form_url(const page_t *page, int form_index,
                              int submit_field_index, char *out, size_t out_cap) {
    if (!page || form_index < 0 || form_index >= page->form_count || !out_cap) return 0;
    const form_t *form = &page->forms[form_index];
    const char *method = form->method[0] ? form->method : "get";
    if (strcasecmp(method, "get")) return -1;

    if (form->action[0]) resolve_url(page->base, form->action, out, out_cap);
    else { strncpy(out, page->base, out_cap); out[out_cap - 1] = 0; }
    char *hash = strchr(out, '#');
    if (hash) *hash = 0;

    bool has_query = strchr(out, '?') != NULL;
    bool first_parameter = true;
    for (int i = 0; i < form->field_count; i++) {
        const form_field_t *field = &form->fields[i];
        if (field->disabled || !field->name[0]) continue;
        bool submit = !strcmp(field->type, "submit");
        bool successful = !strcmp(field->type, "text") || !strcmp(field->type, "search") ||
                          !strcmp(field->type, "url") || !strcmp(field->type, "hidden") ||
                          (submit && i == submit_field_index);
        if (!successful) continue;

        char encoded_name[sizeof(field->name) * 3 + 1];
        char encoded_value[sizeof(field->value) * 3 + 1];
        if (!form_urlencode(field->name, encoded_name, sizeof(encoded_name)) ||
            !form_urlencode(field->value, encoded_value, sizeof(encoded_value))) return 0;

        size_t current_len = strlen(out);
        char separator[2] = {0, 0};
        if (first_parameter) {
            if (!current_len || (out[current_len - 1] != '?' && out[current_len - 1] != '&'))
                separator[0] = has_query ? '&' : '?';
        } else if (current_len && out[current_len - 1] != '?' && out[current_len - 1] != '&') {
            separator[0] = '&';
        }
        if (!append_url_part(out, out_cap, separator) ||
            !append_url_part(out, out_cap, encoded_name) ||
            !append_url_part(out, out_cap, "=") ||
            !append_url_part(out, out_cap, encoded_value)) return 0;
        first_parameter = false;
        has_query = true;
    }
    return 1;
}

typedef enum {
    ACTIVATE_NONE,
    ACTIVATE_NAVIGATE,
    ACTIVATE_EDIT_FIELD,
    ACTIVATE_POST_UNSUPPORTED,
    ACTIVATE_URL_TOO_LONG
} activate_result_t;

static activate_result_t activate_page_action(
    page_t *page, int action_index, char *navigation_url, size_t navigation_cap,
    int *edit_form, int *edit_field, char *edit_buf, size_t edit_cap,
    size_t *edit_cursor) {
    if (!page || action_index < 0 || action_index >= page->action_count)
        return ACTIVATE_NONE;

    const page_action_t *action = &page->actions[action_index];
    if (action->type == ACTION_LINK) {
        if (action->link_index < 0 || action->link_index >= page->link_count)
            return ACTIVATE_NONE;
        strncpy(navigation_url, page->links[action->link_index].href, navigation_cap);
        navigation_url[navigation_cap - 1] = 0;
        return ACTIVATE_NAVIGATE;
    }

    if (action->form_index < 0 || action->form_index >= page->form_count)
        return ACTIVATE_NONE;
    form_t *form = &page->forms[action->form_index];
    if (action->field_index < 0 || action->field_index >= form->field_count)
        return ACTIVATE_NONE;

    if (action->type == ACTION_FORM_FIELD) {
        form_field_t *field = &form->fields[action->field_index];
        strncpy(edit_buf, field->value, edit_cap);
        edit_buf[edit_cap - 1] = 0;
        *edit_cursor = strlen(edit_buf);
        *edit_form = action->form_index;
        *edit_field = action->field_index;
        return ACTIVATE_EDIT_FIELD;
    }

    if (action->type == ACTION_FORM_SUBMIT) {
        int result = build_get_form_url(page, action->form_index,
                                        action->field_index,
                                        navigation_url, navigation_cap);
        if (result < 0) return ACTIVATE_POST_UNSUPPORTED;
        if (!result) return ACTIVATE_URL_TOO_LONG;
        return ACTIVATE_NAVIGATE;
    }
    return ACTIVATE_NONE;
}

static void free_page(page_t *page) {
    if (!page) return;
    free(page->text);
    free(page->text_template);
    free(page);
}

/* ---------- UTF-8 forward declaration ---------- */
static unsigned utf8_next(const char *s, size_t len, size_t *i);

/* ---------- wrap text to columns ---------- */

static char *wrap_text(const char *in, int max_cols) {
    if (!in) return NULL;

    size_t n = strlen(in);

    /*
     * Worst case we add roughly one newline per input codepoint.
     * 2*n + 8 is therefore safely large enough.
     */
    char *out = (char*)malloc(n * 2 + 8);
    if (!out) return NULL;

    size_t i = 0;
    size_t o = 0;
    int col = 0;
    int blank_run = 0;

    while (i < n) {
        size_t start = i;
        unsigned cp = utf8_next(in, n, &i);
        size_t bytes = i - start;

        if (cp == 0)
            break;

        if (cp == '\r')
            continue;

        if (cp == '\n') {
            if (col == 0) {
                if (blank_run)
                    continue;
                blank_run = 1;
            } else {
                blank_run = 0;
            }

            out[o++] = '\n';
            col = 0;
            continue;
        }

        /*
         * Normalize NBSP to an ordinary space.
         */
        if (cp == 0xA0)
            cp = ' ';

        /*
         * Collapse redundant ASCII spaces as before.
         */
        if (cp == ' ' && (col == 0 || (o > 0 && out[o - 1] == ' ')))
            continue;

        /*
         * Wrap BEFORE copying the complete UTF-8 sequence.
         *
         * Most Unicode characters count as one logical column here.
         * The renderer will still use the actual pixel width.
         */
        if (max_cols && col >= max_cols) {
            out[o++] = '\n';
            col = 0;

            /*
             * Don't start a wrapped line with a normal space.
             */
            if (cp == ' ')
                continue;
        }

        if (cp == ' ') {
            out[o++] = ' ';
        } else {
            /*
             * Copy the COMPLETE original UTF-8 sequence.
             * Never split a multi-byte character.
             */
            memcpy(out + o, in + start, bytes);
            o += bytes;
        }

        col++;
        blank_run = 0;
    }

    out[o] = 0;
    return out;
}







/* ---------- curl fetch (tolerant to trimmed-down libcurl) ---------- */
/* ---------- v1.2: proper HTTP status/error handling ---------- */

static int fetch_url(const char *url, mem_t *m, long *http_status) {
    if (!url || !m) return -1;

    CURL *curl = curl_easy_init();
    if (!curl) return -2;

    m->buf = NULL;
    m->len = 0;

    if (http_status) {
        *http_status = 0;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);

#ifdef CURLOPT_BUFFERSIZE
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 1024L);
#endif
#ifdef CURLOPT_MAX_RECV_SPEED_LARGE
    curl_easy_setopt(curl, CURLOPT_MAX_RECV_SPEED_LARGE, 32768L);
#endif
#ifdef CURLOPT_FOLLOWLOCATION
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
#endif
#ifdef CURLOPT_MAXREDIRS
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
#endif
#if defined(CURLOPT_HTTP_VERSION) && defined(CURL_HTTP_VERSION_1_1)
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
#endif
#ifdef CURLOPT_CONNECTTIMEOUT
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
#endif
#ifdef CURLOPT_TIMEOUT
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 35L);
#endif
#ifdef CURLOPT_LOW_SPEED_LIMIT
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 10L);
#endif
#ifdef CURLOPT_LOW_SPEED_TIME
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 20L);
#endif

    /*
     * Explicit request headers.
     *
     * Accept-Encoding is deliberately "identity" because this tiny browser
     * does not need compressed transfer encodings.
     */
    struct curl_slist *hdrs = NULL;

    hdrs = curl_slist_append(hdrs,
        "User-Agent: Mozilla/5.0 (BadgeVMS; ESP32; rv:2.1) "
        "Gecko/20100101 "
        "(compatible; MiniBrowser/2.1; +https://github.com/mactjaap/mini_browser/; HTTP/1.1; identity)");

    hdrs = curl_slist_append(hdrs,
        "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8");

    hdrs = curl_slist_append(hdrs,
        "Accept-Language: en-US,en;q=0.5");

    hdrs = curl_slist_append(hdrs,
        "Accept-Encoding: identity");

    if (hdrs) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    }

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, wr_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, m);

    CURLcode res = curl_easy_perform(curl);

    /*
     * Even when the HTTP server returns 404/500, curl itself can still
     * return CURLE_OK. Therefore keep the HTTP status separately.
     */
    if (http_status) {
        long code = 0;
        if (curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code) == CURLE_OK) {
            *http_status = code;
        }
    }

    /*
     * CURLOPT_HTTPHEADER does not take ownership of the curl_slist,
     * so we must release it ourselves after curl_easy_perform().
     */
    if (hdrs) {
        curl_slist_free_all(hdrs);
        hdrs = NULL;
    }

    curl_easy_cleanup(curl);

    return (res == CURLE_OK) ? 0 : (int)res;
}


/* ---------- tiny text renderer ---------- */


#define UNICODE_FONT_FILE "APPS:[mini_browser]unifont_cjk.bin"

#define UNICODE_GLYPH_BYTES 32
#define UNICODE_GLYPH_W 16
#define UNICODE_GLYPH_H 16

#define UNICODE_CACHE_SIZE 128

typedef struct {
    uint32_t codepoint;
    unsigned char bitmap[UNICODE_GLYPH_BYTES];
    bool valid;
} unicode_cache_entry_t;

static FILE *g_unicode_font = NULL;
static unicode_cache_entry_t g_unicode_cache[UNICODE_CACHE_SIZE];

/*
 * Direct-indexed ranges in unifont_cjk.bin.
 *
 * Header:
 *   12 bytes main header
 *   4 x 12-byte range records
 *
 * Glyphs start at byte 60.
 */
typedef struct {
    uint32_t start;
    uint32_t end;
    uint32_t offset;
} unicode_font_range_t;

static const unicode_font_range_t g_unicode_ranges[] = {
    { 0x3000, 0x303F,       60 },
    { 0x3040, 0x309F,     2108 },
    { 0x30A0, 0x30FF,     5180 },
    { 0x4E00, 0x9FFF,     8252 }
};

#define UNICODE_RANGE_COUNT \
    (sizeof(g_unicode_ranges) / sizeof(g_unicode_ranges[0]))

static int unicode_font_open(void) {
    if (g_unicode_font) {
        return 1;
    }

    g_unicode_font = fopen(UNICODE_FONT_FILE, "rb");

    if (!g_unicode_font) {
        printf("Mini Browser: FAILED to open Unicode font: %s\n",
               UNICODE_FONT_FILE);
        return 0;
    }

    unsigned char header[12];

    if (fread(header, 1, sizeof(header), g_unicode_font) != sizeof(header)) {
        printf("Mini Browser: Unicode font header read failed\n");
        fclose(g_unicode_font);
        g_unicode_font = NULL;
        return 0;
    }

    if (memcmp(header, "MBCJ", 4) != 0) {
        printf("Mini Browser: invalid Unicode font file\n");
        fclose(g_unicode_font);
        g_unicode_font = NULL;
        return 0;
    }

    printf("Mini Browser: Unicode CJK font opened: %s\n",
           UNICODE_FONT_FILE);

    return 1;
}

static int unicode_font_offset(unsigned cp, long *offset) {
    for (size_t i = 0; i < UNICODE_RANGE_COUNT; i++) {
        const unicode_font_range_t *range = &g_unicode_ranges[i];

        if (cp >= range->start && cp <= range->end) {
            *offset =
                (long)range->offset +
                (long)(cp - range->start) * UNICODE_GLYPH_BYTES;

            return 1;
        }
    }

    return 0;
}

static int load_unicode_glyph(unsigned cp,
                              unsigned char bitmap[UNICODE_GLYPH_BYTES]) {
    /*
     * Small direct-mapped RAM cache.
     *
     * Rendering the same screen repeatedly therefore normally requires
     * no filesystem access after the glyph has first been encountered.
     */
    unsigned cache_index = cp % UNICODE_CACHE_SIZE;
    unicode_cache_entry_t *cached = &g_unicode_cache[cache_index];

    if (cached->valid && cached->codepoint == cp) {
        memcpy(bitmap, cached->bitmap, UNICODE_GLYPH_BYTES);
        return 1;
    }

    long offset;

    if (!unicode_font_offset(cp, &offset)) {
        return 0;
    }

    if (!unicode_font_open()) {
        return 0;
    }

    if (fseek(g_unicode_font, offset, SEEK_SET) != 0) {
        return 0;
    }

    if (fread(bitmap, 1, UNICODE_GLYPH_BYTES, g_unicode_font)
        != UNICODE_GLYPH_BYTES) {
        return 0;
    }

    /*
     * An all-zero slot means that the font does not contain this glyph.
     */
    bool empty = true;

    for (int i = 0; i < UNICODE_GLYPH_BYTES; i++) {
        if (bitmap[i] != 0) {
            empty = false;
            break;
        }
    }

    if (empty) {
        return 0;
    }

    cached->codepoint = cp;
    memcpy(cached->bitmap, bitmap, UNICODE_GLYPH_BYTES);
    cached->valid = true;

    return 1;
}


static unsigned utf8_next(const char *s, size_t len, size_t *i) {
    if (*i >= len) return 0;

    const unsigned char *p = (const unsigned char *)s;
    unsigned char c = p[*i];

    if (c < 0x80) {
        (*i)++;
        return c;
    }

    if ((c & 0xE0) == 0xC0 && *i + 1 < len) {
        unsigned char c1 = p[*i + 1];

        if ((c1 & 0xC0) == 0x80) {
            unsigned cp =
                ((unsigned)(c & 0x1F) << 6) |
                (unsigned)(c1 & 0x3F);

            if (cp >= 0x80) {
                *i += 2;
                return cp;
            }
        }
    }

    if ((c & 0xF0) == 0xE0 && *i + 2 < len) {
        unsigned char c1 = p[*i + 1];
        unsigned char c2 = p[*i + 2];

        if ((c1 & 0xC0) == 0x80 &&
            (c2 & 0xC0) == 0x80) {

            unsigned cp =
                ((unsigned)(c & 0x0F) << 12) |
                ((unsigned)(c1 & 0x3F) << 6) |
                (unsigned)(c2 & 0x3F);

            if (cp >= 0x800 &&
                !(cp >= 0xD800 && cp <= 0xDFFF)) {
                *i += 3;
                return cp;
            }
        }
    }

    if ((c & 0xF8) == 0xF0 && *i + 3 < len) {
        unsigned char c1 = p[*i + 1];
        unsigned char c2 = p[*i + 2];
        unsigned char c3 = p[*i + 3];

        if ((c1 & 0xC0) == 0x80 &&
            (c2 & 0xC0) == 0x80 &&
            (c3 & 0xC0) == 0x80) {

            unsigned cp =
                ((unsigned)(c & 0x07) << 18) |
                ((unsigned)(c1 & 0x3F) << 12) |
                ((unsigned)(c2 & 0x3F) << 6) |
                (unsigned)(c3 & 0x3F);

            if (cp >= 0x10000 && cp <= 0x10FFFF) {
                *i += 4;
                return cp;
            }
        }
    }

    (*i)++;
    return 0xFFFD;
}



/* DEBUG NEW */

static int utf8_sequence_length(const unsigned char *p, size_t remaining) {
    if (!remaining) return 0;

    if (p[0] < 0x80)
        return 1;

    if ((p[0] & 0xE0) == 0xC0) {
        if (remaining < 2) return 0;
        if ((p[1] & 0xC0) != 0x80) return 0;

        unsigned cp =
            ((unsigned)(p[0] & 0x1F) << 6) |
            (unsigned)(p[1] & 0x3F);

        return cp >= 0x80 ? 2 : 0;
    }

    if ((p[0] & 0xF0) == 0xE0) {
        if (remaining < 3) return 0;
        if ((p[1] & 0xC0) != 0x80 ||
            (p[2] & 0xC0) != 0x80)
            return 0;

        unsigned cp =
            ((unsigned)(p[0] & 0x0F) << 12) |
            ((unsigned)(p[1] & 0x3F) << 6) |
            (unsigned)(p[2] & 0x3F);

        if (cp < 0x800)
            return 0;

        if (cp >= 0xD800 && cp <= 0xDFFF)
            return 0;

        return 3;
    }

    if ((p[0] & 0xF8) == 0xF0) {
        if (remaining < 4) return 0;
        if ((p[1] & 0xC0) != 0x80 ||
            (p[2] & 0xC0) != 0x80 ||
            (p[3] & 0xC0) != 0x80)
            return 0;

        unsigned cp =
            ((unsigned)(p[0] & 0x07) << 18) |
            ((unsigned)(p[1] & 0x3F) << 12) |
            ((unsigned)(p[2] & 0x3F) << 6) |
            (unsigned)(p[3] & 0x3F);

        return (cp >= 0x10000 && cp <= 0x10FFFF) ? 4 : 0;
    }

    return 0;
}

static void debug_utf8(const char *label, const char *s) {
    if (!s) {
        printf("[UTF8] %s: NULL\n", label);
        return;
    }

    size_t len = strlen(s);
    size_t i = 0;
    unsigned errors = 0;

    while (i < len) {
        int seq =
            utf8_sequence_length(
                (const unsigned char *)s + i,
                len - i);

        if (seq > 0) {
            i += (size_t)seq;
            continue;
        }

        printf("[UTF8] %s INVALID byte %u = %02X  context:",
               label,
               (unsigned)i,
               (unsigned char)s[i]);

        size_t from = i > 8 ? i - 8 : 0;
        size_t to = i + 12;
        if (to > len) to = len;

        for (size_t j = from; j < to; j++) {
            printf(" %02X", (unsigned char)s[j]);
        }

        printf("\n");

        errors++;
        i++;
    }

    printf("[UTF8] %s: %u bytes, %u invalid byte(s)\n",
           label,
           (unsigned)len,
           errors);
}



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



static void draw_unicode_char(SDL_Renderer *r, int x, int y, unsigned cp) {
    unsigned char bitmap[UNICODE_GLYPH_BYTES];

    if (!load_unicode_glyph(cp, bitmap)) {
        /*
         * Missing Unicode glyph.
         * Draw a simple 8x8 box so missing characters remain visible.
         */
        static const unsigned char box[8] = {
            0x7E,
            0x42,
            0x5A,
            0x5A,
            0x5A,
            0x42,
            0x7E,
            0x00
        };

        for (int row = 0; row < 8; row++) {
            for (int col = 0; col < 8; col++) {
                if (box[row] & (1u << (7 - col))) {
                    SDL_FRect px = {
                        (float)(x + col * FONT_SCALE),
                        (float)(y + row * FONT_SCALE),
                        (float)FONT_SCALE,
                        (float)FONT_SCALE
                    };
                    SDL_RenderFillRect(r, &px);
                }
            }
        }

        return;
    }

    /*
     * GNU Unifont CJK glyphs are 16x16 monochrome bitmaps.
     * Each row consists of two bytes, most-significant bit first.
     */
    for (int row = 0; row < UNICODE_GLYPH_H; row++) {
        uint16_t bits =
            ((uint16_t)bitmap[row * 2] << 8) |
            (uint16_t)bitmap[row * 2 + 1];

        for (int col = 0; col < UNICODE_GLYPH_W; col++) {
            if (bits & ((uint16_t)1 << (15 - col))) {
                SDL_FRect px = {
                    (float)(x + col),
                    (float)(y + row),
                    1.0f,
                    1.0f
                };

                SDL_RenderFillRect(r, &px);
            }
        }
    }
}




static void draw_text(SDL_Renderer *r, int x, int y, const char *s, int max_w) {
    if (!r || !s) return;

    int cx = x;
    int cy = y;
    size_t i = 0;
    size_t L = strlen(s);

    while (i < L) {
        unsigned cp = utf8_next(s, L, &i);

        if (cp == 0)
            break;

        if (cp == '\n') {
            cx = x;
            cy += (CH_H + LINE_SPACING);
            continue;
        }

        if (cp == 0xA0)
            cp = ' ';

        /*
         * Existing ASCII path.
         */
        if (cp >= 32 && cp <= 126) {
            int char_w = (FONT_W_COLS + FONT_COL_GAP) * FONT_SCALE;

            if (cx + char_w > x + max_w) {
                cx = x;
                cy += (CH_H + LINE_SPACING);
            }

            draw_char(r, cx, cy, (char)cp);
            cx += char_w;
            continue;
        }

        /*
         * Temporary Unicode test path.
         */
        if (cp >= 0x80 && cp <= 0x10FFFF) {
            int char_w = UNICODE_GLYPH_W + 1;
            if (cx + char_w > x + max_w) {
                cx = x;
                cy += (CH_H + LINE_SPACING);
            }

            draw_unicode_char(r, cx, cy, cp);
            cx += char_w;
        }
    }
}



/* ---------- logo (cyan square + yellow magnifying glass) ---------- */
static void draw_filled_circle_i(SDL_Renderer *r, int cx, int cy, int R) {
    int x = 0, y = R;
    int d = 1 - R;
    while (y >= x) {
        int x0 = cx - x, x1 = cx + x;
        int y0 = cy - y, y1 = cy + y;
        int y0r = cy - x, y1r = cy + x;

        SDL_FRect s1 = { (float)x0, (float)y0, (float)(x1 - x0 + 1), 1.0f };
        SDL_FRect s2 = { (float)x0, (float)y1, (float)(x1 - x0 + 1), 1.0f };
        SDL_FRect s3 = { (float)(cx - y), (float)y0r, (float)(2*y + 1), 1.0f };
        SDL_FRect s4 = { (float)(cx - y), (float)y1r, (float)(2*y + 1), 1.0f };
        SDL_RenderFillRect(r, &s1);
        SDL_RenderFillRect(r, &s2);
        SDL_RenderFillRect(r, &s3);
        SDL_RenderFillRect(r, &s4);

        x++;
        if (d < 0) d += 2*x + 1;
        else { y--; d += 2*(x - y) + 1; }
    }
}
static void draw_logo(SDL_Renderer *r) {
    if (!r) return;
    /* Cyan square background at (ICON_LEFT, ICON_TOP), ICON_SIZE x ICON_SIZE */
    SDL_SetRenderDrawColor(r, 0, 180, 180, 255);
    SDL_FRect bg = { (float)ICON_LEFT, (float)ICON_TOP, (float)ICON_SIZE, (float)ICON_SIZE };
    SDL_RenderFillRect(r, &bg);

    /* Yellow magnifying glass: circle + short handle */
    int cx = ICON_LEFT + ICON_SIZE/2;   /* center within square */
    int cy = ICON_TOP  + ICON_SIZE/2;
    int R  = 6;

    SDL_SetRenderDrawColor(r, 255, 255, 0, 255);   /* yellow circle */
    draw_filled_circle_i(r, cx, cy, R);

    /* 2px thick handle going down-right */
    SDL_SetRenderDrawColor(r, 255, 255, 0, 255);   /* yellow handle */
    SDL_FRect handle = { (float)(cx + R - 1), (float)(cy + R - 1), 6.0f, 2.0f };
    SDL_RenderFillRect(r, &handle);
}

/* --- draw URL bar text, clipped from the LEFT, starting at URL_TEXT_X --- */
static void draw_bar(SDL_Renderer *r, const char *text) {
    int max_cols = (VIEW_W - URL_TEXT_X - PAD_LR) / CH_W;
    if (max_cols < 1) max_cols = 1;

    size_t n = strlen(text);
    char tmp[URL_MAX + 32];
    if ((int)n > max_cols) {
        const char *start = text + (n - (size_t)max_cols + 3);
        snprintf(tmp, sizeof tmp, "...%s", start);
        draw_text(r, URL_TEXT_X, 4, tmp, VIEW_W - URL_TEXT_X - PAD_LR);
    } else {
        draw_text(r, URL_TEXT_X, 4, text, VIEW_W - URL_TEXT_X - PAD_LR);
    }
}

/* ---------- UI ---------- */
static void draw_ui(SDL_Renderer *r, const char *bar_text) {
    if (!r) return;
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_RenderClear(r);

    SDL_FRect top = (SDL_FRect){0, 0, VIEW_W, URLBAR_H};
    SDL_SetRenderDrawColor(r, 30, 30, 30, 255);
    SDL_RenderFillRect(r, &top);

    /* logo first so text draws alongside it */
    draw_logo(r);

    SDL_SetRenderDrawColor(r, 220, 220, 220, 255);
    draw_bar(r, bar_text);

    SDL_FRect mid = (SDL_FRect){0, URLBAR_H + 1, VIEW_W, 2};
    SDL_SetRenderDrawColor(r, 60, 60, 60, 255);
    SDL_RenderFillRect(r, &mid);
}

static int is_printable_ascii(const char *text) {
    if (!text || !*text) return 0;
    unsigned char c = (unsigned char)text[0];
    return (c >= 32 && c <= 126);
}

/* ---------- bookmarks ---------- */
#define BOOKMARK_MAX 32

typedef struct {
    char url[URL_MAX];
    char title[128];
} bookmark_t;

static bookmark_t g_bookmarks[BOOKMARK_MAX];
static int g_bookmark_count = 0;

static int bookmark_find(const char *url) {
    if (!url || !*url) return -1;

    for (int i = 0; i < g_bookmark_count; i++) {
        if (strncmp(g_bookmarks[i].url, url, URL_MAX) == 0) {
            return i;
        }
    }

    return -1;
}

static int bookmark_add(const char *url, const char *title) {
    if (!url || !*url) return 0;

    /* Already bookmarked. */
    if (bookmark_find(url) >= 0) return 0;

    if (g_bookmark_count >= BOOKMARK_MAX) return 0;

    bookmark_t *bm = &g_bookmarks[g_bookmark_count];

    strncpy(bm->url, url, URL_MAX);
    bm->url[URL_MAX - 1] = 0;

    if (title && *title) {
        strncpy(bm->title, title, sizeof(bm->title));
        bm->title[sizeof(bm->title) - 1] = 0;
    } else {
        strncpy(bm->title, url, sizeof(bm->title));
        bm->title[sizeof(bm->title) - 1] = 0;
    }

    g_bookmark_count++;

    printf("[mini_browser] bookmark added: %s\n", bm->url);

    return 1;
}

static int bookmark_remove(const char *url) {
    int idx = bookmark_find(url);
    if (idx < 0) return 0;

    if (idx < g_bookmark_count - 1) {
        memmove(&g_bookmarks[idx],
                &g_bookmarks[idx + 1],
                sizeof(g_bookmarks[0]) *
                    (size_t)(g_bookmark_count - idx - 1));
    }

    g_bookmark_count--;

    printf("[mini_browser] bookmark removed: %s\n", url);

    return 1;
}
static int bookmark_toggle(const char *url, const char *title) {
    if (bookmark_find(url) >= 0) {
        bookmark_remove(url);
        return 0;
    }

    bookmark_add(url, title);
    return 1;
}

/* ---------- persistent bookmarks ---------- */
#define BOOKMARK_FILE "APPS:[mini_browser]bookmarks.txt"

static void bookmark_save(void) {
    /*
     * BadgeVMS currently has a truncation issue with fopen(..., "w"),
     * so follow the same pattern used by the WHY2025 name badge:
     * remove the old file before creating the new one.
     */
    remove(BOOKMARK_FILE);

    FILE *file = fopen(BOOKMARK_FILE, "w");
    if (!file) {
        printf("[mini_browser] failed to save bookmarks to %s\n",
               BOOKMARK_FILE);
        return;
    }

    for (int i = 0; i < g_bookmark_count; i++) {
        fprintf(file, "%s\n", g_bookmarks[i].url);
        fprintf(file, "%s\n", g_bookmarks[i].title);
    }

    fclose(file);

    printf("[mini_browser] saved %d bookmarks to %s\n",
           g_bookmark_count,
           BOOKMARK_FILE);
}

static void bookmark_load(void) {
    FILE *file = fopen(BOOKMARK_FILE, "r");

    if (!file) {
        printf("[mini_browser] no saved bookmarks at %s\n",
               BOOKMARK_FILE);
        return;
    }

    g_bookmark_count = 0;

    char url[URL_MAX];
    char title[128];

    while (g_bookmark_count < BOOKMARK_MAX) {
        if (!fgets(url, sizeof(url), file)) {
            break;
        }

        if (!fgets(title, sizeof(title), file)) {
            break;
        }

        size_t len = strlen(url);
        while (len > 0 &&
               (url[len - 1] == '\n' || url[len - 1] == '\r')) {
            url[--len] = 0;
        }

        len = strlen(title);
        while (len > 0 &&
               (title[len - 1] == '\n' || title[len - 1] == '\r')) {
            title[--len] = 0;
        }

        if (!url[0]) {
            continue;
        }

        bookmark_add(url, title);
    }

    fclose(file);

    printf("[mini_browser] loaded %d bookmarks from %s\n",
           g_bookmark_count,
           BOOKMARK_FILE);
}

static page_t *bookmarks_to_page(void) {



    page_t *pg = (page_t*)calloc(1, sizeof(page_t));
    if (!pg) return NULL;

    strncpy(pg->base, "bookmarks:", URL_MAX);
    pg->base[URL_MAX - 1] = 0;

    strncpy(pg->title, "Bookmarks", sizeof(pg->title));
    pg->title[sizeof(pg->title) - 1] = 0;

    size_t cap = 256 + (size_t)g_bookmark_count * 512;
    char *text = (char*)malloc(cap);

    if (!text) {
        free(pg);
        return NULL;
    }

    size_t used = 0;

    int n = snprintf(text, cap,
                     "= BOOKMARKS =\n\n");

    if (n > 0) used = (size_t)n;

    if (g_bookmark_count == 0) {
        snprintf(text + used, cap - used,
                 "No bookmarks yet.\n\n"
                 "Press WHY+F on a web page to add one.");

        pg->text = text;
        return pg;
    }

    for (int i = 0;
         i < g_bookmark_count && i < MAX_LINKS;
         i++) {

        bookmark_t *bm = &g_bookmarks[i];

        pg->links[pg->link_count] = (link_t){0};

        strncpy(pg->links[pg->link_count].href,
                bm->url,
                URL_MAX);

        pg->links[pg->link_count].href[URL_MAX - 1] = 0;
        int link_index = pg->link_count++;
        add_action(pg, ACTION_LINK, link_index, -1, -1);

        n = snprintf(text + used,
                     cap - used,
                     "[%d] %s\n    %s\n\n",
                     i + 1,
                     bm->title,
                     bm->url);

        if (n < 0) break;

        if ((size_t)n >= cap - used) {
            used = cap - 1;
            break;
        }

        used += (size_t)n;
    }

    text[cap - 1] = 0;
    pg->text = text;

    return pg;
}

/* ---------- history with Back/Forward navigation ---------- */
#define HISTORY_MAX 32
static char g_hist[HISTORY_MAX][URL_MAX];
static int  g_hist_len = 0;
static int  g_hist_pos = -1;  /* -1 = nothing, 0..len-1 = current entry index */

/*
 * INVARIANT: g_hist_pos is the zero-based index of the currently displayed
 * history entry. g_hist_len is the total number of entries.
 *
 * Example: HOME -> A -> B -> C
 * g_hist[0] = HOME, g_hist[1] = A, g_hist[2] = B, g_hist[3] = C
 * g_hist_len = 4, g_hist_pos = 3 (pointing at C)
 */

static void history_push(const char *u) {
    if (!u || !*u) return;
    
    /* Case A: duplicate of current entry - do nothing */
    if (g_hist_pos >= 0 && strncmp(g_hist[g_hist_pos], u, URL_MAX) == 0) {
        return;
    }
    
    /* Case B: user was in forward state, truncate forward branch */
    if (g_hist_pos < g_hist_len - 1) {
        g_hist_len = g_hist_pos + 1;
    }
    
    /* Case C/D: append new entry */
    if (g_hist_len < HISTORY_MAX) {
        /* Normal append */
        strncpy(g_hist[g_hist_len], u, URL_MAX);
        g_hist[g_hist_len][URL_MAX-1] = 0;
        g_hist_pos = g_hist_len;
        g_hist_len++;
    } else {
        /* Full: shift down, append at end */
        memmove(g_hist, g_hist + 1, sizeof(g_hist[0]) * (HISTORY_MAX - 1));
        strncpy(g_hist[HISTORY_MAX - 1], u, URL_MAX);
        g_hist[HISTORY_MAX - 1][URL_MAX - 1] = 0;
        if (g_hist_pos > 0) g_hist_pos--;
        g_hist_pos = HISTORY_MAX - 1;
        g_hist_len = HISTORY_MAX;
    }
}

static int history_back(char *out) {
    if (g_hist_pos <= 0) return 0;  /* Can't go back from first entry */
    g_hist_pos--;
    strncpy(out, g_hist[g_hist_pos], URL_MAX);
    out[URL_MAX - 1] = 0;
    return 1;
}

static int history_forward(char *out) {
    if (g_hist_pos < 0 || g_hist_pos + 1 >= g_hist_len) return 0;
    g_hist_pos++;
    strncpy(out, g_hist[g_hist_pos], URL_MAX);
    out[URL_MAX - 1] = 0;
    return 1;
}

/* ---------- main ---------- */
int main(void) {
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

    /* Load persistent bookmarks from BadgeVMS storage. */
    bookmark_load();

    char url_buf[URL_MAX] = HOME_URL;


    char barline[URL_MAX + 64];
    char *content_wrapped = NULL;
    page_t *page = NULL;
    int  scroll_lines = 0;
    int  need_fetch = 1;
    int  sel_action = -1;
    long last_http_status = 0;
    bool url_editing = false;
    size_t url_cursor = 0;
    bool form_editing = false;
    int form_edit_form = -1;
    int form_edit_field = -1;
    char form_edit_buf[FORM_VALUE_MAX] = "";
    size_t form_edit_cursor = 0;

    /* URL to return to when leaving the bookmarks page with WHY+B. */
    char bookmark_return_url[URL_MAX] = "";
    bool viewing_bookmarks = false;

    /* Temporary message shown in the top bar. */
    char status_message[64] = "";
    Uint64 status_message_until = 0;

    /* Hold-to-scroll state for UP/DOWN arrow keys */
    bool scroll_up_held = false;
    bool scroll_down_held = false;
    Uint64 scroll_repeat_at = 0;

    /* Numbered link navigation state */
    char link_number_buf[8] = "";
    int link_number_len = 0;
    bool link_number_mode = false;

    /* History navigation flag - true when restoring from Back/Forward */
    bool history_navigation = false;

#if defined(ESP_PLATFORM)
    esp_log_level_set("ESP_CURL",        ESP_LOG_ERROR);
    esp_log_level_set("HTTP_CLIENT",     ESP_LOG_ERROR);
    esp_log_level_set("transport_base",  ESP_LOG_ERROR);
#endif

    bool accel_down = false;        /* WHY key held */
    bool inhibit_text_once = false; /* swallow TEXT_INPUT after commands */

    const int max_cols = (VIEW_W - 2*PAD_LR) / CH_W;
    const int lines_per_page = (VIEW_H - PAD_TOP - PAD_BOTTOM) / (CH_H + LINE_SPACING);

    SDL_StartTextInput(win);

    snprintf(barline, sizeof(barline), "%s", url_buf);
    draw_ui(ren, barline);
    SDL_RenderPresent(ren);

    int running = 1;
    while (running) {

                if (need_fetch) {
            url_editing = false;
            url_cursor = 0;
            form_editing = false;
            form_edit_form = -1;
            form_edit_field = -1;
            link_number_mode = false;
            link_number_len = 0;
            link_number_buf[0] = '\0';
            trim_inplace(url_buf);
                if (!url_buf[0]) { need_fetch = 0; }
            else {
                if (!has_scheme(url_buf) && !(url_buf[0]=='/' && url_buf[1]=='/')) {
                    normalize_typed_url(url_buf);
                }
                if (url_buf[0]=='/' && url_buf[1]=='/') {
                    char sch[16]; scheme_from_url(page ? page->base : "https://example.org", sch, sizeof sch);
                    char tmp[URL_MAX]; snprintf(tmp, sizeof tmp, "%s:%s", sch, url_buf);
                    strncpy(url_buf, tmp, URL_MAX); url_buf[URL_MAX-1]=0;
                }
                if (is_http_scheme(url_buf)) {
                    /* record in history just before fetching (unless reloading) */
                    if (!history_navigation) {
                        history_push(url_buf);
                    }
                    history_navigation = false;

                    snprintf(barline, sizeof(barline), "%s", url_buf);
                    draw_ui(ren, barline);
                    SDL_RenderPresent(ren);

                    /* start edit v1.2 */
                    mem_t m = {0};
                    long http_status = 0;

                    int rc = fetch_url(url_buf, &m, &http_status);
                    last_http_status = http_status;

                    if (rc != 0) {
                        printf("[mini_browser] fetch error %d URL='%s'\n",
                               rc, url_buf);

                        if (page) {
                            free_page(page);
                            page = NULL;
                        }

                        free(content_wrapped);
                        content_wrapped = NULL;

                        char error_text[512];
                        const char *curl_error = curl_easy_strerror((CURLcode)rc);

                        snprintf(error_text, sizeof(error_text),
                                 "CONNECTION FAILED\n\n"
                                 "Could not load:\n%s\n\n"
                                 "Reason:\n%s\n\n"
                                 "Press WHY+R to retry, WHY+B to go back, WHY+H for homepage",
                                 url_buf,
                                 curl_error ? curl_error : "Unknown network error");

                        content_wrapped = wrap_text(error_text, max_cols);
                        scroll_lines = 0;
                        sel_action = -1;

                    } else if (http_status >= 400) {
                        printf("[mini_browser] HTTP %ld URL='%s'\n",
                               http_status, url_buf);

                        if (page) {
                            free_page(page);
                            page = NULL;
                        }

                        free(content_wrapped);
                        content_wrapped = NULL;

                        char error_text[512];
                        snprintf(error_text, sizeof(error_text),
                                 "HTTP ERROR %ld\n\n"
                                 "The server returned HTTP status %ld.\n\n"
                                 "URL:\n%s\n\n"
                                 "Press WHY+B to go back or WHY+R to retry.",
                                 http_status,
                                 http_status,
                                 url_buf);

                        content_wrapped = wrap_text(error_text, max_cols);
                        scroll_lines = 0;
                        sel_action = -1;

			} else {
    			debug_utf8("CURL RAW", m.buf ? m.buf : "");

    			page_t *pg = html_to_page(m.buf ? m.buf : "", url_buf);

                        if (!pg) {
                            if (page) {
                                free_page(page);
                                page = NULL;
                            }

                            free(content_wrapped);
                            content_wrapped = NULL;

                            char error_text[256];
                            snprintf(error_text, sizeof(error_text),
                                     "PARSE ERROR\n\n"
                                     "The page was downloaded but could not "
                                     "be converted to text.");

                            content_wrapped = wrap_text(error_text, max_cols);
                            scroll_lines = 0;
                            sel_action = -1;

                            
			} else {
   			 debug_utf8("PAGE TEXT", pg->text);

    			char *wrapped = wrap_text(pg->text, max_cols);

    			debug_utf8("WRAPPED", wrapped);

    			free(content_wrapped);
                            content_wrapped = wrapped;

                            free_page(page);

                            page = pg;
                            scroll_lines = 0;
                            sel_action = -1;

                            printf("[mini_browser] HTTP %ld, %u bytes, %d links from %s\n",
                                   http_status,
                                   (unsigned)m.len,
                                   page->link_count,
                                   url_buf);

                            if (wrapped) {
                                printf("\n--- CONTENT START ---\n%s\n--- CONTENT END ---\n",
                                       wrapped);
                            }
                        }
                    }

                    free(m.buf);
                    /* End edit v1.2 */

                }
            }
            need_fetch = 0;
        }

/* Compose bar text */
        if (status_message[0] &&
            SDL_GetTicks() < status_message_until) {

            snprintf(barline, sizeof(barline),
                      "%s",
                      status_message);

        } else if (form_editing && page &&
                   form_edit_form >= 0 && form_edit_form < page->form_count &&
                   form_edit_field >= 0 &&
                   form_edit_field < page->forms[form_edit_form].field_count) {
            const char *name = page->forms[form_edit_form].fields[form_edit_field].name;
            snprintf(barline, sizeof(barline), "%s: %.*s|%s",
                     name, (int)form_edit_cursor, form_edit_buf,
                     form_edit_buf + form_edit_cursor);

        } else if (link_number_mode && link_number_len > 0) {
            snprintf(barline, sizeof(barline),
                      "%s",
                      link_number_buf);

        } else if (url_editing) {
            size_t curlen = strlen(url_buf);

            if (url_cursor > curlen) {
                url_cursor = curlen;
            }


            snprintf(barline, sizeof(barline),
                     "%.*s|%s",
                     (int)url_cursor,
                     url_buf,
                     url_buf + url_cursor);

        } else if (page && sel_action >= 0 && sel_action < page->action_count) {
            const page_action_t *action = &page->actions[sel_action];
            if (action->type == ACTION_LINK && action->link_index >= 0 &&
                action->link_index < page->link_count) {
                snprintf(barline, sizeof(barline), "[%d/%d]  %s",
                         sel_action + 1, page->action_count,
                         page->links[action->link_index].href);
            } else {
                snprintf(barline, sizeof(barline), "[%d/%d]",
                         sel_action + 1, page->action_count);
            }

        } else if (page && page->title[0] && last_http_status > 0) {
            snprintf(barline, sizeof(barline), "%ld  %s",
                     last_http_status,
                     page->title);

        } else if (last_http_status > 0) {
            snprintf(barline, sizeof(barline), "%ld  %s",
                     last_http_status,
                     url_buf);

        } else {
            snprintf(barline, sizeof(barline), "%s", url_buf);
        }

        /* Draw UI + visible text slice */
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

        /* Events */
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) { running = 0; break; }

            if (ev.type == SDL_EVENT_TEXT_INPUT) {
                if (inhibit_text_once) {
                    inhibit_text_once = false;
                    continue;
                }

                const char *t = ev.text.text;

                if (form_editing && is_printable_ascii(t)) {
                    size_t edit_len = strlen(form_edit_buf);
                    if (form_edit_cursor > edit_len) form_edit_cursor = edit_len;
                    if (edit_len < sizeof(form_edit_buf) - 1) {
                        memmove(&form_edit_buf[form_edit_cursor + 1],
                                &form_edit_buf[form_edit_cursor],
                                edit_len - form_edit_cursor + 1);
                        form_edit_buf[form_edit_cursor++] = t[0];
                    }
                } else if (url_editing && is_printable_ascii(t)) {
                    size_t curlen = strlen(url_buf);

                    if (url_cursor > curlen) {
                        url_cursor = curlen;
                    }

                    if (curlen < URL_MAX - 1) {
                        memmove(&url_buf[url_cursor + 1],
                                &url_buf[url_cursor],
                                curlen - url_cursor + 1);

                        url_buf[url_cursor] = t[0];
                        url_cursor++;
                        sel_action = -1;
                    }
                } else if (page && page->action_count > 0 &&
                           t[0] >= '0' && t[0] <= '9') {
                    if (!link_number_mode) {
                        link_number_mode = true;
                        link_number_len = 0;
                        link_number_buf[0] = '\0';
                    }
                    if (link_number_len < (int)sizeof(link_number_buf) - 1) {
                        link_number_buf[link_number_len++] = t[0];
                        link_number_buf[link_number_len] = '\0';
                    }
                }
            }
            
            if (ev.type == SDL_EVENT_KEY_DOWN) {
                SDL_Scancode sc = ev.key.scancode;

                /* Track accelerator press/release */
                if (sc == SC_ACCELERATOR) {
                    accel_down = true;
                    inhibit_text_once = true;
                    continue;
                }

                /* Special one-shot keys -> direct navigate */
                if (sc == SC_SPECIAL_124) {  strncpy(url_buf, SPECIAL_URL_124, URL_MAX); url_buf[URL_MAX-1]=0; need_fetch=1; sel_action=-1; inhibit_text_once=true; continue; }
                if (sc == SC_SPECIAL_125) {  strncpy(url_buf, SPECIAL_URL_125, URL_MAX); url_buf[URL_MAX-1]=0; need_fetch=1; sel_action=-1; inhibit_text_once=true; continue; }
                if (sc == SC_SPECIAL_126) {  strncpy(url_buf, SPECIAL_URL_126, URL_MAX); url_buf[URL_MAX-1]=0; need_fetch=1; sel_action=-1; inhibit_text_once=true; continue; }
                if (sc == SC_SPECIAL_127) {  strncpy(url_buf, SPECIAL_URL_127, URL_MAX); url_buf[URL_MAX-1]=0; need_fetch=1; sel_action=-1; inhibit_text_once=true; continue; }
                if (sc == SC_SPECIAL_128) {  strncpy(url_buf, SPECIAL_URL_128, URL_MAX); url_buf[URL_MAX-1]=0; need_fetch=1; sel_action=-1; inhibit_text_once=true; continue; }
                if (sc == SC_SPECIAL_129) {  strncpy(url_buf, SPECIAL_URL_129, URL_MAX); url_buf[URL_MAX-1]=0; need_fetch=1; sel_action=-1; inhibit_text_once=true; continue; }

                 
                /* Accelerator combos (E,C,H,R,F,M,B,Q) */       
                 if (accel_down) {
                    switch (sc) {
                        case SDL_SCANCODE_E:
                            form_editing = false;
                            form_edit_form = -1;
                            form_edit_field = -1;
                            strncpy(url_buf, "https://", URL_MAX);
                            url_buf[URL_MAX-1] = 0;
                            url_cursor = strlen(url_buf);
                            sel_action = -1;
                            url_editing = true;
                            link_number_mode = false;
                            link_number_len = 0;
                            link_number_buf[0] = '\0';
                            inhibit_text_once = true;
                            break;
                        case SDL_SCANCODE_C:
                            form_editing = false;
                            form_edit_form = -1;
                            form_edit_field = -1;
                            url_cursor = strlen(url_buf);
                            sel_action = -1;
                            url_editing = true;
                            link_number_mode = false;
                            link_number_len = 0;
                            link_number_buf[0] = '\0';
                            inhibit_text_once = true;
                            break;

                        case SDL_SCANCODE_H:
                            
                            strncpy(url_buf, HOME_URL, URL_MAX);
                            url_buf[URL_MAX-1] = 0;
                            need_fetch = 1;
                            sel_action = -1;
                            inhibit_text_once = true;
                            break;
                        case SDL_SCANCODE_R:
                            history_navigation = true;
                            need_fetch = 1;
                            inhibit_text_once = true;
                            break;

                        case SDL_SCANCODE_M: { /* SHOW BOOKMARKS */
                            /*
                             * Remember the page we were viewing. If WHY+M is
                             * pressed again while already in bookmarks, keep
                             * the original return URL.
                             */
                            if (!viewing_bookmarks &&
                                is_http_scheme(url_buf)) {

                                strncpy(bookmark_return_url,
                                        url_buf,
                                        URL_MAX);

                                bookmark_return_url[URL_MAX - 1] = 0;
                            }

                            page_t *pg = bookmarks_to_page();

                            if (pg) {
                                char *wrapped = wrap_text(pg->text, max_cols);

                                free(content_wrapped);
                                content_wrapped = wrapped;

                                if (page) {
                                    free_page(page);
                                }

                                page = pg;

                                strncpy(url_buf, "bookmarks:", URL_MAX);
                                url_buf[URL_MAX - 1] = 0;

                                last_http_status = 0;
                                scroll_lines = 0;
                                sel_action = -1;
                                url_editing = false;
                                viewing_bookmarks = true;

                                printf("[mini_browser] opened bookmarks: %d entries\n",
                                       g_bookmark_count);
                            }

                            inhibit_text_once = true;
                            break;
                        }

                       
                        case SDL_SCANCODE_F: { /* BOOKMARK current page */
                            const char *title =
                                (page && page->title[0])
                                    ? page->title
                                    : url_buf;

                            int added = bookmark_toggle(url_buf, title);

                            bookmark_save();

                            snprintf(status_message,
                                     sizeof(status_message),
                                     "%s",
                                     added
                                         ? "BOOKMARK ADDED"
                                         : "BOOKMARK REMOVED");

                            status_message_until =
                                SDL_GetTicks() + 1500;

                            printf("[mini_browser] %s bookmark: %s\n",
                                   added ? "added" : "removed",
                                   url_buf);

                            inhibit_text_once = true;
                            break;
                        }
 
                        case SDL_SCANCODE_B: { /* BACK */
                            if (viewing_bookmarks &&
                                bookmark_return_url[0]) {

                                strncpy(url_buf,
                                        bookmark_return_url,
                                        URL_MAX);

                                url_buf[URL_MAX - 1] = 0;
                                bookmark_return_url[0] = 0;

                                viewing_bookmarks = false;
                                history_navigation = true;
                                need_fetch = 1;
                                sel_action = -1;

                            } else {
                                char prev[URL_MAX];

                                if (history_back(prev)) {
                                    strncpy(url_buf, prev, URL_MAX);
                                    url_buf[URL_MAX-1] = 0;
                                    history_navigation = true;
                                    need_fetch = 1;
                                    sel_action = -1;
                                }
                            }

                            inhibit_text_once = true;
                            break;
                        }

                        case SDL_SCANCODE_G: { /* FORWARD */
                            char next_url[URL_MAX];
                            if (history_forward(next_url)) {
                                strncpy(url_buf, next_url, URL_MAX);
                                url_buf[URL_MAX-1] = 0;
                                history_navigation = true;
                                need_fetch = 1;
                                sel_action = -1;
                            }
                            inhibit_text_once = true;
                            break;
                        }

                        case SDL_SCANCODE_Q:
                            running = 0;
                            inhibit_text_once = true;
                            break;
                        default:
                            break;
                    }
                    continue;
                }

                /* Normal keys (no accelerator) */
                switch (sc) {
                    /* URL actions */

                    case SDL_SCANCODE_RETURN:
                    case SDL_SCANCODE_KP_ENTER:
                        if (form_editing) {
                            if (page && form_edit_form >= 0 &&
                                form_edit_form < page->form_count &&
                                form_edit_field >= 0 &&
                                form_edit_field < page->forms[form_edit_form].field_count) {
                                form_field_t *field =
                                    &page->forms[form_edit_form].fields[form_edit_field];
                                strncpy(field->value, form_edit_buf, sizeof(field->value));
                                field->value[sizeof(field->value) - 1] = 0;
                                if (refresh_page_text(page)) {
                                    char *wrapped = wrap_text(page->text, max_cols);
                                    if (wrapped) {
                                        free(content_wrapped);
                                        content_wrapped = wrapped;
                                    }
                                }
                            }
                            form_editing = false;
                            form_edit_form = -1;
                            form_edit_field = -1;
                        } else if (url_editing) {
                            url_editing = false;
                            link_number_mode = false;
                            link_number_len = 0;
                            link_number_buf[0] = '\0';
                            need_fetch = 1;
                        } else if (page && (link_number_mode || sel_action >= 0)) {
                            int action_index = sel_action;
                            if (link_number_mode) {
                                int action_number = atoi(link_number_buf);
                                action_index = action_number - 1;
                            }
                            link_number_mode = false;
                            link_number_len = 0;
                            link_number_buf[0] = '\0';
                            activate_result_t result = activate_page_action(
                                page, action_index, url_buf, sizeof(url_buf),
                                &form_edit_form, &form_edit_field,
                                form_edit_buf, sizeof(form_edit_buf),
                                &form_edit_cursor);
                            if (result == ACTIVATE_EDIT_FIELD) {
                                form_editing = true;
                                url_editing = false;
                            } else if (result == ACTIVATE_NAVIGATE) {
                                viewing_bookmarks = false;
                                bookmark_return_url[0] = 0;
                                history_navigation = false;
                                need_fetch = 1;
                            } else if (result == ACTIVATE_POST_UNSUPPORTED) {
                                snprintf(status_message, sizeof(status_message),
                                         "POST FORMS NOT SUPPORTED");
                                status_message_until = SDL_GetTicks() + 2000;
                            } else if (result == ACTIVATE_URL_TOO_LONG) {
                                snprintf(status_message, sizeof(status_message),
                                         "FORM URL TOO LONG");
                                status_message_until = SDL_GetTicks() + 2000;
                            }
                        } else {
                            need_fetch = 1;
                        }
                        break;
                    case SDL_SCANCODE_BACKSPACE:
                        if (form_editing) {
                            size_t edit_len = strlen(form_edit_buf);
                            if (form_edit_cursor > edit_len) form_edit_cursor = edit_len;
                            if (form_edit_cursor > 0) {
                                memmove(&form_edit_buf[form_edit_cursor - 1],
                                        &form_edit_buf[form_edit_cursor],
                                        edit_len - form_edit_cursor + 1);
                                form_edit_cursor--;
                            }
                        } else if (url_editing) {
                            size_t curlen = strlen(url_buf);

                            if (url_cursor > curlen) {
                                url_cursor = curlen;
                            }

                            if (url_cursor > 0) {
                                memmove(&url_buf[url_cursor - 1],
                                        &url_buf[url_cursor],
                                        curlen - url_cursor + 1);

                                url_cursor--;
                            }

                            sel_action = -1;
                        }
                        break;

                    case SDL_SCANCODE_DELETE:
                        if (form_editing) {
                            size_t edit_len = strlen(form_edit_buf);
                            if (form_edit_cursor > edit_len) form_edit_cursor = edit_len;
                            if (form_edit_cursor < edit_len) {
                                memmove(&form_edit_buf[form_edit_cursor],
                                        &form_edit_buf[form_edit_cursor + 1],
                                        edit_len - form_edit_cursor);
                            }
                        } else if (url_editing) {
                            size_t curlen = strlen(url_buf);

                            if (url_cursor > curlen) {
                                url_cursor = curlen;
                            }

                            if (url_cursor < curlen) {
                                memmove(&url_buf[url_cursor],
                                        &url_buf[url_cursor + 1],
                                        curlen - url_cursor);
                            }

                            sel_action = -1;
                        }
                        break;

                    case SDL_SCANCODE_LEFT:
                        if (form_editing) {
                            if (form_edit_cursor > 0) form_edit_cursor--;
                        } else if (url_editing) {
                            if (url_cursor > 0) {
                                url_cursor--;
                            }
                        }
                        break;

                    case SDL_SCANCODE_RIGHT:
                        if (form_editing) {
                            if (form_edit_cursor < strlen(form_edit_buf))
                                form_edit_cursor++;
                        } else if (url_editing) {
                            size_t curlen = strlen(url_buf);

                            if (url_cursor < curlen) {
                                url_cursor++;
                            }
                        }
                        break;

                    case SDL_SCANCODE_END:
                        if (form_editing) {
                            form_edit_cursor = strlen(form_edit_buf);
                        } else if (url_editing) {
                            url_cursor = strlen(url_buf);
                        }
                        break;

                    /* Numbered link navigation: digits handled via TEXT_INPUT */

                    /* Scrolling */
                    
                    case SDL_SCANCODE_DOWN:
                        if (!url_editing && !form_editing) {
                            if (!scroll_down_held) {
                                scroll_down_held = true;
                                scroll_up_held = false;
                                scroll_lines++;
                                scroll_repeat_at = SDL_GetTicks() + SCROLL_REPEAT_DELAY_MS;
                            }
                        }
                        break;
                    case SDL_SCANCODE_J:
                        if (!url_editing && !form_editing) scroll_lines++;
                        break;
                    case SDL_SCANCODE_UP:
                        if (!url_editing && !form_editing) {
                            if (!scroll_up_held) {
                                scroll_up_held = true;
                                scroll_down_held = false;
                                if (scroll_lines > 0) scroll_lines--;
                                scroll_repeat_at = SDL_GetTicks() + SCROLL_REPEAT_DELAY_MS;
                            }
                        }
                        break;
                    case SDL_SCANCODE_K:
                        if (!url_editing && !form_editing && scroll_lines > 0)
                            scroll_lines--;
                        break;
                    case SDL_SCANCODE_HOME:
                        if (form_editing) {
                            form_edit_cursor = 0;
                        } else if (url_editing) {
                            url_cursor = 0;
                        } else {
                            scroll_lines = 0;
                        }
                        break;
                    case SDL_SCANCODE_PAGEDOWN: {
                        if (!url_editing && !form_editing) {
                            int lpp = (VIEW_H - PAD_TOP - PAD_BOTTOM) / (CH_H + LINE_SPACING);
                            scroll_lines += lpp > 2 ? lpp - 2 : 1;
                        }
                        break;
                    }
                    case SDL_SCANCODE_PAGEUP:
                        if (!url_editing && !form_editing) {
                            if (scroll_lines >= 5) scroll_lines -= 5; else scroll_lines = 0;
                        }
                        break;

                    /* Link navigation */
case SDL_SCANCODE_TAB: {
    bool shift = (ev.key.mod & SDL_KMOD_SHIFT) != 0;
    if (!form_editing && !url_editing && page && page->action_count > 0) {
        if (sel_action < 0) {
            /* First time we tab (Tab or Shift+Tab): start at the first link */
            sel_action = 0;
        } else if (shift) {
            /* Move backward, wrapping to last if we're at the start */
            sel_action = (sel_action == 0) ? (page->action_count - 1) : (sel_action - 1);
        } else {
            /* Move forward with wraparound */
            sel_action = (sel_action + 1) % page->action_count;
        }
    }
    break;
}
                    case SDL_SCANCODE_ESCAPE:
                        if (form_editing) {
                            form_editing = false;
                            form_edit_form = -1;
                            form_edit_field = -1;
                        } else if (link_number_mode) {
                            link_number_mode = false;
                            link_number_len = 0;
                            link_number_buf[0] = '\0';
                        } else {
                            running = 0;
                        }
                        break;
                    /* Digit keys: handled via TEXT_INPUT, just break here */
                    case SDL_SCANCODE_1:
                    case SDL_SCANCODE_2:
                    case SDL_SCANCODE_3:
                    case SDL_SCANCODE_4:
                    case SDL_SCANCODE_5:
                    case SDL_SCANCODE_6:
                    case SDL_SCANCODE_7:
                    case SDL_SCANCODE_8:
                    case SDL_SCANCODE_9:
                    case SDL_SCANCODE_0:
                        break;
                    default:
                        if (link_number_mode) {
                            link_number_mode = false;
                            link_number_len = 0;
                            link_number_buf[0] = '\0';
                        }
                        break;
                }
            } /* KEY_DOWN */

            if (ev.type == SDL_EVENT_KEY_UP) {
                if (ev.key.scancode == SC_ACCELERATOR) {
                    accel_down = false;
                    inhibit_text_once = false;
                } else if (ev.key.scancode == SDL_SCANCODE_UP) {
                    scroll_up_held = false;
                } else if (ev.key.scancode == SDL_SCANCODE_DOWN) {
                    scroll_down_held = false;
                }
            }
        } /* while events */

        /* Timed scroll repeat for held arrow keys */
        if (!url_editing && !form_editing) {
            Uint64 now = SDL_GetTicks();
            if (scroll_down_held && now >= scroll_repeat_at) {
                scroll_lines++;
                scroll_repeat_at = now + SCROLL_REPEAT_INTERVAL_MS;
            } else if (scroll_up_held && now >= scroll_repeat_at) {
                if (scroll_lines > 0) scroll_lines--;
                scroll_repeat_at = now + SCROLL_REPEAT_INTERVAL_MS;
            }
        }

        SDL_Delay(10);
    }

    SDL_StopTextInput(win);
    free_page(page);
    free(content_wrapped);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    SDL_Quit();
    curl_global_cleanup();
    printf("[mini_browser] exit main\n");
    return 0;
}
