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

/* Phase 3: same stb_image v2.30 already used by WHY2025 namebadge. */
#define STBI_ASSERT(x)
#define STBI_NO_THREAD_LOCALS
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

/* --- Yield macro for BadgeVMS/ESP-IDF, no-op on desktop --- */
#if defined(ESP_PLATFORM)
# include "freertos/FreeRTOS.h"
# include "freertos/task.h"
# define YIELD_NET() vTaskDelay(pdMS_TO_TICKS(2))
#else
# define YIELD_NET() ((void)0)
#endif

/* ---------- Mini Browser version ---------- */
#define MINI_BROWSER_VERSION "2.6"

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
#define POST_BODY_MAX  2048

/* Mini Browser 2.5 Phase 4 - bounded session cookie jar.
 * Cookie storage and scratch buffers are static/global on purpose:
 * BadgeVMS gives the application task a tight stack budget.
 */
#define MAX_COOKIES          12
#define COOKIE_NAME_MAX      31
#define COOKIE_VALUE_MAX     95
#define COOKIE_DOMAIN_MAX    95
#define COOKIE_PATH_MAX      95
#define COOKIE_HEADER_MAX   768
#define COOKIE_SET_MAX      384

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


/* ---------- Mini Browser 2.6 Phase 3 Candidate 3 ---------- */
/*
 * Bounded real-world image support:
 *   - remember up to 32 <img> references
 *   - render the first 5 inline
 *   - expose later images as numbered actions
 *   - JPEG/PNG via stb_image
 *   - decode larger source images only inside a strict RGB decode budget
 *   - immediately downscale retained images to RGB565
 *   - direct image URLs and numbered image actions use one image viewer
 *
 * Important memory rule:
 * stb_image still has to decode JPEG/PNG before Mini Browser can resize it.
 * Therefore source images are accepted only when width*height*3 fits inside
 * IMAGE_DECODE_MAX_BYTES. Fix 8 performs RGB888 -> RGB565 downscaling in-place
 * inside stb's decode allocation and then shrinks that allocation, avoiding a
 * second full retained-image allocation at peak decode memory.
 */
#define MAX_PAGE_IMAGES          32
#define MAX_INLINE_IMAGES         5
#define IMAGE_DOWNLOAD_MAX       (512 * 1024)
#define IMAGE_DECODE_MAX_BYTES   (1536 * 1024)
#define IMAGE_SOURCE_MAX_W       1600
#define IMAGE_SOURCE_MAX_H       1600
#define IMAGE_DRAW_MAX_W          320
#define IMAGE_DRAW_MAX_H          240
/*
 * Viewer retention is deliberately capped at 320x240.
 *
 * Fix 8 made the decode path memory-safe by converting RGB888 -> RGB565
 * in-place. A direct 640x480 image nevertheless retained a 614400-byte
 * RGB565 viewer buffer, which exhausted BadgeVMS memory during rendering.
 *
 * Keep viewer storage at the same proven-safe bound as inline images.
 * draw_image_viewer() still scales the retained image to the available
 * screen area, so this changes retained resolution, not viewer layout.
 */
#define IMAGE_VIEW_MAX_W          320
#define IMAGE_VIEW_MAX_H          240
#define IMAGE_RESERVE_LINES         1

typedef enum {
    DISPLAY_BW = 0,
    DISPLAY_COLORS = 1,
    DISPLAY_COLORS_IMAGES = 2
} display_mode_t;

/* Start rich for the Phase 3 proof-of-concept. WHY+O opens the mode menu. */
static display_mode_t g_display_mode = DISPLAY_COLORS_IMAGES;

static const char *display_mode_name(display_mode_t mode) {
    switch (mode) {
        case DISPLAY_BW:            return "Black & White";
        case DISPLAY_COLORS:        return "Colors";
        case DISPLAY_COLORS_IMAGES: return "Colors + Images";
        default:                    return "Unknown";
    }
}

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

/* External Unicode glyph geometry. Needed by wrapping and rendering. */
#define UNICODE_GLYPH_BYTES 32
#define UNICODE_GLYPH_W 16
#define UNICODE_GLYPH_H 16

/* --------- curl memory sink --------- */
typedef struct { char *buf; size_t len; } mem_t;

/*
 * Phase 5: metadata retained from the most recent network fetch.
 * All strings are bounded/static so Page Information does not add large
 * automatic buffers to the ESP32 task stack.
 */
typedef struct {
    char effective_url[URL_MAX];
    char content_type[96];
    long redirect_count;
    size_t downloaded_bytes;

    /* Phase 5B: request-side facts we can know without CURLINFO support. */
    char request_method[5];       /* "GET" or "POST" */
    size_t request_body_bytes;
    int cookies_sent;
} fetch_meta_t;

static fetch_meta_t g_fetch_meta;
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
    char enctype[48];
    form_field_t fields[MAX_FORM_FIELDS];
    int field_count;
} form_t;

typedef enum {
    ACTION_LINK,
    ACTION_FORM_FIELD,
    ACTION_FORM_SUBMIT,
    ACTION_IMAGE
} action_type_t;

typedef struct {
    action_type_t type;
    int link_index;
    int form_index;
    int field_index;
    int image_index;
} page_action_t;

typedef struct {
    char src[URL_MAX];
    char alt[96];
    int requested_width;
    int requested_height;
} page_image_t;

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
    int explicit_color_count;  /* 2.6 Phase 1B valid HTML/CSS foreground colors */
    int explicit_style_count;  /* 2.6 Phase 2A valid inline text-style declarations */
    int explicit_background_count; /* 2.6 Phase 2B valid inline background-color declarations */
    page_image_t images[MAX_PAGE_IMAGES];
    int image_count;              /* 2.6 Phase 3 bounded <img> elements retained */
    int image_seen_count;         /* all supported <img src=...> tags seen */
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
    if (!strncasecmp(href, "http://", 7) ||
    !strncasecmp(href, "https://", 8)) {
    strncpy(out, href, cap);
    out[cap-1]=0;
    return;
}
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

/* ---------- HTML entity decode ---------- */

typedef struct {
    const char *name;
    unsigned long codepoint;
} html_entity_t;

/*
 * Deliberately bounded named-entity table.  These cover the common entities
 * encountered by the Mini Browser without importing a full HTML5 entity
 * database. Numeric decimal/hexadecimal references support all Unicode
 * scalar values.
 */
static const html_entity_t g_html_entities[] = {
    {"amp",    0x0026}, {"lt",     0x003C}, {"gt",     0x003E},
    {"quot",   0x0022}, {"apos",   0x0027}, {"nbsp",   0x0020},
    {"copy",   0x00A9}, {"reg",    0x00AE}, {"trade",  0x2122},
    {"euro",   0x20AC}, {"cent",   0x00A2}, {"pound",  0x00A3},
    {"yen",    0x00A5}, {"sect",   0x00A7}, {"para",   0x00B6},
    {"deg",    0x00B0}, {"plusmn", 0x00B1}, {"times",  0x00D7},
    {"divide", 0x00F7}, {"middot", 0x00B7}, {"bull",   0x2022},
    {"hellip", 0x2026}, {"ndash",  0x2013}, {"mdash",  0x2014},
    {"lsquo",  0x2018}, {"rsquo",  0x2019}, {"ldquo",  0x201C},
    {"rdquo",  0x201D}, {"laquo",  0x00AB}, {"raquo",  0x00BB}
};

static unsigned long html_numeric_codepoint(unsigned long cp) {
    /*
     * HTML numeric character references do not map NUL, surrogate code
     * points or values beyond Unicode to literal output. Use U+FFFD.
     */
    if (cp == 0 || cp > 0x10FFFFUL || (cp >= 0xD800UL && cp <= 0xDFFFUL))
        return 0xFFFDUL;

    /*
     * HTML's legacy numeric-reference replacements for the C1 range.
     * This makes common real-world references such as &#128; render as €.
     */
    static const unsigned short c1[32] = {
        0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
        0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
        0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
        0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178
    };
    if (cp >= 0x80UL && cp <= 0x9FUL)
        return c1[cp - 0x80UL];

    return cp;
}

static int emit_utf8_codepoint(unsigned long cp, char *out, size_t *o, size_t cap) {
    cp = html_numeric_codepoint(cp);

    if (cp <= 0x7F) {
        if (*o + 1 > cap) return 0;
        out[(*o)++] = (char)cp;
    } else if (cp <= 0x7FF) {
        if (*o + 2 > cap) return 0;
        out[(*o)++] = (char)(0xC0 | (cp >> 6));
        out[(*o)++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp <= 0xFFFF) {
        if (*o + 3 > cap) return 0;
        out[(*o)++] = (char)(0xE0 | (cp >> 12));
        out[(*o)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[(*o)++] = (char)(0x80 | (cp & 0x3F));
    } else {
        if (*o + 4 > cap) return 0;
        out[(*o)++] = (char)(0xF0 | (cp >> 18));
        out[(*o)++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[(*o)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[(*o)++] = (char)(0x80 | (cp & 0x3F));
    }
    return 1;
}

static const char *emit_named_entity(const char *h, char *out, size_t *o, size_t cap) {
    if (!h || *h != '&') return NULL;

    const char *p = h + 1;
    const char *semi = strchr(p, ';');
    if (!semi) return NULL;

    size_t name_len = (size_t)(semi - p);
    if (!name_len || name_len > 12) return NULL;

    for (size_t i = 0; i < sizeof(g_html_entities) / sizeof(g_html_entities[0]); i++) {
        const char *name = g_html_entities[i].name;
        if (strlen(name) == name_len && !strncmp(p, name, name_len)) {
            if (!emit_utf8_codepoint(g_html_entities[i].codepoint, out, o, cap))
                return NULL;
            return semi + 1;
        }
    }
    return NULL;
}

static const char *emit_numeric_entity(const char *h, char *out, size_t *o, size_t cap) {
    if (!h || h[0] != '&' || h[1] != '#') return NULL;

    int base = 10;
    const char *p = h + 2;
    if (*p == 'x' || *p == 'X') {
        base = 16;
        p++;
    }

    unsigned long value = 0;
    const char *digits = p;

    while (*p && *p != ';') {
        int digit;
        if (*p >= '0' && *p <= '9') digit = *p - '0';
        else if (base == 16 && *p >= 'a' && *p <= 'f') digit = 10 + *p - 'a';
        else if (base == 16 && *p >= 'A' && *p <= 'F') digit = 10 + *p - 'A';
        else return NULL;

        if (digit >= base ||
            value > (ULONG_MAX - (unsigned long)digit) / (unsigned long)base)
            return NULL;

        value = value * (unsigned long)base + (unsigned long)digit;
        p++;
    }

    if (p == digits || *p != ';') return NULL;
    if (!emit_utf8_codepoint(value, out, o, cap)) return NULL;
    return p + 1;
}

static const char *emit_html_entity(const char *h, char *out, size_t *o, size_t cap) {
    if (!h || *h != '&') return NULL;
    if (h[1] == '#') return emit_numeric_entity(h, out, o, cap);
    return emit_named_entity(h, out, o, cap);
}

/*
 * Decode entities in a bounded string. Unknown/malformed entities are copied
 * literally. src and dst must not overlap.
 */
static void decode_html_entities(const char *src, char *dst, size_t dst_cap) {
    if (!dst || !dst_cap) return;
    dst[0] = 0;
    if (!src) return;

    size_t used = 0;
    const char *p = src;

    while (*p && used + 1 < dst_cap) {
        if (*p == '&') {
            const char *next = emit_html_entity(p, dst, &used, dst_cap - 1);
            if (next) {
                p = next;
                continue;
            }
        }
        dst[used++] = *p++;
    }
    dst[used] = 0;
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
                char raw[URL_MAX];
                size_t copy_len = value_len;
                if (copy_len >= sizeof(raw)) copy_len = sizeof(raw) - 1;
                memcpy(raw, value, copy_len);
                raw[copy_len] = 0;
                decode_html_entities(raw, out, out_cap);
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

#define TEXT_BOLD_ON     0x01
#define TEXT_BOLD_OFF    0x02
#define TEXT_LINK_ON     0x03
#define TEXT_LINK_OFF    0x04
#define TEXT_HEADING_ON  0x05
#define TEXT_HEADING_OFF 0x06
#define TEXT_HRULE       0x07
#define TEXT_FORM_ON     0x08
#define TEXT_FORM_OFF    0x09

/* 2.6 Phase 1B: zero-width RGB color markers encoded as fixed PUA nibbles.
 * A color push is START + six nibble codepoints (RRGGBB). This avoids using
 * arbitrary PUA codepoints as byte values and keeps the marker stream fully
 * deterministic through wrapping/debugging. */
#define TEXT_COLOR_START   0xE400u
#define TEXT_COLOR_NIBBLE  0xE410u  /* E410..E41F = hexadecimal nibble 0..15 */
#define TEXT_COLOR_POP     0xE420u
#define TEXT_COLOR_INHERIT 0xE421u

/* 2.6 Phase 2A: compact inline-style state markers.
 * STYLE_START is followed by two nibble codepoints containing one byte:
 * bit 7 bold-set, bit 6 bold-value, bit 5 italic-set, bit 4 italic-value,
 * bit 3 underline-set, bit 2 underline-value, bit 1..0 alignment
 * (0 inherit, 1 left, 2 center, 3 right). Every supported paired text
 * container pushes one style state; its closing tag emits STYLE_POP. */
#define TEXT_STYLE_START   0xE430u
#define TEXT_STYLE_NIBBLE  0xE440u
#define TEXT_STYLE_POP     0xE450u

/* 2.6 Phase 2B: zero-width background-color state markers.
 * RGB uses the same fixed six-nibble encoding as foreground colors.
 * Every supported paired style container pushes one background state;
 * TRANSPARENT explicitly disables an inherited background until POP. */
#define TEXT_BG_START       0xE460u
#define TEXT_BG_NIBBLE      0xE470u  /* E470..E47F = hexadecimal nibble 0..15 */
#define TEXT_BG_POP         0xE480u
#define TEXT_BG_INHERIT     0xE481u
#define TEXT_BG_TRANSPARENT 0xE482u

static int append_utf8_cp(char *out, size_t cap, size_t *used, unsigned cp) {
    char b[4]; size_t n = 0;
    if (cp <= 0x7F) b[n++] = (char)cp;
    else if (cp <= 0x7FF) { b[n++] = (char)(0xC0 | (cp >> 6)); b[n++] = (char)(0x80 | (cp & 0x3F)); }
    else { b[n++] = (char)(0xE0 | (cp >> 12)); b[n++] = (char)(0x80 | ((cp >> 6) & 0x3F)); b[n++] = (char)(0x80 | (cp & 0x3F)); }
    return append_bytes(out, cap, used, b, n);
}

static int parse_html_color(const char *value, unsigned char *rr, unsigned char *gg, unsigned char *bb) {
    if (!value || !rr || !gg || !bb) return 0;
    while (isspace((unsigned char)*value)) value++;
    char v[32]; size_t n = 0;
    while (*value && *value != ';' && !isspace((unsigned char)*value) && n + 1 < sizeof(v))
        v[n++] = (char)tolower((unsigned char)*value++);
    v[n] = 0;
    if (v[0] == '#') {
        unsigned x = 0;
        if (strlen(v) == 7 && sscanf(v + 1, "%06x", &x) == 1) {
            *rr = (unsigned char)(x >> 16); *gg = (unsigned char)(x >> 8); *bb = (unsigned char)x; return 1;
        }
        if (strlen(v) == 4) {
            unsigned r,g,b;
            if (sscanf(v + 1, "%1x%1x%1x", &r, &g, &b) == 3) {
                *rr=(unsigned char)(r*17); *gg=(unsigned char)(g*17); *bb=(unsigned char)(b*17); return 1;
            }
        }
        return 0;
    }
    struct named_color { const char *name; unsigned char r,g,b; };
    static const struct named_color colors[] = {
        {"black",0,0,0},{"white",255,255,255},{"red",255,0,0},{"green",0,128,0},
        {"blue",0,0,255},{"yellow",255,255,0},{"cyan",0,255,255},{"aqua",0,255,255},
        {"magenta",255,0,255},{"fuchsia",255,0,255},{"gray",128,128,128},{"grey",128,128,128},
        {"orange",255,165,0},{"purple",128,0,128}
    };
    for (size_t i=0;i<sizeof(colors)/sizeof(colors[0]);i++) if (!strcmp(v, colors[i].name)) {
        *rr=colors[i].r; *gg=colors[i].g; *bb=colors[i].b; return 1;
    }
    return 0;
}

static int style_color_value(const char *style, unsigned char *r, unsigned char *g, unsigned char *b) {
    if (!style) return 0;
    const char *p = style;
    while (*p) {
        while (*p == ';' || isspace((unsigned char)*p)) p++;
        const char *name = p;
        while (*p && *p != ':' && *p != ';') p++;
        if (*p != ':') { while (*p && *p != ';') p++; continue; }
        const char *name_end = p++;
        while (name_end > name && isspace((unsigned char)name_end[-1])) name_end--;
        while (name < name_end && isspace((unsigned char)*name)) name++;
        const char *val = p;
        while (*p && *p != ';') p++;
        if ((size_t)(name_end-name)==5 && !strncasecmp(name,"color",5)) {
            char tmp[32]; size_t n=(size_t)(p-val); while(n && isspace((unsigned char)val[n-1])) n--;
            while(n && isspace((unsigned char)*val)){val++;n--;}
            if(n>=sizeof(tmp)) n=sizeof(tmp)-1; memcpy(tmp,val,n); tmp[n]=0;
            return parse_html_color(tmp,r,g,b);
        }
    }
    return 0;
}

typedef struct {
    bool bold_set, bold;
    bool italic_set, italic;
    bool underline_set, underline;
    unsigned char align; /* 0 inherit, 1 left, 2 center, 3 right */
    int valid_count;
} inline_style_t;

/* Parse one inline background-color declaration. Return values:
 * 0 = absent/invalid, 1 = RGB color, 2 = transparent. */
static int style_background_value(const char *style, unsigned char *r, unsigned char *g, unsigned char *b) {
    if (!style) return 0;
    const char *p = style;
    while (*p) {
        while (*p == ';' || isspace((unsigned char)*p)) p++;
        const char *name = p;
        while (*p && *p != ':' && *p != ';') p++;
        if (*p != ':') { while (*p && *p != ';') p++; continue; }
        const char *name_end = p++;
        while (name_end > name && isspace((unsigned char)name_end[-1])) name_end--;
        while (name < name_end && isspace((unsigned char)*name)) name++;
        const char *val = p;
        while (*p && *p != ';') p++;
        size_t nn = (size_t)(name_end - name), vn = (size_t)(p - val);
        if (nn == 16 && !strncasecmp(name, "background-color", 16)) {
            while (vn && isspace((unsigned char)*val)) { val++; vn--; }
            while (vn && isspace((unsigned char)val[vn - 1])) vn--;
            if (vn == 11 && !strncasecmp(val, "transparent", 11)) return 2;
            char tmp[32];
            if (vn >= sizeof(tmp)) return 0;
            memcpy(tmp, val, vn); tmp[vn] = 0;
            return parse_html_color(tmp, r, g, b) ? 1 : 0;
        }
    }
    return 0;
}

static void append_background_push(char *out, size_t cap, size_t *used,
                                   int kind, unsigned char r, unsigned char g, unsigned char b) {
    if (kind == 2) {
        append_utf8_cp(out, cap, used, TEXT_BG_TRANSPARENT);
        return;
    }
    if (kind != 1) {
        append_utf8_cp(out, cap, used, TEXT_BG_INHERIT);
        return;
    }
    append_utf8_cp(out, cap, used, TEXT_BG_START);
    append_utf8_cp(out, cap, used, TEXT_BG_NIBBLE + ((r >> 4) & 0x0F));
    append_utf8_cp(out, cap, used, TEXT_BG_NIBBLE + (r & 0x0F));
    append_utf8_cp(out, cap, used, TEXT_BG_NIBBLE + ((g >> 4) & 0x0F));
    append_utf8_cp(out, cap, used, TEXT_BG_NIBBLE + (g & 0x0F));
    append_utf8_cp(out, cap, used, TEXT_BG_NIBBLE + ((b >> 4) & 0x0F));
    append_utf8_cp(out, cap, used, TEXT_BG_NIBBLE + (b & 0x0F));
}

static bool css_value_eq(const char *v, size_t n, const char *wanted) {
    while (n && isspace((unsigned char)*v)) { v++; n--; }
    while (n && isspace((unsigned char)v[n - 1])) n--;
    size_t w = strlen(wanted);
    return n == w && !strncasecmp(v, wanted, w);
}

static inline_style_t parse_inline_text_style(const char *style, bool allow_align) {
    inline_style_t st = {0};
    if (!style) return st;
    const char *p = style;
    while (*p) {
        while (*p == ';' || isspace((unsigned char)*p)) p++;
        const char *name = p;
        while (*p && *p != ':' && *p != ';') p++;
        if (*p != ':') { while (*p && *p != ';') p++; continue; }
        const char *name_end = p++;
        while (name_end > name && isspace((unsigned char)name_end[-1])) name_end--;
        while (name < name_end && isspace((unsigned char)*name)) name++;
        const char *val = p;
        while (*p && *p != ';') p++;
        size_t nn = (size_t)(name_end - name), vn = (size_t)(p - val);

        if (nn == 11 && !strncasecmp(name, "font-weight", 11)) {
            if (css_value_eq(val, vn, "bold") || css_value_eq(val, vn, "700")) {
                st.bold_set = true; st.bold = true; st.valid_count++;
            } else if (css_value_eq(val, vn, "normal")) {
                st.bold_set = true; st.bold = false; st.valid_count++;
            }
        } else if (nn == 10 && !strncasecmp(name, "font-style", 10)) {
            if (css_value_eq(val, vn, "italic")) {
                st.italic_set = true; st.italic = true; st.valid_count++;
            } else if (css_value_eq(val, vn, "normal")) {
                st.italic_set = true; st.italic = false; st.valid_count++;
            }
        } else if (nn == 15 && !strncasecmp(name, "text-decoration", 15)) {
            if (css_value_eq(val, vn, "underline")) {
                st.underline_set = true; st.underline = true; st.valid_count++;
            } else if (css_value_eq(val, vn, "none")) {
                st.underline_set = true; st.underline = false; st.valid_count++;
            }
        } else if (allow_align && nn == 10 && !strncasecmp(name, "text-align", 10)) {
            if (css_value_eq(val, vn, "left")) { st.align = 1; st.valid_count++; }
            else if (css_value_eq(val, vn, "center")) { st.align = 2; st.valid_count++; }
            else if (css_value_eq(val, vn, "right")) { st.align = 3; st.valid_count++; }
        }
    }
    return st;
}

static bool style_container_tag(const char *tag) {
    static const char *tags[] = {"span","p","div","section","article","main","header","footer","nav","aside","blockquote","address","code","strong","b","em","i","a","h1","h2","h3","h4","h5","h6","td","th"};
    for (size_t i = 0; i < sizeof(tags)/sizeof(tags[0]); i++) if (!strcmp(tag, tags[i])) return true;
    return false;
}

static bool style_block_tag(const char *tag) {
    static const char *tags[] = {"p","div","section","article","main","header","footer","nav","aside","blockquote","address","h1","h2","h3","h4","h5","h6","td","th"};
    for (size_t i = 0; i < sizeof(tags)/sizeof(tags[0]); i++) if (!strcmp(tag, tags[i])) return true;
    return false;
}

static void append_style_push(char *out, size_t cap, size_t *used, inline_style_t st) {
    unsigned char flags = 0;
    if (st.bold_set) flags |= 0x80 | (st.bold ? 0x40 : 0);
    if (st.italic_set) flags |= 0x20 | (st.italic ? 0x10 : 0);
    if (st.underline_set) flags |= 0x08 | (st.underline ? 0x04 : 0);
    flags |= (st.align & 0x03);
    append_utf8_cp(out, cap, used, TEXT_STYLE_START);
    append_utf8_cp(out, cap, used, TEXT_STYLE_NIBBLE + ((flags >> 4) & 0x0F));
    append_utf8_cp(out, cap, used, TEXT_STYLE_NIBBLE + (flags & 0x0F));
}

static int color_container_tag(const char *tag) {
    static const char *tags[] = {"font","span","p","div","section","article","main","header","footer","nav","aside","blockquote","address","code","strong","b","em","i","a","h1","h2","h3","h4","h5","h6","td","th"};
    for (size_t i=0;i<sizeof(tags)/sizeof(tags[0]);i++) if (!strcmp(tag,tags[i])) return 1;
    return 0;
}

static void append_color_push(char *out, size_t cap, size_t *used, unsigned char r, unsigned char g, unsigned char b) {
    append_utf8_cp(out, cap, used, TEXT_COLOR_START);
    append_utf8_cp(out, cap, used, TEXT_COLOR_NIBBLE + ((r >> 4) & 0x0F));
    append_utf8_cp(out, cap, used, TEXT_COLOR_NIBBLE + (r & 0x0F));
    append_utf8_cp(out, cap, used, TEXT_COLOR_NIBBLE + ((g >> 4) & 0x0F));
    append_utf8_cp(out, cap, used, TEXT_COLOR_NIBBLE + (g & 0x0F));
    append_utf8_cp(out, cap, used, TEXT_COLOR_NIBBLE + ((b >> 4) & 0x0F));
    append_utf8_cp(out, cap, used, TEXT_COLOR_NIBBLE + (b & 0x0F));
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
    page->actions[index].image_index = -1;
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
                        snprintf(line, sizeof(line), "%c[%d] %s: %s%c\n",
                                 TEXT_FORM_ON, action_index + 1, field->name, field->value, TEXT_FORM_OFF);
                    }
                } else if (action->type == ACTION_FORM_SUBMIT &&
                           action->form_index >= 0 && action->form_index < page->form_count) {
                    const form_t *form = &page->forms[action->form_index];
                    if (action->field_index >= 0 && action->field_index < form->field_count) {
                        const form_field_t *field = &form->fields[action->field_index];
                        snprintf(line, sizeof(line), "%c[%d] [%s]%c\n",
                                 TEXT_FORM_ON, action_index + 1,
                                 field->label[0] ? field->label : "Submit", TEXT_FORM_OFF);
                    }
                } else if (action->type == ACTION_IMAGE &&
                           action->image_index >= 0 &&
                           action->image_index < page->image_count) {
                    const page_image_t *image = &page->images[action->image_index];
                    snprintf(line, sizeof(line), "%c[%d] Image: %s%c\n",
                             TEXT_LINK_ON, action_index + 1,
                             image->alt[0] ? image->alt : "image", TEXT_LINK_OFF);
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
    char raw[256];
    if (n >= sizeof(raw)) n = sizeof(raw) - 1;
    memcpy(raw, start, n);
    raw[n] = 0;
    decode_html_entities(raw, out, cap);
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

    if (strchr(out, '&')) {
        char raw[256];
        strncpy(raw, out, sizeof(raw));
        raw[sizeof(raw) - 1] = 0;
        decode_html_entities(raw, out, cap);
    }
}

/*
 * Internal text-formatting markers.
 * Must be defined before html_to_page(), because the HTML parser emits them.
 * They are preserved by wrap_text() and consumed by draw_text().
 */


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
                    const char *next = emit_html_entity(cursor, template_text, &used, template_cap - 1);
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
        /* HTML tag names may contain digits after the initial letter.
         * This matters for h1..h6: the old isalpha-only loop parsed <h2>
         * as tag "h", leaving "2" at the start of the attribute range.
         * As a result headings were neither recognized as headings nor as
         * color containers, so style="color:yellow" was never seen. */
        while (p < html_end && tag_len + 1 < sizeof(tag) && isalnum((unsigned char)*p))
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
            else if (!strcmp(tag, "strong") || !strcmp(tag, "b")) {
                char marker[2] = { TEXT_BOLD_OFF, 0 };
                append_text(template_text, template_cap, &used, marker);
            }
            else if (!strcmp(tag, "em") || !strcmp(tag, "i")) append_text(template_text, template_cap, &used, "_");
            else if (!strcmp(tag, "a")) {
                char marker[2] = { TEXT_LINK_OFF, 0 };
                append_text(template_text, template_cap, &used, marker);
            }
            else if (!strcmp(tag, "h1") || !strcmp(tag, "h2") || !strcmp(tag, "h3") ||
                     !strcmp(tag, "h4") || !strcmp(tag, "h5") || !strcmp(tag, "h6")) {
                char marker[2] = { TEXT_HEADING_OFF, 0 };
                append_text(template_text, template_cap, &used, marker);
                append_line_break(template_text, template_cap, &used);
            }
            else if (!strcmp(tag, "p") || !strcmp(tag, "div") || !strcmp(tag, "section") ||
                     !strcmp(tag, "article") || !strcmp(tag, "main") || !strcmp(tag, "header") ||
                     !strcmp(tag, "footer") || !strcmp(tag, "nav") || !strcmp(tag, "aside") ||
                     !strcmp(tag, "blockquote") || !strcmp(tag, "address") || !strcmp(tag, "li") ||
                     !strcmp(tag, "tr") || !strcmp(tag, "table"))
                append_line_break(template_text, template_cap, &used);
            if (color_container_tag(tag)) append_utf8_cp(template_text, template_cap, &used, TEXT_COLOR_POP);
            if (style_container_tag(tag)) {
                append_utf8_cp(template_text, template_cap, &used, TEXT_BG_POP);
                append_utf8_cp(template_text, template_cap, &used, TEXT_STYLE_POP);
            }
            cursor = after_tag;
            continue;
        }

        /* Block elements must break the line BEFORE their color marker is emitted.
         * Otherwise wrap_text() can leave the marker on the preceding line, causing
         * the block's requested color to be lost. Inline elements do not pre-break. */
        bool color_block_tag =
            !strcmp(tag, "p") || !strcmp(tag, "div") || !strcmp(tag, "section") ||
            !strcmp(tag, "article") || !strcmp(tag, "main") || !strcmp(tag, "header") ||
            !strcmp(tag, "footer") || !strcmp(tag, "nav") || !strcmp(tag, "aside") ||
            !strcmp(tag, "blockquote") || !strcmp(tag, "address") ||
            !strcmp(tag, "h1") || !strcmp(tag, "h2") || !strcmp(tag, "h3") ||
            !strcmp(tag, "h4") || !strcmp(tag, "h5") || !strcmp(tag, "h6") ||
            !strcmp(tag, "tr");
        if (color_block_tag)
            append_line_break(template_text, template_cap, &used);

        /* Phase 2A: every supported paired text container pushes a compact
         * text-style state. Alignment is accepted only on block-like elements. */
        if (style_container_tag(tag)) {
            char style_value[192] = "";
            inline_style_t st = {0};
            if (tag_attribute(attributes, tag_end, "style", style_value, sizeof(style_value)))
                st = parse_inline_text_style(style_value, style_block_tag(tag));
            append_style_push(template_text, template_cap, &used, st);
            page->explicit_style_count += st.valid_count;
        }

        /* Phase 2B: background-color follows the same bounded push/pop model as
         * the Phase 2A style state.  Missing/invalid values inherit; transparent
         * is an explicit override that temporarily disables a parent background. */
        if (style_container_tag(tag)) {
            char bg_style_value[192] = "";
            unsigned char br = 0, bg = 0, bb = 0;
            int bg_kind = 0;
            if (tag_attribute(attributes, tag_end, "style", bg_style_value, sizeof(bg_style_value)))
                bg_kind = style_background_value(bg_style_value, &br, &bg, &bb);
            append_background_push(template_text, template_cap, &used, bg_kind, br, bg, bb);
            if (bg_kind) page->explicit_background_count++;
        }

        /* Phase 1B: every supported paired text container pushes a color state.
         * Uncolored containers push inheritance, so nested closing tags restore correctly. */
        if (color_container_tag(tag)) {
            unsigned char cr=0,cg=0,cb=0; int have_color=0;
            char style_value[192] = "";
            if (tag_attribute(attributes, tag_end, "style", style_value, sizeof(style_value)))
                have_color = style_color_value(style_value, &cr, &cg, &cb);
            if (!have_color && !strcmp(tag,"font")) {
                char color_value[32] = "";
                if (tag_attribute(attributes, tag_end, "color", color_value, sizeof(color_value)))
                    have_color = parse_html_color(color_value, &cr, &cg, &cb);
            }
            if (have_color) { append_color_push(template_text, template_cap, &used, cr,cg,cb); page->explicit_color_count++; }
            else append_utf8_cp(template_text, template_cap, &used, TEXT_COLOR_INHERIT);
        }

        if (!strcmp(tag, "head")) in_head = true;
        else if (!strcmp(tag, "script")) in_script = true;
        else if (!strcmp(tag, "style")) in_style = true;
        else if (!strcmp(tag, "pre")) { append_line_break(template_text, template_cap, &used); in_pre = true; }
        else if (!strcmp(tag, "br")) append_line_break(template_text, template_cap, &used);
        else if (!strcmp(tag, "p") || !strcmp(tag, "div") || !strcmp(tag, "section") ||
                 !strcmp(tag, "article") || !strcmp(tag, "main") || !strcmp(tag, "header") ||
                 !strcmp(tag, "footer") || !strcmp(tag, "nav") || !strcmp(tag, "aside") ||
                 !strcmp(tag, "blockquote") || !strcmp(tag, "address") || !strcmp(tag, "tr")) {
            /* line break already emitted before the Phase 1B color marker */
        }
        else if (!strcmp(tag, "h1") || !strcmp(tag, "h2") || !strcmp(tag, "h3") ||
                 !strcmp(tag, "h4") || !strcmp(tag, "h5") || !strcmp(tag, "h6")) {
            char marker[2] = { TEXT_HEADING_ON, 0 };
            append_text(template_text, template_cap, &used, marker);
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
        else if (!strcmp(tag, "strong") || !strcmp(tag, "b")) {
            char marker[2] = { TEXT_BOLD_ON, 0 };
            append_text(template_text, template_cap, &used, marker);
        }
        else if (!strcmp(tag, "em") || !strcmp(tag, "i")) append_text(template_text, template_cap, &used, "_");
        else if (!strcmp(tag, "hr")) {
            append_line_break(template_text, template_cap, &used);
            char marker[3] = { TEXT_HRULE, '\n', 0 };
            append_text(template_text, template_cap, &used, marker);
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
                strncpy(form->enctype, "application/x-www-form-urlencoded",
                        sizeof(form->enctype));
                tag_attribute(attributes, tag_end, "action", form->action, sizeof(form->action));
                if (tag_attribute(attributes, tag_end, "method", form->method, sizeof(form->method)))
                    lower_ascii(form->method);
                if (tag_attribute(attributes, tag_end, "enctype", form->enctype, sizeof(form->enctype)))
                    lower_ascii(form->enctype);
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
        } else if (!strcmp(tag, "img")) {
            char src_value[URL_MAX] = "";
            char alt_value[96] = "";
            char width_value[16] = "";
            char height_value[16] = "";

            if (tag_attribute(attributes, tag_end, "src",
                              src_value, sizeof(src_value)) &&
                is_supported_href(src_value)) {
                page->image_seen_count++;

                tag_attribute(attributes, tag_end, "alt",
                              alt_value, sizeof(alt_value));
                tag_attribute(attributes, tag_end, "width",
                              width_value, sizeof(width_value));
                tag_attribute(attributes, tag_end, "height",
                              height_value, sizeof(height_value));

                append_line_break(template_text, template_cap, &used);

                if (g_display_mode != DISPLAY_COLORS_IMAGES) {
                    char placeholder[160];
                    snprintf(placeholder, sizeof(placeholder),
                             "[Image: %s]",
                             alt_value[0] ? alt_value : "image");
                    append_text(template_text, template_cap, &used, placeholder);
                    append_line_break(template_text, template_cap, &used);
                } else if (page->image_count < MAX_PAGE_IMAGES) {
                    int image_index = page->image_count++;
                    page_image_t *image = &page->images[image_index];
                    memset(image, 0, sizeof(*image));

                    resolve_url(page->base, src_value,
                                image->src, sizeof(image->src));
                    strncpy(image->alt,
                            alt_value[0] ? alt_value : "image",
                            sizeof(image->alt) - 1);

                    if (width_value[0])
                        image->requested_width = atoi(width_value);
                    if (height_value[0])
                        image->requested_height = atoi(height_value);

                    if (image_index < MAX_INLINE_IMAGES) {
                        char image_marker[32];
                        snprintf(image_marker, sizeof(image_marker),
                                 "[[MBIMG%d]]", image_index);
                        append_text(template_text, template_cap, &used, image_marker);
                        append_line_break(template_text, template_cap, &used);
                    } else {
                        int action = add_action(page, ACTION_IMAGE, -1, -1, -1);
                        if (action >= 0) {
                            page->actions[action].image_index = image_index;
                            append_action_marker(template_text, template_cap, &used, action);
                        }
                    }
                } else {
                    char placeholder[160];
                    snprintf(placeholder, sizeof(placeholder),
                             "[Image omitted: %s]",
                             alt_value[0] ? alt_value : "image");
                    append_text(template_text, template_cap, &used, placeholder);
                    append_line_break(template_text, template_cap, &used);
                }
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
                        char marker[2] = { TEXT_LINK_ON, 0 };
                        char number[16];
                        append_text(template_text, template_cap, &used, marker);
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

static int build_post_form_request(const page_t *page, int form_index,
                                   int submit_field_index,
                                   char *url, size_t url_cap,
                                   char *body, size_t body_cap) {
    if (!page || form_index < 0 || form_index >= page->form_count ||
        !url_cap || !body_cap) return 0;

    const form_t *form = &page->forms[form_index];
    const char *method = form->method[0] ? form->method : "get";
    const char *enctype = form->enctype[0]
        ? form->enctype
        : "application/x-www-form-urlencoded";

    if (strcasecmp(method, "post")) return -1;
    if (strcasecmp(enctype, "application/x-www-form-urlencoded")) return -2;

    if (form->action[0]) resolve_url(page->base, form->action, url, url_cap);
    else { strncpy(url, page->base, url_cap); url[url_cap - 1] = 0; }

    char *hash = strchr(url, '#');
    if (hash) *hash = 0;

    body[0] = 0;
    bool first_parameter = true;

    for (int i = 0; i < form->field_count; i++) {
        const form_field_t *field = &form->fields[i];
        if (field->disabled || !field->name[0]) continue;

        bool submit = !strcmp(field->type, "submit");
        bool successful = !strcmp(field->type, "text") ||
                          !strcmp(field->type, "search") ||
                          !strcmp(field->type, "url") ||
                          !strcmp(field->type, "hidden") ||
                          (submit && i == submit_field_index);
        if (!successful) continue;

        char encoded_name[sizeof(field->name) * 3 + 1];
        char encoded_value[sizeof(field->value) * 3 + 1];
        if (!form_urlencode(field->name, encoded_name, sizeof(encoded_name)) ||
            !form_urlencode(field->value, encoded_value, sizeof(encoded_value)))
            return 0;

        if (!first_parameter && !append_url_part(body, body_cap, "&")) return 0;
        if (!append_url_part(body, body_cap, encoded_name) ||
            !append_url_part(body, body_cap, "=") ||
            !append_url_part(body, body_cap, encoded_value))
            return 0;

        first_parameter = false;
    }

    return 1;
}

typedef enum {
    ACTIVATE_NONE,
    ACTIVATE_NAVIGATE,
    ACTIVATE_EDIT_FIELD,
    ACTIVATE_POST,
    ACTIVATE_FORM_UNSUPPORTED,
    ACTIVATE_URL_TOO_LONG,
    ACTIVATE_IMAGE
} activate_result_t;

static activate_result_t activate_page_action(
    page_t *page, int action_index, char *navigation_url, size_t navigation_cap,
    int *edit_form, int *edit_field, char *edit_buf, size_t edit_cap,
    size_t *edit_cursor, char *post_body, size_t post_body_cap) {
    if (!page || action_index < 0 || action_index >= page->action_count)
        return ACTIVATE_NONE;

    const page_action_t *action = &page->actions[action_index];
        if (action->type == ACTION_LINK) {
    if (action->link_index < 0 || action->link_index >= page->link_count)
        return ACTIVATE_NONE;

    const char *href = page->links[action->link_index].href;

    printf("[mini_browser] activating link %d -> %s\n",
       action_index + 1,
       href);

           printf("[mini_browser] activating link %d -> %s\n",
           action_index + 1,
           href);

    /* Google search-result redirect wrapper:
     *   http(s)://www.google.com/url?q=https://example.com/&amp;sa=...
     * Navigate directly to the q= destination.
     */
    const char *google_http  = "http://www.google.com/url?q=";
    const char *google_https = "https://www.google.com/url?q=";
    const char *dest = NULL;

    if (!strncmp(href, google_http, strlen(google_http)))
        dest = href + strlen(google_http);
    else if (!strncmp(href, google_https, strlen(google_https)))
        dest = href + strlen(google_https);

    if (dest) {
        size_t len = strlen(dest);

        const char *end = strstr(dest, "&amp;");
        if (!end)
            end = strchr(dest, '&');

        if (end)
            len = (size_t)(end - dest);

        if (len >= navigation_cap)
            len = navigation_cap - 1;

        memcpy(navigation_url, dest, len);
        navigation_url[len] = 0;

        printf("[mini_browser] Google direct -> %s\n", navigation_url);
} else {
    strncpy(navigation_url, href, navigation_cap);
    navigation_url[navigation_cap - 1] = 0;

    /*
     * Google search links contain HTML-escaped query separators:
     *   &amp;
     * Decode these only for Google /search URLs before navigation.
     */
    if (!strncmp(navigation_url, "http://www.google.com/search?", 29) ||
        !strncmp(navigation_url, "https://www.google.com/search?", 30)) {
        char *p;

        while ((p = strstr(navigation_url, "&amp;")) != NULL) {
            *p = '&';
            memmove(p + 1, p + 5, strlen(p + 5) + 1);
        }

        printf("[mini_browser] Google search URL -> %s\n", navigation_url);
    }
}

return ACTIVATE_NAVIGATE;



 
    }

    /*
     * Image actions are not form actions. Handle them before validating
     * form_index/field_index: ACTION_IMAGE deliberately stores -1 there.
     */
    if (action->type == ACTION_IMAGE) {
        return ACTIVATE_IMAGE;
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
        const char *method = form->method[0] ? form->method : "get";

        if (!strcasecmp(method, "post")) {
            int result = build_post_form_request(page, action->form_index,
                                                 action->field_index,
                                                 navigation_url, navigation_cap,
                                                 post_body, post_body_cap);
            if (result == -2) return ACTIVATE_FORM_UNSUPPORTED;
            if (result < 0) return ACTIVATE_FORM_UNSUPPORTED;
            if (!result) return ACTIVATE_URL_TOO_LONG;
            return ACTIVATE_POST;
        }

        int result = build_get_form_url(page, action->form_index,
                                        action->field_index,
                                        navigation_url, navigation_cap);
        if (result < 0) return ACTIVATE_FORM_UNSUPPORTED;
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

/* ---------- UTF-8 decoder forward declaration ---------- */
static unsigned utf8_next(const char *s, size_t len, size_t *i);

/* ---------- wrap text to columns ---------- */


static int wrap_glyph_width(unsigned cp) {
    if (cp == TEXT_BOLD_ON || cp == TEXT_BOLD_OFF ||
        cp == TEXT_LINK_ON || cp == TEXT_LINK_OFF ||
        cp == TEXT_HEADING_ON || cp == TEXT_HEADING_OFF ||
        cp == TEXT_HRULE || cp == TEXT_FORM_ON || cp == TEXT_FORM_OFF ||
        cp == TEXT_COLOR_START ||
        (cp >= TEXT_COLOR_NIBBLE && cp <= TEXT_COLOR_NIBBLE + 15) ||
        cp == TEXT_COLOR_POP || cp == TEXT_COLOR_INHERIT ||
        cp == TEXT_STYLE_START ||
        (cp >= TEXT_STYLE_NIBBLE && cp <= TEXT_STYLE_NIBBLE + 15) ||
        cp == TEXT_STYLE_POP ||
        cp == TEXT_BG_START ||
        (cp >= TEXT_BG_NIBBLE && cp <= TEXT_BG_NIBBLE + 15) ||
        cp == TEXT_BG_POP || cp == TEXT_BG_INHERIT || cp == TEXT_BG_TRANSPARENT)
        return 0;

    if (cp >= 32 && cp <= 126)
        return CH_W;

    if (cp >= 0x80 && cp <= 0x10FFFF)
        return UNICODE_GLYPH_W + 1;

    return 0;
}

static int wrap_token_width(const char *s, size_t start, size_t end) {
    int width = 0;
    size_t i = start;

    while (i < end) {
        unsigned cp = utf8_next(s, end, &i);
        if (cp == 0)
            break;
        width += wrap_glyph_width(cp);
    }

    return width;
}

static char *wrap_text(const char *in, int max_cols) {
    if (!in) return NULL;

    size_t n = strlen(in);

    /*
     * Word-aware wrapping can replace a space with a newline and can also
     * add newlines inside an overlong token. 2*n + 8 remains a generous
     * upper bound while keeping the allocation small on the badge.
     */
    char *out = (char*)malloc(n * 2 + 8);
    if (!out) return NULL;

    /*
     * Keep the existing caller interface, but make every decision in pixels
     * using exactly the same advances as draw_text().
     */
    const int max_px = max_cols > 0 ? max_cols * CH_W : 0;
    const int space_w = CH_W;

    size_t i = 0;
    size_t o = 0;
    int line_px = 0;
    int blank_run = 0;
    bool pending_space = false;

    while (i < n) {
        size_t cp_start = i;
        unsigned cp = utf8_next(in, n, &i);

        if (cp == 0)
            break;

        if (cp == '\r')
            continue;

        /* Treat NBSP as the same break opportunity as an ordinary space. */
        if (cp == ' ' || cp == 0xA0) {
            if (line_px > 0)
                pending_space = true;
            continue;
        }

        if (cp == '\n') {
            pending_space = false;

            if (line_px == 0) {
                if (blank_run)
                    continue;
                blank_run = 1;
            } else {
                blank_run = 0;
            }

            out[o++] = '\n';
            line_px = 0;
            continue;
        }

        /*
         * Find one complete word/token. Formatting markers are part of the
         * token but have zero width. Space, NBSP, CR and LF end the token.
         */
        size_t token_start = cp_start;
        size_t token_end = i;
        size_t scan = i;

        while (scan < n) {
            size_t next_start = scan;
            unsigned next_cp = utf8_next(in, n, &scan);

            if (next_cp == 0 || next_cp == ' ' || next_cp == 0xA0 ||
                next_cp == '\r' || next_cp == '\n') {
                scan = next_start;
                break;
            }

            token_end = scan;
        }

        int token_px = wrap_token_width(in, token_start, token_end);

        /*
         * Prefer moving the whole word to the next line. This is the key
         * Phase 1.5 behavior: "bookmarks" stays intact instead of becoming
         * "bookmar" / "ks" merely because the remaining pixels are short.
         */
        if (pending_space && line_px > 0) {
            if (max_px > 0 && line_px + space_w + token_px > max_px) {
                out[o++] = '\n';
                line_px = 0;
            } else {
                out[o++] = ' ';
                line_px += space_w;
            }
        }
        pending_space = false;

        /*
         * Emit the token UTF-8 codepoint by codepoint. Normally the whole
         * token fits because of the decision above. If a single token is
         * wider than the line (long URL, CJK without spaces, etc.), fall
         * back to UTF-8-safe glyph wrapping rather than overflowing.
         */
        size_t t = token_start;
        while (t < token_end) {
            size_t glyph_start = t;
            unsigned glyph = utf8_next(in, token_end, &t);
            size_t glyph_bytes = t - glyph_start;

            if (glyph == 0)
                break;

            int glyph_w = wrap_glyph_width(glyph);

            if (max_px > 0 && glyph_w > 0 && line_px > 0 &&
                line_px + glyph_w > max_px) {
                out[o++] = '\n';
                line_px = 0;
            }

            memcpy(out + o, in + glyph_start, glyph_bytes);
            o += glyph_bytes;
            line_px += glyph_w;
            blank_run = 0;
        }

        i = token_end;
    }

    out[o] = 0;
    return out;
}


/* ---------- Mini Browser 2.5 Phase 4: bounded session cookies ---------- */

typedef struct {
    bool used;
    bool secure;
    bool host_only;
    unsigned long age;
    char name[COOKIE_NAME_MAX + 1];
    char value[COOKIE_VALUE_MAX + 1];
    char domain[COOKIE_DOMAIN_MAX + 1];
    char path[COOKIE_PATH_MAX + 1];
} mb_cookie_t;

/*
 * IMPORTANT: these are static-storage objects, not automatic locals.
 * Phase 4 attempt #1 used several large automatic buffers in the curl/header
 * call chain and overflowed the BadgeVMS application task stack.
 */
static mb_cookie_t g_cookie_jar[MAX_COOKIES];
static unsigned long g_cookie_age = 1;
static char g_cookie_request_url[URL_MAX];
static char g_cookie_header_value[COOKIE_HEADER_MAX];
static char g_cookie_header_line[COOKIE_HEADER_MAX + 16];
static char g_cookie_set_line[COOKIE_SET_MAX];
static char g_cookie_host[COOKIE_DOMAIN_MAX + 1];
static char g_cookie_req_path[COOKIE_PATH_MAX + 1];
static char g_cookie_tmp_domain[COOKIE_DOMAIN_MAX + 1];
static char g_cookie_tmp_path[COOKIE_PATH_MAX + 1];
static char g_cookie_tmp_name[COOKIE_NAME_MAX + 1];
static char g_cookie_tmp_value[COOKIE_VALUE_MAX + 1];

static void cookie_copy_trim(char *dst, size_t cap,
                             const char *src, size_t n,
                             bool lower) {
    if (!dst || cap == 0) return;
    while (n && isspace((unsigned char)*src)) { src++; n--; }
    while (n && isspace((unsigned char)src[n - 1])) n--;
    if (n >= cap) n = cap - 1;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)src[i];
        dst[i] = lower ? (char)tolower(c) : (char)c;
    }
    dst[n] = 0;
}

static bool cookie_url_parts(const char *url, bool *is_secure) {
    const char *p;
    bool secure = false;

    if (!url) return false;
    if (!strncasecmp(url, "https://", 8)) {
        p = url + 8;
        secure = true;
    } else if (!strncasecmp(url, "http://", 7)) {
        p = url + 7;
    } else {
        return false;
    }

    const char *authority_end = p;
    while (*authority_end && *authority_end != '/' &&
           *authority_end != '?' && *authority_end != '#')
        authority_end++;

    const char *host_start = p;
    const char *host_end = authority_end;
    const char *at = NULL;
    for (const char *q = p; q < authority_end; q++)
        if (*q == '@') at = q;
    if (at) host_start = at + 1;

    if (host_start < host_end && *host_start == '[') {
        const char *rb = NULL;
        for (const char *q = host_start + 1; q < host_end; q++)
            if (*q == ']') { rb = q; break; }
        if (rb) {
            host_start++;
            host_end = rb;
        }
    } else {
        for (const char *q = host_start; q < authority_end; q++)
            if (*q == ':') { host_end = q; break; }
    }

    if (host_end <= host_start) return false;
    cookie_copy_trim(g_cookie_host, sizeof(g_cookie_host),
                     host_start, (size_t)(host_end - host_start), true);

    if (*authority_end == '/') {
        const char *end = authority_end;
        while (*end && *end != '?' && *end != '#') end++;
        cookie_copy_trim(g_cookie_req_path, sizeof(g_cookie_req_path),
                         authority_end, (size_t)(end - authority_end), false);
    } else {
        strcpy(g_cookie_req_path, "/");
    }

    if (!g_cookie_req_path[0]) strcpy(g_cookie_req_path, "/");
    if (is_secure) *is_secure = secure;
    return true;
}

static bool cookie_domain_match(const char *host, const char *domain) {
    size_t hl, dl;
    if (!host || !domain || !*host || !*domain) return false;
    if (!strcasecmp(host, domain)) return true;
    hl = strlen(host);
    dl = strlen(domain);
    return hl > dl && host[hl - dl - 1] == '.' &&
           !strcasecmp(host + hl - dl, domain);
}

static bool cookie_path_match(const char *request_path, const char *cookie_path) {
    size_t n;
    if (!request_path || !*request_path) request_path = "/";
    if (!cookie_path || !*cookie_path) cookie_path = "/";
    n = strlen(cookie_path);
    if (strncmp(request_path, cookie_path, n)) return false;
    if (!request_path[n]) return true;
    if (cookie_path[n - 1] == '/') return true;
    return request_path[n] == '/';
}

static void cookie_default_path(void) {
    const char *last = strrchr(g_cookie_req_path, '/');
    if (!last || last == g_cookie_req_path) {
        strcpy(g_cookie_tmp_path, "/");
        return;
    }
    cookie_copy_trim(g_cookie_tmp_path, sizeof(g_cookie_tmp_path),
                     g_cookie_req_path,
                     (size_t)(last - g_cookie_req_path), false);
}

static int cookie_count(void) {
    int n = 0;
    for (int i = 0; i < MAX_COOKIES; i++)
        if (g_cookie_jar[i].used) n++;
    return n;
}

static int cookie_find(const char *name, const char *domain, const char *path) {
    for (int i = 0; i < MAX_COOKIES; i++) {
        mb_cookie_t *c = &g_cookie_jar[i];
        if (c->used &&
            !strcmp(c->name, name) &&
            !strcasecmp(c->domain, domain) &&
            !strcmp(c->path, path))
            return i;
    }
    return -1;
}

static int cookie_store_slot(const char *name,
                             const char *domain,
                             const char *path) {
    int existing = cookie_find(name, domain, path);
    if (existing >= 0) return existing;

    for (int i = 0; i < MAX_COOKIES; i++)
        if (!g_cookie_jar[i].used) return i;

    int oldest = 0;
    for (int i = 1; i < MAX_COOKIES; i++)
        if (g_cookie_jar[i].age < g_cookie_jar[oldest].age)
            oldest = i;
    return oldest;
}

static void cookie_store_header(const char *value) {
    bool request_secure = false;
    bool secure = false;
    bool host_only = true;
    bool remove = false;

    if (!value || !cookie_url_parts(g_cookie_request_url, &request_secure))
        return;

    size_t n = strlen(value);
    if (n >= sizeof(g_cookie_set_line)) n = sizeof(g_cookie_set_line) - 1;
    memcpy(g_cookie_set_line, value, n);
    g_cookie_set_line[n] = 0;
    while (n && (g_cookie_set_line[n - 1] == '\r' ||
                 g_cookie_set_line[n - 1] == '\n'))
        g_cookie_set_line[--n] = 0;

    char *semi = strchr(g_cookie_set_line, ';');
    char *pair_end = semi ? semi : g_cookie_set_line + strlen(g_cookie_set_line);
    char *eq = memchr(g_cookie_set_line, '=', (size_t)(pair_end - g_cookie_set_line));
    if (!eq) return;

    cookie_copy_trim(g_cookie_tmp_name, sizeof(g_cookie_tmp_name),
                     g_cookie_set_line, (size_t)(eq - g_cookie_set_line), false);
    cookie_copy_trim(g_cookie_tmp_value, sizeof(g_cookie_tmp_value),
                     eq + 1, (size_t)(pair_end - eq - 1), false);
    if (!g_cookie_tmp_name[0]) return;

    strncpy(g_cookie_tmp_domain, g_cookie_host, sizeof(g_cookie_tmp_domain));
    g_cookie_tmp_domain[sizeof(g_cookie_tmp_domain) - 1] = 0;
    cookie_default_path();

    for (char *a = semi ? semi + 1 : NULL; a && *a; ) {
        while (*a == ';' || isspace((unsigned char)*a)) a++;
        if (!*a) break;

        char *next = strchr(a, ';');
        char *end = next ? next : a + strlen(a);
        char *aeq = memchr(a, '=', (size_t)(end - a));

        if (aeq) {
            *aeq = 0;
            char *av = aeq + 1;
            while (*a && isspace((unsigned char)*a)) a++;
            char *an_end = a + strlen(a);
            while (an_end > a && isspace((unsigned char)an_end[-1])) *--an_end = 0;
            while (av < end && isspace((unsigned char)*av)) av++;
            while (end > av && isspace((unsigned char)end[-1])) end--;
            *end = 0;

            if (!strcasecmp(a, "domain") && *av) {
                while (*av == '.') av++;
                cookie_copy_trim(g_cookie_tmp_domain, sizeof(g_cookie_tmp_domain),
                                 av, strlen(av), true);
                if (!cookie_domain_match(g_cookie_host, g_cookie_tmp_domain))
                    return;
                host_only = false;
            } else if (!strcasecmp(a, "path") && *av == '/') {
                cookie_copy_trim(g_cookie_tmp_path, sizeof(g_cookie_tmp_path),
                                 av, strlen(av), false);
            } else if (!strcasecmp(a, "max-age")) {
                char *ep = NULL;
                long age = strtol(av, &ep, 10);
                if (ep != av && age <= 0) remove = true;
            }
        } else {
            char saved = *end;
            *end = 0;
            while (*a && isspace((unsigned char)*a)) a++;
            char *an_end = a + strlen(a);
            while (an_end > a && isspace((unsigned char)an_end[-1])) *--an_end = 0;
            if (!strcasecmp(a, "secure")) secure = true;
            *end = saved;
        }

        a = next ? next + 1 : NULL;
    }

    int slot = cookie_find(g_cookie_tmp_name,
                           g_cookie_tmp_domain,
                           g_cookie_tmp_path);

    if (remove) {
        if (slot >= 0)
            memset(&g_cookie_jar[slot], 0, sizeof(g_cookie_jar[slot]));
        printf("[mini_browser] cookie delete: %s count=%d\n",
               g_cookie_tmp_name, cookie_count());
        return;
    }

    slot = cookie_store_slot(g_cookie_tmp_name,
                             g_cookie_tmp_domain,
                             g_cookie_tmp_path);
    mb_cookie_t *c = &g_cookie_jar[slot];
    memset(c, 0, sizeof(*c));
    c->used = true;
    c->secure = secure;
    c->host_only = host_only;
    c->age = g_cookie_age++;
    strncpy(c->name, g_cookie_tmp_name, sizeof(c->name) - 1);
    strncpy(c->value, g_cookie_tmp_value, sizeof(c->value) - 1);
    strncpy(c->domain, g_cookie_tmp_domain, sizeof(c->domain) - 1);
    strncpy(c->path, g_cookie_tmp_path, sizeof(c->path) - 1);

    printf("[mini_browser] cookie store: %s=%s domain=%s path=%s secure=%d count=%d\n",
           c->name, c->value, c->domain, c->path,
           c->secure ? 1 : 0, cookie_count());
}

static bool cookie_make_request_header(const char *url) {
    bool request_secure = false;
    size_t used = 0;
    bool any = false;

    g_cookie_header_value[0] = 0;
    if (!cookie_url_parts(url, &request_secure)) return false;

    for (int i = 0; i < MAX_COOKIES; i++) {
        mb_cookie_t *c = &g_cookie_jar[i];
        if (!c->used) continue;
        if (c->secure && !request_secure) continue;

        bool domain_ok = c->host_only
            ? !strcasecmp(g_cookie_host, c->domain)
            : cookie_domain_match(g_cookie_host, c->domain);
        if (!domain_ok || !cookie_path_match(g_cookie_req_path, c->path))
            continue;

        size_t need = strlen(c->name) + strlen(c->value) + 1 + (any ? 2 : 0);
        if (used + need + 1 > sizeof(g_cookie_header_value))
            break;

        if (any) {
            memcpy(g_cookie_header_value + used, "; ", 2);
            used += 2;
        }
        size_t x = strlen(c->name);
        memcpy(g_cookie_header_value + used, c->name, x);
        used += x;
        g_cookie_header_value[used++] = '=';
        x = strlen(c->value);
        memcpy(g_cookie_header_value + used, c->value, x);
        used += x;
        g_cookie_header_value[used] = 0;
        any = true;
    }

    return any;
}

static size_t cookie_header_cb(char *buffer, size_t size,
                               size_t nitems, void *userdata) {
    (void)userdata;
    size_t n = size * nitems;
    static const char prefix[] = "Set-Cookie:";
    const size_t plen = sizeof(prefix) - 1;

    if (buffer && n >= plen && !strncasecmp(buffer, prefix, plen)) {
        size_t vlen = n - plen;
        if (vlen >= sizeof(g_cookie_set_line))
            vlen = sizeof(g_cookie_set_line) - 1;
        memcpy(g_cookie_set_line, buffer + plen, vlen);
        g_cookie_set_line[vlen] = 0;
        cookie_store_header(g_cookie_set_line);
    }
    return n;
}


/* ---------- curl fetch (tolerant to trimmed-down libcurl) ---------- */
/* ---------- v1.2: proper HTTP status/error handling ---------- */

static int fetch_url(const char *url, const char *post_body, mem_t *m, long *http_status) {
    if (!url || !m) return -1;

    CURL *curl = curl_easy_init();
    if (!curl) return -2;

    m->buf = NULL;
    m->len = 0;

    memset(&g_fetch_meta, 0, sizeof(g_fetch_meta));
    strncpy(g_fetch_meta.effective_url, url,
            sizeof(g_fetch_meta.effective_url) - 1);
    strncpy(g_fetch_meta.request_method,
            post_body ? "POST" : "GET",
            sizeof(g_fetch_meta.request_method) - 1);
    g_fetch_meta.request_body_bytes = post_body ? strlen(post_body) : 0;
    g_fetch_meta.cookies_sent = 0;

    if (http_status) {
        *http_status = 0;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);

    if (post_body) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_body);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(post_body));
    }

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
        "User-Agent: Mozilla/5.0 (BadgeVMS; ESP32; rv:" MINI_BROWSER_VERSION ") "
        "Gecko/20100101 "
        "(compatible; MiniBrowser/" MINI_BROWSER_VERSION "; +https://github.com/mactjaap/mini_browser/; HTTP/1.1; identity)");

    hdrs = curl_slist_append(hdrs,
        "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8");

    hdrs = curl_slist_append(hdrs,
        "Accept-Language: en-US,en;q=0.5");

    hdrs = curl_slist_append(hdrs,
        "Accept-Encoding: identity");

    if (post_body) {
        hdrs = curl_slist_append(hdrs,
            "Content-Type: application/x-www-form-urlencoded");
    }

    if (cookie_make_request_header(url)) {
        snprintf(g_cookie_header_line, sizeof(g_cookie_header_line),
                 "Cookie: %s", g_cookie_header_value);
        hdrs = curl_slist_append(hdrs, g_cookie_header_line);

        /*
         * Count name=value pairs without retaining or displaying cookie values.
         * cookie_make_request_header() emits pairs separated by ';'.
         */
        g_fetch_meta.cookies_sent = 1;
        for (const char *p = g_cookie_header_value; *p; p++) {
            if (*p == ';') g_fetch_meta.cookies_sent++;
        }

        printf("[mini_browser] cookie send: %s\n", g_cookie_header_value);
    }

    if (hdrs) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    }

    strncpy(g_cookie_request_url, url, sizeof(g_cookie_request_url) - 1);
    g_cookie_request_url[sizeof(g_cookie_request_url) - 1] = 0;
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, cookie_header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, NULL);

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
     * Phase 5 metadata. These CURLINFO values are long-established libcurl
     * interfaces and are queried only after curl_easy_perform().
     */
    {
        char *effective = NULL;
        char *ctype = NULL;

        if (curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective) == CURLE_OK &&
            effective && effective[0]) {
            strncpy(g_fetch_meta.effective_url, effective,
                    sizeof(g_fetch_meta.effective_url) - 1);
            g_fetch_meta.effective_url[sizeof(g_fetch_meta.effective_url) - 1] = 0;
        }

        if (curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &ctype) == CURLE_OK &&
            ctype && ctype[0]) {
            strncpy(g_fetch_meta.content_type, ctype,
                    sizeof(g_fetch_meta.content_type) - 1);
            g_fetch_meta.content_type[sizeof(g_fetch_meta.content_type) - 1] = 0;
        }

        /*
         * Redirect metadata is not reliably exposed by the trimmed BadgeVMS
         * libcurl. CURLINFO_REDIRECT_COUNT is unavailable and the effective
         * URL can remain the originally requested URL after a redirect.
         */
        g_fetch_meta.redirect_count = 0;

        g_fetch_meta.downloaded_bytes = m->len;
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


/* Renderer functions used by the image viewer are defined later. */
static void draw_text(SDL_Renderer *r, int x, int y, const char *s, int max_w);
static void draw_ui(SDL_Renderer *r, const char *bar_text);

/* ---------- Phase 3 Candidate 3 bounded image subsystem ---------- */

typedef struct {
    uint16_t *rgb565;
    int width;
    int height;
    bool loaded;
    char url[URL_MAX];
} decoded_image_t;

static decoded_image_t g_inline_images[MAX_INLINE_IMAGES];
static decoded_image_t g_viewer_image;

static void decoded_image_release(decoded_image_t *image) {
    if (!image) return;
    free(image->rgb565);
    memset(image, 0, sizeof(*image));
}

static void image_release_all(void) {
    for (int i = 0; i < MAX_INLINE_IMAGES; i++)
        decoded_image_release(&g_inline_images[i]);
    decoded_image_release(&g_viewer_image);
}

/* Unlike the HTML sink, image downloads need their own 512 KiB cap. */
static size_t image_wr_cb(void *ptr, size_t sz, size_t nm, void *ud) {
    size_t n = sz * nm;
    mem_t *m = (mem_t*)ud;
    if (!m || !n) return n;

    if (m->len + n > IMAGE_DOWNLOAD_MAX)
        return 0; /* abort transfer: compressed image is too large */

    char *p = (char*)realloc(m->buf, m->len + n);
    if (!p) return 0;
    m->buf = p;
    memcpy(m->buf + m->len, ptr, n);
    m->len += n;
    return n;
}

/* Image fetch is separate from fetch_url(): it must not replace page metadata. */
static int fetch_image_bytes(const char *url, mem_t *m, long *http_status) {
    if (!url || !m) return -1;

    CURL *curl = curl_easy_init();
    if (!curl) return -2;

    m->buf = NULL;
    m->len = 0;
    if (http_status) *http_status = 0;

    curl_easy_setopt(curl, CURLOPT_URL, url);
#ifdef CURLOPT_FOLLOWLOCATION
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
#endif
#ifdef CURLOPT_MAXREDIRS
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
#endif
#ifdef CURLOPT_CONNECTTIMEOUT
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
#endif
#ifdef CURLOPT_TIMEOUT
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 25L);
#endif
#if defined(CURLOPT_HTTP_VERSION) && defined(CURL_HTTP_VERSION_1_1)
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
#endif

    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs,
        "User-Agent: MiniBrowser/" MINI_BROWSER_VERSION " BadgeVMS Phase3");
    hdrs = curl_slist_append(hdrs,
        "Accept: image/jpeg,image/png,image/*;q=0.5,*/*;q=0.1");
    hdrs = curl_slist_append(hdrs, "Accept-Encoding: identity");
    if (hdrs) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, image_wr_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, m);

    CURLcode res = curl_easy_perform(curl);

    if (http_status) {
        long code = 0;
        if (curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code) == CURLE_OK)
            *http_status = code;
    }

    if (hdrs) curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        free(m->buf);
        m->buf = NULL;
        m->len = 0;
        return (int)res;
    }
    return 0;
}

static void fit_image_size(int src_w, int src_h, int max_w, int max_h,
                           int *out_w, int *out_h) {
    int w = src_w, h = src_h;
    if (w > max_w) {
        h = (int)(((long long)h * max_w + w / 2) / w);
        w = max_w;
    }
    if (h > max_h) {
        w = (int)(((long long)w * max_h + h / 2) / h);
        h = max_h;
    }
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    *out_w = w;
    *out_h = h;
}

static int decode_web_image_bounded(const unsigned char *buf, size_t len,
                                    int target_max_w, int target_max_h,
                                    decoded_image_t *out) {
    if (!buf || !len || !out || len > INT_MAX)
        return 0;

    int w = 0, h = 0, channels = 0;
    if (!stbi_info_from_memory(buf, (int)len, &w, &h, &channels)) {
        printf("[mini_browser] image: stb info failed: %s\n",
               stbi_failure_reason() ? stbi_failure_reason() : "unknown");
        return 0;
    }

    if (w <= 0 || h <= 0 ||
        w > IMAGE_SOURCE_MAX_W || h > IMAGE_SOURCE_MAX_H) {
        printf("[mini_browser] image: rejected dimensions %dx%d (dimension limit %dx%d)\n",
               w, h, IMAGE_SOURCE_MAX_W, IMAGE_SOURCE_MAX_H);
        return 0;
    }

    size_t pixels = (size_t)w * (size_t)h;
    if (pixels > IMAGE_DECODE_MAX_BYTES / 3U) {
        printf("[mini_browser] image: rejected %dx%d: RGB decode would need %u bytes (budget %u)\n",
               w, h, (unsigned)(pixels * 3U), (unsigned)IMAGE_DECODE_MAX_BYTES);
        return 0;
    }

    unsigned char *rgb =
        stbi_load_from_memory(buf, (int)len, &w, &h, &channels, 3);
    if (!rgb) {
        printf("[mini_browser] image: stb decode failed: %s\n",
               stbi_failure_reason() ? stbi_failure_reason() : "unknown");
        return 0;
    }

    int dw = 0, dh = 0;
    fit_image_size(w, h, target_max_w, target_max_h, &dw, &dh);

    size_t retained_bytes = (size_t)dw * (size_t)dh * sizeof(uint16_t);

    /*
     * Candidate 3 Fix 8 memory-safety fix
     * ------------------------------------
     * Do NOT allocate a second RGB565 image while stb's full RGB888 decode
     * is still alive. On BadgeVMS a 640x480 JPEG needs 921600 bytes for the
     * temporary RGB888 decode; adding a separate 153600-byte 320x240 RGB565
     * buffer at the same time caused severe allocator pressure and corrupted
     * unrelated live allocations.
     *
     * Instead, downscale/convert forward into the beginning of stb's own
     * RGB888 allocation. This is overlap-safe:
     *
     *   source position grows as 3 bytes/pixel (or faster when downscaling)
     *   destination grows as 2 bytes/pixel
     *
     * Each source RGB triplet is copied into local r/g/b values before the
     * destination uint16_t is written, so the forward conversion never
     * destroys source bytes that a later output pixel still needs.
     */
    uint16_t *small = (uint16_t*)rgb;

    for (int y = 0; y < dh; y++) {
        int sy = (int)(((long long)y * h) / dh);
        for (int x = 0; x < dw; x++) {
            int sx = (int)(((long long)x * w) / dw);
            size_t off = ((size_t)sy * (size_t)w + (size_t)sx) * 3U;
            unsigned r = rgb[off + 0];
            unsigned g = rgb[off + 1];
            unsigned b = rgb[off + 2];

            small[(size_t)y * (size_t)dw + (size_t)x] =
                (uint16_t)(((r & 0xF8U) << 8) |
                           ((g & 0xFCU) << 3) |
                           (b >> 3));
        }
    }

    /*
     * Release the unused tail of the stb allocation only after conversion.
     * This is a shrink, not a second image allocation. If the allocator
     * cannot shrink/move it, reject the image rather than retaining the
     * original large RGB888-sized block.
     *
     * stb_image uses the normal malloc/realloc/free family in this build, so
     * the resulting pointer remains compatible with decoded_image_release().
     */
    void *shrunk = realloc(rgb, retained_bytes);
    if (!shrunk) {
        stbi_image_free(rgb);
        printf("[mini_browser] image: could not shrink RGB565 cache to %u bytes\n",
               (unsigned)retained_bytes);
        return 0;
    }
    small = (uint16_t*)shrunk;

    decoded_image_release(out);
    out->rgb565 = small;
    out->width = dw;
    out->height = dh;
    out->loaded = true;

    printf("[mini_browser] image: decoded source=%dx%d retained=%dx%d RGB565=%u bytes\n",
           w, h, dw, dh, (unsigned)retained_bytes);
    return 1;
}

static int load_image_url(const char *url, int target_max_w, int target_max_h,
                          decoded_image_t *out) {
    if (!url || !out) return 0;

    mem_t m = {0};
    long status = 0;
    printf("[mini_browser] image: fetching %s\n", url);

    int rc = fetch_image_bytes(url, &m, &status);
    if (rc != 0 || status >= 400) {
        printf("[mini_browser] image: fetch failed rc=%d HTTP=%ld\n", rc, status);
        free(m.buf);
        return 0;
    }

    int ok = decode_web_image_bounded((const unsigned char*)m.buf, m.len,
                                      target_max_w, target_max_h, out);
    if (ok) {
        strncpy(out->url, url, sizeof(out->url) - 1);
        printf("[mini_browser] image: loaded compressed=%u bytes\n",
               (unsigned)m.len);
    } else {
        printf("[mini_browser] image: unsupported, invalid, or outside memory budget\n");
    }

    free(m.buf);
    return ok;
}

static int load_page_images(const page_t *page) {
    for (int i = 0; i < MAX_INLINE_IMAGES; i++)
        decoded_image_release(&g_inline_images[i]);

    if (!page || g_display_mode != DISPLAY_COLORS_IMAGES)
        return 0;

    int count = page->image_count;
    if (count > MAX_INLINE_IMAGES) count = MAX_INLINE_IMAGES;

    int loaded = 0;
    for (int i = 0; i < count; i++) {
        if (load_image_url(page->images[i].src,
                           IMAGE_DRAW_MAX_W, IMAGE_DRAW_MAX_H,
                           &g_inline_images[i]))
            loaded++;
    }
    return loaded;
}

static void image_draw_size(const page_image_t *spec,
                            const decoded_image_t *image,
                            int max_w, int max_h,
                            int *out_w, int *out_h) {
    int src_w = image->width;
    int src_h = image->height;
    int draw_w = src_w;
    int draw_h = src_h;

    if (spec && spec->requested_width > 0) {
        draw_w = spec->requested_width;
        draw_h = (src_h * draw_w + src_w / 2) / src_w;
    } else if (spec && spec->requested_height > 0) {
        draw_h = spec->requested_height;
        draw_w = (src_w * draw_h + src_h / 2) / src_h;
    }

    if (spec && spec->requested_width > 0 && spec->requested_height > 0) {
        int h_by_w = (src_h * spec->requested_width + src_w / 2) / src_w;
        int w_by_h = (src_w * spec->requested_height + src_h / 2) / src_h;
        if (h_by_w <= spec->requested_height) {
            draw_w = spec->requested_width;
            draw_h = h_by_w;
        } else {
            draw_h = spec->requested_height;
            draw_w = w_by_h;
        }
    }

    fit_image_size(draw_w, draw_h, max_w, max_h, &draw_w, &draw_h);
    *out_w = draw_w;
    *out_h = draw_h;
}

static int draw_decoded_image(SDL_Renderer *r, const page_image_t *spec,
                              const decoded_image_t *image,
                              int x, int y, int max_w, int max_h) {
    if (!r || !image || !image->loaded || !image->rgb565)
        return 0;

    int draw_w = 0, draw_h = 0;
    image_draw_size(spec, image, max_w, max_h, &draw_w, &draw_h);

    for (int dy = 0; dy < draw_h; dy++) {
        int sy = (dy * image->height) / draw_h;
        for (int dx = 0; dx < draw_w; dx++) {
            int sx = (dx * image->width) / draw_w;
            uint16_t p = image->rgb565[(size_t)sy * (size_t)image->width + (size_t)sx];
            unsigned char rr = (unsigned char)(((p >> 11) & 0x1F) * 255 / 31);
            unsigned char gg = (unsigned char)(((p >> 5) & 0x3F) * 255 / 63);
            unsigned char bb = (unsigned char)((p & 0x1F) * 255 / 31);
            SDL_SetRenderDrawColor(r, rr, gg, bb, 255);
            SDL_RenderPoint(r, (float)(x + dx), (float)(y + dy));
        }
    }
    return draw_h;
}

static int is_image_marker_line(const char *line, int len, int *index) {
    if (!line || len < 10) return 0;
    char tmp[32];
    if (len >= (int)sizeof(tmp)) return 0;
    memcpy(tmp, line, (size_t)len);
    tmp[len] = 0;

    int n = -1;
    char extra = 0;
    if (sscanf(tmp, "[[MBIMG%d]]%c", &n, &extra) == 1 &&
        n >= 0 && n < MAX_INLINE_IMAGES) {
        if (index) *index = n;
        return 1;
    }
    return 0;
}

static int content_type_is_image(const char *content_type) {
    if (!content_type) return 0;
    return !strncasecmp(content_type, "image/jpeg", 10) ||
           !strncasecmp(content_type, "image/jpg", 9) ||
           !strncasecmp(content_type, "image/png", 9);
}

static int buffer_is_supported_image(const unsigned char *buf, size_t len) {
    if (!buf) return 0;

    /* JPEG SOI */
    if (len >= 3 &&
        buf[0] == 0xFF && buf[1] == 0xD8 && buf[2] == 0xFF)
        return 1;

    /* PNG signature */
    static const unsigned char png_sig[8] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A
    };
    if (len >= sizeof(png_sig) &&
        !memcmp(buf, png_sig, sizeof(png_sig)))
        return 1;

    return 0;
}

static void draw_image_viewer(SDL_Renderer *r, const decoded_image_t *image) {
    draw_ui(r, "Image Viewer - WHY+B/Esc to return");
    if (!image || !image->loaded) {
        draw_text(r, PAD_LR, PAD_TOP, "Image unavailable", VIEW_W - 2 * PAD_LR);
        return;
    }

    int max_w = VIEW_W - 2 * PAD_LR;
    int max_h = VIEW_H - PAD_TOP - PAD_BOTTOM;
    int w = 0, h = 0;
    fit_image_size(image->width, image->height, max_w, max_h, &w, &h);
    int x = (VIEW_W - w) / 2;
    int y = PAD_TOP + (max_h - h) / 2;
    draw_decoded_image(r, NULL, image, x, y, max_w, max_h);
}

/* ---------- tiny text renderer ---------- */


#define UNICODE_FONT_FILE "APPS:[mini_browser]unifont_cjk.bin"

#define UNICODE_CACHE_SIZE 128

typedef struct {
    uint32_t codepoint;
    unsigned char bitmap[UNICODE_GLYPH_BYTES];
    bool valid;
} unicode_cache_entry_t;

static FILE *g_unicode_font = NULL;
static unicode_cache_entry_t g_unicode_cache[UNICODE_CACHE_SIZE];

/*
 * Direct-indexed ranges are loaded from unifont_cjk.bin at runtime.
 *
 * MBCJ v1 file format:
 *
 *   bytes 0..3   "MBCJ"
 *   uint32 LE    format version (1)
 *   uint32 LE    range count
 *
 * followed by range_count records:
 *
 *   uint32 LE    first code point
 *   uint32 LE    last code point
 *   uint32 LE    glyph-data offset
 *
 * Every code-point slot in a range occupies exactly 32 bytes (16x16,
 * one bit per pixel). An all-zero slot means that GNU Unifont did not
 * provide a usable 8x16 or 16x16 glyph for that code point.
 *
 * Loading the table from the file rather than compiling offsets into
 * Mini Browser makes the external font independently replaceable. This
 * is especially useful for the much broader Unicode coverage in 2.4.
 */
typedef struct {
    uint32_t start;
    uint32_t end;
    uint32_t offset;
} unicode_font_range_t;

#define UNICODE_FONT_FORMAT_VERSION 1U
#define UNICODE_MAX_RANGES 128U

static unicode_font_range_t g_unicode_ranges[UNICODE_MAX_RANGES];
static uint32_t g_unicode_range_count = 0;

static uint32_t unicode_read_le32(const unsigned char *p) {
    return
        (uint32_t)p[0] |
        ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) |
        ((uint32_t)p[3] << 24);
}

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
        printf("Mini Browser: invalid Unicode font magic\n");
        fclose(g_unicode_font);
        g_unicode_font = NULL;
        return 0;
    }

    uint32_t version = unicode_read_le32(header + 4);
    uint32_t range_count = unicode_read_le32(header + 8);

    if (version != UNICODE_FONT_FORMAT_VERSION) {
        printf("Mini Browser: unsupported Unicode font format %lu\n",
               (unsigned long)version);
        fclose(g_unicode_font);
        g_unicode_font = NULL;
        return 0;
    }

    if (range_count == 0 || range_count > UNICODE_MAX_RANGES) {
        printf("Mini Browser: invalid Unicode range count %lu\n",
               (unsigned long)range_count);
        fclose(g_unicode_font);
        g_unicode_font = NULL;
        return 0;
    }

    uint32_t previous_end = 0;
    uint32_t table_end = 12U + range_count * 12U;

    for (uint32_t i = 0; i < range_count; i++) {
        unsigned char record[12];

        if (fread(record, 1, sizeof(record), g_unicode_font)
            != sizeof(record)) {
            printf("Mini Browser: Unicode range table read failed\n");
            fclose(g_unicode_font);
            g_unicode_font = NULL;
            g_unicode_range_count = 0;
            return 0;
        }

        unicode_font_range_t *range = &g_unicode_ranges[i];

        range->start = unicode_read_le32(record + 0);
        range->end = unicode_read_le32(record + 4);
        range->offset = unicode_read_le32(record + 8);

        if (range->start > range->end ||
            range->offset < table_end ||
            (i > 0 && range->start <= previous_end)) {
            printf("Mini Browser: invalid Unicode range record %lu\n",
                   (unsigned long)i);
            fclose(g_unicode_font);
            g_unicode_font = NULL;
            g_unicode_range_count = 0;
            return 0;
        }

        previous_end = range->end;
    }

    g_unicode_range_count = range_count;

    printf("Mini Browser: Unicode font opened: %s (%lu ranges)\n",
           UNICODE_FONT_FILE,
           (unsigned long)g_unicode_range_count);

    return 1;
}


static int unicode_font_offset(unsigned cp, long *offset) {
    if (!unicode_font_open()) {
        return 0;
    }

    for (uint32_t i = 0; i < g_unicode_range_count; i++) {
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


static void draw_char_italic(SDL_Renderer *r, int x, int y, char c) {
    if (!r) return;
    if ((unsigned char)c < 32 || (unsigned char)c > 127) c = '?';
    const unsigned char *cols = font5x7[(unsigned char)c - 32];
    for (int col = 0; col < FONT_W_COLS; col++) {
        unsigned char bits = cols[col];
        for (int row = 0; row < FONT_H_ROWS; row++) {
            if (bits & (1u << row)) {
                int skew = (FONT_H_ROWS - 1 - row) / 3;
                SDL_FRect px = { (float)(x + col*FONT_SCALE + skew), (float)(y + row*FONT_SCALE),
                                 (float)FONT_SCALE, (float)FONT_SCALE };
                SDL_RenderFillRect(r, &px);
            }
        }
    }
}

static void draw_unicode_char_italic(SDL_Renderer *r, int x, int y, unsigned cp) {
    unsigned char bitmap[UNICODE_GLYPH_BYTES];
    if (!load_unicode_glyph(cp, bitmap)) {
        draw_unicode_char(r, x, y, cp);
        return;
    }
    for (int row = 0; row < UNICODE_GLYPH_H; row++) {
        uint16_t bits = ((uint16_t)bitmap[row * 2] << 8) | (uint16_t)bitmap[row * 2 + 1];
        int skew = (UNICODE_GLYPH_H - 1 - row) / 5;
        for (int col = 0; col < UNICODE_GLYPH_W; col++) {
            if (bits & ((uint16_t)1 << (15 - col))) {
                SDL_FRect px = { (float)(x + col + skew), (float)(y + row), 1.0f, 1.0f };
                SDL_RenderFillRect(r, &px);
            }
        }
    }
}


typedef struct {
    unsigned cp;
    bool bold;
    bool underline;
    bool italic;
    unsigned char align;
    unsigned char color;
    bool custom_color;
    unsigned char r, g, b;
    bool custom_background;
    unsigned char bg_r, bg_g, bg_b;
    unsigned char dir;
} visual_glyph_t;

#define TEXT_COLOR_NORMAL  0
#define TEXT_COLOR_LINK    1
#define TEXT_COLOR_HEADING 2
#define TEXT_COLOR_FORM    3

#define BIDI_LINE_MAX 256
#define DIR_NEUTRAL 0
#define DIR_LTR     1
#define DIR_RTL     2

/*
 * Renderer scratch storage.
 *
 * BadgeVMS gives the app task a bounded stack.  These buffers used to be
 * automatic arrays in draw_text(), arabic_shape_line() and
 * bidi_visualize_line().  Those functions are nested, so their worst-case
 * stack usage accumulated and could trip the task's stack protector on
 * Unicode/Arabic pages.
 *
 * Rendering is single-threaded in Mini Browser, so one bounded static scratch
 * set is sufficient and preserves the existing algorithms without heap use.
 */
static visual_glyph_t g_render_line[BIDI_LINE_MAX];
static visual_glyph_t g_bidi_tmp[BIDI_LINE_MAX];
static unsigned g_arabic_original[BIDI_LINE_MAX];

typedef struct {
    uint32_t base;
    uint32_t isolated;
    uint32_t final;
    uint32_t initial;
    uint32_t medial;
} arabic_shape_t;

/*
 * Arabic presentation-form mappings used by the compact Phase 2 shaper.
 * A zero form means that joining form does not exist for that character.
 * The table covers the standard Arabic alphabet plus common Persian/Urdu
 * letters for which Unicode provides one-code-point presentation forms.
 */
static const arabic_shape_t g_arabic_shapes[] = {
    {0x0621,0xFE80,0,0,0},{0x0622,0xFE81,0xFE82,0,0},
    {0x0623,0xFE83,0xFE84,0,0},{0x0624,0xFE85,0xFE86,0,0},
    {0x0625,0xFE87,0xFE88,0,0},{0x0626,0xFE89,0xFE8A,0xFE8B,0xFE8C},
    {0x0627,0xFE8D,0xFE8E,0,0},{0x0628,0xFE8F,0xFE90,0xFE91,0xFE92},
    {0x0629,0xFE93,0xFE94,0,0},{0x062A,0xFE95,0xFE96,0xFE97,0xFE98},
    {0x062B,0xFE99,0xFE9A,0xFE9B,0xFE9C},{0x062C,0xFE9D,0xFE9E,0xFE9F,0xFEA0},
    {0x062D,0xFEA1,0xFEA2,0xFEA3,0xFEA4},{0x062E,0xFEA5,0xFEA6,0xFEA7,0xFEA8},
    {0x062F,0xFEA9,0xFEAA,0,0},{0x0630,0xFEAB,0xFEAC,0,0},
    {0x0631,0xFEAD,0xFEAE,0,0},{0x0632,0xFEAF,0xFEB0,0,0},
    {0x0633,0xFEB1,0xFEB2,0xFEB3,0xFEB4},{0x0634,0xFEB5,0xFEB6,0xFEB7,0xFEB8},
    {0x0635,0xFEB9,0xFEBA,0xFEBB,0xFEBC},{0x0636,0xFEBD,0xFEBE,0xFEBF,0xFEC0},
    {0x0637,0xFEC1,0xFEC2,0xFEC3,0xFEC4},{0x0638,0xFEC5,0xFEC6,0xFEC7,0xFEC8},
    {0x0639,0xFEC9,0xFECA,0xFECB,0xFECC},{0x063A,0xFECD,0xFECE,0xFECF,0xFED0},
    {0x0641,0xFED1,0xFED2,0xFED3,0xFED4},{0x0642,0xFED5,0xFED6,0xFED7,0xFED8},
    {0x0643,0xFED9,0xFEDA,0xFEDB,0xFEDC},{0x0644,0xFEDD,0xFEDE,0xFEDF,0xFEE0},
    {0x0645,0xFEE1,0xFEE2,0xFEE3,0xFEE4},{0x0646,0xFEE5,0xFEE6,0xFEE7,0xFEE8},
    {0x0647,0xFEE9,0xFEEA,0xFEEB,0xFEEC},{0x0648,0xFEED,0xFEEE,0,0},
    {0x0649,0xFEEF,0xFEF0,0xFBE8,0xFBE9},{0x064A,0xFEF1,0xFEF2,0xFEF3,0xFEF4},
    {0x0671,0xFB50,0xFB51,0,0},{0x0677,0xFBDD,0,0,0},
    {0x0679,0xFB66,0xFB67,0xFB68,0xFB69},{0x067A,0xFB5E,0xFB5F,0xFB60,0xFB61},
    {0x067B,0xFB52,0xFB53,0xFB54,0xFB55},{0x067E,0xFB56,0xFB57,0xFB58,0xFB59},
    {0x067F,0xFB62,0xFB63,0xFB64,0xFB65},{0x0680,0xFB5A,0xFB5B,0xFB5C,0xFB5D},
    {0x0683,0xFB76,0xFB77,0xFB78,0xFB79},{0x0684,0xFB72,0xFB73,0xFB74,0xFB75},
    {0x0686,0xFB7A,0xFB7B,0xFB7C,0xFB7D},{0x0687,0xFB7E,0xFB7F,0xFB80,0xFB81},
    {0x0688,0xFB88,0xFB89,0,0},{0x068C,0xFB84,0xFB85,0,0},
    {0x068D,0xFB82,0xFB83,0,0},{0x068E,0xFB86,0xFB87,0,0},
    {0x0691,0xFB8C,0xFB8D,0,0},{0x0698,0xFB8A,0xFB8B,0,0},
    {0x06A4,0xFB6A,0xFB6B,0xFB6C,0xFB6D},{0x06A6,0xFB6E,0xFB6F,0xFB70,0xFB71},
    {0x06A9,0xFB8E,0xFB8F,0xFB90,0xFB91},{0x06AD,0xFBD3,0xFBD4,0xFBD5,0xFBD6},
    {0x06AF,0xFB92,0xFB93,0xFB94,0xFB95},{0x06B1,0xFB9A,0xFB9B,0xFB9C,0xFB9D},
    {0x06B3,0xFB96,0xFB97,0xFB98,0xFB99},{0x06BA,0xFB9E,0xFB9F,0,0},
    {0x06BB,0xFBA0,0xFBA1,0xFBA2,0xFBA3},{0x06BE,0xFBAA,0xFBAB,0xFBAC,0xFBAD},
    {0x06C0,0xFBA4,0xFBA5,0,0},{0x06C1,0xFBA6,0xFBA7,0xFBA8,0xFBA9},
    {0x06C5,0xFBE0,0xFBE1,0,0},{0x06C6,0xFBD9,0xFBDA,0,0},
    {0x06C7,0xFBD7,0xFBD8,0,0},{0x06C8,0xFBDB,0xFBDC,0,0},
    {0x06C9,0xFBE2,0xFBE3,0,0},{0x06CB,0xFBDE,0xFBDF,0,0},
    {0x06CC,0xFBFC,0xFBFD,0xFBFE,0xFBFF},{0x06D0,0xFBE4,0xFBE5,0xFBE6,0xFBE7},
    {0x06D2,0xFBAE,0xFBAF,0,0},{0x06D3,0xFBB0,0xFBB1,0,0}
};

static const arabic_shape_t *arabic_shape_info(unsigned cp) {
    for (size_t i = 0; i < sizeof(g_arabic_shapes) / sizeof(g_arabic_shapes[0]); i++) {
        if (g_arabic_shapes[i].base == cp)
            return &g_arabic_shapes[i];
    }
    return NULL;
}

static bool arabic_transparent(unsigned cp) {
    return (cp >= 0x0610 && cp <= 0x061A) ||
           (cp >= 0x064B && cp <= 0x065F) ||
           cp == 0x0670 ||
           (cp >= 0x06D6 && cp <= 0x06ED);
}

static void arabic_shape_line(visual_glyph_t *g, int count) {
    unsigned *original = g_arabic_original;

    for (int i = 0; i < count; i++)
        original[i] = g[i].cp;

    for (int i = 0; i < count; i++) {
        const arabic_shape_t *cur = arabic_shape_info(original[i]);
        if (!cur)
            continue;

        int p = i - 1;
        while (p >= 0 && arabic_transparent(original[p]))
            p--;

        int n = i + 1;
        while (n < count && arabic_transparent(original[n]))
            n++;

        const arabic_shape_t *prev = p >= 0 ? arabic_shape_info(original[p]) : NULL;
        const arabic_shape_t *next = n < count ? arabic_shape_info(original[n]) : NULL;

        bool join_prev = prev && prev->initial && cur->final;
        bool join_next = next && cur->initial && next->final;

        if (join_prev && join_next && cur->medial)
            g[i].cp = cur->medial;
        else if (join_prev && cur->final)
            g[i].cp = cur->final;
        else if (join_next && cur->initial)
            g[i].cp = cur->initial;
        else if (cur->isolated)
            g[i].cp = cur->isolated;
    }
}

static unsigned char bidi_dir(unsigned cp) {
    /* European and Arabic-Indic numbers stay left-to-right as number runs. */
    if ((cp >= '0' && cp <= '9') ||
        (cp >= 0x0660 && cp <= 0x0669) ||
        (cp >= 0x06F0 && cp <= 0x06F9))
        return DIR_LTR;

    if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z'))
        return DIR_LTR;

    if ((cp >= 0x0590 && cp <= 0x05FF) ||
        (cp >= 0x0600 && cp <= 0x08FF) ||
        (cp >= 0xFB1D && cp <= 0xFDFF) ||
        (cp >= 0xFE70 && cp <= 0xFEFF))
        return DIR_RTL;

    if ((cp >= 0x00C0 && cp <= 0x02AF) ||
        (cp >= 0x0370 && cp <= 0x058F) ||
        (cp >= 0x0900 && cp <= 0x1FFF) ||
        (cp >= 0x2C00 && cp <= 0xD7FF))
        return DIR_LTR;

    return DIR_NEUTRAL;
}

static int visual_glyph_width(unsigned cp) {
    return wrap_glyph_width(cp);
}

static void reverse_glyphs(visual_glyph_t *g, int a, int b) {
    while (a < b) {
        visual_glyph_t t = g[a];
        g[a] = g[b];
        g[b] = t;
        a++;
        b--;
    }
}

/*
 * Compact embedded bidi pass. It handles the common browser cases we need:
 * RTL paragraph detection, Hebrew/Arabic runs, mixed LTR text and numbers,
 * and neutral punctuation/spaces. It is intentionally bounded and is not a
 * complete implementation of every Unicode Bidirectional Algorithm rule.
 */
static int bidi_visualize_line(visual_glyph_t *g, int count, bool *base_rtl) {
    if (count <= 0) {
        *base_rtl = false;
        return count;
    }

    unsigned char base = DIR_LTR;
    for (int i = 0; i < count; i++) {
        unsigned char d = bidi_dir(g[i].cp);
        if (d != DIR_NEUTRAL) {
            base = d;
            break;
        }
    }
    *base_rtl = (base == DIR_RTL);

    for (int i = 0; i < count; i++)
        g[i].dir = bidi_dir(g[i].cp);

    /* Resolve neutral characters from their neighbors, otherwise paragraph base. */
    for (int i = 0; i < count; i++) {
        if (g[i].dir != DIR_NEUTRAL)
            continue;

        unsigned char left = DIR_NEUTRAL;
        unsigned char right = DIR_NEUTRAL;

        for (int p = i - 1; p >= 0; p--) {
            if (g[p].dir != DIR_NEUTRAL) {
                left = g[p].dir;
                break;
            }
        }
        for (int n = i + 1; n < count; n++) {
            if (g[n].dir != DIR_NEUTRAL) {
                right = g[n].dir;
                break;
            }
        }

        g[i].dir = (left != DIR_NEUTRAL && left == right) ? left : base;
    }

    visual_glyph_t *tmp = g_bidi_tmp;
    int out = 0;

    if (base == DIR_LTR) {
        int i = 0;
        while (i < count) {
            int j = i + 1;
            while (j < count && g[j].dir == g[i].dir)
                j++;

            if (g[i].dir == DIR_RTL) {
                for (int k = j - 1; k >= i; k--)
                    tmp[out++] = g[k];
            } else {
                for (int k = i; k < j; k++)
                    tmp[out++] = g[k];
            }
            i = j;
        }
    } else {
        int j = count;
        while (j > 0) {
            int i = j - 1;
            while (i > 0 && g[i - 1].dir == g[j - 1].dir)
                i--;

            if (g[i].dir == DIR_RTL) {
                for (int k = j - 1; k >= i; k--)
                    tmp[out++] = g[k];
            } else {
                for (int k = i; k < j; k++)
                    tmp[out++] = g[k];
            }
            j = i;
        }
    }

    memcpy(g, tmp, (size_t)out * sizeof(g[0]));
    return out;
}

static void draw_visual_line(SDL_Renderer *r, int x, int y,
                             visual_glyph_t *g, int count, int max_w) {
    if (count <= 0)
        return;

    arabic_shape_line(g, count);

    bool base_rtl = false;
    count = bidi_visualize_line(g, count, &base_rtl);

    int line_w = 0;
    for (int i = 0; i < count; i++)
        line_w += visual_glyph_width(g[i].cp);

    int cx = x;
    unsigned char align = count > 0 ? g[0].align : 0;
    if (align == 2 && line_w < max_w)
        cx = x + (max_w - line_w) / 2;
    else if (align == 3 && line_w < max_w)
        cx = x + max_w - line_w;
    else if (align == 1)
        cx = x;
    else if (base_rtl && line_w < max_w)
        cx = x + max_w - line_w;

    int cy = y;

    /* A real <hr> is represented by one zero-width internal marker. */
    for (int i = 0; i < count; i++) {
        if (g[i].cp == TEXT_HRULE) {
            SDL_SetRenderDrawColor(r, 70, 90, 100, 255);
            SDL_RenderLine(r, (float)x, (float)(cy + CH_H / 2),
                           (float)(x + max_w), (float)(cy + CH_H / 2));
            return;
        }
    }

    for (int i = 0; i < count; i++) {
        unsigned cp = g[i].cp;
        if (cp == 0xA0)
            cp = ' ';

        int char_w = visual_glyph_width(cp);
        if (char_w <= 0)
            continue;

        if (cx + char_w > x + max_w) {
            cx = x;
            cy += (CH_H + LINE_SPACING);
        }

        /* Phase 2B: paint the background cell first.  This intentionally
         * follows the rendered text run rather than introducing block geometry. */
        if (g_display_mode != DISPLAY_BW && g[i].custom_background) {
            SDL_SetRenderDrawColor(r, g[i].bg_r, g[i].bg_g, g[i].bg_b, 255);
            SDL_FRect bg_rect = { (float)cx, (float)cy, (float)char_w, (float)CH_H };
            SDL_RenderFillRect(r, &bg_rect);
        }

        if (g_display_mode == DISPLAY_BW) {
            SDL_SetRenderDrawColor(r, 220, 220, 220, 255);
        } else if (g[i].custom_color) {
            SDL_SetRenderDrawColor(r, g[i].r, g[i].g, g[i].b, 255);
        } else switch (g[i].color) {
            case TEXT_COLOR_LINK:
                SDL_SetRenderDrawColor(r, 0, 220, 255, 255);
                break;
            case TEXT_COLOR_HEADING:
                SDL_SetRenderDrawColor(r, 185, 90, 255, 255);
                break;
            case TEXT_COLOR_FORM:
                SDL_SetRenderDrawColor(r, 255, 205, 70, 255);
                break;
            default:
                SDL_SetRenderDrawColor(r, 220, 220, 220, 255);
                break;
        }

        if (cp >= 32 && cp <= 126) {
            if (g[i].italic) draw_char_italic(r, cx, cy, (char)cp);
            else draw_char(r, cx, cy, (char)cp);
            if (g[i].bold) {
                if (g[i].italic) draw_char_italic(r, cx + 1, cy, (char)cp);
                else draw_char(r, cx + 1, cy, (char)cp);
            }
        } else if (cp >= 0x80 && cp <= 0x10FFFF) {
            if (g[i].italic) draw_unicode_char_italic(r, cx, cy, cp);
            else draw_unicode_char(r, cx, cy, cp);
            if (g[i].bold) {
                if (g[i].italic) draw_unicode_char_italic(r, cx + 1, cy, cp);
                else draw_unicode_char(r, cx + 1, cy, cp);
            }
        }

        if (g[i].underline && cp != ' ') {
            SDL_RenderLine(r, (float)cx, (float)(cy + CH_H + 1),
                           (float)(cx + char_w - 2), (float)(cy + CH_H + 1));
        }

        cx += char_w;
    }
}

static void debug_print_content_clean(const char *s) {
    if (!s) return;
    size_t i = 0, len = strlen(s);
    while (i < len) {
        size_t start = i;
        unsigned cp = utf8_next(s, len, &i);
        if (cp == TEXT_BOLD_ON || cp == TEXT_BOLD_OFF ||
            cp == TEXT_LINK_ON || cp == TEXT_LINK_OFF ||
            cp == TEXT_HEADING_ON || cp == TEXT_HEADING_OFF ||
            cp == TEXT_HRULE || cp == TEXT_FORM_ON || cp == TEXT_FORM_OFF ||
            cp == TEXT_COLOR_START ||
            (cp >= TEXT_COLOR_NIBBLE && cp <= TEXT_COLOR_NIBBLE + 15) ||
            cp == TEXT_COLOR_POP || cp == TEXT_COLOR_INHERIT ||
        cp == TEXT_STYLE_START ||
        (cp >= TEXT_STYLE_NIBBLE && cp <= TEXT_STYLE_NIBBLE + 15) ||
        cp == TEXT_STYLE_POP ||
        cp == TEXT_BG_START ||
        (cp >= TEXT_BG_NIBBLE && cp <= TEXT_BG_NIBBLE + 15) ||
        cp == TEXT_BG_POP || cp == TEXT_BG_INHERIT || cp == TEXT_BG_TRANSPARENT)
            continue;
        fwrite(s + start, 1, i - start, stdout);
    }
}

static void draw_text(SDL_Renderer *r, int x, int y, const char *s, int max_w) {
    if (!r || !s) return;

    size_t i = 0;
    size_t L = strlen(s);
    int cy = y;
    int bold_depth = 0;
    int link_depth = 0;
    int heading_depth = 0;
    int form_depth = 0;
    struct { bool custom; unsigned char r,g,b; } color_stack[32];
    int color_depth = 0;
    bool custom_color = false; unsigned char custom_r=0, custom_g=0, custom_b=0;
    int color_nibbles = -1;
    unsigned color_value = 0;
    struct { bool bold_set, bold, italic_set, italic, underline_set, underline; unsigned char align; } style_stack[32];
    int style_depth = 0;
    bool css_bold_set = false, css_bold = false;
    bool css_italic_set = false, css_italic = false;
    bool css_underline_set = false, css_underline = false;
    unsigned char css_align = 0;
    int style_nibbles = -1;
    unsigned style_value = 0;
    struct { bool custom; unsigned char r,g,b; } background_stack[32];
    int background_depth = 0;
    bool custom_background = false;
    unsigned char background_r = 0, background_g = 0, background_b = 0;
    int background_nibbles = -1;
    unsigned background_value = 0;

    while (i <= L) {
        visual_glyph_t *line = g_render_line;
        int count = 0;
        bool saw_newline = false;

        while (i < L) {
            unsigned cp = utf8_next(s, L, &i);
            if (cp == 0)
                break;

            if (cp == TEXT_BOLD_ON) {
                bold_depth++;
                continue;
            }
            if (cp == TEXT_BOLD_OFF) {
                if (bold_depth > 0)
                    bold_depth--;
                continue;
            }
            if (cp == TEXT_LINK_ON) { link_depth++; continue; }
            if (cp == TEXT_LINK_OFF) { if (link_depth > 0) link_depth--; continue; }
            if (cp == TEXT_HEADING_ON) { heading_depth++; continue; }
            if (cp == TEXT_HEADING_OFF) { if (heading_depth > 0) heading_depth--; continue; }
            if (cp == TEXT_FORM_ON) { form_depth++; continue; }
            if (cp == TEXT_FORM_OFF) { if (form_depth > 0) form_depth--; continue; }
            if (cp == TEXT_COLOR_START) {
                color_nibbles = 0;
                color_value = 0;
                continue;
            }
            if (color_nibbles >= 0 &&
                cp >= TEXT_COLOR_NIBBLE && cp <= TEXT_COLOR_NIBBLE + 15) {
                color_value = (color_value << 4) | (unsigned)(cp - TEXT_COLOR_NIBBLE);
                color_nibbles++;
                if (color_nibbles == 6) {
                    if (color_depth < 32) {
                        color_stack[color_depth].custom = custom_color;
                        color_stack[color_depth].r = custom_r;
                        color_stack[color_depth].g = custom_g;
                        color_stack[color_depth].b = custom_b;
                        color_depth++;
                    }
                    custom_color = true;
                    custom_r = (unsigned char)((color_value >> 16) & 0xFF);
                    custom_g = (unsigned char)((color_value >> 8) & 0xFF);
                    custom_b = (unsigned char)(color_value & 0xFF);
                    color_nibbles = -1;
                    color_value = 0;
                }
                continue;
            }
            if (cp == TEXT_COLOR_INHERIT) {
                if (color_depth < 32) {
                    color_stack[color_depth].custom = custom_color;
                    color_stack[color_depth].r = custom_r;
                    color_stack[color_depth].g = custom_g;
                    color_stack[color_depth].b = custom_b;
                    color_depth++;
                }
                color_nibbles = -1;
                color_value = 0;
                continue;
            }
            if (cp == TEXT_COLOR_POP) {
                if (color_depth > 0) {
                    color_depth--;
                    custom_color = color_stack[color_depth].custom;
                    custom_r = color_stack[color_depth].r;
                    custom_g = color_stack[color_depth].g;
                    custom_b = color_stack[color_depth].b;
                }
                color_nibbles = -1;
                color_value = 0;
                continue;
            }
            if (cp == TEXT_STYLE_START) {
                style_nibbles = 0; style_value = 0; continue;
            }
            if (style_nibbles >= 0 && cp >= TEXT_STYLE_NIBBLE && cp <= TEXT_STYLE_NIBBLE + 15) {
                style_value = (style_value << 4) | (unsigned)(cp - TEXT_STYLE_NIBBLE);
                style_nibbles++;
                if (style_nibbles == 2) {
                    if (style_depth < 32) {
                        style_stack[style_depth].bold_set = css_bold_set;
                        style_stack[style_depth].bold = css_bold;
                        style_stack[style_depth].italic_set = css_italic_set;
                        style_stack[style_depth].italic = css_italic;
                        style_stack[style_depth].underline_set = css_underline_set;
                        style_stack[style_depth].underline = css_underline;
                        style_stack[style_depth].align = css_align;
                        style_depth++;
                    }
                    unsigned char f = (unsigned char)style_value;
                    if (f & 0x80) { css_bold_set = true; css_bold = (f & 0x40) != 0; }
                    if (f & 0x20) { css_italic_set = true; css_italic = (f & 0x10) != 0; }
                    if (f & 0x08) { css_underline_set = true; css_underline = (f & 0x04) != 0; }
                    if (f & 0x03) css_align = f & 0x03;
                    style_nibbles = -1; style_value = 0;
                }
                continue;
            }
            if (cp == TEXT_STYLE_POP) {
                if (style_depth > 0) {
                    style_depth--;
                    css_bold_set = style_stack[style_depth].bold_set;
                    css_bold = style_stack[style_depth].bold;
                    css_italic_set = style_stack[style_depth].italic_set;
                    css_italic = style_stack[style_depth].italic;
                    css_underline_set = style_stack[style_depth].underline_set;
                    css_underline = style_stack[style_depth].underline;
                    css_align = style_stack[style_depth].align;
                }
                style_nibbles = -1; style_value = 0; continue;
            }
            if (cp == TEXT_BG_START) {
                background_nibbles = 0; background_value = 0; continue;
            }
            if (background_nibbles >= 0 && cp >= TEXT_BG_NIBBLE && cp <= TEXT_BG_NIBBLE + 15) {
                background_value = (background_value << 4) | (unsigned)(cp - TEXT_BG_NIBBLE);
                background_nibbles++;
                if (background_nibbles == 6) {
                    if (background_depth < 32) {
                        background_stack[background_depth].custom = custom_background;
                        background_stack[background_depth].r = background_r;
                        background_stack[background_depth].g = background_g;
                        background_stack[background_depth].b = background_b;
                        background_depth++;
                    }
                    custom_background = true;
                    background_r = (unsigned char)((background_value >> 16) & 0xFF);
                    background_g = (unsigned char)((background_value >> 8) & 0xFF);
                    background_b = (unsigned char)(background_value & 0xFF);
                    background_nibbles = -1; background_value = 0;
                }
                continue;
            }
            if (cp == TEXT_BG_INHERIT || cp == TEXT_BG_TRANSPARENT) {
                if (background_depth < 32) {
                    background_stack[background_depth].custom = custom_background;
                    background_stack[background_depth].r = background_r;
                    background_stack[background_depth].g = background_g;
                    background_stack[background_depth].b = background_b;
                    background_depth++;
                }
                if (cp == TEXT_BG_TRANSPARENT) custom_background = false;
                background_nibbles = -1; background_value = 0;
                continue;
            }
            if (cp == TEXT_BG_POP) {
                if (background_depth > 0) {
                    background_depth--;
                    custom_background = background_stack[background_depth].custom;
                    background_r = background_stack[background_depth].r;
                    background_g = background_stack[background_depth].g;
                    background_b = background_stack[background_depth].b;
                }
                background_nibbles = -1; background_value = 0;
                continue;
            }
            if (cp == '\n') {
                saw_newline = true;
                break;
            }

            if (count < BIDI_LINE_MAX) {
                line[count].cp = cp;
                line[count].bold = css_bold_set ? css_bold : (bold_depth > 0 || heading_depth > 0);
                line[count].underline = css_underline_set ? css_underline : (link_depth > 0);
                line[count].italic = css_italic_set ? css_italic : false;
                line[count].align = css_align;
                line[count].color = heading_depth > 0 ? TEXT_COLOR_HEADING :
                                    link_depth > 0 ? TEXT_COLOR_LINK :
                                    form_depth > 0 ? TEXT_COLOR_FORM : TEXT_COLOR_NORMAL;
                line[count].custom_color = custom_color;
                line[count].r = custom_r; line[count].g = custom_g; line[count].b = custom_b;
                line[count].custom_background = custom_background;
                line[count].bg_r = background_r; line[count].bg_g = background_g; line[count].bg_b = background_b;
                line[count].dir = DIR_NEUTRAL;
                count++;
            }
        }

        draw_visual_line(r, x, cy, line, count, max_w);

        if (saw_newline) {
            cy += (CH_H + LINE_SPACING);
            continue;
        }
        break;
    }
}



/* ---------- Mini Browser logo (tiny globe + orbit) ---------- */
static void logo_point(SDL_Renderer *r, int x, int y) {
    SDL_RenderPoint(r, (float)x, (float)y);
}

static void logo_line(SDL_Renderer *r, int x0, int y0, int x1, int y1) {
    SDL_RenderLine(r, (float)x0, (float)y0, (float)x1, (float)y1);
}

static void logo_circle(SDL_Renderer *r, int cx, int cy, int radius) {
    int x = radius;
    int y = 0;
    int err = 1 - x;

    while (x >= y) {
        logo_point(r, cx + x, cy + y);
        logo_point(r, cx + y, cy + x);
        logo_point(r, cx - y, cy + x);
        logo_point(r, cx - x, cy + y);
        logo_point(r, cx - x, cy - y);
        logo_point(r, cx - y, cy - x);
        logo_point(r, cx + y, cy - x);
        logo_point(r, cx + x, cy - y);

        y++;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            x--;
            err += 2 * (y - x) + 1;
        }
    }
}

static void draw_logo(SDL_Renderer *r) {
    if (!r) return;

    /*
     * 20x20 approximation of the Mini Browser logo:
     * cyan/blue globe, purple/magenta orbit and a small magenta planet.
     * It is drawn directly with SDL primitives: no image decoder, file I/O,
     * heap allocation or extra framebuffer is needed.
     */
    const int left = ICON_LEFT;
    const int top  = ICON_TOP;
    const int cx   = left + 9;
    const int cy   = top + 10;

    /* Dark tile: visually merges into the browser title bar. */
    SDL_SetRenderDrawColor(r, 8, 10, 12, 255);
    SDL_FRect bg = { (float)left, (float)top,
                     (float)ICON_SIZE, (float)ICON_SIZE };
    SDL_RenderFillRect(r, &bg);

    /* Cyan/blue globe outline. */
    SDL_SetRenderDrawColor(r, 0, 220, 255, 255);
    logo_circle(r, cx, cy, 7);

    /* Globe latitude lines. */
    logo_line(r, cx - 6, cy - 3, cx + 6, cy - 3);
    logo_line(r, cx - 7, cy,     cx + 7, cy);
    logo_line(r, cx - 6, cy + 3, cx + 6, cy + 3);

    /* Globe longitude curves, approximated at this tiny resolution. */
    logo_line(r, cx,     cy - 7, cx,     cy + 7);
    logo_line(r, cx - 2, cy - 6, cx - 4, cy);
    logo_line(r, cx - 4, cy,     cx - 2, cy + 6);
    logo_line(r, cx + 2, cy - 6, cx + 4, cy);
    logo_line(r, cx + 4, cy,     cx + 2, cy + 6);

    /* Blue highlight on the lower-left edge. */
    SDL_SetRenderDrawColor(r, 0, 120, 255, 255);
    logo_line(r, cx - 6, cy + 4, cx - 3, cy + 7);
    logo_line(r, cx - 3, cy + 7, cx + 2, cy + 7);

    /* Purple/magenta orbital ring crossing the globe. */
    SDL_SetRenderDrawColor(r, 150, 35, 255, 255);
    logo_line(r, left + 1,  top + 14, left + 5,  top + 11);
    logo_line(r, left + 5,  top + 11, left + 11, top + 9);
    logo_line(r, left + 11, top + 9,  left + 16, top + 6);
    logo_line(r, left + 16, top + 6,  left + 18, top + 4);

    /* Brighter cyan front part of the orbit at lower-left. */
    SDL_SetRenderDrawColor(r, 0, 230, 255, 255);
    logo_line(r, left,     top + 15, left + 4, top + 15);
    logo_line(r, left + 4, top + 15, left + 8, top + 13);

    /* Magenta planet at the end of the orbit. */
    SDL_SetRenderDrawColor(r, 235, 35, 255, 255);
    SDL_FRect planet = { (float)(left + 17), (float)(top + 2), 3.0f, 3.0f };
    SDL_RenderFillRect(r, &planet);

    /* Tiny bright highlight keeps the planet readable at 20x20. */
    SDL_SetRenderDrawColor(r, 255, 150, 255, 255);
    logo_point(r, left + 17, top + 2);
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

/* ---------- Phase 5: Page Information ---------- */

static page_t *page_info_to_page(const page_t *source,
                                 const char *requested_url,
                                 long http_status) {
    page_t *pg = (page_t*)calloc(1, sizeof(page_t));
    if (!pg) return NULL;

    strncpy(pg->base, "page-info:", URL_MAX);
    pg->base[URL_MAX - 1] = 0;

    strncpy(pg->title, "HTTP / TLS Inspector", sizeof(pg->title));
    pg->title[sizeof(pg->title) - 1] = 0;

    /*
     * Phase 5B remains deliberately bounded. The inspector reports only facts
     * that are either known locally or exposed by this BadgeVMS libcurl build.
     * It performs no extra HEAD request and allocates no large automatic data.
     */
    const size_t cap = 2304;
    char *text = (char*)malloc(cap);
    if (!text) {
        free(pg);
        return NULL;
    }

    const char *title =
        (source && source->title[0]) ? source->title : "(none)";
    const char *effective = "(not reported)";
    const char *ctype =
        g_fetch_meta.content_type[0]
            ? g_fetch_meta.content_type
            : "(not reported)";
    const char *method =
        g_fetch_meta.request_method[0]
            ? g_fetch_meta.request_method
            : "(unknown)";
    const char *transport_url = requested_url ? requested_url : "";
    bool is_https = !strncasecmp(transport_url, "https://", 8);
    bool is_http = !strncasecmp(transport_url, "http://", 7);
    const char *transport =
        is_https ? "HTTPS" :
        is_http  ? "HTTP"  : "(unknown)";

    int links = source ? source->link_count : 0;
    int forms = source ? source->form_count : 0;
    int actions = source ? source->action_count : 0;

    snprintf(text, cap,
             "= HTTP / TLS INSPECTOR =\n\n"

             "PAGE\n"
             "Title:\n%s\n"
             "Downloaded: %u / %u bytes\n"
             "Links: %d / %d\n"
             "Forms: %d / %d\n"
             "Actions: %d / %d\n\n"

             "REQUEST\n"
             "Method: %s\n"
             "URL:\n%s\n"
             "Transport: %s\n"
             "POST body: %u bytes\n"
             "Cookies sent: %d\n\n"

             "RESPONSE\n"
             "Status: %ld\n"
             "Content-Type: %s\n"
             "Effective URL: %s\n"
             "Redirect information: not available\n"
             "Negotiated HTTP version: not available\n\n"

             "CONNECTION\n"
             "Remote IP: not available\n"
             "Remote port: not available\n\n"

             "TLS\n"
             "TLS: %s\n"
             "Certificate details: not available\n"
             "Certificate verification result: not available\n\n"

             "COOKIE JAR\n"
             "Stored: %d / %d\n\n"

             "LIBCURL NOTES\n"
             "Available CURLINFO: response code,\n"
             "content length, content type,\n"
             "effective URL.\n"
             "Effective URL is unreliable after redirects.\n\n"

             "Press WHY+B or WHY+I to return.",
             title,
             (unsigned)g_fetch_meta.downloaded_bytes,
             (unsigned)MAX_BYTES,
             links, MAX_LINKS,
             forms, MAX_FORMS,
             actions, MAX_ACTIONS,
             method,
             requested_url ? requested_url : "(unknown)",
             transport,
             (unsigned)g_fetch_meta.request_body_bytes,
             g_fetch_meta.cookies_sent,
             http_status,
             ctype,
             effective,
             is_https ? "yes" : (is_http ? "no" : "(unknown)"),
             cookie_count(),
             MAX_COOKIES);

    text[cap - 1] = 0;
    pg->text = text;

    /*
     * Keep the Phase 5 diagnostic stable: the existing 49/49 regression suite
     * depends on it.
     */
    printf("[mini_browser] page info: status=%ld bytes=%u redirects=na "
           "effective=na content_type=%s cookies=%d links=%d forms=%d actions=%d "
           "requested=%s final=%s\n",
           http_status,
           (unsigned)g_fetch_meta.downloaded_bytes,
           ctype,
           cookie_count(),
           links,
           forms,
           actions,
           requested_url ? requested_url : "(unknown)",
           effective);

    printf("[mini_browser] inspector: method=%s body_bytes=%u cookies_sent=%d "
           "transport=%s http_version=na remote_ip=na remote_port=na "
           "tls=%s certinfo=na certverify=na\n",
           method,
           (unsigned)g_fetch_meta.request_body_bytes,
           g_fetch_meta.cookies_sent,
           transport,
           is_https ? "yes" : (is_http ? "no" : "unknown"));

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


/* ---------- Serial screenshot streaming ---------- */

/*
 * WHY+S requests a screenshot.
 *
 * The renderer is read back in small horizontal stripes so we never need
 * another full-screen framebuffer allocation. Each stripe is converted to
 * RGB24, RLE-compressed, then transported over the shared serial console.
 *
 * The console is not a lossless bulk-data channel: unrelated firmware tasks
 * also write log messages to it. To make screenshot delivery resilient, each
 * data record carries its own CRC32 and every group of four data records gets
 * one XOR parity record. The Mac can therefore reconstruct any one missing or
 * damaged record per group. A final CRC32 still validates the whole compressed
 * screenshot before PNG creation.
 *
 * Wire format (FEC1):
 *
 *   IMG BEGIN <width> <height> RGB24 RLE5FEC1 <chunk-size> <group-size>
 *   IMG D <sequence> <length> <crc32> <base64-data>
 *   IMG P <group> <crc32> <base64-parity>
 *   ...
 *   IMG END <compressed-bytes> <crc32> <data-chunks>
 *
 * Data chunks are padded with zeroes only for parity calculation. The length
 * field is the actual number of compressed bytes in that data chunk.
 */

#define SCREENSHOT_STRIPE_H      8
#define IMG_RAW_CHUNK           48
#define IMG_FEC_GROUP            4
#define SCREENSHOT_PACE_MS       8

typedef struct {
    unsigned char raw[IMG_RAW_CHUNK];
    size_t used;
    unsigned sequence;
    unsigned long compressed_bytes;
    unsigned crc;

    unsigned char parity[IMG_RAW_CHUNK];
    unsigned parity_count;
    unsigned parity_group;
} screenshot_stream_t;

static unsigned screenshot_crc32_update(unsigned crc,
                                        const unsigned char *data,
                                        size_t len) {
    while (len--) {
        crc ^= *data++;

        for (int bit = 0; bit < 8; bit++) {
            unsigned mask = (unsigned)-(int)(crc & 1U);
            crc = (crc >> 1) ^ (0xEDB88320U & mask);
        }
    }

    return crc;
}

static unsigned screenshot_crc32(const unsigned char *data, size_t len) {
    unsigned crc = 0xFFFFFFFFU;
    crc = screenshot_crc32_update(crc, data, len);
    return crc ^ 0xFFFFFFFFU;
}

static size_t screenshot_base64_encode(char *out,
                                       size_t out_size,
                                       const unsigned char *data,
                                       size_t len) {
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789+/";

    size_t needed = 4U * ((len + 2U) / 3U);

    if (!out || out_size < needed + 1U) {
        return 0;
    }

    size_t i = 0;
    size_t o = 0;

    while (i + 3U <= len) {
        unsigned a = data[i++];
        unsigned b = data[i++];
        unsigned c = data[i++];

        out[o++] = table[(a >> 2) & 0x3F];
        out[o++] = table[((a & 0x03) << 4) | ((b >> 4) & 0x0F)];
        out[o++] = table[((b & 0x0F) << 2) | ((c >> 6) & 0x03)];
        out[o++] = table[c & 0x3F];
    }

    size_t remaining = len - i;

    if (remaining == 1U) {
        unsigned a = data[i];

        out[o++] = table[(a >> 2) & 0x3F];
        out[o++] = table[(a & 0x03) << 4];
        out[o++] = '=';
        out[o++] = '=';
    } else if (remaining == 2U) {
        unsigned a = data[i];
        unsigned b = data[i + 1U];

        out[o++] = table[(a >> 2) & 0x3F];
        out[o++] = table[((a & 0x03) << 4) | ((b >> 4) & 0x0F)];
        out[o++] = table[(b & 0x0F) << 2];
        out[o++] = '=';
    }

    out[o] = '\0';
    return o;
}

static void screenshot_transport_pause(void) {
#if defined(ESP_PLATFORM)
    vTaskDelay(pdMS_TO_TICKS(SCREENSHOT_PACE_MS));
#else
    SDL_Delay(SCREENSHOT_PACE_MS);
#endif
}

static bool screenshot_send_parity(screenshot_stream_t *stream) {
    if (!stream || stream->parity_count == 0) {
        return true;
    }

    char encoded[(IMG_RAW_CHUNK * 4 / 3) + 8];
    size_t encoded_len = screenshot_base64_encode(
        encoded,
        sizeof(encoded),
        stream->parity,
        IMG_RAW_CHUNK
    );

    if (encoded_len == 0) {
        printf("IMG ERROR parity-base64-buffer\n");
        fflush(stdout);
        return false;
    }

    unsigned crc = screenshot_crc32(stream->parity, IMG_RAW_CHUNK);

    printf("IMG P %06u %08X %s\n",
           stream->parity_group,
           crc,
           encoded);
    fflush(stdout);
    screenshot_transport_pause();

    memset(stream->parity, 0, sizeof(stream->parity));
    stream->parity_count = 0;
    stream->parity_group++;
    return true;
}

static bool screenshot_stream_flush(screenshot_stream_t *stream) {
    if (!stream || stream->used == 0) {
        return true;
    }

    char encoded[(IMG_RAW_CHUNK * 4 / 3) + 8];
    size_t encoded_len = screenshot_base64_encode(
        encoded,
        sizeof(encoded),
        stream->raw,
        stream->used
    );

    if (encoded_len == 0) {
        printf("IMG ERROR data-base64-buffer\n");
        fflush(stdout);
        stream->used = 0;
        return false;
    }

    unsigned chunk_crc = screenshot_crc32(stream->raw, stream->used);

    printf("IMG D %06u %02u %08X %s\n",
           stream->sequence,
           (unsigned)stream->used,
           chunk_crc,
           encoded);
    fflush(stdout);
    screenshot_transport_pause();

    for (size_t i = 0; i < stream->used; i++) {
        stream->parity[i] ^= stream->raw[i];
    }

    stream->sequence++;
    stream->parity_count++;
    stream->used = 0;

    if (stream->parity_count == IMG_FEC_GROUP) {
        return screenshot_send_parity(stream);
    }

    return true;
}

static bool screenshot_stream_bytes(screenshot_stream_t *stream,
                                    const unsigned char *data,
                                    size_t len) {
    if (!stream || !data) {
        return false;
    }

    stream->crc = screenshot_crc32_update(stream->crc, data, len);
    stream->compressed_bytes += (unsigned long)len;

    while (len > 0) {
        size_t room = IMG_RAW_CHUNK - stream->used;
        size_t take = len < room ? len : room;

        memcpy(stream->raw + stream->used, data, take);
        stream->used += take;
        data += take;
        len -= take;

        if (stream->used == IMG_RAW_CHUNK) {
            if (!screenshot_stream_flush(stream)) {
                return false;
            }
        }
    }

    return true;
}

static bool screenshot_emit_run(screenshot_stream_t *stream,
                                unsigned count,
                                unsigned char r,
                                unsigned char g,
                                unsigned char b) {
    unsigned char record[5];

    record[0] = (unsigned char)(count & 0xFFU);
    record[1] = (unsigned char)((count >> 8) & 0xFFU);
    record[2] = r;
    record[3] = g;
    record[4] = b;

    return screenshot_stream_bytes(stream, record, sizeof(record));
}

static bool screenshot_stream_capture_region(SDL_Renderer *renderer,
                                             screenshot_stream_t *stream,
                                             int capture_height) {
    if (!renderer || !stream || capture_height <= 0 || capture_height > VIEW_H) {
        return false;
    }

    for (int top = 0; top < capture_height; top += SCREENSHOT_STRIPE_H) {
        int stripe_h = SCREENSHOT_STRIPE_H;

        if (top + stripe_h > capture_height) {
            stripe_h = capture_height - top;
        }

        SDL_Rect rect = { 0, top, VIEW_W, stripe_h };
        SDL_Surface *captured = SDL_RenderReadPixels(renderer, &rect);

        if (!captured) {
            printf("IMG ERROR read-pixels y=%d error=%s\n",
                   top, SDL_GetError());
            fflush(stdout);
            return false;
        }

        SDL_Surface *rgb = SDL_ConvertSurface(
            captured,
            SDL_PIXELFORMAT_RGB24
        );
        SDL_DestroySurface(captured);

        if (!rgb) {
            printf("IMG ERROR convert-rgb24 y=%d error=%s\n",
                   top, SDL_GetError());
            fflush(stdout);
            return false;
        }

        bool have_run = false;
        unsigned run_count = 0;
        unsigned char run_r = 0;
        unsigned char run_g = 0;
        unsigned char run_b = 0;
        bool ok = true;

        for (int y = 0; y < rgb->h && ok; y++) {
            const unsigned char *row =
                (const unsigned char *)rgb->pixels +
                (size_t)y * (size_t)rgb->pitch;

            for (int x = 0; x < rgb->w; x++) {
                const unsigned char *pixel = row + (size_t)x * 3U;
                unsigned char r = pixel[0];
                unsigned char g = pixel[1];
                unsigned char b = pixel[2];

                if (have_run &&
                    r == run_r &&
                    g == run_g &&
                    b == run_b &&
                    run_count < 65535U) {
                    run_count++;
                    continue;
                }

                if (have_run &&
                    !screenshot_emit_run(stream, run_count,
                                         run_r, run_g, run_b)) {
                    ok = false;
                    break;
                }

                have_run = true;
                run_count = 1;
                run_r = r;
                run_g = g;
                run_b = b;
            }
        }

        if (ok && have_run) {
            ok = screenshot_emit_run(stream, run_count,
                                     run_r, run_g, run_b);
        }

        SDL_DestroySurface(rgb);

        if (!ok) {
            return false;
        }

        YIELD_NET();
    }

    return true;
}

static bool screenshot_stream_begin(screenshot_stream_t *stream,
                                    int width,
                                    int height) {
    if (!stream || width <= 0 || height <= 0) {
        return false;
    }

    memset(stream, 0, sizeof(*stream));
    stream->crc = 0xFFFFFFFFU;

    printf("IMG BEGIN %d %d RGB24 RLE5FEC1 %d %d\n",
           width, height, IMG_RAW_CHUNK, IMG_FEC_GROUP);
    fflush(stdout);
    screenshot_transport_pause();
    return true;
}

static bool screenshot_stream_finish(screenshot_stream_t *stream) {
    if (!stream) {
        return false;
    }

    if (!screenshot_stream_flush(stream)) {
        return false;
    }

    if (!screenshot_send_parity(stream)) {
        return false;
    }

    unsigned final_crc = stream->crc ^ 0xFFFFFFFFU;

    /* Repeat END so one damaged terminator does not force a timeout. */
    for (int repeat = 0; repeat < 2; repeat++) {
        printf("IMG END %lu %08X %u\n",
               stream->compressed_bytes,
               final_crc,
               stream->sequence);
        fflush(stdout);
        screenshot_transport_pause();
    }

    return true;
}

static bool screenshot_stream_renderer(SDL_Renderer *renderer) {
    if (!renderer) {
        printf("IMG ERROR no-renderer\n");
        fflush(stdout);
        return false;
    }

    screenshot_stream_t stream;

    if (!screenshot_stream_begin(&stream, VIEW_W, VIEW_H)) {
        return false;
    }

    if (!screenshot_stream_capture_region(renderer, &stream, VIEW_H)) {
        return false;
    }

    return screenshot_stream_finish(&stream);
}

static int screenshot_wrapped_line_count(const char *content_wrapped) {
    if (!content_wrapped || !*content_wrapped) {
        return 0;
    }

    int lines = 0;
    const char *p = content_wrapped;

    while (p && *p) {
        lines++;
        const char *nl = strchr(p, '\n');
        p = nl ? nl + 1 : NULL;
    }

    return lines;
}

static int screenshot_full_page_height(const char *content_wrapped) {
    int lines = screenshot_wrapped_line_count(content_wrapped);
    int line_step = CH_H + LINE_SPACING;
    int height = PAD_TOP + PAD_BOTTOM;

    if (lines > 0) {
        height += lines * line_step;
    }

    if (height < VIEW_H) {
        height = VIEW_H;
    }

    return height;
}

static void screenshot_render_full_page_slice(SDL_Renderer *renderer,
                                              const char *bar_text,
                                              const char *content_wrapped,
                                              int slice_top,
                                              int slice_height) {
    if (!renderer || slice_top < 0 || slice_height <= 0) {
        return;
    }

    if (slice_top == 0) {
        /* The browser chrome belongs only at the top of the long image. */
        draw_ui(renderer, bar_text);
    } else {
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
    }

    if (!content_wrapped || !*content_wrapped) {
        return;
    }

    const int line_step = CH_H + LINE_SPACING;
    const int slice_bottom = slice_top + slice_height;
    const char *p = content_wrapped;
    int line_index = 0;

    while (p && *p) {
        const char *nl = strchr(p, '\n');
        int len = nl ? (int)(nl - p) : (int)strlen(p);
        int logical_y = PAD_TOP + line_index * line_step;

        if (logical_y + CH_H > slice_top && logical_y < slice_bottom) {
            char tmp[1024];
            if (len > (int)sizeof(tmp) - 1) {
                len = (int)sizeof(tmp) - 1;
            }

            memcpy(tmp, p, (size_t)len);
            tmp[len] = 0;

            SDL_SetRenderDrawColor(renderer, 220, 220, 220, 255);
            draw_text(renderer,
                      PAD_LR,
                      logical_y - slice_top,
                      tmp,
                      VIEW_W - 2 * PAD_LR);
        }

        line_index++;
        p = nl ? nl + 1 : NULL;
    }
}

static bool screenshot_stream_full_page(SDL_Renderer *renderer,
                                        const char *bar_text,
                                        const char *content_wrapped) {
    if (!renderer) {
        printf("IMG ERROR no-renderer\n");
        fflush(stdout);
        return false;
    }

    int full_height = screenshot_full_page_height(content_wrapped);
    screenshot_stream_t stream;

    printf("[mini_browser] full-page screenshot height=%d\n", full_height);
    fflush(stdout);

    if (!screenshot_stream_begin(&stream, VIEW_W, full_height)) {
        return false;
    }

    for (int slice_top = 0; slice_top < full_height; slice_top += VIEW_H) {
        int slice_height = VIEW_H;

        if (slice_top + slice_height > full_height) {
            slice_height = full_height - slice_top;
        }

        screenshot_render_full_page_slice(renderer,
                                          bar_text,
                                          content_wrapped,
                                          slice_top,
                                          slice_height);

        if (!screenshot_stream_capture_region(renderer,
                                              &stream,
                                              slice_height)) {
            return false;
        }

        YIELD_NET();
    }

    return screenshot_stream_finish(&stream);
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
    bool pending_post = false;
    char post_body[POST_BODY_MAX] = "";
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

    /* Phase 5 Page Information return state. */
    char page_info_return_url[URL_MAX] = "";
    bool viewing_page_info = false;

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
    bool screenshot_pending = false;      /* WHY+S: visible 716x716 frame */
    bool full_screenshot_pending = false; /* WHY+Z: complete rendered page */
    bool options_open = false;
    bool image_viewer_open = false;            /* WHY+O: Phase 3 display mode menu */

    const int max_cols = (VIEW_W - 2*PAD_LR) / CH_W;
    const int lines_per_page = (VIEW_H - PAD_TOP - PAD_BOTTOM) / (CH_H + LINE_SPACING);

    SDL_StartTextInput(win);

    snprintf(barline, sizeof(barline), "%s", url_buf);
    draw_ui(ren, barline);
    SDL_RenderPresent(ren);

    int running = 1;
    while (running) {

                if (need_fetch) {
            image_viewer_open = false;
            decoded_image_release(&g_viewer_image);
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
                    viewing_page_info = false;

                    /* record in history just before fetching (unless reloading) */
                    if (!history_navigation) {
                        history_push(url_buf);
                    }
                    history_navigation = false;

                    /*
                     * User-facing fetch state.  Keep HTTP status codes out of the
                     * normal title bar; detailed HTTP errors are rendered in the
                     * page itself.
                     */
                    snprintf(barline, sizeof(barline), "Loading...");
                    draw_ui(ren, barline);
                    SDL_RenderPresent(ren);

                    /* start edit v1.2 */
                    mem_t m = {0};
                    long http_status = 0;

                    if (pending_post) {
                        printf("[mini_browser] POST %s body=%s\n",
                               url_buf, post_body);
                    }

                    int rc = fetch_url(url_buf,
                                       pending_post ? post_body : NULL,
                                       &m, &http_status);

                    pending_post = false;
                    post_body[0] = 0;
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

			} else if (content_type_is_image(g_fetch_meta.content_type) ||
                                   buffer_is_supported_image(
                                       (const unsigned char *)m.buf, m.len)) {
                            printf("[mini_browser] direct image: content-type=%s magic=%s URL=%s\n",
                                   g_fetch_meta.content_type[0]
                                       ? g_fetch_meta.content_type : "(none)",
                                   buffer_is_supported_image(
                                       (const unsigned char *)m.buf, m.len)
                                       ? "yes" : "no",
                                   url_buf);
                            decoded_image_release(&g_viewer_image);
                            if (g_display_mode == DISPLAY_COLORS_IMAGES &&
                                load_image_url(url_buf, IMAGE_VIEW_MAX_W,
                                               IMAGE_VIEW_MAX_H,
                                               &g_viewer_image)) {
                                image_viewer_open = true;
                                snprintf(status_message, sizeof(status_message),
                                         "Image loaded");
                                status_message_until = SDL_GetTicks() + 1000;
                            } else {
                                image_viewer_open = false;
                                free(content_wrapped);
                                content_wrapped = wrap_text(
                                    g_display_mode == DISPLAY_COLORS_IMAGES
                                        ? "IMAGE UNAVAILABLE\n\nThe JPEG/PNG could not be decoded within the configured memory limits."
                                        : "IMAGE\n\nImages are disabled in the current display mode. Press WHY+O and select Colors + Images.",
                                    max_cols);
                            }
                            scroll_lines = 0;
                            sel_action = -1;

			} else {
    			debug_utf8("CURL", m.buf ? m.buf : "");

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

                            load_page_images(pg);

    			char *wrapped = wrap_text(pg->text, max_cols);

    			debug_utf8("WRAPPED", wrapped);

    			free(content_wrapped);
                            content_wrapped = wrapped;

                            free_page(page);

                            page = pg;
                            scroll_lines = 0;
                            sel_action = -1;

                            snprintf(status_message,
                                     sizeof(status_message),
                                     "Loaded");
                            status_message_until =
                                SDL_GetTicks() + 1000;

                            printf("[mini_browser] HTTP %ld, %u bytes, %d links from %s\n",
                                   http_status,
                                   (unsigned)m.len,
                                   page->link_count,
                                   url_buf);

                            /*
                             * Deterministic parser diagnostic.  Besides being
                             * useful while debugging, the 2.5 regression suite
                             * uses this to verify that HTML entities in <title>
                             * were decoded into page->title.
                             */
                            printf("[mini_browser] page title: %s\n",
                                   page->title[0] ? page->title : "(none)");
                            printf("[mini_browser] parser: links=%d actions=%d forms=%d\n",
                                   page->link_count,
                                   page->action_count,
                                   page->form_count);
                            printf("[mini_browser] visual: explicit_colors=%d\n", page->explicit_color_count);
                            printf("[mini_browser] visual: explicit_styles=%d\n", page->explicit_style_count);
                            printf("[mini_browser] visual: explicit_backgrounds=%d\n", page->explicit_background_count);
                            int inline_loaded = 0;
                            for (int ii = 0; ii < MAX_INLINE_IMAGES; ii++)
                                if (g_inline_images[ii].loaded) inline_loaded++;
                            printf("[mini_browser] display: mode=%s images_seen=%d images_retained=%d images_loaded=%d\n",
                                   display_mode_name(g_display_mode),
                                   page->image_seen_count, page->image_count,
                                   inline_loaded);
                            printf("[mini_browser] cookies: count=%d\n",
                                   cookie_count());

                            if (wrapped) {
                                printf("\n--- CONTENT START ---\n");
                                debug_print_content_clean(wrapped);
                                printf("\n--- CONTENT END ---\n");
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
        if ((screenshot_pending || full_screenshot_pending) &&
            page && page->title[0]) {

            /*
             * Screenshots should always contain the clean page title, even if
             * a transient "Loaded" or bookmark status is still active.
             */
            snprintf(barline, sizeof(barline), "%s",
                     page->title);

        } else if (status_message[0] &&
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
            } else if (action->type == ACTION_IMAGE &&
                       action->image_index >= 0 &&
                       action->image_index < page->image_count) {
                snprintf(barline, sizeof(barline), "[%d/%d] Image: %s",
                         sel_action + 1, page->action_count,
                         page->images[action->image_index].alt);
            } else {
                snprintf(barline, sizeof(barline), "[%d/%d]",
                         sel_action + 1, page->action_count);
            }

        } else if (page && page->title[0]) {
            /*
             * Normal idle state: show the page title, not HTTP 200.
             * HTTP failures are already shown as readable page content.
             */
            snprintf(barline, sizeof(barline), "%s",
                     page->title);

        } else {
            snprintf(barline, sizeof(barline), "%s", url_buf);
        }

        /* Draw UI + visible text slice */
        if (image_viewer_open) {
            draw_image_viewer(ren, &g_viewer_image);
        } else if (options_open) {
            draw_ui(ren, "Mini Browser - Options");
            int oy = PAD_TOP;
            char option_line[128];

            draw_text(ren, PAD_LR, oy, "MINI BROWSER OPTIONS", VIEW_W - 2*PAD_LR);
            oy += 2 * (CH_H + LINE_SPACING);
            draw_text(ren, PAD_LR, oy, "Display mode", VIEW_W - 2*PAD_LR);
            oy += (CH_H + LINE_SPACING);

            snprintf(option_line, sizeof(option_line), "%s 1. Black & White",
                     g_display_mode == DISPLAY_BW ? "(*)" : "( )");
            draw_text(ren, PAD_LR, oy, option_line, VIEW_W - 2*PAD_LR);
            oy += (CH_H + LINE_SPACING);

            snprintf(option_line, sizeof(option_line), "%s 2. Colors",
                     g_display_mode == DISPLAY_COLORS ? "(*)" : "( )");
            draw_text(ren, PAD_LR, oy, option_line, VIEW_W - 2*PAD_LR);
            oy += (CH_H + LINE_SPACING);

            snprintf(option_line, sizeof(option_line), "%s 3. Colors + Images",
                     g_display_mode == DISPLAY_COLORS_IMAGES ? "(*)" : "( )");
            draw_text(ren, PAD_LR, oy, option_line, VIEW_W - 2*PAD_LR);
            oy += 2 * (CH_H + LINE_SPACING);

            draw_text(ren, PAD_LR, oy, "Phase 3 image support:", VIEW_W - 2*PAD_LR);
            oy += (CH_H + LINE_SPACING);
            draw_text(ren, PAD_LR, oy, "5 inline, extra images as links, JPEG/PNG", VIEW_W - 2*PAD_LR);
            oy += (CH_H + LINE_SPACING);
            draw_text(ren, PAD_LR, oy, "Press 1/2/3 to select, Esc to cancel", VIEW_W - 2*PAD_LR);
        } else {
            draw_ui(ren, barline);
            if (content_wrapped) {
                int y = PAD_TOP;
                const char *p = content_wrapped;
                for (int ss = 0; ss < scroll_lines && p && *p; ) { if (*p++ == '\n') ss++; }
                SDL_SetRenderDrawColor(ren, 220, 220, 220, 255);
                int drawn = 0;
                while (p && *p && drawn < lines_per_page &&
                       y < VIEW_H - PAD_BOTTOM) {
                    const char *nl = strchr(p, '\n');
                    int len = nl ? (int)(nl - p) : (int)strlen(p);
                    int image_index = -1;

                    if (is_image_marker_line(p, len, &image_index)) {
                        if (image_index >= 0 &&
                            image_index < MAX_INLINE_IMAGES &&
                            g_inline_images[image_index].loaded) {
                            int image_h = draw_decoded_image(
                                ren, &page->images[image_index],
                                &g_inline_images[image_index],
                                PAD_LR, y, VIEW_W - 2*PAD_LR,
                                IMAGE_DRAW_MAX_H);
                            y += image_h + LINE_SPACING;
                        } else {
                            draw_text(ren, PAD_LR, y, "[Image unavailable]",
                                      VIEW_W - 2*PAD_LR);
                            y += (CH_H + LINE_SPACING);
                        }
                    } else {
                        char tmp[1024];
                        if (len > (int)sizeof(tmp)-1) len = (int)sizeof(tmp)-1;
                        memcpy(tmp, p, len); tmp[len] = 0;
                        draw_text(ren, PAD_LR, y, tmp, VIEW_W - 2*PAD_LR);
                        y += (CH_H + LINE_SPACING);
                    }

                    drawn++;
                    p = nl ? nl + 1 : NULL;
                }
            }
        }
        if (full_screenshot_pending) {
            screenshot_stream_full_page(ren, barline, content_wrapped);
            full_screenshot_pending = false;

            /*
             * Full-page capture reuses the 716x716 renderer as a scratch
             * surface. Do not present its final slice on the badge; the next
             * loop iteration redraws the user's unchanged viewport.
             */
            continue;
        }

        if (screenshot_pending) {
            screenshot_stream_renderer(ren);
            screenshot_pending = false;
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

                /* Phase 3 options menu consumes ordinary 1/2/3/Escape. */
                if (options_open) {
                    display_mode_t selected = g_display_mode;
                    bool changed = false;

                    if (sc == SDL_SCANCODE_1) {
                        selected = DISPLAY_BW; changed = true;
                    } else if (sc == SDL_SCANCODE_2) {
                        selected = DISPLAY_COLORS; changed = true;
                    } else if (sc == SDL_SCANCODE_3) {
                        selected = DISPLAY_COLORS_IMAGES; changed = true;
                    } else if (sc == SDL_SCANCODE_ESCAPE) {
                        options_open = false;
                        inhibit_text_once = true;
                        continue;
                    }

                    if (changed) {
                        g_display_mode = selected;
                        options_open = false;
                        image_release_all();
                        if (is_http_scheme(url_buf))
                            image_viewer_open = false;
                            decoded_image_release(&g_viewer_image);
                            need_fetch = 1; /* reparse <img> for the new mode */
                        scroll_lines = 0;
                        sel_action = -1;
                        snprintf(status_message, sizeof(status_message),
                                 "MODE: %s", display_mode_name(g_display_mode));
                        status_message_until = SDL_GetTicks() + 1500;
                        printf("[mini_browser] display mode: %s\\n",
                               display_mode_name(g_display_mode));
                        inhibit_text_once = true;
                        continue;
                    }

                    /* Ignore other keys while the modal options page is open. */
                    continue;
                }

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

                        case SDL_SCANCODE_I: { /* PAGE INFORMATION */
                            if (viewing_page_info && page_info_return_url[0]) {
                                strncpy(url_buf, page_info_return_url, URL_MAX);
                                url_buf[URL_MAX - 1] = 0;
                                page_info_return_url[0] = 0;
                                viewing_page_info = false;
                                history_navigation = true;
                                need_fetch = 1;
                                sel_action = -1;
                            } else if (page && is_http_scheme(url_buf)) {
                                strncpy(page_info_return_url, url_buf, URL_MAX);
                                page_info_return_url[URL_MAX - 1] = 0;

                                page_t *pg =
                                    page_info_to_page(page, url_buf, last_http_status);

                                if (pg) {
                                    char *wrapped = wrap_text(pg->text, max_cols);

                                    free(content_wrapped);
                                    content_wrapped = wrapped;

                                    free_page(page);
                                    page = pg;

                                    strncpy(url_buf, "page-info:", URL_MAX);
                                    url_buf[URL_MAX - 1] = 0;

                                    scroll_lines = 0;
                                    sel_action = -1;
                                    url_editing = false;
                                    form_editing = false;
                                    link_number_mode = false;
                                    link_number_len = 0;
                                    link_number_buf[0] = 0;
                                    viewing_page_info = true;
                                }
                            }

                            inhibit_text_once = true;
                            break;
                        }

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
                                viewing_page_info = false;
                                page_info_return_url[0] = 0;

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
                            if (image_viewer_open) {
                                image_viewer_open = false;
                                decoded_image_release(&g_viewer_image);
                            } else if (viewing_page_info &&
                                page_info_return_url[0]) {

                                strncpy(url_buf,
                                        page_info_return_url,
                                        URL_MAX);

                                url_buf[URL_MAX - 1] = 0;
                                page_info_return_url[0] = 0;

                                viewing_page_info = false;
                                history_navigation = true;
                                need_fetch = 1;
                                sel_action = -1;

                            } else if (viewing_bookmarks &&
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

                        case SDL_SCANCODE_S:
                            screenshot_pending = true;
                            inhibit_text_once = true;
                            printf("[mini_browser] screenshot requested; clean frame queued\n");
                            break;

                        case SDL_SCANCODE_Z:
                            full_screenshot_pending = true;
                            inhibit_text_once = true;
                            printf("[mini_browser] full-page screenshot requested; clean page queued\n");
                            break;

                        case SDL_SCANCODE_O:
                            options_open = true;
                            inhibit_text_once = true;
                            printf("[mini_browser] options opened; current mode=%s\n",
                                   display_mode_name(g_display_mode));
                            break;

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
                                &form_edit_cursor,
                                post_body, sizeof(post_body));
                            if (result == ACTIVATE_EDIT_FIELD) {
                                form_editing = true;
                                url_editing = false;
                            } else if (result == ACTIVATE_NAVIGATE) {
                                pending_post = false;
                                post_body[0] = 0;
                                viewing_bookmarks = false;
                                bookmark_return_url[0] = 0;
                                history_navigation = false;
                                need_fetch = 1;
                            } else if (result == ACTIVATE_POST) {
                                pending_post = true;
                                viewing_bookmarks = false;
                                bookmark_return_url[0] = 0;
                                history_navigation = false;
                                need_fetch = 1;
                            } else if (result == ACTIVATE_IMAGE) {
                                if (action_index >= 0 &&
                                    action_index < page->action_count) {
                                    int image_index = page->actions[action_index].image_index;
                                    if (image_index >= 0 && image_index < page->image_count &&
                                        g_display_mode == DISPLAY_COLORS_IMAGES) {
                                        decoded_image_release(&g_viewer_image);
                                        if (load_image_url(page->images[image_index].src,
                                                           IMAGE_VIEW_MAX_W, IMAGE_VIEW_MAX_H,
                                                           &g_viewer_image)) {
                                            image_viewer_open = true;
                                        } else {
                                            snprintf(status_message, sizeof(status_message),
                                                     "IMAGE UNAVAILABLE");
                                            status_message_until = SDL_GetTicks() + 2000;
                                        }
                                    }
                                }
                            } else if (result == ACTIVATE_FORM_UNSUPPORTED) {
                                snprintf(status_message, sizeof(status_message),
                                         "FORM METHOD/ENCODING NOT SUPPORTED");
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
                        } else if (link_number_mode && link_number_len > 0) {
                            link_number_len--;
                            link_number_buf[link_number_len] = '\0';
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
                        } else if (link_number_mode && link_number_len > 0) {
                            link_number_len--;
                            link_number_buf[link_number_len] = '\0';
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
                        if (image_viewer_open) {
                            image_viewer_open = false;
                            decoded_image_release(&g_viewer_image);
                        } else if (form_editing) {
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
    image_release_all();
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
