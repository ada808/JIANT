/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Voice allocation, envelopes, LFO and per-track rendering.
 * Runs in the audio ISR.
 *
 * Each synth part has its own NVOICE voices (so MONO / LEGATO / UNISON keep using
 * v[0..]), but all parts share a measured-load budget (8 by default, up to 12 in FULL): a part that starts
 * a voice while the budget is full takes one from any part (voice_victim): the oldest
 * released voice, else the oldest extra UNISON voice, else the oldest held voice of a
 * POLY part that is not its lowest note. A voice taken from another part fades out
 * over one block (stage 4: the envelope goes to 0, the block's amplitude ramp
 * declicks it); one of the part's own is restarted in place, as before. Extra UNISON
 * voices only start when there is room. */
static uint32_t vage;                                   /* voice ages: one clock for every part */
static int32_t lfo_wave(track_t *t, uint32_t ph)
{
    switch (t->p[P_LWAVE]) {
    case 1:
        return osc_tri(ph);
    case 2:
        return (int32_t)(ph >> 16) - 32768;
    case 3:
        return ph < 0x80000000u ? 32767 : -32767;
    case 4:
        return (int32_t)(t->lfo_rnd >> 16) - 32768;
    default:
        return osc_sine(ph);
    }
}

/* the S&H value of a new LFO cycle. Parts 1..3 draw it from the shared generator (rng); part 4 has its
 * own xorshift, so the shared sequence, which also seeds the engines' noise (PHYS, VOICE, ..), does not
 * depend on part 4's LFO (the golden renders rely on it) */
#define LFO_SHARED_RNG 3u
static uint32_t lfo_rand(track_t *t)
{
    uint32_t s = t->lfo_rnd ? t->lfo_rnd : 0x9E3779B9u;
    if ((uint32_t)(t - trk) < LFO_SHARED_RNG)
        return rng();
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

/* (JIANT 0.5) ANALOG's FILTER TYPE COMB: the part's sum through a feedback comb, tuned to its newest note moved by
 * CUT (+-32 semitones; 64: the note: a resonance at the pitch), RES its feedback; the level kept about the dry's. One
 * line a part (1024 samples: down to ~43 Hz).
 * (0.9.9-snd1, the owner: "make it musical") RES the ring's length, the same on every note (T60 25 ms .. ~9 s on an
 * exponential curve: most of the travel where it rings, near self-oscillation at the top), a soft clip inside the loop (however long the ring, bounded: no full-scale lockup), a
 * damping low-pass in the loop (RES opens it, ~3.5 .. ~12 kHz: a string's highs dying first; the line shortened by its
 * delay, in tune), the line's length and the feedback glided over the block (CUT turned or a new note: no zipper, was a
 * step a block), COMB- (neg) the feedback negative over half the line: the odd harmonics of the note, hollow. The wet
 * level by 1 - fb as before, at least a quarter: the clipped ring heard at the top of RES */
#define COMB_LEN 1024u
static int16_t comb_buf[NTRK][COMB_LEN] __attribute__((section(".pool")));   /* (0.6: six tracks, in the pool) */
static uint16_t comb_w[NTRK];
static int16_t comb_p16[NTRK];
static uint32_t comb_pq[NTRK];                          /* (snd1) the line's length at the block's end, Q16 (0: new) */
static int32_t comb_fb[NTRK], comb_lp[NTRK];            /* .. the feedback there (signed, Q15), the damping's state */
static __attribute__((noinline)) void track_comb(track_t *t, int32_t *out, uint32_t n, int32_t cut, int32_t res,
                                                 uint32_t neg)
{
    uint32_t ti = (uint32_t)(t - trk) % NTRK, i, per, age = 0, w = comb_w[ti], pq, pq1;
    int32_t p16 = comb_p16[ti], r = clamp(res, 0, 127), lp = comb_lp[ti], dq, dfb, fbc, gn, fb, a, m, xq;
    int32_t dmp = 6400 + r * 55;                        /* (the damping's pole, Q14) */
    int16_t *b = comb_buf[ti];
    for (i = 0; i < NVOICE; i++)                        /* the newest note sounding */
        if (t->v[i].active && t->v[i].age >= age) {
            age = t->v[i].age;
            p16 = t->v[i].pitch_cur;
        }
    comb_p16[ti] = (int16_t)p16;
    p16 = clamp(p16 + (cut - 64) * 8, 0, 2047);
    per = 0xFFFFFFFFu / ((pitch_inc((uint32_t)p16) >> 8) | 1u);   /* the period, Q8 */
    if (neg)
        per >>= 1;                                      /* (the negative feedback: half a period, the note's odd harmonics) */
    per -= (uint32_t)(((16384 - dmp) << 8) / dmp);      /* (the damping's delay at low frequencies: in tune) */
    per = (int32_t)per < (2 << 8) ? 2u << 8 : per > ((COMB_LEN - 2u) << 8) ? (COMB_LEN - 2u) << 8 : per;
    /* the feedback for a ring of T60 = 25 ms x 2^(8.5 RES / 127) (25 ms .. ~9 s) whatever the note: fb = e^-a, a =
     * 6.9 per / T60 (2^-x linear between octaves, e^-a by its (2,2) Pade, a at most 4); at most 0.998 */
    xq = r * 2176 / 127;                                /* 8.5 RES / 127, Q8 */
    m = (65536 - ((xq & 255) << 7)) >> (xq >> 8);       /* 2^-x, Q16 */
    a = (int32_t)(((per >> 4) * 6566u) >> 12);          /* per / 25 ms x 6.9, Q12 (6566: 6.9 / 1102.5 Q20) */
    a = (a * (m >> 1)) >> 15;
    a = a > 4 * 4096 ? 4 * 4096 : a;
    {
        int32_t a2 = (a * a) / 12 >> 12, num = 4096 - a / 2 + a2, den = 4096 + a / 2 + a2;
        fb = (num << 15) / den;
    }
    fb = fb > 32700 ? 32700 : fb;
    pq1 = per << 8;
    pq = comb_pq[ti] ? comb_pq[ti] : pq1;
    fb = neg ? -fb : fb;
    fbc = comb_pq[ti] ? comb_fb[ti] : fb;
    dq = ((int32_t)pq1 - (int32_t)pq) / (int32_t)(n ? n : 1u);
    dfb = (fb - fbc) / (int32_t)(n ? n : 1u);
    gn = 2 * (32768 - (fb < 0 ? -fb : fb));
    gn = gn < 8192 ? 8192 : gn;
    for (i = 0; i < n; i++) {
        uint32_t d;
        int32_t a, c, x, y;
        pq += (uint32_t)dq;
        fbc += dfb;
        d = pq >> 16;
        a = b[(w - d) & (COMB_LEN - 1u)];
        c = b[(w - d - 1u) & (COMB_LEN - 1u)];
        x = (a + (((c - a) * (int32_t)((pq >> 8) & 255u)) >> 8)) << 1;
        lp += ((x - lp) * dmp) >> 14;                   /* the damping (|x - lp| < 2^17, dmp < 2^14) */
        y = soft_knee(clamp(out[i] + mulq15(lp, fbc), -262000, 262000), 20000);   /* the loop's clip: |y| < 36400 */
        b[w & (COMB_LEN - 1u)] = (int16_t)(y >> 1);
        w++;
        out[i] = soft_knee(((y * gn) >> 14) + (out[i] >> 2), 16000);   /* (under full scale: was 24000, ~40000) */
    }
    comb_w[ti] = (uint16_t)w;
    comb_pq[ti] = pq1;
    comb_fb[ti] = fb;
    comb_lp[ti] = lp;
}

/* (JIANT 0.5) the FILTER of every engine but ANALOG (its own, per voice): the part's sum through a drive (DRV: x1 ..
 * x4 into the soft clip, the level kept) and a state-variable filter, LP HP BP at CUT (moved by the LFO and the loudest
 * voice's envelope through LFO DEST / ENV DEST FLT) and RES; COMB the comb above (CUT about the newest note, RES its
 * feedback). LP open (127), no drive: out of the chain.
 * (JIANT 0.6.8, the crackles turning CUT / RES) going in and out of the chain, and from one TYPE to another, crossfade
 * over the block, the state taken from the input on the way in (it was stale: a step each time CUT or RES crossed the
 * open LP) */
static int32_t tf_ic1[NTRK], tf_ic2[NTRK];
static int32_t tf_side[NTRK][4];                /* (0.6.81) a DRUM part's IMAGE sides' filter states (L ic1 ic2, R) */
/* (0.6.81) a DRUM part's IMAGE: what each side gives up (eng_drum.c dx_sl dx_sr) filtered as the part is, or the side
 * taken out of the filtered sum was unfiltered: its highs back in raw, a grit like a bitcrusher. The same filter, linear,
 * in and out with the part's fades */
static __attribute__((noinline)) void tf_sides(uint32_t ti, const tsvf_t *c, int32_t k, uint32_t ft, uint32_t was,
                                               uint32_t on, uint32_t n)
{
    uint32_t sd, i;
    for (sd = 0; sd < 2u; sd++) {
        int32_t *b = sd ? dx_sr : dx_sl, ic1 = tf_side[ti][sd * 2u], ic2 = tf_side[ti][sd * 2u + 1u];
        if (!was) {
            ic1 = 0;
            ic2 = b[0];
        }
        for (i = 0; i < n; i++) {
            int32_t x = b[i], y, w = (int32_t)((i + 1u) << (12 - CTL_LOG2));
            int32_t v3 = x - ic2, v1 = (c->a1 * ic1 + c->a2 * v3) >> 13, v2 = ic2 + ((c->a2 * ic1 + c->a3 * v3) >> 13);
            ic1 = clamp(2 * v1 - ic1, -150000, 150000);
            ic2 = clamp(2 * v2 - ic2, -150000, 150000);
            y = ft == FT_HP ? x - v2 - ((k * v1) >> 12) : ft == FT_BP ? (k * v1) >> 12 : v2;
            if (!was)
                y = x + (((y - x) * w) >> 12);
            else if (!on)
                y = y + (((x - y) * w) >> 12);
            b[i] = y;
        }
        tf_side[ti][sd * 2u] = ic1;
        tf_side[ti][sd * 2u + 1u] = ic2;
    }
}
static uint8_t tf_on[NTRK], tf_ft[NTRK];        /* the SVF in the chain the last block; the TYPE playing (0.6.71: a
                                                 * byte a track, not a bit: two cores write two tracks at once) */
static uint8_t tf_cmb[NTRK];                    /* (0.6.81) the COMB ran the last block (its line live) */
/* (0.9.9-snd1) TYPE MS20, the owner's "more aggressive filter, the Korg MS-20's": a 2-pole low-pass whose resonance is a
 * positive feedback through a diode-like clip (the Sallen-Key's, the MS-20's: screaming, and controlled, the clip
 * setting how loud it can ring). Here the trapezoidal SVF (dsp.c) held at damping 2 (no resonance of its own: the
 * MS-20's soft knee at RES 0) and its band-pass fed back into the input through softclip (half full scale): its small-
 * signal damping 2 - kn, RES exponential to it (2 .. 0.03: Q 0.5 .. 33, a scream at the top that still dies out when
 * the part is silent: no whistle left on its own), a hot input clipped in the loop (the MS-20's growl). The clip's feedback a sample late (explicit: the
 * linear part stable, the clip bounded: bounded however it is driven). CUT, LFO / ENV DEST FILTER as the SVF's; on
 * ANALOG and LOFI the part's (their own filter open), at ANALOG's CUT RES (E4 E5) */
static uint8_t tf_ms[NTRK];                     /* (snd1) MS20 ran the last block (its state live) */
static int32_t ms_s[NTRK][3];                   /* .. its state: the SVF's ic1 ic2, the band-pass a sample back */
static __attribute__((noinline)) void tf_ms20(uint32_t ti, int32_t *out, uint32_t n, int32_t cut, int32_t res)
{
    int32_t ic1 = ms_s[ti][0], ic2 = ms_s[ti][1], bp = ms_s[ti][2], r = clamp(res, 0, 127), kn, xq;
    uint32_t i;
    tsvf_t c;
    if (!tf_ms[ti]) {                                   /* in: from rest at the input (no step) */
        ic1 = bp = 0;
        ic2 = out[0];
    }
    tsvf_coef_k(&c, cut, 8192);
    xq = r * 1536 / 127;                                /* the damping left 2 x 2^-(6 RES / 127): 2 .. 0.03 (Q 0.5 .. 33) */
    kn = 8192 - (((8192 * (65536 - ((xq & 255) << 7))) >> 16) >> (xq >> 8));   /* Q12 */
    for (i = 0; i < n; i++) {
        int32_t x = clamp(out[i] + ((kn * softclip(bp * 2)) >> 13), -60000, 60000);   /* the diode's feedback */
        int32_t v3 = x - ic2, v1 = (c.a1 * ic1 + c.a2 * v3) >> 13, v2 = ic2 + ((c.a2 * ic1 + c.a3 * v3) >> 13);
        ic1 = clamp(2 * v1 - ic1, -150000, 150000);
        ic2 = clamp(2 * v2 - ic2, -150000, 150000);
        bp = v1;
        out[i] = soft_knee(v2 - (v2 >> 2), 16000);      /* (3/4: its ring about the dry's level, under full scale) */
    }
    ms_s[ti][0] = ic1;
    ms_s[ti][1] = ic2;
    ms_s[ti][2] = bp;
    tf_ms[ti] = 1;
}
static uint8_t tf_xb[NTRK], tf_fo[NTRK];        /* (0.6.81) a TYPE's crossfade: its blocks left, the TYPE from (TF_DRY:
                                                 * no filter) */
#define TF_XB (256u / CTL)                      /* (~5 ms: one block's fade between two filters was a tick) */
#define TF_DRY 0xFFu
/* the filter of TYPE ft on a part's block; fo (not TF_DRY, an SVF TYPE): from fo's output to ft's, w0 + (i + 1) dw
 * (Q12) of the way (the SVF's three outputs from one state); own: the engine filters its notes itself (ANALOG, LOFI):
 * only the COMB here, at cut / res */
static int32_t drum_env(track_t *t);                    /* (gen6d: DRUM-X's ENV, below) */
static __attribute__((noinline)) void tf_run(track_t *t, int32_t *out, uint32_t n, uint32_t ft, uint32_t fo, int32_t w0,
                                             int32_t dw, uint32_t own, int32_t ccut, int32_t cres)
{
    const int16_t *p = t->p;
    uint32_t ti = (uint32_t)(t - trk) % NTRK, i;
    int32_t cut = ccut << 8, envq = 0, ic1 = tf_ic1[ti], ic2 = tf_ic2[ti], k;   /* (snd1: ccut, P_FCUT but on
                                                         * ANALOG, whose own filter returned above but for MS20) */
    tsvf_t c;
    if (FT_IS_COMB(ft)) {
        if (!tf_cmb[ti]) {                              /* (0.6.81) in: its line cleared (the last time's, stale) */
            memset(comb_buf[ti], 0, sizeof comb_buf[ti]);
            comb_pq[ti] = 0;                            /* (snd1: no glide in, its damping from rest) */
            comb_lp[ti] = 0;
        }
        track_comb(t, out, n, ccut, cres, ft == FT_COMBN);
        tf_cmb[ti] = 1;
        return;
    }
    if (own && ft != FT_MS20)
        return;
    if (t->engine == ENGI_DRUM)
        envq = (uint16_t)drum_env(t) + 1;                   /* (gen6d: the kit's ENV, its voices held at full) */
    else
        for (i = 0; i < NVOICE; i++)
            if (t->v[i].active && t->v[i].stage && t->v[i].stage < 4u && (t->v[i].env >> 9) > envq)
                envq = t->v[i].env >> 9;
    {   /* (0.6.81) the sweep kept where it is heard: CUT moved down so its top stays open at most (an open filter swept
         * up did nothing: half an LFO's cycle, a whole ENV, lost) */
        int32_t span = (p[P_LD_FLT] < 0 ? -p[P_LD_FLT] : p[P_LD_FLT]) * 256 + (p[P_ED_FLT] > 0 ? p[P_ED_FLT] * 256 : 0);
        if (cut + span > (127 << 8))
            cut = (127 << 8) - span > 0 ? (127 << 8) - span : 0;
    }
    cut += ((mulq15(t->lfo_val, t->lfo_fade) * p[P_LD_FLT]) >> 7) + ((envq * p[P_ED_FLT]) >> 7);
    if (ft == FT_MS20) {
        tf_ms20(ti, out, n, cut, cres);
        return;
    }
    {
        uint32_t on = !(ft == FT_LP && fo == TF_DRY && cut >= (127 << 8) && !p[P_FRES]), was = tf_on[ti];
        if (!on && !was)
            return;                                     /* (open: nothing to filter) */
        if (!was) {                                     /* in: the state as the input (no step) */
            ic1 = 0;
            ic2 = out[0];
        }
        tsvf_coef(&c, cut, p[P_FRES]);
        k = 8192 - p[P_FRES] * 7600 / 127;
        for (i = 0; i < n; i++) {
            int32_t x = out[i], y, w = (int32_t)((i + 1u) << (12 - CTL_LOG2));   /* 0 .. 1 over the block, Q12 */
            int32_t v3 = x - ic2, v1 = (c.a1 * ic1 + c.a2 * v3) >> 13, v2 = ic2 + ((c.a2 * ic1 + c.a3 * v3) >> 13);
            ic1 = clamp(2 * v1 - ic1, -150000, 150000);
            ic2 = clamp(2 * v2 - ic2, -150000, 150000);
            y = ft == FT_HP ? x - v2 - ((k * v1) >> 12) : ft == FT_BP ? (k * v1) >> 12 : v2;
            if (fo != TF_DRY) {                         /* TYPE changed: from the old one's output */
                int32_t y0 = fo == FT_HP ? x - v2 - ((k * v1) >> 12) : fo == FT_BP ? (k * v1) >> 12 : v2;
                y = y0 + (((y - y0) * (w0 + (int32_t)(i + 1u) * dw)) >> 12);
            }
            y = soft_knee(y, 30000);                    /* (the dry part has no knee: the fades after it) */
            if (!was)
                y = x + (((y - x) * w) >> 12);          /* in */
            else if (!on)
                y = y + (((x - y) * w) >> 12);          /* out */
            out[i] = y;
        }
        if ((dx_xon & 1u) && ENGINES[t->engine] == &ENG_DRUM)
            tf_sides(ti, &c, k, ft, was, on, n > CTL ? CTL : n);
        tf_on[ti] = (uint8_t)on;
    }
    tf_ic1[ti] = ic1;
    tf_ic2[ti] = ic2;
}

/* the FILTER after the voices. (0.6.81) A TYPE turned crossfades over TF_XB blocks from the one playing (the TYPE held
 * until it is done): between LP HP BP inside the SVF; into or out of the COMB (or into it from no filter) both run, the
 * old on a copy (was: one block, and the COMB left out: a step each way) */
static int32_t tf_tmp[CTL];
static __attribute__((noinline)) void track_filter(track_t *t, int32_t *out, uint32_t n, uint32_t own, int32_t ccut,
                                                   int32_t cres)
{
    uint32_t ti = (uint32_t)(t - trk) % NTRK, r = ft_of(t->p), ft, fo, i;
    uint32_t act = (tf_on[ti] && !own) || tf_cmb[ti];
    int32_t w0, dw;
    if (own && !FT_POST(r))
        r = FT_LP;                                      /* (an engine's own filter: the part's TYPEs or nothing here) */
    if (!tf_xb[ti]) {
        if (r != tf_ft[ti] && (act || FT_POST(r)) && !(FT_IS_COMB(r) && FT_IS_COMB(tf_ft[ti]))) {
                                                        /* (snd1: COMB <-> COMB- in the one line, its feedback glided) */
            tf_fo[ti] = (uint8_t)(act ? tf_ft[ti] : TF_DRY);
            tf_xb[ti] = TF_XB;
        }
        tf_ft[ti] = (uint8_t)r;
    }
    ft = tf_ft[ti];
    if (!tf_xb[ti]) {
        tf_run(t, out, n, ft, TF_DRY, 0, 0, own, ccut, cres);
        tf_cmb[ti] = FT_IS_COMB(ft);
        tf_ms[ti] = ft == FT_MS20;
        return;
    }
    if (n > CTL)
        n = CTL;
    fo = tf_fo[ti];
    w0 = (int32_t)(((TF_XB - tf_xb[ti]) << 12) / TF_XB);
    dw = (4096 / (int32_t)TF_XB) >> CTL_LOG2;
    if (fo != TF_DRY && !FT_POST(fo) && !FT_POST(ft) && !own) {
        tf_run(t, out, n, ft, fo, w0, dw, own, ccut, cres);   /* (one SVF, two of its outputs) */
    } else {
        for (i = 0; i < n; i++)
            tf_tmp[i] = out[i];
        if (fo != TF_DRY)
            tf_run(t, tf_tmp, n, fo, TF_DRY, 0, 0, own, ccut, cres);   /* the old one */
        tf_run(t, out, n, ft, TF_DRY, 0, 0, own, ccut, cres);
        for (i = 0; i < n; i++)
            out[i] = tf_tmp[i] + (((out[i] - tf_tmp[i]) * (w0 + (int32_t)(i + 1u) * dw)) >> 12);
    }
    if (!--tf_xb[ti]) {
        tf_cmb[ti] = FT_IS_COMB(ft);
        tf_ms[ti] = ft == FT_MS20;
    }
}

/* the FILTER after the voices: ANALOG's and LOFI's COMB here (their own filters are per voice), every other engine's
 * whole filter (track_filter). Its own function: the audio ISR's loop stays as it was */
static __attribute__((noinline)) void track_post(track_t *t, const engine_t *e, int32_t *out, uint32_t n)
{
    if (e == &ENG_ANALOG)
        track_filter(t, out, n, 1, t->p[P_E4], t->p[P_E5]);
    else
        track_filter(t, out, n, e == &ENG_LOFI, t->p[P_FCUT], t->p[P_FRES]);
}

static uint32_t beat_samples(void);                     /* (fx.c) */
static void track_lfo_tick(track_t *t)
{
    uint32_t old = t->lfo_ph;
    int32_t r = t->p[P_LRATE];
    if (r < 0) {                                        /* (JIANT 0.6.51) in time: 4BAR .. 1/32 (params.c N_LSYNC), on
                                                         * the transport's grid while it plays (PHASE its offset), free
                                                         * while it is stopped */
        static const uint16_t U[12] = {384, 192, 96, 48, 24, 18, 16, 12, 8, 6, 4, 3};   /* 1/24 of a quarter */
        uint32_t len = beat_samples() * U[(uint32_t)(-r - 1) % 12u] / 24u;
        if (len < 64u)
            len = 64u;
        if (song.playing)
            t->lfo_ph = (uint32_t)(((uint64_t)(ms_clock % len) << 32) / len) +
                        (t->p[P_LPHASE] > 0 ? (uint32_t)t->p[P_LPHASE] << 25 : 0u);
        else
            t->lfo_ph += (uint32_t)(((uint64_t)CTL << 32) / len);
    } else
        t->lfo_ph += LFO_INC[r & 127];
    if (t->lfo_ph < old)
        t->lfo_rnd = lfo_rand(t);
    t->lfo_val = lfo_wave(t, t->lfo_ph);
    if (t->lfo_fade < 32767) {
        int32_t step = (int32_t)(ENV_LIN[t->p[P_LFADE] & 127] >> 9);
        t->lfo_fade = t->p[P_LFADE] ? clamp(t->lfo_fade + (step ? step : 1), 0, 32767) : 32767;
    }
}

/* (0.9.18) the LFOs' FADE from 0 again: a project loaded (project.c project_restore_runtime), the power-on (lfo_fade 0
 * already), a track's MODE turned to FREE (ui_input.c knob_turn: the one track). MODE TRIG fades on each fresh phrase
 * anyway (trk_note_on); FREE only then */
static void lfo_fade_restart(uint32_t mask)
{
    uint32_t k;
    for (k = 0; k < NTRK; k++)
        if ((mask >> k) & 1u)
            trk[k].lfo_fade = 0;
}

/* voices the engine may use (POLY and UNISON): its cap, else all of them */
/* (JIANT 0.6.29, 0.6.30) GLO > SYSTEM > LOAD (MENU > SYSTEM > LOAD): how much of the CPU the heavy parts may take,
 * swapped while they sound, the project keeping its settings (ui.c ui_load, rp_apply). FULL: all as set. LITE: their
 * light kin: FM6 two voices, the reverb SHIMMER / CLOUD as ROOM, RESO as SPRING, the GRAIN delay plain (fx.c). ECO:
 * LITE, and every reverb a SPRING (0.6.70: its chain shortened, fx.c SP_N_ECO), no chorus, no SLICER (slicer.c: its
 * tracks straight to the mix, its pages hidden), at most ECO_POLY voices a track (an engine of hits, DRUM, as ever),
 * ECO_VOICES between all (voice_cap), the load budget sooner (audio.c TIGHT_ON_ECO) */
enum { LOAD_FULL, LOAD_LITE, LOAD_ECO };
static volatile uint8_t sys_load;
#define LITE_FM6_POLY 2u
#define ECO_POLY 4u
#define ECO_VOICES 6u            /* (0.6.70) LOAD ECO: the voices all parts share (NVOICE else) */
static uint32_t trk_nvoice(const track_t *t)
{
    uint32_t c = ENGINES[t->engine]->poly;
    if (sys_load && t->engine == ENGI_FM6 && c > LITE_FM6_POLY)
        c = LITE_FM6_POLY;
    if (sys_load == LOAD_ECO && !ENGINES[t->engine]->oneshot && (!c || c > ECO_POLY))
        c = ECO_POLY;
    return c && c < NVOICE ? c : NVOICE;
}

/* the part's voice mode: P_VOICE, POLY for an engine of hits (engine_t.oneshot: DRUM) */
static uint32_t trk_vmode(const track_t *t)
{
    return ENGINES[t->engine]->oneshot ? V_POLY : (uint32_t)t->p[P_VOICE];
}

/* ------------------------------------------------- the shared voice budget --- */
/* (JIANT 0.6.16) the load budget: while it is set the sound may not grow: a new note takes a voice instead of adding
 * one (voice_alloc, voice_room: a dying tail, or at the guard's cap the oldest released), CLOUD spawns half its grains
 * (fx.c). Ahead of a late half (a click, audio.c shed_voice): the load stops climbing before it gets there.
 * (0.9.9 cpu5, after LOOM-8's engine.c) It was set by one half over 65 % and cleared after 16 under 60 %: a 2.9 ms
 * half's share is noisy (a chord's note-ons, a cache miss, a UI frame nested) and one spike closed the budget for
 * 50 ms. Now the guard (load_half) trips on the average of the render's share over ~93 ms past GUARD_ON_AVG, or on
 * GUARD_RUN halves in a row past GUARD_ON_RUN (a load that keeps the audio late); a single spike never trips it.
 * Tripped, it caps the voices at what sounds (GUARD_MIN at least: a lead a track) and lets go a step at a time: a
 * voice more for each GUARD_STEP of calm under GUARD_OFF with no such run, until the cap is the budget's again. LOAD
 * ECO: the same sooner (the user's "room kept for the peaks") */
static volatile uint8_t load_tight;
#define GUARD_AVG_LOG2 5u                               /* the average: ~32 halves, 93 ms */
#define GUARD_ON_AVG 85u                                /* FULL / LITE: the average past this .. */
#define GUARD_ON_RUN 92u                                /* .. or GUARD_RUN halves in a row past this */
#define GUARD_OFF 70u                                   /* calm: the average under this, no half past GUARD_ON_RUN */
#define GUARD_ON_AVG_ECO 65u
#define GUARD_ON_RUN_ECO 75u
#define GUARD_OFF_ECO 50u
#define GUARD_RUN 3u
#define GUARD_STEP ((uint32_t)FS / 2u / HALF_FRAMES)    /* halves of calm a voice given back: 0.5 s */
#define GUARD_MIN 4u                                    /* the cap never under this: a lead for each of 4 tracks */
static uint32_t load_avg_q8, load_all_q8;               /* the render's share of a half (%, Q8), averaged; with TIMER5
                                                         * nested in it (audio.c: the extra voices' admission) */
static uint32_t load_pred_q8;                           /* the admission's prediction: the average, plus what the
                                                         * voices admitted since add (load_admit) */
static uint8_t guard_run, guard_cap, load_cores2;       /* halves in a row past GUARD_ON_RUN; the cap while tripped
                                                         * (0: none: the budget's); both cores render */
static uint32_t guard_calm, guard_trips;                /* halves of calm toward the next voice back; trips (console) */
/* Extra shared voices are admitted only after the audio ISR establishes
 * sustained deadline headroom on two healthy cores. No extra per-part slots. */
static uint8_t voice_headroom;
#define FULL_VOICES 12u
/* (0.6.17) what the audio did since power-on, for MENU > ABOUT (audio.c fills it): the slowest half's share of its
 * time (%), the halves that came late (each a click), the voices the overload guard faded */
static struct { volatile uint32_t peak, late, shed; } load_diag;
/* (JIANT 0.6.70) the budget shared by all parts: NVOICE, LOAD ECO ECO_VOICES (4 parts with reverb and delay still
 * crackled on ECO: the peaks are the voices, a tail ringing on each part as the next hits come) */
static uint32_t voice_cap0(void)                        /* the budget as set (LOAD, the extra voices) */
{
    return sys_load == LOAD_ECO ? ECO_VOICES :
           sys_load == LOAD_FULL && voice_headroom ? FULL_VOICES : NVOICE;
}
static uint32_t voice_cap(void)                         /* .. under the guard's cap while it is tripped */
{
    uint32_t c = voice_cap0();
    return load_tight && guard_cap && guard_cap < c ? guard_cap : c;
}
static uint32_t voices_busy(void)                       /* sounding voices of all parts (not the fading ones) */
{
    uint32_t p, i, n = 0;
    for (p = 0; p < NPART; p++)
        for (i = 0; i < NVOICE; i++)
            n += trk[p].v[i].active && trk[p].v[i].stage != 4u;
    return n;
}

/* the end of a half (audio.c shed_check): pct the render's share of it (%), pct_all with TIMER5 nested in it, cores2
 * both cores rendering. The guard above; the admission's prediction starts again from the average */
static __attribute__((noinline)) void load_half(uint32_t pct, uint32_t pct_all, uint32_t cores2)
{
    uint32_t eco = sys_load == LOAD_ECO;
    uint32_t on_avg = eco ? GUARD_ON_AVG_ECO : GUARD_ON_AVG, on_run = eco ? GUARD_ON_RUN_ECO : GUARD_ON_RUN;
    uint32_t off = eco ? GUARD_OFF_ECO : GUARD_OFF;
    load_avg_q8 = (load_avg_q8 * ((1u << GUARD_AVG_LOG2) - 1u) + (pct << 8) + (1u << (GUARD_AVG_LOG2 - 1u))) >> GUARD_AVG_LOG2;
    load_all_q8 = (load_all_q8 * ((1u << GUARD_AVG_LOG2) - 1u) + (pct_all << 8) + (1u << (GUARD_AVG_LOG2 - 1u))) >> GUARD_AVG_LOG2;
    load_cores2 = (uint8_t)cores2;
    guard_run = pct > on_run ? (uint8_t)(guard_run < 255u ? guard_run + 1u : 255u) : 0u;
    if (guard_run >= GUARD_RUN || load_avg_q8 > (on_avg << 8)) {
        uint32_t busy = voices_busy(), cap = voice_cap0();
        if (busy < GUARD_MIN) busy = GUARD_MIN;
        if (busy > cap) busy = cap;
        if (!load_tight || !guard_cap || busy < guard_cap) {   /* a trip, or one on the way back: down to what sounds */
            guard_cap = (uint8_t)busy;
            guard_trips++;
        }
        load_tight = 1;
        guard_calm = 0;
    } else if (load_tight) {
        if (load_avg_q8 >= (off << 8) || guard_run) {
            guard_calm = 0;
        } else if (++guard_calm >= GUARD_STEP) {        /* calm: a voice back, and so on */
            guard_calm = 0;
            if (!guard_cap || ++guard_cap >= voice_cap0()) {
                guard_cap = 0;
                load_tight = 0;
            }
        }
    }
    if (load_pred_q8 < load_avg_q8)
        load_pred_q8 = load_avg_q8;                     /* (what the voices added decays by the block: load_block) */
}

/* Key-up ends a melodic hold, but a one-shot drum is still a live hit.
 * Allocation/shedding must not confuse its key-up with a disposable release. */
static int voice_held(const track_t *t, const voice_t *v)
{
    return v->gate || ENGINES[t->engine]->oneshot;
}

static uint32_t lowest_held(const track_t *t)           /* index of the lowest held note (the bass), NVOICE = none */
{
    uint32_t i, low = NVOICE;
    for (i = 0; i < NVOICE; i++)
        if (t->v[i].active && t->v[i].stage != 4u && voice_held(t, &t->v[i]) && (low == NVOICE || t->v[i].note < t->v[low].note))
            low = i;
    return low;
}

/* the voice to give up for a new one (see the top); soft: only released voices and
 * extra voices of other non-POLY parts (also ones left after a mode change).
 * Returns its index, *pp its part; NVOICE = none */
static uint32_t voice_victim(const track_t *self, int soft, track_t **pp)
{
    uint32_t p, i, best = NVOICE, cat = 4;
    for (p = 0; p < NPART; p++) {
        track_t *t = &trk[p];
        uint32_t mode = trk_vmode(t);
        uint32_t low = mode == V_POLY ? lowest_held(t) : NVOICE;
        if (soft && t == self)
            continue;
        for (i = 0; i < NVOICE; i++) {
            const voice_t *v = &t->v[i];
            uint32_t c;
            if (!v->active || v->stage == 4u)
                continue;
            if (!voice_held(t, v))
                c = 1;                                  /* released */
            else if (mode != V_POLY && i > 0)
                c = 2;                                  /* extra UNISON, or a voice left by a mode / cap change */
            else if (!soft && mode == V_POLY && i != low)
                c = 3;                                  /* held, not the bass */
            else
                continue;
            if (c < cat || (c == cat && v->age < (*pp)->v[best].age)) {
                cat = c;
                best = i;
                *pp = t;
            }
        }
    }
    return best;
}

/* (0.8.9) console status: voices_given_up counts events, one per voice faded out early, whatever asked (a voice
 * already fading is not counted again); voice_trims the share of them the load budget took: a released tail already
 * under -50 dB (inaudible, one per note while LOAD is tight); the overload guard's are audio.c shed_count; the rest
 * were taken for a new note with the budget of voices full (voices_stolen) */
static uint32_t voice_kills, voice_trims;               /* voices given up (budget, overload): console, hostsim */
static void voice_kill(voice_t *v)                      /* fade out over ~6 ms (env_tick stage 4; 0.6.74, was a block) */
{
    v->gate = 0;
    if (v->stage != 4u) {
        v->kfade = 0;                                   /* (0.6.74: the fade from its start) */
        voice_kills++;
    }
    v->stage = 4;
}

/* (0.6.18) the load budget takes only a tail that is nearly gone: the oldest released voice under -50 dB (any part; 0.6.19, was -30).
 * NVOICE = none: the new voice is added and the tails ring out (0.6.16-17 took the oldest released one, loud or not:
 * with a sequence running every note cut the tails; the overload guard at 80 % stays the last line) */
static uint32_t quiet_tail(track_t **pp)
{
    uint32_t p, i, best = NVOICE;
    for (p = 0; p < NPART; p++)
        for (i = 0; i < NVOICE; i++) {
            const voice_t *v = &trk[p].v[i];
            if (!v->active || voice_held(&trk[p], v) || v->stage == 4u || v->env_out >= 104)
                continue;
            if (best == NVOICE || v->age < (*pp)->v[best].age) {
                best = i;
                *pp = &trk[p];
            }
        }
    return best;
}

/* (0.9.9 cpu5, after LOOM-8's synth.c find_voice) the admission at a note's start, predicted: a voice added costs its
 * part's measured time (fx.c mc_tk: the part's block, smoothed, shared by its voices; a part that has not sounded yet,
 * or the host: a weight by engine) as a share of a half; added to the half's average load (load_half) plus what the
 * voices admitted since the half's end add. Past LOAD_ADMIT the voice is not added: a released voice is given up for it
 * instead (load_victim), the quietest of a POLY part's first, then the oldest of the part's own; never a MONO / LEGATO /
 * UNISON part's released tail (its lead's voice) while a voice is free. The guard (load_half) only sees a half once it
 * has run late; a chord's new voices could take it there in one block. With both cores rendering a voice is shared
 * between them: LOAD_SHARE of its cost */
#define LOAD_ADMIT 90u                                  /* %: the prediction may not pass this (ECO: LOAD_ADMIT_ECO) */
#define LOAD_ADMIT_ECO 70u
#define LOAD_SHARE_Q8 154u                              /* 0.6: the two cores' share (the FM-1: the split is by cost) */
#define LOAD_FRAME_TICKS 544u                           /* TIMER4 ticks a frame (24 MHz / 44,117.6 Hz: audio.c DAC_TICKS) */
#define LOAD_W_PCT 1u                                   /* a weight unit of mc_cost (fx.c) as % of a half: an estimate
                                                         * until the part is measured (FM6 4 %, LOFI / NOISE 2, else 3) */
#define LOAD_ADD_LOG2 (12u - CTL_LOG2)                 /* what the admitted voices added decays over ~93 ms (128 blocks of 32,
                                                         * the average's time: as their load shows in it, load_block) */
static uint32_t mc_tk[NTRK];                            /* (fx.c measures it: a part's block in TIMER4 ticks, smoothed) */
static uint32_t load_steals;                            /* voices given up by the admission (console status) */
static uint32_t load_cost_q8(const track_t *t)          /* a voice more of part t: its share of a half, %, Q8 */
{
    uint32_t ti = (uint32_t)(t - trk) % NTRK, k, nv = 0, c;
    const engine_t *e = ENGINES[t->engine];
    for (k = 0; k < NVOICE; k++)
        nv += t->v[k].active;
    if (mc_tk[ti] && nv) {                              /* measured: the part's block over its voices, a half of them */
        uint32_t half = mc_tk[ti] * (HALF_FRAMES / CTL) / nv;
        c = half * 25600u / (HALF_FRAMES * LOAD_FRAME_TICKS);
    } else
        c = (e == &ENG_FM6 ? 4u : e == &ENG_LOFI || e == &ENG_NOISE ? 2u : 3u) * LOAD_W_PCT * 256u;
    return load_cores2 ? (c * LOAD_SHARE_Q8) >> 8 : c;
}
static void load_take(const track_t *t)                 /* a voice added: into the prediction */
{
    load_pred_q8 += load_cost_q8(t);
}
static void load_block(void)                            /* a block rendered (track_render, part 1: every block, on the
                                                         * host too): the prediction back toward the average */
{
    uint32_t add = load_pred_q8 > load_avg_q8 ? load_pred_q8 - load_avg_q8 : 0u;
    load_pred_q8 = load_avg_q8 + add - ((add + (1u << (LOAD_ADD_LOG2 - 1u))) >> LOAD_ADD_LOG2);
}
static uint32_t load_sounding_q8(void)                  /* what every sounding voice costs: the most the voices admitted
                                                         * since the average can have added (a bound on the prediction:
                                                         * many notes in one block, or none rendered between them) */
{
    uint32_t p, i, s = 0;
    for (p = 0; p < NPART; p++) {
        uint32_t nv = 0;
        for (i = 0; i < NVOICE; i++)
            nv += trk[p].v[i].active && trk[p].v[i].stage != 4u;
        if (nv)
            s += nv * load_cost_q8(&trk[p]);
    }
    return s;
}
static int load_admit(const track_t *t)                 /* room for a voice of part t? taken if so */
{
    uint32_t limit = (sys_load == LOAD_ECO ? LOAD_ADMIT_ECO : LOAD_ADMIT) << 8, add, most;
    add = load_pred_q8 > load_avg_q8 ? load_pred_q8 - load_avg_q8 : 0u;
    most = load_sounding_q8();
    if (add > most) {
        add = most;
        load_pred_q8 = load_avg_q8 + add;
    }
    if (load_avg_q8 + add + load_cost_q8(t) > limit)
        return 0;
    load_take(t);
    return 1;
}
/* the voice given up when the prediction is past the budget: the quietest released voice of a POLY part (self
 * included), else the oldest held voice of self but its bass (POLY); NVOICE = none (the voice is added after all) */
static uint32_t load_victim(track_t *self, track_t **pp)
{
    uint32_t p, i, best = NVOICE, low;
    int32_t quiet = 0x7FFFFFFF;
    for (p = 0; p < NPART; p++) {
        track_t *t = &trk[p];
        if (trk_vmode(t) != V_POLY && t != self)
            continue;                                   /* (a mono part's tail is its lead's voice: kept) */
        for (i = 0; i < NVOICE; i++) {
            const voice_t *v = &t->v[i];
            if (v->active && v->stage != 4u && !voice_held(t, v) && v->env_out < quiet) {
                quiet = v->env_out;
                best = i;
                *pp = t;
            }
        }
    }
    if (best < NVOICE || trk_vmode(self) != V_POLY)
        return best;
    low = lowest_held(self);
    for (i = 0; i < NVOICE; i++) {
        const voice_t *v = &self->v[i];
        if (v->active && v->stage != 4u && i != low && (best == NVOICE || v->age < self->v[best].age)) {
            best = i;
            *pp = self;
        }
    }
    return best;
}

/* the budget is full: free a voice for part t. 0 = nothing to take (soft) */
static int voice_room(track_t *t, int soft)
{
    track_t *vp = 0;
    uint32_t k;
    if (voices_busy() < voice_cap()) {
        if (load_tight && (k = quiet_tail(&vp)) < NVOICE) {
            voice_kill(&vp->v[k]);                      /* (the load budget: a dying tail makes the room) */
            voice_trims++;
        }
        if (soft)                                       /* (cpu5) an extra UNISON voice: only within the prediction */
            return load_admit(t);
        load_take(t);                                   /* (a lead is never refused: counted) */
        return 1;
    }
    k = voice_victim(t, soft, &vp);
    if (k == NVOICE)
        return !soft;                                   /* hard: over the budget (cannot happen: each of the
                                                         * NPART < NVOICE parts protects one voice at most) */
    voice_kill(&vp->v[k]);
    return 1;
}

static voice_t *voice_reuse(track_t *t, voice_t *v)
{
    if (v->stage == 4u)
        voice_room(t, 0);                               /* its budget was taken before this block */
    return v;
}

/* POLY allocation (within the engine's voice cap). DRUM: reuse its lane; other engines: the same note.
 * Else a free voice: ROTATE takes
 * the next one round-robin (release tails ring out), REUSE (or any GLIDE) takes
 * the free voice whose last pitch is closest, so poly portamento moves each
 * voice the shortest way. Steal: the oldest released voice, else the oldest
 * held one, never the lowest held note (the bass). A free voice needs room in the
 * shared budget: when the voice to give up is one of this part's, it is restarted
 * in place instead. */
static voice_t *voice_alloc(track_t *t, uint32_t note)
{
    uint32_t i, np = trk_nvoice(t), best = np, low = np, nfree = 0;
    int32_t bd = 0x7FFFFFFF;
    if (t->engine == ENGI_DRUM) {
        voice_t *v = drum_reuse(t, note);
        if (v)
            return voice_reuse(t, v);
    }
    for (i = 0; i < np; i++) {
        if (t->v[i].active && t->v[i].note == note)
            return voice_reuse(t, &t->v[i]);
        nfree += !t->v[i].active;
    }
    if (nfree) {
        track_t *vp = 0;
        uint32_t full = voices_busy() >= voice_cap(), k, own = np;
        if (full || load_tight) {                       /* (0.6.16: the load budget too; 0.6.18: a dying tail) */
            /* At capacity, this part's released slot can be retriggered in place.
             * Taking another part's release instead both cuts its tail and renders
             * an extra fading voice for several blocks. Held notes stay protected. */
            if (full) {
                for (i = 0; i < np; i++)
                    if (t->v[i].active && !voice_held(t, &t->v[i]) && t->v[i].stage != 4u &&
                        (own == np || t->v[i].age < t->v[own].age))
                        own = i;
                if (own < np)
                    return &t->v[own];
            }
            k = full ? voice_victim(t, 0, &vp) : quiet_tail(&vp);
            if (k < np && vp == t)
                return &t->v[k];                        /* our own: restart it in place (no click) */
            if (k < NVOICE) {
                voice_kill(&vp->v[k]);
                voice_trims += !full;                   /* (the load budget's quiet tail, not a steal) */
            }
        }
        if (full) {
            load_take(t);                               /* (the voice added over the one given up) */
        } else if (!load_admit(t)) {                    /* (cpu5) predicted past the budget: a released voice given up
                                                         * for it (its own first, in place), the count stays */
            k = load_victim(t, &vp);
            if (k < NVOICE) {
                if (vp == t)
                    return &t->v[k];
                voice_kill(&vp->v[k]);
                load_steals++;
            } else
                load_take(t);                           /* (nothing to give up: added after all) */
        }
    }
    if (t->p[P_ALLOC] || t->p[P_GLIDE]) {
        for (i = 0; i < np; i++) {
            int32_t d = t->v[i].pitch_cur - (int32_t)note * 16;
            if (t->v[i].active)
                continue;
            d = d < 0 ? -d : d;
            if (!t->v[i].pitch_cur)
                d = 0x7FFFFFF0;                          /* never played: last choice */
            if (d < bd) {
                bd = d;
                best = i;
            }
        }
    } else {
        for (i = 0; i < np && best == np; i++) {
            uint32_t k = (t->rr + i) % np;
            if (!t->v[k].active)
                best = k;
        }
        t->rr = (uint8_t)((best == np ? t->rr : best) + 1u) % np;
    }
    if (best < np)
        return &t->v[best];
    for (i = 0; i < np; i++)                             /* the lowest held note is protected */
        if (t->v[i].active && t->v[i].stage != 4u && voice_held(t, &t->v[i]) && (low == np || t->v[i].note < t->v[low].note))
            low = i;
    for (i = 0; i < np; i++)                             /* oldest released (not one fading out for another part) */
        if (!voice_held(t, &t->v[i]) && t->v[i].stage != 4u && (best == np || t->v[i].age < t->v[best].age))
            best = i;
    if (best == np)
        for (i = 0; i < np; i++)                         /* oldest held, not the bass */
            if (i != low && (best == np || t->v[i].age < t->v[best].age))
                best = i;
    return voice_reuse(t, &t->v[best == np ? 0 : best]);
}

/* (VOICE) a glide sets out from where the voice sounds: the S-curve (eng_formant.c glide_sung) folded into pitch_cur (a
 * new note mid-glide: no jump) */
static void glide_from(voice_t *v)
{
    v->pitch_cur = (glide_sung(v) + 128) >> 8;
    v->gfrom = (int16_t)v->pitch_cur;
}
static __attribute__((noinline)) void glide_from_trk(const track_t *t, voice_t *v)   /* (a legato note, a chord glide) */
{
    if (ENGINES[t->engine % NENGINES] == &ENG_FORMANT)
        glide_from(v);
}

/* glide: RATE = a fixed speed, TIME = the same time for any interval. (0.9.8) glide 2: VOICE singing words, a legato
 * note with GLIDE 0: a singer's ~60 ms */
static void glide_set(track_t *t, voice_t *v, int glide)
{
    if (!glide || !v->pitch_cur) {
        v->pitch_cur = v->pitch16;
        v->gstep = 0;
        return;
    }
    if (t->p[P_GLMODE] || glide == 2) {
        int32_t d = v->pitch16 - v->pitch_cur, ticks = (glide == 2 ? 80 : 1 + t->p[P_GLIDE] * t->p[P_GLIDE] / 8) * 32 / (int32_t)CTL;   /* (ticks of 32 samples, as set) */
        if (ticks < 1) ticks = 1;
        d = d < 0 ? -d : d;
        v->gstep = d * 256 / ticks > 0 ? d * 256 / ticks : 1;
    } else {
        v->gstep = 0;
    }
}

/* (JIANT 0.6.81) SCL TRANS: the track's every note this many semitones up or down as it sounds (the keys, the
 * sequence, MIDI, the ARP alike; recorded as played: it was the keys' alone, the sequence did not follow). Kits: none */
static int32_t trk_trans(const track_t *t)
{
    const engine_t *e = ENGINES[t->engine % NENGINES];
    return e->oneshot || e->keys ? 0 : t->p[P_TRANS];
}
static void voice_start(track_t *t, voice_t *v, uint32_t note, uint32_t vel, int glide, uint32_t age)
{
    const engine_t *e = ENGINES[t->engine];
    int sounding = v->active && v->stage != 0;
    uint32_t ph0 = v->ph[0], ph1 = v->ph[1], ph2 = v->ph[2];
    int32_t s0 = v->s[0], s1 = v->s[1], s4 = v->s[4], s5 = v->s[5], s6 = v->s[6], s7 = v->s[7];
    uint32_t keep = e->keep;
    v->note = (uint8_t)note;
    v->vel = (uint8_t)vel;
    v->gate = 1;
    v->active = 1;
    v->stage = 1;
    v->age = age;
    if (e == &ENG_FORMANT)
        glide_from(v);
    v->fin_d = 0;
    v->gacc = 0;
    v->pitch16 = ((int32_t)note + trk_trans(t)) * 16 + t->pfx_rpit;   /* (pfx.c RANDOM: 0 but while it is held) */
    v->mvel = t->m_vel;                                 /* mod.c: the note's VEL and RAND */
    v->mrnd = t->m_rnd;
    v->henv = t->hum_env;                               /* (0.6.92) HUMAN: this note's DEC / REL moved */
    t->m_vi = (uint8_t)(v - t->v);
    glide_set(t, v, glide);
    if (!sounding) {
        v->env = 0;
        v->env_out = 0;
    }                                                   /* sounding: the attack starts from the current level */
    e->note_on(t, v);
    if (sounding && !e->sampled) {                     /* retrigger / steal: keep phases and filter */
        v->ph[0] = ph0;                                 /* states (resetting them clicks) */
        v->ph[1] = ph1;
        v->ph[2] = ph2;
        if (keep & 0x01u)                               /* engine_t.keep: the slots the engines use */
            v->s[0] = s0;
        if (keep & 0x02u)
            v->s[1] = s1;
        if (keep & 0x10u)
            v->s[4] = s4;
        if (keep & 0x20u)
            v->s[5] = s5;
        if (keep & 0x40u)
            v->s[6] = s6;
        if (keep & 0x80u)
            v->s[7] = s7;
        if (e == &ENG_LOFI && t->p[P_LRTRG] && t->p[P_E1] >= RW_BYTE)   /* (0.6.12) RTRG: BYTE's / FLOAT's time from 0 */
            v->ph[0] = v->ph[1] = 0;
    }
}

/* (0.9.9 ctl64) a note asked for at ev_off, the sample within the block (seq.c): at the block's first sample (or an
 * engine of whole blocks, FM6, within half a block of it) the voice starts now, as ever (what it sounded is cut at the
 * block's start). Else the start is kept with the voice (voice_t.sub, pn_*) and track_render makes it at its sample:
 * the note sounding until then rendered up to it, the new one from it (voice_start_rec). fine: UNISON's detune; phr:
 * a random phase added to the voice's first oscillator (UNISON). The voice counts as taken from here (active, gate,
 * the new note's age), so a second note of the block gets another slot */
static void voice_begin(track_t *t, voice_t *v, uint32_t note, uint32_t vel, int glide, int32_t fine, uint32_t phr)
{
    const engine_t *e = ENGINES[t->engine];
    uint32_t k = ev_off > CTL ? CTL : ev_off, age = ++vage, old = v->active && v->stage != 0 && v->stage != 4u;
    vs_rnd = e == &ENG_FORMANT ? noise32(&formant_nz) : 0u;   /* (its RAND vowel: drawn here, on core 0) */
    if (e->whole)
        k = k >= CTL / 2u ? CTL : 0u;                   /* the nearer block edge (CTL: the next block's) */
    if (!k || !old) {                                   /* now, or a free slot: the voice as started (what reads it
                                                         * sees the note), rendered from its sample (sub) */
        v->fine = fine;
        v->sub = 0;
        voice_start(t, v, note, vel, glide, age);
        v->ph[0] += phr;
        if (k) {
            v->pn_flags = PN_START;
            v->sub = (uint8_t)(k + 1u);
        }
        return;
    }
    v->pn_flags = PN_START | PN_OLD;                    /* sounding: its note up to the sample, then this one */
    v->pn_note = (uint8_t)note;
    v->pn_vel = (uint8_t)vel;
    v->pn_glide = (uint8_t)glide;
    v->pn_fine = fine;
    v->pn_phr = phr;
    v->pn_age = age;
    v->pn_rnd = vs_rnd;
    v->pn_henv = t->hum_env;                            /* what voice_start and the engine's note_on read of the track */
    v->pn_mvel = t->m_vel;                              /* now, as they would have (seq.c sets them around the note) */
    v->pn_mrnd = t->m_rnd;
    v->pn_dpit = t->dpit;
    v->pn_dbend = t->dbend;
    v->pn_dbt = t->dbt;
    v->pn_tch = (uint8_t)(dx_in_tch ? dx_in_tch : t->dtch);
    if (e == &ENG_DRUM && drum_lane(note) == DV_KICK)
        duck_hit = 1;                                   /* (DUCK: the kick's, as drum_note_on tells it; this block's) */
    v->active = 1;
    v->gate = 1;
    v->note = (uint8_t)note;
    v->vel = (uint8_t)vel;                              /* (what reads the voice sees the new note from here) */
    v->age = age;
    v->sub = (uint8_t)(k + 1u);
}
/* the kept start, made (track_render, at the voice's sample; on either core): the track's values of the time put in
 * for voice_start and the engine's note_on, then back */
static __attribute__((noinline)) void voice_start_rec(track_t *t, voice_t *v)
{
    int8_t he = t->hum_env, dp = t->dpit;
    uint8_t mv = t->m_vel, tch = t->dtch;
    int16_t mr = t->m_rnd, db = t->dbend;
    uint16_t dbt = t->dbt;
    uint32_t off = !v->gate || v->stage == 4u;          /* let go since it was asked for (voice_off_at: the gate down), or
                                                         * given up: on, then off. (gen6e, the owner and a tester: not
                                                         * stage 3: that was the note before's release, still ringing up
                                                         * to this one's sample, so every note on a voice in its release
                                                         * was let go at once: a release ate the steps after it) */
    t->hum_env = v->pn_henv; t->m_vel = v->pn_mvel; t->m_rnd = v->pn_mrnd;
    t->dpit = v->pn_dpit; t->dbend = v->pn_dbend; t->dbt = v->pn_dbt; t->dtch = v->pn_tch;
    v->fine = v->pn_fine;
    vs_rnd = v->pn_rnd;
    voice_start(t, v, v->pn_note, v->pn_vel, v->pn_glide, v->pn_age);
    v->ph[0] += v->pn_phr;
    t->hum_env = he; t->m_vel = mv; t->m_rnd = mr;
    t->dpit = dp; t->dbend = db; t->dbt = dbt; t->dtch = tch;
    if (v->pn_flags & PN_BEND) {                        /* (seq.c seq_step: BEND, set after the note-on) */
        v->pitch_cur = v->pitch16;
        v->pitch16 = clamp(v->pitch16 + v->pn_bend, 0, 2047);
        v->gstep = v->pn_gstep;
    }
    if (v->pn_flags & PN_GLIDE) {                       /* (seq.c chord_glide: a new note of the chord, in as it glides) */
        v->pitch_cur = v->pn_pcur;
        v->gfrom = (int16_t)v->pn_pcur;
        v->gstep = v->pn_gstep;
        v->gacc = 0;
        v->fin_d = v->pn_fin;
    }
    if (off) {
        v->gate = 0;
        v->stage = 3;
    }
}
/* (ctl64) a sounding voice let go at ev_off: released now (gate, stage: as ever), and when the sample is inside the
 * block (sub, no PN_START) track_render keeps its envelope where it was up to that sample and ticks the release from
 * there (an engine of whole blocks: at the nearer block edge). A voice whose start is still kept is let go after its
 * start (off). Hits (DRUM) end by themselves: nothing kept */
static void voice_off_at(track_t *t, voice_t *v)
{
    const engine_t *e = ENGINES[t->engine];
    uint32_t k = ev_off > CTL ? CTL : ev_off;
    if (e->whole)
        k = k >= CTL / 2u ? CTL : 0u;
    if (v->sub && (v->pn_flags & PN_START)) {           /* starting in this block: off right after (zero-length) */
        v->gate = 0;
        v->stage = 3;                                   /* (the start's record puts it on, then off: voice_start_rec) */
        return;
    }
    v->gate = 0;                                        /* let go from here, for what reads the voice; (ctl64) its
                                                         * envelope held up to the sample, released from it (track_render) */
    if (!k || e->oneshot || !v->active || v->stage == 4u) {
        if (v->stage != 4u)
            v->stage = 3;
        v->sub = 0;
        return;
    }
    v->stage = 3;
    v->pn_flags = PN_OLD;
    v->sub = (uint8_t)(k + 1u);
}

/* MONO / LEGATO / UNISON: one note on one voice (eight for UNISON, or the
 * engine's voice cap, spread by DETUNE over the same width) */
static void mono_play(track_t *t, uint32_t note, uint32_t vel, int retrig, int glide)
{
    uint32_t mode = (uint32_t)t->p[P_VOICE], nv = mode == V_UNISON ? trk_nvoice(t) : 1u, i;
    for (i = 0; i < nv; i++) {
        voice_t *v = &t->v[i];
        int32_t k = 2 * (int32_t)i - (int32_t)(nv - 1u);   /* -7 .. 7 */
        if (nv > 1u && nv < NVOICE)
            k = k * (NVOICE - 1) / (int32_t)(nv - 1u);      /* fewer voices: the outer ones as wide */
        int32_t fine = nv > 1u ? k * t->p[P_DETUNE] * 56 / 889 : 0;   /* up to ~±40 cents */
        if (retrig || !v->active || !v->gate) {
            uint32_t phr = 0;
            if ((!v->active || v->stage == 4u) && !voice_room(t, i > 0))
                continue;                                   /* no room for this extra UNISON voice */
            if (nv > 1u && i && !ENGINES[t->engine]->sampled) {   /* random start phases: */
                static uint32_t seed = 0x1234567u;          /* in phase they stack, evenly spread they cancel */
                seed = seed * 1664525u + 1013904223u;
                phr = seed;
            }
            /* level: about the same sum for 8 or 4 voices at random phases */
            voice_begin(t, v, note, nv >= NVOICE ? vel * 36u / 100u : nv > 1u ? vel / 2u : vel, glide, fine, phr);
        } else if (v->sub && (v->pn_flags & PN_OLD)) {      /* (ctl64) restarting later in this block: that start's note */
            v->pn_note = (uint8_t)note;
            v->note = (uint8_t)note;
            v->pn_fine = fine;
            if (vel > v->pn_vel && nv == 1u)
                v->pn_vel = (uint8_t)vel;
        } else {                                            /* legato: new pitch, same envelope */
            v->fine = fine;
            v->note = (uint8_t)note;
            glide_from_trk(t, v);
            v->pitch16 = ((int32_t)note + trk_trans(t)) * 16 + t->pfx_rpit;
            v->mvel = t->m_vel;
            v->mrnd = t->m_rnd;
            if (vel > v->vel && nv == 1u)
                v->vel = (uint8_t)vel;
            /* (0.9.8) VOICE singing words: a legato note glides a little even with GLIDE 0 (a singer's ~60 ms) */
            glide_set(t, v, glide ? glide : ENGINES[t->engine % NENGINES] == &ENG_FORMANT && t->p[P_VLANG] ? 2 : 0);
        }
    }
    t->mono_note = (uint8_t)note;
}

/* the note the mono stack asks for (PRIO: LAST / LOW / HIGH), 0 = none */
static uint32_t mono_pick(const track_t *t)
{
    uint32_t i, n;
    if (!t->nmono)
        return 0;
    n = t->mono_stack[t->nmono - 1u];
    for (i = 0; i < t->nmono; i++) {
        if (t->p[P_PRIO] == 1 && t->mono_stack[i] < n)
            n = t->mono_stack[i];
        if (t->p[P_PRIO] == 2 && t->mono_stack[i] > n)
            n = t->mono_stack[i];
    }
    return n;
}

static void mono_remove(track_t *t, uint32_t note)
{
    uint32_t j, k = 0;
    for (j = 0; j < t->nmono; j++)
        if (t->mono_stack[j] != note)
            t->mono_stack[k++] = t->mono_stack[j];
    t->nmono = (uint8_t)k;
}

static int pfx_note(track_t *t, uint32_t note, uint32_t vel);   /* pfx.c (in fx.c) */
static uint8_t trk_hush;
static uint8_t trk_live = 4;
static volatile uint8_t asleep_hit;                     /* (0.6.28) a note went to a track asleep (ui_input: the hint) */                            /* (JIANT 0.6.18) MENU > TRACKS: the tracks that play (4 LOW CPU, the
                                                         * default; 6 HIGH): the ones past it get no notes and stop,
                                                         * their sounds and steps kept (fx.c mix_part) */                                /* (0.6.15) bit k: track k muted by GLO's solo / the FX layer's
                                                         * black key (perform.c): no new notes, as MUTE (a MIDI mute: a
                                                         * silenced track's voices end, it costs nothing) */
static void trk_note_on(track_t *t, uint32_t note, uint32_t vel)
{
    uint32_t any = 0, i, mode = trk_vmode(t);
    if ((uint32_t)(t - trk) >= trk_live && !t->p[P_MUTE]) {
        asleep_hit = 1;                                 /* (0.6.28) a note for a track asleep: the UI says so */
        return;
    }
    if (t->p[P_MUTE] || ((trk_hush >> ((uint32_t)(t - trk) % NTRK)) & 1u) ||
        (t->engine == ENGI_DRUM && drum_muted(note)))   /* (DRUM: its group mutes) */
        return;
    if (pfx_note(t, note, vel))                         /* the punch-in effects (pfx.c): a repeat holds the track */
        return;
    if (t->xf_on || t->eng_req != t->engine) {          /* engine switch under way: after the fade */
        for (i = 0; i < t->xp_n && t->xp_note[i] != note; i++)
            ;
        if (i == t->xp_n && t->xp_n < 4u)
            t->xp_n++;
        i = i < t->xp_n ? i : t->xp_n - 1u;             /* (full: the last one is replaced) */
        t->xp_note[i] = (uint8_t)note;
        t->xp_vel[i] = (uint8_t)vel;
        return;
    }
    mod_note(t, note, vel);                             /* the matrix's VEL / KEY / RAND */
    for (i = 0; i < NVOICE; i++)
        any |= t->v[i].gate;
    if (!any && t->p[P_LPHASE] >= 0) {                 /* fresh phrase, MODE TRIG: the LFO from PHASE, FADE from 0.
                                                         * (0.9.18) FREE (PHASE -1) restarts neither: it runs on, its
                                                         * FADE once from a project's load (lfo_fade_restart) */
        t->lfo_ph = (uint32_t)t->p[P_LPHASE] << 25;
        t->lfo_fade = 0;
    }
    if (mode == V_POLY) {
        voice_t *v = voice_alloc(t, note);
        t->nmono = 0;                                   /* no stale mono stack after a mode change */
        t->mono_note = 0;
        voice_begin(t, v, note, vel, t->p[P_GLIDE] != 0 && !ENGINES[t->engine]->oneshot, 0, 0u);
        return;
    }
    {
        uint32_t sounding = t->mono_note && t->v[0].active && t->v[0].gate, want;
        mono_remove(t, note);
        if (t->nmono >= 8u)
            mono_remove(t, t->mono_stack[0]);
        t->mono_stack[t->nmono++] = (uint8_t)note;
        want = mono_pick(t);
        if (sounding && want == t->mono_note)
            return;                                     /* LOW / HIGH priority: this key does not win */
        if (mode == V_MONO)                             /* MONO: always retrigger, glide when GLIDE is set */
            mono_play(t, want, vel, 1, t->p[P_GLIDE] != 0 || t->slide_glide);
        else                                            /* LEGATO / UNISON: glide only between held notes */
            mono_play(t, want, vel, !sounding, sounding && (t->p[P_GLIDE] != 0 || t->slide_glide));
    }
}

static void trk_note_off(track_t *t, uint32_t note)
{
    uint32_t i, k = 0, mode = trk_vmode(t);
    for (i = 0; i < t->xp_n; i++)                       /* not sounding yet (engine switch): forget it */
        if (t->xp_note[i] != note) {
            t->xp_note[k] = t->xp_note[i];
            t->xp_vel[k++] = t->xp_vel[i];
        }
    t->xp_n = (uint8_t)k;
    mono_remove(t, note);                              /* a mode change must not retain a released key */
    if (mode != V_POLY) {
        uint32_t nv = mode == V_UNISON ? trk_nvoice(t) : 1u;
        if (t->mono_note == note) {
            uint32_t next = mono_pick(t);
            if (next) {                                 /* fall back to a held note, legato */
                mono_play(t, next, t->v[0].vel, 0, t->p[P_GLIDE] != 0);
            } else {
                for (i = 0; i < nv; i++)
                    if (t->v[i].gate)
                        voice_off_at(t, &t->v[i]);
                t->mono_note = 0;
            }
        }
    }
    for (i = 0; i < NVOICE; i++)                        /* a fallback above already has its new pitch */
        if (t->v[i].note == note && t->v[i].gate)
            voice_off_at(t, &t->v[i]);
}

static void trk_all_off(track_t *t)
{
    uint32_t i;
    for (i = 0; i < NVOICE; i++) {
        t->v[i].gate = 0;
        t->v[i].stage = t->v[i].active ? 3 : 0;
        t->v[i].sub = 0;                                /* (ctl64) a start kept for its sample: dropped */
    }
    t->nmono = 0;
    t->mono_note = 0;
    t->xp_n = 0;
}

/* engine switch, at each block start (events_block), before any note of the block. The UI writes
 * eng_req and the new engine's P_E0..P_E7 together (IRQ off). The part's sounding voices (released by
 * the preset change) then fade out over XF_BLOCKS blocks on the old engine, rendered with its own
 * parameters (pe_old: an engine never reads another engine's values, which index its tables); only
 * then the engine switches and the notes that came during the fade start on it. A part with nothing
 * sounding switches at once. */
#define XF_BLOCKS (128u / CTL)                          /* 128 samples: 2.9 ms */
/* (0.6.41) smoother: an engine switch fades out over XF_ENG (11.6 ms) and what was held (keys, the sequencer's notes:
 * xf_held, before the load's release) starts on the new engine, fading in over XF_ENG; an engine's list value
 * (WAVE, CHIP, TYPE .. F_ENUM) changed while it sounds: a dip, XF_DIP out on the old value and in on the new one, the
 * voices going on (it clicked: the wave swapped mid-cycle); (0.6.81) a value with names too (FM6's ALG, its SLOT) */
#define XF_ENG (512u / CTL)                             /* 11.6 ms */
#define XF_DIP (128u / CTL)                             /* (0.6.81: 2.7 ms each way, was 1.3: a tick) */
static int eng_kit(uint32_t e) { return ENGINES[e % NENGINES]->oneshot; }
static uint32_t xf_held(const track_t *t, uint8_t *nt, uint8_t *vl)   /* the notes held (up to 4): their count */
{
    uint32_t i, k, n = 0;
    if (eng_kit(t->engine) || eng_kit(t->eng_req))
        return 0;                                       /* (hits: nothing held) */
    for (i = 0; i < NVOICE; i++) {
        const voice_t *v = &t->v[i];
        if (!v->active || !v->gate)
            continue;
        for (k = 0; k < n && nt[k] != v->note; k++)
            ;
        if (k == n && n < 4u) { nt[n] = v->note; vl[n++] = v->vel; }
    }
    return n;
}
static void engine_block(track_t *t)
{
    uint32_t i, any = 0;
    if (!t->xf_on && t->eng_req == t->engine && !eng_kit(t->engine)) {   /* a list value changed while it sounds */
        const engine_t *e = ENGINES[t->engine % NENGINES];
        for (i = 0; i < 8u; i++) {                      /* (p1, main 0.9.16) only a value that changed, then followed
                                                         * (pe_old: the last value heard): the engine's desc was asked for
                                                         * all 8 of every part, every block, stopped too */
            const param_desc_t *d;
            uint32_t k;
            if (t->p[P_E0 + i] == t->pe_old[i])
                continue;
            d = e->desc ? e->desc(t, i) : 0;
            d = d ? d : &e->edit[i];
            /* (p1, main 0.9.17) a value with names: only a new name dips; VOICE's THEME and LOOP never (the words, not
             * the sound: a THEME swept dipped at every one of its values, a held note crackling) */
            if (d->fmt == F_ENUM || (d->names && !(d >= FMT_WDESC && d < FMT_WDESC + 8) &&
                                     name_of(d, t->p[P_E0 + i], &k) != name_of(d, t->pe_old[i], &k)))
                break;
            t->pe_old[i] = t->p[P_E0 + i];
        }
        for (any = 0; i < 8u && any < NVOICE; any++)
            if (t->v[any].active && t->v[any].env_out)
                break;
        if (i < 8u && any < NVOICE) {
            t->xf_on = 1; t->xf = t->xf_len = XF_DIP; t->xf_dip = 1;
            return;                                     /* (pe_old: the old value, for the dip's way out) */
        }
        any = 0;
    }
    if (t->xf_on || t->eng_req != t->engine) {
        if (!t->xf_on) {
            for (i = 0; i < NVOICE; i++)
                any |= t->v[i].active;
            if (any) {
                t->xf_on = 1;
                t->xf = t->xf_len = XF_ENG;
                t->xf_dip = 0;
                return;
            }
        } else if (t->xf) {
            return;                                     /* still fading (track_render counts down) */
        }
        if (t->xf_dip) {                                /* the dip's end: the same voices, in again on the new value */
            t->xf_on = t->xf_dip = 0;
            t->xin = t->xin_len = XF_DIP;
            for (i = 0; i < 8u; i++)
                t->pe_old[i] = t->p[P_E0 + i];
            return;
        }
        for (i = 0; i < NVOICE; i++) {                  /* faded to 0: gone */
            voice_t *v = &t->v[i];
            v->active = v->gate = 0;
            v->stage = 0;
            v->env = v->env_out = 0;
        }
        any = t->engine != eng_idx(t->eng_req);
        t->engine = eng_idx(t->eng_req);
        t->xf_on = 0;
        t->nmono = 0;
        t->mono_note = 0;
        {
            uint32_t n = t->xp_n;
            t->xp_n = 0;
            for (i = 0; i < n; i++)
                trk_note_on(t, t->xp_note[i], t->xp_vel[i]);
            if (n && any)                               /* (0.6.41) on the new engine: in, not at once */
                t->xin = t->xin_len = XF_ENG;
        }
    }
    for (i = 0; i < 8u; i++)                            /* the engine's own values, for a later fade */
        t->pe_old[i] = t->p[P_E0 + i];
}

/* (0.6.13) the sound a track had before a load (ui.c load_begin, the main loop, before it writes the new values): the
 * fade renders with all of it, not only its engine's values (pe_old): the filter, its envelope, the LFO .. as they were.
 * One at a time (a load is one track's; a section's six: the others fade on pe_old as before) */
static int16_t xf_snap[P_COUNT];
static volatile uint8_t xf_snap_t = 0xFFu;              /* its track, 0xFF none */
/* (0.6.13) a sound loaded (a preset, a user preset, a section: seq.c panic_req): what still sounds fades out over
 * XF_BLOCKS on the values it had (as an engine switch does), not its release on the new sound's (its filter, its
 * levels: the old notes jumped bright and loud, sharp highs at every preset change); the notes asked for meanwhile
 * after it */
static void trk_fade_out(track_t *t)
{
    uint32_t i, any = 0;
    if (t->xf_on)
        return;
    for (i = 0; i < NVOICE; i++)
        any |= t->v[i].active;
    if (any) {
        t->xf_on = 1;
        t->xf = t->xf_len = t->eng_req != t->engine ? XF_ENG : XF_BLOCKS;   /* (0.6.41: another engine, longer) */
        t->xf_dip = 0;
    } else if (xf_snap_t == (uint32_t)(t - trk)) {
        xf_snap_t = 0xFFu;                              /* (nothing to fade: no snapshot left for a later one) */
    }
}

/* one control tick (CTL samples) of the amplitude envelope; returns Q15 (the tables ENV_LIN / ENV_EXP are per CTL:
 * tools/gen_tables.py) */
static int32_t env_tick(track_t *t, voice_t *v)
{
    const int16_t *p = t->p;
    int32_t sus = (int32_t)p[P_SUS] << 17;              /* Q24 */
    switch (v->stage) {
    case 1:
        v->env += (int32_t)ENV_LIN[p[P_ATK] & 127];
        if (v->env >= (1 << 24)) {
            v->env = 1 << 24;
            v->stage = 2;
        }
        break;
    case 2:
        v->env += mulq16(sus - v->env, ENV_EXP[clamp(p[P_DEC] + v->henv, 0, 127)]);
        if (p[P_ELOOP] > 0 && v->gate && v->env - sus < (1 << 18))   /* (JIANT 0.4) LOOP: at the sustain (within
                                                         * 1/64), held: the attack again from here (an ADSR-shaped LFO) */
            v->stage = 1;
        break;
    case 3:
        v->env -= mulq16(v->env, ENV_EXP[clamp(p[P_REL] + v->henv, 0, 127)]);
        if (v->env < (1 << 12)) {
            v->env = 0;
            v->stage = 0;
            v->active = 0;
        }
        break;
    case 4:                                             /* given up (another part, the load): it fades out. (JIANT
                                                         * 0.6.74) A half cosine over ~4.4 ms (6 blocks of 32; ctl64:
                                                         * 4 of 64, 5.8 ms), each block's ramp between (KFADE: each block's gain
                                                         * over the last's): gentle in and out. One block (full to 0,
                                                         * as before) clicked on every steal, a bass note most; longer
                                                         * would keep more voices rendering under load */
    {
#if CTL == 32
        static const uint8_t KFADE[6] = {239, 206, 171, 128, 69, 0};
#define KFADE_N 6u
#else                                                   /* (ctl64: 4 blocks of 64, 5.8 ms: 1 .85 .6 .3 0, the last two
                                                         * slopes equal: no bend where the fade ends, a bass note most) */
        static const uint8_t KFADE[4] = {218, 181, 128, 0};
#define KFADE_N 4u
#endif
        v->env = (int32_t)(mul64(v->env, KFADE[v->kfade < KFADE_N ? v->kfade : KFADE_N - 1u]) >> 8);   /* (core.h: out of line) */
        v->kfade++;
        if (v->kfade >= KFADE_N || !v->env) {
            v->env = 0;
            v->stage = 0;
            v->active = 0;
        }
        break;
    }
    default:
        v->env = 0;
        break;
    }
    return v->env >> 9;
}

/* (gen6d, a tester: "ENV does nothing on DRUM-X") DRUM-X's ENV: its hits keep their own amplitude (drum_amp holds the
 * voices' ADSR at full), so the ENV page drives one envelope for the kit instead, restarted by every hit (from where it
 * is: no click), its peak the hit's velocity (the kick, the snare and the clap whole, the rest at 5/8): ATK up, DEC to
 * SUS, then REL from there (a hit has no key to let go). ENV DEST's FILTER reads it, once a block (the part's filter:
 * tf_run; PITCH and SHAPE stay the hits' own), and a hit is seen there: a voice newer than the last one seen.
 * No RAM of its own: a kit's part is always POLY, so its mono_stack (MONO's held keys) is free: bytes 0..3 the level
 * (Q24), 4 the stage, 5 the peak, 6..7 the newest voice's age seen; a part come from another engine starts silent */
#define DENV_ST(t) ((t)->mono_stack[4])
#define DENV_PK(t) ((t)->mono_stack[5])
static __attribute__((noinline)) int32_t drum_env(track_t *t)
{
    const int16_t *p = t->p;
    int32_t e, sus = (int32_t)p[P_SUS] << 17;
    uint32_t st = DENV_ST(t), i, pk = 0;
    uint16_t seen, top;
    memcpy(&seen, t->mono_stack + 6, 2);
    top = seen;
    for (i = 0; i < NVOICE; i++) {                      /* the hits since the last block: the loudest of them */
        const voice_t *v = &t->v[i];
        uint32_t w;
        if (!v->active || (int16_t)((uint16_t)v->age - seen) <= 0)
            continue;
        w = drum_lane(v->note) <= DV_CLAP ? v->vel : v->vel * 5u / 8u;
        if (w > pk) pk = w;
        if ((int16_t)((uint16_t)v->age - top) > 0) top = (uint16_t)v->age;
    }
    memcpy(t->mono_stack + 6, &top, 2);
    if (pk) {
        DENV_PK(t) = (uint8_t)pk;
        st = 1;
    }
    if (st - 1u > 2u)
        return 0;
    memcpy(&e, t->mono_stack, 4);
    if (st == 1u) {
        if (e < 0 || e > (1 << 24)) e = 0;              /* (another engine's keys there before: from silence) */
        e += (int32_t)ENV_LIN[p[P_ATK] & 127];
        if (e >= (1 << 24)) { e = 1 << 24; st = 2; }
    } else if (st == 2u) {
        e += mulq16(sus - e, ENV_EXP[p[P_DEC] & 127]);
        if (e - sus < (1 << 18)) st = 3;
    } else {
        e -= mulq16(e, ENV_EXP[p[P_REL] & 127]);
        if (e < (1 << 12)) { e = 0; st = 0; }
    }
    memcpy(t->mono_stack, &e, 4);
    DENV_ST(t) = (uint8_t)st;
    return ((e >> 9) * (int32_t)DENV_PK(t)) / 127;
}

/* (JIANT 0.7.8) DRIFT: each voice its own slow value noise (mod.c drift_noise), on the track's blocks */
static uint32_t drift_clk[NTRK];

/* the per-block values a part's voices share (track_render) */
typedef struct {
    int32_t lfo, tune, tune_fine, bend16, bend_fine, damt;
    uint32_t ti, fade;
} vctx_t;
/* one voice's block: its control tick (the envelope, the glide, the modulation) and its render into out[off .. off + n)
 * (the part's buffer from its sample; (ctl64) n < CTL: a note starting or ending inside the block, track_render: the
 * amplitude ramp then goes on at its slope, the level reached kept for the next block). Returns 1 when it rendered */
static uint32_t voice_block(track_t *t, const engine_t *e, voice_t *v, int32_t *out, uint32_t off, uint32_t n,
                            const vctx_t *c, int hold)
{
    const int16_t *p = t->p;
    vmod_t m;
    int32_t env, pitch, dfine, dcut, fsum;
    {
        env = hold ? v->env >> 9 : env_tick(t, v);      /* (hold: a note let go inside the block, up to its sample: the
                                                         * envelope where it was, the release's first tick from there) */
        if (e->ownenv && v->active) {               /* the engine's envelopes (FM6): they end the voice */
            if (e->done(t, v)) {
                v->active = v->gate = 0;
                v->stage = 0;
                v->env = v->env_out = 0;
                return 0;
            }
            if (v->stage != 4u && !e->amp) {        /* (0.6.74: given up, env_tick's fade; par12: an engine with its own
                                                     * amp() decides, STRING's FDBK keeps the ADSR) */
                v->env = 1 << 24;                   /* (the ADSR's release never ends it) */
                env = 32767;
            }
        }
        if (e->amp)                                 /* the engine's own amplitude curve */
            env = e->amp(t, v, env);
        m.envq15 = e->ownenv ? 0 : env;
        m.amp1 = e->ownenv ? env : mulq15(env, v->vel * 258);
        if (p[P_LD_AMP])
            m.amp1 = mulq15(m.amp1, 32767 - mulq15((c->lfo + 32768) >> 1, p[P_LD_AMP] * 258));
        if (c->fade)                                /* linear to 0 over the fade */
            m.amp1 = m.amp1 * (int32_t)(t->xf - 1u) / (int32_t)(t->xf_len ? t->xf_len : XF_BLOCKS);
        else if (t->xin)                            /* (0.6.41) in, linear, after a switch or a dip */
            m.amp1 = m.amp1 * (int32_t)(t->xin_len - t->xin + 1u) / (int32_t)(t->xin_len + 1u);
        if (v->fin_d) {                             /* (0.6.81) a chord glide's new note, in as it glides */
            int32_t r = v->pitch16 - v->pitch_cur;
            r = r < 0 ? -r : r;
            if (r >= (int32_t)v->fin_d) r = v->fin_d - 1;
            m.amp1 = m.amp1 * ((int32_t)v->fin_d - r) / (int32_t)v->fin_d;
            if (!r) v->fin_d = 0;
        }
        m.amp0 = v->env_out;
        v->env_out = m.amp1;
    }
    if (v->pitch_cur != v->pitch16) {               /* glide */
        int32_t d = v->pitch16 - v->pitch_cur;
        int32_t st;
        if (v->gstep) {                             /* TIME: Q8, the fraction carried (0.6.82) */
            int32_t q = v->gacc + v->gstep;
            st = q >> 8;
            v->gacc = (uint8_t)(q & 255);
        } else {
            st = (int32_t)(ENV_LIN[(p[P_GLIDE] ? p[P_GLIDE] : 40) & 127] >> 14);   /* slide: ~45 ms */
            if (st < 1)
                st = 1;
        }
        v->pitch_cur += d > 0 ? (d < st ? d : st) : (-d < st ? d : -st);
    }
    if (!env && !m.amp0 && v->stage == 2 && !e->sampled)
        return 0;                                   /* held at a silent sustain (SUS 0): nothing to render */
    pitch = v->pitch_cur + t->pfx_pit + c->tune + c->bend16 + ((c->lfo * p[P_LD_PIT] * 3) >> 15) + ((m.envq15 * p[P_ED_PIT] * 3) >> 15);
    m.pitch16 = clamp(pitch, 0, 2047);
    m.inc = pitch_inc(m.pitch16);
    dfine = dcut = 0;
    if (c->damt) {                                     /* (0.7.8) DRIFT: up to ~27 ct, and the filter a little */
        dfine = (drift_noise(c->ti * 8u + (uint32_t)(v - t->v) * 1013u, drift_clk[c->ti] + (uint32_t)(v - t->v) * 397u, 10u - (CTL_LOG2 - 5u)) * c->damt) >> 16;   /* (~0.74 s) */
        dcut = (drift_noise(c->ti * 8u + (uint32_t)(v - t->v) * 1013u + 0x5bd1u, drift_clk[c->ti] + (uint32_t)(v - t->v) * 211u, 11u - (CTL_LOG2 - 5u)) * c->damt) >> 12;   /* (~1.5 s) */
    }
    fsum = v->fine + c->tune_fine + c->bend_fine + dfine;
    m.fine = fsum;
    if (fsum)                                       /* residual below 1/16 semitone */
        m.inc += (uint32_t)((int32_t)(m.inc >> 12) * fsum);
    m.cutoff = ((c->lfo * p[P_LD_FLT]) >> 7) + ((m.envq15 * p[P_ED_FLT]) >> 7) + dcut;
    if (v->vel > 110)                               /* accent opens the filter with the env */
        m.cutoff += (m.envq15 * 24) >> 7;
    m.shape = (64 << 8) + ((c->lfo * p[P_LD_SHP]) >> 7) + ((m.envq15 * p[P_ED_SHP]) >> 7);
    if (MOD.on)                                     /* the modulation matrix (mod.c) */
        mod_voice(t, v, &m, fsum);
    vo_off_c[MC_CPU() & 1u] = (uint8_t)off;
    e->render(t, v, out + off, n, &m);
    vo_off_c[MC_CPU() & 1u] = 0;
    if (n < CTL)                                        /* (ctl64) part of a block: the ramp's level where it got to */
        v->env_out = m.amp0 + (((m.amp1 - m.amp0) * (int32_t)n) >> CTL_LOG2);
    return 1;
}

/* render one block of a part into out (cleared here); returns the voices rendered */
/* Channel bend is live performance state, outside projects/presets. Q8 semitones. */
static int32_t midi_bend_q8[NTRK], midi_bend_target[NTRK];
static uint32_t track_render(track_t *t, int32_t *out, uint32_t n)
{
    const engine_t *e = ENGINES[t->engine];
    const int16_t *p = t->p;
    uint32_t i;
    int32_t lfo = mulq15(t->lfo_val, t->lfo_fade);
    /* TUNE in cents: whole 1/16 semitones in the pitch, the rest as a fine factor (no dead zone) */
    int32_t tune = song.g[G_TUNE] >= 0 ? song.g[G_TUNE] * 16 / 100 : -((-song.g[G_TUNE] * 16 + 99) / 100);
    int32_t tune_fine = (song.g[G_TUNE] * 16 - tune * 100) * 2367 / 16000;   /* rest, in 1/4096 (1 ct = 2.367) */
    int32_t bend, bend16, bend_fine;
    uint32_t ti = (uint32_t)(t - trk);
    if (ENGINES[eng_idx(t->eng_req)] == &ENG_DRUM) {
        midi_bend_q8[ti] = midi_bend_target[ti] = 0;
    } else {
        int32_t d = midi_bend_target[ti] - midi_bend_q8[ti];
        /* ~6 ms smoothing, with an exact landing (no permanent small offset); (ctl64: a quarter a block of 32, a half of 64) */
        midi_bend_q8[ti] += d > 0 ? (d < 4 ? d : (d + 3) / (128 / (int32_t)CTL)) : (d > -4 ? d : (d - 3) / (128 / (int32_t)CTL));
    }
    bend = midi_bend_q8[ti];
    bend16 = bend >= 0 ? bend / 16 : -((-bend + 15) / 16);
    bend_fine = (bend - bend16 * 16) * 2367 / 2560;
    uint32_t nr = 0, fade = t->xf_on && t->xf, snap = fade && xf_snap_t == ti;
    int32_t damt;
    int16_t pe_new[8], p_new[P_COUNT];
    for (i = 0; i < n; i++)
        out[i] = 0;
    if (!ti)
        load_block();                                   /* (cpu5) the block's clock for the admission's prediction */
    if (snap) {                                         /* (0.6.13) a sound load: the whole sound as it was */
        memcpy(p_new, t->p, sizeof p_new);
        memcpy(t->p, xf_snap, sizeof p_new);
    } else if (fade)                                    /* engine switch: the old engine, its own values */
        for (i = 0; i < 8u; i++) {
            pe_new[i] = t->p[P_E0 + i];
            t->p[P_E0 + i] = t->pe_old[i];
        }
    track_lfo_tick(t);
    {   /* (0.6.50) MORPH slewed (at most 3 of the knob's steps a block) and ramped by the engines across the block */
        int32_t g = clamp(p[P_MORPH], 0, 127) << 8, c = t->mo_cur, ms = (3 * (int32_t)CTL / 32) << 8;   /* (3 a block of 32) */
        t->mo_prev = (uint16_t)c;
        t->mo_cur = (uint16_t)(c + clamp(g - c, -ms, ms));
    }
    dx_xon = 0;                                         /* (0.6.48) a DRUM part's IMAGE and SENDs (eng_drum.c) */
    if (e == &ENG_DRUM)
        drum_side_begin(t, n);
    if (e->block)                                       /* the engine's per-part work (WHEEL: bars, rotor) */
        e->block(t);
    damt = e == &ENG_DRUM ? 0 : 127 - clamp(p[P_DRIFT], 0, 127);   /* (0.7.8) DRIFT, 127 stored = 0 % */
    if (damt)
        drift_clk[ti]++;
    {
        vctx_t c;
        c.lfo = lfo; c.tune = tune; c.tune_fine = tune_fine; c.bend16 = bend16; c.bend_fine = bend_fine;
        c.damt = damt; c.ti = ti; c.fade = fade;
        for (i = 0; i < NVOICE; i++) {
            voice_t *v = &t->v[i];
            uint32_t k = v->sub;
            if (!v->active)
                continue;
            if (!k) {
                nr += voice_block(t, e, v, out, 0, n, &c, 0);
                continue;
            }
            /* (ctl64) a start or a release kept for its sample k (voice_begin, voice_off_at) */
            k--;
            v->sub = 0;
            if (v->stage == 4u) {                       /* given up meanwhile: its fade, nothing starts */
                nr += voice_block(t, e, v, out, 0, n, &c, 0);
                continue;
            }
            if (k >= n) {                               /* the next block's first sample (an engine of whole blocks) */
                v->sub = 1;
                if (v->pn_flags & PN_OLD)
                    nr += voice_block(t, e, v, out, 0, n, &c, 0);
                continue;
            }
            if (!(v->pn_flags & PN_START)) {            /* let go at k: held up to it, released from it */
                if (k)
                    voice_block(t, e, v, out, 0, k, &c, 1);
                voice_block(t, e, v, out, k, n - k, &c, 0);
                nr++;
                continue;
            }
            if (v->pn_flags & PN_OLD) {                 /* the note sounding, up to the new one's sample; then it */
                if (k)
                    voice_block(t, e, v, out, 0, k, &c, 0);
                voice_start_rec(t, v);
            } else if (!v->gate && v->stage < 3u)       /* (started at the event; let go since: on, then off) */
                v->stage = 3;
            voice_block(t, e, v, out, k, n - k, &c, 0); /* the new one, from its sample */
            nr++;
        }
    }
    if (nr && e == &ENG_DRUM)                           /* (0.6.16) DRUM-X: DRV and the knee on the kit's sum */
        drum_bus(t, out, n);
    if (nr && (t->p[P_FTYPE] || t->p[P_FCUT] < 127 || t->p[P_FRES] || tf_on[(uint32_t)(t - trk) % NTRK] ||
               ((e == &ENG_WHEEL || e == &ENG_DRUM) && (t->p[P_LD_FLT] || t->p[P_ED_FLT])) ||   /* (0.6.81: their
                                                         * only filter, for the LFO / ENV -> FILTER) */
               tf_xb[(uint32_t)(t - trk) % NTRK] || tf_cmb[(uint32_t)(t - trk) % NTRK]))
        /* (JIANT 0.5) the FILTER (track_post); 0.6.8: and a block more to leave the chain (track_filter's fade) */
        track_post(t, e, out, n);
    if (snap)
        memcpy(t->p, p_new, sizeof p_new);
    else if (fade)
        for (i = 0; i < 8u; i++)
            t->p[P_E0 + i] = pe_new[i];
    if (!fade && t->xin)
        t->xin--;                                       /* (0.6.41) the fade-in's next block */
    if (fade && !--t->xf && snap)
        xf_snap_t = 0xFFu;                              /* (faded out: the snapshot done with) */
    return nr;
}
