/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* STRING and ADD (tests/string_add_test.c, JIANT 0.9.9-snd5): the two engines of src/eng_string.c and src/eng_add.c
 * against simulated audio (hostsim.c), no hardware:
 * 1. STRING in tune: every EXCITE, C1 .. C6 (the loop at its full rate and decimated by 2 .. 32), the fundamental within
 *    a few cents; DECAY a T60 rising with the knob and about the same over the keyboard; BRIGHT the highs' share up.
 * 2. ADD: the partials where MODE puts them (HARM's harmonics, BELL's ratios), BRIGHT the tilt, EVEN the even ones out,
 *    DECAY / DAMP the highs dying first; nothing above ~20 kHz (no alias) at the top of the keyboard.
 * 3. every preset of both: sounds, no DC, ends after the release, the same samples twice from silence, no clipping
 *    at the knobs' corners (8 voices); a STRING part's COMB TYPE turned to BP (the lines are its strings).
 * 4. demos into DEMODIR: every preset playing a phrase. */
#define main hostsim_main
#include "hostsim.c"
#undef main

#define NFFT 8192
static int bad;
static void check(const char *what, int ok)
{
    printf("strings: %-96s %s\n", what, ok ? "ok" : "FAIL");
    bad += !ok;
}

static const engine_t *EG = &ENG_STRING;               /* the engine under test */
static uint32_t eng_of(const engine_t *e)
{
    uint32_t k;
    for (k = 0; k < NENGINES; k++)
        if (ENGINES[k] == e)
            return k;
    return 0;
}
static int32_t preset_of(const char *name)
{
    uint32_t k;
    for (k = 0; k < EG->npresets; k++)
        if (str_eq(EG->presets[k].name, name))
            return (int32_t)k;
    printf("strings: no preset %s\n", name);
    bad++;
    return 0;
}

/* track 0 on EG: preset pi (or -1: its first), then e[] if given; ATK 0, SUS full, no sends */
static void setup(int32_t pi, const int16_t *e)
{
    uint32_t i;
    memset(trk, 0, sizeof trk);
    host_tracks_init();
    host_preset(&trk[0], eng_of(EG), pi < 0 ? 0 : (uint32_t)pi);
    if (e)
        for (i = 0; i < 8u; i++)
            trk[0].p[P_E0 + i] = e[i];
    trk[0].p[P_ATK] = 0;
    trk[0].p[P_SUS] = 127;
    trk[0].p[P_VOICE] = V_POLY;
    trk[0].p[P_CHOR] = trk[0].p[P_DLY] = trk[0].p[P_REV] = 0;
}

/* the voices' sum (before the mix) of a held note, n samples after `skip` s */
static void voice_render(uint32_t note, double skip, double *y, uint32_t n)
{
    static int32_t b[CTL];
    uint32_t i, k, sk = (uint32_t)(skip * FS) / CTL;
    trk_note_on(&trk[0], note, 100);
    for (i = 0; i < sk; i++)
        track_render(&trk[0], b, CTL);
    for (i = 0; i < n; i += CTL) {
        track_render(&trk[0], b, CTL);
        for (k = 0; k < CTL && i + k < n; k++)
            y[i + k] = b[k] / 32768.0;
    }
}

/* ------------------------------------------------------------ analysis --- */
static void fft(double *re, double *im, uint32_t n)
{
    uint32_t i, j, len;
    for (i = 1, j = 0; i < n; i++) {
        uint32_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            double t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (len = 2; len <= n; len <<= 1) {
        double a = -2 * M_PI / len;
        for (i = 0; i < n; i += len)
            for (j = 0; j < len / 2; j++) {
                double wr = cos(a * j), wi = sin(a * j);
                double xr = re[i + j + len / 2] * wr - im[i + j + len / 2] * wi;
                double xi = re[i + j + len / 2] * wi + im[i + j + len / 2] * wr;
                re[i + j + len / 2] = re[i + j] - xr, im[i + j + len / 2] = im[i + j] - xi;
                re[i + j] += xr, im[i + j] += xi;
            }
    }
}
/* the averaged power spectrum (Hann) of y[0..n) in frames of NFFT, into ps[NFFT / 2] */
static void spectrum(const double *y, uint32_t n, double *ps)
{
    static double re[NFFT], im[NFFT];
    uint32_t f, i;
    memset(ps, 0, sizeof(double) * NFFT / 2);
    for (f = 0; f + NFFT <= n; f += NFFT / 2) {
        for (i = 0; i < NFFT; i++) {
            re[i] = y[f + i] * (0.5 - 0.5 * cos(2 * M_PI * i / NFFT));
            im[i] = 0;
        }
        fft(re, im, NFFT);
        for (i = 0; i < NFFT / 2; i++)
            ps[i] += re[i] * re[i] + im[i] * im[i];
    }
}
/* the strongest peak between lo and hi Hz, interpolated */
static double peak_hz(const double *ps, double lo, double hi)
{
    uint32_t i, a = (uint32_t)(lo / FS * NFFT), b = (uint32_t)(hi / FS * NFFT), best = a;
    double l, c, r, d;
    for (i = a; i < b; i++)
        if (ps[i] > ps[best])
            best = i;
    l = log(ps[best - 1] + 1e-30), c = log(ps[best] + 1e-30), r = log(ps[best + 1] + 1e-30);
    d = 0.5 * (l - r) / (l - 2 * c + r);
    return (best + d) * FS / NFFT;
}
/* the RMS envelope (5 ms frames) and its strongest repetition between lo and hi per second */
#define EFR (FS / 200)
static double env_rate(const double *y, uint32_t n, double lo, double hi)
{
    static double e[4096];
    uint32_t m = n / EFR, i, k, best = 0;
    double mean = 0, bv = -1;
    if (m > 4096u)
        m = 4096u;
    for (i = 0; i < m; i++) {
        double s = 0;
        for (k = 0; k < EFR; k++)
            s += y[i * EFR + k] * y[i * EFR + k];
        e[i] = sqrt(s / EFR);
        mean += e[i] / m;
    }
    for (i = 0; i < m; i++)
        e[i] -= mean;
    static double ac[4096];
    uint32_t k0 = (uint32_t)(200 / hi), k1 = (uint32_t)(200 / lo);
    for (k = k0; k <= k1 && k < m / 2; k++) {
        double a = 0, b0 = 0, b1 = 0;
        for (i = 0; i + k < m; i++)
            a += e[i] * e[i + k], b0 += e[i] * e[i], b1 += e[i + k] * e[i + k];
        ac[k] = a / (sqrt(b0 * b1) + 1e-30);
        if (ac[k] > bv)
            bv = ac[k], best = k;
    }
    for (k = k0 + 1; k < best; k++)                     /* the shortest lag that repeats about as well: the period,
                                                         * not a multiple of it */
        if (ac[k] >= 0.85 * bv && ac[k] >= ac[k - 1] && ac[k] >= ac[k + 1])
            return 200.0 / k;
    return best ? 200.0 / best : 0;
}
static double rms(const double *y, uint32_t n, double *mean)
{
    double s = 0, m = 0;
    uint32_t i;
    for (i = 0; i < n; i++)
        m += y[i];
    m /= n;
    for (i = 0; i < n; i++)
        s += (y[i] - m) * (y[i] - m);
    if (mean)
        *mean = m;
    return sqrt(s / n);
}
/* the frequency of the strongest component of y around i0 (a 2048-point frame, zero-padded): a pitch track */
static double local_hz(const double *y, uint32_t i0, double lo, double hi)
{
    static double re[NFFT], im[NFFT], ps[NFFT / 2];
    uint32_t i;
    for (i = 0; i < NFFT; i++) {
        re[i] = i < 1024u ? y[i0 + i] * (0.5 - 0.5 * cos(2 * M_PI * i / 1024)) : 0;
        im[i] = 0;
    }
    fft(re, im, NFFT);
    for (i = 0; i < NFFT / 2; i++)
        ps[i] = re[i] * re[i] + im[i] * im[i];
    return peak_hz(ps, lo, hi);
}

#define NS (FS * 6)
static double Y[NS], PS[NFFT / 2];

static double want_hz(uint32_t nt) { return 440.0 * pow(2, ((int)nt - 69) / 12.0); }

/* ---------------------------------------------------------------- STRING --- */
/* (p10) FDBK: a held note sustains and blooms with GAIN (none: a pluck dying out), bounded at every SPEAKR */
static void string_fdbk(void)
{
    static double y[FS / 4], ps[NFFT];
    static const int16_t G[3] = {0, 60, 127};
    double r[3][2], f[3], top = 0;
    uint32_t g, k;
    char what[200];
    EG = &ENG_STRING;
    for (g = 0; g < 3u; g++) {
        int16_t e[8] = {SX_FDBK, 40, 80, 24, 0, 70, 40, 20};
        e[4] = G[g];
        setup(-1, e);
        voice_render(52, 0.05, y, FS / 4);
        r[g][0] = rms(y, FS / 4, 0);
        voice_render(52, 0, y, 0);                       /* (nothing: the key still held) */
        setup(-1, e);
        voice_render(52, 3.0, y, FS / 4);
        r[g][1] = rms(y, FS / 4, 0);
        for (k = 0; k < FS / 4; k++)
            top = fabs(y[k]) > top ? fabs(y[k]) : top;
        spectrum(y, FS / 4 < NFFT ? FS / 4 : NFFT, ps);
        f[g] = peak_hz(ps, 60, 5000);
        printf("  FDBK GAIN %3d: RMS %.4f -> %.4f at 3 s, peak at %.0f Hz\n", G[g], r[g][0], r[g][1], f[g]);
    }
    snprintf(what, sizeof what, "STRING FDBK: GAIN 0 dies out (%.4f), GAIN 60 / 127 sustain at 3 s (%.3f %.3f), under full scale",
             r[0][1], r[1][1], r[2][1]);
    check(what, r[0][1] < r[0][0] * 0.1 && r[1][1] > 0.02 && r[2][1] > r[1][1] * 0.8 && top < 1.0);
    {   /* (par12) FDBK keeps the ADSR: ATTACK fades it in (a pluck's EXCITE does not: its own amplitude) */
        int16_t e[8] = {SX_FDBK, 40, 80, 24, 100, 70, 40, 20};
        double a0, a1;
        setup(-1, e);
        trk[0].p[P_ATK] = 100;
        voice_render(52, 0, y, FS / 50);
        a0 = rms(y, FS / 50, 0);
        setup(-1, e);
        trk[0].p[P_ATK] = 100;
        voice_render(52, 1.0, y, FS / 50);
        a1 = rms(y, FS / 50, 0);
        snprintf(what, sizeof what, "STRING FDBK: ATTACK fades it in (the first 20 ms %.4f, at 1 s %.4f)", a0, a1);
        check(what, a0 < a1 * 0.3);
    }
}

static void string_tune(void)
{
    static const int16_t E[SX_N][8] = {{SX_NOISE, 100, 90, 20, 0, 0, 40, 0}, {SX_PLUCK, 100, 90, 20, 0, 0, 40, 0},
                                       {SX_PICK, 100, 90, 20, 0, 0, 40, 0}, {SX_HAMMER, 100, 90, 20, 0, 0, 40, 0},
                                       {SX_MALLET, 100, 90, 30, 0, 0, 40, 0},
                                       {SX_FDBK, 100, 90, 20, 0, 0, 40, 0}};   /* (p10: GAIN 0, a plucked string) */
    double worst = 0, f, w;
    uint32_t m, nt, wn = 0, wm = 0;
    char what[200];
    EG = &ENG_STRING;
    for (m = 0; m < SX_N; m++)
        for (nt = 24; nt <= 84; nt += 6) {
            setup(-1, E[m]);
            voice_render(nt, 0.05, Y, NFFT * 2);
            spectrum(Y, NFFT * 2, PS);
            w = want_hz(nt);
            if (nt < 40u)                                /* (low keys: a bin is cents wide there, the 4th harmonic read) */
                f = peak_hz(PS, w * 4 * 0.97, w * 4 * 1.03) / 4;
            else
                f = peak_hz(PS, w * 0.85, w * 1.15);
            if (fabs(1200 * log2(f / w)) > fabs(worst)) { worst = 1200 * log2(f / w); wn = nt; wm = m; }
        }
    snprintf(what, sizeof what, "STRING in tune, every EXCITE, C1 .. C6 (decimated loops too): worst %+.1f cents (%s, key %u)",
             worst, N_STR_EXC[wm], wn);
    check(what, fabs(worst) < 12);
}
/* the time the RMS (20 ms frames) takes to fall 60 dB below its first frames' */
static double t60(const double *y, uint32_t n)
{
    double r0 = 0, r;
    uint32_t i, fr = FS / 50, k;
    for (k = 0; k < fr; k++) r0 += y[fr + k] * y[fr + k];
    r0 = sqrt(r0 / fr);
    for (i = 2; (i + 1) * fr <= n; i++) {
        for (r = 0, k = 0; k < fr; k++) r += y[i * fr + k] * y[i * fr + k];
        if (sqrt(r / fr) < r0 * 0.001) return (double)i / 50;
    }
    return (double)n / FS;
}
static void string_decay(void)
{
    static double T[3][3];
    static const int16_t DK[3] = {30, 70, 100};
    static const uint32_t NT[3] = {24, 48, 72};
    uint32_t d, k;
    int ok = 1;
    char what[200];
    EG = &ENG_STRING;
    for (d = 0; d < 3u; d++)
        for (k = 0; k < 3u; k++) {
            int16_t e[8] = {SX_PLUCK, DK[d], 127, 20, 0, 0, 40, 0};
            setup(-1, e);
            voice_render(NT[k], 0, Y, NS);
            T[d][k] = t60(Y, NS);
        }
    for (d = 0; d < 3u; d++) {
        ok &= T[d][0] < T[d][2] * 3.5 + 0.1 && T[d][2] < T[d][0] * 3.5 + 0.1;   /* (about the same over the keys) */
        if (d) ok &= T[d][1] > T[d - 1][1] * 1.5;
    }
    snprintf(what, sizeof what, "STRING DECAY 30 / 70 / 100: T60 %.2f / %.2f / %.2f s at C3 (C2 %.2f / %.2f / %.2f, C6 %.2f / %.2f / %.2f)",
             T[0][1], T[1][1], T[2][1], T[0][0], T[1][0], T[2][0], T[0][2], T[1][2], T[2][2]);
    check(what, ok);
}
static double hi_share(const double *ps, double f0)
{
    double lo = 0, hi = 0;
    uint32_t i;
    for (i = 1; i < NFFT / 2; i++) {
        double hz = (double)i * FS / NFFT;
        if (hz < f0 * 4) lo += ps[i]; else hi += ps[i];
    }
    return hi / (lo + hi + 1e-30);
}
static void string_bright(void)
{
    int16_t e0[8] = {SX_NOISE, 100, 10, 0, 0, 0, 40, 0}, e1[8] = {SX_NOISE, 100, 120, 0, 0, 0, 40, 0};
    double a, b;
    char what[200];
    EG = &ENG_STRING;
    setup(-1, e0); voice_render(48, 0.1, Y, NFFT * 2); spectrum(Y, NFFT * 2, PS); a = hi_share(PS, want_hz(48));
    setup(-1, e1); voice_render(48, 0.1, Y, NFFT * 2); spectrum(Y, NFFT * 2, PS); b = hi_share(PS, want_hz(48));
    snprintf(what, sizeof what, "STRING BRIGHT 10 -> 120: the share above the 4th harmonic %.3f -> %.3f", a, b);
    check(what, b > a * 3);
}
static void string_comb(void)
{
    EG = &ENG_STRING;
    setup(-1, 0);
    trk[0].p[P_FTYPE] = FT_COMB;
    voice_render(48, 0.05, Y, CTL * 8);
    check("STRING: the part's COMB TYPE plays as BP (the COMB's lines are its strings)", trk[0].p[P_FTYPE] == FT_BP);
}


/* -------------------------------------------------------- every preset --- */
static void presets(const engine_t *e)
{
    uint32_t pi, i;
    int ok = 1, same = 1, dc = 1, ends = 1;
    char what[200];
    EG = e;
    for (pi = 0; pi < e->npresets; pi++) {
        double r, mean, again[2048];
        static int32_t b[CTL];
        setup((int32_t)pi, 0);
        voice_render(48, 0.0, Y, FS);
        r = rms(Y, FS, &mean);
        ok &= r > 0.01;
        dc &= fabs(mean) < 0.01;
        for (i = 0; i < 2048u; i++) again[i] = Y[i];
        setup((int32_t)pi, 0);
        voice_render(48, 0.0, Y, 2048);
        for (i = 0; i < 2048u; i++) same &= Y[i] == again[i];
        trk_note_off(&trk[0], 48);
        for (i = 0; i < (uint32_t)(FS * 8 / CTL); i++) track_render(&trk[0], b, CTL);
        for (i = 0; i < NVOICE; i++) ends &= !trk[0].v[i].active;
        if (r <= 0.01 || fabs(mean) >= 0.01)
            printf("  %s %s: RMS %.4f mean %+.4f\n", e->name, e->presets[pi].name, r, mean);
    }
    snprintf(what, sizeof what, "%s: every preset (%u) sounds, no DC, the same twice from silence, ends after the release", e->name, e->npresets);
    check(what, ok && same && dc && ends);
}
static void corners(const engine_t *e)
{
    uint32_t c, k, i;
    int32_t pk = 0;
    char what[200];
    EG = e;
    for (c = 0; c < 64u; c++) {
        static int32_t b[CTL];
        int16_t ev[8];
        setup(-1, 0);
        for (k = 0; k < 8u; k++) {
            const param_desc_t *d = &e->edit[k];
            ev[k] = (int16_t)((c >> (k % 6u)) & 1u ? d->max : d->min);
        }
        ev[0] = (int16_t)(c % (uint32_t)(e->edit[0].max + 1));
        for (k = 0; k < 8u; k++) trk[0].p[P_E0 + k] = ev[k];
        for (k = 0; k < 8u; k++) trk_note_on(&trk[0], 24 + 9 * k, 127);
        for (i = 0; i < (uint32_t)(FS / CTL); i++) {
            uint32_t j;
            memset(b, 0, sizeof b);
            track_render(&trk[0], b, CTL);
            for (j = 0; j < CTL; j++) { int32_t a = b[j] < 0 ? -b[j] : b[j]; if (a > pk) pk = a; }
        }
    }
    snprintf(what, sizeof what, "%s: the knobs' corners, 8 voices at velocity 127: the part's peak %.2f of full scale", e->name, pk / 32768.0);
    check(what, pk < 6 * 32768);                        /* (8 voices in phase: under 0.75 a voice) */
}
static void demos(const char *dir)
{
    static const engine_t *const EE[1] = {&ENG_STRING};   /* (par11: ADD retired) */
    static const uint8_t PH[8] = {48, 55, 60, 63, 67, 63, 60, 55};
    uint32_t ei, pi, i, k;
    for (ei = 0; ei < 1u; ei++) {
        EG = EE[ei];
        for (pi = 0; pi < EG->npresets; pi++) {
            char path[400];
            FILE *f;
            static int32_t b[CTL];
            uint32_t nb = (uint32_t)(FS * 4.0 / CTL), step = (uint32_t)(FS * 0.25 / CTL);
            setup((int32_t)pi, 0);
            if (PR_MONO(&EG->presets[pi])) trk[0].p[P_VOICE] = V_LEGATO;
            snprintf(path, sizeof path, "%s/%s_%02u_%s.wav", dir, EG->name, pi, EG->presets[pi].name);
            for (k = 0; path[k]; k++) if (path[k] == ' ') path[k] = '_';
            if (!(f = fopen(path, "wb"))) return;
            wav_hdr(f, 0);
            for (i = 0; i < nb; i++) {
                uint32_t j, n8 = i / step;
                if (i % step == 0 && n8 < 12u) {
                    if (n8) trk_note_off(&trk[0], PH[(n8 - 1u) % 8u] - (PR_MONO(&EG->presets[pi]) ? 12 : 0));
                    trk_note_on(&trk[0], PH[n8 % 8u] - (PR_MONO(&EG->presets[pi]) ? 12 : 0), 90 + (n8 % 4u == 0) * 30);
                }
                if (i == step * 12u) trk_note_off(&trk[0], PH[11u % 8u] - (PR_MONO(&EG->presets[pi]) ? 12 : 0));
                memset(b, 0, sizeof b);
                track_render(&trk[0], b, CTL);
                for (j = 0; j < CTL; j++) {
                    int32_t s = b[j] / 2;
                    s = s > 32767 ? 32767 : s < -32767 ? -32767 : s;
                    fputc(s & 255, f); fputc((s >> 8) & 255, f); fputc(s & 255, f); fputc((s >> 8) & 255, f);
                }
            }
            fseek(f, 0, SEEK_SET);
            wav_hdr(f, nb * CTL);
            fclose(f);
        }
    }
}

int main(int argc, char **argv)
{

    string_tune();
    string_decay();
    string_bright();
    string_comb();
    string_fdbk();
    presets(&ENG_STRING);
    corners(&ENG_STRING);
    if (argc > 1)
        demos(argv[1]);
    printf(bad ? "strings: %d FAILED\n" : "strings: all passed\n", bad);
    return bad != 0;
}
