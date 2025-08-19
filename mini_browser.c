#include "badgevms/wifi.h"
#include "curl/curl.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_BYTES  (16 * 1024)
#define TIMEOUT_S  10

typedef struct { char *buf; size_t len; } mem_t;

static size_t wr_cb(void *ptr, size_t sz, size_t nm, void *ud) {
    size_t n = sz * nm, keep = n;
    mem_t *m = (mem_t*)ud;
    if (m->len >= MAX_BYTES) return n;                 // accept but don't store
    if (m->len + keep > MAX_BYTES) keep = MAX_BYTES - m->len;
    char *p = (char*)realloc(m->buf, m->len + keep + 1);
    if (!p) return 0;
    m->buf = p;
    memcpy(m->buf + m->len, ptr, keep);
    m->len += keep;
    m->buf[m->len] = 0;
    return n; // report full chunk consumed
}

static char *html_to_text(const char *h) {
    size_t L = strlen(h), o = 0;
    char *out = (char*)malloc(L + 1);
    if (!out) return NULL;
    for (size_t i = 0; i < L; ) {
        if (h[i] == '<') {
            size_t j=i+1; while (j<L && h[j] != '>') j++;
            int nl=0; if (j>i+1){ char c1=h[i+1], c2=(j>i+2)?h[i+2]:0;
                if ((c1=='b'||c1=='B')&&(c2=='r'||c2=='R')) nl=1;
                if (c1=='p'||c1=='P') nl=1;
            }
            if (nl && o && out[o-1] != '\n') out[o++] = '\n';
            i = j<L ? j+1 : L;
            continue;
        }
        if (h[i] == '&') {
            if (!strncmp(&h[i],"&amp;",5))  { out[o++]='&'; i+=5; continue; }
            if (!strncmp(&h[i],"&lt;",4))   { out[o++]='<'; i+=4; continue; }
            if (!strncmp(&h[i],"&gt;",4))   { out[o++]='>'; i+=4; continue; }
            if (!strncmp(&h[i],"&quot;",6)) { out[o++]='"'; i+=6; continue; }
            if (!strncmp(&h[i],"&#39;",5))  { out[o++]='\''; i+=5; continue; }
        }
        char c = h[i++]; if (c=='\r') continue;
        if (c=='\n') { if (o && out[o-1] != '\n') out[o++] = '\n'; }
        else { if (!(c==' ' && o && out[o-1]==' ')) out[o++]=c; }
    }
    out[o]=0; return out;
}

static int fetch_url(const char *url, mem_t *m) {
    CURL *curl = curl_easy_init(); if (!curl) return -1;
    m->buf=NULL; m->len=0;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "BadgeVMS-mini-browser/0.3");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, wr_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, m);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, TIMEOUT_S);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, TIMEOUT_S);
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    return (res == CURLE_OK) ? 0 : (int)res;
}

static void draw_ui(SDL_Renderer *r, const char *url, const char *msg) {
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_RenderClear(r);
    SDL_Rect top = {0,0,716,24}, mid = {0,30,716,8};
    SDL_SetRenderDrawColor(r, 30,30,30,255); SDL_RenderFillRect(r, &top);
    SDL_SetRenderDrawColor(r, 80,80,80,255); SDL_RenderFillRect(r, &mid);
    SDL_Rect left = {10,5,24,14}, right = {40,5,24,14};
    SDL_SetRenderDrawColor(r, 40,120,255,255); SDL_RenderFillRect(r, &left);
    SDL_SetRenderDrawColor(r, 60,200,60,255); SDL_RenderFillRect(r, &right);
    SDL_Rect stat = {10,50,200,20};
    SDL_SetRenderDrawColor(r, 200,200,40,255); SDL_RenderFillRect(r, &stat);
    SDL_RenderPresent(r);

    if (msg) printf("%s\n", msg);
    printf("URL: %s\nKeys: H=html E=example R=reload Q=quit\n", url);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    wifi_connect();
    curl_global_init(0);

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        printf("SDL init failed: %s\n", SDL_GetError());
        return 0;
    }

    SDL_Window   *win = SDL_CreateWindow("mini_browser",
                           716, 645, SDL_WINDOW_SHOWN);
    if (!win) { printf("CreateWindow failed: %s\n", SDL_GetError()); SDL_Quit(); return 0; }

    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, 0);
    if (!ren) {
        printf("CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(win); SDL_Quit(); return 0;
    }

    const char *URL_HTML = "https://macip.net";
    const char *URL_TEXT = "https://why2025.org/";
    const char *url = URL_HTML;

    int need_fetch = 1;
    int running = 1;

    while (running) {
        if (need_fetch) {
            mem_t m = {0};
            int rc = fetch_url(url, &m);
            if (rc != 0) {
                char buf[96]; snprintf(buf, sizeof(buf), "Fetch failed (%d)", rc);
                draw_ui(ren, url, buf);
            } else {
                char *txt = html_to_text(m.buf ? m.buf : "");
                draw_ui(ren, url, "Fetched (see serial output)");
                if (txt) {
                    printf("\n--- BEGIN CONTENT ---\n%s\n--- END CONTENT ---\n", txt);
                    free(txt);
                } else {
                    printf("OOM converting HTML\n");
                }
            }
            free(m.buf);
            need_fetch = 0;
        }

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) { running = 0; break; }
            if (ev.type == SDL_KEYDOWN) {
                switch (ev.key.keysym.scancode) {
                    case SDL_SCANCODE_Q: running = 0; break;
                    case SDL_SCANCODE_H: url = URL_HTML; need_fetch = 1; break;
                    case SDL_SCANCODE_E: url = URL_TEXT; need_fetch = 1; break;
                    case SDL_SCANCODE_R: need_fetch = 1; break;
                    default: break;
                }
            }
        }
        SDL_Delay(16);
    }

    // --- graceful teardown sequence ---
    // Keep pumping for a short grace period so the compositor can reclaim buffers.
    Uint32 until = SDL_GetTicks() + 120;
    SDL_Event ev;
    while (SDL_GetTicks() < until) {
        while (SDL_PollEvent(&ev)) { /* drain */ }
        SDL_Delay(5);
    }

    SDL_RenderPresent(ren); // final present (no-op but harmless)
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);

    // Extra tiny delay so the window close fully propagates.
    SDL_Delay(30);

    SDL_Quit();
    curl_global_cleanup();
    // intentionally NOT calling wifi_disconnect() to avoid SDIO races.
    fflush(NULL);
    return 0;
}
