# Mini Browser 2.1

A compact, interactive, text-oriented web browser for the WHY2025 badge.

Mini Browser is written in C using SDL3 and libcurl. It retrieves HTML pages, converts them into readable text, extracts links and simple HTML forms, and provides a keyboard-driven browsing interface designed for the 720×720 WHY2025 badge display.

Version **2.1** is a stable release. It supports normal link navigation, persistent bookmarks, Back/Forward history, URL editing, hold-to-scroll, improved HTML rendering, and simple interactive GET forms.

Mini Browser deliberately does **not** try to be a modern graphical browser. There is no JavaScript engine, CSS layout engine, image renderer, or full DOM. The goal is a small, fast browser for text-oriented and lightweight websites.

## Highlights

- Text-oriented HTML browsing over HTTP and HTTPS
- Up to 64 KiB downloaded per page
- Up to 128 extracted links
- Up to 160 numbered interactive actions
- Numbered navigation for links and form controls
- Tab / Shift+Tab action selection
- Back and Forward browsing history
- Persistent bookmarks
- Editable URL bar
- Automatic `https://` for typed URLs without a scheme
- HTTP status and connection error pages
- Hold Up/Down for fast scrolling
- Basic GET form support
- Improved rendering of common HTML structure
- Small built-in 5×7 bitmap font
- No JavaScript, CSS layout or image rendering required

## HTML forms

Mini Browser 2.1 supports simple interactive HTML GET forms.

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

You can also use Tab and Shift+Tab to move through actions and press Enter to activate the selected action.

### Keyboard controls

| Key | Action |
| --- | --- |
| `0`–`9` + Enter | Activate a numbered link or form action |
| `Tab` | Select next action |
| `Shift+Tab` | Select previous action |
| `Enter` | Activate selected action / accept edit |
| `Up` / `Down` | Scroll one line; hold for continuous scrolling |
| `J` / `K` | Scroll down / up one line |
| `Page Down` | Scroll approximately one page |
| `Page Up` | Scroll upward |
| `Home` | Top of page, or start of current editor |
| `Left` / `Right` | Move cursor while editing |
| `Backspace` / `Delete` | Edit URL or form value |
| `End` | Move to end of URL or form value |
| `Escape` | Cancel form/action-number editing; otherwise exit |

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

## History

Mini Browser maintains a browser-style history of up to 32 HTTP/HTTPS entries.

`WHY+B` moves backward and `WHY+G` moves forward. Navigating to a new page after going Back truncates the old forward branch, like a conventional browser.

Reloading a page does not create a duplicate history entry. GET form submissions are ordinary navigations and participate in the same Back/Forward history.

## Networking

Mini Browser uses libcurl and requests HTTP/1.1 where available.

The browser sends a Mini Browser 2.1 user agent and requests uncompressed transfer data with:

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
- a full Unicode font

Sites designed around server-rendered HTML and ordinary links work best.

## Good sites to try

Text-oriented sites are ideal for Mini Browser. Examples include:

- Wiby — `https://wiby.me/`
- Hacker News — `https://news.ycombinator.com/`
- NPR Text — `https://text.npr.org/`
- TEXTFILES.COM — `http://www.textfiles.com/`
- curl — `https://curl.se/`
- ifconfig.co — `https://ifconfig.co/`

Wiby is particularly useful for testing the simple GET form support in version 2.1.

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
    wrapped text
      |
      v
    SDL3 720x720 renderer

Links and form controls are represented as actions. This keeps keyboard navigation consistent: the user activates a number, and the corresponding action either navigates, edits a field, or submits a form.

Form editing uses its own buffer and cursor state and is intentionally separate from URL editing.

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

## Version

**Mini Browser 2.1**

A stable, practical text browser for the WHY2025 badge, now with simple interactive GET forms.
