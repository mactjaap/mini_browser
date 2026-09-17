# Mini Browser 2.5

![Mini Browser logo Logo](Mini_Browser_Logo-small.jpg)

A compact, interactive, text-oriented web browser for the WHY2025 badge.

Mini Browser is written in C using SDL3 and libcurl. It retrieves HTML
pages, converts them into readable text, extracts links and simple HTML
forms, and provides a keyboard-driven browsing interface designed for
the 720×720 WHY2025 badge display.

Version **2.5** builds on the 2.4 Unicode and rendering foundation with a
more robust HTML/entity parser, editable action-number input, bounded POST
form submission, a bounded in-memory cookie jar, and a `WHY+I` HTTP/TLS
Inspector. It retains the 56,352-glyph Unicode font, word-aware wrapping,
RTL/Bidi rendering, contextual Arabic shaping, bookmarks, history and
complete-page screenshots.

Mini Browser deliberately does **not** try to be a modern graphical
browser. There is no JavaScript engine, CSS layout engine, image
renderer, or full DOM. The goal is a small, fast browser for
text-oriented and lightweight websites.

## Highlights

-   Text-oriented HTML browsing over HTTP and HTTPS
-   Up to 64 KiB downloaded per page
-   Up to 128 extracted links and 160 interactive actions
-   Numbered navigation for links and form controls
-   Editable action-number input with Backspace correction
-   Back and Forward browsing history
-   Persistent bookmarks
-   Bounded in-memory session cookie jar
-   Editable URL bar
-   Hold Up/Down for fast scrolling
-   Simple GET and POST form support
-   UTF-8-safe text processing and word-aware wrapping
-   Pixel-aware wrapping for mixed ASCII and Unicode text
-   Whole-word wrapping where possible, with glyph-level fallback for
    CJK and overlong tokens
-   Broad Unicode support using a generated GNU Unifont bitmap file
-   56,352 usable Unicode glyphs across 101 selected ranges
-   RTL/Bidi rendering for Hebrew and Arabic
-   Contextual Arabic shaping
-   CJK, Greek, Cyrillic, punctuation, symbols and many other
    scripts/blocks
-   Monochrome single-codepoint emoji
-   Real rendered bold for `<b>` and `<strong>`
-   `WHY+S` visible 716×716 screenshot capture with reliable serial
    transfer
-   `WHY+Z` complete rendered-page screenshot capture without allocating
    a giant framebuffer
-   `WHY+I` HTTP/TLS Inspector for request, response, page and resource information
-   Standalone `badge_screenshot.py` receiver for macOS/Linux
-   ASCII `*` markers for unordered lists, so list markers remain usable
    without the external Unicode font
-   Small built-in 5×7 ASCII bitmap font
-   No JavaScript, CSS layout or image rendering required

## Unicode and text rendering

ASCII uses the browser's built-in bitmap font. Additional Unicode
characters are loaded from:

    APPS:[mini_browser]unifont_cjk.bin

The generated font is based on GNU Unifont 17.0.02 and combines selected
glyphs from the Japanese Plane-0 font and the supplementary-plane font.

The 2.4 generated font contains **56,352 usable glyphs in 101 ranges**.
Coverage includes broad Latin extensions (including Icelandic), Greek,
Cyrillic, Armenian, Hebrew, Arabic and Arabic presentation forms, Indic
scripts, Thai, Lao, Tibetan, Georgian, Ethiopic, Cherokee, Runic,
Hangul/Korean, Hiragana, Katakana, Bopomofo, CJK Extension A, CJK
ideographs, Yi, punctuation, currency, mathematical and technical
symbols, and selected supplementary symbol/emoji blocks.

The 2.4 loader reads the range table directly from the MBCJ v1 font
header, so Unicode range offsets are no longer hard-coded in the C
source.

Wrapping is UTF-8 safe, pixel aware and word aware. Mini Browser keeps
complete words together where possible while accounting for the
different rendered widths of the built-in ASCII font and 16×16 Unicode
glyphs. CJK text and tokens wider than a complete line fall back to
UTF-8-safe glyph-level wrapping.

Hebrew and Arabic are rendered right-to-left. Arabic letters are
contextually shaped into joining forms. The implementation is
deliberately compact for BadgeVMS and is not presented as complete
support for every Unicode Bidirectional Algorithm control or every
complex-script shaping rule.

If the external Unicode font is not installed, the built-in ASCII
renderer remains available. Unordered HTML list items intentionally use
the ASCII `*` marker rather than requiring a Unicode bullet.

## Bold text

HTML `<b>` and `<strong>` are rendered as genuine bold text rather than
as visible Markdown-style `**` markers.

Mini Browser implements this in the renderer by drawing bold glyphs with
an additional one-pixel horizontal pass. This works with both the
built-in ASCII glyphs and glyphs loaded from the Unicode font.

## Emoji

Mini Browser 2.4 can render many supplementary-plane, single-codepoint
emoji as monochrome GNU Unifont bitmap glyphs.

Examples include:

    😀 😃 😂 😎 🤖
    👍 👋 🙏
    🐶 🐱 🐼
    🍎 🍕 ☕
    🚗 ✈ 🚀
    🌍 🌙 🔥
    💡 💻 🔑

This is bitmap Unicode rendering, not a color emoji engine.

Mini Browser does not currently compose complex emoji sequences such as
ZWJ sequences, skin-tone combinations, gender sequences or
regional-indicator flag pairs. Variation-selector handling is also
limited.

## HTML forms

Mini Browser supports simple interactive HTML GET and POST forms.

Supported form controls include:

-   `<input type="text">`
-   `<input type="search">`
-   `<input type="url">`
-   `<input type="hidden">`
-   `<input type="submit">`
-   `<button>`
-   `<button type="submit">`

Links and visible form controls share the same numbered action system.
Type the action number and press Enter to activate it.

GET and POST submissions use URL encoding compatible with
`application/x-www-form-urlencoded`. Hidden fields are included,
disabled fields are ignored, and the activated named submit button is
included where appropriate. POST request bodies are bounded to 2048 bytes.

Up to 4 forms with up to 8 stored fields per form are supported.

Complex controls such as `textarea`, `select`, checkboxes, radio buttons, file
uploads and JavaScript-driven forms are not currently supported.

## HTML rendering

Mini Browser converts useful HTML structure into a compact text
representation.

Supported or specially handled elements include headings, paragraphs,
line breaks, ordered and unordered lists, preformatted text, inline
code, bold/strong text, emphasis/italic text, horizontal rules, simple
table rows/cells, hyperlinks, simple forms, named HTML entities, and
decimal/hexadecimal numeric entities.

Unordered list items use `*` as their marker. This is intentional: the
marker works with the built-in ASCII font even when the optional
external Unicode font is not installed.

Comments, doctypes, scripts, styles and document head content are
ignored for normal page rendering. The page `<title>` is extracted for
the top bar.

## Navigation

Every usable link or visible form control receives an action number.
Type the number and press Enter to activate it.

### Keyboard controls

  Key                Action
  ------------------ ------------------------------------------------
  `0`--`9` + Enter   Activate a numbered link or form action
  `Enter`            Activate / accept editing
  `Up` / `Down`      Scroll one line; hold for continuous scrolling
  `J` / `K`          Scroll down / up one line
  `Left` / `Right`   Move cursor while editing
  `Backspace`        Delete while editing

The WHY2025 key acts as the browser accelerator:

  Shortcut   Action
  ---------- -------------------------------------------------
  `WHY+E`    Enter a new URL
  `WHY+C`    Edit the current URL
  `WHY+H`    Home
  `WHY+R`    Reload
  `WHY+B`    Back
  `WHY+G`    Forward
  `WHY+F`    Add/remove current bookmark
  `WHY+M`    Open bookmarks
  `WHY+S`    Capture and transmit the visible viewport
  `WHY+Z`    Capture and transmit the complete rendered page
  `WHY+I`    Open HTTP/TLS Inspector / Page Information
  `WHY+Q`    Quit

## Screenshots

Mini Browser 2.4 supports two screenshot modes:

-   `WHY+S` captures the visible 716×716 browser viewport.
-   `WHY+Z` captures the complete rendered page, including content below
    the current viewport.

`WHY+Z` reuses the normal renderer as a slice-based scratch surface and
streams the result as one logical tall image. It does not allocate a
716×N framebuffer and does not change the current scroll position.

`WHY+S` captures the rendered browser view.

The screenshot is read from the SDL renderer as RGB24 data, compressed
with the browser's lightweight RLE5 format, protected with per-record
CRC32 and XOR forward-error correction, and transmitted over the badge's
USB serial connection. The current browser viewport is 716×716 pixels.

The repository includes the standalone receiver:

    badge_screenshot.py

On macOS, for example:

    ./badge_screenshot.py /dev/cu.wchusbserial10

On Linux the serial device will commonly look like:

    ./badge_screenshot.py /dev/ttyUSB0

Start the receiver and press `WHY+S` on the badge. The receiver
validates and repairs the serial transfer where possible, verifies the
final CRC32, decodes the RLE stream and writes a normal PNG file.

A custom firmware is **not required** for physical `WHY+S` screenshot
capture. The screenshot-enabled Mini Browser and a USB serial connection
are sufficient.

The receiver also supports:

    ./badge_screenshot.py /dev/cu.wchusbserial10 --request

For a complete page:

    ./badge_screenshot.py /dev/cu.wchusbserial10 --full-page

For a host-requested complete page:

    ./badge_screenshot.py /dev/cu.wchusbserial10 --full-page --request

`--request` sends the appropriate WHY shortcut from the host instead of
requiring a physical keypress. This requires the custom firmware
described below because stock BadgeVMS firmware does not implement the
host-to-badge serial keyboard protocol.

## Optional custom firmware and automated testing

A customized WHY2025 firmware is available at:

    https://github.com/mactjaap/firmware

This firmware extends the BadgeVMS keyboard path with a serial keyboard
bridge. A macOS/Linux host can send keyboard events over the same USB
serial connection and they enter Mini Browser through the normal
BadgeVMS/SDL keyboard event path.

The firmware repository includes `badge_keyboard.py`, which makes it
possible to operate Mini Browser from a computer keyboard. Browser
accelerator commands such as `WHY+E`, `WHY+H`, `WHY+R`, `WHY+B`,
`WHY+G`, `WHY+F`, `WHY+S`, `WHY+Z` and `WHY+Q` can therefore be
generated remotely.

The custom firmware repository also contains automated Mini Browser test
scripts. These can drive browser navigation, open configured websites,
perform searches, exercise browser commands and automatically request
and save screenshots after test steps. This is useful for repeatable
regression testing without manually operating the badge for every page.

The custom firmware is optional for normal Mini Browser use and for
screenshots triggered physically with `WHY+S`. It is required when the
host needs to send keyboard commands to the badge, including fully
automated tests and `badge_screenshot.py --request`.

## Opening URLs from macOS or Linux

The repository includes `badge_open_url.py`, a small host-side tool for sending URLs
directly to Mini Browser over the custom firmware's serial keyboard bridge. This is
useful for testing because URLs can be pasted on the computer instead of being typed
manually on the badge.

The script requires Python 3 and `pyserial`:

``` sh
python3 -m pip install pyserial
```

To paste a URL interactively:

``` sh
./badge_open_url.py
```

Paste the URL at the prompt and press Enter. The script sends `WHY+E`, enters the URL
through the normal BadgeVMS keyboard path and presses Enter.

A URL can also be supplied directly:

``` sh
./badge_open_url.py https://example.com/
```

To open the URL currently on the macOS or Linux clipboard:

``` sh
./badge_open_url.py --clipboard
```

The script can also run a list of URLs as a simple Mini Browser presentation. Put one
URL per line in a text file. Blank lines and lines beginning with `#` are ignored.

For example, `urls.txt`:

``` text
# Mini Browser demonstration
https://example.com/
https://news.ycombinator.com/
https://wiby.me/
```

Run the presentation with:

``` sh
./badge_open_url.py -f urls.txt
```

Each page is displayed for 30 seconds by default. Change the interval with `-s`:

``` sh
./badge_open_url.py -f urls.txt -s 10
```

Use `-z` to request a complete `WHY+Z` screenshot of every page in the list:

``` sh
./badge_open_url.py -f urls.txt -z
```

This combines the URL presentation with `badge_screenshot.py --full-page --request`.
The screenshot receiver can be specified explicitly:

``` sh
./badge_open_url.py -f urls.txt -z --screenshot-script ./badge_screenshot.py
```

When `badge_screenshot.py` is in the same directory as `badge_open_url.py`, it is
normally found automatically. Screenshots are written through the screenshot receiver;
the presentation script uses a `screenshots` directory by default.

A useful automated demonstration and screenshot run is therefore:

``` sh
./badge_open_url.py -f urls.txt -s 15 -z
```

The script auto-detects common macOS and Linux serial devices. A device can also be
selected explicitly:

``` sh
./badge_open_url.py --device /dev/cu.wchusbserial10 https://example.com/
```

On Linux the device will commonly be something such as `/dev/ttyUSB0` or
`/dev/ttyACM0`.

Because URL entry and host-requested screenshots use the serial keyboard bridge,
`badge_open_url.py` requires the customized WHY2025 firmware described above.

## HTTP/TLS Inspector

Press `WHY+I` to open the HTTP/TLS Inspector for the current page. It shows
bounded metadata retained from the actual request, without making an extra
HEAD request. Information includes page title and size, link/form/action
counts, request method and URL, transport, cookies sent, HTTP status and
Content-Type, plus the current cookie-jar usage and browser limits.

The trimmed BadgeVMS libcurl exposes only a small set of `CURLINFO` fields.
Connection address/port, negotiated HTTP version, certificate details and
certificate-verification result are therefore reported as unavailable rather
than guessed. Effective/final URL and redirect metadata are also reported as
unavailable because they are not reliable in this environment.

`WHY+B` or `WHY+I` returns to the page.

## Cookies

Mini Browser 2.5 includes a bounded in-memory session cookie jar. It stores up
to 12 cookies, supports replacement, path scoping and deletion with
`Max-Age=0`, and sends matching cookies on subsequent requests. Cookies are
kept only in memory and are not persisted across browser restarts.

## Bookmarks and history

Mini Browser stores up to 32 bookmarks and keeps up to 32 HTTP/HTTPS
history entries.

`WHY+B` moves backward and `WHY+G` moves forward. Navigating to a new
page after going Back truncates the old forward branch. Reloading does
not create a duplicate history entry, and GET and POST form submissions
participate in the same history.

Bookmark data is stored at:

    APPS:[mini_browser]bookmarks.txt

## Networking

Mini Browser uses libcurl and requests HTTP/1.1 where available. It
requests uncompressed transfer data with:

    Accept-Encoding: identity

Redirect following is bounded. Network failures and HTTP errors are
shown as readable browser pages.

## Limits

  Resource                          Limit
  ---------------------- ----------------
  Downloaded page data             64 KiB
  URL length                    256 bytes
  Links                               128
  Interactive actions                 160
  Forms per page                        4
  Fields per form                       8
  Editable form value      127 characters
  Bookmarks                            32
  History entries                      32
  Cookies                              12
  POST request body              2048 bytes

## What Mini Browser does not support

Mini Browser does not currently provide JavaScript execution, CSS
layout/styling, images, video/audio, file uploads,
complex HTML form controls, a complete HTML5 DOM/parser, color emoji, or
complex emoji composition.

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
    UTF-8-safe, word-aware, pixel-aware wrapping
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
      +--> RTL/Bidi line rendering
      +--> contextual Arabic shaping

## Unicode font generation

The external font asset is generated from GNU Unifont 17.0.02 sources.
The tested 2.4 font combines:

    unifont_jp-17.0.02.hex
    unifont_upper-17.0.02.hex

into:

    unifont_cjk.bin

The 2.4 generated MBCJ v1 binary contains **56,352 usable glyphs across
101 ranges** and is 1,804,488 bytes (about 1.72 MiB). The browser reads
its range table from the file header at runtime.

GNU Unifont is dual-licensed under the SIL Open Font License 1.1 and GNU
GPL version 2 or later with the GNU Font Embedding Exception. When
redistributing the generated font asset, include the applicable GNU
Unifont licensing and attribution material.

## Building

Mini Browser is part of the WHY2025 BadgeVMS firmware tree. Build it
with the existing WHY2025 ESP-IDF project configuration.

Do not casually regenerate the ESP32-P4 target configuration on early P4
badge hardware.

## Project

Source repository:

    https://github.com/mactjaap/mini_browser/

Home page:

    https://minibrowser.macip.net/

## Version

**Mini Browser 2.5**

Version 2.5 adds robust HTML/entity parsing, editable action-number input,
bounded POST submission, a bounded in-memory cookie jar and the `WHY+I`
HTTP/TLS Inspector to the 2.4 Unicode/rendering foundation. The final 2.5
release passed all 49 automated regression tests on the WHY2025 badge.
