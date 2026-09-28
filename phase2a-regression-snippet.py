# CONFIG entry:
# "phase2a": {
#     "url": "minibrowser.macip.net/phase2a-styles.html",
#     "url_pattern": r"HTTP 200.*https://minibrowser\.macip\.net/phase2a-styles\.html",
# },

def phase2a_inline_text_styles(badge):
    open_direct_url(badge, CONFIG["phase2a"]["url"], CONFIG["phase2a"]["url_pattern"])
    badge.wait_for(r"\[mini_browser\] visual: explicit_styles=15", 13)
    content = latest_content_block(badge)
    if not content:
        raise RuntimeError("Phase 2A style page loaded but no CONTENT block was captured")
    require_content(content, "PHASE2A STYLES", "bold", "weight 700", "italic",
                    "underline", "left aligned", "center aligned", "right aligned",
                    "yellow bold", "parent bold", "bold italic", "restored bold",
                    "normal", "bold again", "not underlined", "underlined again",
                    "invalid ignored", "END PHASE2A STYLES")
    return ["15 valid inline style declarations parsed",
            "Nested style content preserved",
            "Invalid style values ignored"]
