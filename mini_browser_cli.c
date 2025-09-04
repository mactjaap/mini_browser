// mini_browser_cli.c
// Text-only micro browser for terminal: number + Enter to follow links.
//
// Build:
//   sudo apt-get update
//   sudo apt-get install -y build-essential libcurl4-openssl-dev
//   gcc -O2 -Wall -Wextra mini_browser_cli.c -o mini_browser_cli -lcurl
//
// Run:
//   ./mini_browser_cli                         # starts at default HOME_URL
//   ./mini_browser_cli https://example.org/    # start URL
//
// Keys at the prompt:
//   - number (e.g. 3)   -> open link [3]
//   - URL (full or relative) -> navigate to it
//   - r                 -> reload current page
//   - q                 -> quit

#define _GNU_SOURCE
#include <curl/curl.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#ifndef HOME_URL
#define HOME_URL "https://text.npr.org/"
#endif

/* Limits */
#define MAX_BYTES   (64 * 1024)   /* max bytes to keep from body (truncates) */
#define URL_MAX     1024
#define MAX_LINKS   512

/* -------- curl sink -------- */
typedef struct { char *buf; size_t len; } mem_t;

static size_t wr_cb(void *ptr, size_t sz, size_t nm, void *ud) {
    size_t in = sz * nm;
    mem_t *m = (mem_t*)ud;
    if (m->len >= MAX_BYTES) return in;
    size_t keep = in;
    if (m->len + keep > MAX_BYTES) keep = MAX_BYTES - m->len;
    char *p = (char*)realloc(m->buf, m->len + keep + 1);
    if (!p) return 0;
    m->buf = p;
    memcpy(m->buf + m->len, ptr, keep);
    m->len += keep;
    m->buf[m->len] = 0;
    return in;
}

static int fetch_url(const char *url, mem_t *m, long timeout_s) {
    CURL *curl = curl_easy_init();
    if (!curl) return -1;
    m->buf = NULL; m->len = 0;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "mini_browser_cli/1.0");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, wr_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, m);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_s);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, timeout_s);
    CURLcode rc = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    return rc == CURLE_OK ? 0 : (int)rc;
}

/* -------- small helpers -------- */
static void trim_inplace(char *s) {
    if (!s) return;
    size_t n = strlen(s);
    size_t i=0; while (i<n && isspace((unsigned char)s[i])) i++;
    size_t j=n; while (j>i && isspace((unsigned char)s[j-1])) j--;
    if (i>0 || j<n) { memmove(s, s+i, j-i); s[j-i]=0; }
}

static int has_scheme(const char *u) { return u && strstr(u, "://"); }

static int is_http_scheme(const char *u) {
    return u && (!strncmp(u, "http://", 7) || !strncmp(u, "https://", 8));
}

static void scheme_from_url(const char *base, char *out, size_t cap) {
    if (!base) { snprintf(out, cap, "https"); return; }
    const char *p = strstr(base, "://");
    if (!p) { snprintf(out, cap, "https"); return; }
    size_t n = (size_t)(p - base);
    if (n >= cap) n = cap-1;
    memcpy(out, base, n);
    out[n] = 0;
}

static void get_scheme_host(const char *url, char *out, size_t cap) {
    const char *p = strstr(url, "://");
    if (!p) { out[0]=0; return; }
    p += 3;
    const char *slash = strchr(p, '/');
    size_t n = slash ? (size_t)(slash - url) : strlen(url);
    if (n >= cap) n = cap - 1;
    memcpy(out, url, n); out[n] = 0;
}

static void get_dir(const char *url, char *out, size_t cap) {
    const char *p = strrchr(url, '/');
    if (!p) { out[0]=0; return; }
    size_t n = (size_t)(p - url) + 1;
    if (n >= cap) n = cap-1;
    memcpy(out, url, n);
    out[n] = 0;
}

static void base_no_query_or_hash(const char *u, char *out, size_t cap) {
    size_t n = strlen(u), cut = n;
    for (size_t i=0;i<n;i++){ if (u[i]=='?' || u[i]=='#') { cut=i; break; } }
    if (cut >= cap) cut = cap-1;
    memcpy(out, u, cut); out[cut]=0;
}

static void base_no_hash(const char *u, char *out, size_t cap) {
    size_t n = strlen(u), cut = n;
    for (size_t i=0;i<n;i++){ if (u[i]=='#') { cut=i; break; } }
    if (cut >= cap) cut = cap-1;
    memcpy(out, u, cut); out[cut]=0;
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
    if (href[0]=='.' && href[1]=='/') href += 2; /* drop leading ./ */

    char dir[URL_MAX]; get_dir(base, dir, sizeof dir);
    snprintf(out, cap, "%s%s", dir, href);
}

/* --------- minimal entity decoding --------- */
static const char *emit_entity(const char *h, char *out, size_t *o, size_t cap) {
    if      (!strncmp(h,"&amp;",5))  { if(*o<cap) out[(*o)++]='&';  return h+5; }
    else if (!strncmp(h,"&lt;",4))   { if(*o<cap) out[(*o)++]='<';  return h+4; }
    else if (!strncmp(h,"&gt;",4))   { if(*o<cap) out[(*o)++]='>';  return h+4; }
    else if (!strncmp(h,"&quot;",6)) { if(*o<cap) out[(*o)++]='"';  return h+6; }
    else if (!strncmp(h,"&#39;",5))  { if(*o<cap) out[(*o)++]='\''; return h+5; }
    else if (!strncmp(h,"&nbsp;",6)) { if(*o<cap) out[(*o)++]=' ';  return h+6; }
    return NULL;
}

/* --------- page model --------- */
typedef struct { char href[URL_MAX]; } link_t;
typedef struct {
    char *text;                 /* rendered with [n] markers where links open */
    link_t links[MAX_LINKS];
    int link_count;
    char base[URL_MAX];
} page_t;

/* --------- tag filter helpers --------- */
static int is_supported_href(const char *h) {
    if (!h || !*h) return 0;
    if (h[0] == '#') return 0;
    if (!strncasecmp(h, "javascript:", 11)) return 0;
    if (!strncasecmp(h, "mailto:", 7)) return 0;
    if (!strncasecmp(h, "data:", 5)) return 0;
    return 1;
}

/* --------- HTML → page_t (collect links at <a ...>) --------- */
static page_t *html_to_page(const char *html, const char *base_url) {
    if (!html) return NULL;
    size_t L = strlen(html);

    /* generous buffer for text + link markers */
    char *buf = (char*)malloc(L + 1 + MAX_LINKS * 8);
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
            const char *adv = emit_entity(&html[i], buf, &o, L + MAX_LINKS*8);
            if (adv) { i = (size_t)(adv - html); continue; }
        }

        if (c == '<') {
            i++;
            bool closing = false;
            if (i<L && html[i]=='/') { closing=true; i++; }

            /* tag name */
            char tname[16]; int tn=0;
            while (i<L && tn<(int)sizeof(tname)-1 && isalpha((unsigned char)html[i])) {
                tname[tn++] = (char)tolower((unsigned char)html[i]); i++;
            }
            tname[tn]=0;

            /* skip spaces */
            while (i<L && isspace((unsigned char)html[i])) i++;

            if (!closing) {
                if (!strcmp(tname,"head"))   in_head=true;
                else if (!strcmp(tname,"script")) in_script=true;
                else if (!strcmp(tname,"style"))  in_style=true;

                if (!strcmp(tname,"br") || !strcmp(tname,"p")) {
                    if (o && buf[o-1] != '\n') buf[o++] = '\n';
                    while (i<L && html[i] != '>') i++;
                } else if (!strcmp(tname,"a")) {
                    char href_val[URL_MAX]; href_val[0]=0;

                    /* scan attributes until '>' */
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
                                if (quote) { if (html[i]==quote){ i++; break; } }
                                else       { if (isspace((unsigned char)html[i]) || html[i]=='>') break; }
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
                            int wrote = snprintf(buf + o, L + MAX_LINKS*8 - o, "[%d]", idx+1);
                            if (wrote > 0) o += (size_t)wrote;
                        }
                    }

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

        /* normalize newlines/spaces */
        if (c=='\r') { i++; continue; }
        if (c=='\n') {
            if (o && buf[o-1] != '\n') buf[o++] = '\n';
            i++; continue;
        }
        if (c==' ' && o && buf[o-1]==' ') { i++; continue; }

        buf[o++] = c;
        i++;
    }

    buf[o] = 0;
    pg->text = buf;
    return pg;
}

/* --------- wrapping for terminal width --------- */
static char *wrap_text(const char *in, int width_cols) {
    if (!in) return NULL;
    if (width_cols <= 0) width_cols = 80;

    size_t n = strlen(in);
    /* over-allocate a bit for inserted newlines */
    char *out = (char*)malloc(n + n/width_cols + 16);
    if (!out) return NULL;

    int col = 0;
    size_t o = 0;
    int blank_run = 0;

    for (size_t i=0; i<n; i++) {
        char c = in[i];
        if (c == '\r') continue;

        if (c == '\n') {
            if (col == 0) {
                if (blank_run) continue; /* collapse multiple blank lines */
                blank_run = 1;
            } else {
                blank_run = 0;
            }
            out[o++] = '\n';
            col = 0;
            continue;
        }

        if (c == ' ' && (col == 0 || out[o-1] == ' ')) continue;

        if (width_cols && col >= width_cols && c == ' ') {
            out[o++] = '\n'; col = 0; continue;
        }
        if (width_cols && col >= width_cols) {
            out[o++] = '\n'; col = 0;
        }

        out[o++] = c;
        col++;
        blank_run = 0;
    }
    out[o] = 0;
    return out;
}

/* --------- I/O --------- */
static void print_divider(void) {
    puts("-------------------------------------------------------------------------------");
}

static void print_page(const char *url, const page_t *pg, const char *wrapped) {
    print_divider();
    printf("URL: %s\n", url);
    print_divider();

    if (wrapped && *wrapped) {
        puts(wrapped);
    } else {
        puts("[no renderable text]");
    }

    print_divider();
    printf("Links found: %d\n", pg ? pg->link_count : 0);
    if (pg && pg->link_count > 0) {
        int max_show = pg->link_count < 40 ? pg->link_count : 40; /* list first 40 */
        for (int i=0; i<max_show; i++) {
            printf(" [%d] %s\n", i+1, pg->links[i].href);
        }
        if (pg->link_count > max_show) {
            printf(" ... (%d more not listed)\n", pg->link_count - max_show);
        }
    }
    print_divider();
}

/* Try to interpret user input:
   - empty: reload (return 1)
   - 'r'   : reload (return 1)
   - 'q'   : quit   (return 0, *quit=1)
   - number: open link index (return 2, *next_url set)
   - otherwise: treat as URL (absolute or relative to current) (return 3, *next_url set)
*/
static int interpret_command(const char *line, const char *current_url,
                             const page_t *pg, char *next_url, int *quit) {
    *quit = 0;
    next_url[0] = 0;

    char buf[URL_MAX];
    strncpy(buf, line, sizeof buf);
    buf[sizeof(buf)-1]=0;
    trim_inplace(buf);
    if (!*buf) return 1;              /* reload */

    if (!strcmp(buf, "r") || !strcmp(buf, "R")) return 1;  /* reload */
    if (!strcmp(buf, "q") || !strcmp(buf, "Q")) { *quit = 1; return 0; }

    /* numeric? -> follow link number */
    bool all_digits = true;
    for (const char *p=buf; *p; ++p) if (!isdigit((unsigned char)*p)) { all_digits=false; break; }
    if (all_digits) {
        long idx = strtol(buf, NULL, 10);
        if (pg && idx >= 1 && idx <= pg->link_count) {
            strncpy(next_url, pg->links[idx-1].href, URL_MAX);
            next_url[URL_MAX-1]=0;
            return 2;
        } else {
            fprintf(stderr, "No such link: %ld\n", idx);
            return -1;
        }
    }

    /* otherwise: URL / relative */
    if (!has_scheme(buf) && current_url && *current_url) {
        resolve_url(current_url, buf, next_url, URL_MAX);
    } else {
        strncpy(next_url, buf, URL_MAX);
        next_url[URL_MAX-1]=0;
    }
    return 3;
}

int main(int argc, char **argv) {
    const char *start_url = (argc > 1) ? argv[1] : HOME_URL;
    char current_url[URL_MAX];
    strncpy(current_url, start_url, URL_MAX);
    current_url[URL_MAX-1] = 0;

    curl_global_init(0);

    page_t *pg = NULL;
    char *wrapped = NULL;

    for (;;) {
        /* Fetch */
        mem_t m = {0};
        int rc = fetch_url(current_url, &m, 15);
        if (rc != 0) {
            fprintf(stderr, "Fetch error %d for URL: %s\n", rc, current_url);
            free(m.buf);
            /* still show prompt for another command */
        }

        /* Parse */
        if (rc == 0) {
            page_t *np = html_to_page(m.buf ? m.buf : "", current_url);
            if (!np) {
                fprintf(stderr, "Parse error\n");
            } else {
                if (pg) { free(pg->text); free(pg); }
                pg = np;
                /* Wrap to 80 cols (or read $COLUMNS if you want—keeping it simple) */
                free(wrapped);
                wrapped = wrap_text(pg->text, 80);
            }
        }

        /* Print */
        print_page(current_url, pg, wrapped);

        /* Prompt */
        char line[URL_MAX];
        printf("Command (URL, number, r=reload, q=quit): ");
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin)) break;

        int quit = 0;
        char next_url[URL_MAX];
        int action = interpret_command(line, current_url, pg, next_url, &quit);
        if (quit) break;

        if (action == 2 || action == 3) {
            /* navigate to next_url if it's http(s), else ignore */
            if (!is_http_scheme(next_url)) {
                fprintf(stderr, "Unsupported scheme. Only http/https are allowed.\n");
            } else {
                strncpy(current_url, next_url, URL_MAX);
                current_url[URL_MAX-1]=0;
            }
        }
        /* else: action==1 reload; negative -> error, stay on same URL */

        free(m.buf);
    }

    free(wrapped);
    if (pg) { free(pg->text); free(pg); }
    curl_global_cleanup();
    return 0;
}
