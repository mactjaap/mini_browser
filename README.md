# Mini Browser 2.3

A compact, interactive, text-oriented web browser for the WHY2025 badge.

Mini Browser is written in C using SDL3 and libcurl. It retrieves HTML pages, converts them into readable text, extracts links and simple HTML forms, and provides a keyboard-driven browsing interface designed for the 720×720 WHY2025 badge display.

Version **2.3** adds broad Unicode rendering, monochrome Plane-1 emoji, UTF-8-safe pixel-aware wrapping, and genuine rendered bold text for HTML `<b>` and `<strong>`, while retaining the link navigation, bookmarks, Back/Forward history and GET forms from earlier releases.

Mini Browser deliberately does **not** try to be a modern graphical browser. There is no JavaScript engine, CSS layout engine, image renderer, or full DOM. The goal is a small, fast browser for text-oriented and lightweight websites.

## Highlights

- Text-oriented HTML browsing over HTTP and HTTPS
- Up to 64 KiB downloaded per page
- Up to 128 extracted links and 160 interactive actions
- Numbered navigation for links and form controls
- Back and Forward browsing history
- Persistent bookmarks
- Editable URL bar
- Hold Up/Down for fast scrolling
- Simple GET form support
- UTF-8-safe text processing and wrapping
- Pixel-aware wrapping for mixed ASCII and Unicode text
- Broad Unicode support using a generated GNU Unifont bitmap file
- 27,696 generated Unicode glyphs across selected Plane 0 and Plane 1 ranges
- CJK, Greek, Cyrillic, punctuation, symbols and many other scripts/blocks
- Monochrome single-codepoint emoji
- Real rendered bold for `<b>` and `<strong>`
- ASCII `*` markers for unordered lists, so list markers remain usable without the external Unicode font
- Small built-in 5×7 ASCII bitmap font
- No JavaScript, CSS layout or image rendering required

## Unicode and text rendering

ASCII uses the browser's built-in bitmap font. Additional Unicode characters are loaded from:

    APPS:[mini_browser]unifont_cjk.bin

The generated font is based on GNU Unifont 17.0.02 and combines selected glyphs from the Japanese Plane-0 font and the supplementary-plane font.

The current generated font contains **27,696 glyphs** and covers broad ranges including Latin extensions, Greek, Cyrillic, punctuation, currency symbols, arrows, mathematical symbols, box drawing, geometric shapes, CJK punctuation, Hiragana, Katakana, CJK ideographs, fullwidth forms and selected supplementary symbol/emoji blocks.

UTF-8 wrapping is code-point safe: Mini Browser does not intentionally split a multi-byte UTF-8 sequence when wrapping page text. Wrapping is also pixel-aware, accounting for the different rendered widths of the built-in ASCII font and 16×16 Unicode glyphs.

If the external Unicode font is not installed, the built-in ASCII renderer remains available. Unordered HTML list items intentionally use the ASCII `*` marker rather than requiring a Unicode bullet.

## Bold text

HTML `<b>` and `<strong>` are rendered as genuine bold text rather than as visible Markdown-style `**` markers.

Mini Browser implements this in the renderer by drawing bold glyphs with an additional one-pixel horizontal pass. This works with both the built-in ASCII glyphs and glyphs loaded from the Unicode font.

## Emoji

Mini Browser 2.3 can render many supplementary-plane, single-codepoint emoji as monochrome GNU Unifont bitmap glyphs.

Examples include:

    😀 😃 😂 😎 🤖
    👍 👋 🙏
    🐶 🐱 🐼
    🍎 🍕 ☕
    🚗 ✈ 🚀
    🌍 🌙 🔥
    💡 💻 🔑

This is bitmap Unicode rendering, not a color emoji engine.

Mini Browser does not currently compose complex emoji sequences such as ZWJ sequences, skin-tone combinations, gender sequences or regional-indicator flag pairs. Variation-selector handling is also limited.

## HTML forms

Mini Browser supports simple interactive HTML GET forms.

Supported form controls include:

- `<input type="text">`
- `<input type="search">`
- `<input type="url">`
- `<input type="hidden">`
- `<input type="submit">`
- `<button>`
- `<button type="submit">`

Links and visible form controls share the same numbered action system. Type the action number and press Enter to activate it.

GET submissions use URL encoding compatible with `application/x-www-form-urlencoded`. Hidden fields are included, disabled fields are ignored, and the activated named submit button is included where appropriate.

Up to 4 forms with up to 8 stored fields per form are supported.

POST forms are recognised but intentionally not submitted. Complex controls such as `textarea`, `select`, checkboxes, radio buttons, file uploads and JavaScript-driven forms are not currently supported.

## HTML rendering

Mini Browser converts useful HTML structure into a compact text representation.

Supported or specially handled elements include headings, paragraphs, line breaks, ordered and unordered lists, preformatted text, inline code, bold/strong text, emphasis/italic text, horizontal rules, simple table rows/cells, hyperlinks, simple forms, named HTML entities, and decimal/hexadecimal numeric entities.

Unordered list items use `* ` as their marker. This is intentional: the marker works with the built-in ASCII font even when the optional external Unicode font is not installed.

Comments, doctypes, scripts, styles and document head content are ignored for normal page rendering. The page `<title>` is extracted for the top bar.

## Navigation

Every usable link or visible form control receives an action number. Type the number and press Enter to activate it.

### Keyboard controls

| Key | Action |
| --- | --- |
| `0`–`9` + Enter | Activate a numbered link or form action |
| `Enter` | Activate / accept editing |
| `Up` / `Down` | Scroll one line; hold for continuous scrolling |
| `J` / `K` | Scroll down / up one line |
| `Left` / `Right` | Move cursor while editing |
| `Backspace` | Delete while editing |

The WHY2025 key acts as the browser accelerator:

| Shortcut | Action |
| --- | --- |
| `WHY+E` | Enter a new URL |
| `WHY+C` | Edit the current URL |
| `WHY+H` | Home |
| `WHY+R` | Reload |
| `WHY+B` | Back |
| `WHY+G` | Forward |
| `WHY+F` | Add/remove current bookmark |
| `WHY+M` | Open bookmarks |
| `WHY+Q` | Quit |

## Bookmarks and history

Mini Browser stores up to 32 bookmarks and keeps up to 32 HTTP/HTTPS history entries.

`WHY+B` moves backward and `WHY+G` moves forward. Navigating to a new page after going Back truncates the old forward branch. Reloading does not create a duplicate history entry, and GET form submissions participate in the same history.

Bookmark data is stored at:

    APPS:[mini_browser]bookmarks.txt

## Networking

Mini Browser uses libcurl and requests HTTP/1.1 where available. It requests uncompressed transfer data with:

    Accept-Encoding: identity

Redirect following is bounded. Network failures and HTTP errors are shown as readable browser pages.

## Limits

| Resource | Limit |
| --- | ---: |
| Downloaded page data | 64 KiB |
| URL length | 256 bytes |
| Links | 128 |
| Interactive actions | 160 |
| Forms per page | 4 |
| Fields per form | 8 |
| Editable form value | 127 characters |
| Bookmarks | 32 |
| History entries | 32 |

## What Mini Browser does not support

Mini Browser does not currently provide JavaScript execution, CSS layout/styling, images, video/audio, POST form submission, file uploads, complex HTML form controls, a complete HTML5 DOM/parser, color emoji, or complex emoji composition.

Simple server-rendered websites and text-oriented sites work best.

## Architecture

    URL
      |
      v
    libcurl HTTP fetch
      |
      v
    bounded HTML-to-text parser
      |
      +--> links
      +--> forms
      +--> title
      +--> formatting markers
      |
      v
    UTF-8-safe, pixel-aware wrapping
      |
      v
    numbered action model
      |
      v
    SDL3 renderer
      |
      +--> built-in ASCII glyphs
      +--> external Unicode glyphs
      +--> real bold rendering

## Unicode font generation

The external font asset is generated from GNU Unifont 17.0.02 sources. The tested 2.3 font combines:

    unifont_jp-17.0.02.hex
    unifont_upper-17.0.02.hex

into:

    unifont_cjk.bin

The generated binary contains 27,696 usable glyphs in the selected ranges.

GNU Unifont is dual-licensed under the SIL Open Font License 1.1 and GNU GPL version 2 or later with the GNU Font Embedding Exception. When redistributing the generated font asset, include the applicable GNU Unifont licensing and attribution material.

## Building

Mini Browser is part of the WHY2025 BadgeVMS firmware tree. Build it with the existing WHY2025 ESP-IDF project configuration.

Do not casually regenerate the ESP32-P4 target configuration on early P4 badge hardware.

## Project

Source repository:

    https://github.com/mactjaap/mini_browser/

Home page:

    https://minibrowser.macip.net/

## Version

**Mini Browser 2.3**

Version 2.3 combines the stable interactive browser foundation with broad Unicode/emoji rendering, UTF-8-safe pixel-aware wrapping and genuine bold HTML text rendering.
