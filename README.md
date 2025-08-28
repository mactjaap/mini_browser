# mini_browser
tiny text-mode web for the WHY2025 badge

**mini_browser** is a ridiculously small, fast, text-centric browser built for the WHY2025 badge.  
It fetches pages with `libcurl`, strips them down to text, collects links, and renders everything with a 5×7 bitmap font via SDL.

---

## ✨ What it can do

- **Follow links** — Every `<a href>` is collected and numbered like `[12]`.  
  Use **Tab / Shift+Tab** to select, **Enter** to open.
- **Type URLs directly** — Type in the address bar; **Enter** to go.  
  Bare hosts auto-expand (e.g. `example.org` → `https://example.org`).
- **Google search box** — A minimal search control is baked in for Google’s homepage.
- **Redirects** — Handles `3xx` redirects (via `CURLOPT_FOLLOWLOCATION`).
- **Relative URLs** — Resolves paths (`/foo`, `../bar`, `//host/...`).
- **Entity decoding** — Decodes `&amp; &lt; &gt; &quot; &#39;`.
- **Keyboard-only** — Optimized for the badge keyboard.

---

## ⌨️ Keys & shortcuts

- **Type**: edit URL/search field
- **Enter**: open the typed URL or the selected link
- **Backspace**: delete in the address bar
- **Tab / Shift+Tab**: next/previous link
- **↑ / k**: scroll up
- **↓ / j**: scroll down
- **PageUp / PageDown**: faster scrolling
- **Home**: jump to top
- **Ctrl+R**: reload
- **Ctrl+Q / Esc**: quit
- **Ctrl+H**: open `https://httpbin.org/html`
- **Ctrl+E**: open `https://badge.why2025.org/` (demo shortcut)

---

## 🛠 How it works (high level)

1. **Fetch**  
   `libcurl` downloads up to `MAX_BYTES` (16 KB by default) with timeouts.
2. **Parse**  
   A tiny HTML walker:
   - Skips `<head>`, `<script>`, `<style>`
   - Emits text, collapses whitespace, decodes entities
   - Captures `<a href>…</a>` targets, appends numbered markers like `[7]`
   - Resolves relative URLs against the current page
3. **Wrap**  
   Text is wrapped to your display width (monospace 5×7 font).
4. **Render**  
   SDL draws the URL bar and the text content; you drive it with the keyboard.

---

## 🔍 Google search (minimal)

- On `www.google.com`, the browser exposes a **minimal search input**:
  - Press **Enter** while the search box is selected, type your query, **Enter** again
  - It navigates to `https://www.google.com/search?q=...`
- You can also type `https://www.google.com/search?q=<your+query>` directly in the bar.

> Tip: If you mainly search, set the start URL to  
> `https://www.google.com/search?q=` and just type your terms after `q=`.

---

## ⚙️ Build & port notes (tl;dr)

- **Networking**: `wifi_connect()` (BadgeVMS), then `curl_global_init()`
- **Fetch**: `CURLOPT_FOLLOWLOCATION`, UA string, connect/read timeouts
- **Text engine**: custom 5×7 bitmap font (ASCII 32–127)
- **UI layout**: URL bar, separators, content viewport; predictable padding

---

## ⚠️ Known limits (by design)

- **No CSS/JS/Images** — It’s text-only. Scripts/styles are skipped entirely.
- **ASCII-first** — Non-ASCII glyphs are simplified; unknowns are shown as `?`.
- **Cookies/sessions** — No persistent cookie jar. (Curl may receive cookies this session, but the browser doesn’t store them.)
- **Small cap** — Body truncated to `MAX_BYTES` (default 16 KB) for speed/memory.

---

## 💡 Tips for better pages

- Prefer **mobile / lite / text** pages where possible.
- Wikipedia, Hacker News, RFCs, documentation, and many blogs work great.
- Long word soup? Add spaces or use mobile views to help the wrapper.

---

## 🧭 Sites to try

- Wikipedia: `https://en.wikipedia.org/wiki/Main_Page`  
  (or the mobile view: `https://en.m.wikipedia.org/`)
- curl: `https://curl.se/`
- OpenBSD: `https://www.openbsd.org/`  
- SQLite docs: `https://sqlite.org/docs.html`
- man7 (Linux man pages): `https://man7.org/linux/man-pages/`
- RFC Editor: `https://www.rfc-editor.org/rfc/` (e.g. `/rfc2616`)
- Hacker News (classic): `https://news.ycombinator.com/`
- Plan 9 / cat-v: `http://cat-v.org/`
- Linux kernel: `https://www.kernel.org/`
- BusyBox: `https://busybox.net/`
- Lua: `https://www.lua.org/`
- Musl libc: `https://musl.libc.org/`
- GitHub raw content:  
  `https://raw.githubusercontent.com/<user>/<repo>/<branch>/README.md`

> Bonus: host your own text pages on GitHub and open the **raw** URL above.

---

## 🧭 Navigation quick-reference

- **Open a link**: Tab to `[n]`, **Enter**  
- **Back to typing**: start typing; selection is cleared automatically  
- **Reload**: **Ctrl+R**  
- **Quit**: **Ctrl+Q** or **Esc**

---

## 🗺 Roadmap ideas

- Optional transliteration for common UTF-8 accents → ASCII
- Basic history (back/forward)
- Per-page link list pop-up
- Bookmark bar (small INI/TOML file)
- Optional gzip body cap bump with PSRAM check

---

## 🙌 Credits

- Built on **BadgeVMS**, **SDL**, and **libcurl**.  
- Font: custom 5×7 ASCII bitmap.  
- Crafted for the WHY2025 badge by the community 💛

---
Happy hacking, and enjoy the web at 5×7 pixels at a time!