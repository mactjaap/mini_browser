/*
 * Mini Browser 1.6 - Linux text-only test version
 *
 * Derived from the WHY2025 BadgeVMS Mini Browser by mactjaap.
 * Keeps the same lightweight model: libcurl, 64 KiB page limit,
 * simple HTML-to-text conversion, numbered links, 128 links maximum.
 *
 * Build on AlmaLinux 9:
 *   gcc -O2 -Wall -Wextra mini_browser_linux.c -o mini-browser -lcurl
 *
 * Run:
 *   ./mini-browser
 *   ./mini-browser https://news.ycombinator.com/
 */

#include <curl/curl.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define MAX_BYTES     (64 * 1024)
#define URL_MAX       256
#define MAX_LINKS     128
#define HISTORY_MAX   32
#define BOOKMARK_MAX  32
#define HOME_URL      "https://minibrowser.macip.net"

typedef struct {
    char *buf;
    size_t len;
} mem_t;

typedef struct {
    char href[URL_MAX];
} link_t;

typedef struct {
    char *text;
    link_t links[MAX_LINKS];
    int link_count;
    char base[URL_MAX];
    char title[128];
} page_t;

typedef struct {
    char url[URL_MAX];
    char title[128];
} bookmark_t;

static bookmark_t g_bookmarks[BOOKMARK_MAX];
static int g_bookmark_count = 0;

static size_t wr_cb(void *ptr, size_t sz, size_t nm, void *ud)
{
    size_t n = sz * nm, keep = n;
    mem_t *m = (mem_t *)ud;

    if (m->len >= MAX_BYTES)
        return n;

    if (m->len + keep > MAX_BYTES)
        keep = MAX_BYTES - m->len;

    char *p = realloc(m->buf, m->len + keep + 1);
    if (!p)
        return 0;

    m->buf = p;
    memcpy(m->buf + m->len, ptr, keep);
    m->len += keep;
    m->buf[m->len] = 0;
    return n;
}

static void trim_inplace(char *s)
{
    if (!s)
        return;

    size_t n = strlen(s);
    size_t i = 0;
    while (i < n && isspace((unsigned char)s[i]))
        i++;

    size_t j = n;
    while (j > i && isspace((unsigned char)s[j - 1]))
        j--;

    if (i > 0 || j < n) {
        memmove(s, s + i, j - i);
        s[j - i] = 0;
    }
}

static int has_scheme(const char *u)
{
    return u && strstr(u, "://") != NULL;
}

static int is_http_scheme(const char *u)
{
    return u &&
           (strncmp(u, "http://", 7) == 0 ||
            strncmp(u, "https://", 8) == 0);
}

static void normalize_typed_url(char *buf)
{
    trim_inplace(buf);
    if (!buf[0])
        return;

    if (!has_scheme(buf)) {
        char tmp[URL_MAX];
        strncpy(tmp, buf, URL_MAX);
        tmp[URL_MAX - 1] = 0;
        snprintf(buf, URL_MAX, "https://%s", tmp);
    }
}

static void get_scheme_host(const char *url, char *out, size_t cap)
{
    const char *p = strstr(url, "://");
    if (!p) {
        out[0] = 0;
        return;
    }

    p += 3;
    const char *slash = strchr(p, '/');
    size_t n = slash ? (size_t)(slash - url) : strlen(url);

    if (n >= cap)
        n = cap - 1;

    memcpy(out, url, n);
    out[n] = 0;
}

static void get_dir(const char *url, char *out, size_t cap)
{
    const char *p = strrchr(url, '/');
    if (!p) {
        out[0] = 0;
        return;
    }

    size_t n = (size_t)(p - url) + 1;
    if (n >= cap)
        n = cap - 1;

    memcpy(out, url, n);
    out[n] = 0;
}

static void base_no_query_or_hash(const char *u, char *out, size_t cap)
{
    size_t n = strlen(u), cut = n;

    for (size_t i = 0; i < n; i++) {
        if (u[i] == '?' || u[i] == '#') {
            cut = i;
            break;
        }
    }

    if (cut >= cap)
        cut = cap - 1;

    memcpy(out, u, cut);
    out[cut] = 0;
}

static void base_no_hash(const char *u, char *out, size_t cap)
{
    size_t n = strlen(u), cut = n;

    for (size_t i = 0; i < n; i++) {
        if (u[i] == '#') {
            cut = i;
            break;
        }
    }

    if (cut >= cap)
        cut = cap - 1;

    memcpy(out, u, cut);
    out[cut] = 0;
}

static void scheme_from_url(const char *base, char *out, size_t cap)
{
    if (!base) {
        strncpy(out, "https", cap);
        out[cap - 1] = 0;
        return;
    }

    const char *p = strstr(base, "://");
    if (!p) {
        strncpy(out, "https", cap);
        out[cap - 1] = 0;
        return;
    }

    size_t n = (size_t)(p - base);
    if (n >= cap)
        n = cap - 1;

    memcpy(out, base, n);
    out[n] = 0;
}

static void resolve_url(const char *base, const char *href,
                        char *out, size_t cap)
{
    if (!href || !*href) {
        out[0] = 0;
        return;
    }

    if (strstr(href, "://")) {
        strncpy(out, href, cap);
        out[cap - 1] = 0;
        return;
    }

    if (href[0] == '/' && href[1] == '/') {
        char sch[16];
        scheme_from_url(base, sch, sizeof sch);
        snprintf(out, cap, "%s:%s", sch, href);
        return;
    }

    if (href[0] == '/') {
        char origin[URL_MAX];
        get_scheme_host(base, origin, sizeof origin);
        snprintf(out, cap, "%s%s", origin, href);
        return;
    }

    if (href[0] == '?') {
        char base2[URL_MAX];
        base_no_query_or_hash(base, base2, sizeof base2);
        snprintf(out, cap, "%s%s", base2, href);
        return;
    }

    if (href[0] == '#') {
        char base2[URL_MAX];
        base_no_hash(base, base2, sizeof base2);
        snprintf(out, cap, "%s%s", base2, href);
        return;
    }

    if (href[0] == '.' && href[1] == '/')
        href += 2;

    char dir[URL_MAX];
    get_dir(base, dir, sizeof dir);
    snprintf(out, cap, "%s%s", dir, href);
}

static const char *emit_entity(const char *h, char *out,
                               size_t *o, size_t cap)
{
    if (!strncmp(h, "&amp;", 5)) {
        if (*o < cap) out[(*o)++] = '&';
        return h + 5;
    } else if (!strncmp(h, "&lt;", 4)) {
        if (*o < cap) out[(*o)++] = '<';
        return h + 4;
    } else if (!strncmp(h, "&gt;", 4)) {
        if (*o < cap) out[(*o)++] = '>';
        return h + 4;
    } else if (!strncmp(h, "&quot;", 6)) {
        if (*o < cap) out[(*o)++] = '"';
        return h + 6;
    } else if (!strncmp(h, "&#39;", 5)) {
        if (*o < cap) out[(*o)++] = '\'';
        return h + 5;
    } else if (!strncmp(h, "&nbsp;", 6)) {
        if (*o < cap) out[(*o)++] = ' ';
        return h + 6;
    }

    return NULL;
}

static int is_supported_href(const char *h)
{
    if (!h || !*h)
        return 0;
    if (h[0] == '#')
        return 0;
    if (!strncasecmp(h, "javascript:", 11))
        return 0;
    if (!strncasecmp(h, "mailto:", 7))
        return 0;
    if (!strncasecmp(h, "data:", 5))
        return 0;
    return 1;
}

static const char *find_case_insensitive(const char *haystack,
                                         const char *needle)
{
    if (!haystack || !needle || !*needle)
        return haystack;

    size_t needle_len = strlen(needle);

    for (const char *p = haystack; *p; p++) {
        if (strncasecmp(p, needle, needle_len) == 0)
            return p;
    }

    return NULL;
}

static void extract_html_title(const char *html, char *out, size_t cap)
{
    if (!out || cap == 0)
        return;

    out[0] = 0;
    if (!html)
        return;

    const char *start = find_case_insensitive(html, "<title");
    if (!start)
        return;

    start = strchr(start, '>');
    if (!start)
        return;

    start++;

    const char *end = find_case_insensitive(start, "</title>");
    if (!end)
        return;

    size_t n = (size_t)(end - start);
    if (n >= cap)
        n = cap - 1;

    memcpy(out, start, n);
    out[n] = 0;
    trim_inplace(out);

    char *src = out;
    char *dst = out;
    bool in_space = false;

    while (*src) {
        if (isspace((unsigned char)*src)) {
            if (!in_space) {
                *dst++ = ' ';
                in_space = true;
            }
        } else {
            *dst++ = *src;
            in_space = false;
        }
        src++;
    }

    *dst = 0;
}

static page_t *html_to_page(const char *html, const char *base_url)
{
    if (!html)
        return NULL;

    size_t L = strlen(html);
    size_t cap = L + 1 + MAX_LINKS * 6;
    char *buf = malloc(cap);
    if (!buf)
        return NULL;

    page_t *pg = calloc(1, sizeof(page_t));
    if (!pg) {
        free(buf);
        return NULL;
    }

    strncpy(pg->base, base_url ? base_url : "", URL_MAX);
    pg->base[URL_MAX - 1] = 0;
    extract_html_title(html, pg->title, sizeof(pg->title));

    bool in_head = false, in_script = false, in_style = false;
    size_t o = 0;

    for (size_t i = 0; i < L;) {
        char c = html[i];

        if (!in_script && !in_style && !in_head && c == '&') {
            const char *adv = emit_entity(&html[i], buf, &o, cap - 1);
            if (adv) {
                i = (size_t)(adv - html);
                continue;
            }
        }

        if (c == '<') {
            i++;
            bool closing = false;

            if (i < L && html[i] == '/') {
                closing = true;
                i++;
            }

            char tname[16];
            int tn = 0;

            while (i < L && tn < (int)sizeof(tname) - 1 &&
                   isalpha((unsigned char)html[i])) {
                tname[tn++] = (char)tolower((unsigned char)html[i]);
                i++;
            }
            tname[tn] = 0;

            while (i < L && isspace((unsigned char)html[i]))
                i++;

            if (!closing) {
                if (!strcmp(tname, "head"))
                    in_head = true;
                else if (!strcmp(tname, "script"))
                    in_script = true;
                else if (!strcmp(tname, "style"))
                    in_style = true;

                if (!strcmp(tname, "a")) {
                    char href_val[URL_MAX];
                    href_val[0] = 0;

                    while (i < L && html[i] != '>') {
                        char aname[16];
                        int an = 0;

                        while (i < L && isspace((unsigned char)html[i]))
                            i++;

                        while (i < L && an < (int)sizeof(aname) - 1 &&
                               (isalnum((unsigned char)html[i]) ||
                                html[i] == '-' || html[i] == '_')) {
                            aname[an++] =
                                (char)tolower((unsigned char)html[i]);
                            i++;
                        }
                        aname[an] = 0;

                        while (i < L && isspace((unsigned char)html[i]))
                            i++;

                        if (i < L && html[i] == '=') {
                            i++;
                            while (i < L &&
                                   isspace((unsigned char)html[i]))
                                i++;

                            char quote = 0;
                            if (i < L &&
                                (html[i] == '"' || html[i] == '\''))
                                quote = html[i++];

                            char aval[URL_MAX];
                            int av = 0;

                            while (i < L) {
                                if (quote) {
                                    if (html[i] == quote) {
                                        i++;
                                        break;
                                    }
                                } else if (isspace((unsigned char)html[i]) ||
                                           html[i] == '>') {
                                    break;
                                }

                                if (av < (int)sizeof(aval) - 1)
                                    aval[av++] = html[i];
                                i++;
                            }
                            aval[av] = 0;

                            if (!strcmp(aname, "href") && !href_val[0]) {
                                strncpy(href_val, aval, URL_MAX);
                                href_val[URL_MAX - 1] = 0;
                            }
                        } else {
                            while (i < L &&
                                   !isspace((unsigned char)html[i]) &&
                                   html[i] != '>')
                                i++;
                        }
                    }

                    if (is_supported_href(href_val) &&
                        pg->link_count < MAX_LINKS) {
                        char abs[URL_MAX];
                        resolve_url(pg->base, href_val, abs, sizeof abs);

                        if (is_supported_href(abs)) {
                            int idx = pg->link_count++;
                            strncpy(pg->links[idx].href, abs, URL_MAX);
                            pg->links[idx].href[URL_MAX - 1] = 0;

                            int wrote = snprintf(buf + o, cap - o,
                                                 "[%d]", idx + 1);
                            if (wrote > 0)
                                o += (size_t)wrote;
                        }
                    }

                    while (i < L && html[i] != '>')
                        i++;

                } else if (!strcmp(tname, "br")) {
                    if (o && buf[o - 1] != '\n')
                        buf[o++] = '\n';

                    while (i < L && html[i] != '>')
                        i++;

                } else if (!strcmp(tname, "p") ||
                           !strcmp(tname, "div") ||
                           !strcmp(tname, "section") ||
                           !strcmp(tname, "article") ||
                           !strcmp(tname, "header") ||
                           !strcmp(tname, "footer")) {
                    if (o && buf[o - 1] != '\n')
                        buf[o++] = '\n';

                    while (i < L && html[i] != '>')
                        i++;

                } else if (!strcmp(tname, "h1") ||
                           !strcmp(tname, "h2") ||
                           !strcmp(tname, "h3") ||
                           !strcmp(tname, "h4") ||
                           !strcmp(tname, "h5") ||
                           !strcmp(tname, "h6")) {
                    if (o && buf[o - 1] != '\n')
                        buf[o++] = '\n';

                    if (o < cap - 3) {
                        buf[o++] = '=';
                        buf[o++] = ' ';
                    }

                    while (i < L && html[i] != '>')
                        i++;

                } else if (!strcmp(tname, "li")) {
                    if (o && buf[o - 1] != '\n')
                        buf[o++] = '\n';

                    if (o < cap - 3) {
                        buf[o++] = '*';
                        buf[o++] = ' ';
                    }

                    while (i < L && html[i] != '>')
                        i++;

                } else if (!strcmp(tname, "pre")) {
                    if (o && buf[o - 1] != '\n')
                        buf[o++] = '\n';

                    while (i < L && html[i] != '>')
                        i++;
                } else {
                    while (i < L && html[i] != '>')
                        i++;
                }

            } else {
                if (!strcmp(tname, "head"))
                    in_head = false;
                else if (!strcmp(tname, "script"))
                    in_script = false;
                else if (!strcmp(tname, "style"))
                    in_style = false;
                else if (!strcmp(tname, "p") ||
                         !strcmp(tname, "div") ||
                         !strcmp(tname, "section") ||
                         !strcmp(tname, "article") ||
                         !strcmp(tname, "header") ||
                         !strcmp(tname, "footer") ||
                         !strcmp(tname, "li") ||
                         !strcmp(tname, "h1") ||
                         !strcmp(tname, "h2") ||
                         !strcmp(tname, "h3") ||
                         !strcmp(tname, "h4") ||
                         !strcmp(tname, "h5") ||
                         !strcmp(tname, "h6") ||
                         !strcmp(tname, "pre")) {
                    if (o && buf[o - 1] != '\n')
                        buf[o++] = '\n';
                }

                while (i < L && html[i] != '>')
                    i++;
            }

            if (i < L && html[i] == '>')
                i++;

            continue;
        }

        if (in_script || in_style || in_head) {
            i++;
            continue;
        }

        if (c == '\r') {
            i++;
            continue;
        }

        if (c == '\n') {
            if (o && buf[o - 1] != '\n')
                buf[o++] = '\n';
            i++;
            continue;
        }

        if (c == ' ' && o && buf[o - 1] == ' ') {
            i++;
            continue;
        }

        if (o < cap - 1)
            buf[o++] = c;

        i++;
    }

    buf[o] = 0;
    pg->text = buf;
    return pg;
}

static char *wrap_text(const char *in, int max_cols)
{
    if (!in)
        return NULL;

    if (max_cols < 20)
        max_cols = 80;

    size_t n = strlen(in);
    char *out = malloc(n + n / (size_t)max_cols + 256);
    if (!out)
        return NULL;

    int col = 0;
    size_t o = 0;
    int blank_run = 0;

    for (size_t i = 0; i < n; i++) {
        char c = in[i];

        if (c == '\r')
            continue;

        if (c == '\n') {
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

        if (c == ' ' && (col == 0 || (o && out[o - 1] == ' ')))
            continue;

        if (col >= max_cols && c == ' ') {
            out[o++] = '\n';
            col = 0;
            continue;
        }

        if (col >= max_cols) {
            out[o++] = '\n';
            col = 0;
        }

        out[o++] = c;
        col++;
        blank_run = 0;
    }

    out[o] = 0;
    return out;
}

static int fetch_url(const char *url, mem_t *m, long *http_status,
                     char *effective_url, size_t effective_cap)
{
    if (!url || !m)
        return CURLE_BAD_FUNCTION_ARGUMENT;

    CURL *curl = curl_easy_init();
    if (!curl)
        return CURLE_FAILED_INIT;

    m->buf = NULL;
    m->len = 0;

    if (http_status)
        *http_status = 0;
    if (effective_url && effective_cap)
        effective_url[0] = 0;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 35L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 10L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 20L);

    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(
        hdrs,
        "User-Agent: Mozilla/5.0 (Linux; MiniBrowser/1.6) "
        "Gecko/20100101 (compatible; MiniBrowser/1.6; "
        "+https://github.com/mactjaap/mini_browser/; HTTP/1.1; identity)");
    hdrs = curl_slist_append(
        hdrs,
        "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8");
    hdrs = curl_slist_append(hdrs, "Accept-Language: en-US,en;q=0.5");
    hdrs = curl_slist_append(hdrs, "Accept-Encoding: identity");

    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, wr_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, m);

    CURLcode res = curl_easy_perform(curl);

    if (http_status) {
        long code = 0;
        if (curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code) == CURLE_OK)
            *http_status = code;
    }

    if (effective_url && effective_cap) {
        char *eff = NULL;
        if (curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &eff) == CURLE_OK &&
            eff) {
            strncpy(effective_url, eff, effective_cap);
            effective_url[effective_cap - 1] = 0;
        }
    }

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    return (int)res;
}

static void free_page(page_t *pg)
{
    if (!pg)
        return;
    free(pg->text);
    free(pg);
}

static int terminal_width(void)
{
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col >= 20)
        return ws.ws_col;

    return 80;
}

static const char *bookmark_file(void)
{
    static char path[1024];
    const char *home = getenv("HOME");

    if (!home || !*home)
        return ".mini_browser_bookmarks.txt";

    snprintf(path, sizeof path, "%s/.mini_browser_bookmarks.txt", home);
    return path;
}

static int bookmark_find(const char *url)
{
    if (!url || !*url)
        return -1;

    for (int i = 0; i < g_bookmark_count; i++) {
        if (strncmp(g_bookmarks[i].url, url, URL_MAX) == 0)
            return i;
    }

    return -1;
}

static int bookmark_add(const char *url, const char *title)
{
    if (!url || !*url || bookmark_find(url) >= 0 ||
        g_bookmark_count >= BOOKMARK_MAX)
        return 0;

    bookmark_t *bm = &g_bookmarks[g_bookmark_count];

    strncpy(bm->url, url, URL_MAX);
    bm->url[URL_MAX - 1] = 0;

    if (title && *title) {
        strncpy(bm->title, title, sizeof bm->title);
        bm->title[sizeof bm->title - 1] = 0;
    } else {
        strncpy(bm->title, url, sizeof bm->title);
        bm->title[sizeof bm->title - 1] = 0;
    }

    g_bookmark_count++;
    return 1;
}

static int bookmark_remove(const char *url)
{
    int idx = bookmark_find(url);
    if (idx < 0)
        return 0;

    if (idx < g_bookmark_count - 1) {
        memmove(&g_bookmarks[idx],
                &g_bookmarks[idx + 1],
                sizeof(g_bookmarks[0]) *
                    (size_t)(g_bookmark_count - idx - 1));
    }

    g_bookmark_count--;
    return 1;
}

static void bookmark_save(void)
{
    FILE *f = fopen(bookmark_file(), "w");
    if (!f)
        return;

    for (int i = 0; i < g_bookmark_count; i++) {
        fprintf(f, "%s\n", g_bookmarks[i].url);
        fprintf(f, "%s\n", g_bookmarks[i].title);
    }

    fclose(f);
}

static void bookmark_load(void)
{
    FILE *f = fopen(bookmark_file(), "r");
    if (!f)
        return;

    char url[URL_MAX];
    char title[128];

    while (g_bookmark_count < BOOKMARK_MAX &&
           fgets(url, sizeof url, f) &&
           fgets(title, sizeof title, f)) {
        url[strcspn(url, "\r\n")] = 0;
        title[strcspn(title, "\r\n")] = 0;

        if (url[0])
            bookmark_add(url, title);
    }

    fclose(f);
}

static void show_bookmarks(void)
{
    printf("\nBookmarks (%d/%d)\n", g_bookmark_count, BOOKMARK_MAX);
    printf("----------------------------------------\n");

    if (!g_bookmark_count) {
        printf("(none)\n");
        return;
    }

    for (int i = 0; i < g_bookmark_count; i++)
        printf("%2d. %s\n    %s\n",
               i + 1, g_bookmarks[i].title, g_bookmarks[i].url);
}

static void show_help(void)
{
    puts("\nCommands:");
    puts("  NUMBER       open link, e.g. 12");
    puts("  b            back");
    puts("  g URL        go to URL");
    puts("  g            prompt for URL");
    puts("  h            home");
    puts("  r            reload");
    puts("  l            list link URLs");
    puts("  f            add/remove current page bookmark");
    puts("  m            show bookmarks");
    puts("  o NUMBER     open bookmark");
    puts("  ?            help");
    puts("  q            quit");
}

static void show_links(const page_t *pg)
{
    if (!pg || pg->link_count == 0) {
        puts("\nNo links.");
        return;
    }

    printf("\nLinks (%d):\n", pg->link_count);
    for (int i = 0; i < pg->link_count; i++)
        printf("  [%d] %s\n", i + 1, pg->links[i].href);
}

static void display_page(const page_t *pg, const char *url, long status)
{
    int width = terminal_width();
    char *wrapped = wrap_text(pg && pg->text ? pg->text : "", width);

    printf("\033[2J\033[H");
    printf("Mini Browser 1.6 - Linux text-only test\n");
    printf("URL: %s\n", url);

    if (pg && pg->title[0])
        printf("Title: %s\n", pg->title);

    printf("HTTP: %ld   Links: %d   Limit: %d KiB\n",
           status, pg ? pg->link_count : 0, MAX_BYTES / 1024);

    for (int i = 0; i < width && i < 120; i++)
        putchar('-');
    putchar('\n');

    if (wrapped)
        printf("%s\n", wrapped);

    for (int i = 0; i < width && i < 120; i++)
        putchar('-');
    putchar('\n');

    puts("NUMBER=open  b=back  g=URL  h=home  r=reload  l=links");
    puts("f=bookmark  m=bookmarks  o N=open bookmark  ?=help  q=quit");

    free(wrapped);
}

static int load_page(const char *requested_url, page_t **page_out,
                     char *current_url, size_t current_cap,
                     long *status_out)
{
    mem_t mem = {0};
    long status = 0;
    char effective[URL_MAX] = "";

    printf("Loading %s ...\n", requested_url);
    fflush(stdout);

    int rc = fetch_url(requested_url, &mem, &status,
                       effective, sizeof effective);

    if (rc != CURLE_OK) {
        fprintf(stderr, "Connection failed: %s\n",
                curl_easy_strerror((CURLcode)rc));
        free(mem.buf);
        return 0;
    }

    if (status < 200 || status >= 400) {
        fprintf(stderr, "HTTP error: %ld\n", status);
        free(mem.buf);
        return 0;
    }

    const char *base = effective[0] ? effective : requested_url;
    page_t *pg = html_to_page(mem.buf ? mem.buf : "", base);
    free(mem.buf);

    if (!pg) {
        fprintf(stderr, "Could not parse page.\n");
        return 0;
    }

    free_page(*page_out);
    *page_out = pg;

    strncpy(current_url, base, current_cap);
    current_url[current_cap - 1] = 0;

    if (status_out)
        *status_out = status;

    display_page(pg, current_url, status);
    return 1;
}

int main(int argc, char **argv)
{
    char current_url[URL_MAX];
    char history[HISTORY_MAX][URL_MAX];
    int history_count = 0;
    page_t *page = NULL;
    long http_status = 0;

    const char *start = (argc > 1) ? argv[1] : HOME_URL;
    strncpy(current_url, start, sizeof current_url);
    current_url[sizeof current_url - 1] = 0;
    normalize_typed_url(current_url);

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        fprintf(stderr, "Could not initialize libcurl.\n");
        return 1;
    }

    bookmark_load();

    char requested[URL_MAX];
    strncpy(requested, current_url, sizeof requested);
    requested[sizeof requested - 1] = 0;

    if (!load_page(requested, &page, current_url,
                   sizeof current_url, &http_status)) {
        puts("Type 'g URL' to try another address, or 'q' to quit.");
    }

    char cmd[1024];

    while (1) {
        printf("\nmini-browser> ");
        fflush(stdout);

        if (!fgets(cmd, sizeof cmd, stdin))
            break;

        cmd[strcspn(cmd, "\r\n")] = 0;
        trim_inplace(cmd);

        if (!cmd[0])
            continue;

        if (!strcmp(cmd, "q") || !strcmp(cmd, "quit"))
            break;

        if (!strcmp(cmd, "?")) {
            show_help();
            continue;
        }

        if (!strcmp(cmd, "l")) {
            show_links(page);
            continue;
        }

        if (!strcmp(cmd, "m")) {
            show_bookmarks();
            continue;
        }

        if (!strcmp(cmd, "f")) {
            if (!page || !is_http_scheme(current_url)) {
                puts("No HTTP page to bookmark.");
                continue;
            }

            if (bookmark_find(current_url) >= 0) {
                bookmark_remove(current_url);
                bookmark_save();
                puts("BOOKMARK REMOVED");
            } else if (bookmark_add(current_url, page->title)) {
                bookmark_save();
                puts("BOOKMARK ADDED");
            } else {
                puts("Could not add bookmark.");
            }
            continue;
        }

        if (!strncmp(cmd, "o ", 2)) {
            char *end = NULL;
            long n = strtol(cmd + 2, &end, 10);

            if (end == cmd + 2 || *end || n < 1 || n > g_bookmark_count) {
                puts("Invalid bookmark number.");
                continue;
            }

            if (page && history_count < HISTORY_MAX) {
                strncpy(history[history_count++], current_url, URL_MAX);
                history[history_count - 1][URL_MAX - 1] = 0;
            }

            strncpy(requested, g_bookmarks[n - 1].url, sizeof requested);
            requested[sizeof requested - 1] = 0;

            load_page(requested, &page, current_url,
                      sizeof current_url, &http_status);
            continue;
        }

        if (!strcmp(cmd, "h")) {
            if (page && history_count < HISTORY_MAX) {
                strncpy(history[history_count++], current_url, URL_MAX);
                history[history_count - 1][URL_MAX - 1] = 0;
            }

            load_page(HOME_URL, &page, current_url,
                      sizeof current_url, &http_status);
            continue;
        }

        if (!strcmp(cmd, "r")) {
            if (current_url[0])
                load_page(current_url, &page, current_url,
                          sizeof current_url, &http_status);
            continue;
        }

        if (!strcmp(cmd, "b")) {
            if (history_count <= 0) {
                puts("History is empty.");
                continue;
            }

            history_count--;
            strncpy(requested, history[history_count], sizeof requested);
            requested[sizeof requested - 1] = 0;

            load_page(requested, &page, current_url,
                      sizeof current_url, &http_status);
            continue;
        }

        if (!strcmp(cmd, "g")) {
            printf("URL: ");
            fflush(stdout);

            if (!fgets(requested, sizeof requested, stdin))
                continue;

            requested[strcspn(requested, "\r\n")] = 0;
            normalize_typed_url(requested);

            if (!requested[0])
                continue;

            if (page && history_count < HISTORY_MAX) {
                strncpy(history[history_count++], current_url, URL_MAX);
                history[history_count - 1][URL_MAX - 1] = 0;
            }

            load_page(requested, &page, current_url,
                      sizeof current_url, &http_status);
            continue;
        }

        if (!strncmp(cmd, "g ", 2)) {
            strncpy(requested, cmd + 2, sizeof requested);
            requested[sizeof requested - 1] = 0;
            normalize_typed_url(requested);

            if (!requested[0])
                continue;

            if (page && history_count < HISTORY_MAX) {
                strncpy(history[history_count++], current_url, URL_MAX);
                history[history_count - 1][URL_MAX - 1] = 0;
            }

            load_page(requested, &page, current_url,
                      sizeof current_url, &http_status);
            continue;
        }

        char *end = NULL;
        long n = strtol(cmd, &end, 10);

        if (end != cmd && *end == 0) {
            if (!page || n < 1 || n > page->link_count) {
                puts("Invalid link number.");
                continue;
            }

            if (history_count < HISTORY_MAX) {
                strncpy(history[history_count++], current_url, URL_MAX);
                history[history_count - 1][URL_MAX - 1] = 0;
            }

            strncpy(requested, page->links[n - 1].href, sizeof requested);
            requested[sizeof requested - 1] = 0;

            load_page(requested, &page, current_url,
                      sizeof current_url, &http_status);
            continue;
        }

        puts("Unknown command. Type ? for help.");
    }

    free_page(page);
    curl_global_cleanup();
    puts("\nBye.");
    return 0;
}

