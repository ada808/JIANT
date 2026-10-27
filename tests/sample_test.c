/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Juan Ignacio Antonucci (Juan Jiant) */
/* SAMPLE (tests/sample_test.c, JIANT 0.9.9-par13): src/eng_sample.c and src/sample_bank.c against simulated audio
 * (hostsim.c), no hardware. A bank built here (IMA ADPCM, as the web editor's SAMPLES tab builds it): a 440 Hz sine
 * at 8 kHz and a noise at 11025 Hz.
 * 1. The bank indexed: two slots, their names; a bad CRC or magic: none, SAMPLE silent, its slots "-".
 * 2. PITCH: C4 as recorded (440 Hz), C5 an octave up; no DC, under full scale.
 * 3. LOOP OFF ends at the sample's end, ON plays on; START / LENGTH cut it; KEYS SLOTS: C#4 the next slot.
 * 4. BITS and RATE change the sound; the transfer (BEGIN / WRITE / END) with its CRC, a wrong CRC refused. */
#define main hostsim_main
#include "hostsim.c"
#undef main
#define FELUCCA_FLASH 0
static uint32_t st_crc32_add(uint32_t c, const void *p, uint32_t n)   /* (storage.c's, not in the host build) */
{
    const uint8_t *b = p;
    while (n--) {
        uint32_t k;
        c ^= *b++;
        for (k = 0; k < 8u; k++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return c;
}
static uint32_t st_crc32(const void *p, uint32_t n) { return ~st_crc32_add(0xFFFFFFFFu, p, n); }
#include "../firmware/src/sample_bank.c"

static int bad;
static void check(const char *what, int ok)
{
    printf("sample: %-100s %s\n", what, ok ? "ok" : "FAIL");
    bad += !ok;
}

/* IMA ADPCM, the decoder's mirror: sample x onto (*pred, *idx), its nibble */
static uint32_t enc(int32_t x, int32_t *pred, int32_t *idx)
{
    int32_t st = SMP_STEP[*idx], d = x - *pred, nb = 0, p = *pred, i = *idx;
    if (d < 0) { nb = 8; d = -d; }
    if (d >= st) { nb |= 4; d -= st; }
    if (d >= st >> 1) { nb |= 2; d -= st >> 1; }
    if (d >= st >> 2) nb |= 1;
    smp_bank.b[0] = smp_bank.b[0];
    {   /* (the state as the decoder will have it) */
        int32_t dd = st >> 3;
        if (nb & 4) dd += st;
        if (nb & 2) dd += st >> 1;
        if (nb & 1) dd += st >> 2;
        p = clamp(p + ((nb & 8) ? -dd : dd), -32768, 32767);
        i = clamp(i + SMP_IDX[nb & 7], 0, 88);
    }
    *pred = p;
    *idx = i;
    return (uint32_t)nb;
}
static uint32_t put(uint32_t at, uint32_t n, uint32_t rate, const char *name, int kind)
{
    smp_ent_t *e = &smp_bank.h.e[smp_bank.h.count++];
    int32_t pred = 0, idx = 0;
    uint32_t k, z = 12345;
    e->off = (uint16_t)at;
    e->n = (uint16_t)n;
    e->rate = (uint16_t)rate;
    strncpy(e->name, name, sizeof e->name);
    for (k = 0; k < n; k++) {
        int32_t x;
        if (kind == 0)
            x = (int32_t)(20000.0 * sin(2 * M_PI * 440.0 * k / rate));
        else {
            z = z * 1103515245u + 12345u;
            x = (int32_t)((z >> 16) & 0x7FFF) - 16384;
        }
        smp_bank.b[SMP_HEAD + at + k / 2] |= (uint8_t)(enc(x, &pred, &idx) << ((k & 1) * 4));
    }
    return at + (n + 1) / 2;
}
static void build(void)
{
    uint32_t at;
    memset(smp_bank.b, 0, SMP_BANK);
    smp_bank.h.magic = SMP_MAGIC;
    smp_bank.h.ver = 1;
    at = put(0, 4000, 8000, "SINE", 0);                 /* 0.5 s */
    at = put(at, 2205, 11025, "NOISE", 1);              /* 0.2 s */
    smp_bank.h.len = at;
    smp_bank.h.crc = ~st_crc32_add(0xFFFFFFFFu, smp_bank.b + 16, SMP_HEAD - 16u + at);
}

static void setup(const int16_t *e)
{
    uint32_t i;
    memset(trk, 0, sizeof trk);
    host_tracks_init();
    host_preset(&trk[0], ENGI_ADD, 0);
    for (i = 0; i < 8u; i++)
        trk[0].p[P_E0 + i] = e[i];
    trk[0].p[P_ATK] = 0;
    trk[0].p[P_SUS] = 127;
    trk[0].p[P_VOICE] = V_POLY;
    trk[0].p[P_CHOR] = trk[0].p[P_DLY] = trk[0].p[P_REV] = 0;
}
static void play(uint32_t note, double skip, double *y, uint32_t n)
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
static double rms(const double *y, uint32_t n)
{
    double s = 0;
    uint32_t i;
    for (i = 0; i < n; i++)
        s += y[i] * y[i];
    return sqrt(s / n);
}
static double hz(const double *y, uint32_t n)        /* rising zero crossings a second */
{
    uint32_t i, c = 0, f = 0, l = 0;
    for (i = 1; i < n; i++)
        if (y[i - 1] < 0 && y[i] >= 0) {
            if (!c) f = i;
            l = i;
            c++;
        }
    return c > 1 ? (c - 1) * (double)FS / (l - f) : 0;
}

int main(void)
{
    static double y[FS];
    char what[200];
    double f4, f5, r0, r1, mean = 0, top = 0;
    uint32_t i;
    build();
    smp_index();
    check("the bank indexed: two slots, SINE and NOISE", smp_ix.count == 2 && !strcmp(smp_names[0], "SINE") &&
          !strcmp(smp_names[1], "NOISE") && !strcmp(smp_names[2], "-"));
    {
        int16_t e[8] = {0, 0, 127, 0, 0, 0, 0, 0};
        setup(e);
        play(60, 0.05, y, FS / 4);
        f4 = hz(y, FS / 4);
        for (i = 0; i < FS / 4; i++) {
            mean += y[i] / (FS / 4);
            top = fabs(y[i]) > top ? fabs(y[i]) : top;
        }
        setup(e);
        play(72, 0.02, y, FS / 8);
        f5 = hz(y, FS / 8);
        snprintf(what, sizeof what, "PITCH: C4 as recorded (%.1f Hz), C5 an octave up (%.1f Hz); no DC (%+.4f), under full scale (%.2f)",
                 f4, f5, mean, top);
        check(what, fabs(f4 - 440) < 5 && fabs(f5 - 880) < 10 && fabs(mean) < 0.01 && top < 1.0 && top > 0.05);
        setup(e);
        play(60, 0.8, y, FS / 10);
        r0 = rms(y, FS / 10);
        e[3] = 1;
        setup(e);
        play(60, 0.8, y, FS / 10);
        r1 = rms(y, FS / 10);
        snprintf(what, sizeof what, "LOOP OFF silent past the end (%.4f), ON plays on (%.4f)", r0, r1);
        check(what, r0 < 1e-4 && r1 > 0.02);
        e[3] = 0;
        e[2] = 32;                                       /* LENGTH a quarter: 0.125 s */
        setup(e);
        play(60, 0.2, y, FS / 10);
        r0 = rms(y, FS / 10);
        snprintf(what, sizeof what, "LENGTH a quarter: silent at 0.2 s (%.4f)", r0);
        check(what, r0 < 1e-4);
        e[2] = 127;
        e[7] = 1;                                        /* KEYS SLOTS: C#4 = NOISE */
        setup(e);
        play(61, 0.02, y, FS / 10);
        snprintf(what, sizeof what, "KEYS SLOTS: C#4 the next slot (the noise: %.0f crossings a second)", hz(y, FS / 10));
        check(what, hz(y, FS / 10) > 1500);
        e[7] = 0;
        setup(e);
        play(60, 0.05, y, FS / 10);
        r0 = rms(y, FS / 10);
        e[4] = 127;                                      /* BITS: 1 bit */
        e[5] = 64;                                       /* RATE: held 9 */
        setup(e);
        play(60, 0.05, y, FS / 10);
        r1 = rms(y, FS / 10);
        {
            double d = 0;
            static double z[FS];
            setup((int16_t[8]){0, 0, 127, 0, 0, 0, 0, 0});
            play(60, 0.05, z, FS / 10);
            for (i = 0; i < FS / 10; i++)
                d += fabs(z[i] - y[i]);
            snprintf(what, sizeof what, "BITS and RATE crush it (RMS %.3f -> %.3f, it changed by %.1f)", r0, r1, d);
            check(what, d > 50 && r1 > 0.01);
        }
    }
    {
        uint8_t keep[SMP_BANK];
        uint32_t crc, rc;
        memcpy(keep, smp_bank.b, SMP_BANK);
        crc = st_crc32(keep, SMP_BANK);
        rc = smp_begin();
        check("transfer: BEGIN silences SAMPLE (no slot)", !rc && smp_ix.count == 0);
        for (i = 0; i < SMP_BANK; i += 64)
            rc |= smp_write(i, keep + i, 64);
        rc |= smp_end(crc);
        check("  WRITEs and END with the CRC: the bank plays again (two slots)", !rc && smp_ix.count == 2 &&
              !memcmp(keep, smp_bank.b, SMP_BANK));
        smp_begin();
        for (i = 0; i < SMP_BANK; i += 64)
            smp_write(i, keep + i, 64);
        rc = smp_end(crc ^ 1u);
        check("  a wrong CRC: refused (rc 2), no slot", rc == 2 && smp_ix.count == 0);
        check("  WRITE without BEGIN: rc 5; out of the bank: rc 1", smp_write(0, keep, 4) == 5 &&
              (smp_begin(), smp_write(SMP_BANK - 2, keep, 4)) == 1);
        memcpy(smp_bank.b, keep, SMP_BANK);
        smp_bank.b[SMP_HEAD + 10] ^= 0x55;               /* a corrupted byte */
        smp_index();
        {
            int16_t e[8] = {0, 0, 127, 0, 0, 0, 0, 0};
            setup(e);
            play(60, 0.05, y, FS / 10);
            snprintf(what, sizeof what, "a corrupted bank (its CRC): no slot, SLOT \"-\", SAMPLE silent (%.5f)", rms(y, FS / 10));
            check(what, smp_ix.count == 0 && !strcmp(smp_names[0], "-") && rms(y, FS / 10) < 1e-6);
        }
    }
    printf(bad ? "sample: %d FAILED\n" : "sample: all passed\n", bad);
    return bad != 0;
}
