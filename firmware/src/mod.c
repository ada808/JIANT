/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Modulation matrix: per track four slots, each SRC -> DST by AMT (-64..63), added to the fixed
 * routings (ENV DEST, LFO DEST). Runs in the audio ISR (fx.c mix_part, voice.c track_render).
 *
 * Sources, Q15:
 *   LFO   the track LFO with its FADE (as LFO DEST), bipolar
 *   ENV   the amp envelope of the voice (after the engine's own amp curve, as ENV DEST), 0..1
 *   VEL   the note-on velocity (before UNISON's level scaling), 0..1
 *   KEY   the note relative to C4, bipolar: +-64 semitones = +-1 (an octave = 0.19)
 *   RAND  a new random value per note-on (its own generator: the shared rng is not touched), bipolar
 *   MODW  MIDI CC1, AT channel aftertouch, EXPR CC11 of the track's channel (seq.c), 0..1; until a
 *         controller arrives (and after CC121 RESET ALL CONTROLLERS) MODW and AT are 0, EXPR 1
 *   STEP  (JIANT) the track's modulation sequence (LFO > MSEQ, a CV sequencer): the level of its step now (LEN steps of
 *         DIV from PLAY; stopped: where it was), 0..1, slewed by SLEW (ms_tick, once a block when a slot reads it)
 *   M1..M4 (JIANT) the project's macros (macro_v, the knobs with LFO held), 0..1: the same for every track, so a
 *         macro reaches as many destinations as the slots that name it
 * Destinations:
 *   per voice (vmod_t, like the fixed routings):
 *     PITCH  AMT 63 at a full source = +-11.8 semitones (as LFO DEST PIT)
 *     CUT    m->cutoff, SHP m->shape: AMT 63 = +-63 steps (as ENV DEST); the sums are clamped to what
 *            the fixed routings can reach, so the engines see no new range
 *     AMP    a gain <= 1: AMT > 0 the source opens it (AMT 64: silent at 0, full at 1), AMT < 0 closes it
 *            (bipolar sources as 0..1, as LFO DEST AMP); several AMP slots multiply
 *   per track, once per block: PAN, DIST CHO DLY REV (the sends), RATE (LFO rate), VIB (LFO DEST PIT, the
 *     vibrato depth), E1..E8 (the engine's parameters P_E0..P_E7, named by the engine): AMT 64 at a full
 *     source = the parameter's whole range; the sum is clamped to its range
 *   the master (JIANT): CLIP and PNCH (G_CLIP, G_PUNCH, FX > MASTER), from any track's slot, AMT 64 at a full
 *     source = 0..100; the slots of all tracks add (macro_master, once an audio block)
 * A per-voice source on a per-block destination takes the latest note-on's value (VEL, KEY, RAND), ENV the
 * envelope of the latest note-on's voice (one block late, 0 when it has ended).
 *
 * Per-block destinations are modulated in place: mod_begin writes the modulated values into t->p for the
 * part's block (its render and its mix in fx.c mix_part) and mod_end puts the stored values back. Both run
 * in the audio ISR, so nothing outside it ever sees a modulated value (the UI, the editor, projects and
 * presets read and write t->p from the main loop, which the ISR preempts; TIMER5 only fills the MIDI rings),
 * and the engines read t->p as before. An engine reads its parameters in note_on (events_block) unmodulated.
 * Nothing active (every slot with SRC, DST or AMT at 0): no work, and the sound is bit-identical. */
enum { MS_OFF, MS_LFO, MS_ENV, MS_VEL, MS_KEY, MS_RAND, MS_MODW, MS_AT, MS_EXPR, MS_M1, MS_STEP = MS_M1 + 4, MS_NOISE, MS_N };
static uint32_t div_samples(uint32_t div);   /* fx.c */
#define CLK_START 0xFFFFFFFFu
static uint32_t clk_pos = CLK_START, clk_step;  /* (seq.c click_tick) samples into the transport's 1/16 step, the step of
                                                 * the bar (0..15); after events_block: the block's start */
static uint32_t clk_n;                          /* (JIANT) .. the 1/16 steps since PLAY (the SLICER's and the FX layer's grid) */
static uint32_t ms_clock;        /* (JIANT) samples since PLAY on the transport's grid (fx.c mix_block; seq.c seq_start:
                                  * 0): MS_STEP's place */
enum { MD_OFF, MD_PITCH, MD_CUT, MD_SHP, MD_AMP, MD_PAN, MD_DIST, MD_CHO, MD_DLY, MD_REV, MD_RATE, MD_VIB, MD_E1,
       MD_CLIP = MD_E1 + 8, MD_PNCH, MD_DTIME, MD_DFDBK, MD_DCOL, MD_DMIX, MD_RSIZE, MD_RDAMP, MD_CRATE, MD_CDEPTH, MD_N };
/* (0.7.0) the FX buses' globals as destinations (a mod sequence on the delay time ..): once an audio block
 * macro_master sums every track's slots on them and fx.c reads the stored value plus that offset through GFX(k) in
 * place of song.g[] (no slot on a bus: the stored value, live for the UI's graphs and the host tests of a bus alone) */
static const uint8_t GFX_ID[8] = {G_DTIME, G_DFDBK, G_DCOLOR, G_DMIX, G_RSIZE, G_RDAMP, G_CRATE, G_CDEPTH};
enum { GF_DTIME, GF_DFDBK, GF_DCOL, GF_DMIX, GF_RSIZE, GF_RDAMP, GF_CRATE, GF_CDEPTH };
static volatile int16_t gfx_off[8];                      /* the matrix's offset on each (0: the stored value, as before) */
static inline int32_t gfx_val(uint32_t k)
{
    int32_t o = gfx_off[k], v = song.g[GFX_ID[k]];
    return o ? clamp(v + o, GP[GFX_ID[k]].min, GP[GFX_ID[k]].max) : v;
}
#define GFX(k) gfx_val(k)
static uint8_t macro_v[4];       /* (JIANT) the macros M1..M4, 0..127: the project's (project.c), LFO held turns them */
_Static_assert(NELEM(N_MSRC) == MS_N && NELEM(N_MDST) == MD_N, "N_MSRC / N_MDST == MS_* / MD_*");
_Static_assert(P_M2SRC == P_M1SRC + 3 && P_M4AMT == P_M1SRC + 11 && P_M4AMT + 1 == P_FM1_ATK, "matrix slots: 3 ids each");
#define NMSLOT 4u
#define MS_VOICE(s) ((s) >= MS_ENV && (s) <= MS_RAND)             /* a value per voice */
#define MS_BIPOLAR(s) ((s) == MS_LFO || (s) == MS_KEY || (s) == MS_RAND || (s) == MS_NOISE)

/* the track parameter of a per-block destination */
static uint32_t mod_param(uint32_t d)
{
    static const uint8_t ID[MD_E1] = {[MD_PAN] = P_PAN, [MD_DIST] = P_DIST, [MD_CHO] = P_CHOR, [MD_DLY] = P_DLY,
                                      [MD_REV] = P_REV, [MD_RATE] = P_LRATE, [MD_VIB] = P_LD_PIT};
    return d >= MD_E1 ? P_E0 + d - MD_E1 : ID[d];
}

typedef struct {                 /* the part in mix_part (one at a time) */
    uint8_t on;                  /* a slot is active: track_render calls mod_voice */
    uint8_t amp;                 /* an AMP slot: the voice gain applies */
    uint8_t nv, nk;              /* voice terms, per-block parameters held */
    int32_t pit, cut, shp, gain; /* per voice, from the per-track sources (gain Q15) */
    uint8_t vsrc[NMSLOT], vdst[NMSLOT];
    int8_t vamt[NMSLOT];
    uint8_t kid[NMSLOT];         /* per-block: the parameter, its stored value */
    int16_t keep[NMSLOT];
} mod_t;
static mod_t mod_c[2];                      /* (JIANT 0.6.71) one a core: each renders its own parts at once (fx.c mix_block) */
#define MOD (mod_c[MC_CPU() & 1u])

static uint32_t mod_seed = 0x2545F491u;
static int16_t mod_rand(void)    /* RAND of a note-on: xorshift32, bipolar */
{
    mod_seed ^= mod_seed << 13;
    mod_seed ^= mod_seed >> 17;
    mod_seed ^= mod_seed << 5;
    return (int16_t)(mod_seed >> 16);
}

static int32_t mod_key(uint32_t note) { return clamp(((int32_t)note - 60) * 512, -32767, 32767); }

/* (JIANT 0.7.8) a smooth value noise, -32768..32767: a new random point every 2^lg steps of clk, eased between
 * (smoothstep); seed picks the stream. No state. DRIFT's per voice (voice.c), NOISE's per track (mod_tsrc) */
static int32_t drift_noise(uint32_t seed, uint32_t clk, uint32_t lg)   /* (0.7.9: a point every 2^lg steps, no divide) */
{
    uint32_t k = clk >> lg, f = ((clk & ((1u << lg) - 1u)) << 8) >> lg, a = (seed + k) * 2654435761u,
             b = (seed + k + 1u) * 2654435761u;
    int32_t ya, yb;
    a ^= a >> 15; a *= 2246822519u; a ^= a >> 13;
    b ^= b >> 15; b *= 2246822519u; b ^= b >> 13;
    ya = (int32_t)(a >> 16) - 32768;
    yb = (int32_t)(b >> 16) - 32768;
    f = f * f * (768u - 2u * f) >> 16;                  /* smoothstep, 0..256 */
    return ya + (((yb - ya) * (int32_t)f) >> 8);
}
static uint32_t mns_clk;         /* (0.7.8) NOISE's clock: audio blocks (macro_master) */

/* source s of the track: the per-track ones, the per-voice ones of the latest note-on */
static int32_t mod_tsrc(const track_t *t, uint32_t s, int32_t lfo)
{
    switch (s) {
    case MS_LFO:
        return lfo;
    case MS_ENV:
        return t->m_env;
    case MS_VEL:
        return t->m_vel * 258;
    case MS_KEY:
        return mod_key(t->m_key);
    case MS_RAND:
        return t->m_rnd;
    case MS_MODW:
        return t->mw * 258;
    case MS_AT:
        return t->at * 258;
    case MS_EXPR:
        return (127 - t->ex_off) * 258;
    case MS_STEP:
        return t->ms_v;
    case MS_NOISE:                                      /* (0.7.8) a random wander, a new point every ~46 ms, eased */
        return drift_noise((uint32_t)(t - trk) * 7919u + 0x1234u, mns_clk, 6u - (CTL_LOG2 - 5u));
    default:
        return macro_v[(s - MS_M1) & 3u] * 258;
    }
}

/* AMP: the gain (Q15, 0..32767) of source x (bipolar: as 0..1) at amount a */
static int32_t mod_gain(uint32_t s, int32_t x, int32_t a)
{
    int32_t u = MS_BIPOLAR(s) ? (x + 32768) >> 1 : x;
    return clamp(32767 - (a > 0 ? ((32767 - u) * a) >> 6 : (u * -a) >> 6), 0, 32767);
}

/* a per-voice destination d by source value x, amount a */
static void mod_voice_add(uint32_t s, uint32_t d, int32_t x, int32_t a, int32_t *pit, int32_t *cut, int32_t *shp,
                          int32_t *gain)
{
    if (d == MD_PITCH)
        *pit += (x * a * 3) >> 15;
    else if (d == MD_CUT)
        *cut += (x * a) >> 7;
    else if (d == MD_SHP)
        *shp += (x * a) >> 7;
    else
        *gain = mulq15(*gain, mod_gain(s, x, a));
}

/* (JIANT) MS_STEP: the level of the step now towards t->ms_v, at SLEW's pace (0: at once; 127: ~1.5 s) */
static __attribute__((noinline)) void ms_tick(track_t *t)
{
    const int16_t *p = t->p;
    uint32_t per = div_samples((uint32_t)p[P_MSDIV]), len = p[P_MSLEN] > 0 ? (uint32_t)p[P_MSLEN] : 1u;
    int32_t target = p[P_MS0 + (per ? ms_clock / per % len : 0u)] * 258, s = p[P_MSSLW];
    if (!s) {
        t->ms_v = target;
        return;
    }
    t->ms_v += ((target - t->ms_v) * (32767 / (1 + s * s / (8 * (int32_t)CTL / 32)))) >> 15;   /* (a block of 32's step: 1 / (1 + s^2 / 8)) */
}

/* before a part's block (fx.c mix_part): the per-track offsets, the per-block destinations into t->p.
 * 0 = no slot active (then nothing changed) */
static __attribute__((noinline)) int mod_begin(track_t *t)
{
    mod_t *const mp = &MOD;                          /* (0.6.71) this core's */
    const int16_t *p = t->p;
    int32_t lfo, off[NMSLOT];
    uint32_t k, j;
    mp->on = 0;
    for (k = 0; k < NMSLOT; k++)
        if (p[P_M1SRC + 3u * k] && p[P_M1DST + 3u * k] && p[P_M1AMT + 3u * k])
            break;
    if (k == NMSLOT)
        return 0;
    lfo = mulq15(t->lfo_val, t->lfo_fade);              /* (the LFO of the block before: track_render ticks it) */
    for (j = k; j < NMSLOT; j++)
        if (p[P_M1SRC + 3u * j] == MS_STEP && p[P_M1AMT + 3u * j]) {
            ms_tick(t);                                 /* (once, for every slot reading it) */
            break;
        }
    mp->on = 1;
    mp->amp = 0;
    mp->nv = mp->nk = 0;
    mp->pit = mp->cut = mp->shp = 0;
    mp->gain = 32767;
    for (; k < NMSLOT; k++) {
        uint32_t s = (uint32_t)p[P_M1SRC + 3u * k], d = (uint32_t)p[P_M1DST + 3u * k];
        int32_t a = p[P_M1AMT + 3u * k], x;
        if (!s || !d || !a || s >= MS_N || d >= MD_CLIP)   /* (the master's: macro_master) */
            continue;
        if (d <= MD_AMP) {                              /* per voice */
            mp->amp |= d == MD_AMP;
            if (MS_VOICE(s)) {
                mp->vsrc[mp->nv] = (uint8_t)s;
                mp->vdst[mp->nv] = (uint8_t)d;
                mp->vamt[mp->nv++] = (int8_t)a;
            } else {
                mod_voice_add(s, d, mod_tsrc(t, s, lfo), a, &mp->pit, &mp->cut, &mp->shp, &mp->gain);
            }
            continue;
        }
        {                                               /* per block: AMT 64 = the whole range */
            uint32_t id = mod_param(d);
            const param_desc_t *pd = id >= P_E0 ? &ENGINES[t->engine]->edit[id - P_E0] : &TP[id];
            x = mod_tsrc(t, s, lfo);
            for (j = 0; j < mp->nk && mp->kid[j] != id; j++)
                ;
            if (j == mp->nk) {
                mp->kid[j] = (uint8_t)id;
                mp->keep[j] = p[id];
                off[j] = 0;
                mp->nk++;
            }
            off[j] += (((x * a) >> 6) * (id == P_LRATE ? 127 : pd->max - pd->min)) >> 15;   /* (0.6.51: RATE's free span,
                                                                                               * as before its in-time values) */
        }
    }
    for (j = 0; j < mp->nk; j++) {
        uint32_t id = mp->kid[j];
        const param_desc_t *pd = id >= P_E0 ? &ENGINES[t->engine]->edit[id - P_E0] : &TP[id];
        int32_t lo = pd->min, hi = pd->max;
        if (id == P_LRATE)                              /* (0.6.51) the LFO's RATE moved within its kind: free / in time */
            lo = mp->keep[j] < 0 ? lo : 0, hi = mp->keep[j] < 0 ? -1 : hi;
        t->p[id] = (int16_t)clamp(mp->keep[j] + off[j], lo, hi);
    }
    return 1;
}

/* (JIANT) once an audio block, before the parts (fx.c): CLIP and PUNCH as the master plays them, the stored
 * values plus every track's slots on them (per-voice sources: the latest note-on's value; LFO: the block before) */
static __attribute__((noinline)) void macro_master(void)
{
    int32_t off[MD_N - MD_CLIP] = {0};
    uint32_t i, k;
    mns_clk++;
    for (i = 0; i < NTRK; i++) {
        const track_t *t = &trk[i];
        for (k = 0; k < NMSLOT; k++) {
            uint32_t s = (uint32_t)t->p[P_M1SRC + 3u * k], d = (uint32_t)t->p[P_M1DST + 3u * k];
            int32_t a = t->p[P_M1AMT + 3u * k], v;
            if (!s || !a || s >= MS_N || d < MD_CLIP || d >= MD_N)
                continue;
            v = (mod_tsrc(t, s, mulq15(t->lfo_val, t->lfo_fade)) * a) >> 6;   /* Q15: AMT 64 at a full source = the range */
            if (d <= MD_PNCH)
                off[d - MD_CLIP] += (v * 100) >> 15;
            else {                                      /* (0.7.0) an FX bus global: its whole range */
                const param_desc_t *pd = &GP[GFX_ID[d - MD_DTIME]];
                off[d - MD_CLIP] += (v * (d == MD_DTIME ? DT_FREE + 47 : pd->max - pd->min)) >> 15;   /* (0.8.4: TIME's
                                                         * range as it was, its free times since appended: a slot's
                                                         * depth the same) */
            }
        }
    }
    clip_eff = (int16_t)clamp(song.g[G_CLIP] + off[0], 0, 100);
    pnch_eff = (int16_t)clamp(song.g[G_PUNCH] + off[1], 0, 100);
    for (k = 0; k < 8u; k++)
        gfx_off[k] = (int16_t)clamp(off[MD_DTIME - MD_CLIP + k], -32000, 32000);
}

/* after the part's block (when MOD.on): the stored values back */
static __attribute__((noinline)) void mod_end(track_t *t)
{
    mod_t *const mp = &MOD;                          /* (0.6.71) this core's */
    uint32_t j;
    for (j = 0; j < mp->nk; j++)
        t->p[mp->kid[j]] = mp->keep[j];
    mp->on = 0;
}

/* a voice's modulation (voice.c track_render, when MOD.on), onto its vmod_t m as track_render made it:
 * pitch (the increment again, with the fine factor fine as there), cutoff and shape (clamped to what the
 * fixed routings can reach), the amplitude (and v->env_out, the next block's start). env: its envelope */
static __attribute__((noinline)) void mod_voice(track_t *t, voice_t *v, vmod_t *m, int32_t fine)
{
    mod_t *const mp = &MOD;                          /* (0.6.71) this core's */
    int32_t pit = mp->pit, cut = mp->cut, shp = mp->shp, gain = mp->gain, env = m->envq15;
    uint32_t k;
    if (v == &t->v[t->m_vi])
        t->m_env = env;                                 /* ENV of the latest note, for per-block destinations */
    for (k = 0; k < mp->nv; k++) {
        uint32_t s = mp->vsrc[k];
        int32_t x = s == MS_ENV ? env : s == MS_VEL ? v->mvel * 258 : s == MS_KEY ? mod_key(v->note) : v->mrnd;
        mod_voice_add(s, mp->vdst[k], x, mp->vamt[k], &pit, &cut, &shp, &gain);
    }
    if (pit) {
        m->pitch16 = clamp(m->pitch16 + pit, 0, 2047);
        m->inc = pitch_inc(m->pitch16);
        if (fine)
            m->inc += (uint32_t)((int32_t)(m->inc >> 12) * fine);
    }
    m->cutoff = clamp(m->cutoff + cut, -150 * 256, 150 * 256);
    m->shape = clamp(m->shape + shp, -62 * 256, 190 * 256);
    if (mp->amp) {
        m->amp1 = mulq15(m->amp1, gain);
        v->env_out = m->amp1;
    }
}

/* a note-on of track t (voice.c trk_note_on): the per-voice sources of the latest note */
static void mod_note(track_t *t, uint32_t note, uint32_t vel)
{
    t->m_vel = (uint8_t)vel;
    t->m_key = (uint8_t)note;
    t->m_rnd = mod_rand();
}

/* a MIDI controller for track t (seq.c events_block): CC1 / CC11 / channel aftertouch; CC121 resets them */
static __attribute__((noinline)) void mod_midi(track_t *t, uint32_t st, uint32_t d1, uint32_t d2)
{
    if (st == 0xD0u) {
        t->at = (uint8_t)d1;
    } else if (st == 0xB0u) {
        if (d1 == 1u)
            t->mw = (uint8_t)d2;
        else if (d1 == 11u)
            t->ex_off = (uint8_t)(127u - d2);
        else if (d1 == 121u)
            t->mw = t->at = t->ex_off = 0;
    }
}

/* the name of destination value v for track t (UI, <= 5 characters): E1..E8 by the engine's labels (of the
 * engine the UI shows), "E5" for a parameter the engine does not use */
static const char *mod_dst_name(const track_t *t, int32_t v)
{
    v = clamp(v, 0, MD_N - 1);
    if (v >= MD_E1 && v < MD_CLIP) {
        const char *l = track_desc(t, P_E0 + (uint32_t)(v - MD_E1))->label;
        if (l && l[0] && l[0] != '-')
            return l;
    }
    return N_MDST[v];
}
