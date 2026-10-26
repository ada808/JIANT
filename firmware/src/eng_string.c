/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 * Portions Copyright (C) 2026 Juan Ignacio Antonucci (Juan Jiant) */
/* STRING (JIANT 0.9.9-snd5): a plucked, struck or bowed string, Karplus-Strong. A delay line a period long, filled
 * with the EXCITE at the note (noise, a plucked noise, a pick's triangle, a hammer's or a mallet's bump, at POS along
 * the string), read back through a two-point average, a one-pole low-pass (BRIGHT: the loss of the highs each pass)
 * and an allpass (STRETCH: a stiff string's partials sharpened, a piano's, a bar's), times the loop's gain (DECAY:
 * its T60, 50 ms .. 13 s, the same whatever the note). BOW feeds noise into the loop while the key is held (a bowed
 * or blown string's sustain); BODY / SIZE a resonant band (70 .. 800 Hz: a box under the string) mixed in.
 *
 * Memory: none of its own. The lines are the COMB filter's (voice.c comb_buf: 1024 samples a part, in the pool), cut
 * in NVOICE lines of 128; a note lower than ~350 Hz runs its loop at a half, a quarter, ... of the rate (DECIM: 2^d
 * samples a pass of the loop, the line still 128 long: down to ~22 Hz), its output interpolated (a string that low has
 * nothing above a few kHz). So a STRING part has no COMB filter: string_block turns a COMB TYPE into BP (the body's
 * band again). The voice's state: ph[0] the noise, ph[1] the line's write index (and the loop's sample count << 8),
 * ph[2] the output's DC blocker; s[0]
 * the low-pass, s[1] the average's last input, s[2] s[3] the allpass, s[4] s[5] the last two loop outputs, s[6] s[7]
 * the body's band (low, band). LFO / ENV -> SHAPE move BRIGHT. Cost: ~30 operations a sample at the top rate, fewer
 * below (a pass every 2^d samples). */
#define ST_SL 128u                                       /* a voice's line: comb_buf[part] / NVOICE */
#define ST_MAXP ((ST_SL - 6u) << 8)                      /* the longest period a line holds (Q8) */
static int16_t comb_buf[NTRK][1024] __attribute__((section(".pool")));   /* (voice.c: the COMB's lines, COMB_LEN) */
static const char *const N_STR_EXC[] = {"NOISE", "PLUCK", "PICK", "HAMMR", "STICK", "FDBK"};   /* (five letters: a knob's cell) */
enum { SX_NOISE, SX_PLUCK, SX_PICK, SX_HAMMER, SX_MALLET, SX_FDBK, SX_N };
/* (p10) FDBK: a guitar's feedback. Plucked as PLUCK, then an amp hears the string and plays it back into it: the BODY
 * page becomes the amp's. GAIN how much comes back, DIST the amp's drive (a soft clip: the loop's own limiter),
 * SPEAKR where along the period the speaker's sound meets the string (which harmonic it blooms into), BLOOM how slowly
 * the feedback grows while the key is held. BRIGHT stays the tone. No memory more: the speaker's tap is the line. */
static const param_desc_t STR_AMP[4] = {
    {"GAIN", F_PCT, 0, 127, 0, 0, 0}, {"DIST", F_PCT, 0, 127, 40, 0, 0},
    {"SPEAKR", F_PCT, 0, 127, 40, 0, 0}, {"BLOOM", F_PCT, 0, 127, 0, 0, 0}};
static const param_desc_t *string_desc(const track_t *t, uint32_t k)
{
    return k >= 4u && t->p[P_E0] == SX_FDBK ? &STR_AMP[k - 4u] : 0;
}

static int16_t *str_line(const track_t *t, const voice_t *v)
{
    return comb_buf[(uint32_t)(t - trk) % NTRK] + ((uint32_t)(v - t->v) % NVOICE) * ST_SL;
}
/* 2^-x, x Q8 (0 .. 16 octaves), Q16 (a quadratic between the octaves: 0.3 % at worst) */
static uint32_t str_exp2n(uint32_t x)
{
    uint32_t i = x >> 8, f = (x & 255u) << 8, y;
    if (i >= 16u)
        return 0;
    y = 65536u - ((f * 43024u) >> 16) + ((((f * f) >> 16) * 10256u) >> 16);
    return y >> i;
}
/* the loop's period (Q8, in the loop's samples) at phase increment inc, and its rate's shift (a pass every 2^d) */
static uint32_t str_period(uint32_t inc, uint32_t *d)
{
    uint32_t per = 0xFFFFFFFFu / ((inc >> 8) | 1u), k = 0;
    while (per > ST_MAXP && k < 5u) {
        per >>= 1;
        k++;
    }
    *d = k;
    return per > ST_MAXP ? ST_MAXP : per;
}
/* the loop's gain (Q15) for a T60 of 50 ms x 2^(DECAY / 16) at a period of real Q8 samples: g = 2^-(9.97 T / T60) */
static int32_t str_gain(uint32_t real, int32_t decay)
{
    uint32_t x = ((real >> 4) * 296u) >> 4;              /* 9.97 / (44100 x 0.05) a sample, Q16 */
    x = ((x >> 4) * (str_exp2n((uint32_t)clamp(decay, 0, 127) * 2048u / 127u) >> 4)) >> 8;
    x >>= 8;                                             /* (Q8 octaves) */
    return clamp((int32_t)(str_exp2n(x) >> 1), 0, 32740);
}

/* the EXCITE's sample i of n (z a noise sample, lp PLUCK's low-pass state, k its coefficient) */
static int32_t str_exc(uint32_t mode, uint32_t i, uint32_t n, uint32_t pp, uint32_t wd, int32_t z, int32_t *lp, int32_t k)
{
    int32_t dx;
    switch (mode) {
    case SX_NOISE: return z;
    case SX_PLUCK: *lp += ((z - *lp) * k) >> 15; return *lp * 2;
    case SX_PICK:                                        /* the string pulled aside at POS, let go: a triangle */
        return (i < pp ? (int32_t)(i * 16384u / pp) : (int32_t)((n - i) * 16384u / (n - pp + 1u))) + (z >> 3);
    default:                                             /* HAMMER MALLET: a raised cosine at POS, its width */
        dx = (int32_t)i - (int32_t)pp;
        dx = dx < 0 ? -dx : dx;
        return dx < (int32_t)wd ? (16384 + sine_i(0x40000000u + (uint32_t)dx * (0x80000000u / wd))) >> 1 : 0;
    }
}
static void string_note_on(track_t *t, voice_t *v)
{
    const int16_t *p = t->p;
    int16_t *b = str_line(t, v);
    uint32_t d, per = str_period(pitch_inc((uint32_t)clamp(v->pitch16, 0, 2047)), &d), n = per >> 8, mode = (uint32_t)p[P_E0] % SX_N;
    uint32_t i, w, pp, wd;
    if (mode == SX_FDBK) {                               /* (p10) plucked; the feedback grows again from 0 */
        mode = SX_PLUCK;
        v->s[6] = 0;
    }
    int32_t nz = (int32_t)(v->note * 2654435761u + 0x9E37u) | 1, nz2, lp = 0, lp2 = 0, amp = 9000 + v->vel * 90, sum = 0, k, mean;
    int32_t br = clamp(p[P_E2] + v->vel / 4 - 16, 0, 127);
    if (!v->env) {                                       /* (from silence: the line and the loop at rest) */
        for (i = 0; i < ST_SL; i++)
            b[i] = 0;
        for (i = 0; i < 8u; i++)
            v->s[i] = 0;
        v->ph[1] = 0;
        v->ph[2] = 0;
    } else
        nz = (int32_t)v->ph[0] | 1;
    if (n < 2u)
        n = 2;
    pp = 2u + (n * (uint32_t)clamp(p[P_E3], 0, 127)) / 256u;
    wd = mode == SX_HAMMER ? n / 8u + 1u : n / 3u + 1u;
    k = 4000 + br * 225;                                 /* (PLUCK's noise low-passed, brighter with BRIGHT and the velocity) */
    nz2 = nz;
    w = v->ph[1] % ST_SL;
    for (i = 0; i < n; i++) {                            /* (the noises at POS: less what a pick there leaves, the same
                                                         * noise POS samples behind: a comb, without a buffer) */
        uint32_t q = (w + ST_SL - n + i) % ST_SL;
        int32_t e = str_exc(mode, i, n, pp, wd, (int32_t)(noise32(&nz) >> 17) - 16384, &lp, k);
        if (mode <= SX_PLUCK && p[P_E3] && i >= pp)
            e -= str_exc(mode, i - pp, n, pp, wd, (int32_t)(noise32(&nz2) >> 17) - 16384, &lp2, k);
        e = (e * amp) >> 14;
        sum += e;
        b[q] = (int16_t)clamp((b[q] >> 1) + e, -32767, 32767);   /* (struck again: what rings halved) */
    }
    mean = sum / (int32_t)n;                             /* (no DC in the loop) */
    for (i = 0; i < n; i++) {
        uint32_t q = (w + ST_SL - n + i) % ST_SL;
        b[q] = (int16_t)clamp(b[q] - mean, -32767, 32767);
    }
    v->ph[0] = (uint32_t)nz;
}

static void string_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    int16_t *b = str_line(t, v);
    uint32_t d, per = str_period(m->inc, &d), D = 1u << d, i, w = v->ph[1] % ST_SL, cnt = (v->ph[1] >> 8) & 63u;
    int32_t lp = v->s[0], r1 = v->s[1], apx = v->s[2], apy = v->s[3], y0 = v->s[4], y1 = v->s[5], bl = v->s[6], bb = v->s[7];
    int32_t dc = (int32_t)v->ph[2], nst = (int32_t)v->ph[0] | 1, bow = v->gate ? clamp(p[P_E7], 0, 127) : 0, body = clamp(p[P_E5], 0, 127), bf;
    int32_t fb = p[P_E0] == SX_FDBK, fk = 0, fdr = 0;
    uint32_t ft = 0;
    int32_t br = clamp(p[P_E2] + ((m->shape - (64 << 8)) >> 8), 0, 127), a = 5000 + br * 218;
    int32_t s15 = clamp(p[P_E4], 0, 127) * 193, comp, dly, di, fr, g, pk = 0;
    if (fb) {                                            /* (p10) FDBK: the BODY page is the amp's */
        bl = v->gate ? bl + (8192 >> (clamp(p[P_E7], 0, 127) / 12)) : bl - (bl >> 3);   /* BLOOM: 6 ms .. 6 s up */
        bl = clamp(bl, 0, 32767);
        fk = (clamp(p[P_E4], 0, 127) * (bl >> 7)) >> 2;    /* GAIN x the bloom (Q15 / 4: a quarter at most a pass) */
        fdr = 256 + clamp(p[P_E5], 0, 127) * 24;           /* DIST: x1 .. x13 into the clip */
        s15 = 0;
        bow = 0;
        body = 0;
    }
    comp = 128 + ((32768 - a) << 8) / a + ((32768 + s15) << 8) / (32768 - s15);   /* the average's, the low-pass's, the allpass's delays */
    dly = (int32_t)per - comp;
    if (dly < (2 << 8))
        dly = 2 << 8;
    di = dly >> 8;
    fr = dly & 255;
    ft = 1u + (((uint32_t)di * (12u + 2u * (uint32_t)clamp(p[P_E6], 0, 127))) >> 8);   /* SPEAKR: 0.05 .. 1 period back */
    if (ft > ST_SL - 2u)
        ft = ST_SL - 2u;
    g = str_gain(per << d, p[P_E1]);
    if (!v->gate) {                                      /* (p5) let go: RELEASE the damper (low: a palm mute; high: it
                                                         * rings its DECAY out) */
        g = (g * str_gain(per << d, p[P_REL])) >> 15;   /* (both < 2^15: in 32 bits) */
        if ((v->ph[1] >> 16) < 512u)                     /* (under ~-36 dB: damped out, no limit cycle left ringing) */
            g = (g * 29000) >> 15;
    }
    bf = (int32_t)((327u * str_exp2n(2048u - (uint32_t)clamp(p[P_E6], 0, 127) * 896u / 127u)) >> 8);   /* (2 sin(pi f / FS), Q15: 70 .. 790 Hz) */
    if (n > CTL)
        n = CTL;
    for (i = 0; i < n; i++) {
        int32_t s;
        if (++cnt >= D) {                                /* a pass of the loop */
            int32_t x0 = b[(w + ST_SL - (uint32_t)di) % ST_SL], x1 = b[(w + ST_SL - (uint32_t)di - 1u) % ST_SL];
            int32_t r = x0 + (((x1 - x0) * fr) >> 8), x, yy;
            x = (r + r1) >> 1;
            r1 = r;
            if (bow)                                     /* BOW (p5): noise into the loop before its low-pass, so what
                                                         * sounds is the string, not a layer of noise over it */
                x += (((int32_t)(noise32(&nst) >> 20) - (int32_t)(noise32(&nst) >> 20)) * bow) >> 7;
            if (fk) {                                    /* the amp: the string heard SPEAKR back, driven into a soft
                                                         * clip (3x - x^3) / 2, played back into the string */
                int32_t c = b[(w + ST_SL - ft) % ST_SL];
                bb += (c - bb) >> 5;                     /* (its DC tracked and taken out: none builds up in the loop) */
                c = clamp(((c - bb) * fdr) >> 8, -32767, 32767);
                c = (3 * c - ((((c * c) >> 15) * c) >> 15)) >> 1;
                x += (c * fk) >> 15;
            }
            lp += ((x - lp) * a) >> 15;
            yy = ((-s15 * (lp - apy)) >> 15) + apx;      /* (the allpass, its coefficient -STRETCH) */
            apx = lp;
            apy = clamp(yy, -32767, 32767);
            yy = apy * g / 32768;                        /* (towards 0: no limit cycle left ringing) */
            yy = clamp(yy, -32767, 32767);
            b[w] = (int16_t)yy;
            w = (w + 1u) % ST_SL;
            y0 = y1;
            y1 = yy;
            cnt = 0;
        }
        s = y0 + (((y1 - y0) * (int32_t)cnt) >> d);
        if (body) {                                      /* BODY: a band at SIZE under the string */
            int32_t hi;
            bl += (bf * bb) >> 15;
            hi = s - bl - ((10900 * bb) >> 15);
            bb += (bf * hi) >> 15;
            bb = clamp(bb, -65536, 65536);
            s += (bb * body) >> 8;
        }
        if (fk)                                          /* (FDBK's loop rides at the clip: -15 dB once bloomed, beside a
                                                         * pluck) */
            s = (s * (32 - ((bl * 26) >> 15))) >> 5;
        dc += ((s << 12) - dc) >> 10;                    /* (the output's DC blocked: ~7 Hz; Q12, what sticks under a
                                                         * quarter of a step: rounded away) */
        s -= (dc + 2048) >> 12;
        pk = s > pk ? s : -s > pk ? -s : pk;
        out[i] += voice_amp(s, m, i);
    }
    v->ph[0] = (uint32_t)nst;
    v->ph[1] = w | cnt << 8 | (uint32_t)(pk > 65535 ? 65535 : pk) << 16;   /* (p5: the block's peak, string_done) */
    v->ph[2] = (uint32_t)dc;
    v->s[0] = lp; v->s[1] = r1; v->s[2] = apx; v->s[3] = apy; v->s[4] = y0; v->s[5] = y1; v->s[6] = bl; v->s[7] = bb;
}
/* (p5) the string its own amplitude (ownenv): the voice ends once let go and silent (its last block's peak ~-66 dB) */
static int string_done(track_t *t, voice_t *v)
{
    (void)t;
    return !v->gate && (v->ph[1] >> 16) < 16u;
}
/* the part's FILTER: its COMB lines are the strings, so a COMB TYPE plays as BP (the body's band) */
static void string_block(track_t *t)
{
    if (FT_IS_COMB(ft_of(t->p)))
        t->p[P_FTYPE] = FT_BP;
}

/* {EXCITE, DECAY, BRIGHT, POS, STRETCH, BODY, SIZE, BOW (FDBK: GAIN DIST SPEAKR BLOOM)}, {A D S R}, fenv, mono */
static const preset_t STRING_PRESETS[] = {
    {"NYLON", {SX_PLUCK, 72, 52, 28, 0, 60, 40, 0}, {0, 64, 127, 104}, 0, TAG(PLUCK), FX(0, 0, 0, 30), PAT(3)},
    {"STEEL GTR", {SX_PICK, 86, 88, 24, 6, 40, 52, 0}, {0, 64, 127, 104}, 0, TAG(PLUCK), FX(0, 15, 0, 30), PAT(3)},
    {"HARP", {SX_PLUCK, 96, 62, 64, 0, 30, 30, 0}, {0, 64, 127, 112}, 0, TAG(PLUCK), FX(0, 0, 10, 55), PAT(6)},
    {"KOTO", {SX_PICK, 78, 98, 14, 16, 30, 62, 0}, {0, 64, 127, 104}, 0, TAG(PLUCK), FX(0, 0, 15, 40), PAT(5)},
    {"BANJO", {SX_PICK, 52, 112, 10, 4, 90, 92, 0}, {0, 64, 127, 96}, 0, TAG(PLUCK), FX(0, 0, 0, 20), PAT(4)},
    {"PIANO", {SX_HAMMER, 104, 76, 18, 42, 20, 22, 0}, {0, 64, 127, 100}, 0, TAG(KEYS), FX(0, 0, 0, 35), PAT(6)},
    {"DULCIMER", {SX_HAMMER, 92, 100, 20, 26, 36, 60, 0}, {0, 64, 127, 108}, 0, TAG(PLUCK), FX(0, 10, 10, 40), PAT(5)},
    {"MARIMBA", {SX_MALLET, 56, 40, 64, 72, 80, 70, 0}, {0, 64, 127, 96}, 0, TAG(BELL), FX(0, 0, 0, 30), PAT(5)},
    {"PLUCK BASS", {SX_PLUCK, 62, 36, 20, 0, 50, 10, 0}, {0, 64, 127, 80}, 0, 1 | TAG(BASS), FX(0, 0, 0, 5), PAT(2)},
    {"BOWED", {SX_NOISE, 118, 46, 30, 0, 50, 40, 70}, {30, 64, 127, 96}, 0, TAG(PAD), FX(0, 20, 10, 60), PAT(5)},
    {"KS NOISE", {SX_NOISE, 80, 127, 0, 0, 0, 0, 0}, {0, 64, 127, 90}, 0, TAG(FX), FX(0, 0, 15, 30), PAT(4)},
    {"METAL STR", {SX_NOISE, 100, 120, 10, 112, 0, 0, 0}, {0, 64, 127, 104}, 0, TAG(PLUCK), FX(0, 0, 20, 45), PAT(3)},
    {"FEEDBACK", {SX_FDBK, 90, 70, 24, 60, 70, 40, 60}, {0, 64, 127, 90}, 0, TAG(LEAD), FX(30, 0, 20, 35), PAT(5)},
    {"SHOEGAZE", {SX_FDBK, 110, 50, 30, 55, 100, 90, 70}, {0, 64, 127, 112}, 0, TAG(PAD), FX(20, 30, 30, 70), PAT(5)},
};

static const engine_t ENG_STRING = {
    .name = "STRING",
    .page_title = {"STRING", "BODY"},
    .edit = {
        {"EXCITE", F_ENUM, 0, SX_N - 1, SX_PLUCK, N_STR_EXC, 0},
        {"DECAY", F_PCT, 0, 127, 72, 0, 0},
        {"BRIGHT", F_PCT, 0, 127, 60, 0, 0},
        {"POS", F_PCT, 0, 127, 28, 0, 0},
        {"STRETCH", F_PCT, 0, 127, 0, 0, 0},
        {"BODY", F_PCT, 0, 127, 40, 0, 0},
        {"SIZE", F_PCT, 0, 127, 40, 0, 0},
        {"BOW", F_PCT, 0, 127, 0, 0, 0},
    },
    .presets = STRING_PRESETS,
    .npresets = NELEM(STRING_PRESETS),
    .keep = 0xF3u,                                       /* (s[0] s[1] s[4..7]: struck again, the loop rings on) */
    .note_on = string_note_on,
    .render = string_render,
    .knob = {P_E0, P_E1, P_E2, P_E3},
    .block = string_block,
    .ownenv = 1,                                         /* (p5: RELEASE its damper; the sequencer's short gates let it ring) */
    .done = string_done,
    .desc = string_desc,
};
