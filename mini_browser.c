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
#define MINI_BROWSER_VERSION "3.0"

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
#define FONT_SCALE  2
#define CH_W ((FONT_W_COLS + FONT_COL_GAP) * FONT_SCALE)
#define CH_H ((FONT_H_ROWS) * FONT_SCALE)

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
    size_t n = sz * nm;
    mem_t *m = (mem_t*)ud;
    if (!m || !n) return n;
    if (m->truncated) return 0;

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

static page_t *html_to_page(const char *html, const char *base_url) {
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

/* Phase 3 GIF Test3: used by wrapping to preserve inline-image control
 * markers as standalone logical lines. Definition is in the image section. */
static int is_image_marker_line(const char *line, int len, int *index);

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
 * Expires=<HTTP-date> -> true when the date is in the past.  The badge
 * often has no real-time clock, so when time() is not plausible only dates
 * before 2000 (the usual "Thu, 01 Jan 1970 00:00:00 GMT" logout idiom)
 * count as past.
 */
static bool cookie_expires_in_past(const char *value) {
    static const char months[] = "janfebmaraprmayjunjulaugsepoctnovdec";
    int day = 0, year = 0, hh = 0, mm = 0, ss = 0;
    char mon[4] = "";
    const char *p = strchr(value, ',');
    p = p ? p + 1 : value;
    if (sscanf(p, " %d%*[ -]%3s%*[ -]%d %d:%d:%d", &day, mon, &year, &hh, &mm, &ss) < 3)
        return false;
    if (year < 100) year += year < 70 ? 2000 : 1900;
    const char *m = NULL;
    for (int i = 0; mon[i]; i++) mon[i] = (char)tolower((unsigned char)mon[i]);
    if (strlen(mon) == 3) m = strstr(months, mon);
    if (!m || (m - months) % 3) return false;
    int month = (int)(m - months) / 3;

    time_t now = time(NULL);
    if (now < (time_t)1577836800) /* before 2020: clock not set */
        return year < 2000;

    /* Days since epoch (civil-from-days inverse), UTC. */
    int y = year - (month < 2);
    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400;
    long doy = (153 * (month + (month < 2 ? 10 : -2)) + 2) / 5 + day - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long long days = era * 146097LL + doe - 719468;
    if (hh < 0 || hh > 23 || mm < 0 || mm > 59 || ss < 0 || ss > 60 ||
        day < 1 || day > 31)
        return false;
    long long t = days * 86400LL + (long long)hh * 3600 + (long long)mm * 60 + ss;
    return t <= (long long)now;
}

static void cookie_store_header(const char *value, size_t value_len) {
    bool request_secure = false;
    bool secure = false;
    bool host_only = true;
    bool remove = false;

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
            } else if (!strcasecmp(a, "expires")) {
                if (cookie_expires_in_past(av)) remove = true;
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


/* ---------- curl fetch (tolerant to trimmed-down libcurl) ---------- */
/* ---------- v1.2: proper HTTP status/error handling ---------- */

#define MAX_REDIRECTS 5

/*
 * Note: BadgeVMS ships a small curl emulation on top of esp_http_client.
 * Only the options listed in its curl.h exist, the CURLOPT_* names are enum
 * values (so "#ifdef CURLOPT_X" is always false), and the write-callback
 * return value is ignored.  Everything below uses only that subset.
 */
static int fetch_one(const char *url, const char *post_body, mem_t *m, long *http_status) {
    CURL *curl = curl_easy_init();
    if (!curl) return -2;

    curl_easy_setopt(curl, CURLOPT_URL, url);

    if (post_body) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_body);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(post_body));
    }

    /*
     * Redirects are followed by fetch_url(), one hop per request, so that
     * cookies are stored for - and sent to - the host of each hop only.
     */
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 35L);

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

#if MB_LOG_SENSITIVE
        printf("[mini_browser] cookie send: %s\n", g_cookie_header_value);
#else
        printf("[mini_browser] cookie send: %d cookie(s)\n", g_fetch_meta.cookies_sent);
#endif
    }

    if (hdrs) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    }

    snprintf(g_cookie_request_url, sizeof(g_cookie_request_url), "%s", url);
    g_redirect_location[0] = 0;
    g_redirect_location_too_long = false;
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, cookie_header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, NULL);

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, wr_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, m);

    CURLcode res = curl_easy_perform(curl);

    /*
     * Even when the HTTP server returns 404/500, curl itself can still
     * return CURLE_OK. Therefore keep the HTTP status separately.
     */
    long code = 0;
    if (curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code) == CURLE_OK && http_status)
        *http_status = code;

    char *ctype = NULL;
    if (curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &ctype) == CURLE_OK &&
        ctype && ctype[0])
        snprintf(g_fetch_meta.content_type, sizeof(g_fetch_meta.content_type), "%s", ctype);

    /*
     * CURLOPT_HTTPHEADER does not take ownership of the curl_slist,
     * so we must release it ourselves after curl_easy_perform().
     */
    if (hdrs) curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    /* A body cut at our size cap is still a usable (partial) page. */
    if (res == CURLE_WRITE_ERROR && m->truncated) res = CURLE_OK;
    return (res == CURLE_OK) ? 0 : (int)res;
}

static bool http_status_is_redirect(long status) {
    return status == 301 || status == 302 || status == 303 ||
           status == 307 || status == 308;
}

static int fetch_url(const char *url, const char *post_body, mem_t *m, long *http_status) {
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

    char hop[URL_MAX];
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
        if (rc != 0 || !http_status_is_redirect(status) || !g_redirect_location[0])
            break;

        char next[URL_MAX];
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

    CURL *curl = curl_easy_init();
    if (!curl) return -2;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 25L);

    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs,
        "User-Agent: MiniBrowser/" MINI_BROWSER_VERSION " BadgeVMS Phase3");
    hdrs = curl_slist_append(hdrs,
        "Accept: image/jpeg,image/png,image/gif,image/*;q=0.5,*/*;q=0.1");
    hdrs = curl_slist_append(hdrs, "Accept-Encoding: identity");
    if (hdrs) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, wr_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, m);

    CURLcode res = curl_easy_perform(curl);

    if (http_status) {
        long code = 0;
        if (curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code) == CURLE_OK)
            *http_status = code;
    }

    if (hdrs) curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

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

static int load_page_images(const page_t *page) {
    for (int i = 0; i < MAX_INLINE_IMAGES; i++)
        decoded_image_release(&g_inline_images[i]);

    int inline_limit = display_inline_image_limit();
    if (!page || inline_limit <= 0)
        return 0;

    int count = page->image_count;
    if (count > inline_limit) count = inline_limit;

    int loaded = 0;
    for (int i = 0; i < count; i++) {
        if (load_image_url(page_image_src(page, i),
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
            rects[n++] = (SDL_FRect){ (float)(x + col + skew), (float)(y + row),
                                      (float)(run - col + 1), 1.0f };
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
static void draw_bar(SDL_Renderer *r, const char *text) {
    if (!text) text = "";
    int max_cols = (VIEW_W - URL_TEXT_X - PAD_LR) / CH_W;
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
                 "Press WHY+F on a web page to add one.");

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
             "Stored: %d / %d\n\n"

             "LIBCURL NOTES\n"
             "Available CURLINFO: response code,\n"
             "content length.\n"
             "Content type and redirects come from\n"
             "the response headers.\n\n"

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
             final_url,
             g_fetch_meta.redirect_count,
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
    INPUT_LINK_NUMBER    /* typing a link/action number */
} input_mode_t;

typedef enum {
    OVERLAY_NONE,
    OVERLAY_OPTIONS,     /* WHY+O display mode menu */
    OVERLAY_IMAGE        /* image viewer */
} overlay_t;

typedef enum {
    VIEW_WEB,            /* an http(s) page or a message (error) page */
    VIEW_BOOKMARKS,      /* WHY+M */
    VIEW_PAGE_INFO       /* WHY+I */
} view_kind_t;

typedef struct {
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

static const int k_max_cols = (VIEW_W - 2 * PAD_LR) / CH_W;
static const int k_lines_per_page = (VIEW_H - PAD_TOP - PAD_BOTTOM) / (CH_H + LINE_SPACING);

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
static int compute_max_scroll(const page_t *page, const char *content, int lines) {
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
}

static void browser_navigate(browser_t *b, const char *url) {
    snprintf(b->url_buf, sizeof(b->url_buf), "%s", url);
    b->need_fetch = true;
    b->sel_action = -1;
}

/* Show a locally generated page (bookmarks / page info). */
static void browser_show_local_page(browser_t *b, page_t *pg, view_kind_t kind, const char *pseudo_url) {
    if ((b->view == VIEW_WEB) && is_http_scheme(b->url_buf))
        snprintf(b->view_return_url, sizeof(b->view_return_url), "%s", b->url_buf);
    char *wrapped = wrap_text(pg->text, k_max_cols);
    browser_set_page(b, pg);
    browser_set_content(b, wrapped);
    snprintf(b->url_buf, sizeof(b->url_buf), "%s", pseudo_url);
    browser_reset_input(b);
    b->view = kind;
}

/* Leave bookmarks / page info: reload the page we came from. */
static bool browser_return_from_local_page(browser_t *b) {
    if (b->view == VIEW_WEB || !b->view_return_url[0]) return false;
    snprintf(b->url_buf, sizeof(b->url_buf), "%s", b->view_return_url);
    b->view_return_url[0] = 0;
    b->view = VIEW_WEB;
    b->history_navigation = true;
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

static void browser_fetch(browser_t *b) {
    b->need_fetch = false;
    b->overlay = (b->overlay == OVERLAY_IMAGE) ? OVERLAY_NONE : b->overlay;
    decoded_image_release(&g_viewer_image);
    browser_reset_input(b);

    trim_inplace(b->url_buf);
    if (!b->url_buf[0]) return;

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

    /* Any real page load leaves bookmarks / page info. */
    b->view = VIEW_WEB;
    b->view_return_url[0] = 0;

    /* record in history just before fetching (unless reloading) */
    if (!b->history_navigation)
        history_push(b->url_buf);
    b->history_navigation = false;

    /*
     * User-facing fetch state.  Keep HTTP status codes out of the
     * normal title bar; detailed HTTP errors are rendered in the
     * page itself.
     */
    draw_ui(b->ren, "Loading...");
    SDL_RenderPresent(b->ren);

    mem_t m = {0};
    long http_status = 0;

    if (b->pending_post) {
#if MB_LOG_SENSITIVE
        printf("[mini_browser] POST %s body=%s\n", b->url_buf, b->post_body);
#else
        printf("[mini_browser] POST %s body=<%u bytes>\n", b->url_buf,
               (unsigned)strlen(b->post_body));
#endif
    }

    int rc = fetch_url(b->url_buf, b->pending_post ? b->post_body : NULL, &m, &http_status);

    b->pending_post = false;
    b->post_body[0] = 0;
    b->last_http_status = http_status;
    b->dirty = true;

    if (rc != 0) {
        printf("[mini_browser] fetch error %d URL='%s'\n", rc, b->url_buf);

        char error_text[512];
        const char *curl_error = curl_easy_strerror((CURLcode)rc);
        snprintf(error_text, sizeof(error_text),
                 "CONNECTION FAILED\n\n"
                 "Could not load:\n%s\n\n"
                 "Reason:\n%s\n\n"
                 "Press WHY+R to retry, WHY+B to go back, WHY+H for homepage",
                 b->url_buf,
                 curl_error ? curl_error : "Unknown network error");
        browser_show_message(b, error_text);

    } else if (http_status >= 400) {
        printf("[mini_browser] HTTP %ld URL='%s'\n", http_status, b->url_buf);

        char error_text[512];
        snprintf(error_text, sizeof(error_text),
                 "HTTP ERROR %ld\n\n"
                 "The server returned HTTP status %ld.\n\n"
                 "URL:\n%s\n\n"
                 "Press WHY+B to go back or WHY+R to retry.",
                 http_status, http_status, b->url_buf);
        browser_show_message(b, error_text);

    } else if (content_type_is_image(g_fetch_meta.content_type) ||
               buffer_is_supported_image((const unsigned char *)m.buf, m.len)) {
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
            load_page_images(pg);

            char *wrapped = wrap_text(pg->text, k_max_cols);
#if MB_LOG_CONTENT
            debug_utf8("WRAPPED", wrapped);
#endif
            browser_set_content(b, wrapped);
            browser_set_status(b, "Loaded", 1000);

            page_t *page = b->page;
            printf("[mini_browser] HTTP %ld, %u bytes, %d links from %s\n",
                   http_status, (unsigned)m.len, page->link_count, b->url_buf);

            /*
             * Deterministic parser diagnostic.  Besides being useful while
             * debugging, the 2.5 regression suite uses this to verify that
             * HTML entities in <title> were decoded into page->title.
             */
            printf("[mini_browser] page title: %s\n",
                   page->title[0] ? page->title : "(none)");
            printf("[mini_browser] parser: links=%d actions=%d forms=%d\n",
                   page->link_count, page->action_count, page->form_count);
            printf("[mini_browser] visual: explicit_colors=%d\n", page->explicit_color_count);
            printf("[mini_browser] visual: explicit_styles=%d\n", page->explicit_style_count);
            printf("[mini_browser] visual: explicit_backgrounds=%d\n", page->explicit_background_count);
            int inline_loaded = 0;
            for (int ii = 0; ii < MAX_INLINE_IMAGES; ii++)
                if (g_inline_images[ii].loaded) inline_loaded++;
            printf("[mini_browser] display: mode=%s images_seen=%d images_retained=%d images_loaded=%d\n",
                   display_mode_name(g_display_mode),
                   page->image_seen_count, page->image_count, inline_loaded);
            printf("[mini_browser] cookies: count=%d\n", cookie_count());

            browser_log_content(wrapped);
        }
    }

    free(m.buf);
}

static void browser_compose_bar(browser_t *b) {
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
         */
        snprintf(bar, cap, "%s", page->title);
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
            draw_text_ex(ren, PAD_LR, y, p, (size_t)len, VIEW_W - 2 * PAD_LR, &state);
            y += (CH_H + LINE_SPACING);
        }

        drawn++;
        p = nl ? nl + 1 : NULL;
    }
}

static void browser_render(browser_t *b) {
    browser_compose_bar(b);

    if (b->overlay == OVERLAY_IMAGE) draw_image_viewer(b->ren, &g_viewer_image);
    else if (b->overlay == OVERLAY_OPTIONS) browser_render_options(b);
    else browser_render_page(b);

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

        if (b->input == INPUT_FORM) {
            browser_insert_char(b->form_edit_buf, sizeof(b->form_edit_buf), &b->form_edit_cursor, (char)c);
        } else if (b->input == INPUT_URL) {
            browser_insert_char(b->url_buf, sizeof(b->url_buf), &b->url_cursor, (char)c);
            b->sel_action = -1;
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

        case SDL_SCANCODE_F: /* BOOKMARK current page */
            browser_toggle_bookmark(b);
            break;

        case SDL_SCANCODE_B: { /* BACK */
            if (b->overlay == OVERLAY_IMAGE) {
                b->overlay = OVERLAY_NONE;
                decoded_image_release(&g_viewer_image);
            } else if (!browser_return_from_local_page(b)) {
                char prev[URL_MAX];
                if (history_back(prev)) {
                    browser_navigate(b, prev);
                    b->history_navigation = true;
                }
            }
            break;
        }

        case SDL_SCANCODE_G: { /* FORWARD */
            char next_url[URL_MAX];
            if (history_forward(next_url)) {
                browser_navigate(b, next_url);
                b->history_navigation = true;
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

    /* Text editing in the URL bar or a form field. */
    if (b->input == INPUT_URL || b->input == INPUT_FORM) {
        bool url = b->input == INPUT_URL;
        if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER) {
            if (url) {
                browser_reset_input(b);
                b->need_fetch = true;
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
            if (url) b->sel_action = -1;
        }
        return;
    }

    switch (sc) {
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER:
            if (b->page && (b->input == INPUT_LINK_NUMBER || b->sel_action >= 0))
                browser_activate(b);
            else
                b->need_fetch = true;
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
                if (b->sel_action < 0) b->sel_action = 0;   /* first Tab: first action */
                else if (shift) b->sel_action = (b->sel_action == 0) ? n - 1 : b->sel_action - 1;
                else b->sel_action = (b->sel_action + 1) % n;
            }
            break;
        }

        case SDL_SCANCODE_ESCAPE:
            if (b->overlay == OVERLAY_IMAGE) {
                b->overlay = OVERLAY_NONE;
                decoded_image_release(&g_viewer_image);
            } else if (b->input == INPUT_LINK_NUMBER) {
                browser_reset_input(b);
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

    /* Load persistent bookmarks from BadgeVMS storage. */
    bookmark_load();

#if defined(ESP_PLATFORM)
    esp_log_level_set("ESP_CURL",        ESP_LOG_ERROR);
    esp_log_level_set("HTTP_CLIENT",     ESP_LOG_ERROR);
    esp_log_level_set("transport_base",  ESP_LOG_ERROR);
#endif

    snprintf(b->url_buf, sizeof(b->url_buf), "%s", HOME_URL);
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
    }

    SDL_StopTextInput(b->win);
    image_release_all();
    free_page(b->page);
    free(b->content_wrapped);
    SDL_DestroyRenderer(b->ren);
    SDL_DestroyWindow(b->win);
    free(b);
    SDL_Quit();
    curl_global_cleanup();
    printf("[mini_browser] exit main\n");
    return 0;
}
