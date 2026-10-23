/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* FELUCCA user interface.
 * Flat: SURF cards and panels on the palette's background, no rules, one type family (Inter Tight, three sizes), tracks
 * named by their numbers 1..4 on a cushion (icons.c trk_icon). Four columns <-> KNOB 1..4. Rendering is lazy:
 * every element remembers what it last drew and is redrawn only on change. */
/* (0.9.7, flash) the selected track by a call in the main loop's code from here on (core.h's TSEL, an address computed
 * in place, stays for the sound side included before) */
static track_t *tsel_p(void) { return &trk[song.sel]; }
#undef TSEL
#define TSEL tsel_p()
static uint32_t trk_eng(const track_t *t) { return t->eng_req % NENGINES; }   /* (0.9.7, flash) a track's engine */
static const engine_t *trk_engp(const track_t *t) { return ENGINES[trk_eng(t)]; }   /* .. its engine's table */
static int project_save(uint32_t slot);
static void panel_setup(void);
static void project_load(uint32_t slot);
static int section_jump(uint32_t s);    /* (song_main.c: 0.6.80, channel 16's PROGRAM CHANGE) */
static int song_select(uint32_t n);
static void undo_mark(void);                    /* (0.6.21) project.c: the music before a change, */
static void system_init(void);                  /* (0.6.40) project.c: INIT SYSTEM */
static uint32_t mut_progress(uint32_t k);       /* (0.6.43) project.c: MUTATE's next, 0..256 (0xFFFF: off) */
static void mut_keep(void);                     /* .. the music now the anchor */
static void mut_forget(void);                   /* (0.9.3) .. another music now: no going back to the anchor */
static int mut_revert(uint32_t mask);           /* .. those tracks back to the anchor (0: done, 1: NO ANCHOR) */
static void mut_revert_ui(int all);             /* .. REVERT (SEQ + D5) / REVERT ALL (GLO + B4), UNDO first */
static int mut_back_ok(void);                   /* .. an anchor to go back to */
static uint8_t mut_flash[NTRK];                 /* .. a mutation just now: frames left of its flash (HOME) */
static uint8_t mut_freeze;                      /* .. GLO + A4: no mutation until again */
static const uint8_t MUT_BARS[7] = {0, 1, 2, 4, 8, 16, 32};   /* .. SOUND / PATTERN every (OFF, bars) */
static int undo_do(void);                       /* .. back (and again: forward) */
static uint8_t undo_ok;
static int project_used(uint32_t slot);
static uint32_t lib_scan(uint32_t n, char *b);           /* (0.8.5) project.c: SAVE > LIBRARY's tiles */
static uint32_t lib_gen(void);
static int project_recall_sound(uint32_t slot, uint32_t k);
static int undo_sound(uint32_t k);                 /* (0.6.65) project.c: the sound dice's CANCEL */
static int project_recall_slot(void);
static uint32_t chain_prepare(void);
static int up_used(uint32_t k);              /* user presets: upreset.c */
static int up_load(uint32_t k);
static uint32_t up_count(void);
static uint32_t up_nth(uint32_t n);
static uint32_t up_rank(uint32_t slot);
static uint32_t up_engine(uint32_t k);
static void up_name(uint32_t k, char *b);
static void up_slot_label(char *b, uint32_t k);
static void up_ui(uint32_t op, uint32_t k);
static int up_has_pat(uint32_t k);               /* user presets that hold a pattern */
static uint32_t up_pat_count(void);
static uint32_t up_pat_nth(uint32_t n);
static uint32_t up_pat_rank(uint32_t slot);
static void up_pat_load(track_t *t, uint32_t k);
static void up_auto_name(char *b, uint32_t e, uint32_t k);   /* naming (ui_name.c) */
static void up_ui_named(uint32_t op, uint32_t k, const char *name);
static int project_save_as(uint32_t slot, const char *name);
static int project_name(uint32_t slot, char *b);
static int project_rename(uint32_t slot, const char *name);
static void project_cur_name(char *b);
static uint32_t user_of(const track_t *t)    /* user preset slot its sound came from, UP_SLOTS = none */
{
    return t->user && up_used(t->user - 1u) ? t->user - 1u : UP_SLOTS;
}
static uint32_t up_gen;                      /* bumped on every user bank change (redraws) */
#include "favorites.c"
/* MENU's two-valued settings (ui_menu.c MENU_FLAGS), a bit each in a byte no engine uses:
 * saved with the settings; 0 in older ones = every setting's default (append-only: a new setting takes a new bit,
 * its default is 0) */
#define ui_prefs (favorites.factory[15][30])
#define PREF_LATCH 1u                          /* MENU > FX LATCH ON (#40); JIANT 0.6.44: ON once for everyone (prefs_rev);
                                                * 0.6.64: OFF again (the FX screen locks open by itself) */
#define prefs_rev (favorites.factory[15][25])  /* (JIANT 0.6.44) the settings' revision (a byte no engine uses: engine 15):
                                                * 0 before; 1: FX LATCH turned ON once (main.c prefs_update); 2: OFF once (0.6.64) */
#define PREF_ANIM_OFF 2u                       /* MENU > ANIM OFF (#46): values snap (no rolling digits, no glide) */
/* (0.6.57) values snap: MENU > ANIM OFF, or LOAD LITE / ECO (the rolling digits, the roll's and the tabs' glides are
 * frames drawn again for a look: the time goes to the keys and knobs instead; the beings still move) */
/* (0.9.9-beta4) the UI held while the music plays with the render's averaged share of a half (voice.c load_avg_q8, ~93
 * ms) past UI_HOLD_ON %: frames every 50 ms instead of 15 and the animation off, as MENU > ANIM OFF (the HOME beings,
 * the engines' creatures, the live graphs still: their signatures stop changing, nothing is drawn again), until it
 * falls under UI_HOLD_OFF % or the music stops (ui_hold_poll, main.c: once a frame). The I-cache is one for both cores
 * and the UI: a frame drawn is the engines' code evicted (Kirill's audit, LOOM-8) */
static uint8_t ui_hold;
static uint32_t anim_at;                               /* the last frame the animation moved on (ui_draw) .. */
#define UI_HOLD_ON 60u
#define UI_HOLD_OFF 50u
#define ANIM_OFF() ((ui_prefs & PREF_ANIM_OFF) || ui_hold)
#define anim_fr (ANIM_OFF() ? anim_at : ui.frame)      /* .. the animation's frame: ui.frame while it moves, that one while
                                                        * still (the same frame draws the same picture, held or not) */
#define ANIM_SNAP() (ANIM_OFF() || sys_load)
static void ui_hold_poll(void)
{
    uint32_t pct = load_avg_q8 >> 8;
    if (!song.playing || pct < UI_HOLD_OFF)
        ui_hold = 0;
    else if (pct >= UI_HOLD_ON)
        ui_hold = 1;
}
#define PREF_ACCEL_OFF 4u                      /* MENU > KNOB ACCEL OFF (#52; JIANT: ON by default, the bit now means OFF):
                                                * fast turns of wide values x2..x16 */
#define PREF_USB_FIXED 8u                      /* MENU > USB LEVEL FIXED: USB audio at the full level, MASTER after */
#define usb_out (favorites.factory[15][21])   /* (0.7.1) MENU > USB STEMS: 1, USB audio as 8 channels (fx.c fx_stems, usb.c
                                                 * usb_stems); (0.7.5) 2 IN+OUT, the host's audio output too (usb.c
                                                 * usb_duplex); (0.8.0) 3 STEMS+IN, both; 0 stereo (every older setting) */
#define usb_lvl (favorites.factory[15][22])   /* (0.7.0) MENU > USB LEVEL past FIXED: 0 FULL, 1..4 = -6 -12 -18 -24 dB
                                                 * (with PREF_USB_FIXED; a byte no engine uses, 0 in older settings) */
static const int16_t USB_GAIN[5] = {4096, 2048, 1024, 512, 256};   /* Q12: 0 -6 -12 -18 -24 dB */
#define PREF_BPM_LOCK 16u                      /* MENU > BPM LOCK ON (#58): DICE ALL keeps the tempo; (0.6.67) SELECT
                                                * alone never sets it (GLO + SELECT does) */
#define PREF_LARGE 32u                         /* MENU > LARGE ON (#15, Discussion #80): big knob labels and values
                                                * (ui_draw.c large_kind); clear in every older setting = OFF */
#define PREF_SERIAL_OFF 64u                    /* MENU > USB SERIAL OFF (#67): no serial console, the device enumerates as
                                                * audio + MIDI only (usb.c usb_cdc_switch); clear in every older setting = ON */
#define PREF_RESTORE_ON 128u                   /* MENU > RESTORE LAST ON (1.2, Discussion #130: the autosave written and
                                                * restored at power-on, project.c). JIANT 0.6.40: OFF by default (clear =
                                                * OFF, power-on as INIT SYSTEM leaves it); the bit was RESTORE LAST OFF */
#define fx_latch ui_prefs
/* (JIANT 0.6.44) the settings brought up to date at power-on (main.c; 0.6.58 the browser emulator too, web/emu, where
 * the punch-in FX had no LATCH): FX LATCH ON by default, once; (0.6.64) OFF again, once, so who turns it ON after
 * keeps it so */
static void prefs_update(void)
{
    if (prefs_rev < 2u) {                              /* (0.6.64) FX LATCH OFF again (the punch-in effects momentary:
                                                        * held FX locks the FX screen open instead), once */
        ui_prefs &= (uint8_t)~PREF_LATCH;
        prefs_rev = 2u;
        settings_save();
    }
}
/* MENU > AUDIO CLICK, CLICK LEVEL, COUNT-IN (1.1, Discussion #131; click.c, seq.c): two bits each in another byte no
 * engine uses, saved with the settings: bits 0-1 CLICK (OFF REC ON), 2-3 CLICK LEVEL (stored 0 MID, 1 LOW, 2 HIGH),
 * 4-5 COUNT-IN (OFF, 1 BAR, 2 BARS); 0 in older settings = OFF / MID / OFF, so nothing clicks or counts in that did
 * not; 3 (unknown) reads as 0 (settings_persist.c puts it back). Bit 6 (1.2): MENU > SCALE LEDS ON (Discussion #127,
 * ui_input.c scale_leds; 0 in older settings = OFF). Bit 7 free (append-only) */
#define ui_rec_prefs (favorites.factory[15][28])
#define PREF_REC 0xFF00u                       /* (menu_items.c MENU_FLAGS: a bit of ui_rec_prefs, << 8) */
#define PREF_SCALE_LEDS (0x40u << 8)
#define ui_load (favorites.factory[15][26])    /* (JIANT 0.6.30) LOAD (voice.c sys_load): 0 FULL, 1 LITE, 2 ECO; a byte no
                                                * engine uses, 0 in older settings (0.6.29's 6 LITE: 1, LITE) */
#define midi_out_mode (favorites.factory[15][24])   /* (JIANT 0.6.72) MENU > MIDI OUT (seq.c MO_*): 0 KEYS (older settings,
                                                * as ever), 1 CLOCK, 2 ALL, 3 OFF; a byte no engine uses */
#define PREF_SIX (0x80u << 8)                  /* (JIANT 0.6.18) MENU > TRACKS 6: all six tracks play; clear (the
                                                * default): 4, T5 and T6 asleep (voice.c trk_live) */
enum { RP_CLICK, RP_LEVEL, RP_COUNTIN };
static uint32_t rp_get(uint32_t f)                     /* a field as the MENU steps it: 0..2 */
{
    uint32_t v = (ui_rec_prefs >> (2u * f)) & 3u;
    v = v == 3u ? 0u : v;
    return f == RP_LEVEL ? (v == 0u ? 1u : v == 1u ? 0u : 2u) : v;   /* (LEVEL: LOW MID HIGH, MID stored as 0) */
}
static void rp_apply(void)                             /* the settings into click.c / seq.c (every frame: ui_input) */
{
    click_mode = (uint8_t)rp_get(RP_CLICK);
    click_lvl = (uint8_t)rp_get(RP_LEVEL);
    cin_bars = (uint8_t)rp_get(RP_COUNTIN);
    trk_live = (ui_rec_prefs & (PREF_SIX >> 8)) ? NTRK : 4u;
    sys_load = (uint8_t)(ui_load <= LOAD_ECO ? ui_load : LOAD_FULL);   /* (0.6.30) LOAD (voice.c) */
    trks_ui = trk_live == NTRK;                         /* (GLO > SYSTEM's TRACKS and LOAD show them) */
    load_ui = (int16_t)(LOAD_ECO - sys_load);
}
static void trks_put(uint32_t v)                       /* TRACKS: 0 four, 1 six */
{
    ui_rec_prefs = (uint8_t)(v ? ui_rec_prefs | PREF_SIX >> 8 : ui_rec_prefs & ~(PREF_SIX >> 8));
    rp_apply();
}
/* (0.6.30) LOAD as MENU and GLO > SYSTEM step it, the lightest first: 0 ECO, 1 LITE, 2 FULL */
static uint32_t load_get(void) { return LOAD_ECO - (ui_load <= LOAD_ECO ? ui_load : LOAD_FULL); }
static void load_put(uint32_t v)
{
    ui_load = (uint8_t)(LOAD_ECO - (v <= LOAD_ECO ? v : LOAD_ECO));
    rp_apply();
}
static void rp_put(uint32_t f, uint32_t v)
{
    uint32_t st = f == RP_LEVEL ? (v == 1u ? 0u : v == 0u ? 1u : 2u) : v % 3u;
    ui_rec_prefs = (uint8_t)((ui_rec_prefs & ~(3u << (2u * f))) | st << (2u * f));
    rp_apply();
}
/* MENU > STYLE (ui_menu.c): ST_FLAT ST_LINE (gfx.c) in another byte no engine uses, saved with the settings;
 * 0 in older ones = FLAT; 2, the retired PIXEL (1.0.1), reads as LINE; anything else unknown as FLAT (settings_persist.c). gfx.c draws from its copy, ux.style
 * (ui_draw.c style_apply) */
#define ui_style (favorites.factory[15][29])
/* (0.8.9) MENU > DISPLAY > SKIN (ui_menu.c): SKIN_COLORS SKIN_BOTTOM SKIN_HUD (gfx.c) in a byte no engine uses (engine 15,
 * byte 20), saved with the settings; 0 in older ones = COLORS, unknown = COLORS (settings_persist.c). gfx.c draws from its
 * copy, ux.skin (ui_draw.c style_apply) */
#define ui_skin (favorites.factory[15][20])
/* (0.9.7) MENU > DISPLAY > SCOPE (ui_lzedit.c graph_sound_lz): 1 ON, an engine's EDIT pages as before (the creature on the left,
 * the SCOPE card, the chips); 0 OFF (the default: new settings and older ones, which never held it), the creature on the
 * whole panel, centred, the page's and the sound's names in its corners. A byte no engine uses (engine 15) */
#define ui_scope (favorites.factory[15][19])
static void draw_rules(uint32_t y, uint32_t h);       /* (ui_draw.c) */

static uint8_t sync_reload;                  /* engine / preset / project / user preset loaded: editor RELOAD push */

#define ACC T_THEME                /* values, curves */
static uint16_t val_col(uint32_t c);
#define VAL(c) val_col(c)                  /* the knob just turned: accent (0.9.7, flash: a call, was in place) */
/* a parameter's value as 0..1000 (-1: none). (0.9.5) It notes whether the parameter is bipolar (its range across 0: PAN,
 * DETUNE, a MOD amount ...): JIANT's cards heat by the distance from the centre then (ui_draw.c card_heat) */
static struct { int32_t r; uint8_t bip; } ratio_last;
static int32_t ratio_of(int32_t min, int32_t max, int32_t v)
{
    ratio_last.r = max > min ? (v - min) * 1000 / (max - min) : -1;
    ratio_last.bip = (uint8_t)(min < 0 && max > 0);
    return ratio_last.r;
}
static int32_t ratio_d(const param_desc_t *d, int32_t v) { return ratio_of(d->min, d->max, v); }   /* (0.9.7, flash) */
#define RATIO(d, v) ratio_d((d), (int32_t)(v))
/* layout: header 0..24, four cards 28..72 (57 px at x 3 + 59 c),
 * the panel 76..198 (a SURF area; the graphs live in it), footer 202..240; BG between them. (0.8.5) The SEQ pages
 * (LENTE): its level tabs 28..41, the cards 44..88, the panel 92..214, the footer's second row 218..240 */
#define Y_HEAD 0
#define H_HEAD 24
#define Y_LABEL 28                    /* the cards */
#define Y_SEP_END 72
#define CARD_W 57
#define CARD_H 44
#define CARD_X(c) (3 + 59 * (int32_t)(c))
#define Y_GRAPH 76                    /* the panel */
#define H_GRAPH 122
#define Y_FOOT 202
#define H_FOOT 38
/* (0.8.5) the lens pages (LENTE: SEQ, SAVE, FX) laid out as the approved mock-up: the header's rule at 22, the level
 * tabs 26..38, the cards 42..79 (38 tall), the panel 84..223, the footer one line 225..239 (a breadcrumb) */
#define LT_Y 26                       /* the level tabs (ui_draw.c draw_tabs): three, x 4 + 78 l, 76 wide */
#define LT_H 13
#define LZ_CARD_Y 42
#define LZ_CARD_H 38
#define LZ_GRAPH_Y 84
#define LZ_GRAPH_H 140
#define LZ_FOOT_Y 225
#define LZ_FOOT_H 15
#define HEAD_RULE_Y 22                /* (0.8.5) the header's rule (every page): a LINE row in the header's strip */
/* MENU > LARGE (large_kind below): on the value pages the cards are tall (28..132) and the panel a strip
 * (136..198); elsewhere (lists, rolls, layers) the layout above, its card labels in M */
#define LG_CARD_H 104
#define LG_Y_GRAPH 136
#define LG_H_GRAPH 62
#define LZ_LG_Y_GRAPH 150                 /* (0.8.5) .. under a lens's tabs: the tall cards 42..146, the strip 150..212 */
/* (0.8.9) SKIN BOTTOM and HUD (the mock-up's options 3 and 4): the tabs words (26..39), the panel the screen's width and no
 * box (42..189), the knobs a slim row over the breadcrumb (191..223: label, value, a hairline gauge). HUD's HOME (hud_home):
 * the beings over the whole screen, every strip drawn on them (head 0..23, tabs 24..40, the panel 41..190, the knobs
 * 191..223, the breadcrumb 224..239). MENU > LARGE ON keeps COLORS' layout (its tall cards on top) in every skin */
#define LB_TAB_H 14
#define LB_GRAPH_Y 42
#define LB_GRAPH_H 148
#define LB_CARD_Y 191
#define LB_CARD_H 33
/* (0.9.7) SKIN AIR: BOTTOM's strips with the knobs' row on top, under the tabs (42..74), the panel 77..224 (BOTTOM's 148
 * rows); its HOME as HUD's (the beings over the whole screen) with the row 41..73 and the panel 74..223 */
#define LA_CARD_Y 42
#define LA_GRAPH_Y 77
#define LA_HOME_CARD_Y 41
#define LA_HOME_GRAPH_Y 74

/* (0.9.5) the hints a page gives ("OCT+ KEEP  GLO+A4 FREEZE"): only once the page has been dwelt on HINT_DWELL_MS (never
 * while SELECT walks past it), each at most HINT_MAX times ever. The count kept with the settings, a byte a hint in engine
 * 15's spare favourite bytes 0..12 (no engine 15: 0 in older settings, a hint not yet given; 4..12 held the layer buttons'
 * "HOLD [X] QUICK" counts, gone: kept as saved, unused); INIT SYSTEM starts them over */
enum { HK_NONE, HK_MOTION, HK_PUNCH, HK_MUTATE, HK_N };
#define HINT_DWELL_MS 800u
#define HINT_MAX 2u
#define hint_given(k) (favorites.factory[15][(k)])
static struct {
    uint8_t home;
    uint8_t page;                /* index into PAGES */
    uint8_t bank;                /* SEQ: 16-step bank (follows the cursor) */
    uint8_t cursor;              /* SEQ: step being edited (STEP page KNOB 1 moves it) */
    uint8_t entry_open;          /* SEQ: keys held since the first press of this entry */
    uint8_t pr_focus;            /* (0.6.81) SEQ > NOTE: the chord (0) or its note 1..4 (ui_input.c note_edit) */
    uint8_t nmem[13];            /* (0.9.2) each section's page last shown, its chain's entry ^ its default (ui_lens.c
                                  * nav_mem: EDIT one per kind of track) */
    uint8_t fxmod;               /* (0.8.5) FX: the effect last shown (BUSES' tile lit) */
    uint8_t hlv, hset[2];        /* .. HOME: ECO (0) or BEING (1), each one's page (HOME / MACROS, HOME / ROUTES) */
    uint8_t focus;               /* .. EDIT > FOCUS: the parameter (E1..E8: 0..7) */
    uint8_t stage;               /* .. ENV > STAGE: the stage (ATK DEC SUS REL: 0..3) */
    uint8_t ent_len, ent_arp, ent_res, ent_ord, ent_oct, ent_nn;   /* (0.6.37) that entry while held (ui_input.c
                                  * seq_entry): KNOB 1 its LEN in steps; ARP pressed: arpeggiated, KNOB 1 the steps a
                                  * note (RES), 2 the ORDER, 3 the octaves; the keys' notes in order of pitch */
    uint8_t ent_notes[8];
    uint8_t comp_sel;            /* (0.6.39) SEQ > COMP: the TYPE shown (COMP_*); comp_at: the step + 1 (+ the track x 64) */
    uint16_t comp_at;
    uint8_t lane;                /* SEQ > STEP on a DRUM track (the grid): the lane the keys and KNOB 3 / 4 edit */
    uint8_t plk_src;             /* parameter locks (ui_input.c lock_*): the sound page whose KNOB 1..4 a step held
                                  * locks, PAGES index + 1; 0 = HOME's four */
    uint8_t plk_lane, plk_trk;   /* .. the grid: the lane and the track of the hits plk_rm takes away */
    uint64_t plk_rm, plk_acc;    /* .. the grid's step keys pressed on a hit (bit = step): it goes when the key is let
                                  * go, unless the key was held for a lock (its accent: plk_acc) */
    uint8_t hot_col, hot_t;      /* column whose knob was just turned (drawn white) */
    uint8_t menu;                /* 0 off, 1 list, 2 about + credits (HOME held) */
    uint8_t menu_sel;            /* MENU: the row (menu_items.c MI_*; its tab MI_TAB), kept while the device runs */
    uint8_t mlv;                 /* (0.8.5) MENU's view: 1 TAB (its rows; 0.9.2: always, 0 MENU and 2 ROW for the renders) */
    uint8_t lylv;                /* (0.8.5) a held layer's lens: 0 MAP, 1 KNOBS, 2 STATE (ALGORITHM while held) */
    uint16_t menu_scroll;        /* continuous ABOUT + CREDITS position, pixels */
    uint32_t menu_sig, home_t0;  /* HOME press time (btn_hold) */
    uint8_t force;               /* full redraw pending */
    uint8_t msg_t;               /* transient message frames */
    uint8_t bpm_t;               /* frames the BPM stays highlighted after a SELECT turn */
    uint8_t act;                 /* action pages: the column whose action OCT+ does, + 1; 0 = none (act_col) */
    uint32_t rec_t0;             /* REC press time: a tap arms (btn_hold, no hold: held, REC opens its layer) */
    uint32_t seq_t0;             /* SEQ held: direct SONG entry */
    uint32_t save_t0;            /* SAVE press time (btn_hold: held = UNDO) */
    uint8_t confirm;             /* the OCT- / OCT+ dialog: CF_*, 0 = none */
    uint8_t confirm_trk;         /* the track it clears, the slot it overwrites */
    uint8_t uslot;               /* SAVE > USER: the selected user preset slot */
    uint8_t ppick;               /* SEQ > PATTERNS: the pattern picked (pat_count list index) */
    uint8_t song_row;            /* SONG: row selected, count selects the next empty row */
    uint8_t ev_row;              /* SEQ > AUTO LIST (ui_input.c ev_*): the row selected (the track's count: + ADD LOCK) */
    uint8_t ev_step, ev_id;      /* .. + ADD LOCK: the step and the parameter a lock is added on */
    uint8_t uboot;               /* main.c: seconds left before UPDATE MODE (OCT- + OCT+ held), 0 = none */
    uint32_t ly_t0;              /* the layer button's press time | 1, LY_* bits (ui_layer.c layer_gesture) */
    uint8_t ly;                  /* the layer whose button is down (LAYER_*), 0 = none */
    uint8_t layer;               /* the layer whose map is shown (LAYER_*), 0 = none */
    uint16_t pg_down;            /* page buttons down (panel ids) that act when let go */
    uint32_t layer_sig;          /* drawn-state cache of the map */
    char msg[40];                /* (0.9.0: 40, was 24 and cut the long ones silently; longer: ellipsised, ui_say) */
    char msg2[40];               /* a second message, shown when the first is over */
    uint8_t hint_pg;             /* (0.9.0) the page (+ 1, HOME 255) a page's hint (MUTATE's) was given on: gone
                                  * on another (ui_draw_page) */
    uint8_t hint_k;              /* (0.9.5) a hint waiting for its page to be dwelt on (HK_*; 0 none: ui_layer.c hint_tick) */
    uint8_t hint_at;             /* .. the page it is for (+ 1, HOME 255) */
    uint32_t hint_ms;            /* .. shown since */
    uint32_t enc_t[NE];
    /* drawn-state cache */
    char col[4][56];   /* (0.6.10: was 32, a key up to 51 lost its colour / gauge / icon tail) */
    uint32_t graph_sig, head_sig, foot_sig, frame;
    /* rolling digits (ui_draw.c roll_*): the four card values and the header BPM */
    struct {
        char from[7];            /* the value rolling out; "" = idle */
        uint8_t t0;              /* (ui.frame) of the value's last change */
        int8_t dir;              /* +1: it went up (the old digit leaves upward), -1: down */
        uint8_t sig;             /* a card: what its value is of (label, unit, track, engine, palette) */
    } roll[5];
    int16_t roll_bpm;            /* the BPM last drawn */
    uint32_t knob_ms;            /* (0.8.9) when KNOB 1..4 last turned (SKIN HUD: the knob row fades while they rest) */
    uint32_t nav_sig, nav_at;    /* (0.9.2) ui_lens.c nav_cur's cache: what it last resolved, the entry */
} ui;
static uint16_t val_col(uint32_t c) { return c == ui.hot_col && ui.hot_t ? T_ACCENT : T_THEME; }

enum { CF_NONE, CF_CLEAR_SEQ, CF_CLEAR_TRK, CF_OVR_PROJ, CF_OVR_USER, CF_LOAD_PAT,
       CF_DEL_ROW, CF_CLEAR_SONG, CF_INIT_SOUND, CF_CLEAR_MOTION, CF_ERASE_USER, CF_CLEAR_PFX,
       CF_RECALL_SOUND, CF_CLEAR_ALL, CF_DICE_ALL, CF_INIT_SYS, CF_DICE_SOUND, CF_DICE_SEQ, CF_DICE_LFO,
       CF_DICE_FX, CF_CPU_TEST, CF_MUT_BACK };   /* ui.confirm: TOOLS' clears; (0.6.21) HOME + SELECT:
                                   * everything diced; (0.6.65) EDIT + SELECT: keep the diced sound?; (0.6.67) SEQ / LFO / FX + SELECT:
                                   * keep the diced sequence / LFO / effects?;
                                   * SAVE over a used slot; a pattern over the user's steps;
                                   * USER ERASE; (0.9.3) MUTATE turned OFF: BACK TO BEFORE MUTATE? */

/* MENU > DISPLAY > SCREEN OFF (1.1.5): after a while with no input on the panel the screen goes dark (the backlight
 * stays on: lcd.c lcd_power)
 * (a picture held for hours can stay faintly on the panel); the sound, the sequencer, MIDI and USB go on, the LEDs
 * stay as they are. NEVER (the default since 1.1.5.1), 5, 15, 30, 60 minutes, kept in a byte no engine uses, saved
 * with the settings (SCR_CODE). 1.1.5 stored value ^ 3, 0 (its default and every older setting) being 30 MIN; 1.1.5.1
 * reads that 0 as NEVER (the new default: the panel went dark for good on 1.1.5, see lcd.c lcd_power) and keeps the
 * other codes, 30 MIN now 4. Unknown codes read as NEVER.
 * Input is the panel's only: a button or key held or pressed, a knob turned (not MIDI, the editor or MASTER, an
 * analogue pot). The press, key or turn that wakes it is swallowed: no note (seq.c kb_asleep), no button, no
 * value, until everything is let go and the knobs rest for SCR_EAT_MS. A dialog, the count-in and the UPDATE MODE
 * countdown keep it on (and wake it); the crash screen, UPDATE and UBOOT wake it at once (lcd_wake_now).
 * Off: lcd_power(0) (DISPOFF, SLPIN) and nothing is drawn. Waking, a frame at a time (never a wait in
 * the loop): 120 ms after the SLPIN, SLPOUT (lcd_power(1)); 120 ms after that one frame redraws everything
 * (ui.force), then DISPON (lcd_power(2)) */
#define ui_scr (favorites.factory[15][27])
#define SCR_DEF 0u                                     /* NEVER (1.1.5.1; 1.1.5: 30 MIN) */
static const uint8_t SCR_CODE[5] = {0, 2, 1, 4, 7};    /* the stored byte of NEVER 5 15 30 60 MIN (1.1.5: 3 2 1 0 7) */
static const uint8_t SCR_DEC[8] = {0, 2, 1, 0, 3, 0, 0, 4};   /* .. and back (3: 1.1.5's NEVER; 5, 6 unknown) */
#define SCR_EAT_MS 300u
static const uint8_t SCR_MIN[5] = {0, 5, 15, 30, 60};  /* NEVER, minutes */
enum { SCR_ON, SCR_OFF, SCR_WAKE, SCR_SLPOUT, SCR_SHOW };
static struct {
    uint8_t st;                  /* SCR_*: SCR_WAKE asked, SCR_SLPOUT sent, SCR_SHOW the frame that redraws */
    uint8_t eat;                 /* the wake gesture: panel input swallowed (ui_input) */
    uint32_t idle;               /* fm1_ms of the last panel input (or blocker) */
    uint32_t t;                  /* fm1_ms of the last panel command (SLPIN, SLPOUT) */
    uint32_t eat_t;              /* fm1_ms of the last swallowed knob detent */
} scrn;
static uint32_t scr_get(void)                          /* the MENU's value: 0 NEVER .. 4 60 MIN */
{
    return ui_scr < NELEM(SCR_DEC) ? SCR_DEC[ui_scr] : SCR_DEF;
}
static void scr_put(uint32_t v)                        /* (the time counts from the change: the editor's too) */
{
    ui_scr = SCR_CODE[v < NELEM(SCR_MIN) ? v : SCR_DEF];
    scrn.idle = fm1_ms;
}
static void scr_wake(void)
{
    if (scrn.st == SCR_OFF)
        scrn.st = SCR_WAKE;
    scrn.eat = 1;
}
static void scr_wake_now(void)                         /* main.c: the crash screen, UPDATE, UBOOT */
{
    if (scrn.st != SCR_ON)
        lcd_wake_now();
    scrn.st = SCR_ON;
    scrn.idle = fm1_ms;
}
/* ui_input, every pass: 1 = the pass is swallowed (the screen dark or waking, or the wake gesture going on) */
static int scr_input(uint32_t pressed, uint32_t notes)
{
    uint32_t held = fm1_in.buttons | fm1_in.notes, k;
    int32_t moved = 0;
    if (pressed | notes | held | panel_moved)
        scrn.idle = fm1_ms;
    panel_moved = 0;
    if (scrn.st == SCR_ON && !scrn.eat)
        return 0;
    for (k = 0; k < NE; k++)                           /* (every knob's turns go) */
        moved |= panel_enc(k);
    panel_moved = 0;
    if (moved)
        scrn.eat_t = scrn.idle = fm1_ms;
    if (pressed | notes | held | (uint32_t)moved)
        scr_wake();
    if (scrn.st != SCR_ON || held || (int32_t)(fm1_ms - scrn.eat_t) < (int32_t)SCR_EAT_MS)
        return 1;
    scrn.eat = 0;                                       /* all let go, the knobs at rest: input again */
    kb_asleep = 0;
    return 0;
}
/* ui_draw, every frame: 1 = draw it */
static int scr_frame(void)
{
    uint32_t lim = SCR_MIN[scr_get()] * 60000u;
    if (ui.confirm || ui.uboot || seq_counting()) {    /* (never dark under these) */
        scrn.idle = fm1_ms;
        if (scrn.st == SCR_OFF)
            scr_wake();
    }
    switch (scrn.st) {
    case SCR_ON:
        if (!lim || (int32_t)(fm1_ms - scrn.idle) < (int32_t)lim)   /* (signed: robust to a wrap) */
            return 1;
        lcd_power(0);
        scrn.st = SCR_OFF;
        scrn.t = fm1_ms;
        scrn.eat = 1;
        kb_asleep = 1;
        break;
    case SCR_WAKE:
        if (fm1_ms - scrn.t >= 120u) {
            lcd_power(1);
            scrn.st = SCR_SLPOUT;
            scrn.t = fm1_ms;
        }
        break;
    case SCR_SLPOUT:
        if (fm1_ms - scrn.t >= 120u) {
            scrn.st = SCR_SHOW;
            ui.force = 1;
            return 1;
        }
        break;
    case SCR_SHOW:
        return 1;
    }
    ui.msg_t = 0;                                      /* (dark: what would have shown is over; a clean page back) */
    ui.msg2[0] = 0;
    ui.hot_t = ui.bpm_t = 0;
    return 0;
}
static void scr_shown(void)                            /* after the frame: the redrawn page on */
{
    if (scrn.st != SCR_SHOW)
        return;
    lcd_power(2);
    scrn.st = SCR_ON;
    scrn.idle = fm1_ms;
}

static const page_t *page_over;   /* a quick layer's own four knobs (ui_layer.c), while it edits or draws them */
static const page_t *cur_page(void) { return page_over ? page_over : &PAGES[ui.page]; }

/* MENU > LARGE (#15, Discussion #80: what KNOB 1..4 do, in bigger type). Per page type:
 *   LK_TALL  HOME and the value pages (EDIT ENV LFO MOD FX SLICER SCL SHIFT ARP VOICE PATTERN STEP's knobs, MIXER,
 *            GLOBAL, SYSTEM, TOOLS, MOTION): tall cards (a K1..K4 keycap, the icon, the label in M, the value in L,
 *            in M or S when L is too wide or lacks a glyph, the unit under it, the gauge), the panel a strip: HOME's
 *            scope, ENV's ADSR, LFO's wave, PATTERN's 64 steps, MIXER's four tracks (name, state, meter) small; the
 *            page's title and number in L on the others (their charts cannot be read that small);
 *   LK_LABEL the list and graph pages (PRESETS, USER, PROJECT, PATTERNS, SONG, the piano roll and the drum grid,
 *            CHANCE) and the quick layers' maps: the layout as it is, the card labels in M;
 *   LK_OFF   LARGE off; the menu (its own LARGE: ui_menu.c), the dialogs and NAME keep their own layout in every case. */
enum { LK_OFF, LK_LABEL, LK_TALL };
static int lens_on(void);
static uint32_t lz_on(void);
static uint32_t lz_fam(void);
static int lz_mock_cards(void);
static int name_on(void);                       /* (ui_name.c) */
static uint32_t large_kind(void)
{
    uint32_t g;
    if (!(ui_prefs & PREF_LARGE))
        return LK_OFF;
    if (ui.layer || lz_mock_cards() || ui.menu || name_on())   /* (0.8.5: the menu's lens too; the NAME screen, its own
                                                         * geometry: from any page) */                    /* (0.8.5: SEQ, SAVE, FX's lenses keep the mock-up's cards, M labels;
                                                         * HOME and the other lenses: tall cards under their tabs) */
        return LK_LABEL;
    if (ui.home)
        return LK_TALL;
    g = cur_page()->graph;
    return g == GR_BROWSE || g == GR_SLOTS || g == GR_USER || g == GR_PATS || g == GR_SONG || g == GR_ROLL ||
           g == GR_CHANCE || g == GR_COMP || g == GR_EVENTS || g == GR_LYRIC ? LK_LABEL : LK_TALL;
}
/* the geometry of the page shown: the cards' height, the panel's top and height. (0.8.5) A lens page (lz_on): the
 * mock-up's (LZ_*), under its level tabs; a layer's map over it: the usual layout */
/* (0.8.9) a lens page in SKIN BOTTOM / HUD's layout (LARGE: COLORS'); HUD's HOME (ECO): the beings full screen */
static int lz_low(void) { return ux.skin && lz_on() && !(ui_prefs & PREF_LARGE); }
static int hud_home(void);
static uint32_t card_y(void)
{
    return lz_low() ? (ux.skin == SKIN_AIR ? (hud_home() ? LA_HOME_CARD_Y : LA_CARD_Y) : LB_CARD_Y) : lz_on() ? LZ_CARD_Y : Y_LABEL;
}
static uint32_t foot_y(void) { return hud_home() ? LZ_FOOT_Y - 1u : lz_on() ? LZ_FOOT_Y : Y_FOOT; }
static uint32_t foot_h(void) { return hud_home() ? LZ_FOOT_H + 1u : lz_on() ? LZ_FOOT_H : H_FOOT; }
static uint32_t card_h(void) { return large_kind() == LK_TALL ? LG_CARD_H : lz_low() ? LB_CARD_H : lz_on() ? LZ_CARD_H : CARD_H; }
static uint32_t graph_y(void)
{
    int air = ux.skin == SKIN_AIR;
    return large_kind() == LK_TALL ? (lz_on() ? LZ_LG_Y_GRAPH : LG_Y_GRAPH) : hud_home() ? (air ? LA_HOME_GRAPH_Y : LB_GRAPH_Y - 1u) :
           lz_low() ? (air ? LA_GRAPH_Y : LB_GRAPH_Y) : lz_on() ? LZ_GRAPH_Y : Y_GRAPH;
}
static uint32_t graph_h(void)
{
    return large_kind() == LK_TALL ? LG_H_GRAPH : hud_home() ? LB_CARD_Y - LB_GRAPH_Y + 1u : lz_low() ? LB_GRAPH_H :
           lz_on() ? LZ_GRAPH_H : H_GRAPH;
}
static int large_face_has(const char *s)                /* every glyph of s in the sparse L face */
{
    for (; *s; s++)
        if (glyph_at(&AF_L, fold(&AF_L, (uint8_t)*s)) < 0)
            return 0;
    return 1;
}
static int32_t ink_w(const aafont_t *f, const char *s)
{
    int32_t b[4];
    text_ink(f, s, b);
    return b[2] > b[0] ? b[2] - b[0] : 0;
}

/* the quick layers (ui_layer.c): a button held, the keys and KNOB 1..4 are its shortcuts, its map over the page */
enum { LAYER_NONE, LAYER_FX, LAYER_GLO, LAYER_SCL, LAYER_EDIT, LAYER_SEQ, LAYER_REC, LAYER_SAVE, LAYER_MACRO, LAYER_N };
static void draw_layer(void);
static const char *layer_head(void);
static uint32_t layer_leds(uint32_t *br);
static uint32_t layer_btn(void);

/* FM operator pages belong to DIGITAL; they never appear on other instruments (without FELUCCA_FM4: never). LANES /
 * LANES 2 a DRUM track's (its lane levels); SOUND / SOUND 2 a DRUM track's (DRUM-X: ui_dx.c) */
static int page_visible(uint32_t i)
{
    if (PAGES[i].graph == GR_SONG && PAGES[i].fam == FAM_SEQ)
        return 0;                                       /* (0.6.24) the song in one place: SAVE > ARRANGE (SEQ > SONG
                                                         * was the same page twice; kept in the table: its index) */
    if (sys_load == LOAD_ECO && ((PAGES[i].graph == GR_SLCR || PAGES[i].graph == GR_SLPIT) ||
                                 (PAGES[i].fam == FAM_FX && PAGES[i].id[0] == G_CRATE)))
        return 0;                                       /* (0.6.30) LOAD ECO: no SLICER, no chorus: their pages gone */
    if (PAGES[i].graph == GR_MORPH) {                   /* (0.6.45) MORPH: ANALOG's plain waves, PHASE, LOFI's (not BYTE FLOAT) */
        uint32_t e = trk_eng(TSEL);
        return e == ENGI_ANALOG || ENGINES[e] == &ENG_PHASE ||   /* (0.6.81: every ANALOG WAVE) */
               (ENGINES[e] == &ENG_LOFI && TSEL->p[P_E1] < RW_BYTE);
    }
    if (PAGES[i].graph == GR_LYRIC)                     /* (0.8.5) WORDS: VOICE's */
        return trk_engp(TSEL) == &ENG_FORMANT;
    if (PAGES[i].graph == GR_PATS || PAGES[i].graph == GR_CHANCE)   /* (0.6.37) PHRASES, a step's CHANCE: out of the */
        return !ui.home && ui.page == i;               /* SEQ pages' cycle (only where code put the screen) */
    if (PAGES[i].graph == GR_DXSND || PAGES[i].graph == GR_DXIMG)   /* (0.6.48: IMAGE) */
        return trk_eng(TSEL) == ENGI_DRUM;
    if (PAGES[i].scope == SC_TRACK && PAGES[i].id[0] >= P_LN0 && PAGES[i].id[0] <= P_LN7)
        return trk_eng(TSEL) == ENGI_DRUM;
    if (PAGES[i].graph == GR_FOCUS)                     /* (0.8.5) FOCUS: a synth's (FM6: OPERATOR, DRUM: SOUND) */
        return trk_eng(TSEL) != ENGI_DRUM && trk_eng(TSEL) != ENGI_FM6 &&
               !(FELUCCA_FM4 && trk_eng(TSEL) == ENGI_DIGITAL);
    if (PAGES[i].id[0] == P_ALLOC)                      /* (0.8.5) VOICE 2: not a kit's */
        return trk_eng(TSEL) != ENGI_DRUM;
    if (PAGES[i].graph >= GR_FMOP && PAGES[i].graph <= GR_FMENV)   /* (0.6.10) OPERATOR, OP ENV, OPERATOR 2: FM6's */
        return trk_eng(TSEL) == ENGI_FM6;
    if (PAGES[i].fam == FAM_EDIT && PAGES[i].id[0] == P_WRDEP)   /* (0.7.8) ROTOR: WHEEL's */
        return trk_engp(TSEL) == &ENG_WHEEL;
    if (PAGES[i].id[0] == P_LRTRG && PAGES[i].id[1] == 0xFFu)   /* (0.6.12) RETRIG: LOFI BYTE / FLOAT's */
        return trk_engp(TSEL) == &ENG_LOFI && TSEL->p[P_E1] >= RW_BYTE;
    if (PAGES[i].scope == SC_TRACK && PAGES[i].id[0] == P_FTYPE)   /* (JIANT 0.5) FILTER: ANALOG's, or the part's */
        return (trk_eng(TSEL) == ENGI_ANALOG) == (PAGES[i].id[1] == P_E4);
    return !(PAGES[i].fam == FAM_EDIT && PAGES[i].id[0] >= P_FM1_ATK &&
             PAGES[i].id[0] <= P_FM4_LEVEL) || (FELUCCA_FM4 && trk_eng(TSEL) == ENGI_DIGITAL);
}

static uint32_t page_first(uint32_t fam)
{
    uint32_t i;
    for (i = 0; i < NPAGES; i++)
        if (PAGES[i].fam == fam)
            return i;
    return 0;
}

/* (0.9.2) the sections' pages (ui_lens.c: the chains, the six rules). The 0.8.5 lenses' pictures stay, each page drawn
 * for the level it was on (lens_level, save_level, fx_level, pd_level, home_level):
 *   SEQ   PATTERN's bars (PATTERN STRETCH TRACK, the new BAR), the roll (STEP NOTE AUTOMATION AUTO LIST), the step
 *         large (COMP COMP 2); PHRASES (PRESETS' pattern loader) and SONG read as PATTERN, CHANCE as STEP
 *   SAVE  LIBRARY (PROJECTS TOOLS), PROJECT (SECTIONS ARRANGE: the PROJECT page's KNOB 1 the section), SOUND (USER PRESETS)
 *   FX    TRACK (SENDS DIST AUTO PAN), BUSES, the effects (ui.fxmod: the module shown last, BUSES' tile lit) */
static int lens_on(void) { return !ui.home && !page_over && PAGES[ui.page].fam == FAM_SEQ; }
static int save_on(void) { return !ui.home && !page_over && PAGES[ui.page].fam == FAM_SAVE; }
static void page_entered(void);
static void page_flash(void);
#define FX_NMOD 6u
static const char *const FX_MOD[FX_NMOD] = {"DELAY", "REVERB", "CHORUS", "SLICER", "MASTER", "PUNCH"};
static const char *const FX_MHEAD[FX_NMOD] = {"DLY", "REVERB", "CHORUS", "SLICER", "MASTER", "PUNCH"};   /* its first page */
static const uint8_t FX_MN[FX_NMOD] = {3, 2, 1, 4, 1, 1};   /* its pages, one after another in PAGES */
static int page_visible(uint32_t i);
static int fx_on(void) { return !ui.home && !page_over && PAGES[ui.page].fam == FAM_FX; }
static uint32_t fx_page(const char *title)
{
    uint32_t i;
    for (i = 0; i < NPAGES; i++)
        if (PAGES[i].fam == FAM_FX && str_eq(PAGES[i].title, title))
            return i;
    return 0;
}
static int32_t fx_trk_k(uint32_t i)                     /* page i's place among TRACK's pages, -1 none */
{
    static const char *const FX_TRK[3] = {"FX", "DIST", "AUTO PAN"};
    uint32_t k;
    for (k = 0; k < 3u; k++)
        if (PAGES[i].fam == FAM_FX && str_eq(PAGES[i].title, FX_TRK[k]))
            return (int32_t)k;
    return -1;
}
static uint32_t fx_mod_of(uint32_t i)                   /* an effect page's module (another page: ui.fxmod) */
{
    uint32_t m, f;
    for (m = 0; m < FX_NMOD; m++) {
        f = fx_page(FX_MHEAD[m]);
        if (i >= f && i < f + FX_MN[m])
            return m;
    }
    return ui.fxmod % FX_NMOD;
}
static uint32_t fx_mod_k(uint32_t i) { return i - fx_page(FX_MHEAD[fx_mod_of(i)]); }   /* .. its place in it */
static uint32_t fx_mod_n(uint32_t m) { return FX_MN[m % FX_NMOD]; }
static int fx_mod_ok(uint32_t m) { return page_visible(fx_page(FX_MHEAD[m % FX_NMOD])); }   /* (LOAD ECO: no CHORUS, SLICER) */

/* (0.8.5) a lens page shown (every section's, the menu's, a layer's map): the mock-up's layout, its tabs and breadcrumb */
static int name_on(void);                               /* (ui_name.c: NAME keeps its own layout) */
#include "ui_lens.c"
static uint32_t fx_level(void)
{
    if (fx_trk_k(ui.page) >= 0)
        return FXL_TRACK;
    return PAGES[ui.page].graph == GR_BUSES ? FXL_BUSES : FXL_EFFECT;
}
static uint32_t lens_level(void)                        /* SEQ: the level the page is drawn as */
{
    uint32_t g = PAGES[ui.page].graph;
    return g == GR_PATS || g == GR_SONG ? LV_PAT : g == GR_CHANCE ? LV_STEP : nav_lv();
}
static int home_lz(void) { return (ui.home && !ui.menu && !dlg_on()) || gen_on(); }   /* (GEN: HOME's) */
static uint32_t lz_on(void) { return ((lens_on() || save_on() || fx_on() || pd_on() || home_lz()) && !ui.layer && !name_on()) || ui.menu == 1u || (ui.layer && !ui.menu); }
static uint32_t lz_fam(void)                            /* which lens */
{
    if (ui.menu == 1u)                                  /* (0.8.5) the menu (ui_menu.c) */
        return LZ_MENU;
    if (ui.layer)                                       /* .. a layer held: MAP . KNOBS . STATE (ui_layer.c) */
        return LZ_LAYER;
    if (gen_on() || ui.home)
        return LZ_HOME;
    if (pd_on())
        return LZ_EDIT + pd_cur();
    return save_on() ? LZ_SAVE : fx_on() ? LZ_FX : LZ_SEQ;
}
static int lz_mock_cards(void) { return lz_on() && lz_fam() <= LZ_FX; }
/* (MENU > LARGE: their cards as they are; 0.9.0: not under the count-in or UPDATE MODE's countdown, which cover the page:
 * their header on BG, as every other page's) */
static int hud_home(void)
{
    return skin_hud() && !ui.uboot && !seq_counting() && lz_low() && lz_fam() == LZ_HOME && home_level() == 0u;   /* (0.9.7 AIR too) */
}
/* the tab lit: the section's group shown, the menu's tab, a layer's view */
static uint32_t menu_tab(void);
static uint32_t lz_level(void)
{
    uint8_t cnt[8], gi[8];
    uint32_t f = lz_fam(), g, p, i, m;
    if (f == LZ_MENU)
        return menu_tab();
    if (f == LZ_LAYER)
        return ui.lylv % 3u;
    (void)nav_groups(cnt, gi, &g, &p, &i, &m);
    return g;
}

/* transient message in the top bar, right of the transport and the BPM: a + b */
static void ui_say(const char *a, const char *b)
{
    uint32_t n;
    str_cpy(ui.msg, a, sizeof ui.msg);
    n = str_len(ui.msg);
    str_cpy(ui.msg + n, b, sizeof ui.msg - n);
    if (str_len(a) + str_len(b) >= sizeof ui.msg)       /* (0.9.0) too long for it: the end an ellipsis, never cut silently */
        ui.msg[sizeof ui.msg - 2u] = ELLIPSIS;
    ui.msg_t = 40;
    ui.msg2[0] = 0;
    ui.hint_pg = 0;
}

static void ui_message(const char *s) { ui_say(s, ""); }
/* a message led by an icon: its first byte (ui_draw.c draw_head draws the icon, then the words) */
#define MSG_NOFILE "\x01"                      /* the no-file icon (ICON_X_NOFILE) */

static int chain_busy(void) { return chain.running || chain.armed; }
/* (0.6.22) ARRANGE: the rows 0..count-1, + ADD (while there is room), then the song's END row */
static uint32_t song_end_row(void) { return chain_config.count < CHAIN_ROWS ? chain_config.count + 1u : CHAIN_ROWS; }
/* Main loop only, with interrupts enabled. PLAY may be consumed between reads;
 * take one coherent snapshot before a flash operation or song preparation. */
static int transport_busy(void)
{
    int busy;
    fm1_irq_off();
    busy = song.playing || chain_busy() || transport_req == 1u || seq_counting();   /* (a count-in: as playing) */
    fm1_irq_on();
    return busy;
}
static void chain_play_ui(void)
{
    uint32_t rc = chain_prepare();
    if (!rc) {
        ui.force = 1;
    } else if (rc >= 3u) {
        char b[8] = "A EMPTY";
        b[0] = (char)('A' + rc - 3u);
        ui_say("PATTERN ", b);
    } else {
        ui_message(rc == 1u ? "ADD A SONG ROW" : "STOP FIRST");
    }
}


/* parameter locks: a page whose KNOB 1..4 can lock (a track's sound: one of its parameters can be motion; not MOD's
 * slots), the parameter KNOB k locks (ui.plk_src's page, HOME's four), 0xFF = none */
static int lock_page_ok(uint32_t i)
{
    uint32_t k;
    if (i >= NPAGES || (PAGES[i].scope != SC_TRACK && PAGES[i].scope != SC_ENGINE) || PAGES[i].graph == GR_MOD ||
        PAGES[i].graph == GR_MSEQ)
        return 0;
    for (k = 0; k < 4u; k++)
        if (PAGES[i].id[k] != 0xFFu && motion_param(PAGES[i].id[k]))
            return 1;
    return 0;
}
static uint32_t lock_id(uint32_t k)
{
    uint32_t id = ui.plk_src && lock_page_ok(ui.plk_src - 1u) ? PAGES[ui.plk_src - 1u].id[k & 3u]
                                                               : trk_engp(TSEL)->knob[k & 3u];
    return id < P_COUNT && motion_param(id) ? id : 0xFFu;
}

static void ev_enter(void);
static void page_entered(void)
{
    const page_t *pg = cur_page();
    ui.knob_ms = fm1_ms;                         /* (0.9.0) HUD: the knob row whole again on a new page */
    if (!ui.home && pg->fam == FAM_FX && fx_trk_k(ui.page) < 0 && pg->graph != GR_BUSES)
        ui.fxmod = (uint8_t)fx_mod_of(ui.page);  /* (FX: the effect shown, BUSES' tile lit) */
    if (!ui.home && pg->graph == GR_EVENTS)      /* AUTO LIST: + ADD LOCK on the SEQ cursor's step */
        ev_enter();
    if (!ui.home && lock_page_ok(ui.page))
        ui.plk_src = (uint8_t)(ui.page + 1u);
    song.seq_mode = !ui.home && pg->fam == FAM_SEQ;
    ui.hint_k = ui.home ? HK_NONE : pg->graph == GR_MOTION ? HK_MOTION : pg->graph == GR_PUNCH ? HK_PUNCH :
                pg->graph == GR_MUTATE ? HK_MUTATE : HK_NONE;   /* (0.9.5: its hint once dwelt on: ui_layer.c hint_tick) */
    ui.hint_at = (uint8_t)(ui.home ? 255u : ui.page + 1u);
    ui.hint_ms = fm1_ms;
    ui.entry_open = 0;
    ui.hot_t = 0;                                /* clear the previous page's emphasis */
    ui.act = pg->graph == GR_USER ? 4u : 0u;     /* the save screen is ready for OCT+ */
    ui.force = 1;
}

static int step_on(const step_t *st) { return st->time == ST_NOTE && (st->n || st->hit); }
/* a note's length: step c and the TIEs after it, round past LEN's end as the sequencer plays them (0.8.4: here from
 * ui_input.c, the TIME card and LENTE's STEP show it) */
static uint32_t note_len(const track_t *t, uint32_t c, uint32_t len)
{
    uint32_t l = 1;
    while (l < len && t->step[(c + l) % len].time == ST_TIE)
        l++;
    return l;
}

enum { COMP_OFF, COMP_CHANCE, COMP_RATCH, COMP_ARPN, COMP_ARPQ, COMP_ARPS, COMP_TRIG, COMP_RAND, COMP_OCT, COMP_TONAL,
       COMP_BEND, COMP_ECHO, COMP_PULSE, COMP_JUMP, COMP_REV, COMP_FILL, COMP_KILL, COMP_WALK, COMP_CLIMB, COMP_TOUCH, COMP_N };   /* (CP_* in order) */
static const char *const EV_NAMES[EV_N] = {"ALWAYS", "1:2", "2:2", "SKIP", "1:3", "2:3", "3:3", "1:4", "2:4", "3:4", "4:4",
                                           "1:8", "FIRST", "!FIRST"};   /* (0.6.92) EVERY */
/* (0.6.92) a part of the step as a note value: n parts a step of the track's DIV (1/16: x2 1/32, x3 1/32T, x4 1/64 ..;
 * a triplet or a bar's DIV, or an odd count: "xn") */
static void rate_name(const track_t *t, uint32_t n, char *b)
{
    static const uint8_t BASE[10] = {4, 8, 16, 32, 0, 0, 2, 1, 0, 0};   /* N_DIV: 1/4 1/8 1/16 1/32 8T 16T 1/2 1/1 2BAR 4BAR */
    uint32_t d = (uint32_t)clamp(t->p[P_SDIV], 0, 9), base = BASE[d], v = 0;
    const char *tr = "";
    if (n == 1u && base) { str_cpy(b, N_DIV[d], 8); return; }
    if (base && (n == 2u || n == 4u || n == 8u)) v = base * n;
    else if (base && (n == 3u || n == 6u)) { v = base * n * 2u / 3u; tr = "T"; }
    if (!v) { b[0] = 'x'; fmt_int(b + 1, (int32_t)n); return; }
    b[0] = '1'; b[1] = '/'; fmt_int(b + 2, (int32_t)v); str_cpy(b + str_len(b), tr, 2);
}
static const char *const COMP_NAMES[COMP_N] = {"OFF", "CHANCE", "RATCH", "NOTES", "SEQ", "SCALE", "TRIG", "RAND", "OCT",
                                               "TONAL", "BEND", "ECHO", "PULSE", "JUMP", "REV", "FILL", "KILL", "WALK", "CLIMB",
                                               "TOUCH"};
/* (0.6.82) the COMP pages one after another: COMP (the step's first component), COMP 2 (its second), WHEN (theirs, and
 * the step's LEN): which this is (0..2) */
static uint32_t comp_slot(void)
{
    uint32_t i = ui.page, k = 0;
    while (i > 0u && k < 2u && PAGES[i - 1u].graph == GR_COMP)
        i--, k++;
    return k;
}
static uint32_t comp_type(const step_t *s) { return comp_slot() == 1u ? s->comp2 : s->comp; }   /* (the page's) */
static uint32_t comp_arg(const step_t *s) { return comp_slot() == 1u ? s->carg2 : s->carg; }
static uint32_t comp_view1(void);
static uint32_t comp_view(void)                         /* the TYPE shown for the cursor step: COMP 2's its second */
{
    const step_t *s = &TSEL->step[ui.cursor];
    if (comp_slot() == 1u)
        return s->comp2 ? COMP_ARPN + s->comp2 - CP_ARPN : COMP_OFF;
    return comp_view1();
}
static uint32_t comp_view1(void)                        /* the first's, CHANCE and RATCH (read once a step) */
{
    const step_t *s = &TSEL->step[ui.cursor];
    uint32_t at = (uint32_t)ui.cursor + 1u + (uint32_t)song.sel * 64u;
    if (ui.comp_at != at || ui.comp_sel >= COMP_N) {
        ui.comp_at = (uint16_t)at;
        ui.comp_sel = (uint8_t)(s->comp ? COMP_ARPN + s->comp - CP_ARPN : step_chance(s) < 100u ? COMP_CHANCE :
                                step_ratchet(s) > 1u ? COMP_RATCH : COMP_OFF);
    }
    if (s->comp && ui.comp_sel != COMP_ARPN + s->comp - CP_ARPN)   /* (changed elsewhere: the editor, a dice) */
        ui.comp_sel = (uint8_t)(COMP_ARPN + s->comp - CP_ARPN);
    return ui.comp_sel;
}
static void step_clear(step_t *st)
{
    st->n = 0;
    st->time = ST_REST;
    st->flags = 0;
    st->vel = 0;
    st->hit = st->acc = 0;
    st->comp = st->carg = 0;
    st->comp2 = st->carg2 = st->cwhen = st->ctime = 0;
    st->probability = 0;
    st->lm[0] = st->lm[1] = st->lm[2] = 0;                /* (0.8.5) */
}

/* ------------------------------------------------------- the DRUM grid --- */
/* SEQ > STEP on a DRUM track is the grid: 8 lanes x the 16 steps of a page. The white keys are the
 * steps of the page shown (a tap toggles the selected lane there), black keys 1..8 select the lane (and play
 * it), black key 9 held is ACC (white keys toggle accents, their LEDs show them), black keys 10 / 11 the page
 * down / up. KNOB 1 STEP, 2 LANE, 3 HIT, 4 ACC edit the cursor step. A sound load never converts the
 * steps: the grid shows a step's notes on their lanes (eng_drum.c step_lanes) and an edit makes the lane its
 * own (grid_own). Live recording on a DRUM track writes hits (seq.c rec_note) */
static int grid_on(void) { return !ui.home && !ui.menu && !ui.confirm && cur_page()->graph == GR_ROLL && drum_track(TSEL); }   /* (STEP only: CHANCE is SC_STEP too) */

/* black key place p (seq.c key_place) held, 0 = not */
static int black_held(uint32_t p)
{
    uint32_t k;
    for (k = 0; k < 27u; k++)
        if (key_black(k) && key_place(k) == p)
            return (int)(((fm1_in.notes & ~kb_layer) >> k) & 1u);
    return 0;
}

/* lane l of step s from now on is its hit alone: the step's lane notes become hits (nothing sounds different),
 * a note of another pitch on the lane (a low tom 41) becomes the lane's own */
static void grid_own(step_t *s, uint32_t l)
{
    uint32_t k, j = 0;
    step_to_grid(s);
    for (k = 0; k < s->n; k++)
        if (drum_lane(s->note[k]) == l)
            s->hit |= (uint8_t)(1u << l);
        else
            s->note[j++] = s->note[k];
    for (k = j; k < 4u; k++)
        s->note[k] = 0;
    s->n = (uint8_t)j;
}

/* lane l of step i: on 1, off 0, toggled 2 */
static void grid_hit(track_t *t, uint32_t i, uint32_t l, uint32_t on)
{
    step_t *s = &t->step[i % NSTEP];
    uint32_t b = 1u << (l % NLANE);
    if (on == 2u)
        on = !(step_lanes(s) & b);
    if (s->time != ST_NOTE) {                    /* a REST or a TIE: an empty step (nothing to turn off) */
        if (!on)
            return;
        step_clear(s);
        s->time = ST_NOTE;
    }
    grid_own(s, l % NLANE);
    if (on) {
        s->hit |= (uint8_t)b;
    } else {
        s->hit &= (uint8_t)~b;
        s->acc &= (uint8_t)~b;
        step_set_lmode(s, l % NLANE, LM_NONE);         /* (0.8.5) its mode goes with it */
        if (!s->n && !s->hit)
            step_clear(s);
    }
}
/* (0.8.5) the grid's HIT of lane l at step i as one list: --, ON, then its modes x2 x4 x8 SHORT GHOST FLAM (0..7) */
static const char *const GRID_HIT[LM_N + 1] = {"--", "ON", "x2", "x4", "x8", "SHORT", "GHOST", "FLAM"};
static uint32_t grid_hit_of(const step_t *s, uint32_t l)
{
    return (step_lanes(s) >> (l % NLANE)) & 1u ? 1u + step_lmode(s, l) : 0u;
}
static void grid_hit_set(track_t *t, uint32_t i, uint32_t l, uint32_t v)
{
    if (!v) {
        grid_hit(t, i, l, 0);
        return;
    }
    grid_hit(t, i, l, 1);
    step_set_lmode(&t->step[i % NSTEP], l, v - 1u);
}

/* the accent of lane l at step i (on 1, off 0, toggled 2); an accent on an empty lane adds the hit */
static void grid_acc(track_t *t, uint32_t i, uint32_t l, uint32_t on)
{
    step_t *s = &t->step[i % NSTEP];
    uint32_t b = 1u << (l % NLANE);
    if (on == 2u)
        on = !(step_accents(s) & b);
    if (on)
        grid_hit(t, i, l, 1);
    if (!(step_lanes(s) & b))
        return;
    grid_own(s, l % NLANE);
    if (s->flags & SF_ACCENT) {                  /* a step accent: each hit's own from now on */
        s->acc |= s->hit;
        s->flags &= (uint8_t)~SF_ACCENT;
    }
    s->acc = (uint8_t)(on ? s->acc | b : s->acc & ~b);
}

/* SEQ cursor: wraps inside the pattern length, the bank follows, a step entry ends */
static void cursor_set(int32_t c)
{
    int32_t len = TSEL->p[P_SLEN] > 0 ? TSEL->p[P_SLEN] : 1;
    ui.cursor = (uint8_t)((c % len + len) % len);
    ui.bank = (uint8_t)(ui.cursor / 16u);
    ui.entry_open = 0;
    ui.pr_focus = 0;                                    /* (0.6.81: another step, the whole chord) */
}

static void cursor_fix(void)                           /* LEN got shorter: onto the last step */
{
    if (ui.cursor >= (uint32_t)TSEL->p[P_SLEN])
        cursor_set(TSEL->p[P_SLEN] - 1);
}

static void note_name(char *b, uint32_t n)
{
    str_cpy(b, N_NOTE[n % 12u], 4);
    fmt_int(b + str_len(b), (int32_t)(n / 12u) - 1);
}

/* (JIANT 0.6.51) a page (or the track) changed by hand: its name in the header (0.6.61), (0.6.64) the footer, for PF_MS, so the
 * pages need no memorising (ui_graph.c page_flash_draw); a page set by code (a test, a dialog's way back) shows none */
#ifndef PF_MS                                        /* (tests/ui_test.c: 0, the cards always shown) */
#define PF_MS 900u                                   /* (0.6.61: in the header, so it can stay a little longer) */
#endif
static uint8_t fg_ui;                                /* (0.6.52) SEQ > FUGUE: the head edited (0..2: heads 2..4) */
static uint32_t pf_ms;
static uint8_t pf_on;
static void page_flash(void)
{
    pf_ms = fm1_ms;
    pf_on = 1;
}
/* a section's button (rule 2, ui_lens.c nav_button): its page last shown; on it, the next group's first */
static void open_family(uint32_t fam) { nav_button(sec_of_fam(fam)); }

static void go_home(void)
{
    ui.home = 1;
    ui.plk_src = 0;                              /* (parameter locks: HOME's four) */
    ui.entry_open = 0;
    ui.hot_t = 0;
    song.seq_mode = 0;
    ui.force = 1;
}

/* ------------------------------------------------------- track setup --- */
static int seq_is_empty(const track_t *t)
{
    uint32_t i;
    for (i = 0; i < NSTEP; i++)
        if (t->step[i].n || t->step[i].hit)
            return 0;
    return 1;
}

/* Loads. A sound load (a factory or user preset, an engine jump, TOOLS INIT, the editor's PRESET / G_ENGSEL /
 * UP_LOAD) changes the sound only; a pattern load (SEQ > PATTERNS, a clear) changes the steps and the pattern
 * parameters only. A sound load keeps the track's motion on the parameters every engine has (load_end, motion.c
 * motion_sound_loaded); a pattern load drops it all (the steps it was on are gone).
 * TONIC: no undo. Felucca's one-step undo (SAVE held) and the layers' OCT- put back are gone; the song layer's
 * RECALL (docs/TONIC-SONG-PLAN.md) brings a section back as it was stored instead. */
enum { LOAD_SOUND = 1, LOAD_PAT = 2 };
static uint8_t load_depth;       /* loads nest (an engine jump loads its first preset): the outer one counts */
static uint32_t pat_sig[NTRK];   /* steps_sig of the pattern the last pattern load put into each track: such
                                  * steps, untouched, are replaced by the next pattern without asking */
static uint8_t pat_last[NTRK];   /* that pattern's list index + 1, 0 = none */
static uint8_t load_from;        /* a sound load (the outer one): the engine before it (load_end) */
static uint8_t load_mo;          /* .. the track's motion paused while the sound changes: bit 0 set, bit 1 it was ON */

static uint32_t fnv(uint32_t h, const void *p, uint32_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    while (n--)
        h = (h ^ *b++) * 16777619u;
    return h;
}
static uint32_t steps_sig(const track_t *t) { return fnv(2166136261u, t->step, sizeof t->step); }

/* load_begin: a pattern load drops the track's motion (its steps go); a sound load pauses it (PLAY OFF while the
 * sound changes under it) and load_end keeps what still means the same (motion_sound_loaded) */
static void load_motion_hold(track_t *t, uint32_t what)
{
    if (what & LOAD_PAT) {
        motion_reset(t);
        load_mo = 0;
        return;
    }
    load_from = t->eng_req;
    load_mo = (uint8_t)(1u | (motion_enabled(t) ? 2u : 0u));
    motion_set_enabled(t, 0);
}

static void load_begin(track_t *t, uint32_t what)
{
    if (load_depth++)
        return;
    if (what & LOAD_SOUND) {                      /* (0.6.13) the sound as it is: the fade of what sounds renders
                                                   * with it (voice.c xf_snap), unless one is fading on it now */
        uint32_t k = trk_index(t);
        if (xf_snap_t == 0xFFu || !trk[xf_snap_t % NTRK].xf_on) {
            memcpy(xf_snap, t->p, sizeof xf_snap);
            xf_snap_t = (uint8_t)k;
        }
    }
    motion_restore(t);
    load_motion_hold(t, what);
}

static void load_end(track_t *t)
{
    if (--load_depth)
        return;
    if (load_mo & 1u) {                           /* a sound load: the motion on the common parameters stays */
        (void)motion_sound_loaded(t, load_from, t->eng_req);
        if ((load_mo & 2u) && motion_count(t))
            motion_set_enabled(t, 1);
        load_mo = 0;
    }
    motion_rebase(t);
}

static int param_kept(uint32_t i);

/* a 16-step pattern (PATTERNS[] format, user presets too) into steps 1..16, the rest empty, LEN 16 */
static void load_pat16(track_t *t, const uint8_t *note, const uint8_t *flags)
{
    uint32_t i;
    for (i = 0; i < NSTEP; i++) {
        step_t *s = &t->step[i];
        uint8_t n = i < 16u ? note[i] : 0, fl = i < 16u ? flags[i] : 0;
        s->note[0] = n;
        s->n = n ? 1 : 0;
        s->time = (fl & 4u) ? ST_TIE : n ? ST_NOTE : ST_REST;
        s->flags = (uint8_t)((n ? (fl & (SF_ACCENT | SF_SLIDE | SF_RATCH)) : 0) | (s->flags & SF_MFINE));   /* (0.9.1: MICRO
                                                         * whole, its sixteenth in cwhen kept as before) */
        s->vel = n ? 96 : 0;
        s->hit = s->acc = 0;
        s->probability = 0;
        s->lm[0] = s->lm[1] = s->lm[2] = 0;             /* (0.8.5) */
        if (drum_track(t))                           /* a DRUM track: the lanes' notes as its grid */
            step_to_grid(s);
    }
    t->p[P_SLEN] = 16;
}

/* a 16-step drum grid (user presets of version 3: lane hits, their accents) into steps 1..16, the rest empty,
 * LEN 16 */
static void load_grid16(track_t *t, const uint8_t *hit, const uint8_t *acc)
{
    uint32_t i;
    for (i = 0; i < NSTEP; i++) {
        step_t *s = &t->step[i];
        step_clear(s);
        if (i < 16u && hit[i]) {
            s->time = ST_NOTE;
            s->hit = hit[i];
            s->acc = acc[i] & hit[i];
        }
    }
    t->p[P_SLEN] = 16;
}

static void track_defaults_steps(track_t *t)
{
    uint32_t i;
    latch_drop_req |= (uint8_t)(1u << trk_index(t));    /* (0.6.28) its held / latched / repeated notes end too */
    if (!chain_busy() && (live.req >= 0 || chain.armed_bar)) {   /* (a section jump or quick chain waiting for the bar */
        live.req = -1;                                  /* would bring the steps back: called off) */
        chain.armed_bar = 0;
    }
    motion_reset(t);
    for (i = 0; i < NSTEP; i++)
        step_clear(&t->step[i]);
}

/* SEQ > PATTERNS: the factory patterns (PATTERNS[], "01".."13"), then the used user presets that hold
 * one ("U07"): list index n. Loading one replaces the track's steps 1..16 (the rest cleared) and LEN;
 * a user preset's pattern brings its stored LEN (at most 16), DIV, SWING and GATE too. The notes are
 * loaded as they are: the patterns are written for the register of their kind of sound, DRUM and
 * SLICE patterns are drum and slice numbers, and SCL TRANS / OCT transpose what plays */
static uint32_t pat_count(void) { return NPATTERNS + up_pat_count(); }

static void pat_label(uint32_t n, char *tag, char *name)   /* tag: 4 bytes ("01", "U07"), name: 13 */
{
    if (n < NPATTERNS) {
        tag[0] = (char)('0' + (n + 1u) / 10u);
        tag[1] = (char)('0' + (n + 1u) % 10u);
        tag[2] = 0;
        str_cpy(name, PATTERNS[n].name, 13);
    } else {
        uint32_t k = up_pat_nth(n - NPATTERNS);
        up_slot_label(tag, k);
        up_name(k, name);
    }
}

static void pat_load(track_t *t, uint32_t n)
{
    load_begin(t, LOAD_PAT);
    if (n < NPATTERNS)
        load_pat16(t, PATTERNS[n].note, PATTERNS[n].flags);
    else
        up_pat_load(t, up_pat_nth(n - NPATTERNS));
    pat_sig[trk_index(t)] = steps_sig(t);
    pat_last[trk_index(t)] = (uint8_t)(n + 1u);
    load_end(t);
    ui.force = 1;
}

/* a pattern load would throw away steps of the user's (recorded, edited, from a project): ask first */
static int pat_needs_confirm(const track_t *t)
{
    return !seq_is_empty(t) && steps_sig(t) != pat_sig[trk_index(t)];
}

static uint32_t pat_pick(void)                   /* ui.ppick inside the list (user presets may be gone) */
{
    uint32_t n = pat_count();
    return ui.ppick < n ? ui.ppick : n - 1u;
}

/* SEQ > PATTERNS LOAD: pattern n into track t, "LOADED 03 MELODY" */
static void pat_load_ui(track_t *t, uint32_t n)
{
    char tag[4], nm[13], b[20];
    pat_label(n, tag, nm);
    pat_load(t, n);
    str_cpy(b, tag, sizeof b);
    str_cpy(b + str_len(b), " ", 2);
    str_cpy(b + str_len(b), nm, sizeof b - str_len(b));
    ui_say("LOADED ", b);
}

/* the track's settings, not the sound's: what a sound load (factory or user preset, an engine jump,
 * TOOLS INIT) leaves alone. The mix (LEVEL, PAN, MUTE: the TRACKS faders), the arpeggiator (ARP, ARP 2),
 * the scale and key map (SCL), the pattern parameters (LEN, DIV, SWING, GATE) and the SLICER insert,
 * which chops whatever the track plays in time with its sequencer */
static int param_kept(uint32_t i)
{
    return i == P_LEVEL || i == P_PAN || i == P_MUTE || (i >= P_AMODE && i <= P_SGATE) ||
           (i >= P_SLCR && i <= P_SLDEPTH) || i == P_SOFS || i == P_POFS ||
           (!FELUCCA_FM4 && (i == P_HVEL || i == P_HTIM || i == P_HGAT || i == P_HRQ)) ||   /* (0.6.87) HUMAN: the sequencer's, a sound
                                                         * keeps it (FELUCCA_FM4: DIGITAL's OP SUS / LEVEL there) */
           (!FELUCCA_FM4 && (i == P_CHRD || i == P_CVOI || i == P_CSTR || i == P_CLEN || i == P_TCHAN ||
                             i == P_MSND || i == P_MPAT || i == P_MAMT || i == P_MMODE));   /* (0.7.0) SCL > CHORD, C.LEN, CHANCE,
                                                         * MUTATE: the sequencer's too (a community report: browsing
                                                         * presets switched CHORD off) */
}

/* a retired preset kept as an alias (SAMPLE's, gone with it in JIANT): none now. -> the preset k stands for */
static uint32_t preset_orig(const engine_t *e, uint32_t k) { (void)e; return k; }
/* the presets of engine e that browsing shows before preset k (k = npresets: all of them) */
/* (0.5.1) browsing order: LOFI's BYTE presets, then FLOAT's (appended in 0.5.1: the stored numbers stay), then the
 * rest; every other engine in its own order. preset_nth: the preset shown n-th */
static const uint8_t LOFI_ORDER[17] = {0, 1, 2, 8, 9, 10, 11, 3, 4, 5, 6, 7, 12, 13, 14, 15, 16};
_Static_assert(sizeof LOFI_ORDER == NELEM(LOFI_PRESETS), "LOFI's browsing order: every preset once");
static uint32_t preset_rank(const engine_t *e, uint32_t k)
{
    uint32_t i;
    if (e == &ENG_LOFI)
        for (i = 0; i < sizeof LOFI_ORDER; i++)
            if (LOFI_ORDER[i] == k)
                return i;
    return k;
}
static uint32_t preset_nth(const engine_t *e, uint32_t n)
{
    return e == &ENG_LOFI && n < sizeof LOFI_ORDER ? LOFI_ORDER[n] : n;
}
#define preset_shown(e) (ENGINES[e]->npresets)

#if !FELUCCA_FM4
/* DIGITAL (engine 1, retired): t's sound = p, values as DIGITAL has them, converted to FM6 with a patch of its own
 * (fm4_convert.c). Every path that brings a DIGITAL sound into a track ends here: a track never keeps engine 1 */
static void fm4_apply(track_t *t, int16_t *p)
{
    uint8_t v[FP_SIZE + 1u];
    uint32_t pr = fm4_convert(p, v), tr = trk_index(t), f;
    fm6_set_patch(tr, v);
    f = motion_guard();                               /* the audio ISR sees the old sound or the new one */
    memcpy(t->p, p, sizeof t->p);
    t->eng_req = ENGI_FM6;
    t->preset = (uint8_t)pr;
    motion_unguard(f);
    fm6_adopt(tr);                                    /* SLOT OWN: the converted patch is the track's own */
}
static void fm4_track(track_t *t)                     /* t holds a DIGITAL sound (engine 1): convert it */
{
    int16_t p[P_COUNT];
    memcpy(p, t->p, sizeof p);
    fm4_apply(t, p);
}
/* DIGITAL preset k (a stored preset number of engine 1: the editor's PRESET, SET G_ENGSEL, an old project's
 * power-on sound): its sound as a preset load sets it, converted. The sound only (not the steps, not param_kept) */
static void fm4_load_preset(track_t *t, uint32_t k)
{
    int16_t p[P_COUNT];
    uint32_t i;
    load_begin(t, LOAD_SOUND);
    panic_req |= (uint8_t)(1u << trk_index(t));
    t->user = 0;
    if (t == TSEL)
        sync_reload = 1;
    memcpy(p, t->p, sizeof p);
    for (i = 0; i < P_E0; i++)
        if (!param_kept(i))
            p[i] = TP[i].def;
    fm4_preset_values(p, k);
    fm4_apply(t, p);
    load_end(t);
}
#endif

static void set_engine_of(track_t *t, uint32_t ei);
/* preset pi of the engine the track asked for: the sound only (not the steps, not param_kept) */
static void apply_preset_to(track_t *t, uint32_t pi)
{
    const engine_t *e = trk_engp(t);
    uint32_t i;
#if !FELUCCA_FM4
    if (trk_eng(t) == ENGI_DIGITAL) {
        fm4_load_preset(t, pi);
        return;
    }
#endif
    if (trk_eng(t) == ENGI_SAMPLE && pi == SMP_SET_PERC) {   /* SAMPLE preset 4 was PERC (retired, a stored */
        set_engine_of(t, ENGI_DRUM);                  /* number: the editor's PRESET, a favourite): DRUM's kit */
        return;                                       /* (core.h drum_from_perc) */
    }
    if (eng_gone(trk_eng(t))) {            /* PHYS SAMPLE GRAIN SLICE (retired, core.h): ANALOG's first */
        set_engine_of(t, ENGI_PHYS_TO);
        return;
    }
    load_begin(t, LOAD_SOUND);
    panic_req |= (uint8_t)(1u << trk_index(t));       /* MONO/POLY may change: release what sounds */
    t->user = 0;
    if (t == TSEL)
        sync_reload = 1;
    if (!e->npresets) {
        load_end(t);
        return;
    }
    pi = preset_orig(e, pi % e->npresets);
    t->preset = (uint8_t)pi;
    for (i = 0; i < P_E0; i++)                        /* the rest of the sound to its defaults: a preset */
        if (!param_kept(i))                           /* sounds the same after any edit */
            t->p[i] = TP[i].def;
    for (i = 0; i < 8u; i++)
        t->p[P_E0 + i] = (int16_t)e->presets[pi].e[i];
    t->p[P_ATK] = e->presets[pi].env[0];
    t->p[P_DEC] = e->presets[pi].env[1];
    t->p[P_SUS] = e->presets[pi].env[2];
    t->p[P_REL] = e->presets[pi].env[3];
    t->p[P_ED_FLT] = e->presets[pi].fenv;
    t->p[P_VOICE] = e->presets[pi].mono ? V_LEGATO : V_POLY;   /* mono presets keep the legato feel */
    if (e == &ENG_FORMANT) {                          /* (0.8.5) VOICE's words: LANG, LYRIC (0: its vowels) */
        t->p[P_VLANG] = (int16_t)(e->presets[pi].xp & 3u);
        t->p[P_VLYR] = (int16_t)(e->presets[pi].xp >> 2);
    }
    {   /* the sends */
        static const uint8_t FX_DEF[4] = {0, 24, 28, 36};
        const preset_t *pr = &e->presets[pi];
        for (i = 0; i < 4u; i++)
            t->p[P_DIST + i] = (int16_t)(pr->fx[i] ? pr->fx[i] - 1 : FX_DEF[i]);
    }
    fm6_track_loaded(t);                              /* FM6: the preset's patch (its SLOT) */
    load_end(t);
}

/* the engine's defaults and its first preset. With the audio IRQ off: the ISR sees the old engine with
 * its values or the new one with its own (voice.c engine_block), never one with the other's */
static void set_engine_of(track_t *t, uint32_t ei)
{
    const engine_t *e;
    uint32_t i;
    if (eng_gone(ei % NENGINES))                     /* PHYS SAMPLE GRAIN SLICE (retired, core.h): ANALOG */
        ei = ENGI_PHYS_TO;
    e = ENGINES[ei % NENGINES];
#if !FELUCCA_FM4
    if (ei % NENGINES == ENGI_DIGITAL) {             /* DIGITAL (retired): its first preset, as FM6 */
        fm4_load_preset(t, 0);
        return;
    }
#endif
    load_begin(t, LOAD_SOUND);
    fm1_irq_off();
    t->eng_req = (uint8_t)(ei % NENGINES);
    for (i = 0; i < 8u; i++)
        t->p[P_E0 + i] = e->edit[i].def;
    apply_preset_to(t, 0);
    fm1_irq_on();
    load_end(t);
}

/* (0.8.1) another live track playing DRUM-X (1 + its index; 0 none): the section has one kit, theirs */
static uint32_t drum_elsewhere(const track_t *t)
{
    uint32_t k;
    for (k = 0; k < trk_live; k++)
        if (&trk[k] != t && trk_eng(&trk[k]) == ENGI_DRUM)
            return k + 1u;
    return 0;
}
static void apply_preset(uint32_t pi)                 /* the selected track's sound (browsing, the editor, PROGRAM CHANGE) */
{
    apply_preset_to(TSEL, pi);
    if (trk_eng(TSEL) == ENGI_DRUM) {      /* (0.7.2) DRUM: the preset is a factory kit, into the section's kit
                                                       * (drumx_voice.c DX_KITS; a project load or an engine switch keeps the
                                                       * section's kit: apply_preset_to alone) */
        uint32_t o = drum_elsewhere(TSEL);
        if (!o)
            dx_kit_load(pi % ENG_DRUM.npresets);
        else {                                         /* (0.8.1, reported: browsing T5's sounds past the kits changed T4's
                                                        * drums) the kit is shared: another track's stays as it is */
            static char m[16] = "KIT OF T? KEPT";
            m[8] = (char)('0' + o);
            ui_message(m);
        }
    }
}
static void set_engine(uint32_t ei) { set_engine_of(TSEL, ei); }
/* (0.6.46) INIT SOUND, NEW PROJECT, (0.6.49) the power-on: the engine's own defaults (its EDIT values as the engine
 * declares them, not its first preset's colour; the envelope, VOICE and the filter envelope as a new track has them),
 * dry (no DIST CHOR DLY REV send: the effects are the player's to add), on FM6 the init voice */
static void init_sound_of(track_t *t, uint32_t ei)
{
    static const uint8_t ENVP[6] = {P_ATK, P_DEC, P_SUS, P_REL, P_ED_FLT, P_VOICE};
    const engine_t *e;
    uint32_t i;
    load_begin(t, LOAD_SOUND);
    set_engine_of(t, ei);
    e = trk_engp(t);
    for (i = 0; i < 8u; i++)
        t->p[P_E0 + i] = e->edit[i].def;
    for (i = 0; i < 6u; i++)
        t->p[ENVP[i]] = TP[ENVP[i]].def;
    for (i = 0; i < 4u; i++)
        t->p[P_DIST + i] = 0;
    fm6_init_voice(t);
    load_end(t);
}
/* (0.9.9) the power-on, INIT SYSTEM and NEW PROJECT: part i (engines.c TRK_DEF) its engine with its preset's sound
 * (the EDIT values, the envelope, MONO / POLY, VOICE's words, FM6's patch), over the engine's defaults as INIT SOUND
 * leaves them, and dry: no DIST CHOR DLY REV send (the effects are the player's to add). Was INIT SOUND's alone */
static void power_sound_of(track_t *t, uint32_t i)
{
    uint32_t k;
    init_sound_of(t, TRK_DEF[i % NPART][0]);
    apply_preset_to(t, TRK_DEF[i % NPART][1]);
    for (k = 0; k < 4u; k++)
        t->p[P_DIST + k] = 0;
}

static void track_defaults(track_t *t)
{
    uint32_t i;
    for (i = 0; i < P_E0; i++)
        t->p[i] = TP[i].def;
    track_defaults_steps(t);
}
/* (0.9.9) the power-on's parts (main.c felucca_init, inside its load): each track at its defaults with its power-on
 * sound (power_sound_of), the sequencers empty (or TRK_DEF's pattern: none has one) */
static void power_on_parts(void)
{
    uint32_t i;
    for (i = 0; i < NTRK; i++) {
        track_t *t = &trk[i];
        track_defaults(t);
        power_sound_of(t, i);
        t->engine = t->eng_req;
        track_defaults_steps(t);              /* (a sound load never touches them) */
        if (TRK_DEF[i][2])
            load_pat16(t, PATTERNS[TRK_DEF[i][2] - 1u].note, PATTERNS[TRK_DEF[i][2] - 1u].flags);
        pat_sig[i] = steps_sig(t);            /* a default pattern, not the user's */
        pat_last[i] = TRK_DEF[i][2];
    }
}

/* switch engine (its defaults + first preset) and say so */
static void select_engine(uint32_t e)
{
    set_engine(e);
    ui_say("ENGINE ", ENGINES[TSEL->eng_req]->name);
    ui.force = 1;
}

/* the presets of every engine (in ENGINE_ORDER), then the used user presets, as one list (the PRESETS knob and the
 * PRESETS page browse it) */
static uint32_t preset_all_pos(uint32_t *total)          /* list index of the selected track's preset */
{
    uint32_t n = 0, cur = 0, e, r;
    for (r = 0; r < NENG_SHOWN; r++) {                  /* (engines.c ENGINE_ORDER) */
        const engine_t *en = ENGINES[e = eng_vis(r)];
        if (e == TSEL->eng_req)
            cur = n + preset_rank(en, preset_orig(en, TSEL->preset % (en->npresets ? en->npresets : 1u)));
        n += preset_shown(e);
    }
    if (user_of(TSEL) < UP_SLOTS)
        cur = n + up_rank(user_of(TSEL));
    *total = n + up_count();
    return cur;
}

/* list index n (< total) -> engine, *k its preset; NENGINES = user preset, *k its slot. The engines in the order of
 * `order` (NENG_SHOWN of them): ENGINE_ORDER, the list shown; (0.9.9) ENGINE_PC_ORDER, PROGRAM CHANGE's numbers */
static uint32_t preset_all_at_in(uint32_t n, uint32_t *k, const uint8_t *order)
{
    uint32_t e, r;
    for (r = 0; r < NENG_SHOWN && n >= preset_shown(order[r]); r++)
        n -= preset_shown(order[r]);
    if (r == NENG_SHOWN) {
        *k = up_nth(n);
        return NENGINES;
    }
    e = order[r];
    *k = preset_nth(ENGINES[e], n);                     /* the n-th shown preset */
    return e;
}
static uint32_t preset_all_at(uint32_t n, uint32_t *k) { return preset_all_at_in(n, k, ENGINE_ORDER); }

static uint32_t preset_pos(uint32_t *total)
{
    uint32_t all, current = preset_all_pos(&all), n = 0, pos = 0xFFFFFFFFu;
    if (!favorites.filter) { *total = all; return current; }
    for (uint32_t i = 0; i < all; i++) {
        uint32_t k, e = preset_all_at(i, &k);
        if (!favorite_has(e, k)) continue;
        if (i == current) pos = n;
        n++;
    }
    *total = n;
    return pos == 0xFFFFFFFFu ? n : pos; /* current sound need not be a favorite */
}
static uint32_t preset_at(uint32_t n, uint32_t *k)
{
    uint32_t all;
    if (!favorites.filter) return preset_all_at(n, k);
    preset_all_pos(&all);
    for (uint32_t i = 0; i < all; i++) {
        uint32_t e = preset_all_at(i, k);
        if (favorite_has(e, *k) && !n--) return e;
    }
    *k = UP_SLOTS;
    return NENGINES;
}
static int preset_favorite(void)
{
    uint32_t k = user_of(TSEL);
    return favorite_has(k < UP_SLOTS ? NENGINES : TSEL->eng_req,
                                        k < UP_SLOTS ? k : TSEL->preset);
}
static void preset_mark(int on)
{
    uint32_t k = user_of(TSEL);
    if (favorite_set(k < UP_SLOTS ? NENGINES : TSEL->eng_req, k < UP_SLOTS ? k : TSEL->preset, on)) {
        ui.force = 1;
        settings_save();
    }
}

/* the pattern the selected track's sound suggests: its index in the SEQ > PATTERNS list, or -1. A factory
 * preset's PAT(n); a user preset that holds a pattern: that one ("U07") */
static int32_t preset_pat_hint(void)
{
    const engine_t *e = trk_engp(TSEL);
    uint32_t u = user_of(TSEL);
    if (u < UP_SLOTS)
        return up_has_pat(u) ? (int32_t)(NPATTERNS + up_pat_rank(u)) : -1;
    if (!e->npresets)
        return -1;
    u = e->presets[TSEL->preset % e->npresets].pat;
    return u && u <= NPATTERNS ? (int32_t)u - 1 : -1;
}

static void preset_hinted(void)                     /* after a sound load: SEQ > PATTERNS starts at the suggested pattern */
{
    int32_t h;
    if ((h = preset_pat_hint()) >= 0)
        ui.ppick = (uint8_t)h;
    ui.force = 1;
}

static void preset_go(uint32_t n)                    /* load list index n into the selected track (the sound only) */
{
    uint32_t k, e = preset_at(n, &k);
    if (e == NENGINES) {
        up_load(k);
    } else {
        if (e != TSEL->eng_req)
            select_engine(e);
        apply_preset(k);
    }
    preset_hinted();
}

/* (JIANT 0.6.72) PROGRAM CHANGE (midi_control.c midi_pc_req): the list's n-th sound into the track whose channel it
 * came on, the selection kept; out of the list's range, nothing. (0.9.9) The list in 0.9.8's engine order
 * (engines.c ENGINE_PC_ORDER), whatever the PRESETS page shows: a rig's program numbers keep their sounds. Every frame
 * (ui_input) */
static void midi_pc_poll(void)
{
    uint32_t i, sel = song.sel;
    if (midi_song_req) {                               /* (0.6.80) channel 16: 0..3 a section, 16..23 a project */
        uint32_t n = midi_song_req - 1u;
        midi_song_req = 0;
        if (n < 4u)
            (void)section_jump(n);
        else if (n >= 16u && n < 16u + NSONG)
            (void)song_select(n - 16u);
    }
    for (i = 0; i < NTRK; i++)
        if (midi_pc_req[i]) {
            uint32_t n = midi_pc_req[i] - 1u, all, k, e;
            midi_pc_req[i] = 0;
            song.sel = (uint8_t)i;
            preset_all_pos(&all);
            if (n < all) {
                e = preset_all_at_in(n, &k, ENGINE_PC_ORDER);   /* (0.9.9: 0.9.8's numbers) */
                if (e == NENGINES) {
                    up_load(k);
                } else {
                    if (e != TSEL->eng_req)
                        set_engine(e);
                    apply_preset(k);
                }
                ui.force = 1;
            }
            song.sel = (uint8_t)sel;
        }
}

/* the EDIT layer's KNOB 2 (ui_layer.c): the selected track's engine only: its factory presets, then the used user presets
 * made with it (slot order). List index of the current sound; *total the length */
static uint32_t eng_list_pos(uint32_t *total)
{
    const engine_t *en = trk_engp(TSEL);
    uint32_t e = trk_eng(TSEL), np = preset_shown(e), u = user_of(TSEL), k, n = 0;
    uint32_t cur = np ? preset_rank(en, preset_orig(en, TSEL->preset % en->npresets)) : 0u;   /* (#124: the aliases
                                        * skipped, as on PRESETS: SAMPLE 1 loads as 0, so counting it stuck KNOB 2) */
    for (k = 0; k < UP_SLOTS; k++)
        if (up_used(k) && up_engine(k) == e) {
            if (k == u)
                cur = np + n;
            n++;
        }
    *total = np + n;
    return cur;
}

static void eng_list_step(int32_t direction)         /* the next / previous sound of the engine (wraps) */
{
    uint32_t total, cur = eng_list_pos(&total), e = trk_eng(TSEL), np = preset_shown(e), k, n;
    if (total < 2u)
        return;
    n = (cur + (direction > 0 ? 1u : total - 1u)) % total;
    if (n < np) {
        apply_preset(preset_nth(ENGINES[e], n));        /* the n-th shown preset */
    } else {
        n -= np;
        for (k = 0; k < UP_SLOTS; k++)
            if (up_used(k) && up_engine(k) == e && !n--) {
                up_load(k);
                break;
            }
    }
    preset_hinted();
}

static void preset_step(int32_t direction)
{
    uint32_t total, cur = preset_pos(&total);
    if (!total) { ui_message("NO FAVORITES"); return; }
    preset_go(cur >= total ? (direction > 0 ? 0 : total - 1) :
              (cur + (direction > 0 ? 1u : total - 1u)) % total);
}

/* Seven display rows. Favorites use a bounded window, not a repeating carousel.
 * Return total for an empty row; a non-favorite current sound shows the start. */
static uint32_t preset_visible(uint32_t cur, uint32_t total, uint32_t row)
{
    uint32_t first, last;
    if (!total || row >= 7u) return total;
    if (!favorites.filter)
        return (cur + total * 4u + row - 3u) % total;
    first = cur < total && cur > 3u ? cur - 3u : 0u;
    last = total > 7u ? total - 7u : 0u;
    if (first > last) first = last;
    return first + row < total ? first + row : total;
}

/* (JIANT 0.4) the MACRO layer latched: LFO held opens it, let go it stays (M1..M4 on KNOB 1..4, SELECT the dice, its
 * cards drawn by ui_draw over any page, no layer open: the keys stay notes) until LFO is pressed again (ui_layer.c) */
static uint8_t mac_latch;
/* HOME: what KNOB k edits: the engine's four main parameters */
static const param_desc_t *home_param(uint32_t k, int16_t **vp)
{
    uint32_t id = trk_engp(TSEL)->knob[k & 3u];
    *vp = &TSEL->p[id];
    return track_desc(TSEL, id);
}

/* select track i (KNOB 1 on TRACKS, the editor): its sound, pages and pattern from now on */
static void track_select(uint32_t i)
{
    if (i >= NTRK || i == song.sel)
        return;
    song.sel = (uint8_t)i;
    ui.entry_open = 0;
    ui.hot_t = 0;
    ui.cursor = 0;
    ui.bank = 0;
    sync_reload = 1;
    ui.force = 1;
    page_flash();                                    /* (0.6.51) the track's number and engine shown big */
}

#include "ui_events.c"                            /* SEQ > AUTO LIST: the locks and events as a list (an action page) */
/* EDIT > SOUND / SOUND 2 (ui_dx.c, included by ui_input.c): the DRUM-X sound edited and its patch (0 A, 1 B) */
static uint8_t dx_ui_lane, dx_ui_side;
static uint32_t dx_pagen(const page_t *pg)            /* SOUND 1..3; (0.5.1) X-MOD 4; (0.6.54) SOUND 4: 8 */
{
    if (pg->title[0] == 'X')
        return 4u;
    return pg->title[5] == ' ' ? (pg->title[6] == '4' ? 8u : (uint32_t)(pg->title[6] - '0')) : 1u;
}
static void dx_knob(const page_t *pg, uint32_t k, int32_t s);
static void dx_cards(const page_t *pg);
static void dx_kit3_knob(uint32_t k, int32_t s);      /* (0.6.54) KIT 3: TONE's BP, FEEDBK */
#define DX_KIT3(pg) ((pg)->graph == GR_DXIMG && (pg)->title[0] == 'K')   /* (0.6.58) GR_DXIMG: KIT 3, or STEREO (IMAGE) */
static void dx_kit3_cards(void);
static uint32_t dx_sig(void);

/* ---------------------------------------------------- action pages --- */
/* Pages whose purpose is an action (SEQ > PATTERNS, SAVE > USER, PROJECT, TOOLS): the knobs pick,
 * OCT+ does it, OCT- cancels the picked action or goes HOME (ui_input.c). There OCT- / OCT+ do not
 * shift the octave */
static int go_id(uint32_t id) { return id == G_LOAD || id == G_SAVE || id == G_CLRSEQ || id == G_INITSND; }

static uint32_t act_cols(void)                   /* the columns that are actions, a bit each; 0 = not such a page */
{
    const page_t *pg = cur_page();
    uint32_t c, m = 0;
    if (ui.home)
        return 0;
    if (pg->graph == GR_MOTION || pg->graph == GR_PUNCH) return 8u;   /* (0.6.99) PUNCH: KNOB 4 CLEAR */
    if (pg->graph == GR_EVENTS) return 16u;     /* AUTO LIST: no knob is an action; OCT+ adds / toggles (ev_oct) */
    if (pg->graph == GR_TOOLS) return 15u;
    if (pg->graph == GR_LYRIC) return 8u;       /* (0.8.5) WORDS: OCT+ TYPE (the lyric, ui_name.c) */
    if (pg->graph == GR_SONG)
        return 1u;                               /* (0.8.5) ADD a row (PLAY: the song) */
    if (pg->graph == GR_PATS)
        return 2u;                               /* LOAD */
    if (pg->graph == GR_USER)
        return 14u;                              /* LOAD ERASE SAVE */
    if (pg->scope == SC_GLOBAL)
        for (c = 0; c < 4u; c++)
            if (go_id(pg->id[c]))
                m |= 1u << c;
    return m;
}

/* the action OCT+ does: its column + 1, 0 = none picked yet (PATTERNS has LOAD only) */
/* (0.6.85) SEQ > STRETCH's picks (ui_input.c shape_knob); (0.6.92) done as the knob turns, from a neutral middle:
 * STRETCH /8 .. /2 (0..6), x1 (7: as it was), x2 .. x8 (8..14); REPEAT OFF (0), x2 .. x8 (1..7), TO 64 (8) */
static uint8_t shp_i = 7u, shp_r = 0u;
static int32_t shp_k(void) { return shp_i < 7u ? -(int32_t)(8u - shp_i) : shp_i == 7u ? 1 : (int32_t)shp_i - 6; }
static uint32_t shp_rep(void) { return shp_r == 0u ? 1u : shp_r < 8u ? shp_r + 1u : 0u; }   /* 1 as it was, 2..8, 0 to 64 */

static uint32_t act_col(void)
{
    if (!ui.home && cur_page()->graph == GR_SONG) return 1u;
    if (!ui.home && cur_page()->graph == GR_EVENTS) return 5u;
    if (!ui.home && cur_page()->graph == GR_LYRIC) return 4u;
    return !ui.home && cur_page()->graph == GR_PATS ? 2u : ui.act;
}

static const char *act_name(uint32_t c)          /* column c's action (the footer hint) */
{
    static const char *const UP_GO[3] = {"LOAD", "ERASE", "SAVE"};
    uint32_t id = cur_page()->id[c & 3u];
    if (cur_page()->graph == GR_MOTION || cur_page()->graph == GR_PUNCH) return "CLEAR";
    if (cur_page()->graph == GR_EVENTS) return ev_act_name();
    if (cur_page()->graph == GR_LYRIC) return "TYPE";
    if (cur_page()->graph == GR_TOOLS) {
        static const char *const actions[] = {"CLEAR", "INIT", "DELETE", "CLEAR", "INIT"};
        return actions[c < 5u ? c : 4u];
    }
    if (cur_page()->graph == GR_SONG)                   /* (0.8.5) SAVE's lens: OCT+ adds a row (PLAY plays) */
        return "ADD";
    if (cur_page()->graph == GR_PATS)
        return "LOAD";
    if (cur_page()->graph == GR_USER)
        return UP_GO[(c + 2u) % 3u];
    return id == G_CLRSEQ ? "CLEAR" : id == G_INITSND ? "INIT" : id == G_LOAD ? "LOAD" : "SAVE";
}

/* the picked action would do something now (OCT+ breathes): another pattern, a used slot, stopped for
 * a flash write, steps to clear */
static int act_ready(void)
{
    uint32_t c = act_col(), s = song.sel, id;
    if (!c--)
        return 0;
    if (cur_page()->graph == GR_MOTION) return !chain_busy() && motion_count(TSEL);
    if (cur_page()->graph == GR_EVENTS) return ev_act_ready();
    if (cur_page()->graph == GR_LYRIC) return 1;
    if (cur_page()->graph == GR_TOOLS)                  /* one case per column: CLEAR PAT, INIT, DELETE ROW, CLEAR SONG */
        return !chain_busy() && (c == 0u ? !seq_is_empty(TSEL) || motion_count(TSEL) : c == 1u || c >= 4u ? 1 :
                                 c == 2u ? ui.song_row < chain_config.count : chain_config.count != 0u);
    if (cur_page()->graph == GR_SONG)
        return !chain_busy() && chain_config.count < CHAIN_ROWS;
    if (cur_page()->graph == GR_PATS)
        return pat_last[s] != pat_pick() + 1u || steps_sig(TSEL) != pat_sig[s];
    if (cur_page()->graph == GR_USER)
        return c == 3u ? !song.playing : up_used(ui.uslot) && (c == 1u || !song.playing);
    id = cur_page()->id[c & 3u];
    if (id == G_LOAD)
        return project_used((uint32_t)song.g[G_SLOT] - 1u);
    if (id == G_SAVE)
        return !song.playing;
    if (id == G_CLRSEQ)
        return !seq_is_empty(TSEL);
    return 1;
}
