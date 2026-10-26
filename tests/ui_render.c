/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Renders of the firmware UI (firmware/src/ui*.c on tests/ui_test.c's stubs), the layout lint, the GREY and MONO
 * checks and the host draw cost.
 *   ui_render OUTDIR [SLOTDIR]   (run by tests/run_tests.sh; tests/ui_render.py makes the PNGs;
 *                                 SLOTDIR: filmstrips of the rolling digits, GREY MONO and GREEN)
 * Layout lint, over every screen below in every palette, every page of every engine and every value of every
 * column: the ink box of every text and icon drawn (gfx.c GFX_HOOK_TEXT) as it lands on the screen. A finding:
 * a box off the screen, cut by its canvas, partly covered by a later strip or fill, overlapping another box, or
 * ellipsised when it is not free text (labels and values must fit; names and messages may be ellipsised), or
 * spilling out of its cell: touching a rounded rectangle of its canvas (gfx.c cv_rrect: a cell, card, row, button,
 * GFX_HOOK_CELL) without lying inside it, though still on the screen (the FX map's REVERSE), or drawn in the
 * colour it is drawn on (gfx.c GFX_HOOK_INK: a text, icon or keycap label that cannot be seen; MONO's tokens share
 * values, DIM and RAISE its one grey).
 * GREY: every pixel of every screen is RGB565 gray (R = B, G = 2 R). MONO (black and white, blended per channel):
 * every pixel is neutral (R = B, G within 2 of 2 R).
 * MENU > STYLE LINE: every screen again in every palette, the same lint; a text or icon on a divider
 * (ui_draw.c lcd_rule, GFX_HOOK_RULE: the rules between the strips; cv_rule's are cells) is a finding too.
 * Rolling digits: every frame of a header BPM roll and a card roll, up and down, in every palette (lint, GREY, MONO).
 * FM6 colonies (0.9.6, ui_engvis.c vx_fm6 on OPERATOR, each bicho through VX_FM6_HOOK), every algorithm: no two
 * operators' bichos overlapping or touching (as large as a full envelope makes them), every one inside the panel.
 * Output: OUTDIR/ppm/<PALETTE>_<screen>.ppm for GREY MONO GREEN PAPER NIGHT, OUTDIR/report.txt (findings, ellipsised
 * free text, the draw cost), OUTDIR/text_audit.tsv (GREY, per screen: texts, icons and keycaps with their ink-box px,
 * the ellipsised ones, the texts closer than 2 px to their cell's edge, the words).
 * Alignment (gfx.c GFX_HOOK_ALIGN): every place where a text, icon or keycap is meant to sit centred (in a cell, a
 * chip, a button, a row, a column, a knob, a strip) or on a line shared with its neighbours (an icon on its label's
 * line, a word on its keycap's, a unit on its value's baseline) declares its box; the ink drawn there (a text: its
 * glyphs' boxes across, its capitals' band up and down; an icon: its cell's nibbles; a keycap: its pill; also the
 * keycaps' labels in their pills, from the data, and rows of keys or bars against their panel) is measured against
 * it, on every screen above in FLAT and LINE and every palette, the page / value sweep, and align_sweeps (every value
 * each place can show). 0.5 px is the odd pixel left over (it goes left / up); 1 px or more is a finding.
 * OUTDIR/align.txt: per place, how often measured, the worst offsets and where.
 * Exit 1 on a finding, a GREY pixel off gray, a MONO one off neutral, or an alignment 1 px or more off. */
#define KC_REF 1                 /* (0.9.0) ui_keycaps.h: the pills as nibbles too (al_keycaps; KC_DATA is Huffman-coded) */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <math.h>

typedef struct { int16_t x0, y0, x1, y1; uint8_t flags; char s[28]; } rbox_t;
static rbox_t pend[256], scr[1200];
static int16_t cells[64][4];                    /* GFX_HOOK_CELL: the cells and cards of the canvas being drawn */
static uint32_t ncells, nspill;
static FILE *aud;                               /* OUTDIR/text_audit.tsv: GREY, the text of each screen */
static char tight[400];                         /* .. the texts of this screen closer than 2 px to their cell's edge */
static uint32_t ntight;
static uint32_t npend, nscr, nfind, nfree;
static uint64_t px_visited, n_text, blit_px;
static FILE *rep;
static const char *cur_name = "";
static char free_seen[64][40];
static uint32_t nfree_seen;
/* the alignment check (gfx.c GFX_HOOK_ALIGN): a place declares the box its next item(s) are meant to be centred in,
 * across and / or up and down; the items' ink as drawn (a text: its glyphs' boxes across, its capitals' band up and
 * down, a text of no letter or figure: across only; an icon: the ink of its cell; a keycap: its pill; a cushion: the
 * cushion) against the box: the offset of the ink's centre from the box's, in half pixels. 0.5 is the odd pixel
 * left over (it goes left / up: a +0.5 is counted apart); 1 px or more is a finding. Per place (its tag): how
 * often it was measured, the worst offsets and where, in OUTDIR/align.txt */
typedef struct { int32_t x0, y0, x1, y1, ix0, iy0, ix1, iy1; uint32_t mode, left, nv; const char *tag; char s[24]; } al_t;
typedef struct { const char *tag; uint32_t n, nfail, half_lo, half_hi; int32_t wh2, wv2; char ex_h[112], ex_v[112]; } alstat_t;
static al_t al_stk[8];
static uint32_t al_n, al_fail, al_lost, al_count, al_off;
static int32_t al_cy0, al_cy1;                  /* GFX_HOOK_PEN: the capitals' band of the text being hooked */
static alstat_t al_st[160];
static uint32_t al_nst;
static FILE *al_rep;
static alstat_t *al_stat(const char *tag)
{
    uint32_t i;
    for (i = 0; i < al_nst && strcmp(al_st[i].tag, tag); i++) ;
    if (i == al_nst && al_nst < 160u) { memset(&al_st[i], 0, sizeof al_st[i]); al_st[i].tag = tag; al_nst++; }
    return &al_st[i < 160u ? i : 159u];
}
/* one measurement: offsets in half pixels (h2, v2; 99: not measured) */
static void al_record(const char *tag, int32_t h2, int32_t v2, const char *s, int32_t bx0, int32_t by0, int32_t bx1,
                      int32_t by1, int32_t ix0, int32_t iy0, int32_t ix1, int32_t iy1)
{
    alstat_t *a = al_stat(tag);
    int32_t ah = h2 == 99 ? 0 : h2 < 0 ? -h2 : h2, av = v2 == 99 ? 0 : v2 < 0 ? -v2 : v2;
    a->n++;
    al_count++;
    if (h2 == 1 || v2 == 1) a->half_hi++;
    if (h2 == -1 || v2 == -1) a->half_lo++;
    if (ah > a->wh2 || (!a->ex_h[0] && h2 != 99)) {
        if (ah > a->wh2) a->wh2 = ah;
        snprintf(a->ex_h, sizeof a->ex_h, "%+.1f '%s' %s ink x %d..%d box %d..%d", h2 / 2.0, s, cur_name, ix0, ix1, bx0, bx1);
    }
    if (av > a->wv2 || (!a->ex_v[0] && v2 != 99)) {
        if (av > a->wv2) a->wv2 = av;
        snprintf(a->ex_v, sizeof a->ex_v, "%+.1f '%s' %s ink y %d..%d box %d..%d", v2 / 2.0, s, cur_name, iy0, iy1, by0, by1);
    }
    if (ah >= 2 || av >= 2) {
        a->nfail++;
        al_fail++;
        if (al_rep && a->nfail <= 3u)
            fprintf(al_rep, "OFF  %-28s %-26s '%s' across %+.1f, up/down %+.1f  (ink %d,%d-%d,%d box %d,%d-%d,%d)\n", tag, cur_name,
                    s, h2 == 99 ? 0.0 : h2 / 2.0, v2 == 99 ? 0.0 : v2 / 2.0, ix0, iy0, ix1, iy1, bx0, by0, bx1, by1);
    }
}
static void hk_align(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t mode, const char *tag)
{
    al_t *a;
    if (al_off) return;                         /* (the draw cost: timed without the check)  */
    if (al_n >= 8u) { al_lost++; return; }
    a = &al_stk[al_n++];
    memset(a, 0, sizeof *a);
    a->x0 = x0; a->y0 = y0; a->x1 = x1; a->y1 = y1; a->mode = mode; a->tag = tag;
    a->left = mode >> 8 ? mode >> 8 : 1u;
    a->ix0 = a->iy0 = 1 << 20; a->ix1 = a->iy1 = -(1 << 20);
}
/* an item drawn (cell: a cv_rrect, else ink): its extent across (x0..x1), up and down (y0..y1; vy 0: none), to the
 * place declared last (AL_PASS, its item done: to the one before it too) */
static void al_item_k(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int vy, const char *s, int cell)
{
    al_t *a;
    int32_t h2 = 99, v2 = 99;
    uint32_t pass;
    if (!al_n) return;
    a = &al_stk[al_n - 1u];
    if (!(a->mode & 32u) != !cell) return;          /* (AL_CELLS: its cells only; else ink only) */
    if (x0 < a->ix0) a->ix0 = x0;
    if (x1 > a->ix1) a->ix1 = x1;
    if (vy) {
        if (y0 < a->iy0) a->iy0 = y0;
        if (y1 > a->iy1) a->iy1 = y1;
        a->nv++;
    }
    if (!a->s[0] || strlen(a->s) + strlen(s) + 2u < sizeof a->s)
        snprintf(a->s + strlen(a->s), sizeof a->s - strlen(a->s), "%s%s", a->s[0] ? "+" : "", s);
    if (--a->left) return;
    al_n--;
    pass = a->mode & 16u;
    if (a->mode & 1u) h2 = (a->ix0 + a->ix1) - (a->x0 + a->x1);       /* AL_H: the centres */
    if (a->mode & 8u) h2 = 2 * (a->ix1 - a->x1);                      /* AL_R: the right edges */
    if (a->nv && (a->mode & 2u)) v2 = (a->iy0 + a->iy1) - (a->y0 + a->y1);
    if (a->nv && (a->mode & 4u)) v2 = 2 * (a->iy1 - a->y1);           /* AL_B: the baselines (capitals' bottoms) */
    al_record(a->tag, h2, v2, a->s, a->x0, a->y0, a->x1, a->y1, a->ix0, a->iy0, a->ix1, a->iy1);
    if (pass) al_item_k(x0, y0, x1, y1, vy, s, cell);
}
static void al_item(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int vy, const char *s) { al_item_k(x0, y0, x1, y1, vy, s, 0); }
static int al_alnum(const char *s)
{
    for (; *s; s++)
        if ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9')) return 1;
    return 0;
}
static void al_text(int32_t x0, int32_t y0, int32_t x1, int32_t y1, const char *s, uint32_t flags)
{
    if (!al_n || ((flags & 4u) && !strcmp(s, "icon"))) return;      /* (an icon: GFX_HOOK_ICON) */
    if (flags & 4u) al_item(x0, y0, x1, y1, 1, s);                    /* a keycap: its pill */
    else al_item(x0, al_cy0, x1, al_cy1, al_alnum(s), s);
}
static void al_flush(void)                      /* a canvas blitted or begun: declared places left without their items */
{
    uint32_t i;
    for (i = 0; i < al_n; i++)
        if (al_rep && al_lost + i < 12u) fprintf(al_rep, "LOST %-28s %s (declared, nothing drawn after it)\n", al_stk[i].tag, cur_name);
    al_lost += al_n;
    al_n = 0;
}
static int32_t icon_cell(uint32_t *size, uint32_t id);
static void hk_icon(int32_t x, int32_t y, uint32_t size, uint32_t id);   /* (after the icons: their cells' nibbles) */
#define GFX_HOOK_ALIGN(x0, y0, x1, y1, mode, tag) hk_align(x0, (y0) + cv_oy, x1, (y1) + cv_oy, mode, tag)
#define GFX_HOOK_PEN(y, f) (al_n ? (al_cy0 = (y) + cv_oy + ag((f), glyph((f), 'H')).by, al_cy1 = al_cy0 + ag((f), glyph((f), 'H')).bh) : 0)
#define GFX_HOOK_ITEM(x0, y0, x1, y1) al_item(x0, (y0) + cv_oy, x1, (y1) + cv_oy, 1, "cushion")
#define GFX_HOOK_ICON(x, y, size, id) hk_icon(x, (y) + cv_oy, size, id)
static char splash_seen[96];                    /* the splash scene: its texts as drawn, '|' between (draw) */
static uint8_t in_splash;
static void hk_text(int32_t x0, int32_t y0, int32_t x1, int32_t y1, const char *s, uint32_t flags)
{
    al_text(x0, y0, x1, y1, s, flags);
    if (in_splash && strlen(splash_seen) + strlen(s) + 2u < sizeof splash_seen)
        snprintf(splash_seen + strlen(splash_seen), sizeof splash_seen - strlen(splash_seen), "%s%s", splash_seen[0] ? "|" : "", s);
    n_text++;
    if (npend >= 256u) return;
    pend[npend] = (rbox_t){(int16_t)x0, (int16_t)y0, (int16_t)x1, (int16_t)y1, (uint8_t)flags, {0}};
    strncpy(pend[npend].s, s, 27);
    npend++;
}
static void hk_cell(int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    al_item_k(x0, y0, x1, y1, 1, "cell", 1);
    if (ncells < 64u) {
        cells[ncells][0] = (int16_t)x0; cells[ncells][1] = (int16_t)y0;
        cells[ncells][2] = (int16_t)x1; cells[ncells][3] = (int16_t)y1;
        ncells++;
    }
}
static void hk_blit(uint32_t x, uint32_t y, uint32_t r0, uint32_t w, uint32_t h);
static void hk_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
/* STYLE LINE: the dividers drawn on the screen (ui_draw.c lcd_rule) while no later blit or fill covers them */
static int16_t rules[32][4];
static uint32_t nrules;
static void hk_rule_k(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t k);
static void hk_rule(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    hk_rule_k((int32_t)x, (int32_t)y, (int32_t)(x + w), (int32_t)(y + h), 0u);
}
static void hk_rule_put(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (nrules < 32u) {
        rules[nrules][0] = (int16_t)x; rules[nrules][1] = (int16_t)y;
        rules[nrules][2] = (int16_t)(x + w); rules[nrules][3] = (int16_t)(y + h);
        nrules++;
    }
}
static void rules_cover(int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    uint32_t i, k = 0;
    for (i = 0; i < nrules; i++)
        if (!(rules[i][0] >= x0 && rules[i][2] <= x1 && rules[i][1] >= y0 && rules[i][3] <= y1))
            memcpy(rules[k++], rules[i], sizeof rules[i]);
    nrules = k;
}
#define GFX_HOOK_TEXT(x0, y0, x1, y1, s, flags) hk_text(x0, y0, x1, y1, s, flags)
#define GFX_HOOK_BLIT(x, y, r0) hk_blit(x, y, r0, cv_w, cv_h)
#define GFX_HOOK_BEGIN() (npend = ncells = 0, al_flush())
#define GFX_HOOK_CELL(x0, y0, x1, y1) hk_cell(x0, y0, x1, y1)
#define GFX_HOOK_RULE(x, y, w, h) hk_rule(x, y, w, h)
static uint32_t nink;                           /* GFX_HOOK_INK: inks drawn on their own colour */
static void hk_ink(uint16_t ink, uint16_t bg);
#define GFX_HOOK_INK(ink, bg) hk_ink(ink, bg)
#define GFX_HOOK_PIXELS(n) (px_visited += (n))
#define UI_FILL_HOOK(x, y, w, h) hk_fill(x, y, w, h)
static void hk_overlay(int32_t x0, int32_t y0, int32_t x1, int32_t y1);
#define GFX_HOOK_COVER(x0, y0, x1, y1) hk_overlay(x0, y0, x1, y1)
static void fm6_hook(int32_t k, int32_t x, int32_t y, int32_t r);
#define VX_FM6_HOOK(k, x, y, r) fm6_hook(k, x, y, r)
/* MENU > LARGE: the tall cards' values by the face they got (ui_draw.c LARGE_HOOK), the characters L lacked */
static uint64_t lg_n[4];
static char lg_miss[96], lg_wide[16][12], lg_small[16][12];
static uint32_t lg_nwide, lg_nsmall;
static void lg_hook(const char *s, int why);
#define LARGE_HOOK(s, why) lg_hook(s, why)
#define UI_TEST_NO_MAIN 1
#define PF_MS 900u                                       /* (0.6.61) the page name drawn (ui_test.c: off) */
#include "ui_test.c"

static uint8_t large_on;                         /* the LARGE pass: every scene with MENU > LARGE ON */
static void lg_hook(const char *s, int why)
{
    uint32_t i;
    lg_n[why & 3]++;
    if (why == 1)
        for (; *s; s++) {
            uint32_t c = fold(&AF_L, (uint8_t)*s);
            if (glyph_at(&AF_L, c) < 0 && !strchr(lg_miss, (int)c) && strlen(lg_miss) + 1u < sizeof lg_miss)
                lg_miss[strlen(lg_miss)] = (char)c;
        }
    if (why == 2 || why == 3) {                     /* a few of each, distinct */
        char (*l)[12] = why == 2 ? lg_wide : lg_small;
        uint32_t *n = why == 2 ? &lg_nwide : &lg_nsmall;
        for (i = 0; i < *n && strcmp(l[i], s); i++) ;
        if (i == *n && *n < 16u) snprintf(l[(*n)++], 12, "%s", s);
    }
}

/* an icon drawn: its ink, read from its cell's nibbles here (not icons.c icon_ink's table: measured, not trusted) */
/* n alpha values of a cell from its bit stream (the canonical Huffman code AI_HC, as gen_aa_font.py huff_unpack:
 * decoded here bit by bit, not by gfx.c's table) */
static void icon_unpack(const uint8_t *d, uint8_t *out, uint32_t n)
{
    uint32_t k = 0, pos = 0;
    while (k < n) {
        uint32_t code = 0, first = 0, idx = 0, len, s, r;
        for (len = 0; len < 15u; len++) {
            code |= (d[pos >> 3] >> (7u - (pos & 7u))) & 1u;
            pos++;
            if (code - first < AI_HC[len]) break;
            idx += AI_HC[len];
            first = (first + AI_HC[len]) << 1;
            code <<= 1;
        }
        s = AI_HC[15u + idx + code - first];
        for (r = 0; r <= s >> 4 && k < n; r++) out[k++] = (uint8_t)(s & 15u);
    }
}
static void hk_icon(int32_t x, int32_t y, uint32_t size, uint32_t id)
{
    int32_t k, x0 = 99, y0 = 99, x1 = -1, y1 = -1;
    uint32_t i;
    const uint8_t *c;
    uint8_t cell[24 * 24];
    if (!al_n || (k = icon_cell(&size, id)) < 0) return;
    c = size == 24u ? AI24_DATA + AI24_OFF[k] : size == 16u ? AI16_DATA + AI16_OFF[k] : AI12_DATA + AI12_OFF[k];
    icon_unpack(c, cell, size * size);
    for (i = 0; i < size * size; i++)
        if (cell[i]) {
            int32_t px = (int32_t)(i % size), py = (int32_t)(i / size);
            if (px < x0) x0 = px;
            if (px + 1 > x1) x1 = px + 1;
            if (py < y0) y0 = py;
            if (py + 1 > y1) y1 = py + 1;
        }
    if (cv_flip) { int32_t t = x0; x0 = (int32_t)size - x1; x1 = (int32_t)size - t; }   /* (mirrored: the metronome) */
    al_item(x + x0, y + y0, x + x1, y + y1, 1, "icon");
}

/* (0.9.6) the FM6 colony lint: each operator's bicho (its centre, its largest radius, Q4 px) as drawn; at the sixth the
 * six checked: apart by a pixel at least, at the largest a full LEVEL and envelope make them (vx_fm6 says), and
 * inside the panel */
static int32_t fmp_x[6], fmp_y[6], fmp_r[6];
static uint32_t fmp_charts;
static void finding(const char *what, const rbox_t *b, const rbox_t *o);
static void fm6_hook(int32_t k, int32_t x, int32_t y, int32_t r)
{
    int32_t i, j;
    fmp_x[k] = x; fmp_y[k] = y; fmp_r[k] = r;
    if (k < 5)
        return;
    for (i = 0; i < 6; i++) {
        rbox_t b = {(int16_t)((fmp_x[i] - fmp_r[i]) >> 4), (int16_t)((fmp_y[i] - fmp_r[i]) >> 4), (int16_t)((fmp_x[i] + fmp_r[i]) >> 4),
                    (int16_t)((fmp_y[i] + fmp_r[i]) >> 4), 0, {0}}, o = b;
        snprintf(b.s, sizeof b.s, "FM6 OP%d", (int)i + 1);
        if (b.x0 < 4 || b.x1 > 236 || b.y0 < 0 || b.y1 >= (int32_t)cv_h) {
            snprintf(o.s, sizeof o.s, "panel");
            finding("FM6 bicho off the panel", &b, &o);
        }
        for (j = 0; j < i; j++) {
            int64_t dx = fmp_x[i] - fmp_x[j], dy = fmp_y[i] - fmp_y[j], d = fmp_r[i] + fmp_r[j] + 16;
            if (dx * dx + dy * dy < d * d) {
                snprintf(o.s, sizeof o.s, "FM6 OP%d", (int)j + 1);
                finding("FM6 bichos overlap", &b, &o);
            }
        }
    }
    fmp_charts++;
}

static void finding(const char *what, const rbox_t *b, const rbox_t *o)
{
    static char seen[600][64];
    static uint32_t nseen;
    char key[64];
    uint32_t i;
    nfind++;
    snprintf(key, sizeof key, "%.20s|%.14s|%.24s", cur_name, what, b->s);
    for (i = 0; i < nseen && strcmp(seen[i], key); i++) ;
    if (i < nseen) return;                       /* each finding once per screen */
    if (nseen < 600u) strcpy(seen[nseen++], key);
    else return;
    fprintf(rep, "LINT %-22s %-32s '%s' (%d,%d)-(%d,%d)", cur_name, what, b->s, b->x0, b->y0, b->x1, b->y1);
    if (o) fprintf(rep, " / '%s' (%d,%d)-(%d,%d)", o->s, o->x0, o->y0, o->x1, o->y1);
    fputc('\n', rep);
}
/* a rectangle of the screen is drawn again: boxes inside it are gone, boxes it covers in part are hidden */
static void cover(int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    uint32_t i, k = 0;
    for (i = 0; i < nscr; i++) {
        rbox_t *b = &scr[i];
        int in = b->x0 >= x0 && b->x1 <= x1 && b->y0 >= y0 && b->y1 <= y1;
        int touch = b->x0 < x1 && x0 < b->x1 && b->y0 < y1 && y0 < b->y1;
        if (in) continue;
        if (touch && !(b->flags & 48u)) finding("hidden in part by a later draw", b, 0);
        if (touch && (b->flags & 48u)) scr[k++] = *b;   /* split across bands / scrolled: drawn on purpose */
        else scr[k++] = *b;
    }
    nscr = k;
}
static void hk_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    cover((int32_t)x, (int32_t)y, (int32_t)(x + w), (int32_t)(y + h));
    rules_cover((int32_t)x, (int32_t)y, (int32_t)(x + w), (int32_t)(y + h));
}
/* a text across two bands (the menu and the document draw 124 + 85 rows): each band draws its part, the same
 * screen box twice, cut at the band edge; together they are whole (flag 16) */
static int split_pair(rbox_t *b)
{
    uint32_t j;
    if (!(b->flags & 2u)) return 0;
    for (j = 0; j < nscr; j++) {
        rbox_t *o = &scr[j];
        if ((o->flags & 2u) && o->x0 == b->x0 && o->x1 == b->x1 && o->y0 == b->y0 && o->y1 == b->y1 && !strcmp(o->s, b->s)) {
            o->flags = (uint8_t)((o->flags & ~2u) | 16u);
            return 1;
        }
    }
    return 0;
}
/* a text or icon box that touches a cell or card of its canvas (GFX_HOOK_CELL) but spills past its edge; for the
 * audit, a text inside its smallest cell closer than 2 px to an edge */
static void contain(int32_t x, int32_t y)
{
    uint32_t i, c;
    for (i = 0; i < npend; i++) {
        const rbox_t *b = &pend[i];
        int32_t best = 1 << 30, gap = 99;
        for (c = 0; c < ncells && !(b->flags & 4u); c++) {   /* (texts only: an icon's box is its whole cell) */
            const int16_t *r = cells[c];
            int32_t a = (r[2] - r[0]) * (r[3] - r[1]), g;
            if (b->x0 < r[0] || b->x1 > r[2] || b->y0 < r[1] || b->y1 > r[3] || a >= best) continue;
            best = a;
            g = b->x0 - r[0];
            if (r[2] - b->x1 < g) g = r[2] - b->x1;
            if (b->y0 - r[1] < g) g = b->y0 - r[1];
            if (r[3] - b->y1 < g) g = r[3] - b->y1;
            gap = g;
        }
        if (gap < 2 && aud) {
            ntight++;
            if (strlen(tight) + strlen(b->s) + 8u < sizeof tight)
                snprintf(tight + strlen(tight), sizeof tight - strlen(tight), "%s'%s' %dpx", tight[0] ? ", " : "", b->s, (int)gap);
        }
        for (c = 0; c < ncells; c++) {
            const int16_t *r = cells[c];
            int touch = b->x0 < r[2] && r[0] < b->x1 && b->y0 < r[3] && r[1] < b->y1;
            int in = b->x0 >= r[0] && b->x1 <= r[2] && b->y0 >= r[1] && b->y1 <= r[3];
            int around = r[0] >= b->x0 && r[2] <= b->x1 && r[1] >= b->y0 && r[3] <= b->y1;   /* (a mark inside the box) */
            if (touch && !in && !around) {
                rbox_t t = *b, o = {(int16_t)(r[0] + x), (int16_t)(r[1] + y), (int16_t)(r[2] + x), (int16_t)(r[3] + y), 0, "cell"};
                t.x0 += (int16_t)x; t.x1 += (int16_t)x; t.y0 += (int16_t)y; t.y1 += (int16_t)y;
                nspill++;
                finding("spills out of its cell", &t, &o);
                break;
            }
        }
    }
}
/* (0.9.5) an overlay drawn over its canvas (ui_graph.c toast_pill): the cells under it are hidden by it, on purpose (the
 * page's graph goes on under a toast); a text of the page under it is a finding (the pill keeps clear of the page's
 * words: gfx.c cv_txt, MUTATE's lines, AUTO LIST's rows, MOTION's, PUNCH's; none clear: no pill) */
static void hk_overlay(int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    uint32_t i, k = 0;
    al_flush();
    for (i = 0; i < npend; i++)
        if (!(pend[i].x0 < x1 && x0 < pend[i].x1 && pend[i].y0 < y1 && y0 < pend[i].y1))
            pend[k++] = pend[i];
        else {
            rbox_t o = {(int16_t)x0, (int16_t)y0, (int16_t)x1, (int16_t)y1, 0, "toast pill"};
            finding("page text under the toast pill", &pend[i], &o);
        }
    npend = k;
    for (i = k = 0; i < ncells; i++)
        if (!(cells[i][0] < x1 && x0 < cells[i][2] && cells[i][1] < y1 && y0 < cells[i][3]))
            memmove(cells[k++], cells[i], sizeof cells[i]);
    ncells = k;
}
/* (0.9.5) a screen box under one of the holes a NOTICE keeps (gfx.c scr_holes): never sent, the dialog's there */
static int in_hole(const rbox_t *b)
{
    uint32_t j;
    for (j = 0; j < scr_nhole; j++)
        if (b->x0 < scr_hole[j][0] + scr_hole[j][2] && scr_hole[j][0] < b->x1 && b->y0 < scr_hole[j][1] + scr_hole[j][3] &&
            scr_hole[j][1] < b->y1)
            return 1;
    return 0;
}
/* (0.9.5) a divider less the NOTICE's holes (what of it the screen got: gfx.c sends nothing there) */
static void hk_rule_k(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t k)
{
    int32_t hx0, hy0, hx1, hy1, a, b;
    if (x1 <= x0 || y1 <= y0)
        return;
    if (k >= scr_nhole) {
        hk_rule_put((uint32_t)x0, (uint32_t)y0, (uint32_t)(x1 - x0), (uint32_t)(y1 - y0));
        return;
    }
    hx0 = scr_hole[k][0]; hy0 = scr_hole[k][1]; hx1 = hx0 + scr_hole[k][2]; hy1 = hy0 + scr_hole[k][3];
    if (hx0 >= x1 || hx1 <= x0 || hy0 >= y1 || hy1 <= y0) {
        hk_rule_k(x0, y0, x1, y1, k + 1u);
        return;
    }
    a = hy0 > y0 ? hy0 : y0;
    b = hy1 < y1 ? hy1 : y1;
    hk_rule_k(x0, y0, x1, a, k + 1u);
    hk_rule_k(x0, b, x1, y1, k + 1u);
    hk_rule_k(x0, a, hx0 < x1 ? hx0 : x1, b, k + 1u);
    hk_rule_k(hx1 > x0 ? hx1 : x0, a, x1, b, k + 1u);
}
static void hk_blit(uint32_t x, uint32_t y, uint32_t r0, uint32_t w, uint32_t h)
{
    uint32_t i, k = 0;
    al_flush();
    contain((int32_t)x, (int32_t)y);
    ncells = 0;
    blit_px += (uint64_t)w * (h > r0 ? h - r0 : 0u);
    for (i = 0; i < npend; i++) {
        rbox_t b = pend[i];
        if (b.y1 <= (int32_t)r0) continue;           /* rows not blitted */
        if (b.y0 < (int32_t)r0) b.flags |= 2u;
        b.x0 += (int16_t)x; b.x1 += (int16_t)x; b.y0 += (int16_t)y; b.y1 += (int16_t)y;
        if (in_hole(&b)) continue;                   /* (under a NOTICE's box: not on the screen) */
        if (!split_pair(&b)) pend[k++] = b;
    }
    cover((int32_t)x, (int32_t)(y + r0), (int32_t)(x + w), (int32_t)(y + h));
    rules_cover((int32_t)x, (int32_t)(y + r0), (int32_t)(x + w), (int32_t)(y + h));
    for (i = 0; i < k && nscr < 1200u; i++)
        scr[nscr++] = pend[i];
    npend = 0;
}

static void lint(void)
{
    uint32_t i, j;
    for (i = 0; i < nscr; i++) {
        const rbox_t *b = &scr[i];
        if (b->x0 < 0 || b->y0 < 0 || b->x1 > 240 || b->y1 > 240) finding("off the screen", b, 0);
        if ((b->flags & 2u) && !(b->flags & 32u)) finding("cut by its canvas", b, 0);
        if ((b->flags & 1u) && !(b->flags & 8u)) finding("ellipsised (not free text)", b, 0);
        if ((b->flags & 9u) == 9u) {
            for (j = 0; j < nfree_seen && strcmp(free_seen[j], b->s); j++) ;
            if (j == nfree_seen && nfree_seen < 64u) {
                snprintf(free_seen[nfree_seen++], 40, "%s", b->s);
                fprintf(rep, "free   %-22s ellipsised '%s'\n", cur_name, b->s);
            }
            nfree++;
        }
        for (j = i + 1u; j < nscr; j++) {
            const rbox_t *o = &scr[j];
            if (b->x0 < o->x1 && o->x0 < b->x1 && b->y0 < o->y1 && o->y0 < b->y1) finding("overlaps", b, o);
        }
        for (j = 0; j < nrules; j++) {               /* a divider is a cell edge: nothing on it */
            const int16_t *r = rules[j];
            rbox_t o = {r[0], r[1], r[2], r[3], 0, "divider"};
            if (b->x0 < r[2] && r[0] < b->x1 && b->y0 < r[3] && r[1] < b->y1) finding("touches a divider", b, &o);
        }
    }
}

/* the audit of one screen (GREY): its texts (count, ink-box px), icons, keycaps, the ellipsised and tight texts,
 * and the words themselves */
static FILE *audf;
static void audit_scene(const char *name)
{
    uint32_t i, nt = 0, ni = 0, nk = 0, ne = 0, at = 0, ai = 0, ak = 0;
    char words[1200] = "";
    if (!audf) return;
    for (i = 0; i < nscr; i++) {
        const rbox_t *b = &scr[i];
        uint32_t a = (uint32_t)((b->x1 - b->x0) * (b->y1 - b->y0));
        if ((b->flags & 4u) && !strcmp(b->s, "icon")) { ni++; ai += a; continue; }
        if (b->flags & 4u) { nk++; ak += a; continue; }
        nt++; at += a;
        if (b->flags & 1u) ne++;
        if (strlen(words) + strlen(b->s) + 4u < sizeof words)
            snprintf(words + strlen(words), sizeof words - strlen(words), "%s%s", words[0] ? " | " : "", b->s);
    }
    fprintf(audf, "%s\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%s\t%s\n", name, nt, at, ni, ai, nk, ak, ne, ntight, words, tight);
}
/* GREY: every pixel RGB565 gray (R = B, G = 2 R); MONO (black and white, its edges blended per channel): neutral
 * (R = B, G within 2 of 2 R, the 5-6-5 rounding). Other palettes: nothing */
static uint32_t mono_bad;
static void mono_check(void)
{
    uint32_t i, bw = settings.palette == UI_BW_INDEX;
    if (settings.palette != UI_GREY_INDEX && !bw) return;
    for (i = 0; i < 240u * 240u; i++) {
        uint16_t c = swap16(host_screen[i]);
        int32_t d = (int32_t)((c >> 5) & 63u) - (int32_t)(c >> 11) * 2;
        if ((c >> 11) != (c & 31u) || (bw ? d < -2 || d > 2 : d != 0)) {
            if (!mono_bad) fprintf(rep, "%s %s: pixel %u,%u = %04x off %s\n", bw ? "MONO" : "GREY", cur_name, i % 240u, i / 240u, c,
                                   bw ? "neutral" : "gray");
            mono_bad++;
        }
    }
}
static void hk_ink(uint16_t ink, uint16_t bg)
{
    if (ink != bg) return;
    if (nink < 20u) fprintf(rep, "%s: ink %04x drawn on its own colour (%s)\n", cur_name, ink, UI_PALETTES[settings.palette % NPALETTES].name);
    nink++;
}
static void write_ppm(const char *dir, const char *pal, const char *name)
{
    char path[512];
    uint32_t i;
    FILE *f;
    snprintf(path, sizeof path, "%s/ppm/%s_%s.ppm", dir, pal, name);
    f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); return; }
    fprintf(f, "P6\n240 240\n255\n");
    for (i = 0; i < 240u * 240u; i++) {
        uint16_t c = swap16(host_screen[i]);
        uint8_t b[3] = {(uint8_t)((c >> 11) * 255u / 31u), (uint8_t)(((c >> 5) & 63u) * 255u / 63u), (uint8_t)((c & 31u) * 255u / 31u)};
        fwrite(b, 1, 3, f);
    }
    fclose(f);
}

/* ------------------------------------------------------------------ the screens --- */
static void pal(uint32_t p) { settings.palette = p; palette_set(p); }
static void state(void)                          /* a playing song with steps on every track */
{
    uint32_t i;
    ui_power_on();
    dx_mute_set(0);
    memset(macro_v, 0, sizeof macro_v);
    if (large_on) ui_prefs |= PREF_LARGE;
    song.playing = 1;
    song.g[G_BPM] = 124;
    song.g[G_STRN] = 0;                           /* (lente_bar_long: ALL TR set) */
    clk_pos = 0; clk_step = 8; cin_total = cin_left = 0;   /* the bar's 3rd beat: the header's metronome at rest */
    for (i = 0; i < NTRK; i++) trk[i].seq_idx = 5;
    my_steps(&trk[1]);
    trk[0].step[2].flags |= SF_ACCENT;
    trk[0].step[6].flags |= SF_SLIDE;
    for (i = 0; i < SCOPE_N; i++)                 /* a stand-in signal for the scope */
        scope_buf[i] = (int16_t)((int32_t)((i * 37u) % 128u) * 200 - 12800 + (int32_t)((i % 32u) < 16u ? 3000 : -3000));
    scope_w = 0;
}
static void drum(uint32_t kit)
{
    uint32_t i;
    song.sel = 3;
    trk[3].p[P_E0] = (int16_t)kit;
    for (i = 0; i < 16u; i++) {
        step_t *s = &trk[3].step[i];
        memset(s, 0, sizeof *s);
        s->time = ST_NOTE; s->vel = 100;
        if (i % 4u == 0u) s->hit |= 1u;
        if (i == 4u || i == 12u) s->hit |= 2u, s->acc |= 2u;
        if (i == 10u) s->hit |= 4u;
        if (i % 2u == 0u) s->hit |= 8u;
        if (i == 14u) s->hit |= 16u;
        if (i == 7u || i == 15u) s->hit |= 32u;
        if (i == 3u) s->hit |= 64u;
        if (i == 0u) s->hit |= 128u, s->acc |= 128u;
    }
    trk[3].p[P_SLEN] = 32;
}
static void eng(uint32_t e) { set_engine_of(TSEL, e); }
#define ENGI_VOICE_UI 5u                         /* (0.8.5) VOICE (engines.c ENGINES[5]) */
/* the FM engine of the FM screens: DIGITAL with FELUCCA_FM4, else FM6 (DIGITAL retired: its own screens, the OP ENV /
 * OP LEVEL pages and the algorithm charts, exist only there: fm4_screen) */
#define E_FM (FELUCCA_FM4 ? ENGI_DIGITAL : ENGI_FM6)

enum { S_HOME, S_HOME_IDLE, S_MESSAGE, S_MESSAGE_KEY, S_MESSAGE_NOFILE, S_PRESETS, S_PRESETS_NOFAV, S_USER, S_PHRASES, S_PROJECT, S_TOOLS,
       S_SONG_EMPTY, S_SONG, S_STEP, S_PATTERN, S_CHANCE, S_COMP, S_COMP_TRIG, S_COMP_ECHO, S_MUTATE, S_MORPH, S_DXIMG, S_DXSEND, S_PFLASH, S_PFLASH_HOME, S_FUGUE, S_MOTION, S_DRUM, S_DRUM_HAND, S_DRUM_CYM, S_MIXER, S_MIXER_PAN,
       S_ENV, S_ENVDEST, S_LFO, S_MOD, S_FX, S_SLICER, S_SLBUS, S_SLPITCH, S_DLY, S_SCL, S_SHIFT, S_ARP, S_VOICE, S_GLOBAL, S_SYSTEM,
       S_EDIT_ANALOG, S_EDIT_DIGITAL, S_OP_ENV, S_EDIT_WHEEL, S_EDIT_SAMPLE, S_EDIT_GRAIN,
       S_ALG1, S_ALG2, S_ALG3, S_ALG4, S_ALG5, S_ALG6, S_ALG7, S_ALG8, S_OP_LEVEL,
       S_FM6_ALG1, S_FM6_ALG5, S_FM6_ALG22, S_FM6_ALG32, S_DRUMX, S_DRUMX_HIT, S_DX_SOUND, S_DX_SOUND2,
       S_CONFIRM_SEQ, S_CONFIRM_PROJ, S_CONFIRM_USER, S_CONFIRM_PAT, S_CONFIRM_MOTION, S_CONFIRM_ERASE, S_GEN, S_GEN_PARTS,
       S_MENU, S_MENU_SPEAKER, S_ABOUT, S_ABOUT_REC, S_ABOUT_CREDITS, S_ABOUT_END, S_UBOOT, S_CALIBRATION, S_SPLASH,
       S_BATT0, S_BATT1, S_BATT2, S_BATT3, S_BATT_USB, S_MOTION_REC, S_MOTION_OFF, S_MOTION_CARD, S_SONG_HOME,
       S_FX_PEEK, S_FX_HELD, S_FX_WAIT, S_FX_HARM, S_MENU_HOLD, S_MENU_LEDS, S_MENU_END, S_MENU_SYSTEM, S_MENU_SLIDE, S_REVERB, S_REVERB_RESO,
       S_GLO_PEEK, S_GLO_ACTIVE, S_GLO_EXT, S_SCL_PEEK, S_SCL_ACTIVE, S_EDIT_PEEK, S_EDIT_ACTIVE, S_EDIT_USER, S_EDIT_DRUM, S_LAYER_HINT, S_LAYER_LOCK, S_LAYER_LOCK_FX, S_LAYER_SONG, S_LAYER_SONG_CHAIN,
       S_SEQ_TOOLS, S_SEQ_TOOLS_DRUM, S_SEQ_TOOLS_SONG,
       S_LENTE_PAT, S_LENTE_PAT_DRUM, S_LENTE_STRETCH, S_LENTE_BAR, S_LENTE_BAR_DRUM, S_LENTE_STEP, S_LENTE_STEP_COMP,
       S_LENTE_STEP_COMP2, S_LENTE_STEP_TIE, S_LENTE_STEP_REST, S_LENTE_STEP_DRUM, S_LENTE_BAR_LONG, S_LENTE_STEP_LONG,
       S_SEQ_BAR, S_SEQ_BAR_DRUM,
       S_NAME_USER, S_NAME_TYPING, S_NAME_123, S_NAME_EMPTY, S_NAME_FULL, S_NAME_PLAYING, S_PROJECT_NAMED, S_SONG_NAMED,
       S_USER_FOOT,
       S_WORDS, S_WORDS_EN, S_WORDS_JA, S_WORDS_EDIT, S_WORDS_OFF, S_NAME_LYRIC, S_NAME_LYRIC_LONG,
       S_ROLL_EMPTY, S_ROLL_ACID, S_ROLL_CHORDS, S_ROLL_TIES, S_ROLL_LEN32, S_ROLL_HIGH, S_ROLL_LOW, S_ROLL_WIDE, S_ROLL_PLAYING,
       S_ROLL_LOCKS, S_ROLL_LOCK_HELD, S_DRUM_LOCKS, S_DRUM_LOCK_HELD, S_MOTION_LOCKS, S_AUTO_LIST, S_AUTO_LIST_ADD, S_AUTO_LIST_DRUM, S_REC_LAYER, S_REC_LAYER_SET, S_REC_LAYER_SONG, S_MACRO_LAYER, S_MSEQ, S_DIST_PAGE, S_DLY2, S_REVERB2, S_CHORUS, S_MASTER, S_LFO_DEST, S_LFO2, S_LFO2_FREE,
       S_COUNTIN, S_COUNTIN2, S_MENU_AUDIO, S_HEAD_PLAY_REC, S_HEAD_REC_OTHER, S_HEAD_BPM_TURN, S_HEAD_BPM_LOCK,
       S_HEAD_BPM_LOCKED, S_HEAD_GLO_TURN, S_HEAD_DOWNBEAT, S_HEAD_BEAT_2,
       S_SAVE_LIB, S_SAVE_TOOLS, S_SAVE_PROJ, S_SAVE_ARR, S_SAVE_SOUND, S_SAVE_PRESETS,
       S_FXL_SENDS, S_FXL_DIST, S_FXL_PAN, S_FXL_BUSES, S_FXL_DELAY, S_FXL_DELAY2, S_FXL_REVERB, S_FXL_SLICER, S_FXL_MASTER,
       S_HOME_MACROS, S_HOME_BEING, S_HOME_ROUTES, S_GEN_LEAD,
       S_ED_FILTER, S_ED_FILTER_BP, S_ED_VOICE2, S_ED_FOCUS, S_ED_FOCUS_MOD, S_ED_OPERATOR,
       S_ENV_TRACKS, S_ENV_TRACKS_DEST, S_ENV_STAGE, S_ENV_STAGE_SUS, S_MSEQ2, S_KEY_CHORD, S_KEY_CHORD_OFF, S_KEY_ARP, S_KEY_ARP2, S_GLO_SENDS, S_MENU_ROW, S_MENU_ROW_LEDS, S_LY_FX_KNOBS, S_LY_FX_STATE, S_LY_GLO_STATE, S_DLG_DICE_SOUND, S_DLG_DICE_SEQ, S_DLG_DICE_FX, S_DLG_MUT_BACK,
       S_DRUM_X4, S_DRUM_GHOST, S_DRUM_OCT_UP, S_DRUM_OCT_DN, S_COMP_TOUCH, S_COMP_EVEN, S_GEN_EXPLORE,
       S_VX_HEAD_A, S_VX_HEAD_I, S_VX_HEAD_U, S_VX_HEAD_S, S_VX_HEAD_M, S_VX_HEAD_K, S_VX_HEAD_T, S_VX_HEAD_F, S_VX_HEAD_VOWEL,
       S_VX_HEAD_HOME, S_VX_HEAD_QUIET, S_VX_BIRD, S_VX_CMOS, S_VX_INSECT, S_VX_WING, S_VX_MAMMAL, S_VX_MORPH_IB, S_VX_MORPH_BM,
       S_VX_ANIMAL_HOME, S_VX_KIT, S_VX_KIT_HITS, S_VX_KIT_GHOST, S_VX_KIT_HOME, S_VX_KIT_MUTED,
       S_VX_BUG_BD, S_VX_BUG_SD, S_VX_BUG_CP, S_VX_BUG_CH, S_VX_BUG_OH, S_VX_BUG_TM, S_VX_BUG_RS, S_VX_BUG_CB, S_VX_BUG_GHOST,
       S_VX_BUG_FLAM, S_VX_BUG_RATCH, S_VX_BUG_QUIET,
       S_V6_ANALOG, S_V6_ANALOG_SQR, S_V6_ANALOG_RING, S_V6_ANALOG_QUIET, S_V6_ANALOG_HOME, S_V6_ANALOG_FULL, S_V6_FM6, S_V6_FM6_ALG32, S_V6_FM6_OP, S_V6_FM6_OP2, S_V6_FM6_ENV, S_V6_FM6_ENV_REL, S_V6_FM6_HOME, S_V6_FM6_FULL, S_V6_WHEEL, S_V6_WHEEL_FAST, S_V6_WHEEL_HOME, S_V6_WHEEL_FULL, S_V6_NOISE, S_V6_NOISE_LFSR, S_V6_NOISE_WHITE, S_V6_NOISE_HOME, S_V6_NOISE_FULL, S_V6_PHASE, S_V6_PHASE_RING, S_V6_PHASE_HOME, S_V6_PHASE_FULL, S_V6_LOFI, S_V6_LOFI_FLOAT, S_V6_LOFI_PLS, S_V6_LOFI_HOME, S_V6_LOFI_FULL, S_V6_VOICE_FULL, S_V6_ANIMAL_FULL, S_VX_TRACT_A_FULL, S_VX_TRACT_M_FULL, S_VX_TRACT_A_EDIT, S_VX_TRACT_M_EDIT, S_VX_TRACT_A_HOME, S_VX_TRACT_M_HOME, S_V6_STRING, S_V6_STRING_BOW, S_V6_STRING_HOME, S_V6_STRING_FULL, S_V6_ADD, S_V6_ADD_BELL, S_V6_ADD_HOME, S_V6_ADD_FULL, S_HOME_MUT, S_HOME_FACES,
       S_LIVE_RIELES, S_LIVE_RIELES_T3, S_LIVE_MANO, S_LIVE_MANO_KEYS,
       S_MIXER_T4, S_HOME_T4, S_MOCK_HOME, S_MOCK_PRESETS, S_MOCK_SEQ, S_MOCK_DRUM, S_MOCK_MIXER, S_MOCK_DIALOG, S_MOCK_MENU,
       S_TOAST_ENV, S_TOAST_MUTATE, S_TOAST_MUTATE_HINT, S_TOAST_MOTION, S_TOAST_MOTION_LOCKS, S_TOAST_PUNCH, S_TOAST_AUTO_LIST,
       S_TOAST_AUTO_LIST_DRUM, S_TOAST_PATTERN, S_TOAST_FM6_OP, S_TOAST_ANALOG, S_COUNT };
static const char *const S_NAME[S_COUNT] = {"home", "home_idle", "message", "message_key", "message_nofile", "presets", "presets_nofav", "user",
    "phrases", "project", "tools", "song_empty", "song", "step", "pattern", "chance", "comp", "comp_trig", "comp_echo", "mutate", "morph", "dx_image", "dx_send", "page_flash", "page_flash_home", "fugue", "motion", "drum",
    "drum_hand", "drum_cym", "mixer", "mixer_pan", "env", "env_dest", "lfo", "mod", "fx", "slicer", "slbus", "slpitch", "dly", "scl", "shift", "arp",
    "voice", "global", "system", "edit_analog", FELUCCA_FM4 ? "edit_digital" : "edit_fm6", "op_env", "edit_wheel", "edit_voice",
    "edit_noise", "alg_1", "alg_2", "alg_3", "alg_4", "alg_5", "alg_6", "alg_7", "alg_8", "op_level", "fm6_alg_01", "fm6_alg_05", "fm6_alg_22", "fm6_alg_32", "drumx", "drumx_hit", "dx_sound", "dx_sound2", "confirm_seq", "confirm_project", "confirm_user", "confirm_pattern",
    "confirm_motion", "confirm_erase", "gen", "gen_parts", "menu", "menu_speaker", "about", "about_rec", "about_credits", "about_end", "uboot", "calibration", "splash",
    "batt_0", "batt_1", "batt_2", "batt_3", "batt_usb", "motion_rec", "motion_off", "motion_card", "song_home",
    "perform_peek", "perform_held", "perform_wait", "perform_harm", "menu_hold", "menu_leds", "menu_end", "menu_system", "menu_slide", "reverb_spring", "reverb_reso",
    "layer_glo_peek", "layer_glo_active", "layer_glo_ext", "layer_scl_peek", "layer_scl_active", "layer_edit_peek",
    "layer_edit_active", "layer_edit_user", "layer_edit_drum", "layer_hint", "layer_lock", "layer_lock_fx", "layer_song", "layer_song_chain",
    "layer_seq_tools", "layer_seq_tools_drum", "layer_seq_tools_song",
    "lente_pattern", "lente_pattern_drum", "lente_stretch", "lente_bar", "lente_bar_drum", "lente_step_note",
    "lente_step_comp", "lente_step_comp2", "lente_step_tie", "lente_step_rest", "lente_step_drum", "lente_bar_long",
    "lente_step_long", "seq_bar", "seq_bar_drum",
    "name_user", "name_typing", "name_123", "name_empty", "name_full", "name_playing", "project_named", "song_named",
    "user_foot",
    "words", "words_en", "words_ja", "words_edit", "words_off", "name_lyric", "name_lyric_long",
    "roll_empty", "roll_acid", "roll_chords", "roll_ties", "roll_len32_p2", "roll_high", "roll_low", "roll_wide", "roll_playing",
    "roll_locks", "roll_lock_held", "drum_locks", "drum_lock_held", "motion_locks", "auto_list", "auto_list_add", "auto_list_drum", "layer_rec", "layer_rec_set", "layer_rec_song", "layer_macro", "mseq", "dist", "dly2", "reverb2", "chorus", "master", "lfo_dest", "lfo2", "lfo2_free",
    "countin", "countin_2bars", "menu_audio", "head_play_rec", "head_rec_other", "head_bpm_turn", "head_bpm_lock",
    "head_bpm_locked", "head_glo_turn", "head_downbeat", "head_beat_2",
    "save_library", "save_tools", "save_project", "save_arrange", "save_sound", "save_presets",
    "fx_track_sends", "fx_track_dist", "fx_track_pan", "fx_buses", "fx_effect_delay", "fx_effect_delay2", "fx_effect_reverb",
    "fx_effect_slicer", "fx_effect_master",
    "home_macros", "home_being", "home_routes", "gen_lead",
    "ed_filter", "ed_filter_bp", "ed_voice2", "ed_focus", "ed_focus_mod", "ed_operator",
    "env_tracks", "env_tracks_dest", "env_stage", "env_stage_sus", "mseq2", "key_chord", "key_chord_off", "key_arp", "key_arp2", "glo_sends", "menu_row", "menu_row_leds", "ly_fx_knobs", "ly_fx_state", "ly_glo_state", "dlg_dice_sound", "dlg_dice_seq", "dlg_dice_fx", "dlg_mut_back",
    "drum_hit_x4", "drum_hit_ghost", "drum_oct_x8", "drum_oct_flam", "comp_touch", "comp_echo_even", "gen_explore",
    "vx_head_a", "vx_head_i", "vx_head_u", "vx_head_s", "vx_head_m", "vx_head_k", "vx_head_t", "vx_head_f", "vx_head_vowel",
    "vx_head_home", "vx_head_quiet", "vx_bird", "vx_cmos", "vx_insect", "vx_wing", "vx_mammal", "vx_morph_insect_bird", "vx_morph_bird_mammal",
    "vx_animal_home", "vx_kit", "vx_kit_hits", "vx_kit_ghost", "vx_kit_home", "vx_kit_muted",
    "vx_bug_bd", "vx_bug_sd", "vx_bug_cp", "vx_bug_ch", "vx_bug_oh", "vx_bug_tm", "vx_bug_rs", "vx_bug_cb", "vx_bug_ghost",
    "vx_bug_flam", "vx_bug_ratchet", "vx_bug_quiet",
    "v6_analog", "v6_analog_sqr", "v6_analog_ring", "v6_analog_quiet", "v6_analog_home", "v6_analog_full", "v6_fm6", "v6_fm6_alg32", "v6_fm6_op", "v6_fm6_op2", "v6_fm6_env", "v6_fm6_env_rel", "v6_fm6_home", "v6_fm6_full", "v6_wheel", "v6_wheel_fast", "v6_wheel_home", "v6_wheel_full", "v6_noise", "v6_noise_lfsr", "v6_noise_white", "v6_noise_home", "v6_noise_full", "v6_phase", "v6_phase_ring", "v6_phase_home", "v6_phase_full", "v6_lofi", "v6_lofi_float", "v6_lofi_pls", "v6_lofi_home", "v6_lofi_full", "v6_voice_full", "v6_animal_full", "vx_tract_a_full", "vx_tract_m_full", "vx_tract_a_edit", "vx_tract_m_edit", "vx_tract_a_home", "vx_tract_m_home", "v6_string", "v6_string_bow", "v6_string_home", "v6_string_full", "v6_add", "v6_add_bell", "v6_add_home", "v6_add_full", "home_mutate", "home_faces",
    "live_rieles", "live_rieles_t3", "live_mano", "live_mano_keys",
    "mixer_tracks4", "home_tracks4", "mock_home", "mock_presets", "mock_seq", "mock_drum", "mock_mixer", "mock_dialog", "mock_menu",
    "toast_env", "toast_mutate", "toast_mutate_hint", "toast_motion", "toast_motion_locks", "toast_punch", "toast_auto_list",
    "toast_auto_list_drum", "toast_pattern", "toast_fm6_op", "toast_analog"};

/* (p2, main 0.9.11) GLO > LIVE: six sequences of their own lengths (16 32 7 16 . 1/8 20 4 . 1/4, as the mock-up's),
 * modifiers on T2 (THIN 70 %), T3 (x2 RATCH x3), T4 (LOOP 8), T5 (2/3), played two seconds from PLAY so the playheads
 * and ghosts are where they would be; RIELES (KNOB 1 ALL or T3), MANO (black key 3 and C4 held, A4 PING latched on T5,
 * LATCH on) */
static void live_scene(int s)
{
    static const uint8_t NN[5] = {60, 67, 63, 72, 58};
    uint32_t k;
    trk[2].p[P_SLEN] = 7; trk[3].p[P_SDIV] = 1; trk[4].p[P_SLEN] = 20; trk[5].p[P_SLEN] = 4; trk[5].p[P_SDIV] = 0;
    for (k = 0; k < 20u; k++) {
        step_t *st = &trk[4].step[k];
        if (k % 3u) continue;
        st->time = ST_NOTE; st->n = 1; st->note[0] = NN[k % 5u]; st->vel = 100;
    }
    for (k = 0; k < 4u; k++) { trk[5].step[k].time = ST_NOTE; trk[5].step[k].n = 1; trk[5].step[k].note[0] = NN[k]; trk[5].step[k].vel = 90; }
    for (k = 0; k < 16u; k++) {                         /* T1 an acid line, T3 seven steps, T4 a beat of 1/8 */
        static const uint8_t A[16] = {36, 0, 48, 36, 0, 39, 0, 36, 43, 0, 36, 48, 0, 41, 39, 0};
        step_t *st = &trk[0].step[k], *d = &trk[3].step[k];
        if (A[k]) { st->time = ST_NOTE; st->n = 1; st->note[0] = A[k]; st->vel = 100; st->flags = k % 4u ? 0 : SF_ACCENT; }
        memset(d, 0, sizeof *d);
        d->time = ST_NOTE; d->vel = 100;
        d->hit = (uint8_t)((k % 4u == 0u ? 1u : 0u) | (k % 8u == 4u ? 2u : 0u) | (k % 2u ? 0u : 8u));
        if (k < 7u && k != 4u) { trk[2].step[k].time = ST_NOTE; trk[2].step[k].n = 1; trk[2].step[k].note[0] = NN[k % 5u] - 12u; trk[2].step[k].vel = 100; }
    }
    memset(&lv, 0, sizeof lv);
    lv.dn[1] = -3; lv.rate[2] = 2; lv.dn[2] = 2; lv.cut[3] = 5; lv.here = 1u << 3; lv.rate[4] = -1;
    if (s == S_LIVE_RIELES_T3) lv.tgt = 3;
    if (s == S_LIVE_MANO_KEYS) {
        lv.latch = 1; lv.lt[9] = 1u << 4; lv.tk[5] = 1u << 2;
        lv.kb = 1u << key_at(1, 2) | 1u << key_at(0, 5);
    }
    song.sel = 2;
    seq_start();
    live_publish();
    for (k = 0; k < 2800u; k++) { events_block(CTL); if (!(k % 64u)) live_publish(); }
    nav_put(SEC_GLO, s >= S_LIVE_MANO ? "MANO" : "RIELES");
    if (s == S_LIVE_RIELES_T3) { ui.hot_col = 0; ui.hot_t = 30; }
}

/* the scenes of the UI design screens: the state the UI-redesign
 * prototype drew them from (its setup(): two pattern tracks, the drum pattern on track 4, a synthetic scope),
 * plus the hot knob each mock shows; tests/ui_mockcmp.py compares these renders with the mock PNGs */
static void mock_state(int s)
{
    uint32_t i;
    ui_power_on();
    if (large_on) ui_prefs |= PREF_LARGE;
    usb.config = 0;
    song.batt_raw = 600;
    for (i = 0; i < SCOPE_N; i++) {                  /* a saw with a little second harmonic */
        double ph = fmod(i / 64.0, 1.0);
        scope_buf[i] = (int16_t)((ph * 2.0 - 1.0) * 9000.0 + sin(i * 0.4908) * 2500.0);
    }
    scope_w = 0;
    song.playing = 1;
    song.g[G_BPM] = 124;
    clk_pos = 0; clk_step = 8; cin_total = cin_left = 0;   /* (as state()) */
    for (i = 0; i < NTRK; i++) trk[i].seq_idx = 5;
    load_pat16(&trk[0], PATTERNS[0].note, PATTERNS[0].flags);
    load_pat16(&trk[1], PATTERNS[4].note, PATTERNS[4].flags);
    for (i = 0; i < 16u; i++) {
        step_t *st = &trk[3].step[i];
        memset(st, 0, sizeof *st);
        st->time = ST_NOTE; st->vel = 100;
        if (i % 4u == 0u) st->hit |= 1u;
        if (i == 4u || i == 12u) st->hit |= 2u, st->acc |= 2u;
        if (i == 10u) st->hit |= 4u;
        if (i % 2u == 0u) st->hit |= 8u;
        if (i == 14u) st->hit |= 16u;
        if (i == 7u || i == 15u) st->hit |= 32u;
        if (i == 3u) st->hit |= 64u;
        if (i == 0u) st->hit |= 128u, st->acc |= 128u;
    }
    trk[3].p[P_SLEN] = 32;
    switch (s) {
    case S_MOCK_HOME: ui.home = 1; ui.hot_col = 1; ui.hot_t = 30; break;
    case S_MOCK_PRESETS: go_page(GR_BROWSE); break;
    case S_MOCK_SEQ: song.sel = 0; song.rec = 1; go_page(GR_ROLL); ui.cursor = 6; ui.hot_col = 0; ui.hot_t = 30; break;
    case S_MOCK_DRUM: song.sel = 3; go_page(GR_ROLL); ui.cursor = 4; ui.lane = 1; trk[3].seq_idx = 9; ui.hot_col = 0; ui.hot_t = 30; break;
    case S_MOCK_MIXER:
        go_page(GR_TRK);
        trk[0].p[P_LEVEL] = 100; trk[1].p[P_LEVEL] = 84; trk[2].p[P_LEVEL] = 64; trk[3].p[P_LEVEL] = 110;
        trk[0].p[P_PAN] = -20; trk[1].p[P_PAN] = 24; trk[2].p[P_PAN] = 0; trk[3].p[P_PAN] = -4;
        trk[0].p[P_REV] = 30; trk[1].p[P_REV] = 80; trk[2].p[P_REV] = 10; trk[3].p[P_REV] = 0;
        trk[2].p[P_MUTE] = 1;
        song.rec = 2u;
        trk[0].peak = 11000; trk[1].peak = 3300; trk[3].peak = 20000;     /* (the mock's stand-in meters) */
        ui.hot_col = 1; ui.hot_t = 30;
        break;
    case S_MOCK_DIALOG: ui.confirm = CF_OVR_PROJ; ui.confirm_trk = 0; go_page(GR_PATS); break;
    case S_MOCK_MENU: ui.menu = 1; ui.menu_sel = 0; break;
    default: break;
    }
}

/* the STEP page's piano roll (ui_graph.c graph_roll) on track 1, stopped unless said: an empty pattern; ACID in
 * A minor; POLY chords (Am7 F C G, tied); a line with ties, slides and accents in C major; LEN 32 on its second
 * page, playing; high (A6..) and low (C-1..) notes; a page wider than the view (edge marks); ACID playing with a
 * key held (A3, its row lit) */
static void step_put(step_t *st, uint32_t time, uint32_t flags, uint32_t n, const uint8_t *notes)
{
    uint32_t j;
    memset(st, 0, sizeof *st);
    st->time = (uint8_t)time; st->flags = (uint8_t)flags; st->n = (uint8_t)n; st->vel = 100;
    for (j = 0; j < n; j++) st->note[j] = notes[j];
}
static void roll_scene(int s)
{
    static const uint8_t AM7[] = {57, 60, 64, 67}, FM7[] = {53, 57, 60, 64}, CMA[] = {60, 64, 67}, GMA[] = {55, 59, 62, 67};
    static const uint8_t LINE[16] = {48, 0, 55, 60, 0, 59, 0, 0, 62, 64, 0, 67, 0, 65, 64, 0};
    static const uint8_t LINE_T[16] = {0, 1, 0, 0, 1, 0, 2, 2, 0, 0, 1, 0, 1, 0, 0, 2};   /* 0 note, 1 tie, 2 rest */
    track_t *t;
    uint32_t i;
    song.sel = 0; t = TSEL;
    song.playing = 0;
    track_defaults_steps(t);
    t->p[P_SLEN] = 16;
    go_page(GR_ROLL);
    ui.cursor = 0;
    switch (s) {
    case S_ROLL_EMPTY: break;
    case S_ROLL_ACID:
    case S_ROLL_PLAYING:
        load_pat16(t, PATTERNS[0].note, PATTERNS[0].flags);
        t->p[P_ROOT] = 9; t->p[P_SCALE] = 2; ui.cursor = 6;
        if (s == S_ROLL_PLAYING) {
            song.playing = 1; t->seq_idx = 9;
            kb_trk[3] = 0; kb_on[3] = 1; kb_snd[3][0] = 57;           /* A3 held */
        }
        break;
    case S_ROLL_CHORDS:
        t->p[P_VOICE] = V_POLY; t->p[P_ROOT] = 9; t->p[P_SCALE] = 2;
        for (i = 0; i < 16u; i++) {
            const uint8_t *c = i / 4u == 0u ? AM7 : i / 4u == 1u ? FM7 : i / 4u == 2u ? CMA : GMA;
            uint32_t n = i / 4u == 2u ? 3u : 4u;
            if (i % 4u == 0u) step_put(&t->step[i], ST_NOTE, i == 8u ? SF_ACCENT : 0u, n, c);
            else if (i % 4u == 1u) step_put(&t->step[i], ST_TIE, 0, 0, c);
            else if (i % 4u == 2u) step_put(&t->step[i], ST_NOTE, 0, n, c);
            else step_put(&t->step[i], ST_REST, 0, 0, c);
        }
        ui.cursor = 4;
        break;
    case S_ROLL_TIES:
        t->p[P_ROOT] = 0; t->p[P_SCALE] = 1;
        for (i = 0; i < 16u; i++) {
            uint8_t n = LINE[i];
            uint32_t f = (i == 3u || i == 9u ? SF_SLIDE : 0u) | (i == 0u || i == 8u || i == 13u ? SF_ACCENT : 0u);
            if (LINE_T[i] == 0u) step_put(&t->step[i], ST_NOTE, f, 1, &n);
            else step_put(&t->step[i], LINE_T[i] == 1u ? ST_TIE : ST_REST, 0, 0, &n);
        }
        t->step[2].flags |= SF_SLIDE;                  /* (slides into a note, from before a tie) */
        ui.cursor = 9;
        break;
    case S_ROLL_LEN32:
        load_pat16(t, PATTERNS[0].note, PATTERNS[0].flags);
        t->p[P_SLEN] = 32;
        for (i = 16; i < 32u; i++) {
            uint8_t n = (uint8_t)(60 + (i * 5u) % 12u);
            if (i % 3u != 2u) step_put(&t->step[i], ST_NOTE, i % 8u == 0u ? SF_ACCENT : 0u, 1, &n);
        }
        ui.cursor = 20; song.playing = 1; t->seq_idx = 22;
        break;
    case S_ROLL_HIGH:
    case S_ROLL_LOW:
        for (i = 0; i < 16u; i += 2u) {
            uint8_t n = (uint8_t)(s == S_ROLL_HIGH ? 112 + (i * 7u) % 15u : (i * 7u) % 15u);
            step_put(&t->step[i], ST_NOTE, 0, 1, &n);
        }
        ui.cursor = 2;
        break;
    case S_ROLL_WIDE:
        t->p[P_VOICE] = V_POLY;
        for (i = 0; i < 16u; i += 2u) {
            uint8_t n[2] = {(uint8_t)(36 + i), (uint8_t)(72 + i)};
            step_put(&t->step[i], ST_NOTE, 0, 2, n);
        }
        ui.cursor = 4;
        break;
    default: break;
    }
    ui.bank = (uint8_t)(ui.cursor / 16u);
}

/* (0.9.2) a section's page by its name in the chain (ui_lens.c nav_put), as a scene: no page flash over the footer */
static void nav_scene(uint32_t sec, const char *nm)
{
    uint32_t i;
    if (!nav_put(sec, nm)) {                            /* (seq2: BAR, NOTE out of the chain, the PATTERN page "LENGTH"
                                                         * in it: a page that code puts, by its title) */
        for (i = 0; i < NPAGES; i++)
            if (str_eq(PAGES[i].title, nm)) {
                ui.home = 0; ui.page = (uint8_t)i; page_entered(); pf_on = 0;
                return;
            }
        fprintf(stderr, "nav_scene: no page \"%s\"\n", nm);
        exit(2);
    }
    pf_on = 0;
}
/* (0.9.4) the engines' own creatures (ui_engvis.c): a phoneme sung on VOICE's head (the articulation as sing.c holds it,
 * the track sounding), an ANIMAL calling, a DRUM-X kit struck (the real engines run a few blocks) */
static void vx_run(uint32_t blocks)
{
    int32_t o[2 * CTL];
    uint32_t b, i;
    for (b = 0; b < blocks; b++) {
        events_block(CTL);
        mix_block(o, CTL);
        for (i = 1; i < CTL; i += 2u)
            scope_buf[scope_w++ & (SCOPE_N - 1u)] = (int16_t)clamp(o[2u * i], -32768, 32767);
    }
}
static void vx_phon(uint32_t ph, int32_t peak)
{
    sing_t *g = &sing[song.sel];
    const phon_t *P = &PHON[ph];
    eng(ENGI_VOICE_UI);
    TSEL->p[P_VLANG] = LG_ES;
    memset(g, 0, sizeof *g);
    g->lang = LG_ES; g->live = 1; g->np = 1; g->nu = 0; g->cd = 1; g->i = 0; g->ph[0] = (uint8_t)ph; g->st = 1;
    g->f[0] = (int16_t)(P->f1 ? P->f1 : 500); g->f[1] = (int16_t)(P->f1 ? P->f2 : 1500); g->f[2] = (int16_t)(P->f1 ? P->f3 : 2500);
    g->gv = (int16_t)(P->av << 7); g->gf = (int16_t)(P->af << 7); g->ga = (int16_t)(P->cls == PC_ASP ? 14000 : 0);
    str_cpy(g->w, "voz", sizeof g->w);
    TSEL->peak = peak;
    vxs.init = 0;
    memset(vxs.fr, 0xFF, sizeof vxs.fr);   /* (its level read afresh at the first frame drawn) */
    go_title("EDIT 1");
}
/* (0.9.9) VOICE's tract sung a while (the mock-up's time running: the nod struck at the vowel's onset, the pulses up the
 * tube and the waves out on their way): ph0 for n0 frames of 33 ms, then ph for n, at peak; full: the whole panel (SCOPE
 * OFF), home: HOME's BEING */
static void vx_sing(uint32_t ph0, uint32_t n0, uint32_t ph, uint32_t n, int32_t peak, int full, int home)
{
    sing_t *g = &sing[song.sel];
    uint32_t i, ms = fm1_ms, fr = ui.frame;              /* (the clock and the frame put back after: the scenes after
                                                          * these see the time they always did) */
    vx_phon(ph0, peak);
    for (i = 0; i < n0 + n; i++) {
        const phon_t *P = &PHON[i < n0 ? ph0 : ph];
        g->ph[0] = (uint8_t)(i < n0 ? ph0 : ph);
        g->f[0] = (int16_t)(P->f1 ? P->f1 : 500); g->f[1] = (int16_t)(P->f1 ? P->f2 : 1500);
        g->gv = (int16_t)(P->av << 7); g->gf = (int16_t)(P->af << 7);
        TSEL->peak = peak;
        ui.frame++;
        fm1_ms += 33u;
        cv_begin(240, 140, T_SURF);
        vx_draw(TSEL, 4, 2, 232, 136, -1);
    }
    fm1_ms = vxs.ms[0] = ms;
    ui.frame = vxs.fr[0] = fr;
    TSEL->peak = peak;
    ui_scope = !full;
    if (home) { ui.home = 1; ui.hlv = 1; ui.hset[1] = 0; eco.en[song.sel] = 220; }
}
static void vx_animal_scene(const char *pre, int32_t size)
{
    uint32_t e = ENGI_ANIMAL, k;
    eng(e);
    for (k = 0; k < ENGINES[e]->npresets; k++)
        if (!strcmp(ENGINES[e]->presets[k].name, pre)) apply_preset_to(TSEL, k);
    if (size >= 0) TSEL->p[P_E3] = (int16_t)size;
    TSEL->engine = TSEL->eng_req;
    vxs.w[1] = 0;
    memset(vxs.fr, 0xFF, sizeof vxs.fr);
    song.playing = 0;
    song.master_q12 = 4096;
    for (k = 0; k < NTRK; k++) if (k != song.sel) trk[k].p[P_MUTE] = 1;
    vx_run(40);                                   /* (the engine switched: its fade done) */
    trk_note_on(TSEL, 72, 120);
    vx_run(size == -2 ? 40 : 120);
    TSEL->peak = 7000;
    go_title("EDIT 1");
}
static void vx_kit_scene(uint32_t lanes, uint32_t dm)
{
    static const uint8_t N[8] = {36, 38, 39, 42, 46, 45, 37, 49};
    uint32_t k;
    eng(ENGI_DRUM);
    TSEL->engine = TSEL->eng_req;
    song.playing = 0;
    song.master_q12 = 4096;
    for (k = 0; k < NTRK; k++) if (k != song.sel) trk[k].p[P_MUTE] = 1;
    vx_run(40);
    {                                             /* (the kit silent: no hit of an earlier screen ringing on) */
        drum_lane_t *K = drum_kit_of(TSEL);
        for (k = 0; K && k < 8u; k++) { K[k].x.live = 0; K[k].age = 255; K[k].fl = 0; }
        vxs.last = 0;
    }
    vx_kit(TSEL, 0, 0, 256, -1, -1);              /* (the lanes' ages seen once: the hits below are new) */
    for (k = 0; k < 8u; k++)
        if (lanes >> k & 1u) { dx_in_tch = (uint8_t)dm; trk_note_on(TSEL, N[k], k == 1u ? 127 : 100); }
    dx_in_tch = 0;
    vx_run(6);
    memset(vxs.fr, 0xFF, sizeof vxs.fr);
}
/* (0.9.4) EDIT > KIT's beings: lane l struck n times (dm: its TOUCH, 2 GHOST 3 FLAM), each hit seen by a frame drawn
 * before the next (a ratchet's squashes); n 0: the kit silent */
static void vx_bug_scene(uint32_t l, uint32_t dm, uint32_t n)
{
    static const uint8_t N[8] = {36, 38, 39, 42, 46, 45, 37, 56};
    uint32_t k;
    vx_kit_scene(0, 0);
    for (k = 0; k < n; k++) {
        if (k) { vx_kit(TSEL, 0, 0, 256, -1, -1); fm1_ms += 40u; }
        dx_in_tch = (uint8_t)dm;
        trk_note_on(TSEL, N[l & 7u], 110);
        dx_in_tch = 0;
        vx_run(dm == 3u ? 2 : 6);                       /* (a FLAM: its grace hit, the hit to come) */
    }
    go_title("EDIT 1");
}
/* (0.9.6) the six creatures of the mock-up's family (ui_engvis.c): track 1 as engine e (preset pre), the knobs k[0..7]
 * (-99: the preset's), a note (none: silent) played and rung a while, then on page (0: HOME's BEING); age: ms since the
 * note when drawn (its squash, its blink), mt: the creature's time (its phases, NOISE's swarm moving) */
enum { ENGI_PHASE_UI = 2, ENGI_LOFI_UI = 3, ENGI_WHEEL_UI = 7, ENGI_NOISE_UI = 11 };   /* (engines.c ENGINES[]) */
static void v6_scene(uint32_t e, const char *pre, const char *page, const int16_t *k, uint32_t note, uint32_t age, uint32_t c)
{
    uint32_t i;
    eng(e);
    for (i = 0; i < ENGINES[e]->npresets; i++)
        if (!strcmp(ENGINES[e]->presets[i].name, pre)) apply_preset_to(TSEL, i);
    for (i = 0; k && i < 8u; i++)
        if (k[i] != -99) TSEL->p[P_E0 + i] = k[i];
    TSEL->engine = TSEL->eng_req;
    song.playing = 0;
    song.master_q12 = 4096;
    for (i = 0; i < NTRK; i++) if (i != song.sel) trk[i].p[P_MUTE] = 1;
    memset(vxs.fr, 0xFF, sizeof vxs.fr);
    vx_run(40);
    if (note) {
        trk_note_on(TSEL, note, 110);
        vx_run(60);
        TSEL->peak = 7000;
    } else
        memset(scope_buf, 0, sizeof scope_buf);
    vx_age(TSEL, c, 0);                                  /* (the note seen, age ms ago) */
    vxs.nms[c] = fm1_ms - age;
    vxs.mt[c] = 30u * 256u * 3u + c * 977u;
    vxs.swt = vxs.mt[c] >> 8;
    if (page) go_title(page); else { ui.home = 1; ui.hlv = 1; ui.hset[1] = 0; }
}
static void setup(int s)
{
    memset(kb_on, 0, sizeof kb_on);               /* no key held (roll_playing holds one) */
    live.req = live.cur = -1; live.mode = live.srec = live.dirty = 0; lys.nqc = 0;   /* (the song layer: off) */
    trk_live = NTRK;                              /* (the six awake but on *_tracks4) */
    if (s >= S_TOAST_ENV) {                       /* (0.9.5) a toast over a page with words to its foot: the pill keeps clear
                                                   * of them (or is not drawn: the header says it), never over one (hk_overlay) */
        static const uint16_t BASE[] = {S_ENV, S_MUTATE, S_MUTATE, S_MOTION, S_MOTION_LOCKS, S_COUNT, S_AUTO_LIST,
                                       S_AUTO_LIST_DRUM, S_PATTERN, S_V6_FM6_OP, S_V6_ANALOG};   /* (0.9.6: over the creatures too) */
        int b = BASE[s - S_TOAST_ENV];
        if (b == S_COUNT) { setup(S_HOME); go_page(GR_PUNCH); }
        else setup(b);
        if (s == S_TOAST_MUTATE_HINT) {           /* (MUTATE's hint, dwelt on: ui_layer.c hint_tick) */
            ui_message("OCT+ KEEP  GLO+A4 FREEZE");
            str_cpy(ui.msg2, "SEQ+D5 REVERT  GLO+B4 ALL", sizeof ui.msg2);
        } else
            ui_message("SAVED A LONG MESSAGE");
        return;
    }
    if (s >= S_MOCK_HOME) {
        mock_state(s);
        return;
    }
    state();
    ui_scope = 1;                                   /* (0.9.7) the scenes as before: MENU > DISPLAY > SCOPE ON (the *_full
                                                     * ones: OFF, the default, the creature on the whole EDIT panel) */
    switch (s) {
    case S_HOME: {                                /* (0.9.9) the device's power-on parts (T1 DRUM-X, T2 VOICE, T3 ANIMAL,
                                                   * T4 LOFI, ...: ui.c power_sound_of) with state()'s steps: README's HOME */
        uint32_t k;
        load_depth++;
        for (k = 0; k < NTRK; k++) { power_sound_of(&trk[k], k); trk[k].engine = trk[k].eng_req; }
        load_depth--;
        song.octave = 2; song.rec = 1; usb.config = 1; break;
    }
    case S_HOME_IDLE: song.playing = 0; song.batt_raw = 570; ui.hot_col = 1; ui.hot_t = 30; break;
    case S_MESSAGE: ui_say("LOADED ", "07 A VERY LONG PATTERN NAME"); break;
    case S_MESSAGE_KEY: ui_message("[SAVE] HOLD TO UNDO"); break;            /* a message with a keycap */
    case S_MESSAGE_NOFILE: ui_message(MSG_NOFILE "NO FILE"); break;   /* a message led by the no-file icon */
    case S_PRESETS: favorite_set(0, 4, 1); favorite_set(0, 5, 1); go_page(GR_BROWSE); break;
    case S_PRESETS_NOFAV: favorites.filter = 1; go_page(GR_BROWSE); break;
    case S_USER: song.playing = 0; up_store(3, "MY LONG BASS NAME"); up_store(4, "PAD"); ui.uslot = 3; go_page(GR_USER); break;
    case S_PHRASES: go_page(GR_PATS); break;
    case S_PROJECT: song.playing = 0; project_save(1); song.g[G_SLOT] = 2; go_page(GR_SLOTS); ui.act = 4; break;
    case S_TOOLS: go_page(GR_TOOLS); ui.act = 1; break;
    case S_SONG_EMPTY: song.playing = 0; go_page(GR_SONG); break;
    case S_SONG:
        song.playing = 0; project_save(0); project_save(1);
        chain_config.count = 3;
        chain_config.row[0] = (chain_row_t){0, 2}; chain_config.row[1] = (chain_row_t){1, 4}; chain_config.row[2] = (chain_row_t){2, 1};
        ui.song_row = 1; go_page(GR_SONG); chain_prepare(); events_block(32);
        break;
    case S_STEP: song.rec = 1; go_page(GR_ROLL); ui.cursor = 6; break;
    case S_PATTERN: go_title("PATTERN"); ui.cursor = 3; break;
    case S_FUGUE:                                       /* (0.6.52) heads 2 x2 +2, 3 1/2 REV -3, playing */
        for (uint32_t j = 0; j < 8u; j++) { uint8_t nn = (uint8_t)(60 + (j * 5u) % 12u); step_put(&TSEL->step[j], ST_NOTE, 0, 1, &nn); }
        TSEL->p[P_SLEN] = 8;
        fg_set[song.sel][0] = (fg_set_t){7, 0, 2}; fg_set[song.sel][1] = (fg_set_t){3, 1, -3};
        fg_head[song.sel][0].idx = 5; fg_head[song.sel][1].idx = 2; song.playing = 1; fg_ui = 1;
        go_page(GR_FUGUE);
        break;
    case S_PFLASH:                                      /* (0.6.51) a page changed by hand: its name big, held */
        go_title("LFO"); page_flash(); pf_ms = fm1_ms - 200u;
        break;
    case S_PFLASH_HOME:                                 /* (0.6.61) a track picked on HOME: its engine, "T1 HOME" */
        ui.home = 1; page_flash(); pf_ms = fm1_ms - 200u;
        break;
    case S_DXIMG:                                       /* (0.6.48) DRUM IMAGE 40 */
        set_engine_of(TSEL, ENGI_DRUM); TSEL->p[P_DXIMG] = 40; go_title("STEREO");
        break;
    case S_DXSEND:                                      /* (0.6.48) SEND: the snare to the REVERB, 70 % */
        set_engine_of(TSEL, ENGI_DRUM); dx_ui_lane = 1; dx_snd[1] = 3u << 6 | 47u; dx_lay[1] = 0x26; go_title("SOUND 4");
        break;
    case S_MORPH:                                       /* (0.6.45) ANALOG SAW, MORPH 50 */
        set_engine_of(TSEL, ENGI_ANALOG); TSEL->p[P_E0] = 0; TSEL->p[P_MORPH] = 50; go_page(GR_MORPH);
        break;
    case S_MUTATE:                                      /* (0.6.43) three tracks mutating, bar 5 */
        trk[0].p[P_MSND] = 2; trk[0].p[P_MPAT] = 4; trk[1].p[P_MSND] = 3; trk[1].p[P_MMODE] = 1; trk[3].p[P_MPAT] = 1;
        bar_n = 5; go_page(GR_MUTATE);
        break;
    case S_CHANCE: go_page(GR_CHANCE); step_set_chance(&TSEL->step[0], 65); break;
    case S_COMP:                                        /* (0.6.39) an ARP NOTES step: UP-DN x6 over two octaves */
    case S_COMP_TRIG:                                   /* .. a TRIG 3:4 */
    case S_COMP_ECHO:                                   /* (0.6.42) .. an ECHO x4, a JUMP, a PULSE */
        TSEL->step[0] = (step_t){{60, 63, 67}, 3, ST_NOTE, 0, 100};
        TSEL->step[0].comp = s == S_COMP ? CP_ARPN : s == S_COMP_ECHO ? CP_ECHO : CP_TRIG;
        TSEL->step[0].carg = s == S_COMP ? CP_ARG(2u, 3u, 1u) : s == S_COMP_ECHO ? 2u : 7u;
        if (s == S_COMP_ECHO) { TSEL->step[10].comp = CP_JUMP; TSEL->step[10].carg = 2; TSEL->step[12].comp = CP_PULSE; }
        TSEL->step[4].comp = CP_TRIG; step_set_chance(&TSEL->step[6], 50); step_set_ratchet(&TSEL->step[8], 3);
        go_page(GR_COMP);
        break;
    case S_MOTION: go_page(GR_MOTION); break;
    /* 1.1.5, the REC layer (REC held): over HOME, the settings as they come (CLICK OFF, COUNT-IN OFF, LEVEL MID);
     * over STEP armed, CLICK REC, COUNT-IN 2 BARS, LEVEL HIGH (KNOB 2 just turned); a song playing on an empty
     * track (CLEAR dimmed: STOP TO EDIT) */
    case S_REC_LAYER: go_home(); ui.layer = LAYER_REC; break;
    case S_MSEQ: {                                   /* a rising, falling line over LEN 12, step 5 picked, playing */
        static const uint8_t L[16] = {10, 40, 70, 100, 127, 90, 60, 30, 0, 50, 110, 20, 80, 80, 80, 80};
        uint32_t k;
        for (k = 0; k < 16u; k++) TSEL->p[P_MS0 + k] = L[k];
        TSEL->p[P_MSLEN] = 12; ms_ui_step = 4; ms_clock = div_samples(2) * 6u + 3u;
        TSEL->ms_v = 60 * 258;
        go_title("MSEQ");
        break;
    }
    case S_DIST_PAGE: TSEL->p[P_DIST] = 70; TSEL->p[P_DTYPE] = 2; TSEL->p[P_DTONE] = 20; go_title("DIST"); break;
    /* (JIANT 0.5) the technical graphs (ui_tech.c) */
    case S_DLY2: song.g[G_WIDTH] = 90; song.g[G_DPIT] = 5; song.g[G_DSPRY] = 60; go_title("DLY 2"); break;
    case S_REVERB2: song.g[G_RMOD] = 80; song.g[G_RWIDE] = 100; go_title("REVERB 2"); break;
    case S_CHORUS: song.g[G_CDEPTH] = 90; go_title("CHORUS"); break;
    case S_MASTER: song.g[G_CLIP] = 60; song.g[G_PUNCH] = 50; song.g[G_DUCK] = 40; go_title("MASTER"); break;
    case S_LFO2: TSEL->p[P_LPHASE] = 32; go_title("LFO 2"); break;   /* (0.9.18) PHASE, TRIG */
    case S_LFO2_FREE: TSEL->p[P_LPHASE] = -1; go_title("LFO 2"); break;   /* .. FREE: PHASE dimmed */
    case S_LFO_DEST: TSEL->p[P_LD_PIT] = 20; TSEL->p[P_LD_FLT] = -40; TSEL->p[P_LD_AMP] = 60; go_title("LFO DEST"); break;
    case S_MACRO_LAYER:                              /* M1: T1 CUT and T4's MRPH; M2: CLIP; M3 at 0, M4 none */
        go_home(); ui.layer = LAYER_MACRO;
        macro_v[0] = 90; macro_v[1] = 40; macro_v[3] = 127;
        trk[0].p[P_M1SRC] = MS_M1; trk[0].p[P_M1DST] = MD_CUT; trk[0].p[P_M1AMT] = 40;
        trk[3].p[P_M2SRC] = MS_M1; trk[3].p[P_M2DST] = MD_E1; trk[3].p[P_M2AMT] = -20;
        trk[1].p[P_M1SRC] = MS_M1 + 1; trk[1].p[P_M1DST] = MD_CLIP; trk[1].p[P_M1AMT] = 63;
        trk[2].p[P_M1SRC] = MS_M1 + 2; trk[2].p[P_M1DST] = MD_REV; trk[2].p[P_M1AMT] = 30;
        break;
    case S_REC_LAYER_SET: song.rec = 1; go_page(GR_ROLL); ui.cursor = 6; rp_put(RP_CLICK, 1u); rp_put(RP_COUNTIN, 2u);
        rp_put(RP_LEVEL, 2u); ui.layer = LAYER_REC; ui.hot_col = 1; ui.hot_t = 30; break;
    case S_REC_LAYER_SONG: drum(1); track_defaults_steps(TSEL); go_page(GR_SONG); chain.running = 1; ui.layer = LAYER_REC;
        break;
    /* 1.1 (Discussion #131): the count-in over the page: 1 BAR on its second beat (3 to go); 2 BARS, the first bar's
     * first beat (STEP under it); the MENU's AUDIO tab with CLICK REC selected, COUNT-IN 1 BAR */
    case S_COUNTIN: song.playing = 0; song.rec = 1; cin_total = 4; cin_left = 3; go_home(); break;
    case S_COUNTIN2: song.playing = 0; song.rec = 1; cin_total = 8; cin_left = 8; go_page(GR_ROLL); break;
    case S_MENU_AUDIO: ui.menu = 1; ui.menu_sel = MI_CLICK; rp_put(RP_CLICK, 1u); rp_put(RP_COUNTIN, 1u); break;
    /* 1.2: the header's three slots (ui_draw.c draw_head): playing with the selected track armed; another track armed;
     * the BPM being turned (the accent's); BPM LOCK ON (the lock right of the BPM), and its message after a turn;
     * GLO with the tempo turned and BPM LOCK ON (the label right of the lock); the metronome on the bar's 1st beat
     * (the accent's) and its 2nd (mirrored) */
    case S_HEAD_PLAY_REC: song.rec = 1u << song.sel; go_home(); break;
    case S_HEAD_REC_OTHER: song.rec = 1u << ((song.sel + 2u) % NTRK); go_home(); break;
    case S_HEAD_BPM_TURN: song.g[G_BPM] = 128; ui.roll_bpm = 128; ui.bpm_t = 40; go_home(); break;
    case S_HEAD_BPM_LOCK: ui_prefs |= PREF_BPM_LOCK; go_home(); break;
    case S_HEAD_BPM_LOCKED: ui_prefs |= PREF_BPM_LOCK; go_home(); ui_message("BPM LOCKED"); break;
    case S_HEAD_GLO_TURN: ui_prefs |= PREF_BPM_LOCK; go_home(); ui.layer = LAYER_GLO; ui.bpm_t = 40; break;
    case S_HEAD_DOWNBEAT: clk_step = 1; song.rec = 1u << song.sel; go_home(); break;
    case S_HEAD_BEAT_2: clk_step = 5; song.rec = 1u << song.sel; go_home(); break;
    case S_DRUM: drum(0); go_page(GR_ROLL); ui.cursor = 4; ui.lane = 1; trk[3].seq_idx = 9; break;
    case S_DRUM_HAND: drum(1); go_page(GR_ROLL); ui.lane = 5; ui.cursor = 7; break;
    case S_DRUM_CYM: drum(2); go_page(GR_ROLL); ui.lane = 7; break;
    case S_MIXER:
        go_page(GR_TRK);
        trk[0].p[P_LEVEL] = 100; trk[1].p[P_LEVEL] = 84; trk[2].p[P_LEVEL] = 64; trk[3].p[P_LEVEL] = 110;
        trk[0].p[P_PAN] = -20; trk[1].p[P_PAN] = 24; trk[3].p[P_PAN] = -4; trk[1].p[P_REV] = 80;
        trk[2].p[P_MUTE] = 1; trk[3].p[P_MUTE] = 1; song.rec = 2u | 8u; trk[0].peak = 9000;
        song.g[G_DJF] = -24;                     /* (0.9.1) KNOB 4: the DJ FILTER, a low-pass */
        break;
    case S_MIXER_T4:                             /* (0.6.22) TRACKS 4: T5, T6 asleep, grey, OFF */
        go_page(GR_TRK); trk_live = 4; break;
    case S_HOME_T4: trk_live = 4; break;
    case S_HOME_MUT: {                           /* (0.9.7) HOME A: MUTATE on, one landed on T2 (it looks up, its number white,
                                                  * its ring opening), T5 asleep */
        uint32_t k;
        for (k = 0; k < NTRK; k++) { trk[k].peak = (int32_t)(1200u + k * 1100u); trk[k].p[P_MSND] = 1; }
        trk[4].p[P_MUTE] = 1;
        mut_flash[1] = 7;
        break;
    }
    case S_HOME_FACES: {                         /* (0.9.7) HOME A: the faces: T1 hit just now (its eyes shut), the others
                                                  * quiet .. loud (their mouths, their colours), T6 muted (asleep) */
        uint32_t k;
        for (k = 0; k < NTRK; k++) { mut_flash[k] = 0; trk[k].peak = (int32_t)(k * 1700u); }
        trk[0].peak = 3500;
        eco.en[0] = 128;
        eco_fc.ms[0] = (uint16_t)fm1_ms;
        trk[5].p[P_MUTE] = 1;
        break;
    }
    case S_HOME_MACROS: case S_HOME_BEING: case S_HOME_ROUTES: {   /* (0.8.5) HOME's lens: MACROS, BEING, ROUTES */
        uint32_t k;
        ui.hlv = s != S_HOME_MACROS;
        ui.hset[s == S_HOME_MACROS ? 0 : 1] = s != S_HOME_BEING;
        for (k = 0; k < NTRK; k++) trk[k].peak = (int32_t)(1500u + k * 900u);
        trk[1].p[P_M1SRC] = MS_M1; trk[1].p[P_M1DST] = MD_CUT; trk[1].p[P_M1AMT] = 40;
        trk[3].p[P_M1SRC + 3] = MS_M1 + 2; trk[3].p[P_M1DST + 3] = MD_E1; trk[3].p[P_M1AMT + 3] = -20;
        macro_v[0] = 40; macro_v[1] = 90; macro_v[2] = 20; macro_v[3] = 64;
        if (s == S_HOME_BEING) { ui.hot_col = 2; ui.hot_t = 30; }
        break;
    }
    case S_ED_FILTER: go_title("FILTER"); TSEL->p[P_E5] = 90; break;   /* (0.8.5) EDIT's lens: TONE, FILTER (ANALOG) */
    case S_ED_FILTER_BP: eng(7); go_title("FILTER"); TSEL->p[P_FTYPE] = 2; TSEL->p[P_DIST] = 60; ui.hot_col = 1; ui.hot_t = 30; break;
    case S_ED_VOICE2: go_title("VOICE 2"); TSEL->p[P_VOICE] = 3; trk[0].v[0].stage = 2; trk[0].v[0].env = 9u << 20; break;
    case S_ED_FOCUS: case S_ED_FOCUS_MOD:              /* .. FOCUS: CUT, (MOD) M2 moving it */
        go_title("FOCUS"); ui.focus = 4;
        if (s == S_ED_FOCUS_MOD) { TSEL->p[P_M2SRC] = 1; TSEL->p[P_M2DST] = MD_E1 + 4; TSEL->p[P_M2AMT] = 40; }
        break;
    case S_ENV_TRACKS: case S_ENV_TRACKS_DEST:          /* (0.8.5) ENV's lens: TRACKS (ENV / DEST), T2 sounding */
        song.sel = 1; nav_scene(SEC_ENV, s == S_ENV_TRACKS ? "TRACKS" : "TRACKS DEST");
        trk[1].v[0].stage = 2; trk[1].v[0].env = 10u << 20; trk[2].p[P_ED_FLT] = 60; trk[2].p[P_ED_PIT] = -40;
        trk[1].p[P_ED_SHP] = 90; trk[3].p[P_MUTE] = 1;
        break;
    case S_ENV_STAGE: case S_ENV_STAGE_SUS:
        go_title("STAGE"); ui.stage = s == S_ENV_STAGE ? 1 : 2; ui.hot_col = 1; ui.hot_t = 30; break;
    case S_MSEQ2: go_title("MSEQ 2"); break;
    case S_DLG_MUT_BACK:                                /* (0.9.3) MUTATE turned OFF on T3 after mutating: the NOTICE */
        song.sel = 2; trk[2].p[P_MSND] = 2; trk[2].p[P_MPAT] = 4; mut_keep();
        trk[2].p[P_MSND] = trk[2].p[P_MPAT] = 0;
        go_page(GR_MUTATE);
        ui.confirm = CF_MUT_BACK; ui.confirm_trk = 2;
        break;
    case S_DLG_DICE_SOUND: case S_DLG_DICE_SEQ: case S_DLG_DICE_FX: {   /* (0.8.5) the DICE dialog: BEFORE / NOW */
        uint32_t k = s == S_DLG_DICE_SOUND ? CF_DICE_SOUND : s == S_DLG_DICE_SEQ ? CF_DICE_SEQ : CF_DICE_FX;
        go_title("EDIT 1"); song.playing = 0;
        dk_keep(k); dk_vis_take();
        if (k == CF_DICE_SOUND) { TSEL->p[ENGINES[TSEL->eng_req]->knob[0]] = 100; TSEL->p[ENGINES[TSEL->eng_req]->knob[2]] = 5; }
        else if (k == CF_DICE_SEQ) for (uint32_t i = 0; i < 16u; i += 3u) { TSEL->step[i].time = ST_NOTE; TSEL->step[i].n = 1; TSEL->step[i].note[0] = (uint8_t)(48 + i); }
        else { TSEL->p[P_DIST] = 90; TSEL->p[P_CHOR] = 60; }
        ui.confirm = k; ui.confirm_trk = song.sel;
        break;
    }
    case S_LY_FX_KNOBS: case S_LY_FX_STATE:            /* (0.8.5) a layer's lens: FX's KNOBS, STATE */
        go_home(); ui.layer = LAYER_FX; ui.lylv = s == S_LY_FX_KNOBS ? 1 : 2;
        perf_held = perf_act = PF_BIT(PF_R16) | PF_BIT(PF_LPF) | PF_BIT(PF_M1 + 1);
        perf_k[0] = -40; perf_k[1] = 25; perf_k[3] = 30;
        break;
    case S_LY_GLO_STATE: go_home(); ui.layer = LAYER_GLO; ui.lylv = 2; break;
    case S_MENU_ROW: case S_MENU_ROW_LEDS:             /* (0.8.5) the menu's lens, ROW: COLOR's palettes, LEDS's names */
        ui.menu = 1; ui.mlv = 2; ui.menu_sel = s == S_MENU_ROW ? MI_COLOR : MI_LEDS; break;
    case S_GLO_SENDS:                                  /* (0.8.5) GLO's lens: MIX > SENDS */
        trk[0].p[P_DIST] = 30; trk[1].p[P_CHOR] = 90; trk[2].p[P_DLY] = 60; trk[3].p[P_REV] = 127; trk[4].p[P_MUTE] = 1;
        go_title("SENDS"); ui.hot_col = 2; ui.hot_t = 30;
        break;
    case S_KEY_CHORD: TSEL->p[P_CHRD] = 2; TSEL->p[P_CVOI] = 1; TSEL->p[P_ROOT] = 9; TSEL->p[P_SCALE] = 2; go_title("CHORD"); break;
    case S_KEY_CHORD_OFF: go_title("CHORD"); break;
    case S_KEY_ARP: case S_KEY_ARP2:                   /* (0.8.5) SCL . ARP's lens: ARP on (UPDN, 2 octaves), ARP 2 */
        TSEL->p[P_AMODE] = 3; TSEL->p[P_AOCT] = 2; TSEL->p[P_ASWING] = 40; TSEL->p[P_APROB] = 100;
        go_title(s == S_KEY_ARP ? "ARP" : "ARP 2");
        break;           /* (0.8.5) LFO . MOD's lens: MSEQ 2 */
    case S_ED_OPERATOR: eng(ENGI_FM6); go_title("OPERATOR"); break;
    case S_GEN_EXPLORE:                                 /* (0.8.6) GEN 1/3 on EXPLORE, where it opens, after a piece */
        ui.confirm = CF_DICE_ALL; gp.page = 0; gp.hot = 0; gp.genre = GEN_EXPLORE; gp.seed = 421;
        str_cpy(gp.last, "EXPLORE 0421", sizeof gp.last);
        break;
    case S_GEN_LEAD: ui.confirm = CF_DICE_ALL; gp.page = 2; gp.hot = 1; gp.form = 2; break;   /* (0.8.5) GEN 3/3, SONG */
    case S_MIXER_PAN:                            /* PAN just turned, the ends of each knob, OFF, armed while stopped */
        go_page(GR_TRK); song.playing = 0; song.sel = 1;
        trk[0].p[P_LEVEL] = 127; trk[1].p[P_LEVEL] = 0; trk[2].p[P_LEVEL] = 1; trk[3].p[P_LEVEL] = 104;
        trk[0].p[P_PAN] = -64; trk[1].p[P_PAN] = 63; trk[2].p[P_PAN] = 0; trk[3].p[P_PAN] = 1;
        trk[0].p[P_REV] = 127; trk[2].p[P_REV] = 1; trk[3].p[P_MUTE] = 1;
        song.rec = 1u | 8u; ui.hot_col = 3; ui.hot_t = 30;
        song.g[G_DJF] = 63;                      /* (0.9.1) the DJ FILTER just turned: the high-pass's end */
        break;
    case S_ENV: go_title("ENV"); ui.hot_col = 2; ui.hot_t = 30; break;
    case S_ENVDEST: go_title("ENV DEST"); break;
    case S_LFO: go_title("LFO"); break;
    case S_MOD: TSEL->p[P_M1SRC] = 1; TSEL->p[P_M1DST] = 2; TSEL->p[P_M1AMT] = 40; TSEL->p[P_M2SRC] = 6;
        TSEL->p[P_M2DST] = 14; TSEL->p[P_M2AMT] = -64; mod_ui_slot = 1; go_title("MOD"); break;
    case S_FX: go_title("FX"); break;
    case S_SLICER: song.g[G_SLMODE] = 1; go_title("SLICER"); break;   /* (0.6.3: the bus) */
    case S_SLBUS: song.g[G_SLMODE] = 2; song.g[G_SLT1 + 2] = 0; song.g[G_SLFX] = 0; go_title("SL BUS 2"); break;
    case S_SLPITCH: {
        static const int8_t PS[16] = {0, 7, 12, -5, 0, 3, 24, -12, 5, 0, -24, 7, 12, 0, 0, 0};
        for (uint32_t k = 0; k < 16u; k++) song.g[G_SLP0 + k] = PS[k];
        song.g[G_SLMODE] = 2; song.g[G_SLPLEN] = 12; sl_ui_step = 2; go_title("SL PITCH");
        break;
    }
    case S_DLY: go_title("DLY"); break;
    case S_SCL: TSEL->p[P_SCALE] = 2; go_title("SCL"); break;
    case S_SHIFT: TSEL->p[P_SOFS] = -3; TSEL->p[P_POFS] = 7; TSEL->p[P_HVEL] = 80; go_title("TRACK"); break;   /* (0.6.92: TRACK) */
    case S_ARP: go_title("ARP"); break;
    case S_VOICE: go_title("VOICE"); break;
    case S_GLOBAL: go_title("GLOBAL"); break;
    case S_SYSTEM: go_title("SYSTEM"); break;
    case S_EDIT_ANALOG: go_title("EDIT 1"); break;
    case S_EDIT_DIGITAL: eng(E_FM); go_title("EDIT 1"); break;
    case S_OP_ENV: eng(1); go_title("OP1 ENV"); break;
    case S_EDIT_WHEEL: eng(7); go_title("EDIT 1"); ui.hot_col = 1; ui.hot_t = 30; break;
    case S_EDIT_SAMPLE: eng(5); go_title("EDIT 1"); break;   /* (JIANT: SAMPLE retired; VOICE in its place) */
    case S_EDIT_GRAIN: eng(11); go_title("EDIT 2"); break;   /* (GRAIN retired: NOISE's second page) */
    case S_ALG1: case S_ALG2: case S_ALG3: case S_ALG4: case S_ALG5: case S_ALG6: case S_ALG7: case S_ALG8:
        eng(1); TSEL->p[P_E0] = (int16_t)(s - S_ALG1);      /* the 8 DIGITAL charts; FB on the odd ones, IDX high .. 0 */
        TSEL->p[P_E6] = (s - S_ALG1) & 1 ? 40 : 0; TSEL->p[P_E4] = (int16_t)((S_ALG8 - s) * 18);
        go_title("EDIT 2"); break;
    case S_OP_LEVEL: eng(1); TSEL->p[P_E0] = 1; TSEL->p[P_FM4_LEVEL] = 0; go_title("OP LEVEL");   /* op 4 silent, op 3 hot */
        ui.hot_col = 2; ui.hot_t = 30; break;
    /* FM6's algorithm charts: 1 as it is; 5 with FB +3 just turned; 22 on EDIT 2, DTUN just turned (the carriers);
     * 32 with operator 6 at output level 0, MLVL just turned (no routes: nothing in ACCENT but nothing either) */
    case S_FM6_ALG1: eng(ENGI_FM6); TSEL->p[P_E0] = 1; go_title("EDIT 1"); break;
    /* DRUM-X: the specimen at rest; then a kick and a hat just struck (their petals swell and light up) */
    case S_DRUMX: eng(ENGI_DRUM); TSEL->p[P_E0] = 40; go_title("EDIT 1"); break;
    case S_DRUMX_HIT:
        eng(ENGI_DRUM); TSEL->p[P_E0] = 100; go_title("EDIT 1");
        trk_note_on(TSEL, 36, 120); trk_note_on(TSEL, 42, 100); events_block(256);
        break;
    /* EDIT > SOUND / SOUND 2: the snare's patch B (its organ singled out, the anatomy at B), PTCH just turned */
    case S_DX_SOUND:
        eng(ENGI_DRUM); dx_ui_lane = 1; dx_ui_side = 1; dx_ui_seen = last_note;
        go_title("SOUND"); ui.hot_col = 2; ui.hot_t = 30;
        break;
    case S_DX_SOUND2:
        eng(ENGI_DRUM); dx_ui_lane = 0; dx_ui_side = 0; dx_ui_seen = last_note;
        go_title("SOUND 2");
        break;
    case S_FM6_ALG5: eng(ENGI_FM6); TSEL->p[P_E0] = 5; TSEL->p[P_E1] = 3; go_title("EDIT 1"); ui.hot_col = 1; ui.hot_t = 30; break;
    case S_FM6_ALG22: eng(ENGI_FM6); TSEL->p[P_E0] = 22; TSEL->p[P_E6] = 40; go_title("EDIT 2"); ui.hot_col = 2; ui.hot_t = 30; break;
    case S_FM6_ALG32:
        eng(ENGI_FM6); TSEL->p[P_E0] = 32; fm6_patch[song.sel][FP_OL] = 0; fm6_pgen[song.sel]++;   /* (op 6: the patch's first) */
        go_title("EDIT 1"); ui.hot_col = 2; ui.hot_t = 30;
        break;
    case S_CONFIRM_SEQ: ui.confirm = CF_CLEAR_SEQ; ui.confirm_trk = 2; break;
    case S_CONFIRM_PROJ: ui.confirm = CF_OVR_PROJ; ui.confirm_trk = 0; break;
    case S_CONFIRM_USER: song.playing = 0; up_store(6, "A VERY LONG SOUND NAME"); ui.confirm = CF_OVR_USER; ui.confirm_trk = 6; break;
    case S_CONFIRM_PAT: ui.confirm = CF_LOAD_PAT; ui.confirm_trk = 0; ui.ppick = 4; break;
    case S_CONFIRM_MOTION: ui.confirm = CF_CLEAR_MOTION; ui.confirm_trk = 3; break;
    case S_CONFIRM_ERASE: song.playing = 0; up_store(6, "A VERY LONG SOUND NAME"); ui.confirm = CF_ERASE_USER; ui.confirm_trk = 6; break;
    case S_GEN: ui.confirm = CF_DICE_ALL; gp.page = 0; gp.hot = 0; gp.genre = 1; gp.seed = 421; str_cpy(gp.last, "HOUSE 0421", sizeof gp.last); break;   /* (0.6.91) GEN */
    case S_GEN_PARTS: ui.confirm = CF_DICE_ALL; gp.page = 1; gp.hot = 2; gp.amt[GP_CHORDS] = 70; gp.amt[GP_ARP] = 0; break;
    case S_MENU: ui.menu = 1; ui.menu_sel = 0; song.rec = 1; break;
    case S_MENU_SPEAKER: ui.menu = 1; ui.menu_sel = MI_LOWCUT; settings.lowcut = 2; break;
    case S_ABOUT: ui.menu = 2; ui.menu_scroll = 0; break;
    case S_ABOUT_REC: ui.menu = 2; ui.menu_scroll = 0; song.rec = 1; break;          /* the REC mark beside OCT- BACK */
    case S_ABOUT_CREDITS: ui.menu = 2; ui.menu_scroll = 360; break;
    case S_ABOUT_END: ui.menu = 2; ui.menu_scroll = (uint16_t)menu_scroll_max(); break;
    case S_UBOOT: ui.uboot = 3; break;
    /* the header's battery at each stock level (raw ADC: under 531, 531.., 561.., 591..) and on USB power */
    case S_BATT0: usb.config = 0; song.batt_raw = 500; go_home(); break;
    case S_BATT1: usb.config = 0; song.batt_raw = 540; go_home(); break;
    case S_BATT2: usb.config = 0; song.batt_raw = 570; go_home(); break;
    case S_BATT3: usb.config = 0; song.batt_raw = 600; go_home(); break;
    case S_BATT_USB: usb.config = 1; usb.suspended = 0; song.batt_raw = 500; go_home(); break;
    case S_MOTION_REC: song.rec = 1u << song.sel; go_page(GR_MOTION); break;    /* recording into the motion */
    case S_MOTION_OFF: song.playing = 0; go_page(GR_MOTION); ui.act = 4; break;  /* stopped, CLEAR picked */
    case S_MOTION_CARD: {                                          /* #63: HOME, MOTION drives KNOB 1 and 3 (3 just turned) */
        const engine_t *en = ENGINES[TSEL->eng_req];
        motion_set_event(TSEL, 2, en->knob[0], TSEL->p[en->knob[0]]);
        motion_set_event(TSEL, 6, en->knob[2], TSEL->p[en->knob[2]]);
        go_home(); ui.hot_col = 2; ui.hot_t = 30;
        break;
    }
    case S_SONG_HOME:                            /* the song playing: the disc and its row in the header */
        song.playing = 0; project_save(0); project_save(1);
        chain_config.count = 2;
        chain_config.row[0] = (chain_row_t){0, 2}; chain_config.row[1] = (chain_row_t){1, 4};
        go_page(GR_SONG); chain_prepare(); events_block(32); go_home(); ui.msg_t = 0;
        break;
    /* the FX layer's map: held alone; REPEAT 1/16 + LPF + a mute playing with the macros turned;
     * at 72 BPM: a REPEAT 1/16 waiting for its 1/16 (shown THEME) beside LPF playing, and REPEAT 1/8
     * held, too long at that tempo (dimmed) */
    case S_FX_PEEK: go_title("ENV"); ui.layer = LAYER_FX; break;
    case S_FX_HELD:
        go_home(); ui.layer = LAYER_FX;
        perf_held = perf_act = PF_BIT(PF_R16) | PF_BIT(PF_LPF) | PF_BIT(PF_M1 + 1);
        perf_k[0] = -40; perf_k[1] = 25; perf_k[3] = 30; ui.hot_col = 0; ui.hot_t = 30;
        break;
    case S_FX_WAIT:
        song.playing = 1; song.g[G_BPM] = 72; ui.layer = LAYER_FX;
        perf_held = PF_BIT(PF_R16) | PF_BIT(PF_R8) | PF_BIT(PF_LPF); perf_act = PF_BIT(PF_LPF);
        perf_k[2] = 100;
        break;
    case S_FX_HARM:                                 /* (TONIC: no harmonizer) HPF playing, KNOB 4 DEPTH turned */
        song.playing = 1; ui.layer = LAYER_FX;
        perf_held = perf_act = PF_BIT(PF_HPF);
        perf_k[3] = 60; ui.hot_col = 3; ui.hot_t = 30;
        break;
    case S_REVERB: go_title("REVERB"); song.g[G_RTYPE] = 1; ui.hot_col = 0; ui.hot_t = 30; break;   /* TYPE: SPRING */
    case S_REVERB_RESO: go_title("REVERB"); song.g[G_RTYPE] = 3; song.g[G_RDAMP] = 2; song.g[G_RPRE] = 4; break;   /* (0.6.5) PITCH, FOLW */
    case S_MENU_HOLD: ui.menu = 1; ui.menu_sel = MI_HOLD; settings_hold = 2; break;
    case S_MENU_LEDS: ui.menu = 1; ui.menu_sel = MI_LEDS; settings_leds = LEDS_INV; break;
    case S_MENU_END: ui.menu = 1; ui.menu_sel = MI_COUNT - 1u; ui_prefs = 0xFF; ui_rec_prefs = 0x2A; break;   /* the last row,
                                                     * every flag set (CLICK ON, LEVEL HIGH, COUNT-IN 2 BARS) */
    /* the MENU's tabs (1.0.5): menu DISPLAY (COLOR), menu_hold CONTROL, menu_speaker AUDIO, menu_system SYSTEM (USB
     * SERIAL); menu_slide: from DISPLAY to CONTROL with ANIM ON, the frame half way (draw) */
    case S_MENU_SYSTEM: ui.menu = 1; ui.menu_sel = MI_SERIAL; break;
    case S_MENU_SLIDE: ui.menu = 1; ui.menu_sel = MI_ACCEL; ui_prefs |= PREF_LATCH; break;
    /* the GLO SCL EDIT layers (ui_layer.c): just opened (a peek), and in use: GLO with T2 muted, T3 soloed (its key
     * held) and KNOB 1 turned; with CLK EXT (TAP dimmed); SCL at D# minor, KNOB 2 turned; EDIT on DIGITAL preset 3,
     * a favourite; a user preset; the hint after a tap */
    case S_GLO_PEEK: go_title("ENV"); ui.layer = LAYER_GLO; break;
    case S_GLO_ACTIVE:
        go_home(); ui.layer = LAYER_GLO; trk[1].p[P_MUTE] = 1; perf_solo = 4; trk[0].p[P_LEVEL] = 90;
        ui.hot_col = 0; ui.hot_t = 30;
        break;
    case S_GLO_EXT: go_home(); ui.layer = LAYER_GLO; song.g[G_CLOCK] = 1; trk[0].p[P_MUTE] = trk[3].p[P_MUTE] = 1; break;
    case S_SCL_PEEK: go_title("ENV"); ui.layer = LAYER_SCL; break;
    case S_SCL_ACTIVE: go_home(); ui.layer = LAYER_SCL; TSEL->p[P_ROOT] = 3; TSEL->p[P_SCALE] = 2; ui.hot_col = 1; ui.hot_t = 30; break;
    case S_EDIT_PEEK: go_title("EDIT 1"); ui.layer = LAYER_EDIT; break;
    case S_EDIT_ACTIVE: eng(E_FM); apply_preset_to(TSEL, 2); favorite_set(E_FM, 2, 1); go_home(); ui.layer = LAYER_EDIT;
        ui.hot_col = 0; ui.hot_t = 30; break;
    case S_EDIT_DRUM: eng(ENGI_DRUM); go_home(); dx_mute_set(DXM_LANE(2) | DXM_LANE(6)); ui.layer = LAYER_EDIT; break;
    case S_EDIT_USER: song.playing = 0; eng(7); up_store(6, "MY LONG WHEEL NAME"); up_load(6); favorite_set(NENGINES, 6, 1);
        go_home(); ui.layer = LAYER_EDIT; break;
    case S_LAYER_HINT: go_page(GR_TRK); ui.msg_t = 0; layer_tap(LAYER_GLO); break;
    /* #83: locked open by a double tap (the lock after the header's name): EDIT and FX */
    case S_LAYER_LOCK: go_title("ENV"); ui.layer = LAYER_EDIT; break;   /* (TONIC: no lock; the map as held) */
    case S_LAYER_LOCK_FX: go_title("ENV"); ui.layer = LAYER_FX; break;
    /* JIANT: the song layer, section B playing, SONG mode, SONG REC on; then a quick chain being tapped */
    case S_LAYER_SONG: go_home(); ui.layer = LAYER_SAVE; live.cur = 1; live.mode = 1; live.srec = 2; live.dirty = 4; break;
    case S_LAYER_SONG_CHAIN:
        go_home(); ui.layer = LAYER_SAVE; live.cur = 0; live.req = 2;
        lys.nqc = 4; lys.qc[0] = 0; lys.qc[1] = 1; lys.qc[2] = 1; lys.qc[3] = 2;
        break;
    /* 1.2 SEQ TOOLS (SEQ held on STEP): a melodic track (the sequence's tools), the DRUM grid (+ BEAT, the lane's tools,
     * the lanes; CLAP selected), a song playing (every tool dimmed: STOP TO EDIT) */
    case S_SEQ_TOOLS: go_page(GR_ROLL); ui.layer = LAYER_SEQ; break;
    case S_SEQ_TOOLS_DRUM: drum(0); go_page(GR_ROLL); ui.lane = 2; ui.layer = LAYER_SEQ; break;
    case S_SEQ_TOOLS_SONG: drum(1); go_page(GR_ROLL); ui.lane = 5; chain.running = 1; ui.layer = LAYER_SEQ; break;
    /* (0.8.3) LENTE, SEQ's three levels: PATTERN (a LEN 64 line over four bars, the third edited, playing in the
     * second; a kit's 32; STRETCH), BAR (the roll over LEN 64; the grid), STEP (a chord, its second note in focus,
     * MICRO +3, an accent, an ARP, 60 %; COMP's and COMP 2's chips lit; a TIE holding it; a REST; a kit's hits) */
    case S_LENTE_PAT:
    case S_LENTE_STRETCH:
    case S_LENTE_BAR:
    case S_LENTE_STEP:
    case S_LENTE_STEP_COMP:
    case S_LENTE_STEP_COMP2:
    case S_LENTE_STEP_TIE:
    case S_LENTE_STEP_REST:
    case S_LENTE_BAR_LONG:                              /* (0.8.4) a chord 16 steps long: TIME 16 ST, the big step's LEN 16 */
    case S_LENTE_STEP_LONG:
    case S_SEQ_BAR: {                                   /* (0.9.2) SEQ > BAR: KNOB 1 the bar (3/4 here), KNOB 2 LEN */
        static const uint8_t CH[3] = {60, 64, 69};
        track_t *t = TSEL;
        uint32_t i;
        track_defaults_steps(t);
        t->p[P_SLEN] = 64; t->p[P_VOICE] = V_POLY;
        for (i = 0; i < 64u; i++) {
            uint8_t n = (uint8_t)(48 + (i * 7u + i / 16u * 5u) % 24u);
            if (i % 5u == 4u) step_put(&t->step[i], ST_TIE, 0, 0, &n);
            else if (i % 7u == 6u) step_put(&t->step[i], ST_REST, 0, 0, &n);
            else step_put(&t->step[i], ST_NOTE, i % 8u == 0u ? SF_ACCENT : 0u, 1, &n);
        }
        step_put(&t->step[34], ST_NOTE, SF_ACCENT | SF_SLIDE, 3, CH);
        ST_MICRO_SET(&t->step[34], 13); step_set_chance(&t->step[34], 60);   /* (0.9.1: +13/64) */
        t->step[34].comp = CP_ARPN; t->step[34].comp2 = CP_ECHO;
        step_put(&t->step[35], ST_TIE, 0, 0, CH);
        step_put(&t->step[36], ST_REST, 0, 0, CH);
        if (s == S_LENTE_BAR_LONG || s == S_LENTE_STEP_LONG) {
            step_put(&t->step[40], ST_NOTE, 0, 3, CH);
            for (i = 41; i < 56u; i++) step_put(&t->step[i], ST_TIE, 0, 0, CH);
        }
        if (s == S_LENTE_BAR_LONG)                      /* (0.8.5) ALL TR at the right of the bars' strip */
            song.g[G_STRN] = -12;
        song.playing = 1; t->seq_idx = 21;
        nav_scene(SEC_SEQ, s == S_LENTE_PAT ? "LENGTH" : s == S_LENTE_STRETCH ? "STRETCH" : s == S_LENTE_BAR || s == S_LENTE_BAR_LONG ?
                  "STEP" : s == S_LENTE_STEP_COMP2 ? "COMP 2" : s == S_LENTE_STEP ? "NOTE" : s == S_SEQ_BAR ? "BAR" : "COMP");   /* (0.9.2: NOTE the roll; the
                                                         * step large on COMP: TIE, REST, LONG there) */
        cursor_set(s == S_LENTE_STEP_TIE ? 35 : s == S_LENTE_STEP_REST ? 36 : s == S_LENTE_BAR_LONG || s == S_LENTE_STEP_LONG ? 40 : 34);
        ui.pr_focus = s == S_LENTE_STEP ? 2u : 0u;
        break;
    }
    case S_LENTE_PAT_DRUM:
    case S_LENTE_BAR_DRUM:
    case S_LENTE_STEP_DRUM:
    case S_SEQ_BAR_DRUM:
        drum(0); trk[3].p[P_SLEN] = 32;
        memcpy(&trk[3].step[16], &trk[3].step[0], 16u * sizeof(step_t));
        trk[3].step[20].hit |= 0x30u; trk[3].step[20].acc |= 0x10u;
        nav_scene(SEC_SEQ, s == S_LENTE_PAT_DRUM ? "LENGTH" : s == S_LENTE_BAR_DRUM ? "STEP" : s == S_SEQ_BAR_DRUM ? "BAR" : "COMP");
        cursor_set(20); ui.lane = 4;
        break;
    /* NAME (ui_name.c): USER SAVE prefilled; a letter cycling (RS: S, R next); 123 on a project; an empty project name
     * (the placeholder); 12 of the widest letters, the cursor past them; playing (OCT+ dim) */
    case S_NAME_USER: song.playing = 0; go_page(GR_USER); ui.uslot = 6; name_open(NK_USER_SAVE, 6); break;
    case S_NAME_TYPING:
        song.playing = 0; go_page(GR_USER); name_open(NK_USER_SAVE, 6);
        str_cpy(nm.s, "SUB BAS", sizeof nm.s); nm.len = 7; nm.cur = 6; nm.key = 9; nm.tap = 1; nm.t = fm1_ms;
        break;
    case S_NAME_123:
        song.playing = 0; go_page(GR_SLOTS); name_open(NK_PROJ_SAVE, 1);
        str_cpy(nm.s, "LIVE 2026", sizeof nm.s); nm.len = 9; nm.cur = 5; nm.num = 1;
        break;
    case S_NAME_EMPTY: song.playing = 0; go_page(GR_SLOTS); proj_name[0] = 0; name_open(NK_PROJ_SAVE, 2); break;
    case S_NAME_FULL:
        song.playing = 0; up_store(4, "PAD"); go_page(GR_USER); name_open(NK_USER_RENAME, 4);
        str_cpy(nm.s, "MWMWMWMWMWMW", sizeof nm.s); nm.len = nm.cur = 12;
        break;
    case S_NAME_PLAYING: go_page(GR_USER); name_open(NK_USER_SAVE, 6); ui_message("STOP TO SAVE"); break;
    case S_PROJECT_NAMED:
        song.playing = 0; project_save_as(0, "LOFI JAM"); project_save_as(1, "MWMWMWMWMWMW"); project_save_as(2, "");
        song.g[G_SLOT] = 2; go_page(GR_SLOTS); ui.act = 4; ui.msg_t = 0;
        break;
    case S_SONG_NAMED:
        song.playing = 0; project_save_as(0, "LOFI JAM"); project_save_as(1, "MWMWMWMWMWMW");
        chain_config.count = 3;
        chain_config.row[0] = (chain_row_t){0, 2}; chain_config.row[1] = (chain_row_t){1, 4}; chain_config.row[2] = (chain_row_t){2, 1};
        ui.song_row = 0; go_page(GR_SONG); ui.msg_t = 0;
        break;
    case S_ROLL_EMPTY: case S_ROLL_ACID: case S_ROLL_CHORDS: case S_ROLL_TIES: case S_ROLL_LEN32: case S_ROLL_HIGH:
    case S_ROLL_LOW: case S_ROLL_WIDE: case S_ROLL_PLAYING: roll_scene(s); break;
    /* 1.1 parameter locks (ui_input.c lock_*): the roll's steps with a lock marked under them; a step held (the note
     * key of its entry) shows what KNOB 1..4 lock (HOME's: ANALOG E5 E6 ATK REL), the locked ones bright; the DRUM grid
     * with its marks over the steps; a step key held after FX (DIST CHO DLY REV, REV and DLY locked); AUTOMATION's
     * LOCK count */
    case S_ROLL_LOCKS: case S_ROLL_LOCK_HELD:
        roll_scene(S_ROLL_ACID);
        motion_set_lock(TSEL, 2, P_REV, 100); motion_set_lock(TSEL, 6, P_E4, 20); motion_set_lock(TSEL, 6, P_ATK, 90);
        motion_set_lock(TSEL, 9, P_E5, 110); motion_set_lock(TSEL, 13, P_REV, 80);
        if (s == S_ROLL_LOCK_HELD) {
            ui.plk_src = 0;
            ui.entry_open = 1;
            fm1_in.notes = 1u << 10;
            ui.hot_col = 0; ui.hot_t = 30;
        }
        break;
    case S_DRUM_LOCKS: case S_DRUM_LOCK_HELD:
        drum(0); go_page(GR_ROLL); ui.cursor = 4; ui.lane = 1; trk[3].seq_idx = 9;
        motion_set_lock(TSEL, 2, P_E1, 90); motion_set_lock(TSEL, 4, P_REV, 100); motion_set_lock(TSEL, 4, P_DLY, 70);
        motion_set_lock(TSEL, 8, P_E2, 30); motion_set_lock(TSEL, 15, P_E1, 40);
        if (s == S_DRUM_LOCK_HELD) {
            uint32_t i;
            for (i = 0; i < NPAGES && PAGES[i].graph != GR_FX; i++)
                ;
            ui.plk_src = (uint8_t)(i + 1u);
            fm1_in.notes = 1u << key_at(0, 4);
            ui.hot_col = 3; ui.hot_t = 30;
        }
        break;
    case S_MOTION_LOCKS:
        motion_set_event(TSEL, 1, P_REV, 40); motion_set_lock(TSEL, 2, P_REV, 100); motion_set_lock(TSEL, 6, P_ATK, 90);
        go_page(GR_MOTION);
        break;
    /* 1.1.5 SEQ > AUTO LIST (ui_events.c): locks and automation events of the track, the 4th row selected, its
     * VALUE just turned; empty (+ ADD LOCK: step 5, the first knob of HOME); a DRUM track's long list, scrolled
     * (its lane levels, E names, the last row), playing */
    case S_AUTO_LIST:
        motion_set_event(TSEL, 0, P_LRATE, 70); motion_set_lock(TSEL, 2, P_REV, 100); motion_set_lock(TSEL, 2, P_ED_FLT, -30);
        motion_set_lock(TSEL, 6, P_ATK, 90); motion_set_lock(TSEL, 6, P_E4, 20); motion_set_event(TSEL, 8, P_PAN, -40);
        motion_set_lock(TSEL, 11, P_LD_FLT, 50); motion_set_lock(TSEL, 13, P_DLY, 80); motion_set_lock(TSEL, 15, P_GLIDE, 60);
        go_page(GR_EVENTS); ui.ev_row = 3; ui.hot_col = 3; ui.hot_t = 30;
        break;
    case S_AUTO_LIST_ADD: ui.cursor = 4; go_page(GR_EVENTS); break;
    case S_AUTO_LIST_DRUM: {
        uint32_t i;
        drum(0); go_page(GR_ROLL);
        for (i = 0; i < 12u; i++)
            motion_set_lock(TSEL, i, i & 1u ? P_LN0 + i % 8u : P_E0 + i % 8u, 100);
        motion_set_event(TSEL, 15, P_REV, 127);
        go_page(GR_EVENTS); ui.ev_row = 13; song.playing = 1;
        break;
    }
    case S_USER_FOOT: song.playing = 0; up_store(3, "MY BASS"); ui.uslot = 3; go_page(GR_USER); break;   /* EDIT NAME lit */
    /* (0.8.5) FX's lens as the mock-up's values: T1's sends DIST 0 CHORUS 0 DLY 0 REV 10 (sends: 30 60 40 50 to show
     * them), DIST SOFT, AUTO PAN 40 SIN; DLY 1/8 FDBK 70 MIX 50, REVERB ROOM SIZE 70, CHORUS DEPTH 64, SLICER GATE PAT 3
     * DEPTH 90, CLIP 30 PUNCH 40 DUCK 20 REL 60 */
    case S_FXL_SENDS: case S_FXL_DIST: case S_FXL_PAN: case S_FXL_BUSES: case S_FXL_DELAY: case S_FXL_DELAY2:
    case S_FXL_REVERB: case S_FXL_SLICER: case S_FXL_MASTER: {
        static const char *const T[] = {"FX", "DIST", "AUTO PAN", "BUSES", "DLY", "DLY 2", "REVERB", "SLICER", "MASTER"};
        TSEL->p[P_DIST] = 30; TSEL->p[P_CHOR] = 60; TSEL->p[P_DLY] = 40; TSEL->p[P_REV] = 50;
        TSEL->p[P_DTYPE] = 0; TSEL->p[P_APDEP] = 40; TSEL->p[P_APWAVE] = 0;
        song.g[G_DFDBK] = 70; song.g[G_DMIX] = 50; song.g[G_RTYPE] = 0; song.g[G_RSIZE] = 70; song.g[G_CDEPTH] = 64;
        song.g[G_SLMODE] = SL_GATE; song.g[G_SLPAT] = 3; song.g[G_SLDEP] = 90;
        song.g[G_CLIP] = 30; song.g[G_PUNCH] = 40; song.g[G_DUCK] = 20; song.g[G_DREL] = 60;
        ui.fxmod = s == S_FXL_REVERB || s == S_FXL_BUSES ? 1u : s == S_FXL_SLICER ? 3u : s == S_FXL_MASTER ? 4u : 0u;
        go_title(T[s - S_FXL_SENDS]);
        ui.msg_t = 0;
        break;
    }
    /* (0.8.5) SAVE's lens as the mock-up's data: projects NOCHE LARGA (A B C), ACID 03, DUB KIT, BOSQUE, TRANCE A; the
     * song B16 C16 A32 D8 C16 A32 (scenes on 2 and 4, END LOOP); seven user sounds */
    case S_SAVE_LIB: case S_SAVE_TOOLS: case S_SAVE_PROJ: case S_SAVE_ARR: case S_SAVE_SOUND: case S_SAVE_PRESETS: {
        static const char *const PN[NSONG] = {"", "ACID 03", "DUB KIT", "", "BOSQUE", "", "TRANCE A", ""};
        static const uint8_t PM[NSONG] = {0, 3, 1, 0, 15, 0, 5, 0};
        static const chain_row_t AR[6] = {{1, 16}, {2, 16}, {0, 32}, {3, 8}, {2, 16}, {0, 32}};
        static const char *const UN[7] = {"DEEP SUB", "GLASS", "PAD NUBE", "WOBBLE", "BIRDCALL", "KEYS 2", "GRIT"};
        static const uint8_t UK[7] = {0, 1, 2, 4, 6, 9, 13}, UE[7] = {ENGI_ANALOG, ENGI_FM6, 2, ENGI_ANALOG, ENGI_ANIMAL, ENGI_FM6, 3};
        uint32_t k, e0 = TSEL->eng_req;
        song.playing = 0;
        project_save_as(0, "NOCHE LARGA"); project_save_as(1, "NOCHE B"); project_save_as(2, "NOCHE C");
        for (k = 0; k < NSONG; k++) { str_cpy(lib.name[k], PN[k], sizeof lib.name[k]); lib.mask[k] = PM[k]; }
        chain_config.count = 6;
        for (k = 0; k < 6u; k++) chain_config.row[k] = AR[k];
        SONG_END(&chain_config) = SONG_FOREVER;
        song_idx.scene[song_cur][1].on = 1; song_idx.scene[song_cur][3].on = 1;
        live.cur = 0; song.g[G_SLOT] = 2;
        for (k = 0; k < 7u; k++) { eng(UE[k]); up_store(UK[k], UN[k]); }
        eng(e0);
        ui.uslot = 0; ui.song_row = 0; ui.msg_t = 0; ui.frame += 7u;   /* (the sections' cache read again) */
        if (s == S_SAVE_LIB) { nav_scene(SEC_SAVE, "PROJECTS"); ui.act = 4; }
        else if (s == S_SAVE_TOOLS) { nav_scene(SEC_SAVE, "TOOLS"); ui.act = 1; }
        else if (s == S_SAVE_PROJ) { nav_scene(SEC_SAVE, "SECTIONS"); ui.act = 4; }
        else if (s == S_SAVE_ARR) nav_scene(SEC_SAVE, "ARRANGE");
        else if (s == S_SAVE_SOUND) { nav_scene(SEC_SAVE, "USER"); ui.act = 4; }
        else nav_scene(SEC_SAVE, "PRESETS");
        ui.msg_t = 0;
        break;
    }
    /* (0.8.5) VOICE's words: EDIT > WORDS singing (TEXT, a consonant sounding; W3 in English; the dice in Japanese),
     * EDIT 1 singing, WORDS with LANG OFF; the LYRIC typing screen (a short lyric; a long one, its window scrolled) */
    case S_WORDS: case S_WORDS_EN: case S_WORDS_JA: case S_WORDS_EDIT: {
        static const char *const W[4][2] = {{"corazon", "la noche es azul y el mar canta para ti"}, {"sunshine", ""},
                                            {"sakura", ""}, {"strength", "construir estrellas"}};
        static const uint8_t PH[4][4] = {{PH_S, PH_O, PH_N, 0}, {PH_SH, PH_AA, PH_IH, PH_N}, {PH_K, PH_UJ, 0, 0},
                                         {PH_S, PH_T, PH_RA, PH_EH}};
        uint32_t k = s - S_WORDS;
        sing_t *g = &sing[song.sel];
        eng(ENGI_VOICE_UI);
        TSEL->p[P_VLANG] = (int16_t)(k == 1u ? LG_EN : k == 2u ? LG_JA : LG_ES);
        TSEL->p[P_VLYR] = (int16_t)(k == 1u ? 3 : k == 2u ? 4 : 0);
        TSEL->p[P_E1] = (int16_t)(k == 1u ? 8 : 64);
        lyr_set(song.sel, W[k][1]);
        memset(g, 0, sizeof *g);
        str_cpy(g->w, W[k][0], sizeof g->w);
        memcpy(g->ph, PH[k], 4); g->np = PH[k][3] ? 4u : PH[k][2] ? 3u : 2u; g->nu = 1; g->cd = 2; g->i = k == 3u ? 2u : 1u;
        g->nsy = (uint16_t)(7u + k * 5u); g->to = k ? 0u : 13u;
        g->f[0] = 520; g->f[1] = 950; g->f[2] = 2450; g->ff = 3300; g->fb = 250; g->bm = 16;
        g->gv = k == 3u ? 2000 : 30000; g->gf = k == 3u ? 22000 : 0;
        if (k == 3u) go_title("EDIT 1");
        else go_page(GR_LYRIC);
        break;
    }
    case S_VX_HEAD_A: vx_phon(PH_A, 7000); break;
    case S_VX_HEAD_I: vx_phon(PH_I, 6000); break;
    case S_VX_HEAD_U: vx_phon(PH_U, 6000); break;
    case S_VX_HEAD_S: vx_phon(PH_S, 5000); break;
    case S_VX_HEAD_M: vx_phon(PH_M, 4000); break;
    case S_VX_HEAD_K: vx_phon(PH_K, 5000); break;
    case S_VX_HEAD_T: vx_phon(PH_T, 5000); break;
    case S_VX_HEAD_F: vx_phon(PH_F, 4000); ui.hot_col = 2; ui.hot_t = 30; break;
    case S_VX_HEAD_VOWEL:                               /* LANG OFF: VOWEL -> VOWEL2 (O, rounded) */
        eng(ENGI_VOICE_UI); TSEL->p[P_VLANG] = 0; TSEL->p[P_E0] = 100; TSEL->p[P_E5] = 90; TSEL->peak = 6000; vxs.init = 0;
        memset(vxs.fr, 0xFF, sizeof vxs.fr);
        go_title("EDIT 2");
        break;
    case S_VX_HEAD_HOME: vx_phon(PH_E, 6000); ui.home = 1; ui.hlv = 1; ui.hset[1] = 0; break;
    case S_VX_HEAD_QUIET: vx_phon(PH_SIL, 0); break;
    case S_VX_BIRD: vx_animal_scene("CANARY", -1); break;
    case S_VX_CMOS: vx_animal_scene("CMOS BIRD", -1); break;
    case S_VX_INSECT: vx_animal_scene("CRICKET", -1); break;
    case S_VX_WING: vx_animal_scene("BEE", -1); break;
    case S_VX_MAMMAL: vx_animal_scene("WOLF", -1); break;
    case S_VX_MORPH_IB: vx_animal_scene("CRICKET", 127); go_title("EDIT 2"); break;
    case S_VX_MORPH_BM: vx_animal_scene("DOG BARK", 4); break;
    case S_VX_ANIMAL_HOME: vx_animal_scene("OWL", -1); ui.home = 1; ui.hlv = 1; ui.hset[1] = 0; break;
    case S_VX_KIT: vx_kit_scene(0x03u, 0); dx_ui_lane = 1; dx_ui_seen = last_note; go_title("SOUND"); break;
    case S_VX_KIT_HITS: vx_kit_scene(0xBCu, 0); dx_ui_lane = 4; dx_ui_seen = last_note; go_title("SOUND 2"); break;
    case S_VX_KIT_GHOST: vx_kit_scene(0x22u, 2); dx_ui_lane = 5; dx_ui_seen = last_note; go_title("SOUND 3"); break;
    case S_VX_KIT_HOME: vx_kit_scene(0x8Bu, 0); ui.home = 1; ui.hlv = 1; ui.hset[1] = 0; break;
    case S_VX_BUG_BD: case S_VX_BUG_SD: case S_VX_BUG_CP: case S_VX_BUG_CH: case S_VX_BUG_OH: case S_VX_BUG_TM:
    case S_VX_BUG_RS: case S_VX_BUG_CB: vx_bug_scene((uint32_t)(s - S_VX_BUG_BD), 0, 1); break;
    case S_VX_BUG_GHOST: vx_bug_scene(1, 2, 1); break;
    case S_VX_BUG_FLAM: vx_bug_scene(1, 3, 1); break;
    case S_VX_BUG_RATCH: vx_bug_scene(3, 0, 3); break;
    case S_VX_BUG_QUIET: vx_bug_scene(0, 0, 0); break;
    case S_V6_ANALOG: { static const int16_t K[8] = {-99, -99, -99, -99, 70, 100, 110, -99}; v6_scene(ENGI_ANALOG, "ACID", "EDIT 1", K, 45, 400, 3); break; }
    case S_V6_ANALOG_SQR: { static const int16_t K[8] = {1, 40, 80, 70, 40, 30, 0, 7}; v6_scene(ENGI_ANALOG, "SQR BASS", "EDIT 2", K, 40, 40, 3); break; }
    case S_V6_ANALOG_RING: { static const int16_t K[8] = {6, 60, 90, -99, 90, 70, 40, -99}; v6_scene(ENGI_ANALOG, "SAW LEAD", "EDIT 1", K, 64, 700, 3); break; }
    case S_V6_ANALOG_QUIET: v6_scene(ENGI_ANALOG, "ACID", "EDIT 1", 0, 0, 4000, 3); break;
    case S_V6_ANALOG_HOME: v6_scene(ENGI_ANALOG, "SOFT PAD", 0, 0, 57, 300, 3); break;
    case S_V6_ANALOG_FULL: { static const int16_t K[8] = {-99, -99, -99, 30, 70, 100, 110, -99}; v6_scene(ENGI_ANALOG, "ACID", "EDIT 1", K, 45, 300, 3); ui_scope = 0; break; }
    case S_V6_FM6: v6_scene(ENGI_FM6, "TINE EP", "EDIT 1", 0, 60, 300, 4); break;
    case S_V6_FM6_ALG32: { static const int16_t K[8] = {32, 5, -99, -99, -99, -99, -99, -99}; v6_scene(ENGI_FM6, "BELL", "EDIT 2", K, 72, 120, 4); break; }
    case S_V6_FM6_OP: v6_scene(ENGI_FM6, "TINE EP", "OPERATOR", 0, 60, 300, 4); fop_op = 2; break;
    case S_V6_FM6_OP2:
        v6_scene(ENGI_FM6, "BRASS", "OPERATOR 2", 0, 55, 500, 4); fop_op = 1;
        fm6_patch[song.sel][(5u - 1u) * FP_OP + FP_DET] = 13;   /* (DETUNE +6: OP 2 shimmers) */
        break;
    case S_V6_FM6_ENV: v6_scene(ENGI_FM6, "BELL", "OP ENV", 0, 72, 600, 4); fop_op = 0; fop_stg = 1; break;
    case S_V6_FM6_ENV_REL: v6_scene(ENGI_FM6, "PAD", "OP ENV", 0, 0, 4000, 4); fop_op = 2; fop_stg = 3; break;
    case S_V6_FM6_HOME: v6_scene(ENGI_FM6, "MARIMBA", 0, 0, 67, 200, 4); break;
    case S_V6_FM6_FULL: v6_scene(ENGI_FM6, "TINE EP", "EDIT 1", 0, 60, 300, 4); ui_scope = 0; break;
    case S_V6_WHEEL: v6_scene(ENGI_WHEEL_UI, "ROCK DRIVE", "EDIT 1", 0, 60, 300, 5); break;
    case S_V6_WHEEL_FAST: { static const int16_t K[8] = {15, 0, 0, 0, 2, 100, 60, 2}; v6_scene(ENGI_WHEEL_UI, "JAZZ PERC", "EDIT 2", K, 62, 60, 5); break; }
    case S_V6_WHEEL_HOME: v6_scene(ENGI_WHEEL_UI, "GOSPEL", 0, 0, 55, 300, 5); break;
    case S_V6_WHEEL_FULL: v6_scene(ENGI_WHEEL_UI, "ROCK DRIVE", "EDIT 1", 0, 60, 300, 5); ui_scope = 0; break;
    case S_V6_NOISE: { static const int16_t K[8] = {1, 40, 80, 90, 64, 100, 30, 0}; v6_scene(ENGI_NOISE_UI, "RAIN", "EDIT 1", K, 60, 300, 6); break; }
    case S_V6_NOISE_LFSR: { static const int16_t K[8] = {2, 64, 90, 40, 64, 32, 0, 40}; v6_scene(ENGI_NOISE_UI, "ARCADE", "EDIT 2", K, 60, 300, 6); break; }
    case S_V6_NOISE_WHITE: { static const int16_t K[8] = {0, 0, 120, 10, 64, 0, 60, 0}; v6_scene(ENGI_NOISE_UI, "WIND", "EDIT 1", K, 48, 900, 6); break; }
    case S_V6_NOISE_HOME: v6_scene(ENGI_NOISE_UI, "SEA", 0, 0, 48, 300, 6); break;
    case S_V6_NOISE_FULL: { static const int16_t K[8] = {1, 40, 80, 90, 64, 100, 30, 0}; v6_scene(ENGI_NOISE_UI, "RAIN", "EDIT 1", K, 60, 300, 6); ui_scope = 0; break; }
    case S_V6_PHASE: v6_scene(ENGI_PHASE_UI, "BRASS", "EDIT 1", 0, 50, 300, 7); break;
    case S_V6_PHASE_RING: { static const int16_t K[8] = {3, 2, 90, 60, 30, 1, 80, 70}; v6_scene(ENGI_PHASE_UI, "BELL", "EDIT 2", K, 62, 50, 7); break; }
    case S_V6_PHASE_HOME: v6_scene(ENGI_PHASE_UI, "CZ PIANO", 0, 0, 57, 300, 7); break;
    case S_V6_PHASE_FULL: v6_scene(ENGI_PHASE_UI, "BRASS", "EDIT 1", 0, 50, 300, 7); ui_scope = 0; break;
    case S_V6_LOFI: v6_scene(ENGI_LOFI_UI, "BYTEBEAT", "EDIT 1", 0, 60, 300, 8); break;
    case S_V6_LOFI_FLOAT: v6_scene(ENGI_LOFI_UI, "FLOAT PAD", "EDIT 2", 0, 60, 120, 8); break;
    case S_V6_LOFI_PLS: v6_scene(ENGI_LOFI_UI, "PULSE LD", "EDIT 1", 0, 72, 600, 8); break;
    case S_V6_LOFI_HOME: v6_scene(ENGI_LOFI_UI, "GB ARP", 0, 0, 72, 300, 8); break;
    case S_V6_LOFI_FULL: v6_scene(ENGI_LOFI_UI, "BYTEBEAT", "EDIT 1", 0, 60, 300, 8); ui_scope = 0; break;
    case S_V6_STRING: v6_scene(ENGI_STRING, "STEEL GTR", "EDIT 1", 0, 48, 80, 9); break;   /* (0.9.9-snd5) TEJEDORA */
    case S_V6_STRING_BOW: v6_scene(ENGI_STRING, "BOWED", "EDIT 2", 0, 55, 900, 9); break;
    case S_V6_STRING_HOME: v6_scene(ENGI_STRING, "HARP", 0, 0, 60, 300, 9); break;
    case S_V6_STRING_FULL: v6_scene(ENGI_STRING, "PIANO", "EDIT 1", 0, 48, 120, 9); ui_scope = 0; break;
    case S_V6_ADD: v6_scene(ENGI_STRING, "FEEDBACK", "EDIT 2", 0, 48, 300, 10); break;   /* (par11: ADD retired; STRING FDBK's amp page) */
    case S_V6_ADD_BELL: v6_scene(ENGI_STRING, "SHOEGAZE", "EDIT 2", 0, 60, 120, 10); break;
    case S_V6_ADD_HOME: v6_scene(ENGI_STRING, "SQUEAL", 0, 0, 55, 300, 10); break;
    case S_V6_ADD_FULL: v6_scene(ENGI_STRING, "EBOW", "EDIT 1", 0, 60, 200, 10); ui_scope = 0; break;
    case S_V6_VOICE_FULL: vx_phon(PH_E, 6000); ui_scope = 0; break;            /* (0.9.7) SCOPE OFF: VOICE's head, ANIMAL's */
    case S_V6_ANIMAL_FULL: vx_animal_scene("CANARY", -1); ui_scope = 0; break;
    case S_VX_TRACT_A_FULL: vx_sing(PH_M, 6, PH_A, 4, 7000, 1, 0); break;   /* (main 0.9.9) an open vowel, a nasal */
    case S_VX_TRACT_M_FULL: vx_sing(PH_A, 8, PH_M, 7, 5000, 1, 0); break;
    case S_VX_TRACT_A_EDIT: vx_sing(PH_M, 6, PH_A, 4, 7000, 0, 0); break;
    case S_VX_TRACT_M_EDIT: vx_sing(PH_A, 8, PH_M, 7, 5000, 0, 0); break;
    case S_VX_TRACT_A_HOME: vx_sing(PH_M, 6, PH_A, 4, 7000, 0, 1); break;
    case S_VX_TRACT_M_HOME: vx_sing(PH_A, 8, PH_M, 7, 5000, 0, 1); break;
    case S_VX_KIT_MUTED: dx_mute_set(DXG_HAT | DXM_LANE(7)); vx_kit_scene(0x01u, 0); dx_ui_lane = 7; dx_ui_seen = last_note; go_title("SOUND 4"); break;
    case S_WORDS_OFF: eng(ENGI_VOICE_UI); TSEL->p[P_VLANG] = 0; go_page(GR_LYRIC); break;
    case S_NAME_LYRIC:
        eng(ENGI_VOICE_UI); TSEL->p[P_VLANG] = LG_ES; lyr_set(song.sel, "LA NOCHE");
        go_page(GR_LYRIC); name_open(NK_LYRIC, song.sel);
        break;
    case S_NAME_LYRIC_LONG:
        eng(ENGI_VOICE_UI); TSEL->p[P_VLANG] = LG_JA; song.playing = 1;
        lyr_set(song.sel, "SAKURA SAKURA YAYOI NO SORA WA MI WATASU KAGIRI KASUMI KA KUMO");
        go_page(GR_LYRIC); name_open(NK_LYRIC, song.sel); nm.cur = 40; nm.key = 3; nm.tap = 1; nm.t = fm1_ms;
        break;
    /* (0.8.5) DRUM-X octaves: the grid's HIT as its modes (the hat on step 3 x4; the snare on step 5 GHOST, the hat
     * beside it FLAM); OCT on a DRUM track names the octave (+3 RATCHET x2 since p1, -3 FLAM); COMP's TOUCH and ECHO EVEN */
    case S_DRUM_X4: case S_DRUM_GHOST:
        drum(0); go_page(GR_ROLL); trk[3].seq_idx = 9;
        step_set_lmode(&trk[3].step[2], 3, LM_X4); step_set_lmode(&trk[3].step[4], 1, LM_GHOST);
        step_set_lmode(&trk[3].step[4], 3, LM_FLAM);
        if (s == S_DRUM_X4) { ui.cursor = 2; ui.lane = 3; } else { ui.cursor = 4; ui.lane = 1; }
        break;
    case S_DRUM_OCT_UP: case S_DRUM_OCT_DN:
        drum(0); go_page(GR_ROLL); ui.cursor = 4; ui.lane = 1;
        song.octave = s == S_DRUM_OCT_UP ? 3 : -3;
        ui_message(drum_oct_name(song.octave));
        break;
    case S_LIVE_RIELES: case S_LIVE_RIELES_T3: case S_LIVE_MANO: case S_LIVE_MANO_KEYS: live_scene(s); break;
    case S_COMP_TOUCH: case S_COMP_EVEN:
        drum(0);
        trk[3].step[0].comp = s == S_COMP_TOUCH ? CP_TOUCH : CP_ECHO;
        trk[3].step[0].carg = s == S_COMP_TOUCH ? 1u : (uint8_t)(2u | CP_ECHO_EVEN);
        go_page(GR_COMP); ui.cursor = 0;
        break;
    default: break;
    }
    if (s != S_FUGUE)
        memset(fg_set, 0, sizeof fg_set);
    if (s != S_PFLASH && s != S_PFLASH_HOME)      /* (0.6.51) the page's big name: its own screen only (the clock stands */
        pf_on = 0;                                /* still here: it would never go) */
}
static void draw(int s)
{
    nscr = npend = nrules = 0;
    ntight = 0;
    tight[0] = 0;
    memset(host_screen, 0, sizeof host_screen);
    if (s == S_CALIBRATION) {                     /* the blocking setup screen: its two drawing steps */
        setup_title();
        setup_show("TURN RIGHT", "ALGORITHM");
        return;
    }
    if (s == S_SPLASH) {                          /* the power-on splash (main.c, before the UI): its lines, the version
                                                   * shown FELUCCA_VERSION (run_tests.sh: src/felucca.c's) */
        static const char WANT[] = FELUCCA_VERSION "|by Juan Jiant";   /* (JIANT 0.5: under the signature) */
        splash_seen[0] = 0;
        in_splash = 1;
        draw_splash();
        in_splash = 0;
        {   /* the signature written whole (spl_anim 0): its ink in its band (y 40 .. 164), the rest of the band BG */
            uint16_t bg = host_screen[0];
            uint32_t x, y, ink = 0;
            for (y = 40; y < 164u; y++)
                for (x = 0; x < 240u; x++)
                    ink += host_screen[y * 240u + x] != bg;
            if (ink < 600u) {
                if (rep) fprintf(rep, "FIND %-26s the splash's signature has %u pixels of ink (< 600)\n", cur_name, ink);
                nfind++;
            }
        }
        if (strcmp(splash_seen, WANT)) {
            if (rep) fprintf(rep, "FIND %-26s the splash shows '%s', not '%s'\n", cur_name, splash_seen, WANT);
            nfind++;
        } else {
            static uint8_t said;
            if (!said++) printf("ui_render: the splash shows the version " FELUCCA_VERSION " (FELUCCA_VERSION) and its lines\n");
        }
        return;
    }
    ui.force = 1;
    if (ui.menu == 1 && !ui.mlv && s != S_MENU_SLIDE) ui.mlv = 1;   /* (0.8.5: the lens at TAB, as it opens; S_MENU_SLIDE: MENU) */
    ui_draw();
}

/* every value of every column on every page of every engine: labels and values fit their column */
static uint32_t nsweep, nmotion;
/* #63: the page drawn again with MOTION driving each of its cards' track parameters (the motion icon beside
 * every label, the long ones too): the lint over it; the motion cleared again */
static void sweep_motion(void)
{
    uint32_t c, any = 0;
    for (c = 0; c < 4u; c++) {
        int16_t *vp = 0;
        const param_desc_t *d = ui.home ? home_param(c, &vp) : page_desc(cur_page(), c, &vp);
        uintptr_t id = vp ? ((uintptr_t)vp - (uintptr_t)TSEL->p) / sizeof *vp : P_COUNT;
        if (d && d->label && d->label[0] != '-' && id < P_COUNT && !motion_set_event(TSEL, c, (uint32_t)id, *vp))
            any = 1;
    }
    if (!any) return;
    draw(-1);
    for (c = 0; c < 4u; c++) nmotion += (card_mot >> c) & 1u;
    lint();
    memset(&motion, 0, sizeof motion);
}
static void sweep_columns(void)
{
    uint32_t e, i, c;
    char name[64];
    for (e = 0; e < NENGINES; e++) {
        if (!eng_ok(e))
            continue;                                    /* (DIGITAL without FELUCCA_FM4: no track has it) */
        for (i = 0; i < NPAGES; i++) {
            state();
            pal(UI_GREY_INDEX);
            eng(e);
            ui.home = 0; ui.page = (uint8_t)i; page_entered();
            if (!page_visible(i)) continue;
            snprintf(name, sizeof name, "%s%s/%s", large_on ? "LARGE " : "", ENGINES[e]->name, PAGES[i].title);
            cur_name = name;
            for (c = 0; c < 4u; c++) {
                int16_t *vp;
                const param_desc_t *d = PAGES[i].scope == SC_GLOBAL || PAGES[i].scope == SC_TRACK || PAGES[i].scope == SC_ENGINE
                                        ? page_desc(cur_page(), c, &vp) : 0;
                int32_t v, v0;
                if (!d || !d->label || d->label[0] == '-' || PAGES[i].graph == GR_MOD) continue;
                v0 = *vp;
                for (v = d->min; v <= d->max; v++) {
                    *vp = (int16_t)v;
                    nscr = npend = 0;
                    ui.force = 1;
                    draw_columns();
                    lint();
                    nsweep++;
                    if (v - d->min > 300) v = d->max - 1;      /* wide ranges: the ends */
                }
                *vp = (int16_t)v0;
            }
            if (PAGES[i].scope == SC_STEP && PAGES[i].id[0] == 4u && !e) {   /* (0.9.1) NOTE's MICRO: every 64th, the card and
                                                         * the big step (its chip, the notes moved) */
                int32_t v;
                step_t *st = &TSEL->step[ui.cursor];
                step_t keep = *st;
                st->time = ST_NOTE; st->n = 2; st->note[0] = 60; st->note[1] = 64; st->vel = 100;
                for (v = MICRO_MIN; v <= MICRO_MAX; v++) {
                    ST_MICRO_SET(st, v);
                    nscr = npend = 0;
                    ui.force = 1;
                    draw(-1);
                    lint();
                    nsweep++;
                }
                *st = keep;
            }
            if (PAGES[i].scope == SC_TRK && !e) {        /* (0.9.1) MIXER's KNOB 4: every DJ FILTER value */
                int32_t v;
                for (v = -64; v <= 63; v++) {
                    song.g[G_DJF] = (int16_t)v;
                    nscr = npend = 0;
                    ui.force = 1;
                    draw_columns();
                    lint();
                    nsweep++;
                }
                song.g[G_DJF] = 0;
            }
            draw(-1);                                    /* the whole page */
            lint();
            sweep_motion();
        }
        state(); pal(UI_GREY_INDEX); eng(e); go_home();   /* HOME's four knobs of this engine */
        snprintf(name, sizeof name, "%s%s/HOME", large_on ? "LARGE " : "", ENGINES[e]->name);
        cur_name = name;
        draw(-1);
        lint();
        sweep_motion();
    }
    for (e = 0; e < 4u; e++) {                           /* the MOD page: every source and destination */
        int32_t v;
        state(); pal(UI_GREY_INDEX); go_title("MOD");
        cur_name = large_on ? "LARGE MOD sweep" : "MOD sweep";
        for (v = 0; v < MD_N; v++) {
            TSEL->p[P_M1SRC] = (int16_t)(v % MS_N); TSEL->p[P_M1DST] = (int16_t)v; TSEL->p[P_M1AMT] = (int16_t)(e * 40 - 64);
            mod_ui_slot = (uint8_t)(v & 3u);
            draw(-1);
            lint();
        }
    }
}

/* the rolling digits (ui_draw.c roll_*): GLOBAL, SELECT and KNOB 4 turned together, BPM 129 -> 130 in the header
 * and TUNE 19 -> 20 on a card, then back. Every frame is linted and (GREY, MONO) checked for gray; with a directory,
 * filmstrips of both (each frame side by side, x4: the static frame before, then the roll's frames) as
 * DIR/<PALETTE>_bpm_roll.ppm and DIR/<PALETTE>_card_roll.ppm, the up roll above the down roll. */
#define FS_N (1u + ROLL_FRAMES)                   /* frames per filmstrip row */
#define FS_Z 4u                                   /* zoom */
#define FS_GAP 2u                                 /* px between frames, before the zoom */
static void roll_turns(int32_t s)                 /* one UI frame with the tempo and KNOB 4 moved by s */
{
    song.g[G_BPM] = (int16_t)(song.g[G_BPM] + s);  /* (0.6.67: SELECT alone scrolls the pages; the tempo set here) */
    host_enc[panel.enc[EN_K4]] += s * panel.dir[EN_K4];
    host_ticks += 16000u; fm1_ms += 16u;
    ui_input();
    ui_draw();
    host_ticks += 200000u;
}
static void roll_film_put(uint8_t *img, uint32_t iw, uint32_t row, uint32_t k, int32_t x0, int32_t y0, uint32_t w, uint32_t h)
{
    uint32_t x, y;
    for (y = 0; y < h * FS_Z; y++)
        for (x = 0; x < w * FS_Z; x++) {
            uint16_t c = swap16(host_screen[(uint32_t)(y0 + (int32_t)(y / FS_Z)) * 240u + (uint32_t)x0 + x / FS_Z]);
            uint8_t *p = img + (((row * (h + FS_GAP) + FS_GAP) * FS_Z + y) * iw + (k * (w + FS_GAP) + FS_GAP) * FS_Z + x) * 3u;
            p[0] = (uint8_t)((c >> 11) * 255u / 31u);
            p[1] = (uint8_t)(((c >> 5) & 63u) * 255u / 63u);
            p[2] = (uint8_t)((c & 31u) * 255u / 31u);
        }
}
static void roll_film_save(const char *dir, const char *pal, const char *name, const uint8_t *img, uint32_t iw, uint32_t ih)
{
    char path[512];
    FILE *f;
    snprintf(path, sizeof path, "%s/%s_%s.ppm", dir, pal, name);
    f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); return; }
    fprintf(f, "P6\n%u %u\n255\n", iw, ih);
    fwrite(img, 1, (size_t)iw * ih * 3u, f);
    fclose(f);
}
static void roll_frames(const char *dir)
{
    enum { HX = 56, HW = 48, HH = H_HEAD };       /* the header crop: the tempo icon and the BPM strip */
    static uint8_t bimg[(FS_N * (HW + FS_GAP) + FS_GAP) * FS_Z * (2u * (HH + FS_GAP) + FS_GAP) * FS_Z * 3u];
    static uint8_t cimg[(FS_N * (COL_W + FS_GAP) + FS_GAP) * FS_Z * (2u * (COL_H + FS_GAP) + FS_GAP) * FS_Z * 3u];
    const uint32_t biw = (FS_N * (HW + FS_GAP) + FS_GAP) * FS_Z, bih = (2u * (HH + FS_GAP) + FS_GAP) * FS_Z;
    const uint32_t ciw = (FS_N * (COL_W + FS_GAP) + FS_GAP) * FS_Z, cih = (2u * (COL_H + FS_GAP) + FS_GAP) * FS_Z;
    uint32_t p, row, k;
    for (p = 0; p < NPALETTES; p++) {
        char name[64];
        memset(bimg, 90, sizeof bimg);
        memset(cimg, 90, sizeof cimg);
        state(); song.playing = 0; pal(p);
        song.g[G_BPM] = 129; song.g[G_TUNE] = 19;
        go_title("GLOBAL");
        snprintf(name, sizeof name, "%s/roll", UI_PALETTES[p].name);
        cur_name = name;
        draw(-1);
        for (k = 0; k < 8u; k++) { ui.hot_t = 0; ui.bpm_t = 0; ui_draw(); }   /* settled, nothing hot */
        lint();
        for (row = 0; row < 2u; row++) {
            roll_film_put(bimg, biw, row, 0, HX, 0, HW, HH);
            roll_film_put(cimg, ciw, row, 0, CARD_X(3), Y_LABEL, COL_W, COL_H);
            for (k = 1; k < FS_N; k++) {
                if (k == 1u) roll_turns(row ? -1 : 1);
                else ui_draw();
                lint();
                mono_check();
                roll_film_put(bimg, biw, row, k, HX, 0, HW, HH);
                roll_film_put(cimg, ciw, row, k, CARD_X(3), Y_LABEL, COL_W, COL_H);
            }
            if (ui.roll[3].from[0] || ui.roll[ROLL_BPM].from[0] || song.g[G_BPM] != (row ? 129 : 130) ||
                song.g[G_TUNE] != (row ? 19 : 20)) {
                fprintf(rep, "ROLL %s: not over after %u frames (BPM %d TUNE %d)\n", name, ROLL_FRAMES, song.g[G_BPM], song.g[G_TUNE]);
                nfind++;
            }
            for (k = 0; k < 8u; k++) ui_draw();
        }
        if (dir && (p == UI_GREY_INDEX || p == UI_BW_INDEX || !strcmp(UI_PALETTES[p].name, "GREEN"))) {
            roll_film_save(dir, UI_PALETTES[p].name, "bpm_roll", bimg, biw, bih);
            roll_film_save(dir, UI_PALETTES[p].name, "card_roll", cimg, ciw, cih);
        }
    }
}
/* the host cost of a roll frame: the header and the cards (draw_head, draw_columns) while both strips roll,
 * against the same calls on an idle frame (nothing drawn) and a full redraw of the four cards and the header */
static void roll_cost(void)
{
    enum { N = 400 };
    uint32_t i, k;
    double t_roll = 0, t_full = 0;
    uint64_t px_roll = 0, bl_roll = 0, nroll = 0, px_full = 0, bl_full = 0;
    for (i = 0; i < N; i++) {
        clock_t c0;
        state(); song.playing = 0; pal(1);
        song.g[G_BPM] = 129; song.g[G_TUNE] = 19;
        go_title("GLOBAL");
        draw(-1);
        for (k = 0; k < 8u; k++) { ui.hot_t = 0; ui.bpm_t = 0; ui_draw(); }
        px_visited = 0; blit_px = 0;
        c0 = clock();
        ui.force = 1; ui.frame++; draw_head(); draw_columns(); ui.force = 0;
        t_full += (double)(clock() - c0); px_full += px_visited; bl_full += blit_px;
        nscr = npend = 0;
        roll_turns(i & 1u ? -1 : 1);                /* the change: the card and the header redrawn whole */
        for (k = 2; k < ROLL_FRAMES; k++) {         /* the strip-only frames */
            px_visited = 0; blit_px = 0;
            c0 = clock();
            ui.frame++; draw_head(); draw_columns();
            t_roll += (double)(clock() - c0); px_roll += px_visited; bl_roll += blit_px; nroll++;
            nscr = npend = 0;
        }
    }
    fprintf(rep, "  roll frame (header BPM 129->130 and card TUNE 19->20, strips only): %.2f us, %llu glyph px, %llu px blitted;"
            " header + 4 cards redrawn whole: %.2f us, %llu glyph px, %llu px blitted\n",
            t_roll / CLOCKS_PER_SEC / (double)nroll * 1e6, (unsigned long long)(px_roll / nroll), (unsigned long long)(bl_roll / nroll),
            t_full / CLOCKS_PER_SEC / N * 1e6, (unsigned long long)(px_full / N), (unsigned long long)(bl_full / N));
}

/* the alignment check over every value its places can show, beyond the screens above (FLAT and LINE, GREY): the
 * track cushions 1..4 at 12 16 24 px; every icon in a card's icon slot; the dialogs of every kind; every menu row with
 * each of its values; NAME's every key, letter and cycle in ABC and 123, the field holding every character; the
 * piano roll's C labels C-1 .. C9; the drum lanes of every kit; the mixer's LEVEL, PAN and REV over their ranges */
static void align_sweeps(void)
{
    uint32_t st, i, k, nf = nfind;
    FILE *r0 = rep, *nul = fopen("/dev/null", "w");
    int32_t v;
    if (nul) rep = nul;                              /* (not lint scenes: their draws pile up) */
    for (st = ST_FLAT; st <= ST_LINE; st++) {
        state(); pal(UI_GREY_INDEX);
        ui_style = (uint8_t)st; style_apply();
        cur_name = st ? "sweep LINE: cushions" : "sweep FLAT: cushions";
        for (k = 0; k < NTRK; k++)
            for (i = 0; i < 3u; i++) {
                cv_begin(40, 40, T_SURF);
                cv_icon_on(4, 4, i == 0u ? 12u : i == 1u ? 16u : 24u, trk_icon(k, 1), T_ACCENT, T_SURF);
            }
        cur_name = st ? "sweep LINE: card icons" : "sweep FLAT: card icons";
        for (i = 0; i < ICON_COUNT; i++) {
            ui.force = 1;
            draw_column(i & 3u, "LVL", "64", "", T_THEME, 500, i);
            ui.col[i & 3u][0] = 0;
        }
        cur_name = st ? "sweep LINE: LARGE card icons" : "sweep FLAT: LARGE card icons";
        ui_prefs = PREF_LARGE; ui.home = 1;             /* (tall: HOME) */
        for (i = 0; i < ICON_COUNT; i++) {
            ui.force = 1; ui.hot_col = (uint8_t)(i & 3u); ui.hot_t = (uint8_t)(i & 4u);
            draw_column(i & 3u, "LVL", i & 8u ? "OFF" : "64", i & 16u ? "%" : "", T_THEME, 500, i);
            ui.col[i & 3u][0] = 0;
        }
        ui.layer = LAYER_FX;                            /* (the labels in M: a layer's cards) */
        for (i = 0; i < ICON_COUNT; i++) {
            ui.force = 1; ui.hot_col = (uint8_t)(i & 3u); ui.hot_t = (uint8_t)(i & 4u);
            draw_column(i & 3u, "LVL", "64", "", T_THEME, 500, i);
            ui.col[i & 3u][0] = 0;
        }
        ui.layer = 0; ui.hot_t = 0; ui_prefs = 0;
        cur_name = st ? "sweep LINE: dialogs" : "sweep FLAT: dialogs";
        for (i = CF_CLEAR_SEQ; i <= CF_DICE_SOUND; i++)   /* (0.6.65: every dialog, the sound dice's CANCEL / OK too) */
            for (k = 0; k < 4u; k++) {
                ui.confirm = (uint8_t)i; ui.confirm_trk = (uint8_t)k;
                draw_confirm();
            }
        ui.confirm = 0;
        cur_name = st ? "sweep LINE: menu rows" : "sweep FLAT: menu rows";
        ui.menu = 1;
        for (i = 0; i < MI_COUNT; i++)
            for (k = 0; k < 4u; k++) {
                ui.menu_sel = (uint8_t)i; ui_prefs = k & 1u ? 0xFFu : 0u; settings_hold = (uint8_t)k; settings.lowcut = (uint8_t)(k % 3u);
                ui_rec_prefs = (uint8_t)((k % 3u) | (k % 3u) << 2 | ((k + 1u) % 3u) << 4);   /* (CLICK, LEVEL, COUNT-IN: each value) */
                settings_leds = (uint8_t)(k % LEDS_COUNT); ui.force = 1;   /* (every LEDS name) */
                draw_menu();
            }
        cur_name = st ? "sweep LINE: menu levels" : "sweep FLAT: menu levels";
        for (i = 0; i < MI_COUNT; i++)                  /* (0.8.5) every row on each level of the lens, S and LARGE */
            for (k = 0; k < 6u; k++) {
                ui.menu_sel = (uint8_t)i; ui_prefs = k & 1u ? PREF_LARGE : 0u; ui.mlv = (uint8_t)(k / 2u); ui.force = 1;
                draw_menu();
            }
        ui.mlv = 1;
        ui.menu = 0; ui_prefs = 0; ui_rec_prefs = 0; settings_hold = 0; settings.lowcut = 0; settings_leds = 0;
        cur_name = st ? "sweep LINE: REC layer values" : "sweep FLAT: REC layer values";
        ui.home = 1; ui.layer = LAYER_REC;              /* (the map and the cards: every value of the three, each track) */
        for (k = 0; k < 27u * 2u; k++) {
            rp_put(RP_CLICK, k % 3u); rp_put(RP_COUNTIN, k / 3u % 3u); rp_put(RP_LEVEL, k / 9u % 3u);
            song.sel = (uint8_t)(k % NTRK); ui_prefs = k >= 27u ? PREF_LARGE : 0u;
            ui.force = 1; draw_layer(); ui.force = 0;
        }
        ui.layer = 0; song.sel = 0; ui_prefs = 0; ui_rec_prefs = 0;
        cur_name = st ? "sweep LINE: count-in" : "sweep FLAT: count-in";
        for (i = 1; i <= 2u; i++)                       /* 1 and 2 bars, every beat */
            for (k = 4u * i; k >= 1u; k--) {
                cin_total = (uint8_t)(4u * i); cin_left = (uint8_t)k; ui.force = 1;
                draw_countin();
            }
        cin_total = cin_left = 0;
        state(); pal(UI_GREY_INDEX);
        cur_name = st ? "sweep LINE: NAME" : "sweep FLAT: NAME";
        song.playing = 0; go_page(GR_USER); name_open(NK_USER_SAVE, 6);
        for (k = 0; k < 2u; k++) {                       /* ABC, 123: every key cycling, every letter typed next */
            nm.num = (uint8_t)k;
            for (i = 0; i <= 16u; i++) {
                uint32_t n = i ? str_len(nm_group(i - 1u)) : 1u, j;
                for (j = 0; j < n; j++) {
                    nm.key = (uint8_t)i; nm.tap = (uint8_t)j;
                    nm_draw_panel();
                }
            }
        }
        nm.key = 0;
        for (i = 0; i < sizeof NM_SET - 1u; i += NM_LEN) {   /* the field: every character */
            for (k = 0; k < NM_LEN; k++) nm.s[k] = NM_SET[(i + k) % (sizeof NM_SET - 1u)];
            nm.s[NM_LEN] = 0; nm.len = NM_LEN; nm.cur = 0;
            nm_draw_field();
        }
        name_close();
        cur_name = st ? "sweep LINE: piano roll" : "sweep FLAT: piano roll";
        roll_scene(S_ROLL_ACID);
        for (v = 0; v + PR_ROWS <= 128; v++) {
            proll.lo = (uint8_t)v;
            cv_begin(240, H_GRAPH, T_SURF);
            graph_roll(TSEL, T_THEME);
        }
        state(); pal(UI_GREY_INDEX);
        cur_name = st ? "sweep LINE: drum kits" : "sweep FLAT: drum kits";
        for (k = 0; k < 4u; k++) {
            drum(k);
            for (i = 0; i < NLANE; i++) {
                ui.lane = (uint8_t)i;
                cv_begin(240, H_GRAPH, T_SURF);
                graph_grid(TSEL, T_THEME);
            }
        }
        state(); pal(UI_GREY_INDEX);
        cur_name = st ? "sweep LINE: mixer" : "sweep FLAT: mixer";
        go_page(GR_TRK);
        for (v = 0; v < 256; v++) {                    /* (then with LARGE: the strip) */
            ui_prefs = v >= 128 ? PREF_LARGE : 0u;
            trk[0].p[P_LEVEL] = (int16_t)(v & 127); trk[0].p[P_PAN] = (int16_t)((v & 127) - 64); trk[0].p[P_REV] = (int16_t)(v & 127);
            trk[1].p[P_MUTE] = (int16_t)(v & 1); song.rec = v & 2 ? 2u : 0u; ui.hot_t = (uint8_t)(v & 4); ui.hot_col = 3;
            ts.meter[0] = (uint8_t)(v % (TS_MH - 1));
            ui.force = 1;
            draw_tracks();
        }
        ui_prefs = 0; ui.hot_t = 0;
        ui.force = 0;
    }
    ui_style = ST_FLAT; style_apply();
    state();
    nscr = npend = nrules = 0;
    nfind = nf;
    rep = r0;
    if (nul) fclose(nul);
}

/* the keycaps' labels in their pills (tools/gen_aa_keycaps.py), from the data: the label's ink (nibbles 6..15) in the
 * pill (KC_H rows, its width) */
static void al_keycaps(void)
{
    uint32_t id;
    cur_name = "keycap data";
    for (id = 0; id < KC_COUNT; id++) {
        const kc_t *k = &KC[id];
        int32_t x, y, x0 = 999, y0 = 999, x1 = -1, y1 = -1, w = k->w;
        uint32_t i = 0;
        for (y = 0; y < KC_H; y++)
            for (x = 0; x < w; x++, i++)
                if (((KC_REF_DATA[KC_REF_OFF[id] + (i >> 1)] >> ((i & 1u) ? 0 : 4)) & 15u) >= 6u) {   /* (0.9.0: the nibbles) */
                    if (x < x0) x0 = x;
                    if (y < y0) y0 = y;
                    if (x + 1 > x1) x1 = x + 1;
                    if (y + 1 > y1) y1 = y + 1;
                }
        al_record("keycap label in its pill", (x0 + x1) - w, (y0 + y1) - KC_H, k->label, 0, 0, w, KC_H, x0, y0, x1, y1);
        if (al_rep) fprintf(al_rep, "keycap %-8s w %2d  ink x %2d..%2d y %d..%d  across %+.1f up/down %+.1f\n", k->label, w, x0, x1, y0, y1,
                            ((x0 + x1) - w) / 2.0, ((y0 + y1) - KC_H) / 2.0);
    }
}
static void al_table(void)
{
    uint32_t i;
    if (!al_rep) return;
    fprintf(al_rep, "\n%-30s %8s %5s %5s %5s %6s %6s\n", "place", "measured", "1px+", "-0.5", "+0.5", "across", "up/dn");
    for (i = 0; i < al_nst; i++) {
        const alstat_t *a = &al_st[i];
        if (!strcmp(a->tag, "self-test")) continue;
        fprintf(al_rep, "%-30s %8u %5u %5u %5u %6.1f %6.1f\n", a->tag, a->n, a->nfail, a->half_lo, a->half_hi, a->wh2 / 2.0,
                a->wv2 / 2.0);
        if (a->ex_h[0]) fprintf(al_rep, "    across  worst %s\n", a->ex_h);
        if (a->ex_v[0]) fprintf(al_rep, "    up/down worst %s\n", a->ex_v);
    }
    fprintf(al_rep, "\n%u measurements, %u places, %u off by 1 px or more; %u declared places drawn without their items\n",
            al_count, al_nst, al_fail, al_lost);
}

int main(int argc, char **argv)
{
    const char *out = argc > 1 ? argv[1] : "build/ui_new";
    char path[600];
    uint32_t p, s;
    static const char *const SHOW[] = {"GREY", "MONO", "GREEN", "PAPER", "NIGHT", "JIANT"};   /* (NIGHT: #50, to review) */
    snprintf(path, sizeof path, "%s/report.txt", out);
    rep = fopen(path, "w");
    if (!rep) { fprintf(stderr, "cannot write %s\n", path); return 1; }
    {
        char ap[600];
        snprintf(ap, sizeof ap, "%s/align.txt", out);
        al_rep = fopen(ap, "w");
        if (al_rep) fprintf(al_rep, "alignment: the ink of each centred text / icon / keycap against its box (gfx.c GFX_HOOK_ALIGN);"
                            " offsets in px, + right / down\n\n");
    }
    {
        char ap[600];
        snprintf(ap, sizeof ap, "%s/text_audit.tsv", out);
        audf = fopen(ap, "w");
    }
    if (audf) fprintf(audf, "screen\ttexts\ttext_px\ticons\ticon_px\tkeycaps\tkeycap_px\tellipsised\ttight\twords\ttight_list\n");
    {   /* the lint itself: an overlap, a value too wide for its column, a text past its canvas, one past its cell */
        state(); pal(0); cur_name = "self-test";
        nscr = npend = 0;
        cv_begin(COL_W, COL_H, T_BG);
        cv_text(0, 0, &AF_S, "AAAA", T_TEXT);
        cv_text(10, 4, &AF_S, "BBBB", T_TEXT);
        cv_text_fit(0, 15, &AF_M, "WWWWWWWW", T_THEME, T_BG, COL_W);
        cv_text(40, 30, &AF_M, "CUT", T_THEME);
        cv_rrect(2, 31, 20, 13, 3, T_SURF, T_BG);          /* a cell narrower than its word */
        cv_text(4, 31, &AF_S, "SPILL", T_TEXT);
        cv_blit(4, Y_LABEL);
        lint();
        nrules = 0;                                         /* a word on a divider (STYLE LINE) */
        lcd_rule(0, 120, 240, 1);
        cv_begin(40, 16, T_BG);
        cv_text(2, 0, &AF_S, "RULE", T_TEXT);
        cv_blit(8, 112);
        lint();
        nrules = 0;
        {   /* the alignment check: a word 2 px right of its box's centre, one centred */
            uint32_t f0 = al_fail;
            cv_begin(60, 20, T_BG);
            GFX_HOOK_ALIGN(0, 0, 60, 20, AL_HV, "self-test");
            cv_text_in(4, 0 + CAP_IN(S, 20), 60, &AF_S, "OFF", T_TEXT, T_BG);
            GFX_HOOK_ALIGN(0, 0, 60, 20, AL_HV, "self-test");
            cv_text_in(0, 0 + CAP_IN(S, 20), 60, &AF_S, "MID", T_TEXT, T_BG);
            if (al_fail - f0 != 1u) { fprintf(stderr, "ui_render: the alignment check missed its self-test (%u)\n", al_fail - f0); return 1; }
            al_fail = f0;
            al_count -= 2u;
            npend = 0;
        }
        if (nfind < 5u || !nspill) { fprintf(stderr, "ui_render: the lint missed its self-test (%u)\n", nfind); return 1; }
        fprintf(rep, "(self-test: %u findings above are expected)\n\n", nfind);
        nfind = 0;
    }
    for (p = 0; p < NPALETTES; p++)
        for (s = 0; s < S_COUNT; s++) {
            char name[64];
            uint32_t k;
            if (!FELUCCA_FM4 && (s == S_OP_ENV || (s >= S_ALG1 && s <= S_OP_LEVEL)))
                continue;                           /* (DIGITAL's own screens: FELUCCA_FM4=1 only) */
            snprintf(name, sizeof name, "%s/%s", UI_PALETTES[p].name, S_NAME[s]);
            cur_name = name;
            setup((int)s);
            pal(p);
            if (p == UI_GREY_INDEX) aud = audf;
            draw((int)s);
            lint();
            if (p == UI_GREY_INDEX) { audit_scene(S_NAME[s]); aud = 0; }
            mono_check();
            for (k = 0; k < sizeof SHOW / sizeof SHOW[0]; k++)
                if (!strcmp(UI_PALETTES[p].name, SHOW[k])) write_ppm(out, SHOW[k], S_NAME[s]);
        }
    {   /* STYLE LINE (gfx.c ST_*): every screen in every palette, linted (GREY: gray, MONO: neutral); GREY MONO NIGHT
         * PAPER as OUTDIR/ppm/<STYLE>_<PALETTE>_<screen>.ppm (ui_render.py: sheet_<STYLE>_<PALETTE>.png) */
        static const char *const ST_N[2] = {"FLAT", "LINE"}, *const ST_PAL[] = {"GREY", "MONO", "NIGHT", "PAPER"};
        uint32_t st, k, n0 = nfind, nruled = 0;
        for (st = ST_LINE; st <= ST_LINE; st++)
            for (p = 0; p < NPALETTES; p++) {
                for (k = 0; k < sizeof ST_PAL / sizeof ST_PAL[0] && strcmp(UI_PALETTES[p].name, ST_PAL[k]); k++) ;
                for (s = 0; s < S_COUNT; s++) {
                    char name[64], tag[24];
                    if (!FELUCCA_FM4 && (s == S_OP_ENV || (s >= S_ALG1 && s <= S_OP_LEVEL)))
                        continue;
                    snprintf(tag, sizeof tag, "%s_%s", ST_N[st], UI_PALETTES[p].name);
                    snprintf(name, sizeof name, "%s/%s", tag, S_NAME[s]);
                    cur_name = name;
                    setup((int)s);
                    pal(p);
                    ui_style = (uint8_t)st; style_apply();
                    draw((int)s);
                    if (ux.style != st) { fprintf(stderr, "ui_render: %s drawn in style %u\n", name, ux.style); return 1; }
                    nruled += nrules != 0u;
                    lint();
                    mono_check();
                    if (k < sizeof ST_PAL / sizeof ST_PAL[0]) write_ppm(out, tag, S_NAME[s]);
                }
            }
        ui_style = ST_FLAT; style_apply();
        if (nruled < 100u) { fprintf(stderr, "ui_render: dividers on %u STYLE screens only\n", nruled); return 1; }
        fprintf(rep, "STYLE LINE (every palette): %u lint findings\n", nfind - n0);
    }
    {   /* MENU > LARGE (ui.c large_kind): every screen in every palette in FLAT and LINE, linted (GREY: gray, MONO:
         * neutral); GREY MONO in FLAT as OUTDIR/ppm/LARGE_<PALETTE>_<screen>.ppm (ui_render.py: sheet_LARGE_<PALETTE>.png),
         * LINE GREY as LARGE-LINE_GREY; the tall cards' value faces counted (lg_hook) */
        static const char *const ST_N[2] = {"LARGE", "LARGE-LINE"};
        uint32_t st, n0 = nfind, ntall = 0;
        large_on = 1;
        for (st = ST_FLAT; st <= ST_LINE; st++)
            for (p = 0; p < NPALETTES; p++)
                for (s = 0; s < S_COUNT; s++) {
                    char name[64], tag[24];
                    if (!FELUCCA_FM4 && (s == S_OP_ENV || (s >= S_ALG1 && s <= S_OP_LEVEL)))
                        continue;
                    snprintf(tag, sizeof tag, "%s_%s", ST_N[st], UI_PALETTES[p].name);
                    snprintf(name, sizeof name, "%s/%s", tag, S_NAME[s]);
                    cur_name = name;
                    setup((int)s);
                    ui_prefs |= PREF_LARGE;
                    pal(p);
                    ui_style = (uint8_t)st; style_apply();
                    draw((int)s);
                    ntall += !ui.menu && !ui.confirm && !name_on() && large_kind() == LK_TALL;
                    lint();
                    mono_check();
                    if ((p == UI_GREY_INDEX || (!st && p == UI_BW_INDEX)))
                        write_ppm(out, tag, S_NAME[s]);
                }
        ui_style = ST_FLAT; style_apply();
        sweep_columns();                                /* every value of every column, tall */
        large_on = 0;
        if (ntall < 100u) { fprintf(stderr, "ui_render: LARGE drew %u screens with tall cards only\n", ntall); return 1; }
        fprintf(rep, "MENU > LARGE (FLAT and LINE, every palette, the page / value sweep): %u lint findings; %u screens with tall"
                " cards\n", nfind - n0, ntall);
        fprintf(rep, "  tall card values: %llu in L, %llu in M (a glyph not in L: \"%s\"), %llu in M (L too wide), %llu in S\n",
                (unsigned long long)lg_n[0], (unsigned long long)lg_n[1], lg_miss, (unsigned long long)lg_n[2],
                (unsigned long long)lg_n[3]);
        fprintf(rep, "  too wide for L, e.g.:");
        for (s = 0; s < lg_nwide; s++) fprintf(rep, " '%s'", lg_wide[s]);
        fprintf(rep, "\n  in S, e.g.:");
        for (s = 0; s < lg_nsmall; s++) fprintf(rep, " '%s'", lg_small[s]);
        fprintf(rep, "\n");
    }
    {   /* (0.8.9) MENU > DISPLAY > SKIN BOTTOM and HUD (0.9.7: AIR) (ui.c lz_low, hud_home): every screen in every palette in FLAT, in
         * LINE and with MENU > LARGE ON (COLORS' layout, the skin's tabs and colours), linted (GREY: gray, MONO: neutral); FLAT
         * as OUTDIR/ppm/<SKIN>_<PALETTE>_<screen>.ppm for JIANT PAPER GREY MONO (ui_render.py: sheet_<SKIN>_<PALETTE>.png) */
        static const char *const SK_N[SKIN_N] = {"COLORS", "BOTTOM", "HUD", "AIR"}, *const ST_N[3] = {"", "-LINE", "-LARGE"};
        uint32_t sk, st, n0 = nfind, nlow = 0, nhud = 0;
        for (sk = SKIN_BOTTOM; sk <= SKIN_AIR; sk++)   /* (0.9.7: AIR, the knobs' row on top) */
            for (st = 0; st < 3u; st++)
                for (p = 0; p < NPALETTES; p++)
                    for (s = 0; s < S_COUNT; s++) {
                        char name[64], tag[24];
                        if (!FELUCCA_FM4 && (s == S_OP_ENV || (s >= S_ALG1 && s <= S_OP_LEVEL)))
                            continue;
                        snprintf(tag, sizeof tag, "%s%s_%s", SK_N[sk], ST_N[st], UI_PALETTES[p].name);
                        snprintf(name, sizeof name, "%s/%s", tag, S_NAME[s]);
                        cur_name = name;
                        setup((int)s);
                        if (st == 2u) ui_prefs |= PREF_LARGE;
                        pal(p);
                        ui_skin = (uint8_t)sk;
                        ui_style = (uint8_t)(st == 1u); style_apply();
                        draw((int)s);
                        if (ux.skin != sk) { fprintf(stderr, "ui_render: %s drawn in skin %u\n", name, ux.skin); return 1; }
                        nlow += (uint32_t)lz_low();
                        nhud += (uint32_t)hud_home();
                        lint();
                        mono_check();
                        if (!st && (p == UI_JIANT_INDEX || p == UI_GREY_INDEX || p == UI_BW_INDEX || !strcmp(UI_PALETTES[p].name, "PAPER")))
                            write_ppm(out, tag, S_NAME[s]);
                    }
        ui_skin = SKIN_COLORS; ui_style = ST_FLAT; style_apply();
        if (nlow < 1000u || nhud < 20u) { fprintf(stderr, "ui_render: SKIN drew %u screens low, %u HUD HOMEs only\n", nlow, nhud); return 1; }
        fprintf(rep, "SKIN BOTTOM and HUD (FLAT, LINE, LARGE, every palette): %u lint findings; %u screens with the knob row, %u HUD "
                "HOMEs\n", nfind - n0, nlow, nhud);
    }
    {   /* every FM6 colony (the lint above, fm6_hook, runs on each), in GREY: gray */
        uint32_t a, c0 = fmp_charts;
        for (a = 1; a <= 32u; a++) {
            char name[32];
            snprintf(name, sizeof name, "FM6 ALG %u", a);
            cur_name = name;
            state(); pal(UI_GREY_INDEX); eng(ENGI_FM6); TSEL->p[P_E0] = (int16_t)a; go_title("OPERATOR");   /* (0.8.5) */
            draw(-1);
            lint();
            mono_check();
        }
        if (fmp_charts - c0 != 32u) { fprintf(stderr, "ui_render: %u FM6 colonies drawn of 32\n", fmp_charts - c0); return 1; }
    }
    {   /* (0.9.6) FM6's operator pages with their colony keep every number they showed: ALG, the operator and its frequency
         * (OPERATOR, OPERATOR 2), OP ENV's stage with its R and L, the four cards */
        static const struct { int s; const char *w[6]; } FT[] = {
            {S_V6_FM6_OP, {"ALG 05 PAT", "OP3 x1.00", "RATIO", "FINE", "LEVEL", "/6"}},
            {S_V6_FM6_OP2, {"ALG ", "OP2 x", "MODE", "DETUNE", "VEL", "+6"}},
            {S_V6_FM6_ENV, {"OP1", "DEC  R ", "STAGE", "RATE", "LEVEL", "DEC"}}};
        uint32_t f, w, i, n0 = nfind;
        for (f = 0; f < NELEM(FT); f++) {
            cur_name = S_NAME[FT[f].s];
            setup(FT[f].s); pal(UI_GREY_INDEX);
            draw(FT[f].s);
            for (w = 0; w < 6u; w++) {
                for (i = 0; i < nscr && !strstr(scr[i].s, FT[f].w[w]); i++)
                    ;
                if (i == nscr) {
                    fprintf(rep, "FIND %-26s FM6's page lost '%s'\n", cur_name, FT[f].w[w]);
                    nfind++;
                }
            }
        }
        if (nfind != n0) { fprintf(stderr, "ui_render: FM6's operator pages lost numbers (report.txt)\n"); return 1; }
    }
    sweep_columns();
    roll_frames(argc > 2 ? argv[2] : 0);
    al_off = 1;                                     /* (the alignment check is not timed) */
    /* draw cost: a full redraw of a screen (all strips), and HOME frame by frame (the scope, every other frame) */
    {
        static const int COST[] = {S_HOME, S_PRESETS, S_STEP, S_ROLL_CHORDS, S_DRUM, S_MIXER, S_MENU, S_ABOUT_CREDITS, S_CONFIRM_PROJ,
                                   S_PATTERN, S_COMP, S_LENTE_PAT, S_LENTE_BAR, S_LENTE_STEP, S_LENTE_STEP_DRUM,
                                   S_SAVE_LIB, S_SAVE_PROJ, S_SAVE_SOUND, S_FXL_SENDS, S_FXL_BUSES, S_FXL_REVERB,
                                   S_HOME_BEING, S_EDIT_ANALOG, S_ED_FILTER, S_ED_FOCUS, S_ENV_TRACKS, S_MOD, S_KEY_CHORD,
                                   S_SYSTEM, S_MENU_ROW, S_LY_FX_KNOBS, S_DLG_DICE_SOUND, S_EDIT_SAMPLE, S_VX_HEAD_S, S_VX_BIRD,
                                   S_VX_MAMMAL, S_VX_KIT_HITS, S_VX_HEAD_HOME, S_VX_KIT_HOME, S_VX_BUG_SD,
                                   S_V6_ANALOG, S_V6_FM6, S_V6_FM6_OP, S_V6_FM6_OP2, S_V6_FM6_ENV, S_V6_WHEEL, S_V6_NOISE, S_V6_PHASE,
                                   S_V6_LOFI, S_V6_LOFI_FLOAT, S_V6_ANALOG_HOME, S_V6_FM6_HOME, S_V6_WHEEL_HOME, S_V6_NOISE_HOME,
                                   S_V6_PHASE_HOME, S_V6_LOFI_HOME, S_V6_ANALOG_FULL, S_V6_FM6_FULL, S_V6_WHEEL_FULL,
                                   S_V6_NOISE_FULL, S_V6_PHASE_FULL, S_V6_LOFI_FULL};   /* (0.9.6: the six, EDIT, HOME, the whole panel) */   /* (0.9.4: the creatures against a being) */   /* (0.8.5: the lenses everywhere) */   /* (0.8.3:
                                                                         * LENTE's three levels) */
        enum { N = 200 };
        uint32_t i, k;
        fprintf(rep, "\ndraw cost on the host (cc -O1; only the ratios mean anything on the device):\n");
        fprintf(rep, "  screen            full redraw us   texts+icons   glyph px read\n");
        for (k = 0; k < sizeof COST / sizeof COST[0]; k++) {
            clock_t c0;
            uint64_t px, nt;
            setup(COST[k]); pal(1);
            draw(COST[k]);
            px_visited = n_text = 0;
            c0 = clock();
            for (i = 0; i < N; i++) draw(COST[k]);
            px = px_visited / N; nt = n_text / N;
            fprintf(rep, "  %-16s %15.1f %13llu %15llu\n", S_NAME[COST[k]], (double)(clock() - c0) / CLOCKS_PER_SEC / N * 1e6,
                    (unsigned long long)nt, (unsigned long long)px);
        }
        {
            clock_t c0;
            setup(S_HOME); pal(1); draw(S_HOME);
            px_visited = n_text = 0;
            c0 = clock();
            for (i = 0; i < N * 4; i++) { ui_draw(); nscr = npend = 0; }
            fprintf(rep, "  HOME, a frame without force (lazy strips): %.1f us, %llu texts\n",
                    (double)(clock() - c0) / CLOCKS_PER_SEC / (N * 4) * 1e6, (unsigned long long)(n_text / (N * 4)));
        }
        {   /* the STEP page's piano roll: its panel alone redrawn, and a frame that changes nothing (the signature only) */
            static const int G[] = {S_ROLL_CHORDS, S_ROLL_PLAYING, S_DRUM, S_PATTERN, S_COMP, S_LENTE_PAT, S_LENTE_STEP,
                                    S_SAVE_LIB, S_SAVE_SOUND, S_FXL_BUSES, S_FXL_REVERB, S_FXL_DELAY, S_EDIT_ANALOG,
                                    S_ENV_TRACKS, S_MOD, S_SYSTEM, S_VX_HEAD_S, S_VX_INSECT, S_VX_KIT_HITS, S_DX_SOUND};
            for (k = 0; k < sizeof G / sizeof G[0]; k++) {
                clock_t c0;
                double tg, ti;
                setup(G[k]); pal(1); draw(G[k]);
                c0 = clock();
                for (i = 0; i < N * 4; i++) { ui.force = 1; draw_graph(); ui.force = 0; nscr = npend = 0; }
                tg = (double)(clock() - c0) / CLOCKS_PER_SEC / (N * 4) * 1e6;
                song.playing = 0;                        /* (no playhead moving) */
                ui_draw(); ui_draw(); nscr = npend = 0;
                c0 = clock();
                for (i = 0; i < N * 4; i++) { ui.frame++; draw_graph(); nscr = npend = 0; }
                ti = (double)(clock() - c0) / CLOCKS_PER_SEC / (N * 4) * 1e6;
                fprintf(rep, "  %s: the panel redrawn %.1f us; an unchanged frame (signature only) %.2f us\n", S_NAME[G[k]], tg, ti);
            }
        }
        roll_cost();
        {   /* (0.8.9) SKIN: HOME, EDIT, SEQ's BAR, FX's BUSES redrawn whole in each skin (us, texts, LCD px sent), and HOME frame
             * by frame (HUD: every strip each frame, on the beings) */
            static const int SC[] = {S_HOME, S_EDIT_ANALOG, S_LENTE_BAR, S_FXL_BUSES, S_VX_HEAD_S, S_VX_WING, S_VX_KIT_HOME};
            static const char *const SK_N[SKIN_N] = {"COLORS", "BOTTOM", "HUD", "AIR"};
            uint32_t sk;
            fprintf(rep, "  SKIN (full redraw us / texts / LCD px; HOME a frame without force):\n");
            for (sk = 0; sk < SKIN_N; sk++) {
                fprintf(rep, "  %-7s", SK_N[sk]);
                for (k = 0; k < sizeof SC / sizeof SC[0]; k++) {
                    clock_t c0;
                    setup(SC[k]); ui_skin = (uint8_t)sk; pal(UI_JIANT_INDEX); style_apply();
                    draw(SC[k]);
                    px_visited = n_text = blit_px = 0;
                    c0 = clock();
                    for (i = 0; i < N; i++) draw(SC[k]);
                    fprintf(rep, "  %s %.1f / %llu / %llu", S_NAME[SC[k]], (double)(clock() - c0) / CLOCKS_PER_SEC / N * 1e6,
                            (unsigned long long)(n_text / N), (unsigned long long)(blit_px / N));
                }
                {
                    clock_t c0;
                    setup(S_HOME); ui_skin = (uint8_t)sk; pal(UI_JIANT_INDEX); style_apply(); draw(S_HOME);
                    blit_px = 0;
                    c0 = clock();
                    for (i = 0; i < N * 4; i++) { ui_draw(); nscr = npend = 0; }
                    fprintf(rep, "  HOME frame %.1f us, %llu LCD px\n", (double)(clock() - c0) / CLOCKS_PER_SEC / (N * 4) * 1e6,
                            (unsigned long long)(blit_px / (N * 4)));
                }
            }
            ui_skin = SKIN_COLORS; style_apply();
        }
        {   /* MENU > LARGE: the same full redraws, tall cards and the strip */
            static const int LC[] = {S_HOME, S_EDIT_ANALOG, S_ENV, S_LFO, S_MIXER, S_FX, S_PATTERN};
            fprintf(rep, "  MENU > LARGE:\n");
            large_on = 1;
            for (k = 0; k < sizeof LC / sizeof LC[0]; k++) {
                clock_t c0;
                uint64_t px, nt;
                setup(LC[k]); pal(1);
                draw(LC[k]);
                px_visited = n_text = 0;
                c0 = clock();
                for (i = 0; i < N; i++) draw(LC[k]);
                px = px_visited / N; nt = n_text / N;
                fprintf(rep, "  %-16s %15.1f %13llu %15llu\n", S_NAME[LC[k]], (double)(clock() - c0) / CLOCKS_PER_SEC / N * 1e6,
                        (unsigned long long)nt, (unsigned long long)px);
            }
            {
                clock_t c0;
                setup(S_HOME); pal(1); draw(S_HOME);
                px_visited = n_text = 0;
                c0 = clock();
                for (i = 0; i < N * 4; i++) { ui_draw(); nscr = npend = 0; }
                fprintf(rep, "  LARGE HOME, a frame without force (lazy strips): %.1f us, %llu texts\n",
                        (double)(clock() - c0) / CLOCKS_PER_SEC / (N * 4) * 1e6, (unsigned long long)(n_text / (N * 4)));
                setup(S_ENV); pal(1); draw(S_ENV);
                px_visited = n_text = 0;
                c0 = clock();
                for (i = 0; i < N * 4; i++) { ui_draw(); nscr = npend = 0; }
                fprintf(rep, "  LARGE ENV, a frame without force (lazy strips): %.1f us, %llu texts\n",
                        (double)(clock() - c0) / CLOCKS_PER_SEC / (N * 4) * 1e6, (unsigned long long)(n_text / (N * 4)));
            }
            large_on = 0;
        }
    }
    al_off = 0;
    al_keycaps();
    align_sweeps();
    al_table();
    if (al_rep) fclose(al_rep);
    fprintf(rep, "\nalignment: %u measurements of %u places, %u off by 1 px or more (OUTDIR/align.txt)\n", al_count, al_nst - 1u, al_fail);
    if (audf) fclose(audf);
    fprintf(rep, "\n%u lint findings, %u inks on their own colour, %u ellipsised free texts, %u GREY / MONO pixels off gray / "
            "neutral; %u column values swept; %u FM6 charts linted; %u cards linted with MOTION's icon\n", nfind, nink, nfree, mono_bad,
            nsweep, fmp_charts, nmotion);
    if (!nmotion) { fprintf(stderr, "ui_render: the MOTION sweep marked no card\n"); nfind++; }
    fclose(rep);
    printf("ui_render: %u screens x %u palettes + the page/value sweep; %u lint findings, %u ellipsised free texts (%u distinct), "
           "%u inks on their own colour, %u GREY / MONO pixels off gray / neutral; report %s\n",
           (unsigned)(S_COUNT - (FELUCCA_FM4 ? 0 : 1 + S_OP_LEVEL - S_ALG1 + 1)), (unsigned)NPALETTES, nfind, nfree, nfree_seen, nink,
           mono_bad, path);
    printf("ui_render: alignment: %u measurements of %u places, %u off by 1 px or more%s\n", al_count, al_nst - 1u, al_fail,
           al_fail ? " (see align.txt)" : "");
    return nfind || nink || mono_bad || al_fail ? 1 : 0;
}
