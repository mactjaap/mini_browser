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
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdint.h>
#if defined(__riscv)
#include "badgevms/pathfuncs.h"   /* mkdir_p() */
#else
static bool mkdir_p(const char *path) { (void)path; return true; }   /* host test build */
#endif

/*
 * Diagnostic logging switches (compile-time, default on).
 *
 * The host regression scripts read the serial log, including the dumped page
 * content, POST bodies and cookie values.  Builds that are handed to other
 * people can disable those by compiling with -DMB_LOG_SENSITIVE=0 and/or
 * -DMB_LOG_CONTENT=0: values are then replaced by their length.
 */
#ifndef MB_LOG_SENSITIVE
#define MB_LOG_SENSITIVE 1
#endif
#ifndef MB_LOG_CONTENT
#define MB_LOG_CONTENT 1
#endif

/*
 * Bounded allocator for stb_image.
 *
 * The decoder's real peak memory is far above the w*h*3 output size (PNG
 * keeps the inflated scanlines and the output alive together, progressive
 * JPEG keeps coefficient planes, GIF keeps three frame-sized buffers) and the
 * zlib decoder grows its output buffer until the stream ends, so a tiny PNG
 * with a highly compressible IDAT can try to allocate many megabytes.
 *
 * Every stb allocation goes through these wrappers.  They account for the
 * live bytes and refuse to exceed MB_STBI_HEAP_CAP, so an oversized image
 * fails cleanly inside stb ("outofmem") instead of starving WiFi/TLS/SDL.
 * Decoded images keep their (shrunk) stb allocation, so they are released
 * with stbi_image_free() and count towards the cap while they are retained.
 */
#define MB_STBI_HEAP_CAP (4u * 1024u * 1024u)

typedef union { size_t n; long double align_ld; void *align_p; } mb_stbi_hdr_t;
static size_t g_stbi_live = 0;

static void *mb_stbi_malloc(size_t n) {
    if (n > MB_STBI_HEAP_CAP - g_stbi_live) return NULL;
    mb_stbi_hdr_t *h = (mb_stbi_hdr_t *)malloc(sizeof(mb_stbi_hdr_t) + n);
    if (!h) return NULL;
    h->n = n;
    g_stbi_live += n;
    return h + 1;
}

static void mb_stbi_free(void *p) {
    if (!p) return;
    mb_stbi_hdr_t *h = (mb_stbi_hdr_t *)p - 1;
    g_stbi_live -= h->n;
    free(h);
}

static void *mb_stbi_realloc(void *p, size_t n) {
    if (!p) return mb_stbi_malloc(n);
    mb_stbi_hdr_t *h = (mb_stbi_hdr_t *)p - 1;
    size_t old = h->n;
    if (n > old && n - old > MB_STBI_HEAP_CAP - g_stbi_live) return NULL;
    mb_stbi_hdr_t *nh = (mb_stbi_hdr_t *)realloc(h, sizeof(mb_stbi_hdr_t) + n);
    if (!nh) return NULL;
    nh->n = n;
    g_stbi_live = g_stbi_live - old + n;
    return nh + 1;
}

/* Phase 3: same stb_image v2.30 already used by WHY2025 namebadge. */
#define STBI_MALLOC(sz)       mb_stbi_malloc(sz)
#define STBI_REALLOC(p, newsz) mb_stbi_realloc(p, newsz)
#define STBI_FREE(p)          mb_stbi_free(p)
#define STBI_ASSERT(x)
#define STBI_NO_THREAD_LOCALS
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_GIF
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include "qrcodegen_mb.h"   /* 4.4 Share: QR code (Project Nayuki, MIT) */


/*
 * Mini Browser GIF first-frame loader
 * -----------------------------------
 * stb_image 2.30's normal stbi__gif_load() declares `stbi__gif g` as a
 * local variable.  stbi__gif contains codes[8192]; with the palettes and
 * other state this is roughly 35 KiB, which is too large for the BadgeVMS
 * application task stack and causes a stack-protection fault as soon as a
 * GIF is decoded.
 *
 * Mini Browser only needs the first GIF frame.  Keep stb_image's proven GIF
 * parser/LZW implementation, but allocate the large stbi__gif decoder state
 * on the heap instead of the task stack.  The returned pixel buffer keeps
 * normal stb_image ownership semantics and must be released with
 * stbi_image_free().
 */
static stbi_uc *mb_stbi_load_gif_first_frame_from_memory(
    stbi_uc const *buffer, int len, int *x, int *y, int *comp, int req_comp) {
    if (!buffer || len <= 0 || !x || !y || !comp)
        return NULL;

    stbi__context ctx;
    stbi__start_mem(&ctx, buffer, len);

    stbi__gif *g = (stbi__gif *)stbi__malloc(sizeof(stbi__gif));
    if (!g) {
        stbi__err("outofmem", "Out of memory");
        return NULL;
    }
    memset(g, 0, sizeof(*g));

    printf("[mini_browser] image: GIF decoder state=%u bytes allocated on heap\n",
           (unsigned)sizeof(*g));

    stbi_uc *u = stbi__gif_load_next(&ctx, g, comp, req_comp, NULL);
    if (u == (stbi_uc *)&ctx)
        u = NULL;

    /* These are animation/disposal work buffers. We intentionally stop after
     * frame 1, so release them before the format conversion below allocates
     * its output: that keeps the GIF decode peak at ~4+3 bytes/pixel. */
    STBI_FREE(g->history);
    g->history = NULL;
    STBI_FREE(g->background);
    g->background = NULL;

    if (u) {
        *x = g->w;
        *y = g->h;

        /* stbi__gif_load_next() produces RGBA. Match stbi__gif_load() by
         * converting only after the first frame has decoded successfully. */
        if (req_comp && req_comp != 4)
            u = stbi__convert_format(u, 4, req_comp,
                                     (unsigned)g->w, (unsigned)g->h);
    } else if (g->out) {
        STBI_FREE(g->out);
    }

    STBI_FREE(g);

    return u;
}

/* --- Yield macro for BadgeVMS/ESP-IDF, no-op on desktop --- */
#if defined(ESP_PLATFORM)
# include "freertos/FreeRTOS.h"
# include "freertos/task.h"
# define YIELD_NET() vTaskDelay(pdMS_TO_TICKS(2))
#else
# define YIELD_NET() ((void)0)
#endif

/* ---------- Mini Browser version ---------- */
#define MINI_BROWSER_VERSION "4.4-dev1"

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
#define MAX_COOKIES          32
#define COOKIE_NAME_MAX      63
#define COOKIE_VALUE_MAX    255
#define COOKIE_DOMAIN_MAX    95
#define COOKIE_PATH_MAX      95
#define COOKIE_HEADER_MAX  1536
#define COOKIE_SET_MAX      768

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
 *   - JPEG/PNG/GIF via stb_image (GIF renders the first frame only)
 *   - decode larger source images only inside a strict RGB decode budget
 *   - immediately downscale retained images to RGB565
 *   - direct image URLs and numbered image actions use one image viewer
 *
 * Important memory rule:
 * stb_image still has to decode JPEG/PNG/GIF before Mini Browser can resize it.
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
    DISPLAY_COLORS_IMAGE = 2,
    DISPLAY_COLORS_IMAGES_EXPERIMENTAL = 3
} display_mode_t;

/*
 * Fix 15: tiered visual modes.
 * Default mode 3 decodes one inline image. Mode 4 keeps the proven
 * five-image path as an explicit experimental higher-memory option.
 */
static display_mode_t g_display_mode = DISPLAY_COLORS_IMAGE;

static const char *display_mode_name(display_mode_t mode) {
    switch (mode) {
        case DISPLAY_BW:                         return "Black & White";
        case DISPLAY_COLORS:                     return "Colors";
        case DISPLAY_COLORS_IMAGE:               return "Colors + Image";
        case DISPLAY_COLORS_IMAGES_EXPERIMENTAL: return "Colors + 5 Images (Experimental)";
        default:                                 return "Unknown";
    }
}

static int display_inline_image_limit(void) {
    switch (g_display_mode) {
        case DISPLAY_COLORS_IMAGE:               return 1;
        case DISPLAY_COLORS_IMAGES_EXPERIMENTAL: return MAX_INLINE_IMAGES;
        default:                                 return 0;
    }
}

static int display_mode_has_images(void) {
    return display_inline_image_limit() > 0;
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
/* 4.4: text zoom.  FONT_SCALE is the scale of the text being drawn right
 * now: g_page_scale (WHY+= / WHY+-) for page content, 2 for the bar and
 * menus.  PAGE_SCALE_BEGIN/END switch to the page scale for a block. */
static int g_font_scale = 2;
static int g_page_scale = 2;
#define FONT_SCALE  g_font_scale
#define CH_W ((FONT_W_COLS + FONT_COL_GAP) * FONT_SCALE)
#define CH_H ((FONT_H_ROWS) * FONT_SCALE)
#define PAGE_SCALE_BEGIN() int saved_font_scale_ = g_font_scale; g_font_scale = g_page_scale
#define PAGE_SCALE_END()   g_font_scale = saved_font_scale_

/* External Unicode glyph geometry. Needed by wrapping and rendering. */
#define UNICODE_GLYPH_BYTES 32
#define UNICODE_GLYPH_W 16
#define UNICODE_GLYPH_H 16

/* --------- curl memory sink --------- */
/*
 * Download buffer.  Grows geometrically up to `limit`.  When the body is
 * larger, the first `limit` bytes are kept and `truncated` is set; all
 * later data is ignored (never appended after a gap).  The callback returns
 * 0 then, which makes libcurl stop the transfer; BadgeVMS's curl ignores
 * the return value, so there the rest is downloaded and discarded.
 */
typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    size_t limit;      /* 0 = decide on the first bytes (page fetches) */
    bool truncated;
} mem_t;

static void mem_reset(mem_t *m) {
    free(m->buf);
    m->buf = NULL;
    m->len = 0;
    m->cap = 0;
    m->truncated = false;
}

static bool bytes_look_like_image(const unsigned char *b, size_t n) {
    static const unsigned char png_sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    return (n >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) ||
           (n >= 8 && !memcmp(b, png_sig, 8)) ||
           (n >= 6 && (!memcmp(b, "GIF87a", 6) || !memcmp(b, "GIF89a", 6)));
}

/*
 * Phase 5: metadata retained from the most recent network fetch.
 * All strings are bounded/static so Page Information does not add large
 * automatic buffers to the ESP32 task stack.
 */
typedef enum {
    FETCH_FROM_NETWORK = 0,
    FETCH_FROM_CACHE,          /* fresh copy on flash, no request */
    FETCH_FROM_CACHE_304       /* server said "Not Modified" */
} fetch_source_t;

typedef struct {
    char effective_url[URL_MAX];
    char content_type[96];
    long redirect_count;
    size_t downloaded_bytes;

    /* Phase 5B: request-side facts we can know without CURLINFO support. */
    char request_method[5];       /* "GET" or "POST" */
    size_t request_body_bytes;
    int cookies_sent;

    fetch_source_t source;        /* 4.3: network or disk cache */
    unsigned load_ms;             /* 4.4: request to page shown */
    double wire_bytes;            /* 4.4: bytes received (compressed), 0: unknown */
} fetch_meta_t;

static fetch_meta_t g_fetch_meta;

/* ---------- 4.3 part 2: clock ----------
 *
 * BadgeVMS has no network time: time() counts from boot.  Cookie expiry and
 * cache freshness need the real time, so it is taken from the Date header
 * every web server sends.  0 = not known yet.
 */
static long long g_clock_offset;
static bool g_clock_valid;

static long long clock_now(void) {
    time_t t = time(NULL);
    if (t > (time_t)1577836800) return (long long)t;           /* a real clock (2020+) */
    if (!g_clock_valid) return 0;
    return g_clock_offset + (long long)(SDL_GetTicks() / 1000);
}

/* "Sun, 06 Nov 1994 08:49:37 GMT" (also "06-Nov-94" cookie style) ->
 * seconds since 1970 (UTC), or 0 when it cannot be read. */
static long long http_date_parse(const char *value) {
    static const char months[] = "janfebmaraprmayjunjulaugsepoctnovdec";
    int day = 0, year = 0, hh = 0, mm = 0, ss = 0;
    char mon[4] = "";
    if (!value) return 0;
    const char *p = strchr(value, ',');
    p = p ? p + 1 : value;
    if (sscanf(p, " %d%*[ -]%3s%*[ -]%d %d:%d:%d", &day, mon, &year, &hh, &mm, &ss) < 3)
        return 0;
    if (year < 100) year += year < 70 ? 2000 : 1900;
    for (int i = 0; mon[i]; i++) mon[i] = (char)tolower((unsigned char)mon[i]);
    const char *m = strlen(mon) == 3 ? strstr(months, mon) : NULL;
    if (!m || (m - months) % 3) return 0;
    int month = (int)(m - months) / 3;
    if (hh < 0 || hh > 23 || mm < 0 || mm > 59 || ss < 0 || ss > 60 || day < 1 || day > 31)
        return 0;
    /* Days since 1970-01-01 (days-from-civil), UTC. */
    int y = year - (month < 2);
    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400;
    long doy = (153 * (month + (month < 2 ? 10 : -2)) + 2) / 5 + day - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long long days = era * 146097LL + doe - 719468;
    long long t = days * 86400LL + (long long)hh * 3600 + (long long)mm * 60 + ss;
    return t > 0 ? t : 1;   /* 1970 itself still means "in the past" */
}

static void clock_learn(long long server_now) {
    if (server_now < 1577836800LL) return;
    if (!g_clock_valid)
        printf("[mini_browser] clock: set from the server Date header\n");
    g_clock_offset = server_now - (long long)(SDL_GetTicks() / 1000);
    g_clock_valid = true;
}

/* ---------- 4.3 part 2: response headers ----------
 * Filled by the header callbacks of every request (page, image, download). */
typedef struct {
    char etag[96];
    char last_modified[48];
    char disposition[192];         /* Content-Disposition */
    long long date;                /* Date */
    long long expires;             /* Expires */
    long max_age;                  /* Cache-Control: max-age (-1: none) */
    bool no_store;
    bool no_cache;
    long long content_length;      /* -1: unknown */
    bool download;                 /* 4.3: a file to download, not a page */
} resp_headers_t;

static resp_headers_t g_resp;
static CURL *g_net_active;          /* handle of the transfer in progress */
static bool g_fetch_is_page;        /* page fetch: may turn into a download */

static void resp_reset(void) {
    memset(&g_resp, 0, sizeof g_resp);
    g_resp.max_age = -1;
    g_resp.content_length = -1;
}

static void resp_copy(char *dst, size_t cap, const char *v, size_t n) {
    if (n >= cap) n = cap - 1;
    memcpy(dst, v, n);
    dst[n] = 0;
}

/* Header value after "Name:" with surrounding whitespace/CRLF removed. */
static bool header_value(const char *buffer, size_t n, const char *name,
                         const char **value, size_t *value_len);

/* The headers every request records (cache, download, clock). */
static void resp_parse_header(const char *buffer, size_t n) {
    const char *v;
    size_t len;
    static char tmp[128];
    if (header_value(buffer, n, "ETag", &v, &len)) {
        if (len < sizeof g_resp.etag) resp_copy(g_resp.etag, sizeof g_resp.etag, v, len);
    } else if (header_value(buffer, n, "Last-Modified", &v, &len)) {
        if (len < sizeof g_resp.last_modified)
            resp_copy(g_resp.last_modified, sizeof g_resp.last_modified, v, len);
    } else if (header_value(buffer, n, "Content-Disposition", &v, &len)) {
        resp_copy(g_resp.disposition, sizeof g_resp.disposition, v, len);
    } else if (header_value(buffer, n, "Content-Length", &v, &len)) {
        resp_copy(tmp, sizeof tmp, v, len);
        g_resp.content_length = atoll(tmp);
    } else if (header_value(buffer, n, "Date", &v, &len)) {
        resp_copy(tmp, sizeof tmp, v, len);
        g_resp.date = http_date_parse(tmp);
        clock_learn(g_resp.date);
    } else if (header_value(buffer, n, "Expires", &v, &len)) {
        resp_copy(tmp, sizeof tmp, v, len);
        g_resp.expires = http_date_parse(tmp);
    } else if (header_value(buffer, n, "Cache-Control", &v, &len)) {
        resp_copy(tmp, sizeof tmp, v, len);
        for (char *c = tmp; *c; c++) *c = (char)tolower((unsigned char)*c);
        if (strstr(tmp, "no-store")) g_resp.no_store = true;
        if (strstr(tmp, "no-cache")) g_resp.no_cache = true;
        const char *ma = strstr(tmp, "max-age=");
        if (ma) g_resp.max_age = atol(ma + 8);
    } else if (header_value(buffer, n, "Pragma", &v, &len)) {
        if (len >= 8 && !strncasecmp(v, "no-cache", 8)) g_resp.no_cache = true;
    }
}

/* Can the browser show this response, or is it a file to download? */
static bool content_type_is_viewable(const char *ct) {
    if (!ct || !*ct) return true;               /* unknown: try to show it */
    if (!strncasecmp(ct, "text/", 5)) return true;
    if (!strncasecmp(ct, "image/png", 9) || !strncasecmp(ct, "image/jpeg", 10) ||
        !strncasecmp(ct, "image/jpg", 9) || !strncasecmp(ct, "image/gif", 9))
        return true;
    return strstr(ct, "html") || strstr(ct, "xml") || strstr(ct, "json") ||
           strstr(ct, "javascript");
}

static void response_check_download(void) {
    long code = 0;
    if (!g_fetch_is_page || !g_net_active) return;
    curl_easy_getinfo(g_net_active, CURLINFO_RESPONSE_CODE, &code);
    if (code < 200 || code >= 300) return;
    bool attachment = !strncasecmp(g_resp.disposition, "attachment", 10);
    if (attachment || !content_type_is_viewable(g_fetch_meta.content_type))
        g_resp.download = true;
}

static size_t wr_cb(void *ptr, size_t sz, size_t nm, void *ud) {
    size_t n = sz * nm;
    mem_t *m = (mem_t*)ud;
    if (!m || !n) return n;
    if (m->truncated) return 0;

    /* 4.3: a file to download is not read into memory: stop here and ask. */
    if (m->len == 0 && !g_resp.download) response_check_download();
    if (g_resp.download) return 0;

    if (!m->limit) {
        /* A page fetch that turns out to be a JPEG/PNG/GIF may use the image
         * cap, so a direct image URL is decoded from this download instead of
         * being fetched a second time. */
        m->limit = bytes_look_like_image((const unsigned char *)ptr, n)
                       ? IMAGE_DOWNLOAD_MAX : MAX_BYTES;
    }

    size_t keep = n;
    if (m->len + keep > m->limit) {
        keep = m->limit - m->len;
        m->truncated = true;
    }

    if (m->len + keep + 1 > m->cap) {
        size_t cap = m->cap ? m->cap : 4096;
        while (cap < m->len + keep + 1) cap *= 2;
        if (cap > m->limit + 1) cap = m->limit + 1;
        char *p = (char*)realloc(m->buf, cap);
        if (!p) {
            m->truncated = true;
            return 0;
        }
        m->buf = p;
        m->cap = cap;
    }
    memcpy(m->buf + m->len, ptr, keep);
    m->len += keep;
    m->buf[m->len] = 0;
    return m->truncated ? 0 : n;
}

/* ---------- link + page model ---------- */
/*
 * Link targets and image sources are stored once in a per-page string pool
 * (page_t.strings) and referenced by offset.  A fixed char[URL_MAX] per slot
 * cost 32 KiB for links and 8 KiB for images on every page, even when a page
 * has three links; the pool only uses what the page needs.
 */
typedef struct {
    uint32_t href;      /* offset into page_t.strings */
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
    uint32_t src;       /* offset into page_t.strings */
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
    char *strings;                /* NUL-separated link/image URL pool */
    size_t strings_len;
    size_t strings_cap;
    char *html;                   /* 4.4: the HTML, for reader mode */
    size_t html_len;
    bool reader_offer;            /* 4.4: show the "Simplified view" chip */
} page_t;

#define PAGE_STRINGS_MAX (64 * 1024)

/* Store s in the page's string pool. Returns its offset, or -1 when out of
 * memory / over the pool limit. Offset 0 is always the empty string. */
static long page_store_string(page_t *page, const char *s) {
    if (!page || !s) return -1;
    size_t n = strlen(s) + 1;
    if (!page->strings_len) {
        /* Reserve offset 0 as "" so a zeroed link/image never points at garbage. */
        page->strings = (char *)malloc(1024);
        if (!page->strings) return -1;
        page->strings_cap = 1024;
        page->strings[0] = 0;
        page->strings_len = 1;
    }
    if (page->strings_len + n > page->strings_cap) {
        size_t cap = page->strings_cap;
        while (cap < page->strings_len + n) cap *= 2;
        if (cap > PAGE_STRINGS_MAX) return -1;
        char *grown = (char *)realloc(page->strings, cap);
        if (!grown) return -1;
        page->strings = grown;
        page->strings_cap = cap;
    }
    long off = (long)page->strings_len;
    memcpy(page->strings + off, s, n);
    page->strings_len += n;
    return off;
}

static const char *page_string(const page_t *page, uint32_t off) {
    if (!page || !page->strings || off >= page->strings_len) return "";
    return page->strings + off;
}

static const char *page_link_href(const page_t *page, int index) {
    if (!page || index < 0 || index >= page->link_count) return "";
    return page_string(page, page->links[index].href);
}

static const char *page_image_src(const page_t *page, int index) {
    if (!page || index < 0 || index >= page->image_count) return "";
    return page_string(page, page->images[index].src);
}

/* ---------- URL helpers ---------- */
/*
 * Length of a leading URL scheme ("https" in "https://x"), or 0 when the
 * string does not start with one.  RFC 3986: ALPHA *( ALPHA / DIGIT / + - . ) ":"
 * Only a prefix counts: "example.com/r?u=https://x" has no scheme.
 */
static size_t url_scheme_len(const char *u) {
    if (!u || !isalpha((unsigned char)u[0])) return 0;
    const char *p = u + 1;
    while (isalnum((unsigned char)*p) || *p == '+' || *p == '-' || *p == '.') p++;
    return *p == ':' ? (size_t)(p - u) : 0;
}

static bool url_scheme_is(const char *u, const char *scheme) {
    size_t n = url_scheme_len(u);
    return n && n == strlen(scheme) && !strncasecmp(u, scheme, n);
}

static void get_scheme_host(const char *url, char *out, size_t cap) {
    if (!out || !cap) return;
    out[0] = 0;
    if (!url) return;
    const char *p = strstr(url, "://");
    if (!p) return;
    p += 3;
    /* The authority ends at the first '/', '?' or '#'. */
    size_t n = (size_t)(p - url) + strcspn(p, "/?#");
    if (n >= cap) n = cap - 1;
    memcpy(out, url, n); out[n]=0;
}
/*
 * Candidate 3 Fix 13: document-relative URL base directory.
 *
 * The old get_dir() used the final '/' anywhere in the URL. For a root
 * document URL such as "https://example.org" the final slash is one of the
 * two slashes in "://", so "img/pic.png" incorrectly became
 * "https://img/pic.png".
 *
 * Work only on the URL before ?/# and distinguish the authority separator
 * from an actual path. A URL with no path resolves relative references
 * against the origin root.
 */
static void get_dir(const char *url, char *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = 0;
    if (!url || !*url) return;

    char clean[URL_MAX];
    size_t n = strlen(url);
    size_t cut = n;

    for (size_t i = 0; i < n; i++) {
        if (url[i] == '?' || url[i] == '#') {
            cut = i;
            break;
        }
    }

    if (cut >= sizeof(clean))
        cut = sizeof(clean) - 1;
    memcpy(clean, url, cut);
    clean[cut] = 0;

    const char *scheme = strstr(clean, "://");
    if (scheme) {
        const char *authority = scheme + 3;
        const char *path = strchr(authority, '/');

        if (!path) {
            /* Too long for a trailing '/': return nothing rather than a
             * directory that is not the URL's (resolve_url then fails). */
            if (snprintf(out, cap, "%s/", clean) >= (int)cap) out[0] = 0;
            return;
        }

        const char *last = strrchr(path, '/');
        size_t dir_len = (size_t)(last - clean) + 1;
        if (dir_len >= cap)
            dir_len = cap - 1;
        memcpy(out, clean, dir_len);
        out[dir_len] = 0;
        return;
    }

    const char *last = strrchr(clean, '/');
    if (!last) {
        out[0] = 0;
        return;
    }

    size_t dir_len = (size_t)(last - clean) + 1;
    if (dir_len >= cap)
        dir_len = cap - 1;
    memcpy(out, clean, dir_len);
    out[dir_len] = 0;
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
/* Returns 1 when out holds the complete URL; 0 when it does not fit (out is
 * then emptied, so a truncated - i.e. different - URL is never used). */
static int resolve_url(const char *base, const char *href, char *out, size_t cap) {
    int n;
    if (!out || !cap) return 0;
    out[0] = 0;
    if (!href || !*href) return 0;
    if (!base) base = "";
    if (url_scheme_len(href)) {
        n = snprintf(out, cap, "%s", href);
    } else if (href[0]=='/' && href[1]=='/') {
        char sch[16]; scheme_from_url(base, sch, sizeof sch);
        n = snprintf(out, cap, "%s:%s", sch, href);
    } else if (href[0]=='/') {
        char origin[URL_MAX]; get_scheme_host(base, origin, sizeof origin);
        n = snprintf(out, cap, "%s%s", origin, href);
    } else if (href[0]=='?') {
        char base2[URL_MAX]; base_no_query_or_hash(base, base2, sizeof base2);
        n = snprintf(out, cap, "%s%s", base2, href);
    } else if (href[0]=='#') {
        char base2[URL_MAX]; base_no_hash(base, base2, sizeof base2);
        n = snprintf(out, cap, "%s%s", base2, href);
    } else {
        if (href[0]=='.' && href[1]=='/') href += 2;
        char dir[URL_MAX]; get_dir(base, dir, sizeof dir);
        n = snprintf(out, cap, "%s%s", dir, href);
    }
    if (n < 0 || (size_t)n >= cap) {
        out[0] = 0;
        return 0;
    }
    return 1;
}

/* --- URL sanitation --- */
static void trim_inplace(char *s) {
    if (!s) return;
    size_t n = strlen(s);
    size_t i = 0; while (i < n && isspace((unsigned char)s[i])) i++;
    size_t j = n; while (j > i && isspace((unsigned char)s[j-1])) j--;
    if (i > 0 || j < n) { memmove(s, s + i, j - i); s[j - i] = 0; }
}
static int has_scheme(const char *u) { return url_scheme_len(u) > 0 && strstr(u, "://") == u + url_scheme_len(u); }
static int is_http_scheme(const char *u) {
    return url_scheme_is(u, "http") || url_scheme_is(u, "https");
}
static void normalize_typed_url(char *buf) {
    trim_inplace(buf);
    if (!buf[0]) return;
    if (!has_scheme(buf)) {
        char tmp[URL_MAX]; snprintf(tmp, sizeof tmp, "%s", buf);
        int n = snprintf(buf, URL_MAX, "https://%s", tmp);
        if (n < 0 || n >= URL_MAX) buf[0] = 0;   /* too long: do not fetch a truncated URL */
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

    /*
     * Prefer the normal semicolon-terminated form. Keep this bounded so a
     * semicolon much later in ordinary page text cannot accidentally become
     * part of an entity name.
     */
    const char *semi = NULL;
    for (size_t n = 0; n <= 12 && p[n]; n++) {
        if (p[n] == ';') {
            semi = p + n;
            break;
        }
        if (!(isalnum((unsigned char)p[n])))
            break;
    }

    if (semi) {
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

    /*
     * Legacy HTML compatibility: old pages commonly omit the semicolon,
     * for example "&copy 1995". Accept our known named entities only when
     * the name is followed by a safe boundary. Do not turn prefixes such as
     * "&copyright" into "&copy" + "right".
     */
    for (size_t i = 0; i < sizeof(g_html_entities) / sizeof(g_html_entities[0]); i++) {
        const char *name = g_html_entities[i].name;
        size_t name_len = strlen(name);

        if (strncmp(p, name, name_len))
            continue;

        unsigned char next = (unsigned char)p[name_len];
        if (next != 0 &&
            !isspace(next) &&
            next != '<' && next != '>' &&
            next != '"' && next != '\'' &&
            next != '&')
            continue;

        if (!emit_utf8_codepoint(g_html_entities[i].codepoint, out, o, cap))
            return NULL;
        return p + name_len;
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
static bool decode_html_entities(const char *src, char *dst, size_t dst_cap) {
    if (!dst || !dst_cap) return false;
    dst[0] = 0;
    if (!src) return true;

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
    return *p == 0;   /* false: dst was too small and the value was cut */
}

/* --------- href filter --------- */
/* Allowlist: relative references and http(s) only.  Everything with another
 * scheme (javascript:, mailto:, data:, tel:, ftp:, file: ...) is not a link
 * this browser can follow, so it must not be resolved as a relative path. */
static int is_supported_href(const char *h) {
    if (!h || !*h || h[0] == '#') return 0;
    if (url_scheme_len(h)) return is_http_scheme(h);
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
            int result = 1;
            if (out && out_cap) {
                if (value) {
                    char raw[URL_MAX];
                    size_t copy_len = value_len;
                    if (copy_len >= sizeof(raw)) {
                        copy_len = sizeof(raw) - 1;
                        result = 2;
                    }
                    memcpy(raw, value, copy_len);
                    raw[copy_len] = 0;
                    if (!decode_html_entities(raw, out, out_cap))
                        result = 2;
                }
            }
            /* 1 = found, 2 = found but the value did not fit in out. */
            return result;
        }
    }
    return 0;
}

static void lower_ascii(char *s) {
    for (; s && *s; s++) *s = (char)tolower((unsigned char)*s);
}

/* UTF-8 decoder, defined with the text renderer. */
static unsigned utf8_next(const char *s, size_t len, size_t *i);

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

#define TEXT_BOLD_ON     0x01
#define TEXT_BOLD_OFF    0x02
#define TEXT_LINK_ON     0x03
#define TEXT_LINK_OFF    0x04
#define TEXT_HEADING_ON  0x05
#define TEXT_HEADING_OFF 0x06
#define TEXT_HRULE       0x07
#define TEXT_FORM_ON     0x08
#define TEXT_FORM_OFF    0x09

/*
 * Parser-only records.  The action marker used to be "\001NNN\002", which is
 * byte-for-byte the same as a bold three-digit number (<b>404</b>), so bold
 * numbers vanished.  Actions now use their own bytes.  TEXT_IMAGE_MARK
 * prefixes the [[MBIMGn]] record written by the parser, so the same text
 * typed in a page is just text.  TEXT_PRE_ON/OFF bracket <pre> content so
 * wrap_text() keeps its spaces and blank lines.
 *
 * Page content can never produce any of these: sanitize_cp() drops C0
 * control bytes (other than whitespace) and replaces the private-use
 * marker range with U+FFFD before anything reaches the template.
 */
#define TEXT_ACTION_START 0x0E
#define TEXT_ACTION_END   0x0F
#define TEXT_IMAGE_MARK   0x10
#define TEXT_PRE_ON       0x11
#define TEXT_PRE_OFF      0x12

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

/* Range reserved for internal markers; page text is never allowed into it. */
#define TEXT_MARKER_PUA_FIRST 0xE400u
#define TEXT_MARKER_PUA_LAST  0xE4FFu

/* One definition of "zero-width internal marker", shared by wrapping,
 * rendering, image-line detection and the serial content dump. */
static bool is_format_marker(unsigned cp) {
    /* 0x01..0x12 except LF/CR: page text never contains these (0x09 is
     * TEXT_FORM_OFF; real tabs are expanded or collapsed by the parser). */
    if (cp >= TEXT_BOLD_ON && cp <= TEXT_PRE_OFF)
        return cp != '\n' && cp != '\r';
    return cp >= TEXT_COLOR_START && cp <= TEXT_BG_TRANSPARENT;
}

/* width="120" / height="80px" -> pixels; percentages and junk -> 0
 * (unspecified).  Clamped so later size arithmetic cannot overflow. */
static int parse_html_dimension(const char *s) {
    if (!s || !*s) return 0;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s || v <= 0) return 0;
    while (isspace((unsigned char)*end)) end++;
    if (*end == '%') return 0;
    if (v > 4096) v = 4096;
    return (int)v;
}

/*
 * Page content -> template filter.  Returns 0 when the codepoint must be
 * dropped, otherwise the codepoint to emit.
 */
static unsigned sanitize_cp(unsigned cp) {
    if (cp == '\n' || cp == '\t' || cp == '\r' || cp == '\f') return cp;
    if (cp < 0x20 || cp == 0x7F) return 0;
    if (cp >= TEXT_MARKER_PUA_FIRST && cp <= TEXT_MARKER_PUA_LAST) return 0xFFFD;
    return cp;
}

/* In-place filter for short attribute strings (titles, alt text, form
 * names/values/labels).  Control bytes become spaces; the marker range
 * becomes U+FFFD (same length, so this never grows the string). */
static void sanitize_text_inplace(char *s) {
    if (!s) return;
    unsigned char *p = (unsigned char *)s;
    for (; *p; p++) {
        if (*p < 0x20 || *p == 0x7F) {
            *p = ' ';
        } else if (p[0] == 0xEE && p[1] >= 0x90 && p[1] <= 0x93 && p[2] >= 0x80 && p[2] <= 0xBF) {
            p[0] = 0xEF; p[1] = 0xBF; p[2] = 0xBD;   /* U+E400..U+E4FF -> U+FFFD */
            p += 2;
        }
    }
}

/* ---------- growable template builder ---------- */
/*
 * The template used to be one fixed buffer of strlen(html) + 1280 bytes.
 * Container tags write far more marker bytes than their source (<b> alone
 * writes 16), so markup-dense pages were cut off silently, and a failed
 * multi-codepoint push could leave a half-written marker behind.
 * sb_* grows by doubling up to a hard limit, and every marker push is
 * all-or-nothing.  Once anything fails, the builder stays failed.
 */
typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    size_t limit;
    bool failed;
} sbuf_t;

static bool sb_reserve(sbuf_t *sb, size_t extra) {
    if (sb->failed) return false;
    if (sb->len + extra + 1 <= sb->cap) return true;
    size_t cap = sb->cap ? sb->cap : 1024;
    while (cap < sb->len + extra + 1) cap *= 2;
    if (cap > sb->limit) cap = sb->limit;
    if (sb->len + extra + 1 > cap) { sb->failed = true; return false; }
    char *grown = (char *)realloc(sb->buf, cap);
    if (!grown) { sb->failed = true; return false; }
    sb->buf = grown;
    sb->cap = cap;
    return true;
}

static bool sb_bytes(sbuf_t *sb, const char *p, size_t n) {
    if (!sb_reserve(sb, n)) return false;
    memcpy(sb->buf + sb->len, p, n);
    sb->len += n;
    sb->buf[sb->len] = 0;
    return true;
}

static bool sb_text(sbuf_t *sb, const char *s) {
    return sb_bytes(sb, s, strlen(s));
}

static bool sb_byte(sbuf_t *sb, char c) {
    return sb_bytes(sb, &c, 1);
}

static size_t utf8_encode(unsigned cp, char out[4]) {
    if (cp <= 0x7F) { out[0] = (char)cp; return 1; }
    if (cp <= 0x7FF) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp <= 0xFFFF) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static bool sb_cp(sbuf_t *sb, unsigned cp) {
    char b[4];
    return sb_bytes(sb, b, utf8_encode(cp, b));
}

/*
 * Candidate 3 Fix 10: marker-aware vertical whitespace normalization.
 *
 * Color/style/background state markers are zero-width. Closing a block can
 * leave: visible text + newline + zero-width POP markers. The old code saw
 * the final marker byte instead of the existing newline and added another
 * newline when the next block opened, creating a visually empty line.
 *
 * Normalize at line-break insertion time, not by collapsing the finished
 * text, so literal newlines inside <pre> remain untouched.
 */
static int tail_after_last_newline_is_zero_width(const char *out, size_t used) {
    size_t start = used;
    while (start > 0 && out[start - 1] != '\n')
        start--;

    for (size_t i = start; i < used; ) {
        unsigned char c = (unsigned char)out[i];

        if (c >= 0x01 && c <= TEXT_PRE_OFF && c != '\n') {
            i++;
            continue;
        }

        if (c == ' ' || c == '\r') {
            i++;
            continue;
        }

        if ((c & 0xF0) == 0xE0 && i + 2 < used) {
            unsigned char c1 = (unsigned char)out[i + 1];
            unsigned char c2 = (unsigned char)out[i + 2];
            if ((c1 & 0xC0) == 0x80 && (c2 & 0xC0) == 0x80) {
                unsigned cp = ((unsigned)(c & 0x0F) << 12) |
                              ((unsigned)(c1 & 0x3F) << 6) |
                              (unsigned)(c2 & 0x3F);
                if (cp >= TEXT_COLOR_START && cp <= TEXT_BG_TRANSPARENT) {
                    i += 3;
                    continue;
                }
            }
        }

        return 0;
    }

    return 1;
}

static bool sb_at_line_start(const sbuf_t *sb) {
    return !sb->len || sb->buf[sb->len - 1] == '\n' ||
           tail_after_last_newline_is_zero_width(sb->buf, sb->len);
}

static void sb_line_break(sbuf_t *sb) {
    if (sb_at_line_start(sb))
        return;
    sb_byte(sb, '\n');
}

/* Visible columns since the last newline, for <pre> tab stops. */
static int sb_column(const sbuf_t *sb) {
    int col = 0;
    size_t i = sb->len;
    while (i > 0 && sb->buf[i - 1] != '\n') i--;
    for (; i < sb->len; i++) {
        unsigned char c = (unsigned char)sb->buf[i];
        /* Formatting markers (U+E400..U+E4FF, EE 90..93 xx) are zero-width. */
        if (c == 0xEE && i + 2 < sb->len &&
            (unsigned char)sb->buf[i + 1] >= 0x90 && (unsigned char)sb->buf[i + 1] <= 0x93) {
            i += 2;
            continue;
        }
        if (c >= 0x20 && (c & 0xC0) != 0x80) col++;   /* count lead bytes only */
    }
    return col;
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
            if (n >= sizeof(tmp)) n = sizeof(tmp) - 1;
            memcpy(tmp, val, n);
            tmp[n] = 0;
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

/* HTML align="..." value: 1 left, 2 center, 3 right, 0 anything else. */
static unsigned char html_align_value(const char *value) {
    size_t n = value ? strlen(value) : 0;
    if (css_value_eq(value, n, "left")) return 1;
    if (css_value_eq(value, n, "center")) return 2;
    if (css_value_eq(value, n, "right")) return 3;
    return 0;
}

/*
 * Horizontal placement of an <img> from its own style="": the usual
 * "display:block; margin: 0 auto" centering idiom (either the margin
 * shorthand or margin-left/margin-right), and margin-left:auto alone to push
 * it to the right.  Returns 0 (not set), 2 center or 3 right.
 */
static unsigned char css_image_margin_align(const char *style) {
    bool left_auto = false, right_auto = false;
    const char *p = style;
    if (!p) return 0;
    while (*p) {
        while (*p == ';' || isspace((unsigned char)*p)) p++;
        const char *name = p;
        while (*p && *p != ':' && *p != ';') p++;
        if (*p != ':') { while (*p && *p != ';') p++; continue; }
        const char *name_end = p++;
        while (name_end > name && isspace((unsigned char)name_end[-1])) name_end--;
        const char *val = p;
        while (*p && *p != ';') p++;
        size_t nn = (size_t)(name_end - name), vn = (size_t)(p - val);

        if (nn == 6 && !strncasecmp(name, "margin", 6)) {
            /* margin: all | vertical horizontal | top horizontal bottom |
             *         top right bottom left */
            const char *tok[4];
            size_t tok_len[4];
            int count = 0;
            const char *q = val, *end = val + vn;
            while (q < end) {
                while (q < end && isspace((unsigned char)*q)) q++;
                if (q >= end) break;
                const char *t = q;
                while (q < end && !isspace((unsigned char)*q)) q++;
                if (count < 4) { tok[count] = t; tok_len[count] = (size_t)(q - t); }
                count++;
            }
            if (count < 1 || count > 4) continue;
            int r = count == 1 ? 0 : 1;
            int l = count == 4 ? 3 : r;
            right_auto = css_value_eq(tok[r], tok_len[r], "auto");
            left_auto = css_value_eq(tok[l], tok_len[l], "auto");
        } else if (nn == 11 && !strncasecmp(name, "margin-left", 11)) {
            left_auto = css_value_eq(val, vn, "auto");
        } else if (nn == 12 && !strncasecmp(name, "margin-right", 12)) {
            right_auto = css_value_eq(val, vn, "auto");
        }
    }
    if (left_auto && right_auto) return 2;
    if (left_auto) return 3;
    return 0;
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

static bool sb_action_marker(sbuf_t *sb, int action_index) {
    char marker[8];
    int n = snprintf(marker, sizeof(marker), "%c%03d%c",
                     TEXT_ACTION_START, action_index, TEXT_ACTION_END);
    return n > 0 && (size_t)n < sizeof(marker) && sb_bytes(sb, marker, (size_t)n);
}

static int refresh_page_text(page_t *page) {
    if (!page || !page->text_template) return 0;
    size_t cap = strlen(page->text_template) +
                 (size_t)page->action_count * (FORM_VALUE_MAX + 96) + 1;
    char *rendered = (char*)malloc(cap);
    if (!rendered) return 0;
    rendered[0] = 0;

    size_t used = 0;
    const char *p = page->text_template;
    while (*p) {
        if ((unsigned char)p[0] == TEXT_ACTION_START && isdigit((unsigned char)p[1]) &&
            isdigit((unsigned char)p[2]) && isdigit((unsigned char)p[3]) &&
            (unsigned char)p[4] == TEXT_ACTION_END) {
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
    sanitize_text_inplace(out);
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
        snprintf(raw, sizeof(raw), "%s", out);
        decode_html_entities(raw, out, cap);
    }
    sanitize_text_inplace(out);
}

/* Case-insensitive search for needle inside [start, end). */
static const char *find_ci_n(const char *start, const char *end, const char *needle) {
    size_t n = strlen(needle);
    if (!start || !end || end < start || !n) return NULL;
    for (const char *p = start; p + n <= end; p++)
        if (!strncasecmp(p, needle, n)) return p;
    return NULL;
}

/* ---------- HTML tag table ---------- */
enum {
    TF_BREAK_BEFORE = 1u << 0,   /* line break before the opening tag's markers */
    TF_BREAK_AFTER  = 1u << 1,   /* line break after the closing tag's markers  */
    TF_STYLE        = 1u << 2,   /* pushes style + background state            */
    TF_COLOR        = 1u << 3,   /* pushes foreground color state              */
    TF_ALIGN        = 1u << 4,   /* text-align accepted in style=""            */
    TF_HEADING      = 1u << 5,
    TF_SCOPE        = 1u << 6,   /* tracked on the element stack (no pushes)   */
    TF_PCLOSER      = 1u << 7,   /* opening it implicitly closes an open <p>   */
};

#define TF_BLOCKBOX (TF_BREAK_BEFORE | TF_BREAK_AFTER | TF_STYLE | TF_COLOR | TF_ALIGN | TF_PCLOSER)

typedef struct { const char *name; unsigned flags; } html_tag_info_t;

static const html_tag_info_t g_html_tags[] = {
    {"p",          TF_BLOCKBOX}, {"div",     TF_BLOCKBOX}, {"section", TF_BLOCKBOX},
    {"article",    TF_BLOCKBOX}, {"main",    TF_BLOCKBOX}, {"header",  TF_BLOCKBOX},
    {"footer",     TF_BLOCKBOX}, {"nav",     TF_BLOCKBOX}, {"aside",   TF_BLOCKBOX},
    {"blockquote", TF_BLOCKBOX}, {"address", TF_BLOCKBOX},
    {"center",     TF_BLOCKBOX},   /* obsolete, but still common: text-align:center */
    {"h1", TF_BLOCKBOX | TF_HEADING}, {"h2", TF_BLOCKBOX | TF_HEADING},
    {"h3", TF_BLOCKBOX | TF_HEADING}, {"h4", TF_BLOCKBOX | TF_HEADING},
    {"h5", TF_BLOCKBOX | TF_HEADING}, {"h6", TF_BLOCKBOX | TF_HEADING},
    {"td", TF_STYLE | TF_COLOR | TF_ALIGN}, {"th", TF_STYLE | TF_COLOR | TF_ALIGN},
    {"span", TF_STYLE | TF_COLOR}, {"code", TF_STYLE | TF_COLOR},
    {"strong", TF_STYLE | TF_COLOR}, {"b", TF_STYLE | TF_COLOR},
    {"em", TF_STYLE | TF_COLOR}, {"i", TF_STYLE | TF_COLOR},
    {"a", TF_STYLE | TF_COLOR}, {"font", TF_COLOR},
    {"li",    TF_BREAK_AFTER | TF_SCOPE | TF_PCLOSER},
    {"tr",    TF_BREAK_BEFORE | TF_BREAK_AFTER | TF_SCOPE},
    {"table", TF_BREAK_AFTER | TF_SCOPE | TF_PCLOSER},
    {"ul",    TF_SCOPE | TF_PCLOSER}, {"ol", TF_SCOPE | TF_PCLOSER},
    {"pre",   TF_PCLOSER}, {"form", TF_PCLOSER}, {"hr", TF_PCLOSER},
};

static unsigned html_tag_flags(const char *tag) {
    for (size_t i = 0; i < sizeof(g_html_tags) / sizeof(g_html_tags[0]); i++)
        if (!strcmp(tag, g_html_tags[i].name)) return g_html_tags[i].flags;
    return 0;
}

/* Tags that may appear in <head>; anything else means the body started. */
static bool html_head_tag(const char *tag) {
    static const char *const head_tags[] = {
        "head", "html", "title", "meta", "link", "style", "script", "base", "noscript"
    };
    for (size_t i = 0; i < sizeof(head_tags) / sizeof(head_tags[0]); i++)
        if (!strcmp(tag, head_tags[i])) return true;
    return false;
}

/* ---------- HTML -> page parser ---------- */

#define PARSE_STACK_MAX  64   /* open elements tracked by the parser */
#define PARSE_PUSH_MAX   30   /* marker pushes; renderer stacks hold 32 */
#define PARSE_LIST_MAX    8
#define TEMPLATE_MAX     (320 * 1024)

typedef struct {
    char tag[12];
    unsigned flags;
    bool pushed;      /* style/background/color markers were emitted */
    bool link_on;     /* TEXT_LINK_ON was emitted for this <a> */
} open_element_t;

typedef struct {
    page_t *page;
    sbuf_t sb;
    const char *html_end;
    bool in_head, in_pre, pre_skip_newline, tags_exhausted;
    int current_form;
    open_element_t stack[PARSE_STACK_MAX];
    int depth;
    int push_depth;
    struct { bool ordered; int item; } lists[PARSE_LIST_MAX];
    int list_depth;
} html_parser_t;

static void parse_emit_close_markers(html_parser_t *ps, const open_element_t *el) {
    sbuf_t *sb = &ps->sb;
    if (!strcmp(el->tag, "code")) sb_byte(sb, '`');
    else if (!strcmp(el->tag, "strong") || !strcmp(el->tag, "b")) sb_byte(sb, TEXT_BOLD_OFF);
    else if (!strcmp(el->tag, "em") || !strcmp(el->tag, "i")) sb_byte(sb, '_');
    else if (!strcmp(el->tag, "a") && el->link_on) sb_byte(sb, TEXT_LINK_OFF);

    if (el->flags & TF_HEADING) sb_byte(sb, TEXT_HEADING_OFF);

    /*
     * Fix 14: close block formatting state BEFORE emitting the block line
     * break.  Trailing markers are zero-width; text + POPs + "\n" keeps a
     * following image marker on its own line and avoids marker-only lines.
     */
    if (el->pushed) {
        char tmp[9];
        size_t n = 0;
        if (el->flags & TF_COLOR) n += utf8_encode(TEXT_COLOR_POP, tmp + n);
        if (el->flags & TF_STYLE) {
            n += utf8_encode(TEXT_BG_POP, tmp + n);
            n += utf8_encode(TEXT_STYLE_POP, tmp + n);
        }
        sb_bytes(sb, tmp, n);
        ps->push_depth--;
    }

    if (el->flags & TF_BREAK_AFTER)
        sb_line_break(sb);
}

/* Pop elements down to (and including) index; emits each one's close. */
static void parse_close_to(html_parser_t *ps, int index) {
    while (ps->depth > index) {
        ps->depth--;
        parse_emit_close_markers(ps, &ps->stack[ps->depth]);
    }
}

static int parse_find_open(const html_parser_t *ps, const char *tag, const char *const *boundaries) {
    for (int i = ps->depth - 1; i >= 0; i--) {
        if (!strcmp(ps->stack[i].tag, tag)) return i;
        for (const char *const *b = boundaries; b && *b; b++)
            if (!strcmp(ps->stack[i].tag, *b)) return -1;
    }
    return -1;
}

/* HTML implied end tags, simplified: a new <p>/block closes an open <p>,
 * <li> closes the previous <li>, <td>/<th> the previous cell, <tr> the
 * previous row.  This keeps the marker pushes balanced on sloppy pages. */
static void parse_implied_close(html_parser_t *ps, const char *tag, unsigned flags) {
    static const char *const p_scope[] = {"td", "th", "li", "table", "button", NULL};
    static const char *const li_scope[] = {"ul", "ol", NULL};
    static const char *const cell_scope[] = {"tr", "table", NULL};
    static const char *const row_scope[] = {"table", NULL};
    int at;

    if ((flags & TF_PCLOSER) && (at = parse_find_open(ps, "p", p_scope)) >= 0)
        parse_close_to(ps, at);
    if (!strcmp(tag, "li") && (at = parse_find_open(ps, "li", li_scope)) >= 0)
        parse_close_to(ps, at);
    if (!strcmp(tag, "td") || !strcmp(tag, "th") || !strcmp(tag, "tr")) {
        int td = parse_find_open(ps, "td", cell_scope);
        int th = parse_find_open(ps, "th", cell_scope);
        at = td > th ? td : th;
        if (at >= 0) parse_close_to(ps, at);
    }
    if (!strcmp(tag, "tr") && (at = parse_find_open(ps, "tr", row_scope)) >= 0)
        parse_close_to(ps, at);
}

static open_element_t *parse_push_element(html_parser_t *ps, const char *tag, unsigned flags) {
    if (ps->depth >= PARSE_STACK_MAX) return NULL;
    open_element_t *el = &ps->stack[ps->depth++];
    memset(el, 0, sizeof(*el));
    snprintf(el->tag, sizeof(el->tag), "%s", tag);
    el->flags = flags;
    return el;
}

/* Style + background + color pushes for one container, all or nothing. */
static bool parse_emit_push_markers(html_parser_t *ps, const char *tag, unsigned flags,
                                    const char *attributes, const char *tag_end) {
    page_t *page = ps->page;
    char style_value[192] = "";
    bool has_style = tag_attribute(attributes, tag_end, "style", style_value, sizeof(style_value)) != 0;
    char tmp[3 * 24];
    size_t n = 0;

    if (flags & TF_STYLE) {
        /* Phase 2A: compact text-style state; alignment only on block-like elements. */
        inline_style_t st = {0};
        if (has_style) st = parse_inline_text_style(style_value, (flags & TF_ALIGN) != 0);
        /* Legacy alignment: <center> and align="left|center|right" on block
         * elements and table cells.  style="text-align:..." wins. */
        if ((flags & TF_ALIGN) && st.align == 0) {
            if (!strcmp(tag, "center")) {
                st.align = 2;
            } else {
                char align_value[16] = "";
                if (tag_attribute(attributes, tag_end, "align", align_value, sizeof(align_value)) == 1)
                    st.align = html_align_value(align_value);
            }
        }
        unsigned char f = 0;
        if (st.bold_set) f |= 0x80 | (st.bold ? 0x40 : 0);
        if (st.italic_set) f |= 0x20 | (st.italic ? 0x10 : 0);
        if (st.underline_set) f |= 0x08 | (st.underline ? 0x04 : 0);
        f |= (st.align & 0x03);
        n += utf8_encode(TEXT_STYLE_START, tmp + n);
        n += utf8_encode(TEXT_STYLE_NIBBLE + ((f >> 4) & 0x0F), tmp + n);
        n += utf8_encode(TEXT_STYLE_NIBBLE + (f & 0x0F), tmp + n);
        page->explicit_style_count += st.valid_count;

        /* Phase 2B: background-color; missing/invalid inherits, transparent overrides. */
        unsigned char br = 0, bg = 0, bb = 0;
        int bg_kind = has_style ? style_background_value(style_value, &br, &bg, &bb) : 0;
        if (bg_kind == 2) {
            n += utf8_encode(TEXT_BG_TRANSPARENT, tmp + n);
        } else if (bg_kind == 1) {
            unsigned rgb = ((unsigned)br << 16) | ((unsigned)bg << 8) | bb;
            n += utf8_encode(TEXT_BG_START, tmp + n);
            for (int i = 5; i >= 0; i--)
                n += utf8_encode(TEXT_BG_NIBBLE + ((rgb >> (4 * i)) & 0x0F), tmp + n);
        } else {
            n += utf8_encode(TEXT_BG_INHERIT, tmp + n);
        }
        if (bg_kind) page->explicit_background_count++;
    }

    if (flags & TF_COLOR) {
        /* Phase 1B: uncolored containers push inheritance so nested pops restore. */
        unsigned char cr = 0, cg = 0, cb = 0;
        int have_color = has_style ? style_color_value(style_value, &cr, &cg, &cb) : 0;
        if (!have_color && !strcmp(tag, "font")) {
            char color_value[32] = "";
            if (tag_attribute(attributes, tag_end, "color", color_value, sizeof(color_value)))
                have_color = parse_html_color(color_value, &cr, &cg, &cb);
        }
        if (have_color) {
            unsigned rgb = ((unsigned)cr << 16) | ((unsigned)cg << 8) | cb;
            n += utf8_encode(TEXT_COLOR_START, tmp + n);
            for (int i = 5; i >= 0; i--)
                n += utf8_encode(TEXT_COLOR_NIBBLE + ((rgb >> (4 * i)) & 0x0F), tmp + n);
            page->explicit_color_count++;
        } else {
            n += utf8_encode(TEXT_COLOR_INHERIT, tmp + n);
        }
    }

    return sb_bytes(&ps->sb, tmp, n);
}

static void parse_text(html_parser_t *ps, const char **cursor_io) {
    const char *cursor = *cursor_io;
    sbuf_t *sb = &ps->sb;

    if (ps->in_head) { *cursor_io = cursor + 1; return; }

    if (*cursor == '&') {
        char tmp[8];
        size_t used = 0;
        const char *next = emit_html_entity(cursor, tmp, &used, sizeof(tmp));
        if (next) {
            /* emit_utf8_codepoint() already applied the numeric-reference
             * rules; re-check the result against the marker alphabet. */
            size_t k = 0;
            unsigned cp = sanitize_cp(utf8_next(tmp, used, &k));
            if (cp == '\t' || cp == '\n' || cp == '\r' || cp == '\f') {
                /* &#9; &#10; ... are whitespace, never raw control bytes. */
                if (ps->in_pre) sb_byte(sb, cp == '\n' ? '\n' : ' ');
                else if (sb->len && sb->buf[sb->len - 1] != ' ' && sb->buf[sb->len - 1] != '\n')
                    sb_byte(sb, ' ');
            } else if (cp) {
                sb_cp(sb, cp);
            }
            *cursor_io = next;
            return;
        }
    }

    unsigned char c = (unsigned char)*cursor;

    /* Raw UTF-8 for U+E400..U+E4FF (our marker alphabet) -> U+FFFD. */
    if (c == 0xEE && cursor + 2 < ps->html_end &&
        (unsigned char)cursor[1] >= 0x90 && (unsigned char)cursor[1] <= 0x93 &&
        ((unsigned char)cursor[2] & 0xC0) == 0x80) {
        sb_cp(sb, 0xFFFD);
        *cursor_io = cursor + 3;
        return;
    }

    if (ps->in_pre) {
        if (c == '\n' && ps->pre_skip_newline) {
            /* HTML ignores one newline directly after <pre>. */
        } else if (c == '\t') {
            int spaces = 8 - (sb_column(sb) % 8);
            while (spaces-- > 0) sb_byte(sb, ' ');
        } else if (c == '\n' || (c >= 0x20 && c != 0x7F) || c >= 0x80) {
            sb_byte(sb, (char)c);
        }
        if (c != '\r') ps->pre_skip_newline = false;
    } else if (isspace(c)) {
        if (sb->len && sb->buf[sb->len - 1] != ' ' && sb->buf[sb->len - 1] != '\n')
            sb_byte(sb, ' ');
    } else if (c >= 0x20 && c != 0x7F) {
        sb_byte(sb, (char)c);
    }
    /* other C0 control bytes are dropped */
    *cursor_io = cursor + 1;
}

static void parse_close_tag(html_parser_t *ps, const char *tag) {
    sbuf_t *sb = &ps->sb;

    if (!strcmp(tag, "head")) { ps->in_head = false; return; }
    if (!strcmp(tag, "form")) { ps->current_form = -1; sb_line_break(sb); return; }
    if (!strcmp(tag, "pre")) {
        if (ps->in_pre) sb_byte(sb, TEXT_PRE_OFF);
        ps->in_pre = false;
        sb_line_break(sb);
        return;
    }
    if (!strcmp(tag, "ul") || !strcmp(tag, "ol")) {
        if (ps->list_depth > 0) ps->list_depth--;
    }

    unsigned flags = html_tag_flags(tag);
    if (!(flags & (TF_STYLE | TF_COLOR | TF_HEADING | TF_SCOPE)))
        return;

    /* Only close an element that is actually open; a stray close tag must
     * not pop state that belongs to an enclosing element. */
    static const char *const no_boundary[] = {NULL};
    int at = parse_find_open(ps, tag, no_boundary);
    if (at >= 0)
        parse_close_to(ps, at);
    else if (flags & TF_BREAK_AFTER)
        sb_line_break(sb);
}

static void parse_open_tag(html_parser_t *ps, const char *tag,
                           const char *attributes, const char *tag_end,
                           const char **cursor_io) {
    page_t *page = ps->page;
    sbuf_t *sb = &ps->sb;
    const char *after_tag = tag_end + 1;

    if (ps->in_head && !html_head_tag(tag)) ps->in_head = false;
    if (!strcmp(tag, "body")) ps->in_head = false;
    if (!strcmp(tag, "head")) { ps->in_head = true; return; }

    /* Raw-text elements: their content is never markup or page text. */
    if (!strcmp(tag, "script") || !strcmp(tag, "style") || !strcmp(tag, "title")) {
        char close[20];
        snprintf(close, sizeof(close), "</%s", tag);
        const char *end = find_ci_n(after_tag, ps->html_end, close);
        if (end) {
            const char *close_end = find_tag_end(end + 2, ps->html_end);
            *cursor_io = close_end ? close_end + 1 : ps->html_end;
        } else {
            *cursor_io = ps->html_end;
        }
        return;
    }

    unsigned flags = html_tag_flags(tag);
    parse_implied_close(ps, tag, flags);

    /* Block elements break the line BEFORE their markers, so wrap_text()
     * cannot leave the marker on the preceding line. */
    if (flags & TF_BREAK_BEFORE)
        sb_line_break(sb);

    /* Table cells: the separator belongs before the cell's own markers. */
    if (!strcmp(tag, "td") || !strcmp(tag, "th")) {
        if (!sb_at_line_start(sb) && sb->buf[sb->len - 1] != ' ')
            sb_text(sb, " | ");
    }

    open_element_t *el = NULL;
    if (flags & (TF_STYLE | TF_COLOR | TF_HEADING | TF_SCOPE)) {
        el = parse_push_element(ps, tag, flags);
        if (el && (flags & (TF_STYLE | TF_COLOR)) && ps->push_depth < PARSE_PUSH_MAX &&
            parse_emit_push_markers(ps, tag, flags, attributes, tag_end)) {
            el->pushed = true;
            ps->push_depth++;
        }
    }

    /* Every container needs a stack entry so its close markers are written.
     * If the stack is full (pathologically nested page), render the element
     * without its formatting rather than leave an unmatched open marker. */
    bool tracked = el != NULL || !(flags & (TF_STYLE | TF_COLOR | TF_HEADING | TF_SCOPE));

    if (!tracked) {
        /* nothing: no heading/bold/italic/code/link markers */
    } else if (flags & TF_HEADING) {
        sb_byte(sb, TEXT_HEADING_ON);
        sb_text(sb, "= ");
    } else if (!strcmp(tag, "pre")) {
        sb_line_break(sb);
        if (!ps->in_pre) sb_byte(sb, TEXT_PRE_ON);
        ps->in_pre = true;
        ps->pre_skip_newline = true;
    } else if (!strcmp(tag, "br")) {
        if (ps->in_pre) sb_byte(sb, '\n');
        else sb_line_break(sb);
    } else if (!strcmp(tag, "ul") || !strcmp(tag, "ol")) {
        if (ps->list_depth < PARSE_LIST_MAX) {
            ps->lists[ps->list_depth].ordered = !strcmp(tag, "ol");
            ps->lists[ps->list_depth].item = 0;
        }
        ps->list_depth++;
    } else if (!strcmp(tag, "li")) {
        sb_line_break(sb);
        int d = ps->list_depth - 1;
        if (d >= PARSE_LIST_MAX) d = PARSE_LIST_MAX - 1;
        if (d >= 0 && ps->lists[d].ordered) {
            char number[16];
            snprintf(number, sizeof(number), "%d. ", ++ps->lists[d].item);
            sb_text(sb, number);
        } else {
            sb_text(sb, "* ");
        }
    } else if (!strcmp(tag, "code")) {
        sb_byte(sb, '`');
    } else if (!strcmp(tag, "strong") || !strcmp(tag, "b")) {
        sb_byte(sb, TEXT_BOLD_ON);
    } else if (!strcmp(tag, "em") || !strcmp(tag, "i")) {
        sb_byte(sb, '_');
    } else if (!strcmp(tag, "hr")) {
        sb_line_break(sb);
        sb_byte(sb, TEXT_HRULE);
        sb_byte(sb, '\n');
    } else if (!strcmp(tag, "form")) {
        sb_line_break(sb);
        if (page->form_count < MAX_FORMS) {
            ps->current_form = page->form_count++;
            form_t *form = &page->forms[ps->current_form];
            memset(form, 0, sizeof(*form));
            snprintf(form->method, sizeof(form->method), "get");
            snprintf(form->enctype, sizeof(form->enctype), "application/x-www-form-urlencoded");
            if (tag_attribute(attributes, tag_end, "action", form->action, sizeof(form->action)) == 2)
                form->action[0] = 0;   /* too long: never submit to a truncated URL */
            trim_inplace(form->action);
            if (tag_attribute(attributes, tag_end, "method", form->method, sizeof(form->method)))
                lower_ascii(form->method);
            if (tag_attribute(attributes, tag_end, "enctype", form->enctype, sizeof(form->enctype)))
                lower_ascii(form->enctype);
        } else {
            ps->current_form = -1;
        }
    } else if (!strcmp(tag, "input") && ps->current_form >= 0) {
        form_t *form = &page->forms[ps->current_form];
        if (form->field_count < MAX_FORM_FIELDS) {
            form_field_t field;
            memset(&field, 0, sizeof(field));
            snprintf(field.type, sizeof(field.type), "text");
            tag_attribute(attributes, tag_end, "name", field.name, sizeof(field.name));
            tag_attribute(attributes, tag_end, "value", field.value, sizeof(field.value));
            if (tag_attribute(attributes, tag_end, "type", field.type, sizeof(field.type))) lower_ascii(field.type);
            field.disabled = tag_attribute(attributes, tag_end, "disabled", NULL, 0);
            sanitize_text_inplace(field.name);
            sanitize_text_inplace(field.value);

            bool editable = !strcmp(field.type, "text") || !strcmp(field.type, "search") || !strcmp(field.type, "url");
            bool hidden = !strcmp(field.type, "hidden");
            bool submit = !strcmp(field.type, "submit");
            if ((editable && field.name[0]) || (hidden && field.name[0]) || submit) {
                int field_index = form->field_count++;
                form->fields[field_index] = field;
                if (submit) {
                    form_field_t *f = &form->fields[field_index];
                    snprintf(f->label, sizeof(f->label), "%s", f->value[0] ? f->value : "Submit");
                    if (!field.disabled) {
                        int action = add_action(page, ACTION_FORM_SUBMIT, -1, ps->current_form, field_index);
                        if (action >= 0) sb_action_marker(sb, action);
                    }
                } else if (editable && !field.disabled) {
                    int action = add_action(page, ACTION_FORM_FIELD, -1, ps->current_form, field_index);
                    if (action >= 0) sb_action_marker(sb, action);
                }
            }
        }
    } else if (!strcmp(tag, "button") && ps->current_form >= 0) {
        form_t *form = &page->forms[ps->current_form];
        char type[16] = "submit";
        tag_attribute(attributes, tag_end, "type", type, sizeof(type));
        lower_ascii(type);

        /* Look for </button> only up to the next <button> or </form>, and
         * within a bounded window, so an unclosed button cannot swallow the
         * rest of the page (or make many buttons quadratic). */
        const char *window_end = after_tag + 4096 < ps->html_end ? after_tag + 4096 : ps->html_end;
        const char *close = find_ci_n(after_tag, window_end, "</button");
        const char *next_button = find_ci_n(after_tag, close ? close : window_end, "<button");
        const char *form_end = find_ci_n(after_tag, close ? close : window_end, "</form");
        if (next_button || form_end) close = NULL;

        if (!strcmp(type, "submit") && form->field_count < MAX_FORM_FIELDS) {
            int field_index = form->field_count++;
            form_field_t *field = &form->fields[field_index];
            memset(field, 0, sizeof(*field));
            snprintf(field->type, sizeof(field->type), "submit");
            tag_attribute(attributes, tag_end, "name", field->name, sizeof(field->name));
            tag_attribute(attributes, tag_end, "value", field->value, sizeof(field->value));
            field->disabled = tag_attribute(attributes, tag_end, "disabled", NULL, 0);
            sanitize_text_inplace(field->name);
            sanitize_text_inplace(field->value);
            if (close) extract_button_label(after_tag, close, field->label, sizeof(field->label));
            if (!field->label[0])
                snprintf(field->label, sizeof(field->label), "%s", field->value[0] ? field->value : "Submit");
            if (!field->disabled) {
                int action = add_action(page, ACTION_FORM_SUBMIT, -1, ps->current_form, field_index);
                if (action >= 0) sb_action_marker(sb, action);
            }
        }
        if (close) {
            const char *close_end = find_tag_end(close + 2, ps->html_end);
            *cursor_io = close_end ? close_end + 1 : ps->html_end;
        }
    } else if (!strcmp(tag, "img")) {
        char src_value[URL_MAX] = "";
        char alt_value[96] = "";
        char width_value[16] = "";
        char height_value[16] = "";

        int src_found = tag_attribute(attributes, tag_end, "src", src_value, sizeof(src_value));
        if (src_found == 2) {
            /* src longer than URL_MAX: keep a visible placeholder. */
            tag_attribute(attributes, tag_end, "alt", alt_value, sizeof(alt_value));
            sanitize_text_inplace(alt_value);
            page->image_seen_count++;
            sb_line_break(sb);
            char placeholder[160];
            snprintf(placeholder, sizeof(placeholder), "[Image omitted: %s]",
                     alt_value[0] ? alt_value : "image");
            sb_text(sb, placeholder);
            sb_line_break(sb);
        } else if (src_found == 1) {
            trim_inplace(src_value);
            if (!is_supported_href(src_value)) return;
            page->image_seen_count++;

            tag_attribute(attributes, tag_end, "alt", alt_value, sizeof(alt_value));
            tag_attribute(attributes, tag_end, "width", width_value, sizeof(width_value));
            tag_attribute(attributes, tag_end, "height", height_value, sizeof(height_value));
            sanitize_text_inplace(alt_value);

            sb_line_break(sb);

            char absolute[URL_MAX];
            long src_off = -1;
            if (display_mode_has_images() && page->image_count < MAX_PAGE_IMAGES &&
                resolve_url(page->base, src_value, absolute, sizeof(absolute)))
                src_off = page_store_string(page, absolute);

            if (!display_mode_has_images()) {
                char placeholder[160];
                snprintf(placeholder, sizeof(placeholder), "[Image: %s]",
                         alt_value[0] ? alt_value : "image");
                sb_text(sb, placeholder);
                sb_line_break(sb);
            } else if (src_off >= 0) {
                int image_index = page->image_count++;
                page_image_t *image = &page->images[image_index];
                memset(image, 0, sizeof(*image));
                image->src = (uint32_t)src_off;
                snprintf(image->alt, sizeof(image->alt), "%s", alt_value[0] ? alt_value : "image");
                image->requested_width = parse_html_dimension(width_value);
                image->requested_height = parse_html_dimension(height_value);

                if (image_index < display_inline_image_limit()) {
                    /* The image's own placement (align="left|center|right",
                     * or style margin auto) wraps the marker in a style
                     * push/pop.  Both are zero-width, so the line is still
                     * an image line; the renderer reads the alignment from
                     * the text state at the marker. */
                    unsigned char img_align = 0;
                    char img_style[192] = "";
                    if (tag_attribute(attributes, tag_end, "style", img_style, sizeof(img_style)))
                        img_align = css_image_margin_align(img_style);
                    if (img_align == 0) {
                        char align_value[16] = "";
                        if (tag_attribute(attributes, tag_end, "align", align_value,
                                          sizeof(align_value)) == 1)
                            img_align = html_align_value(align_value);
                    }

                    char image_marker[48];
                    size_t n = 0;
                    if (img_align) {
                        n += utf8_encode(TEXT_STYLE_START, image_marker + n);
                        n += utf8_encode(TEXT_STYLE_NIBBLE, image_marker + n);
                        n += utf8_encode(TEXT_STYLE_NIBBLE + img_align, image_marker + n);
                    }
                    n += (size_t)snprintf(image_marker + n, sizeof(image_marker) - n,
                                          "%c[[MBIMG%d]]", TEXT_IMAGE_MARK, image_index);
                    if (img_align)
                        n += utf8_encode(TEXT_STYLE_POP, image_marker + n);
                    sb_bytes(sb, image_marker, n);
                    sb_line_break(sb);
                } else {
                    int action = add_action(page, ACTION_IMAGE, -1, -1, -1);
                    if (action >= 0) {
                        page->actions[action].image_index = image_index;
                        sb_action_marker(sb, action);
                    }
                }
            } else {
                char placeholder[160];
                snprintf(placeholder, sizeof(placeholder), "[Image omitted: %s]",
                         alt_value[0] ? alt_value : "image");
                sb_text(sb, placeholder);
                sb_line_break(sb);
            }
        }
    } else if (!strcmp(tag, "a") && el) {
        char href[URL_MAX] = "";
        if (tag_attribute(attributes, tag_end, "href", href, sizeof(href)) == 1) {
            trim_inplace(href);
            char absolute[URL_MAX];
            long href_off = -1;
            if (is_supported_href(href) && page->link_count < MAX_LINKS &&
                resolve_url(page->base, href, absolute, sizeof(absolute)) &&
                is_supported_href(absolute))
                href_off = page_store_string(page, absolute);
            if (href_off >= 0) {
                int link_index = page->link_count++;
                page->links[link_index].href = (uint32_t)href_off;
                int action = add_action(page, ACTION_LINK, link_index, -1, -1);
                if (action >= 0) {
                    char number[16];
                    sb_byte(sb, TEXT_LINK_ON);
                    snprintf(number, sizeof(number), "[%d]", action + 1);
                    sb_text(sb, number);
                    if (el) el->link_on = true;
                }
            }
        }
    }
}

__attribute__((noinline)) static page_t *html_to_page(const char *html, const char *base_url) {
    if (!html) return NULL;
    size_t length = strlen(html);
    page_t *page = (page_t*)calloc(1, sizeof(page_t));
    if (!page) return NULL;

    html_parser_t *ps = (html_parser_t *)calloc(1, sizeof(html_parser_t));
    if (!ps) { free(page); return NULL; }
    ps->page = page;
    ps->html_end = html + length;
    ps->current_form = -1;
    ps->sb.limit = length * 4 + 4096 < TEMPLATE_MAX ? length * 4 + 4096 : TEMPLATE_MAX;
    if (!sb_reserve(&ps->sb, length + 256)) {
        free(ps->sb.buf); free(ps); free(page);
        return NULL;
    }
    ps->sb.buf[0] = 0;

    snprintf(page->base, sizeof(page->base), "%s", base_url ? base_url : "");
    extract_html_title(html, page->title, sizeof(page->title));

    const char *html_end = ps->html_end;
    for (const char *cursor = html; cursor < html_end && *cursor && !ps->sb.failed; ) {
        if (*cursor != '<' || ps->tags_exhausted) {
            parse_text(ps, &cursor);
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

        /* HTML tokenizer rule: '<' only starts a tag when followed by an
         * ASCII letter. "a < b" and "1 < 2" are text. */
        if (p >= html_end || !isalpha((unsigned char)*p)) {
            parse_text(ps, &cursor);
            continue;
        }

        char tag[16]; size_t tag_len = 0;
        /* Tag names may contain digits after the first letter (h1..h6). */
        while (p < html_end && isalnum((unsigned char)*p)) {
            if (tag_len + 1 < sizeof(tag)) tag[tag_len++] = (char)tolower((unsigned char)*p);
            p++;
        }
        tag[tag_len] = 0;
        const char *attributes = p;
        const char *tag_end = find_tag_end(attributes, html_end);
        if (!tag_end) {
            /* No '>' anywhere after this point: the rest is text, not a
             * truncated document. */
            ps->tags_exhausted = true;
            parse_text(ps, &cursor);
            continue;
        }

        const char *next = tag_end + 1;
        if (closing) parse_close_tag(ps, tag);
        else parse_open_tag(ps, tag, attributes, tag_end, &next);
        cursor = next;
    }

    /* Close everything still open so every push has its pop. */
    if (ps->in_pre) sb_byte(&ps->sb, TEXT_PRE_OFF);
    parse_close_to(ps, 0);

    if (ps->sb.failed) {
        /* Over the template limit or out of memory: keep what fits and say so. */
        ps->sb.failed = false;
        ps->sb.limit += 64;
        sb_text(&ps->sb, "\n[Page truncated]\n");
        printf("[mini_browser] parser: template limit reached, page truncated\n");
    }

    page->text_template = ps->sb.buf;
    free(ps);
    if (!refresh_page_text(page)) {
        free(page->text_template);
        free(page->strings);
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

/*
 * application/x-www-form-urlencoded serialization of a form's successful
 * controls, appended to out (which may already hold "url?").  Shared by
 * GET and POST so the "which fields are sent" rule lives in one place.
 * Encodes straight into out: no large per-field stack buffers.
 */
static int form_serialize(const form_t *form, int submit_field_index,
                          char *out, size_t out_cap) {
    bool first = true;
    for (int i = 0; i < form->field_count; i++) {
        const form_field_t *field = &form->fields[i];
        if (field->disabled || !field->name[0]) continue;
        bool submit = !strcmp(field->type, "submit");
        bool successful = !strcmp(field->type, "text") || !strcmp(field->type, "search") ||
                          !strcmp(field->type, "url") || !strcmp(field->type, "hidden") ||
                          (submit && i == submit_field_index);
        if (!successful) continue;

        size_t used = strlen(out);
        if (!first) {
            if (used + 1 >= out_cap) return 0;
            out[used++] = '&';
            out[used] = 0;
        }
        if (!form_urlencode(field->name, out + used, out_cap - used)) return 0;
        if (!append_url_part(out, out_cap, "=")) return 0;
        used = strlen(out);
        if (!form_urlencode(field->value, out + used, out_cap - used)) return 0;
        first = false;
    }
    return 1;
}

/* Form target: action resolved against the page, or the page itself. */
static int form_target_url(const page_t *page, const form_t *form, char *out, size_t out_cap) {
    if (form->action[0]) {
        if (!resolve_url(page->base, form->action, out, out_cap)) return 0;
    } else if (snprintf(out, out_cap, "%s", page->base) >= (int)out_cap) {
        return 0;
    }
    char *hash = strchr(out, '#');
    if (hash) *hash = 0;
    return 1;
}

static int build_get_form_url(const page_t *page, int form_index,
                              int submit_field_index, char *out, size_t out_cap) {
    if (!page || form_index < 0 || form_index >= page->form_count || !out_cap) return 0;
    const form_t *form = &page->forms[form_index];
    const char *method = form->method[0] ? form->method : "get";
    if (strcasecmp(method, "get")) return -1;

    if (!form_target_url(page, form, out, out_cap)) return 0;

    /* HTML: GET submission replaces the action URL's query.  Appending made
     * a second search from a results page send "?q=old&q=new". */
    char *query = strchr(out, '?');
    if (query) *query = 0;
    if (!append_url_part(out, out_cap, "?")) return 0;
    if (!form_serialize(form, submit_field_index, out, out_cap)) return 0;

    /* No successful controls: do not leave a dangling '?'. */
    size_t len = strlen(out);
    if (len && out[len - 1] == '?') out[len - 1] = 0;
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

    if (!form_target_url(page, form, url, url_cap)) return 0;
    body[0] = 0;
    return form_serialize(form, submit_field_index, body, body_cap);
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

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/*
 * Google search-result redirect wrapper:
 *   http(s)://www.google.com/url?q=https%3A%2F%2Fexample.com%2F&sa=...
 * Returns 1 and the percent-decoded q= destination when it is an http(s)
 * URL that fits; otherwise 0 and the caller follows href itself.
 * (tag_attribute() has already decoded &amp; in the href.)
 */
static int unwrap_google_redirect(const char *href, char *out, size_t cap) {
    static const char *const prefixes[] = {
        "http://www.google.com/url?", "https://www.google.com/url?"
    };
    const char *query = NULL;
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        size_t n = strlen(prefixes[i]);
        if (!strncmp(href, prefixes[i], n)) { query = href + n; break; }
    }
    if (!query || !cap) return 0;

    /* Find the q= parameter (it may not be the first one). */
    const char *q = NULL;
    for (const char *p = query; p && *p; ) {
        if (!strncmp(p, "q=", 2)) { q = p + 2; break; }
        p = strchr(p, '&');
        if (p) p++;
    }
    if (!q) return 0;

    size_t used = 0;
    for (const char *p = q; *p && *p != '&' && *p != '#'; p++) {
        char c = *p;
        if (c == '%' && hex_value(p[1]) >= 0 && hex_value(p[2]) >= 0) {
            c = (char)(hex_value(p[1]) * 16 + hex_value(p[2]));
            p += 2;
        } else if (c == '+') {
            c = ' ';
        }
        if ((unsigned char)c < 0x21 || used + 1 >= cap) return 0;
        out[used++] = c;
    }
    out[used] = 0;
    return is_http_scheme(out);
}

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

        const char *href = page_link_href(page, action->link_index);
        printf("[mini_browser] activating link %d -> %s\n", action_index + 1, href);

        if (unwrap_google_redirect(href, navigation_url, navigation_cap)) {
            printf("[mini_browser] Google direct -> %s\n", navigation_url);
        } else if (snprintf(navigation_url, navigation_cap, "%s", href) >= (int)navigation_cap) {
            return ACTIVATE_URL_TOO_LONG;
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
        snprintf(edit_buf, edit_cap, "%s", field->value);
        *edit_cursor = strlen(edit_buf);
        *edit_form = action->form_index;
        *edit_field = action->field_index;
        return ACTIVATE_EDIT_FIELD;
    }

    if (action->type == ACTION_FORM_SUBMIT) {
        const char *method = form->method[0] ? form->method : "get";
        int result;
        if (!strcasecmp(method, "post")) {
            result = build_post_form_request(page, action->form_index,
                                             action->field_index,
                                             navigation_url, navigation_cap,
                                             post_body, post_body_cap);
            if (result > 0) return ACTIVATE_POST;
        } else {
            result = build_get_form_url(page, action->form_index,
                                        action->field_index,
                                        navigation_url, navigation_cap);
            if (result > 0) return ACTIVATE_NAVIGATE;
        }
        return result < 0 ? ACTIVATE_FORM_UNSUPPORTED : ACTIVATE_URL_TOO_LONG;
    }
    return ACTIVATE_NONE;
}

static void free_page(page_t *page) {
    if (!page) return;
    free(page->text);
    free(page->text_template);
    free(page->strings);
    free(page->html);
    free(page);
}

/* ---------- UTF-8 decoder forward declaration ---------- */
static unsigned utf8_next(const char *s, size_t len, size_t *i);

/* ---------- wrap text to columns ---------- */


static int wrap_glyph_width(unsigned cp) {
    if (is_format_marker(cp))
        return 0;

    if (cp >= 32 && cp <= 126)
        return CH_W;

    if (cp >= 0x80 && cp <= 0x10FFFF)
        return (UNICODE_GLYPH_W + 1) * FONT_SCALE / 2;

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

/* Phase 3 GIF Test3: used by wrapping to preserve inline-image control
 * markers as standalone logical lines. Definition is in the image section. */
static int is_image_marker_line(const char *line, int len, int *index);

static char *wrap_text_scaled(const char *in, int max_cols);

/* Wrapping measures text at the page's zoom. */
static char *wrap_text(const char *in, int max_cols) {
    PAGE_SCALE_BEGIN();
    char *out = wrap_text_scaled(in, max_cols);
    PAGE_SCALE_END();
    return out;
}

static char *wrap_text_scaled(const char *in, int max_cols) {
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
    bool in_pre = false;   /* inside TEXT_PRE_ON..OFF: keep spaces and blank lines */

    while (i < n) {
        size_t cp_start = i;
        unsigned cp = utf8_next(in, n, &i);

        if (cp == 0)
            break;

        if (cp == '\r')
            continue;

        /* Treat NBSP as the same break opportunity as an ordinary space. */
        if (cp == ' ' || cp == 0xA0) {
            if (in_pre) {
                /* Preformatted text: every space is significant. */
                if (max_px > 0 && line_px + space_w > max_px) {
                    out[o++] = '\n';
                    line_px = 0;
                }
                out[o++] = ' ';
                line_px += space_w;
                continue;
            }
            if (line_px > 0)
                pending_space = true;
            continue;
        }

        if (cp == '\n') {
            pending_space = false;

            if (in_pre) {
                blank_run = 0;
            } else if (line_px == 0) {
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
         * Image markers are renderer control records, not ordinary text.
         * Keep [[MBIMGn]] on a logical line by itself even when real-world
         * HTML places an <img> directly after text without a block break.
         * The renderer intentionally recognizes image markers only when the
         * complete line is the marker.
         */
        int marker_index = -1;
        if (is_image_marker_line(in + token_start,
                                 (int)(token_end - token_start),
                                 &marker_index)) {
            if (line_px > 0)
                out[o++] = '\n';

            memcpy(out + o, in + token_start, token_end - token_start);
            o += token_end - token_start;
            out[o++] = '\n';
            line_px = 0;
            blank_run = 0;
            i = token_end;
            continue;
        }

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

            if (glyph == TEXT_PRE_ON) in_pre = true;
            else if (glyph == TEXT_PRE_OFF) in_pre = false;

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
    long long expires;           /* 4.3: 0 = session cookie (kept in memory only) */
    long pending_age;            /* 4.3: Max-Age seen before the clock was known */
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

static bool host_is_ip_literal(const char *host) {
    if (!host || !*host) return false;
    if (strchr(host, ':')) return true;              /* IPv6 */
    for (const char *p = host; *p; p++)
        if (!isdigit((unsigned char)*p) && *p != '.') return false;
    return true;                                     /* dotted IPv4 */
}

/*
 * A Domain= attribute must not name a public suffix, or one site could set
 * a cookie for every ".com" / ".co.uk" host.  Without a full public-suffix
 * list: a domain needs an internal dot, and a few common multi-label
 * suffixes are refused explicitly.
 */
static bool cookie_domain_is_public_suffix(const char *domain) {
    static const char *const suffixes[] = {
        "co.uk", "org.uk", "ac.uk", "gov.uk", "me.uk", "ltd.uk", "plc.uk",
        "com.au", "net.au", "org.au", "co.nz", "co.jp", "ne.jp", "or.jp",
        "com.br", "com.cn", "com.tw", "co.kr", "co.za", "co.in", "com.mx",
        "github.io", "gitlab.io", "blogspot.com", "herokuapp.com",
        "netlify.app", "vercel.app", "pages.dev", "workers.dev"
    };
    if (!strchr(domain, '.')) return true;
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++)
        if (!strcasecmp(domain, suffixes[i])) return true;
    return false;
}

static bool cookie_domain_match(const char *host, const char *domain) {
    size_t hl, dl;
    if (!host || !domain || !*host || !*domain) return false;
    if (!strcasecmp(host, domain)) return true;
    /* IP addresses only match exactly ("0.0.5" is not a parent of 10.0.0.5). */
    if (host_is_ip_literal(host)) return false;
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

/*
 * Expires=<HTTP-date> -> true when the date is in the past.  Without a
 * known clock only dates before 2000 (the usual "Thu, 01 Jan 1970 00:00:00
 * GMT" logout idiom) count as past.
 */
static bool cookie_expires_in_past(const char *value) {
    long long t = http_date_parse(value);
    if (!t) return false;
    long long now = clock_now();
    if (!now) return t < 946684800LL;
    return t <= now;
}

/* ---------- 4.3 part 2: cookies saved across restarts ----------
 *
 * Cookies with Max-Age or Expires are written to COOKIE_FILE; session
 * cookies stay in memory, as in other browsers.  The file is rewritten only
 * when a saved cookie changes, from the main loop (never during a load).
 */
#define COOKIE_FILE "APPS:[mini_browser]cookies.txt"
static bool g_cookie_dirty;

static bool cookie_is_persistent(const mb_cookie_t *c) {
    return c->used && (c->expires > 0 || c->pending_age > 0);
}

static bool cookie_expired(const mb_cookie_t *c, long long now) {
    return c->expires > 0 && now > 0 && now >= c->expires;
}

/* Max-Age arrived before any Date header: count it from now on. */
static void cookie_resolve_pending(void) {
    long long now = clock_now();
    if (!now) return;
    for (int i = 0; i < MAX_COOKIES; i++) {
        mb_cookie_t *c = &g_cookie_jar[i];
        if (c->used && c->pending_age > 0) {
            c->expires = now + c->pending_age;
            c->pending_age = 0;
            g_cookie_dirty = true;
        }
    }
}

__attribute__((noinline)) static void cookie_save(void) {
    long long now = clock_now();
    int saved = 0;
    g_cookie_dirty = false;
    remove(COOKIE_FILE);
    FILE *f = NULL;
    for (int i = 0; i < MAX_COOKIES; i++) {
        mb_cookie_t *c = &g_cookie_jar[i];
        if (!c->used || c->expires <= 0 || cookie_expired(c, now)) continue;
        if (!f) f = fopen(COOKIE_FILE, "w");
        if (!f) break;
        fprintf(f, "%s\t%s\t%s\t%s\t%lld\t%d\t%d\n", c->domain, c->path, c->name, c->value,
                c->expires, c->secure ? 1 : 0, c->host_only ? 1 : 0);
        saved++;
    }
    if (f) fclose(f);
    printf("[mini_browser] cookies: saved %d of %d\n", saved, cookie_count());
}

__attribute__((noinline)) static void cookie_load(void) {
    static char line[COOKIE_DOMAIN_MAX + COOKIE_PATH_MAX + COOKIE_NAME_MAX + COOKIE_VALUE_MAX + 64];
    FILE *f = fopen(COOKIE_FILE, "r");
    if (!f) {
        printf("[mini_browser] cookies: none saved\n");
        return;
    }
    int loaded = 0, slot = 0;
    while (fgets(line, sizeof line, f) && slot < MAX_COOKIES) {
        line[strcspn(line, "\r\n")] = 0;
        char *field[7];
        int nf = 0;
        for (char *p = line; nf < 7; ) {
            field[nf++] = p;
            char *tab = strchr(p, '\t');
            if (!tab) break;
            *tab = 0;
            p = tab + 1;
        }
        if (nf != 7 || !field[0][0] || !field[2][0] || field[1][0] != '/' ||
            strlen(field[0]) > COOKIE_DOMAIN_MAX || strlen(field[1]) > COOKIE_PATH_MAX ||
            strlen(field[2]) > COOKIE_NAME_MAX || strlen(field[3]) > COOKIE_VALUE_MAX)
            continue;
        mb_cookie_t *c = &g_cookie_jar[slot++];
        memset(c, 0, sizeof *c);
        c->used = true;
        snprintf(c->domain, sizeof c->domain, "%s", field[0]);
        snprintf(c->path, sizeof c->path, "%s", field[1]);
        snprintf(c->name, sizeof c->name, "%s", field[2]);
        snprintf(c->value, sizeof c->value, "%s", field[3]);
        c->expires = atoll(field[4]);
        c->secure = atoi(field[5]) != 0;
        c->host_only = atoi(field[6]) != 0;
        c->age = g_cookie_age++;
        loaded++;
    }
    fclose(f);
    printf("[mini_browser] cookies: loaded %d saved\n", loaded);
}

static int cookie_persistent_count(void) {
    int n = 0;
    for (int i = 0; i < MAX_COOKIES; i++)
        if (cookie_is_persistent(&g_cookie_jar[i])) n++;
    return n;
}

static void cookie_clear_all(void) {
    int n = cookie_count();
    memset(g_cookie_jar, 0, sizeof g_cookie_jar);
    remove(COOKIE_FILE);
    g_cookie_dirty = false;
    printf("[mini_browser] cookies: cleared %d\n", n);
}

static void cookie_store_header(const char *value, size_t value_len) {
    bool request_secure = false;
    bool secure = false;
    bool host_only = true;
    bool remove = false;
    long long expires = 0;        /* 4.3: from Expires= */
    long max_age = 0;             /* 4.3: from Max-Age= (wins over Expires) */
    bool has_max_age = false;

    if (!value || !cookie_url_parts(g_cookie_request_url, &request_secure))
        return;

    /* Never store a cut-off cookie: sending back a truncated session token
     * is worse than not storing it. */
    if (value_len >= sizeof(g_cookie_set_line)) {
        printf("[mini_browser] cookie ignored: Set-Cookie header too long (%u bytes)\n",
               (unsigned)value_len);
        return;
    }
    size_t n = value_len;
    memcpy(g_cookie_set_line, value, n);
    g_cookie_set_line[n] = 0;
    while (n && (g_cookie_set_line[n - 1] == '\r' ||
                 g_cookie_set_line[n - 1] == '\n'))
        g_cookie_set_line[--n] = 0;

    char *semi = strchr(g_cookie_set_line, ';');
    char *pair_end = semi ? semi : g_cookie_set_line + strlen(g_cookie_set_line);
    char *eq = memchr(g_cookie_set_line, '=', (size_t)(pair_end - g_cookie_set_line));
    if (!eq) return;

    /* Measure name and value before copying, so oversize ones are dropped
     * instead of being stored cut off. */
    const char *nv = eq + 1;
    size_t nv_len = (size_t)(pair_end - nv);
    while (nv_len && isspace((unsigned char)*nv)) { nv++; nv_len--; }
    while (nv_len && isspace((unsigned char)nv[nv_len - 1])) nv_len--;
    const char *nm = g_cookie_set_line;
    size_t name_len = (size_t)(eq - g_cookie_set_line);
    size_t nm_len = name_len;
    while (nm_len && isspace((unsigned char)*nm)) { nm++; nm_len--; }
    while (nm_len && isspace((unsigned char)nm[nm_len - 1])) nm_len--;
    if (nm_len > COOKIE_NAME_MAX || nv_len > COOKIE_VALUE_MAX) {
        printf("[mini_browser] cookie ignored: name/value too long\n");
        return;
    }

    cookie_copy_trim(g_cookie_tmp_name, sizeof(g_cookie_tmp_name),
                     g_cookie_set_line, name_len, false);
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
                if (strcasecmp(g_cookie_host, g_cookie_tmp_domain) &&
                    cookie_domain_is_public_suffix(g_cookie_tmp_domain)) {
                    printf("[mini_browser] cookie ignored: Domain=%s is a public suffix\n",
                           g_cookie_tmp_domain);
                    return;
                }
                host_only = false;
            } else if (!strcasecmp(a, "path") && *av == '/') {
                cookie_copy_trim(g_cookie_tmp_path, sizeof(g_cookie_tmp_path),
                                 av, strlen(av), false);
            } else if (!strcasecmp(a, "max-age")) {
                char *ep = NULL;
                long age = strtol(av, &ep, 10);
                if (ep != av && age <= 0) remove = true;
                if (ep != av && age > 0) { max_age = age; has_max_age = true; }
            } else if (!strcasecmp(a, "expires")) {
                if (cookie_expires_in_past(av)) remove = true;
                else expires = http_date_parse(av);
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

    /* RFC 6265bis: a plain-http response may not set (or overwrite) a
     * Secure cookie. */
    if (secure && !request_secure) {
        printf("[mini_browser] cookie ignored: Secure cookie over http: %s\n",
               g_cookie_tmp_name);
        return;
    }

    int slot = cookie_find(g_cookie_tmp_name,
                           g_cookie_tmp_domain,
                           g_cookie_tmp_path);

    if (remove) {
        if (slot >= 0) {
            if (cookie_is_persistent(&g_cookie_jar[slot])) g_cookie_dirty = true;
            memset(&g_cookie_jar[slot], 0, sizeof(g_cookie_jar[slot]));
        }
        printf("[mini_browser] cookie delete: %s count=%d\n",
               g_cookie_tmp_name, cookie_count());
        return;
    }

    slot = cookie_store_slot(g_cookie_tmp_name,
                             g_cookie_tmp_domain,
                             g_cookie_tmp_path);
    mb_cookie_t *c = &g_cookie_jar[slot];
    if (cookie_is_persistent(c)) g_cookie_dirty = true;   /* replaced or evicted */
    memset(c, 0, sizeof(*c));
    c->used = true;
    if (has_max_age) {
        long long now = clock_now();
        if (now) c->expires = now + max_age;
        else c->pending_age = max_age;
    } else if (expires > 0) {
        c->expires = expires;
    }
    if (cookie_is_persistent(c)) g_cookie_dirty = true;
    c->secure = secure;
    c->host_only = host_only;
    c->age = g_cookie_age++;
    strncpy(c->name, g_cookie_tmp_name, sizeof(c->name) - 1);
    strncpy(c->value, g_cookie_tmp_value, sizeof(c->value) - 1);
    strncpy(c->domain, g_cookie_tmp_domain, sizeof(c->domain) - 1);
    strncpy(c->path, g_cookie_tmp_path, sizeof(c->path) - 1);

#if MB_LOG_SENSITIVE
    printf("[mini_browser] cookie store: %s=%s domain=%s path=%s secure=%d count=%d\n",
           c->name, c->value, c->domain, c->path,
           c->secure ? 1 : 0, cookie_count());
#else
    printf("[mini_browser] cookie store: %s=<%u bytes> domain=%s path=%s secure=%d count=%d\n",
           c->name, (unsigned)strlen(c->value), c->domain, c->path,
           c->secure ? 1 : 0, cookie_count());
#endif
}

static bool cookie_make_request_header(const char *url) {
    bool request_secure = false;
    size_t used = 0;
    bool any = false;

    g_cookie_header_value[0] = 0;
    if (!cookie_url_parts(url, &request_secure)) return false;

    long long now = clock_now();
    for (int i = 0; i < MAX_COOKIES; i++) {
        mb_cookie_t *c = &g_cookie_jar[i];
        if (!c->used) continue;
        if (cookie_expired(c, now)) {          /* 4.3: expired since it was stored */
            memset(c, 0, sizeof *c);
            g_cookie_dirty = true;
            continue;
        }
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

/* Location of the most recent 3xx response (raw header value). */
static char g_redirect_location[URL_MAX];
static bool g_redirect_location_too_long;

/* Header value after "Name:" with surrounding whitespace/CRLF removed. */
static bool header_value(const char *buffer, size_t n, const char *name,
                         const char **value, size_t *value_len) {
    size_t nl = strlen(name);
    if (!buffer || n <= nl || strncasecmp(buffer, name, nl) || buffer[nl] != ':')
        return false;
    const char *v = buffer + nl + 1;
    size_t len = n - nl - 1;
    while (len && (*v == ' ' || *v == '\t')) { v++; len--; }
    while (len && (v[len - 1] == '\r' || v[len - 1] == '\n' || v[len - 1] == ' ')) len--;
    *value = v;
    *value_len = len;
    return true;
}

static size_t cookie_header_cb(char *buffer, size_t size,
                               size_t nitems, void *userdata) {
    (void)userdata;
    size_t n = size * nitems;
    const char *value;
    size_t len;

    resp_parse_header(buffer, n);   /* 4.3: Date, cache and download headers */
    if (header_value(buffer, n, "Set-Cookie", &value, &len)) {
        /* g_cookie_request_url is the URL of this hop: redirects are
         * followed one request at a time by fetch_url(). */
        cookie_store_header(value, len);
    } else if (header_value(buffer, n, "Location", &value, &len)) {
        g_redirect_location_too_long = len >= sizeof(g_redirect_location);
        if (!g_redirect_location_too_long) {
            memcpy(g_redirect_location, value, len);
            g_redirect_location[len] = 0;
        }
    } else if (header_value(buffer, n, "Content-Type", &value, &len)) {
        /* BadgeVMS's curl does not implement CURLINFO_CONTENT_TYPE. */
        if (len >= sizeof(g_fetch_meta.content_type)) len = sizeof(g_fetch_meta.content_type) - 1;
        memcpy(g_fetch_meta.content_type, value, len);
        g_fetch_meta.content_type[len] = 0;
    }
    return n;
}


/* ---------- 4.3 part 2: disk cache on FLASH0 ----------
 *
 * Pages and images are kept in CACHE_DIR, one file per URL, with the
 * validators the server sent (ETag, Last-Modified) and its freshness
 * (Cache-Control max-age / Expires).  On the next load:
 *   - still fresh: the copy on flash is used, no request at all;
 *   - otherwise the request carries If-None-Match / If-Modified-Since and a
 *     "304 Not Modified" answer means the copy on flash is used.
 * Only 200 responses to GET are stored, never POST results, error pages,
 * "no-store" responses or bodies cut at a size limit.  Responses without a
 * validator or freshness are not stored either (nothing to check them with).
 *
 * The total is CACHE_BUDGET bytes counted in whole 4 KB FAT clusters; the
 * least recently used entries are removed first.  The index (hash, size,
 * last use) is a small text file, saved from the main loop.
 *
 * Entry file: "MBC1\n" url "\n" etag "\n" last-modified "\n" content-type "\n"
 *             stored-time "\n" max-age "\n" body-length "\n\n" body
 */
#define CACHE_DIR          "FLASH0:[MBCACHE]"
#define CACHE_INDEX        CACHE_DIR "index.txt"
#define CACHE_BUDGET       (2L * 1024 * 1024)
#define CACHE_MAX_ENTRIES  160
#define CACHE_CLUSTER      4096L
#define CACHE_TOUCH_SAVE   10      /* save the index after this many uses */

typedef struct {
    uint32_t hash;
    uint32_t size;          /* file size in bytes */
    uint32_t stamp;         /* last use (higher = more recent) */
} cache_entry_t;

typedef struct {
    char url[URL_MAX];
    char etag[96];
    char last_modified[48];
    char content_type[96];
    long long stored;       /* clock_now() when stored (0: unknown) */
    long max_age;           /* seconds fresh after stored (-1: revalidate) */
    long length;            /* body bytes */
} cache_meta_t;

static cache_entry_t g_cache[CACHE_MAX_ENTRIES];
static int g_cache_count;
static uint32_t g_cache_clock;
static bool g_cache_ready;
static bool g_cache_index_dirty;
static int g_cache_touches;
static bool g_cache_revalidate;     /* reload: never use a copy without asking */

static uint32_t cache_hash(const char *url) {
    uint32_t h = 2166136261u;                  /* FNV-1a */
    for (const unsigned char *p = (const unsigned char *)url; *p; p++)
        h = (h ^ *p) * 16777619u;
    return h;
}

static void cache_path(uint32_t hash, char *out, size_t cap) {
    snprintf(out, cap, CACHE_DIR "C%08lX.DAT", (unsigned long)hash);
}

static long cache_clusters(uint32_t size) {
    return ((long)size + CACHE_CLUSTER - 1) / CACHE_CLUSTER * CACHE_CLUSTER;
}

static long cache_total(void) {
    long total = 0;
    for (int i = 0; i < g_cache_count; i++) total += cache_clusters(g_cache[i].size);
    return total;
}

static int cache_find(uint32_t hash) {
    for (int i = 0; i < g_cache_count; i++)
        if (g_cache[i].hash == hash) return i;
    return -1;
}

static void cache_drop(int i, bool delete_file) {
    if (delete_file) {
        char path[64];
        cache_path(g_cache[i].hash, path, sizeof path);
        remove(path);
    }
    g_cache[i] = g_cache[--g_cache_count];
    g_cache_index_dirty = true;
}

__attribute__((noinline)) static void cache_save_index(void) {
    g_cache_index_dirty = false;
    g_cache_touches = 0;
    FILE *f = fopen(CACHE_INDEX, "w");
    if (!f) {
        printf("[mini_browser] cache: cannot write %s\n", CACHE_INDEX);
        return;
    }
    fprintf(f, "MBCI1 %lu\n", (unsigned long)g_cache_clock);
    for (int i = 0; i < g_cache_count; i++)
        fprintf(f, "%08lX %lu %lu\n", (unsigned long)g_cache[i].hash,
                (unsigned long)g_cache[i].size, (unsigned long)g_cache[i].stamp);
    fclose(f);
}

/* Files in the cache folder that the index does not know (after a power
 * cut, for example) would use flash for nothing: remove them. */
static void cache_remove_orphans(void) {
#if defined(__riscv)
    DIR *dir = opendir(CACHE_DIR);
    if (!dir) return;
    static char path[96];
    int removed = 0;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL) {
        unsigned long h = 0;
        char tail[8] = "";
        if (sscanf(e->d_name, "C%8lX.%3s", &h, tail) != 2 || strcasecmp(tail, "DAT")) continue;
        if (cache_find((uint32_t)h) >= 0) continue;
        snprintf(path, sizeof path, CACHE_DIR "%s", e->d_name);
        if (remove(path) == 0) removed++;
    }
    closedir(dir);
    if (removed) printf("[mini_browser] cache: removed %d unlisted files\n", removed);
#endif
}

__attribute__((noinline)) static void cache_init(void) {
    if (!mkdir_p(CACHE_DIR)) {
        printf("[mini_browser] cache: cannot create %s, cache off\n", CACHE_DIR);
        return;
    }
    g_cache_ready = true;
    FILE *f = fopen(CACHE_INDEX, "r");
    if (f) {
        static char line[64];
        unsigned long clock = 0;
        if (fgets(line, sizeof line, f) && sscanf(line, "MBCI1 %lu", &clock) == 1) {
            g_cache_clock = (uint32_t)clock;
            while (g_cache_count < CACHE_MAX_ENTRIES && fgets(line, sizeof line, f)) {
                unsigned long h, size, stamp;
                if (sscanf(line, "%lx %lu %lu", &h, &size, &stamp) != 3) continue;
                if (cache_find((uint32_t)h) >= 0) continue;
                g_cache[g_cache_count++] = (cache_entry_t){ (uint32_t)h, (uint32_t)size, (uint32_t)stamp };
            }
        }
        fclose(f);
    }
    cache_remove_orphans();
    printf("[mini_browser] cache: %d entries, %ld KB of %ld KB\n",
           g_cache_count, cache_total() / 1024, CACHE_BUDGET / 1024);
}

static void cache_read_line(FILE *f, char *out, size_t cap) {
    out[0] = 0;
    if (!fgets(out, (int)cap, f)) return;
    out[strcspn(out, "\r\n")] = 0;
}

/* Read the header of the entry for url; false when there is none. The file
 * is left open at the start of the body in *fp when fp is not NULL. */
__attribute__((noinline)) static bool cache_open(const char *url, cache_meta_t *meta, FILE **fp) {
    static char path[64], line[URL_MAX + 8];
    if (!g_cache_ready || !url) return false;
    uint32_t h = cache_hash(url);
    int i = cache_find(h);
    if (i < 0) return false;
    cache_path(h, path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f) {                                  /* gone: forget it */
        cache_drop(i, false);
        return false;
    }
    cache_read_line(f, line, sizeof line);
    bool ok = !strcmp(line, "MBC1");
    if (ok) {
        cache_read_line(f, meta->url, sizeof meta->url);
        ok = !strcmp(meta->url, url);          /* another URL with the same hash */
    }
    if (ok) {
        cache_read_line(f, meta->etag, sizeof meta->etag);
        cache_read_line(f, meta->last_modified, sizeof meta->last_modified);
        cache_read_line(f, meta->content_type, sizeof meta->content_type);
        cache_read_line(f, line, sizeof line); meta->stored = atoll(line);
        cache_read_line(f, line, sizeof line); meta->max_age = atol(line);
        cache_read_line(f, line, sizeof line); meta->length = atol(line);
        cache_read_line(f, line, sizeof line);
        ok = !line[0] && meta->length >= 0 && meta->length <= IMAGE_DOWNLOAD_MAX;
    }
    if (!ok || !fp) fclose(f);
    if (ok && fp) *fp = f;
    return ok;
}

static bool cache_is_fresh(const cache_meta_t *meta) {
    long long now = clock_now();
    return !g_cache_revalidate && meta->max_age > 0 && meta->stored > 0 && now > 0 &&
           now >= meta->stored && now < meta->stored + meta->max_age;
}

/* The body of the entry for url into m (replacing what it held). */
__attribute__((noinline)) static bool cache_read_body(const char *url, cache_meta_t *meta, mem_t *m) {
    FILE *f = NULL;
    if (!cache_open(url, meta, &f)) return false;
    char *buf = (char *)malloc((size_t)meta->length + 1);
    bool ok = buf && fread(buf, 1, (size_t)meta->length, f) == (size_t)meta->length;
    fclose(f);
    if (!ok) {
        free(buf);
        int i = cache_find(cache_hash(url));
        if (i >= 0) cache_drop(i, true);
        return false;
    }
    buf[meta->length] = 0;
    free(m->buf);
    m->buf = buf;
    m->len = (size_t)meta->length;
    m->cap = (size_t)meta->length + 1;
    m->truncated = false;
    int i = cache_find(cache_hash(url));
    if (i >= 0) {
        g_cache[i].stamp = ++g_cache_clock;
        if (++g_cache_touches >= CACHE_TOUCH_SAVE) g_cache_index_dirty = true;
    }
    return true;
}

/* Store a 200 response just received (g_resp holds its headers). */
__attribute__((noinline)) static void cache_store(const char *url, const char *content_type,
                                                  const char *body, size_t len) {
    static char path[64];
    if (!g_cache_ready || !url || !body || g_resp.no_store) return;
    if (!g_resp.etag[0] && !g_resp.last_modified[0] && g_resp.max_age <= 0 &&
        !(g_resp.expires > 0 && g_resp.date > 0 && g_resp.expires > g_resp.date))
        return;                                /* nothing to validate it with */
    if (strlen(url) >= URL_MAX || len > IMAGE_DOWNLOAD_MAX) return;

    long max_age = -1;
    if (!g_resp.no_cache) {
        if (g_resp.max_age >= 0) max_age = g_resp.max_age;
        else if (g_resp.expires > 0 && g_resp.date > 0 && g_resp.expires > g_resp.date)
            max_age = (long)(g_resp.expires - g_resp.date);
    }
    long long stored = clock_now();

    uint32_t h = cache_hash(url);
    int old = cache_find(h);
    if (old >= 0) cache_drop(old, true);

    /* Room for it: oldest entries go first. */
    long need = cache_clusters((uint32_t)(len + strlen(url) + 512));
    while (g_cache_count > 0 && (cache_total() + need > CACHE_BUDGET || g_cache_count >= CACHE_MAX_ENTRIES)) {
        int oldest = 0;
        for (int i = 1; i < g_cache_count; i++)
            if (g_cache[i].stamp < g_cache[oldest].stamp) oldest = i;
        cache_drop(oldest, true);
    }
    if (need > CACHE_BUDGET) return;

    cache_path(h, path, sizeof path);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    int hn = fprintf(f, "MBC1\n%s\n%s\n%s\n%s\n%lld\n%ld\n%lu\n\n", url, g_resp.etag,
                     g_resp.last_modified, content_type ? content_type : "",
                     stored, max_age, (unsigned long)len);
    bool ok = hn > 0 && fwrite(body, 1, len, f) == len;
    if (fclose(f) != 0) ok = false;
    if (!ok) {                                 /* flash full? */
        remove(path);
        printf("[mini_browser] cache: could not write %s\n", url);
        return;
    }
    g_cache[g_cache_count++] = (cache_entry_t){ h, (uint32_t)(hn + len), ++g_cache_clock };
    g_cache_index_dirty = true;
    printf("[mini_browser] cache: stored %s (%u bytes, %ld KB used, %d entries)\n",
           url, (unsigned)len, cache_total() / 1024, g_cache_count);
}

/* Request headers that ask "only send it when it changed". */
static struct curl_slist *cache_conditional_headers(struct curl_slist *hdrs, const cache_meta_t *meta) {
    static char line1[128], line2[80];
    if (meta->etag[0]) {
        snprintf(line1, sizeof line1, "If-None-Match: %s", meta->etag);
        hdrs = curl_slist_append(hdrs, line1);
    }
    if (meta->last_modified[0]) {
        snprintf(line2, sizeof line2, "If-Modified-Since: %s", meta->last_modified);
        hdrs = curl_slist_append(hdrs, line2);
    }
    return hdrs;
}

static void cache_clear_all(void) {
    int n = g_cache_count;
    while (g_cache_count > 0) cache_drop(0, true);
    cache_save_index();
    printf("[mini_browser] cache: cleared %d entries\n", n);
}

/* ---------- curl fetch (tolerant to trimmed-down libcurl) ---------- */
/* ---------- v1.2: proper HTTP status/error handling ---------- */

#define MAX_REDIRECTS 5

/*
 * Note: BadgeVMS ships a small curl emulation on top of esp_http_client.
 * Only the options listed in its curl.h exist and the CURLOPT_* names are
 * enum values (so "#ifdef CURLOPT_X" is always false).
 *
 * 4.3: BadgeVMS 4.3 firmware adds what a browser needs: the write callback
 * can stop a transfer, CURLOPT_XFERINFOFUNCTION reports progress (and stops
 * on request), gzip/deflate bodies are decoded (CURLOPT_ACCEPT_ENCODING) and
 * a handle keeps its connection open for the next request to the same host.
 * The browser therefore uses one curl handle for everything.  On older
 * firmware these options are refused; the browser then works as before,
 * with a new handle per request (the old firmware also sends every cookie
 * it ever saw on a reused handle, so reusing one there would be wrong).
 */
static CURL *g_net_curl;          /* the shared handle (4.3 firmware) */
static bool  g_net_modern;        /* firmware supports the 4.3 options */
static bool  g_net_probed;

/* Loading line / stop key state for the transfer in progress. */
typedef struct {
    struct browser_s *b;          /* NULL: no loading line (background work) */
    bool stopped;                 /* Esc pressed: the transfer was stopped */
    bool images;                  /* loading the images of the new page */
    const char *download_name;    /* 4.3: saving a download */
    bool background;              /* 4.3: image loaded while the page is in use */
    bool yielded;                 /* 4.3: interrupted by a key, try again later */
    int image_index, image_count;
    Uint64 next_draw;
} load_state_t;
static load_state_t g_load;

static int load_progress_cb(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                            curl_off_t ultotal, curl_off_t ulnow);

/* Images: only the cache and clock headers (no cookies, no page metadata). */
static size_t image_header_cb(char *buffer, size_t size, size_t nitems, void *userdata) {
    (void)userdata;
    resp_parse_header(buffer, size * nitems);
    return size * nitems;
}

/* A curl handle for one request.  With 4.3 firmware it is the shared,
 * kept-alive handle; release it with net_release(). */
static CURL *net_acquire(void) {
    if (g_net_modern && g_net_curl) return g_net_curl;
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;
    if (!g_net_probed) {
        g_net_probed = true;
        g_net_modern = curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, load_progress_cb) == CURLE_OK;
        printf("[mini_browser] net: %s\n", g_net_modern
               ? "progress, stop, gzip and keep-alive available"
               : "old firmware curl: no progress/stop/gzip/keep-alive");
        if (g_net_modern) g_net_curl = curl;
    }
    return curl;
}

static void net_release(CURL *curl) {
    if (!curl) return;
    /* The slist given with CURLOPT_HTTPHEADER is freed by the caller. */
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, NULL);
    if (curl != g_net_curl) curl_easy_cleanup(curl);
}

static void net_shutdown(void) {
    if (g_net_curl) curl_easy_cleanup(g_net_curl);
    g_net_curl = NULL;
}

/* Progress, stop key and (for pages) decoding: the same on every request. */
static void net_common_options(CURL *curl, bool decode, struct browser_s *b) {
    if (!g_net_modern) return;
    /* The browser keeps its own cookies (per host and path, with expiry):
     * curl's cookie store on the shared handle must not add any. */
    curl_easy_setopt(curl, CURLOPT_COOKIELIST, "ALL");
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, load_progress_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, (void *)b);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, decode ? "gzip, deflate" : NULL);
}

/* Last transfer: compressed bytes on the wire (0 = unknown). */
static double g_net_wire_bytes;

static int fetch_one(const char *url, const char *post_body, mem_t *m, long *http_status) {
    /* 4.3: a copy on flash that is still fresh needs no request at all. */
    static cache_meta_t cached;
    bool have_cached = !post_body && cache_open(url, &cached, NULL);
    resp_reset();
    if (have_cached && cache_is_fresh(&cached) && cache_read_body(url, &cached, m)) {
        if (http_status) *http_status = 200;
        snprintf(g_fetch_meta.content_type, sizeof g_fetch_meta.content_type, "%s", cached.content_type);
        g_fetch_meta.source = FETCH_FROM_CACHE;
        g_net_wire_bytes = 0;
        printf("[mini_browser] cache: fresh %s (%u bytes)\n", url, (unsigned)m->len);
        return 0;
    }

    CURL *curl = net_acquire();
    if (!curl) return -2;

    curl_easy_setopt(curl, CURLOPT_URL, url);

    if (post_body) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_body);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(post_body));
    } else {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);   /* the shared handle may have POSTed */
    }

    /*
     * Redirects are followed by fetch_url(), one hop per request, so that
     * cookies are stored for - and sent to - the host of each hop only.
     */
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 35L);
    net_common_options(curl, true, g_load.b);

    /* Explicit request headers. */
    struct curl_slist *hdrs = NULL;

    hdrs = curl_slist_append(hdrs,
        "User-Agent: Mozilla/5.0 (BadgeVMS; ESP32; rv:" MINI_BROWSER_VERSION ") "
        "Gecko/20100101 "
        "(compatible; MiniBrowser/" MINI_BROWSER_VERSION "; +https://github.com/mactjaap/mini_browser/; HTTP/1.1)");

    hdrs = curl_slist_append(hdrs,
        "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8");

    hdrs = curl_slist_append(hdrs,
        "Accept-Language: en-US,en;q=0.5");

    /* With 4.3 firmware curl sends "Accept-Encoding: gzip, deflate" and
     * decodes the body; older firmware cannot, so ask for plain bytes. */
    if (!g_net_modern)
        hdrs = curl_slist_append(hdrs, "Accept-Encoding: identity");

    if (post_body) {
        hdrs = curl_slist_append(hdrs,
            "Content-Type: application/x-www-form-urlencoded");
    }
    if (have_cached) hdrs = cache_conditional_headers(hdrs, &cached);

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

#if MB_LOG_SENSITIVE
        printf("[mini_browser] cookie send: %s\n", g_cookie_header_value);
#else
        printf("[mini_browser] cookie send: %d cookie(s)\n", g_fetch_meta.cookies_sent);
#endif
    }

    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

    snprintf(g_cookie_request_url, sizeof(g_cookie_request_url), "%s", url);
    g_redirect_location[0] = 0;
    g_redirect_location_too_long = false;
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, cookie_header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, NULL);

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, wr_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, m);

    g_net_active = curl;
    g_fetch_is_page = true;
    CURLcode res = curl_easy_perform(curl);
    g_fetch_is_page = false;
    g_net_active = NULL;
    cookie_resolve_pending();

    /*
     * Even when the HTTP server returns 404/500, curl itself can still
     * return CURLE_OK. Therefore keep the HTTP status separately.
     */
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);

    char *ctype = NULL;
    if (curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &ctype) == CURLE_OK &&
        ctype && ctype[0])
        snprintf(g_fetch_meta.content_type, sizeof(g_fetch_meta.content_type), "%s", ctype);

    g_net_wire_bytes = 0;
    if (g_net_modern) {
        double wire = 0;
        if (curl_easy_getinfo(curl, CURLINFO_SIZE_DOWNLOAD, &wire) == CURLE_OK)
            g_net_wire_bytes = wire;
    }

    /* CURLOPT_HTTPHEADER does not take ownership of the curl_slist. */
    net_release(curl);
    if (hdrs) curl_slist_free_all(hdrs);

    /* A file to download: the transfer was stopped on purpose. */
    if (g_resp.download) {
        mem_reset(m);
        if (http_status) *http_status = code;
        return 0;
    }

    /* A body cut at our size cap is still a usable (partial) page, and so
     * is the part received before the stop key was pressed. */
    if (res == CURLE_WRITE_ERROR && m->truncated) res = CURLE_OK;
    if (res == CURLE_ABORTED_BY_CALLBACK && g_load.stopped && m->len > 0) {
        m->truncated = true;
        res = CURLE_OK;
    }

    /* 4.3: "304 Not Modified": the copy on flash is still right. */
    if (res == CURLE_OK && code == 304 && have_cached) {
        if (cache_read_body(url, &cached, m)) {
            code = 200;
            snprintf(g_fetch_meta.content_type, sizeof g_fetch_meta.content_type, "%s", cached.content_type);
            g_fetch_meta.source = FETCH_FROM_CACHE_304;
            printf("[mini_browser] cache: 304 not modified %s (%u bytes)\n", url, (unsigned)m->len);
        }
    } else if (res == CURLE_OK && code == 200 && !post_body && !m->truncated && m->len > 0) {
        cache_store(url, g_fetch_meta.content_type, m->buf, m->len);
    }
    if (http_status) *http_status = code;
    return (res == CURLE_OK) ? 0 : (int)res;
}

static bool http_status_is_redirect(long status) {
    return status == 301 || status == 302 || status == 303 ||
           status == 307 || status == 308;
}

__attribute__((noinline)) static int fetch_url(const char *url, const char *post_body, mem_t *m, long *http_status) {
    if (!url || !m) return -1;
    m->buf = NULL;
    m->len = 0;
    m->cap = 0;
    m->limit = 0;
    m->truncated = false;

    memset(&g_fetch_meta, 0, sizeof(g_fetch_meta));
    snprintf(g_fetch_meta.effective_url, sizeof(g_fetch_meta.effective_url), "%s", url);
    snprintf(g_fetch_meta.request_method, sizeof(g_fetch_meta.request_method),
             "%s", post_body ? "POST" : "GET");
    g_fetch_meta.request_body_bytes = post_body ? strlen(post_body) : 0;

    if (http_status) *http_status = 0;

    static char hop[URL_MAX];   /* static: keep the stack small under TLS */
    snprintf(hop, sizeof(hop), "%s", url);
    const char *body = post_body;
    long status = 0;
    int rc = 0;

    for (int redirects = 0; ; redirects++) {
        mem_reset(m);
        m->limit = 0;
        g_fetch_meta.content_type[0] = 0;
        g_fetch_meta.cookies_sent = 0;
        status = 0;

        rc = fetch_one(hop, body, m, &status);
        if (rc != 0 || g_load.stopped || !http_status_is_redirect(status) || !g_redirect_location[0])
            break;

        static char next[URL_MAX];
        if (redirects >= MAX_REDIRECTS ||
            !resolve_url(hop, g_redirect_location, next, sizeof(next)) ||
            !is_http_scheme(next)) {
            printf("[mini_browser] redirect not followed: %s\n", g_redirect_location);
            break;
        }

        /* Browsers turn POST into GET on 301/302/303; 307/308 keep it. */
        if (status == 301 || status == 302 || status == 303) body = NULL;

        printf("[mini_browser] redirect %ld -> %s\n", status, next);
        snprintf(hop, sizeof(hop), "%s", next);
        g_fetch_meta.redirect_count = redirects + 1;
    }

    if (http_status) *http_status = status;
    snprintf(g_fetch_meta.effective_url, sizeof(g_fetch_meta.effective_url), "%s", hop);
    g_fetch_meta.downloaded_bytes = m->len;
    if (m->truncated)
        printf("[mini_browser] download truncated at %u bytes\n", (unsigned)m->len);
    return rc;
}

/* Renderer functions used by the image viewer are defined later. */
static void draw_text(SDL_Renderer *r, int x, int y, const char *s, int max_w);
static void draw_ui(SDL_Renderer *r, const char *bar_text);

/* ---------- Phase 3 Candidate 3 bounded image subsystem ---------- */

/*
 * A decoded image is kept as RGB565 pixels (in its shrunk stb allocation)
 * until it is first drawn.  It is then uploaded once into an SDL texture
 * and the pixel buffer is released: every later frame is a single
 * SDL_RenderTexture call instead of one SDL call per pixel.
 */
typedef struct {
    uint16_t *rgb565;        /* stb allocation; free with stbi_image_free() */
    SDL_Texture *texture;    /* created on first draw */
    int width;
    int height;
    bool loaded;
    char url[URL_MAX];
} decoded_image_t;

static decoded_image_t g_inline_images[MAX_INLINE_IMAGES];
static decoded_image_t g_viewer_image;

static void decoded_image_release(decoded_image_t *image) {
    if (!image) return;
    if (image->texture) SDL_DestroyTexture(image->texture);
    stbi_image_free(image->rgb565);
    memset(image, 0, sizeof(*image));
}

static void image_release_all(void) {
    for (int i = 0; i < MAX_INLINE_IMAGES; i++)
        decoded_image_release(&g_inline_images[i]);
    decoded_image_release(&g_viewer_image);
}

/* Image fetch is separate from fetch_url(): it must not replace page metadata. */
static int fetch_image_bytes(const char *url, mem_t *m, long *http_status) {
    if (!url || !m) return -1;

    m->buf = NULL;
    m->len = 0;
    m->cap = 0;
    m->limit = IMAGE_DOWNLOAD_MAX;
    m->truncated = false;
    if (http_status) *http_status = 0;

    /* Image URLs come from untrusted HTML: only ever fetch http(s). */
    if (!is_http_scheme(url)) return (int)CURLE_UNSUPPORTED_PROTOCOL;

    if (g_load.stopped) return (int)CURLE_ABORTED_BY_CALLBACK;   /* Esc: no more images */

    /* 4.3: images are what the disk cache saves most on. */
    static cache_meta_t cached;
    bool have_cached = cache_open(url, &cached, NULL);
    resp_reset();
    if (have_cached && cache_is_fresh(&cached) && cache_read_body(url, &cached, m)) {
        if (http_status) *http_status = 200;
        printf("[mini_browser] cache: fresh %s (%u bytes)\n", url, (unsigned)m->len);
        return 0;
    }

    CURL *curl = net_acquire();
    if (!curl) return -2;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 25L);
    net_common_options(curl, false, g_load.b);   /* images are compressed already */

    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs,
        "User-Agent: MiniBrowser/" MINI_BROWSER_VERSION " BadgeVMS Phase3");
    hdrs = curl_slist_append(hdrs,
        "Accept: image/jpeg,image/png,image/gif,image/*;q=0.5,*/*;q=0.1");
    hdrs = curl_slist_append(hdrs, "Accept-Encoding: identity");
    if (have_cached) hdrs = cache_conditional_headers(hdrs, &cached);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

    /* Image responses must not set cookies or replace page metadata. */
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, image_header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, NULL);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, wr_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, m);

    CURLcode res = curl_easy_perform(curl);

    if (http_status) {
        long code = 0;
        if (curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code) == CURLE_OK)
            *http_status = code;
    }

    net_release(curl);
    if (hdrs) curl_slist_free_all(hdrs);

    /* A compressed image over the cap cannot be decoded from its first
     * half; reject it instead of handing a cut-off file to stb. */
    if (m->truncated) {
        printf("[mini_browser] image: download larger than %u bytes\n",
               (unsigned)IMAGE_DOWNLOAD_MAX);
        mem_reset(m);
        return (int)CURLE_FILESIZE_EXCEEDED;
    }

    if (res != CURLE_OK) {
        mem_reset(m);
        return (int)res;
    }

    long code = http_status ? *http_status : 0;
    if (code == 304 && have_cached) {
        if (cache_read_body(url, &cached, m)) {
            *http_status = 200;
            printf("[mini_browser] cache: 304 not modified %s (%u bytes)\n", url, (unsigned)m->len);
        }
    } else if (code == 200 && m->len > 0) {
        cache_store(url, "", m->buf, m->len);
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
    int is_gif = len >= 6 &&
                 (!memcmp(buf, "GIF87a", 6) || !memcmp(buf, "GIF89a", 6));
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

    if (is_gif)
        printf("[mini_browser] image: GIF detected; rendering first frame only\n");

    /* The decoder's working memory is capped by mb_stbi_malloc(); an image
     * that needs more fails here with "outofmem" instead of exhausting the
     * heap. */
    unsigned char *rgb = NULL;
    if (is_gif) {
        rgb = mb_stbi_load_gif_first_frame_from_memory(
            buf, (int)len, &w, &h, &channels, 3);
    } else {
        rgb = stbi_load_from_memory(buf, (int)len, &w, &h, &channels, 3);
    }
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
     * is still alive. Downscale/convert forward into the beginning of stb's
     * own RGB888 allocation. This is overlap-safe:
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
     * This is a shrink, not a second image allocation.  It must go through
     * the same bounded allocator stb used (STBI_REALLOC).
     */
    void *shrunk = STBI_REALLOC(rgb, retained_bytes);
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

/* Decode an already downloaded image (direct image URL or <img> fetch). */
static int decode_image_buffer(const char *url, const unsigned char *buf, size_t len,
                               int target_max_w, int target_max_h,
                               decoded_image_t *out) {
    int ok = decode_web_image_bounded(buf, len, target_max_w, target_max_h, out);
    if (ok) {
        snprintf(out->url, sizeof(out->url), "%s", url ? url : "");
        printf("[mini_browser] image: loaded compressed=%u bytes\n", (unsigned)len);
    } else {
        printf("[mini_browser] image: unsupported, invalid, or outside memory budget\n");
    }
    return ok;
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

    int ok = decode_image_buffer(url, (const unsigned char*)m.buf, m.len,
                                 target_max_w, target_max_h, out);
    free(m.buf);
    return ok;
}

static void image_draw_size(const page_image_t *spec,
                            const decoded_image_t *image,
                            int max_w, int max_h,
                            int *out_w, int *out_h) {
    long long src_w = image->width > 0 ? image->width : 1;
    long long src_h = image->height > 0 ? image->height : 1;
    long long draw_w = src_w;
    long long draw_h = src_h;
    long long req_w = spec ? spec->requested_width : 0;
    long long req_h = spec ? spec->requested_height : 0;

    /* 64-bit arithmetic: width/height come from page HTML. */
    if (req_w > 0) {
        draw_w = req_w;
        draw_h = (src_h * draw_w + src_w / 2) / src_w;
    } else if (req_h > 0) {
        draw_h = req_h;
        draw_w = (src_w * draw_h + src_h / 2) / src_h;
    }

    if (req_w > 0 && req_h > 0) {
        long long h_by_w = (src_h * req_w + src_w / 2) / src_w;
        long long w_by_h = (src_w * req_h + src_h / 2) / src_h;
        if (h_by_w <= req_h) {
            draw_w = req_w;
            draw_h = h_by_w;
        } else {
            draw_h = req_h;
            draw_w = w_by_h;
        }
    }

    if (draw_w > 4096) draw_w = 4096;
    if (draw_h > 4096) draw_h = 4096;
    if (draw_w < 1) draw_w = 1;
    if (draw_h < 1) draw_h = 1;
    int fw = 0, fh = 0;
    fit_image_size((int)draw_w, (int)draw_h, max_w, max_h, &fw, &fh);
    *out_w = fw;
    *out_h = fh;
}

/* Upload the RGB565 pixels into a texture once; keep the pixels if the
 * renderer cannot create one (fallback path below). */
static SDL_Texture *decoded_image_texture(SDL_Renderer *r, decoded_image_t *image) {
    if (image->texture) return image->texture;
    if (!image->rgb565) return NULL;

    SDL_Texture *tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGB565,
                                         SDL_TEXTUREACCESS_STATIC,
                                         image->width, image->height);
    if (!tex) return NULL;
    if (!SDL_UpdateTexture(tex, NULL, image->rgb565, image->width * (int)sizeof(uint16_t))) {
        SDL_DestroyTexture(tex);
        return NULL;
    }
    /* Same nearest-neighbour look as the old per-pixel scaler. */
    SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
    image->texture = tex;
    stbi_image_free(image->rgb565);   /* the texture holds the pixels now */
    image->rgb565 = NULL;
    return tex;
}

static int draw_decoded_image(SDL_Renderer *r, const page_image_t *spec,
                              decoded_image_t *image,
                              int x, int y, int max_w, int max_h) {
    if (!r || !image || !image->loaded)
        return 0;

    int draw_w = 0, draw_h = 0;
    image_draw_size(spec, image, max_w, max_h, &draw_w, &draw_h);

    SDL_Texture *tex = decoded_image_texture(r, image);
    if (tex) {
        SDL_FRect dst = { (float)x, (float)y, (float)draw_w, (float)draw_h };
        SDL_RenderTexture(r, tex, NULL, &dst);
        return draw_h;
    }
    if (!image->rgb565)
        return 0;

    /* Fallback: per-pixel drawing, as before, if textures are unavailable. */
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

/*
 * Phase 3 GIF Test4:
 * Accept [[MBIMGn]] when everything before and after it has zero rendered
 * width. Real-world HTML can leave invisible color/style/background state
 * markers around the image marker. debug_utf8() hides those state markers,
 * so CONTENT may look exactly like "[[MBIMG0]]" even though the old strict
 * byte-for-byte matcher rejected the line.
 */
static int image_marker_side_is_zero_width(const char *s, size_t len) {
    size_t i = 0;
    while (i < len) {
        unsigned cp = utf8_next(s, len, &i);
        if (cp == 0) break;
        if (cp == '\r' || cp == ' ' || cp == 0xA0) continue;
        if (wrap_glyph_width(cp) != 0) return 0;
    }
    return 1;
}

static int is_image_marker_line(const char *line, int len, int *index) {
    if (!line || len < 11) return 0;

    /* Only the parser writes TEXT_IMAGE_MARK, so "[[MBIMG0]]" typed in a
     * page (or in a URL shown on an error page) is plain text. */
    static const char prefix[] = "\x10[[MBIMG";
    const size_t prefix_len = sizeof(prefix) - 1;
    const char *end = line + len;

    for (const char *p = line; p + prefix_len + 3 <= end; p++) {
        if (memcmp(p, prefix, prefix_len) != 0) continue;

        const char *q = p + prefix_len;
        int n = 0, digits = 0;
        while (q < end && *q >= '0' && *q <= '9' && digits < 3) {
            n = n * 10 + (*q - '0');
            q++;
            digits++;
        }

        if (digits == 0 || n >= MAX_INLINE_IMAGES) continue;
        if (q + 2 > end || q[0] != ']' || q[1] != ']') continue;
        q += 2;

        if (!image_marker_side_is_zero_width(line, (size_t)(p - line))) continue;
        if (!image_marker_side_is_zero_width(q, (size_t)(end - q))) continue;

        if (index) *index = n;
        return 1;
    }
    return 0;
}

static int content_type_is_image(const char *content_type) {
    if (!content_type) return 0;
    return !strncasecmp(content_type, "image/jpeg", 10) ||
           !strncasecmp(content_type, "image/jpg", 9) ||
           !strncasecmp(content_type, "image/png", 9) ||
           !strncasecmp(content_type, "image/gif", 9);
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

    /* GIF87a / GIF89a signature. Animated GIFs intentionally render only
     * the first frame through the normal bounded stb_image decode path. */
    if (len >= 6 &&
        (!memcmp(buf, "GIF87a", 6) || !memcmp(buf, "GIF89a", 6)))
        return 1;

    return 0;
}

static void draw_image_viewer(SDL_Renderer *r, decoded_image_t *image) {
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

/*
 * Direct-mapped glyph cache.  512 entries (~19 KiB) hold a full screen of
 * CJK text; with 128 entries a CJK page evicted its own glyphs every frame.
 * Misses are cached too (present == false), so codepoints the font lacks
 * do not cost an fseek/fread on every redraw.
 */
#define UNICODE_CACHE_SIZE 512

typedef struct {
    uint32_t codepoint;
    unsigned char bitmap[UNICODE_GLYPH_BYTES];
    bool valid;
    bool present;
} unicode_cache_entry_t;

static FILE *g_unicode_font = NULL;
static bool g_unicode_font_failed = false;   /* open/validation failed: do not retry */
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

static void unicode_font_fail(const char *why, unsigned long detail) {
    printf("Mini Browser: %s %lu (%s)\n", why, detail, UNICODE_FONT_FILE);
    if (g_unicode_font) fclose(g_unicode_font);
    g_unicode_font = NULL;
    g_unicode_range_count = 0;
    /* Remember the failure: without this every non-ASCII glyph on every
     * frame re-opened the file and printed this message again. */
    g_unicode_font_failed = true;
}

static int unicode_font_open(void) {
    if (g_unicode_font) return 1;
    if (g_unicode_font_failed) return 0;

    g_unicode_font = fopen(UNICODE_FONT_FILE, "rb");
    if (!g_unicode_font) {
        unicode_font_fail("FAILED to open Unicode font", 0);
        return 0;
    }

    /* File size, so every range can be checked against it once here
     * instead of failing on each glyph read later. */
    long file_size = -1;
    if (fseek(g_unicode_font, 0, SEEK_END) == 0) file_size = ftell(g_unicode_font);
    if (file_size < 12 || fseek(g_unicode_font, 0, SEEK_SET) != 0) {
        unicode_font_fail("Unicode font size check failed", (unsigned long)(file_size < 0 ? 0 : file_size));
        return 0;
    }

    unsigned char header[12];
    if (fread(header, 1, sizeof(header), g_unicode_font) != sizeof(header)) {
        unicode_font_fail("Unicode font header read failed", 0);
        return 0;
    }
    if (memcmp(header, "MBCJ", 4) != 0) {
        unicode_font_fail("invalid Unicode font magic", 0);
        return 0;
    }

    uint32_t version = unicode_read_le32(header + 4);
    uint32_t range_count = unicode_read_le32(header + 8);

    if (version != UNICODE_FONT_FORMAT_VERSION) {
        unicode_font_fail("unsupported Unicode font format", version);
        return 0;
    }
    if (range_count == 0 || range_count > UNICODE_MAX_RANGES) {
        unicode_font_fail("invalid Unicode range count", range_count);
        return 0;
    }

    uint32_t previous_end = 0;
    uint64_t table_end = 12U + (uint64_t)range_count * 12U;

    for (uint32_t i = 0; i < range_count; i++) {
        unsigned char record[12];

        if (fread(record, 1, sizeof(record), g_unicode_font) != sizeof(record)) {
            unicode_font_fail("Unicode range table read failed at record", i);
            return 0;
        }

        unicode_font_range_t *range = &g_unicode_ranges[i];
        range->start = unicode_read_le32(record + 0);
        range->end = unicode_read_le32(record + 4);
        range->offset = unicode_read_le32(record + 8);

        uint64_t span = ((uint64_t)range->end - range->start + 1U) * UNICODE_GLYPH_BYTES;
        if (range->start > range->end ||
            range->end > 0x10FFFFu ||
            range->offset < table_end ||
            (uint64_t)range->offset + span > (uint64_t)file_size ||
            (uint64_t)range->offset + span > (uint64_t)LONG_MAX ||
            (i > 0 && range->start <= previous_end)) {
            unicode_font_fail("invalid Unicode range record", i);
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
            /* Validated at open time: offset + span fits in the file and in long. */
            *offset = (long)((uint64_t)range->offset +
                             (uint64_t)(cp - range->start) * UNICODE_GLYPH_BYTES);
            return 1;
        }
    }

    return 0;
}

/* Returns a pointer to the cached 16x16 bitmap, or NULL when the font has
 * no glyph for cp.  The pointer stays valid until the next lookup. */
static const unsigned char *load_unicode_glyph(unsigned cp) {
    unicode_cache_entry_t *cached = &g_unicode_cache[cp % UNICODE_CACHE_SIZE];

    if (cached->valid && cached->codepoint == cp)
        return cached->present ? cached->bitmap : NULL;

    long offset;
    bool present = false;

    if (unicode_font_offset(cp, &offset) &&
        fseek(g_unicode_font, offset, SEEK_SET) == 0 &&
        fread(cached->bitmap, 1, UNICODE_GLYPH_BYTES, g_unicode_font) == UNICODE_GLYPH_BYTES) {
        /* An all-zero slot means that the font does not contain this glyph. */
        for (int i = 0; i < UNICODE_GLYPH_BYTES; i++) {
            if (cached->bitmap[i] != 0) {
                present = true;
                break;
            }
        }
    }

    /* While the font is unavailable nothing is cached, so a font that
     * appears later (fresh app start) is not masked by stale misses. */
    if (!g_unicode_font) return NULL;

    cached->codepoint = cp;
    cached->valid = true;
    cached->present = present;
    return present ? cached->bitmap : NULL;
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



static void debug_utf8(const char *label, const char *s) {
    if (!s) {
        printf("[UTF8] %s: NULL\n", label);
        return;
    }

    size_t len = strlen(s);
    size_t i = 0;
    unsigned errors = 0;

    while (i < len) {
        /* utf8_next() returns U+FFFD and advances one byte for an invalid
         * sequence (a real U+FFFD in the text is three bytes). */
        size_t start = i;
        unsigned cp = utf8_next(s, len, &i);
        if (cp != 0xFFFD || i - start != 1)
            continue;
        i = start;

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



/*
 * Glyph drawing.  Each glyph is turned into a handful of rectangles (runs of
 * set pixels are merged) and submitted with ONE SDL_RenderFillRects call.
 * Drawing every set pixel with its own SDL_RenderFillRect was ~100k renderer
 * calls per frame for a screen of CJK text.
 */
#define GLYPH_RECT_MAX 160

static void draw_char_ex(SDL_Renderer *r, int x, int y, char c, bool italic) {
    if (!r) return;
    if ((unsigned char)c < 32 || (unsigned char)c > 127) c = '?';
    const unsigned char *cols = font5x7[(unsigned char)c - 32];
    SDL_FRect rects[FONT_W_COLS * FONT_H_ROWS];
    int n = 0;

    for (int col = 0; col < FONT_W_COLS; col++) {
        unsigned char bits = cols[col];
        for (int row = 0; row < FONT_H_ROWS; row++) {
            if (!(bits & (1u << row))) continue;
            if (italic) {
                /* Italic shifts every row differently: one cell per pixel. */
                int skew = (FONT_H_ROWS - 1 - row) / 3;
                rects[n++] = (SDL_FRect){ (float)(x + col * FONT_SCALE + skew),
                                          (float)(y + row * FONT_SCALE),
                                          (float)FONT_SCALE, (float)FONT_SCALE };
                continue;
            }
            /* Merge the vertical run of set rows in this column. */
            int run = row;
            while (run + 1 < FONT_H_ROWS && (bits & (1u << (run + 1)))) run++;
            rects[n++] = (SDL_FRect){ (float)(x + col * FONT_SCALE),
                                      (float)(y + row * FONT_SCALE),
                                      (float)FONT_SCALE,
                                      (float)((run - row + 1) * FONT_SCALE) };
            row = run;
        }
    }
    if (n) SDL_RenderFillRects(r, rects, n);
}

static void draw_unicode_char_ex(SDL_Renderer *r, int x, int y, unsigned cp, bool italic) {
    const unsigned char *bitmap = load_unicode_glyph(cp);
    SDL_FRect rects[GLYPH_RECT_MAX];
    int n = 0;

    if (!bitmap) {
        /*
         * Missing Unicode glyph.
         * Draw a simple 8x8 box so missing characters remain visible.
         */
        static const unsigned char box[8] = {
            0x7E, 0x42, 0x5A, 0x5A, 0x5A, 0x42, 0x7E, 0x00
        };
        for (int row = 0; row < 8; row++) {
            for (int col = 0; col < 8; col++) {
                if (!(box[row] & (1u << (7 - col)))) continue;
                int run = col;
                while (run + 1 < 8 && (box[row] & (1u << (7 - (run + 1))))) run++;
                rects[n++] = (SDL_FRect){ (float)(x + col * FONT_SCALE),
                                          (float)(y + row * FONT_SCALE),
                                          (float)((run - col + 1) * FONT_SCALE),
                                          (float)FONT_SCALE };
                col = run;
            }
        }
        if (n) SDL_RenderFillRects(r, rects, n);
        return;
    }

    /*
     * GNU Unifont CJK glyphs are 16x16 monochrome bitmaps.
     * Each row consists of two bytes, most-significant bit first.
     * Horizontal runs of set pixels become one rectangle each.
     */
    for (int row = 0; row < UNICODE_GLYPH_H; row++) {
        uint16_t bits = ((uint16_t)bitmap[row * 2] << 8) | (uint16_t)bitmap[row * 2 + 1];
        int skew = italic ? (UNICODE_GLYPH_H - 1 - row) / 5 : 0;

        for (int col = 0; col < UNICODE_GLYPH_W; col++) {
            if (!(bits & ((uint16_t)1 << (15 - col)))) continue;
            int run = col;
            while (run + 1 < UNICODE_GLYPH_W && (bits & ((uint16_t)1 << (15 - (run + 1))))) run++;
            if (n == GLYPH_RECT_MAX) {
                SDL_RenderFillRects(r, rects, n);
                n = 0;
            }
            const float f = (float)FONT_SCALE / 2.0f;     /* 4.4: zoom */
            rects[n++] = (SDL_FRect){ (float)x + (float)(col + skew) * f, (float)y + (float)row * f,
                                      (float)(run - col + 1) * f, f };
            col = run;
        }
    }
    if (n) SDL_RenderFillRects(r, rects, n);
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
    unsigned char mark;          /* 4.4: MARK_FIND, MARK_FIND_CURRENT, MARK_FOCUS */
} visual_glyph_t;

/* 4.4: highlighted byte ranges of the line being drawn (find matches,
 * the focused link or field).  Offsets are relative to that line. */
#define MARK_FIND          1
#define MARK_FIND_CURRENT  2
#define MARK_FOCUS         3
#define LINE_MARKS_MAX     24
typedef struct { size_t start, end; unsigned char kind; } line_mark_t;
static line_mark_t g_line_marks[LINE_MARKS_MAX];
static int g_line_mark_count;

static unsigned char line_mark_at(size_t offset) {
    unsigned char kind = 0;
    for (int i = 0; i < g_line_mark_count; i++)
        if (offset >= g_line_marks[i].start && offset < g_line_marks[i].end &&
            g_line_marks[i].kind > kind)
            kind = g_line_marks[i].kind;
    return kind;
}

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
static unsigned char g_bidi_right[BIDI_LINE_MAX];

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
    /* Table spans U+0621..U+06D3; most glyphs on most pages are outside. */
    if (cp < 0x0621 || cp > 0x06D3)
        return NULL;
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

    /* Resolve neutral characters from their neighbors, otherwise paragraph
     * base.  Two linear passes (nearest strong direction to the right, then
     * to the left) instead of scanning both ways for every neutral. */
    unsigned char *right_dir = g_bidi_right;
    unsigned char next_strong = DIR_NEUTRAL;
    for (int i = count - 1; i >= 0; i--) {
        right_dir[i] = next_strong;
        if (g[i].dir != DIR_NEUTRAL) next_strong = g[i].dir;
    }
    unsigned char prev_strong = DIR_NEUTRAL;
    for (int i = 0; i < count; i++) {
        if (g[i].dir != DIR_NEUTRAL) {
            prev_strong = g[i].dir;
            continue;
        }
        g[i].dir = (prev_strong != DIR_NEUTRAL && prev_strong == right_dir[i]) ? prev_strong : base;
        /* Same as before: an already resolved neutral is the left
         * neighbour of the next one. */
        prev_strong = g[i].dir;
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

/* Draws one logical line; returns the number of screen rows it used
 * (more than one when an unwrapped line is wider than max_w). */
static int draw_visual_line(SDL_Renderer *r, int x, int y,
                            visual_glyph_t *g, int count, int max_w) {
    if (count <= 0)
        return 1;

    /* Shaping and bidi only matter for lines that contain RTL text.  For
     * everything else (most pages) the result would be the identity order. */
    bool any_rtl = false;
    for (int i = 0; i < count && !any_rtl; i++)
        any_rtl = bidi_dir(g[i].cp) == DIR_RTL;

    bool base_rtl = false;
    if (any_rtl) {
        arabic_shape_line(g, count);
        count = bidi_visualize_line(g, count, &base_rtl);
    }

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
    int rows = 1;

    /* A real <hr> is represented by one zero-width internal marker. */
    for (int i = 0; i < count; i++) {
        if (g[i].cp == TEXT_HRULE) {
            SDL_SetRenderDrawColor(r, 70, 90, 100, 255);
            SDL_RenderLine(r, (float)x, (float)(cy + CH_H / 2),
                           (float)(x + max_w), (float)(cy + CH_H / 2));
            return 1;
        }
    }

    /* 4.4: focus ring around the focused link/field (one box per row). */
    int focus_x0 = -1, focus_x1 = 0, focus_y = 0;

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
            rows++;
        }

        if (g[i].mark == MARK_FOCUS) {
            if (focus_x0 >= 0 && focus_y != cy) {
                SDL_FRect ring = { (float)(focus_x0 - 2), (float)(focus_y - 2),
                                   (float)(focus_x1 - focus_x0 + 4), (float)(CH_H + 4) };
                SDL_SetRenderDrawColor(r, 0x64, 0xEF, 0xFE, 255);
                SDL_RenderRect(r, &ring);
                focus_x0 = -1;
            }
            if (focus_x0 < 0) { focus_x0 = cx; focus_y = cy; }
            focus_x1 = cx + char_w;
        }
        if (g[i].mark == MARK_FIND || g[i].mark == MARK_FIND_CURRENT) {
            if (g[i].mark == MARK_FIND_CURRENT) SDL_SetRenderDrawColor(r, 0xFF, 0x8F, 0x00, 255);
            else SDL_SetRenderDrawColor(r, 0xFF, 0xE0, 0x60, 255);
            SDL_FRect hl = { (float)cx, (float)(cy - 1), (float)char_w, (float)(CH_H + 2) };
            SDL_RenderFillRect(r, &hl);
            SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
            if (cp >= 32 && cp <= 126) draw_char_ex(r, cx, cy, (char)cp, g[i].italic);
            else if (cp >= 0x80 && cp <= 0x10FFFF) draw_unicode_char_ex(r, cx, cy, cp, g[i].italic);
            cx += char_w;
            continue;
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
            draw_char_ex(r, cx, cy, (char)cp, g[i].italic);
            if (g[i].bold)
                draw_char_ex(r, cx + 1, cy, (char)cp, g[i].italic);
        } else if (cp >= 0x80 && cp <= 0x10FFFF) {
            draw_unicode_char_ex(r, cx, cy, cp, g[i].italic);
            if (g[i].bold)
                draw_unicode_char_ex(r, cx + 1, cy, cp, g[i].italic);
        }

        if (g[i].underline && cp != ' ') {
            SDL_RenderLine(r, (float)cx, (float)(cy + CH_H + 1),
                           (float)(cx + char_w - 2), (float)(cy + CH_H + 1));
        }

        cx += char_w;
    }
    if (focus_x0 >= 0) {
        SDL_FRect ring = { (float)(focus_x0 - 2), (float)(focus_y - 2),
                           (float)(focus_x1 - focus_x0 + 4), (float)(CH_H + 4) };
        SDL_SetRenderDrawColor(r, 0x64, 0xEF, 0xFE, 255);
        SDL_RenderRect(r, &ring);
    }
    return rows;
}

static void debug_print_content_clean(const char *s) {
    if (!s) return;
    size_t i = 0, len = strlen(s);
    while (i < len) {
        size_t start = i;
        unsigned cp = utf8_next(s, len, &i);
        if (is_format_marker(cp))
            continue;
        fwrite(s + start, 1, i - start, stdout);
    }
}

/*
 * Formatting state carried by the marker stream.
 *
 * The state used to live in draw_text()'s local variables, and callers draw
 * one wrapped line per call, so a link/bold/colored span that wrapped lost
 * its formatting from its second line on (and scrolling past a block's first
 * line lost the block's color).  Callers now keep one text_state_t for the
 * whole page and pass it from line to line.
 *
 * The stacks hold TEXT_STATE_DEPTH entries.  Deeper pushes are counted in
 * *_overflow so their pops are matched, instead of popping a parent's state.
 */
#define TEXT_STATE_DEPTH 32

typedef struct { bool custom; unsigned char r, g, b; } text_rgb_t;
typedef struct {
    bool bold_set, bold, italic_set, italic, underline_set, underline;
    unsigned char align;
} text_css_t;

typedef struct {
    int bold_depth, link_depth, heading_depth, form_depth;

    text_rgb_t color, color_stack[TEXT_STATE_DEPTH];
    int color_depth, color_overflow;
    int color_nibbles;
    unsigned color_value;

    text_css_t css, style_stack[TEXT_STATE_DEPTH];
    int style_depth, style_overflow;
    int style_nibbles;
    unsigned style_value;

    text_rgb_t background, background_stack[TEXT_STATE_DEPTH];
    int background_depth, background_overflow;
    int background_nibbles;
    unsigned background_value;
} text_state_t;

static void text_state_reset(text_state_t *st) {
    memset(st, 0, sizeof(*st));
    st->color_nibbles = -1;
    st->style_nibbles = -1;
    st->background_nibbles = -1;
}

static void text_rgb_push(text_rgb_t *stack, int *depth, int *overflow, text_rgb_t value) {
    if (*depth < TEXT_STATE_DEPTH) stack[(*depth)++] = value;
    else (*overflow)++;
}

static void text_rgb_pop(text_rgb_t *stack, int *depth, int *overflow, text_rgb_t *value) {
    if (*overflow > 0) (*overflow)--;
    else if (*depth > 0) *value = stack[--(*depth)];
}

/* Applies cp to the state when it is a formatting marker.  Returns true when
 * cp was consumed (zero width), false for ordinary text. */
static bool text_state_apply(text_state_t *st, unsigned cp) {
    switch (cp) {
        case TEXT_BOLD_ON:     st->bold_depth++; return true;
        case TEXT_BOLD_OFF:    if (st->bold_depth > 0) st->bold_depth--; return true;
        case TEXT_LINK_ON:     st->link_depth++; return true;
        case TEXT_LINK_OFF:    if (st->link_depth > 0) st->link_depth--; return true;
        case TEXT_HEADING_ON:  st->heading_depth++; return true;
        case TEXT_HEADING_OFF: if (st->heading_depth > 0) st->heading_depth--; return true;
        case TEXT_FORM_ON:     st->form_depth++; return true;
        case TEXT_FORM_OFF:    if (st->form_depth > 0) st->form_depth--; return true;
        default: break;
    }

    if (cp == TEXT_COLOR_START) {
        st->color_nibbles = 0;
        st->color_value = 0;
        return true;
    }
    if (st->color_nibbles >= 0 && cp >= TEXT_COLOR_NIBBLE && cp <= TEXT_COLOR_NIBBLE + 15) {
        st->color_value = (st->color_value << 4) | (unsigned)(cp - TEXT_COLOR_NIBBLE);
        if (++st->color_nibbles == 6) {
            text_rgb_push(st->color_stack, &st->color_depth, &st->color_overflow, st->color);
            st->color.custom = true;
            st->color.r = (unsigned char)((st->color_value >> 16) & 0xFF);
            st->color.g = (unsigned char)((st->color_value >> 8) & 0xFF);
            st->color.b = (unsigned char)(st->color_value & 0xFF);
            st->color_nibbles = -1;
            st->color_value = 0;
        }
        return true;
    }
    if (cp == TEXT_COLOR_INHERIT) {
        text_rgb_push(st->color_stack, &st->color_depth, &st->color_overflow, st->color);
        st->color_nibbles = -1;
        return true;
    }
    if (cp == TEXT_COLOR_POP) {
        text_rgb_pop(st->color_stack, &st->color_depth, &st->color_overflow, &st->color);
        st->color_nibbles = -1;
        return true;
    }

    if (cp == TEXT_STYLE_START) {
        st->style_nibbles = 0;
        st->style_value = 0;
        return true;
    }
    if (st->style_nibbles >= 0 && cp >= TEXT_STYLE_NIBBLE && cp <= TEXT_STYLE_NIBBLE + 15) {
        st->style_value = (st->style_value << 4) | (unsigned)(cp - TEXT_STYLE_NIBBLE);
        if (++st->style_nibbles == 2) {
            if (st->style_depth < TEXT_STATE_DEPTH) st->style_stack[st->style_depth++] = st->css;
            else st->style_overflow++;
            unsigned char f = (unsigned char)st->style_value;
            if (f & 0x80) { st->css.bold_set = true; st->css.bold = (f & 0x40) != 0; }
            if (f & 0x20) { st->css.italic_set = true; st->css.italic = (f & 0x10) != 0; }
            if (f & 0x08) { st->css.underline_set = true; st->css.underline = (f & 0x04) != 0; }
            if (f & 0x03) st->css.align = f & 0x03;
            st->style_nibbles = -1;
            st->style_value = 0;
        }
        return true;
    }
    if (cp == TEXT_STYLE_POP) {
        if (st->style_overflow > 0) st->style_overflow--;
        else if (st->style_depth > 0) st->css = st->style_stack[--st->style_depth];
        st->style_nibbles = -1;
        return true;
    }

    if (cp == TEXT_BG_START) {
        st->background_nibbles = 0;
        st->background_value = 0;
        return true;
    }
    if (st->background_nibbles >= 0 && cp >= TEXT_BG_NIBBLE && cp <= TEXT_BG_NIBBLE + 15) {
        st->background_value = (st->background_value << 4) | (unsigned)(cp - TEXT_BG_NIBBLE);
        if (++st->background_nibbles == 6) {
            text_rgb_push(st->background_stack, &st->background_depth,
                          &st->background_overflow, st->background);
            st->background.custom = true;
            st->background.r = (unsigned char)((st->background_value >> 16) & 0xFF);
            st->background.g = (unsigned char)((st->background_value >> 8) & 0xFF);
            st->background.b = (unsigned char)(st->background_value & 0xFF);
            st->background_nibbles = -1;
            st->background_value = 0;
        }
        return true;
    }
    if (cp == TEXT_BG_INHERIT || cp == TEXT_BG_TRANSPARENT) {
        text_rgb_push(st->background_stack, &st->background_depth,
                      &st->background_overflow, st->background);
        if (cp == TEXT_BG_TRANSPARENT) st->background.custom = false;
        st->background_nibbles = -1;
        return true;
    }
    if (cp == TEXT_BG_POP) {
        text_rgb_pop(st->background_stack, &st->background_depth,
                     &st->background_overflow, &st->background);
        st->background_nibbles = -1;
        return true;
    }

    /* Any other internal marker (actions, image/pre markers) is zero-width. */
    return is_format_marker(cp);
}

/* Applies the markers of s[0..len) without drawing (used for lines that are
 * scrolled off screen, so the first visible line has the right state). */
static void text_state_advance(text_state_t *st, const char *s, size_t len) {
    size_t i = 0;
    while (i < len) {
        unsigned cp = utf8_next(s, len, &i);
        if (cp == 0) break;
        text_state_apply(st, cp);
    }
}

/*
 * X position of an inline image on an image-marker line.  st is the state
 * at the start of the line; markers on the line before the image record
 * (the enclosing block's or the <img>'s own alignment) are applied to a
 * copy, so st itself is not changed.
 */
static int image_line_x(const text_state_t *st, const char *line, int len, int draw_w) {
    static text_state_t scratch;   /* large: keep it off the small app stack */
    const int max_w = VIEW_W - 2 * PAD_LR;
    if (!st || draw_w >= max_w) return PAD_LR;

    scratch = *st;
    const char *mark = memchr(line, TEXT_IMAGE_MARK, (size_t)len);
    if (mark) text_state_advance(&scratch, line, (size_t)(mark - line));

    switch (scratch.css.align) {
        case 2:  return PAD_LR + (max_w - draw_w) / 2;
        case 3:  return PAD_LR + max_w - draw_w;
        default: return PAD_LR;
    }
}

/* Draws s (which may contain several lines).  st carries formatting state
 * across calls; NULL starts from a clean state.  Returns the height used. */
static int draw_text_ex(SDL_Renderer *r, int x, int y, const char *s, size_t L,
                        int max_w, text_state_t *st) {
    static text_state_t scratch;
    if (!r || !s) return 0;
    if (!st) {
        text_state_reset(&scratch);
        st = &scratch;
    }

    size_t i = 0;
    int cy = y;

    while (i <= L) {
        visual_glyph_t *line = g_render_line;
        int count = 0;
        bool saw_newline = false;

        while (i < L) {
            size_t glyph_at = i;
            unsigned cp = utf8_next(s, L, &i);
            if (cp == 0)
                break;
            if (cp == '\n') {
                saw_newline = true;
                break;
            }
            /* TEXT_HRULE is a marker the line renderer needs to see. */
            if (cp != TEXT_HRULE && text_state_apply(st, cp))
                continue;

            if (count < BIDI_LINE_MAX) {
                const text_css_t *css = &st->css;
                line[count].cp = cp;
                line[count].bold = css->bold_set ? css->bold : (st->bold_depth > 0 || st->heading_depth > 0);
                line[count].underline = css->underline_set ? css->underline : (st->link_depth > 0);
                line[count].italic = css->italic_set ? css->italic : false;
                line[count].align = css->align;
                line[count].color = st->heading_depth > 0 ? TEXT_COLOR_HEADING :
                                    st->link_depth > 0 ? TEXT_COLOR_LINK :
                                    st->form_depth > 0 ? TEXT_COLOR_FORM : TEXT_COLOR_NORMAL;
                line[count].custom_color = st->color.custom;
                line[count].r = st->color.r; line[count].g = st->color.g; line[count].b = st->color.b;
                line[count].custom_background = st->background.custom;
                line[count].bg_r = st->background.r;
                line[count].bg_g = st->background.g;
                line[count].bg_b = st->background.b;
                line[count].dir = DIR_NEUTRAL;
                line[count].mark = g_line_mark_count ? line_mark_at(glyph_at) : 0;
                count++;
            }
        }

        int rows = draw_visual_line(r, x, cy, line, count, max_w);
        cy += rows * (CH_H + LINE_SPACING);

        if (!saw_newline)
            break;
    }
    return cy - y;
}

static void draw_text(SDL_Renderer *r, int x, int y, const char *s, int max_w) {
    if (s) draw_text_ex(r, x, y, s, strlen(s), max_w, NULL);
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
static bool g_bar_lock;     /* 4.4: draw the lock: an https page is shown */
static void draw_bar_lock(SDL_Renderer *r);

static void draw_bar(SDL_Renderer *r, const char *text) {
    if (!text) text = "";
    int max_cols = (VIEW_W - URL_TEXT_X - PAD_LR - (g_bar_lock ? 22 : 0)) / CH_W;
    if (max_cols < 4) max_cols = 4;

    size_t n = strlen(text);
    char tmp[URL_MAX + 32];
    if ((int)n > max_cols) {
        /* Keep the end of the text; never start inside a UTF-8 sequence. */
        const char *start = text + (n - (size_t)max_cols + 3);
        while (*start && ((unsigned char)*start & 0xC0) == 0x80) start++;
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
    if (g_bar_lock) draw_bar_lock(r);

    SDL_FRect mid = (SDL_FRect){0, URLBAR_H + 1, VIEW_W, 2};
    SDL_SetRenderDrawColor(r, 60, 60, 60, 255);
    SDL_RenderFillRect(r, &mid);
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

/* Bookmark text is stored one entry per line ("url<TAB>title"), so URLs
 * and titles must not contain line or field separators. */
static void bookmark_clean_field(char *s) {
    sanitize_text_inplace(s);   /* control bytes (incl. \t \r \n) -> space */
    trim_inplace(s);
}

/* Returns 1 when added, 0 when already present or invalid, -1 when full. */
static int bookmark_add(const char *url, const char *title) {
    if (!url || !*url) return 0;

    /* Already bookmarked. */
    if (bookmark_find(url) >= 0) return 0;

    if (g_bookmark_count >= BOOKMARK_MAX) return -1;

    bookmark_t *bm = &g_bookmarks[g_bookmark_count];

    snprintf(bm->url, sizeof(bm->url), "%s", url);
    snprintf(bm->title, sizeof(bm->title), "%s", (title && *title) ? title : url);
    bookmark_clean_field(bm->url);
    bookmark_clean_field(bm->title);
    if (!bm->url[0]) return 0;

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

/* Returns 1 added, 0 removed, -1 list full (nothing changed). */
static int bookmark_toggle(const char *url, const char *title) {
    if (bookmark_find(url) >= 0) {
        bookmark_remove(url);
        return 0;
    }

    return bookmark_add(url, title) > 0 ? 1 : -1;
}

/* ---------- persistent bookmarks ---------- */
#define BOOKMARK_FILE     "APPS:[mini_browser]bookmarks.txt"
#define BOOKMARK_TMP_FILE "APPS:[mini_browser]bookmarks.tmp"

static bool bookmark_write_file(const char *path) {
    FILE *file = fopen(path, "w");
    if (!file) return false;

    bool ok = true;
    for (int i = 0; i < g_bookmark_count && ok; i++) {
        /* Format 2: one line per bookmark, "url<TAB>title". */
        if (fprintf(file, "%s\t%s\n", g_bookmarks[i].url, g_bookmarks[i].title) < 0)
            ok = false;
    }
    if (ferror(file)) ok = false;
    if (fclose(file) != 0) ok = false;
    return ok;
}

static void bookmark_save(void) {
    /*
     * Write the new list to a temporary file first and only then replace
     * the old one, so a power cut or full disk while saving cannot lose
     * every bookmark.
     *
     * BadgeVMS currently has a truncation issue with fopen(..., "w"),
     * so (like the WHY2025 name badge) remove a file before creating it.
     */
    remove(BOOKMARK_TMP_FILE);
    bool ok = bookmark_write_file(BOOKMARK_TMP_FILE);
    if (ok) {
        remove(BOOKMARK_FILE);
        if (rename(BOOKMARK_TMP_FILE, BOOKMARK_FILE) != 0) {
            /* No rename on this filesystem: fall back to a direct write. */
            remove(BOOKMARK_TMP_FILE);
            remove(BOOKMARK_FILE);
            ok = bookmark_write_file(BOOKMARK_FILE);
        }
    }

    if (!ok) {
        printf("[mini_browser] failed to save bookmarks to %s\n",
               BOOKMARK_FILE);
        return;
    }

    printf("[mini_browser] saved %d bookmarks to %s\n",
           g_bookmark_count,
           BOOKMARK_FILE);
}

/* Reads one line into buf.  A line that does not fit is consumed completely
 * and reported as too long (returns 2) instead of being split in two. */
static int bookmark_read_line(FILE *file, char *buf, size_t cap) {
    if (!fgets(buf, (int)cap, file)) return 0;
    size_t n = strcspn(buf, "\r\n");
    if (buf[n] == 0 && !feof(file)) {
        int c;
        while ((c = fgetc(file)) != '\n' && c != EOF) {}
        buf[0] = 0;
        return 2;
    }
    buf[n] = 0;
    return 1;
}

static void bookmark_load(void) {
    FILE *file = fopen(BOOKMARK_FILE, "r");

    if (!file) {
        printf("[mini_browser] no saved bookmarks at %s\n",
               BOOKMARK_FILE);
        return;
    }

    g_bookmark_count = 0;

    /* Static: two URL-sized lines would be a lot of task stack. */
    static char line[URL_MAX + 160];
    static char title[160];
    int format = 0;   /* 0 unknown, 1 old two-line format, 2 url<TAB>title */

    while (g_bookmark_count < BOOKMARK_MAX) {
        int r = bookmark_read_line(file, line, sizeof(line));
        if (!r) break;
        if (r == 2 || !line[0]) continue;   /* skip over-long / empty lines */

        char *tab = strchr(line, '\t');
        if (!format) format = tab ? 2 : 1;

        if (format == 2) {
            if (!tab) continue;
            *tab = 0;
            bookmark_add(line, tab + 1);
        } else {
            /* Old format: URL line followed by a title line.  Reading
             * whole lines keeps the pairs aligned even for long titles. */
            int rt = bookmark_read_line(file, title, sizeof(title));
            if (!rt) {
                bookmark_add(line, NULL);
                break;
            }
            bookmark_add(line, rt == 1 ? title : NULL);
        }
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
                 "Press WHY+K on a web page to add one.");

        pg->text = text;
        return pg;
    }

    for (int i = 0;
         i < g_bookmark_count && i < MAX_LINKS;
         i++) {

        bookmark_t *bm = &g_bookmarks[i];

        long href_off = page_store_string(pg, bm->url);
        if (href_off < 0) break;
        pg->links[pg->link_count].href = (uint32_t)href_off;
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

static void format_kb(char *out, size_t cap, curl_off_t bytes);

/* 4.4: security, the site's cookies, load time and sizes, at the top of
 * Page Information. */
__attribute__((noinline)) static char *page_info_extend(char *text, size_t cap, const page_t *source,
                                                        const char *url) {
    static char host[COOKIE_DOMAIN_MAX + 1], when[48], size_a[32], size_b[32];
    size_t extra_cap = 2048 + (size_t)MAX_COOKIES * 160;
    char *out = (char *)malloc(cap + extra_cap);
    if (!out) return text;
    size_t o = 0;
#define PI_APPEND(...) do { int n_ = snprintf(out + o, cap + extra_cap - o, __VA_ARGS__); \
        if (n_ > 0) o += ((size_t)n_ < cap + extra_cap - o) ? (size_t)n_ : cap + extra_cap - o - 1; } while (0)
    bool https = url && !strncasecmp(url, "https://", 8);
    bool secure_req = false;
    host[0] = 0;
    if (url && cookie_url_parts(url, &secure_req))
        snprintf(host, sizeof host, "%s", g_cookie_host);

    PI_APPEND("= PAGE INFORMATION =\n\n");
    PI_APPEND("SECURITY\n");
    if (https)
        PI_APPEND("Connection is secure (HTTPS).\nCertificate: verified for %s\nby the built-in certificate bundle.\n\n",
                  host[0] ? host : "this site");
    else
        PI_APPEND("Connection is NOT secure (HTTP):\ndon't enter passwords on this site.\n\n");

    PI_APPEND("LOADING\n");
    if (g_fetch_meta.load_ms) PI_APPEND("Load time: %u ms\n", g_fetch_meta.load_ms);
    format_kb(size_a, sizeof size_a, (curl_off_t)g_fetch_meta.downloaded_bytes);
    PI_APPEND("Page size: %s\n", size_a);
    if (g_fetch_meta.wire_bytes > 0 && (size_t)g_fetch_meta.wire_bytes != g_fetch_meta.downloaded_bytes) {
        format_kb(size_b, sizeof size_b, (curl_off_t)g_fetch_meta.wire_bytes);
        PI_APPEND("Transferred: %s (compressed)\n", size_b);
    }
    if (source && source->html) {
        format_kb(size_b, sizeof size_b, (curl_off_t)source->html_len);
        PI_APPEND("HTML: %s, %d images, %d links\n", size_b, source->image_count, source->link_count);
    }
    PI_APPEND("\nCOOKIES SET BY %s\n", host[0] ? host : "THIS SITE");
    int listed = 0;
    long long now = clock_now();
    for (int i = 0; i < MAX_COOKIES; i++) {
        const mb_cookie_t *c = &g_cookie_jar[i];
        if (!c->used || !host[0]) continue;
        bool match = c->host_only ? !strcasecmp(host, c->domain) : cookie_domain_match(host, c->domain);
        if (!match) continue;
        if (c->expires > 0 && now > 0) {
            long long days = (c->expires - now) / 86400;
            snprintf(when, sizeof when, "kept %lld more day%s", days, days == 1 ? "" : "s");
        } else if (c->expires > 0 || c->pending_age > 0) {
            snprintf(when, sizeof when, "kept after a restart");
        } else {
            snprintf(when, sizeof when, "until the browser quits");
        }
        PI_APPEND("- %s (%s%s)\n", c->name, when, c->secure ? ", secure" : "");
        listed++;
    }
    if (!listed) PI_APPEND("none\n");
    PI_APPEND("\n");
#undef PI_APPEND
    /* The original inspector text follows, then the keys. */
    size_t tl = strlen(text);
    if (o + tl + 128 < cap + extra_cap) {
        memcpy(out + o, text, tl);
        o += tl;
        o += (size_t)snprintf(out + o, cap + extra_cap - o,
                              "WHY+X clears all cookies and the disk cache.\nPress WHY+B or WHY+I to return.");
    }
    free(text);
    return out;
}

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
    const size_t cap = 2560;
    char *text = (char*)malloc(cap);
    if (!text) {
        free(pg);
        return NULL;
    }

    const char *title =
        (source && source->title[0]) ? source->title : "(none)";
    /* The serial diagnostic below keeps its historical "(not reported)"
     * wording (the regression suite matches it); the screen shows the URL
     * fetch_url() actually ended on after following redirects itself. */
    const char *effective = "(not reported)";
    const char *final_url =
        g_fetch_meta.effective_url[0] ? g_fetch_meta.effective_url : "(not reported)";
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
             "Loaded from: %s\n"
             "Content-Type: %s\n"
             "Effective URL:\n%s\n"
             "Redirects followed: %ld\n"
             "Negotiated HTTP version: not available\n\n"

             "CONNECTION\n"
             "Remote IP: not available\n"
             "Remote port: not available\n\n"

             "TLS\n"
             "TLS: %s\n"
             "Certificate details: not available\n"
             "Certificate verification result: not available\n\n"

             "COOKIE JAR\n"
             "Stored: %d / %d (%d kept after a restart)\n\n"

             "DISK CACHE\n"
             "Used: %ld KB of %ld KB, %d files\n\n"

             "LIBCURL NOTES\n"
             "Available CURLINFO: response code,\n"
             "content length.\n"
             "Content type and redirects come from\n"
             "the response headers.\n\n",
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
             g_fetch_meta.source == FETCH_FROM_CACHE ? "disk cache (still fresh)" :
             g_fetch_meta.source == FETCH_FROM_CACHE_304 ? "disk cache (checked: not modified)" :
                                                           "network",
             ctype,
             final_url,
             g_fetch_meta.redirect_count,
             is_https ? "yes" : (is_http ? "no" : "(unknown)"),
             cookie_count(),
             MAX_COOKIES,
             cookie_persistent_count(),
             cache_total() / 1024, CACHE_BUDGET / 1024, g_cache_count);

    text[cap - 1] = 0;
    pg->text = page_info_extend(text, cap, source, requested_url);

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
        g_hist_pos = HISTORY_MAX - 1;
        g_hist_len = HISTORY_MAX;
    }
}

/* Back (-1) / Forward (+1): the URL and index of that entry, or -1.  The
 * entry becomes current only when the page is shown (a stopped load leaves
 * the history as it was). */
static int history_peek(int delta, char *out) {
    int pos = g_hist_pos + delta;
    if (g_hist_pos < 0 || pos < 0 || pos >= g_hist_len) return -1;
    snprintf(out, URL_MAX, "%s", g_hist[pos]);
    return pos;
}


/* ---------- 4.1 persistent history (visited pages) ----------
 *
 * Separate from the Back/Forward stack above: this is the "History" list a
 * user browses (WHY+Y) and the source of omnibox suggestions.  Most recent
 * first, one entry per URL, saved as "url<TAB>title" lines like bookmarks.
 * Saving rewrites the file, so it happens every VISIT_SAVE_EVERY new visits
 * and on exit instead of on every page load (flash wear, load time).
 */
#define VISIT_MAX         150
#define VISIT_TITLE_MAX   96
#define VISIT_SAVE_EVERY  5
#define VISIT_FILE        "APPS:[mini_browser]history.txt"
#define VISIT_TMP_FILE    "APPS:[mini_browser]history.tmp"

typedef struct {
    char url[URL_MAX];
    char title[VISIT_TITLE_MAX];
} visit_t;

static visit_t g_visits[VISIT_MAX];   /* [0] = most recent */
static int g_visit_count = 0;
static int g_visit_unsaved = 0;
/* Set by visit_record(); the main loop saves, never the page loader: file
 * I/O deep inside browser_fetch() hung the app on the badge (small task
 * stack).  The main loop is the shallowest point of the program. */
static bool g_visit_save_due = false;

static void visit_clean_field(char *s) {
    for (; *s; s++)
        if (*s == '\t' || *s == '\r' || *s == '\n') *s = ' ';
}

static bool visit_write_file(const char *path) {
    FILE *file = fopen(path, "w");
    if (!file) return false;
    printf("[mini_browser] history: file opened\n");
    bool ok = true;
    /* fputs, not fprintf: far less stack, and nothing to format. */
    for (int i = 0; i < g_visit_count && ok; i++) {
        if (fputs(g_visits[i].url, file) < 0 || fputs("\t", file) < 0 ||
            fputs(g_visits[i].title, file) < 0 || fputs("\n", file) < 0)
            ok = false;
    }
    if (ferror(file)) ok = false;
    if (fclose(file) != 0) ok = false;
    printf("[mini_browser] history: file closed\n");
    return ok;
}

__attribute__((noinline)) static void visit_save(void) {
    /*
     * Plain rewrite (no tmp file + rename): history is not precious, and
     * every extra file operation is one more place to get stuck on the
     * badge.  Each step is logged so a hang can be located from the serial
     * log.  Like bookmark_save(), remove first: BadgeVMS fopen("w") does
     * not always truncate.
     */
    g_visit_save_due = false;
    printf("[mini_browser] history: saving %d entries\n", g_visit_count);
    remove(VISIT_TMP_FILE);   /* left over from older builds */
    remove(VISIT_FILE);
    printf("[mini_browser] history: old file removed\n");
    bool ok = visit_write_file(VISIT_FILE);
    if (ok) {
        g_visit_unsaved = 0;
        printf("[mini_browser] history: saved %d entries\n", g_visit_count);
    } else {
        printf("[mini_browser] history: failed to save %s\n", VISIT_FILE);
    }
}

__attribute__((noinline)) static void visit_load(void) {
    FILE *file = fopen(VISIT_FILE, "r");
    if (!file) {
        printf("[mini_browser] history: none saved yet\n");
        return;
    }
    static char line[URL_MAX + VISIT_TITLE_MAX + 8];
    int r;
    while (g_visit_count < VISIT_MAX && (r = bookmark_read_line(file, line, sizeof line)) != 0) {
        if (r != 1) continue;                    /* over-long line: skip it */
        char *tab = strchr(line, '\t');
        if (tab) *tab = 0;
        const char *title = tab ? tab + 1 : "";
        if (!is_http_scheme(line) || strlen(line) >= URL_MAX) continue;
        visit_t *v = &g_visits[g_visit_count++];
        snprintf(v->url, sizeof v->url, "%.*s", URL_MAX - 1, line);
        snprintf(v->title, sizeof v->title, "%s", title);
    }
    fclose(file);
    printf("[mini_browser] history: loaded %d entries\n", g_visit_count);
}

/* Move url to the top of the history (adding it when new). */
__attribute__((noinline)) static void visit_record(const char *url, const char *title) {
    if (!url || !is_http_scheme(url) || strlen(url) >= URL_MAX) return;

    int found = -1;
    for (int i = 0; i < g_visit_count; i++) {
        if (!strcmp(g_visits[i].url, url)) { found = i; break; }
    }

    static visit_t v;   /* static: keep it off the small app stack */
    snprintf(v.url, sizeof v.url, "%s", url);
    if (title && title[0])
        snprintf(v.title, sizeof v.title, "%.*s", VISIT_TITLE_MAX - 1, title);
    else if (found >= 0)
        snprintf(v.title, sizeof v.title, "%s", g_visits[found].title);
    else
        v.title[0] = 0;
    visit_clean_field(v.title);

    int shift;
    if (found >= 0) shift = found;                       /* entries above it */
    else if (g_visit_count < VISIT_MAX) shift = g_visit_count++;
    else shift = VISIT_MAX - 1;                          /* drop the oldest */
    memmove(&g_visits[1], &g_visits[0], (size_t)shift * sizeof(visit_t));
    g_visits[0] = v;

    if (++g_visit_unsaved >= VISIT_SAVE_EVERY) g_visit_save_due = true;
}

static void visit_clear(void) {
    g_visit_count = 0;
    g_visit_unsaved = 0;
    remove(VISIT_FILE);
    printf("[mini_browser] history: cleared\n");
}

__attribute__((noinline)) static page_t *history_to_page(void) {
    page_t *pg = (page_t *)calloc(1, sizeof(page_t));
    if (!pg) return NULL;
    snprintf(pg->base, URL_MAX, "history:");
    snprintf(pg->title, sizeof(pg->title), "History");

    size_t cap = 512 + (size_t)g_visit_count * (URL_MAX + VISIT_TITLE_MAX + 24);
    char *text = (char *)malloc(cap);
    if (!text) { free(pg); return NULL; }

    size_t used = 0;
    int n = snprintf(text, cap, "= HISTORY =\n\n");
    if (n > 0) used = (size_t)n;

    if (g_visit_count == 0) {
        snprintf(text + used, cap - used,
                 "No history yet.\n\nPages you visit appear here, most recent first.");
        pg->text = text;
        return pg;
    }

    n = snprintf(text + used, cap - used,
                 "%d pages, most recent first. Type a number and Enter to open.\n"
                 "WHY+Y returns to the page, WHY+X clears the history.\n\n",
                 g_visit_count);
    if (n > 0 && (size_t)n < cap - used) used += (size_t)n;

    for (int i = 0; i < g_visit_count && i < MAX_LINKS; i++) {
        visit_t *v = &g_visits[i];
        long href_off = page_store_string(pg, v->url);
        if (href_off < 0) break;
        pg->links[pg->link_count].href = (uint32_t)href_off;
        int link_index = pg->link_count++;
        add_action(pg, ACTION_LINK, link_index, -1, -1);

        n = snprintf(text + used, cap - used, "[%d] %s\n    %s\n\n",
                     i + 1, v->title[0] ? v->title : "(no title)", v->url);
        if (n < 0) break;
        if ((size_t)n >= cap - used) { used = cap - 1; break; }
        used += (size_t)n;
    }
    text[cap - 1] = 0;
    pg->text = text;
    return pg;
}

/* ---------- 4.1 omnibox: one bar for URLs and searches ---------- */
#define SEARCH_URL_PREFIX "http://www.google.com/search?q="
#define SUGGEST_MAX       5

/* Case-insensitive substring test (no strcasestr in the BadgeVMS libc). */
static bool ci_contains(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    if (!n) return true;
    for (; *hay; hay++)
        if (!strncasecmp(hay, needle, n)) return true;
    return false;
}

/* "https://example.org/x" -> "example.org/x"; other text unchanged. */
static const char *omnibox_strip_scheme(const char *s) {
    if (!strncasecmp(s, "https://", 8)) return s + 8;
    if (!strncasecmp(s, "http://", 7)) return s + 7;
    return s;
}

/* Does the text before the first '/', '?' or '#' look like a host name? */
static bool omnibox_looks_like_host(const char *s) {
    if (!*s) return false;
    for (const char *p = s; *p; p++)
        if (*p == ' ') return false;              /* words: a search */
    size_t host_len = strcspn(s, "/?#");
    if (host_len == 0) return false;
    if (!strncasecmp(s, "localhost", 9) && (host_len == 9 || s[9] == ':')) return true;

    int dots = 0;
    bool all_numeric = true, has_port = false;
    for (size_t i = 0; i < host_len; i++) {
        char c = s[i];
        if (c == '.') dots++;
        else if (c == ':') { has_port = true; break; }
        else if (!isdigit((unsigned char)c)) all_numeric = false;
    }
    if (dots == 0) return false;                  /* "esp32": a search */
    if (all_numeric && dots != 3) return false;   /* "3.14": a search */
    /* Last label must be non-empty: "foo." is not a host. */
    size_t end = host_len;
    if (has_port) end = strcspn(s, ":");
    if (end == 0 || s[end - 1] == '.' || s[0] == '.') return false;
    return true;
}

static void omnibox_url_encode(const char *in, char *out, size_t cap) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; *in && o + 4 < cap; in++) {
        unsigned char c = (unsigned char)*in;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out[o++] = (char)c;
        else if (c == ' ') out[o++] = '+';
        else { out[o++] = '%'; out[o++] = hex[c >> 4]; out[o++] = hex[c & 15]; }
    }
    out[o] = 0;
}

/*
 * Turn what was typed in the omnibox into a URL.  Returns false when there
 * is nothing to load (empty, or only the "https://" WHY+E seeds).
 * *searched tells the caller whether it became a search.
 */
__attribute__((noinline)) static bool omnibox_resolve(const char *typed, char *out, size_t cap, bool *searched) {
    static char s[URL_MAX];   /* static: 16 KB app stack */
    snprintf(s, sizeof s, "%s", typed);
    trim_inplace(s);
    *searched = false;
    if (!s[0]) return false;

    const char *rest = omnibox_strip_scheme(s);
    bool had_http = rest != s;
    if (!had_http && has_scheme(s)) {             /* another scheme: leave it */
        snprintf(out, cap, "%s", s);
        return true;
    }
    if (!*rest) return false;

    if (omnibox_looks_like_host(rest)) {
        /* Local devices (localhost, a bare IPv4 address) rarely have TLS. */
        size_t host_len = strcspn(rest, ":/?#");
        bool local = !strncasecmp(rest, "localhost", 9) && host_len == 9;
        if (!local) {
            local = true;
            for (size_t i = 0; i < host_len; i++)
                if (!isdigit((unsigned char)rest[i]) && rest[i] != '.') { local = false; break; }
        }
        int n = had_http ? snprintf(out, cap, "%s", s)
                         : snprintf(out, cap, "%s%s", local ? "http://" : "https://", rest);
        return n > 0 && (size_t)n < cap;
    }

    static char q[URL_MAX * 3];
    omnibox_url_encode(rest, q, sizeof q);
    int n = snprintf(out, cap, "%s%s", SEARCH_URL_PREFIX, q);
    if (n <= 0 || (size_t)n >= cap) return false;  /* query too long */
    *searched = true;
    return true;
}

typedef struct {
    const char *url;
    const char *title;
    bool bookmark;
} suggestion_t;

/* Fill out[] with up to SUGGEST_MAX matches: bookmarks first, then history,
 * matching the typed text (without its scheme) in the URL or the title. */
__attribute__((noinline)) static int omnibox_suggest(const char *typed, suggestion_t *out) {
    static char needle[URL_MAX];
    snprintf(needle, sizeof needle, "%s", omnibox_strip_scheme(typed));
    trim_inplace(needle);
    if (strlen(needle) < 1) return 0;

    int count = 0;
    for (int i = 0; i < g_bookmark_count && count < SUGGEST_MAX; i++) {
        const bookmark_t *bm = &g_bookmarks[i];
        if (ci_contains(omnibox_strip_scheme(bm->url), needle) || ci_contains(bm->title, needle))
            out[count++] = (suggestion_t){ bm->url, bm->title, true };
    }
    for (int i = 0; i < g_visit_count && count < SUGGEST_MAX; i++) {
        const visit_t *v = &g_visits[i];
        if (!ci_contains(omnibox_strip_scheme(v->url), needle) && !ci_contains(v->title, needle))
            continue;
        bool dup = false;
        for (int k = 0; k < count; k++)
            if (!strcmp(out[k].url, v->url)) { dup = true; break; }
        if (!dup) out[count++] = (suggestion_t){ v->url, v->title, false };
    }
    return count;
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
/* Pause after every N records instead of after each one: a 2x-2.5x faster
 * transfer, still paced for the shared console.  Set to 1 for the old rate. */
#define SCREENSHOT_PACE_EVERY    2
/* WHY+Z: longer pages are cut off (a 716-px slice takes ~minutes over serial). */
#define SCREENSHOT_MAX_HEIGHT    (20 * VIEW_H)

typedef struct {
    unsigned char raw[IMG_RAW_CHUNK];
    size_t used;
    unsigned sequence;
    unsigned long compressed_bytes;
    unsigned crc;

    unsigned char parity[IMG_RAW_CHUNK];
    unsigned parity_count;
    unsigned parity_group;

    unsigned records_since_pause;
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

static void screenshot_record_sent(screenshot_stream_t *stream) {
    if (++stream->records_since_pause >= SCREENSHOT_PACE_EVERY) {
        stream->records_since_pause = 0;
        screenshot_transport_pause();
    }
}

/* Removes queued Esc key presses (and only those) and reports them. */
static bool SDLCALL screenshot_escape_filter(void *userdata, SDL_Event *ev) {
    if (ev->type == SDL_EVENT_KEY_DOWN && ev->key.scancode == SDL_SCANCODE_ESCAPE) {
        *(bool *)userdata = true;
        return false;
    }
    return true;
}

/* Esc pressed while a screenshot is streaming?  Other keys stay queued. */
static bool screenshot_cancel_requested(void) {
    bool cancel = false;
    SDL_PumpEvents();
    SDL_FilterEvents(screenshot_escape_filter, &cancel);
    return cancel;
}

/* One terminator for every failure after IMG BEGIN, so the host receiver
 * can stop waiting immediately. */
static void screenshot_abort(const char *reason) {
    printf("IMG ABORT %s\n", reason);
    fflush(stdout);
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
    screenshot_record_sent(stream);

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
    screenshot_record_sent(stream);

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

        if (screenshot_cancel_requested()) {
            printf("IMG ERROR cancelled\n");
            fflush(stdout);
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

    /* Sent twice (like END) so one damaged line does not lose the image;
     * receivers treat a repeated identical BEGIN as a restart with no data. */
    for (int repeat = 0; repeat < 2; repeat++) {
        printf("IMG BEGIN %d %d RGB24 RLE5FEC1 %d %d\n",
               width, height, IMG_RAW_CHUNK, IMG_FEC_GROUP);
        fflush(stdout);
        screenshot_transport_pause();
    }
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
        printf("IMG ERROR begin\n");
        fflush(stdout);
        return false;
    }

    if (!screenshot_stream_capture_region(renderer, &stream, VIEW_H) ||
        !screenshot_stream_finish(&stream)) {
        screenshot_abort("capture");
        return false;
    }

    return true;
}

/*
 * Candidate 3 Fix 12a:
 * Pass the active page_t explicitly through the WHY+Z full-page screenshot
 * functions so image metadata is available without relying on a global.
 */
/*
 * Candidate 3 Fix 12:
 * Full-page screenshots must use the same vertical layout rules as the
 * normal viewport renderer. An inline image marker occupies the rendered
 * image height, not one text line.
 */
static int screenshot_full_page_height(const page_t *page,
                                       const char *content_wrapped) {
    PAGE_SCALE_BEGIN();
    int height = PAD_TOP + PAD_BOTTOM;

    if (content_wrapped && *content_wrapped) {
        const char *p = content_wrapped;

        while (p && *p) {
            const char *nl = strchr(p, '\n');
            int len = nl ? (int)(nl - p) : (int)strlen(p);
            int image_index = -1;

            if (page &&
                is_image_marker_line(p, len, &image_index) &&
                image_index >= 0 &&
                image_index < MAX_INLINE_IMAGES &&
                image_index < page->image_count &&
                g_inline_images[image_index].loaded) {
                int draw_w = 0;
                int draw_h = 0;

                image_draw_size(&page->images[image_index],
                                &g_inline_images[image_index],
                                VIEW_W - 2 * PAD_LR,
                                IMAGE_DRAW_MAX_H,
                                &draw_w,
                                &draw_h);

                height += draw_h + LINE_SPACING;
            } else {
                height += CH_H + LINE_SPACING;
            }

            p = nl ? nl + 1 : NULL;
        }
    }

    if (height < VIEW_H) {
        height = VIEW_H;
    }

    PAGE_SCALE_END();
    return height;
}

static void screenshot_render_full_page_slice(SDL_Renderer *renderer,
                                              const page_t *page,
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

    /* Formatting state runs from the top of the page, as on screen. */
    static text_state_t state;
    text_state_reset(&state);
    PAGE_SCALE_BEGIN();

    const int slice_bottom = slice_top + slice_height;
    const char *p = content_wrapped;
    int logical_y = PAD_TOP;

    while (p && *p) {
        const char *nl = strchr(p, '\n');
        int len = nl ? (int)(nl - p) : (int)strlen(p);
        int image_index = -1;

        if (page &&
            is_image_marker_line(p, len, &image_index) &&
            image_index >= 0 &&
            image_index < MAX_INLINE_IMAGES &&
            image_index < page->image_count &&
            g_inline_images[image_index].loaded) {
            int draw_w = 0;
            int draw_h = 0;

            image_draw_size(&page->images[image_index],
                            &g_inline_images[image_index],
                            VIEW_W - 2 * PAD_LR,
                            IMAGE_DRAW_MAX_H,
                            &draw_w,
                            &draw_h);

            /*
             * Render the image for every screenshot slice it intersects.
             * Coordinates outside the current 716-pixel renderer are clipped
             * by SDL, so an image crossing a slice boundary is reconstructed
             * correctly when the slices are concatenated by the receiver.
             */
            if (logical_y + draw_h > slice_top &&
                logical_y < slice_bottom) {
                draw_decoded_image(renderer,
                                   &page->images[image_index],
                                   &g_inline_images[image_index],
                                   image_line_x(&state, p, len, draw_w),
                                   logical_y - slice_top,
                                   VIEW_W - 2 * PAD_LR,
                                   IMAGE_DRAW_MAX_H);
            }

            text_state_advance(&state, p, (size_t)len);
            logical_y += draw_h + LINE_SPACING;
        } else {
            if (logical_y + CH_H > slice_top &&
                logical_y < slice_bottom) {
                SDL_SetRenderDrawColor(renderer, 220, 220, 220, 255);
                draw_text_ex(renderer,
                             PAD_LR,
                             logical_y - slice_top,
                             p, (size_t)len,
                             VIEW_W - 2 * PAD_LR,
                             &state);
            } else {
                text_state_advance(&state, p, (size_t)len);
            }

            logical_y += CH_H + LINE_SPACING;
        }

        p = nl ? nl + 1 : NULL;
    }
    PAGE_SCALE_END();
}

static bool screenshot_stream_full_page(SDL_Renderer *renderer,
                                        const page_t *page,
                                        const char *bar_text,
                                        const char *content_wrapped) {
    if (!renderer) {
        printf("IMG ERROR no-renderer\n");
        fflush(stdout);
        return false;
    }

    int full_height = screenshot_full_page_height(page, content_wrapped);
    screenshot_stream_t stream;

    printf("[mini_browser] full-page screenshot height=%d\n", full_height);
    if (full_height > SCREENSHOT_MAX_HEIGHT) {
        printf("[mini_browser] full-page screenshot limited to %d pixels\n",
               SCREENSHOT_MAX_HEIGHT);
        full_height = SCREENSHOT_MAX_HEIGHT;
    }
    fflush(stdout);

    if (!screenshot_stream_begin(&stream, VIEW_W, full_height)) {
        printf("IMG ERROR begin\n");
        fflush(stdout);
        return false;
    }

    for (int slice_top = 0; slice_top < full_height; slice_top += VIEW_H) {
        int slice_height = VIEW_H;

        if (slice_top + slice_height > full_height) {
            slice_height = full_height - slice_top;
        }

        screenshot_render_full_page_slice(renderer,
                                          page,
                                          bar_text,
                                          content_wrapped,
                                          slice_top,
                                          slice_height);

        if (!screenshot_stream_capture_region(renderer,
                                              &stream,
                                              slice_height)) {
            screenshot_abort("capture");
            return false;
        }

        YIELD_NET();
    }

    if (!screenshot_stream_finish(&stream)) {
        screenshot_abort("finish");
        return false;
    }
    return true;
}

/* ---------- main ---------- */

/*
 * Browser state.
 *
 * main() used to be one ~1200-line function with a dozen independent
 * booleans (url_editing, form_editing, link_number_mode, options_open,
 * image_viewer_open, viewing_bookmarks, viewing_page_info, ...) that could
 * contradict each other.  The state now lives in one struct with three
 * small enums, and the work is split into fetch / compose / render / input
 * functions.
 */
typedef enum {
    INPUT_NONE,          /* browsing: arrows scroll, Tab selects, digits pick */
    INPUT_URL,           /* editing the URL bar */
    INPUT_FORM,          /* editing a form field */
    INPUT_LINK_NUMBER,   /* typing a link/action number */
    INPUT_FIND           /* 4.4: typing in the find bar (WHY+F) */
} input_mode_t;

typedef enum {
    OVERLAY_NONE,
    OVERLAY_OPTIONS,     /* WHY+O display mode menu */
    OVERLAY_IMAGE,       /* image viewer */
    OVERLAY_TABS,        /* WHY+A tab overview */
    OVERLAY_DOWNLOAD,    /* 4.3: "Download this file?" */
    OVERLAY_SHARE        /* 4.4: QR code of the address (WHY+U) */
} overlay_t;

typedef enum {
    VIEW_WEB,            /* an http(s) page or a message (error) page */
    VIEW_BOOKMARKS,      /* WHY+M */
    VIEW_PAGE_INFO,      /* WHY+I */
    VIEW_HISTORY,        /* WHY+Y */
    VIEW_NEWTAB,         /* WHY+T: a new, empty tab */
    VIEW_DOWNLOADS,      /* 4.3: WHY+D */
    VIEW_READER,         /* 4.4: simplified view (WHY+V) */
    VIEW_SAVED           /* 4.4: a page saved for offline reading */
} view_kind_t;

typedef struct browser_s {
    SDL_Window *win;
    SDL_Renderer *ren;
    bool running;
    bool dirty;                 /* something changed: redraw before waiting */

    char url_buf[URL_MAX];
    char url_before_edit[URL_MAX];   /* restored when URL editing is cancelled */
    size_t url_cursor;
    char barline[URL_MAX + 64];
    char *content_wrapped;
    int content_lines;          /* number of lines in content_wrapped */
    int max_scroll;             /* last useful scroll position */
    page_t *page;
    int scroll_lines;
    bool need_fetch;
    bool history_navigation;    /* true when restoring from Back/Forward/Reload */
    bool bf_navigation;         /* Back/Forward: the page may come from the bfcache */
    int  hist_target;           /* Back/Forward: history entry to make current, or -1 */
    bool error_page;            /* the page on screen is an error page (R reloads) */
    bool page_from_post;        /* the page on screen is a POST result */
    bool page_partial;          /* the load of the page on screen was stopped */
    int sel_action;
    bool pending_post;
    char post_body[POST_BODY_MAX];
    long last_http_status;

    input_mode_t input;
    overlay_t overlay;
    view_kind_t view;
    char view_return_url[URL_MAX];   /* page to return to from bookmarks/info */

    int form_edit_form;
    int form_edit_field;
    char form_edit_buf[FORM_VALUE_MAX];
    size_t form_edit_cursor;

    char link_number_buf[8];
    int link_number_len;

    /* Omnibox suggestions while editing the URL (bookmarks + history). */
    suggestion_t sugg[SUGGEST_MAX];
    int sugg_count;
    int sugg_sel;               /* -1 = the typed text itself */

    int tab_sel;                /* selected row in the tab overview */

    /* 4.3: the file offered for download */
    char dl_url[URL_MAX];
    char dl_name[64];
    char dl_type[64];
    long long dl_size;          /* -1: unknown */

    bool reader_chip_hidden;    /* 4.4: "Simplified view" chip dismissed */

    char status_message[64];
    Uint64 status_message_until;

    bool scroll_up_held;
    bool scroll_down_held;
    Uint64 scroll_repeat_at;

    bool accel_down;            /* WHY key held */
    bool inhibit_text_once;     /* swallow TEXT_INPUT after commands */
    bool screenshot_pending;    /* WHY+S: visible 716x716 frame */
    bool full_screenshot_pending; /* WHY+Z: complete rendered page */

    /* First visible line for the current scroll position, with the
     * formatting state at that point (so scrolling is O(lines scrolled)). */
    const char *scroll_cache_content;
    int scroll_cache_line;
    const char *scroll_cache_ptr;
    text_state_t scroll_cache_state;
} browser_t;

static void bfcache_store(browser_t *b, const char *url, const fetch_meta_t *meta);
static void bg_cancel(void);
static void bg_start(browser_t *b);
static void find_clear_hook(void);
static int bg_progress_keys(browser_t *b, curl_off_t dltotal, curl_off_t dlnow);
static const char *shown_web_url(const browser_t *b);

/* Columns and lines of page text at the current zoom (4.4). */
#define k_max_cols       ((VIEW_W - 2 * PAD_LR) / ((FONT_W_COLS + FONT_COL_GAP) * g_page_scale))
#define k_lines_per_page ((VIEW_H - PAD_TOP - PAD_BOTTOM) / (FONT_H_ROWS * g_page_scale + LINE_SPACING))

static void browser_set_status(browser_t *b, const char *message, Uint64 ms) {
    snprintf(b->status_message, sizeof(b->status_message), "%s", message);
    b->status_message_until = SDL_GetTicks() + ms;
}

static int count_lines(const char *s) {
    if (!s || !*s) return 0;
    int lines = 1;
    for (; *s; s++)
        if (*s == '\n') lines++;
    return lines;
}

/* Height a content line takes on screen (inline images are taller). */
static int content_line_height(const page_t *page, const char *line, int len) {
    int image_index = -1;
    if (page && is_image_marker_line(line, len, &image_index) &&
        image_index >= 0 && image_index < MAX_INLINE_IMAGES &&
        image_index < page->image_count && g_inline_images[image_index].loaded) {
        int w = 0, h = 0;
        image_draw_size(&page->images[image_index], &g_inline_images[image_index],
                        VIEW_W - 2 * PAD_LR, IMAGE_DRAW_MAX_H, &w, &h);
        return h + LINE_SPACING;
    }
    return CH_H + LINE_SPACING;
}

/* Smallest scroll position from which the rest of the page fits on one
 * screen: scrolling further only shows empty space. */
static int compute_max_scroll_scaled(const page_t *page, const char *content, int lines);

static int compute_max_scroll(const page_t *page, const char *content, int lines) {
    PAGE_SCALE_BEGIN();
    int r = compute_max_scroll_scaled(page, content, lines);
    PAGE_SCALE_END();
    return r;
}

static int compute_max_scroll_scaled(const page_t *page, const char *content, int lines) {
    if (!content || lines <= 0) return 0;
    const int avail = VIEW_H - PAD_TOP - PAD_BOTTOM;
    long total = 0;
    for (const char *p = content; p; ) {
        const char *nl = strchr(p, '\n');
        total += content_line_height(page, p, nl ? (int)(nl - p) : (int)strlen(p));
        p = nl ? nl + 1 : NULL;
    }
    int s = 0;
    for (const char *p = content; p && total > avail; s++) {
        const char *nl = strchr(p, '\n');
        total -= content_line_height(page, p, nl ? (int)(nl - p) : (int)strlen(p));
        p = nl ? nl + 1 : NULL;
    }
    int by_count = lines - k_lines_per_page;
    return s > by_count ? s : (by_count > 0 ? by_count : 0);
}

static void browser_set_content(browser_t *b, char *wrapped) {
    free(b->content_wrapped);
    b->content_wrapped = wrapped;
    b->content_lines = count_lines(wrapped);
    b->max_scroll = compute_max_scroll(b->page, wrapped, b->content_lines);
    b->scroll_cache_content = NULL;
    b->scroll_lines = 0;
    b->sel_action = -1;
}

/* Replace the current page.  Inline images belong to the page they were
 * decoded for, so they are always released with it: the renderer must never
 * look up page->images[] of a page that is gone. */
static void browser_set_page(browser_t *b, page_t *pg) {
    if (b->page != pg) {
        bg_cancel();                       /* its images are no longer wanted */
        find_clear_hook();                 /* 4.4: matches belong to the old page */
        free_page(b->page);
        for (int i = 0; i < MAX_INLINE_IMAGES; i++)
            decoded_image_release(&g_inline_images[i]);
    }
    b->page = pg;
}

static void browser_show_message(browser_t *b, const char *text) {
    browser_set_page(b, NULL);
    browser_set_content(b, wrap_text(text, k_max_cols));
}

static void browser_clamp_scroll(browser_t *b) {
    if (b->scroll_lines > b->max_scroll) b->scroll_lines = b->max_scroll;
    if (b->scroll_lines < 0) b->scroll_lines = 0;
}

static void browser_reset_input(browser_t *b) {
    b->input = INPUT_NONE;
    b->url_cursor = 0;
    b->form_edit_form = -1;
    b->form_edit_field = -1;
    b->link_number_len = 0;
    b->link_number_buf[0] = 0;
    b->sugg_count = 0;
    b->sugg_sel = -1;
}

__attribute__((noinline)) static void browser_update_suggestions(browser_t *b) {
    if (b->input != INPUT_URL) {
        b->sugg_count = 0;
        b->sugg_sel = -1;
        return;
    }
    b->sugg_count = omnibox_suggest(b->url_buf, b->sugg);
    if (b->sugg_sel >= b->sugg_count) b->sugg_sel = b->sugg_count - 1;
}

/* Enter in the omnibox: a chosen suggestion, a URL, or a search. */
__attribute__((noinline)) static void browser_omnibox_go(browser_t *b) {
    static char target[URL_MAX];   /* static: 16 KB app stack */
    bool searched = false;
    bool ok;
    if (b->sugg_sel >= 0 && b->sugg_sel < b->sugg_count) {
        snprintf(target, sizeof target, "%s", b->sugg[b->sugg_sel].url);
        ok = true;
        printf("[mini_browser] omnibox: suggestion -> %s\n", target);
    } else {
        ok = omnibox_resolve(b->url_buf, target, sizeof target, &searched);
        if (ok && searched)
            printf("[mini_browser] omnibox: search -> %s\n", target);
    }
    browser_reset_input(b);
    if (!ok) {
        /* Nothing to load (empty or only "https://"): back to the page. */
        snprintf(b->url_buf, sizeof(b->url_buf), "%s", b->url_before_edit);
        return;
    }
    snprintf(b->url_buf, sizeof(b->url_buf), "%s", target);
    b->need_fetch = true;
}

static void browser_navigate(browser_t *b, const char *url) {
    snprintf(b->url_buf, sizeof(b->url_buf), "%s", url);
    b->need_fetch = true;
    b->sel_action = -1;
}

/* Show a locally generated page (bookmarks / page info). */
static void browser_show_local_page(browser_t *b, page_t *pg, view_kind_t kind, const char *pseudo_url) {
    if ((b->view == VIEW_WEB) && is_http_scheme(b->url_buf)) {
        snprintf(b->view_return_url, sizeof(b->view_return_url), "%s", b->url_buf);
        bfcache_store(b, shown_web_url(b), &g_fetch_meta);   /* returning is then instant */
    }
    char *wrapped = wrap_text(pg->text, k_max_cols);
    browser_set_page(b, pg);
    browser_set_content(b, wrapped);
    snprintf(b->url_buf, sizeof(b->url_buf), "%s", pseudo_url);
    browser_reset_input(b);
    b->view = kind;
}

/* Leave bookmarks / page info / history: back to the page we came from
 * (from the Back/Forward cache when it is still there).  The view changes
 * when that page is shown, so a stopped load leaves this page as it is. */
static bool browser_return_from_local_page(browser_t *b) {
    if (b->view == VIEW_WEB || !b->view_return_url[0]) return false;
    snprintf(b->url_buf, sizeof(b->url_buf), "%s", b->view_return_url);
    b->history_navigation = true;
    b->bf_navigation = true;
    b->need_fetch = true;
    b->sel_action = -1;
    return true;
}

static void browser_log_content(const char *wrapped) {
#if MB_LOG_CONTENT
    if (wrapped) {
        printf("\n--- CONTENT START ---\n");
        debug_print_content_clean(wrapped);
        printf("\n--- CONTENT END ---\n");
    }
#else
    (void)wrapped;
#endif
}

/* ---------- 4.1 part 2: tabs ----------
 *
 * The active tab lives in browser_t and the globals, exactly as before
 * tabs existed, so the rest of the browser does not know about tabs.
 * g_tabs[] holds the saved state of every tab; the slot of the active tab
 * is refreshed by tab_save() just before another tab becomes active.
 * Background tabs keep their parsed page, wrapped text, scroll position,
 * Back/Forward list and fetch metadata; their images are released and
 * fetched again when the tab returns to the front.
 */
#define TAB_MAX 5

typedef struct {
    char url[URL_MAX];
    page_t *page;
    char *content_wrapped;
    int content_lines;
    int max_scroll;
    int scroll_lines;
    long last_http_status;
    view_kind_t view;
    char view_return_url[URL_MAX];
    fetch_meta_t meta;
    char hist[HISTORY_MAX][URL_MAX];
    int hist_len;
    int hist_pos;
    bool error_page;
    bool page_from_post;
    bool page_partial;
} tab_t;

static tab_t g_tabs[TAB_MAX];
static int g_tab_count = 1;
static int g_tab_cur = 0;

static const char *tab_title(const browser_t *b, int i) {
    const page_t *pg = (i == g_tab_cur) ? b->page : g_tabs[i].page;
    const char *url = (i == g_tab_cur) ? b->url_buf : g_tabs[i].url;
    if (pg && pg->title[0]) return pg->title;
    if (!strcmp(url, "newtab:")) return "New tab";
    return url[0] ? omnibox_strip_scheme(url) : "(empty)";
}

static const char *tab_url(const browser_t *b, int i) {
    return (i == g_tab_cur) ? b->url_buf : g_tabs[i].url;
}

/* Move the active tab's state out of the browser into g_tabs[i]. */
__attribute__((noinline)) static void tab_save(browser_t *b, int i) {
    tab_t *t = &g_tabs[i];
    snprintf(t->url, sizeof t->url, "%s", b->url_buf);
    t->page = b->page;
    t->content_wrapped = b->content_wrapped;
    t->content_lines = b->content_lines;
    t->max_scroll = b->max_scroll;
    t->scroll_lines = b->scroll_lines;
    t->last_http_status = b->last_http_status;
    t->view = b->view;
    snprintf(t->view_return_url, sizeof t->view_return_url, "%s", b->view_return_url);
    t->meta = g_fetch_meta;
    memcpy(t->hist, g_hist, sizeof g_hist);
    t->hist_len = g_hist_len;
    t->hist_pos = g_hist_pos;
    t->error_page = b->error_page;
    t->page_from_post = b->page_from_post;
    t->page_partial = b->page_partial;
    bg_cancel();

    /* The browser no longer owns them: nothing may free them now. */
    b->page = NULL;
    b->content_wrapped = NULL;
    for (int k = 0; k < MAX_INLINE_IMAGES; k++)
        decoded_image_release(&g_inline_images[k]);
    decoded_image_release(&g_viewer_image);
    if (b->overlay == OVERLAY_IMAGE) b->overlay = OVERLAY_NONE;
}

/* Make g_tabs[i] the active tab. */
__attribute__((noinline)) static void tab_load(browser_t *b, int i) {
    tab_t *t = &g_tabs[i];
    snprintf(b->url_buf, sizeof b->url_buf, "%s", t->url);
    b->page = t->page;
    b->content_wrapped = t->content_wrapped;
    b->content_lines = t->content_lines;
    b->max_scroll = t->max_scroll;
    b->scroll_lines = t->scroll_lines;
    b->last_http_status = t->last_http_status;
    b->view = t->view;
    snprintf(b->view_return_url, sizeof b->view_return_url, "%s", t->view_return_url);
    g_fetch_meta = t->meta;
    memcpy(g_hist, t->hist, sizeof g_hist);
    g_hist_len = t->hist_len;
    g_hist_pos = t->hist_pos;
    b->error_page = t->error_page;
    b->page_from_post = t->page_from_post;
    b->page_partial = t->page_partial;

    t->page = NULL;               /* owned by the browser again */
    t->content_wrapped = NULL;
    find_clear_hook();            /* 4.4: matches belong to the other tab */

    browser_reset_input(b);
    b->sel_action = -1;
    b->scroll_cache_content = NULL;
    g_tab_cur = i;

    /* Images were released when this tab went to the background: load
     * them again in the background. */
    if (b->page && b->view == VIEW_WEB && b->page->image_count > 0)
        bg_start(b);
    browser_clamp_scroll(b);
    b->dirty = true;
}

static void tab_free_slot(tab_t *t) {
    free_page(t->page);
    free(t->content_wrapped);
    memset(t, 0, sizeof *t);
}

/* "New tab" page: a short hint, bookmarks and recently visited pages. */
__attribute__((noinline)) static page_t *newtab_to_page(void) {
    page_t *pg = (page_t *)calloc(1, sizeof(page_t));
    if (!pg) return NULL;
    snprintf(pg->base, URL_MAX, "newtab:");
    snprintf(pg->title, sizeof(pg->title), "New tab");

    const int recent_max = 5;
    size_t cap = 1024 + (size_t)(g_bookmark_count + recent_max) * (URL_MAX + 160);
    char *text = (char *)malloc(cap);
    if (!text) { free(pg); return NULL; }
    size_t used = 0;

#define NT_APPEND(...) do { \
        int n_ = snprintf(text + used, cap - used, __VA_ARGS__); \
        if (n_ > 0) used += ((size_t)n_ < cap - used) ? (size_t)n_ : cap - used - 1; \
    } while (0)

    NT_APPEND("= NEW TAB =\n\n"
              "Type an address or search words in the bar and press Enter.\n"
              "WHY+L opens the bar, WHY+A shows all tabs, WHY+W closes this tab.\n\n");

    int number = 0;
    NT_APPEND("Bookmarks\n\n");
    if (g_bookmark_count == 0) NT_APPEND("No bookmarks yet (WHY+K on a page adds one).\n\n");
    for (int i = 0; i < g_bookmark_count && pg->link_count < MAX_LINKS; i++) {
        long off = page_store_string(pg, g_bookmarks[i].url);
        if (off < 0) break;
        pg->links[pg->link_count].href = (uint32_t)off;
        add_action(pg, ACTION_LINK, pg->link_count++, -1, -1);
        NT_APPEND("[%d] %s\n\n", ++number, g_bookmarks[i].title[0] ? g_bookmarks[i].title
                                                                    : omnibox_strip_scheme(g_bookmarks[i].url));
    }

    NT_APPEND("Recently visited\n\n");
    if (g_visit_count == 0) NT_APPEND("Nothing yet.\n");
    for (int i = 0; i < g_visit_count && i < recent_max && pg->link_count < MAX_LINKS; i++) {
        long off = page_store_string(pg, g_visits[i].url);
        if (off < 0) break;
        pg->links[pg->link_count].href = (uint32_t)off;
        add_action(pg, ACTION_LINK, pg->link_count++, -1, -1);
        NT_APPEND("[%d] %s\n\n", ++number, g_visits[i].title[0] ? g_visits[i].title
                                                                 : omnibox_strip_scheme(g_visits[i].url));
    }
#undef NT_APPEND
    text[cap - 1] = 0;
    pg->text = text;
    return pg;
}

/* Show the new-tab page in the (empty) active tab and open the omnibox. */
static void tab_show_newtab_page(browser_t *b) {
    page_t *pg = newtab_to_page();
    if (pg) {
        char *wrapped = wrap_text(pg->text, k_max_cols);
        browser_set_page(b, pg);
        browser_set_content(b, wrapped);
    }
    snprintf(b->url_buf, sizeof b->url_buf, "newtab:");
    b->view = VIEW_NEWTAB;
    b->view_return_url[0] = 0;
    b->last_http_status = 0;
    browser_log_content(b->content_wrapped);   /* for the regression suite */

    browser_reset_input(b);
    snprintf(b->url_before_edit, sizeof b->url_before_edit, "%s", b->url_buf);
    b->url_buf[0] = 0;
    b->url_cursor = 0;
    b->input = INPUT_URL;
}

__attribute__((noinline)) static void tab_new(browser_t *b) {
    if (g_tab_count >= TAB_MAX) {
        browser_set_status(b, "MAXIMUM 5 TABS", 1500);
        printf("[mini_browser] tab: new refused, %d tabs open\n", g_tab_count);
        return;
    }
    b->overlay = OVERLAY_NONE;
    tab_save(b, g_tab_cur);
    int at = g_tab_cur + 1;              /* like Chrome: right of the current tab */
    memmove(&g_tabs[at + 1], &g_tabs[at], (size_t)(g_tab_count - at) * sizeof(tab_t));
    memset(&g_tabs[at], 0, sizeof(tab_t));
    g_tab_count++;
    g_tab_cur = at;

    /* A fresh, empty tab. */
    g_hist_len = 0;
    g_hist_pos = -1;
    b->error_page = false;
    b->page_from_post = false;
    b->page_partial = false;
    memset(&g_fetch_meta, 0, sizeof g_fetch_meta);
    b->content_lines = 0;
    b->max_scroll = 0;
    b->scroll_lines = 0;
    b->sel_action = -1;
    b->scroll_cache_content = NULL;
    printf("[mini_browser] tab: new %d/%d\n", g_tab_cur + 1, g_tab_count);
    tab_show_newtab_page(b);
    b->dirty = true;
}

__attribute__((noinline)) static void tab_switch(browser_t *b, int i) {
    if (i < 0 || i >= g_tab_count || i == g_tab_cur) return;
    b->overlay = OVERLAY_NONE;
    tab_save(b, g_tab_cur);
    tab_load(b, i);
    printf("[mini_browser] tab: switch %d/%d url=%s\n", g_tab_cur + 1, g_tab_count, b->url_buf);
}

/* Close tab i (the active one or a background one). */
__attribute__((noinline)) static void tab_close(browser_t *b, int i) {
    if (i < 0 || i >= g_tab_count) return;
    if (g_tab_count == 1) {
        browser_set_status(b, "LAST TAB - WHY+Q QUITS", 1500);
        return;
    }
    printf("[mini_browser] tab: close %d/%d url=%s\n", i + 1, g_tab_count, tab_url(b, i));
    if (i == g_tab_cur) {
        /* Free the active tab's page through the browser, then promote a neighbour. */
        browser_set_page(b, NULL);
        free(b->content_wrapped);
        b->content_wrapped = NULL;
        memset(&g_tabs[i], 0, sizeof(tab_t));
        memmove(&g_tabs[i], &g_tabs[i + 1], (size_t)(g_tab_count - i - 1) * sizeof(tab_t));
        g_tab_count--;
        memset(&g_tabs[g_tab_count], 0, sizeof(tab_t));
        tab_load(b, i < g_tab_count ? i : g_tab_count - 1);
    } else {
        tab_free_slot(&g_tabs[i]);
        memmove(&g_tabs[i], &g_tabs[i + 1], (size_t)(g_tab_count - i - 1) * sizeof(tab_t));
        g_tab_count--;
        memset(&g_tabs[g_tab_count], 0, sizeof(tab_t));
        if (i < g_tab_cur) g_tab_cur--;
    }
    b->dirty = true;
}

static void tab_free_all_background(void) {
    for (int i = 0; i < g_tab_count; i++)
        if (i != g_tab_cur) tab_free_slot(&g_tabs[i]);
}

/* ---------- 4.4: find in page (WHY+F) and the focus ring (Tab) ----------
 *
 * Matches are searched line by line in the wrapped text, case-insensitive
 * for ASCII, with the formatting markers left out.  They are kept as byte
 * ranges of content_wrapped and drawn highlighted (the current one in
 * orange).  Enter / n go to the next match, N (Shift+n) to the previous one,
 * Esc clears the highlights.
 *
 * The focused action (Tab / Shift+Tab) is found in the text by its marker
 * and "[n]" number, and drawn with a ring; Enter activates it.
 */
#define FIND_MAX        400
#define FIND_QUERY_MAX  64

typedef struct {
    bool shown;                      /* highlights on screen */
    char query[FIND_QUERY_MAX];
    int count;
    int current;                     /* -1: none */
    size_t start[FIND_MAX];
    unsigned short len[FIND_MAX];
    int line[FIND_MAX];
    const char *content;             /* the text the matches belong to */
} find_state_t;

static find_state_t g_find;

static struct {
    const char *content;
    int action;
    size_t start, end;               /* bytes in content_wrapped */
    int line;
    bool found;
} g_focus = { NULL, -1, 0, 0, 0, false };

/* Search the wrapped text for g_find.query. */
__attribute__((noinline)) static void find_compute(const char *content) {
    static char clean[1024];
    static size_t map[1024];
    static char needle[FIND_QUERY_MAX];
    g_find.count = 0;
    g_find.content = content;
    size_t qn = strlen(g_find.query);
    if (!content || !qn) { g_find.current = -1; return; }
    for (size_t i = 0; i <= qn; i++) needle[i] = (char)tolower((unsigned char)g_find.query[i]);

    int lineno = 0;
    for (const char *p = content; p && *p; lineno++) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        size_t n = 0, i = 0;
        while (i < len && n < sizeof clean) {
            size_t at = i;
            unsigned cp = utf8_next(p, len, &i);
            if (cp == 0) break;
            if (cp < 0x20 || is_format_marker(cp)) continue;     /* markers */
            for (size_t k = at; k < i && n < sizeof clean; k++) {
                clean[n] = (char)tolower((unsigned char)p[k]);
                map[n++] = (size_t)(p - content) + k;
            }
        }
        for (size_t j = 0; j + qn <= n && g_find.count < FIND_MAX; ) {
            if (!memcmp(clean + j, needle, qn)) {
                g_find.start[g_find.count] = map[j];
                g_find.len[g_find.count] = (unsigned short)(map[j + qn - 1] + 1 - map[j]);
                g_find.line[g_find.count] = lineno;
                g_find.count++;
                j += qn;
            } else {
                j++;
            }
        }
        p = nl ? nl + 1 : NULL;
    }
    if (g_find.current >= g_find.count) g_find.current = g_find.count - 1;
}

static void scroll_line_into_view(browser_t *b, int line) {
    int page = k_lines_per_page;
    if (line < b->scroll_lines || line >= b->scroll_lines + page - 1) {
        b->scroll_lines = line - page / 3;
        browser_clamp_scroll(b);
    }
}

/* First match on or after the top of the screen. */
static void find_first_visible(browser_t *b) {
    g_find.current = -1;
    for (int i = 0; i < g_find.count; i++)
        if (g_find.line[i] >= b->scroll_lines) { g_find.current = i; break; }
    if (g_find.current < 0 && g_find.count > 0) g_find.current = 0;
    if (g_find.current >= 0) scroll_line_into_view(b, g_find.line[g_find.current]);
}

static void find_step(browser_t *b, int dir) {
    if (b->content_wrapped != g_find.content) find_compute(b->content_wrapped);
    if (g_find.count <= 0) {
        browser_set_status(b, "NO MATCHES", 1200);
        return;
    }
    g_find.current = g_find.current < 0 ? 0 : (g_find.current + dir + g_find.count) % g_find.count;
    scroll_line_into_view(b, g_find.line[g_find.current]);
    printf("[mini_browser] find: match %d of %d (line %d)\n", g_find.current + 1, g_find.count,
           g_find.line[g_find.current] + 1);
}

static void find_clear(void) {
    g_find.shown = false;
    g_find.count = 0;
    g_find.current = -1;
    g_find.content = NULL;
}

static void find_clear_hook(void) {
    find_clear();
    g_focus.found = false;
    g_focus.content = NULL;
}

/* Where is action `action` (0-based) in the text? */
static void focus_locate(browser_t *b) {
    g_focus.content = b->content_wrapped;
    g_focus.action = b->sel_action;
    g_focus.found = false;
    if (!b->content_wrapped || b->sel_action < 0) return;
    char number[16];
    int nlen = snprintf(number, sizeof number, "[%d]", b->sel_action + 1);
    for (const char *p = b->content_wrapped; (p = strstr(p, number)) != NULL; p++) {
        if (p == b->content_wrapped) continue;
        unsigned char on = (unsigned char)p[-1];
        if (on != TEXT_LINK_ON && on != TEXT_FORM_ON) continue;
        unsigned char off = on == TEXT_LINK_ON ? TEXT_LINK_OFF : TEXT_FORM_OFF;
        const char *end = strchr(p + nlen, (char)off);
        const char *limit = p + 600;                 /* a link is not that long */
        if (!end || end > limit) end = p + nlen;
        g_focus.start = (size_t)(p - b->content_wrapped);
        g_focus.end = (size_t)(end - b->content_wrapped);
        int line = 0;
        for (const char *q = b->content_wrapped; q < p; q++) if (*q == '\n') line++;
        g_focus.line = line;
        g_focus.found = true;
        return;
    }
}

/* Marks of the line at byte offset off (length len) for draw_text_ex. */
static void line_marks_build(const browser_t *b, size_t off, size_t len) {
    g_line_mark_count = 0;
    if (g_find.shown && g_find.content == b->content_wrapped) {
        for (int i = 0; i < g_find.count && g_line_mark_count < LINE_MARKS_MAX; i++) {
            if (g_find.start[i] < off || g_find.start[i] >= off + len) continue;
            g_line_marks[g_line_mark_count++] = (line_mark_t){
                g_find.start[i] - off, g_find.start[i] - off + g_find.len[i],
                (unsigned char)(i == g_find.current ? MARK_FIND_CURRENT : MARK_FIND) };
        }
    }
    if (g_focus.found && g_focus.content == b->content_wrapped && b->sel_action == g_focus.action &&
        g_focus.start < off + len && g_focus.end > off && g_line_mark_count < LINE_MARKS_MAX) {
        size_t s0 = g_focus.start > off ? g_focus.start - off : 0;
        size_t e0 = g_focus.end < off + len ? g_focus.end - off : len;
        g_line_marks[g_line_mark_count++] = (line_mark_t){ s0, e0, MARK_FOCUS };
    }
}

static void browser_render_page(browser_t *b);
static void colored_text(char *out, size_t cap, unsigned rgb, const char *s);

/* ---------- 4.3: loading line and stop key ----------
 *
 * While a page (or its images) loads, curl calls load_progress_cb() at least
 * twice a second.  It redraws the bar as a loading line with a progress
 * strip under it - over the page that is still shown, as Chrome does - and
 * Esc stops the transfer.  Other keys stay queued for after the load.
 */
static void url_host(const char *url, char *out, size_t cap) {
    out[0] = 0;
    const char *p = url ? strstr(url, "://") : NULL;
    if (!p) return;
    p += 3;
    size_t n = strcspn(p, "/?#");
    const char *at = memchr(p, '@', n);
    if (at) { n -= (size_t)(at + 1 - p); p = at + 1; }
    if (n >= cap) n = cap - 1;
    memcpy(out, p, n);
    out[n] = 0;
}

static void format_kb(char *out, size_t cap, curl_off_t bytes) {
    if (bytes < 1024) snprintf(out, cap, "%u bytes", (unsigned)bytes);
    else snprintf(out, cap, "%u KB", (unsigned)((bytes + 1023) / 1024));
}

__attribute__((noinline)) static void browser_draw_loading(browser_t *b, curl_off_t total, curl_off_t now) {
    static char host[96], amount[64], done[24], all[24];
    url_host(b->url_buf, host, sizeof host);
    if (now <= 0) {
        snprintf(amount, sizeof amount, "waiting for %s", host[0] ? host : "the server");
    } else {
        format_kb(done, sizeof done, now);
        if (total > 0) {
            format_kb(all, sizeof all, total);
            snprintf(amount, sizeof amount, "%s of %s", done, all);
        } else {
            snprintf(amount, sizeof amount, "%s", done);
        }
    }
    if (g_load.background)
        snprintf(b->barline, sizeof b->barline, "[img %d/%d] %s",
                 g_load.image_index + 1, g_load.image_count,
                 b->page && b->page->title[0] ? b->page->title : b->url_buf);
    else if (g_load.download_name)
        snprintf(b->barline, sizeof b->barline, "Saving %s: %s - Esc stops",
                 g_load.download_name, amount);
    else if (g_load.images)
        snprintf(b->barline, sizeof b->barline, "Image %d of %d: %s - Esc stops",
                 g_load.image_index + 1, g_load.image_count, amount);
    else
        snprintf(b->barline, sizeof b->barline, "Loading %s - Esc stops", amount);

    if (b->page && b->content_wrapped && b->overlay == OVERLAY_NONE)
        browser_render_page(b);            /* the page stays visible */
    else
        draw_ui(b->ren, b->barline);

    /* Progress strip on the line under the bar. */
    SDL_FRect track = { 0, URLBAR_H + 1, VIEW_W, 2 };
    SDL_SetRenderDrawColor(b->ren, 0x2A, 0x34, 0x46, 255);
    SDL_RenderFillRect(b->ren, &track);
    SDL_FRect bar = track;
    if (total > 0 && now > 0) {
        bar.w = (float)VIEW_W * (float)(now > total ? total : now) / (float)total;
    } else {
        /* Unknown size: a block that keeps moving. */
        int span = VIEW_W + 160;
        bar.x = (float)((int)((SDL_GetTicks() / 3) % (Uint64)span) - 160);
        bar.w = 160;
    }
    SDL_SetRenderDrawColor(b->ren, 0x64, 0xEF, 0xFE, 255);
    SDL_RenderFillRect(b->ren, &bar);
    SDL_RenderPresent(b->ren);
}

static int load_progress_cb(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                            curl_off_t ultotal, curl_off_t ulnow) {
    (void)ultotal; (void)ulnow;
    browser_t *b = (browser_t *)clientp;
    if (!b) return 0;
    if (g_load.background) {
        if (bg_progress_keys(b, dltotal, dlnow)) {
            if (g_load.stopped) printf("[mini_browser] stop: Esc pressed, image loading stopped\n");
            return 1;
        }
    } else if (screenshot_cancel_requested()) {   /* takes queued Esc presses only */
        g_load.stopped = true;
        printf("[mini_browser] stop: Esc pressed, %s stopped\n",
               g_load.download_name ? "download" : g_load.images ? "image loading" : "page load");
        return 1;
    }
    Uint64 now = SDL_GetTicks();
    if (now >= g_load.next_draw) {
        g_load.next_draw = now + 150;
        browser_draw_loading(b, dltotal, dlnow);
    }
    return 0;
}

/* ---------- 4.3: error pages in the style of Chrome ---------- */
typedef struct {
    const char *title;
    const char *detail;     /* "%s" = host */
    const char *code;
} error_text_t;

static error_text_t error_text_for(int rc, long http_status) {
    if (rc == 0) {
        if (http_status == 404 || http_status == 410)
            return (error_text_t){ "This page can't be found",
                "No web page was found at this address on %s.", NULL };
        if (http_status == 401 || http_status == 403)
            return (error_text_t){ "Access denied",
                "You don't have permission to view this page on %s.", NULL };
        if (http_status == 429)
            return (error_text_t){ "Too many requests",
                "%s asks to slow down. Wait a moment and try again.", NULL };
        if (http_status >= 500)
            return (error_text_t){ "This page isn't working",
                "%s is currently unable to handle this request.", NULL };
        return (error_text_t){ "This page isn't working", "%s returned an error.", NULL };
    }
    switch (rc) {
        case CURLE_COULDNT_RESOLVE_HOST:
            return (error_text_t){ "This site can't be reached",
                "The server IP address of %s could not be found.", "ERR_NAME_NOT_RESOLVED" };
        case CURLE_COULDNT_CONNECT:
            return (error_text_t){ "This site can't be reached",
                g_net_modern ? "%s refused to connect."
                             : "%s could not be reached (address not found or connection refused).",
                g_net_modern ? "ERR_CONNECTION_REFUSED" : "ERR_CONNECTION_FAILED" };
        case CURLE_OPERATION_TIMEDOUT:
            return (error_text_t){ "This site can't be reached",
                "%s took too long to respond.", "ERR_TIMED_OUT" };
        case CURLE_SSL_CONNECT_ERROR:
            return (error_text_t){ "This site can't provide a secure connection",
                "%s sent an invalid response, or the secure (TLS) connection failed.",
                "ERR_SSL_PROTOCOL_ERROR" };
        case CURLE_SSL_PEER_CERTIFICATE:
#ifndef CURLE_SSL_CACERT   /* in libcurl both are aliases of one code */
        case CURLE_SSL_CACERT:
#endif
            return (error_text_t){ "Your connection is not private",
                "The certificate of %s could not be verified.", "ERR_CERT_INVALID" };
        case CURLE_RECV_ERROR:
        case CURLE_SEND_ERROR:
        case CURLE_PARTIAL_FILE:
            return (error_text_t){ "This page isn't working",
                "%s closed the connection unexpectedly.", "ERR_CONNECTION_CLOSED" };
        case CURLE_BAD_CONTENT_ENCODING:
            return (error_text_t){ "This page isn't working",
                "The compressed page from %s could not be decoded.", "ERR_CONTENT_DECODING_FAILED" };
        case CURLE_TOO_MANY_REDIRECTS:
            return (error_text_t){ "This page isn't working",
                "%s redirected you too many times.", "ERR_TOO_MANY_REDIRECTS" };
        case CURLE_OUT_OF_MEMORY:
            return (error_text_t){ "Not enough memory",
                "The page from %s did not fit in memory.", "ERR_OUT_OF_MEMORY" };
        case CURLE_UNSUPPORTED_PROTOCOL:
            return (error_text_t){ "This address is not supported",
                "Only http:// and https:// pages can be opened.", "ERR_UNKNOWN_URL_SCHEME" };
        default:
            return (error_text_t){ "This site can't be reached", NULL, "ERR_FAILED" };
    }
}

/* Show the error page for a failed load.  It is a message page (no
 * page_t); R or Enter loads the URL again. */
__attribute__((noinline)) static void browser_show_error(browser_t *b, int rc, long http_status) {
    static char text[1024], host[96], detail[256], line[200], code[64];
    url_host(b->url_buf, host, sizeof host);
    error_text_t e = error_text_for(rc, http_status);

    if (e.detail) snprintf(detail, sizeof detail, e.detail, host[0] ? host : "The server");
    else snprintf(detail, sizeof detail, "%s", curl_easy_strerror((CURLcode)rc));
    if (rc == 0) snprintf(code, sizeof code, "HTTP ERROR %ld", http_status);
    else snprintf(code, sizeof code, "%s (curl %d)", e.code, rc);

    size_t n = 0;
    colored_text(line, sizeof line, 0x64EFFE, ":(");
    n += (size_t)snprintf(text + n, sizeof text - n, "\n%s\n\n", line);
    colored_text(line, sizeof line, 0xFFFB96, e.title);
    n += (size_t)snprintf(text + n, sizeof text - n, "%s\n\n%s\n\n", line, detail);
    if (rc != 0 && rc != CURLE_UNSUPPORTED_PROTOCOL && rc != CURLE_OUT_OF_MEMORY)
        n += (size_t)snprintf(text + n, sizeof text - n,
                              "Try:\n- checking the WiFi connection\n- checking the address for typos\n\n");
    colored_text(line, sizeof line, 0x9CA3AF, code);
    n += (size_t)snprintf(text + n, sizeof text - n, "%s\n\n", line);
    colored_text(line, sizeof line, 0x9CA3AF, b->url_buf);
    n += (size_t)snprintf(text + n, sizeof text - n, "%s\n\n", line);
    if (n < sizeof text)
        snprintf(text + n, sizeof text - n, "Press R or Enter to reload, WHY+B to go back.");

    browser_show_message(b, text);
    b->error_page = true;
    browser_log_content(b->content_wrapped);   /* for the regression suite */
}

/* ---------- 4.3: back/forward cache ----------
 *
 * The last BFCACHE_MAX pages that were left by a navigation are kept as
 * they were: parsed page, wrapped text, scroll position and - within an
 * image memory budget - their decoded images.  Back/Forward (and leaving
 * Bookmarks / History / Page info) to one of them shows it at once, without
 * the network.  Reload (WHY+R) always fetches; POST results are not kept.
 */
#define BFCACHE_MAX           3
#define BFCACHE_IMAGE_BUDGET  (3u * 1024u * 1024u)

typedef struct {
    bool used;
    char url[URL_MAX];
    page_t *page;
    char *content_wrapped;
    int content_lines;
    int max_scroll;
    int scroll_lines;
    long http_status;
    fetch_meta_t meta;
    decoded_image_t images[MAX_INLINE_IMAGES];
    size_t image_bytes;
    Uint64 stamp;
} bfcache_entry_t;

static bfcache_entry_t g_bfcache[BFCACHE_MAX];
static Uint64 g_bfcache_clock;

static void bfcache_free(bfcache_entry_t *e) {
    free_page(e->page);
    free(e->content_wrapped);
    for (int i = 0; i < MAX_INLINE_IMAGES; i++)
        decoded_image_release(&e->images[i]);
    memset(e, 0, sizeof *e);
}

static void bfcache_clear(void) {
    for (int i = 0; i < BFCACHE_MAX; i++)
        if (g_bfcache[i].used) bfcache_free(&g_bfcache[i]);
}

static int bfcache_find(const char *url) {
    for (int i = 0; i < BFCACHE_MAX; i++)
        if (g_bfcache[i].used && !strcmp(g_bfcache[i].url, url)) return i;
    return -1;
}

/* The URL of the web page on screen: the current Back/Forward entry. */
static const char *shown_web_url(const browser_t *b) {
    if (b->view != VIEW_WEB || g_hist_pos < 0 || g_hist_pos >= g_hist_len) return NULL;
    return g_hist[g_hist_pos];
}

/* Move the page on screen into the cache (when it can be kept). */
__attribute__((noinline)) static void bfcache_store(browser_t *b, const char *url,
                                                     const fetch_meta_t *meta) {
    if (!url || b->view != VIEW_WEB || !b->page || !b->content_wrapped || b->page_from_post ||
        b->page_partial || b->error_page || !is_http_scheme(url) || !is_http_scheme(b->page->base) ||
        b->last_http_status >= 400)
        return;

    int slot = bfcache_find(url);
    if (slot < 0) {
        for (int i = 0; i < BFCACHE_MAX; i++) {
            if (!g_bfcache[i].used) { slot = i; break; }
            if (slot < 0 || g_bfcache[i].stamp < g_bfcache[slot].stamp) slot = i;
        }
    }
    if (g_bfcache[slot].used) bfcache_free(&g_bfcache[slot]);

    bfcache_entry_t *e = &g_bfcache[slot];
    e->used = true;
    e->stamp = ++g_bfcache_clock;
    snprintf(e->url, sizeof e->url, "%s", url);
    e->page = b->page;
    e->content_wrapped = b->content_wrapped;
    e->content_lines = b->content_lines;
    e->max_scroll = b->max_scroll;
    e->scroll_lines = b->scroll_lines;
    e->http_status = b->last_http_status;
    e->meta = *meta;

    /* Decoded images, if they fit in the budget next to the other entries. */
    size_t others = 0, mine = 0;
    for (int i = 0; i < BFCACHE_MAX; i++) others += g_bfcache[i].image_bytes;
    for (int i = 0; i < MAX_INLINE_IMAGES; i++)
        if (g_inline_images[i].loaded)
            mine += (size_t)g_inline_images[i].width * (size_t)g_inline_images[i].height * 2u;
    int kept = 0;
    for (int i = 0; i < MAX_INLINE_IMAGES; i++) {
        if (others + mine <= BFCACHE_IMAGE_BUDGET && g_inline_images[i].loaded) {
            e->images[i] = g_inline_images[i];
            memset(&g_inline_images[i], 0, sizeof g_inline_images[i]);
            kept++;
        } else {
            decoded_image_release(&g_inline_images[i]);
        }
    }
    if (kept) e->image_bytes = mine;

    /* The cache owns them now. */
    bg_cancel();
    b->page = NULL;
    b->content_wrapped = NULL;
    b->scroll_cache_content = NULL;
    printf("[mini_browser] bfcache: stored %s (%d images)\n", url, kept);
}

/* Show cache entry i (taking it out of the cache). */
__attribute__((noinline)) static void bfcache_restore(browser_t *b, int i) {
    bfcache_entry_t *e = &g_bfcache[i];
    browser_set_page(b, e->page);          /* frees what was on screen */
    for (int k = 0; k < MAX_INLINE_IMAGES; k++)
        g_inline_images[k] = e->images[k];
    free(b->content_wrapped);
    b->content_wrapped = e->content_wrapped;
    b->content_lines = e->content_lines;
    b->max_scroll = e->max_scroll;
    b->scroll_lines = e->scroll_lines;
    b->last_http_status = e->http_status;
    b->scroll_cache_content = NULL;
    b->sel_action = -1;
    g_fetch_meta = e->meta;
    memset(e, 0, sizeof *e);

    /* Images that did not fit in the cache are loaded again, in the
     * background. */
    bg_start(b);
    browser_clamp_scroll(b);
}

/* ---------- 4.3 part 3: images in the background ----------
 *
 * A page is shown as soon as its text is there; its inline images are then
 * loaded one by one from the main loop, the ones on screen (or nearest to
 * it) first, and each appears as soon as it is decoded.  While an image
 * downloads, the progress callback keeps the page usable:
 *   - Up/Down/PgUp/PgDn/J/K scroll at once;
 *   - Esc stops the image loading (instead of quitting the browser);
 *   - any other key interrupts the download and is handled by the main loop;
 *     the image is tried again when the browser is idle on the same page.
 * Images that belong to a page that is no longer shown are dropped.
 *
 * Everything runs on the app's one thread: BadgeVMS app threads share a
 * memory allocator without locking, so curl cannot run in two threads.
 */
typedef struct {
    bool active;
    const page_t *page;             /* the page these images belong to */
    int count;                      /* inline images to load */
    int done;                       /* loaded or failed */
    int loaded;
    bool finished[MAX_INLINE_IMAGES];
    int line[MAX_INLINE_IMAGES];    /* content line of each image marker */
    Uint64 started;
} bg_images_t;

static bg_images_t g_bg;

typedef struct {
    int scroll;                     /* lines to scroll */
    bool esc;
    bool other_key;
} bg_keys_t;

static void bg_cancel(void) {
    g_bg.active = false;
}

/* "display:" summary line (the regression suite reads it). */
static void browser_log_display(const page_t *page) {
    int inline_loaded = 0;
    for (int ii = 0; ii < MAX_INLINE_IMAGES; ii++)
        if (g_inline_images[ii].loaded) inline_loaded++;
    printf("[mini_browser] display: mode=%s images_seen=%d images_retained=%d images_loaded=%d\n",
           display_mode_name(g_display_mode),
           page->image_seen_count, page->image_count, inline_loaded);
}

/* Start loading the images of the page on screen that are not loaded yet. */
__attribute__((noinline)) static void bg_start(browser_t *b) {
    memset(&g_bg, 0, sizeof g_bg);
    const page_t *page = b->page;
    int limit = display_inline_image_limit();
    if (!page || !b->content_wrapped || limit <= 0 || page->image_count <= 0) return;
    int count = page->image_count < limit ? page->image_count : limit;
    if (count > MAX_INLINE_IMAGES) count = MAX_INLINE_IMAGES;

    for (int i = 0; i < count; i++) g_bg.line[i] = 1 << 30;   /* not in the text */
    int lineno = 0;
    for (const char *p = b->content_wrapped; p; lineno++) {
        const char *nl = strchr(p, '\n');
        int idx = -1;
        if (is_image_marker_line(p, nl ? (int)(nl - p) : (int)strlen(p), &idx) &&
            idx >= 0 && idx < count && g_bg.line[idx] == 1 << 30)
            g_bg.line[idx] = lineno;
        p = nl ? nl + 1 : NULL;
    }

    int pending = 0;
    for (int i = 0; i < count; i++) {
        if (g_inline_images[i].loaded) {
            g_bg.finished[i] = true;
            g_bg.done++;
            g_bg.loaded++;
        } else {
            pending++;
        }
    }
    if (!pending) return;
    g_bg.active = true;
    g_bg.page = page;
    g_bg.count = count;
    g_bg.started = SDL_GetTicks();
}

static bool bg_can_run(const browser_t *b) {
    return g_bg.active && !b->need_fetch && b->input == INPUT_NONE &&
           b->overlay == OVERLAY_NONE && (b->view == VIEW_WEB || b->view == VIEW_READER);
}

static void bg_finish(browser_t *b, bool stopped) {
    g_bg.active = false;
    printf("[mini_browser] images: %d of %d loaded in %u ms%s\n", g_bg.loaded, g_bg.count,
           (unsigned)(SDL_GetTicks() - g_bg.started), stopped ? " (stopped)" : "");
    if (b->page == g_bg.page) browser_log_display(b->page);
    b->dirty = true;
}

/* Esc while images are loading (between two downloads). */
static void bg_stop(browser_t *b) {
    printf("[mini_browser] stop: Esc pressed, image loading stopped\n");
    browser_set_status(b, "IMAGES STOPPED", 1500);
    bg_finish(b, true);
}

/* Take scroll keys and Esc out of the queue; note any other key. */
static bool SDLCALL bg_key_filter(void *userdata, SDL_Event *ev) {
    bg_keys_t *k = (bg_keys_t *)userdata;
    if (ev->type != SDL_EVENT_KEY_DOWN && ev->type != SDL_EVENT_KEY_UP) {
        if (ev->type == SDL_EVENT_QUIT) k->other_key = true;
        return true;
    }
    SDL_Scancode sc = ev->key.scancode;
    int step = 0;
    switch (sc) {
        case SDL_SCANCODE_UP: case SDL_SCANCODE_K: step = -1; break;
        case SDL_SCANCODE_DOWN: case SDL_SCANCODE_J: step = 1; break;
        case SDL_SCANCODE_PAGEDOWN: step = k_lines_per_page > 2 ? k_lines_per_page - 2 : 1; break;
        case SDL_SCANCODE_PAGEUP: step = -5; break;
        case SDL_SCANCODE_ESCAPE:
            if (ev->type == SDL_EVENT_KEY_UP) return true;
            k->esc = true;
            return false;
        default:
            if (ev->type == SDL_EVENT_KEY_DOWN) k->other_key = true;
            return true;                  /* left for the main loop */
    }
    if (ev->type == SDL_EVENT_KEY_UP) return true;   /* clears "held" in the main loop */
    k->scroll += step;
    return false;
}

/* Called from load_progress_cb while a background image downloads. */
static int bg_progress_keys(browser_t *b, curl_off_t dltotal, curl_off_t dlnow) {
    bg_keys_t keys = { 0 };
    SDL_PumpEvents();
    SDL_FilterEvents(bg_key_filter, &keys);
    if (keys.esc) {
        g_load.stopped = true;
        return 1;
    }
    if (keys.other_key) {                 /* a link, WHY+..., typing: go */
        g_load.yielded = true;
        return 1;
    }
    if (keys.scroll) {
        b->scroll_lines += keys.scroll;
        browser_clamp_scroll(b);
        g_load.next_draw = 0;             /* redraw now */
    }
    return 0;
}

/* Load one image (the one nearest to the screen); called when idle. */
__attribute__((noinline)) static void bg_step(browser_t *b) {
    if (!g_bg.active) return;
    if (b->page != g_bg.page || !b->content_wrapped) {
        g_bg.active = false;              /* the page is gone */
        return;
    }
    int pick = -1;
    long best = 0;
    int top = b->scroll_lines, bottom = b->scroll_lines + k_lines_per_page;
    for (int i = 0; i < g_bg.count; i++) {
        if (g_bg.finished[i]) continue;
        int l = g_bg.line[i];
        long dist = l < top ? (long)(top - l) : l >= bottom ? (long)(l - bottom + 1) : 0;
        if (pick < 0 || dist < best) { pick = i; best = dist; }
    }
    if (pick < 0) {
        bg_finish(b, false);
        return;
    }

    memset(&g_load, 0, sizeof g_load);
    g_load.b = b;
    g_load.background = true;
    g_load.images = true;
    g_load.image_index = pick;
    g_load.image_count = g_bg.count;
    browser_draw_loading(b, 0, 0);
    g_load.next_draw = SDL_GetTicks() + 150;

    int ok = load_image_url(page_image_src(b->page, pick), IMAGE_DRAW_MAX_W, IMAGE_DRAW_MAX_H,
                            &g_inline_images[pick]);
    bool yielded = g_load.yielded, stopped = g_load.stopped;
    g_load.b = NULL;

    if (!yielded) {                       /* loaded, or failed for good */
        g_bg.finished[pick] = true;
        g_bg.done++;
        if (ok) g_bg.loaded++;
    }
    b->max_scroll = compute_max_scroll(b->page, b->content_wrapped, b->content_lines);
    browser_clamp_scroll(b);
    b->dirty = true;
    if (stopped) {
        browser_set_status(b, "IMAGES STOPPED", 1500);
        bg_finish(b, true);
    } else if (g_bg.done >= g_bg.count) {
        bg_finish(b, false);
    }
}

/* ---------- page loading ---------- */

/* The navigation is final: update Back/Forward and leave special pages. */
static void browser_commit_navigation(browser_t *b, int hist_target, bool history_navigation) {
    if (hist_target >= 0 && hist_target < g_hist_len)
        g_hist_pos = hist_target;             /* Back / Forward */
    else if (!history_navigation)
        history_push(b->url_buf);
    b->view = VIEW_WEB;
    b->view_return_url[0] = 0;
    b->error_page = false;
    b->page_partial = false;
}

/* A load was stopped before anything arrived: the old page stays, and so
 * does its address. */
static void browser_restore_shown_url(browser_t *b) {
    switch (b->view) {
        case VIEW_WEB:
            if (g_hist_pos >= 0 && g_hist_pos < g_hist_len)
                snprintf(b->url_buf, sizeof b->url_buf, "%s", g_hist[g_hist_pos]);
            break;
        case VIEW_BOOKMARKS: snprintf(b->url_buf, sizeof b->url_buf, "bookmarks:"); break;
        case VIEW_PAGE_INFO: snprintf(b->url_buf, sizeof b->url_buf, "page-info:"); break;
        case VIEW_HISTORY:   snprintf(b->url_buf, sizeof b->url_buf, "history:"); break;
        case VIEW_NEWTAB:    snprintf(b->url_buf, sizeof b->url_buf, "newtab:"); break;
        case VIEW_DOWNLOADS: snprintf(b->url_buf, sizeof b->url_buf, "downloads:"); break;
        case VIEW_READER:    snprintf(b->url_buf, sizeof b->url_buf, "reader:"); break;
        case VIEW_SAVED:     snprintf(b->url_buf, sizeof b->url_buf, "saved:"); break;
    }
}

/* The lines the regression suite reads after every page load. */
__attribute__((noinline)) static void browser_log_page(browser_t *b, long http_status,
                                                        unsigned bytes, const char *how) {
    page_t *page = b->page;
    printf("[mini_browser] HTTP %ld, %u bytes, %d links from %s%s\n",
           http_status, bytes, page->link_count, b->url_buf, how);
    /*
     * Deterministic parser diagnostic.  Besides being useful while
     * debugging, the 2.5 regression suite uses this to verify that
     * HTML entities in <title> were decoded into page->title.
     */
    printf("[mini_browser] page title: %s\n", page->title[0] ? page->title : "(none)");
    printf("[mini_browser] parser: links=%d actions=%d forms=%d\n",
           page->link_count, page->action_count, page->form_count);
    printf("[mini_browser] visual: explicit_colors=%d\n", page->explicit_color_count);
    printf("[mini_browser] visual: explicit_styles=%d\n", page->explicit_style_count);
    printf("[mini_browser] visual: explicit_backgrounds=%d\n", page->explicit_background_count);
    /* With images still loading, the display line follows when they are
     * done (bg_finish). */
    if (!g_bg.active) browser_log_display(page);
    printf("[mini_browser] cookies: count=%d\n", cookie_count());
    browser_log_content(b->content_wrapped);
}

/* ---------- 4.3 part 2: downloads ----------
 *
 * A response the browser cannot show (zip, pdf, mp3, a binary...) or one
 * sent with "Content-Disposition: attachment" is not read into memory: the
 * load stops at its first bytes and the browser asks whether to save it.
 * The file is then fetched again and written straight to DOWNLOAD_DIR, with
 * the loading line and Esc to stop; a file that is not complete is removed.
 * The list of saved files (WHY+D) is kept in DOWNLOAD_LIST.
 */
#define DOWNLOAD_DIR       "FLASH0:[DOWNLOADS]"
#define DOWNLOAD_LIST      "APPS:[mini_browser]downloads.txt"
#define DOWNLOAD_MAX       (4LL * 1024 * 1024)
#define DOWNLOAD_LIST_MAX  50
#define SAVED_DIR          "FLASH0:[SAVED]"          /* 4.4: pages saved with WHY+P */
#define SAVED_LIST         "APPS:[mini_browser]saved.txt"
#define SAVED_LIST_MAX     50

/* BadgeVMS file names: letters, digits, '_', '-', '$' and '.'. */
static void download_sanitize(const char *in, size_t n, char *out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; i < n && in[i] && o + 1 < cap; i++) {
        unsigned char c = (unsigned char)in[i];
        bool ok = isalnum(c) || c == '_' || c == '-' || c == '$' || c == '.';
        char ch = ok ? (char)c : '_';
        if (ch == '_' && o > 0 && out[o - 1] == '_') continue;
        if (ch == '.' && o == 0) continue;            /* no hidden names */
        out[o++] = ch;
    }
    while (o > 0 && (out[o - 1] == '_' || out[o - 1] == '.')) o--;
    out[o] = 0;
    if (!o) snprintf(out, cap, "download");
}

/* Name from Content-Disposition, else from the last part of the URL path. */
static void download_name_from(const char *url, const char *disposition, char *out, size_t cap) {
    const char *v = NULL;
    size_t n = 0;
    const char *star = disposition ? strstr(disposition, "filename*=") : NULL;
    const char *plain = disposition ? strstr(disposition, "filename=") : NULL;
    if (star) {
        v = star + 10;
        const char *q = strstr(v, "''");             /* UTF-8''name */
        if (q) v = q + 2;
        n = strcspn(v, "; ");
    } else if (plain) {
        v = plain + 9;
        if (*v == '"') { v++; n = strcspn(v, "\""); }
        else n = strcspn(v, "; ");
    }
    if (!v || !n) {
        const char *p = strstr(url, "://");
        p = p ? p + 3 : url;
        size_t path_len = strcspn(p, "?#");
        const char *last = NULL;
        for (const char *q = p; q < p + path_len; q++)
            if (*q == '/') last = q;
        v = last ? last + 1 : "";
        n = last ? (size_t)(p + path_len - v) : 0;
    }
    download_sanitize(v, n, out, cap);
}

static bool file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

/* DOWNLOAD_DIR + name, with -2, -3, ... before the extension when taken. */
static void download_unique_path(const char *name, char *path, size_t cap) {
    snprintf(path, cap, DOWNLOAD_DIR "%s", name);
    if (!file_exists(path)) return;
    const char *dot = strrchr(name, '.');
    int base_len = dot ? (int)(dot - name) : (int)strlen(name);
    for (int i = 2; i < 100; i++) {
        snprintf(path, cap, DOWNLOAD_DIR "%.*s-%d%s", base_len, name, i, dot ? dot : "");
        if (!file_exists(path)) return;
    }
}

typedef struct {
    FILE *f;
    long long written;
    bool write_failed;
    bool too_big;
} download_sink_t;

static size_t download_write_cb(void *ptr, size_t sz, size_t nm, void *ud) {
    download_sink_t *d = (download_sink_t *)ud;
    size_t n = sz * nm;
    if (d->written + (long long)n > DOWNLOAD_MAX) {
        d->too_big = true;
        return 0;
    }
    if (fwrite(ptr, 1, n, d->f) != n) {
        d->write_failed = true;
        return 0;
    }
    d->written += (long long)n;
    return n;
}

/* Newest first, at most DOWNLOAD_LIST_MAX lines "path<TAB>bytes<TAB>url". */
__attribute__((noinline)) static void downloads_record(const char *path, long long bytes, const char *url) {
    static char lines[DOWNLOAD_LIST_MAX][URL_MAX + 160];
    int count = 0;
    FILE *f = fopen(DOWNLOAD_LIST, "r");
    if (f) {
        while (count < DOWNLOAD_LIST_MAX - 1 && fgets(lines[count], sizeof lines[count], f)) {
            lines[count][strcspn(lines[count], "\r\n")] = 0;
            char *tab = strchr(lines[count], '\t');
            if (!lines[count][0] || !tab) continue;
            if ((size_t)(tab - lines[count]) == strlen(path) && !strncmp(lines[count], path, strlen(path)))
                continue;                       /* same file saved again */
            count++;
        }
        fclose(f);
    }
    remove(DOWNLOAD_LIST);
    f = fopen(DOWNLOAD_LIST, "w");
    if (!f) return;
    fprintf(f, "%s\t%lld\t%s\n", path, bytes, url);
    for (int i = 0; i < count; i++) fprintf(f, "%s\n", lines[i]);
    fclose(f);
}

static void format_size(char *out, size_t cap, long long bytes) {
    if (bytes < 0) snprintf(out, cap, "unknown size");
    else if (bytes < 1024) snprintf(out, cap, "%lld bytes", bytes);
    else if (bytes < 1024 * 1024) snprintf(out, cap, "%lld KB", (bytes + 1023) / 1024);
    else snprintf(out, cap, "%lld.%lld MB", bytes / (1024 * 1024), (bytes % (1024 * 1024)) * 10 / (1024 * 1024));
}

/* Save b->dl_url to flash (after Enter on the download question). */
__attribute__((noinline)) static void browser_download(browser_t *b) {
    static char path[128], size_text[32], message[64];
    if (b->dl_size > DOWNLOAD_MAX) {
        browser_set_status(b, "FILE TOO BIG (MAX 4 MB)", 2000);
        printf("[mini_browser] download: refused, %lld bytes is over the limit\n", b->dl_size);
        return;
    }
    mkdir_p(DOWNLOAD_DIR);
    download_unique_path(b->dl_name, path, sizeof path);
    download_sink_t sink = { 0 };
    sink.f = fopen(path, "wb");
    if (!sink.f) {
        browser_set_status(b, "CANNOT WRITE TO FLASH0", 2000);
        printf("[mini_browser] download: cannot create %s\n", path);
        return;
    }

    CURL *curl = net_acquire();
    if (!curl) {
        fclose(sink.f);
        remove(path);
        return;
    }
    curl_easy_setopt(curl, CURLOPT_URL, b->dl_url);
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);       /* big files take time; idle timeout only */
    net_common_options(curl, false, b);

    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "User-Agent: Mozilla/5.0 (BadgeVMS; ESP32; rv:" MINI_BROWSER_VERSION ") "
                                   "(compatible; MiniBrowser/" MINI_BROWSER_VERSION ")");
    hdrs = curl_slist_append(hdrs, "Accept: */*");
    hdrs = curl_slist_append(hdrs, "Accept-Encoding: identity");
    if (cookie_make_request_header(b->dl_url)) {
        snprintf(g_cookie_header_line, sizeof g_cookie_header_line, "Cookie: %s", g_cookie_header_value);
        hdrs = curl_slist_append(hdrs, g_cookie_header_line);
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, image_header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, NULL);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, download_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);

    printf("[mini_browser] download: saving %s to %s\n", b->dl_url, path);
    memset(&g_load, 0, sizeof g_load);
    g_load.b = b;
    g_load.download_name = b->dl_name;
    browser_draw_loading(b, b->dl_size > 0 ? b->dl_size : 0, 0);
    g_load.next_draw = SDL_GetTicks() + 150;

    resp_reset();
    CURLcode res = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    net_release(curl);
    curl_slist_free_all(hdrs);
    if (fclose(sink.f) != 0) sink.write_failed = true;

    const char *problem = NULL;
    if (g_load.stopped) problem = "DOWNLOAD STOPPED";
    else if (sink.too_big) problem = "FILE TOO BIG (MAX 4 MB)";
    else if (sink.write_failed) problem = "FLASH FULL - DOWNLOAD REMOVED";
    else if (res != CURLE_OK) problem = "DOWNLOAD FAILED";
    else if (code < 200 || code >= 300) problem = "DOWNLOAD FAILED (HTTP ERROR)";

    if (problem) {
        remove(path);
        printf("[mini_browser] download: %s (curl %d, HTTP %ld, %lld bytes), file removed\n",
               problem, (int)res, code, sink.written);
        browser_set_status(b, problem, 2500);
    } else {
        downloads_record(path, sink.written, b->dl_url);
        format_size(size_text, sizeof size_text, sink.written);
        printf("[mini_browser] download: saved %s (%lld bytes)\n", path, sink.written);
        snprintf(message, sizeof message, "SAVED %s (%s)", b->dl_name, size_text);
        browser_set_status(b, message, 3000);
    }
    g_load.b = NULL;
    b->dirty = true;
}

/* "Download this file?" over the page. */
__attribute__((noinline)) static void browser_render_download(browser_t *b) {
    static char line[URL_MAX + 160], text[160], size_text[32], host[96];
    SDL_Renderer *ren = b->ren;
    const int x = 40, w = VIEW_W - 80, h = 9 * (CH_H + LINE_SPACING) + 24;
    const int y = (VIEW_H - h) / 2;
    SDL_FRect box = { (float)x, (float)y, (float)w, (float)h };
    SDL_SetRenderDrawColor(ren, 0x11, 0x18, 0x26, 255);
    SDL_RenderFillRect(ren, &box);
    SDL_SetRenderDrawColor(ren, 0x64, 0xEF, 0xFE, 255);
    SDL_RenderRect(ren, &box);

    int ty = y + 12;
    const int step = CH_H + LINE_SPACING;
    colored_text(line, sizeof line, 0xFFFB96, "Download this file?");
    draw_text(ren, x + 14, ty, line, w - 28); ty += 2 * step;
    draw_text(ren, x + 14, ty, b->dl_name, w - 28); ty += step;
    format_size(size_text, sizeof size_text, b->dl_size);
    snprintf(text, sizeof text, "%s, %s", b->dl_type[0] ? b->dl_type : "unknown type", size_text);
    colored_text(line, sizeof line, 0x9CA3AF, text);
    draw_text(ren, x + 14, ty, line, w - 28); ty += step;
    url_host(b->dl_url, host, sizeof host);
    snprintf(text, sizeof text, "from %s", host);
    colored_text(line, sizeof line, 0x9CA3AF, text);
    draw_text(ren, x + 14, ty, line, w - 28); ty += 2 * step;
    colored_text(line, sizeof line, 0x9CA3AF, "Saved to FLASH0:[DOWNLOADS] (WHY+D lists them)");
    draw_text(ren, x + 14, ty, line, w - 28); ty += 2 * step;
    colored_text(line, sizeof line, 0x64EFFE, b->dl_size > DOWNLOAD_MAX
                     ? "Too big (max 4 MB) - Esc: back" : "Enter: download    Esc: cancel");
    draw_text(ren, x + 14, ty, line, w - 28);
}

/* WHY+D: the files saved so far. */
__attribute__((noinline)) static page_t *downloads_to_page(int *count_out) {
    static char line[URL_MAX + 160], size_text[32], host[96];
    page_t *pg = (page_t *)calloc(1, sizeof(page_t));
    if (!pg) return NULL;
    snprintf(pg->base, URL_MAX, "downloads:");
    snprintf(pg->title, sizeof(pg->title), "Downloads");
    size_t cap = 1024 + (size_t)DOWNLOAD_LIST_MAX * (URL_MAX + 200);
    char *text = (char *)malloc(cap);
    if (!text) { free(pg); return NULL; }
    size_t used = 0;
    int count = 0;
#define DL_APPEND(...) do { \
        int n_ = snprintf(text + used, cap - used, __VA_ARGS__); \
        if (n_ > 0) used += ((size_t)n_ < cap - used) ? (size_t)n_ : cap - used - 1; \
    } while (0)
    DL_APPEND("= DOWNLOADS =\n\n");
    FILE *f = fopen(DOWNLOAD_LIST, "r");
    if (f) {
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            char *t1 = strchr(line, '\t');
            char *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
            if (!t1 || !t2) continue;
            *t1 = 0; *t2 = 0;
            const char *name = strchr(line, ']');
            name = name ? name + 1 : line;
            format_size(size_text, sizeof size_text, atoll(t1 + 1));
            url_host(t2 + 1, host, sizeof host);
            DL_APPEND("%d. %s\n    %s, from %s%s\n\n", ++count, name, size_text, host,
                      file_exists(line) ? "" : " (deleted)");
        }
        fclose(f);
    }
    if (!count) DL_APPEND("No downloads yet.\n\nA link to a file the browser cannot show\n(zip, pdf, mp3, ...) offers to save it.\n\n");
    DL_APPEND("Saved in FLASH0:[DOWNLOADS], max 4 MB per file.\n\n");

    /* 4.4: pages saved for offline reading (WHY+P), as links. */
    DL_APPEND("= SAVED PAGES =\n\n");
    int saved = 0;
    f = fopen(SAVED_LIST, "r");
    if (f) {
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            char *t1 = strchr(line, '\t');
            if (!line[0] || !t1) continue;
            *t1 = 0;
            char *t2 = strchr(t1 + 1, '\t');
            if (t2) *t2 = 0;
            saved++;
            char href[24];
            snprintf(href, sizeof href, "saved:%d", saved);
            long off = pg->link_count < MAX_LINKS ? page_store_string(pg, href) : -1;
            struct stat st;
            bool there = stat(line, &st) == 0;
            format_size(size_text, sizeof size_text, there ? (long long)st.st_size : 0);
            if (off >= 0 && there) {
                pg->links[pg->link_count].href = (uint32_t)off;
                int action = add_action(pg, ACTION_LINK, pg->link_count++, -1, -1);
                DL_APPEND("[%d] %s\n    %s, %s\n\n", action + 1, t1 + 1, size_text,
                          t2 ? omnibox_strip_scheme(t2 + 1) : "");
            } else {
                DL_APPEND("- %s (deleted)\n\n", t1 + 1);
            }
        }
        fclose(f);
    }
    if (!saved) DL_APPEND("None yet. WHY+P saves the page you are reading\n(or its simplified view) as text.\n\n");
    DL_APPEND("WHY+D returns, WHY+X deletes all downloads and saved pages.\n");
#undef DL_APPEND
    text[cap - 1] = 0;
    pg->text = text;
    if (count_out) *count_out = count;
    return pg;
}

/* WHY+X on the Downloads page. */
static void downloads_delete_all(void) {
    static char line[URL_MAX + 160];
    int removed = 0;
    FILE *f = fopen(DOWNLOAD_LIST, "r");
    if (f) {
        while (fgets(line, sizeof line, f)) {
            char *tab = strchr(line, '\t');
            if (!tab) continue;
            *tab = 0;
            if (!strncmp(line, DOWNLOAD_DIR, strlen(DOWNLOAD_DIR)) && remove(line) == 0) removed++;
        }
        fclose(f);
    }
    remove(DOWNLOAD_LIST);
    printf("[mini_browser] downloads: deleted %d files\n", removed);

    /* 4.4: and the saved pages. */
    removed = 0;
    f = fopen(SAVED_LIST, "r");
    if (f) {
        while (fgets(line, sizeof line, f)) {
            char *tab = strchr(line, '\t');
            if (!tab) continue;
            *tab = 0;
            if (!strncmp(line, SAVED_DIR, strlen(SAVED_DIR)) && remove(line) == 0) removed++;
        }
        fclose(f);
    }
    remove(SAVED_LIST);
    printf("[mini_browser] saved pages: deleted %d\n", removed);
}

/* ---------- 4.4: text zoom (WHY+= / WHY+- / WHY+0) ----------
 * The page text is drawn with the 5x7 font at 2x (100%), 3x (150%) or
 * 4x (200%) and re-wrapped; Unicode glyphs scale along.  The bar and menus
 * stay at 100%.  The choice is kept in SETTINGS_FILE. */
#define SETTINGS_FILE  "APPS:[mini_browser]settings.txt"
#define ZOOM_MIN_SCALE 2
#define ZOOM_MAX_SCALE 4

static void settings_save(void) {
    FILE *f = fopen(SETTINGS_FILE, "w");
    if (!f) return;
    fprintf(f, "zoom=%d\n", g_page_scale * 50);
    fclose(f);
}

static void settings_load(void) {
    FILE *f = fopen(SETTINGS_FILE, "r");
    if (!f) return;
    char line[64];
    while (fgets(line, sizeof line, f)) {
        int z = 0;
        if (sscanf(line, "zoom=%d", &z) == 1 && z >= ZOOM_MIN_SCALE * 50 && z <= ZOOM_MAX_SCALE * 50)
            g_page_scale = z / 50;
    }
    fclose(f);
    if (g_page_scale != 2) printf("[mini_browser] zoom: %d%%\n", g_page_scale * 50);
}

/* Re-wrap a page's text for the current zoom. */
static char *rewrap_page(const page_t *pg) {
    return pg && pg->text ? wrap_text(pg->text, k_max_cols) : NULL;
}

__attribute__((noinline)) static void browser_set_zoom(browser_t *b, int scale) {
    if (scale < ZOOM_MIN_SCALE) scale = ZOOM_MIN_SCALE;
    if (scale > ZOOM_MAX_SCALE) scale = ZOOM_MAX_SCALE;
    char msg[32];
    snprintf(msg, sizeof msg, "ZOOM %d%%", scale * 50);
    if (scale == g_page_scale) {
        browser_set_status(b, msg, 1000);
        return;
    }
    /* Keep the same part of the page on screen. */
    double at = b->content_lines > 0 ? (double)b->scroll_lines / (double)b->content_lines : 0.0;
    g_page_scale = scale;
    if (b->page && b->page->text) {
        char *wrapped = rewrap_page(b->page);
        if (wrapped) {
            free(b->content_wrapped);
            b->content_wrapped = wrapped;
            b->content_lines = count_lines(wrapped);
            b->max_scroll = compute_max_scroll(b->page, wrapped, b->content_lines);
            b->scroll_cache_content = NULL;
            b->scroll_lines = (int)(at * b->content_lines);
            browser_clamp_scroll(b);
        }
    }
    /* Other tabs are re-wrapped too; cached pages are simply dropped. */
    for (int i = 0; i < g_tab_count; i++) {
        if (i == g_tab_cur || !g_tabs[i].page) continue;
        char *wrapped = rewrap_page(g_tabs[i].page);
        if (!wrapped) continue;
        free(g_tabs[i].content_wrapped);
        g_tabs[i].content_wrapped = wrapped;
        g_tabs[i].content_lines = count_lines(wrapped);
        g_tabs[i].max_scroll = 0;
        g_tabs[i].scroll_lines = 0;
    }
    bfcache_clear();
    settings_save();
    browser_set_status(b, msg, 1200);
    printf("[mini_browser] zoom: %d%%, %d columns\n", scale * 50, k_max_cols);
    b->dirty = true;
}

/* ---------- 4.4: reader mode ("Simplified view", WHY+V) ----------
 *
 * A Readability-style pass over the page's HTML: every container element
 * (div, article, section, main, td, ...) collects the text of the
 * paragraphs directly inside it; a paragraph scores 1 + its commas + one
 * point per 100 characters (max 3), for its container and half for the
 * container above.  Link-heavy containers lose score, and class/id names
 * like "article"/"content" win while "nav"/"footer"/"sidebar"/"share" lose.
 * The best container becomes the simplified page: title, text and images,
 * without the navigation, footers and widgets inside it.
 */
#define RD_MAX_NODES 512

typedef struct {
    int parent;
    size_t start, end;
    int text, total_text, link_text, total_link;
    int paras, total_paras;
    float score;
    int bias;
    char tag[12];
} rd_node_t;

static rd_node_t g_rd[RD_MAX_NODES];
static int g_rd_count;

static bool rd_name_is(const char *name, const char *const *list) {
    for (; *list; list++) if (!strcmp(name, *list)) return true;
    return false;
}

static const char *const rd_containers[] = { "div", "article", "section", "main", "td", "body",
    "blockquote", "nav", "aside", "footer", "header", "form", "ul", "ol", "table", NULL };
static const char *const rd_junk[] = { "nav", "aside", "footer", "header", "form", NULL };
static const char *const rd_skip[] = { "script", "style", "noscript", "template", "svg", "iframe", NULL };
static const char *const rd_paragraph[] = { "p", "pre", "br", "h1", "h2", "h3", "h4", "h5", "h6",
    "li", "tr", "dd", "dt", "figcaption", NULL };

/* Score from the tag and its class/id/role names. */
static int rd_bias(const char *tag, const char *attrs, size_t len) {
    static const char *const bad[] = { "comment", "nav", "menu", "footer", "header", "sidebar",
        "share", "social", "related", "promo", "banner", "advert", "cookie", "popup", "widget",
        "breadcrumb", "subscribe", "newsletter", "login", "masthead", "meta", "tags", NULL };
    static const char *const good[] = { "article", "content", "main", "post", "entry", "story",
        "text", "body", "blog", "read", NULL };
    static char names[256];
    int bias = 0;
    if (!strcmp(tag, "article") || !strcmp(tag, "main")) bias += 25;
    if (rd_name_is(tag, rd_junk)) bias -= 100;
    size_t n = 0;
    for (size_t i = 0; i + 4 < len && n + 1 < sizeof names; i++) {
        if (strncasecmp(attrs + i, "class=", 6) && strncasecmp(attrs + i, "id=", 3) &&
            strncasecmp(attrs + i, "role=", 5)) continue;
        const char *v = memchr(attrs + i, '=', len - i) + 1;
        char q = (*v == '"' || *v == '\'') ? *v++ : ' ';
        while (v < attrs + len && *v != q && *v != '>' && n + 1 < sizeof names)
            names[n++] = (char)tolower((unsigned char)*v++);
        names[n++] = ' ';
        i = (size_t)(v - attrs);
    }
    names[n] = 0;
    if (!n) return bias;
    int neg = 0, pos = 0;
    for (const char *const *w = bad; *w; w++) if (strstr(names, *w)) neg++;
    for (const char *const *w = good; *w; w++) if (strstr(names, *w)) pos++;
    bias += (pos ? 25 : 0) + (pos > 1 ? 25 : 0) - (neg ? 25 : 0) - (neg > 1 ? 25 : 0);
    return bias;
}

/* Analyse the page; returns the best container or -1. */
__attribute__((noinline)) static int rd_analyze(const char *html, size_t n) {
    static int stack[64];
    int depth = 0;
    g_rd_count = 0;
    if (!html || !n) return -1;
    memset(&g_rd[0], 0, sizeof g_rd[0]);
    g_rd[0].parent = -1;
    g_rd[0].end = n;
    snprintf(g_rd[0].tag, sizeof g_rd[0].tag, "doc");
    g_rd_count = 1;
    stack[depth++] = 0;

    int in_a = 0, para_chars = 0, para_commas = 0, para_node = -1;
#define RD_FLUSH() do { \
        if (para_node >= 0 && para_chars >= 25) { \
            float s_ = 1.0f + (float)para_commas + (float)(para_chars / 100 > 3 ? 3 : para_chars / 100); \
            g_rd[para_node].score += s_; \
            g_rd[para_node].paras++; \
            int up_ = g_rd[para_node].parent; \
            if (up_ >= 0) g_rd[up_].score += s_ / 2.0f; \
        } \
        para_chars = 0; para_commas = 0; para_node = -1; \
    } while (0)

    size_t i = 0;
    while (i < n) {
        if (html[i] == '<') {
            if (i + 3 < n && !strncmp(html + i, "<!--", 4)) {
                const char *e = strstr(html + i + 4, "-->");
                i = e ? (size_t)(e - html) + 3 : n;
                continue;
            }
            bool closing = i + 1 < n && html[i + 1] == '/';
            size_t k = i + 1 + (closing ? 1 : 0);
            char name[12];
            size_t nl = 0;
            while (k < n && nl + 1 < sizeof name && (isalnum((unsigned char)html[k])))
                name[nl++] = (char)tolower((unsigned char)html[k++]);
            name[nl] = 0;
            size_t attrs = k;
            char quote = 0;
            while (k < n && (quote || html[k] != '>')) {
                if (quote) { if (html[k] == quote) quote = 0; }
                else if (html[k] == '"' || html[k] == '\'') quote = html[k];
                k++;
            }
            size_t tag_end = k < n ? k + 1 : n;
            if (!nl) { i = tag_end; continue; }

            if (!closing && rd_name_is(name, rd_skip)) {      /* skip <script>...</script> */
                char close[16];
                snprintf(close, sizeof close, "</%s", name);
                size_t j = tag_end;
                while (j < n && strncasecmp(html + j, close, strlen(close))) j++;
                const char *gt = j < n ? memchr(html + j, '>', n - j) : NULL;
                i = gt ? (size_t)(gt - html) + 1 : n;
                continue;
            }
            if (rd_name_is(name, rd_containers)) {
                RD_FLUSH();
                if (!closing) {
                    if (g_rd_count < RD_MAX_NODES && depth < 64) {
                        rd_node_t *nd = &g_rd[g_rd_count];
                        memset(nd, 0, sizeof *nd);
                        nd->parent = stack[depth - 1];
                        nd->start = i;
                        nd->end = n;
                        nd->bias = rd_bias(name, html + attrs, k - attrs);
                        snprintf(nd->tag, sizeof nd->tag, "%s", name);
                        stack[depth++] = g_rd_count++;
                    }
                } else {
                    for (int d = depth - 1; d > 0; d--) {
                        if (!strcmp(g_rd[stack[d]].tag, name)) {
                            for (int e = depth - 1; e >= d; e--) g_rd[stack[e]].end = tag_end;
                            depth = d;
                            break;
                        }
                    }
                }
            } else if (!strcmp(name, "a")) {
                in_a = closing ? 0 : 1;
            } else if (rd_name_is(name, rd_paragraph)) {
                RD_FLUSH();
            }
            i = tag_end;
            continue;
        }
        /* Text up to the next tag. */
        int node = stack[depth - 1];
        int chars = 0, commas = 0;
        while (i < n && html[i] != '<') {
            unsigned char c = (unsigned char)html[i];
            if (c == '&') {                    /* an entity counts as one character */
                const char *semi = memchr(html + i, ';', (n - i) < 10 ? n - i : 10);
                if (semi) i = (size_t)(semi - html);
                chars++;
            } else if (!isspace(c) && (c & 0xC0) != 0x80) {
                chars++;
                if (c == ',') commas++;
            }
            i++;
        }
        if (!chars) continue;
        g_rd[node].text += chars;
        if (in_a) g_rd[node].link_text += chars;
        if (para_node != node) { RD_FLUSH(); para_node = node; }
        para_chars += chars;
        para_commas += commas;
    }
    RD_FLUSH();
#undef RD_FLUSH

    /* Totals per subtree (children come after their parents). */
    for (int k2 = 0; k2 < g_rd_count; k2++) {
        g_rd[k2].total_text = g_rd[k2].text;
        g_rd[k2].total_link = g_rd[k2].link_text;
        g_rd[k2].total_paras = g_rd[k2].paras;
    }
    for (int k2 = g_rd_count - 1; k2 > 0; k2--) {
        int p = g_rd[k2].parent;
        g_rd[p].total_text += g_rd[k2].total_text;
        g_rd[p].total_link += g_rd[k2].total_link;
        g_rd[p].total_paras += g_rd[k2].total_paras;
    }

    int best = -1;
    float best_score = 0;
    for (int k2 = 1; k2 < g_rd_count; k2++) {
        rd_node_t *nd = &g_rd[k2];
        if (nd->score <= 0 || nd->total_text < 300 || nd->bias <= -100) continue;
        float ld = nd->total_text ? (float)nd->total_link / (float)nd->total_text : 1.0f;
        float s = (nd->score + (float)nd->bias) * (1.0f - ld);
        if (s > best_score) { best_score = s; best = k2; }
    }
    return best;
}

static void html_escape_into(char *out, size_t cap, const char *in) {
    size_t o = 0;
    for (; *in && o + 6 < cap; in++) {
        if (*in == '<') { memcpy(out + o, "&lt;", 4); o += 4; }
        else if (*in == '>') { memcpy(out + o, "&gt;", 4); o += 4; }
        else if (*in == '&') { memcpy(out + o, "&amp;", 5); o += 5; }
        else out[o++] = *in;
    }
    out[o] = 0;
}

/* Called for every new page: should the "Simplified view" chip show? */
static void reader_check_offer(page_t *pg) {
    pg->reader_offer = false;
    if (!pg->html) return;
    int best = rd_analyze(pg->html, pg->html_len);
    if (best < 0) return;
    const rd_node_t *nd = &g_rd[best];
    int all = g_rd[0].total_text;
    if (nd->total_text >= 1000 && nd->total_paras >= 4 && all >= nd->total_text + nd->total_text / 4) {
        pg->reader_offer = true;
        printf("[mini_browser] reader: offered (%d of %d characters, %d paragraphs)\n",
               nd->total_text, all, nd->total_paras);
    }
}

/* The simplified page for the page on screen, or NULL. */
__attribute__((noinline)) static page_t *reader_build(const page_t *src) {
    static char title[400];
    if (!src || !src->html) return NULL;
    int best = rd_analyze(src->html, src->html_len);
    if (best < 0) return NULL;
    const rd_node_t *nd = &g_rd[best];
    size_t len = nd->end > nd->start ? nd->end - nd->start : 0;
    char *body = (char *)malloc(len + 1);
    if (!body) return NULL;
    memcpy(body, src->html + nd->start, len);
    body[len] = 0;
    /* Blank navigation, footers and link-heavy widgets inside the article. */
    for (int k = best + 1; k < g_rd_count; k++) {
        const rd_node_t *c = &g_rd[k];
        if (c->start < nd->start || c->end > nd->end || c->end <= c->start) continue;
        float ld = c->total_text ? (float)c->total_link / (float)c->total_text : 0.0f;
        if (c->bias <= -100 || (c->bias <= -25 && ld > 0.4f) || (ld > 0.7f && c->total_text > 40))
            memset(body + (c->start - nd->start), ' ', c->end - c->start);
    }
    html_escape_into(title, sizeof title, src->title[0] ? src->title : "Simplified view");
    size_t cap = len + 2 * strlen(title) + 160;
    char *html = (char *)malloc(cap);
    if (!html) { free(body); return NULL; }
    snprintf(html, cap, "<html><head><title>%s</title></head><body><h1>%s</h1>%s</body></html>",
             title, title, body);
    free(body);
    page_t *pg = html_to_page(html, src->base);
    free(html);
    if (pg) printf("[mini_browser] reader: built from <%s>, %d characters, %d paragraphs\n",
                   nd->tag, nd->total_text, nd->total_paras);
    return pg;
}

/* WHY+V: simplified view of the page, or back to the full page. */
__attribute__((noinline)) static void browser_toggle_reader(browser_t *b) {
    if (b->view == VIEW_READER) {
        browser_return_from_local_page(b);
        return;
    }
    if (b->view != VIEW_WEB || !b->page || !b->page->html) {
        browser_set_status(b, "NO SIMPLIFIED VIEW HERE", 1500);
        return;
    }
    page_t *pg = reader_build(b->page);
    if (!pg || !pg->text || !pg->text[0]) {
        free_page(pg);
        browser_set_status(b, "NO ARTICLE FOUND ON THIS PAGE", 1800);
        printf("[mini_browser] reader: no article found\n");
        return;
    }
    b->reader_chip_hidden = true;
    browser_show_local_page(b, pg, VIEW_READER, "reader:");
    b->last_http_status = 0;
    bg_start(b);                                  /* the article's images */
    printf("[mini_browser] reader: opened\n");
    browser_log_content(b->content_wrapped);
}

/* The "Simplified view" chip at the bottom of a long article. */
static void browser_render_reader_chip(browser_t *b) {
    static char line[96];
    SDL_Renderer *ren = b->ren;
    const char *label = "Show simplified view  WHY+V";
    int w = (int)strlen(label) * 12 + 28, h = CH_H + 16;
    int x = (VIEW_W - w) / 2, y = VIEW_H - h - 14;
    SDL_FRect box = { (float)x, (float)y, (float)w, (float)h };
    SDL_SetRenderDrawColor(ren, 0x18, 0x28, 0x3C, 255);
    SDL_RenderFillRect(ren, &box);
    SDL_SetRenderDrawColor(ren, 0x64, 0xEF, 0xFE, 255);
    SDL_RenderRect(ren, &box);
    colored_text(line, sizeof line, 0xE5E7EB, label);
    draw_text(ren, x + 14, y + 8, line, w - 20);
}

/* ---------- 4.4: saved pages (WHY+P) ----------
 * The text of the page (or of its simplified view) is saved to SAVED_DIR
 * for reading offline; the Downloads page (WHY+D) lists the saved pages. */

/* Page text without formatting markers; image lines become "[image]". */
static size_t page_plain_text(const char *in, char *out, size_t cap) {
    size_t o = 0, i = 0, len = strlen(in);
    while (i < len && o + 8 < cap) {
        const char *line = in + i;
        const char *nl = strchr(line, '\n');
        size_t ll = nl ? (size_t)(nl - line) : strlen(line);
        int idx = -1;
        if (is_image_marker_line(line, (int)ll, &idx)) {
            o += (size_t)snprintf(out + o, cap - o, "[image]");
            i += ll;
        } else {
            size_t end = i + ll;
            while (i < end && o + 5 < cap) {
                size_t at = i;
                unsigned cp = utf8_next(in, end, &i);
                if (cp == 0) { i = end; break; }
                if ((cp < 0x20 && cp != '\t') || is_format_marker(cp)) continue;
                memcpy(out + o, in + at, i - at);
                o += i - at;
            }
        }
        if (nl && o + 1 < cap) { out[o++] = '\n'; i++; }
        else if (!nl) break;
    }
    out[o] = 0;
    return o;
}

__attribute__((noinline)) static void browser_save_page(browser_t *b) {
    static char name[64], path[128], line[URL_MAX + 200];
    if (!b->page || !b->page->text || (b->view != VIEW_WEB && b->view != VIEW_READER)) {
        browser_set_status(b, "NOTHING TO SAVE HERE", 1500);
        return;
    }
    const char *url = b->view == VIEW_READER ? b->view_return_url : b->url_buf;
    const char *title = b->page->title[0] ? b->page->title : url;
    size_t cap = strlen(b->page->text) + 1024;
    char *text = (char *)malloc(cap);
    if (!text) return;
    page_plain_text(b->page->text, text, cap);

    /* The list, newest first.  Saving a page again replaces its earlier
     * copy; entries whose file is gone are dropped. */
    static char lines[SAVED_LIST_MAX][URL_MAX + 200];
    static char reuse[128];
    int count = 0;
    reuse[0] = 0;
    FILE *l = fopen(SAVED_LIST, "r");
    if (l) {
        while (count < SAVED_LIST_MAX - 1 && fgets(lines[count], sizeof lines[count], l)) {
            char *e = lines[count];
            e[strcspn(e, "\r\n")] = 0;
            char *t1 = strchr(e, '\t');
            char *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
            if (!t1) continue;
            *t1 = 0;
            bool keep = file_exists(e);
            if (keep && t2 && !strcmp(t2 + 1, url) && !reuse[0]) {
                snprintf(reuse, sizeof reuse, "%s", e);
                keep = false;
            }
            *t1 = '\t';
            if (keep) count++;
        }
        fclose(l);
    }

    mkdir_p(SAVED_DIR);
    if (reuse[0]) {
        snprintf(path, sizeof path, "%s", reuse);
        const char *nm = strchr(path, ']');
        snprintf(name, sizeof name, "%s", nm ? nm + 1 : path);
    } else {
        download_sanitize(title, strlen(title), name, 48);
        if (strlen(name) + 5 < sizeof name) strcat(name, ".txt");
        snprintf(path, sizeof path, SAVED_DIR "%s", name);
        for (int i = 2; i < 100 && file_exists(path); i++) {
            name[strlen(name) - 4] = 0;
            snprintf(path, sizeof path, SAVED_DIR "%.40s-%d.txt", name, i);
            strcat(name, ".txt");
        }
    }
    FILE *f = fopen(path, "w");
    bool ok = f != NULL;
    if (f) {
        if (fprintf(f, "%s\n%s\n\n%s\n", title, url, text) < 0) ok = false;
        if (fclose(f) != 0) ok = false;
    }
    free(text);
    if (!ok) {
        remove(path);
        browser_set_status(b, "COULD NOT SAVE (FLASH FULL?)", 2000);
        printf("[mini_browser] saved page: failed %s\n", path);
        return;
    }
    remove(SAVED_LIST);
    l = fopen(SAVED_LIST, "w");
    if (l) {
        snprintf(line, sizeof line, "%s\t%s\t%s", path, title, url);
        for (char *c = line + strlen(path) + 1; *c; c++) if (*c == '\n') *c = ' ';
        fprintf(l, "%s\n", line);
        for (int i = 0; i < count; i++) fprintf(l, "%s\n", lines[i]);
        fclose(l);
    }
    struct stat st;
    long size = stat(path, &st) == 0 ? (long)st.st_size : 0;
    printf("[mini_browser] saved page: %s (%ld bytes%s)\n", path, size, reuse[0] ? ", replaced" : "");
    snprintf(line, sizeof line, "SAVED FOR OFFLINE: %s", name);
    browser_set_status(b, line, 2500);
}

/* "saved:N" (from the Downloads page): show saved page N. */
__attribute__((noinline)) static void browser_open_saved(browser_t *b, int number) {
    static char line[URL_MAX + 200];
    FILE *l = fopen(SAVED_LIST, "r");
    int n = 0;
    bool found = false;
    if (l) {
        while (fgets(line, sizeof line, l)) {
            line[strcspn(line, "\r\n")] = 0;
            if (line[0] && ++n == number) { found = true; break; }
        }
        fclose(l);
    }
    char *tab = found ? strchr(line, '\t') : NULL;
    if (!tab) {
        browser_set_status(b, "SAVED PAGE NOT FOUND", 1500);
        return;
    }
    *tab = 0;
    FILE *f = fopen(line, "r");
    if (!f) {
        browser_set_status(b, "SAVED PAGE WAS DELETED", 1500);
        return;
    }
    page_t *pg = (page_t *)calloc(1, sizeof(page_t));
    size_t cap = 256 * 1024;
    char *text = (char *)malloc(cap);
    if (!pg || !text) { free(pg); free(text); fclose(f); return; }
    size_t got = fread(text, 1, cap - 1, f);
    fclose(f);
    text[got] = 0;
    /* Keep the line breaks of the text file; other control characters
     * (and the marker range) are made harmless line by line. */
    for (char *ln = text; ln && *ln;) {
        char *nl = strchr(ln, '\n');
        if (nl) *nl = 0;
        sanitize_text_inplace(ln);
        if (!nl) break;
        *nl = '\n';
        ln = nl + 1;
    }
    char *t2 = strchr(tab + 1, '\t');
    if (t2) *t2 = 0;
    snprintf(pg->title, sizeof pg->title, "Saved: %s", tab + 1);
    snprintf(pg->base, URL_MAX, "saved:");
    pg->text = text;
    browser_show_local_page(b, pg, VIEW_SAVED, "saved:");
    b->last_http_status = 0;
    printf("[mini_browser] opened saved page: %s (%u bytes)\n", line, (unsigned)got);
    browser_log_content(b->content_wrapped);
}

/* ---------- 4.4: share (WHY+U) ----------
 * The address as a QR code on screen (for a phone camera) and as a line on
 * the USB serial port ("SHARE <url>"). */
static uint8_t g_qr[qrcodegen_BUFFER_LEN_FOR_VERSION(15)];
static uint8_t g_qr_tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(15)];
static char g_share_url[URL_MAX];
static bool g_qr_ok;

static const char *browser_page_url(const browser_t *b) {
    if (b->view == VIEW_WEB) return b->url_buf;
    if (b->view == VIEW_READER && b->view_return_url[0]) return b->view_return_url;
    return NULL;
}

static void browser_share(browser_t *b) {
    const char *url = browser_page_url(b);
    if (!url || !is_http_scheme(url)) {
        browser_set_status(b, "NOTHING TO SHARE HERE", 1500);
        return;
    }
    snprintf(g_share_url, sizeof g_share_url, "%s", url);
    g_qr_ok = qrcodegen_encodeText(g_share_url, g_qr_tmp, g_qr, qrcodegen_Ecc_MEDIUM, 1, 15,
                                   qrcodegen_Mask_AUTO, true);
    printf("[mini_browser] share: %s (QR %s)\n", g_share_url, g_qr_ok ? "ok" : "too long");
    printf("SHARE %s\n", g_share_url);
    fflush(stdout);
    b->overlay = OVERLAY_SHARE;
}

__attribute__((noinline)) static void browser_render_share(browser_t *b) {
    static char line[URL_MAX + 64];
    SDL_Renderer *ren = b->ren;
    draw_ui(ren, "Share this page - any key returns");
    int top = PAD_TOP + 8;
    if (g_qr_ok) {
        int size = qrcodegen_getSize(g_qr);
        int quiet = 4;
        int scale = (VIEW_W - 160) / (size + 2 * quiet);
        if (scale > 12) scale = 12;
        int px = (size + 2 * quiet) * scale;
        int x0 = (VIEW_W - px) / 2;
        SDL_FRect bg = { (float)x0, (float)top, (float)px, (float)px };
        SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
        SDL_RenderFillRect(ren, &bg);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        for (int yy = 0; yy < size; yy++) {
            for (int xx = 0; xx < size; xx++) {
                if (!qrcodegen_getModule(g_qr, xx, yy)) continue;
                int run = xx;
                while (run + 1 < size && qrcodegen_getModule(g_qr, run + 1, yy)) run++;
                SDL_FRect m = { (float)(x0 + (quiet + xx) * scale), (float)(top + (quiet + yy) * scale),
                                (float)((run - xx + 1) * scale), (float)scale };
                SDL_RenderFillRect(ren, &m);
                xx = run;
            }
        }
        top += px + 14;
    } else {
        colored_text(line, sizeof line, 0xFFFB96, "The address is too long for a QR code.");
        draw_text(ren, PAD_LR, top, line, VIEW_W - 2 * PAD_LR);
        top += 2 * (CH_H + LINE_SPACING);
    }
    colored_text(line, sizeof line, 0xE5E7EB, g_share_url);
    top += draw_text_ex(ren, PAD_LR, top, line, strlen(line), VIEW_W - 2 * PAD_LR, NULL) + 8;
    colored_text(line, sizeof line, 0x9CA3AF, "Scan it with a phone, or read it on the USB serial port:");
    top += draw_text_ex(ren, PAD_LR, top, line, strlen(line), VIEW_W - 2 * PAD_LR, NULL);
    colored_text(line, sizeof line, 0x9CA3AF, "a line \"SHARE <address>\" was sent.");
    draw_text(ren, PAD_LR, top, line, VIEW_W - 2 * PAD_LR);
}

/* ---------- 4.4: lock in the bar ---------- */

static void draw_bar_lock(SDL_Renderer *r) {
    const int x = VIEW_W - 22, y = 4;
    SDL_SetRenderDrawColor(r, 0x34, 0xD3, 0x99, 255);
    SDL_FRect body = { (float)x, (float)(y + 7), 14.0f, 10.0f };
    SDL_RenderFillRect(r, &body);
    SDL_FRect shackle_l = { (float)(x + 2), (float)(y + 2), 2.0f, 6.0f };
    SDL_FRect shackle_r = { (float)(x + 10), (float)(y + 2), 2.0f, 6.0f };
    SDL_FRect shackle_t = { (float)(x + 2), (float)y, 10.0f, 2.0f };
    SDL_RenderFillRect(r, &shackle_l);
    SDL_RenderFillRect(r, &shackle_r);
    SDL_RenderFillRect(r, &shackle_t);
    SDL_SetRenderDrawColor(r, 0x11, 0x18, 0x26, 255);
    SDL_FRect hole = { (float)(x + 6), (float)(y + 10), 2.0f, 4.0f };
    SDL_RenderFillRect(r, &hole);
}

__attribute__((noinline)) static void browser_fetch(browser_t *b) {
    b->need_fetch = false;
    b->overlay = (b->overlay == OVERLAY_IMAGE) ? OVERLAY_NONE : b->overlay;
    decoded_image_release(&g_viewer_image);
    browser_reset_input(b);

    /* How we got here: Back/Forward (may use the cache), Reload, or new. */
    const bool bf_navigation = b->bf_navigation;
    const bool history_navigation = b->history_navigation;
    const int hist_target = b->hist_target;
    b->bf_navigation = false;
    b->history_navigation = false;
    b->hist_target = -1;

    trim_inplace(b->url_buf);
    if (!b->url_buf[0]) return;

    /* 4.4: "saved:N" links on the Downloads page. */
    if (!strncmp(b->url_buf, "saved:", 6)) {
        int number = atoi(b->url_buf + 6);
        browser_restore_shown_url(b);
        if (number > 0) browser_open_saved(b, number);
        return;
    }

    if (b->url_buf[0] == '/' && b->url_buf[1] == '/') {
        char sch[16]; scheme_from_url(b->page ? b->page->base : "https://example.org", sch, sizeof sch);
        char tmp[URL_MAX + 16];
        snprintf(tmp, sizeof tmp, "%s:%s", sch, b->url_buf);
        if (strlen(tmp) >= sizeof(b->url_buf)) return;   /* too long: do not fetch a cut URL */
        memcpy(b->url_buf, tmp, strlen(tmp) + 1);
    } else if (!has_scheme(b->url_buf)) {
        normalize_typed_url(b->url_buf);
    }
    if (!is_http_scheme(b->url_buf)) return;

    const bool was_post = b->pending_post;
    b->dirty = true;

    /* Back/Forward to a page that is still in the cache: no network. */
    int cached = (bf_navigation && !was_post) ? bfcache_find(b->url_buf) : -1;
    if (cached >= 0) {
        bfcache_store(b, shown_web_url(b), &g_fetch_meta);
        browser_commit_navigation(b, hist_target, history_navigation);
        bfcache_restore(b, cached);
        b->page_from_post = false;
        printf("[mini_browser] bfcache: hit %s\n", b->url_buf);
        visit_record(b->url_buf, b->page->title);
        browser_set_status(b, "Loaded (from cache)", 1000);
        browser_log_page(b, b->last_http_status, (unsigned)g_fetch_meta.downloaded_bytes, " (bfcache)");
        return;
    }

    if (was_post) {
#if MB_LOG_SENSITIVE
        printf("[mini_browser] POST %s body=%s\n", b->url_buf, b->post_body);
#else
        printf("[mini_browser] POST %s body=<%u bytes>\n", b->url_buf,
               (unsigned)strlen(b->post_body));
#endif
    }

    /* Loading line over the current page; Esc stops. */
    const Uint64 load_started = SDL_GetTicks();
    memset(&g_load, 0, sizeof g_load);
    g_load.b = b;
    browser_draw_loading(b, 0, 0);
    g_load.next_draw = SDL_GetTicks() + 150;

    /* fetch_url() replaces g_fetch_meta: keep the shown page's copy. */
    static fetch_meta_t shown_meta;
    shown_meta = g_fetch_meta;

    /* Reload (WHY+R, Enter) asks the server even when the cached copy is
     * still fresh; Back/Forward and new pages may use it directly. */
    g_cache_revalidate = history_navigation && !bf_navigation;

    mem_t m = {0};
    long http_status = 0;
    int rc = fetch_url(b->url_buf, was_post ? b->post_body : NULL, &m, &http_status);
    g_cache_revalidate = false;

    b->pending_post = false;
    b->post_body[0] = 0;

    if (rc != 0 && g_load.stopped) {
        /* Stopped before anything arrived: keep the page on screen. */
        browser_restore_shown_url(b);
        g_fetch_meta = shown_meta;
        printf("[mini_browser] stop: nothing received, staying on %s\n", b->url_buf);
        browser_set_status(b, "STOPPED", 1500);
        g_load.b = NULL;
        free(m.buf);
        return;
    }

    if (rc == 0 && g_resp.download) {
        /* A file, not a page: stay on the page and ask (Enter saves it). */
        snprintf(b->dl_url, sizeof b->dl_url, "%s",
                 g_fetch_meta.effective_url[0] ? g_fetch_meta.effective_url : b->url_buf);
        snprintf(b->dl_type, sizeof b->dl_type, "%s", g_fetch_meta.content_type);
        download_name_from(b->dl_url, g_resp.disposition, b->dl_name, sizeof b->dl_name);
        b->dl_size = g_resp.content_length;
        browser_restore_shown_url(b);
        g_fetch_meta = shown_meta;
        b->overlay = OVERLAY_DOWNLOAD;
        printf("[mini_browser] download: offered %s (%s, %lld bytes) from %s\n",
               b->dl_name, b->dl_type[0] ? b->dl_type : "unknown type", b->dl_size, b->dl_url);
        g_load.b = NULL;
        free(m.buf);
        return;
    }

    if (g_net_wire_bytes > 0 && m.len > 0 && (size_t)g_net_wire_bytes != m.len)
        printf("[mini_browser] net: %.0f bytes received, %u bytes after decoding\n",
               g_net_wire_bytes, (unsigned)m.len);

    const bool image = rc == 0 && http_status < 400 &&
                       (content_type_is_image(g_fetch_meta.content_type) ||
                        buffer_is_supported_image((const unsigned char *)m.buf, m.len));

    /* The page that was on screen goes to the Back/Forward cache, unless
     * the result is shown over it (a direct image). */
    const char *shown_url = shown_web_url(b);
    if (!image && shown_url && strcmp(shown_url, b->url_buf) != 0)   /* not on reload */
        bfcache_store(b, shown_url, &shown_meta);
    browser_commit_navigation(b, hist_target, history_navigation);
    b->reader_chip_hidden = false;
    g_fetch_meta.wire_bytes = g_net_wire_bytes;
    int stale = bfcache_find(b->url_buf);   /* superseded by this load */
    if (stale >= 0) bfcache_free(&g_bfcache[stale]);
    b->last_http_status = http_status;

    if (rc != 0) {
        printf("[mini_browser] fetch error %d URL='%s'\n", rc, b->url_buf);
        browser_show_error(b, rc, 0);

    } else if (http_status >= 400) {
        printf("[mini_browser] HTTP %ld URL='%s'\n", http_status, b->url_buf);
        browser_show_error(b, 0, http_status);

    } else if (image) {
        printf("[mini_browser] direct image: content-type=%s magic=%s URL=%s\n",
               g_fetch_meta.content_type[0] ? g_fetch_meta.content_type : "(none)",
               buffer_is_supported_image((const unsigned char *)m.buf, m.len) ? "yes" : "no",
               b->url_buf);
        decoded_image_release(&g_viewer_image);

        bool shown = false;
        if (display_mode_has_images()) {
            if (m.truncated) {
                printf("[mini_browser] image: download larger than %u bytes\n",
                       (unsigned)IMAGE_DOWNLOAD_MAX);
                printf("[mini_browser] image: unsupported, invalid, or outside memory budget\n");
            } else {
                /* Decode the bytes we already have: no second download. */
                shown = decode_image_buffer(b->url_buf, (const unsigned char *)m.buf, m.len,
                                            IMAGE_VIEW_MAX_W, IMAGE_VIEW_MAX_H, &g_viewer_image);
            }
        }

        if (shown) {
            b->overlay = OVERLAY_IMAGE;
            browser_set_status(b, "Image loaded", 1000);
        } else {
            browser_show_message(b, display_mode_has_images()
                ? "IMAGE UNAVAILABLE\n\nThe JPEG/PNG/GIF could not be decoded within the configured memory limits."
                : "IMAGE\n\nImages are disabled in the current display mode. Press WHY+O and select Colors + Images.");
        }

    } else {
#if MB_LOG_CONTENT
        debug_utf8("CURL", m.buf ? m.buf : "");
#endif
        /* Resolve relative links against the URL we ended on after
         * redirects, not the one that was requested. */
        page_t *pg = html_to_page(m.buf ? m.buf : "",
                                  g_fetch_meta.effective_url[0] ? g_fetch_meta.effective_url
                                                                : b->url_buf);

        if (!pg) {
            browser_show_message(b,
                "PARSE ERROR\n\n"
                "The page was downloaded but could not be converted to text.");
        } else {
#if MB_LOG_CONTENT
            debug_utf8("PAGE TEXT", pg->text);
#endif
            browser_set_page(b, pg);   /* releases the old page and its images */
            b->page_from_post = was_post;
            b->page_partial = m.truncated && g_load.stopped;
            /* 4.4: keep the HTML for reader mode (and offer it). */
            pg->html = m.buf;
            pg->html_len = m.len;
            m.buf = NULL;
            reader_check_offer(pg);
            g_fetch_meta.load_ms = (unsigned)(SDL_GetTicks() - load_started);

            char *wrapped = wrap_text(pg->text, k_max_cols);
#if MB_LOG_CONTENT
            debug_utf8("WRAPPED", wrapped);
#endif
            browser_set_content(b, wrapped);

            /* 4.3: the page is ready now; its images follow in the
             * background (bg_step from the main loop). */
            bg_start(b);
            browser_set_status(b, g_load.stopped ? "STOPPED" : "Loaded", 1000);

            if (!was_post) visit_record(b->url_buf, pg->title);
            browser_log_page(b, http_status, (unsigned)m.len, m.truncated && g_load.stopped ? " (stopped)" : "");
        }
    }

    g_load.b = NULL;
    free(m.buf);
}

__attribute__((noinline)) static void browser_compose_bar(browser_t *b) {
    char *bar = b->barline;
    size_t cap = sizeof(b->barline);
    page_t *page = b->page;

    if ((b->screenshot_pending || b->full_screenshot_pending) && page && page->title[0]) {
        /*
         * Screenshots should always contain the clean page title, even if
         * a transient "Loaded" or bookmark status is still active.
         */
        snprintf(bar, cap, "%s", page->title);
    } else if (b->status_message[0] && SDL_GetTicks() < b->status_message_until) {
        snprintf(bar, cap, "%s", b->status_message);
    } else if (b->input == INPUT_FORM && page &&
               b->form_edit_form >= 0 && b->form_edit_form < page->form_count &&
               b->form_edit_field >= 0 &&
               b->form_edit_field < page->forms[b->form_edit_form].field_count) {
        const char *name = page->forms[b->form_edit_form].fields[b->form_edit_field].name;
        snprintf(bar, cap, "%s: %.*s|%s", name, (int)b->form_edit_cursor,
                 b->form_edit_buf, b->form_edit_buf + b->form_edit_cursor);
    } else if (b->input == INPUT_LINK_NUMBER && b->link_number_len > 0) {
        snprintf(bar, cap, "%s", b->link_number_buf);
    } else if (b->input == INPUT_FIND) {
        if (g_find.query[0])
            snprintf(bar, cap, "Find: %s|   %d of %d", g_find.query,
                     g_find.count ? g_find.current + 1 : 0, g_find.count);
        else
            snprintf(bar, cap, "Find: |");
    } else if (g_find.shown && g_find.count > 0) {
        snprintf(bar, cap, "\"%s\" %d of %d - n next, N previous, Esc", g_find.query,
                 g_find.current + 1, g_find.count);
    } else if (b->input == INPUT_URL) {
        size_t curlen = strlen(b->url_buf);
        if (b->url_cursor > curlen) b->url_cursor = curlen;
        snprintf(bar, cap, "%.*s|%s", (int)b->url_cursor, b->url_buf, b->url_buf + b->url_cursor);
    } else if (page && b->sel_action >= 0 && b->sel_action < page->action_count) {
        const page_action_t *action = &page->actions[b->sel_action];
        if (action->type == ACTION_LINK && action->link_index >= 0 &&
            action->link_index < page->link_count) {
            snprintf(bar, cap, "[%d/%d]  %s", b->sel_action + 1, page->action_count,
                     page_link_href(page, action->link_index));
        } else if (action->type == ACTION_IMAGE &&
                   action->image_index >= 0 && action->image_index < page->image_count) {
            snprintf(bar, cap, "[%d/%d] Image: %s", b->sel_action + 1, page->action_count,
                     page->images[action->image_index].alt);
        } else {
            snprintf(bar, cap, "[%d/%d]", b->sel_action + 1, page->action_count);
        }
    } else if (page && page->title[0]) {
        /*
         * Normal idle state: show the page title, not HTTP 200.
         * HTTP failures are already shown as readable page content.
         * With several tabs open, "[2/3]" in front says which tab this is.
         */
        static char img[24];
        img[0] = 0;
        if (g_bg.active && g_bg.page == page)
            snprintf(img, sizeof img, "[img %d/%d] ", g_bg.done, g_bg.count);
        if (g_tab_count > 1)
            snprintf(bar, cap, "[%d/%d] %s%s", g_tab_cur + 1, g_tab_count, img, page->title);
        else
            snprintf(bar, cap, "%s%s", img, page->title);
    } else {
        snprintf(bar, cap, "%s", b->url_buf);
    }
}

static void browser_render_options(browser_t *b) {
    SDL_Renderer *ren = b->ren;
    draw_ui(ren, "Mini Browser - Options");
    int oy = PAD_TOP;
    char option_line[128];
    const int w = VIEW_W - 2 * PAD_LR;
    static const struct { display_mode_t mode; const char *label; } options[] = {
        { DISPLAY_BW,                         "1. Black & White" },
        { DISPLAY_COLORS,                     "2. Colors" },
        { DISPLAY_COLORS_IMAGE,               "3. Colors + Image (default)" },
        { DISPLAY_COLORS_IMAGES_EXPERIMENTAL, "4. Colors + 5 Images" },
    };

    draw_text(ren, PAD_LR, oy, "MINI BROWSER OPTIONS", w);
    oy += 2 * (CH_H + LINE_SPACING);
    draw_text(ren, PAD_LR, oy, "Display mode", w);
    oy += (CH_H + LINE_SPACING);

    for (size_t i = 0; i < sizeof(options) / sizeof(options[0]); i++) {
        snprintf(option_line, sizeof(option_line), "%s %s",
                 g_display_mode == options[i].mode ? "(*)" : "( )", options[i].label);
        draw_text(ren, PAD_LR, oy, option_line, w);
        oy += (CH_H + LINE_SPACING);
    }
    oy += (CH_H + LINE_SPACING);

    draw_text(ren, PAD_LR, oy, "Mode 3: 1 inline, extra images as links", w);
    oy += (CH_H + LINE_SPACING);
    draw_text(ren, PAD_LR, oy, "Mode 4: 5 inline - EXPERIMENTAL / more memory", w);
    oy += (CH_H + LINE_SPACING);
    draw_text(ren, PAD_LR, oy, "JPEG/PNG/GIF - Press 1/2/3/4, Esc to cancel", w);
}

/* Pointer to the first visible line, with the formatting state there. */
static const char *browser_visible_start(browser_t *b, text_state_t *state) {
    const char *content = b->content_wrapped;
    if (b->scroll_cache_content != content || b->scroll_lines < b->scroll_cache_line) {
        b->scroll_cache_content = content;
        b->scroll_cache_line = 0;
        b->scroll_cache_ptr = content;
        text_state_reset(&b->scroll_cache_state);
    }
    const char *p = b->scroll_cache_ptr;
    while (b->scroll_cache_line < b->scroll_lines && p && *p) {
        const char *nl = strchr(p, '\n');
        if (!nl) break;
        text_state_advance(&b->scroll_cache_state, p, (size_t)(nl - p));
        p = nl + 1;
        b->scroll_cache_line++;
    }
    b->scroll_cache_ptr = p;
    *state = b->scroll_cache_state;
    return p;
}

static void browser_render_page(browser_t *b) {
    SDL_Renderer *ren = b->ren;
    draw_ui(ren, b->barline);
    if (!b->content_wrapped) return;

    /* 4.4: matches and focus belong to this text (zoom re-wraps it). */
    if (g_find.shown && g_find.content != b->content_wrapped) find_compute(b->content_wrapped);
    if (b->sel_action >= 0 && (g_focus.content != b->content_wrapped || g_focus.action != b->sel_action))
        focus_locate(b);

    PAGE_SCALE_BEGIN();
    static text_state_t state;
    int y = PAD_TOP;
    const char *p = browser_visible_start(b, &state);
    SDL_SetRenderDrawColor(ren, 220, 220, 220, 255);
    int drawn = 0;

    while (p && *p && drawn < k_lines_per_page && y < VIEW_H - PAD_BOTTOM) {
        const char *nl = strchr(p, '\n');
        int len = nl ? (int)(nl - p) : (int)strlen(p);
        int image_index = -1;

        if (is_image_marker_line(p, len, &image_index)) {
            if (b->page && image_index >= 0 &&
                image_index < MAX_INLINE_IMAGES &&
                image_index < b->page->image_count &&
                g_inline_images[image_index].loaded) {
                int draw_w = 0, draw_h = 0;
                image_draw_size(&b->page->images[image_index],
                                &g_inline_images[image_index],
                                VIEW_W - 2 * PAD_LR, IMAGE_DRAW_MAX_H,
                                &draw_w, &draw_h);
                int image_h = draw_decoded_image(
                    ren, &b->page->images[image_index],
                    &g_inline_images[image_index],
                    image_line_x(&state, p, len, draw_w), y, VIEW_W - 2 * PAD_LR,
                    IMAGE_DRAW_MAX_H);
                y += image_h + LINE_SPACING;
            } else {
                draw_text(ren, PAD_LR, y, "[Image unavailable]", VIEW_W - 2 * PAD_LR);
                y += (CH_H + LINE_SPACING);
            }
            text_state_advance(&state, p, (size_t)len);
        } else {
            line_marks_build(b, (size_t)(p - b->content_wrapped), (size_t)len);
            draw_text_ex(ren, PAD_LR, y, p, (size_t)len, VIEW_W - 2 * PAD_LR, &state);
            g_line_mark_count = 0;
            y += (CH_H + LINE_SPACING);
        }

        drawn++;
        p = nl ? nl + 1 : NULL;
    }
    PAGE_SCALE_END();
}

/* Wrap s in a colour marker so draw_text() draws it in rgb. */
static void colored_text(char *out, size_t cap, unsigned rgb, const char *s) {
    char tmp[4 * 8];
    size_t n = 0;
    n += utf8_encode(TEXT_COLOR_START, tmp + n);
    for (int i = 5; i >= 0; i--)
        n += utf8_encode(TEXT_COLOR_NIBBLE + ((rgb >> (4 * i)) & 0x0F), tmp + n);
    tmp[n] = 0;
    char pop[4];
    size_t pn = utf8_encode(TEXT_COLOR_POP, pop);
    pop[pn] = 0;
    snprintf(out, cap, "%s%s%s", tmp, s, pop);
}

/* WHY+A tab overview (WHY2025 style). */
__attribute__((noinline)) static void browser_render_tabs(browser_t *b) {
    SDL_Renderer *ren = b->ren;
    static char line[URL_MAX + 160];
    static char label[200];
    snprintf(label, sizeof label, "Tabs (%d of %d)", g_tab_count, TAB_MAX);
    draw_ui(ren, label);

    const int row_h = 2 * (CH_H + LINE_SPACING) + 12;
    const int x = 8, w = VIEW_W - 16;
    int y = PAD_TOP + 4;
    for (int i = 0; i < g_tab_count; i++, y += row_h + 6) {
        SDL_FRect box = { (float)x, (float)y, (float)w, (float)row_h };
        SDL_SetRenderDrawColor(ren, i == b->tab_sel ? 0x18 : 0x11, i == b->tab_sel ? 0x28 : 0x18,
                               i == b->tab_sel ? 0x3C : 0x26, 255);
        SDL_RenderFillRect(ren, &box);
        if (i == b->tab_sel) SDL_SetRenderDrawColor(ren, 0x64, 0xEF, 0xFE, 255);
        else SDL_SetRenderDrawColor(ren, 0x2A, 0x34, 0x46, 255);
        SDL_RenderRect(ren, &box);

        snprintf(label, sizeof label, "%d%s %s", i + 1, i == g_tab_cur ? " *" : "  ", tab_title(b, i));
        colored_text(line, sizeof line, i == g_tab_cur ? 0xFFFB96 : 0xE5E7EB, label);
        draw_text(ren, x + 10, y + 6, line, w - 20);
        colored_text(line, sizeof line, 0x9CA3AF, omnibox_strip_scheme(tab_url(b, i)));
        draw_text(ren, x + 10 + 3 * CH_W, y + 6 + CH_H + LINE_SPACING, line, w - 20 - 3 * CH_W);
    }
    y += 8;
    colored_text(line, sizeof line, 0x9CA3AF,
                 "Up/Down select  Enter open  X close  Esc back  WHY+T new tab");
    draw_text(ren, x + 4, y, line, w - 8);
}

/* Omnibox suggestion list, under the bar (WHY2025 style). */
__attribute__((noinline)) static void browser_render_suggestions(browser_t *b) {
    SDL_Renderer *ren = b->ren;
    const int row_h = 2 * (CH_H + LINE_SPACING) + 8;
    const int x = 4, w = VIEW_W - 8;
    const int y0 = URLBAR_H + 4;
    SDL_FRect panel = { (float)x, (float)y0, (float)w, (float)(b->sugg_count * row_h + 4) };
    SDL_SetRenderDrawColor(ren, 0x11, 0x18, 0x26, 255);
    SDL_RenderFillRect(ren, &panel);
    SDL_SetRenderDrawColor(ren, 0x2A, 0x34, 0x46, 255);
    SDL_RenderRect(ren, &panel);

    static char line[URL_MAX + 160];   /* static: 16 KB app stack */
    static char label[160];
    for (int i = 0; i < b->sugg_count; i++) {
        int y = y0 + 2 + i * row_h;
        if (i == b->sugg_sel) {
            SDL_FRect sel = { (float)(x + 2), (float)y, (float)(w - 4), (float)(row_h - 2) };
            SDL_SetRenderDrawColor(ren, 0x18, 0x28, 0x3C, 255);
            SDL_RenderFillRect(ren, &sel);
            SDL_SetRenderDrawColor(ren, 0x64, 0xEF, 0xFE, 255);
            SDL_RenderRect(ren, &sel);
        }
        snprintf(label, sizeof label, "%s %s", b->sugg[i].bookmark ? "*" : ">",
                 b->sugg[i].title[0] ? b->sugg[i].title : omnibox_strip_scheme(b->sugg[i].url));
        colored_text(line, sizeof line, b->sugg[i].bookmark ? 0xFFFB96 : 0xE5E7EB, label);
        draw_text(ren, x + 10, y + 4, line, w - 20);
        colored_text(line, sizeof line, 0x9CA3AF, omnibox_strip_scheme(b->sugg[i].url));
        draw_text(ren, x + 10 + 2 * CH_W, y + 4 + CH_H + LINE_SPACING, line, w - 20 - 2 * CH_W);
    }
}

__attribute__((noinline)) static void browser_render(browser_t *b) {
    browser_compose_bar(b);
    g_bar_lock = b->overlay == OVERLAY_NONE && (b->view == VIEW_WEB || b->view == VIEW_READER) &&
                 b->input == INPUT_NONE && !strncasecmp(b->view == VIEW_READER ? b->view_return_url
                                                                              : b->url_buf, "https://", 8);

    if (b->overlay == OVERLAY_IMAGE) draw_image_viewer(b->ren, &g_viewer_image);
    else if (b->overlay == OVERLAY_OPTIONS) browser_render_options(b);
    else if (b->overlay == OVERLAY_TABS) browser_render_tabs(b);
    else if (b->overlay == OVERLAY_DOWNLOAD) { browser_render_page(b); browser_render_download(b); }
    else if (b->overlay == OVERLAY_SHARE) browser_render_share(b);
    else browser_render_page(b);

    /* 4.4: offer the simplified view on long articles (until scrolled). */
    if (b->overlay == OVERLAY_NONE && b->input == INPUT_NONE && b->view == VIEW_WEB &&
        b->page && b->page->reader_offer && !b->reader_chip_hidden && !b->screenshot_pending &&
        !b->full_screenshot_pending) {
        if (b->scroll_lines > 12) b->reader_chip_hidden = true;
        else browser_render_reader_chip(b);
    }

    if (b->input == INPUT_URL && b->sugg_count > 0 && b->overlay == OVERLAY_NONE)
        browser_render_suggestions(b);

    if (b->full_screenshot_pending) {
        b->full_screenshot_pending = false;
        screenshot_stream_full_page(b->ren, b->page, b->barline, b->content_wrapped);
        /*
         * Full-page capture reuses the 716x716 renderer as a scratch
         * surface. Do not present its final slice on the badge; redraw the
         * user's unchanged viewport on the next pass.
         */
        b->dirty = true;
        return;
    }

    if (b->screenshot_pending) {
        b->screenshot_pending = false;
        screenshot_stream_renderer(b->ren);
    }

    SDL_RenderPresent(b->ren);
}

static void browser_insert_char(char *buf, size_t cap, size_t *cursor, char c) {
    size_t len = strlen(buf);
    if (*cursor > len) *cursor = len;
    if (len + 1 >= cap) return;
    memmove(&buf[*cursor + 1], &buf[*cursor], len - *cursor + 1);
    buf[(*cursor)++] = c;
}

static void browser_handle_text(browser_t *b, const char *t) {
    if (b->inhibit_text_once) {
        b->inhibit_text_once = false;
        return;
    }
    if (!t) return;

    /* One TEXT_INPUT event can carry several characters (paste, IME, a
     * batched key repeat): handle all of them, not just the first. */
    for (; *t; t++) {
        unsigned char c = (unsigned char)*t;
        if (c < 32 || c > 126) continue;   /* the URL bar and fields are ASCII */

        if (b->input == INPUT_FIND) {
            size_t n = strlen(g_find.query);
            if (n + 1 < sizeof g_find.query) {
                g_find.query[n] = (char)c;
                g_find.query[n + 1] = 0;
                find_compute(b->content_wrapped);
                find_first_visible(b);
            }
        } else if (b->input == INPUT_FORM) {
            browser_insert_char(b->form_edit_buf, sizeof(b->form_edit_buf), &b->form_edit_cursor, (char)c);
        } else if (b->input == INPUT_URL) {
            browser_insert_char(b->url_buf, sizeof(b->url_buf), &b->url_cursor, (char)c);
            b->sel_action = -1;
            b->sugg_sel = -1;
            browser_update_suggestions(b);
        } else if (b->page && b->page->action_count > 0 && c >= '0' && c <= '9') {
            if (b->input != INPUT_LINK_NUMBER) {
                b->input = INPUT_LINK_NUMBER;
                b->link_number_len = 0;
                b->link_number_buf[0] = 0;
            }
            if (b->link_number_len < (int)sizeof(b->link_number_buf) - 1) {
                b->link_number_buf[b->link_number_len++] = (char)c;
                b->link_number_buf[b->link_number_len] = 0;
            }
        }
    }
}

static void browser_handle_options_key(browser_t *b, SDL_Scancode sc) {
    display_mode_t selected = g_display_mode;
    bool changed = false;

    if (sc == SDL_SCANCODE_1) { selected = DISPLAY_BW; changed = true; }
    else if (sc == SDL_SCANCODE_2) { selected = DISPLAY_COLORS; changed = true; }
    else if (sc == SDL_SCANCODE_3) { selected = DISPLAY_COLORS_IMAGE; changed = true; }
    else if (sc == SDL_SCANCODE_4) { selected = DISPLAY_COLORS_IMAGES_EXPERIMENTAL; changed = true; }
    else if (sc == SDL_SCANCODE_ESCAPE) {
        b->overlay = g_viewer_image.loaded ? OVERLAY_IMAGE : OVERLAY_NONE;
        b->inhibit_text_once = true;
        return;
    }

    /* Ignore other keys while the modal options page is open. */
    if (!changed) return;

    g_display_mode = selected;
    b->overlay = OVERLAY_NONE;
    image_release_all();
    bfcache_clear();   /* cached pages were parsed for the old mode */
    if (is_http_scheme(b->url_buf)) {
        /* Re-fetch so <img> is re-parsed for the new mode. A POST result is
         * not re-submitted: the page is re-requested with GET as before. */
        b->need_fetch = true;
        b->history_navigation = true;
    }
    b->scroll_lines = 0;
    b->sel_action = -1;
    char message[64];
    snprintf(message, sizeof(message), "MODE: %s", display_mode_name(g_display_mode));
    browser_set_status(b, message, 1500);
    printf("[mini_browser] display mode: %s\n", display_mode_name(g_display_mode));
    b->inhibit_text_once = true;
}

static void browser_toggle_bookmark(browser_t *b) {
    /* Only real pages: not "bookmarks:", "page-info:", a URL being typed or
     * a failed load. */
    if (b->input == INPUT_URL || b->view != VIEW_WEB || !b->page || !is_http_scheme(b->url_buf)) {
        browser_set_status(b, "CANNOT BOOKMARK THIS PAGE", 1500);
        return;
    }
    const char *title = b->page->title[0] ? b->page->title : b->url_buf;
    int result = bookmark_toggle(b->url_buf, title);

    if (result < 0) {
        browser_set_status(b, "BOOKMARKS FULL", 1500);
        printf("[mini_browser] bookmarks full (%d)\n", BOOKMARK_MAX);
        return;
    }

    bookmark_save();
    browser_set_status(b, result ? "BOOKMARK ADDED" : "BOOKMARK REMOVED", 1500);
    printf("[mini_browser] %s bookmark: %s\n", result ? "added" : "removed", b->url_buf);
}

/* WHY+<key>.  Returns after handling; every command swallows its text. */
static void browser_handle_accel_key(browser_t *b, SDL_Scancode sc) {
    b->inhibit_text_once = true;

    switch (sc) {
        case SDL_SCANCODE_E:
            browser_reset_input(b);
            snprintf(b->url_before_edit, sizeof(b->url_before_edit), "%s", b->url_buf);
            snprintf(b->url_buf, sizeof(b->url_buf), "https://");
            b->url_cursor = strlen(b->url_buf);
            b->sel_action = -1;
            b->input = INPUT_URL;
            break;

        case SDL_SCANCODE_C:
            browser_reset_input(b);
            snprintf(b->url_before_edit, sizeof(b->url_before_edit), "%s", b->url_buf);
            b->url_cursor = strlen(b->url_buf);
            b->sel_action = -1;
            b->input = INPUT_URL;
            break;

        case SDL_SCANCODE_T:   /* new tab */
            tab_new(b);
            break;

        case SDL_SCANCODE_W:   /* close tab */
            tab_close(b, g_tab_cur);
            break;

        case SDL_SCANCODE_TAB: /* next tab */
            if (g_tab_count > 1) tab_switch(b, (g_tab_cur + 1) % g_tab_count);
            break;

        case SDL_SCANCODE_A:   /* tab overview */
            browser_reset_input(b);
            b->tab_sel = g_tab_cur;
            b->overlay = OVERLAY_TABS;
            printf("[mini_browser] tab: overview, %d tabs, current %d\n", g_tab_count, g_tab_cur + 1);
            break;

        case SDL_SCANCODE_1: case SDL_SCANCODE_2: case SDL_SCANCODE_3:
        case SDL_SCANCODE_4: case SDL_SCANCODE_5:   /* WHY+n: tab n */
            tab_switch(b, (int)(sc - SDL_SCANCODE_1));
            break;

        case SDL_SCANCODE_L:   /* omnibox, empty (Chrome: Ctrl+L) */
            browser_reset_input(b);
            snprintf(b->url_before_edit, sizeof(b->url_before_edit), "%s", b->url_buf);
            b->url_buf[0] = 0;
            b->url_cursor = 0;
            b->sel_action = -1;
            b->input = INPUT_URL;
            break;

        case SDL_SCANCODE_Y: { /* HISTORY */
            if (b->view == VIEW_HISTORY) {
                browser_return_from_local_page(b);
            } else {
                page_t *pg = history_to_page();
                if (pg) {
                    browser_show_local_page(b, pg, VIEW_HISTORY, "history:");
                    b->last_http_status = 0;
                    printf("[mini_browser] opened history: %d entries\n", g_visit_count);
                    browser_log_content(b->content_wrapped);   /* for the regression suite */
                }
            }
            break;
        }

        case SDL_SCANCODE_D: { /* 4.3: DOWNLOADS */
            if (b->view == VIEW_DOWNLOADS) {
                browser_return_from_local_page(b);
            } else {
                int count = 0;
                page_t *pg = downloads_to_page(&count);
                if (pg) {
                    browser_show_local_page(b, pg, VIEW_DOWNLOADS, "downloads:");
                    b->last_http_status = 0;
                    printf("[mini_browser] opened downloads: %d files\n", count);
                    browser_log_content(b->content_wrapped);   /* for the regression suite */
                }
            }
            break;
        }

        case SDL_SCANCODE_X:   /* clear: history / downloads / cookies + cache */
            if (b->view == VIEW_DOWNLOADS) {
                downloads_delete_all();
                page_t *pg = downloads_to_page(NULL);
                if (pg) {
                    char *wrapped = wrap_text(pg->text, k_max_cols);
                    browser_set_page(b, pg);
                    browser_set_content(b, wrapped);
                }
                browser_set_status(b, "DOWNLOADS DELETED", 1500);
            } else if (b->view == VIEW_PAGE_INFO) {
                cookie_clear_all();
                cache_clear_all();
                bfcache_clear();
                browser_set_status(b, "COOKIES AND CACHE CLEARED", 2000);
            } else if (b->view == VIEW_HISTORY) {
                visit_clear();
                page_t *pg = history_to_page();
                if (pg) {
                    char *wrapped = wrap_text(pg->text, k_max_cols);
                    browser_set_page(b, pg);
                    browser_set_content(b, wrapped);
                }
                browser_set_status(b, "HISTORY CLEARED", 1500);
            }
            break;

        case SDL_SCANCODE_H:
            browser_navigate(b, HOME_URL);
            break;

        case SDL_SCANCODE_R:
            if (b->view != VIEW_WEB) {
                browser_return_from_local_page(b);
            } else {
                b->history_navigation = true;
                b->need_fetch = true;
            }
            break;

        case SDL_SCANCODE_I: { /* PAGE INFORMATION */
            if (b->view == VIEW_PAGE_INFO) {
                browser_return_from_local_page(b);
            } else if (b->page && b->view == VIEW_WEB && is_http_scheme(b->url_buf)) {
                page_t *pg = page_info_to_page(b->page, b->url_buf, b->last_http_status);
                if (pg) browser_show_local_page(b, pg, VIEW_PAGE_INFO, "page-info:");
            }
            break;
        }

        case SDL_SCANCODE_M: { /* SHOW BOOKMARKS */
            page_t *pg = bookmarks_to_page();
            if (pg) {
                /* WHY+M again inside bookmarks keeps the original return URL. */
                if (b->view == VIEW_PAGE_INFO) b->view = VIEW_WEB;
                browser_show_local_page(b, pg, VIEW_BOOKMARKS, "bookmarks:");
                b->last_http_status = 0;
                printf("[mini_browser] opened bookmarks: %d entries\n", g_bookmark_count);
            }
            break;
        }

        case SDL_SCANCODE_K: /* BOOKMARK current page (4.4: was WHY+F) */
            browser_toggle_bookmark(b);
            break;

        case SDL_SCANCODE_V: /* 4.4: SIMPLIFIED VIEW (reader mode) */
            browser_toggle_reader(b);
            break;

        case SDL_SCANCODE_P: /* 4.4: SAVE PAGE for offline reading */
            browser_save_page(b);
            break;

        case SDL_SCANCODE_U: /* 4.4: SHARE (QR code + serial) */
            browser_share(b);
            break;

        case SDL_SCANCODE_EQUALS:      /* 4.4: zoom in (WHY+ =/+) */
        case SDL_SCANCODE_KP_PLUS:
            browser_set_zoom(b, g_page_scale + 1);
            break;
        case SDL_SCANCODE_MINUS:       /* zoom out */
        case SDL_SCANCODE_KP_MINUS:
            browser_set_zoom(b, g_page_scale - 1);
            break;
        case SDL_SCANCODE_0:           /* zoom 100% */
            browser_set_zoom(b, 2);
            break;

        case SDL_SCANCODE_F: /* 4.4: FIND IN PAGE */
            if (b->content_wrapped && b->overlay == OVERLAY_NONE) {
                browser_reset_input(b);
                b->input = INPUT_FIND;
                g_find.shown = true;
                find_compute(b->content_wrapped);   /* the previous query, if any */
                find_first_visible(b);
                printf("[mini_browser] find: opened\n");
            }
            break;

        case SDL_SCANCODE_B: { /* BACK */
            if (b->overlay == OVERLAY_IMAGE) {
                b->overlay = OVERLAY_NONE;
                decoded_image_release(&g_viewer_image);
            } else if (!browser_return_from_local_page(b)) {
                static char prev[URL_MAX];
                int target = history_peek(-1, prev);
                if (target >= 0) {
                    browser_navigate(b, prev);
                    b->history_navigation = true;
                    b->bf_navigation = true;
                    b->hist_target = target;
                }
            }
            break;
        }

        case SDL_SCANCODE_G: { /* FORWARD */
            static char next_url[URL_MAX];
            int target = history_peek(+1, next_url);
            if (target >= 0) {
                browser_navigate(b, next_url);
                b->history_navigation = true;
                b->bf_navigation = true;
                b->hist_target = target;
            }
            break;
        }

        case SDL_SCANCODE_S:
            b->screenshot_pending = true;
            printf("[mini_browser] screenshot requested; clean frame queued\n");
            break;

        case SDL_SCANCODE_Z:
            b->full_screenshot_pending = true;
            printf("[mini_browser] full-page screenshot requested; clean page queued\n");
            break;

        case SDL_SCANCODE_O:
            b->overlay = OVERLAY_OPTIONS;
            printf("[mini_browser] options opened; current mode=%s\n",
                   display_mode_name(g_display_mode));
            break;

        case SDL_SCANCODE_Q:
            b->running = false;
            break;

        default:
            break;
    }
}

static void browser_activate(browser_t *b) {
    page_t *page = b->page;
    int action_index = b->sel_action;
    if (b->input == INPUT_LINK_NUMBER)
        action_index = atoi(b->link_number_buf) - 1;
    browser_reset_input(b);

    /* Build the target in a scratch buffer: a failed activation (URL too
     * long, unsupported form) must not overwrite the current URL. */
    static char target[URL_MAX];
    static char body[POST_BODY_MAX];
    target[0] = 0;
    body[0] = 0;
    activate_result_t result = activate_page_action(
        page, action_index, target, sizeof(target),
        &b->form_edit_form, &b->form_edit_field,
        b->form_edit_buf, sizeof(b->form_edit_buf),
        &b->form_edit_cursor,
        body, sizeof(body));

    if (result == ACTIVATE_NAVIGATE || result == ACTIVATE_POST) {
        memcpy(b->url_buf, target, sizeof(b->url_buf));
        memcpy(b->post_body, body, sizeof(b->post_body));
    }

    switch (result) {
        case ACTIVATE_EDIT_FIELD:
            b->input = INPUT_FORM;
            break;
        case ACTIVATE_NAVIGATE:
            b->pending_post = false;
            b->post_body[0] = 0;
            b->history_navigation = false;
            b->need_fetch = true;
            break;
        case ACTIVATE_POST:
            b->pending_post = true;
            b->history_navigation = false;
            b->need_fetch = true;
            break;
        case ACTIVATE_IMAGE: {
            int image_index = page->actions[action_index].image_index;
            if (image_index >= 0 && image_index < page->image_count && display_mode_has_images()) {
                decoded_image_release(&g_viewer_image);
                if (load_image_url(page_image_src(page, image_index),
                                   IMAGE_VIEW_MAX_W, IMAGE_VIEW_MAX_H, &g_viewer_image))
                    b->overlay = OVERLAY_IMAGE;
                else
                    browser_set_status(b, "IMAGE UNAVAILABLE", 2000);
            }
            break;
        }
        case ACTIVATE_FORM_UNSUPPORTED:
            browser_set_status(b, "FORM METHOD/ENCODING NOT SUPPORTED", 2000);
            break;
        case ACTIVATE_URL_TOO_LONG:
            browser_set_status(b, "FORM URL TOO LONG", 2000);
            break;
        default:
            break;
    }
}

static void browser_commit_form_field(browser_t *b) {
    page_t *page = b->page;
    if (page && b->form_edit_form >= 0 && b->form_edit_form < page->form_count &&
        b->form_edit_field >= 0 &&
        b->form_edit_field < page->forms[b->form_edit_form].field_count) {
        form_field_t *field = &page->forms[b->form_edit_form].fields[b->form_edit_field];
        snprintf(field->value, sizeof(field->value), "%s", b->form_edit_buf);
        sanitize_text_inplace(field->value);
        if (refresh_page_text(page)) {
            char *wrapped = wrap_text(page->text, k_max_cols);
            if (wrapped) {
                int keep_scroll = b->scroll_lines;
                browser_set_content(b, wrapped);
                b->scroll_lines = keep_scroll;
                browser_clamp_scroll(b);
            }
        }
    }
    browser_reset_input(b);
}

/* Edit keys shared by the URL bar and form fields. */
static bool browser_edit_key(char *buf, size_t *cursor, SDL_Scancode sc) {
    size_t len = strlen(buf);
    if (*cursor > len) *cursor = len;
    switch (sc) {
        case SDL_SCANCODE_BACKSPACE:
            if (*cursor > 0) {
                memmove(&buf[*cursor - 1], &buf[*cursor], len - *cursor + 1);
                (*cursor)--;
            }
            return true;
        case SDL_SCANCODE_DELETE:
            if (*cursor < len) memmove(&buf[*cursor], &buf[*cursor + 1], len - *cursor);
            return true;
        case SDL_SCANCODE_LEFT:  if (*cursor > 0) (*cursor)--; return true;
        case SDL_SCANCODE_RIGHT: if (*cursor < len) (*cursor)++; return true;
        case SDL_SCANCODE_HOME:  *cursor = 0; return true;
        case SDL_SCANCODE_END:   *cursor = len; return true;
        default: return false;
    }
}

static void browser_handle_key(browser_t *b, const SDL_KeyboardEvent *key) {
    SDL_Scancode sc = key->scancode;

    /* Phase 3 Fix 15 options menu consumes ordinary 1/2/3/4/Escape. */
    if (b->overlay == OVERLAY_OPTIONS) {
        browser_handle_options_key(b, sc);
        return;
    }

    /* 4.4: the share screen closes on any key (WHY+S still takes a
     * screenshot of it). */
    if (b->overlay == OVERLAY_SHARE && sc != SC_ACCELERATOR &&
        !(b->accel_down && sc == SDL_SCANCODE_S)) {
        b->inhibit_text_once = true;
        b->overlay = OVERLAY_NONE;
        return;
    }

    /* 4.3: "Download this file?": Enter saves, Esc cancels. */
    if (b->overlay == OVERLAY_DOWNLOAD && sc != SC_ACCELERATOR && !b->accel_down) {
        b->inhibit_text_once = true;
        if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER || sc == SDL_SCANCODE_Y) {
            b->overlay = OVERLAY_NONE;
            browser_download(b);
        } else if (sc == SDL_SCANCODE_ESCAPE || sc == SDL_SCANCODE_N) {
            b->overlay = OVERLAY_NONE;
            printf("[mini_browser] download: cancelled %s\n", b->dl_name);
            browser_set_status(b, "DOWNLOAD CANCELLED", 1500);
        }
        return;
    }

    /* Tab overview: Up/Down, Enter, digits, X/Delete closes, Esc. */
    if (b->overlay == OVERLAY_TABS && sc != SC_ACCELERATOR && !b->accel_down) {
        b->inhibit_text_once = true;
        if (sc == SDL_SCANCODE_UP && b->tab_sel > 0) b->tab_sel--;
        else if (sc == SDL_SCANCODE_DOWN && b->tab_sel < g_tab_count - 1) b->tab_sel++;
        else if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER) {
            b->overlay = OVERLAY_NONE;
            tab_switch(b, b->tab_sel);
        } else if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_5) {
            b->overlay = OVERLAY_NONE;
            tab_switch(b, (int)(sc - SDL_SCANCODE_1));
        } else if (sc == SDL_SCANCODE_X || sc == SDL_SCANCODE_DELETE || sc == SDL_SCANCODE_BACKSPACE) {
            tab_close(b, b->tab_sel);
            if (b->tab_sel >= g_tab_count) b->tab_sel = g_tab_count - 1;
        } else if (sc == SDL_SCANCODE_ESCAPE) {
            b->overlay = OVERLAY_NONE;
        }
        return;
    }

    /* Track accelerator press/release */
    if (sc == SC_ACCELERATOR) {
        b->accel_down = true;
        b->inhibit_text_once = true;
        return;
    }

    /* Special one-shot keys -> direct navigate */
    static const struct { SDL_Scancode sc; const char *url; } specials[] = {
        { SC_SPECIAL_124, SPECIAL_URL_124 }, { SC_SPECIAL_125, SPECIAL_URL_125 },
        { SC_SPECIAL_126, SPECIAL_URL_126 }, { SC_SPECIAL_127, SPECIAL_URL_127 },
        { SC_SPECIAL_128, SPECIAL_URL_128 }, { SC_SPECIAL_129, SPECIAL_URL_129 },
    };
    for (size_t i = 0; i < sizeof(specials) / sizeof(specials[0]); i++) {
        if (sc == specials[i].sc) {
            browser_navigate(b, specials[i].url);
            b->inhibit_text_once = true;
            return;
        }
    }

    /* Accelerator combos (E,C,H,R,F,M,B,Q,...) */
    if (b->accel_down) {
        browser_handle_accel_key(b, sc);
        return;
    }

    /* 4.4: the find bar. */
    if (b->input == INPUT_FIND) {
        size_t n = strlen(g_find.query);
        if (sc == SDL_SCANCODE_BACKSPACE || sc == SDL_SCANCODE_DELETE) {
            if (n) g_find.query[n - 1] = 0;
            find_compute(b->content_wrapped);
            find_first_visible(b);
        } else if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER) {
            b->input = INPUT_NONE;          /* keep the highlights: n / N jump */
            if (b->content_wrapped != g_find.content) find_compute(b->content_wrapped);
            printf("[mini_browser] find: '%s' %d matches\n", g_find.query, g_find.count);
            if (g_find.count == 0) {
                browser_set_status(b, "NO MATCHES", 1500);
                find_clear();
            } else if (g_find.current < 0) {
                find_first_visible(b);
            }
        } else if (sc == SDL_SCANCODE_DOWN) {
            find_step(b, +1);
        } else if (sc == SDL_SCANCODE_UP) {
            find_step(b, -1);
        } else if (sc == SDL_SCANCODE_ESCAPE) {
            b->input = INPUT_NONE;
            find_clear();
            printf("[mini_browser] find: closed\n");
        }
        return;
    }

    /* Text editing in the URL bar or a form field. */
    if (b->input == INPUT_URL || b->input == INPUT_FORM) {
        bool url = b->input == INPUT_URL;
        if (url && sc == SDL_SCANCODE_DOWN) {
            if (b->sugg_sel < b->sugg_count - 1) b->sugg_sel++;
            return;
        }
        if (url && sc == SDL_SCANCODE_UP) {
            if (b->sugg_sel >= 0) b->sugg_sel--;
            return;
        }
        if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER) {
            if (url) {
                browser_omnibox_go(b);
            } else {
                browser_commit_form_field(b);
            }
            return;
        }
        if (sc == SDL_SCANCODE_ESCAPE) {
            /* Cancel editing.  (Esc in the URL bar used to quit the app and
             * left the half-typed URL in place for WHY+R.) */
            if (url) snprintf(b->url_buf, sizeof(b->url_buf), "%s", b->url_before_edit);
            browser_reset_input(b);
            return;
        }
        if (browser_edit_key(url ? b->url_buf : b->form_edit_buf,
                             url ? &b->url_cursor : &b->form_edit_cursor, sc)) {
            if (url) {
                b->sel_action = -1;
                if (sc == SDL_SCANCODE_BACKSPACE || sc == SDL_SCANCODE_DELETE) {
                    b->sugg_sel = -1;
                    browser_update_suggestions(b);
                }
            }
        }
        return;
    }

    switch (sc) {
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER:
            if (b->page && (b->input == INPUT_LINK_NUMBER || b->sel_action >= 0)) {
                browser_activate(b);
            } else {
                b->history_navigation = true;   /* reload: no new history entry */
                b->need_fetch = true;
            }
            break;

        case SDL_SCANCODE_N:   /* 4.4: next / previous find match */
            if (g_find.shown) find_step(b, (key->mod & SDL_KMOD_SHIFT) ? -1 : +1);
            else if (b->input == INPUT_LINK_NUMBER) browser_reset_input(b);
            break;

        case SDL_SCANCODE_R:   /* error page: reload (as Chrome's Reload button) */
            if (b->error_page && b->view == VIEW_WEB) {
                printf("[mini_browser] error page: reload %s\n", b->url_buf);
                b->history_navigation = true;
                b->need_fetch = true;
            } else if (b->input == INPUT_LINK_NUMBER) {
                browser_reset_input(b);
            }
            break;

        case SDL_SCANCODE_BACKSPACE:
        case SDL_SCANCODE_DELETE:
            if (b->input == INPUT_LINK_NUMBER && b->link_number_len > 0)
                b->link_number_buf[--b->link_number_len] = 0;
            break;

        /* Scrolling */
        case SDL_SCANCODE_DOWN:
            if (!b->scroll_down_held) {
                b->scroll_down_held = true;
                b->scroll_up_held = false;
                b->scroll_lines++;
                b->scroll_repeat_at = SDL_GetTicks() + SCROLL_REPEAT_DELAY_MS;
            }
            break;
        case SDL_SCANCODE_J:
            b->scroll_lines++;
            break;
        case SDL_SCANCODE_UP:
            if (!b->scroll_up_held) {
                b->scroll_up_held = true;
                b->scroll_down_held = false;
                b->scroll_lines--;
                b->scroll_repeat_at = SDL_GetTicks() + SCROLL_REPEAT_DELAY_MS;
            }
            break;
        case SDL_SCANCODE_K:
            b->scroll_lines--;
            break;
        case SDL_SCANCODE_HOME:
            b->scroll_lines = 0;
            break;
        case SDL_SCANCODE_END:
            b->scroll_lines = b->content_lines;   /* clamped below */
            break;
        case SDL_SCANCODE_PAGEDOWN:
            b->scroll_lines += k_lines_per_page > 2 ? k_lines_per_page - 2 : 1;
            break;
        case SDL_SCANCODE_PAGEUP:
            b->scroll_lines -= 5;
            break;

        /* Link navigation */
        case SDL_SCANCODE_TAB: {
            bool shift = (key->mod & SDL_KMOD_SHIFT) != 0;
            if (b->page && b->page->action_count > 0) {
                int n = b->page->action_count;
                if (b->sel_action < 0) b->sel_action = shift ? n - 1 : 0;   /* first Tab */
                else if (shift) b->sel_action = (b->sel_action == 0) ? n - 1 : b->sel_action - 1;
                else b->sel_action = (b->sel_action + 1) % n;
                /* 4.4: ring around it, and scroll it into view. */
                focus_locate(b);
                if (g_focus.found) scroll_line_into_view(b, g_focus.line);
                printf("[mini_browser] focus: action %d of %d%s\n", b->sel_action + 1, n,
                       g_focus.found ? "" : " (not in the text)");
            }
            break;
        }

        case SDL_SCANCODE_ESCAPE:
            if (b->overlay == OVERLAY_IMAGE) {
                b->overlay = OVERLAY_NONE;
                decoded_image_release(&g_viewer_image);
            } else if (b->input == INPUT_LINK_NUMBER) {
                browser_reset_input(b);
            } else if (g_find.shown) {
                find_clear();              /* 4.4: Esc clears the find highlights */
                printf("[mini_browser] find: closed\n");
            } else if (g_bg.active) {
                bg_stop(b);                /* 4.3: stop the images, not the browser */
            } else {
                b->running = false;
            }
            break;

        /* As before: Left/Right do nothing while browsing (and keep a
         * partly typed link number). */
        case SDL_SCANCODE_LEFT:
        case SDL_SCANCODE_RIGHT:
            break;

        /* Digit keys: handled via TEXT_INPUT */
        case SDL_SCANCODE_1: case SDL_SCANCODE_2: case SDL_SCANCODE_3:
        case SDL_SCANCODE_4: case SDL_SCANCODE_5: case SDL_SCANCODE_6:
        case SDL_SCANCODE_7: case SDL_SCANCODE_8: case SDL_SCANCODE_9:
        case SDL_SCANCODE_0:
            break;

        default:
            if (b->input == INPUT_LINK_NUMBER) browser_reset_input(b);
            break;
    }
    browser_clamp_scroll(b);
}

static void browser_handle_key_up(browser_t *b, SDL_Scancode sc) {
    if (sc == SC_ACCELERATOR) {
        b->accel_down = false;
        b->inhibit_text_once = false;
    } else if (sc == SDL_SCANCODE_UP) {
        b->scroll_up_held = false;
    } else if (sc == SDL_SCANCODE_DOWN) {
        b->scroll_down_held = false;
    }
}

static void browser_handle_event(browser_t *b, const SDL_Event *ev) {
    switch (ev->type) {
        case SDL_EVENT_QUIT:
            b->running = false;
            break;
        case SDL_EVENT_TEXT_INPUT:
            browser_handle_text(b, ev->text.text);
            break;
        case SDL_EVENT_KEY_DOWN:
            browser_handle_key(b, &ev->key);
            break;
        case SDL_EVENT_KEY_UP:
            browser_handle_key_up(b, ev->key.scancode);
            break;
        default:
            return;   /* not a state change: no redraw */
    }
    b->dirty = true;
}

/* Timed scroll repeat for held arrow keys. */
static void browser_scroll_repeat(browser_t *b) {
    if (b->input == INPUT_URL || b->input == INPUT_FORM) return;
    if (!b->scroll_down_held && !b->scroll_up_held) return;
    Uint64 now = SDL_GetTicks();
    if (now < b->scroll_repeat_at) return;
    int before = b->scroll_lines;
    b->scroll_lines += b->scroll_down_held ? 1 : -1;
    browser_clamp_scroll(b);
    b->scroll_repeat_at = now + SCROLL_REPEAT_INTERVAL_MS;
    if (b->scroll_lines != before) b->dirty = true;
}

/* How long to sleep in SDL_WaitEventTimeout before something needs a redraw
 * on its own (status message expiry, held-key repeat). */
static Sint32 browser_wait_ms(const browser_t *b) {
    if (bg_can_run(b)) return 0;           /* 4.3: images to load */
    Uint64 now = SDL_GetTicks();
    Uint64 wake = now + 1000;
    if (b->status_message[0] && b->status_message_until > now && b->status_message_until < wake)
        wake = b->status_message_until + 1;
    if ((b->scroll_down_held || b->scroll_up_held) && b->scroll_repeat_at < wake)
        wake = b->scroll_repeat_at > now ? b->scroll_repeat_at : now;
    return (Sint32)(wake - now);
}

int main(void) {
    printf("[mini_browser] enter main\n");

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        printf("[mini_browser] SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    /* Heap, not stack: the BadgeVMS app task stack is small. */
    browser_t *b = (browser_t *)calloc(1, sizeof(browser_t));
    if (!b) {
        printf("[mini_browser] out of memory\n");
        SDL_Quit();
        return 1;
    }

    b->win = SDL_CreateWindow("mini_browser", VIEW_W, VIEW_H, SDL_WINDOW_FULLSCREEN);
    if (!b->win) {
        printf("[mini_browser] CreateWindow failed: %s\n", SDL_GetError());
        free(b);
        SDL_Quit();
        return 1;
    }
    b->ren = SDL_CreateRenderer(b->win, NULL);
    if (!b->ren) {
        printf("[mini_browser] CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(b->win);
        free(b);
        SDL_Quit();
        return 1;
    }

    wifi_connect();
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
        printf("[mini_browser] curl_global_init failed\n");

    /* Load persistent bookmarks and history from BadgeVMS storage. */
    bookmark_load();
    visit_load();
    cookie_load();     /* 4.3 */
    cache_init();      /* 4.3 */
    settings_load();   /* 4.4: zoom */

#if defined(ESP_PLATFORM)
    esp_log_level_set("ESP_CURL",        ESP_LOG_ERROR);
    esp_log_level_set("HTTP_CLIENT",     ESP_LOG_ERROR);
    esp_log_level_set("transport_base",  ESP_LOG_ERROR);
#endif

    snprintf(b->url_buf, sizeof(b->url_buf), "%s", HOME_URL);
    b->hist_target = -1;
    b->running = true;
    b->need_fetch = true;
    b->dirty = true;
    b->sel_action = -1;
    browser_reset_input(b);

    SDL_StartTextInput(b->win);

    draw_ui(b->ren, b->url_buf);
    SDL_RenderPresent(b->ren);

    while (b->running) {
        if (b->need_fetch)
            browser_fetch(b);

        /* History is saved here, at the top of the loop, never while a
         * page is being loaded (see g_visit_save_due). */
        if (g_visit_save_due)
            visit_save();
        if (g_cookie_dirty)           /* 4.3: saved cookies changed */
            cookie_save();
        if (g_cache_index_dirty)      /* 4.3: disk cache index */
            cache_save_index();

        browser_clamp_scroll(b);

        /* Redraw only when something changed (or a screenshot was asked
         * for); the old loop redrew and presented ~100 times a second. */
        if (b->dirty || b->screenshot_pending || b->full_screenshot_pending) {
            b->dirty = false;
            browser_render(b);
        }

        /* Wait for input (or the next timed redraw), then drain the queue. */
        SDL_Event ev;
        if (SDL_WaitEventTimeout(&ev, browser_wait_ms(b))) {
            browser_handle_event(b, &ev);
            while (b->running && SDL_PollEvent(&ev))
                browser_handle_event(b, &ev);
        } else if (b->status_message[0] && SDL_GetTicks() >= b->status_message_until) {
            b->status_message[0] = 0;   /* timed wake: status message expired */
            b->dirty = true;
        }

        browser_scroll_repeat(b);

        /* 4.3: idle on a page with images still to load: load one. */
        if (b->running && bg_can_run(b) && !b->dirty) {
            SDL_PumpEvents();
            if (SDL_PeepEvents(NULL, 0, SDL_PEEKEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST) == 0)
                bg_step(b);
        }
    }

    SDL_StopTextInput(b->win);
    if (g_visit_unsaved) visit_save();
    if (g_cookie_dirty) cookie_save();
    if (g_cache_index_dirty || g_cache_touches) cache_save_index();
    tab_free_all_background();
    bfcache_clear();
    image_release_all();
    free_page(b->page);
    free(b->content_wrapped);
    net_shutdown();
    SDL_DestroyRenderer(b->ren);
    SDL_DestroyWindow(b->win);
    free(b);
    SDL_Quit();
    curl_global_cleanup();
    printf("[mini_browser] exit main\n");
    return 0;
}
