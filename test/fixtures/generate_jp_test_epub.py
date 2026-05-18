#!/usr/bin/env python3
"""Generate a Japanese font test EPUB for microreader2.

Systematically covers every character in the ranges included in NotoSansJP.mfb:
  ascii, latin1, general-punct, japanese (hiragana, katakana, CJK punct,
  enclosed CJK, CJK unified ideographs, CJK compat ideographs, fullwidth)
"""

import zipfile
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


def make_xhtml(title, body_html):
    return f"""\
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="ja">
<head><title>{title}</title></head>
<body>
{body_html}
</body>
</html>"""


def codepoints_to_para(start, end, per_line=32):
    """Emit all codepoints in [start, end] as paragraphs of per_line chars each,
    skipping surrogates and control characters."""
    chars = []
    for cp in range(start, end + 1):
        if 0xD800 <= cp <= 0xDFFF:
            continue  # surrogates
        try:
            c = chr(cp)
            c.encode('utf-8')
        except (ValueError, UnicodeEncodeError):
            continue
        chars.append(c)

    lines = []
    for i in range(0, len(chars), per_line):
        chunk = ''.join(chars[i:i + per_line])
        chunk = chunk.replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')
        lines.append(f'<p>{chunk}</p>')
    return '\n'.join(lines)


# Each chapter: (title, body_html)
# "Range map" chapters show every codepoint in the range.
# "Sample text" chapters show real Japanese prose.

RANGE_CHAPTERS = [
    ("ASCII (U+0020-U+007E)", "ASCII - Basic Latin", (0x0020, 0x007E)),
    ("Latin-1 (U+00A0-U+00FF)", "Latin-1 Supplement", (0x00A0, 0x00FF)),
    ("General Punctuation (U+2000-U+206F)", "General Punctuation", (0x2000, 0x206F)),
    ("CJK Symbols and Punct (U+3000-U+303F)", "CJK Symbols and Punctuation", (0x3000, 0x303F)),
    ("Hiragana (U+3040-U+309F)", "Hiragana", (0x3040, 0x309F)),
    ("Katakana (U+30A0-U+30FF)", "Katakana", (0x30A0, 0x30FF)),
    ("Enclosed CJK (U+3200-U+32FF)", "Enclosed CJK Letters and Months", (0x3200, 0x32FF)),
    ("CJK Compatibility (U+3300-U+33FF)", "CJK Compatibility", (0x3300, 0x33FF)),
    ("CJK Unified Ideographs 1 (U+4E00-U+5FFF)", "CJK Unified Ideographs - Part 1", (0x4E00, 0x5FFF)),
    ("CJK Unified Ideographs 2 (U+6000-U+6FFF)", "CJK Unified Ideographs - Part 2", (0x6000, 0x6FFF)),
    ("CJK Unified Ideographs 3 (U+7000-U+7FFF)", "CJK Unified Ideographs - Part 3", (0x7000, 0x7FFF)),
    ("CJK Unified Ideographs 4 (U+8000-U+8FFF)", "CJK Unified Ideographs - Part 4", (0x8000, 0x8FFF)),
    ("CJK Unified Ideographs 5 (U+9000-U+9FFF)", "CJK Unified Ideographs - Part 5", (0x9000, 0x9FFF)),
    ("CJK Compat Ideographs (U+F900-U+FAFF)", "CJK Compatibility Ideographs", (0xF900, 0xFAFF)),
    ("Fullwidth Forms (U+FF01-U+FF5E)", "Fullwidth Latin and Digits", (0xFF01, 0xFF5E)),
    ("Halfwidth Katakana (U+FF61-U+FF9F)", "Halfwidth Katakana", (0xFF61, 0xFF9F)),
    ("Fullwidth Currency (U+FFE0-U+FFE6)", "Fullwidth Currency Signs", (0xFFE0, 0xFFE6)),
]

SAMPLE_CHAPTERS = [
    ("Sentences", """\
<h1>Sentences</h1>
<p>Wagahai wa neko de aru. Namae wa mada nai.</p>
<p>吾輩は猫である。名前はまだ無い。</p>
<p>国境の長いトンネルを抜けると雪国であった。</p>
<p>メロスは激怒した。必ず、かの邪智暴虐の王を除かなければならぬと決意した。</p>
<p>親譲りの無鉄砲で小供の時から損ばかりしている。</p>
<p>祇園精舎の鐘の声、諸行無常の響きあり。</p>
<p>春はあけぼの。やうやうしろくなりゆく山ぎは、少しあかりて、紫だちたる雲の細くたなびきたる。</p>"""),
    ("Mixed Text", """\
<h1>Mixed Text</h1>
<p>日本語と English を混ぜたテキストです。</p>
<p>東京 (Tokyo) は日本の首都 (capital) です。人口 (population) は約 1,400万人 (14 million) です。</p>
<p>このスマートフォン (smartphone) は ¥50,000 です。</p>
<p>数字混じり: 第1章、第2節、3番目、4回目</p>
<p>括弧の種類: (丸括弧)、[角括弧]、「かぎ括弧」、『二重かぎ括弧』、【隅付き括弧】</p>
<p>記号: ・中点　…三点リーダ　—ダッシュ　〜波ダッシュ　※米印</p>"""),
]


def build_chapters():
    chapters = []
    for (short_title, long_title, (start, end)) in RANGE_CHAPTERS:
        body = f'<h1>{long_title}</h1>\n<p>Range: {short_title}</p>\n'
        body += codepoints_to_para(start, end)
        chapters.append((short_title, body))
    chapters.extend(SAMPLE_CHAPTERS)
    return chapters


def write_epub(name, chapters):
    opf_path = "OEBPS/content.opf"
    manifest_items = []
    spine_idrefs = []
    nav_points = []
    chapter_files = []

    for i, (title, body) in enumerate(chapters, 1):
        cid = f"ch{i}"
        fname = f"chapter{i}.xhtml"
        manifest_items.append(f'<item id="{cid}" href="{fname}" media-type="application/xhtml+xml"/>')
        spine_idrefs.append(f'<itemref idref="{cid}"/>')
        nav_points.append(f"""
    <navPoint id="np{i}" playOrder="{i}">
      <navLabel><text>{title}</text></navLabel>
      <content src="{fname}"/>
    </navPoint>""")
        chapter_files.append(("OEBPS/" + fname, make_xhtml(title, body)))

    manifest_items.append('<item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>')

    opf = f"""\
<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="2.0" unique-identifier="uid">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:title>Japanese Font Test</dc:title>
    <dc:creator>microreader2</dc:creator>
    <dc:language>ja</dc:language>
    <dc:identifier id="uid">test-japanese-font</dc:identifier>
  </metadata>
  <manifest>
    {"    ".join(manifest_items)}
  </manifest>
  <spine toc="ncx">
    {"    ".join(spine_idrefs)}
  </spine>
</package>"""

    ncx = f"""\
<?xml version="1.0" encoding="UTF-8"?>
<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1">
  <head/>
  <docTitle><text>Japanese Font Test</text></docTitle>
  <navMap>{"".join(nav_points)}
  </navMap>
</ncx>"""

    path = OUT / name
    with zipfile.ZipFile(path, "w") as zf:
        info = zipfile.ZipInfo("mimetype")
        info.compress_type = zipfile.ZIP_STORED
        zf.writestr(info, MIMETYPE)

        zf.writestr(zipfile.ZipInfo("META-INF/container.xml"), CONTAINER_XML)
        zf.writestr(zipfile.ZipInfo(opf_path), opf.encode("utf-8"))
        zf.writestr(zipfile.ZipInfo("OEBPS/toc.ncx"), ncx.encode("utf-8"))
        for arcname, data in chapter_files:
            zf.writestr(zipfile.ZipInfo(arcname), data.encode("utf-8"))

    print(f"  wrote {path}  ({path.stat().st_size:,} bytes)")


if __name__ == "__main__":
    print("Generating Japanese font test EPUB...")
    chapters = build_chapters()
    print(f"  {len(chapters)} chapters")
    write_epub("japanese_font_test.epub", chapters)
    print("Done!")
    print()
    print("Copy to sd/books/ to test on device or desktop emulator:")
    print("  copy test\\fixtures\\japanese_font_test.epub sd\\books\\")
