/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 * Portions Copyright (C) 2026 Juan Ignacio Antonucci (Juan Jiant) */
/* SAMPLE (JIANT 0.9.9-par13, engine 13: ADD's number before): a hyper lo-fi sampler. The owner's own sounds, sent from
 * the web editor (its SAMPLES box: WAV -> mono, 2..16 kHz, IMA ADPCM 4 bits), up to SMP_SLOTS of them in one bank of
 * SMP_BANK bytes: ~1.5 s at 8 kHz in all (~2 s at 6 kHz). The data flash is no XIP on the FM-1 (it is read with IRQs off: storage_hw.c),
 * so the bank lives in RAM (the pool, smp_bank) and in flash at 0xDA000 (sample_bank.c: power-on copies it in).
 *
 * A voice plays its slot from START to START + LENGTH (LOOP: round again from START), the decoder at the sample's own
 * rate times the note's (C4 = as recorded; KEYS SLOTS: every key the next slot from SLOT, at its own pitch: a kit),
 * no interpolation (each sample held: the aliasing is the sound). Seeking: the ADPCM state every SMP_CHK samples,
 * kept at load (smp_chk), so START and LOOP decode at most SMP_CHK - 1 samples ahead. Then BITS (16 .. 1), RATE (a
 * sample held 1 .. 16 output samples) and GRAIN (one knob: grains, the time stretched to a freeze; par13b, was DRIVE:
 * the track's DIST does that). Its amplitude: the ADSR, as any engine.
 *
 * Voice state: ph[0] the read position (Q16, in the bank's samples), ph[1] the slot | the bank's generation << 8
 * (a bank sent while it plays: the voice goes silent) | SMP_END, ph[2] RATE's countdown | the grain's samples << 8; s[0] the ADPCM predictor, s[1] its
 * step index, s[2] the next sample to decode, s[3] the end, s[4] the loop's start, s[5] the held output, s[6] the last
 * decoded, s[7] GRAIN's anchor (Q16). */
#define SMP_SLOTS 15u
#define SMP_BANK 6144u                         /* the bank: its header (256 bytes) and the data (the pool's reserve
                                                * went from 8 to 4 KiB for it: tools/build.py) */
#define SMP_HEAD 256u
#define SMP_DATA (SMP_BANK - SMP_HEAD)
#define SMP_CHK 256u                           /* the samples between two kept ADPCM states */
#define SMP_NCHK (SMP_DATA * 2u / SMP_CHK + SMP_SLOTS)
#define SMP_MAGIC 0x504D534Au                  /* "JSMP" */
typedef struct {
    uint16_t off, n;                           /* its first byte (in the data), its samples (4 bits each) */
    uint16_t rate;                             /* Hz, 1000 .. 16000 (par13b: from 1 kHz, the long ones) */
    char name[10];                             /* ASCII, 0-padded (the knob shows 5) */
} smp_ent_t;
typedef struct {
    uint32_t magic;
    uint16_t ver, count;                       /* 1, the slots used (from 0) */
    uint32_t len;                              /* the data's bytes used */
    uint32_t crc;                              /* CRC-32 (storage.c) of the header's bytes 16.. and the data used */
    smp_ent_t e[SMP_SLOTS];
} smp_head_t;
_Static_assert(sizeof(smp_head_t) == SMP_HEAD, "the sample bank's header");

static union {
    smp_head_t h;
    uint8_t b[SMP_BANK];
} smp_bank __attribute__((section(".pool")));
static struct {
    int16_t pred[SMP_NCHK];
    uint8_t idx[SMP_NCHK];
    uint8_t c0[SMP_SLOTS];                     /* a slot's first kept state (255: no sample there) */
    uint8_t count, gen;                        /* the slots one can play (0 while a bank is sent), the bank's generation */
} smp_ix __attribute__((section(".pool")));
static const char *smp_names[SMP_SLOTS] = {"-", "-", "-", "-", "-", "-", "-", "-", "-", "-", "-", "-", "-", "-", "-"};   /* (SLOT's
                                                         * names: the bank's, sample_bank.c smp_index) */

static const int16_t SMP_STEP[89] __attribute__((section(".rodata.smp"))) = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
    107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428,
    4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
    22385, 24623, 27086, 29794, 32767};
static const int8_t SMP_IDX[8] __attribute__((section(".rodata.smp"))) = {-1, -1, -1, -1, 2, 4, 6, 8};
static const char *const N_SMP_LOOP[] __attribute__((section(".rodata.smp"))) = {"OFF", "ON"};
static const char *const N_SMP_KEYS[] __attribute__((section(".rodata.smp"))) = {"PITCH", "SLOTS"};

/* sample k (of the bank's data) decoded onto the state (*pred, *idx) */
static int32_t smp_dec(uint32_t k, int32_t *pred, int32_t *idx)
{
    uint32_t nb = (smp_bank.b[SMP_HEAD + (k >> 1)] >> ((k & 1u) << 2)) & 15u;
    int32_t st = SMP_STEP[*idx], d = st >> 3;
    if (nb & 4u) d += st;
    if (nb & 2u) d += st >> 1;
    if (nb & 1u) d += st >> 2;
    *pred = clamp(*pred + ((nb & 8u) ? -d : d), -32768, 32767);
    *idx = clamp(*idx + SMP_IDX[nb & 7u], 0, 88);
    return *pred;
}

/* the voice at sample `at` of its slot s (absolute: the bank's), from the kept state before it */
static void smp_seek(voice_t *v, uint32_t s, uint32_t at)
{
    const smp_ent_t *e = &smp_bank.h.e[s];
    uint32_t b = (uint32_t)e->off * 2u, r = at - b, c = smp_ix.c0[s] + r / SMP_CHK, k = b + (r / SMP_CHK) * SMP_CHK;
    int32_t pred = smp_ix.pred[c], idx = smp_ix.idx[c];
    while (k < at)
        smp_dec(k++, &pred, &idx);
    v->s[0] = pred;
    v->s[1] = idx;
    v->s[2] = (int32_t)at;
    v->s[6] = pred;
}

#define SMP_END 0x80000000u                    /* ph[1]: the voice ended */
static void sample_note_on(track_t *t, voice_t *v)
{
    const int16_t *p = t->p;
    uint32_t s = (uint32_t)clamp(p[P_E0], 0, SMP_SLOTS - 1), n, st, len;
    const smp_ent_t *e;
    if (p[P_E7] == 1)                                    /* KEYS SLOTS: C4 the slot, each key up the next */
        s = (uint32_t)((int32_t)s + (int32_t)v->note - 60 + 4 * (int32_t)SMP_SLOTS) % SMP_SLOTS;
    v->ph[1] = s | (uint32_t)smp_ix.gen << 8 | SMP_END;
    if (s >= smp_ix.count || smp_ix.c0[s] == 255u)
        return;
    e = &smp_bank.h.e[s];
    n = e->n;
    st = (uint32_t)e->off * 2u + n * (uint32_t)clamp(p[P_E1], 0, 127) / 128u;
    len = 1u + (((uint32_t)e->off * 2u + n - st) * (uint32_t)clamp(p[P_E2] + 1, 1, 128)) / 128u;
    smp_seek(v, s, st);
    v->s[3] = (int32_t)(st + len);
    v->s[4] = (int32_t)st;
    v->s[5] = 0;
    v->s[7] = (int32_t)(st << 16);                       /* GRAIN's anchor */
    v->ph[1] &= ~SMP_END;
    v->ph[0] = st << 16;
    v->ph[2] = 0;
}

/* (par13b) GRAIN, one knob: past 0 the sample is played in grains of 50 .. 10 ms, each from the anchor (which walks on
 * slower and slower: at the top it stands, a freeze) plus a scatter that grows with it, under a triangle window */
static void sample_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    uint32_t s = v->ph[1] & 255u, i, pos = v->ph[0], step, hold = 1u + (uint32_t)clamp(p[P_E5], 0, 127) / 8u;
    uint32_t hc = v->ph[2] & 255u, gc = v->ph[2] >> 8, gl, an = (uint32_t)v->s[7];
    int32_t pred = v->s[0], idx = v->s[1], nx = v->s[2], end = v->s[3], ls = v->s[4], ho = v->s[5], cur = v->s[6];
    int32_t sh = clamp(p[P_E4], 0, 127) * 15 / 127, gr = clamp(p[P_E6], 0, 127), loop = p[P_E3] == 1;
    if ((v->ph[1] & SMP_END) || s >= smp_ix.count || ((v->ph[1] >> 8) & 255u) != smp_ix.gen)
        return;                                          /* (ended, or a new bank: silent) */
    if (p[P_E7] == 1)                                    /* KEYS SLOTS: as recorded */
        step = ((uint32_t)smp_bank.h.e[s].rate << 16) / 44100u;
    else                                                 /* PITCH: C4 as recorded */
        step = ((m->inc >> 14) * (uint32_t)smp_bank.h.e[s].rate) / 1046u;
    gl = 2205u - (uint32_t)gr * 13u;
    for (i = 0; i < n; i++) {
        int32_t y;
        if (gr) {                                        /* GRAIN */
            an += (step * (uint32_t)(128 - gr)) >> 7;
            if ((int32_t)(an >> 16) >= end) {
                if (!loop) {
                    v->ph[1] |= SMP_END;
                    break;
                }
                an = (uint32_t)ls << 16;
            }
            if (++gc >= gl || nx >= end) {               /* a new grain: the anchor and a scatter */
                uint32_t at = (an >> 16) + ((((an >> 4) ^ gc) * 2654435761u) >> 20) % (uint32_t)(gr * 24 + 1);
                gc = 0;
                if ((int32_t)at >= end)
                    at = (uint32_t)ls;
                smp_seek(v, s, at);
                pred = v->s[0];
                idx = v->s[1];
                nx = (int32_t)at;
                pos = at << 16;
            }
        }
        while (nx <= (int32_t)(pos >> 16)) {
            if (nx >= end) {
                if (gr)
                    break;                               /* (GRAIN: the next grain takes it) */
                if (!loop) {                             /* the end: silent from here */
                    v->ph[1] |= SMP_END;
                    break;
                }
                smp_seek(v, s, (uint32_t)ls);            /* LOOP: from START again */
                pred = v->s[0];
                idx = v->s[1];
                nx = ls;
                pos = (uint32_t)ls << 16;
            }
            cur = smp_dec((uint32_t)nx++, &pred, &idx);
        }
        if (v->ph[1] & SMP_END)
            break;
        pos += step;
        if (++hc >= hold) {                              /* RATE: a sample held for `hold` outputs */
            hc = 0;
            ho = (cur >> sh) << sh;                      /* BITS */
        }
        y = ho;
        if (gr)                                          /* the grain's triangle */
            y = y * (int32_t)(gc < gl / 2u ? gc : gl - gc) / (int32_t)(gl / 2u);
        out[i] += voice_amp(y, m, i);
    }
    v->ph[0] = pos;
    v->ph[2] = hc | gc << 8;
    v->s[0] = pred; v->s[1] = idx; v->s[2] = nx; v->s[5] = ho; v->s[6] = cur; v->s[7] = (int32_t)an;
}

/* {SLOT, START, LENGTH, LOOP, BITS, RATE, GRAIN, KEYS}, {A D S R}, fenv, mono */
static const preset_t SAMPLE_PRESETS[] = {
    {"SAMPLE", {0, 0, 127, 0, 0, 0, 0, 0}, {0, 64, 127, 20}, 0, TAG(FX), FX(0, 0, 0, 10), PAT(4)},
    {"KIT KEYS", {0, 0, 127, 0, 0, 0, 0, 1}, {0, 64, 127, 10}, 0, TAG(DRUM), FX(0, 0, 0, 5), PAT(1)},
    {"8 BIT", {0, 0, 127, 0, 60, 8, 0, 0}, {0, 64, 127, 20}, 0, TAG(FX), FX(0, 0, 10, 10), PAT(4)},
    {"CRUSH", {0, 0, 127, 0, 100, 40, 0, 0}, {0, 64, 127, 20}, 0, TAG(FX), FX(20, 0, 10, 10), PAT(4)},
    {"LOOP PAD", {0, 20, 60, 1, 20, 0, 70, 0}, {40, 64, 127, 70}, 0, TAG(PAD), FX(0, 30, 20, 50), PAT(5)},
    {"SAMP BASS", {0, 0, 127, 0, 30, 0, 0, 0}, {0, 64, 127, 15}, 0, 1 | TAG(BASS), FX(10, 0, 0, 5), PAT(2)},
};

static const engine_t ENG_SAMPLE = {
    .name = "SAMPLE",
    .page_title = {"SAMPLE", "LOFI"},
    .edit = {
        {"SLOT", F_ENUM, 0, SMP_SLOTS - 1, 0, smp_names, 0},
        {"START", F_PCT, 0, 127, 0, 0, 0},
        {"LENGTH", F_PCT, 0, 127, 127, 0, 0},
        {"LOOP", F_ENUM, 0, 1, 0, N_SMP_LOOP, 0},
        {"BITS", F_PCT, 0, 127, 0, 0, 0},
        {"RATE", F_PCT, 0, 127, 0, 0, 0},
        {"GRAIN", F_PCT, 0, 127, 0, 0, 0},
        {"KEYS", F_ENUM, 0, 1, 0, N_SMP_KEYS, 0},
    },
    .presets = SAMPLE_PRESETS,
    .npresets = NELEM(SAMPLE_PRESETS),
    .note_on = sample_note_on,
    .render = sample_render,
    .knob = {P_E0, P_E1, P_E2, P_E4},
};
