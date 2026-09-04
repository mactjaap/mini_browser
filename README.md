# Mini Browser

A compact, text-oriented web browser for the **WHY2025 badge**.

Mini Browser is a lightweight browser written in C using SDL and libcurl. It retrieves HTML pages, converts them into readable text, extracts links, and provides a keyboard-driven browsing interface designed specifically for the WHY2025 badge.

It is intentionally small and simple: no JavaScript, no CSS engine, no images, and no attempt to behave like a modern desktop browser.

Current version: **1.6**

---

## Features

### Text-based web browsing

Mini Browser downloads HTML pages and converts them into readable text suitable for the badge's 720×720 display.

Basic HTML structure is recognised, including:

- paragraphs
- headings
- lists
- links
- line breaks
- preformatted text
- page titles

Scripts, stylesheets, and other non-content elements are ignored.

### Link navigation

All `<a href>` targets are extracted from the page.

Links can be selected using the keyboard:

- **Tab** — select the next link
- **Shift+Tab** — select the previous link
- **Enter** — open the selected link

The selected link and its destination URL are shown in the top bar.

### URL editor

Mini Browser includes an editable address bar with a real cursor.

You can:

- type a URL
- move the cursor left and right
- insert characters at the cursor
- use Backspace/Delete
- edit the current URL
- start a fresh URL with `https://`

Bare hostnames are automatically expanded:

`example.org` → `https://example.org`

### Redirect handling

Standard HTTP redirects are followed automatically by libcurl.

### Relative URL resolution

Mini Browser resolves common relative URLs against the current page, including:

- `/path`
- `./path`
- `//example.org/path`
- query strings
- fragments

Full `../` path normalisation is still limited.

### HTML entity decoding

Common HTML entities are converted to readable characters, including:

- `&amp;`
- `&lt;`
- `&gt;`
- `&quot;`
- `&#39;`
- `&nbsp;`

### Page titles

The HTML `<title>` is extracted and displayed in the top bar together with the HTTP status.

### HTTP and connection errors

Network failures and HTTP errors are displayed as readable error pages instead of silently failing.

Connection errors include the libcurl error description and offer the normal browser shortcuts for retry, back, and home.

---

## Bookmarks

Version 1.6 adds **persistent bookmarks**.

Press:

- **WHY+F** — add the current page to bookmarks
- **WHY+F** again — remove the current page from bookmarks
- **WHY+M** — open the bookmarks page

The browser briefly displays:

`BOOKMARK ADDED`

or:

`BOOKMARK REMOVED`

after WHY+F is pressed.

The bookmarks page uses the normal Mini Browser navigation system, so bookmarks can be selected with **Tab / Shift+Tab** and opened with **Enter**.

Up to **32 bookmarks** can currently be stored.

Both the page title and URL are saved.

Bookmarks are stored on the BadgeVMS filesystem in:

`APPS:[mini_browser]bookmarks.txt`

Bookmarks survive:

- quitting and restarting Mini Browser
- rebooting the badge
- completely powering the badge off and on

A **complete BadgeVMS firmware reflash** replaces the application storage image and therefore removes saved bookmarks.

---

## Keyboard Controls

### Normal keys

- **Typing** — enter/edit a URL
- **Enter** — navigate to the typed URL or open the selected link
- **Backspace** — delete before the URL cursor
- **Delete** — delete at the URL cursor
- **Left / Right** — move the URL cursor
- **Up / Down** — scroll through the page
- **Tab** — select next link
- **Shift+Tab** — select previous link

### WHY key shortcuts

Hold the WHY key (`0xE3`) and press:

- **WHY+E** — edit a new URL, prefilled with `https://`
- **WHY+C** — edit the current URL
- **WHY+H** — go to the Mini Browser home page
- **WHY+R** — reload the current page
- **WHY+F** — add/remove current page as a bookmark
- **WHY+M** — open bookmarks
- **WHY+B** — go back
- **WHY+Q** — quit Mini Browser

When the bookmarks page is open, **WHY+B** returns to the webpage you were viewing before opening bookmarks.

---

## Special Keys

The WHY2025 badge's dedicated coloured keys provide instant navigation:

- 🟥 **Square (Red)** → [NPR Text](https://text.npr.org/)
- 🔺 **Triangle (Orange)** → [Hacker News](https://news.ycombinator.com/)
- ❌ **Cross (Yellow)** → [textfiles.com](http://www.textfiles.com/)
- 🟢 **Circle (Green)** → [What is my IP address?](https://ifconfig.co/)
- ☁️ **Cloud (Blue)** → [Bobcat Browser](https://ohmeadhbh.github.io/bobcat/)
- 🔷 **Diamond (Purple)** → [curl](https://curl.se/)

---

## User Interface

The interface is intentionally minimal.

At the top of the screen is the URL/status bar with:

- a cyan square
- yellow magnifying-glass icon
- current URL
- HTTP status
- page title
- selected link information
- temporary status messages

Long URLs are clipped to fit the available display width.

The rest of the screen is used for the text representation of the current webpage.

---

## Implementation Overview

Mini Browser is deliberately small and does not contain a full browser engine.

### Networking

Pages are retrieved using **libcurl**.

The default maximum downloaded page size is:

`64 KB`

HTTP compression is disabled and identity encoding is requested to keep processing predictable on the badge.

### HTML parsing

A small custom HTML parser:

- removes scripts and styles
- skips non-visible `<head>` content
- recognises common block elements
- collapses whitespace
- decodes common HTML entities
- extracts page titles
- extracts and indexes links

### URL handling

The browser performs lightweight URL resolution for absolute and relative links.

It is not intended to implement the complete WHATWG URL specification.

### Rendering

Text is wrapped to the display width and rendered through SDL using a custom fixed **5×7 ASCII bitmap font**.

### Bookmarks

Bookmarks are kept in RAM while Mini Browser is running and written to the BadgeVMS filesystem whenever the bookmark list changes.

They are loaded again when Mini Browser starts.

The bookmark page itself is generated internally as a normal Mini Browser page, allowing it to reuse the existing link selection and rendering code.

---

## Known Limitations

Mini Browser is deliberately not a full web browser.

- **No JavaScript**
- **No CSS rendering**
- **No images**
- **No forms**
- **No cookies or login sessions**
- **ASCII-focused rendering**
- **64 KB page download limit**
- **Maximum 128 extracted links per page**
- **Maximum 32 bookmarks**
- Complex modern websites will often produce poor or unusable output
- Relative paths containing complex `../` traversal are not fully normalised
- Reflashing the complete BadgeVMS firmware removes saved bookmarks

Simple HTML and text-oriented websites work best.

---

## Recommended Sites

Mini Browser works particularly well with lightweight and text-oriented websites.

- [NPR Text](https://text.npr.org/)
- [Hacker News](https://news.ycombinator.com/)
- [textfiles.com](http://www.textfiles.com/)
- [curl](https://curl.se/)
- [Wikipedia Mobile](https://en.m.wikipedia.org/)
- [OpenBSD](https://www.openbsd.org/)
- [SQLite Documentation](https://sqlite.org/docs.html)
- [Linux Man Pages](https://man7.org/linux/man-pages/)
- [RFC Editor](https://www.rfc-editor.org/rfc/)
- [Kernel.org](https://www.kernel.org/)
- [Lua](https://www.lua.org/)
- [musl libc](https://musl.libc.org/)

---

## Building

Mini Browser is built as a BadgeVMS application inside the WHY2025 firmware tree.

For example:

```bash
cp mini_browser.c /root/firmware/sdk_apps/mini_browser/mini_browser.c
cp manifest.json /root/firmware/sdk_apps/mini_browser/manifest.json

cd /root/firmware
idf.py build
```

To flash the complete firmware to the ESP32-P4 badge:

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

Note that flashing the complete firmware also flashes the generated BadgeVMS storage image. Runtime-created files such as saved Mini Browser bookmarks are therefore replaced.

---

## Version 1.6

Version 1.6 introduces the first complete bookmark implementation:

- persistent bookmarks
- WHY+F add/remove bookmark
- WHY+M bookmark browser
- page title + URL stored for each bookmark
- maximum 32 bookmarks
- bookmark persistence across application restarts
- bookmark persistence across badge power cycles
- visible `BOOKMARK ADDED` / `BOOKMARK REMOVED` feedback
- proper WHY+B behaviour from the bookmarks page
- bookmarks use the normal Tab / Shift+Tab / Enter navigation

---

## Credits

- Built on **BadgeVMS**, **SDL**, and **libcurl**
- Custom **5×7 ASCII bitmap font**
- Developed for the WHY2025 badge community
- Lots of experimenting, debugging, copy-and-paste, and example code...

...but it seems to work. :-)

## mini-browser for Linux
If you don't have a badge .... try the mini-browser for Linux. How to included.


