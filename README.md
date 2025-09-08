# Mini Browser

A compact, text-oriented web browser for the WHY2025 badge.

`Mini Browser` is a lightweight, SDL-based browser that retrieves and renders plain text web pages.  
It is designed for constrained environments where speed, memory efficiency, and simplicity are essential.

---

## Features

- **Link navigation**  
  All `<a href>` targets are extracted and indexed numerically as `[n]`.  
  Use **Tab** / **Shift+Tab** to select a link, and **Enter** to open it.

- **Direct URL input**  
  Type into the address bar, press **Enter** to navigate.  
  Bare hostnames are automatically expanded (e.g. `example.org → https://example.org`).

- **Redirect handling**  
  Supports standard HTTP 3xx redirects.

- **Relative URL resolution**  
  Handles relative paths (`/foo`, `../bar`, `//host/...`) against the active base URL.

- **Entity decoding**  
  Decodes common HTML entities (`&amp;`, `&lt;`, `&gt;`, `&quot;`, `&#39;`).

- **Keyboard-only interface**  
  Fully controlled using the badge keyboard.

- **URL bar with icon**
  - Cyan square at the top-left with a **yellow magnifying glass** icon.
  - Long URLs are left-clipped with an ellipsis (`...example.com/page`).

---

- **Keyboard controls**
  - **Typing**: edits the URL in the bar (ASCII only).
  - **Enter**: navigate to typed URL, or follow the currently selected `[n]` link.
  - **Backspace**: delete last character in the URL bar.
  - **Scrolling**:
    - `↓`: scroll down
    - `↑`: scroll up
  - **Link selection**:
    - `Shift+Tab`: show links in pages with numbers 
    - `Tab`: show next link
    - `Shift+Tab`: previous link
    - `Enter`: go to selected link
 
  - **WHY key (accelerator) combos** (hold the WHY/`0xE3` key and press…):
    - `E` — **Edit**: prefill URL with `https://`
    - `H` — **Home**: go to home page
    - `R` — **Reload** current page
    - `B` — **Back** in history
    - `Q` — **Quit**
---

## Special Keys

These dedicated keys provide instant navigation:

- 🟥 **Square** → [NPR Text](https://text.npr.org)    
- 🔺 **Triangle** → [Hacker News](https://news.ycombinator.com/) 
- ❌ **Cross** →  [Hacker News](https://news.ycombinator.com/) 
- 🟢 **Circle** → [What is my IP address?](http://ifconfig.co)  
- ☁️ **Cloud** → [Bobcat Browser](https://ohmeadhbh.github.io/bobcat/) 
- 🔷 **Diamond** →   [Curl](https://curl.se) 

---

## Implementation Overview

- **Fetch**: pages retrieved with libcurl (up to 64 KB by default).  
- **Parse**: minimal HTML walker:  
  - skips `<head>`, `<script>`, `<style>`  
  - collapses whitespace and decodes entities  
  - extracts and indexes links  
- **Wrap**: text wrapped to fit viewport width using a fixed 5×7 bitmap font.  
- **Render**: SDL draws the address bar, separators, and text viewport.

---

## Known Limitations

- **No CSS/JS/Images** — purely text output.  
- **ASCII focus** — non-ASCII simplified to `?`.  
- **No cookies or sessions** — no persistence across requests.  
- **Content cap** — maximum page size 64 KB.
- **Rookie project** - it is what it is  

---

## Recommended Sites

- [Wikipedia (mobile)](https://en.m.wikipedia.org/)  
- [Hacker News](https://news.ycombinator.com/)  
- [OpenBSD](https://www.openbsd.org/)  
- [SQLite Documentation](https://sqlite.org/docs.html)  
- [Linux Man Pages](https://man7.org/linux/man-pages/)  
- [RFC Editor](https://www.rfc-editor.org/rfc/)  
- [Plan 9 / cat-v](http://cat-v.org/)  
- [Kernel.org](https://www.kernel.org/)  
- [Lua](https://www.lua.org/)  
- [musl libc](https://musl.libc.org/)

---

## Credits

- Built on **BadgeVMS**, **SDL**, and **libcurl**.  
- Font: custom **5×7 ASCII bitmap**.  
- Developed for the WHY2025 badge community.
- Lots of copy and paste example code .... but it seems to work.
