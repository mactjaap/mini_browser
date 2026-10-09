# Mini Browser 4.4

![Mini Browser logo Logo](Mini_Browser_Logo-small.jpg)

A compact, interactive web browser for the WHY2025 badge.

Mini Browser is written in C using SDL3 and libcurl. It retrieves HTML
pages, converts them into readable text, extracts links and simple HTML
forms, and provides a keyboard-driven browsing interface designed for
the 720×720 WHY2025 badge display.

Version **4.4** completes the road towards a browser that feels like a modern
mobile browser while staying small. It adds find in page, a focus ring for
links and fields, text zoom, a simplified (reader) view for long articles,
pages saved for offline reading, sharing a page as a QR code, a Page
Information screen with a lock for secure pages, and a built-in help page:
press `WHY+/` (the `/?` key) to see every key.

Version 4.3 added a loading line with progress, a stop key, clear error pages,
an instant Back/Forward cache, compressed (gzip) pages, connection reuse,
cookies that are kept after a restart, a 2 MB disk cache, downloads, and
images that load after the text. Version 4.1 added an omnibox for addresses and
searches with suggestions, a persistent history of visited pages and up to five
tabs.

Version **3.0** built on the Unicode, forms, cookies and HTTP/TLS foundations
of the 2.x releases with PNG, JPEG and GIF image support, inline image rendering,
image sizing and scaling, multiple display modes and fullscreen BadgeVMS operation.
4.x retains the 56,352-glyph Unicode font, word-aware wrapping, RTL/Bidi rendering,
contextual Arabic shaping, bookmarks, history, GET/POST forms, cookies and
complete-page screenshots.

Mini Browser deliberately does **not** try to be a modern graphical browser.
There is no JavaScript engine, CSS layout engine or full DOM. Version 3.0 adds
bounded image rendering while keeping the goal of a small, fast browser for
lightweight and server-rendered websites.

## Highlights

-   Text-oriented HTML browsing over HTTP and HTTPS
-   Up to 64 KiB downloaded per page
-   Up to 128 extracted links and 160 interactive actions
-   Numbered navigation for links and form controls
-   Editable action-number input with Backspace correction
-   Omnibox (`WHY+L`): one bar for addresses and Google searches, with
    suggestions from bookmarks and history while typing (4.1)
-   Persistent history of the last 150 visited pages (`WHY+Y`) (4.1)
-   Up to five tabs with a new tab page and a tab overview (`WHY+T`,
    `WHY+A`) (4.1)
-   Loading line with progress; `Esc` stops loading (4.3)
-   Clear error pages ("This site can't be reached", "HTTP ERROR 404");
    `R` or `Enter` reloads (4.3)
-   Back/Forward cache: the last three pages return instantly (4.3)
-   Compressed (gzip/deflate) pages and connection reuse with BadgeVMS 4.3
    firmware (4.3)
-   Cookies kept across restarts (those with `Expires` or `Max-Age`),
    up to 32 (4.3)
-   Disk cache of 2 MB on `FLASH0:` with `ETag` / `Last-Modified`
    revalidation (4.3)
-   Downloads of files the browser cannot show, up to 4 MB each (`WHY+D`)
    (4.3)
-   Text first, images in the background; `Esc` stops the images (4.3)
-   Find in page with highlighted matches (`WHY+F`, `n`, `Shift+N`) (4.4)
-   Focus ring for links and form fields (`Tab`, `Shift+Tab`) (4.4)
-   Text zoom 100/150/200 % (`WHY+=`, `WHY+-`, `WHY+0`) (4.4)
-   Simplified view for long articles (`WHY+V`) (4.4)
-   Pages saved for offline reading (`WHY+P`) (4.4)
-   Share a page as a QR code and on the serial port (`WHY+U`) (4.4)
-   Page Information with security, load time, sizes and cookies, and a
    lock in the bar for HTTPS pages (`WHY+I`) (4.4)
-   Built-in help with every key (`WHY+/`) (4.4)
-   Back and Forward browsing history
-   Persistent bookmarks (`WHY+K`)
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
-   PNG, JPEG and GIF image rendering
-   Inline images integrated with normal page layout
-   Image sizing and scaling for the badge display
-   Multiple display/rendering modes
-   BadgeVMS fullscreen operation via `SDL_WINDOW_FULLSCREEN`
-   `WHY+I` HTTP/TLS Inspector for request, response, page and resource information
-   Standalone `badge_screenshot.py` receiver for macOS/Linux
-   ASCII `*` markers for unordered lists, so list markers remain usable
    without the external Unicode font
-   Small built-in 5×7 ASCII bitmap font
-   No JavaScript engine, CSS layout engine or full DOM required

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

## Omnibox: addresses and searches (4.1)

Press `WHY+L` to open an empty address bar, like Ctrl+L in a desktop browser.
Type a web address or a few words and press Enter:

-   A host name such as `wiby.me` or `minibrowser.macip.net/readme.html` opens
    over https.
-   A local address such as `192.168.1.10/status` or `localhost:8080` opens
    over http, because devices on a local network rarely use TLS.
-   Text without a host name, such as `esp32 badge` or a single word, becomes a
    Google search. The search uses `http://www.google.com/search`, which shows
    results directly instead of the EU cookie-consent page that the https
    version shows.

`WHY+E` still opens the bar with `https://` already filled in, as before. A
word without a dot typed after it, such as `esp32`, now also becomes a search
instead of a failing address.

While you type, up to five suggestions appear under the bar: bookmarks first
(yellow, marked `*`), then visited pages, matching the address or the page
title. `Down`/`Up` select a suggestion, `Enter` opens it, and `Esc` closes the
bar and returns to the current page.

## Tabs (4.1)

Mini Browser can keep up to five pages open in tabs. `WHY+T` opens a new tab
next to the current one with a new tab page that lists your bookmarks and
recently visited pages, and opens the omnibox. Each tab keeps its own page,
scroll position and Back/Forward list. With more than one tab open, the bar
shows which tab you are on, for example `[2/3] Page title`.

-   `WHY+Tab` goes to the next tab, `WHY+1` ... `WHY+5` to a tab by number.
-   `WHY+A` opens the tab overview: `Up`/`Down` select, `Enter` opens,
    `X` closes the selected tab and `Esc` returns.
-   `WHY+W` closes the current tab. The last tab is never closed; `WHY+Q`
    quits the browser.

Tabs in the background keep their text; their images are loaded again when
the tab returns to the front.

## Loading, stopping and error pages (4.3)

While a page loads, the bar shows how much has arrived (for example
`Loading 12 KB of 40 KB - Esc stops`) with a progress strip under it. The
current page stays visible until the new one is ready, as in Chrome. When the
page has images, the bar shows `Image 2 of 5` while they arrive.

Press `Esc` to stop loading:

-   If nothing has arrived yet, the old page and its address stay.
-   If part of the page has arrived, that part is shown.
-   While images load, the remaining images are skipped.

A page that cannot be loaded is shown as a clear error page with what went
wrong, for example *This site can't be reached - the server IP address could
not be found*, *refused to connect*, *took too long to respond*, a secure
connection or certificate problem, *This page can't be found* (HTTP 404) or
*This page isn't working* (HTTP 5xx), with a code such as
`ERR_NAME_NOT_RESOLVED` or `HTTP ERROR 404`. Press `R` or `Enter` to try again,
or `WHY+B` to go back.

Stop and the precise error messages need the BadgeVMS 4.3 firmware (see
Networking); on older firmware pages load as before.

## Images in the background (4.3)

The text of a page is shown as soon as it has arrived; its images follow one
by one while you read, and the bar shows `[img 2/5]` meanwhile. `Esc` stops
the remaining images (and no longer quits the browser while they load).
Scrolling and other keys keep working while images load.

## Cookies, disk cache and downloads (4.3)

Cookies with `Expires` or `Max-Age` are kept after a restart in
`APPS:[mini_browser]cookies.txt`; session cookies stay in memory only, as in
other browsers. Up to 32 cookies are kept, scoped to the site that set them.

Pages and images are cached on flash in `FLASH0:[MBCACHE]` (2 MB, least
recently used entries go first). A cached copy that is still fresh is used
without a request; otherwise the browser asks the server with
`If-None-Match` / `If-Modified-Since` and a `304 Not Modified` answer shows the
cached copy. `WHY+R` always asks the server. POST results, error pages and
`no-store` responses are never cached.

A link to a file the browser cannot show (zip, pdf, mp3, ...) or a response
sent as an attachment asks *Download?*: `Enter` or `Y` saves it in
`FLASH0:[DOWNLOADS]` (max 4 MB per file), `Esc` or `N` cancels. `WHY+D` lists
the downloads; `WHY+X` on that page deletes them.

`WHY+X` on the Page Information page clears all cookies and the disk cache.

## Find, focus ring and text zoom (4.4)

Press `WHY+F` and type a word: the matches light up while you type (the
current one in orange) and the bar shows `Find: word  3 of 12`. `Enter`
closes the find bar and keeps the highlights; `n` and `Shift+N` (or `Down`
and `Up` in the find bar) go to the next and previous match, and `Esc` turns
them off. Upper and lower case are treated the same.

`Tab` and `Shift+Tab` move a cyan ring over the links and form fields of the
page, scrolling it into view; `Enter` opens the one with the ring. Typing a
number and `Enter` still works as before.

`WHY+=` and `WHY+-` change the page text between 100 %, 150 % and 200 %;
`WHY+0` returns to 100 %. The page is wrapped again for the new size, the bar
shows the size when it is not 100 % (for example `150% Page title`) and the
Options page (`WHY+O`) shows it too. The bar and menus keep their size. The
zoom is for the current session only.

## Simplified view (4.4)

Many pages surround their text with menus, sidebars, share buttons and
footers. On a long article Mini Browser offers a *Show simplified view*
chip at the bottom of the screen; `WHY+V` then shows only the article: its
title, text and images. `WHY+V` again returns to the full page. The chip goes
away when you scroll down.

The article is found by scoring the containers of the page (paragraphs,
commas, text length, link density and class names such as `article`,
`content`, `nav` or `sidebar`), in the spirit of Firefox's Readability. Pages
that need JavaScript often still read well in the simplified view.

## Saved pages and sharing (4.4)

`WHY+P` saves the text of the page (or of its simplified view) as a text file
in `FLASH0:[SAVED]`, to read later without a network. Saving the same page
again replaces the earlier copy. The Downloads page (`WHY+D`) lists saved
pages under *SAVED PAGES*; open one by its number. `WHY+X` on that page
deletes the downloads and the saved pages.

`WHY+U` shows a QR code of the page address, to open it on a phone, and sends
the line `SHARE <address>` on the USB serial port. A `user:password@` part of
an address is never shared. Any key returns to the page.

## Help (4.4)

`WHY+/` (the `/?` key) opens a help page that lists every key, grouped by
task, together with where Mini Browser keeps its files. It scrolls and zooms
like any page; `WHY+/` or `WHY+B` returns to the page you came from.

## Navigation

Every usable link or visible form control receives an action number.
Type the number and press Enter to activate it.

### Keyboard controls

  Key                Action
  ------------------ ------------------------------------------------
  `0`--`9` + Enter   Activate a numbered link or form action
  `Tab`              Move the focus ring to the next link or field (4.4)
  `Shift+Tab`        Move the focus ring to the previous one (4.4)
  `Enter`            Activate the link with the ring / accept editing
  `Up` / `Down`      Scroll one line; hold for continuous scrolling;
                     select a suggestion in the omnibox
  `J` / `K`          Scroll down / up one line
  `Left` / `Right`   Move cursor while editing
  `Backspace`        Delete while editing
  `n` / `Shift+N`    Next / previous find match (4.4)
  `Esc`              Stop loading or images (4.3); clear find matches
                     (4.4); close the omnibox or an overlay; quit when
                     nothing else is open
  `R`                On an error page: reload (4.3)

The WHY2025 key acts as the browser accelerator:

  Shortcut   Action
  ---------- -------------------------------------------------
  `WHY+/`    Help with every key (4.4)
  `WHY+L`    Omnibox: type an address or a search (4.1)
  `WHY+E`    Enter a new URL
  `WHY+C`    Edit the current URL
  `WHY+H`    Home
  `WHY+R`    Reload
  `WHY+B`    Back (from the Back/Forward cache when possible)
  `WHY+G`    Forward (from the Back/Forward cache when possible)
  `WHY+T`    Open a new tab (4.1)
  `WHY+W`    Close the current tab (4.1)
  `WHY+Tab`  Next tab (4.1)
  `WHY+1-5`  Go to tab 1-5 (4.1)
  `WHY+A`    Tab overview (4.1)
  `WHY+F`    Find in page (4.4)
  `WHY+V`    Simplified view (4.4)
  `WHY+=`    Bigger text (4.4)
  `WHY+-`    Smaller text (4.4)
  `WHY+0`    Text back to 100 % (4.4)
  `WHY+K`    Add/remove current bookmark (4.4: was `WHY+F`)
  `WHY+M`    Open bookmarks
  `WHY+Y`    Open the history of visited pages (4.1)
  `WHY+P`    Save the page for offline reading (4.4)
  `WHY+D`    Downloads and saved pages (4.3)
  `WHY+U`    Share the page as a QR code (4.4)
  `WHY+X`    Clear: history (history page), downloads and saved
             pages (downloads page), cookies and cache (Page
             Information)
  `WHY+O`    Open display mode options
  `WHY+S`    Capture and transmit the visible viewport
  `WHY+Z`    Capture and transmit the complete rendered page
  `WHY+I`    Open HTTP/TLS Inspector / Page Information
  `WHY+Q`    Quit

## Images and display modes

Mini Browser 3.0 adds bounded JPEG, PNG and GIF image rendering. Images are
sized/scaled for the badge display. Press `WHY+O` to open the display mode
options and choose between four rendering modes:

1. **Black & White** - black-and-white page rendering.
2. **Colors** - color page rendering without inline images.
3. **Colors and image (default)** - color rendering with one inline image;
   additional images are presented as links.
4. **Color and 5 images (experimental)** - color rendering with up to five
   inline images. This mode uses more memory and is intentionally experimental.

Mini Browser now uses the BadgeVMS fullscreen window path through
`SDL_WINDOW_FULLSCREEN`. This removes the normal BadgeVMS window decorations
and lets the browser use the display area directly without modifying the
BadgeVMS compositor or `BORDER_TOP_PX`.

Known limitation: in Mode 4, complete full-page screenshots are currently
limited to five images. This is documented as a 3.0 resource limitation.

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
`WHY+G`, `WHY+K`, `WHY+O`, `WHY+S`, `WHY+Z` and `WHY+Q` can therefore be
generated remotely.

The custom firmware repository also contains automated Mini Browser test
scripts. These can drive browser navigation, open configured websites,
perform searches, exercise browser commands and automatically request
and save screenshots after test steps. This is useful for repeatable
regression testing without manually operating the badge for every page.

The regression suite `mini-browser-3.0-regression-selectable.py` also
covers the 4.1 omnibox and history (tests 57-63), tabs (tests 64-67), the
4.3 Back/Forward cache, stop key, error pages and gzip (tests 68-72), the disk
cache, downloads and cookies (tests 73-77), background images (test 78) and
the 4.4 find, focus ring, zoom, simplified view, saved pages, share, Page
Information, `WHY+K` bookmarks and help (tests 79-87). Each loaded page stays on
screen for three seconds so the run can be followed on the badge; `--view N`
changes the pause and `--fast` removes it:

``` sh
./mini-browser-3.0-regression-selectable.py --list
./mini-browser-3.0-regression-selectable.py --test 57-63
./mini-browser-3.0-regression-selectable.py --test 68-78
./mini-browser-3.0-regression-selectable.py --test 79-87
./mini-browser-3.0-regression-selectable.py --fast
```

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

## Page Information and HTTP/TLS Inspector

Press `WHY+I` to open Page Information for the current page. Since 4.4 it
starts with a summary:

-   **Security**: whether the connection is HTTPS with a certificate checked
    against the built-in certificate bundle, or plain HTTP (don't type
    passwords there). HTTPS pages also show a green lock in the bar.
-   **Loading**: load time, page size, bytes transferred when the page came
    compressed, and the HTML size with its numbers of images and links.
-   **Cookies set by** the site, with how long each is kept (names only, never
    values).

Below the summary follows the HTTP/TLS Inspector. It shows
bounded metadata retained from the actual request, without making an extra
HEAD request. Information includes page title and size, link/form/action
counts, request method and URL, transport, cookies sent, HTTP status and
Content-Type, plus the current cookie-jar usage and browser limits.

The trimmed BadgeVMS libcurl exposes only a small set of `CURLINFO` fields.
Connection address/port, negotiated HTTP version, certificate details and
certificate-verification result are therefore reported as unavailable rather
than guessed.

`WHY+B` or `WHY+I` returns to the page.

## Cookies

Mini Browser keeps up to 32 cookies (values up to 255 bytes). It supports
`Domain`, `Path`, `Secure`, replacement, and deletion with `Max-Age=0` or an
`Expires` date in the past, and sends matching cookies on later requests to
the same site. Since 4.3 cookies with `Expires` or `Max-Age` are saved in
`APPS:[mini_browser]cookies.txt` and come back after a restart; session
cookies stay in memory only. The badge has no network clock: expiry times
are counted from the `Date` header of the first web page that sends one.
`WHY+X` on the Page Information page clears all cookies.

## Bookmarks and history

Mini Browser stores up to 32 bookmarks and keeps up to 32 entries for
Back and Forward.

`WHY+B` moves backward and `WHY+G` moves forward. Since 4.3 the last three
pages you left are kept in a Back/Forward cache, with their scroll position
and (up to 3 MB) their images: going Back or Forward to one of them, or
leaving the bookmarks, history or Page Information page, shows it at once
without loading it again. `WHY+R` always reloads from the network, and POST
results are not cached. Navigating to a new
page after going Back truncates the old forward branch. Reloading does
not create a duplicate Back/Forward entry, and GET and POST form
submissions participate in the same Back/Forward list.

Since 4.1 Mini Browser also keeps a persistent history of visited pages:
the last 150 pages, newest first, each address once with its title. POST
results are not recorded. Press `WHY+Y` to open the history page, type a
number and press Enter to open a page, and press `WHY+Y` again to return.
`WHY+X` on the history page clears the history.

The history is saved every five new pages and when Mini Browser quits, so
the last few pages can be missing after the badge is switched off without
quitting the browser.

Bookmarks (`WHY+K` adds or removes the current page, `WHY+M` lists them)
and history are stored at:

    APPS:[mini_browser]bookmarks.txt
    APPS:[mini_browser]history.txt

Other files Mini Browser keeps:

    APPS:[mini_browser]cookies.txt     cookies kept after a restart (4.3)
    APPS:[mini_browser]downloads.txt   list of downloads (4.3)
    APPS:[mini_browser]saved.txt       list of saved pages (4.4)
    FLASH0:[MBCACHE]                   disk cache, 2 MB (4.3)
    FLASH0:[DOWNLOADS]                 downloaded files (4.3)
    FLASH0:[SAVED]                     saved pages as text files (4.4)

## Networking

Mini Browser uses libcurl and requests HTTP/1.1 where available.
Redirect following is bounded. Network failures and HTTP errors are
shown as readable error pages.

The BadgeVMS 4.3 firmware (https://github.com/mactjaap/firmware) extends its
curl with what a browser needs: progress reporting and stopping a transfer,
gzip/deflate decoding (using the inflater in the ESP32-P4 ROM), keeping the
connection open between requests to the same site, and distinct errors for
name resolution, connection, TLS, certificate and timeout failures. With that
firmware Mini Browser asks for compressed pages with:

    Accept-Encoding: gzip, deflate

and reuses one connection for a page and its images, which saves a TLS
handshake per request. On older firmware Mini Browser requests uncompressed
data (`Accept-Encoding: identity`) and works as before, without the loading
progress and stop key.

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
  Back/Forward entries                 32
  Persistent history            150 pages
  Omnibox suggestions                   5
  Tabs                                  5
  Back/Forward cache              3 pages
  Cached images                     3 MiB
  Cookies                              32
  POST request body            2048 bytes
  Disk cache                        2 MiB
  Download size            4 MiB per file
  Downloads listed                     50
  Saved pages listed                   50
  Find matches                        400
  Text zoom                 100/150/200 %

## What Mini Browser does not support

Mini Browser does not currently provide JavaScript execution, a CSS layout
engine, video/audio, file uploads, complex HTML form controls, a complete HTML5
DOM/parser, color emoji, or complex emoji composition. Version 3.0 provides
bounded PNG, JPEG and GIF image rendering rather than a general-purpose modern
browser graphics engine.

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

**Mini Browser 4.4**

Version 4.4 adds find in page (`WHY+F`), a focus ring for links and fields
(`Tab` / `Shift+Tab`), text zoom (`WHY+=`, `WHY+-`, `WHY+0`), a simplified view
for long articles (`WHY+V`), pages saved for offline reading (`WHY+P`),
sharing as a QR code (`WHY+U`), a Page Information summary with a lock for
HTTPS pages (`WHY+I`) and a help page (`WHY+/`). Bookmarking moved from
`WHY+F` to `WHY+K`. On start the browser prints its version and build time on
the serial port, for example
`[mini_browser] enter main (version 4.4-dev3, built Oct  9 2026 07:40:12)`.

Version 4.3 added a loading line with progress, the `Esc` stop key, clear
error pages with `R` to reload, a Back/Forward cache of three pages, gzip pages
and connection reuse, cookies kept after a restart, a 2 MB disk cache,
downloads (`WHY+D`) and images that load in the background (several need the
BadgeVMS 4.3 firmware, see Networking).

Version 4.1 added the omnibox (`WHY+L`) with Google search and suggestions, a
persistent history of visited pages (`WHY+Y`, `WHY+X`) and tabs (`WHY+T`,
`WHY+W`, `WHY+Tab`, `WHY+1-5`, `WHY+A`).

Version 3.0 adds PNG, JPEG and GIF image rendering, inline images, image sizing
and scaling, multiple display modes and fullscreen BadgeVMS operation while
preserving the Unicode, navigation, forms, cookies, bookmarks, history,
HTTP/TLS Inspector and screenshot functionality developed in the 2.x releases.
