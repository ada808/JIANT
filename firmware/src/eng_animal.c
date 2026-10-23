/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* ANIMAL (JIANT 0.8.3): animal calls from four cheap physical primitives (reports/Síntesis bioacústica de animales.md):
 * a source that oscillates or strikes, one or two resonators it excites, and a slow layer of pressure, tension and
 * rhythm. MODE picks the source; each mode reads the eight knobs its own way (animal_desc):
 *
 *   CMOS   the owner's 40106 / 4093 "bird machine": Schmitt-trigger RC relaxation oscillators. A fast one (an octave
 *          over the key: C5 chirps at the video's ~1.05 kHz; its capacitor charging and discharging exponentially between two thresholds, its square band-limited at the
 *          fractional edge, polyBLEP) is gated (4093) and frequency-modulated (40106) by a slow one's capacitor: the
 *          exponential charge is the chirp's pitch "hook". A third, slower one groups the chirps into phrases.
 *          TOUCH the hand on the probe (a lower R: faster, less regular chirps), COUPLE a second fast oscillator
 *          coupled both ways (locking, then chaos), CAP the slow capacitor's charge / discharge split (chirp length),
 *          HYST its threshold window (the hook's depth; the sign its direction), RATE the slow one, GROUP the third,
 *          FM: 0 the 4093's gate only, 20 gate and sweep (the bird), 40 the sweep only (a siren)
 *   PULSE  escapement and buckling: an impulse train (a cricket's file teeth, a cicada's tymbal ribs, a frog's
 *          laryngeal pulses) into a two-pole resonator (wing, abdomen, vocal sac) tuned to the key. CLICK the pulse
 *          rate, 100 % one pulse a cycle of the resonance (the clockwork cricket, a pure tone) down to 1/64 of it
 *          (clicks ringing it: cicada, frog); Q the resonance (broadband katydid .. pure cricket); DETUNE the pulses
 *          off the resonance (beats, an "anomalous" song) and their jitter
 *   WHSTL  a bird's syrinx as a Hopf oscillator in polar form: the amplitude r' = mu r - r^3 born by bifurcation as the
 *          air-sac PRESS(ure) crosses the threshold (r ~ sqrt(mu): an attack without an envelope), the phase at the
 *          key; the pitch rises a little with the amplitude. SWEEP each syllable's pitch hook (the exponential
 *          approach to the key, from below or above; at fast trill rates it narrows: Podos' rate x bandwidth limit),
 *          CHAOS noise in mu and the pitch, and the second harmonic
 *   WING   flight tones: a polyBLEP pulse at the wingbeat with a slow random FM (DRIFT), BUZZ its width and
 *          brightness; each syllable a fly-by (DOPPLR its pitch shift, the level and the brightness by distance)
 *   GLOTT  a mammal's larynx, lite: a band-limited glottal pulse into two formants (MOUTH closed .. open; SIZE the
 *          tract length, every formant together, not the pitch), ROUGH in two halves of a mammal's nonlinear phenomena
 *          (period doubling up to 50 %, then a logistic map once a cycle: chaos), AIR breath noise
 *
 * The pattern (every mode but CMOS, at the control rate in animal_amp): syllables at RATE, grouped by GROUP (HOLD:
 * one syllable as long as the note; TRILL: no gaps; n+m: n syllables, m silent), all rates scaled by TEMP (Dolbear's
 * law: a cricket's chirps faster as it warms; GLOTT has AIR there: mammals keep their temperature). SIZE (but
 * GLOTT's) moves everything up or down from the key, a semitone a step (64: as played). The key sets the tension
 * (the carrier), the velocity the pressure's peak; a note starts a call, so sequenced notes are calls.
 *
 * All integer, 32-bit products. State (voice_t ph[3] + s[8], nothing more): s[2] the pattern's syllable phase,
 * s[3] the mode's tag (bits 28..31: a mode change reinitialises), the syllable count (20..27), its random speed
 * (16..19) and envelope (0..15); CMOS: s[2] the slow capacitor, s[3] the third's (bits 0..23). The rest per mode:
 *   CMOS   ph[0] the fast capacitor (Q24 of VDD), ph[1] the coupled one's, ph[2] the fast charge rate k (Q18, ramped),
 *          s[0] the sample held back for the BLEP, s[1] the tone pole, s[4] the coupling capacitor's DC, s[5] the
 *          gates' bits, s[6] the slow capacitor's pitch offset (1/16 st), s[7] the random generator
 *   PULSE  ph[0] the pulse phase, s[0] s[1] the resonator, s[4] the pulse's carried half, s[7] the generator
 *   WHSTL  ph[0] the phase, ph[1] the amplitude r (Q14), ph[2] the pitch wander, s[7] the generator
 *   WING   ph[0] the phase, ph[1] the drift, ph[2] its target and countdown, s[0] the tone pole, s[7] the generator
 *   GLOTT  ph[0] the phase, ph[1] the logistic map (Q16) and the cycle's parity (bit 31), ph[2] the tilt, s[0] s[1]
 *          F1, s[4] s[5] F2, s[6] the cycle's phase increment, s[7] the generator */
enum { AN_CMOS, AN_PULSE, AN_WHSTL, AN_WING, AN_GLOTT, AN_NMODE };
static const char *const N_ANIMAL_MODE[] = {"CMOS", "PULSE", "WHSTL", "WING", "GLOTT"};
/* GROUP: syllables sounding + silent (HOLD: one as long as the note, TRILL: no gap) */
static const char *const N_AN_GROUP[] = {"HOLD", "TRILL", "1+1", "1+3", "2+2", "2+6", "3+3", "3+9", "4+4", "4+12",
                                         "6+6", "8+8", "8+24", "12+12", "16+16", "32+32"};
static const uint8_t AN_GROUP[16][2] = {{1, 0}, {1, 0}, {1, 1}, {1, 3}, {2, 2}, {2, 6}, {3, 3}, {3, 9}, {4, 4}, {4, 12},
                                        {6, 6}, {8, 8}, {8, 24}, {12, 12}, {16, 16}, {32, 32}};
static const param_desc_t AN_DESC[AN_NMODE][8] = {
    [AN_CMOS] = {{0}, {"TOUCH", F_PCT, 0, 127, 100, 0, 0}, {"COUPLE", F_PCT, 0, 127, 0, 0, 0},
                 {"CAP", F_PCT, 0, 127, 64, 0, 0}, {"HYST", F_BIPCT, -64, 63, 0, 0, 0}, {0},
                 {"GROUP", F_INT, 0, 15, 0, 0, 0}, {"FM", F_INT, 0, 40, 22, 0, 0}},
    [AN_PULSE] = {{0}, {"CLICK", F_PCT, 0, 127, 100, 0, 0}, {"Q", F_PCT, 0, 127, 0, 0, 0}, {0},
                  {"DETUNE", F_BIPCT, -64, 63, 0, 0, 0}},
    [AN_WING] = {{0}, {"BUZZ", F_PCT, 0, 127, 100, 0, 0}, {"DRIFT", F_PCT, 0, 127, 0, 0, 0}, {0},
                 {"DOPPLR", F_BIPCT, -64, 63, 0, 0, 0}},
    [AN_GLOTT] = {{0}, {"MOUTH", F_PCT, 0, 127, 100, 0, 0}, {"ROUGH", F_PCT, 0, 127, 0, 0, 0}, {0}, {0}, {0}, {0},
                  {"AIR", F_INT, 0, 40, 22, 0, 0}},
};

static uint32_t an_mode(const int16_t *p) { return (uint32_t)clamp(p[P_E0], 0, AN_NMODE - 1); }
static const param_desc_t *animal_desc(const track_t *t, uint32_t k)
{
    const param_desc_t *d = &AN_DESC[an_mode(t->p)][k & 7u];
    return d->label ? d : 0;
}

/* 2^(x / 4096) for x <= 0 (Q12 octaves), Q15 (FM6_EXP2: 2^(i / 1024) - 1, Q30) */
static int32_t an_exp2n(int32_t x)
{
    uint32_t n = (uint32_t)(x < 0 ? -x : 0), o = n >> 12, f = n & 4095u;
    if (o >= 15u)
        return 0;
    return (int32_t)(((1u << 30) + FM6_EXP2[(4096u - f) >> 2]) >> (16u + o));
}
/* the phase increment of p16 (1/16 semitone; past MIDI 127 too, up to ~20 kHz) with the voice's fine residual */
static uint32_t an_inc(int32_t p16, int32_t fine)
{
    uint32_t inc = noise_inc(clamp(p16, 0, 2160));
    return fine ? inc + (uint32_t)((int32_t)(inc >> 12) * fine) : inc;
}
/* p16's frequency in the SVF's cutoff units (0..127 << 8: 30 Hz .. 16 kHz, 0.856 st a unit) */
static int32_t an_cut(int32_t p16) { return (((p16 - 1104) * 4785) >> 8) + 13909; }
/* 1 - e^-l (Q24 both): an RC step's charge from its rate */
static int32_t an_rck(int32_t l)
{
    int32_t a = l >> 8, l2 = (a * a) >> 8, l3 = ((l2 >> 8) * a) >> 8;
    return l - (l2 >> 1) + (l3 / 6);
}
/* TEMP, degrees C: the rates as a Q8 of 22 C's (Dolbear: linear above ~3 C) */
static int32_t an_temp(int32_t v) { int32_t s = (clamp(v, 0, 40) - 3) * 256 / 19; return s < 26 ? 26 : s; }
/* the register: SIZE 64 as played, a semitone a step (smaller: higher) */
static int32_t an_reg(const int16_t *p) { return (64 - p[P_E3]) * 16; }

#define AN_V1 (1 << 24)          /* VDD (CMOS), Q24 */
#define AN_VTH 9730785           /* the 40106's thresholds at 5 V, typical: 2.9 V and 1.9 V of 5 (0.58, 0.38) */
#define AN_VTL 6375342
#define AN_FAST_L 184            /* the fast one's charge rate per cycle a sample, Q8: ln(0.62 / 0.42) + ln(0.58 / 0.38)
                                  * / 1.28 (it discharges 1.28 x faster: a duty of ~44 %, the 2nd harmonic of the video) */
#define AN_SQ 11000              /* its square's level */
/* ln((1 + w) / (1 - w)), Q12, w = 0.12 .. 0.80 in 8 steps (|HYST| 0..64): the slow one's half period in RC */
static const uint16_t AN_LW[9] = {991, 1712, 2464, 3274, 4182, 5232, 6487, 8058, 8999};

static void animal_seed(voice_t *v)
{
    uint32_t seed = (v->note + 7u) * 0x9E3779B9u ^ (uint32_t)v->fine * 0x85EBCA6Bu;
    v->s[7] = (int32_t)(seed ? seed : 1u);
}
/* the mode's state from rest (fresh: the kept slots too) */
static void animal_init(voice_t *v, uint32_t mode, int fresh)
{
    if (fresh) {
        v->s[0] = v->s[1] = v->s[4] = v->s[5] = 0;
        v->ph[0] = v->ph[1] = v->ph[2] = 0;
    }
    v->s[2] = 0;
    v->s[3] = (int32_t)((mode + 1u) << 28);
    v->s[6] = 0;
    animal_seed(v);
    if (mode == AN_CMOS) {
        v->s[2] = AN_V1 / 2 - (AN_V1 / 16);             /* the slow capacitor low, charging: a chirp at once */
        v->s[5] = 4 | 8;                                 /* fast low, slow high (charging), the group's open */
        v->s[3] |= AN_V1 / 3;
        if (fresh) {
            v->ph[0] = (uint32_t)AN_VTL;
            v->ph[1] = (uint32_t)AN_VTL;
        }
    } else if (mode == AN_GLOTT) {
        v->ph[1] = 0x9000u;                              /* the logistic map's start (0.56) */
        v->s[6] = 0;
    } else if (mode == AN_WING) {
        v->ph[2] = 0;                                    /* a drift target at once */
    }
}

static void animal_note_on(track_t *t, voice_t *v)
{
    animal_init(v, an_mode(t->p), !v->env && !v->env_out);   /* (a retrigger keeps the kept slots: no click) */
}

/* ------------------------------------------------------- the control layer --- */
/* CMOS: the slow oscillator (the chirps) and the third (the phrases), once a block: the fast one's gate (Q15) */
static int32_t cmos_block(const track_t *t, voice_t *v)
{
    const int16_t *p = t->p;
    uint32_t bits = (uint32_t)v->s[5], ss = (bits >> 2) & 1u, sg = (bits >> 3) & 1u, r = (uint32_t)v->s[7];
    int32_t vs = v->s[2], vg = v->s[3] & 0xFFFFFF, h = p[P_E4], ah = h < 0 ? -h : h, w, vth, vtl, lw, d8, la, lb, x15;
    int32_t touch = clamp(p[P_E1] * 258 + (v->vel - 100) * 160, 0, 32767), gamt, famt, rf = 256, lvl;
    uint32_t inc;
    if (p[P_E6]) {                                       /* the third: 0.15 .. 3.4 Hz, its charge 60 % of the cycle */
        uint32_t ig = LFO_INC[20 + clamp(p[P_E6], 0, 15) * 4] >> 8;
        int32_t k = (int32_t)(sg ? (ig * 296u) >> 8 : (ig * 444u) >> 8), T = sg ? AN_V1 : 0;   /* ln 2 / .6, / .4 */
        vg += (((T - vg) >> 8) * (k >> 8)) >> 8;
        if (sg && vg >= AN_V1 * 2 / 3)
            sg = 0;
        else if (!sg && vg <= AN_V1 / 3)
            sg = 1;
        rf = 166 + (((vg - AN_V1 / 3) >> 12) * 45 >> 8);   /* the chirps speed up as it charges: x0.65 .. x1.6 */
    } else {
        sg = 1;
    }
    /* the slow one: RATE (and the touch: x1 .. x1.6), CAP its duty (15 .. 85 % charging), HYST its window */
    w = 1966 + ah * 174;                                 /* (0.12 .. 0.80, Q14) */
    vth = AN_V1 / 2 + (w << 9);
    vtl = AN_V1 / 2 - (w << 9);
    lw = AN_LW[ah >> 3] + (((AN_LW[(ah >> 3) + (ah < 64)] - AN_LW[ah >> 3]) * (ah & 7)) >> 3);
    rf = (rf * (256 + ((touch * 5) >> 10))) >> 8;   /* (the touch: x1 .. x1.6) */
    inc = (LFO_INC[p[P_E5] & 127] >> 8) * (uint32_t)rf;
    d8 = 38 + p[P_E3] * 179 / 127;
    la = (int32_t)((((inc >> 12) * (uint32_t)lw) >> 8) * 256u / (uint32_t)d8);
    lb = (int32_t)((((inc >> 12) * (uint32_t)lw) >> 8) * 256u / (uint32_t)(256 - d8));
    if (sg) {
        int32_t k = an_rck(clamp(ss ? la : lb, 0, AN_V1 / 2)), T = ss ? AN_V1 : 0;
        vs += (((T - vs) >> 9) * (k >> 8)) >> 7;
        if (ss && vs >= vth) {
            ss = 0;
            r ^= r << 13; r ^= r >> 17; r ^= r << 5;
            vs -= (((int32_t)r >> 20) * touch) >> 8;   /* the hand: the next one a little early or late */
        } else if (!ss && vs <= vtl) {
            ss = 1;
            r ^= r << 13; r ^= r >> 17; r ^= r << 5;
            vs += (((int32_t)r >> 20) * touch) >> 8;
        }
    } else {                                             /* a phrase's gap: held low, ready to chirp */
        vs = vtl;
        ss = 1;
    }
    vs = clamp(vs, 0, AN_V1);
    x15 = clamp((vs - vtl) / (((vth - vtl) >> 15) | 1), 0, 32767);
    gamt = clamp((40 - p[P_E7]) * 127 / 20, 0, 127);   /* FM 0 .. 20: the gate full (silent between), then less */
    famt = clamp(p[P_E7] * 127 / 20, 0, 127);           /* .. 0 .. 20: the sweep in */
    /* the sweep: up to 2 octaves at full HYST; up from below to the key (HYST > 0) or down from above */
    v->s[6] = (((ah * famt * 6) / 127) * (32767 - x15) >> 15) * (h < 0 ? 1 : -1);
    lvl = !sg ? 0 : ss ? 32767 : 32767 - gamt * 258;
    v->s[2] = vs;
    v->s[3] = (v->s[3] & ~0xFFFFFF) | (vg & 0xFFFFFF);
    v->s[5] = (int32_t)((bits & 3u) | ss << 2 | sg << 3);
    v->s[7] = (int32_t)r;
    return lvl;
}

/* the pattern of the other modes: the syllable's envelope (Q15) into s[3]; the voice's level (Q15) */
static int32_t animal_amp(track_t *t, voice_t *v, int32_t adsr)
{
    const int16_t *p = t->p;
    uint32_t mode = an_mode(p), s3, ph, inc, g, n, cnt, spd, env;
    if (((uint32_t)v->s[3] >> 28) != mode + 1u)          /* MODE turned while it sounds: this mode's state */
        animal_init(v, mode, 1);
    if (mode == AN_CMOS)
        return mulq15(adsr, cmos_block(t, v));
    s3 = (uint32_t)v->s[3];
    ph = (uint32_t)v->s[2];
    g = (uint32_t)clamp(p[P_E6], 0, 15);
    n = AN_GROUP[g][0];
    cnt = (s3 >> 20) & 255u;
    spd = (s3 >> 16) & 15u;
    inc = (LFO_INC[p[P_E5] & 127] >> 8) * (uint32_t)(mode == AN_GLOTT ? 256 : an_temp(p[P_E7]));
    {   /* irregularity (PULSE: |DETUNE|, else CHAOS / DRIFT / ROUGH): each syllable its own speed, +-45 % */
        int32_t irr = mode == AN_PULSE ? (p[P_E4] < 0 ? -p[P_E4] : p[P_E4]) * 2 : p[P_E2];
        if (irr)
            inc = (uint32_t)((int32_t)inc + (((int32_t)(inc >> 8) * (((int32_t)spd - 8) * irr)) >> 3));
    }
    if (!g) {                                            /* HOLD: one syllable, the note's */
        ph = ph + inc < ph ? 0xFFFFFFFFu : ph + inc;
        env = 32767;
    } else {
        uint32_t o = ph, u, x, d;
        ph += inc;
        if (ph < o) {                                    /* the next syllable (or gap) */
            uint32_t r = (uint32_t)v->s[7];
            cnt = cnt + 1u >= n + AN_GROUP[g][1] ? 0u : cnt + 1u;
            r ^= r << 13; r ^= r >> 17; r ^= r << 5;
            v->s[7] = (int32_t)r;
            spd = r >> 28;
        }
        u = ph >> 16;
        d = mode == AN_PULSE ? 32768u : mode == AN_GLOTT ? 39322u : mode == AN_WHSTL ? 45875u : 65535u;   /* its duty */
        if (cnt >= n || u >= d) {
            env = 0;
        } else {
            x = (u << 15) / d;                           /* 0..32767 across the sounding part */
            switch (mode) {
            case AN_PULSE:                               /* a fast rise, a slower fall (a cricket's syllable) */
                env = x < 6554u ? x * 5u : ((32767u - x) * 40960u) >> 15;
                break;
            case AN_GLOTT:                               /* a bark: in fast, out as (1 - x)^2 */
                env = x < 3277u ? x * 10u : ((((32767u - x) * (32767u - x)) >> 15) * 40454u) >> 15;
                break;
            case AN_WHSTL:                               /* the air sac's pressure: a smooth arch */
                env = (x * (32767u - x)) >> 13;
                break;
            default: {                                   /* WING: the level by the distance of a fly-by, 1 / (1 + z^2) */
                int32_t z = ((int32_t)x - 16384) * 5;   /* (+-2.5 at the ends) */
                env = (uint32_t)((1 << 30) / (32768 + (((z >> 1) * (z >> 1)) >> 13)));
                break;
            }
            }
            env = env > 32767u ? 32767u : env;
        }
    }
    v->s[2] = (int32_t)ph;
    v->s[3] = (int32_t)((s3 & 0xF0000000u) | cnt << 20 | spd << 16 | env);
    return mode == AN_WHSTL ? adsr : mulq15(adsr, (int32_t)env);   /* (WHSTL: the pressure makes its own level) */
}

/* the syllable's pitch hook: e^(-4u) of depth16 (1/16 st), from below (depth > 0) or above, towards the key */
static int32_t an_hook(const voice_t *v, int32_t depth16)
{
    int32_t u = (int32_t)((uint32_t)v->s[2] >> 16);
    return -((depth16 * an_exp2n(-((u * 23637) >> 16))) >> 15);
}

/* ------------------------------------------------------------------ render --- */
static void animal_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    uint32_t mode = an_mode(p), i;
    int32_t env = v->s[3] & 0xFFFF;
    switch (mode) {
    case AN_CMOS: {
        uint32_t bits = (uint32_t)v->s[5], sf = bits & 1u, sf2 = (bits >> 1) & 1u, inc;
        int32_t vf = clamp((int32_t)v->ph[0], 0, AN_V1), v2 = clamp((int32_t)v->ph[1], 0, AN_V1);
        int32_t kc = clamp((int32_t)v->ph[2], 0, 1 << 16), kn, dk, ck = 0, k2 = 0, ck2 = 0, cpl = p[P_E2];
        int32_t yd = v->s[0], lp = v->s[1], dc = v->s[4], l;
        inc = an_inc(m->pitch16 + 192 + v->s[6], m->fine);   /* (an octave up: C5 the video's ~1.1 kHz) */
        l = (int32_t)(((inc >> 8) * AN_FAST_L) >> 8);   /* the charge's rate (Q24) .. its step k (Q18) */
        kn = clamp(an_rck(clamp(l, 0, AN_V1 / 4)) >> 6, 1, 1 << 16);
        if (!kc)
            kc = kn;
        dk = (kn - kc) / (int32_t)CTL;
        if (cpl) {                                       /* COUPLE: a second fast one, 0.5 .. 0.87 x, pulling both ways */
            ck = (kn * cpl) >> 8;
            k2 = (kn * (128 + cpl * 95 / 127)) >> 8;
            ck2 = (k2 * cpl) >> 8;
        }
        for (i = 0; i < n; i++) {
            int32_t ku = kc, kk = sf ? ku : ku + (ku >> 2) + (ku >> 5), T = sf ? AN_V1 : 0, vp = vf, y, o;
            if (cpl)
                kk += sf2 ? ck : -ck;
            vf += (((T - vf) >> 10) * kk) >> 8;
            y = sf ? AN_SQ : -AN_SQ;
            if (sf ? vf >= AN_VTH : vf <= AN_VTL) {      /* an edge, d of a sample ago: back on the curve, BLEP'd */
                int32_t vt = sf ? AN_VTH : AN_VTL, ov = sf ? vf - vt : vt - vf, dv = sf ? vf - vp : vp - vf;
                int32_t d = clamp((ov << 7) / ((dv >> 8) | 1), 0, 32767), e = 32768 - d, h, st;
                sf ^= 1u;
                T = sf ? AN_V1 : 0;
                kk = sf ? ku : ku + (ku >> 2) + (ku >> 5);
                st = (((T - vt) >> 10) * kk) >> 8;
                vf = vt + (((st >> 7) * d) >> 8);
                h = sf ? 2 * AN_SQ : -2 * AN_SQ;
                yd += (h * ((d * d) >> 15)) >> 16;      /* the sample before: + H d^2 / 2 */
                y = (sf ? AN_SQ : -AN_SQ) - ((h * ((e * e) >> 15)) >> 16);   /* this one: - H (1 - d)^2 / 2 */
            }
            o = yd;
            yd = y;
            if (cpl) {
                int32_t T2 = sf2 ? AN_V1 : 0, k = k2 + (sf ? ck2 : -ck2);
                v2 += (((T2 - v2) >> 10) * k) >> 8;
                if (sf2 ? v2 >= AN_VTH : v2 <= AN_VTL)
                    sf2 ^= 1u;
            }
            lp += ((o - lp) * 12000) >> 15;              /* the breadboard and its speaker, ~3.3 kHz */
            dc += (lp - dc) >> 9;                        /* the coupling capacitor */
            out[i] += voice_amp((lp - dc) << 1, m, i);
            kc += dk;
        }
        v->ph[0] = (uint32_t)vf;
        v->ph[1] = (uint32_t)v2;
        v->ph[2] = (uint32_t)kn;
        v->s[0] = yd;
        v->s[1] = lp;
        v->s[4] = dc;
        v->s[5] = (int32_t)((bits & ~3u) | sf | sf2 << 1);
        break;
    }
    case AN_PULSE: {
        int32_t p16 = m->pitch16 + an_reg(p), d16 = ((127 - p[P_E1]) * 2322) >> 8, det = p[P_E4] >> 1, k, a, og, jit;
        int32_t ic1 = v->s[0], ic2 = v->s[1], cr = v->s[4], rng = v->s[7];
        uint32_t inc = an_inc(p16 - d16 + det, m->fine), ph = v->ph[0], rcp;
        tsvf_t c;
        k = (2731 * an_exp2n(-p[P_E2] * 145)) >> 15;     /* Q 1.5 .. 34 (damping, Q12) */
        tsvf_coef_k(&c, an_cut(p16) + m->cutoff, k < 100 ? 100 : k);
        rcp = (1u << 31) / ((inc >> 8) | 1u);
        {   /* the pulse's size for a ring of ~16000 at the resonance (w = 2 pi f / FS): in phase a cycle (CLICK 100 %)
             * it builds to a w / (pi k), alone it rings a w: a = 16000 / w x min(1, pi k D), D the cycles a pulse */
            uint32_t ic = an_inc(p16, m->fine);
            int32_t a1 = (16000 * k) / (int32_t)((inc >> 19) | 1u), a2 = (int32_t)(166886000u / ((ic >> 16) | 1u));
            a = clamp(a1 < a2 ? a1 : a2, 0, 400000);
        }
        og = 4096 + k * 4;                               /* (a broadband ring is short: louder) */
        jit = (int32_t)(((inc >> 12) * (uint32_t)(p[P_E4] < 0 ? -p[P_E4] : p[P_E4])) >> 6);
        for (i = 0; i < n; i++) {
            int32_t x = cr, v1, v2, v3;
            cr = 0;
            ph += inc;
            if (ph < inc) {                              /* a pulse, ph / inc of a sample ago: split over two */
                int32_t f = (int32_t)(((ph >> 8) * rcp) >> 16), q = (a * (f >> 3)) >> 12;
                x += q;
                cr = a - q;
                if (jit)
                    ph += (uint32_t)((((int32_t)noise32(&rng) >> 20) * jit) >> 2);
            }
            v3 = x - ic2;
            v1 = (c.a1 * ic1 + c.a2 * v3 + 4096) >> 13;
            v2 = ic2 + ((c.a2 * ic1 + c.a3 * v3 + 4096) >> 13);
            ic1 = clamp(2 * v1 - ic1, -150000, 150000);
            ic2 = clamp(2 * v2 - ic2, -150000, 150000);
            out[i] += voice_amp(soft_knee(((v1 >> 1) * og) >> 10, 26000), m, i);
        }
        v->ph[0] = ph;
        v->s[0] = ic1;
        v->s[1] = ic2;
        v->s[4] = cr;
        v->s[7] = rng;
        break;
    }
    case AN_WHSTL: {
        uint32_t ph = v->ph[0], inc, isyl = (LFO_INC[p[P_E5] & 127] >> 8) * (uint32_t)an_temp(p[P_E7]);
        int32_t r0 = clamp((int32_t)v->ph[1], 0, 24000), r = r0, rng = v->s[7], jw = (int32_t)v->ph[2], dep, mu, pr, hs, k, a2;
        /* the pressure: the syllable's arch x PRESS x the velocity; mu: past the threshold (0.2), with CHAOS's noise */
        pr = (env * ((p[P_E1] * 258 * (40 + (v->vel * 60 >> 7))) / 100)) >> 15;
        mu = pr - 6554 + ((((int32_t)noise32(&rng) >> 17) * p[P_E2]) >> 9);
        hs = 5000 + (int32_t)(isyl >> 13);              /* the growth a step (8 samples): faster for fast trills */
        hs = hs > 16000 ? 16000 : hs;
        for (k = 0; k < 4; k++) {                        /* r' = mu r - r^3 (Q14), four steps a block */
            int32_t r3 = (((r * r) >> 14) * r) >> 14;
            r += (hs * (((mu * r) >> 15) - r3)) >> 15;
            if (pr > 6554 && r < 40)
                r = 40;                                  /* (the noise floor it grows from) */
            r = clamp(r, 0, 24000);
        }
        jw += ((((int32_t)noise32(&rng) >> 22) * p[P_E2]) >> 7) - (jw >> 5);   /* the pitch wanders (CHAOS) */
        dep = p[P_E4] * 6;                               /* SWEEP: up to 2 octaves; narrower past 16 syllables a second */
        if (isyl > 49848000u && dep)                    /* (16 a second) */
            dep = (dep * (int32_t)(49848000u / (isyl >> 10))) >> 10;
        inc = an_inc(m->pitch16 + an_reg(p) + an_hook(v, dep) + (jw >> 8) + ((r * r >> 14) * 3 >> 14), m->fine);
        a2 = (r * p[P_E2]) >> 8;                         /* the second harmonic */
        v->ph[1] = (uint32_t)r;
        k = r - r0;                                      /* (the ramp in Q(14 + CTL_LOG2): exact over the CTL samples) */
        r = r0 << CTL_LOG2;
        for (i = 0; i < n; i++) {
            int32_t y = (sine_i(ph) * (r >> CTL_LOG2)) >> 14;
            if (a2)
                y += (sine_i(ph << 1) * a2) >> 15;
            ph += inc;
            r += k;
            out[i] += voice_amp(y + (y >> 1), m, i);
        }
        v->ph[0] = ph;
        v->ph[2] = (uint32_t)jw;
        v->s[7] = rng;
        break;
    }
    case AN_WING: {
        uint32_t ph = v->ph[0], inc, pw, tg = v->ph[2], g = (uint32_t)clamp(p[P_E6], 0, 15);
        int32_t dr = (int32_t)v->ph[1], rng = v->s[7], lp = v->s[0], dop = 0, lk;
        if (!(tg & 0xFFFFu)) {                           /* DRIFT: a new target every 40 .. 160 ms */
            uint32_t r = noise32(&rng);
            tg = (r & 0xFFFF0000u) | (55u + ((r >> 4) & 127u));
        }
        tg--;
        dr += (((((int32_t)tg >> 16) * p[P_E2]) >> 9) - dr) >> (10 - CTL_LOG2);   /* (Q8 of 1/16 st: +-2 st at full; a block's step) */
        if (g) {                                         /* the fly-by: higher coming, lower going (3u^2 - 2u^3) */
            int32_t u = (int32_t)((uint32_t)v->s[2] >> 17), s = (((u * u) >> 15) * (98304 - 2 * u)) >> 15;
            dop = (p[P_E4] * (32768 - 2 * s)) >> 14;
        }
        inc = an_inc(m->pitch16 + an_reg(p) + dop + (dr >> 8), m->fine);
        pw = 0x80000000u - (uint32_t)p[P_E1] * 14200000u;   /* BUZZ: 50 % .. 8 % */
        lk = clamp(3000 + p[P_E1] * 110 + ((env * (p[P_E1] + 40)) >> 10) + (m->cutoff >> 1), 1500, 30000);
        for (i = 0; i < n; i++) {
            int32_t x = osc_pulse(ph, inc, pw);
            ph += inc;
            lp += ((x - lp) * lk) >> 15;
            out[i] += voice_amp(lp << 1, m, i);
        }
        v->ph[0] = ph;
        v->ph[1] = (uint32_t)dr;
        v->ph[2] = tg;
        v->s[0] = lp;
        v->s[7] = rng;
        break;
    }
    default: {                                           /* GLOTT */
        uint32_t ph = v->ph[0], lx = v->ph[1] & 0xFFFFu, par = v->ph[1] >> 31, inc0, ic;
        int32_t tl = (int32_t)v->ph[2], a1 = v->s[0], b1 = v->s[1], a2 = v->s[4], b2 = v->s[5], rng = v->s[7];
        int32_t ro = p[P_E2], alt = (ro > 64 ? 64 : ro) * 30, cha = ro > 64 ? (ro - 64) * 67 : 0, sh = (ro > 64 ? 64 : ro) * 160;
        int32_t sz = (64 - p[P_E3]) * 112, mo = p[P_E1], air = (p[P_E7] * p[P_E7] * 5) >> 4, tk = 1500 + mo * 190, gamp = 32767;
        tsvf_t f1, f2;
        inc0 = an_inc(m->pitch16 + an_hook(v, p[P_E4] * 6), m->fine);
        ic = (uint32_t)v->s[6] ? (uint32_t)v->s[6] : inc0;
        tsvf_coef_k(&f1, 11878 + mo * 37 + sz + m->cutoff, 760);    /* F1 300 .. 750 Hz, F2 700 .. 1300 Hz (x SIZE) */
        tsvf_coef_k(&f2, 16333 + mo * 25 + sz + m->cutoff, 520);
        if (par && alt)
            gamp = 32767 - sh;
        for (i = 0; i < n; i++) {
            int32_t x, v1, v2, v3, y;
            ph += ic;
            if (ph < ic) {                               /* a new glottal cycle: its length and its strength */
                par ^= 1u;
                ic = inc0;
                if (alt)                                 /* period doubling: long, short, long .. (F0 / 2) */
                    ic += (uint32_t)((int32_t)(inc0 >> 12) * (par ? alt : -alt) >> 3);
                if (cha) {                               /* chaos: the logistic map x <- 3.9 x (1 - x), once a cycle */
                    lx = (uint32_t)(((lx * (65536u - lx)) >> 16) * 255u >> 6) & 0xFFFFu;
                    if (lx < 64u) lx = 0x9000u;
                    ic += (uint32_t)((int32_t)(inc0 >> 12) * (((((int32_t)lx - 32768) >> 6) * cha) >> 12));
                }
                gamp = par && alt ? 32767 - sh : 32767;
            }
            x = (osc_saw(ph, ic) * gamp) >> 15;
            tl += ((x - tl) * tk) >> 15;                 /* the source's tilt: MOUTH opens it */
            x = tl;
            if (air)
                x += (((int32_t)noise32(&rng) >> 17) * air) >> 8;
            x >>= 2;
            v3 = x - b1;
            v1 = (f1.a1 * a1 + f1.a2 * v3 + 4096) >> 13;
            v2 = b1 + ((f1.a2 * a1 + f1.a3 * v3 + 4096) >> 13);
            a1 = clamp(2 * v1 - a1, -150000, 150000);
            b1 = clamp(2 * v2 - b1, -150000, 150000);
            y = v1;
            v3 = x - b2;
            v1 = (f2.a1 * a2 + f2.a2 * v3 + 4096) >> 13;
            v2 = b2 + ((f2.a2 * a2 + f2.a3 * v3 + 4096) >> 13);
            a2 = clamp(2 * v1 - a2, -150000, 150000);
            b2 = clamp(2 * v2 - b2, -150000, 150000);
            y += v1 - (v1 >> 2);
            out[i] += voice_amp(soft_knee(y + (y >> 1), 26000), m, i);
        }
        v->ph[0] = ph;
        v->ph[1] = lx | par << 31;
        v->ph[2] = (uint32_t)tl;
        v->s[0] = a1;
        v->s[1] = b1;
        v->s[4] = a2;
        v->s[5] = b2;
        v->s[6] = (int32_t)ic;
        v->s[7] = rng;
        break;
    }
    }
}

/* {MODE, PRESS, CHAOS, SIZE | SWEEP, RATE, GROUP, TEMP} (as each mode names them: animal_desc) */
static const preset_t ANIMAL_PRESETS[] = {
    /* CMOS (as named there: TOUCH COUPLE CAP | HYST RATE GROUP FM): the owner's circuit, played an octave down */
    {"CMOS BIRD", {AN_CMOS, 90, 0, 37, 16, 74, 0, 20}, {0, 64, 127, 40}, 0, 0, FX(0, 0, 25, 40), PAT(7)},
    {"CMOS TRILL", {AN_CMOS, 70, 0, 50, 6, 94, 0, 6}, {0, 64, 127, 40}, 0, 0, FX(0, 0, 25, 40), PAT(7)},
    {"CMOS MONKEY", {AN_CMOS, 110, 20, 80, -40, 70, 5, 28}, {0, 64, 127, 50}, 0, 0, FX(0, 10, 20, 45), PAT(3)},
    {"CMOS BUG", {AN_CMOS, 60, 90, 20, 12, 104, 0, 16}, {0, 64, 127, 30}, 0, 0, FX(0, 0, 20, 30), PAT(7)},
    /* PULSE (CLICK Q SIZE | DETUNE RATE GROUP TEMP): Gryllus at ~4.8 kHz from C5, 26 syllables / s, 3 a chirp */
    {"CRICKET", {AN_PULSE, 127, 100, 26, 0, 119, 7, 22}, {0, 64, 127, 30}, 0, 0, FX(0, 0, 0, 35), PAT(5)},
    {"TREE CRICKT", {AN_PULSE, 127, 110, 32, 4, 102, 1, 22}, {0, 64, 127, 40}, 0, 0, FX(0, 0, 0, 45), PAT(5)},
    {"KATYDID", {AN_PULSE, 10, 20, 24, 50, 80, 7, 22}, {0, 64, 127, 30}, 0, 0, FX(0, 0, 0, 35), PAT(5)},
    {"CICADA", {AN_PULSE, 38, 85, 28, 14, 60, 0, 24}, {20, 64, 127, 60}, 0, 0, FX(0, 0, 0, 40), PAT(5)},
    {"TREEFROG", {AN_PULSE, 106, 49, 41, 6, 127, 15, 26}, {0, 64, 127, 40}, 0, 0, FX(0, 0, 0, 40), PAT(5)},
    {"TOAD", {AN_PULSE, 127, 80, 44, 8, 116, 1, 22}, {4, 64, 127, 50}, 0, 0, FX(0, 0, 0, 40), PAT(5)},
    {"BULLFROG", {AN_PULSE, 70, 30, 53, 10, 52, 7, 22}, {2, 64, 127, 50}, 0, 0, FX(0, 0, 0, 40), PAT(8)},
    /* WHSTL (PRESS CHAOS SIZE | SWEEP RATE GROUP TEMP) */
    {"CANARY", {AN_WHSTL, 100, 20, 38, -30, 112, 14, 22}, {0, 64, 127, 30}, 0, 0, FX(0, 0, 20, 40), PAT(3)},
    {"CARDINAL", {AN_WHSTL, 110, 6, 46, -40, 77, 10, 22}, {0, 64, 127, 30}, 0, 0, FX(0, 0, 20, 40), PAT(3)},
    {"DOVE", {AN_WHSTL, 70, 10, 66, 15, 60, 9, 22}, {6, 64, 127, 50}, 0, 0, FX(0, 0, 15, 50), PAT(5)},
    {"OWL", {AN_WHSTL, 80, 8, 68, -8, 68, 9, 22}, {4, 64, 127, 50}, 0, 0, FX(0, 0, 15, 55), PAT(8)},
    {"PEEPER", {AN_WHSTL, 100, 4, 34, 30, 80, 3, 22}, {0, 64, 127, 30}, 0, 0, FX(0, 0, 10, 40), PAT(5)},
    {"BAT", {AN_WHSTL, 120, 0, 40, -64, 127, 3, 22}, {0, 64, 127, 20}, 0, 0, FX(0, 0, 30, 30), PAT(7)},
    /* WING (BUZZ DRIFT SIZE | DOPPLR RATE GROUP TEMP) */
    {"MOSQUITO", {AN_WING, 30, 50, 64, 20, 40, 2, 22}, {10, 64, 127, 40}, 0, 0, FX(0, 0, 0, 30), PAT(5)},
    {"BEE", {AN_WING, 90, 30, 75, 10, 40, 2, 22}, {10, 64, 127, 40}, 0, 0, FX(0, 0, 0, 30), PAT(5)},
    {"FLY", {AN_WING, 70, 90, 78, 25, 50, 1, 22}, {10, 64, 127, 40}, 0, 0, FX(0, 0, 0, 30), PAT(5)},
    /* GLOTT (MOUTH ROUGH SIZE | SWEEP RATE GROUP AIR) */
    {"WOLF", {AN_GLOTT, 0, 8, 64, 40, 50, 0, 5}, {20, 64, 127, 60}, 0, 1, FX(0, 0, 15, 60), PAT(5)},
    {"DOG BARK", {AN_GLOTT, 100, 70, 64, -15, 85, 5, 6}, {0, 64, 127, 30}, 0, 0, FX(0, 0, 10, 30), PAT(3)},
    {"ROAR", {AN_GLOTT, 110, 100, 100, -20, 50, 3, 9}, {6, 64, 127, 50}, 0, 1, FX(10, 0, 10, 40), PAT(8)},
    {"PURR", {AN_GLOTT, 10, 60, 50, 0, 60, 1, 19}, {10, 64, 127, 40}, 0, 1, FX(0, 0, 0, 20), PAT(8)},
};

static const engine_t ENG_ANIMAL = {
    .name = "ANIMAL",
    .page_title = {"SRC", "CALL"},
    .edit = {
        {"MODE", F_ENUM, 0, AN_NMODE - 1, 0, N_ANIMAL_MODE, 0},
        {"PRESS", F_PCT, 0, 127, 100, 0, 0},
        {"CHAOS", F_PCT, 0, 127, 0, 0, 0},
        {"SIZE", F_INT, 0, 127, 64, 0, 0},
        {"SWEEP", F_BIPCT, -64, 63, 0, 0, 0},
        {"RATE", F_LFOHZ, 0, 127, 80, 0, 0},
        {"GROUP", F_ENUM, 0, 15, 0, N_AN_GROUP, 0},
        {"TEMP", F_INT, 0, 40, 22, 0, "C"},
    },
    .presets = ANIMAL_PRESETS,
    .npresets = NELEM(ANIMAL_PRESETS),
    .knob = {P_E1, P_E4, P_E5, P_E3},
    .keep = 0x33,                /* the resonators, the formants, the tone poles */
    .note_on = animal_note_on,
    .render = animal_render,
    .amp = animal_amp,
    .desc = animal_desc,
};
