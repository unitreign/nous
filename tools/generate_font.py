#!/usr/bin/env python3
"""
Generate MBF (Microreader Bitmap Font) binary from a TTF/OTF font file.

Usage:
    python tools/generate_font.py <font.ttf> <size_px> -o <output.mbf> [--ranges ...]

Requires: freetype-py (pip install freetype-py)
"""

import argparse
import math
import os
import struct
import sys

try:
    import fontTools.ttLib
except ImportError:
    print("ERROR: fonttools not installed. Run: pip install fonttools")
    sys.exit(1)

# For PNG output
try:
    from PIL import Image
except ImportError:
    Image = None
    # PNG output will be skipped if Pillow is not installed

try:
    import freetype
except ImportError:
    print("ERROR: freetype-py not installed. Run: pip install freetype-py")
    sys.exit(1)

# ---------------------------------------------------------------------------
# Unicode range presets
# ---------------------------------------------------------------------------

RANGE_PRESETS = {
    "ascii": [(0x0020, 0x007E)],  # Basic ASCII (space through ~)
    "latin1": [(0x00A0, 0x00FF)],  # Latin-1 Supplement
    "latin-ext-a": [(0x0100, 0x017F)],  # Latin Extended-A
    "latin-ext-b": [(0x0180, 0x024F)],  # Latin Extended-B
    "cyrillic": [(0x0400, 0x04FF)],  # Cyrillic
    "hiragana": [(0x3040, 0x309F)],  # Hiragana
    "katakana": [(0x30A0, 0x30FF)],  # Katakana
    "cjk-punct": [(0x3000, 0x303F)],  # CJK Symbols and Punctuation (。「」、・ etc.)
    "halfwidth-katakana": [(0xFF65, 0xFF9F)],  # Halfwidth Katakana
    "cjk": [(0x4E00, 0x9FFF)],  # CJK Unified Ideographs (main block)
    "cjk-ext-a": [(0x3400, 0x4DBF)],  # CJK Extension A (rare/archaic kanji)
    "cjk-compat": [(0xF900, 0xFAFF)],  # CJK Compatibility Ideographs
    "cjk-enclosed": [
        (0x3200, 0x33FF)
    ],  # Enclosed CJK Letters + CJK Compatibility (㋿ ㎞ etc.)
    "fullwidth": [
        (0xFF01, 0xFF5E),
        (0xFFE0, 0xFFE6),
    ],  # Fullwidth Latin/digits + currency signs
    # Convenience bundle: everything needed for Japanese text
    "japanese": [
        (0x3000, 0x303F),  # CJK Symbols and Punctuation
        (0x3040, 0x30FF),  # Hiragana + Katakana
        (0x3200, 0x33FF),  # Enclosed CJK + CJK Compatibility
        (0x4E00, 0x9FFF),  # CJK Unified Ideographs
        (0xF900, 0xFAFF),  # CJK Compatibility Ideographs
        (0xFF01, 0xFF5E),  # Fullwidth Latin/digits
        (0xFF61, 0xFF9F),  # Halfwidth Katakana
        (0xFFE0, 0xFFE6),  # Fullwidth currency signs
        (0xFFFD, 0xFFFD),  # Replacement character (□ fallback for missing glyphs)
    ],
    "greek": [(0x0370, 0x03FF)],  # Greek and Coptic
    "general-punct": [
        (0x2000, 0x206F)
    ],  # General Punctuation (curly quotes, em-dash, ellipsis)
    "currency": [(0x20A0, 0x20CF)],  # Currency Symbols (€ etc.)
    "letterlike": [(0x2100, 0x214F)],  # Letterlike Symbols (™ ℃ etc.)
    "number-forms": [(0x2150, 0x218F)],  # Number Forms (fractions)
    "arrows": [(0x2190, 0x21FF)],  # Arrows
    "math-ops": [(0x2200, 0x22FF)],  # Mathematical Operators
    "geometric": [(0x25A0, 0x25FF)],  # Geometric Shapes
    "ui-shapes": [
        (0x25B2, 0x25B2),
        (0x25BC, 0x25BC),
        (0x25C0, 0x25C0),
        (0x25B6, 0x25B6),
    ],  # UI Button hints: ◀ ▶ ▼ ▲
    "misc-symbols": [(0x2600, 0x26FF)],  # Miscellaneous Symbols
    "combining": [(0x0300, 0x036F)],  # Combining Diacritical Marks
    "spacing-mod": [(0x02B0, 0x02FF)],  # Spacing Modifier Letters
    "latin-ext-add": [(0x1E00, 0x1EFF)],  # Latin Extended Additional (Vietnamese etc.)
    "super-sub": [(0x2070, 0x209F)],  # Superscripts and Subscripts
    "specials": [
        (0xFFF0, 0xFFFF)
    ],  # Specials (includes U+FFFD replacement character &#xFFFD;)
    # Convenience bundle: all scripts sensible for a multilingual UI font
    # (excludes large CJK blocks — those add tens of thousands of glyphs)
    "ui": [
        (0x0020, 0x007E),  # Basic ASCII
        (0x00A0, 0x00FF),  # Latin-1 Supplement (Western European + common symbols)
        (0x0100, 0x017F),  # Latin Extended-A (Central/Eastern European)
        (0x0180, 0x024F),  # Latin Extended-B (extra Latin letters)
        (0x02B0, 0x02FF),  # Spacing Modifier Letters
        (0x0300, 0x036F),  # Combining Diacritical Marks
        (0x0370, 0x03FF),  # Greek and Coptic
        (0x0400, 0x04FF),  # Cyrillic (Russian, Ukrainian, Bulgarian, Serbian, etc.)
        (0x0500, 0x052F),  # Cyrillic Supplement
        (0x1E00, 0x1EFF),  # Latin Extended Additional (Vietnamese, Welsh, etc.)
        (0x2000, 0x206F),  # General Punctuation (curly quotes, em-dash, ellipsis, etc.)
        (0x2070, 0x209F),  # Superscripts and Subscripts
        (0x20A0, 0x20CF),  # Currency Symbols (€, £, ¥, ₽, etc.)
        (0x2100, 0x214F),  # Letterlike Symbols (™, ©, ℃, etc.)
        (0x2150, 0x218F),  # Number Forms (fractions)
        (0x25B2, 0x25B2),  # UI shapes: ▲
        (0x25B6, 0x25B6),  # UI shapes: ▶
        (0x25BC, 0x25BC),  # UI shapes: ▼
        (0x25C0, 0x25C0),  # UI shapes: ◀
        (0xFFF0, 0xFFFF),  # Specials (U+FFFD replacement character)
    ],
}

DEFAULT_RANGES = [
    "ascii",
    "latin1",
    "latin-ext-a",
    "greek",
    "combining",
    "spacing-mod",
    "general-punct",
    "super-sub",
    "currency",
    "letterlike",
    "number-forms",
    "ui-shapes",
    "specials",
]

# ---------------------------------------------------------------------------
# FreeType glyph rasterization
# ---------------------------------------------------------------------------


def render_glyph(face, codepoint, size, use_hinted_advance=False, fallback_face=None):
    """Render a single glyph. Returns dict with metrics + BW/LSB/MSB bitmap bytes."""
    glyph_index = face.get_char_index(codepoint)
    active_face = face

    if glyph_index == 0:
        if fallback_face is not None:
            fallback_index = fallback_face.get_char_index(codepoint)
            if fallback_index != 0:
                glyph_index = fallback_index
                active_face = fallback_face
                active_face.set_pixel_sizes(0, size)

    if glyph_index == 0:
        return None  # glyph not in font

    if use_hinted_advance:
        active_face.load_glyph(glyph_index, freetype.FT_LOAD_DEFAULT)
        adv = active_face.glyph.advance.x
    else:
        # First, get the unhinted advance so we base our layout on true proportions
        active_face.load_glyph(
            glyph_index, freetype.FT_LOAD_NO_HINTING | freetype.FT_LOAD_NO_BITMAP
        )
        adv = active_face.glyph.advance.x

    x_advance = int(round(adv * 4.0 / 64.0))  # Scale by 4 for quarter-pixel precision

    # Then render the actual bitmap with hinting so it aligns nicely on the pixels
    active_face.load_glyph(glyph_index, freetype.FT_LOAD_RENDER)

    bitmap = active_face.glyph.bitmap
    width = bitmap.width
    height = bitmap.rows
    x_offset = active_face.glyph.bitmap_left
    y_offset = (
        -active_face.glyph.bitmap_top
    )  # bitmap_top = pixels above baseline (positive), negate for MBF convention

    if width == 0 or height == 0:
        return {
            "codepoint": codepoint,
            "bitmap_width": 0,
            "bitmap_height": 0,
            "x_offset": 0,
            "y_offset": 0,
            "x_advance": x_advance,
            "bw_bytes": b"",
            "lsb_bytes": b"",
            "msb_bytes": b"",
        }

    # Read FreeType buffer and convert to 0-255 grayscale values.
    # FreeType grayscale: 0=background, 255=ink.
    # Our convention: 0=black(ink), 255=white.
    ft_buffer = bitmap.buffer
    ft_pitch = abs(bitmap.pitch)

    # Handle both grayscale (8-bit) and mono (1-bit) pixel modes.
    pixel_mode = bitmap.pixel_mode
    pixels = []  # flat list of 0-255 values (0=ink, 255=white) in our convention
    for y in range(height):
        for x in range(width):
            if pixel_mode == 1:  # FT_PIXEL_MODE_MONO
                byte_idx = y * ft_pitch + (x >> 3)
                ft_bit = (ft_buffer[byte_idx] >> (7 - (x & 7))) & 1
                # FreeType mono: 1=ink, 0=background → invert to our convention
                pixels.append(0 if ft_bit else 255)
            else:  # FT_PIXEL_MODE_GRAY (8-bit)
                ft_val = ft_buffer[y * ft_pitch + x]
                # FreeType grayscale: 0=background, 255=ink → invert
                pixels.append(255 - ft_val)

    row_stride = math.ceil(width / 8)
    bw_out = bytearray(row_stride * height)
    lsb_out = bytearray(row_stride * height)
    msb_out = bytearray(row_stride * height)

    # Also build a 5-level grayscale image for PNG output (0=white, 1=light, 2=gray, 3=dark, 4=black)
    grayscale5 = [0] * (width * height)

    for y in range(height):
        for x in range(width):
            pixel = pixels[y * width + x]
            pixel = (
                pixel / 255
            ) ** 1.6 * 255  # adjust the gamma to make the 5-level quantization look better

            # BW plane: threshold at 154 (values < 154 are ink)
            # MBF polarity: 1=white, 0=black(ink)
            bw_bit = 1 if pixel >= 154 else 0

            # 5-level grayscale quantization for PNG:
            #   >=205 -> 0 (white)
            #   >=154 -> 1 (light gray)
            #   >=103 -> 2 (gray)
            #   >= 52 -> 3 (dark gray)
            #   <  52 -> 4 (black)
            if pixel >= 205:
                gray5 = 0
            elif pixel >= 154:
                gray5 = 1
            elif pixel >= 103:
                gray5 = 2
            elif pixel >= 52:
                gray5 = 3
            else:
                gray5 = 4
            grayscale5[y * width + x] = gray5

            # 2-bit grayscale quantization for MBF planes (use all 4 levels):
            if pixel >= 205:
                gray = 0  # white
            elif pixel >= 154:
                gray = 1  # light gray
            elif pixel >= 103:
                gray = 2  # gray
            elif pixel >= 52:
                gray = 3  # dark gray
            else:
                gray = 0  # black (map to darkest)

            byte_idx = y * row_stride + (x >> 3)
            bit_mask = 0x80 >> (x & 7)

            if bw_bit:
                bw_out[byte_idx] |= bit_mask
            if gray & 1:  # LSB
                lsb_out[byte_idx] |= bit_mask
            if gray & 2:  # MSB
                msb_out[byte_idx] |= bit_mask

    return {
        "codepoint": codepoint,
        "bitmap_width": width,
        "bitmap_height": height,
        "x_offset": x_offset,
        "y_offset": y_offset,
        "x_advance": x_advance,
        "bw_bytes": bytes(bw_out),
        "lsb_bytes": bytes(lsb_out),
        "msb_bytes": bytes(msb_out),
        "grayscale5": grayscale5,
    }


# ---------------------------------------------------------------------------
# MBF writer
# ---------------------------------------------------------------------------


def extract_kerning_fonttools(ttfont, scale):
    """
    Extracts kerning pairs from 'kern' and 'GPOS' tables using fonttools.
    Returns: dict mapping (left_ft_idx, right_ft_idx) -> px_kern
    """
    pairs = {}

    if "kern" in ttfont:
        for table in ttfont["kern"].kernTables:
            if hasattr(table, "kernTable"):
                for (l, r), value in table.kernTable.items():
                    pairs[(l, r)] = value

    if "GPOS" in ttfont:
        gpos = ttfont["GPOS"].table
        if getattr(gpos, "LookupList", None) is not None:
            all_glyphs = ttfont.getGlyphOrder()
            for lookup in gpos.LookupList.Lookup:
                subtables = []
                if lookup.LookupType == 2:
                    subtables = lookup.SubTable
                elif lookup.LookupType == 9:
                    for ext in getattr(lookup, "SubTable", []):
                        if getattr(ext, "ExtensionLookupType", 0) == 2:
                            if hasattr(ext, "ExtSubTable"):
                                subtables.append(ext.ExtSubTable)

                for sub in subtables:
                    if sub.Format == 1:
                        for p, pairSet in zip(sub.Coverage.glyphs, sub.PairSet):
                            for pairValue in pairSet.PairValueRecord:
                                r = pairValue.SecondGlyph
                                val = getattr(
                                    getattr(pairValue, "Value1", None), "XAdvance", 0
                                )
                                if val != 0:
                                    pairs[(p, r)] = val
                    elif sub.Format == 2:
                        classDef1 = sub.ClassDef1.classDefs if sub.ClassDef1 else {}
                        classDef2 = sub.ClassDef2.classDefs if sub.ClassDef2 else {}

                        c1_glyphs = {}
                        for glyph, c in classDef1.items():
                            c1_glyphs.setdefault(c, []).append(glyph)

                        c2_glyphs = {}
                        for glyph, c in classDef2.items():
                            c2_glyphs.setdefault(c, []).append(glyph)

                        for class1, rec1 in enumerate(sub.Class1Record):
                            if class1 not in c1_glyphs:
                                continue
                            for class2, rec2 in enumerate(rec1.Class2Record):
                                if class2 not in c2_glyphs:
                                    continue
                                val = getattr(
                                    getattr(rec2, "Value1", None), "XAdvance", 0
                                )
                                if val != 0:
                                    for l in c1_glyphs[class1]:
                                        for right in c2_glyphs[class2]:
                                            pairs[(l, right)] = val

    result = {}
    for (l_name, r_name), val in pairs.items():
        try:
            l_id = ttfont.getGlyphID(l_name)
            r_id = ttfont.getGlyphID(r_name)
            px_kern = int(
                round(val * scale * 4.0)
            )  # Scale by 4 for quarter-pixel precision
            if px_kern != 0:
                result[(l_id, r_id)] = px_kern
        except KeyError:
            continue

    return result


def render_style_glyphs(
    face_info, size, codepoint_ranges, use_hinted_advance=False, fallback_face=None
):
    """Render glyphs for one style. Returns (ranges, all_glyphs, bw_bitmaps, lsb_bitmaps, msb_bitmaps, max_glyph_height, kerning_pairs).

    ranges: list of (first_cp, count, glyph_table_start)
    all_glyphs: flat list of glyph dicts
    bw_bitmaps: bytearray of BW 1-bit bitmap data
    lsb_bitmaps: bytearray of grayscale LSB 1-bit bitmap data
    msb_bitmaps: bytearray of grayscale MSB 1-bit bitmap data
    kerning_pairs: bytearray of (uint16_t left, uint16_t right, int8_t x_offset)
    """
    face = face_info["face"]
    ttfont = face_info["ttfont"]

    face.set_pixel_sizes(0, size)
    if fallback_face:
        fallback_face.set_pixel_sizes(0, size)

    total_codepoints = sum(e - s + 1 for s, e in codepoint_ranges)
    print(f"  Rasterizing {total_codepoints} codepoints at {size}px...")

    ranges = []
    all_glyphs = []
    bw_bitmaps = bytearray()
    lsb_bitmaps = bytearray()
    msb_bitmaps = bytearray()
    max_glyph_height = 0
    rendered_count = 0
    last_progress = -1

    for range_start, range_end in codepoint_ranges:
        count = range_end - range_start + 1
        glyph_table_start = len(all_glyphs)
        has_any = False

        for cp in range(range_start, range_end + 1):
            rendered_count += 1
            pct = rendered_count * 100 // total_codepoints
            if pct != last_progress and pct % 5 == 0:
                print(
                    f"  Rendering glyphs... {pct}% ({rendered_count}/{total_codepoints})",
                    end="\r",
                    flush=True,
                )
                last_progress = pct
            g = render_glyph(face, cp, size, use_hinted_advance, fallback_face)
            if g is None:
                all_glyphs.append(
                    {
                        "codepoint": cp,
                        "bitmap_width": 0,
                        "bitmap_height": 0,
                        "x_offset": 0,
                        "y_offset": 0,
                        "x_advance": 0,
                        "bw_bytes": b"",
                        "lsb_bytes": b"",
                        "msb_bytes": b"",
                        "bitmap_offset": 0,
                    }
                )
            else:
                g["bitmap_offset"] = len(bw_bitmaps)
                bw_bitmaps.extend(g["bw_bytes"])
                lsb_bitmaps.extend(g["lsb_bytes"])
                msb_bitmaps.extend(g["msb_bytes"])
                all_glyphs.append(g)
                has_any = True
                if g["bitmap_height"] > max_glyph_height:
                    max_glyph_height = g["bitmap_height"]

        if has_any:
            ranges.append((range_start, count, glyph_table_start))

    if total_codepoints > 100:
        print(f"  Rendering glyphs... done ({rendered_count} codepoints)        ")

    kerning_mbf_bytes = bytearray()

    upem = ttfont["head"].unitsPerEm
    scale = size / upem
    print(f"  Extracting kerning...", end="\r", flush=True)
    fonttools_kern = extract_kerning_fonttools(ttfont, scale)

    if fonttools_kern:
        # Build reverse map: FreeType glyph index -> glyph table index
        ft_idx_to_glyph_idx = {}
        for g_idx, g in enumerate(all_glyphs):
            if g["bitmap_width"] == 0 and g["x_advance"] == 0:
                continue
            ft_idx = face.get_char_index(g["codepoint"])
            if ft_idx != 0:
                ft_idx_to_glyph_idx[ft_idx] = g_idx

        flat_pairs = {}
        for (l_ft_idx, r_ft_idx), px_kern in fonttools_kern.items():
            l_idx = ft_idx_to_glyph_idx.get(l_ft_idx, -1)
            r_idx = ft_idx_to_glyph_idx.get(r_ft_idx, -1)
            if l_idx >= 0 and r_idx >= 0:
                flat_pairs[(l_idx, r_idx)] = px_kern

        if flat_pairs:
            # Step 1: Compress Left Classes based on right kerning signatures
            l_profiles = {}
            for (l_idx, r_idx), val in flat_pairs.items():
                l_profiles.setdefault(l_idx, {})[r_idx] = val

            l_class_map = [0] * len(all_glyphs)
            l_class_list = [{}]
            l_profile_to_class = {frozenset(): 0}

            for l_idx, profile in l_profiles.items():
                fs = frozenset(profile.items())
                if fs not in l_profile_to_class:
                    new_class = len(l_class_list)
                    if new_class > 255:
                        new_class = 255
                    l_profile_to_class[fs] = new_class
                    if new_class == len(l_class_list):
                        l_class_list.append(profile)
                l_class_map[l_idx] = l_profile_to_class[fs]

            # Step 2: Compress Right Classes based on left kerning classes
            r_profiles = {}
            for (l_idx, r_idx), val in flat_pairs.items():
                lc = l_class_map[l_idx]
                if lc == 255:
                    continue
                r_profiles.setdefault(r_idx, {})[lc] = val

            r_class_map = [0] * len(all_glyphs)
            r_class_list = [{}]
            r_profile_to_class = {frozenset(): 0}

            for r_idx, profile in r_profiles.items():
                fs = frozenset(profile.items())
                if fs not in r_profile_to_class:
                    new_class = len(r_class_list)
                    if new_class > 255:
                        new_class = 255
                    r_profile_to_class[fs] = new_class
                    if new_class == len(r_class_list):
                        r_class_list.append(profile)
                r_class_map[r_idx] = r_profile_to_class[fs]

            num_l_classes = min(256, len(l_class_list))
            num_r_classes = min(256, len(r_class_list))

            # Step 3: Build 2D Kerning Matrix
            matrix = [0] * (num_l_classes * num_r_classes)
            for (l_idx, r_idx), val in flat_pairs.items():
                lc = l_class_map[l_idx]
                rc = r_class_map[r_idx]
                if lc < 256 and rc < 256:
                    matrix[lc * num_r_classes + rc] = val

            # Step 4: Pack MBF Format 3 Class Kerning
            kerning_mbf_bytes.append(num_l_classes - 1)  # 0-255 stores 1-256
            kerning_mbf_bytes.append(num_r_classes - 1)
            for cid in l_class_map:
                kerning_mbf_bytes.append(cid)
            for cid in r_class_map:
                kerning_mbf_bytes.append(cid)
            for val in matrix:
                kerning_mbf_bytes.append(max(-128, min(127, val)) & 0xFF)

            print(
                f"    Style kerning: {len(flat_pairs)} basic pairs -> {num_l_classes}x{num_r_classes} classes ({len(kerning_mbf_bytes)} bytes)"
            )
        else:
            print(f"    Style kerning pairs: 0")
    else:
        print(f"    Style kerning pairs: 0")

    return (
        ranges,
        all_glyphs,
        bw_bitmaps,
        lsb_bitmaps,
        msb_bitmaps,
        max_glyph_height,
        kerning_mbf_bytes,
    )


def _encode_ranges_and_glyphs(ranges, all_glyphs, bitmap_base_offset):
    """Encode range table + glyph table into bytes. bitmap_base_offset is added
    to each glyph's bitmap_offset so all offsets are relative to the start of
    the shared bitmap data section."""
    buf = bytearray()
    for first_cp, count, glyph_start in ranges:
        buf.extend(struct.pack("<IHH", first_cp, count, glyph_start))
    for g in all_glyphs:
        bmp_off = g.get("bitmap_offset", 0) + bitmap_base_offset
        buf.extend(
            struct.pack(
                "<IBBBbbB",
                bmp_off,
                g["x_advance"] & 0xFF,
                g["bitmap_width"] & 0xFF,
                g["bitmap_height"] & 0xFF,
                g["x_offset"],
                g["y_offset"],
                0,
            )
        )
    return buf


def build_mbf(face_info, size, codepoint_ranges, bw_only=False, fallback_face=None):
    """Build single-style MBF binary. Returns (bytes, stats_dict)."""
    (
        ranges,
        all_glyphs,
        bw_bitmaps,
        lsb_bitmaps,
        msb_bitmaps,
        max_glyph_height,
        kerning_bytes,
    ) = render_style_glyphs(
        face_info,
        size,
        codepoint_ranges,
        use_hinted_advance=bw_only,
        fallback_face=fallback_face,
    )

    if bw_only:
        lsb_bitmaps = bytearray()
        msb_bitmaps = bytearray()

    face = face_info["face"]
    face.set_pixel_sizes(0, size)
    ascender = face.size.ascender >> 6
    descender = face.size.descender >> 6
    y_advance = ascender - descender
    baseline = ascender
    default_advance = (size // 2) * 4  # Scale by 4 for quarter-pixel precision

    # Underline metrics from the font's post table (in font design units).
    em = face.units_per_EM or 1000
    ul_pos_raw = getattr(face, "underline_position", -100)
    ul_thick_raw = getattr(face, "underline_thickness", 50)
    underline_pos = max(1, round(-ul_pos_raw * size / em))  # positive = below baseline
    underline_thickness = max(1, round(ul_thick_raw * size / em))

    num_ranges = len(ranges)
    num_glyphs = len(all_glyphs)
    header_size = 50
    ranges_size = num_ranges * 8
    glyphs_size = num_glyphs * 10

    kerning_length = len(kerning_bytes)
    kerning_offset = header_size + ranges_size + glyphs_size
    bitmap_data_offset = kerning_offset + kerning_length

    bw_size = len(bw_bitmaps)
    gray_lsb_offset = (bitmap_data_offset + bw_size) if lsb_bitmaps else 0
    gray_msb_offset = (gray_lsb_offset + len(lsb_bitmaps)) if msb_bitmaps else 0

    buf = bytearray()
    buf.extend(
        struct.pack(
            "<IBBBBBBHHHIIIIIIIIbB",
            0x3446424D,  # MBF4
            4,  # version 4
            max_glyph_height & 0xFF,
            baseline & 0xFF,
            y_advance & 0xFF,
            default_advance & 0xFF,
            0,  # style_flags
            num_ranges,
            num_glyphs,
            size,  # nominal_size
            kerning_length,
            bitmap_data_offset,
            0,  # bold_offset
            0,  # italic_offset
            0,  # bold_italic_offset
            kerning_offset,
            gray_lsb_offset,
            gray_msb_offset,
            underline_pos,
            underline_thickness,
        )
    )
    buf.extend(_encode_ranges_and_glyphs(ranges, all_glyphs, 0))
    buf.extend(kerning_bytes)
    buf.extend(bw_bitmaps)
    buf.extend(lsb_bitmaps)
    buf.extend(msb_bitmaps)

    stats = {
        "num_ranges": num_ranges,
        "num_glyphs": num_glyphs,
        "rendered_glyphs": sum(
            1 for g in all_glyphs if g["bitmap_width"] > 0 or g["x_advance"] > 0
        ),
        "bitmap_bytes": bw_size + len(lsb_bitmaps) + len(msb_bitmaps),
        "total_bytes": len(buf),
        "glyph_height": max_glyph_height,
        "baseline": baseline,
        "y_advance": y_advance,
    }
    return bytes(buf), stats


def build_multi_style_mbf(
    faces, size, codepoint_ranges, bw_only=False, fallback_face=None
):
    """Build multi-style MBF from dict of {FontStyle: face_info}.

    faces keys: 'regular' (required), 'bold', 'italic', 'bold_italic' (optional).
    Returns (bytes, stats_dict).
    """
    # Render all styles — now returns 6-tuples with 3 bitmap sections
    style_data = {}
    for style_name, face_info in faces.items():
        style_data[style_name] = render_style_glyphs(
            face_info,
            size,
            codepoint_ranges,
            use_hinted_advance=bw_only,
            fallback_face=fallback_face,
        )

    if bw_only:
        # Strip grayscale planes from all styles
        for key in style_data:
            r, g, bw, lsb, msb, h, k = style_data[key]
            style_data[key] = (r, g, bw, bytearray(), bytearray(), h, k)

    # Metrics from Regular face
    reg_face = faces["regular"]["face"]
    reg_face.set_pixel_sizes(0, size)
    ascender = reg_face.size.ascender >> 6
    descender = reg_face.size.descender >> 6
    y_advance = ascender - descender
    baseline = ascender
    default_advance = (size // 2) * 4  # Scale by 4 for quarter-pixel precision

    # Underline metrics from the font's post table (in font design units).
    em = reg_face.units_per_EM or 1000
    ul_pos_raw = getattr(reg_face, "underline_position", -100)
    ul_thick_raw = getattr(reg_face, "underline_thickness", 50)
    underline_pos = max(1, round(-ul_pos_raw * size / em))  # positive = below baseline
    underline_thickness = max(1, round(ul_thick_raw * size / em))

    max_glyph_height = max(d[5] for d in style_data.values())

    # Compute layout sizes
    reg = style_data["regular"]
    reg_ranges, reg_glyphs, reg_bw, reg_lsb, reg_msb, _, reg_kerning = reg

    header_size = 50
    reg_ranges_size = len(reg_ranges) * 8
    reg_glyphs_size = len(reg_glyphs) * 10

    # After regular ranges+glyphs, append regular kerning, then extra styles
    cursor = header_size + reg_ranges_size + reg_glyphs_size + len(reg_kerning)

    extra_styles = ["bold", "italic", "bold_italic"]
    style_offsets = {}  # style_name -> file offset
    style_sections = (
        {}
    )  # style_name -> encoded bytes (section header + ranges + glyphs + kerning)

    for sname in extra_styles:
        if sname not in style_data:
            style_offsets[sname] = 0
            continue
        ranges, glyphs, bw, lsb, msb, _, kerning = style_data[sname]
        style_offsets[sname] = cursor

        kerning_length = len(kerning)
        kerning_offset = cursor + 8 + len(ranges) * 8 + len(glyphs) * 10

        # MbfStyleSection: uint16 num_ranges, uint16 num_glyphs, uint32 kerning_length
        sec = struct.pack("<HHI", len(ranges), len(glyphs), kerning_length)
        # Ranges + glyphs (bitmap offsets will be fixed up later)
        style_sections[sname] = (sec, ranges, glyphs, kerning)
        cursor = kerning_offset + len(kerning)

    bitmap_data_offset = cursor

    # Now we know where bitmaps start — concatenate all BW bitmaps and compute
    # per-style bitmap base offsets
    all_bw = bytearray()
    all_lsb = bytearray()
    all_msb = bytearray()
    bitmap_bases = {}

    bitmap_bases["regular"] = len(all_bw)
    all_bw.extend(reg_bw)
    all_lsb.extend(reg_lsb)
    all_msb.extend(reg_msb)

    for sname in extra_styles:
        if sname not in style_data:
            continue
        bitmap_bases[sname] = len(all_bw)
        all_bw.extend(style_data[sname][2])
        all_lsb.extend(style_data[sname][3])
        all_msb.extend(style_data[sname][4])

    gray_lsb_offset = (bitmap_data_offset + len(all_bw)) if all_lsb else 0
    gray_msb_offset = (gray_lsb_offset + len(all_lsb)) if all_msb else 0

    # style_flags bitmask
    style_flags = 0x01  # Regular always
    if "bold" in style_data:
        style_flags |= 0x02
    if "italic" in style_data:
        style_flags |= 0x04
    if "bold_italic" in style_data:
        style_flags |= 0x08

    # Build file
    # Extra section offsets / counts
    kerning_length = len(reg_kerning)
    kerning_offset = header_size + reg_ranges_size + reg_glyphs_size

    buf = bytearray()
    buf.extend(
        struct.pack(
            "<IBBBBBBHHHIIIIIIIIbB",
            0x3446424D,  # MBF4
            4,  # version 4
            max_glyph_height & 0xFF,
            baseline & 0xFF,
            y_advance & 0xFF,
            default_advance & 0xFF,
            style_flags,
            len(reg_ranges),
            len(reg_glyphs),
            size,  # nominal_size
            kerning_length,
            bitmap_data_offset,
            style_offsets.get("bold", 0),
            style_offsets.get("italic", 0),
            style_offsets.get("bold_italic", 0),
            kerning_offset,
            gray_lsb_offset,
            gray_msb_offset,
            underline_pos,
            underline_thickness,
        )
    )

    # Regular ranges + glyphs
    buf.extend(
        _encode_ranges_and_glyphs(reg_ranges, reg_glyphs, bitmap_bases["regular"])
    )
    buf.extend(reg_kerning)

    # Extra style sections
    for sname in extra_styles:
        if sname not in style_sections:
            continue
        sec_header, ranges, glyphs, kerning = style_sections[sname]
        buf.extend(sec_header)
        buf.extend(_encode_ranges_and_glyphs(ranges, glyphs, bitmap_bases[sname]))
        buf.extend(kerning)

    # Bitmap data: BW, then LSB, then MSB
    buf.extend(all_bw)
    buf.extend(all_lsb)
    buf.extend(all_msb)

    total_rendered = sum(
        sum(1 for g in d[1] if g["bitmap_width"] > 0 or g["x_advance"] > 0)
        for d in style_data.values()
    )
    total_glyphs = sum(len(d[1]) for d in style_data.values())

    stats = {
        "num_styles": len(style_data),
        "style_names": list(style_data.keys()),
        "num_ranges": len(reg_ranges),
        "num_glyphs": total_glyphs,
        "rendered_glyphs": total_rendered,
        "bitmap_bytes": len(all_bw) + len(all_lsb) + len(all_msb),
        "total_bytes": len(buf),
        "glyph_height": max_glyph_height,
        "baseline": baseline,
        "y_advance": y_advance,
    }
    return bytes(buf), stats


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def parse_ranges(range_strs):
    """Parse range specifications into (start, end) tuples."""
    result = []
    for s in range_strs:
        s = s.strip().lower()
        if s in RANGE_PRESETS:
            result.extend(RANGE_PRESETS[s])
        elif "-" in s and s.startswith("0x"):
            # Hex range: 0x0020-0x007E
            parts = s.split("-", 1)
            result.append((int(parts[0], 16), int(parts[1], 16)))
        elif s.startswith("0x"):
            # Single codepoint
            cp = int(s, 16)
            result.append((cp, cp))
        else:
            print(f"WARNING: unknown range '{s}', skipping")
    # Sort and merge overlapping ranges
    result.sort()
    merged = []
    for start, end in result:
        if merged and start <= merged[-1][1] + 1:
            merged[-1] = (merged[-1][0], max(merged[-1][1], end))
        else:
            merged.append((start, end))
    return merged


def main():
    p = argparse.ArgumentParser(description="Generate MBF bitmap font from TTF/OTF")
    p.add_argument("font", nargs="?", help="Path to TTF/OTF font file (Regular)")
    p.add_argument(
        "size", nargs="?", type=int, default=20, help="Pixel size (default: 20)"
    )
    p.add_argument("-o", "--output", required=True, help="Output .mbf file path")
    p.add_argument(
        "--ranges",
        nargs="+",
        default=DEFAULT_RANGES,
        help=(
            f"Unicode ranges (presets: {', '.join(RANGE_PRESETS.keys())}; "
            "or hex: 0x0020-0x007E)"
        ),
    )
    p.add_argument("--header", help="Also output C++ header with embedded font data")
    p.add_argument("--bold", help="Path to Bold TTF/OTF font file")
    p.add_argument("--italic", help="Path to Italic TTF/OTF font file")
    p.add_argument("--bold-italic", help="Path to BoldItalic TTF/OTF font file")
    p.add_argument(
        "--fallback",
        default="resources/fonts/bookerly/Bookerly.ttf",
        help="Fallback TTF font file to use for missing glyphs (e.g. U+FFFD). Default: resources/fonts/bookerly/Bookerly.ttf",
    )
    p.add_argument(
        "--with-styles",
        action="store_true",
        help="Enable multi-style generation (bold, italic, bold-italic) using explicit files.",
    )
    p.add_argument(
        "--bundle",
        action="store_true",
        help="Generate combined FNTS bundle (.mfb)",
    )
    p.add_argument(
        "--bundle-sizes",
        type=int,
        nargs="+",
        default=[20, 24, 28, 32],
        help="Font sizes for --bundle mode (default: 20 24 28 32). "
        "The second value is 'Normal' (default body text).",
    )
    p.add_argument(
        "--keep-mbf",
        action="store_true",
        help="In bundle mode, also output the individual per-size .mbf files",
    )
    p.add_argument(
        "--bw-only",
        action="store_true",
        help="Generate BW-only font (no grayscale planes). Use for UI fonts.",
    )
    p.add_argument(
        "--font-name",
        help="Font family name to embed in the FNTS bundle header "
        "(default: derived from font filename, e.g. 'Bookerly').",
    )
    args = p.parse_args()

    # Resolve font paths
    tools_dir = os.path.dirname(os.path.abspath(__file__))
    fonts_cache = os.path.join(tools_dir, ".font-cache")

    if args.font:
        font_path = args.font
    else:
        print("ERROR: No font specified.")
        sys.exit(1)

    if not os.path.isfile(font_path):
        print(f"ERROR: font file not found: {font_path}")
        sys.exit(1)

    # Resolve style font paths
    bold_path = args.bold
    italic_path = args.italic
    bold_italic_path = args.bold_italic

    # Parse ranges
    codepoint_ranges = parse_ranges(args.ranges)
    total_codepoints = sum(e - s + 1 for s, e in codepoint_ranges)
    print(f"Font: {os.path.basename(font_path)}")
    print(f"Size: {args.size}px")
    print(f"Ranges: {len(codepoint_ranges)} blocks, {total_codepoints} codepoints")

    # Check if multi-style
    has_extra_styles = bold_path or italic_path or bold_italic_path

    if has_extra_styles:
        faces = {
            "regular": {
                "face": freetype.Face(font_path),
                "ttfont": fontTools.ttLib.TTFont(font_path),
            }
        }
        if bold_path and os.path.isfile(bold_path):
            faces["bold"] = {
                "face": freetype.Face(bold_path),
                "ttfont": fontTools.ttLib.TTFont(bold_path),
            }
            print(f"Bold: {os.path.basename(bold_path)}")
        if italic_path and os.path.isfile(italic_path):
            faces["italic"] = {
                "face": freetype.Face(italic_path),
                "ttfont": fontTools.ttLib.TTFont(italic_path),
            }
            print(f"Italic: {os.path.basename(italic_path)}")
        if bold_italic_path and os.path.isfile(bold_italic_path):
            faces["bold_italic"] = {
                "face": freetype.Face(bold_italic_path),
                "ttfont": fontTools.ttLib.TTFont(bold_italic_path),
            }
            print(f"BoldItalic: {os.path.basename(bold_italic_path)}")

        fallback_face = freetype.Face(args.fallback) if args.fallback else None

        if args.bundle:
            _generate_bundle(
                faces,
                args,
                codepoint_ranges,
                multi_style=True,
                fallback_face=fallback_face,
            )
            return

        mbf_data, stats = build_multi_style_mbf(
            faces,
            args.size,
            codepoint_ranges,
            bw_only=args.bw_only,
            fallback_face=fallback_face,
        )
    else:
        face_info = {
            "face": freetype.Face(font_path),
            "ttfont": fontTools.ttLib.TTFont(font_path),
        }

        fallback_face = freetype.Face(args.fallback) if args.fallback else None

        if args.bundle:
            _generate_bundle(
                {"regular": face_info},
                args,
                codepoint_ranges,
                multi_style=False,
                fallback_face=fallback_face,
            )
            return

        mbf_data, stats = build_mbf(
            face_info,
            args.size,
            codepoint_ranges,
            bw_only=args.bw_only,
            fallback_face=fallback_face,
        )

    # Write output
    os.makedirs(os.path.dirname(os.path.abspath(args.output)) or ".", exist_ok=True)
    with open(args.output, "wb") as f:
        f.write(mbf_data)

    # Stats
    print(f"\nGenerated: {args.output}")
    if "num_styles" in stats:
        print(f"  Styles:   {stats['num_styles']} ({', '.join(stats['style_names'])})")
    print(f"  Ranges:   {stats['num_ranges']}")
    print(
        f"  Glyphs:   {stats['num_glyphs']} total, {stats['rendered_glyphs']} rendered"
    )
    print(f"  Height:   {stats['glyph_height']}px max, baseline={stats['baseline']}px")
    print(f"  Y-advance: {stats['y_advance']}px")
    print(f"  Bitmaps:  {stats['bitmap_bytes']} bytes")
    print(
        f"  Total:    {stats['total_bytes']} bytes ({stats['total_bytes']/1024:.1f} KB)"
    )

    # Write PNGs for a few sample characters (A, B, C, a, b, c, 0, 1, 2)
    if Image is not None:
        try:
            sample_chars = ["A", "B", "C", "a", "b", "c", "0", "1", "2"]
            face = freetype.Face(font_path)
            face.set_pixel_sizes(0, args.size)
            out_dir = os.path.dirname(os.path.abspath(args.output)) or "."
            glyph_imgs = []
            glyph_sizes = []
            palette = [255, 200, 140, 80, 0]
            max_h = 0
            for ch in sample_chars:
                cp = ord(ch)
                g = render_glyph(face, cp, args.size, use_hinted_advance=args.bw_only)
                if g is None or g["bitmap_width"] == 0 or g["bitmap_height"] == 0:
                    glyph_imgs.append(None)
                    glyph_sizes.append((0, 0))
                    continue
                arr = g["grayscale5"]
                img = Image.new("L", (g["bitmap_width"], g["bitmap_height"]))
                img.putdata([palette[v] for v in arr])
                glyph_imgs.append(img)
                glyph_sizes.append((g["bitmap_width"], g["bitmap_height"]))
                if g["bitmap_height"] > max_h:
                    max_h = g["bitmap_height"]

            pad = 4
            total_w = sum(w for w, h in glyph_sizes) + pad * (len(sample_chars) + 1)
            total_h = max_h
            if total_h > 0 and total_w > 0:
                out_img = Image.new("L", (total_w, total_h), 255)
                x = pad
                for i in range(len(sample_chars)):
                    img = glyph_imgs[i]
                    w, h = glyph_sizes[i]
                    if img is not None:
                        y = (max_h - h) // 2
                        out_img.paste(img, (x, y))
                    x += w + pad
                png_path = os.path.join(out_dir, "sample_all_gray5.png")
                out_img.save(png_path)
                print(f"  Sample PNG: {png_path}")
        except Exception as e:
            print(f"  [PNG sample error: {e}]")
    else:
        print("  [Pillow not installed: skipping PNG samples]")

    # Optional C++ header
    if args.header:
        _write_cpp_header(args.header, mbf_data, os.path.basename(args.output))
        print(f"  Header:   {args.header}")


def _generate_bundle(faces, args, codepoint_ranges, multi_style, fallback_face=None):
    """Generate Small/Normal/Large .mbf files and a combined FNTS .mfb bundle."""
    out_dir = os.path.dirname(os.path.abspath(args.output)) or "."
    os.makedirs(out_dir, exist_ok=True)

    sizes = list(enumerate(args.bundle_sizes))

    mbf_files = []  # (label, px_size, mbf_bytes, stats)

    for idx, px_size in sizes:
        label = f"Size{idx}"
        print(f"\n{'='*40}")
        print(f"  {label} ({px_size}px)")
        print(f"{'='*40}")
        if multi_style:
            data, stats = build_multi_style_mbf(
                faces,
                px_size,
                codepoint_ranges,
                bw_only=args.bw_only,
                fallback_face=fallback_face,
            )
            face = faces["regular"]["face"]
        else:
            face_info = faces["regular"]
            face = face_info["face"]
            data, stats = build_mbf(
                face_info,
                px_size,
                codepoint_ranges,
                bw_only=args.bw_only,
                fallback_face=fallback_face,
            )

        out_path = os.path.join(out_dir, f"font-{idx}.mbf")
        if args.keep_mbf:
            with open(out_path, "wb") as f:
                f.write(data)

        kb = len(data) / 1024
        print(f"  Glyphs:   {stats['rendered_glyphs']} rendered")
        print(f"  Height:   {stats['glyph_height']}px, baseline={stats['baseline']}px")
        if args.keep_mbf:
            print(f"  File:     {out_path} ({kb:.1f} KB)")
        mbf_files.append((label, px_size, data, stats))

        # Write a single PNG with all A-Z, a-z, 0-9, ä, Ä for this size, no labels
        if Image is not None and args.keep_mbf:
            try:
                # Build sample character list: A-Z, a-z, 0-9, ä, Ä
                sample_chars = [chr(c) for c in range(ord("A"), ord("Z") + 1)]
                sample_chars += [chr(c) for c in range(ord("a"), ord("z") + 1)]
                sample_chars += [chr(c) for c in range(ord("0"), ord("9") + 1)]
                sample_chars += ["ä", "Ä"]
                face.set_pixel_sizes(0, px_size)
                glyph_imgs = []
                glyph_sizes = []
                bw_imgs = []
                lsb_imgs = []
                msb_imgs = []
                palette = [255, 200, 140, 80, 0]
                max_h = 0
                for ch in sample_chars:
                    cp = ord(ch)
                    g = render_glyph(face, cp, px_size, use_hinted_advance=args.bw_only)
                    if g is None or g["bitmap_width"] == 0 or g["bitmap_height"] == 0:
                        glyph_imgs.append(None)
                        glyph_sizes.append((0, 0))
                        bw_imgs.append(None)
                        lsb_imgs.append(None)
                        msb_imgs.append(None)
                        continue
                    arr = g["grayscale5"]
                    img = Image.new("L", (g["bitmap_width"], g["bitmap_height"]))
                    img.putdata([palette[v] for v in arr])
                    glyph_imgs.append(img)
                    glyph_sizes.append((g["bitmap_width"], g["bitmap_height"]))

                    # BW, LSB, MSB bitmaps
                    # Unpack bits to 0/255 images
                    def unpack_bitmap(bits, w, h):
                        row_stride = math.ceil(w / 8)
                        arr = []
                        for y in range(h):
                            for x in range(w):
                                byte_idx = y * row_stride + (x >> 3)
                                bit_mask = 0x80 >> (x & 7)
                                v = 255 if (bits[byte_idx] & bit_mask) else 0
                                arr.append(v)
                        return arr

                    bw_arr = unpack_bitmap(
                        g["bw_bytes"], g["bitmap_width"], g["bitmap_height"]
                    )
                    lsb_arr = unpack_bitmap(
                        g["lsb_bytes"], g["bitmap_width"], g["bitmap_height"]
                    )
                    msb_arr = unpack_bitmap(
                        g["msb_bytes"], g["bitmap_width"], g["bitmap_height"]
                    )
                    bw_img = Image.new("L", (g["bitmap_width"], g["bitmap_height"]))
                    lsb_img = Image.new("L", (g["bitmap_width"], g["bitmap_height"]))
                    msb_img = Image.new("L", (g["bitmap_width"], g["bitmap_height"]))
                    bw_img.putdata(bw_arr)
                    lsb_img.putdata(lsb_arr)
                    msb_img.putdata(msb_arr)
                    bw_imgs.append(bw_img)
                    lsb_imgs.append(lsb_img)
                    msb_imgs.append(msb_img)
                    if g["bitmap_height"] > max_h:
                        max_h = g["bitmap_height"]

                # Compose into one image, with 4px padding between glyphs
                pad = 4
                total_w = sum(w for w, h in glyph_sizes) + pad * (len(sample_chars) + 1)
                total_h = max_h
                out_img = Image.new("L", (total_w, total_h), 255)
                bw_out_img = Image.new("L", (total_w, total_h), 255)
                lsb_out_img = Image.new("L", (total_w, total_h), 255)
                msb_out_img = Image.new("L", (total_w, total_h), 255)

                # Draw glyphs
                x = pad
                for i in range(len(sample_chars)):
                    img = glyph_imgs[i]
                    bw_img = bw_imgs[i]
                    lsb_img = lsb_imgs[i]
                    msb_img = msb_imgs[i]
                    w, h = glyph_sizes[i]
                    if img is not None:
                        y = (max_h - h) // 2
                        out_img.paste(img, (x, y))
                        bw_out_img.paste(bw_img, (x, y))
                        lsb_out_img.paste(lsb_img, (x, y))
                        msb_out_img.paste(msb_img, (x, y))
                    x += w + pad

                png_path = os.path.join(
                    out_dir, f"sample_all_{label.lower()}_gray5.png"
                )
                out_img.save(png_path)
                print(f"  Sample PNG: {png_path}")
            except Exception as e:
                print(f"  [PNG sample error: {e}]")
        else:
            print("  [Pillow not installed: skipping PNG samples]")

    # Derive font name: --font-name, or stem of the font file path.
    if args.font_name:
        font_name = args.font_name
    else:
        font_name = os.path.splitext(os.path.basename(args.font or "font"))[0]
        # Strip style suffixes like "-Regular", " Regular" etc.
        for suffix in ["-Regular", " Regular", "-regular"]:
            if font_name.endswith(suffix):
                font_name = font_name[: -len(suffix)]
                break

    # Build FNTS bundle (version 2):
    #   [4 bytes] "FNTS" magic
    #   [1 byte]  num_fonts
    #   [1 byte]  version (2)
    #   [2 bytes] reserved
    #   [32 bytes] font_name (null-terminated, zero-padded)
    #   [num × 4 bytes] sizes (uint32 LE)
    #   [font data concatenated]
    num = len(mbf_files)
    bundle = bytearray()
    bundle.extend(b"FNTS")
    bundle.append(num)
    bundle.append(2)  # version
    bundle.extend(bytes(2))  # reserved
    name_bytes = font_name.encode("utf-8")[:31]  # max 31 chars + null
    bundle.extend(name_bytes)
    bundle.extend(bytes(32 - len(name_bytes)))  # pad to 32 bytes
    for _, _, data, _ in mbf_files:
        bundle.extend(struct.pack("<I", len(data)))
    for _, _, data, _ in mbf_files:
        bundle.extend(data)

    bundle_path = args.output
    if not bundle_path.endswith(".mfb"):
        bundle_path = os.path.splitext(bundle_path)[0] + ".mfb"

    with open(bundle_path, "wb") as f:
        f.write(bundle)

    # Also write the zlib-compressed version for firmware embedding.
    # Format: [uncompressed_size:4 LE] [zlib_compressed_data...]
    # The leading uint32 lets provision_embedded() erase exactly the right
    # number of flash blocks without over-estimating.
    import zlib as _zlib

    compressed = _zlib.compress(bytes(bundle), level=9)
    bin_path = os.path.splitext(bundle_path)[0] + ".bin"
    with open(bin_path, "wb") as f:
        f.write(struct.pack("<I", len(bundle)))  # uncompressed size prefix
        f.write(compressed)

    print(f"\n{'='*40}")
    print(f"  FNTS Bundle")
    print(f"{'='*40}")
    print(f"  File:   {bundle_path} ({len(bundle)/1024:.1f} KB)")
    print(f"  Compressed: {bin_path} ({len(compressed)/1024:.1f} KB)")
    for label, px_size, data, stats in mbf_files:
        print(
            f"  {label:7s} ({px_size:2d}px): {len(data)/1024:6.1f} KB, "
            f"{stats['rendered_glyphs']} glyphs"
        )
    print(f"\nUpload to device:")
    print(f"  python tools/serial_cmd.py --upload-font {bundle_path}")


def _write_cpp_header(path, data, name):
    """Write a C++ header embedding the font data as a constexpr array."""
    os.makedirs(os.path.dirname(os.path.abspath(path)) or ".", exist_ok=True)
    var_name = "kFontData_" + name.replace(".", "_").replace("-", "_")
    with open(path, "w") as f:
        f.write("#pragma once\n\n")
        f.write("#include <cstddef>\n")
        f.write("#include <cstdint>\n\n")
        f.write(f"// Auto-generated from {name}\n")
        f.write(f"// Size: {len(data)} bytes\n\n")
        f.write(f"alignas(4) static constexpr uint8_t {var_name}[] = {{\n")
        for i in range(0, len(data), 16):
            chunk = data[i : i + 16]
            hex_str = ", ".join(f"0x{b:02X}" for b in chunk)
            f.write(f"    {hex_str},\n")
        f.write("};\n")
        f.write(f"static constexpr size_t {var_name}_size = sizeof({var_name});\n")


if __name__ == "__main__":
    main()
