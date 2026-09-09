# Mini Browser 2.3

A compact, interactive, text-oriented web browser for the WHY2025 badge.

Mini Browser is written in C using SDL3 and libcurl. It retrieves HTML pages, converts them into readable text, extracts links and simple HTML forms, and provides a keyboard-driven browsing interface designed for the 720×720 WHY2025 badge display.

Version **2.3** is a stable release. It supports normal link navigation, persistent bookmarks, Back/Forward history, URL editing, hold-to-scroll, simple interactive GET forms, broad Unicode rendering, CJK text, Unicode symbols and monochrome emoji.

Mini Browser deliberately does **not** try to be a modern graphical browser. There is no JavaScript engine, CSS layout engine, image renderer, or full DOM. The goal is a small, fast browser for text-oriented and lightweight websites.

## Highlights

- Text-oriented HTML browsing over HTTP and HTTPS
- Broad UTF-8 and Unicode support
- Extended Latin, Greek and Cyrillic text
- Japanese Hiragana and Katakana
- Chinese CJK characters
- Unicode punctuation and symbols
- Unicode Plane-1 characters
- Monochrome Unicode emoji
- 27,696-glyph generated Unicode bitmap font
- UTF-8-safe text processing and wrapping
- Pixel-aware wrapping for mixed ASCII and Unicode text
- Up to 64 KiB downloaded per page
- Up to 128 extracted links
- Up to 160 numbered interactive actions
- Numbered navigation for links and form controls
- Back and Forward browsing history
- Persistent bookmarks
- Editable URL bar
- Automatic `https://` for typed URLs without a scheme
- HTTP status and connection error pages
- Hold Up/Down for fast scrolling
- Basic GET form support
- Improved rendering of common HTML structure
- Compact built-in ASCII bitmap font
- 16×16 Unicode bitmap glyph renderer
- No JavaScript, CSS layout or image rendering required

## Unicode

Mini Browser 2.3 introduces a substantially expanded Unicode rendering system.

The browser uses its compact built-in font for normal ASCII text and a generated 16×16 bitmap font for Unicode characters.

The generated Unicode font contains **27,696 glyphs** covering selected ranges from Unicode Plane 0 and Plane 1.

Coverage includes:

- Latin-1 Supplement
- Latin Extended-A and Extended-B
- IPA Extensions
- Combining Diacritical Marks
- Greek and Coptic
- Greek Extended
- Cyrillic
- Cyrillic Supplement
- Armenian
- Georgian
- General Punctuation
- Superscripts and Subscripts
- Currency Symbols
- Letterlike Symbols
- Number Forms
- Arrows
- Mathematical Operators
- Miscellaneous Technical
- Box Drawing
- Block Elements
- Geometric Shapes
- Miscellaneous Symbols
- Dingbats
- CJK Symbols and Punctuation
- Hiragana
- Katakana
- CJK Unified Ideographs
- Halfwidth and Fullwidth Forms
- selected Plane-1 symbols and pictographs
- Unicode emoji

The Unicode font is generated from GNU Unifont 17.0.02 bitmap data, including the Japanese and upper-plane font sources.

### UTF-8-safe wrapping

Text wrapping is UTF-8 aware.

Mini Browser processes complete Unicode code points when wrapping text rather than treating the individual bytes of a UTF-8 sequence as separate characters.

This prevents wrapping from inserting a line break in the middle of a multi-byte UTF-8 character.

### Pixel-aware wrapping

Wrapping is also based on rendered pixel width rather than simply counting characters.

ASCII characters use the compact browser font while Unicode characters use wider 16×16 glyphs. Mini Browser accounts for this difference when deciding where a line should wrap.

This allows pages containing combinations such as English, Japanese, Chinese, Greek, Cyrillic, symbols and emoji to remain correctly aligned on the 720×720 display.

## Emoji

Mini Browser 2.3 supports many real Unicode Plane-1 emoji code points.

Examples include:

    😀 😃 😄 😁 😂 🙂 😉 😊 😎 🤔 😴 😢 😭 😡 🤖

    👍 👎 👋 👏 🙌 🙏 💪

    🐶 🐱 🐭 🐰 🦊 🐻 🐼 🐸

    🍎 🍊 🍋 🍌 🍉 🍇 🍓 🍕 🍔 🍟

    🚗 🚕 🚌 🚲 🚀 🚁 🚂 🚢

    🌍 🌎 🌏 🌞 🌙 🌈 🔥

Emoji are rendered as **monochrome 16×16 bitmap glyphs**.

Mini Browser does not currently provide a colour emoji renderer.

Complex multi-codepoint emoji sequences are also outside the current renderer. This includes things such as:

- Zero Width Joiner (ZWJ) compositions
- skin-tone compositions
- gender compositions
- composed flag sequences
- other emoji sequences requiring shaping

Single Unicode code-point emoji within the included font ranges are the primary target.

## HTML forms

Mini Browser supports simple interactive HTML GET forms.

Supported form controls:

- `<input type="text">`
- `<input type="search">`
- `<input type="url">`
- `<input type="hidden">`
- `<input type="submit">`
- `<button>`
- `<button type="submit">`

Links and visible form controls share the same numbered action system. A page can therefore look like:

    Search the web

    [1] q:
    [2] [Search]

    [3] About

Type `1` and press Enter to edit the field. Type the search text and press Enter again to store the value without leaving the page. Then type `2` and press Enter to submit the form.

Form editing is separate from URL editing, so entering text into a form does not change the current page URL.

GET submissions use URL encoding compatible with `application/x-www-form-urlencoded`:

- spaces become `+`
- unsafe characters are percent encoded
- field names and values are encoded
- hidden fields are included
- the activated named submit button is included
- disabled and unnamed fields are omitted where appropriate
- existing query strings in the form action are preserved

Up to 4 forms with up to 8 stored fields per form are supported.

POST forms are recognised but intentionally not submitted. The browser displays:

    POST FORMS NOT SUPPORTED

Complex controls such as `textarea`, `select`, checkboxes, radio buttons, file uploads and JavaScript-driven forms are not currently supported.

## HTML rendering

Mini Browser converts useful HTML structure into a compact text representation.

Supported or specially handled elements include:

- headings (`h1` through `h6`)
- paragraphs and common block elements
- line breaks
- unordered and ordered lists
- preformatted text
- inline code
- bold/strong text
- emphasis/italic text
- horizontal rules
- simple table rows and cells
- hyperlinks
- simple forms
- common named HTML entities
- decimal and hexadecimal numeric entities

Comments, doctypes, scripts, styles and document head content are ignored for normal page rendering. The page `<title>` is extracted and displayed in the top bar.

Mini Browser is intentionally a tolerant text extractor rather than a standards-complete HTML parser.

## Navigation

Every usable link or visible form control receives an action number.

For a link:

    [7] Example page

type:

    7

and press Enter.

Actions can also be selected using the browser's keyboard navigation and activated with Enter.

### Keyboard controls

The controls below describe the keyboard operations used directly by Mini Browser:

| Key | Action |
| --- | --- |
| `0`–`9` + Enter | Activate a numbered link or form action |
| `Enter` | Activate selected action / accept edit |
| `Up` / `Down` | Scroll one line; hold for continuous scrolling |
| `J` / `K` | Scroll down / up one line |
| `Left` / `Right` | Move the cursor while editing |
| `Backspace` | Delete the character before the cursor |

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

## Bookmarks

Mini Browser stores up to 32 bookmarks.

Press `WHY+F` to add or remove the current page. Press `WHY+M` to open the bookmark page.

Bookmarks are stored persistently in BadgeVMS storage at:

    APPS:[mini_browser]bookmarks.txt

The bookmark page itself is generated locally and requires no network connection.

Bookmarks survive quitting Mini Browser, restarting the application, rebooting the badge, and completely powering the badge off and on.

A complete BadgeVMS firmware flash replaces the application storage image and therefore removes saved bookmarks.

## History

Mini Browser maintains a browser-style history of up to 32 HTTP/HTTPS entries.

`WHY+B` moves backward and `WHY+G` moves forward. Navigating to a new page after going Back truncates the old forward branch, like a conventional browser.

Reloading a page does not create a duplicate history entry. GET form submissions are ordinary navigations and participate in the same Back/Forward history.

## Networking

Mini Browser uses libcurl and requests HTTP/1.1 where available.

The browser sends a Mini Browser 2.3 user agent and requests uncompressed transfer data with:

    Accept-Encoding: identity

Redirect following is enabled where supported, with a maximum of five redirects.

Network failures and HTTP errors are shown as readable browser pages rather than silently failing.

## Limits

Mini Browser is intentionally bounded for an embedded system.

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

These limits are design choices, not bugs. They keep memory use predictable on the badge.

## What Mini Browser does not support

Mini Browser is not intended to replace Firefox, Chrome or Safari.

It does not currently provide:

- JavaScript execution
- CSS layout or styling
- images
- video or audio
- cookies/session-oriented web applications
- POST form submission
- file uploads
- authentication workflows requiring modern browser APIs
- complex HTML form controls
- a complete HTML5 parser
- colour emoji
- complex emoji shaping and ZWJ composition

Sites designed around server-rendered HTML and ordinary links work best.

## Good sites to try

Text-oriented sites are ideal for Mini Browser. Examples include:

- Wiby — `https://wiby.me/`
- Hacker News — `https://news.ycombinator.com/`
- NPR Text — `https://text.npr.org/`
- TEXTFILES.COM — `http://www.textfiles.com/`
- curl — `https://curl.se/`
- ifconfig.co — `https://ifconfig.co/`

Wiby is particularly useful for testing the simple GET form support.

## Architecture

The browser is deliberately small.

The main pipeline is:

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
      +--> page title
      |
      v
    numbered action model
      |
      v
    UTF-8-safe pixel-aware wrapping
      |
      v
    ASCII / Unicode glyph selection
      |
      +--> compact ASCII bitmap font
      |
      +--> 16x16 Unicode bitmap font
      |
      v
    SDL3 720x720 renderer

Links and form controls are represented as actions. This keeps keyboard navigation consistent: the user activates a number, and the corresponding action either navigates, edits a field, or submits a form.

Form editing uses its own buffer and cursor state and is intentionally separate from URL editing.

The Unicode renderer decodes UTF-8 into Unicode code points and looks up supported characters in a direct-indexed bitmap font. The same Unicode-aware logic is used by the wrapping system so that multi-byte characters are not split and wider glyphs are correctly accounted for.

## Unicode font

The Unicode font is generated as a Mini Browser-specific binary asset:

    unifont_cjk.bin

The current version contains **27,696 glyphs** and occupies approximately **887 KB**.

It is built from GNU Unifont 17.0.02 source data, including:

    unifont_jp-17.0.02.hex
    unifont_upper-17.0.02.hex

The font generator selects the Unicode ranges required by Mini Browser and converts the source bitmaps into the browser's direct-indexed 16×16 glyph format.

At runtime BadgeVMS provides the font at:

    APPS:[mini_browser]unifont_cjk.bin

This approach avoids requiring a general-purpose font rendering engine on the badge.

## Building

Mini Browser is part of the WHY2025 BadgeVMS firmware tree.

With the existing WHY2025 ESP-IDF environment configured:

    cd /root/firmware
    . ~/esp-idf/export.sh
    idf.py build

Flash the ESP32-P4 through the badge's P4 flashing port using the normal BadgeVMS firmware procedure.

Important: use the existing project configuration for the WHY2025 badge. Do not casually regenerate the ESP32-P4 target configuration on early P4 hardware.

## Project

Source repository:

https://github.com/mactjaap/mini_browser/

Home page:

https://minibrowser.macip.net/

## Version history

### Mini Browser 2.3

Version 2.3 introduces broad Unicode and emoji rendering:

- UTF-8-safe text processing
- pixel-aware text wrapping
- extended Latin support
- Greek support
- Cyrillic support
- Japanese Hiragana and Katakana
- Chinese CJK characters
- broad Unicode punctuation and symbols
- Unicode Plane-1 support
- monochrome emoji
- 27,696-glyph generated Unicode bitmap font
- GNU Unifont Japanese and upper-plane glyph sources

### Mini Browser 2.1

Version 2.1 introduced interactive HTML GET forms and the completed browser-style Back/Forward history implementation:

- simple GET forms
- editable text, search and URL fields
- hidden fields
- submit buttons
- URL-encoded form submission
- single Back/Forward history model
- forward-branch truncation
- reload without duplicate history entries

## Credits

Mini Browser is built using:

- WHY2025 BadgeVMS
- SDL3
- libcurl
- GNU Unifont 17.0.02 for Unicode bitmap glyph data

The compact ASCII font is built into Mini Browser.

The Unicode font used by Mini Browser is generated from GNU Unifont bitmap data. When redistributing the generated font asset, the applicable GNU Unifont copyright, attribution and licensing information should be included with the distribution.

## Version

**Mini Browser 2.3**

A stable, practical text browser for the WHY2025 badge with interactive GET forms, browser-style navigation, persistent bookmarks, broad multilingual Unicode rendering, symbols and monochrome emoji.
