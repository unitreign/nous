#!/usr/bin/env python3
"""Generate a comprehensive regression-test EPUB for microreader2.

Each chapter exercises a specific feature or edge case so regressions
are immediately visible in the HTML export.

Run:  python generate_regression_epub.py
"""

import io
import struct
import zipfile
import zlib
from pathlib import Path

OUT = Path(__file__).parent
MIMETYPE = "application/epub+zip"

CONTAINER_XML = """\
<?xml version="1.0" encoding="UTF-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles>
    <rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>"""


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def make_xhtml(title, body_html, css_link=None, inline_css=None):
    head_extra = ""
    if css_link:
        head_extra += f'\n<link href="{css_link}" rel="stylesheet" type="text/css"/>'
    if inline_css:
        head_extra += f"\n<style>{inline_css}</style>"
    return f"""\
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml">
<head><title>{title}</title>{head_extra}</head>
<body>
{body_html}
</body>
</html>"""


def make_opf(manifest_items, spine_idrefs, toc_id=None):
    manifest = "\n    ".join(
        f'<item id="{mid}" href="{href}" media-type="{mt}"/>'
        for mid, href, mt in manifest_items
    )
    spine_items = "\n    ".join(f'<itemref idref="{idref}"/>' for idref in spine_idrefs)
    toc_attr = f' toc="{toc_id}"' if toc_id else ""
    return f"""\
<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="2.0" unique-identifier="uid">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:title>Regression Test Suite</dc:title>
    <dc:creator>microreader2 tests</dc:creator>
    <dc:language>en</dc:language>
    <dc:identifier id="uid">test-regression-suite</dc:identifier>
  </metadata>
  <manifest>
    {manifest}
  </manifest>
  <spine{toc_attr}>
    {spine_items}
  </spine>
</package>"""


def make_ncx(nav_points):
    points = ""
    for i, (label, src) in enumerate(nav_points, 1):
        points += f"""
    <navPoint id="np{i}" playOrder="{i}">
      <navLabel><text>{label}</text></navLabel>
      <content src="{src}"/>
    </navPoint>"""
    return f"""\
<?xml version="1.0" encoding="UTF-8"?>
<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1">
  <head/>
  <docTitle><text>Regression Test Suite</text></docTitle>
  <navMap>{points}
  </navMap>
</ncx>"""


def make_jpeg(width, height, gray=200):
    """Generate a minimal grayscale JPEG using PIL."""
    from PIL import Image

    img = Image.new("L", (width, height), gray)
    buf = io.BytesIO()
    img.save(buf, format="JPEG", quality=85)
    return buf.getvalue()


def make_png(width, height, r=100, g=100, b=100):
    """Generate a minimal RGB PNG without external deps."""

    def chunk(ctype, data):
        c = ctype + data
        crc = struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)
        return struct.pack(">I", len(data)) + c + crc

    sig = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    # Build raw scanlines
    raw = b""
    for _ in range(height):
        raw += b"\x00" + bytes([r, g, b]) * width
    idat = zlib.compress(raw)
    return sig + chunk(b"IHDR", ihdr) + chunk(b"IDAT", idat) + chunk(b"IEND", b"")


def write_epub(name, files, opf_path="OEBPS/content.opf"):
    path = OUT / name
    with zipfile.ZipFile(path, "w") as zf:
        info = zipfile.ZipInfo("mimetype")
        info.compress_type = zipfile.ZIP_STORED
        zf.writestr(info, MIMETYPE)
        for arcname, data, compress in files:
            info = zipfile.ZipInfo(arcname)
            info.compress_type = (
                zipfile.ZIP_DEFLATED if compress else zipfile.ZIP_STORED
            )
            zf.writestr(info, data if isinstance(data, bytes) else data.encode("utf-8"))
    print(f"  wrote {path}  ({path.stat().st_size} bytes)")


# ---------------------------------------------------------------------------
# Chapter content generators
# ---------------------------------------------------------------------------

STYLESHEET = """\
/* --- Font sizes --- */
.fs-small     { font-size: small; }
.fs-xsmall    { font-size: x-small; }
.fs-large     { font-size: large; }
.fs-xlarge    { font-size: x-large; }
.fs-80pct     { font-size: 80%; }
.fs-120pct    { font-size: 120%; }
.fs-0_75em    { font-size: 0.75em; }
.fs-1_3em     { font-size: 1.3em; }
.fs-medium    { font-size: medium; }

/* --- Footnote container (ohler-style nesting bug) --- */
.fnotecontent { font-size: 0.83333em; }
.thin-space   { font-size: 0.75em; }

/* --- Margins --- */
.ml-2em       { margin-left: 2em; }
.mr-2em       { margin-right: 2em; }
.ml-10pct     { margin-left: 10%; }
.mr-10pct     { margin-right: 10%; }
.ml-30pct     { margin-left: 30%; }
.mr-30pct     { margin-right: 30%; }

/* --- Indent --- */
.indent-2em   { text-indent: 2em; }
.indent-neg   { text-indent: -1.5em; margin-left: 1.5em; }

/* --- Alignment --- */
.align-center { text-align: center; }
.align-right  { text-align: right; }
.align-justify { text-align: justify; }

/* --- Float --- */
.floatleft    { float: left; }
.floatright   { float: right; }

/* --- Hidden --- */
.hidden       { display: none; }

/* --- Poem-style (nested margins) --- */
.poem         { margin-left: 30%; margin-right: 30%; }
.poem-inner   { margin-left: 2em; }

/* --- Bold/Italic via CSS --- */
.css-bold     { font-weight: bold; }
.css-italic   { font-style: italic; }
.css-normal   { font-weight: normal; font-style: normal; }

/* --- Small caps (UC class from ohler) --- */
.uc           { font-variant: small-caps; text-transform: uppercase; }

/* --- Margin shorthand --- */
.margin-1val  { margin: 24px; }
.margin-2val  { margin: 10px 36px; }
.margin-4val  { margin: 5px 48px 5px 24px; }

/* --- Text-transform --- */
.tt-upper     { text-transform: uppercase; }
.tt-lower     { text-transform: lowercase; }
.tt-cap       { text-transform: capitalize; }

/* --- Vertical margins --- */
.mt-large     { margin-top: 24px; }
.mb-large     { margin-bottom: 24px; }
.mt-zero      { margin-top: 0; }
.mb-zero      { margin-bottom: 0; }
.vert-margin  { margin: 20px 0; }

/* --- Line height --- */
.lh-tight     { line-height: 100%; }
.lh-normal    { line-height: normal; }
.lh-loose     { line-height: 180%; }
.lh-1_8       { line-height: 1.8; }

/* --- HR width via class (ch11) --- */
.hr-full      { width: 100%; }
.hr-half      { width: 50%; }
.hr-quarter   { width: 25%; }

"""


def ch_basic_text():
    """Chapter 1: Basic text and paragraphs."""
    return make_xhtml(
        "Basic Text",
        """\
<h1>1. Basic Text</h1>
<p>This is a normal paragraph with enough text to wrap across multiple lines on a 480-pixel-wide display. It tests basic word wrapping and line layout.</p>
<p>Second paragraph. Shorter.</p>
<p>Third paragraph with <b>bold text</b>, <i>italic text</i>, and <b><i>bold-italic text</i></b> inline.</p>
<p>A paragraph with a very-long-hyphenated-compound-word-that-should-break-somehow in the middle of it.</p>
""",
        css_link="style.css",
    )


def ch_headings():
    """Chapter 2: All heading levels."""
    return make_xhtml(
        "Headings",
        """\
<h1>2. Headings</h1>
<h1>Heading Level 1</h1>
<p>Text after h1.</p>
<h2>Heading Level 2</h2>
<p>Text after h2.</p>
<h3>Heading Level 3</h3>
<p>Text after h3.</p>
<h4>Heading Level 4</h4>
<p>Text after h4.</p>
<h5>Heading Level 5</h5>
<p>Text after h5.</p>
<h6>Heading Level 6</h6>
<p>Text after h6.</p>
""",
        css_link="style.css",
    )


def ch_font_sizes():
    """Chapter 3: Font size via CSS keywords, %, em."""
    return make_xhtml(
        "Font Sizes",
        """\
<h1>3. Font Sizes</h1>
<p>Normal size text.</p>
<p class="fs-small">Small (keyword: small).</p>
<p class="fs-xsmall">X-Small (keyword: x-small).</p>
<p class="fs-large">Large (keyword: large).</p>
<p class="fs-xlarge">X-Large (keyword: x-large).</p>
<p class="fs-80pct">80% size text.</p>
<p class="fs-120pct">120% size text.</p>
<p class="fs-0_75em">0.75em size text.</p>
<p class="fs-1_3em">1.3em size text.</p>
<p class="fs-medium">Medium (keyword: medium) — should be normal.</p>
<p>Inline: <span style="font-size: 0.7em">0.7em inline</span> then normal again.</p>
<p>Inline: <span style="font-size: larger">larger inline</span> then normal again.</p>
""",
        css_link="style.css",
    )


def ch_nested_font_size():
    """Chapter 4: Nested font-size — the ohler bug regression test."""
    return make_xhtml(
        "Nested Font Size",
        """\
<h1>4. Nested Font Size (ohler bug)</h1>
<p>Normal text before the footnote container.</p>
<div class="fnotecontent">
  <p>This entire block should be SMALL (0.83em). Words: alpha beta gamma delta epsilon.</p>
  <p>Inside the small block, a <span class="thin-space"> </span>thin-space<span class="thin-space"> </span> span. The text AFTER the thin-space spans should STILL be small, not revert to normal.</p>
  <p>Citation: Holzer, a.<span class="thin-space"> </span>a.<span class="thin-space"> </span>O., S.&nbsp;191. — all of this line must remain small.</p>
  <p>Multiple nesting: <span class="thin-space"> </span>x<span class="thin-space"> </span>y<span class="thin-space"> </span>z — still small throughout.</p>
</div>
<p>This paragraph is OUTSIDE the footnote container — should be normal size again.</p>
""",
        css_link="style.css",
    )


def ch_style_nesting():
    """Chapter 5: Deep style nesting (bold inside italic inside size)."""
    return make_xhtml(
        "Style Nesting",
        """\
<h1>5. Style Nesting</h1>
<p>Normal <b>bold <i>bold-italic <span style="font-size: large">bold-italic-large</span> bold-italic</i> bold</b> normal.</p>
<p><i>Italic start <b>bold-italic <span style="font-size: small">small-bold-italic</span> bold-italic</b> italic</i> normal.</p>
<p class="fs-small">Small paragraph with <b>bold</b> and <i>italic</i> and <b><i>bold-italic</i></b> — all should stay small.</p>
""",
        css_link="style.css",
    )


def ch_margins():
    """Chapter 6: Margins in px, em, %."""
    return make_xhtml(
        "Margins",
        """\
<h1>6. Margins</h1>
<p>No margins — full width.</p>
<p class="ml-2em">Left margin 2em. This paragraph should be indented from the left by about two character widths.</p>
<p class="mr-2em">Right margin 2em. This paragraph should have reduced right boundary.</p>
<p class="ml-10pct">Left margin 10%. A reasonable indent.</p>
<p class="mr-10pct">Right margin 10%.</p>
<p style="margin-left: 10%; margin-right: 10%;">Both margins 10% (from same rule — budget clamping to 15% applies).</p>
<p style="margin-left: 20%; margin-right: 20%;">Both margins 20% each = 40% total — should be clamped to 15% total proportionally.</p>
<p class="ml-30pct">Left margin 30%. Very deep indent.</p>
""",
        css_link="style.css",
    )


def ch_nested_margins():
    """Chapter 7: Nested / additive margins (poem style)."""
    return make_xhtml(
        "Nested Margins",
        """\
<h1>7. Nested Margins (poem style)</h1>
<p>Normal paragraph above the poem.</p>
<div class="poem">
  <p>Poem line — 30% margins on both sides.</p>
  <p class="poem-inner">Nested poem line — 30% + 2em on the left.</p>
  <p>Back to poem base margin.</p>
</div>
<p>Normal paragraph after the poem — margins should be fully restored.</p>
""",
        css_link="style.css",
    )


def ch_alignment():
    """Chapter 8: Text alignment."""
    return make_xhtml(
        "Alignment",
        """\
<h1>8. Alignment</h1>
<p>Default alignment (LTR).</p>
<p class="align-center">Centered text. This line should be centered on the page.</p>
<p class="align-right">Right-aligned text. This should be flush right.</p>
<p class="align-justify">Justified text. This paragraph has enough words to demonstrate justified alignment where spaces are expanded to fill the entire line width evenly across the available space.</p>
<p style="text-align: center;">Inline style center.</p>
""",
        css_link="style.css",
    )


def ch_indent():
    """Chapter 9: Text indentation."""
    return make_xhtml(
        "Indentation",
        """\
<h1>9. Indentation</h1>
<p>No indent.</p>
<p class="indent-2em">Indented by 2em. Only the first line should be indented.</p>
<p class="indent-neg">Negative indent with compensating margin (hanging indent). The first line should stick out to the left relative to the rest of the paragraph, which is useful for bibliographies and definitions.</p>
<p style="text-indent: 3em;">Inline indent 3em.</p>
""",
        css_link="style.css",
    )


def ch_lists():
    """Chapter 10: Ordered and unordered lists."""
    return make_xhtml(
        "Lists",
        """\
<h1>10. Lists</h1>
<p>Unordered list (direct text in li):</p>
<ul>
  <li>First item</li>
  <li>Second item with <b>bold</b> text</li>
  <li>Third item</li>
</ul>
<p>Ordered list (direct text in li):</p>
<ol>
  <li>First numbered item</li>
  <li>Second numbered item</li>
  <li>Third numbered item</li>
</ol>
<p>Unordered list with inner &lt;p&gt; (bullet must be inline with text, not on separate line):</p>
<ul>
  <li>
    <p>First item wrapped in p.</p>
  </li>
  <li>
    <p>Second item wrapped in p.</p>
  </li>
  <li>
    <p>Third item wrapped in p.</p>
  </li>
</ul>
<p>Ordered list with inner &lt;p&gt;:</p>
<ol>
  <li>
    <p>First numbered item wrapped in p.</p>
  </li>
  <li>
    <p>Second numbered item wrapped in p.</p>
  </li>
</ol>
<p>Unordered list with list-style-type none (no bullets should appear):</p>
<ul style="list-style-type: none;">
  <li>No bullet here</li>
  <li>No bullet here either</li>
</ul>
<p>Ordered list with list-style-type none (no numbers should appear):</p>
<ol style="list-style-type: none;">
  <li>No number here</li>
  <li>No number here either</li>
</ol>
<p>Text after lists.</p>
""",
        css_link="style.css",
    )


def ch_hr():
    """Chapter 11: Horizontal rules — basic rendering and CSS width percentage."""
    return make_xhtml(
        "Horizontal Rules",
        """\
<h1>11. Horizontal Rules</h1>

<h2>Basic HRs</h2>
<p>Text before first HR.</p>
<hr/>
<p>Text between HRs.</p>
<hr/>
<p>Text after second HR. Default HRs should be 1/3 content width, centered.</p>

<h2>Border-top div as HR</h2>
<p>Div with border-top acts as HR (as in animal.epub heading decorations):</p>
<div style="border-top-style: solid; border-top-width: 1px; margin-top: 0.8em; margin-bottom: 0.8em;"> </div>
<h2>Heading between border-top divs</h2>
<div style="border-top-style: solid; border-top-width: 1px; margin-top: 0.8em; margin-bottom: 1em;"> </div>
<p>Text after border-top div HR.</p>

<h2>CSS width percentage (inline style)</h2>
<p>width: 100% — full content width:</p>
<hr style="width: 100%;"/>
<p>width: 50% — half width, centered:</p>
<hr style="width: 50%;"/>
<p>width: 25% — quarter width, centered:</p>
<hr style="width: 25%;"/>
<p>width: 10% — narrow, centered:</p>
<hr style="width: 10%;"/>

<h2>Border-top div with width: 50%</h2>
<p>The decorative rule below should be 50% wide, centered:</p>
<div style="border-top-style: solid; border-top-width: 1px; width: 50%; margin-top: 0.8em; margin-bottom: 0.8em;"> </div>
<p>Text after width-constrained border-top div HR.</p>

<h2>Descending widths in sequence</h2>
<hr style="width: 100%;"/>
<hr style="width: 66%;"/>
<hr style="width: 33%;"/>
<hr style="width: 10%;"/>
<p>Four HRs above: 100%, 66%, 33%, 10% — each visually narrower, all centered.</p>

<h2>CSS class width (external stylesheet)</h2>
<p>class="hr-full" (100%):</p>
<hr class="hr-full"/>
<p>class="hr-half" (50%):</p>
<hr class="hr-half"/>
<p>class="hr-quarter" (25%):</p>
<hr class="hr-quarter"/>
<p>These three HRs use classes from style.css — widths must match the inline-style HRs above.</p>
""",
        css_link="style.css",
    )


def ch_small_sub_sup():
    """Chapter 12: <small>, <sub>, <sup> elements."""
    return make_xhtml(
        "Small / Sub / Sup",
        """\
<h1>12. small, sub, sup</h1>
<p>Normal text with <small>small text</small> inline.</p>
<p>H<sub>2</sub>O is water. E = mc<sup>2</sup> is famous.</p>
<p>Nested: <small>small containing <sub>subscript</sub> text</small> then normal.</p>
<p>Multiple: x<sup>2</sup> + y<sup>2</sup> = z<sup>2</sup>.</p>
""",
        css_link="style.css",
    )


def ch_br():
    """Chapter 13: Line breaks."""
    return make_xhtml(
        "Line Breaks",
        """\
<h1>13. Line Breaks</h1>
<p>First line.<br/>Second line after br.<br/>Third line after br.</p>
<p>Normal paragraph after br paragraph.</p>
""",
        css_link="style.css",
    )


def ch_tables():
    """Chapter 14: Basic table rendering."""
    return make_xhtml(
        "Tables",
        """\
<h1>14. Tables</h1>
<table>
  <tr><td>Row 1, Col 1</td><td>Row 1, Col 2</td><td>Row 1, Col 3</td></tr>
  <tr><td>Row 2, Col 1</td><td>Row 2, Col 2</td><td>Row 2, Col 3</td></tr>
</table>
<p>Text after table.</p>
<table>
  <tr><th>Header A</th><th>Header B</th></tr>
  <tr><td>Data A1</td><td>Data B1</td></tr>
</table>
""",
        css_link="style.css",
    )


def ch_float():
    """Chapter 15: Float elements (converted to alt text)."""
    return make_xhtml(
        "Floats",
        """\
<h1>15. Floats</h1>
<p>Text before the float.</p>
<div class="floatleft">
  <img src="images/icon_16x16.jpg" alt="[float icon]"/>
</div>
<p>Text after a float div — should merge with float content.</p>
<p>Normal paragraph well after float.</p>
""",
        css_link="style.css",
    )


def ch_hidden():
    """Chapter 16: Hidden elements (display: none)."""
    return make_xhtml(
        "Hidden Elements",
        """\
<h1>16. Hidden Elements</h1>
<p>Visible paragraph.</p>
<div class="hidden"><p>THIS SHOULD NOT APPEAR in the output.</p></div>
<p>Another visible paragraph.</p>
<p>Inline: before <span class="hidden">HIDDEN TEXT</span> after.</p>
""",
        css_link="style.css",
    )


def ch_css_bold_italic():
    """Chapter 17: Bold/italic via CSS classes vs HTML tags."""
    return make_xhtml(
        "CSS Bold/Italic",
        """\
<h1>17. CSS Bold and Italic</h1>
<p class="css-bold">Bold via CSS class (font-weight: bold).</p>
<p class="css-italic">Italic via CSS class (font-style: italic).</p>
<p><b>Bold via &lt;b&gt; tag.</b></p>
<p><i>Italic via &lt;i&gt; tag.</i></p>
<p><em>Italic via &lt;em&gt; tag.</em></p>
<p><strong>Bold via &lt;strong&gt; tag.</strong></p>
<p class="css-bold">CSS bold containing <span class="css-normal">normal override</span> then bold again.</p>
""",
        css_link="style.css",
    )


def ch_unicode():
    """Chapter 18: Unicode and special characters."""
    return make_xhtml(
        "Unicode",
        """\
<h1>18. Unicode &amp; Special Characters</h1>

<h2>Latin-1 Supplement (U+00A0–00FF)</h2>
<p>German: Ä Ö Ü ä ö ü ß — Straße, Größe, Übung.</p>
<p>French: é è ê ë à â ç î ï ô ù û — Ça va? L'élève, crème brûlée.</p>
<p>Spanish: ¿Dónde está la biblioteca? ¡Excelente! Año, señor, niño.</p>
<p>Portuguese: São Paulo, coração, irmã, avô,ções.</p>
<p>Scandinavian: Ångström, blåbær, Ærø, fjörd, Ölüdeniz.</p>
<p>Icelandic: Þetta reddast. Ísland, Eyjafjallajökull, Þór, Ævar, Ö.</p>
<p>Latin-1 symbols: © ® ™ ° ± × ÷ § ¶ « » ¡ ¿ £ ¥ ¢ µ ¹ ² ³ ½ ¼ ¾.</p>

<h2>Latin Extended-A (U+0100–017F)</h2>
<p>Polish: Łódź, Świętokrzyski, źródło, żółty, ćma, Wrocław, Gdańsk.</p>
<p>Czech: Příliš žluťoučký kůň úpěl ďábelské ódy. Říp, Ústí, Brně.</p>
<p>Slovak: Ľúbostný šťastný ďateľ, žriebä, ťava, Nitra, Žilina.</p>
<p>Hungarian: Tűzoltóság, Székesfehérvár, Győr, ökör, ütő, Ő, Ű.</p>
<p>Romanian: Văd că după încheierea lucrărilor, ședința s-a încheiat.</p>
<p>Turkish: İstanbul, Ankara, güneş, öğretmen, çiçek, Ğ ğ İ ı Ş ş.</p>
<p>Latvian: Rīga, ķēniņš, ļoti, ņemt, ģimene, āboltiņš.</p>
<p>Lithuanian: Vilnius, Klaipėda, širdis, žmogus, ūkis, čia.</p>
<p>Croatian: Čakovec, Đurđevac, Šibenik, Žumberak, ćevapi.</p>

<h2>Latin Extended-B (U+0180–024F)</h2>
<p>African languages: Ɓ ɓ Ɗ ɗ Ƙ ƙ Ɲ ɲ Ŋ ŋ Ɔ ɔ Ʃ ʃ.</p>
<p>Pinyin tone marks: ǎ ǐ ǒ ǔ ǖ ǘ ǚ ǜ.</p>

<h2>Greek (U+0370–03FF)</h2>
<p>Ελληνικά: Η γρήγορη αλεπού πηδά πάνω από το τεμπέλικο σκυλί.</p>
<p>Alphabet: Α Β Γ Δ Ε Ζ Η Θ Ι Κ Λ Μ Ν Ξ Ο Π Ρ Σ Τ Υ Φ Χ Ψ Ω.</p>
<p>Lowercase: α β γ δ ε ζ η θ ι κ λ μ ν ξ ο π ρ σ τ υ φ χ ψ ω.</p>
<p>Math/science: π ≈ 3.14159, Σ (sigma), Δx, Ω (ohm), θ = 45°.</p>

<h2>Cyrillic (U+0400–04FF)</h2>
<p>Russian: Съешь ещё этих мягких французских булок, да выпей чаю.</p>
<p>Ukrainian: Щасливий їжак, ґанок, єдність. Їжак з'їв яблуко.</p>
<p>Bulgarian: Шумно поле. Живот, цвят, чудо, ъгъл.</p>
<p>Serbian Cyrillic: Ђурђевдан, Љубав, Њушка, Ћуприја, Џеп.</p>
<p>Alphabet: А Б В Г Д Е Ж З И Й К Л М Н О П Р С Т У Ф Х Ц Ч Ш Щ Ъ Ы Ь Э Ю Я.</p>

<h2>Latin Extended Additional (U+1E00–1EFF) — Vietnamese</h2>
<p>Vietnamese: Tôi yêu Việt Nam. Hà Nội, Sài Gòn, Đà Nẵng.</p>
<p>Tones: ả ạ ắ ằ ẳ ẵ ặ ế ề ể ễ ệ ỉ ị ỏ ọ ố ồ ổ ỗ ộ ớ ờ ở ỡ ợ ủ ụ ứ ừ ử ữ ự ỳ ỵ ỷ ỹ.</p>

<h2>General Punctuation (U+2000–206F)</h2>
<p>Dashes: en–dash, em—dash, figure‒dash, horizontal―bar.</p>
<p>Quotes: "double curly" 'single curly' „German low-high" «guillemets».</p>
<p>Dots: Ellipsis… Mid·dot. Bullet•list.</p>
<p>Spaces: thin\u2009space, hair\u200aspace, en\u2002space, em\u2003space.</p>
<p>Other: † dagger, ‡ double dagger, ‰ per mille, ′ prime, ″ double prime.</p>

<h2>Typographic / Smart Quotes</h2>
<p>\N{LEFT DOUBLE QUOTATION MARK}Double opening\N{RIGHT DOUBLE QUOTATION MARK} and \N{LEFT DOUBLE QUOTATION MARK}double closing\N{RIGHT DOUBLE QUOTATION MARK} curly quotes (U+201C / U+201D).</p>
<p>\N{LEFT SINGLE QUOTATION MARK}Single opening\N{RIGHT SINGLE QUOTATION MARK} and \N{LEFT SINGLE QUOTATION MARK}single closing\N{RIGHT SINGLE QUOTATION MARK} curly quotes (U+2018 / U+2019).</p>
<p>Apostrophe: it\N{RIGHT SINGLE QUOTATION MARK}s, don\N{RIGHT SINGLE QUOTATION MARK}t, rock \N{LEFT SINGLE QUOTATION MARK}n\N{RIGHT SINGLE QUOTATION MARK} roll, the \N{RIGHT SINGLE QUOTATION MARK}90s, o\N{RIGHT SINGLE QUOTATION MARK}clock.</p>
<p>\N{DOUBLE LOW-9 QUOTATION MARK}German low-9 open\N{LEFT DOUBLE QUOTATION MARK} and \N{LEFT DOUBLE QUOTATION MARK}high-6 close\N{RIGHT DOUBLE QUOTATION MARK} (U+201E / U+201C / U+201D).</p>
<p>\N{SINGLE LOW-9 QUOTATION MARK}Single low-9\N{LEFT SINGLE QUOTATION MARK} and \N{LEFT SINGLE QUOTATION MARK}high-6\N{RIGHT SINGLE QUOTATION MARK} (U+201A / U+2018 / U+2019).</p>
<p>«French guillemets» and »reversed guillemets« (U+00AB / U+00BB).</p>
<p>\N{SINGLE LEFT-POINTING ANGLE QUOTATION MARK}Single guillemet\N{SINGLE RIGHT-POINTING ANGLE QUOTATION MARK} (U+2039 / U+203A).</p>
<p>Mixed: She said, \N{LEFT DOUBLE QUOTATION MARK}It\N{RIGHT SINGLE QUOTATION MARK}s a \N{LEFT SINGLE QUOTATION MARK}great\N{RIGHT SINGLE QUOTATION MARK} day!\N{RIGHT DOUBLE QUOTATION MARK} and he replied, \N{LEFT DOUBLE QUOTATION MARK}Indeed.\N{RIGHT DOUBLE QUOTATION MARK}</p>
<p>Nested: \N{LEFT DOUBLE QUOTATION MARK}He asked \N{LEFT SINGLE QUOTATION MARK}Are you sure?\N{RIGHT SINGLE QUOTATION MARK} and left.\N{RIGHT DOUBLE QUOTATION MARK}</p>

<h2>Currency Symbols (U+20A0–20CF)</h2>
<p>€ Euro, ₹ Rupee, ₿ Bitcoin, ₺ Lira, ₴ Hryvnia, ₱ Peso, ₩ Won.</p>
<p>Prices: €12.50, $99.99, £45.00, ¥2500, ₹750, ₩15000.</p>

<h2>Superscripts, Subscripts &amp; Number Forms</h2>
<p>Superscripts: ⁰ ¹ ² ³ ⁴ ⁵ ⁶ ⁷ ⁸ ⁹ ⁿ.</p>
<p>Subscripts: ₀ ₁ ₂ ₃ ₄ ₅ ₆ ₇ ₈ ₉.</p>
<p>Fractions: ½ ⅓ ⅔ ¼ ¾ ⅕ ⅖ ⅗.</p>

<h2>Entities &amp; Misc</h2>
<p>Entities: &amp; &lt; &gt; &quot; &apos; &mdash; &ndash; &hellip;</p>
<p>Non-breaking space: word1&nbsp;word2&nbsp;word3 (should not break).</p>
<p>Thin space: §§\u200942\u2009b (U+2009 thin space).</p>
<p>Guillemets: »Quoted text« and «French quotes».</p>
<p>Em dash: text — more text — end.</p>
""",
        css_link="style.css",
    )


def ch_images_large():
    """Chapter 19: Large images (should scale to full width)."""
    return make_xhtml(
        "Large Images",
        """\
<h1>19. Large Images (&gt;= half page width)</h1>
<p>Text before image.</p>
<img src="images/large_400x300.jpg" alt="Large 400x300"/>
<p>Text after large image. The image above should scale to full page width (480px).</p>
<img src="images/full_480x360.png" alt="Full width 480x360"/>
<p>Already full-width image — no scaling needed.</p>
""",
        css_link="style.css",
    )


def ch_images_small():
    """Chapter 20: Small images (should NOT scale up)."""
    return make_xhtml(
        "Small Images",
        """\
<h1>20. Small Images (&lt; half page width)</h1>
<p>Text before small icon.</p>
<img src="images/icon_16x16.jpg" alt="Tiny icon 16x16"/>
<p>The 16x16 icon above should NOT be blown up to full width. It should stay tiny and centered.</p>
<img src="images/medium_100x80.jpg" alt="Medium 100x80"/>
<p>The 100x80 image above should also stay at original size (under 240px threshold).</p>
""",
        css_link="style.css",
    )


def ch_images_tall():
    """Chapter 21: Tall/narrow images (height > page height)."""
    return make_xhtml(
        "Tall Images",
        """\
<h1>21. Tall / Narrow Images</h1>
<p>A very narrow tall image (like a book spine):</p>
<img src="images/spine_60x500.png" alt="Narrow spine 60x500"/>
<p>The spine image should stay at its intrinsic 60px width (too narrow to scale up).</p>
<p>A tall wide image that exceeds page height:</p>
<img src="images/tall_400x1000.jpg" alt="Tall 400x1000"/>
<p>Should be scaled to fit in page height (800px), aspect ratio preserved.</p>
""",
        css_link="style.css",
    )


def ch_page_break():
    """Chapter 22: Page breaks via CSS page-break-before."""
    return make_xhtml(
        "Page Breaks",
        """\
<h1>22. Page Breaks (CSS-driven)</h1>
<p>Headings do NOT force page breaks by default. Only CSS page-break-before: always does.</p>
<h2>This H2 should NOT start a new page</h2>
<p>Because there is no page-break-before CSS on it.</p>
<div style="page-break-before: always;">
  <p>This paragraph IS on a new page — the div has page-break-before: always.</p>
</div>
<p>Content continues after the forced break.</p>
""",
        css_link="style.css",
    )


def ch_empty_elements():
    """Chapter 23: Empty paragraphs and whitespace-only content."""
    return make_xhtml(
        "Empty Elements",
        """\
<h1>23. Empty Elements</h1>
<p>Normal paragraph.</p>
<p></p>
<p>   </p>
<p>After two empty/whitespace paragraphs.</p>
<div></div>
<p>After an empty div.</p>
""",
        css_link="style.css",
    )


def ch_links():
    """Chapter 24: Links and anchors — comprehensive href attribution tests."""
    return make_xhtml(
        "Links",
        """\
<h1>24. Links</h1>

<p>Simple inline link: before <a href="ch01_basic.xhtml">link text</a> after. Only the link should be underlined.</p>

<p>Link at paragraph start: <a href="ch02_headings.xhtml">start link</a> then normal text.</p>

<p>Multiple links on one line: <a href="#sec1">Section 1</a> and <a href="#sec2">Section 2</a> and <a href="#sec3">Section 3</a>.</p>

<p>Footnote reference<a href="#fn1"><sup>1</sup></a> in text. More text after footnote marker.</p>

<p>Link with styled content: <a href="ch03_font_sizes.xhtml"><b>bold link</b></a> and <a href="ch04_nested_font.xhtml"><i>italic link</i></a>.</p>

<h2>Table of Contents (tests href attribution in table rows)</h2>
<table>
  <tr><td style="text-align: right;">I. </td><td style="text-align: left;"><span>First Chapter</span></td><td style="text-align: right;"><a href="ch01_basic.xhtml#Page_1">1</a></td></tr>
  <tr><td style="text-align: right;">II. </td><td style="text-align: left;"><span>Second Chapter</span></td><td style="text-align: right;"><a href="ch02_headings.xhtml#Page_13">13</a></td></tr>
  <tr><td style="text-align: right;">III. </td><td style="text-align: left;"><span>Third Chapter</span></td><td style="text-align: right;"><a href="ch03_font_sizes.xhtml#Page_24">24</a></td></tr>
</table>

<div id="fn1"><p><a href="#fn1">1</a> This is the footnote text. Clicking the footnote marker in the paragraph above should jump here.</p></div>
""",
        css_link="style.css",
    )


def ch_mixed_stress():
    """Chapter 25: Long mixed content stress test."""
    paras = []
    for i in range(1, 21):
        styles = [
            f"Paragraph {i}.",
            f"<b>Bold chunk {i}.</b>",
            f"<i>Italic chunk {i}.</i>",
            f'<span class="fs-small">Small chunk {i}.</span>',
            f"Normal ending for paragraph {i}.",
        ]
        paras.append("<p>" + " ".join(styles) + "</p>")
    body = "<h1>25. Mixed Content Stress Test</h1>\n" + "\n".join(paras)
    return make_xhtml("Stress Test", body, css_link="style.css")


def ch_margin_shorthand():
    """Chapter 26: Margin shorthand (1, 2, and 4-value forms)."""
    return make_xhtml(
        "Margin Shorthand",
        """\
<h1>26. Margin Shorthand</h1>
<p>Normal paragraph (no margin).</p>
<p class="margin-1val">24px all sides (margin: 24px) — left and right should be 24px.</p>
<p class="margin-2val">LR 36px (margin: 10px 36px) — left and right margins.</p>
<p class="margin-4val">L24 R48 (margin: 5px 48px 5px 24px) — asymmetric.</p>
<p>Back to normal paragraph.</p>
""",
        css_link="style.css",
    )


def ch_page_break_after():
    """Chapter 27: Page-break-after CSS property."""
    return make_xhtml(
        "Page Break After",
        """\
<h1>27. Page Break After</h1>
<p>This paragraph has no page break.</p>
<div style="page-break-after: always;">
  <p>This div has page-break-after: always. A page break should follow.</p>
</div>
<p>This paragraph should be on a NEW page after the break-after div.</p>
<p>More content following the break.</p>
""",
        css_link="style.css",
    )


def ch_text_transform():
    """Chapter 28: text-transform CSS property."""
    return make_xhtml(
        "Text Transform",
        """\
<h1>28. Text Transform</h1>
<p class="tt-upper">this text should be uppercase.</p>
<p class="tt-lower">THIS TEXT SHOULD BE LOWERCASE.</p>
<p class="tt-cap">these words should each be capitalized.</p>
<p>This paragraph has <span class="tt-upper">uppercase span</span> inside.</p>
<p>Normal paragraph with no transform.</p>
""",
        css_link="style.css",
    )


def ch_blockquote():
    """Chapter 29: Blockquote and definition list defaults."""
    return make_xhtml(
        "Blockquote",
        """\
<h1>29. Blockquote &amp; Definition Lists</h1>
<p>Normal paragraph before blockquote.</p>
<blockquote>
  <p>This is a blockquote. It should have a default left margin even without CSS styling.</p>
  <p>Second paragraph inside the blockquote.</p>
</blockquote>
<p>Normal paragraph after blockquote.</p>
<dl>
  <dt>Definition Term</dt>
  <dd>Definition description — should be indented by default.</dd>
  <dt>Another Term</dt>
  <dd>Another description with default indent.</dd>
</dl>
""",
        css_link="style.css",
    )


def ch_vertical_margins():
    """Chapter 30: Vertical margins (margin-top / margin-bottom)."""
    return make_xhtml(
        "Vertical Margins",
        """\
<h1>30. Vertical Margins</h1>
<p>Normal spacing before this paragraph.</p>
<p class="mt-large">This paragraph has margin-top: 24px — extra space above.</p>
<p>Back to normal spacing.</p>
<p class="mb-large">This paragraph has margin-bottom: 24px.</p>
<p>The gap above should be larger than default.</p>
<p class="mt-zero">margin-top: 0 — should be tight against the previous paragraph.</p>
<p>Normal paragraph.</p>
<p class="vert-margin">margin: 20px 0 — vertical shorthand, 20px top and bottom.</p>
<p>After shorthand vertical margin.</p>
""",
        css_link="style.css",
    )


def ch_preformatted():
    """Chapter 31: Preformatted text (<pre>)."""
    return make_xhtml(
        "Preformatted Text",
        """\
<h1>31. Preformatted Text</h1>
<p>Normal paragraph before pre block.</p>
<pre>
  This is preformatted text.
  Spaces    are    preserved.
  Line breaks are kept.

  Blank lines too.
</pre>
<p>Normal paragraph after pre block.</p>
<pre>int main() {
    printf("Hello, world!\\n");
    return 0;
}</pre>
""",
        css_link="style.css",
    )


def ch_inline_float_images():
    """Chapter 32: Inline float images (drop-cap style, various sizes)."""
    return make_xhtml(
        "Inline Float Images",
        """\
<h1>32. Inline Float Images</h1>

<p>Tiny 16x16 float icon beside text:</p>
<div class="floatleft">
  <img src="images/icon_16x16.jpg" alt="[tiny]"/>
</div>
<p>This text should have a tiny 16x16 icon at the start, rendered inline like a drop cap.</p>

<p>Medium 40x40 float image beside text:</p>
<div class="floatleft">
  <img src="images/float_40x40.png" alt="[medium]"/>
</div>
<p>This text should have a 40x40 image at the start. The first line is indented by the image width.</p>

<p>Tall 60x100 float image beside text:</p>
<div class="floatleft">
  <img src="images/float_60x100.png" alt="[tall]"/>
</div>
<p>This text has a 60x100 float. Since height exceeds 120px threshold? No, 100 is under. The image bottom aligns with the first text line bottom, extending upward above the text.</p>

<p>Large float (200x200) should be promoted to standalone:</p>
<div class="floatleft">
  <img src="images/float_200x200.png" alt="[large]"/>
</div>
<p>This text follows a 200x200 float image. Since it exceeds width/3 threshold, it should be promoted to a standalone image paragraph, not rendered inline.</p>
""",
        css_link="style.css",
    )


def ch_inline_whitespace():
    """Chapter 33: Whitespace between inline elements (Bible TOC bug).
    Numbers inside <a> tags separated by whitespace-only text nodes
    must render as separate words, not merged."""
    return make_xhtml(
        "Inline Whitespace",
        """\
<h1>33. Inline Whitespace</h1>
<p>Numbers in links separated by whitespace:</p>
<p><a href="#1">1</a> <a href="#2">2</a> <a href="#3">3</a> <a href="#4">4</a> <a href="#5">5</a> <a href="#6">6</a> <a href="#7">7</a> <a href="#8">8</a> <a href="#9">9</a> <a href="#10">10</a></p>
<p>Should read: 1 2 3 4 5 6 7 8 9 10 (not 12345678910).</p>

<p>Spans separated by whitespace:</p>
<p><span class="css-bold">Alpha</span> <span class="css-italic">Beta</span> <span>Gamma</span> <span class="css-bold">Delta</span></p>
<p>Should read: Alpha Beta Gamma Delta.</p>

<p>Mixed inline elements:</p>
<p><b>one</b> <i>two</i> <em>three</em> <strong>four</strong> <a href="#">five</a></p>
""",
        css_link="style.css",
    )


def ch_punctuation_after_style():
    """Chapter 34: Punctuation directly after styled text (comma-spacing bug).
    <b>word</b>, should NOT have a gap before the comma during justification."""
    return make_xhtml(
        "Punctuation After Style",
        """\
<h1>34. Punctuation After Style</h1>
<p style="text-align: justify;">This sentence has <b>bold text</b>, followed by a comma. The comma must sit right against the bold word with no extra gap introduced by justification.</p>

<p style="text-align: justify;">Here is <i>italic text</i>; with a semicolon. And <em>emphasized</em>: with a colon. And <b>bold</b>. with a period.</p>

<p style="text-align: justify;">Parentheses test: (<b>bold inside parens</b>) should have no gap before the closing paren.</p>

<p style="text-align: justify;">Multiple style changes: <b>bold</b>, <i>italic</i>, <em>emph</em>, <strong>strong</strong>. All commas should be tight against the preceding word.</p>

<p style="text-align: justify;">End of sentence with <b>bold</b>. New sentence starts here and continues with enough words to trigger justification on the line above.</p>
""",
        css_link="style.css",
    )


def ch_br_whitespace():
    """Chapter 35: Whitespace after <br/> should not cause indentation.
    HTML source formatting (newlines/tabs after <br/>) must not create
    visible leading space on the next line."""
    return make_xhtml(
        "BR Whitespace",
        """\
<h1>35. BR Whitespace</h1>
<p>Line before br.<br/>
    This line has source indentation after br — should start at left:0.</p>

<p>Another test:<br/>
        Deep indent in source — should still start at left:0.</p>

<p>Multiple br with source formatting:<br/>
  First line after br.<br/>
  Second line after br.<br/>
  Third line after br.</p>

<p>Br with tab indentation:<br/>	Tab indented in source — should start at left:0.</p>
""",
        css_link="style.css",
    )


def ch_standalone_br_spacing():
    """Chapter 36: Standalone <br/> between block elements should create
    visible vertical space (empty blank lines)."""
    return make_xhtml(
        "Standalone BR Spacing",
        """\
<h1>36. Standalone BR Spacing</h1>
<p>Paragraph before two br tags.</p>
<br/>
<br/>
<h2>Heading After Two BRs</h2>
<p>There should be visible empty space (two blank lines) between the paragraph above and this heading.</p>

<p>One br between paragraphs:</p>
<br/>
<p>This paragraph follows one br. There should be one blank line gap.</p>

<p>Three br tags for a big gap:</p>
<br/>
<br/>
<br/>
<p>Three blank lines of space should appear above this paragraph.</p>

<p>Br between headings:</p>
<h3>Heading A</h3>
<br/>
<h3>Heading B</h3>
<p>There should be a visible blank line between Heading A and Heading B.</p>
""",
        css_link="style.css",
    )


def ch_nested_lists():
    """Chapter 37: Nested lists — each level should be indented further."""
    return make_xhtml(
        "Nested Lists",
        """\
<h1>37. Nested Lists</h1>
<p>Two-level unordered list:</p>
<ul>
  <li>Level 1 item A</li>
  <li>Level 1 item B
    <ul>
      <li>Level 2 item B.1</li>
      <li>Level 2 item B.2</li>
    </ul>
  </li>
  <li>Level 1 item C</li>
</ul>

<p>Two-level ordered list:</p>
<ol>
  <li>First
    <ol>
      <li>Sub-first</li>
      <li>Sub-second</li>
    </ol>
  </li>
  <li>Second</li>
</ol>

<p>Three-level mixed nesting:</p>
<ul>
  <li>Outer bullet
    <ol>
      <li>Middle numbered
        <ul>
          <li>Inner bullet</li>
        </ul>
      </li>
    </ol>
  </li>
</ul>
<p>Text after nested lists.</p>
""",
        css_link="style.css",
    )


def ch_ol_start():
    """Chapter 38: Ordered list with start attribute."""
    return make_xhtml(
        "OL Start Attribute",
        """\
<h1>38. OL Start Attribute</h1>
<p>Ordered list starting at 5:</p>
<ol start="5">
  <li>This should be 5.</li>
  <li>This should be 6.</li>
  <li>This should be 7.</li>
</ol>

<p>Ordered list starting at 10:</p>
<ol start="10">
  <li>This should be 10.</li>
  <li>This should be 11.</li>
</ol>

<p>Default ordered list (starts at 1):</p>
<ol>
  <li>This should be 1.</li>
  <li>This should be 2.</li>
</ol>
<p>Text after OL start tests.</p>
""",
        css_link="style.css",
    )


def ch_figure_figcaption():
    """Chapter 39: figure and figcaption with default styling."""
    return make_xhtml(
        "Figure & Figcaption",
        """\
<h1>39. Figure &amp; Figcaption</h1>
<figure>
  <p>This text is inside a figure and should be centered.</p>
  <figcaption>This caption should be small, italic, and centered.</figcaption>
</figure>

<p>Normal paragraph between figures.</p>

<figure>
  <figcaption>Caption before content — small italic centered.</figcaption>
  <p>Figure body text — centered.</p>
</figure>

<p>Figcaption with CSS override:</p>
<figure>
  <figcaption style="text-align: left; font-style: normal; font-size: large;">
    Left-aligned, normal weight, large caption.
  </figcaption>
</figure>
<p>Text after figure tests.</p>
""",
        css_link="style.css",
    )


def ch_line_height():
    """Chapter 40: line-height CSS property."""
    return make_xhtml(
        "Line Height",
        """\
<h1>40. Line Height</h1>
<p>Normal line height (default). This paragraph has enough text to span multiple lines so you can see the default line spacing applied by the reader.</p>

<p class="lh-tight">Tight line height (100% = tighter than default). This paragraph has enough text to span multiple lines so you can clearly see the reduced line spacing.</p>

<p class="lh-loose">Loose line height (180% = much more spacing). This paragraph has enough text to span multiple lines so you can clearly see the increased line spacing.</p>

<p class="lh-1_8">Unitless 1.8 line height (same as 180% effectively). This paragraph has enough text to span multiple lines so you can see the wider line spacing.</p>

<p class="lh-normal">Explicit normal line height. Should look like the default paragraph above.</p>

<div class="lh-loose">
  <p>Paragraph inside a div with loose line-height. The div's line-height should propagate to this paragraph.</p>
  <p>Second paragraph inside the same div. Also loose.</p>
</div>

<p>Normal paragraph after the div — should return to default line-height.</p>
""",
        css_link="style.css",
    )


def ch_centered_with_margins():
    """Chapter 42: Centered text inside indented containers.
    Regression test: centered lines must be visually symmetric even when
    the paragraph has a margin-left (the flush_line normalisation fix)."""
    return make_xhtml(
        "Centered With Margins",
        """\
<h1>42. Centered Text With Margins</h1>
<p>Normal full-width centered paragraph -- the visible left and right whitespace should be equal.</p>
<p class="align-center">Short centered line.</p>
<p class="align-center">A longer centered line that takes up most of the available width on the page.</p>

<p>Centered text inside a 2em left-margin container:</p>
<div class="ml-2em">
  <p class="align-center">Centered inside 2em margin div.</p>
  <p class="align-center">Longer centered line inside the 2em left-margin div -- should still be symmetric.</p>
</div>

<p>Centered text inside a blockquote (has default left margin):</p>
<blockquote>
  <p class="align-center">Centered inside blockquote -- symmetric gaps on both sides.</p>
  <p class="align-center">Second blockquote line, centered.</p>
</blockquote>

<p>Centered poem lines (poem class has 30% margins on both sides):</p>
<div class="poem">
  <p class="align-center">Tyger, Tyger, burning bright,</p>
  <p class="align-center">In the forests of the night,</p>
  <p class="align-center">What immortal hand or eye</p>
  <p class="align-center">Could frame thy fearful symmetry?</p>
</div>

<p>Right-aligned text inside a 2em margin container:</p>
<div class="ml-2em">
  <p class="align-right">Right-aligned inside 2em margin -- should be flush with the right edge.</p>
</div>
<p>Text after all centered-with-margin tests.</p>
""",
        css_link="style.css",
    )


def ch_mixed_sizes_baseline():
    """Chapter 41: Mixed font sizes on one line -- baseline alignment test."""
    return make_xhtml(
        "Mixed Size Baseline",
        """\
<h1>41. Mixed Font Sizes — Baseline Alignment</h1>

<p>All sizes inline: Normal <span style="font-size: small">Small</span> Normal <span style="font-size: large">Large</span> Normal <span style="font-size: x-large">XLarge</span> Normal <span style="font-size: xx-large">XXLarge</span> Normal.</p>

<p>The bottoms of all words above should sit on the same baseline, regardless of size.</p>

<p>Small to XXLarge ramp: <span style="font-size: small">Abc</span> <span style="font-size: medium">Abc</span> <span style="font-size: large">Abc</span> <span style="font-size: x-large">Abc</span> <span style="font-size: xx-large">Abc</span>.</p>

<p>Numbers at each size: <span style="font-size: small">123</span> <span style="font-size: medium">123</span> <span style="font-size: large">123</span> <span style="font-size: x-large">123</span> <span style="font-size: xx-large">123</span>.</p>

<p>Mixed with bold: <b style="font-size: small">Bold-S</b> Normal <b style="font-size: large">Bold-L</b> Normal <b style="font-size: xx-large">Bold-XXL</b> Normal.</p>

<p>Mixed with italic: <i style="font-size: small">Italic-S</i> Normal <i style="font-size: x-large">Italic-XL</i> Normal.</p>

<p>Sub and sup mixed with sizes: Normal H<sub>2</sub>O and E=mc<sup>2</sup> plus <span style="font-size: large">Large H<sub>2</sub>O</span> and <span style="font-size: x-large">XL E=mc<sup>2</sup></span>.</p>

<p>All five sizes with descenders: <span style="font-size: small">gypsy</span> <span style="font-size: medium">gypsy</span> <span style="font-size: large">gypsy</span> <span style="font-size: x-large">gypsy</span> <span style="font-size: xx-large">gypsy</span>.</p>
""",
        css_link="style.css",
    )


# ---------------------------------------------------------------------------
# Image generation
# ---------------------------------------------------------------------------


def generate_images():
    """Generate all test images. Returns list of (arcname, bytes) pairs."""
    images = []

    # Large image (400x300) — should scale up to 480
    images.append(("OEBPS/images/large_400x300.jpg", make_jpeg(400, 300, 180)))

    # Full width (480x360) — no scaling needed
    images.append(("OEBPS/images/full_480x360.png", make_png(480, 360, 120, 180, 120)))

    # Tiny icon (16x16) — should NOT scale up
    images.append(("OEBPS/images/icon_16x16.jpg", make_jpeg(16, 16, 100)))

    # Medium image (100x80) — should NOT scale up (< 240px)
    images.append(("OEBPS/images/medium_100x80.jpg", make_jpeg(100, 80, 150)))

    # Narrow spine (60x500) — should NOT scale up (< 240px)
    images.append(("OEBPS/images/spine_60x500.png", make_png(60, 500, 80, 80, 80)))

    # Tall image (400x1000) — should scale up then cap to page height
    images.append(("OEBPS/images/tall_400x1000.jpg", make_jpeg(400, 1000, 160)))

    # Float images for ch32 — inline float tests
    images.append(("OEBPS/images/float_40x40.png", make_png(40, 40, 200, 100, 100)))
    images.append(("OEBPS/images/float_60x100.png", make_png(60, 100, 100, 200, 100)))
    images.append(("OEBPS/images/float_200x200.png", make_png(200, 200, 100, 100, 200)))

    return images


# ---------------------------------------------------------------------------
# Assemble EPUB
# ---------------------------------------------------------------------------


def main():
    chapters = [
        ("ch01", "ch01_basic.xhtml", "1. Basic Text", ch_basic_text()),
        ("ch02", "ch02_headings.xhtml", "2. Headings", ch_headings()),
        ("ch03", "ch03_font_sizes.xhtml", "3. Font Sizes", ch_font_sizes()),
        (
            "ch04",
            "ch04_nested_font.xhtml",
            "4. Nested Font Size",
            ch_nested_font_size(),
        ),
        ("ch05", "ch05_style_nesting.xhtml", "5. Style Nesting", ch_style_nesting()),
        ("ch06", "ch06_margins.xhtml", "6. Margins", ch_margins()),
        ("ch07", "ch07_nested_margins.xhtml", "7. Nested Margins", ch_nested_margins()),
        ("ch08", "ch08_alignment.xhtml", "8. Alignment", ch_alignment()),
        ("ch09", "ch09_indent.xhtml", "9. Indentation", ch_indent()),
        ("ch10", "ch10_lists.xhtml", "10. Lists", ch_lists()),
        ("ch11", "ch11_hr.xhtml", "11. Horizontal Rules", ch_hr()),
        (
            "ch12",
            "ch12_small_sub_sup.xhtml",
            "12. small / sub / sup",
            ch_small_sub_sup(),
        ),
        ("ch13", "ch13_br.xhtml", "13. Line Breaks", ch_br()),
        ("ch14", "ch14_tables.xhtml", "14. Tables", ch_tables()),
        ("ch15", "ch15_floats.xhtml", "15. Floats", ch_float()),
        ("ch16", "ch16_hidden.xhtml", "16. Hidden Elements", ch_hidden()),
        (
            "ch17",
            "ch17_css_bold_italic.xhtml",
            "17. CSS Bold/Italic",
            ch_css_bold_italic(),
        ),
        ("ch18", "ch18_unicode.xhtml", "18. Unicode", ch_unicode()),
        ("ch19", "ch19_images_large.xhtml", "19. Large Images", ch_images_large()),
        ("ch20", "ch20_images_small.xhtml", "20. Small Images", ch_images_small()),
        ("ch21", "ch21_images_tall.xhtml", "21. Tall Images", ch_images_tall()),
        ("ch22", "ch22_page_break.xhtml", "22. Page Breaks", ch_page_break()),
        ("ch23", "ch23_empty.xhtml", "23. Empty Elements", ch_empty_elements()),
        ("ch24", "ch24_links.xhtml", "24. Links", ch_links()),
        ("ch25", "ch25_stress.xhtml", "25. Stress Test", ch_mixed_stress()),
        (
            "ch26",
            "ch26_margin_short.xhtml",
            "26. Margin Shorthand",
            ch_margin_shorthand(),
        ),
        (
            "ch27",
            "ch27_break_after.xhtml",
            "27. Page Break After",
            ch_page_break_after(),
        ),
        (
            "ch28",
            "ch28_text_transform.xhtml",
            "28. Text Transform",
            ch_text_transform(),
        ),
        ("ch29", "ch29_blockquote.xhtml", "29. Blockquote & DL", ch_blockquote()),
        (
            "ch30",
            "ch30_vert_margins.xhtml",
            "30. Vertical Margins",
            ch_vertical_margins(),
        ),
        ("ch31", "ch31_preformatted.xhtml", "31. Preformatted Text", ch_preformatted()),
        (
            "ch32",
            "ch32_inline_floats.xhtml",
            "32. Inline Float Images",
            ch_inline_float_images(),
        ),
        (
            "ch33",
            "ch33_inline_ws.xhtml",
            "33. Inline Whitespace",
            ch_inline_whitespace(),
        ),
        (
            "ch34",
            "ch34_punct_style.xhtml",
            "34. Punctuation After Style",
            ch_punctuation_after_style(),
        ),
        ("ch35", "ch35_br_ws.xhtml", "35. BR Whitespace", ch_br_whitespace()),
        (
            "ch36",
            "ch36_br_spacing.xhtml",
            "36. Standalone BR Spacing",
            ch_standalone_br_spacing(),
        ),
        ("ch37", "ch37_nested_lists.xhtml", "37. Nested Lists", ch_nested_lists()),
        ("ch38", "ch38_ol_start.xhtml", "38. OL Start Attribute", ch_ol_start()),
        (
            "ch39",
            "ch39_figcaption.xhtml",
            "39. Figure & Figcaption",
            ch_figure_figcaption(),
        ),
        ("ch40", "ch40_line_height.xhtml", "40. Line Height", ch_line_height()),
        (
            "ch41",
            "ch41_mixed_baseline.xhtml",
            "41. Mixed Size Baseline",
            ch_mixed_sizes_baseline(),
        ),
        (
            "ch42",
            "ch42_centered_margins.xhtml",
            "42. Centered With Margins",
            ch_centered_with_margins(),
        ),
    ]

    # Build manifest and spine
    manifest_items = []
    spine_idrefs = []
    nav_points = []

    manifest_items.append(("ncx", "toc.ncx", "application/x-dtbncx+xml"))
    manifest_items.append(("css", "style.css", "text/css"))

    for cid, fname, label, _ in chapters:
        manifest_items.append((cid, fname, "application/xhtml+xml"))
        spine_idrefs.append(cid)
        nav_points.append((label, fname))

    # Add image manifest entries
    manifest_items.append(("img_large", "images/large_400x300.jpg", "image/jpeg"))
    manifest_items.append(("img_full", "images/full_480x360.png", "image/png"))
    manifest_items.append(("img_icon", "images/icon_16x16.jpg", "image/jpeg"))
    manifest_items.append(("img_medium", "images/medium_100x80.jpg", "image/jpeg"))
    manifest_items.append(("img_spine", "images/spine_60x500.png", "image/png"))
    manifest_items.append(("img_tall", "images/tall_400x1000.jpg", "image/jpeg"))
    manifest_items.append(("img_float40", "images/float_40x40.png", "image/png"))
    manifest_items.append(("img_float60", "images/float_60x100.png", "image/png"))
    manifest_items.append(("img_float200", "images/float_200x200.png", "image/png"))

    opf = make_opf(manifest_items, spine_idrefs, toc_id="ncx")
    ncx = make_ncx(nav_points)

    # Generate images
    images = generate_images()

    # Assemble files
    files = [
        ("META-INF/container.xml", CONTAINER_XML, False),
        ("OEBPS/content.opf", opf, True),
        ("OEBPS/toc.ncx", ncx, True),
        ("OEBPS/style.css", STYLESHEET, True),
    ]

    for _, fname, _, xhtml in chapters:
        files.append(("OEBPS/" + fname, xhtml, True))

    for arcname, data in images:
        files.append((arcname, data, False))

    write_epub("regression_test.epub", files)
    print("Done!")


if __name__ == "__main__":
    main()
