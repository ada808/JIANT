/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Small-canvas renderer (no full framebuffer). Draw text/lines into
 * an off-screen strip, then blit it in one DMA transfer. Pixels are stored
 * byte-swapped (the panel takes RGB565 big-endian).
 * Text and icons are 4-bit alpha bitmaps made at build time (tools/gen_aa_font.py: Inter Tight;
 * tools/gen_aa_icons.py: Fukiai). A glyph is trimmed to its ink box, advances are in 1/16 px, and
 * each glyph is rasterised at 1 << psh horizontal phases (S M 4, L 2: drawn at a pen of +0, +1/4 .. px);
 * cv_text takes the phase that puts it within a phase step of its true place. Blending uses a 16-entry
 * ramp per (ink, background) pair, cached, so a pixel costs a nibble read and a store. Colours come from the theme tokens. */
typedef struct { uint16_t off, adv; int8_t bx; uint8_t by, bw, bh; } aag_t;   /* a glyph at a phase (ag) */
typedef struct { uint16_t off, adv; } aagh_t;   /* (0.9.0) a glyph: its phases' lowest data offset, the advance */
typedef struct {
    uint8_t h, asc, first, last, nex;   /* line height, baseline; ASCII range; extras */
    uint16_t nk;                        /* kerning: the glyphs ki covers (0: none) */
    const aagh_t *gh;                   /* (0.9.0) the glyph table, packed (tools/aa_raster.py glyph_tables): a glyph's */
    const uint32_t *gp;                 /* .. and each phase's off - gh.off | bx + 1 << 10 | by << 13 | bw << 19 | bh << 26 */
    const uint8_t *data;
    const uint16_t *ki;                 /* (0.9.0) glyph a's pairs: kp[2 ki[a] .. 2 ki[a + 1] - 1] */
    const uint8_t *kp;                  /* .. each b (sorted) and the delta (int8, 1/16 px) */
    const uint8_t *ex;                  /* codes of the extra glyphs */
    uint8_t psh;                        /* log2 of the phases per glyph: entries i << psh .. (i << psh) + phases - 1 */
    const uint8_t *hc;                  /* data's Huffman code (cv_alpha_hc), 0 = 2 px per byte (cv_alpha) */
    uint8_t bits;                       /* (0.8.5) 1: data is 1 bit a pixel, no ramp (AF_X: cv_bits) */
} aafont_t;
typedef struct { const char *name; uint16_t bg, surf, text, theme, accent; } ui_pal_t;
typedef struct { uint16_t off; uint8_t w; const char *label; } kc_t;
#include "ui_fonts.h"                   /* AF_S 12 px / 400, AF_M 15 px / 500, AF_L 28 px / 600 */
#include "ui_font_x.h"                  /* (0.8.5) AF_X: the 5 x 7 mono pixel face (tools/gen_px_font.py) */
#include "ui_palettes.h"
#include "ui_keycaps.h"                 /* KC_*: the keycaps / badges (tools/gen_aa_keycaps.py), KNOB_* arcs */
#define ELLIPSIS '\x85'                 /* the ellipsis glyph of AF_S and AF_M */

/* host tests hook in here (layout lint, draw cost); nothing in the firmware */
#ifndef GFX_HOOK_TEXT
#define GFX_HOOKS 0                     /* (the firmware: what only the hooks read is not even computed) */
#define GFX_HOOK_TEXT(x0, y0, x1, y1, s, flags) ((void)0)   /* an ink box; flags 1 ellipsised, 2 cut, 4 icon, 8 free text, 32 in a scrolled view */
#define GFX_HOOK_BLIT(x, y, r0) ((void)0)
#define GFX_HOOK_BEGIN() ((void)0)
#define GFX_HOOK_PIXELS(n) ((void)0)
#endif
#ifndef GFX_HOOK_SENT
#define GFX_HOOK_SENT(n) ((void)0)                   /* (0.9.0) pixels sent to the panel (the harness's LCD cost) */
#endif
#ifndef GFX_HOOKS
#define GFX_HOOKS 1
#endif
#ifndef GFX_HOOK_CELL
#define GFX_HOOK_CELL(x0, y0, x1, y1) ((void)0)    /* a cell, card, row or button (cv_rrect): a box touching it lies inside it */
#endif
#ifndef GFX_HOOK_INK
#define GFX_HOOK_INK(ink, bg) ((void)0)            /* a text, icon or keycap label's ink and what it is drawn on */
#endif
#ifndef GFX_HOOK_COVER
#define GFX_HOOK_COVER(x0, y0, x1, y1) ((void)0)   /* (0.9.5) an overlay (a toast's pill) hides what its canvas holds there */
#endif
#ifndef GFX_HOOK_RULE
#define GFX_HOOK_RULE(x, y, w, h) ((void)0)        /* a divider drawn on the screen (ui_draw.c lcd_rule): no box may touch it */
#endif
/* the alignment check (tests/ui_render.c): GFX_HOOK_ALIGN declares that the next item(s) drawn (a text, an icon, a
 * keycap, a cushion) are meant to sit centred in the box x0..x1, y0..y1 (canvas drawing coordinates, x1 y1 past it)
 * along AL_H and / or AL_V; AL_N(n): the next n items together. A text's ink is measured across (its glyphs' boxes)
 * and its capitals' band up and down (GFX_HOOK_PEN: its line top and face); an icon's ink from its cell
 * (GFX_HOOK_ITEM). The tag names the place in the report. */
#ifndef GFX_HOOK_ALIGN
#define GFX_HOOK_ALIGN(x0, y0, x1, y1, mode, tag) ((void)0)
#define GFX_HOOK_PEN(y, f) ((void)0)
#define GFX_HOOK_ITEM(x0, y0, x1, y1) ((void)0)
#define GFX_HOOK_ICON(x, y, size, id) ((void)0)        /* (an icon drawn: GFX_HOOK_ITEM with its ink, icons.c icon_ink) */
#endif
#define AL_H 1u                                    /* centred across */
#define AL_V 2u                                    /* centred up and down (a row's items: on the same centre line) */
#define AL_HV 3u
#define AL_B 4u                                    /* on the baseline y1 (a text: its capitals' bottom) */
#define AL_R 8u                                    /* the ink ending at x1 */
#define AL_PASS 16u                                /* (its item counts for the place declared before it too) */
#define AL_CELLS 32u                               /* the next cells (cv_rrect), not ink: a row of keys, a fill */
#define AL_N(n) ((uint32_t)(n) << 8)

#define CV_MAX (240u * 150u)      /* the graph strip: 240 x 150 (0.8.5: a lens page's panel, 140; was 124; 0.8.9 HUD's HOME:
                                   * its middle, 150) */
static uint16_t cv_px[CV_MAX] __attribute__((section(".pool"), aligned(4)));
static uint32_t cv_w, cv_h;
static uint16_t cv_bg;           /* what cv_begin cleared the canvas to */
static uint8_t cv_scroll;        /* 1: drawing a scrolled view (its text may run past the canvas) */
static int32_t cv_oy;            /* y offset for graph drawing */
static int16_t cv_cy0, cv_cy1;   /* text clip: canvas rows cv_cy0 .. cv_cy1 - 1 (cv_begin: the whole canvas) */
static uint8_t cv_flip;          /* cv_alpha_hc: the cell mirrored left <-> right (the header's metronome, ui_draw.c) */
/* (0.9.5) the texts drawn on the canvas since cv_begin (their ink boxes, canvas rows: x0 y0 x1 y1), so an overlay (ui_graph.c
 * toast_pill) can keep clear of them; past CV_NTXT the last one grows to take the rest in */
#define CV_NTXT 32u
static int16_t cv_txt[CV_NTXT][4] __attribute__((section(".pool")));
static uint8_t cv_ntxt __attribute__((section(".pool")));
static uint8_t cv_halo __attribute__((section(".pool")));   /* (0.8.9) SKIN HUD: text drawn with a 1 px BG outline under it
                                 * (cv_text_flags), over a picture (the pool: the audio's data stays where it was) */

/* (0.9.0) the outline's offsets: every pixel within 2 px (dx^2 + dy^2 <= 5), as the 2 px halo of HUD's words over the beings
 * (the 0.8.9 1 px one left them hard to read on a busy picture) */
#define HALO_N 20u
static const int8_t HALO_X[HALO_N] = {-1, 0, 1, -2, -1, 0, 1, 2, -2, -1, 1, 2, -2, -1, 0, 1, 2, -1, 0, 1};
static const int8_t HALO_Y[HALO_N] = {-2, -2, -2, -1, -1, -1, -1, -1, 0, 0, 0, 0, 1, 1, 1, 1, 1, 2, 2, 2};

#define RGB(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))

/* ------------------------------------------------------------- theme --- */
/* Five stored colours per palette, the rest derived with fixed blends (tools/gen_ui_palettes.py) */
static struct {
    uint16_t bg, surf, text, theme, accent;
    uint16_t mid, dim, line, sel, tint, ink, rec, raise, key, lane, grid;
    uint16_t k[4];               /* (0.8.9) KNOB 1..4's colours (tools/gen_ui_palettes.py KNOBS): the skins' cards, as 0.6 */
    uint8_t light, mono;
    uint8_t chamfer;             /* (JIANT 0.5) the JIANT palette: corners cut on the diagonal (cv_rrect), not rounded */
    uint8_t style;               /* MENU > STYLE (ui.c style_apply): ST_FLAT, ST_LINE */
    uint8_t skin;                /* (0.8.9) MENU > SKIN (ui_draw.c style_apply): SKIN_COLORS, SKIN_BOTTOM, SKIN_HUD, SKIN_AIR */
    uint32_t gen;                /* bumped by palette_set (the text ramps follow) */
} ux;
/* STYLE: FLAT, the filled SURF cards and panels; LINE, no SURF (it is BG) and 1 px T_RULE dividers between the
 * areas instead (ui_draw.c draw_frame). (1.0.2: PIXEL, LINE un-antialiased, retired; a saved PIXEL reads as LINE.)
 * Selections, accents, keys and gauges keep their fills in every style */
#define ST_FLAT 0u
#define ST_LINE 1u
/* (0.8.9) SKIN (the approved mock-up opciones_skin.html): COLORS, the lens's cards and panel in the 0.6 colours; BOTTOM, the
 * knobs a slim row at the foot and the middle free (no panel, no boxes: SURF is BG); HUD, the picture full screen and the
 * words floating over it. (0.9.7) AIR (the default of new settings): HUD's look everywhere, the knobs' row on top, right
 * under the tabs, the middle free for the pictures, the breadcrumb at the foot (stored 3: older settings keep theirs) */
#define SKIN_COLORS 0u
#define SKIN_BOTTOM 1u
#define SKIN_HUD 2u
#define SKIN_AIR 3u
#define SKIN_N 4u
static int skin_hud(void) { return ux.skin == SKIN_HUD || ux.skin == SKIN_AIR; }   /* (0.9.7) HUD's look: no rules, words float */
#define T_K(c) ux.k[(c) & 3u]    /* KNOB c's colour */
#define T_RULE ux.dim            /* LINE: the dividers (DIM: visible on NIGHT, GREY, MONO and PAPER, quieter than labels) */
#define T_BG ux.bg               /* background */
#define T_SURF ux.surf           /* dialogs, menu rows, mixer strips */
#define T_TEXT ux.text           /* primary text */
#define T_THEME ux.theme         /* values, curves, the gauge's end line */
#define T_ACCENT ux.accent       /* the one active thing: hot knob, cursor, playhead */
#define T_MID ux.mid             /* labels, units, secondary text */
#define T_DIM ux.dim             /* inactive, empty */
#define T_LINE ux.line           /* 1 px dividers, gauge track */
#define T_SEL ux.sel             /* selection and gauge fill */
#define T_TINT ux.tint           /* a faint theme area */
#define T_INK ux.ink             /* text on T_SEL / T_THEME */
#define T_REC ux.rec             /* REC: fixed red (GREY, MONO: the accent) */
#define T_RAISE ux.raise         /* a raised area on a surface: stubs, guides, slots, chips, button wells */
#define T_KEY ux.key             /* a keycap's fill (its label: T_INK; unavailable: a T_DIM fill) */
#define T_LANE ux.lane           /* the piano roll: an in-scale row (SURF -> THEME 10 %) */
#define T_GRID ux.grid           /* the piano roll: a step line (SURF -> TEXT 6 %; beats and C rows: RAISE) */
#define NPALETTES UI_NPALETTES

static inline uint16_t ux_gray(uint32_t x5) { return (uint16_t)((x5 << 11) | (x5 << 6) | x5); }
/* a + (b - a) * pct / 100 per channel, rounded; GREY on the red channel, so it stays gray */
static uint16_t ux_mix(uint16_t a, uint16_t b, int32_t pct)
{
    static const uint8_t SH[3] = {11, 5, 0}, MK[3] = {31, 63, 31};
    uint32_t out = 0, k;
    if (ux.mono) {
        int32_t x = a >> 11, d = ((b >> 11) - x) * pct;
        return ux_gray((uint32_t)(x + (d + (d >= 0 ? 50 : -50)) / 100));
    }
    for (k = 0; k < 3u; k++) {
        int32_t x = (a >> SH[k]) & MK[k], d = (((b >> SH[k]) & MK[k]) - x) * pct;
        out |= (uint32_t)(x + (d + (d >= 0 ? 50 : -50)) / 100) << SH[k];
    }
    return (uint16_t)out;
}
static uint32_t ux_luma(uint16_t c)        /* 0..255 */
{
    return ((c >> 11) * 255u / 31u * 54u + ((c >> 5) & 63u) * 255u / 63u * 183u + (c & 31u) * 255u / 31u * 19u) >> 8;
}

static void palette_set(uint32_t i)
{
    const ui_pal_t *p = &UI_PALETTES[i % NPALETTES];
    uint32_t k;
    i %= NPALETTES;
    ux.mono = i == UI_GREY_INDEX;
    ux.chamfer = i == UI_JIANT_INDEX;
    ux.bg = p->bg; ux.surf = ux.style || ux.skin ? p->bg : p->surf; ux.text = p->text; ux.theme = p->theme; ux.accent = p->accent;
    ux.mid = ux_mix(p->bg, p->text, UI_MID_PCT);
    ux.dim = ux_mix(p->bg, p->text, UI_DIM_PCT);
    ux.line = ux_mix(p->bg, p->text, UI_LINE_PCT);
    ux.sel = ux_mix(p->bg, p->theme, UI_SEL_PCT);
    ux.tint = ux_mix(p->bg, p->theme, UI_TINT_PCT);
    ux.ink = p->bg;
    ux.raise = ux_mix(p->surf, p->text, UI_RAISE_OF[i]);
    for (k = 0; k < 4u; k++)
        ux.k[k] = UI_KNOBS[i][k];
    ux.key = ux_mix(p->bg, p->text, UI_KEY_PCT);
    ux.lane = ux_mix(ux.surf, p->theme, 10);
    ux.grid = ux_mix(ux.surf, p->text, 6);
    ux.light = ux_luma(p->bg) > 128u;
    ux.rec = ux.mono ? p->accent : ux.light ? UI_REC_LIGHT : UI_REC_DARK;
    if (i == UI_BW_INDEX) {                      /* MONO: black and white, set, not blended (tools/gen_ui_palettes.py) */
        ux.mid = ux.sel = ux.key = ux.rec = p->text;   /* a selection, a keycap, REC: white, its INK black */
        ux.dim = ux.line = ux.raise = ux.lane = UI_BW_GREY;   /* the one grey: inactive, tracks, wells */
        ux.tint = ux.grid = p->bg;
    }
    ux.gen++;                                    /* the text ramps change with the coverage curve */
}

/* settings store a tagged id; ids below 20 are the palettes of earlier firmware */
static uint32_t palette_from_stored(uint32_t v)
{
    if (v >= UI_PAL_TAG && v - UI_PAL_TAG < NPALETTES) return v - UI_PAL_TAG;
    return v < 20u ? UI_PALETTE_MIGRATE[v] : UI_GREY_INDEX;
}
static uint32_t palette_to_stored(uint32_t i) { return UI_PAL_TAG + i % NPALETTES; }
static int palette_stored_ok(uint32_t v) { return v < 20u || (v >= UI_PAL_TAG && v - UI_PAL_TAG < NPALETTES); }

static inline uint16_t swap16(uint32_t c) { return (uint16_t)(((c >> 8) & 0xFFu) | ((c & 0xFFu) << 8)); }

/* ------------------------------------------------------------ canvas --- */
static void cv_begin(uint32_t w, uint32_t h, uint16_t bg)
{
    uint32_t i, n;
    uint16_t s = swap16(bg);
    if (w * h > CV_MAX)
        h = CV_MAX / w;
    lcd_sync();                     /* the last blit may still read cv_px */
    GFX_HOOK_BEGIN();
    cv_w = w;
    cv_h = h;
    cv_cy0 = 0;
    cv_cy1 = (int16_t)h;
    cv_bg = bg;
    cv_ntxt = 0;
    n = w * h;
    for (i = 0; i < n; i++)
        cv_px[i] = s;
}

/* (0.9.5) holes in the screen: while a dialog's box is up over a live page (ui_draw.c ui_draw_page), what the page sends
 * there is left out (a band at a time round each hole), so the page goes on drawing and moving round the box and nothing
 * blacks it out. No hole (the rest of the time): straight to the panel */
#define SCR_HOLES 3u
static int16_t scr_hole[SCR_HOLES][4];  /* x, y, w, h */
static uint8_t scr_nhole;
static void scr_holes(const int16_t (*r)[4], uint32_t n)
{
    uint32_t i;
    scr_nhole = (uint8_t)(n < SCR_HOLES ? n : SCR_HOLES);
    for (i = 0; i < scr_nhole; i++) {
        scr_hole[i][0] = r[i][0]; scr_hole[i][1] = r[i][1]; scr_hole[i][2] = r[i][2]; scr_hole[i][3] = r[i][3];
    }
}
/* x, y, w, h less holes k..: the parts sent (px: rows of stride pixels; else a fill of c) */
static void scr_out(int32_t x, int32_t y, int32_t w, int32_t h, const uint16_t *px, uint32_t stride, uint16_t c, uint32_t k)
{
    int32_t hx, hy, hx1, hy1, y0, y1, r;
    if (w <= 0 || h <= 0)
        return;
    if (k >= scr_nhole) {
        if (!px)
            lcd_fill((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h, c);
        else if ((uint32_t)w == stride)
            lcd_blit((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h, px);
        else
            for (r = 0; r < h; r++)
                lcd_blit((uint32_t)x, (uint32_t)(y + r), (uint32_t)w, 1u, px + (uint32_t)r * stride);
        return;
    }
    hx = scr_hole[k][0]; hy = scr_hole[k][1]; hx1 = hx + scr_hole[k][2]; hy1 = hy + scr_hole[k][3];
    if (hx >= x + w || hx1 <= x || hy >= y + h || hy1 <= y) {
        scr_out(x, y, w, h, px, stride, c, k + 1u);
        return;
    }
    y0 = hy > y ? hy : y;
    y1 = hy1 < y + h ? hy1 : y + h;
    scr_out(x, y, w, y0 - y, px, stride, c, k + 1u);                                   /* above it */
    scr_out(x, y1, w, y + h - y1, px ? px + (uint32_t)(y1 - y) * stride : 0, stride, c, k + 1u);   /* below */
    if (hx > x)                                                                         /* left of it */
        scr_out(x, y0, hx - x, y1 - y0, px ? px + (uint32_t)(y0 - y) * stride : 0, stride, c, k + 1u);
    if (hx1 < x + w)                                                                    /* right */
        scr_out(hx1, y0, x + w - hx1, y1 - y0, px ? px + (uint32_t)(y0 - y) * stride + (uint32_t)(hx1 - x) : 0, stride, c, k + 1u);
}
static void scr_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c)
{
    if (!scr_nhole) lcd_fill(x, y, w, h, c);
    else scr_out((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h, 0, 0u, c, 0u);
}
static void scr_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px)
{
    if (!scr_nhole) lcd_blit(x, y, w, h, px);
    else scr_out((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h, px, w, 0u, 0u);
}
#ifdef LCD_HAS_STRIDE
static void scr_blit_stride(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px, uint32_t stride)
{
    if (!scr_nhole) lcd_blit_stride(x, y, w, h, px, stride);
    else scr_out((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h, px, stride, 0u, 0u);
}
#define lcd_blit_stride scr_blit_stride
#endif
#define lcd_fill scr_fill               /* (everything after: through the holes) */
#define lcd_blit scr_blit

static uint8_t dc_stale;          /* (0.9.0) a strip went to the panel outside cv_blit_cells: the cells' hashes are stale */
static void cv_blit(uint32_t x, uint32_t y)
{
    GFX_HOOK_BLIT(x, y, 0u);
    GFX_HOOK_SENT(cv_w * cv_h);
    dc_stale = 1;
    lcd_blit(x, y, cv_w, cv_h, cv_px);
}

/* canvas rows r0 .. cv_h-1 only, to screen row y + r0 */
static void cv_blit_from(uint32_t x, uint32_t y, uint32_t r0)
{
    GFX_HOOK_BLIT(x, y, r0);
    dc_stale = 1;
    if (r0 < cv_h) {
        GFX_HOOK_SENT(cv_w * (cv_h - r0));
        lcd_blit(x, y + r0, cv_w, cv_h - r0, cv_px + r0 * cv_w);
    }
}

/* (0.9.0) SKIN HUD's HOME (ui_draw.c ui_draw_page): every strip is drawn whole each frame, the beings swimming under all of
 * them, but only what changed goes to the panel (at 12 MHz the whole screen took ~77 ms, the UI ~12 frames a second). Each
 * strip is cut in 8 x 4 cells, a hash of each kept; the runs of cells whose hash changed are sent (cv_blit_cells), a row
 * of them a window. A frame drawn in full (dc_frame(1): ui.force, a page change, a wake, a blit outside the cells) sends
 * them all. Exact by construction: a cell goes out when any of its pixels changed (a 32-bit hash: a change missed about once
 * in 4e9 changed cells, until that cell changes again) */
#ifndef DC_CW
#define DC_CW 8u                        /* a cell: 8 x 4 (8 x 8 sent ~15 % more, 4 x 4 wants twice the hashes) */
#define DC_CH 4u
#endif
#define DC_MAX 1920u                    /* cells: HUD's HOME has 1878 (header 180, tabs 150, the panel 1140, the knobs 4 x 72,
                                         * the footer 120): 7.5 kB of the pool */
#define DC_STRIPS 10u
static uint32_t dc_hash[DC_MAX] __attribute__((section(".pool")));
static struct { int16_t x, y, w, h; uint16_t base, n; } dc_st[DC_STRIPS] __attribute__((section(".pool")));
static uint8_t dc_n __attribute__((section(".pool")));
static uint8_t dc_all __attribute__((section(".pool")));
#ifndef LCD_HAS_STRIDE
static void lcd_blit_stride(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px, uint32_t stride)
{
    uint32_t r;
    for (r = 0; r < h; r++)
        lcd_blit(x, y + r, w, 1u, px + r * stride);
}
#endif
static void dc_frame(int all)           /* a frame of cells begins; all: every cell sent (the strips found again) */
{
    dc_all = (uint8_t)(all || dc_stale);
    dc_stale = 0;
    if (dc_all)
        dc_n = 0;
}
static void dc_send(uint32_t x, uint32_t y, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h)
{
    GFX_HOOK_SENT(w * h);
    if (w == cv_w)
        lcd_blit(x + sx, y + sy, w, h, cv_px + sy * cv_w);
    else
        lcd_blit_stride(x + sx, y + sy, w, h, cv_px + sy * cv_w + sx, cv_w);
}
static uint32_t dc_cell(uint32_t cx, uint32_t cy)     /* the hash of canvas cell cx, cy */
{
    uint32_t x0 = cx * DC_CW, y0 = cy * DC_CH, w = cv_w - x0 < DC_CW ? cv_w - x0 : DC_CW;
    uint32_t h = cv_h - y0 < DC_CH ? cv_h - y0 : DC_CH, r, k, hs = 2166136261u;
    for (r = 0; r < h; r++) {
        const uint16_t *p = cv_px + (y0 + r) * cv_w + x0;
        for (k = 0; k < w; k++)
            hs = (hs ^ p[k]) * 16777619u;
    }
    return hs;
}
static void cv_blit_cells(uint32_t x, uint32_t y)
{
    uint32_t i, ncx = (cv_w + DC_CW - 1u) / DC_CW, ncy = (cv_h + DC_CH - 1u) / DC_CH, cx, cy, base;
    uint32_t *hp;
    GFX_HOOK_BLIT(x, y, 0u);                            /* (the screen shows the whole strip after it) */
    for (i = 0; i < dc_n; i++)
        if (dc_st[i].x == (int16_t)x && dc_st[i].y == (int16_t)y && dc_st[i].w == (int16_t)cv_w && dc_st[i].h == (int16_t)cv_h)
            break;
    if (i == dc_n) {                                    /* a strip not seen since the last full frame: its cells, all sent */
        base = dc_n ? dc_st[dc_n - 1u].base + dc_st[dc_n - 1u].n : 0u;
        if (dc_n == DC_STRIPS || base + ncx * ncy > DC_MAX) {
            GFX_HOOK_SENT(cv_w * cv_h);
            dc_stale = 1;                               /* (no room: sent whole, the next frame all) */
            lcd_blit(x, y, cv_w, cv_h, cv_px);
            return;
        }
        dc_st[i].x = (int16_t)x; dc_st[i].y = (int16_t)y; dc_st[i].w = (int16_t)cv_w; dc_st[i].h = (int16_t)cv_h;
        dc_st[i].base = (uint16_t)base;
        dc_st[i].n = (uint16_t)(ncx * ncy);
        dc_n++;
    } else if (!dc_all) {
        hp = dc_hash + dc_st[i].base;
        for (cy = 0; cy < ncy; cy++) {                  /* each row of cells: its runs of changed cells, a window each */
            uint32_t y0 = cy * DC_CH, h = cv_h - y0 < DC_CH ? cv_h - y0 : DC_CH, run = 0xFFFFu;
            for (cx = 0; cx <= ncx; cx++) {
                uint32_t ch = 0;
                if (cx < ncx) {
                    uint32_t hs = dc_cell(cx, cy);
                    ch = hp[cy * ncx + cx] != hs;
                    hp[cy * ncx + cx] = hs;
                }
                if (ch && run == 0xFFFFu)
                    run = cx;
                else if (!ch && run != 0xFFFFu) {
                    uint32_t e = cx * DC_CW < cv_w ? cx * DC_CW : cv_w;
                    dc_send(x, y, run * DC_CW, y0, e - run * DC_CW, h);
                    run = 0xFFFFu;
                }
            }
        }
        return;
    }
    hp = dc_hash + dc_st[i].base;                       /* every cell: hashed, the strip sent whole */
    for (cy = 0; cy < ncy; cy++)
        for (cx = 0; cx < ncx; cx++)
            hp[cy * ncx + cx] = dc_cell(cx, cy);
    dc_send(x, y, 0u, 0u, cv_w, cv_h);
}

static inline void cv_pset(int32_t x, int32_t y, uint16_t c)
{
    y += cv_oy;
    if ((uint32_t)x < cv_w && (uint32_t)y < cv_h)
        cv_px[(uint32_t)y * cv_w + (uint32_t)x] = swap16(c);
}

static void cv_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c)
{
    int32_t x1 = x + w, y1 = y + h + cv_oy, i;
    uint16_t sc = swap16(c);
    y += cv_oy;                             /* clipped once, then filled row by row */
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (x1 > (int32_t)cv_w)
        x1 = (int32_t)cv_w;
    if (y1 > (int32_t)cv_h)
        y1 = (int32_t)cv_h;
    for (; y < y1; y++)
        for (i = x; i < x1; i++)
            cv_px[(uint32_t)y * cv_w + (uint32_t)i] = sc;
}

/* a divider (LINE): a 1 px T_RULE line on the canvas, a cell edge for the host lint */
static void cv_rule(int32_t x, int32_t y, int32_t w, int32_t h)
{
    GFX_HOOK_CELL(x, y + cv_oy, x + w, y + h + cv_oy);
    cv_rect(x, y, w, h, T_RULE);
}

/* a 1 px outline */
static void cv_frame(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c)
{
    cv_rect(x, y, w, 1, c);
    cv_rect(x, y + h - 1, w, 1, c);
    cv_rect(x, y, 1, h, c);
    cv_rect(x + w - 1, y, 1, h, c);
}

static void cv_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint16_t c)
{
    int32_t dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int32_t dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int32_t err = dx + dy, guard = 2000;
    while (guard--) {                       /* bounded: a line is never longer than 480 px */
        int32_t e2 = 2 * err;               /* both tests use the same error value */
        cv_pset(x0, y0, c);
        if (x0 == x1 && y0 == y1)
            break;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

/* a line `thick` px tall (the scope's trace: 2) */
static void cv_line_t(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint16_t c, int32_t thick)
{
    int32_t t;
    for (t = 0; t < thick; t++)
        cv_line(x0, y0 + t, x1, y1 + t, c);
}

/* coverage (0..16) of corner pixel (i, j) of a radius-r corner, (0, 0) the outermost: 4 x 4 samples */
static uint32_t rr_cov(int32_t r, int32_t i, int32_t j)
{
    int32_t sx, sy, n = 0;
    for (sy = 0; sy < 4; sy++)
        for (sx = 0; sx < 4; sx++) {
            int32_t dx = r * 8 - ((i * 4 + sx) * 2 + 1), dy = r * 8 - ((j * 4 + sy) * 2 + 1);   /* 1/8 px */
            n += dx * dx + dy * dy <= r * r * 64;
        }
    return (uint32_t)n;
}
/* a filled rectangle with rounded corners (r <= 8), the corners anti-aliased against `under` (what lies
 * behind it): the flat look's cards, panels, rows, bars and buttons */
static void cv_rrect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint16_t c, uint16_t under)
{
    int32_t i, j;
    GFX_HOOK_CELL(x, y + cv_oy, x + w, y + h + cv_oy);   /* (the lint: what is drawn on it stays inside it) */
    if (r < 0) r = -r;                           /* (-r: rounded in every style: a keycap-like cushion) */
    else if (ux.style == ST_LINE) r = 0;         /* LINE: square cursors, selections and fills (FLAT: rounded) */
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r < 1) {
        cv_rect(x, y, w, h, c);
        return;
    }
    /* ((JIANT 0.6.60 .. 0.8.8) JIANT's SURF was black: its boxes square; 0.8.9 the 0.6 colours, a near-black SURF:
     * rounded, as the mock-up) */
    cv_rect(x + r, y, w - 2 * r, h, c);
    cv_rect(x, y + r, r, h - 2 * r, c);
    cv_rect(x + w - r, y + r, r, h - 2 * r, c);
    for (j = 0; j < r; j++)
        for (i = 0; i < r; i++) {
            uint32_t a = rr_cov(r, i, j), q;
            uint16_t px = a >= 16u ? c : a == 0u ? under : ux_mix(under, c, (int32_t)(a * 100u / 16u));
            if (cv_halo && under == ux.bg && a < 16u) {   /* (0.9.0) over HUD's picture: the corners on what is there */
                for (q = 0; q < 4u; q++) {
                    int32_t px_ = q & 1u ? x + w - 1 - i : x + i, py = (q & 2u ? y + h - 1 - j : y + j) + cv_oy;
                    if ((uint32_t)px_ < cv_w && (uint32_t)py < cv_h && a) {
                        uint16_t *d = &cv_px[(uint32_t)py * cv_w + (uint32_t)px_];
                        *d = swap16(ux_mix(swap16(*d), c, (int32_t)(a * 100u / 16u)));
                    }
                }
                continue;
            }
            cv_pset(x + i, y + j, px);
            cv_pset(x + w - 1 - i, y + j, px);
            cv_pset(x + i, y + h - 1 - j, px);
            cv_pset(x + w - 1 - i, y + h - 1 - j, px);
        }
}
/* (0.9.0) a few canvas pixels kept and put back (HUD's picture through a ring's or a disc's hole, ui_draw.c) */
static void cv_keep(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t *buf, int put)
{
    int32_t i, j;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++) {
            int32_t px = x + i, py = y + j + cv_oy;
            if ((uint32_t)px < cv_w && (uint32_t)py < cv_h) {
                if (put)
                    cv_px[(uint32_t)py * cv_w + (uint32_t)px] = buf[j * w + i];
                else
                    buf[j * w + i] = cv_px[(uint32_t)py * cv_w + (uint32_t)px];
            }
        }
}

/* ------------------------------------------------- 4-bit alpha blit --- */
/* coverage curves: a little heavier for light ink on dark, lighter for dark ink on light */
static const uint8_t CURVE_DARK[16] = {0, 24, 43, 62, 80, 97, 114, 130, 147, 163, 178, 194, 210, 225, 240, 255};
static const uint8_t CURVE_LIGHT[16] = {0, 12, 27, 42, 58, 75, 91, 109, 126, 144, 162, 180, 199, 217, 236, 255};
#define NRAMP 6u
static struct {
    uint16_t fg[NRAMP], bg[NRAMP], v[NRAMP][16];   /* v: byte-swapped panel values */
    uint32_t gen;
    uint8_t next, n;
} rc;

static const uint16_t *ramp(uint16_t fg, uint16_t bg)
{
    static const uint8_t SH[3] = {11, 5, 0}, MK[3] = {31, 63, 31};
    const uint8_t *cv = ux.light ? CURVE_LIGHT : CURVE_DARK;
    uint32_t i, a, k;
    uint16_t *v;
    if (fg == bg)                                /* MONO: DIM on a RAISE well (one grey): black, recessed */
        fg = ux.bg;
    GFX_HOOK_INK(fg, bg);
    if (rc.gen != ux.gen) {
        rc.gen = ux.gen;
        rc.n = rc.next = 0;
    }
    for (i = 0; i < rc.n; i++)
        if (rc.fg[i] == fg && rc.bg[i] == bg)
            return rc.v[i];
    i = rc.next;
    rc.next = (uint8_t)((i + 1u) % NRAMP);
    if (rc.n < NRAMP)
        rc.n++;
    rc.fg[i] = fg;
    rc.bg[i] = bg;
    v = rc.v[i];
    for (a = 0; a < 16u; a++) {
        uint32_t out = 0;
        int32_t w = cv[a];
        if (ux.mono) {
            int32_t x = bg >> 11;
            out = ux_gray((uint32_t)(x + ((((fg >> 11) - x) * w + 128) >> 8)));
        } else {
            for (k = 0; k < 3u; k++) {
                int32_t x = (bg >> SH[k]) & MK[k], y = (fg >> SH[k]) & MK[k];
                out |= (uint32_t)(x + (((y - x) * w + 128) >> 8)) << SH[k];
            }
        }
        v[a] = swap16(out);
    }
    return v;
}

/* w x h nibbles, rows back to back, at canvas (x, y) */
static void cv_alpha(int32_t x, int32_t y, uint32_t w, uint32_t h, const uint8_t *d, const uint16_t *rv)
{
    uint32_t k = 0, gx, gy;
    y += cv_oy;
    for (gy = 0; gy < h; gy++) {
        int32_t py = y + (int32_t)gy;
        uint16_t *row;
        if (py < cv_cy0 || py >= cv_cy1) {
            k += w;
            continue;
        }
        row = cv_px + (uint32_t)py * cv_w;
        for (gx = 0; gx < w; gx++, k++) {
            uint32_t v = (d[k >> 1] >> ((k & 1u) ? 0 : 4)) & 15u;
            int32_t px = x + (int32_t)gx;
            if (v && (uint32_t)px < cv_w)
                row[px] = rv[v];
        }
    }
    GFX_HOOK_PIXELS(w * h);
}

/* the same from a glyph's Huffman-coded bit stream (tools/gen_aa_font.py huff_pack): canonical codes, MSB
 * first; hc = the number of codes of each length 1..15, then the symbols in code order, each a value and
 * its repeat count ((count - 1) << 4 | value: runs of zeros and 15s; 0.9.0, the keycaps: of any value). Codes of up
 * to 8 bits come from a 256-entry table made once per font in RAM (symbol | length << 8; 0 = a longer code: bit by bit). The
 * stream is read in order, so rows outside the clip are decoded and dropped; a run spans rows */
#define HC_FONTS 4                       /* the codes: the faces M and L (gen_aa_font.py HUFF), the icons, the keycaps */
static struct { const uint8_t *hc; uint16_t lut[256]; } hc_tab[HC_FONTS]
    __attribute__((section(".lowbuf")));   /* (the drawing's, main loop only: 0.8.4 LOWBUF, was .bss; 0.8.5 the pool, LOWBUF
                                            * full; 0.9.9-beta4 LOWBUF again, 4 KiB wider: the pool's top went to the RAM code) */
static const uint16_t *hc_lut(const uint8_t *hc)
{
    uint32_t k, len, code = 0, sym = 15, i;
    for (k = 0; k < HC_FONTS - 1u && hc_tab[k].hc && hc_tab[k].hc != hc; k++)
        ;
    if (hc_tab[k].hc != hc) {                        /* (more fonts than slots: the last slot is rebuilt) */
        hc_tab[k].hc = hc;
        for (i = 0; i < 256u; i++)
            hc_tab[k].lut[i] = 0;
        for (len = 1; len <= 8u; len++, code <<= 1)
            for (i = 0; i < hc[len - 1u]; i++, code++, sym++) {
                uint32_t a = code << (8u - len), n = 1u << (8u - len);
                while (n--)
                    hc_tab[k].lut[a + n] = (uint16_t)(hc[sym] | len << 8);
            }
    }
    return hc_tab[k].lut;
}

static void cv_alpha_hc(int32_t x, int32_t y, uint32_t w, uint32_t h, const uint8_t *d, const uint8_t *hc,
                        const uint16_t *rv)
{
    const uint16_t *lut = hc_lut(hc);
    uint32_t gy, v = 0, run = 0, acc = 0, nb = 0;
    y += cv_oy;
    for (gy = 0; gy < h; gy++) {
        int32_t py = y + (int32_t)gy;
        uint16_t *row = py < cv_cy0 || py >= cv_cy1 ? 0 : cv_px + (uint32_t)py * cv_w;
        uint32_t gx = 0;
        while (gx < w) {
            uint32_t n;
            if (!run) {                              /* the next symbol */
                uint32_t e, s;
                while (nb < 16u) {                   /* (the data ends with 2 spare bytes) */
                    acc = acc << 8 | *d++;
                    nb += 8;
                }
                e = lut[(acc >> (nb - 8u)) & 255u];
                if (e) {
                    nb -= e >> 8;
                    s = e & 255u;
                } else {                             /* a code over 8 bits */
                    uint32_t code = 0, first = 0, idx = 0, len = 0;
                    for (;;) {
                        code |= (acc >> --nb) & 1u;
                        if (code - first < hc[len])
                            break;
                        idx += hc[len];
                        first = (first + hc[len]) << 1;
                        code <<= 1;
                        len++;
                    }
                    s = hc[15u + idx + code - first];
                }
                v = s & 15u;
                run = (s >> 4) + 1u;
            }
            n = run < w - gx ? run : w - gx;
            if (v && row) {
                int32_t px = cv_flip ? x + (int32_t)(w - 1u - gx) : x + (int32_t)gx, dx = cv_flip ? -1 : 1;
                uint32_t k;
                for (k = 0; k < n; k++, px += dx)
                    if ((uint32_t)px < cv_w)
                        row[px] = rv[v];
            }
            gx += n;
            run -= n;
        }
    }
    GFX_HOOK_PIXELS(w * h);
}

/* (0.8.5) w x h pixels of one bit each, MSB first, rows back to back, at canvas (x, y): AF_X's glyphs, crisp (the ink
 * colour alone: rv[15]) */
static void cv_bits(int32_t x, int32_t y, uint32_t w, uint32_t h, const uint8_t *d, const uint16_t *rv)
{
    uint32_t k = 0, gx, gy;
    uint16_t c = rv[15];
    y += cv_oy;
    for (gy = 0; gy < h; gy++) {
        int32_t py = y + (int32_t)gy;
        uint16_t *row = py < cv_cy0 || py >= cv_cy1 ? 0 : cv_px + (uint32_t)py * cv_w;
        for (gx = 0; gx < w; gx++, k++) {
            int32_t px = x + (int32_t)gx;
            if (row && ((d[k >> 3] >> (7u - (k & 7u))) & 1u) && (uint32_t)px < cv_w)
                row[px] = c;
        }
    }
    GFX_HOOK_PIXELS(w * h);
}

/* ------------------------------------------------------------- text --- */
/* the glyph number of ch in f, -1 = not in the face */
static int32_t glyph_at(const aafont_t *f, uint32_t ch)
{
    uint32_t i;
    if (ch >= f->first && ch <= f->last)
        return (int32_t)(ch - f->first);
    for (i = 0; i < f->nex; i++)
        if (f->ex[i] == ch)
            return (int32_t)(f->last - f->first + 1u + i);
    return -1;
}

/* the table entry of ch at phase 0 (its other phases follow it). Not in the face: the ellipsis as '.',
 * else '?', else the face's first glyph (L is sparse: its space) */
static uint32_t glyph(const aafont_t *f, uint32_t ch)
{
    int32_t k;
    if (ch >= 'a' && ch <= 'z' && f->last < 'a')
        ch -= 32u;                                   /* L has capitals only */
    if ((k = glyph_at(f, ch)) < 0 && (ch != (uint8_t)ELLIPSIS || (k = glyph_at(f, '.')) < 0) &&
        (k = glyph_at(f, '?')) < 0)
        k = 0;
    return (uint32_t)k << f->psh;
}
/* table entry i (glyph i >> psh at phase i & (phases - 1)), unpacked */
static aag_t ag(const aafont_t *f, uint32_t i)
{
    const aagh_t *h = &f->gh[i >> f->psh];
    uint32_t v = f->gp[i];
    aag_t g;
    g.off = (uint16_t)(h->off + (v & 1023u));
    g.adv = h->adv;
    g.bx = (int8_t)((int32_t)((v >> 10) & 7u) - 1);
    g.by = (uint8_t)((v >> 13) & 63u);
    g.bw = (uint8_t)((v >> 19) & 127u);
    g.bh = (uint8_t)(v >> 26);
    return g;
}

static int32_t kern(const aafont_t *f, uint32_t a, uint32_t b)       /* 1/16 px */
{
    int32_t ga = glyph_at(f, a);
    uint32_t lo, hi;
    if (ga < 0 || (uint32_t)ga >= f->nk)
        return 0;
    lo = f->ki[ga];
    hi = f->ki[ga + 1];
    while (lo < hi) {                                /* (a's pairs, by b) */
        uint32_t m = (lo + hi) / 2u, c = f->kp[2u * m];
        if (c == b)
            return (int8_t)f->kp[2u * m + 1u];
        if (c < b)
            lo = m + 1u;
        else
            hi = m;
    }
    return 0;
}

static uint32_t fold(const aafont_t *f, uint32_t c) { return c >= 'a' && c <= 'z' && f->last < 'a' ? c - 32u : c; }

static int32_t text_w(const aafont_t *f, const char *s)
{
    int32_t pen = 0;
    uint32_t prev = 0;
    for (; *s; s++) {
        uint32_t c = fold(f, (uint8_t)*s);
        if (prev)
            pen += kern(f, prev, c);
        pen += f->gh[glyph(f, c) >> f->psh].adv;
        prev = c;
    }
    return (pen + 8) >> 4;
}

static int32_t text_run(int32_t x, int32_t y, const aafont_t *f, const char *s, const uint16_t *rv, int32_t *bb)
{
    int32_t pen = 0, err = 0, x0 = 0x7FFF, y0 = 0x7FFF, x1 = -0x7FFF, y1 = -0x7FFF;
    const int32_t step = 16 >> f->psh;
    uint32_t prev = 0;
    for (; *s; s++) {
        uint32_t c = fold(f, (uint8_t)*s), gi;
        aag_t gv;
        const aag_t *g = &gv;
        int32_t pos, d0, d1;
        if (prev)
            pen += kern(f, prev, c);
        gi = glyph(f, c);
        if (f->psh && (f->gp[gi + 1u] & 1023u) != (f->gp[gi] & 1023u)) {   /* (phase 1's bitmap is not phase 0's) */
            pos = pen - (pen & (step - 1));          /* the phase position at or left of the pen */
            d0 = pos - pen - err;                    /* .. and the one right of it: which keeps the gap truer */
            d1 = d0 + step;
            if ((d0 < 0 ? -d0 : d0) > (d1 < 0 ? -d1 : d1))
                pos += step;
            gi += ((uint32_t)pos >> (4u - f->psh)) & ((1u << f->psh) - 1u);
        } else {
            pos = (pen + 8) & ~15;                   /* one phase (figures, ., blanks): the rounded pen */
        }
        gv = ag(f, gi);
        err = pos - pen;
        if (g->bw) {
            int32_t gx = x + (pos >> 4) + g->bx, gy = y + g->by;
            if (!rv)
                ;                                    /* (text_ink: measured only) */
            else if (f->bits)
                cv_bits(gx, gy, g->bw, g->bh, f->data + g->off, rv);
            else if (f->hc)
                cv_alpha_hc(gx, gy, g->bw, g->bh, f->data + g->off, f->hc, rv);
            else
                cv_alpha(gx, gy, g->bw, g->bh, f->data + g->off, rv);
            if (gx < x0) x0 = gx;
            if (gx + g->bw > x1) x1 = gx + g->bw;
            if (gy < y0) y0 = gy;                    /* (the rows: the host's lint, cv_txt_note) */
            if (gy + g->bh > y1) y1 = gy + g->bh;
        }
        pen += g->adv;
        prev = c;
    }
    bb[0] = x0;
    bb[1] = y0;
    bb[2] = x1;
    bb[3] = y1;
    return pen;
}
/* (0.9.5) a text's ink box on the canvas (its rows with cv_oy), kept for an overlay to keep clear of (cv_txt) */
static void cv_txt_note(int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    int16_t *t;
    if (x1 <= x0 || y1 <= y0)
        return;
    if (cv_ntxt < CV_NTXT) {
        t = cv_txt[cv_ntxt++];
        t[0] = (int16_t)x0; t[1] = (int16_t)y0; t[2] = (int16_t)x1; t[3] = (int16_t)y1;
        return;
    }
    t = cv_txt[CV_NTXT - 1u];                       /* (full: the last box takes this one in) */
    if (x0 < t[0]) t[0] = (int16_t)x0;
    if (y0 < t[1]) t[1] = (int16_t)y0;
    if (x1 > t[2]) t[2] = (int16_t)x1;
    if (y1 > t[3]) t[3] = (int16_t)y1;
}
/* any text noted on the canvas touching x0 .. x1 - 1, y0 .. y1 - 1 */
static int cv_txt_in(int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    uint32_t i;
    for (i = 0; i < cv_ntxt; i++)
        if (cv_txt[i][0] < x1 && x0 < cv_txt[i][2] && cv_txt[i][1] < y1 && y0 < cv_txt[i][3])
            return 1;
    return 0;
}
/* y = top of the line box (the baseline is y + f->asc); returns the pen x at the end.
 * fg on bg: bg is what lies under the text (the canvas, a card, a selection).
 * Placement (text_run): the pen (1/16 px) lies between two phase positions a step (16 >> psh) apart; the glyph takes
 * the one whose offset from the pen is nearer the previous glyph's offset, so the space between neighbours stays
 * truest and no glyph is a whole step from its true place (tools/aa_raster.py, text_spacing_test.py).
 * A glyph with one phase (its phases share one bitmap: figures and .) sits at the rounded pen, as when it
 * is drawn alone at text_w of what precedes it (ui_draw.c roll_text). */
static int32_t cv_text_flags(int32_t x, int32_t y, const aafont_t *f, const char *s, uint16_t fg, uint16_t bg,
                             uint32_t flags)
{
    int32_t b[4], pen;
    if (cv_halo) {                                   /* (0.8.9) the outline: the ink, solid BG, round it (0.9.0: 2 px) */
        static uint16_t hv[16] __attribute__((section(".pool")));
        uint32_t k;
        for (k = 0; k < 16u; k++)
            hv[k] = swap16(ux.bg);
        for (k = 0; k < HALO_N; k++)
            text_run(x + HALO_X[k], y + HALO_Y[k], f, s, hv, b);
    }
    pen = text_run(x, y, f, s, ramp(fg, bg), b);
    cv_txt_note(b[0], b[1] + cv_oy, b[2], b[3] + cv_oy);
    if (GFX_HOOKS && b[2] > b[0]) {
        int32_t y0 = b[1], y1 = b[3];
        if (b[0] < 0 || b[2] > (int32_t)cv_w || y0 + cv_oy < cv_cy0 || y1 + cv_oy > cv_cy1)
            flags |= 2u;                             /* cut by the canvas */
        if (cv_scroll) {                             /* a scrolled view: what lands on the screen is its visible part */
            flags |= 32u;
            if (y0 + cv_oy < cv_cy0) y0 = cv_cy0 - cv_oy;
            if (y1 + cv_oy > cv_cy1) y1 = cv_cy1 - cv_oy;
        }
        GFX_HOOK_PEN(y, f);
        GFX_HOOK_TEXT(b[0], y0 + cv_oy, b[2], y1 + cv_oy, s, flags);
    }
    (void)flags;
    return x + ((pen + 8) >> 4);
}
static int32_t cv_text_on(int32_t x, int32_t y, const aafont_t *f, const char *s, uint16_t fg, uint16_t bg)
{
    return cv_text_flags(x, y, f, s, fg, bg, 0);
}
/* .. on the canvas background */
static int32_t cv_text(int32_t x, int32_t y, const aafont_t *f, const char *s, uint16_t fg)
{
    return cv_text_flags(x, y, f, s, fg, cv_bg, 0);
}
/* right-aligned: the advance ends at xr */
static int32_t cv_text_r(int32_t xr, int32_t y, const aafont_t *f, const char *s, uint16_t fg, uint16_t bg)
{
    int32_t w = text_w(f, s);
    cv_text_on(xr - w, y, f, s, fg, bg);
    return xr - w;
}
/* (centred: cv_text_in, by the ink; by the advance a text sat up to 1.5 px off) */

/* centring by ink: the ink of s in f drawn at (0, 0) across (b[0] .. b[2], past it; b[2] <= b[0]: none); up and down
 * a text is centred by its face's capitals' band (AF_S_CAP_Y ..: the ink of H; figures and round letters reach at most
 * a faint row past it), so the words of a row keep one baseline */
static void text_ink(const aafont_t *f, const char *s, int32_t *b) { text_run(0, 0, f, s, 0, b); }
#define HALF_UP(v) ((v) >> 1)                       /* half, a pixel left over going left / up (also below 0) */
/* the line top that centres face F's (S M) capitals' band in h rows (a row, a cell, a button): constant */
#define CAP_IN(F, h) (HALF_UP((h) - AF_##F##_CAP_H) - AF_##F##_CAP_Y)
/* the pen x that centres s's ink in w px */
static int32_t ink_in(const aafont_t *f, const char *s, int32_t w)
{
    int32_t b[4];
    text_ink(f, s, b);
    return HALF_UP(w - (b[2] - b[0])) - b[0];
}
/* s with its ink centred across x0 .. x0 + w (y: its line top, CAP_IN to centre it up and down too); a pixel left
 * over goes left / up, as everywhere (the cushions too) */
static int32_t cv_text_in(int32_t x0, int32_t y, int32_t w, const aafont_t *f, const char *s, uint16_t fg, uint16_t bg)
{
    return cv_text_on(x0 + ink_in(f, s, w), y, f, s, fg, bg);
}

/* s into d (n bytes), at most maxw px wide: never a smaller font, the end ellipsised. 1 = cut */
static int text_fit(char *d, uint32_t n, const char *s, const aafont_t *f, int32_t maxw)
{
    uint32_t k = 0;
    while (s[k] && k + 1u < n) {
        d[k] = s[k];
        k++;
    }
    d[k] = 0;
    if (!s[k] && text_w(f, d) <= maxw)
        return 0;
    if (k + 2u > n)
        k = n - 2u;
    for (;;) {                                       /* drop characters until "text..." fits */
        while (k && d[k - 1u] == ' ')
            k--;
        d[k] = ELLIPSIS;
        d[k + 1u] = 0;
        if (!k || text_w(f, d) <= maxw)
            return 1;
        k--;
    }
}
/* text in maxw px, ellipsised when it is longer: a label or a value should always fit (the host
 * layout lint reports a cut one), free text (cv_free_text: a preset name, a message) may be cut */
static int32_t cv_text_fit(int32_t x, int32_t y, const aafont_t *f, const char *s, uint16_t fg, uint16_t bg,
                           int32_t maxw)
{
    char b[48];
    int cut = text_fit(b, sizeof b, s, f, maxw);
    return cv_text_flags(x, y, f, b, fg, bg, cut ? 1u : 0u);
}
static int32_t cv_free_text(int32_t x, int32_t y, const aafont_t *f, const char *s, uint16_t fg, uint16_t bg,
                            int32_t maxw)
{
    char b[48];
    int cut = text_fit(b, sizeof b, s, f, maxw);
    return cv_text_flags(x, y, f, b, fg, bg, cut ? 9u : 8u);
}
/* (0.8.5) free text cut to maxw without an ellipsis, as the mock-up's chips cut a name ("WOBBL"): its first characters */
static int32_t cv_clip_text(int32_t x, int32_t y, const aafont_t *f, const char *s, uint16_t fg, uint16_t bg, int32_t maxw)
{
    char b[48];
    uint32_t k = 0;
    while (s[k] && k + 1u < sizeof b) {
        b[k] = s[k];
        b[k + 1u] = 0;
        if (text_w(f, b) > maxw)
            break;
        k++;
    }
    while (k && b[k - 1u] == ' ')
        k--;
    b[k] = 0;
    return cv_text_flags(x, y, f, b, fg, bg, 8u);
}

/* (0.8.9) a 1-bit face's s turned a quarter left (reading upwards, as the mock-up's keys name themselves): its line's top
 * at column x, its first character's foot at row y; maxh: at most this many rows (the rest dropped). Returns the rows used */
static int32_t cv_text_up(int32_t x, int32_t y, const aafont_t *f, const char *s, uint16_t fg, uint16_t bg, int32_t maxh)
{
    int32_t pen = 0, x0 = 0x7FFF, y0 = 0x7FFF, x1 = -0x7FFF, y1 = -0x7FFF;
    uint16_t c = swap16(fg);
    const char *s0 = s;
    GFX_HOOK_INK(fg, bg);
    (void)bg;
    for (; *s; s++) {
        const aag_t gv = ag(f, glyph(f, fold(f, (uint8_t)*s))), *g = &gv;
        uint32_t r, q, k = 0;
        int32_t adv = (g->adv + 8) >> 4;
        if (pen + adv - 1 > maxh)
            break;
        for (r = 0; r < g->bh; r++)
            for (q = 0; q < g->bw; q++, k++)
                if ((f->data[g->off + (k >> 3)] >> (7u - (k & 7u))) & 1u) {
                    int32_t px = x + g->by + (int32_t)r, py = y - (pen + g->bx + (int32_t)q) + cv_oy;
                    if ((uint32_t)px < cv_w && py >= cv_cy0 && py < cv_cy1)
                        cv_px[(uint32_t)py * cv_w + (uint32_t)px] = c;
                    if (px < x0) x0 = px;
                    if (px + 1 > x1) x1 = px + 1;
                    if (py < y0) y0 = py;
                    if (py + 1 > y1) y1 = py + 1;
                }
        pen += adv;
    }
    cv_txt_note(x0, y0, x1, y1);
    if (GFX_HOOKS && x1 > x0)
        GFX_HOOK_TEXT(x0, y0, x1, y1, s0, 0u);
    (void)x0; (void)y0; (void)x1; (void)y1; (void)s0;
    return pen;
}

/* ---------------------------------------------------------- keycaps --- */
/* a pre-rendered pill (tools/gen_aa_keycaps.py): nibble 0 outside, 1..4 the edge over `under`, 5 the fill,
 * 6..15 the label over the fill; the 16 colours per (under, fill, ink) are cached like the text ramps */
#define NKRAMP 4u
static struct {
    uint16_t key[NKRAMP][3], v[NKRAMP][16];
    uint32_t gen;
    uint8_t next, n;
} kr __attribute__((section(".pool")));      /* (zero-initialised; outside .bss) */
static const uint16_t *kc_ramp(uint16_t under, uint16_t fill, uint16_t ink)
{
    uint32_t i, a;
    uint16_t *v;
    if (ink == fill)                             /* (as ramp) */
        ink = ux.bg;
    GFX_HOOK_INK(ink, fill);
    if (kr.gen != ux.gen) {
        kr.gen = ux.gen;
        kr.n = kr.next = 0;
    }
    for (i = 0; i < kr.n; i++)
        if (kr.key[i][0] == under && kr.key[i][1] == fill && kr.key[i][2] == ink)
            return kr.v[i];
    i = kr.next;
    kr.next = (uint8_t)((i + 1u) % NKRAMP);
    if (kr.n < NKRAMP)
        kr.n++;
    kr.key[i][0] = under;
    kr.key[i][1] = fill;
    kr.key[i][2] = ink;
    v = kr.v[i];
    v[0] = swap16(under);
    for (a = 1; a < 16u; a++) {
        int32_t p = a < 5u ? (int32_t)a * 20 : (int32_t)(a - 5u) * 10;
        v[a] = swap16(a < 5u ? ux_mix(under, fill, p) : ux_mix(fill, ink, p));
    }
    return v;
}
static int32_t kc_w(uint32_t id) { return id < KC_COUNT ? KC[id].w : 0; }
/* keycap / badge `id` with its top-left at (x, y), KC_H tall: fill and label tinted with tokens, `under` what lies
 * behind its corners; returns its right edge */
static int32_t cv_keycap(int32_t x, int32_t y, uint32_t id, uint16_t fill, uint16_t ink, uint16_t under)
{
    const kc_t *k = &KC[id < KC_COUNT ? id : 0u];
    const uint16_t *rv = kc_ramp(under, fill, ink);
    cv_alpha_hc(x, y, k->w, KC_H, KC_DATA + k->off, KC_HC, rv);   /* (0.9.0: Huffman-coded, 2.4 KB less) */
    if (x < 0 || x + k->w > (int32_t)cv_w || y + cv_oy < 0 || y + KC_H + cv_oy > (int32_t)cv_h)
        GFX_HOOK_TEXT(x, y + cv_oy, x + k->w, y + KC_H + cv_oy, k->label, 6u);      /* cut by the canvas */
    else
        GFX_HOOK_TEXT(x, y + cv_oy, x + k->w, y + KC_H + cv_oy, k->label, 4u);
    return x + k->w;
}

/* a key hint: the keycap, then what it does (S, TEXT); unavailable: a DIM keycap and a DIM word. The word sits on
 * the keycap's centre line (S caps 9 rows, the keycap's 7). act may be 0 (the keycap alone); returns the end x */
#define KH_GAP 4                                  /* keycap -> its word */
static int32_t kh_w(uint32_t id, const char *act) { return kc_w(id) + (act && act[0] ? KH_GAP + text_w(&AF_S, act) : 0); }
/* .. its ink: the keycap to the word's last inked column (centring a hint by what is seen) */
static int32_t kh_ink(uint32_t id, const char *act)
{
    int32_t b[4];
    if (!act || !act[0])
        return kc_w(id);
    text_ink(&AF_S, act, b);
    return kc_w(id) + KH_GAP + b[2];
}
static int32_t cv_key_hint(int32_t x, int32_t y, uint32_t id, const char *act, int on, uint16_t under)
{
    x = cv_keycap(x, y, id, on ? T_KEY : T_DIM, T_INK, under);
    if (act && act[0]) {
        GFX_HOOK_ALIGN(0, y, 0, y + KC_H, AL_V | AL_PASS, "key hint word on its keycap's line");
        x = cv_text_on(x + KH_GAP, y - 1, &AF_S, act, on ? T_TEXT : T_DIM, under);
    }
    return x;
}
/* a row of key hints from x0 to x1: the first at x0, the last ending at x1, the space shared between them. When
 * the row is too wide the action words go first, from the last hint back (the keycaps never shrink).
 * on: a bit per hint (1 = available) */
typedef struct { uint8_t key; const char *act; } khint_t;
static void cv_key_row(int32_t x0, int32_t x1, int32_t y, const khint_t *h, uint32_t n, uint32_t on, uint16_t under)
{
    uint32_t i, words = n;                         /* hints 0 .. words-1 keep their word */
    int32_t w, gap, x = x0;
    for (;;) {
        w = 0;
        for (i = 0; i < n; i++)
            w += kh_w(h[i].key, i < words ? h[i].act : 0);
        if (w + 6 * (int32_t)(n - 1u) <= x1 - x0 || !words)
            break;
        words--;
    }
    gap = n > 1u ? (x1 - x0 - w) / (int32_t)(n - 1u) : 0;
    for (i = 0; i < n; i++) {
        if (n > 1u && i + 1u == n)
            x = x1 - kh_w(h[i].key, i < words ? h[i].act : 0);   /* the last one flush right */
        x = cv_key_hint(x, y, h[i].key, i < words ? h[i].act : 0, (on >> i) & 1u, under) + gap;
    }
}

/* "[OCT+]..." -> the keycap's id and the length of the bracket, else -1 (a bracketed word that is no keycap is text) */
static int32_t kc_tag(const char *s, uint32_t *len)
{
    uint32_t i, n;
    if (s[0] != '[')
        return -1;
    for (n = 1; s[n] && s[n] != ']'; n++)
        if (n > 8u)
            return -1;
    if (s[n] != ']')
        return -1;
    for (i = 0; i < KC_COUNT; i++) {
        const char *l = KC[i].label;
        uint32_t k = 0;
        while (k + 1u < n && l[k] == s[k + 1u])
            k++;
        if (k + 1u == n && !l[k]) {
            *len = n + 1u;
            return (int32_t)i;
        }
    }
    return -1;
}

/* one-shot: a line of text in a box of colour bg, blitted (align: 0 left, 1 centre, 2 right) */
static void draw_text_line(uint32_t x, uint32_t y, uint32_t w, const aafont_t *f, const char *s,
                           uint16_t c, uint16_t bg, int align)
{
    int32_t tw = text_w(f, s), tx = 0;
    cv_begin(w, f->h, bg);
    if (align == 1) {
        tx = ink_in(f, s, (int32_t)w);
        GFX_HOOK_ALIGN(0, 0, (int32_t)w, 0, AL_H, "one-shot line centred");
    }
    else if (align == 2)
        tx = (int32_t)w - tw;
    cv_text(tx, 0, f, s, c);
    cv_blit(x, y);
    lcd_sync();                     /* one-shots (boot, crash, UBOOT, update) finish here */
}
static void draw_text_box(uint32_t x, uint32_t y, uint32_t w, const aafont_t *f, const char *s,
                          uint16_t c, int align)
{
    draw_text_line(x, y, w, f, s, c, T_BG, align);
}
