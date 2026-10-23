/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* VOICE: formant oscillator, sung vowels from the keyboard. 4 voices.
 * After D. H. Klatt, "Software for a cascade/parallel formant synthesizer",
 * JASA 67 (1980): a glottal source (derivative of a Rosenberg (1971) pulse,
 * no DC) into four two-pole resonators in cascade, F1 -> F2 -> F3 -> F4.
 * Design follows klattsch by Tony Gies (MIT, design reference only, no code
 * copied). Formant data from Klatt (1980) and Hillenbrand et al. (1995).
 *
 *   source  rising half-sine while opening (Tp), falling quarter-sine while
 *           closing (Tn), 0 while closed; the closure step goes through blep()
 *   BUZZ    open quotient 0.9 .. 0.35 and a one-pole spectral tilt
 *   BREATH  noise, louder while the glottis is open, mixed before the
 *           resonators; at the top the voicing fades out (whisper)
 *   F1..F4  y = A x + B y1 + C y2, C = -r^2, B = 2 r cos(2 pi F / FS),
 *           A = 1 - B - C (unity gain at DC), Q30 coefficients per block;
 *           F4 is fixed at 3300 Hz and moves only with SHIFT / key tracking
 *   PRES    out = y + k (y - y[-1]), a first-difference shelf; k follows BUZZ
 *
 * Voice state: ph[0] glottal phase, ph[1..2] F4 y1 / y2, s[0..5] F1..F3
 * y1 / y2, s[6] tilt, s[7] TALK progress (Q24, bits 0..24) and the note's
 * random vowel (bits 25..31). Formant key tracking is fixed at 30 / 127.
 *
 * (JIANT 0.8.5) WORDS: with LANG on (EDIT > WORDS) the track sings syllables, words and lyrics (sing.c): the formants
 * and the source's gains come from the track's articulation (sing_block, the block hook), the same source and
 * cascade render them (formant_word_render), with noise for the consonants: aspiration into the cascade, frication
 * into F4 (tuned to the consonant while it sounds). The EDIT values read POS THEME ARTIC SHIFT | BUZZ BREATH Q LOOP
 * then (formant_desc). LANG OFF: every sound as before, bit for bit.
 *
 * (0.9.8) A singer: WORDS' frication band-passed (1 - z^-2) and softer, a vibrato on held notes, jitter and shimmer each
 * glottal period, a softer source; VOICE's glide (LANG OFF too) an S-curve in pitch (glide_sung, fmt_sung). */
#define FMT_FOLOW 30
#include "sing.c"
#define FMT_TALK_MASK 0x01FFFFFF

/* formants (Hz) of A E I O U: adult male means, Hillenbrand et al., JASA 97
 * (1995) (/a/ hod, /E/ head, /i/ heed, /o/ hoed, /u/ who'd); bandwidths (Hz)
 * in the range of Klatt (1980) and of measured vowel bandwidths */
static const uint16_t VOWEL_F[5][3] = {
    {768, 1333, 2522}, {580, 1799, 2605}, {342, 2322, 3000}, {497, 910, 2459}, {378, 997, 2343},
};
static const uint8_t VOWEL_B[5][3] = {
    {90, 100, 160}, {70, 100, 170}, {50, 120, 200}, {70, 80, 150}, {60, 80, 150},
};
/* 2^(k/12), Q16, k = 0..12 */
static const uint32_t SEMI_Q16[13] = {
    65536, 69433, 73562, 77936, 82570, 87480, 92682, 98193, 104032, 110218, 116772, 123715, 131072,
};

/* 2^(st16 / 192) in Q16 (st16 in 1/16 semitone), clamped to 1/16 .. 16.
 * Divisions by constants are written as multiplies: -Os keeps them as divides */
static uint32_t fmt_ratio(int32_t st16)
{
    int32_t o, k;
    uint32_t r;
    st16 = clamp(st16, -4 * 192, 4 * 192 - 1) + 4 * 192;   /* 0 .. 8 octaves */
    o = (st16 * 2731) >> 19;                            /* / 192, exact for 0 .. 1535 */
    k = st16 - o * 192;
    r = SEMI_Q16[k >> 4] + (((SEMI_Q16[(k >> 4) + 1] - SEMI_Q16[k >> 4]) * (uint32_t)(k & 15)) >> 4);
    return o >= 4 ? r << (o - 4) : r >> (4 - o);
}

/* the three formants of vowel position pos (0..127 << 8, A E I O U), Hz * 16 */
static void vowel_at(int32_t pos, int32_t *f, int32_t *b)
{
    int32_t x = (clamp(pos, 0, 127 << 8) * 33027) >> 20, i = x >> 8, fr = x & 255, j;   /* * 4 / 127 */
    if (i >= 4) {
        i = 3;
        fr = 256;
    }
    for (j = 0; j < 3; j++) {
        f[j] = (VOWEL_F[i][j] << 4) + (((VOWEL_F[i + 1][j] - VOWEL_F[i][j]) * fr) >> 4);
        b[j] = (VOWEL_B[i][j] << 4) + (((VOWEL_B[i + 1][j] - VOWEL_B[i][j]) * fr) >> 4);
    }
}

/* (0.9.8) VOICE glides as a singer does: the glide's course (pitch_cur, linear in semitones: log frequency, its time
 * GLIDE's; with TIME's fraction carried, gacc) eased in and out, an S-curve (3u^2 - 2u^3 of the way from gfrom), so the
 * pitch leaves the note and lands on the next one gently. In 1/4096 semitone (1/16 st << 8: fmt_sung puts the fraction
 * in the fine tune, no 1/16 st staircase). The other engines: pitch_cur, as before (voice.c, untouched) */
static int32_t glide_sung(const voice_t *v)
{
    int32_t d = v->pitch16 - v->gfrom, u;
    if (!d || v->pitch_cur == v->pitch16)
        return v->pitch_cur << 8;
    u = ((((v->pitch_cur - v->gfrom) << 8) + (d > 0 ? v->gacc : -v->gacc)) << 4) / d;   /* Q12 (|d| < 2048) */
    if ((uint32_t)u > 4096u)                            /* (not this glide's: as it is) */
        return v->pitch_cur << 8;
    u = (((u * u) >> 12) * (3 * 4096 - 2 * u)) >> 12;
    return (v->gfrom << 8) + ((d * u) >> 4);
}
/* the phase increment of the pitch as sung: m's (voice.c: the glide's linear course and the rest) moved by the S-curve's
 * offset from that course; *p16 the pitch (1/16 st). Only while the voice glides (the renders' block header) */
static __attribute__((noinline)) uint32_t fmt_sung(const voice_t *v, const vmod_t *m, int32_t *p16)
{
    int32_t g = glide_sung(v) - (v->pitch_cur << 8), fine = m->fine + (((g & 255) * 237) >> 12);   /* (1/16 st: 14.8 / 4096) */
    uint32_t inc;
    *p16 = clamp(m->pitch16 + (g >> 8), 0, 2047);
    inc = pitch_inc(*p16);
    return fine ? inc + (uint32_t)((int32_t)(inc >> 12) * fine) : inc;
}

typedef struct { int32_t a, b, c; } fres_t;
static int32_t formant_nz = 0x2545F491;                 /* breath noise, one xorshift for all voices */

/* Klatt resonator coefficients (Q30) for F, BW in Hz * 16 */
static void fres_coef(fres_t *r, uint32_t f16, uint32_t bw16)
{
    int32_t s = sine_i(f16 * 3044u);                    /* sin(pi F / FS), Q15 (2^32 / (2 FS) / 16) */
    int32_t x = (int32_t)(bw16 * 4781u);                /* pi BW / FS, Q30 (<= 0.2) */
    int32_t x2 = (int32_t)(((int64_t)x * x) >> 30), x3 = (int32_t)(((int64_t)x2 * x) >> 30);
    int32_t x4 = (int32_t)(((int64_t)x3 * x) >> 30);
    int32_t e = (1 << 30) - x + (x2 >> 1) - (int32_t)(((int64_t)x3 * 178956971) >> 30)   /* exp(-x): x^3 / 6, */
                + (int32_t)(((int64_t)x4 * 44739243) >> 30);                            /* x^4 / 24 */
    int32_t cs = (1 << 30) - 2 * s * s;                 /* cos(2 pi F / FS) = 1 - 2 sin^2, Q30 */
    r->b = (int32_t)(((int64_t)e * cs) >> 29);          /* 2 r cos */
    r->c = -(int32_t)(((int64_t)e * e) >> 30);          /* -r^2 */
    r->a = (1 << 30) - r->b - r->c;
}

static void formant_note_on(track_t *t, voice_t *v)
{
    if (t->p[P_VLANG]) {                                /* WORDS: the next syllable (sing_block); a fresh voice from */
        sing_note(t);                                   /* rest (s[7] unused) */
        if (!v->env && !v->env_out) {
            v->ph[0] = v->ph[1] = v->ph[2] = 0;
            v->s[0] = v->s[1] = v->s[2] = v->s[3] = v->s[4] = v->s[5] = v->s[6] = v->s[7] = 0;
        }
        return;
    }
    v->s[7] = (int32_t)((vs_rnd >> 25) << 25);          /* TALK starts over; a random vowel (RAND; ctl64: drawn of
                                                         * formant_nz when the note was asked for, voice.c voice_begin) */
    if (!v->env && !v->env_out) {                       /* a fresh voice (not a retrigger): from rest */
        uint32_t i;
        v->ph[0] = 0;                                   /* the opening starts at zero: no click */
        v->ph[1] = v->ph[2] = 0;
        for (i = 0; i < 7u; i++)
            v->s[i] = 0;
    }
}

/* WORDS (sing.c): the articulation's formants and gains through the same source and cascade; aspiration noise into
 * F1 with the breath's, frication (first-difference noise: high-passed) into F4, F4 at the consonant's resonance
 * while it sounds (3300 Hz between). The gains ramp across the block (sing_t pv -> gv): no step, no click */
static void __attribute__((noinline)) formant_word_render(track_t *t, voice_t *v, int32_t *out, uint32_t n,
                                                          const vmod_t *m)
{
    static const uint8_t BW0[3] = {80, 100, 160};      /* the bandwidths of F1 F2 F3 at x1 (Hz) */
    const int16_t *p = t->p;
    const sing_t *sg = sing_view(t);
    sing_x_t *sx = &sing_x[(uint32_t)(t - trk) % NTRK];
    fres_t r1, r2, r3, r4;
    int32_t fa[3], ba[3], j, buzz, oq, tilt, gvb, gn, og, ogk, gv, dgv, ga, dga, gf, dgf, fw, hum, jd, jr, shim, bm, bw[3];
    uint32_t inc = m->inc, incj, i, te, tp, rp, rn, ratio, bws, f0, r4r;
    uint32_t ph = v->ph[0];
    int32_t y1 = v->s[0], y2 = v->s[1], y3 = v->s[2], y4 = v->s[3], y5 = v->s[4], y6 = v->s[5];
    int32_t y7 = (int32_t)v->ph[1], y8 = (int32_t)v->ph[2], lp = v->s[6], nst = formant_nz, nzp = 0, nzq = 0;
    int32_t p16 = m->pitch16, incq;
    /* (0.9.8) the singer: a vibrato on a held note (~5.5 Hz, +-~20 ct, after ~0.3 s, in over ~0.4 s; none while the
     * pitch glides, then it waits again), each voice of a choir at its own phase; each glottal period its own length
     * (jitter, ~0.3 %) and level (shimmer, ~0.4 dB). BUZZ at its top (the last ~12 %) steadies them away: a machine */
    hum = clamp((127 - p[P_E4]) * 4, 0, 64);
    if (v->pitch_cur != v->pitch16) {                   /* gliding: as sung (an S-curve), no vibrato */
        inc = fmt_sung(v, m, &p16);
        sx->vb = 0;
    }
    {
        int32_t vd = (clamp((int32_t)sx->vb - 400, 0, 512) * hum) >> 9;   /* 0 .. 64 */
        int32_t vib = (sine_i((uint32_t)(sx->vph + (uint32_t)(v - t->v) * 0x3A00u) << 16) * vd * 3) >> 17;
        inc += (uint32_t)((int32_t)(inc >> 12) * vib);   /* (+-48 / 4096: +-20 ct) */
    }
    jd = (hum * 3) >> 4;                                 /* +-12 / 4096 at the most */
    jr = v->vjit;                                        /* (the period under way: as it began) */
    incq = (int32_t)(inc >> 12);
    incj = inc + (uint32_t)(incq * jr);
    shim = 32768 + (jr << 7);
    ratio = fmt_ratio(p[P_E3] * 16 + ((FMT_FOLOW * (p16 - 60 * 16) * 516) >> 16));   /* (the pitch sung: its glide) */
    bws = fmt_ratio((64 - p[P_E6]) * 4);
    f0 = (uint32_t)(((uint64_t)inc * 705600u) >> 32);
    bm = clamp(sg->bm, 8, 64);
    bw[0] = BW0[0] * (bm < 24 ? bm : 24);               /* (0.9.8) a nasal's, a lateral's F1 less widened than F2 F3: */
    bw[1] = BW0[1] * bm;                                /* the murmur low and round, the mouth's formants damped */
    bw[2] = BW0[2] * bm;
    for (j = 0; j < 3; j++) {
        int32_t f = (int32_t)(((int64_t)(sg->f[j] << 4) * ratio) >> 16);
        int32_t b = (int32_t)(((((int64_t)bw[j] * ratio) >> 16) * bws) >> 16);
        if (j == 0 && (uint32_t)f < f0 + (f0 >> 4) && f > 0) {
            b = (int32_t)(((int64_t)b * (int32_t)(f0 + (f0 >> 4))) / f);
            f = (int32_t)(f0 + (f0 >> 4));
        }
        fa[j] = clamp(f, 60 * 16, 7000 * 16);
        ba[j] = clamp(b, 20 * 16, 1500 * 16);
    }
    fres_coef(&r1, (uint32_t)fa[0], (uint32_t)ba[0]);
    fres_coef(&r2, (uint32_t)fa[1], (uint32_t)ba[1]);
    fres_coef(&r3, (uint32_t)fa[2], (uint32_t)ba[2]);
    fw = clamp(sg->gf * 16, 0, 32767);                  /* frication's weight: F4 its resonance (SHIFT not on it), */
    r4r = ratio + (uint32_t)((((int32_t)65536 - (int32_t)ratio) * fw) >> 15);   /* no presence on the noise */
    fres_coef(&r4, (uint32_t)clamp((int32_t)(((uint32_t)clamp(sg->ff, 200, 7600) * 16u * (uint64_t)r4r) >> 16),
                                   60 * 16, 7600 * 16),
              (uint32_t)clamp((int32_t)(((uint32_t)clamp(sg->fb, 50, 5000) * 16u * (uint64_t)r4r) >> 16), 20 * 16,
                              5000 * 16));
    buzz = clamp((p[P_E4] << 8) + m->shape - (64 << 8) + (v->vel - 96) * 40, 0, 127 << 8);
    oq = 58982 - ((buzz * 284) >> 8);
    tilt = 1900 + ((buzz * 96) >> 8);                   /* (0.9.8) a softer source than the vowels' (~6 kHz at 80 %) */
    te = (uint32_t)oq << 16;
    tp = (te >> 8) * 166u;
    rp = (1u << 30) / ((tp >> 16) | 1u);
    rn = (1u << 30) / (((te - tp) >> 16) | 1u);
    gvb = 32767 - p[P_E5] * p[P_E5] * 2;                /* BREATH: voicing fades at the top, noise comes in */
    gn = p[P_E5] * 258;
    gv = (sg->pv * gvb) >> 15;                          /* the articulation's gains, ramped over the block */
    dgv = (((sg->gv * gvb) >> 15) - gv) >> CTL_LOG2;
    ga = sg->pa;
    dga = (sg->ga - ga) >> CTL_LOG2;
    gf = sg->pf;
    dgf = (sg->gf - gf) >> CTL_LOG2;
    og = 13000 - ((p[P_E6] - 64) * 50);
    og = (int32_t)(((int64_t)og * fmt_ratio((60 * 16 - p16) / 4)) >> 16);
    ogk = (int32_t)(((int64_t)((og >> 2) * (6 + (buzz >> 11))) * (32767 - fw)) >> 15);   /* (0.9.8: less presence) */
    for (i = 0; i < n; i++) {
        int32_t e, x, a, s, nz, fz;
        if (ph < tp)
            e = (sine_i(((ph >> 16) * rp) << 1) * 17644) >> 15;
        else if (ph < te)
            e = -sine_i(((ph - tp) >> 16) * rn);
        else
            e = 0;
        e += blep(ph - te, incj) >> 1;
        e = (e * shim) >> 15;
        lp += ((e - lp) * tilt) >> 14;
        x = (lp * gv) >> 15;
        gv += dgv;
        nz = (int32_t)(noise32(&nst) >> 16) - 32768;
        if (gn)
            x += ph < te ? (nz * gn) >> 15 : (nz * gn) >> 16;   /* breath: with the glottis open */
        x += (nz * ga) >> 15;                           /* aspiration: h, after p t k */
        ga += dga;
        fz = ((nz - nzq) * gf) >> 15;                   /* frication: noise, (0.9.8) band-passed (1 - z^-2: none at DC */
        nzq = nzp;                                      /* nor at the top, no fizz past ~12 kHz), into F4 */
        nzp = nz;
        gf += dgf;
        a = (int32_t)(((int64_t)r1.a * (x << 6) + (int64_t)r1.b * y1 + (int64_t)r1.c * y2 + (1 << 29)) >> 30);
        y2 = y1;
        y1 = a;
        a = (int32_t)(((int64_t)r2.a * a + (int64_t)r2.b * y3 + (int64_t)r2.c * y4 + (1 << 29)) >> 30);
        y4 = y3;
        y3 = a;
        a = (int32_t)(((int64_t)r3.a * a + (int64_t)r3.b * y5 + (int64_t)r3.c * y6 + (1 << 29)) >> 30);
        y6 = y5;
        y5 = a;
        a = (int32_t)(((int64_t)r4.a * (a + (fz << 6)) + (int64_t)r4.b * y7 + (int64_t)r4.c * y8 + (1 << 29)) >> 30);
        y8 = y7;
        y7 = clamp(a, -(1 << 28), 1 << 28);
        s = soft_knee(clamp((int32_t)(((int64_t)y7 * og + (int64_t)(y7 - y8) * ogk) >> 21), -200000, 200000), 24000);
        ph += incj;
        if (ph < incj) {                                /* a glottal period begins (the glottis shut: no step) */
            jr = (nz * jd) >> 15;
            incj = inc + (uint32_t)(incq * jr);
            shim = 32768 + (jr << 7);
        }
        out[i] += voice_amp(s, m, i) << 1;
    }
    v->ph[0] = ph;
    v->ph[1] = (uint32_t)y7;
    v->ph[2] = (uint32_t)y8;
    v->vjit = (int8_t)jr;
    formant_nz = nst;
    v->s[0] = y1;
    v->s[1] = y2;
    v->s[2] = y3;
    v->s[3] = y4;
    v->s[4] = y5;
    v->s[5] = y6;
    v->s[6] = lp;
}

static void formant_block(track_t *t) { sing_block(t); }   /* (LANG OFF: nothing) */
/* WORDS: the release sings the coda (the last consonants) at the level the note had, not faded with the release: the
 * level at the release (s[7]) held while a timed coda phoneme sounds (a held voiced one, n l m, fades as the ADSR
 * does). LANG OFF: the ADSR as it is */
static int32_t formant_amp(track_t *t, voice_t *v, int32_t adsr)
{
    const sing_t *s;
    if (!t->p[P_VLANG])
        return adsr;
    if (v->gate) {
        v->s[7] = adsr;
        return adsr;
    }
    s = sing_view(t);
    return s->cdon && s->i > s->nu && s->i < s->np && s->tb && v->s[7] > adsr ? v->s[7] : adsr;
}

/* WORDS: the EDIT values relabelled (the same ranges and defaults): POS (0 AUTO: each note the next syllable; 1..127
 * that syllable of the lyric at the next note, as it changes: a lock, automation), THEME (the dice's words), ARTIC (the
 * consonants: 100 % crisp .. 0 % slurred; stored the other way, 0 = 100 %), LOOP (the lyric from its start every 1 2 4
 * 8 loops of the pattern, FREE: on through it) */
static const param_desc_t FMT_WDESC[8] = {
    {"POS", F_INT, 0, 127, 0, 0, 0}, {"THEME", F_INT, 0, 127, 64, N_THEME, 0}, {"ARTIC", F_PCTINV, 0, 127, 0, 0, 0},
    {0}, {0}, {0}, {0}, {"LOOP", F_INT, 0, 127, 0, N_LOOP, 0},
};
static const param_desc_t *formant_desc(const track_t *t, uint32_t k)
{
    const param_desc_t *d = &FMT_WDESC[k & 7u];
    return t->p[P_VLANG] && d->label ? d : 0;
}

static void formant_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    fres_t r1, r2, r3, r4;
    int32_t fa[3], ba[3], fb[3], bb[3], k, j, buzz, oq, tilt, gv, gn, og, ogk;
    uint32_t inc = m->inc, i, te, tp, rp, rn, ratio, bws, f0;
    uint32_t ph = v->ph[0];
    int32_t y1 = v->s[0], y2 = v->s[1], y3 = v->s[2], y4 = v->s[3], y5 = v->s[4], y6 = v->s[5];
    int32_t y7 = (int32_t)v->ph[1], y8 = (int32_t)v->ph[2], lp = v->s[6], nst = 0;

    if (p[P_VLANG]) {                                   /* (0.8.5) WORDS */
        formant_word_render(t, v, out, n, m);
        return;
    }
    if (v->pitch_cur != v->pitch16) {                   /* (0.9.8) gliding: as sung, an S-curve */
        int32_t p16;
        inc = fmt_sung(v, m, &p16);
    }
    /* vowel: VOWEL -> VOWL2 over TALK after note-on; ENV / LFO -> FLT move both */
    k = 0;
    if (p[P_E2]) {
        uint32_t g = ((uint32_t)v->s[7] & FMT_TALK_MASK) + ENV_LIN[p[P_E2] & 127];
        g = g > (1u << 24) ? 1u << 24 : g;
        v->s[7] = (int32_t)(((uint32_t)v->s[7] & ~(uint32_t)FMT_TALK_MASK) | g);
        k = (int32_t)(g >> 9);                          /* Q15 */
        k = (k * (65536 - k)) >> 15;                    /* ease out: the mouth opens fast, then settles */
    }
    {   /* RAND: both vowels move towards this note's random vowel (0 = off, 127 = fully random) */
        int32_t rv = (int32_t)(((uint32_t)v->s[7] >> 25) << 8), v0 = p[P_E0] << 8, v1 = p[P_E1] << 8;
        v0 += ((rv - v0) * p[P_E7] * 258) >> 15;
        v1 += ((rv - v1) * p[P_E7] * 258) >> 15;
        vowel_at(v0 + m->cutoff, fa, ba);
        /* TALK=0 (or its exact onset): only the first vowel contributes.
         * Avoid preparing and blending a second unused formant set. */
        if (k) {
            vowel_at(v1 + m->cutoff, fb, bb);
            for (j = 0; j < 3; j++) {
                fa[j] += ((fb[j] - fa[j]) * k) >> 15;
                ba[j] += ((bb[j] - ba[j]) * k) >> 15;
            }
        }
    }
    /* SHIFT (semitones) and FOLOW (key tracking around C4, fixed), one ratio for all four */
    ratio = fmt_ratio(p[P_E3] * 16 + ((FMT_FOLOW * (v->pitch_cur - 60 * 16) * 516) >> 16));   /* FOLOW / 127 */
    bws = fmt_ratio((64 - p[P_E6]) * 4);                /* Q: bandwidth x2.5 .. x0.4 */
    f0 = (uint32_t)(((uint64_t)inc * 705600u) >> 32);   /* F0 in Hz * 16 */
    for (j = 0; j < 3; j++) {
        int32_t f = fa[j], b = ba[j];
        f = (int32_t)(((int64_t)f * ratio) >> 16);
        b = (int32_t)(((((int64_t)b * ratio) >> 16) * bws) >> 16);
        if (j == 0 && (uint32_t)f < f0 + (f0 >> 4) && f > 0) {   /* sung high: F1 follows F0 (sopranos do), */
            b = (int32_t)(((int64_t)b * (int32_t)(f0 + (f0 >> 4))) / f);   /* same Q: no boom on the fundamental */
            f = (int32_t)(f0 + (f0 >> 4));
        }
        fa[j] = clamp(f, 60 * 16, 7000 * 16);
        ba[j] = clamp(b, 20 * 16, 1500 * 16);
    }
    fres_coef(&r1, (uint32_t)fa[0], (uint32_t)ba[0]);
    fres_coef(&r2, (uint32_t)fa[1], (uint32_t)ba[1]);
    fres_coef(&r3, (uint32_t)fa[2], (uint32_t)ba[2]);
    fres_coef(&r4, (uint32_t)clamp((int32_t)((3300u * 16u * (uint64_t)ratio) >> 16), 60 * 16, 7000 * 16),
              (uint32_t)clamp((int32_t)((((250u * 16u * (uint64_t)ratio) >> 16) * bws) >> 16), 20 * 16, 1500 * 16));

    /* source: BUZZ (+ SHP modulation, + velocity) -> open quotient and tilt */
    buzz = clamp((p[P_E4] << 8) + m->shape - (64 << 8) + (v->vel - 96) * 40, 0, 127 << 8);
    oq = 58982 - ((buzz * 284) >> 8);                   /* Q16: 0.90 .. 0.35 */
    tilt = 2500 + ((buzz * 109) >> 8);                  /* one-pole coefficient Q14: dark .. open */
    te = (uint32_t)oq << 16;                            /* closure (end of the open phase) */
    tp = (te >> 8) * 166u;                              /* opening: 65 % of it, closing 35 % */
    rp = (1u << 30) / ((tp >> 16) | 1u);
    rn = (1u << 30) / (((te - tp) >> 16) | 1u);
    gv = 32767 - p[P_E5] * p[P_E5] * 2;                 /* BREATH: voicing fades at the top */
    gn = p[P_E5] * 258;                                 /* noise: Q15, up to 1.0 */
    if (gn) nst = formant_nz;
    og = 13000 - ((p[P_E6] - 64) * 50);                 /* output: narrower bands ring louder */
    og = (int32_t)(((int64_t)og * fmt_ratio((60 * 16 - v->pitch_cur) / 4)) >> 16);   /* 1.5 dB / oct: low notes are sparse pulses */
    ogk = (og >> 2) * (4 + (buzz >> 10));               /* presence k = 1 .. 4.9 */

    for (i = 0; i < n; i++) {
        int32_t e, x, a, s;
        if (ph < tp)                                    /* opening: + sin(pi u) * Tn / Tp */
            e = (sine_i(((ph >> 16) * rp) << 1) * 17644) >> 15;
        else if (ph < te)                               /* closing: - sin(pi / 2 u) */
            e = -sine_i(((ph - tp) >> 16) * rn);
        else
            e = 0;
        e += blep(ph - te, inc) >> 1;                   /* the closure step, band-limited */
        lp += ((e - lp) * tilt) >> 14;
        x = (lp * gv) >> 15;
        if (gn) {
            int32_t nz = (((int32_t)(noise32(&nst) >> 16) - 32768) * gn) >> 15;
            x += ph < te ? nz : nz >> 1;                /* aspiration follows the glottal opening */
        }
        /* F1 -> F2 -> F3 -> F4, Q30, rounded; the signal x 64 inside: the rounding
         * dead band of F1 (+-0.5 / A, ~340 at 270 Hz) stays below 6 LSB */
        a = (int32_t)(((int64_t)r1.a * (x << 6) + (int64_t)r1.b * y1 + (int64_t)r1.c * y2 + (1 << 29)) >> 30);
        y2 = y1;
        y1 = a;
        a = (int32_t)(((int64_t)r2.a * a + (int64_t)r2.b * y3 + (int64_t)r2.c * y4 + (1 << 29)) >> 30);
        y4 = y3;
        y3 = a;
        a = (int32_t)(((int64_t)r3.a * a + (int64_t)r3.b * y5 + (int64_t)r3.c * y6 + (1 << 29)) >> 30);
        y6 = y5;
        y5 = a;
        a = (int32_t)(((int64_t)r4.a * a + (int64_t)r4.b * y7 + (int64_t)r4.c * y8 + (1 << 29)) >> 30);
        y8 = y7;
        y7 = clamp(a, -(1 << 28), 1 << 28);             /* int32 state stays far from overflow */
        s = soft_knee(clamp((int32_t)(((int64_t)y7 * og + (int64_t)(y7 - y8) * ogk) >> 21), -200000, 200000), 24000);
        ph += inc;
        out[i] += voice_amp(s, m, i) << 1;
    }
    v->ph[0] = ph;
    v->ph[1] = (uint32_t)y7;
    v->ph[2] = (uint32_t)y8;
    if (gn) formant_nz = nst;
    v->s[0] = y1;
    v->s[1] = y2;
    v->s[2] = y3;
    v->s[3] = y4;
    v->s[4] = y5;
    v->s[5] = y6;
    v->s[6] = lp;
}

static const preset_t FORMANT_PRESETS[] = {
    /* VOWEL VOWL2 TALK SHIFT | BUZZ BREATH Q RAND */
    {"CHOIR AAH", {0, 0, 0, 0, 40, 22, 60, 0}, {85, 90, 115, 95}, 0, 0, FX(0, 70, 15, 90), PAT(5)},
    {"VOX LEAD", {32, 0, 0, 2, 90, 8, 72, 0}, {6, 70, 105, 55}, 0, 1, FX(0, 20, 45, 40), PAT(4)},
    {"WOW BASS", {95, 0, 68, 0, 100, 0, 80, 0}, {0, 70, 70, 30}, 0, 1, FX(10, 0, 10, 10), PAT(8)},
    {"WHISPER", {0, 95, 88, 3, 50, 120, 50, 0}, {50, 90, 110, 90}, 0, 0, FX(0, 40, 30, 70), PAT(5)},
    /* (0.7.0) an OOH pad, a chant an octave down, a talk box lead */
    {"OOH PAD", {80, 100, 50, 0, 55, 25, 55, 0}, {70, 90, 115, 95}, 0, 0, FX(0, 60, 25, 85), PAT(5)},
    {"CHANT", {20, 60, 100, -12, 70, 15, 65, 10}, {30, 90, 110, 80}, 0, 0, FX(0, 30, 35, 75), PAT(5)},
    {"TALK BOX", {10, 110, 40, 0, 110, 4, 85, 0}, {2, 65, 95, 40}, 0, 1, FX(10, 10, 35, 30), PAT(4)},
    /* (0.8.5) WORDS (sing.c): LANG and LYRIC in WORDS(); the EDIT values POS THEME ARTIC SHIFT | BUZZ BREATH Q LOOP.
     * (0.9.8) the singers' REL longer (~190 ms to -40 dB, was ~70): a syllable's vowel carries on into the next note's
     * consonant, as sung, not cut off by the step's gate; their ATK ~5 ms (was ~1.5): a voice's onset, no tick */
    {"CANTOR ES", {0, 64, 0, 0, 80, 6, 72, 0}, {24, 70, 112, 72}, 0, 1, FX(0, 10, 30, 35), PAT(4), WORDS(LG_ES, 1)},
    {"CANTORA ES", {0, 0, 0, 4, 70, 10, 74, 0}, {26, 70, 112, 72}, 0, 1, FX(0, 15, 30, 45), PAT(4), WORDS(LG_ES, 2)},
    {"SINGER EN", {0, 64, 0, 0, 84, 6, 72, 0}, {24, 70, 112, 72}, 0, 1, FX(0, 10, 30, 35), PAT(4), WORDS(LG_EN, 3)},
    {"UTA JA", {0, 48, 0, 5, 76, 8, 74, 0}, {24, 70, 112, 70}, 0, 1, FX(0, 15, 30, 40), PAT(4), WORDS(LG_JA, 4)},
    {"WORD CHOIR", {0, 64, 16, 0, 45, 20, 62, 0}, {60, 90, 115, 85}, 0, 0, FX(0, 70, 15, 90), PAT(5), WORDS(LG_ES, 5)},
    {"SCAT EN", {0, 64, 0, 0, 96, 4, 76, 0}, {16, 60, 100, 60}, 0, 1, FX(0, 0, 35, 30), PAT(4), WORDS(LG_EN, 17)},
    {"ROBOT EN", {0, 64, 0, -3, 127, 0, 90, 0}, {2, 65, 120, 56}, 0, 1, FX(10, 10, 35, 30), PAT(4), WORDS(LG_EN, 6)},
    {"WHISPER JA", {0, 64, 0, 3, 50, 120, 52, 0}, {40, 90, 110, 80}, 0, 0, FX(0, 40, 30, 70), PAT(5), WORDS(LG_JA, 7)},
};

static const engine_t ENG_FORMANT = {
    .name = "VOICE",
    .page_title = {"VOWL", "TONE"},
    .edit = {
        {"VOWEL", F_INT, 0, 127, 0, 0, 0},
        {"VOWEL2", F_INT, 0, 127, 64, 0, 0},
        {"TALK", F_TIME, 0, 127, 0, 0, 0},
        {"SHIFT", F_SEMI, -12, 12, 0, 0, 0},
        {"BUZZ", F_PCT, 0, 127, 64, 0, 0},
        {"BREATH", F_PCT, 0, 127, 10, 0, 0},
        {"Q", F_PCT, 0, 127, 64, 0, 0},
        {"RAND", F_PCT, 0, 127, 0, 0, 0},
    },
    .presets = FORMANT_PRESETS,
    .npresets = NELEM(FORMANT_PRESETS),
    .note_on = formant_note_on,
    .render = formant_render,
    .knob = {P_E0, P_E4, P_E5, P_E2},
    .poly = 4,
    .desc = formant_desc,
    .block = formant_block,
    .amp = formant_amp,
};
