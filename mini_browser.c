#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include "badgevms/wifi.h"
#include "curl/curl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_BYTES  (16 * 1024)
#define TIMEOUT_S  10

typedef struct {
    SDL_Window   *win;
    SDL_Renderer *ren;
    const char   *url_html;
    const char   *url_text;
    const char   *url;       // current
    int           need_fetch;
    int           initted;
} App;

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

static char *html_to_text(const char *h) {
    size_t L = strlen(h), o = 0; char *out = (char*)malloc(L + 1); if (!out) return NULL;
    for (size_t i = 0; i < L; ) {
        if (h[i] == '<') { size_t j=i+1; while (j<L && h[j] != '>') j++;
            int nl=0; if (j>i+1){ char c1=h[i+1], c2=(j>i+2)?h[i+2]:0;
                if ((c1=='b'||c1=='B')&&(c2=='r'||c2=='R')) nl=1; if (c1=='p'||c1=='P') nl=1; }
            if (nl && o && out[o-1] != '\n') out[o++] = '\n'; i = j<L ? j+1 : L; continue; }
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

static int fetch_and_print(const char *url) {
    mem_t m = {0};
    CURL *curl = curl_easy_init(); if (!curl) return -1;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "BadgeVMS-mini-browser/0.4");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, wr_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &m);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, TIMEOUT_S);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, TIMEOUT_S);
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        printf("Fetch failed (%d): %s\n", (int)res, curl_easy_strerror(res));
        free(m.buf);
        return -1;
    }

    char *txt = html_to_text(m.buf ? m.buf : "");
    if (txt) {
        printf("\n--- BEGIN CONTENT (%s) ---\n%s\n--- END CONTENT ---\n", url, txt);
        free(txt);
    } else {
        printf("OOM converting HTML\n");
    }
    free(m.buf);
    return 0;
}

static void draw_ui(SDL_Renderer *r, const char *url, const char *msg) {
    SDL_SetRenderDrawColor(r, 0,0,0,255); SDL_RenderClear(r);
    SDL_FRect top = {0,0,716,24}, mid = {0,30,716,8};
    SDL_SetRenderDrawColor(r, 30,30,30,255); SDL_RenderFillRect(r, &top);
    SDL_SetRenderDrawColor(r, 80,80,80,255); SDL_RenderFillRect(r, &mid);
    SDL_FRect left = {10,5,24,14}, right = {40,5,24,14};
    SDL_SetRenderDrawColor(r, 40,120,255,255); SDL_RenderFillRect(r, &left);   // H
    SDL_SetRenderDrawColor(r, 60,200,60,255);  SDL_RenderFillRect(r, &right);  // E
    SDL_RenderPresent(r);
    if (msg) printf("%s\n", msg);
    printf("URL: %s\nKeys: H=html  E=example  R=reload  Q=quit\n", url);
}

/* ==== SDL callbacks ==== */

SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);
    wifi_connect();              // idempotent inside firmware
    curl_global_init(0);         // safe to call per task

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        printf("SDL init failed: %s\n", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    App *app = (App*)SDL_calloc(1, sizeof(App));
    if (!app) return SDL_APP_FAILURE;

    app->url_html = "https://httpbin.org/html";
    app->url_text = "https://example.org/";
    app->url      = app->url_html;
    app->need_fetch = 1;

    app->win = SDL_CreateWindow("mini_browser", 716, 645, 0);
    if (!app->win) { printf("CreateWindow failed: %s\n", SDL_GetError()); SDL_free(app); return SDL_APP_FAILURE; }

    app->ren = SDL_CreateRenderer(app->win, NULL);
    if (!app->ren) { printf("CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(app->win); SDL_free(app); return SDL_APP_FAILURE; }

    draw_ui(app->ren, app->url, "Ready.");
    *appstate = app;
    app->initted = 1;
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *e) {
    App *app = (App*)appstate;
    if (!app) return SDL_APP_FAILURE;

    if (e->type == SDL_EVENT_QUIT) return SDL_APP_SUCCESS;

    if (e->type == SDL_EVENT_KEY_DOWN) {
        switch (e->key.scancode) {
            case SDL_SCANCODE_Q: return SDL_APP_SUCCESS;
            case SDL_SCANCODE_H: app->url = app->url_html; app->need_fetch = 1; break;
            case SDL_SCANCODE_E: app->url = app->url_text; app->need_fetch = 1; break;
            case SDL_SCANCODE_R: app->need_fetch = 1; break;
            default: break;
        }
    }
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void *appstate) {
    App *app = (App*)appstate;
    if (!app || !app->initted) return SDL_APP_FAILURE;

    if (app->need_fetch) {
        draw_ui(app->ren, app->url, "Fetching...");
        (void)fetch_and_print(app->url);
        draw_ui(app->ren, app->url, "Fetched (see serial).");
        app->need_fetch = 0;
    }
    SDL_Delay(16);
    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *appstate, SDL_AppResult result) {
    App *app = (App*)appstate;
    if (app) {
        if (app->ren) SDL_DestroyRenderer(app->ren);
        if (app->win) SDL_DestroyWindow(app->win);
        SDL_free(app);
    }
    SDL_Quit();
    curl_global_cleanup();
    // intentionally no wifi_disconnect()
}
