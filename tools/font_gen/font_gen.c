/*
 * font_gen.c — MBF4 bitmap font generator
 *
 * Drop-in replacement for tools/generate_font.py. Uses FreeType for glyph
 * rendering and HarfBuzz for raw table access (kern + GPOS). Produces
 * bit-for-bit identical MBF4 / FNTS output for the same inputs.
 *
 * Usage (see usage() below):
 *   font_gen <regular.ttf> [options]
 */

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include <hb.h>

/* ── Portability ─────────────────────────────────────────────────────────── */

#ifdef _WIN32
#  define PATH_SEP '\\'
#else
#  define PATH_SEP '/'
#endif

/* ── OpenType big-endian readers ─────────────────────────────────────────── */

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}
static int16_t rd16s(const uint8_t *p) { return (int16_t)rd16(p); }
static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

/* ── MBF output little-endian writers ────────────────────────────────────── */

static void wr16(uint8_t *p, uint16_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static void wr32(uint8_t *p, uint32_t v) {
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24);
}

/* ── Dynamic byte buffer ──────────────────────────────────────────────────── */

typedef struct { uint8_t *data; uint32_t size, cap; } Buf;

static void buf_init(Buf *b)  { b->data=NULL; b->size=b->cap=0; }
static void buf_free(Buf *b)  { free(b->data); buf_init(b); }

static void buf_need(Buf *b, uint32_t extra) {
    uint32_t need = b->size + extra;
    if (need <= b->cap) return;
    uint32_t nc = b->cap ? b->cap*2 : 4096;
    while (nc < need) nc *= 2;
    b->data = (uint8_t*)realloc(b->data, nc);
    if (!b->data) { fputs("OOM\n", stderr); exit(1); }
    b->cap = nc;
}
static void buf_append(Buf *b, const void *src, uint32_t n) {
    buf_need(b, n);
    memcpy(b->data + b->size, src, n);
    b->size += n;
}
static void buf_u8(Buf *b, uint8_t v)  { buf_append(b, &v, 1); }
static void buf_i8(Buf *b, int8_t v)   { buf_u8(b, (uint8_t)v); }
static void buf_u16le(Buf *b, uint16_t v) {
    uint8_t t[2] = { (uint8_t)v, (uint8_t)(v>>8) };
    buf_append(b, t, 2);
}
static void buf_u32le(Buf *b, uint32_t v) {
    uint8_t t[4] = { (uint8_t)v,(uint8_t)(v>>8),(uint8_t)(v>>16),(uint8_t)(v>>24) };
    buf_append(b, t, 4);
}
static void buf_i32le(Buf *b, int32_t v) { buf_u32le(b, (uint32_t)v); }
static void buf_zero(Buf *b, uint32_t n) {
    buf_need(b, n);
    memset(b->data + b->size, 0, n);
    b->size += n;
}

/* ── File I/O ─────────────────────────────────────────────────────────────── */

static uint8_t *read_file(const char *path, uint32_t *size_out) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Cannot open: %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = (uint8_t*)malloc(sz);
    if (!data || (long)fread(data, 1, sz, f) != sz) {
        fprintf(stderr, "Read error: %s\n", path); free(data); fclose(f); return NULL;
    }
    fclose(f);
    *size_out = (uint32_t)sz;
    return data;
}

static int write_file(const char *path, const uint8_t *data, uint32_t size) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "Cannot write: %s\n", path); return 0; }
    fwrite(data, 1, size, f);
    fclose(f);
    return 1;
}

/* ── Glyph ───────────────────────────────────────────────────────────────── */

typedef struct {
    uint32_t codepoint;
    uint8_t  bmp_w, bmp_h;
    int8_t   x_off, y_off;
    uint8_t  x_adv;      /* quarter-pixel units */
    uint32_t bmp_off;    /* byte offset into style's BW section */
    uint8_t *pixels;     /* bmp_w*bmp_h, 0=ink 255=white, NULL if no bitmap */
} Glyph;

/* ── Range entry ─────────────────────────────────────────────────────────── */

typedef struct { uint32_t first_cp; uint16_t count, glyph_start; } Range;

/* ── Unicode range presets (mirrors generate_font.py) ──────────────────── */

typedef struct { uint32_t lo, hi; } CPRange;

#define PRESET(name, ...) \
    static const CPRange preset_##name[] = { __VA_ARGS__ }; \
    static const int preset_##name##_count = \
        (int)(sizeof(preset_##name)/sizeof(preset_##name[0]));

PRESET(ascii,         {0x0020,0x007E})
PRESET(latin1,        {0x00A0,0x00FF})
PRESET(latin_ext_a,   {0x0100,0x017F})
PRESET(latin_ext_b,   {0x0180,0x024F})
PRESET(latin_ext_add, {0x1E00,0x1EFF})
PRESET(combining,     {0x0300,0x036F})
PRESET(spacing_mod,   {0x02B0,0x02FF})
PRESET(greek,         {0x0370,0x03FF})
PRESET(cyrillic,      {0x0400,0x04FF})
PRESET(general_punct, {0x2000,0x206F})
PRESET(super_sub,     {0x2070,0x209F})
PRESET(currency,      {0x20A0,0x20CF})
PRESET(letterlike,    {0x2100,0x214F})
PRESET(number_forms,  {0x2150,0x218F})
PRESET(arrows,        {0x2190,0x21FF})
PRESET(ui_shapes,     {0x25B2,0x25B2},{0x25BC,0x25BC},{0x25C0,0x25C0},{0x25B6,0x25B6})
PRESET(specials,      {0xFFF0,0xFFFF})
PRESET(hiragana,      {0x3040,0x309F})
PRESET(katakana,      {0x30A0,0x30FF})
PRESET(cjk_punct,     {0x3000,0x303F})
PRESET(cjk,           {0x4E00,0x9FFF})

typedef struct { const char *name; const CPRange *ranges; int count; } Preset;
static const Preset kPresets[] = {
    {"ascii",         preset_ascii,         preset_ascii_count},
    {"latin1",        preset_latin1,        preset_latin1_count},
    {"latin-ext-a",   preset_latin_ext_a,   preset_latin_ext_a_count},
    {"latin-ext-b",   preset_latin_ext_b,   preset_latin_ext_b_count},
    {"latin-ext-add", preset_latin_ext_add, preset_latin_ext_add_count},
    {"combining",     preset_combining,     preset_combining_count},
    {"spacing-mod",   preset_spacing_mod,   preset_spacing_mod_count},
    {"greek",         preset_greek,         preset_greek_count},
    {"cyrillic",      preset_cyrillic,      preset_cyrillic_count},
    {"general-punct", preset_general_punct, preset_general_punct_count},
    {"super-sub",     preset_super_sub,     preset_super_sub_count},
    {"currency",      preset_currency,      preset_currency_count},
    {"letterlike",    preset_letterlike,    preset_letterlike_count},
    {"number-forms",  preset_number_forms,  preset_number_forms_count},
    {"arrows",        preset_arrows,        preset_arrows_count},
    {"ui-shapes",     preset_ui_shapes,     preset_ui_shapes_count},
    {"specials",      preset_specials,      preset_specials_count},
    {"hiragana",      preset_hiragana,      preset_hiragana_count},
    {"katakana",      preset_katakana,      preset_katakana_count},
    {"cjk-punct",     preset_cjk_punct,     preset_cjk_punct_count},
    {"cjk",           preset_cjk,           preset_cjk_count},
};
static const int kNumPresets = (int)(sizeof(kPresets)/sizeof(kPresets[0]));

static const char *kDefaultRanges[] = {
    "ascii","latin1","latin-ext-a","greek","combining","spacing-mod",
    "general-punct","super-sub","currency","letterlike","number-forms",
    "ui-shapes","specials",
    NULL
};

/* Merge and sort CP ranges, removing duplicates. */
static CPRange *build_ranges(const char **names, int num_names, int *out_count) {
    CPRange *buf = NULL; int count = 0, cap = 0;
    for (int ni = 0; ni < num_names; ni++) {
        const char *name = names[ni];
        for (int pi = 0; pi < kNumPresets; pi++) {
            if (strcmp(kPresets[pi].name, name) == 0) {
                for (int ri = 0; ri < kPresets[pi].count; ri++) {
                    if (count == cap) {
                        cap = cap ? cap*2 : 64;
                        buf = (CPRange*)realloc(buf, cap * sizeof(CPRange));
                    }
                    buf[count++] = kPresets[pi].ranges[ri];
                }
                break;
            }
        }
    }
    /* Sort */
    for (int i = 1; i < count; i++) {
        CPRange x = buf[i]; int j = i-1;
        while (j >= 0 && buf[j].lo > x.lo) { buf[j+1]=buf[j]; j--; }
        buf[j+1] = x;
    }
    /* Merge overlapping */
    int out = 0;
    for (int i = 0; i < count; i++) {
        if (out > 0 && buf[i].lo <= buf[out-1].hi + 1)
            buf[out-1].hi = buf[out-1].hi > buf[i].hi ? buf[out-1].hi : buf[i].hi;
        else buf[out++] = buf[i];
    }
    *out_count = out;
    return buf;
}

/* ── FreeType glyph rendering ────────────────────────────────────────────── */

static uint8_t ft_advance_to_qpx(long adv26_6) {
    double v = (double)adv26_6 * 4.0 / 64.0;
    int qpx = (int)(v >= 0 ? v + 0.5 : v - 0.5);
    if (qpx < 0) qpx = 0;
    if (qpx > 255) qpx = 255;
    return (uint8_t)qpx;
}

/* Render one glyph. Returns 1 on success (glyph in font), 0 if missing. */
static int render_glyph_with_flags(FT_Face face, FT_Face fallback, uint32_t cp,
                                   int bw_only, FT_Int32 render_flags, Glyph *out) {
    out->codepoint = cp; out->pixels = NULL; out->bmp_off = 0;

    FT_UInt gi = FT_Get_Char_Index(face, (FT_ULong)cp);
    FT_Face active = face;
    if (gi == 0 && fallback) {
        FT_UInt fgi = FT_Get_Char_Index(fallback, (FT_ULong)cp);
        if (fgi != 0) { gi = fgi; active = fallback; }
    }
    if (gi == 0) return 0;

    /* Advance pass */
    if (bw_only) {
        FT_Load_Glyph(active, gi, FT_LOAD_DEFAULT);
    } else {
        FT_Load_Glyph(active, gi, FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP);
    }
    out->x_adv = ft_advance_to_qpx(active->glyph->advance.x);

    /* Render pass (hinted bitmap) */
    FT_Load_Glyph(active, gi, render_flags);

    FT_Bitmap *bm = &active->glyph->bitmap;
    out->bmp_w  = (uint8_t)(bm->width < 255 ? bm->width : 255);
    out->bmp_h  = (uint8_t)(bm->rows  < 255 ? bm->rows  : 255);
    out->x_off  = (int8_t)active->glyph->bitmap_left;
    out->y_off  = (int8_t)(-active->glyph->bitmap_top);

    if (out->bmp_w == 0 || out->bmp_h == 0) return 1;

    out->pixels = (uint8_t*)malloc((uint32_t)out->bmp_w * out->bmp_h);
    if (!out->pixels) { fputs("OOM\n", stderr); exit(1); }

    int pitch = abs(bm->pitch);
    for (int y = 0; y < out->bmp_h; y++) {
        for (int x = 0; x < out->bmp_w; x++) {
            uint8_t v;
            if (bm->pixel_mode == FT_PIXEL_MODE_MONO) {
                v = ((bm->buffer[y*pitch+(x>>3)] >> (7-(x&7))) & 1) ? 0 : 255;
            } else {
                /* FT GRAY: 0=bg,255=ink → invert to 0=ink,255=bg */
                v = 255 - bm->buffer[y*pitch + x];
            }
            out->pixels[y * out->bmp_w + x] = v;
        }
    }
    return 1;
}

/* Render one glyph. Returns 1 on success (glyph in font), 0 if missing. */
static int render_glyph(FT_Face face, FT_Face fallback, uint32_t cp,
                         int bw_only, Glyph *out) {
    return render_glyph_with_flags(face, fallback, cp, bw_only, FT_LOAD_RENDER, out);
}

/* Pack glyph pixels into BW/LSB/MSB bit planes (same algorithm as Python). */
static void pack_glyph(const Glyph *g, int bw_only, Buf *bw, Buf *lsb, Buf *msb) {
    if (!g->pixels) return;
    int w = g->bmp_w, h = g->bmp_h;
    int stride = (w + 7) / 8;
    uint32_t row_bytes = (uint32_t)(stride * h);

    buf_need(bw, row_bytes);
    uint8_t *bw_p = bw->data + bw->size;
    memset(bw_p, 0, row_bytes);
    bw->size += row_bytes;

    uint8_t *lsb_p = NULL, *msb_p = NULL;
    if (!bw_only) {
        buf_need(lsb, row_bytes);
        lsb_p = lsb->data + lsb->size;
        memset(lsb_p, 0, row_bytes);
        lsb->size += row_bytes;

        buf_need(msb, row_bytes);
        msb_p = msb->data + msb->size;
        memset(msb_p, 0, row_bytes);
        msb->size += row_bytes;
    }

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint8_t raw = g->pixels[y * w + x]; /* 0=ink 255=white */
            double p = pow(raw / 255.0, 1.6) * 255.0;

            int bi = y * stride + (x >> 3);
            uint8_t bit = (uint8_t)(0x80 >> (x & 7));

            if (p >= 154.0) bw_p[bi] |= bit;  /* 1=white */

            if (!bw_only) {
                int gray;
                if      (p >= 205.0) gray = 0;
                else if (p >= 154.0) gray = 1;
                else if (p >= 103.0) gray = 2;
                else if (p >= 52.0)  gray = 3;
                else                 gray = 0; /* darkest → also 0 in Python */
                if (gray & 1) lsb_p[bi] |= bit;
                if (gray & 2) msb_p[bi] |= bit;
            }
        }
    }
}

/* ── Preview rendering ───────────────────────────────────────────────────── */

static const char *kPreviewGlyphChars =
    "ABCDEFGHIJKLabcdefghijkl0123456789";

static const char *kPreviewDeviceText =
    "The quick brown fox jumps over the lazy dog. "
    "Pack my box with five dozen liquor jugs. "
    "How vexingly quick daft zebras jump! "
    "It was the best of times, it was the worst of times, "
    "it was the age of wisdom, it was the age of foolishness.";

typedef struct {
    int style_id;
    int size;
    int w, h;
    uint8_t *pixels; /* w*h grayscale, 0=black 255=white */
} PreviewSection;

typedef struct {
    uint32_t cp;
    int present;
    int loaded;
    Glyph glyph;
} PreviewGlyphEntry;

typedef struct {
    PreviewGlyphEntry *items;
    int count;
    int cap;
} PreviewGlyphMap;

static int cp_in_ranges(uint32_t cp, const CPRange *ranges, int n_ranges) {
    for (int i = 0; i < n_ranges; i++) {
        if (cp >= ranges[i].lo && cp <= ranges[i].hi) return 1;
    }
    return 0;
}

static int utf8_next_cp(const char **p, uint32_t *out_cp) {
    const unsigned char *s = (const unsigned char*)*p;
    if (!*s) return 0;

    uint32_t cp = 0xFFFD;
    int len = 1;
    if (s[0] < 0x80) {
        cp = s[0];
        len = 1;
    } else if ((s[0] & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
        cp = ((uint32_t)(s[0] & 0x1F) << 6) |
             ((uint32_t)(s[1] & 0x3F));
        len = 2;
    } else if ((s[0] & 0xF0) == 0xE0 &&
               (s[1] & 0xC0) == 0x80 &&
               (s[2] & 0xC0) == 0x80) {
        cp = ((uint32_t)(s[0] & 0x0F) << 12) |
             ((uint32_t)(s[1] & 0x3F) << 6) |
             ((uint32_t)(s[2] & 0x3F));
        len = 3;
    } else if ((s[0] & 0xF8) == 0xF0 &&
               (s[1] & 0xC0) == 0x80 &&
               (s[2] & 0xC0) == 0x80 &&
               (s[3] & 0xC0) == 0x80) {
        cp = ((uint32_t)(s[0] & 0x07) << 18) |
             ((uint32_t)(s[1] & 0x3F) << 12) |
             ((uint32_t)(s[2] & 0x3F) << 6) |
             ((uint32_t)(s[3] & 0x3F));
        len = 4;
    }

    *p += len;
    *out_cp = cp;
    return 1;
}

static int preview_cp_is_space(uint32_t cp) {
    return cp == 0x20 || cp == '\t' || cp == 0x00A0 || cp == 0x3000;
}

static int preview_cp_is_newline(uint32_t cp) {
    return cp == '\n' || cp == '\r';
}

static void preview_map_init(PreviewGlyphMap *m) {
    m->items = NULL;
    m->count = 0;
    m->cap = 0;
}

static void preview_map_free(PreviewGlyphMap *m) {
    for (int i = 0; i < m->count; i++) free(m->items[i].glyph.pixels);
    free(m->items);
    m->items = NULL;
    m->count = 0;
    m->cap = 0;
}

static int render_preview_missing_glyph(FT_Face face, FT_Face fallback,
                                        int bw_only, Glyph *out) {
    FT_Int32 render_flags = bw_only
        ? (FT_LOAD_RENDER | FT_LOAD_MONOCHROME | FT_LOAD_TARGET_MONO)
        : FT_LOAD_RENDER;
    memset(out, 0, sizeof(*out));
    out->codepoint = '?';
    return render_glyph_with_flags(face, fallback, '?', bw_only, render_flags, out);
}

static PreviewGlyphEntry *preview_map_get(PreviewGlyphMap *m,
                                          FT_Face face, FT_Face fallback,
                                          const CPRange *ranges, int n_ranges,
                                          uint32_t cp, int bw_only) {
    for (int i = 0; i < m->count; i++) {
        if (m->items[i].cp == cp) return &m->items[i];
    }

    if (m->count == m->cap) {
        int new_cap = m->cap ? m->cap * 2 : 64;
        PreviewGlyphEntry *new_items =
            (PreviewGlyphEntry*)realloc(m->items, (size_t)new_cap * sizeof(PreviewGlyphEntry));
        if (!new_items) { fputs("OOM\n", stderr); exit(1); }
        m->items = new_items;
        m->cap = new_cap;
    }

    PreviewGlyphEntry *entry = &m->items[m->count++];
    memset(entry, 0, sizeof(*entry));
    entry->cp = cp;
    entry->loaded = 1;
    if (cp_in_ranges(cp, ranges, n_ranges)) {
        FT_Int32 render_flags = bw_only
            ? (FT_LOAD_RENDER | FT_LOAD_MONOCHROME | FT_LOAD_TARGET_MONO)
            : FT_LOAD_RENDER;
        entry->present = render_glyph_with_flags(
            face, fallback, cp, bw_only, render_flags,
            &entry->glyph
        );
    }
    if (!entry->present && !preview_cp_is_space(cp) && !preview_cp_is_newline(cp)) {
        entry->present = render_preview_missing_glyph(face, fallback, bw_only, &entry->glyph);
    }
    return entry;
}

static uint8_t quantize_preview_pixel(uint8_t raw, int bw_only) {
    double p = pow(raw / 255.0, 1.6) * 255.0;
    if (bw_only) return p >= 154.0 ? 255 : 0;
    if (p >= 205.0) return 255;
    if (p >= 154.0) return 200;
    if (p >= 103.0) return 140;
    if (p >= 52.0)  return 80;
    return 0;
}

static void blit_glyph_to_preview(uint8_t *dst, int dst_w, int dst_h,
                                  int origin_x, int origin_y,
                                  const Glyph *g, int bw_only) {
    if (!g->pixels || g->bmp_w == 0 || g->bmp_h == 0) return;
    for (int py = 0; py < g->bmp_h; py++) {
        int dy = origin_y + py;
        if (dy < 0 || dy >= dst_h) continue;
        for (int px = 0; px < g->bmp_w; px++) {
            int dx = origin_x + px;
            if (dx < 0 || dx >= dst_w) continue;
            uint8_t v = quantize_preview_pixel(g->pixels[py * g->bmp_w + px], bw_only);
            if (v < dst[dy * dst_w + dx]) dst[dy * dst_w + dx] = v;
        }
    }
}

static void build_glyph_preview(FT_Face face, FT_Face fallback,
                                const CPRange *ranges, int n_ranges,
                                int size, int bw_only,
                                uint8_t **out_pixels, int *out_w, int *out_h) {
    const int pad = 2;
    const int sample_len = (int)strlen(kPreviewGlyphChars);
    Glyph *glyphs = (Glyph*)calloc((size_t)sample_len, sizeof(Glyph));
    int n_glyphs = 0;
    int max_h = 1;
    int total_w = pad;

    FT_Set_Pixel_Sizes(face, 0, (FT_UInt)size);
    if (fallback) FT_Set_Pixel_Sizes(fallback, 0, (FT_UInt)size);

    for (int i = 0; i < sample_len; i++) {
        uint32_t cp = (uint8_t)kPreviewGlyphChars[i];
        if (!cp_in_ranges(cp, ranges, n_ranges)) continue;
        Glyph g;
        memset(&g, 0, sizeof(g));
        if (!render_glyph(face, fallback, cp, bw_only, &g)) continue;
        if (!g.pixels || g.bmp_w == 0 || g.bmp_h == 0) {
            free(g.pixels);
            continue;
        }
        glyphs[n_glyphs++] = g;
        if (g.bmp_h > max_h) max_h = g.bmp_h;
        total_w += g.bmp_w + pad;
    }

    if (n_glyphs == 0) {
        free(glyphs);
        *out_pixels = (uint8_t*)malloc(1);
        if (*out_pixels) (*out_pixels)[0] = 255;
        *out_w = *out_h = 1;
        return;
    }

    uint8_t *pixels = (uint8_t*)malloc((size_t)total_w * max_h);
    if (!pixels) { fputs("OOM\n", stderr); exit(1); }
    memset(pixels, 255, (size_t)total_w * max_h);

    int cx = pad;
    for (int i = 0; i < n_glyphs; i++) {
        int cy = (max_h - glyphs[i].bmp_h) / 2;
        blit_glyph_to_preview(pixels, total_w, max_h, cx, cy, &glyphs[i], bw_only);
        cx += glyphs[i].bmp_w + pad;
    }

    for (int i = 0; i < n_glyphs; i++) free(glyphs[i].pixels);
    free(glyphs);

    *out_pixels = pixels;
    *out_w = total_w;
    *out_h = max_h;
}

static PreviewSection build_device_preview_section(FT_Face face, FT_Face fallback,
                                                   const CPRange *ranges, int n_ranges,
                                                   int style_id, int size, int bw_only,
                                                   const char *preview_text) {
    enum { PREVIEW_W = 480, MARGIN_X = 24, TOP_PAD = 4, BOTTOM_PAD = 4 };
    PreviewSection out;
    memset(&out, 0, sizeof(out));
    out.style_id = style_id;
    out.size = size;
    out.w = PREVIEW_W;

    if (!preview_text || !preview_text[0]) preview_text = kPreviewDeviceText;

    FT_Set_Pixel_Sizes(face, 0, (FT_UInt)size);
    if (fallback) FT_Set_Pixel_Sizes(fallback, 0, (FT_UInt)size);

    int ascender = (int)(face->size->metrics.ascender >> 6);
    int descender = (int)(face->size->metrics.descender >> 6);
    int y_advance = ascender - descender;
    if (y_advance < 1) y_advance = size > 0 ? size : 1;

    PreviewGlyphMap glyphs;
    preview_map_init(&glyphs);

    int default_adv_q = (int)lround(size * 2.0);
    int space_adv_q = default_adv_q;
    {
        PreviewGlyphEntry *space_g = preview_map_get(&glyphs, face, fallback, ranges, n_ranges, ' ', bw_only);
        if (space_g->present) space_adv_q = (int)space_g->glyph.x_adv;
        else space_adv_q = (int)lround(size * 1.2);
    }

    int max_w_q = (PREVIEW_W - MARGIN_X * 2) * 4;
    int line_count = 0;
    int line_has = 0;
    int line_x_q = 0;
    const char *text = preview_text;

    while (*text) {
        const char *ws = text;
        uint32_t cp = 0;
        while (*ws) {
          const char *next = ws;
          if (!utf8_next_cp(&next, &cp)) break;
          if (!preview_cp_is_space(cp) && !preview_cp_is_newline(cp)) break;
          ws = next;
          if (preview_cp_is_newline(cp)) {
              line_count++;
              line_has = 0;
              line_x_q = 0;
          }
        }
        text = ws;
        if (!*text) break;
        const char *word = text;
        int word_w_q = 0;
        while (*text) {
            const char *next = text;
            if (!utf8_next_cp(&next, &cp)) break;
            if (preview_cp_is_space(cp) || preview_cp_is_newline(cp)) break;
            PreviewGlyphEntry *entry = preview_map_get(&glyphs, face, fallback, ranges, n_ranges, cp, bw_only);
            word_w_q += entry->present ? entry->glyph.x_adv : default_adv_q;
            text = next;
        }
        if (line_has && line_x_q + space_adv_q + word_w_q > max_w_q) {
            line_count++;
            line_has = 0;
            line_x_q = 0;
        }
        if (line_has) line_x_q += space_adv_q;
        line_x_q += word_w_q;
        line_has = 1;
        (void)word;
    }
    if (line_has || line_count == 0) line_count++;
    if (line_count < 1) line_count = 1;

    out.h = TOP_PAD + line_count * y_advance + BOTTOM_PAD;
    out.pixels = (uint8_t*)malloc((size_t)out.w * out.h);
    if (!out.pixels) { fputs("OOM\n", stderr); exit(1); }
    memset(out.pixels, 255, (size_t)out.w * out.h);

    int cur_y = TOP_PAD;
    line_has = 0;
    line_x_q = 0;
    text = preview_text;

    while (*text) {
        const char *ws = text;
        uint32_t cp = 0;
        while (*ws) {
            const char *next = ws;
            if (!utf8_next_cp(&next, &cp)) break;
            if (!preview_cp_is_space(cp) && !preview_cp_is_newline(cp)) break;
            ws = next;
            if (preview_cp_is_newline(cp)) {
                cur_y += y_advance;
                line_has = 0;
                line_x_q = 0;
            }
        }
        text = ws;
        if (!*text) break;
        const char *word = text;
        int word_w_q = 0;
        while (*text) {
            const char *next = text;
            if (!utf8_next_cp(&next, &cp)) break;
            if (preview_cp_is_space(cp) || preview_cp_is_newline(cp)) break;
            PreviewGlyphEntry *entry = preview_map_get(&glyphs, face, fallback, ranges, n_ranges, cp, bw_only);
            word_w_q += entry->present ? entry->glyph.x_adv : default_adv_q;
            text = next;
        }
        if (line_has && line_x_q + space_adv_q + word_w_q > max_w_q) {
            cur_y += y_advance;
            line_has = 0;
            line_x_q = 0;
        }
        if (line_has) line_x_q += space_adv_q;

        int glyph_x_q = line_x_q;
        int base_y = cur_y + ascender;
        const char *p = word;
        while (p < text) {
            const char *next = p;
            utf8_next_cp(&next, &cp);
            int adv_q = default_adv_q;
            PreviewGlyphEntry *entry = preview_map_get(&glyphs, face, fallback, ranges, n_ranges, cp, bw_only);
            if (entry->present) {
                Glyph *g = &entry->glyph;
                int draw_x = MARGIN_X + (glyph_x_q + 2) / 4 + g->x_off;
                int draw_y = base_y + g->y_off;
                blit_glyph_to_preview(out.pixels, out.w, out.h, draw_x, draw_y, g, bw_only);
                adv_q = g->x_adv;
            }
            glyph_x_q += adv_q;
            p = next;
        }
        line_x_q = glyph_x_q;
        line_has = 1;
    }

    preview_map_free(&glyphs);
    return out;
}

static Buf build_preview_bundle(const PreviewSection *sections, int n_sections) {
    Buf out;
    buf_init(&out);

    buf_append(&out, (const uint8_t*)"FPRV", 4);
    buf_u16le(&out, 2);
    buf_u16le(&out, (uint16_t)n_sections);

    for (int i = 0; i < n_sections; i++) {
        uint32_t len = (uint32_t)(sections[i].w * sections[i].h);
        buf_u16le(&out, (uint16_t)sections[i].size);
        buf_u16le(&out, (uint16_t)sections[i].w);
        buf_u16le(&out, (uint16_t)sections[i].h);
        buf_u16le(&out, (uint16_t)sections[i].style_id);
        buf_u32le(&out, len);
    }

    for (int i = 0; i < n_sections; i++) {
        uint32_t len = (uint32_t)(sections[i].w * sections[i].h);
        buf_append(&out, sections[i].pixels, len);
    }
    return out;
}

/* ── Coverage table helpers ──────────────────────────────────────────────── */

/* Returns 0-based coverage index of glyph_id, or -1 if not covered. */
static int coverage_index(const uint8_t *cov, uint32_t cov_len, uint16_t gid) {
    if (cov_len < 4) return -1;
    uint16_t fmt = rd16(cov);
    if (fmt == 1) {
        uint16_t n = rd16(cov+2);
        for (uint16_t i = 0; i < n; i++)
            if (rd16(cov+4+i*2) == gid) return (int)i;
    } else if (fmt == 2) {
        uint16_t n = rd16(cov+2);
        for (uint16_t i = 0; i < n; i++) {
            const uint8_t *r = cov+4+i*6;
            uint16_t s=rd16(r), e=rd16(r+2), si=rd16(r+4);
            if (gid >= s && gid <= e) return (int)(si + (gid-s));
        }
    }
    return -1;
}

/* Fill out[] with all covered glyph IDs. Returns count. */
static int coverage_all(const uint8_t *cov, uint32_t cov_len,
                         uint16_t *out, int max_out) {
    if (cov_len < 4) return 0;
    int n_out = 0;
    uint16_t fmt = rd16(cov);
    if (fmt == 1) {
        uint16_t n = rd16(cov+2);
        for (uint16_t i = 0; i < n && n_out < max_out; i++)
            out[n_out++] = rd16(cov+4+i*2);
    } else if (fmt == 2) {
        uint16_t n = rd16(cov+2);
        for (uint16_t i = 0; i < n; i++) {
            const uint8_t *r = cov+4+i*6;
            for (uint16_t g = rd16(r); g <= rd16(r+2) && n_out < max_out; g++)
                out[n_out++] = g;
        }
    }
    return n_out;
}

/* ── ClassDef helper ─────────────────────────────────────────────────────── */

/* Returns class value for glyph_id (0 if not in ClassDef). */
static uint16_t classdef_get(const uint8_t *cd, uint32_t cd_len, uint16_t gid) {
    if (!cd || cd_len < 4) return 0;
    uint16_t fmt = rd16(cd);
    if (fmt == 1) {
        uint16_t start=rd16(cd+2), n=rd16(cd+4);
        if (gid >= start && gid < (uint16_t)(start+n))
            return rd16(cd+6+(gid-start)*2);
    } else if (fmt == 2) {
        uint16_t n = rd16(cd+2);
        for (uint16_t i = 0; i < n; i++) {
            const uint8_t *r = cd+4+i*6;
            uint16_t s=rd16(r), e=rd16(r+2);
            if (gid >= s && gid <= e) return rd16(r+4);
        }
    }
    return 0;
}

/* ── ValueRecord helpers ─────────────────────────────────────────────────── */

/* Number of bytes in a ValueRecord for the given valueFormat. */
static int vr_size(uint16_t vfmt) {
    int n = 0;
    for (int b = 0; b < 16; b++) if (vfmt & (1<<b)) n++;
    return n * 2;
}

/* Extract XAdvance from a ValueRecord (offset of XAdvance depends on which
   lower bits are set). Returns 0 if XAdvance not present (bit 2 not set). */
static int16_t vr_xadv(const uint8_t *rec, uint16_t vfmt) {
    if (!(vfmt & 0x0004)) return 0;
    int off = 0;
    if (vfmt & 0x0001) off += 2;
    if (vfmt & 0x0002) off += 2;
    return rd16s(rec + off);
}

/* ── Flat kern pair accumulator ──────────────────────────────────────────── */

/* Key: (left_glyph_id << 16) | right_glyph_id. Value: kern in design units.
   We use a simple dynamic array and rely on the caller to iterate in priority
   order (later entries overwrite earlier ones for the same key). */

typedef struct { uint32_t key; int16_t value; } KP;

/* Open-addressed hash map: slot is empty when key==0 and used==0. */
typedef struct { uint32_t key; int16_t value; uint8_t used; } KPSlot;
typedef struct { KPSlot *slots; int size, count; } KPMap;

static void kpmap_init(KPMap *m) { m->slots = NULL; m->size = m->count = 0; }

static void kpmap_free(KPMap *m) { free(m->slots); kpmap_init(m); }

/* Mix bits so that keys differing only in the high half land in different buckets. */
static uint32_t kp_hash(uint32_t k) {
    k ^= k >> 16;
    k *= 0x45d9f3bu;
    k ^= k >> 16;
    return k;
}

static void kp_set(KPMap *m, uint16_t l, uint16_t r, int16_t v) {
    uint32_t key = ((uint32_t)l << 16) | r;
    /* Grow at 75% load */
    if (m->count >= m->size * 3 / 4) {
        int nsz = m->size ? m->size * 2 : 65536;
        KPSlot *nt = (KPSlot*)calloc(nsz, sizeof(KPSlot));
        for (int i = 0; i < m->size; i++) {
            if (!m->slots[i].used) continue;
            uint32_t h = kp_hash(m->slots[i].key) & (uint32_t)(nsz-1);
            while (nt[h].used) h = (h+1) & (uint32_t)(nsz-1);
            nt[h] = m->slots[i];
        }
        free(m->slots); m->slots = nt; m->size = nsz;
    }
    uint32_t h = kp_hash(key) & (uint32_t)(m->size-1);
    while (m->slots[h].used && m->slots[h].key != key)
        h = (h+1) & (uint32_t)(m->size-1);
    if (!m->slots[h].used) { m->slots[h].used = 1; m->slots[h].key = key; m->count++; }
    m->slots[h].value = v;
}

/* Extract flat KP array for compress_kerning; caller frees. */
static KP *kpmap_to_array(const KPMap *m, int *out_count) {
    KP *arr = (KP*)malloc(m->count * sizeof(KP));
    int n = 0;
    for (int i = 0; i < m->size; i++) {
        if (!m->slots[i].used) continue;
        arr[n].key = m->slots[i].key; arr[n].value = m->slots[i].value; n++;
    }
    *out_count = n;
    return arr;
}

/* ── kern table (legacy) ─────────────────────────────────────────────────── */

static void parse_kern_table(const uint8_t *data, uint32_t len, KPMap *out) {
    if (len < 4) return;
    /* Detect OTF kern (version=1) vs TTF kern (version=0) */
    uint32_t version32 = rd32(data);
    if (version32 == 0x00010000) {
        /* OTF kern: uint32 version, uint32 nTables */
        uint32_t n = rd32(data + 4);
        uint32_t off = 8;
        for (uint32_t ti = 0; ti < n && off + 16 <= len; ti++) {
            /* length:uint32, coverage:uint16, tupleIndex:uint16, then Format0/2 */
            uint32_t tlen = rd32(data+off);
            uint16_t fmt  = rd16(data+off+8);
            if (fmt == 0 && off+16 <= len) {
                uint16_t np = rd16(data+off+10);
                const uint8_t *p = data+off+16;
                for (uint16_t pi = 0; pi < np && (uint32_t)(p+6-data) <= len; pi++, p+=6)
                    kp_set(out, rd16(p), rd16(p+2), rd16s(p+4));
            }
            off += tlen;
        }
    } else {
        /* TTF kern: uint16 version=0, uint16 nTables */
        uint16_t n = rd16(data+2);
        uint32_t off = 4;
        for (uint16_t ti = 0; ti < n && off + 6 <= len; ti++) {
            uint16_t tlen = rd16(data+off+2);
            uint16_t cov  = rd16(data+off+4);
            uint8_t  fmt  = (uint8_t)(cov >> 8);
            int horiz     = (cov & 0x01) != 0;
            (void)horiz;
            if (fmt == 0 && off+14 <= len) {
                uint16_t np = rd16(data+off+6);
                const uint8_t *p = data+off+14;
                for (uint16_t pi = 0; pi < np && (uint32_t)(p+6-data) <= len; pi++, p+=6)
                    kp_set(out, rd16(p), rd16(p+2), rd16s(p+4));
            }
            off += tlen;
        }
    }
}

/* ── GPOS PairPos subtable parser ────────────────────────────────────────── */

static void parse_pairpos_f1(const uint8_t *sub, uint32_t sub_len, KPMap *out) {
    if (sub_len < 10) return;
    uint16_t cov_off  = rd16(sub+2);
    uint16_t vfmt1    = rd16(sub+4);
    uint16_t vfmt2    = rd16(sub+6);
    uint16_t pair_cnt = rd16(sub+8);

    if (!(vfmt1 & 0x0004)) return; /* no XAdvance in value1 */

    int vr1 = vr_size(vfmt1), vr2 = vr_size(vfmt2);
    int pvr_sz = 2 + vr1 + vr2; /* secondGlyph + value1 + value2 */

    if (cov_off + 4 > sub_len) return;

    uint16_t left_glyphs[8192];
    int n_left = coverage_all(sub+cov_off, sub_len-cov_off, left_glyphs, 8192);

    for (int i = 0; i < n_left && i < (int)pair_cnt; i++) {
        if (10 + i*2 + 2 > (int)sub_len) break;
        uint16_t ps_off = rd16(sub+10+i*2);
        if ((uint32_t)ps_off + 2 > sub_len) continue;
        const uint8_t *ps = sub + ps_off;
        uint16_t pvc = rd16(ps);
        for (int j = 0; j < (int)pvc; j++) {
            const uint8_t *pvr = ps + 2 + j * pvr_sz;
            if (pvr + pvr_sz > sub + sub_len) break;
            int16_t kern = vr_xadv(pvr+2, vfmt1);
            if (kern) kp_set(out, left_glyphs[i], rd16(pvr), kern);
        }
    }
}

static void parse_pairpos_f2(const uint8_t *sub, uint32_t sub_len, KPMap *out,
                               const uint16_t *rendered_gids, int n_rendered) {
    if (sub_len < 16) return;
    uint16_t cov_off = rd16(sub+2);
    uint16_t vfmt1   = rd16(sub+4);
    uint16_t vfmt2   = rd16(sub+6);
    uint16_t cd1_off = rd16(sub+8);
    uint16_t cd2_off = rd16(sub+10);
    uint16_t c1_cnt  = rd16(sub+12);
    uint16_t c2_cnt  = rd16(sub+14);

    if (!(vfmt1 & 0x0004)) return;
    if (c1_cnt == 0 || c2_cnt == 0) return;

    int vr1 = vr_size(vfmt1), vr2 = vr_size(vfmt2);
    int c2r_sz = vr1 + vr2;
    int c1r_sz = c2_cnt * c2r_sz;

    if (cov_off + 4 > sub_len) return;

    const uint8_t *cd1 = (cd1_off && cd1_off < sub_len) ? sub+cd1_off : NULL;
    const uint8_t *cd2 = (cd2_off && cd2_off < sub_len) ? sub+cd2_off : NULL;
    uint32_t cd1_len = cd1 ? sub_len-cd1_off : 0;
    uint32_t cd2_len = cd2 ? sub_len-cd2_off : 0;

    /* Pre-compute class assignments for each rendered glyph — O(n) passes
       instead of calling coverage_index/classdef_get inside a nested loop. */
    uint16_t *lc = (uint16_t*)malloc(n_rendered * sizeof(uint16_t));
    uint16_t *rc = (uint16_t*)malloc(n_rendered * sizeof(uint16_t));
    if (!lc || !rc) { free(lc); free(rc); return; }

    int n_lvalid = 0;
    for (int i = 0; i < n_rendered; i++) {
        uint16_t g = rendered_gids[i];
        uint16_t c1 = classdef_get(cd1, cd1_len, g);
        if (c1 == 0 || c1 >= c1_cnt || coverage_index(sub+cov_off, sub_len-cov_off, g) < 0)
            lc[i] = 0xFFFF; /* invalid */
        else {
            lc[i] = c1;
            n_lvalid++;
        }
        rc[i] = classdef_get(cd2, cd2_len, g);
    }

    if (n_lvalid == 0) { free(lc); free(rc); return; }

    /* Build c1 → glyph list and c2 → glyph list.  O(n). */
    int *c1_cnt_arr = (int*)calloc(c1_cnt, sizeof(int));
    int *c2_cnt_arr = (int*)calloc(c2_cnt, sizeof(int));
    for (int i = 0; i < n_rendered; i++) {
        if (lc[i] != 0xFFFF) c1_cnt_arr[lc[i]]++;
        if (rc[i] < c2_cnt)  c2_cnt_arr[rc[i]]++;
    }

    uint16_t **c1g = (uint16_t**)calloc(c1_cnt, sizeof(uint16_t*));
    uint16_t **c2g = (uint16_t**)calloc(c2_cnt, sizeof(uint16_t*));
    int *c1fill = (int*)calloc(c1_cnt, sizeof(int));
    int *c2fill = (int*)calloc(c2_cnt, sizeof(int));

    for (int c = 1; c < (int)c1_cnt; c++)
        if (c1_cnt_arr[c] > 0) c1g[c] = (uint16_t*)malloc(c1_cnt_arr[c] * sizeof(uint16_t));
    for (int c = 0; c < (int)c2_cnt; c++)
        if (c2_cnt_arr[c] > 0) c2g[c] = (uint16_t*)malloc(c2_cnt_arr[c] * sizeof(uint16_t));

    for (int i = 0; i < n_rendered; i++) {
        uint16_t g = rendered_gids[i];
        if (lc[i] != 0xFFFF && c1g[lc[i]]) c1g[lc[i]][c1fill[lc[i]]++] = g;
        if (rc[i] < c2_cnt && c2g[rc[i]])  c2g[rc[i]][c2fill[rc[i]]++] = g;
    }

    fprintf(stderr, "  [t] f2: class setup done, c1_cnt=%d c2_cnt=%d n_lvalid=%d\n", c1_cnt, c2_cnt, n_lvalid);
    /* Iterate class matrix — O(c1_cnt × c2_cnt), emit pairs only for non-zero cells. */
    for (int c1 = 1; c1 < (int)c1_cnt; c1++) {
        if (!c1g[c1] || c1fill[c1] == 0) continue;
        for (int c2 = 0; c2 < (int)c2_cnt; c2++) {
            uint32_t moff = 16 + (uint32_t)c1 * c1r_sz + (uint32_t)c2 * c2r_sz;
            if (moff + vr1 > sub_len) continue;
            int16_t kern = vr_xadv(sub + moff, vfmt1);
            if (!kern || !c2g[c2] || c2fill[c2] == 0) continue;
            for (int li = 0; li < c1fill[c1]; li++)
                for (int ri = 0; ri < c2fill[c2]; ri++)
                    kp_set(out, c1g[c1][li], c2g[c2][ri], kern);
        }
    }

    fprintf(stderr, "  [t] f2: matrix iteration done\n");
    for (int c = 1; c < (int)c1_cnt; c++) free(c1g[c]);
    for (int c = 0; c < (int)c2_cnt; c++) free(c2g[c]);
    free(lc); free(rc);
    free(c1_cnt_arr); free(c2_cnt_arr);
    free(c1g); free(c2g);
    free(c1fill); free(c2fill);
}

/* ── Full GPOS extraction ─────────────────────────────────────────────────── */

static void parse_gpos(const uint8_t *gpos, uint32_t gpos_len, KPMap *out,
                        const uint16_t *rendered_gids, int n_rendered) {
    if (gpos_len < 10) return;

    uint16_t ll_off = rd16(gpos+8); /* LookupList offset */
    if ((uint32_t)ll_off + 2 > gpos_len) return;

    const uint8_t *ll = gpos + ll_off;
    uint32_t ll_rem = gpos_len - ll_off;
    if (ll_rem < 2) return;

    uint16_t n_lookups = rd16(ll);

    for (uint16_t li = 0; li < n_lookups; li++) {
        if (2 + li*2 + 2 > (int)ll_rem) break;
        uint16_t l_off = rd16(ll + 2 + li*2);
        if ((uint32_t)ll_off + l_off + 6 > gpos_len) continue;
        const uint8_t *look = ll + l_off;
        uint32_t look_rem = ll_rem - l_off;
        if (look_rem < 6) continue;

        uint16_t l_type  = rd16(look);
        uint16_t l_flag  = rd16(look+2);
        uint16_t n_subs  = rd16(look+4);
        (void)l_flag;

        for (uint16_t si = 0; si < n_subs; si++) {
            if ((uint32_t)(6 + si*2 + 2) > look_rem) break;
            uint16_t sub_off = rd16(look + 6 + si*2);
            if ((uint32_t)l_off + sub_off + 2 > ll_rem) continue;
            const uint8_t *sub = look + sub_off;
            uint32_t sub_rem = look_rem - sub_off;

            uint16_t actual_type = l_type;
            if (l_type == 9) {
                /* Extension: format(2)+extensionLookupType(2)+extensionOffset(4) */
                if (sub_rem < 8) continue;
                actual_type = rd16(sub+2);
                uint32_t ext_off = rd32(sub+4);
                if (ext_off > sub_rem - 2) continue;
                sub = sub + ext_off;
                sub_rem = sub_rem - ext_off;
            }
            if (actual_type != 2) continue;
            if (sub_rem < 2) continue;

            uint16_t fmt = rd16(sub);
            if (fmt == 1) parse_pairpos_f1(sub, sub_rem, out);
            else if (fmt == 2) parse_pairpos_f2(sub, sub_rem, out, rendered_gids, n_rendered);
        }
    }
}

/* ── Kerning class compression ────────────────────────────────────────────── */

typedef struct { int16_t l_idx, r_idx; int16_t val; } KPair;
typedef struct { int16_t l_class, r_idx; int16_t val; } KPair2;

static int cmp_kpair_lr(const void *a, const void *b) {
    const KPair *pa = (const KPair*)a, *pb = (const KPair*)b;
    if (pa->l_idx != pb->l_idx) return (int)pa->l_idx - (int)pb->l_idx;
    return (int)pa->r_idx - (int)pb->r_idx;
}
static int cmp_kpair2_rl(const void *a, const void *b) {
    const KPair2 *pa = (const KPair2*)a, *pb = (const KPair2*)b;
    if (pa->r_idx != pb->r_idx) return (int)pa->r_idx - (int)pb->r_idx;
    return (int)pa->l_class - (int)pb->l_class;
}

static Buf compress_kerning(const KP *flat, int n_flat, int n_glyphs,
                             const int16_t *glyph_id_to_idx,
                             const uint16_t *glyph_ids,
                             int total_rendered,
                             int size, int upem) {
    Buf result; buf_init(&result);
    if (n_flat == 0 || n_glyphs == 0) return result;

    /* Scale flat pairs to quarter-pixel units; filter zeros. */
    KPair *pairs = (KPair*)malloc(n_flat * sizeof(KPair));
    int np = 0;
    for (int i = 0; i < n_flat; i++) {
        uint16_t lg = (uint16_t)(flat[i].key >> 16);
        uint16_t rg = (uint16_t)(flat[i].key);
        int16_t du = flat[i].value;
        int qpx = (int)round((double)du * size / upem * 4.0);
        if (qpx == 0) continue;
        if (qpx < -128) qpx = -128;
        if (qpx >  127) qpx =  127;
        int16_t li = (lg < 65535) ? glyph_id_to_idx[lg] : -1;
        int16_t ri = (rg < 65535) ? glyph_id_to_idx[rg] : -1;
        if (li < 0 || ri < 0) continue;
        KPair _p; _p.l_idx = li; _p.r_idx = ri; _p.val = (int16_t)qpx;
        pairs[np++] = _p;
    }
    if (np == 0) { free(pairs); return result; }

    /* Sort by (l_idx, r_idx) so we can do a single linear pass per left glyph. */
    qsort(pairs, np, sizeof(KPair), cmp_kpair_lr);

    uint8_t *l_class_map = (uint8_t*)calloc(n_glyphs, 1);
    char **l_class_keys = (char**)malloc(256 * sizeof(char*));
    int n_l_classes = 1;
    l_class_keys[0] = strdup("");

    /* Single pass: each run of pairs with the same l_idx is one left profile. */
    char key[8192];
    int p = 0;
    while (p < np) {
        int l = pairs[p].l_idx;
        int start = p;
        while (p < np && pairs[p].l_idx == l) p++;
        /* pairs[start..p) are sorted by r_idx (secondary key from qsort) */
        int off = 0;
        for (int q = start; q < p; q++)
            off += snprintf(key+off, sizeof(key)-off, "%d:%d|",
                            pairs[q].r_idx, pairs[q].val);
        int cls = 0;
        for (int ci = 1; ci < n_l_classes; ci++)
            if (strcmp(l_class_keys[ci], key) == 0) { cls = ci; break; }
        if (cls == 0 && n_l_classes < 255) {
            cls = n_l_classes;
            l_class_keys[n_l_classes++] = strdup(key);
        } else if (cls == 0) cls = 255;
        l_class_map[l] = (uint8_t)cls;
    }

    /* Build pairs2: (l_class, r_idx, val), skip unmapped left glyphs. */
    KPair2 *pairs2 = (KPair2*)malloc(np * sizeof(KPair2));
    int np2 = 0;
    for (int i = 0; i < np; i++) {
        uint8_t lc = l_class_map[pairs[i].l_idx];
        if (lc == 0 || lc == 255) continue;
        KPair2 _p; _p.l_class = (int16_t)lc;
        _p.r_idx = pairs[i].r_idx; _p.val = pairs[i].val;
        pairs2[np2++] = _p;
    }

    /* Sort by (r_idx, l_class) for a single linear pass per right glyph. */
    qsort(pairs2, np2, sizeof(KPair2), cmp_kpair2_rl);

    uint8_t *r_class_map = (uint8_t*)calloc(n_glyphs, 1);
    char **r_class_keys = (char**)malloc(256 * sizeof(char*));
    int n_r_classes = 1;
    r_class_keys[0] = strdup("");

    p = 0;
    while (p < np2) {
        int r = pairs2[p].r_idx;
        int start = p;
        while (p < np2 && pairs2[p].r_idx == r) p++;
        int off = 0;
        for (int q = start; q < p; q++)
            off += snprintf(key+off, sizeof(key)-off, "%d:%d|",
                            pairs2[q].l_class, pairs2[q].val);
        int cls = 0;
        for (int ci = 1; ci < n_r_classes; ci++)
            if (strcmp(r_class_keys[ci], key) == 0) { cls = ci; break; }
        if (cls == 0 && n_r_classes < 255) {
            cls = n_r_classes;
            r_class_keys[n_r_classes++] = strdup(key);
        } else if (cls == 0) cls = 255;
        r_class_map[r] = (uint8_t)cls;
    }

    int numL = n_l_classes < 256 ? n_l_classes : 256;
    int numR = n_r_classes < 256 ? n_r_classes : 256;

    int8_t *matrix = (int8_t*)calloc(numL * numR, 1);
    for (int i = 0; i < np; i++) {
        uint8_t lc = l_class_map[pairs[i].l_idx];
        uint8_t rc = r_class_map[pairs[i].r_idx];
        if (lc > 0 && lc < 256 && rc < 256)
            matrix[lc * numR + rc] = (int8_t)pairs[i].val;
    }

    buf_u8(&result, (uint8_t)(numL - 1));
    buf_u8(&result, (uint8_t)(numR - 1));
    buf_append(&result, l_class_map, n_glyphs);
    buf_append(&result, r_class_map, n_glyphs);
    buf_append(&result, (uint8_t*)matrix, numL * numR);

    fprintf(stderr, "    Kerning: %d pairs -> %d x %d classes (%u bytes)\n",
            np, numL, numR, result.size);

    free(pairs); free(pairs2);
    for (int i = 0; i < n_l_classes; i++) free(l_class_keys[i]);
    for (int i = 0; i < n_r_classes; i++) free(r_class_keys[i]);
    free(l_class_keys); free(r_class_keys);
    free(l_class_map); free(r_class_map); free(matrix);
    return result;
}

/* ── Per-style rendering pipeline ────────────────────────────────────────── */

typedef struct {
    Range   *ranges;    int n_ranges;
    Glyph   *glyphs;    int n_glyphs;
    Buf      bw, lsb, msb;
    Buf      kern_bytes;
    uint8_t  max_height;
    int      ascender, descender;
} StyleOut;

static void style_free(StyleOut *s) {
    for (int i = 0; i < s->n_glyphs; i++) free(s->glyphs[i].pixels);
    free(s->ranges); free(s->glyphs);
    buf_free(&s->bw); buf_free(&s->lsb); buf_free(&s->msb);
    buf_free(&s->kern_bytes);
}

/*
 * Render all codepoints in the given ranges with face (fallback for missing).
 * Extracts kerning from kern+GPOS tables via hb_face.
 */
static StyleOut render_style(FT_Face face, FT_Face fallback, hb_face_t *hb_face,
                               const CPRange *ranges, int n_ranges, int size,
                               int bw_only, int upem) {
    StyleOut out;
    memset(&out, 0, sizeof(out));
    buf_init(&out.bw); buf_init(&out.lsb); buf_init(&out.msb);
    buf_init(&out.kern_bytes);

    /* Set pixel size on both faces */
    FT_Set_Pixel_Sizes(face, 0, (FT_UInt)size);
    out.ascender  = (int)(face->size->metrics.ascender >> 6);
    out.descender = (int)(face->size->metrics.descender >> 6); /* negative */
    if (fallback) FT_Set_Pixel_Sizes(fallback, 0, (FT_UInt)size);

    /* Count total codepoints */
    int total_cp = 0;
    for (int ri = 0; ri < n_ranges; ri++)
        total_cp += (int)(ranges[ri].hi - ranges[ri].lo + 1);

    fprintf(stderr, "  Rasterizing %d codepoints at %dpx...\n", total_cp, size);

    /* Allocate glyph + range arrays */
    out.glyphs = (Glyph*)malloc(total_cp * sizeof(Glyph));
    out.ranges = (Range*)malloc(n_ranges * sizeof(Range));

    /* Also collect rendered glyph IDs for GPOS lookup */
    uint16_t *rendered_gids = (uint16_t*)malloc(total_cp * sizeof(uint16_t));
    int16_t  *gid_to_idx    = (int16_t*)malloc(65536 * sizeof(int16_t));
    memset(gid_to_idx, 0xFF, 65536 * sizeof(int16_t)); /* -1 = not in set */

    int rendered_count = 0, last_pct = -1;

    for (int ri = 0; ri < n_ranges; ri++) {
        uint32_t first_cp = ranges[ri].lo;
        uint32_t last_cp  = ranges[ri].hi;
        int glyph_start   = out.n_glyphs;
        int has_any       = 0;

        for (uint32_t cp = first_cp; cp <= last_cp; cp++) {
            rendered_count++;
            int pct = rendered_count * 100 / total_cp;
            if (pct != last_pct && pct % 10 == 0) {
                fprintf(stderr, "  Rendering... %d%%\r", pct);
                last_pct = pct;
            }

            Glyph g; memset(&g, 0, sizeof(g));
            int ok = render_glyph(face, fallback, cp, bw_only, &g);

            if (!ok) {
                /* Missing glyph: push empty placeholder */
                g.codepoint = cp;
            } else {
                /* Pack bitmap into BW/LSB/MSB */
                g.bmp_off = out.bw.size;
                pack_glyph(&g, bw_only, &out.bw, &out.lsb, &out.msb);

                if (g.bmp_h > out.max_height) out.max_height = g.bmp_h;
                has_any = 1;

                /* Track glyph ID → array index for kerning */
                FT_UInt gi = FT_Get_Char_Index(face, (FT_ULong)cp);
                if (gi != 0 && gi < 65535 && out.n_glyphs < 65535) {
                    if (gid_to_idx[gi] < 0) {
                        gid_to_idx[gi] = (int16_t)out.n_glyphs;
                        rendered_gids[out.n_glyphs] = (uint16_t)gi;
                    }
                }
            }
            out.glyphs[out.n_glyphs++] = g;
        }

        if (has_any) {
            Range _r;
            _r.first_cp    = first_cp;
            _r.count       = (uint16_t)(last_cp - first_cp + 1);
            _r.glyph_start = (uint16_t)glyph_start;
            out.ranges[out.n_ranges++] = _r;
        } else {
            out.n_glyphs = glyph_start; /* discard empty range */
        }
    }
    if (last_pct != 100) fprintf(stderr, "  Rendering... done           \n");

    /* ── Kerning extraction ── */
    fprintf(stderr, "  Extracting kerning...\n");
    KPMap kpmap; kpmap_init(&kpmap);

    /* kern table */
    fprintf(stderr, "  [t] kern parse start\n");
    hb_blob_t *kern_blob = hb_face_reference_table(hb_face, HB_TAG('k','e','r','n'));
    uint32_t kern_len = 0;
    const uint8_t *kern_data = (const uint8_t*)hb_blob_get_data(kern_blob, &kern_len);
    if (kern_data && kern_len > 0) parse_kern_table(kern_data, kern_len, &kpmap);
    hb_blob_destroy(kern_blob);
    fprintf(stderr, "  [t] kern parse done (%d pairs)\n", kpmap.count);

    /* GPOS table */
    fprintf(stderr, "  [t] gpos parse start (n_glyphs=%d)\n", out.n_glyphs);
    hb_blob_t *gpos_blob = hb_face_reference_table(hb_face, HB_TAG('G','P','O','S'));
    uint32_t gpos_len = 0;
    const uint8_t *gpos_data = (const uint8_t*)hb_blob_get_data(gpos_blob, &gpos_len);
    fprintf(stderr, "  [t] gpos table size: %u bytes\n", gpos_len);
    if (gpos_data && gpos_len > 0)
        parse_gpos(gpos_data, gpos_len, &kpmap, rendered_gids, out.n_glyphs);
    hb_blob_destroy(gpos_blob);
    fprintf(stderr, "  [t] gpos parse done (%d pairs total)\n", kpmap.count);

    /* Compress into MBF class kerning bytes */
    fprintf(stderr, "  [t] compress start\n");
    if (kpmap.count > 0) {
        int n_flat = 0;
        KP *flat = kpmap_to_array(&kpmap, &n_flat);
        out.kern_bytes = compress_kerning(flat, n_flat,
                                           out.n_glyphs, gid_to_idx,
                                           rendered_gids, out.n_glyphs,
                                           size, upem);
        free(flat);
    }
    fprintf(stderr, "  [t] compress done\n");

    kpmap_free(&kpmap);
    free(rendered_gids);
    free(gid_to_idx);
    return out;
}

/* ── Range + glyph table encoder ─────────────────────────────────────────── */

static void encode_ranges_and_glyphs(Buf *out, const StyleOut *s, uint32_t bmp_base) {
    for (int i = 0; i < s->n_ranges; i++) {
        buf_u32le(out, s->ranges[i].first_cp);
        buf_u16le(out, s->ranges[i].count);
        buf_u16le(out, s->ranges[i].glyph_start);
    }
    for (int i = 0; i < s->n_glyphs; i++) {
        const Glyph *g = &s->glyphs[i];
        buf_u32le(out, g->bmp_off + bmp_base);
        buf_u8(out, g->x_adv);
        buf_u8(out, g->bmp_w);
        buf_u8(out, g->bmp_h);
        buf_i8(out, g->x_off);
        buf_i8(out, g->y_off);
        buf_u8(out, 0); /* reserved */
    }
}

/* ── MBF builder ─────────────────────────────────────────────────────────── */

typedef struct {
    const StyleOut *regular;
    const StyleOut *bold;        /* NULL if not present */
    const StyleOut *italic;
    const StyleOut *bold_italic;
    int size, upem;
    int bw_only;
    /* Font metrics (from regular face post table) */
    int ul_pos, ul_thick;
} MbfParams;

#define HEADER_SIZE 50

static Buf build_mbf(const MbfParams *p) {
    Buf out; buf_init(&out);

    const StyleOut *reg = p->regular;
    const StyleOut *extras[3] = { p->bold, p->italic, p->bold_italic };
    const char *enames[3] = { "bold","italic","bold_italic" };
    int n_extras = 0;
    for (int i = 0; i < 3; i++) if (extras[i]) n_extras = i+1;

    uint8_t style_flags = 0;
    if (p->bold || p->italic || p->bold_italic) style_flags |= 0x01; /* Regular present */
    if (p->bold)        style_flags |= 0x02;
    if (p->italic)      style_flags |= 0x04;
    if (p->bold_italic) style_flags |= 0x08;

    int y_advance = reg->ascender - reg->descender;
    int baseline  = reg->ascender;
    int default_advance = (p->size / 2) * 4;

    int max_height = reg->max_height;
    for (int i = 0; i < 3; i++)
        if (extras[i] && extras[i]->max_height > max_height)
            max_height = extras[i]->max_height;

    /* Compute layout */
    uint32_t reg_ranges_sz = (uint32_t)reg->n_ranges * 8;
    uint32_t reg_glyphs_sz = (uint32_t)reg->n_glyphs * 10;
    uint32_t kern_offset   = HEADER_SIZE + reg_ranges_sz + reg_glyphs_sz;
    uint32_t cursor        = kern_offset + reg->kern_bytes.size;

    uint32_t style_offsets[3] = {0,0,0};
    for (int i = 0; i < 3; i++) {
        if (!extras[i]) continue;
        style_offsets[i] = cursor;
        cursor += 8 + (uint32_t)extras[i]->n_ranges * 8
                    + (uint32_t)extras[i]->n_glyphs * 10
                    + extras[i]->kern_bytes.size;
    }
    uint32_t bitmap_data_offset = cursor;

    /* Concatenate BW bitmaps: reg first, then extras */
    uint32_t bmp_bases[4] = {0,0,0,0}; /* [0]=reg, [1..3]=extras */
    uint32_t bw_total = reg->bw.size;
    for (int i = 0; i < 3; i++) {
        if (!extras[i]) continue;
        bmp_bases[i+1] = bw_total;
        bw_total += extras[i]->bw.size;
    }

    uint32_t gray_lsb_offset = 0, gray_msb_offset = 0;
    if (!p->bw_only && reg->lsb.size > 0) {
        gray_lsb_offset = bitmap_data_offset + bw_total;
        uint32_t lsb_total = reg->lsb.size;
        for (int i = 0; i < 3; i++) if (extras[i]) lsb_total += extras[i]->lsb.size;
        gray_msb_offset = gray_lsb_offset + lsb_total;
    }

    /* ── Header ── */
    buf_zero(&out, HEADER_SIZE);
    uint8_t *h = out.data;
    wr32(h+0,  0x3446424D);      /* "MBF4" */
    h[4]  = 4;                   /* version */
    h[5]  = (uint8_t)max_height;
    h[6]  = (uint8_t)baseline;
    h[7]  = (uint8_t)y_advance;
    h[8]  = (uint8_t)default_advance;
    h[9]  = style_flags;
    wr16(h+10, (uint16_t)reg->n_ranges);
    wr16(h+12, (uint16_t)reg->n_glyphs);
    wr16(h+14, (uint16_t)p->size);
    wr32(h+16, reg->kern_bytes.size);
    wr32(h+20, bitmap_data_offset);
    wr32(h+24, style_offsets[0]);   /* bold */
    wr32(h+28, style_offsets[1]);   /* italic */
    wr32(h+32, style_offsets[2]);   /* bold_italic */
    wr32(h+36, kern_offset);
    wr32(h+40, gray_lsb_offset);
    wr32(h+44, gray_msb_offset);
    h[48] = (uint8_t)(int8_t)p->ul_pos;
    h[49] = (uint8_t)p->ul_thick;

    /* Regular ranges + glyphs */
    encode_ranges_and_glyphs(&out, reg, bmp_bases[0]);
    /* Regular kerning */
    buf_append(&out, reg->kern_bytes.data, reg->kern_bytes.size);

    /* Extra style sections */
    for (int i = 0; i < 3; i++) {
        if (!extras[i]) continue;
        /* MbfStyleSection: num_ranges(2) num_glyphs(2) kerning_length(4) */
        buf_u16le(&out, (uint16_t)extras[i]->n_ranges);
        buf_u16le(&out, (uint16_t)extras[i]->n_glyphs);
        buf_u32le(&out, extras[i]->kern_bytes.size);
        encode_ranges_and_glyphs(&out, extras[i], bmp_bases[i+1]);
        buf_append(&out, extras[i]->kern_bytes.data, extras[i]->kern_bytes.size);
    }

    /* BW bitmap data */
    buf_append(&out, reg->bw.data, reg->bw.size);
    for (int i = 0; i < 3; i++)
        if (extras[i]) buf_append(&out, extras[i]->bw.data, extras[i]->bw.size);

    /* Grayscale LSB */
    if (!p->bw_only) {
        buf_append(&out, reg->lsb.data, reg->lsb.size);
        for (int i = 0; i < 3; i++)
            if (extras[i]) buf_append(&out, extras[i]->lsb.data, extras[i]->lsb.size);
        /* Grayscale MSB */
        buf_append(&out, reg->msb.data, reg->msb.size);
        for (int i = 0; i < 3; i++)
            if (extras[i]) buf_append(&out, extras[i]->msb.data, extras[i]->msb.size);
    }

    (void)enames;
    return out;
}

/* ── FNTS bundle builder ─────────────────────────────────────────────────── */

static Buf build_fnts(const char *font_name, Buf *mbf_files, int n_files) {
    Buf out; buf_init(&out);

    /* "FNTS" + num + version(2) + reserved(2) + name(32) + sizes[n × 4] */
    buf_append(&out, (const uint8_t*)"FNTS", 4);
    buf_u8(&out, (uint8_t)n_files);
    buf_u8(&out, 2); /* version */
    buf_u16le(&out, 0); /* reserved */

    uint8_t name_buf[32]; memset(name_buf, 0, 32);
    int nlen = (int)strlen(font_name);
    if (nlen > 31) nlen = 31;
    memcpy(name_buf, font_name, nlen);
    buf_append(&out, name_buf, 32);

    for (int i = 0; i < n_files; i++) buf_u32le(&out, mbf_files[i].size);
    for (int i = 0; i < n_files; i++) buf_append(&out, mbf_files[i].data, mbf_files[i].size);

    return out;
}

/* ── Underline metrics via FreeType face (matches freetype-py behavior) ──── */

static void get_underline(FT_Face face, int size, int *ul_pos, int *ul_thick) {
    int upem = (int)face->units_per_EM;
    if (upem <= 0) upem = 1000;
    int pos   = (int)round(-(double)face->underline_position  * size / upem);
    int thick = (int)round( (double)face->underline_thickness * size / upem);
    *ul_pos   = pos   < 1 ? 1 : pos;
    *ul_thick = thick < 1 ? 1 : thick;
}

/* ── get upem from head table ────────────────────────────────────────────── */

static int get_upem(hb_face_t *hb_face) {
    hb_blob_t *blob = hb_face_reference_table(hb_face, HB_TAG('h','e','a','d'));
    uint32_t len = 0;
    const uint8_t *data = (const uint8_t*)hb_blob_get_data(blob, &len);
    int upem = 1000;
    if (data && len >= 20) upem = (int)rd16(data + 18);
    hb_blob_destroy(blob);
    return upem ? upem : 1000;
}

/* ── CLI ─────────────────────────────────────────────────────────────────── */

static void usage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s <regular.ttf> [options]\n"
        "\n"
        "Options:\n"
        "  --size N           Single pixel size (default: 20)\n"
        "  --output FILE      Output file (.mbf single, .mfb bundle)\n"
        "  --bold FILE        Bold TTF/OTF\n"
        "  --italic FILE      Italic TTF/OTF\n"
        "  --bold-italic FILE BoldItalic TTF/OTF\n"
        "  --fallback FILE    Fallback font for missing glyphs\n"
        "  --bw-only          BW 1-bit only (no grayscale planes)\n"
        "  --bundle           Generate FNTS bundle\n"
        "  --bundle-sizes N.. Space-separated sizes for bundle mode\n"
        "  --font-name NAME   Font family name embedded in FNTS header\n"
        "  --ranges NAME..    Unicode range presets (default: ascii latin1 ...)\n"
        "\n"
        "Range presets: ascii latin1 latin-ext-a latin-ext-b latin-ext-add\n"
        "  combining spacing-mod greek cyrillic general-punct super-sub\n"
        "  currency letterlike number-forms arrows ui-shapes specials\n"
        "  hiragana katakana cjk-punct cjk\n",
        argv0);
}

int main(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "--help") == 0) { usage(argv[0]); return 1; }

    const char *font_regular    = argv[1];
    const char *font_bold       = NULL;
    const char *font_italic     = NULL;
    const char *font_bold_italic = NULL;
    const char *font_fallback   = NULL;
    const char *output_path     = NULL;
    const char *font_name       = NULL;
    int bundle_mode   = 0;
    int bw_only       = 0;
    int single_size   = 20;

    int bundle_sizes[8] = {20,24,28,32};
    int n_bundle_sizes  = 4;

    const char *range_names[64];
    int n_range_names = 0;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i],"--bold")==0 && i+1<argc)         font_bold=argv[++i];
        else if (strcmp(argv[i],"--italic")==0 && i+1<argc)  font_italic=argv[++i];
        else if (strcmp(argv[i],"--bold-italic")==0 && i+1<argc) font_bold_italic=argv[++i];
        else if (strcmp(argv[i],"--fallback")==0 && i+1<argc) font_fallback=argv[++i];
        else if (strcmp(argv[i],"--output")==0 && i+1<argc)  output_path=argv[++i];
        else if (strcmp(argv[i],"--font-name")==0 && i+1<argc) font_name=argv[++i];
        else if (strcmp(argv[i],"--size")==0 && i+1<argc)    single_size=atoi(argv[++i]);
        else if (strcmp(argv[i],"--bundle")==0)               bundle_mode=1;
        else if (strcmp(argv[i],"--bw-only")==0)              bw_only=1;
        else if (strcmp(argv[i],"--bundle-sizes")==0) {
            n_bundle_sizes = 0;
            while (i+1 < argc && isdigit((unsigned char)argv[i+1][0]))
                bundle_sizes[n_bundle_sizes++] = atoi(argv[++i]);
        } else if (strcmp(argv[i],"--ranges")==0) {
            while (i+1 < argc && argv[i+1][0] != '-' && n_range_names < 64)
                range_names[n_range_names++] = argv[++i];
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            usage(argv[0]); return 1;
        }
    }

    if (!output_path) { fprintf(stderr, "Need --output\n"); usage(argv[0]); return 1; }
    if (n_range_names == 0) {
        for (int i = 0; kDefaultRanges[i]; i++) range_names[n_range_names++] = kDefaultRanges[i];
    }

    /* Derive font name from regular file if not given */
    if (!font_name) {
        const char *base = strrchr(font_regular, PATH_SEP);
        base = base ? base+1 : font_regular;
        static char fname_buf[256];
        strncpy(fname_buf, base, sizeof(fname_buf)-1);
        /* Strip extension */
        char *dot = strrchr(fname_buf, '.');
        if (dot) *dot = '\0';
        /* Strip trailing style suffixes */
        const char *suffixes[] = {"-Regular","-regular"," Regular",NULL};
        for (int i = 0; suffixes[i]; i++) {
            int sl = (int)strlen(suffixes[i]);
            int nl = (int)strlen(fname_buf);
            if (nl > sl && strcmp(fname_buf+nl-sl, suffixes[i])==0)
                fname_buf[nl-sl] = '\0';
        }
        font_name = fname_buf;
    }

    /* Build CP ranges */
    int n_cp_ranges = 0;
    CPRange *cp_ranges = build_ranges(range_names, n_range_names, &n_cp_ranges);

    int total_cp = 0;
    for (int i = 0; i < n_cp_ranges; i++) total_cp += (int)(cp_ranges[i].hi - cp_ranges[i].lo + 1);
    fprintf(stderr, "Font: %s\nRanges: %d blocks, %d codepoints\n",
            font_regular, n_cp_ranges, total_cp);

    /* ── Load fonts ── */
    FT_Library ft_lib;
    if (FT_Init_FreeType(&ft_lib)) { fputs("FT_Init_FreeType failed\n", stderr); return 1; }

    uint32_t reg_sz = 0, bold_sz = 0, ital_sz = 0, bi_sz = 0, fb_sz = 0;
    uint8_t *reg_data  = read_file(font_regular, &reg_sz);
    uint8_t *bold_data = font_bold        ? read_file(font_bold, &bold_sz)          : NULL;
    uint8_t *ital_data = font_italic      ? read_file(font_italic, &ital_sz)        : NULL;
    uint8_t *bi_data   = font_bold_italic ? read_file(font_bold_italic, &bi_sz)     : NULL;
    uint8_t *fb_data   = font_fallback    ? read_file(font_fallback, &fb_sz)        : NULL;

    if (!reg_data) return 1;

    FT_Face ft_reg=NULL, ft_bold=NULL, ft_ital=NULL, ft_bi=NULL, ft_fb=NULL;
    if (FT_New_Memory_Face(ft_lib, reg_data,  reg_sz,  0, &ft_reg))  { fputs("Bad regular font\n",  stderr); return 1; }
    if (bold_data && FT_New_Memory_Face(ft_lib, bold_data, bold_sz, 0, &ft_bold))  { fputs("Bad bold font\n",    stderr); }
    if (ital_data && FT_New_Memory_Face(ft_lib, ital_data, ital_sz, 0, &ft_ital))  { fputs("Bad italic font\n",  stderr); }
    if (bi_data   && FT_New_Memory_Face(ft_lib, bi_data,   bi_sz,   0, &ft_bi))    { fputs("Bad bold-italic\n",  stderr); }
    if (fb_data   && FT_New_Memory_Face(ft_lib, fb_data,   fb_sz,   0, &ft_fb))    { fputs("Bad fallback font\n",stderr); }

    /* HarfBuzz faces for table access */
    hb_blob_t *hb_reg_blob  = hb_blob_create((const char*)reg_data,  reg_sz,  HB_MEMORY_MODE_READONLY, NULL, NULL);
    hb_blob_t *hb_bold_blob = bold_data ? hb_blob_create((const char*)bold_data, bold_sz, HB_MEMORY_MODE_READONLY, NULL, NULL) : NULL;
    hb_blob_t *hb_ital_blob = ital_data ? hb_blob_create((const char*)ital_data, ital_sz, HB_MEMORY_MODE_READONLY, NULL, NULL) : NULL;
    hb_blob_t *hb_bi_blob   = bi_data   ? hb_blob_create((const char*)bi_data,   bi_sz,   HB_MEMORY_MODE_READONLY, NULL, NULL) : NULL;

    hb_face_t *hb_reg  = hb_face_create(hb_reg_blob,  0);
    hb_face_t *hb_bold = hb_bold_blob ? hb_face_create(hb_bold_blob, 0) : NULL;
    hb_face_t *hb_ital = hb_ital_blob ? hb_face_create(hb_ital_blob, 0) : NULL;
    hb_face_t *hb_bi   = hb_bi_blob   ? hb_face_create(hb_bi_blob,   0) : NULL;

    hb_blob_destroy(hb_reg_blob);
    if (hb_bold_blob) hb_blob_destroy(hb_bold_blob);
    if (hb_ital_blob) hb_blob_destroy(hb_ital_blob);
    if (hb_bi_blob)   hb_blob_destroy(hb_bi_blob);

    int upem = get_upem(hb_reg);

    /* ── Single-size or bundle ── */
    int *sizes     = bundle_mode ? bundle_sizes : &single_size;
    int  n_sizes   = bundle_mode ? n_bundle_sizes : 1;

    Buf *mbf_files = (Buf*)malloc(n_sizes * sizeof(Buf));

    for (int si = 0; si < n_sizes; si++) {
        int sz = sizes[si];
        fprintf(stderr, "\n=== %dpx ===\n", sz);

        int ul_pos, ul_thick;
        get_underline(ft_reg, sz, &ul_pos, &ul_thick);

        fprintf(stderr, "[Regular]\n");
        StyleOut reg_out = render_style(ft_reg, ft_fb, hb_reg,
                                         cp_ranges, n_cp_ranges, sz, bw_only, upem);

        StyleOut bold_out = {0}, ital_out = {0}, bi_out = {0};
        if (ft_bold && hb_bold) {
            fprintf(stderr, "[Bold]\n");
            bold_out = render_style(ft_bold, ft_fb, hb_bold,
                                     cp_ranges, n_cp_ranges, sz, bw_only, upem);
        }
        if (ft_ital && hb_ital) {
            fprintf(stderr, "[Italic]\n");
            ital_out = render_style(ft_ital, ft_fb, hb_ital,
                                     cp_ranges, n_cp_ranges, sz, bw_only, upem);
        }
        if (ft_bi && hb_bi) {
            fprintf(stderr, "[BoldItalic]\n");
            bi_out = render_style(ft_bi, ft_fb, hb_bi,
                                   cp_ranges, n_cp_ranges, sz, bw_only, upem);
        }

        MbfParams params;
        memset(&params, 0, sizeof(params));
        params.regular     = &reg_out;
        params.bold        = ft_bold ? &bold_out : NULL;
        params.italic      = ft_ital ? &ital_out : NULL;
        params.bold_italic = ft_bi   ? &bi_out   : NULL;
        params.size        = sz;
        params.upem        = upem;
        params.bw_only     = bw_only;
        params.ul_pos      = ul_pos;
        params.ul_thick    = ul_thick;
        mbf_files[si] = build_mbf(&params);

        style_free(&reg_out);
        if (ft_bold) style_free(&bold_out);
        if (ft_ital) style_free(&ital_out);
        if (ft_bi)   style_free(&bi_out);

        fprintf(stderr, "  MBF %dpx: %u bytes\n", sz, mbf_files[si].size);
    }

    /* ── Write output ── */
    int ok = 1;
    if (bundle_mode || n_sizes > 1) {
        Buf fnts = build_fnts(font_name, mbf_files, n_sizes);
        /* Ensure .mfb extension */
        char out_path[1024];
        strncpy(out_path, output_path, sizeof(out_path)-5);
        char *dot = strrchr(out_path, '.');
        if (!dot || strcmp(dot, ".mfb") != 0) {
            if (dot) *dot = '\0';
            strcat(out_path, ".mfb");
        }
        ok = write_file(out_path, fnts.data, fnts.size);
        fprintf(stderr, "\nFNTS bundle: %s (%u bytes)\n", out_path, fnts.size);
        buf_free(&fnts);
    } else {
        ok = write_file(output_path, mbf_files[0].data, mbf_files[0].size);
        fprintf(stderr, "\nMBF: %s (%u bytes)\n", output_path, mbf_files[0].size);
    }

    for (int i = 0; i < n_sizes; i++) buf_free(&mbf_files[i]);
    free(mbf_files);
    free(cp_ranges);

    /* Cleanup */
    if (hb_reg)  hb_face_destroy(hb_reg);
    if (hb_bold) hb_face_destroy(hb_bold);
    if (hb_ital) hb_face_destroy(hb_ital);
    if (hb_bi)   hb_face_destroy(hb_bi);
    FT_Done_FreeType(ft_lib);
    free(reg_data); free(bold_data); free(ital_data); free(bi_data); free(fb_data);

    return ok ? 0 : 1;
}

/* ── WASM entry point ────────────────────────────────────────────────────── */
#ifdef __EMSCRIPTEN__
#include <emscripten.h>

/* Called from a Web Worker running our WASM module.
 * All font data pointers are already in the WASM heap (caller owns them — do NOT free).
 * ranges: flat uint32 array [lo0,hi0,lo1,hi1,...], n_range_pairs = number of lo/hi pairs.
 * Returns pointer to FNTS bytes (heap-allocated; JS must call _free on it) and sets *out_len.
 * Returns NULL on error.                                                                    */
EMSCRIPTEN_KEEPALIVE
uint8_t *font_gen_fnts_wasm(
    uint8_t *reg_data,  int reg_len,
    uint8_t *bold_data, int bold_len,
    uint8_t *ital_data, int ital_len,
    uint8_t *bi_data,   int bi_len,
    uint8_t *fb_data,   int fb_len,
    const char *font_name,
    int *sizes,         int n_sizes,
    uint32_t *ranges,   int n_range_pairs,
    int bw_only,
    int *out_len)
{
    *out_len = 0;

    FT_Library ft_lib;
    if (FT_Init_FreeType(&ft_lib)) {
        EM_ASM({ postMessage({type:'log',msg:'FT_Init_FreeType failed'}); });
        return NULL;
    }

    FT_Face ft_reg=NULL, ft_bold=NULL, ft_ital=NULL, ft_bi=NULL, ft_fb=NULL;
    if (FT_New_Memory_Face(ft_lib, reg_data, (FT_Long)reg_len, 0, &ft_reg)) {
        EM_ASM({ postMessage({type:'log',msg:'Failed to load regular font'}); });
        FT_Done_FreeType(ft_lib);
        return NULL;
    }
    if (bold_data && bold_len > 0) FT_New_Memory_Face(ft_lib, bold_data, (FT_Long)bold_len, 0, &ft_bold);
    if (ital_data && ital_len > 0) FT_New_Memory_Face(ft_lib, ital_data, (FT_Long)ital_len, 0, &ft_ital);
    if (bi_data   && bi_len   > 0) FT_New_Memory_Face(ft_lib, bi_data,   (FT_Long)bi_len,   0, &ft_bi);
    if (fb_data   && fb_len   > 0) FT_New_Memory_Face(ft_lib, fb_data,   (FT_Long)fb_len,   0, &ft_fb);

    hb_blob_t *hb_reg_blob  = hb_blob_create((const char*)reg_data, (unsigned)reg_len, HB_MEMORY_MODE_READONLY, NULL, NULL);
    hb_blob_t *hb_bold_blob = (bold_data && bold_len) ? hb_blob_create((const char*)bold_data, (unsigned)bold_len, HB_MEMORY_MODE_READONLY, NULL, NULL) : NULL;
    hb_blob_t *hb_ital_blob = (ital_data && ital_len) ? hb_blob_create((const char*)ital_data, (unsigned)ital_len, HB_MEMORY_MODE_READONLY, NULL, NULL) : NULL;
    hb_blob_t *hb_bi_blob   = (bi_data   && bi_len)   ? hb_blob_create((const char*)bi_data,   (unsigned)bi_len,   HB_MEMORY_MODE_READONLY, NULL, NULL) : NULL;

    hb_face_t *hb_reg  = hb_face_create(hb_reg_blob, 0);
    hb_face_t *hb_bold = hb_bold_blob ? hb_face_create(hb_bold_blob, 0) : NULL;
    hb_face_t *hb_ital = hb_ital_blob ? hb_face_create(hb_ital_blob, 0) : NULL;
    hb_face_t *hb_bi   = hb_bi_blob   ? hb_face_create(hb_bi_blob,   0) : NULL;

    hb_blob_destroy(hb_reg_blob);
    if (hb_bold_blob) hb_blob_destroy(hb_bold_blob);
    if (hb_ital_blob) hb_blob_destroy(hb_ital_blob);
    if (hb_bi_blob)   hb_blob_destroy(hb_bi_blob);

    /* Build CPRange array from flat uint32 pairs */
    CPRange *cp_ranges = (CPRange*)malloc((size_t)n_range_pairs * sizeof(CPRange));
    if (!cp_ranges) { FT_Done_FreeType(ft_lib); return NULL; }
    for (int i = 0; i < n_range_pairs; i++) {
        cp_ranges[i].lo = ranges[i * 2];
        cp_ranges[i].hi = ranges[i * 2 + 1];
    }

    Buf *mbf_files = (Buf*)malloc((size_t)n_sizes * sizeof(Buf));
    if (!mbf_files) { free(cp_ranges); FT_Done_FreeType(ft_lib); return NULL; }

    int upem = get_upem(hb_reg);

    int n_styles = 1 + (ft_bold ? 1 : 0) + (ft_ital ? 1 : 0) + (ft_bi ? 1 : 0);
    int total_steps = n_sizes * n_styles;
    int step = 0;

    for (int si = 0; si < n_sizes; si++) {
        int sz = sizes[si];
        {
            char msg[64];
            snprintf(msg, sizeof(msg), "Rendering %dpx regular...", sz);
            EM_ASM({ postMessage({type:'log',  msg: UTF8ToString($0)}); }, msg);
            EM_ASM({ postMessage({type:'progress', pct:$0, label:UTF8ToString($1)}); },
                   step * 100 / (total_steps ? total_steps : 1), msg);
        }

        int ul_pos, ul_thick;
        get_underline(ft_reg, sz, &ul_pos, &ul_thick);

        StyleOut reg_out = render_style(ft_reg, ft_fb, hb_reg,
                                        cp_ranges, n_range_pairs, sz, bw_only, upem);
        step++;

        StyleOut bold_out = {0}, ital_out = {0}, bi_out = {0};
        if (ft_bold && hb_bold) {
            char msg[64]; snprintf(msg, sizeof(msg), "Rendering %dpx bold...", sz);
            EM_ASM({ postMessage({type:'progress', pct:$0, label:UTF8ToString($1)}); },
                   step * 100 / total_steps, msg);
            bold_out = render_style(ft_bold, ft_fb, hb_bold,
                                    cp_ranges, n_range_pairs, sz, bw_only, upem);
            step++;
        }
        if (ft_ital && hb_ital) {
            char msg[64]; snprintf(msg, sizeof(msg), "Rendering %dpx italic...", sz);
            EM_ASM({ postMessage({type:'progress', pct:$0, label:UTF8ToString($1)}); },
                   step * 100 / total_steps, msg);
            ital_out = render_style(ft_ital, ft_fb, hb_ital,
                                    cp_ranges, n_range_pairs, sz, bw_only, upem);
            step++;
        }
        if (ft_bi && hb_bi) {
            char msg[64]; snprintf(msg, sizeof(msg), "Rendering %dpx bold-italic...", sz);
            EM_ASM({ postMessage({type:'progress', pct:$0, label:UTF8ToString($1)}); },
                   step * 100 / total_steps, msg);
            bi_out = render_style(ft_bi, ft_fb, hb_bi,
                                  cp_ranges, n_range_pairs, sz, bw_only, upem);
            step++;
        }

        MbfParams params;
        memset(&params, 0, sizeof(params));
        params.regular     = &reg_out;
        params.bold        = ft_bold ? &bold_out : NULL;
        params.italic      = ft_ital ? &ital_out : NULL;
        params.bold_italic = ft_bi   ? &bi_out   : NULL;
        params.size        = sz;
        params.bw_only     = bw_only;
        params.ul_pos      = ul_pos;
        params.ul_thick    = ul_thick;
        mbf_files[si] = build_mbf(&params);

        style_free(&reg_out);
        if (ft_bold) style_free(&bold_out);
        if (ft_ital) style_free(&ital_out);
        if (ft_bi)   style_free(&bi_out);

        {
            char msg[64];
            snprintf(msg, sizeof(msg), "MBF %dpx: %u bytes", sz, mbf_files[si].size);
            EM_ASM({ postMessage({type:'log', msg:UTF8ToString($0)}); }, msg);
        }
    }

    EM_ASM({ postMessage({type:'progress', pct:95, label:'Building FNTS bundle...'}); });
    Buf result = build_fnts(font_name, mbf_files, n_sizes);

    for (int i = 0; i < n_sizes; i++) buf_free(&mbf_files[i]);
    free(mbf_files);
    free(cp_ranges);

    if (hb_reg)  hb_face_destroy(hb_reg);
    if (hb_bold) hb_face_destroy(hb_bold);
    if (hb_ital) hb_face_destroy(hb_ital);
    if (hb_bi)   hb_face_destroy(hb_bi);
    if (ft_reg)  FT_Done_Face(ft_reg);
    if (ft_bold) FT_Done_Face(ft_bold);
    if (ft_ital) FT_Done_Face(ft_ital);
    if (ft_bi)   FT_Done_Face(ft_bi);
    if (ft_fb)   FT_Done_Face(ft_fb);
    FT_Done_FreeType(ft_lib);

    {
        char msg[80];
        snprintf(msg, sizeof(msg), "FNTS bundle: %u bytes", result.size);
        EM_ASM({ postMessage({type:'log', msg:UTF8ToString($0)}); }, msg);
    }

    *out_len = (int)result.size;
    return result.data; /* Caller must call _free() on this */
}

EMSCRIPTEN_KEEPALIVE
uint8_t *font_gen_preview_wasm(
    uint8_t *reg_data,  int reg_len,
    uint8_t *bold_data, int bold_len,
    uint8_t *ital_data, int ital_len,
    uint8_t *bi_data,   int bi_len,
    uint8_t *fb_data,   int fb_len,
    const char *preview_text,
    int *sizes,         int n_sizes,
    uint32_t *ranges,   int n_range_pairs,
    int bw_only,
    int *out_len)
{
    *out_len = 0;
    if (!reg_data || reg_len <= 0 || !sizes || n_sizes <= 0) return NULL;

    FT_Library ft_lib;
    if (FT_Init_FreeType(&ft_lib)) return NULL;

    FT_Face ft_reg = NULL, ft_bold = NULL, ft_ital = NULL, ft_bi = NULL, ft_fb = NULL;
    if (FT_New_Memory_Face(ft_lib, reg_data, (FT_Long)reg_len, 0, &ft_reg)) {
        FT_Done_FreeType(ft_lib);
        return NULL;
    }
    if (bold_data && bold_len > 0)
        FT_New_Memory_Face(ft_lib, bold_data, (FT_Long)bold_len, 0, &ft_bold);
    if (ital_data && ital_len > 0)
        FT_New_Memory_Face(ft_lib, ital_data, (FT_Long)ital_len, 0, &ft_ital);
    if (bi_data && bi_len > 0)
        FT_New_Memory_Face(ft_lib, bi_data, (FT_Long)bi_len, 0, &ft_bi);
    if (fb_data && fb_len > 0) {
        FT_New_Memory_Face(ft_lib, fb_data, (FT_Long)fb_len, 0, &ft_fb);
    }

    CPRange *cp_ranges = (CPRange*)malloc((size_t)n_range_pairs * sizeof(CPRange));
    if (!cp_ranges) {
        if (ft_reg) FT_Done_Face(ft_reg);
        if (ft_bold) FT_Done_Face(ft_bold);
        if (ft_ital) FT_Done_Face(ft_ital);
        if (ft_bi) FT_Done_Face(ft_bi);
        if (ft_fb) FT_Done_Face(ft_fb);
        FT_Done_FreeType(ft_lib);
        return NULL;
    }
    for (int i = 0; i < n_range_pairs; i++) {
        cp_ranges[i].lo = ranges[i * 2];
        cp_ranges[i].hi = ranges[i * 2 + 1];
    }

    PreviewSection *sections = (PreviewSection*)calloc((size_t)n_sizes, sizeof(PreviewSection));
    int n_styles = 1 + (ft_bold ? 1 : 0) + (ft_ital ? 1 : 0) + (ft_bi ? 1 : 0);
    free(sections);
    sections = (PreviewSection*)calloc((size_t)(n_sizes * n_styles), sizeof(PreviewSection));
    if (!sections) {
        free(cp_ranges);
        if (ft_reg) FT_Done_Face(ft_reg);
        if (ft_bold) FT_Done_Face(ft_bold);
        if (ft_ital) FT_Done_Face(ft_ital);
        if (ft_bi) FT_Done_Face(ft_bi);
        if (ft_fb) FT_Done_Face(ft_fb);
        FT_Done_FreeType(ft_lib);
        return NULL;
    }

    int section_count = 0;
    for (int i = 0; i < n_sizes; i++) {
        sections[section_count++] = build_device_preview_section(
            ft_reg, ft_fb, cp_ranges, n_range_pairs, 0, sizes[i], bw_only, preview_text);
        if (ft_bold) {
            sections[section_count++] = build_device_preview_section(
                ft_bold, ft_fb, cp_ranges, n_range_pairs, 1, sizes[i], bw_only, preview_text);
        }
        if (ft_ital) {
            sections[section_count++] = build_device_preview_section(
                ft_ital, ft_fb, cp_ranges, n_range_pairs, 2, sizes[i], bw_only, preview_text);
        }
        if (ft_bi) {
            sections[section_count++] = build_device_preview_section(
                ft_bi, ft_fb, cp_ranges, n_range_pairs, 3, sizes[i], bw_only, preview_text);
        }
    }

    Buf result = build_preview_bundle(sections, section_count);

    for (int i = 0; i < section_count; i++) free(sections[i].pixels);
    free(sections);
    free(cp_ranges);
    if (ft_reg) FT_Done_Face(ft_reg);
    if (ft_bold) FT_Done_Face(ft_bold);
    if (ft_ital) FT_Done_Face(ft_ital);
    if (ft_bi) FT_Done_Face(ft_bi);
    if (ft_fb) FT_Done_Face(ft_fb);
    FT_Done_FreeType(ft_lib);

    *out_len = (int)result.size;
    return result.data;
}
#endif /* __EMSCRIPTEN__ */
