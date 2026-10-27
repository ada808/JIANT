/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Host test of the project formats (firmware/src/project.c, -DPROJ_HOST part): a format 4 ("FUN4", 8-byte
 * steps: before the drum grid), a format 3 ("FUN3", 57 parameters per track: before the modulation matrix), a
 * format 2 ("FUN2", 53: before the SLICER) and a format 1 ("FUN1") project, built byte for byte as the
 * firmware stored them, convert to format 6 ("FUN6"): every old value at its parameter, the parameters
 * added since at their defaults (SLICER OFF,
 * every matrix slot OFF), steps, globals, selection, the engine bytes (0..7 kept: the engines added since
 * were appended); damaged ones are refused. Track 4 of a project written before 1.0 (formats 2 and 3,
 * `parts` 0) was the GM drum part: it becomes a DRUM part 4 (PROJ_DEF_KEEP: project_load gives it DRUM's kit;
 * SAMPLE PERC until 1.0.2), its steps (its lanes' notes as hits) and parameters kept, the drum level / reverb send (G_DRLVL /
 * G_DRREV) as its LEVEL / REV; global id 24 (the drum part's MIDI channel then, REVERB TYPE now) loads as ROOM
 * from every format before FUN7. A FUN4 written while the drums were PHYS's MODEL DRUM (`phys` 1) loads such
 * a track as the DRUM engine with the same sound (its E values moved), other PHYS tracks as they were; one
 * of before 1.0 (`phys` 0) its DUST as MODAL bowed. The steps of a DRUM track of before FUN5 (DRUM, or
 * PHYS DRUM become DRUM) get their lanes' notes as hits, the other notes (a low tom, a crash) kept, the
 * step accent as the hits' accents; other tracks' steps stay as they were (no hits). A SAMPLE track of the retired
 * PERC set (SET 4) loads as DRUM with its default kit, in any format (proj_perc).
 * Run by tests/run_tests.sh (needs build/gen from one firmware build). */
#define main hostsim_main
#include "hostsim.c"
#undef main
#define PROJ_HOST 1                             /* (trk_old_engine: engines.c, through hostsim.c) */
#define PROJ_FUNB_WRITER 1                      /* (JIANT 0.6) proj_pack_vb: the older formats are made of a FUNB */
#include "../firmware/src/project.c"

static int check(const char *what, int ok)
{
    printf("%-60s %s\n", what, ok ? "ok" : "FAIL");
    return ok ? 0 : 1;
}

/* the value parameter k (old id) of track t had in the old project */
static int16_t oldv(uint32_t t, uint32_t k) { return (int16_t)(t * 100u + k * 3u + 1u); }

static const uint8_t OLD_ENG[PROJ_LT] = {7, 0, 5, 0};   /* WHEEL, ANALOG, VOICE; the drum track: 0 (no engine) */
static void fill_v2_track(proj_trk_v2_t *d, uint32_t t)
{
    uint32_t k;
    for (k = 0; k < PROJ_NP_V2; k++)
        d->p[k] = oldv(t, k);
    d->engine = OLD_ENG[t];
    d->preset = (uint8_t)(t + 5u);
    for (k = 0; k < NSTEP; k++) {
        step8_t *s = &d->step[k];
        s->note[0] = (uint8_t)(36u + (k + t) % 40u);
        s->n = (uint8_t)(k % 3u);
        s->time = (uint8_t)(k % 3u);
        s->flags = (uint8_t)(k & 3u);
        s->vel = (uint8_t)(64u + t);
    }
}

/* today's steps n are the old steps o (8 bytes each), with no hits */
static int steps_same(const step_t *n, const step8_t *o)
{
    uint32_t k;
    for (k = 0; k < NSTEP; k++)
        if (memcmp(n[k].note, o[k].note, 4) || n[k].n != o[k].n || n[k].time != o[k].time ||
            n[k].flags != o[k].flags || n[k].vel != o[k].vel || n[k].hit || n[k].acc)
            return 0;
    return 1;
}

/* .. as a DRUM track of before the grid gets them (proj_grid: its lanes' notes as hits) */
static int steps_grid_same(const step_t *n, const step8_t *o)
{
    uint32_t k;
    for (k = 0; k < NSTEP; k++) {
        step_t s;
        memset(&s, 0, sizeof s);
        memcpy(s.note, o[k].note, 4);
        s.n = o[k].n; s.time = o[k].time; s.flags = o[k].flags; s.vel = o[k].vel;
        step_to_grid(&s);
        if (memcmp(n[k].note, s.note, 4) || n[k].n != s.n || n[k].time != s.time || n[k].flags != s.flags ||
            n[k].vel != s.vel || n[k].hit != s.hit || n[k].acc != s.acc)
            return 0;
    }
    return 1;
}

/* today's project q as format 4 stored it (8-byte steps; the hits dropped) */
static void to_v4(project_v4_t *v, const project_t *q)
{
    uint32_t t, k;
    memset(v, 0, sizeof *v);
    v->magic = PROJ_MAGIC_V4;
    v->size = sizeof *v;
    memcpy(v->g, q->g, sizeof v->g);
    v->sel = q->sel;
    v->parts = q->parts;
    v->phys = q->phys;
    for (t = 0; t < PROJ_LT; t++) {
        for (k = 0; k < 61u; k++) v->t[t].p[k] = q->t[t].p[k];
        for (k = 0; k < 8u; k++) v->t[t].p[61u + k] = q->t[t].p[P_E0 + k];
        v->t[t].engine = q->t[t].engine;
        v->t[t].preset = q->t[t].preset;
        for (k = 0; k < NSTEP; k++) {
            memcpy(v->t[t].step[k].note, q->t[t].step[k].note, 4);
            v->t[t].step[k].n = q->t[t].step[k].n;
            v->t[t].step[k].time = q->t[t].step[k].time;
            v->t[t].step[k].flags = q->t[t].step[k].flags;
            v->t[t].step[k].vel = q->t[t].step[k].vel;
        }
    }
    v->sum = proj_hash(v, sizeof *v - 4u);
}

/* track t of the converted project has the old values where they belong; drum: track 4 was the drum part,
 * lvl / rev: its level and reverb send (the old globals) */
/* (JIANT 0.4) a project's tracks as an older format's import leaves them: LOFI's and ANALOG's E values converted */
static void v04_tracks(proj_trk_t *t)
{
    uint32_t k;
    for (k = 0; k < PROJ_LT; k++)
        sound_v04(t[k].engine, &t[k].p[P_E0]);
}
static int track_ok(const proj_trk_t *n, const proj_trk_v2_t *o, uint32_t t, int drum, int16_t lvl, int16_t rev)
{
    uint32_t k;
    int ok = (drum ? n->engine == ENGI_DRUM && n->preset == PROJ_DEF_KEEP && steps_grid_same(n->step, o->step)
                   : n->engine == o->engine && n->preset == o->preset && steps_same(n->step, o->step));
    for (k = 0; k <= P_DETUNE; k++)
        ok &= n->p[k] == (drum && k == P_LEVEL ? lvl : drum && k == P_REV ? rev : oldv(t, k));
    ok &= n->p[P_SLCR] == 0 && n->p[P_SLPAT] == TP[P_SLPAT].def && n->p[P_SLRATE] == TP[P_SLRATE].def &&
          n->p[P_SLDEPTH] == TP[P_SLDEPTH].def;
    for (k = P_M1SRC; k <= P_M4AMT; k++)
        ok &= n->p[k] == 0;                     /* every matrix slot OFF */
    {
        int16_t e[8];                           /* (JIANT 0.4: LOFI's and ANALOG's E values as today's, core.h sound_v04) */
        for (k = 0; k < 8u; k++) e[k] = oldv(t, 45u + k);
        sound_v04(n->engine, e);
        for (k = 0; k < 8u; k++)
            ok &= n->p[P_E0 + k] == e[k];
    }
    return ok;
}

/* a format 3 track (57 parameters: the SLICER at 45..48, P_E0 49) */
static int16_t oldv3(uint32_t t, uint32_t k) { return (int16_t)(t * 200u + k * 5u + 3u); }
static void fill_v3_track(proj_trk_v3_t *d, uint32_t t)
{
    uint32_t k;
    for (k = 0; k < PROJ_NP_V3; k++)
        d->p[k] = oldv3(t, k);
    d->engine = (uint8_t)(t == 3u ? 11u : t == 2u ? 7u : t * 3u);   /* (0 3 7 11: not a retired one, PHYS 9, TRIO 6) */
    d->preset = (uint8_t)(t + 1u);
    for (k = 0; k < NSTEP; k++) {
        step8_t *s = &d->step[k];
        s->note[0] = (uint8_t)(40u + (k * 7u + t) % 40u);
        s->n = (uint8_t)((k + t) % 3u);
        s->time = (uint8_t)(k % 3u);
        s->flags = (uint8_t)((k + 1u) & 3u);
        s->vel = (uint8_t)(70u + t);
    }
}
static int track_v3_ok(const proj_trk_t *n, const proj_trk_v3_t *o, uint32_t t)
{
    uint32_t k;
    int ok = n->engine == o->engine && n->preset == o->preset && steps_same(n->step, o->step);
    for (k = 0; k <= P_SLDEPTH; k++)            /* up to the SLICER: the same ids (0.6.9: its slots AUTO PAN's, off) */
        ok &= n->p[k] == (k >= P_SLCR ? TP[k].def : oldv3(t, k));
    for (k = P_M1SRC; k <= P_M4AMT; k++)
        ok &= n->p[k] == TP[k].def && TP[k].def == 0;
    {
        int16_t e[8];                           /* (JIANT 0.4: as today's) */
        for (k = 0; k < 8u; k++) e[k] = oldv3(t, 49u + k);
        sound_v04(n->engine, e);
        for (k = 0; k < 8u; k++)
            ok &= n->p[P_E0 + k] == e[k];
    }
    return ok;
}

/* (JIANT, FUNB) an older record of the project packed in st: np parameters a track (the first np - 8, then E0..E7;
 * the motion's engine ids moved down with them), the layout of that size (FUNA: the punch-in block and the kit; FUN8 /
 * FUN9: the FM6 patches; the name at the end), its magic and hash. old_motion: where its motion went */
static uint32_t old_motion;
static void old_image(uint8_t *o, const project_store_t *st, uint32_t np, uint32_t size, uint32_t magic)
{
    const uint8_t *b = st->raw;
    uint32_t pos = 68u, src = 68u, t, j, sum, k;
    uint32_t name = size - 4u - PROJ_NAME_LEN, fm6 = name - PROJ_LT * FM6_PACKED;
    memset(o, 0, size);
    memcpy(o, b, 68);
    memcpy(o, &magic, 4); memcpy(o + 4, &size, 4);
    o[66] = (uint8_t)np;
    for (t = 0; t < PROJ_LT; t++) {
        for (j = 0; j < np; j++) o[pos++] = b[src + (j < np - 8u ? j : P_E0 + j - (np - 8u))];
        src += P_COUNT;
        memcpy(o + pos, b + src, 2u + NSTEP * 9u);
        pos += 2u + NSTEP * 9u; src += 2u + NSTEP * 9u;
    }
    memcpy(o + pos, b + src, sizeof(chain_config_t) + sizeof(motion_store_t));
    old_motion = pos + sizeof(chain_config_t) + 4u;
    for (k = 0; k < MOTION_MAX; k++) {
        uint8_t *p = o + old_motion + k * 4u + 1u;
        if ((*p & 0x7Fu) >= P_E0)
            *p = (uint8_t)((*p & MOTION_LOCK) | ((*p & 0x7Fu) - (P_COUNT - np)));
    }
    if (size == PROJ_STORE_VA) {
        memcpy(o + fm6 - 96u - 48u, b + PROJ_PFX_OFF, 37u);   /* (the lane, its tracks, the macros: no new globals) */
        memcpy(o + fm6 - 96u, b + PROJ_DX_OFF, 96u);
    }
    if (size >= PROJ_STORE_V8)
        memcpy(o + fm6, b + PROJ_FM6_OFF, PROJ_LT * FM6_PACKED);
    memcpy(o + name, b + PROJ_NAME_OFF, PROJ_NAME_LEN);
    sum = proj_hash(o, size - 4u);
    memcpy(o + size - 4u, &sum, 4);
}

int main(void)
{
    static project_v4_t v4;
    static project_v3_t v3;
    static project_v2_t v2;
    static project_v1_t v1;
    static project_t q, q2;
    static proj_trk_t qx[NTRK];                         /* (JIANT 0.4: what an older format's import leaves) */
    static union {
        project_t v5;
        project_v4_t v4;
        project_v3_t v3;
        project_v2_t v2;
        project_v1_t v1;
    } buf;
    uint32_t i, t;
    int bad = 0, ok;

    bad += check("layout: SLICER after DETUNE, then the matrix just before P_E0",
                 P_SLCR == P_DETUNE + 1 && P_SLDEPTH + 1 == P_M1SRC && P_M4AMT + 1 == P_FM1_ATK && P_FM4_LEVEL + 1 == P_SOFS && P_POFS + 1 == P_LN0 &&
                 P_LN7 + 1 == P_DTYPE && P_MS15 + 1 == P_FTYPE && P_FRES + 1 == P_E0 && P_E0 == 115 && P_COUNT == PROJ_NP_V3 + 66u &&
                 PROJ_NP_V3 == PROJ_NP_V2 + 4u);
    bad += check("FUNB fits one flash object (its whole payload), the retained cache in NOINIT",
                 sizeof(project_store_t) == 4096u - 256u && 0xC8u + 4u * sizeof(project_store_t) <= 0x3D50u);

    /* format 2, as written before the SLICER */
    memset(&v2, 0, sizeof v2);
    v2.magic = PROJ_MAGIC_V2;
    v2.size = sizeof v2;
    for (i = 0; i < PROJ_NG_V2; i++)
        v2.g[i] = (int16_t)(500 + i);
    v2.sel = 2;
    for (t = 0; t < PROJ_LT; t++)
        fill_v2_track(&v2.t[t], t);
    v2.sum = proj_hash(&v2, sizeof v2 - 4u);
    bad += check("FUN2 image is 2552 bytes (as stored)", sizeof v2 == 2552u);
    memcpy(&buf, &v2, sizeof v2);
    ok = proj_import(&q, &buf, (int)sizeof v2);
    bad += check("FUN2 -> FUN6: converted, valid format 5 slot", ok && proj_ok(&q) && q.magic == PROJ_MAGIC);
    ok = q.sel == 2;
    for (i = 0; i < G_NSTORE; i++)
        ok &= q.g[i] == (i == G_RTYPE || i == G_DRLVL || i == G_DRREV ? 0 : (int16_t)(500 + i));
    bad += check("FUN2 -> FUN6: globals (id 24, the old drum channel: ROOM; 25 26, the drum part's: off) and selected track", ok);
    ok = 1;
    for (t = 0; t < PROJ_LT; t++)
        ok &= track_ok(&q.t[t], &v2.t[t], t, t == 3u, 127, 127);   /* (G_DRLVL / G_DRREV 525 / 526: 127) */
    bad += check("FUN2 -> FUN6: every parameter mapped, SLICER and matrix OFF (4 tracks)", ok);

    bad += check("FUN2 -> FUN6: engine bytes kept (WHEEL 7, ANALOG 0, VOICE 5)",
                 q.t[0].engine == 7 && q.t[1].engine == 0 && q.t[2].engine == 5 &&
                 str_eq(ENGINES[7]->name, "WHEEL") && str_eq(ENGINES[5]->name, "VOICE") && NENGINES > 8);
    bad += check("FUN2 -> FUN6: the drum track -> part 4, DRUM (its kit on load), steps kept (lanes as hits)",
                 q.parts == NPART && q.t[3].engine == ENGI_DRUM && str_eq(ENGINES[ENGI_DRUM]->name, "DRUM") &&
                 TRK_OLD[3][0] == ENGI_DRUM && q.t[3].preset == PROJ_DEF_KEEP &&
                 steps_grid_same(q.t[3].step, v2.t[3].step));

    /* format 3 (1.0, four parts), as written before the modulation matrix */
    memset(&v3, 0, sizeof v3);
    v3.magic = PROJ_MAGIC_V3;
    v3.size = sizeof v3;
    for (i = 0; i < G_NSTORE; i++)
        v3.g[i] = (int16_t)(600 + i);
    v3.sel = 3;
    v3.parts = NPART;
    for (t = 0; t < PROJ_LT; t++)
        fill_v3_track(&v3.t[t], t);
    v3.sum = proj_hash(&v3, sizeof v3 - 4u);
    bad += check("FUN3 image is 2584 bytes (as stored)", sizeof v3 == 2584u);
    memcpy(&buf, &v3, sizeof v3);
    ok = proj_import(&q, &buf, (int)sizeof v3) && proj_ok(&q) && q.magic == PROJ_MAGIC && q.sel == 3 &&
         q.parts == NPART;
    for (i = 0; i < G_NSTORE; i++)
        ok &= q.g[i] == (i == G_RTYPE ? 0 : (int16_t)(600 + i));
    for (t = 0; t < PROJ_LT; t++)
        ok &= track_v3_ok(&q.t[t], &v3.t[t], t);
    bad += check("FUN3 -> FUN6: every parameter kept, the matrix OFF, steps, globals (24: ROOM)", ok);

    /* a FUN3 written before 1.0 (parts 0: track 4 the drum part, engine byte 0) */
    v3.parts = 0;
    v3.t[3] = v3.t[0];
    v3.t[3].engine = 0;
    v3.t[3].preset = 0;
    v3.g[G_DRLVL] = 90;
    v3.g[G_DRREV] = 20;
    v3.sum = proj_hash(&v3, sizeof v3 - 4u);
    memcpy(&buf, &v3, sizeof v3);
    ok = proj_import(&q2, &buf, (int)sizeof v3) && proj_ok(&q2) && q2.parts == NPART && q2.t[3].engine == ENGI_DRUM &&
         q2.t[3].preset == PROJ_DEF_KEEP && q2.t[3].p[P_LEVEL] == 90 && q2.t[3].p[P_REV] == 20 &&
         q2.t[3].p[P_PAN] == oldv3(0, P_PAN) && q2.t[3].p[P_SLEN] == oldv3(0, P_SLEN) &&
         q2.t[3].p[P_APDEP] == 0 && q2.t[3].p[P_M1SRC] == 0 &&   /* (0.6.9: the old SLICER's slots: AUTO PAN off) */
         steps_grid_same(q2.t[3].step, v3.t[0].step) && !memcmp(&q2.t[0], &q.t[0], sizeof q.t[0]);
    bad += check("FUN3 before 1.0: the drum track -> part 4 (LEVEL / REV from G_DRLVL / G_DRREV)", ok);

    /* a FUN6 round trip: stored as is (a matrix slot set; another engine: WHEEL, 7 (GRAIN, 8, is retired in JIANT); a
     * DRUM track with notes on its lanes stays so: only an older format is converted) */
    q.t[1].engine = 7;
    q.t[2].p[P_M2SRC] = 6;
    q.t[2].p[P_M2DST] = 11;
    q.t[2].p[P_M2AMT] = -17;
    q.t[0].engine = ENGI_DRUM;
    q.t[0].step[0] = (step_t){{36, 42, 0, 0}, 2, ST_NOTE, 0, 96, 1u << DV_SNARE, 1u << DV_SNARE};
    q.sum = proj_sum(&q);
    memcpy(&buf, &q, sizeof q);
    bad += check("FUN6 -> FUN6: as stored (a matrix slot, engine 7, a DRUM step with notes and hits)",
                 proj_import(&q2, &buf, (int)sizeof q) && !memcmp(&q, &q2, sizeof q) && q2.t[1].engine == 7 &&
                 q2.t[2].p[P_M2AMT] == -17 && q2.t[0].step[0].n == 2u && q2.t[0].step[0].hit == 1u << DV_SNARE);
    q.t[0].engine = 3;
    q.t[0].step[0] = q.t[0].step[1];
    q.sum = proj_sum(&q);

    {
        project_v5_t old;
        memset(&old, 0, sizeof old);
        memcpy(old.g, q.g, sizeof old.g); old.sel = q.sel; old.parts = q.parts; old.phys = q.phys;
        for (uint32_t k = 0; k < PROJ_LT; k++) {
            for (uint32_t j = 0; j < 61u; j++) old.t[k].p[j] = q.t[k].p[j];
            for (uint32_t j = 0; j < 8u; j++) old.t[k].p[61u + j] = q.t[k].p[P_E0 + j];
            old.t[k].engine = q.t[k].engine; old.t[k].preset = q.t[k].preset;
            for (uint32_t j = 0; j < NSTEP; j++) memcpy(&old.t[k].step[j], &q.t[k].step[j], sizeof(step10_t));
        }
        old.magic = PROJ_MAGIC_V5;
        old.size = sizeof old;
        old.sum = proj_hash(&old, sizeof old - 4u);
        bad += check("FUN5 -> FUN6: sounds, grid and reserved lane bytes kept, no chain",
            proj_import(&q2, &old, sizeof old) && (memcpy(qx, q.t, sizeof q.t), v04_tracks(qx), !memcmp(q2.t, qx, PROJ_LT * sizeof q.t[0])) &&
            !q2.chain.count && chain_valid(&q2.chain));
        old.sum ^= 1u;
        bad += check("FUN5: damaged checksum refused", !proj_import(&q2, &old, sizeof old));
        q.chain.count = 2;
        q.chain.row[0] = (chain_row_t){0, 4};
        q.chain.row[1] = (chain_row_t){3, 16};
        q.sum = proj_sum(&q);
        bad += check("FUN6: chain round trip", proj_import(&q2, &q, sizeof q) &&
            !memcmp(&q.chain, &q2.chain, sizeof q.chain));
        q.chain.row[0].bars = 0;
        q.sum = proj_sum(&q);
        bad += check("FUN6: invalid row refused even with a correct checksum", !proj_import(&q2, &q, sizeof q));
        chain_defaults(&q.chain);
        q.sum = proj_sum(&q);
    }

    /* format 4 (1.0 development builds): 8-byte steps; a DRUM track's lane notes become its grid */
    to_v4(&v4, &q);
    bad += check("FUN4 image is 2680 bytes (as stored)", sizeof v4 == 2680u);
    memcpy(&buf, &v4, sizeof v4);
    ok = proj_import(&q2, &buf, (int)sizeof v4) && proj_ok(&q2) && q2.magic == PROJ_MAGIC && q2.sel == q.sel &&
         !memcmp(q2.g, q.g, sizeof q.g);
    memcpy(qx, q.t, sizeof q.t);
    v04_tracks(qx);
    for (t = 0; t < PROJ_LT; t++)
        ok &= !memcmp(q2.t[t].p, qx[t].p, sizeof q.t[t].p) && q2.t[t].engine == q.t[t].engine &&
              steps_same(q2.t[t].step, v4.t[t].step);
    bad += check("FUN4 -> FUN6: parameters, engines, globals; steps as they were, no hits", ok);
    {   /* track 2 a DRUM track: BEAT-like steps, a chord, a low tom 41, a crash 49, a step accent, a TIE */
        static const step8_t D[6] = {
            {{36, 0, 0, 0}, 1, ST_NOTE, SF_ACCENT, 96}, {{42, 38, 0, 0}, 2, ST_NOTE, 0, 100},
            {{41, 36, 0, 0}, 2, ST_NOTE, SF_ACCENT, 96}, {{49, 0, 0, 0}, 1, ST_NOTE, 0, 96},
            {{0, 0, 0, 0}, 0, ST_TIE, 0, 0}, {{38, 46, 0, 0}, 2, ST_REST, 0, 96},
        };
        const step_t *s = q2.t[1].step;
        v4.t[1].engine = ENGI_DRUM;
        for (i = 0; i < 6u; i++)
            v4.t[1].step[i] = D[i];
        v4.sum = proj_hash(&v4, sizeof v4 - 4u);
        memcpy(&buf, &v4, sizeof v4);
        ok = proj_import(&q2, &buf, (int)sizeof v4) && q2.t[1].engine == ENGI_DRUM;
        ok &= s[0].n == 0 && s[0].hit == 1u << DV_KICK && s[0].acc == 1u << DV_KICK && !(s[0].flags & SF_ACCENT) &&
              s[0].vel == 96 && s[0].time == ST_NOTE;
        ok &= s[1].n == 0 && s[1].hit == ((1u << DV_HATC) | (1u << DV_SNARE)) && !s[1].acc && s[1].vel == 100;
        ok &= s[2].n == 1 && s[2].note[0] == 41 && s[2].hit == 1u << DV_KICK && (s[2].flags & SF_ACCENT) &&
              step_lanes(&s[2]) == ((1u << DV_KICK) | (1u << DV_TOM)) && step_accents(&s[2]) == step_lanes(&s[2]);
        ok &= s[3].n == 1 && s[3].note[0] == 49 && !s[3].hit && step_lanes(&s[3]) == 1u << DV_BELL;
        ok &= s[4].time == ST_TIE && !s[4].hit && s[5].time == ST_REST && s[5].n == 2 && !s[5].hit;
        ok &= steps_same(q2.t[0].step, v4.t[0].step);   /* (not DRUM: as it was) */
        bad += check("FUN4 -> FUN6: a DRUM track's lane notes -> hits (accents too), a low tom / crash kept", ok);
    }
    v3.t[0].p[7]++;
    memcpy(&buf, &v3, sizeof v3);
    bad += check("FUN3 with a bad checksum: refused", !proj_import(&q2, &buf, (int)sizeof v3));
    memcpy(&buf, &q, sizeof q);
    buf.v5.magic = PROJ_MAGIC_V3;
    bad += check("FUN6 size with a FUN3 magic: refused", !proj_import(&q2, &buf, (int)sizeof q));

    /* damaged / wrong size */
    v2.t[1].p[3]++;
    memcpy(&buf, &v2, sizeof v2);
    bad += check("FUN2 with a bad checksum: refused", !proj_import(&q2, &buf, (int)sizeof v2));
    v2.t[1].p[3]--;
    memcpy(&buf, &v2, sizeof v2);
    bad += check("FUN2 with a wrong length: refused", !proj_import(&q2, &buf, (int)sizeof v2 - 2));
    memcpy(&buf, &q, sizeof q);
    buf.v5.magic = PROJ_MAGIC_V2;
    bad += check("FUN6 size with a FUN2 magic: refused", !proj_import(&q2, &buf, (int)sizeof q));

    /* format 1: one instrument -> track 1, the others their defaults */
    memset(&v1, 0, sizeof v1);
    v1.magic = PROJ_MAGIC_V1;
    v1.size = sizeof v1;
    for (i = 0; i < PROJ_NG_V2; i++)
        v1.g[i] = (int16_t)(700 + i);
    fill_v2_track(&v1.t, 0);
    v1.sum = proj_hash(&v1, sizeof v1 - 4u);
    memcpy(&buf, &v1, sizeof v1);
    ok = proj_import(&q, &buf, (int)sizeof v1) && proj_ok(&q) && track_ok(&q.t[0], &v1.t, 0, 0, 0, 0) && q.g[5] == 705 &&
         q.parts == NPART;
    for (t = 1; t < PROJ_LT; t++)
        ok &= q.t[t].preset == PROJ_DEF_SOUND && q.t[t].engine == trk_old_engine(t) && q.t[t].p[P_SLCR] == 0 && q.t[t].p[P_LEVEL] == TP[P_LEVEL].def &&
              q.t[t].p[P_E0] == ENGINES[trk_old_engine(t)]->edit[0].def && q.t[t].step[0].time == ST_REST;
    bad += check("FUN1 -> FUN6: track 1 mapped, tracks 2..4 0.9.8's power-on sounds (TRK_OLD, whatever today's)", ok);

    /* PHYS MODEL DRUM (a FUN4 with phys 1) -> the DRUM engine; the old DRUM KIT preset becomes DRUM's */
    {
        static const int16_t OLD_KIT[8] = {4, 64, 70, 64, 64, 100, 0, 0};   /* PHYS DRUM KIT before 1.0 */
        static const int16_t E0[8] = {4, 70, 80, 60, 50, 110, 100, 70}, E1[8] = {2, 10, 20, 30, 40, 50, 60, 70};
        const preset_t *dk = &ENGINES[ENGI_DRUM]->presets[0];
        q.t[0].engine = ENGI_PHYS;                      /* MODEL DRUM, PERC 70 (CYM), KICK 100 (ROUND) */
        q.t[0].preset = 9;
        q.t[1].engine = ENGI_PHYS;                      /* MEMB (2: DUST only before phys 1) */
        q.t[2].engine = ENGI_PHYS;
        q.t[3].engine = ENGI_PHYS;
        for (i = 0; i < 8u; i++) {
            q.t[0].p[P_E0 + i] = E0[i];
            q.t[1].p[P_E0 + i] = E1[i];
            q.t[2].p[P_E0 + i] = OLD_KIT[i];
            q.t[3].p[P_E0 + i] = (int16_t)(i ? 30 : 3);   /* (3: SYMP) */
        }
        q.phys = 1;
        for (i = 1; i < NSTEP; i++)                     /* (the other steps rests) */
            q.t[0].step[i] = (step_t){{50, 0, 0, 0}, 1, ST_REST, 0, 0, 0, 0};
        q.t[0].step[0] = (step_t){{36, 42, 0, 0}, 2, ST_NOTE, 0, 96, 0, 0};
        q.sum = proj_sum(&q);
        to_v4(&v4, &q);
        memcpy(&buf, &v4, sizeof v4);
        ok = proj_import(&q2, &buf, (int)sizeof v4) && proj_ok(&q2) && q2.phys == PROJ_PHYS &&
             q2.t[0].engine == ENGI_DRUM && q2.t[0].preset == 0 && str_eq(ENGINES[ENGI_DRUM]->name, "DRUM") &&
             q2.t[0].p[P_E0] == 64 && q2.t[0].p[P_E4] == 64 && q2.t[0].p[P_E6] == 0 && q2.t[0].p[P_E7] == 0;
        for (i = 1; i < 6u; i++)
            ok &= i == 4u || q2.t[0].p[P_E0 + i] == (i == 5u ? 0 : E0[i]);   /* TUNE TONE DECY in place (MRPH NOIS 64;
                                                         * E5, ACC then, FM now: 0) */
        {   /* PHYS (retired in TONIC): ANALOG with its first preset's EDIT values, the rest of the track kept */
            const preset_t *an = &ENGINES[ENGI_PHYS_TO]->presets[0];
            uint32_t k2;
            for (k2 = 1; k2 < 4u; k2 += 2u) {
                ok &= q2.t[k2].engine == ENGI_PHYS_TO && q2.t[k2].p[P_LEVEL] == q.t[k2].p[P_LEVEL];
                for (i = 0; i < 8u; i++)
                    ok &= q2.t[k2].p[P_E0 + i] == an->e[i];
            }
        }
        ok &= q2.t[2].engine == ENGI_DRUM;              /* the old DRUM KIT: DRUM's DRUM KIT */
        for (i = 0; i < 8u; i++)
            ok &= q2.t[2].p[P_E0 + i] == dk->e[i];
        ok &= !memcmp(&q2.t[0].step[1], &q.t[0].step[1], sizeof q.t[0].step - sizeof(step_t)) &&
              q2.t[0].p[P_LEVEL] == q.t[0].p[P_LEVEL];
        ok &= q2.t[0].step[0].n == 0 && q2.t[0].step[0].hit == ((1u << DV_KICK) | (1u << DV_HATC));   /* the grid */
        ok &= !memcmp(q2.t[1].step, q.t[1].step, sizeof q.t[1].step);   /* (the steps: as they were) */
        bad += check("FUN4 phys 1: PHYS MODEL DRUM -> DRUM engine (its lane notes as hits), other PHYS -> ANALOG", ok);
        memcpy(&buf, &q2, sizeof q2);
        bad += check("FUN6 after it: imported again, as it is", proj_import(&q, &buf, (int)sizeof q2) &&
                                                                !memcmp(&q, &q2, sizeof q));
        q.phys = 0;                                     /* before 1.0: MODEL 2 was DUST */
        q.t[1].engine = ENGI_PHYS;
        q.sum = proj_sum(&q);
        to_v4(&v4, &q);
        memcpy(&buf, &v4, sizeof v4);
        ok = proj_import(&q2, &buf, (int)sizeof v4) && q2.phys == PROJ_PHYS && q2.t[1].engine == ENGI_PHYS_TO &&
             q2.t[1].p[P_E0] == ENGINES[ENGI_PHYS_TO]->presets[0].e[0];
        bad += check("FUN4 phys 0: PHYS DUST -> ANALOG (PHYS retired)", ok);
    }

    {   /* the FUN7 name: the reserved tail's last 12 bytes, covered by the hash; zero = no name */
        project_t a, c;
        project_store_t st, st2;
        uint32_t i, zero = 1;
        bad += check("FUNB name at the end of the reserved tail, the FM6 patches and the kit before it, after the data (48: the punch-in block, no spare)",
                     PROJ_NAME_OFF == 3824u && PROJ_FM6_OFF == 3312u && PROJ_DX_OFF == 3216u &&
                     68u + PROJ_LT * (P_COUNT + 2u + NSTEP * 9u) + sizeof(chain_config_t) + sizeof(motion_store_t) + 48u ==
                     PROJ_DX_OFF);
        memset(&a, 0, sizeof a);
        a.magic = PROJ_MAGIC; a.size = sizeof a; a.parts = NPART; a.phys = PROJ_PHYS;
        chain_defaults(&a.chain);
        a.t[1].p[P_LEVEL] = 99;
        a.sum = proj_sum(&a);
        ok = proj_pack_vb(&st, &a);
        for (i = 0; i < PROJ_NAME_LEN; i++) zero &= st.raw[PROJ_NAME_OFF + i] == 0u;
        bad += check("FUN7 no name: the tail all zero (as firmware before wrote it)", ok && zero);
        bad += check("FUN7 of before names (zero tail): loads, no name", proj_import(&c, &st, sizeof st) && !c.name[0] &&
                     c.t[1].p[P_LEVEL] == 99);
        memcpy(a.name, "lofi jam", 8);
        a.sum = proj_sum(&a);
        ok = proj_pack_vb(&st, &a) && proj_import(&c, &st, sizeof st);
        bad += check("FUN7 name round trip (upper case, 0-padded)", ok && !memcmp(c.name, "LOFI JAM\0\0\0\0", 12) &&
                     !memcmp(st.raw + PROJ_NAME_OFF, "LOFI JAM\0\0\0\0", 12));
        memcpy(a.name, "ABCDEFGHIJKL", 12);         /* 12: no 0 */
        a.sum = proj_sum(&a);
        ok = proj_pack_vb(&st, &a) && proj_import(&c, &st, sizeof st);
        bad += check("FUN7 name of 12 characters (no terminator)", ok && !memcmp(c.name, "ABCDEFGHIJKL", 12));
        st2 = st;
        st2.raw[PROJ_NAME_OFF + 3] ^= 1u;
        bad += check("FUN7 name byte changed: the hash refuses the project", !proj_import(&c, &st2, sizeof st2));
        st2 = st;
        st2.raw[PROJ_NAME_OFF + 3] = 7u;            /* not printable, the hash right */
        {
            uint32_t sum = proj_hash(st2.raw, PROJ_STORE_SIZE - 4u);
            memcpy(st2.raw + PROJ_STORE_SIZE - 4u, &sum, 4);
        }
        bad += check("FUN7 name with a byte outside 32..126: the project loads, no name",
                     proj_import(&c, &st2, sizeof st2) && !c.name[0] && c.t[1].p[P_LEVEL] == 99);
        bad += check("FUN6..FUN1 imports: no name", !q2.name[0] && !q.name[0]);
    }

    {   /* FUN8: each track's FM6 patch travels with the project; FUN7 and older get the init patch */
        project_t a, c;
        static project_store_t st, slot;
        static uint8_t v7[PROJ_STORE_V7];
        uint32_t i, k, init = 1, sum;
        memset(&a, 0, sizeof a);
        a.magic = PROJ_MAGIC; a.size = sizeof a; a.parts = NPART; a.phys = PROJ_PHYS;
        chain_defaults(&a.chain);
        for (k = 0; k < PROJ_LT; k++) {
            memcpy(a.fm6[k], FM6_FACTORY[k * 2u], FM6_PACKED);
            a.t[k].engine = ENGI_FM6;
            a.t[k].p[P_E7] = (int16_t)(k * 2u);
        }
        memcpy(a.name, "FM SONG", 7);
        a.sum = proj_sum(&a);
        ok = proj_pack_vb(&st, &a) && ((uint32_t *)st.raw)[0] == 0x46554E42u && proj_import(&c, &st, sizeof st);
        for (k = 0; k < PROJ_LT; k++)
            ok &= !memcmp(c.fm6[k], FM6_FACTORY[k * 2u], FM6_PACKED) && c.t[k].engine == ENGI_FM6 &&
                  !memcmp(st.raw + PROJ_FM6_OFF + k * FM6_PACKED, FM6_FACTORY[k * 2u], FM6_PACKED);
        bad += check("FUNA: the four FM6 patches round trip (bytes at PROJ_FM6_OFF)", ok && !memcmp(c.name, "FM SONG", 7));
        st.raw[PROJ_FM6_OFF + 5] ^= 1u;
        bad += check("FUNA: a patch byte changed: the hash refuses it", !proj_import(&c, &st, sizeof st));
        st.raw[PROJ_FM6_OFF + 5] ^= 1u;
        {   /* FUNA's DRUM-X kit (JIANT): round trip; a value out of range refused; a FUN9 loads the factory kit */
            static uint8_t v9[3648];
            static project_store_t st2;
            uint32_t magic9 = 0x46554E39u, size9 = 3648u, sum9;
            memcpy(a.dx, DX_KIT_DEF, sizeof a.dx);
            a.dx[3].b[DXP_COLOR] = 99;
            a.dx[0].mode = DX_MODE(DXW_BELL, DXF_HP, 1);
            a.sum = proj_sum(&a);
            ok = proj_pack_vb(&st, &a) && proj_import(&c, &st, sizeof st) && !memcmp(c.dx, a.dx, sizeof a.dx) &&
                 !memcmp(st.raw + PROJ_DX_OFF, a.dx, sizeof a.dx);
            bad += check("FUNA: the DRUM-X kit round trips (88 bytes at PROJ_DX_OFF)", ok);
            a.dx_mute = (uint16_t)(DXG_SNARE | DXG_PERC | DXM_LANE(6));
            a.sum = proj_sum(&a);
            ok = proj_pack_vb(&st, &a) && st.raw[PROJ_DX_OFF + 88u] == (DXG_SNARE | DXG_PERC) && st.raw[PROJ_DX_OFF + 89u] == 0x40 &&
                 proj_import(&c, &st, sizeof st) && c.dx_mute == a.dx_mute;
            bad += check("FUNA: the group and sound mutes round trip (the kit's first two reserved bytes)", ok);
            memset(a.pfx_lane, 0, sizeof a.pfx_lane);
            a.pfx_lane[2] = 0x21; a.pfx_lane[31] = 0xA0; a.pfx_ltgt = 2;
            a.sum = proj_sum(&a);
            ok = proj_pack_vb(&st, &a) && st.raw[PROJ_PFX_OFF + 2] == 0x21 && st.raw[PROJ_PFX_OFF + 32] == 2 &&
                 proj_import(&c, &st, sizeof st) && !memcmp(c.pfx_lane, a.pfx_lane, sizeof a.pfx_lane) && c.pfx_ltgt == 2;
            bad += check("FUNA: the punch-in lane round trips (the spare bytes at PROJ_PFX_OFF)", ok);
            {   /* (JIANT 0.3) the nine new globals round trip behind a 2; a FUNB of 0.2 (behind a 1: five) loads the
                 * other four at their defaults; DRUM's E5 (FM) kept behind the kit's marker 2, 0 behind a 1 */
                uint32_t sm;
                a.g[G_RPRE] = 40; a.g[G_RFILT] = -30; a.g[G_RWIDE] = 90; a.g[G_DPIT] = -7; a.g[G_DSPRY] = 55; a.g[G_STRN] = -3;
                a.t[1].engine = ENGI_DRUM; a.t[1].p[P_E5] = 77;
                a.sum = proj_sum(&a);
                ok = proj_pack_vb(&st, &a) && st.raw[PROJ_GX_OFF] == 3u && proj_import(&c, &st, sizeof st) &&
                     c.g[G_RPRE] == 40 && c.g[G_RFILT] == -30 && c.g[G_RWIDE] == 90 && c.g[G_DPIT] == -7 &&
                     c.g[G_DSPRY] == 55 && c.g[G_STRN] == -3 && c.t[1].p[P_E5] == 77;
                bad += check("FUNB: the ten new globals round trip (FILT -30, PITCH -7, STRN -3: signed), DRUM's FM kept", ok);
                st.raw[PROJ_GX_OFF] = 2u;                   /* (0.3: nine; STRN at its default) */
                sm = proj_hash(st.raw, PROJ_STORE_SIZE - 4u);
                memcpy(st.raw + PROJ_STORE_SIZE - 4u, &sm, 4);
                ok = proj_import(&c, &st, sizeof st) && c.g[G_DSPRY] == 55 && c.g[G_STRN] == 0;
                bad += check("FUNB of JIANT 0.3 (marker 2): its nine globals, STRN at its default", ok);
                st.raw[PROJ_GX_OFF] = 1u;
                st.raw[PROJ_DX_OFF + 90u] = 1u;
                sm = proj_hash(st.raw, PROJ_STORE_SIZE - 4u);
                memcpy(st.raw + PROJ_STORE_SIZE - 4u, &sm, 4);
                ok = proj_import(&c, &st, sizeof st) && c.g[G_RPRE] == 40 && c.g[G_RFILT] == GP[G_RFILT].def &&
                     c.g[G_RWIDE] == GP[G_RWIDE].def && c.g[G_DPIT] == 0 && c.g[G_DSPRY] == 0 && c.t[1].p[P_E5] == 0;
                bad += check("FUNB of JIANT 0.2 (markers 1): its five globals, the four new at their defaults, DRUM's E5 (ACC) FM 0", ok);
                a.g[G_RPRE] = a.g[G_RFILT] = a.g[G_RWIDE] = a.g[G_DPIT] = a.g[G_DSPRY] = a.g[G_STRN] = 0;
            }
            {   /* (JIANT 0.4) a TRIO track (engine 6, folded into ANALOG): ANALOG's nearest, CUT RES kept, envelope kept */
                static const int16_t TS[8] = {9, 9, 0, 0, 0, 82, 30, 64};   /* SYNC LEAD: WAVE SYNC, INT2 9 */
                uint32_t i;
                a.t[2].engine = ENGI_TRIO;
                for (i = 0; i < 8u; i++) a.t[2].p[P_E0 + i] = TS[i];
                a.t[2].p[P_ATK] = 2; a.t[2].p[P_REL] = 40;
                a.sum = proj_sum(&a);
                ok = proj_pack_vb(&st, &a) && proj_import(&c, &st, sizeof st) && c.t[2].engine == 0 &&
                     c.t[2].p[P_E0] == 5 && c.t[2].p[P_E0 + 1] == 11 && c.t[2].p[P_E0 + 4] == 82 && c.t[2].p[P_E0 + 5] == 30 &&
                     c.t[2].p[P_ATK] == 2 && c.t[2].p[P_REL] == 40;
                bad += check("TRIO (engine 6, folded in): ANALOG SYNC (its INT2 9 the ratio, DTN 11), CUT RES and the envelope kept", ok);
                a.t[2].engine = ENGI_FM6;                   /* (as it was: FM6, E7 its patch 4) */
                for (i = 0; i < 8u; i++) a.t[2].p[P_E0 + i] = (int16_t)(i == 7u ? 4 : 0);
                a.t[2].p[P_ATK] = a.t[2].p[P_REL] = 0;
                a.sum = proj_sum(&a);
                proj_pack_vb(&st, &a);                         /* (st as before: the older images below are made of it) */
                a.t[1].engine = ENGI_FM6; a.t[1].p[P_E5] = 0;
                a.sum = proj_sum(&a);
            }
            a.dx[2].a[DXP_PITCH] = 200;
            a.sum = proj_sum(&a);
            ok = !proj_pack_vb(&st2, &a);
            a.dx[2].a[DXP_PITCH] = 60;
            a.sum = proj_sum(&a);
            proj_pack_vb(&st2, &a);
            st2.raw[PROJ_DX_OFF + 7] = 0xC8;             /* (a value past 127, the hash made right) */
            {
                uint32_t h = proj_hash(st2.raw, PROJ_STORE_SIZE - 4u);
                memcpy(st2.raw + PROJ_STORE_SIZE - 4u, &h, 4);
            }
            ok &= !proj_import(&c, &st2, sizeof st2);
            bad += check("FUNA: a kit value out of range: not packed, not loaded", ok);
            (void)magic9; (void)size9; (void)sum9;
            old_image(v9, &st, 99u, 3648u, 0x46554E39u);  /* FUN9 = FUNA without the kit (99 parameters) */
            ok = proj_import(&c, v9, sizeof v9) && !c.dx_mute && !memcmp(c.dx, DX_KIT_DEF, sizeof c.dx) &&
                 !memcmp(c.fm6, a.fm6, PROJ_LT * sizeof c.fm6[0]) && !memcmp(c.name, "FM SONG", 7) && c.t[2].p[P_E7] == 4 &&
                 c.t[0].p[P_MSLEN] == 16 && c.g[G_RRATE] == 30;
            bad += check("FUN9 (Felucca 1.1) loads: its patches and name, the factory DRUM-X kit, the new values' defaults", ok);
            {   /* FUNA (JIANT 0.1, 99 parameters): its kit, mutes and lane; the new parameters and globals their defaults */
                static uint8_t va[3744];
                a.dx_mute = DXG_SNARE; a.g[G_RMOD] = 77; a.t[1].p[P_DTYPE] = 3; a.sum = proj_sum(&a);
                ok = proj_pack_vb(&st2, &a);
                old_image(va, &st2, 99u, 3744u, 0x46554E41u);
                ok &= proj_import(&c, va, sizeof va) && c.dx_mute == DXG_SNARE && !memcmp(c.dx, a.dx, sizeof c.dx) &&
                      c.t[2].p[P_E7] == 4 && c.t[1].p[P_DTYPE] == 0 && c.g[G_RMOD] == 0 && c.t[3].p[P_MSDIV] == 2;
                bad += check("FUNA (JIANT 0.1) loads: kit, mutes, E0..E7 at P_E0; DIST TYPE, MSEQ, the new globals: defaults", ok);
                ok = proj_import(&c, &st2, sizeof st2) && c.g[G_RMOD] == 77 && c.t[1].p[P_DTYPE] == 3;
                bad += check("FUNB round trip: the new globals and parameters kept", ok);
                a.dx_mute = 0; a.g[G_RMOD] = 0; a.t[1].p[P_DTYPE] = 0; a.sum = proj_sum(&a);
            }
        }
        {   /* FUN8 as 1.0.x wrote it (3584 bytes, 91 parameters: E0..E7 at 83..90, the patches at 3056): the DRUM
             * lane levels 100 %, E0..E7 and their motion at today's P_E0.., the patches and the name kept */
            static uint8_t v8[3584];
            uint32_t pos = 68u, pos9 = 68u, j;
            project_t e = a;
            e.t[3].engine = ENGI_DRUM; e.t[3].p[P_E3] = 30;
            e.motion.count = 2; e.motion.on = 8;
            e.motion.event[0] = (motion_event_t){3u << 6 | 2u, P_E3, 99};
            e.motion.event[1] = (motion_event_t){3u << 6 | 4u, P_REV, 50};
            {   /* FUN9 holds parameter locks (bit 7 of the id): on a DRUM lane level and on an engine value */
                project_t f = e, g;
                f.motion.count = 4;
                f.motion.event[2] = (motion_event_t){3u << 6 | 5u, (uint8_t)(P_LN2 | MOTION_LOCK), 40};
                f.motion.event[3] = (motion_event_t){3u << 6 | 6u, (uint8_t)(P_E5 | MOTION_LOCK), -3};
                f.sum = proj_sum(&f);
                ok = proj_pack_vb(&st, &f) && proj_import(&g, &st, sizeof st) && !memcmp(&f.motion, &g.motion, sizeof f.motion);
                bad += check("FUN9 round trip: locks on a lane level (P_LN2) and an engine value (P_E5) kept", ok &&
                             st.raw[68u + PROJ_LT * (P_COUNT + 2u + NSTEP * 9u) + sizeof(chain_config_t) + 4u + 8u + 1u] ==
                             (P_LN2 | MOTION_LOCK) && g.motion.event[3].param == (P_E5 | MOTION_LOCK));
            }
            e.motion.count = 3;   /* a FUN8 of a 1.1 development build (91 parameters) could hold a lock: its E5 then */
            e.motion.event[2] = (motion_event_t){3u << 6 | 6u, (uint8_t)(P_E5 | MOTION_LOCK), -3};
            e.sum = proj_sum(&e);
            ok = proj_pack_vb(&st, &e);
            (void)pos; (void)pos9; (void)j;
            old_image(v8, &st, 91u, 3584u, 0x46554E38u);  /* (its E3 then: 86, its E5 a lock: 88 | the bit) */
            ok &= v8[old_motion + 1u] == 83u + 3u && v8[old_motion + 8u + 1u] == ((83u + 5u) | MOTION_LOCK);
            ok = ok && proj_import(&c, v8, sizeof v8);
            for (k = 0; ok && k < PROJ_LT; k++) {
                for (j = P_LN0; j <= P_LN7; j++) ok &= c.t[k].p[j] == 127;
                ok &= !memcmp(c.fm6[k], FM6_FACTORY[k * 2u], FM6_PACKED);
            }
            bad += check("FUN8 of 1.0.x (91 parameters): lane levels 100 %, E0..E7 at P_E0, patches and name kept", ok &&
                         c.t[3].engine == ENGI_DRUM && c.t[3].p[P_E3] == 30 && c.t[2].p[P_E7] == 4 &&
                         !memcmp(c.name, "FM SONG", 7));
            bad += check("  its motion: E3 (86 then) at P_E3, REV kept", c.motion.count == 3u &&
                         c.motion.event[0].param == P_E3 && c.motion.event[0].value == 99 && c.motion.event[1].param == P_REV);
            bad += check("  a lock on its E5 (88 then): at P_E5, still a lock", c.motion.event[2].param == (P_E5 | MOTION_LOCK) &&
                         c.motion.event[2].value == -3);
            bad += check("  one byte short: refused", !proj_import(&c, v8, sizeof v8 - 1u));
            memset(&slot, 0xA5, sizeof slot);
            memcpy(slot.raw, v8, sizeof v8);
            bad += check("  in a retained (longer) slot: loads", proj_import(&c, &slot, sizeof slot) && c.t[3].p[P_E3] == 30);
        }
        /* the same music as FUN7 (3388 bytes, no patches): the data where it was, the name at 3372 */
        old_image(v7, &st, 99u, PROJ_STORE_V7, 0x46554E37u);
        ok = proj_import(&c, v7, sizeof v7);
        for (k = 0; k < PROJ_LT; k++)
            init &= !memcmp(c.fm6[k], FM6_INIT, FM6_PACKED);
        bad += check("FUN7 -> FUN9: the music and name kept, every track the init patch",
                     ok && init && c.t[2].engine == ENGI_FM6 && c.t[2].p[P_E7] == 4 && !memcmp(c.name, "FM SONG", 7));
        {   /* REVERB TYPE (id 24) of a FUN7 holding the old drum channel (10): ROOM; SPRING (1) kept */
            int16_t g24 = 10;
            memcpy(v7 + 8u + 2u * G_RTYPE, &g24, 2);
            sum = proj_hash(v7, PROJ_STORE_V7 - 4u);
            memcpy(v7 + PROJ_STORE_V7 - 4u, &sum, 4);
            ok = proj_import(&c, v7, sizeof v7) && c.g[G_RTYPE] == 0 && proj_ok(&c);
            g24 = 1;
            memcpy(v7 + 8u + 2u * G_RTYPE, &g24, 2);
            sum = proj_hash(v7, PROJ_STORE_V7 - 4u);
            memcpy(v7 + PROJ_STORE_V7 - 4u, &sum, 4);
            ok &= proj_import(&c, v7, sizeof v7) && c.g[G_RTYPE] == 1;
            bad += check("FUN7 REVERB TYPE above SPRING (an old drum channel): ROOM", ok);
        }
        ok = proj_import(&c, v7, sizeof v7 - 1u);
        bad += check("FUN7 one byte short: refused", !ok);
        /* the retained cache after an update: slot 1's FUN7 record, longer slot, garbage after it */
        memset(&slot, 0xA5, sizeof slot);
        memcpy(slot.raw, v7, sizeof v7);
        bad += check("a retained 3648-byte slot holding a FUN7 record loads it", proj_import(&c, &slot, sizeof slot) &&
                     c.t[2].p[P_E7] == 4 && !memcmp(c.fm6[0], FM6_INIT, FM6_PACKED));
        memset(&slot, 0xA5, sizeof slot);
        bad += check("a retained slot of garbage is empty", !proj_import(&c, &slot, sizeof slot));
        for (i = 0; i < FM6_PACKED; i++) a.fm6[1][i] = (uint8_t)(i | 0x80u);
        a.sum = proj_sum(&a);
        bad += check("FUN8: a patch byte above 127 cannot be packed", !proj_pack_vb(&st, &a));
        memcpy(a.fm6[1], FM6_INIT, FM6_PACKED);
        a.t[0].step[3].n = 1; a.t[0].step[3].note[0] = 200; a.t[0].step[3].note[2] = 128;
        a.t[0].step[3].vel = 255; a.t[0].step[3].hit = 0x05; a.t[0].step[3].acc = 0xF7;
        a.sum = proj_sum(&a);
        ok = proj_pack_vb(&st, &a) && proj_import(&c, &st, sizeof st);
        bad += check("FUN8: a step out of range packs inside it (note, velocity 127; accents on hits): it loads",
                     ok && c.t[0].step[3].note[0] == 127u && c.t[0].step[3].note[2] == 127u && c.t[0].step[3].vel == 127u &&
                     c.t[0].step[3].hit == 0x05u && c.t[0].step[3].acc == 0x05u);
    }

#if !FELUCCA_FM4
    {   /* DIGITAL tracks (engine 1, retired: src/fm4_convert.c) in FUN8 / FUN7 / FUN4 / FUN1 projects load as FM6 with
         * the converted patch as the track's own; their motion on the EDIT or OP ENV values goes, the rest stays */
        project_t a, c, d;
        static project_store_t st;
        static uint8_t v7[PROJ_STORE_V7];
        static project_v4_t w4;
        static project_v1_t w1;
        uint8_t v[FP_SIZE + 1u], pk[FM6_PACKED];
        int16_t p[P_COUNT];
        uint32_t i, sum;
        memset(&a, 0, sizeof a);
        a.magic = PROJ_MAGIC; a.size = sizeof a; a.parts = NPART; a.phys = PROJ_PHYS;
        chain_defaults(&a.chain);
        for (t = 0; t < PROJ_LT; t++) {
            for (i = 0; i < P_COUNT; i++)
                a.t[t].p[i] = param_desc_of(t == 1u ? ENGI_DIGITAL : 0u, i)->def;
            a.t[t].engine = t == 1u ? ENGI_DIGITAL : 0u;
            a.t[t].preset = t == 1u ? 5u : 0u;
            memcpy(a.fm6[t], FM6_INIT, FM6_PACKED);
        }
        fm4_preset_values(a.t[1].p, 5);                 /* DIGITAL PAD, an OP ENV edit on op 2 */
        a.t[1].p[P_FM2_ATK] = 30;
        a.t[1].p[P_FM2_LEVEL] = 100;
        a.t[1].p[P_LEVEL] = 90;
        memcpy(p, a.t[1].p, sizeof p);
        fm4_convert(p, v);
        fm6_pack(v, pk);
        a.motion.count = 5;
        a.motion.event[0] = (motion_event_t){1u << 6 | 3u, P_E4, 20};          /* track 2: INDEX */
        a.motion.event[1] = (motion_event_t){1u << 6 | 4u, P_FM1_LEVEL, 50};   /* track 2: OP LEVEL */
        a.motion.event[2] = (motion_event_t){1u << 6 | 5u, P_CHOR, 60};        /* track 2: a send: stays */
        a.motion.event[3] = (motion_event_t){0u << 6 | 3u, P_E4, 70};          /* track 1 (ANALOG): stays */
        a.motion.event[4] = (motion_event_t){1u << 6 | 9u, P_E7, 0};           /* track 2: E7 */
        a.sum = proj_sum(&a);
#define FM4_OK(c) ((c).t[1].engine == ENGI_FM6 && (c).t[1].preset == 4u && !memcmp((c).fm6[1], pk, FM6_PACKED) && \
                   !memcmp((c).t[1].p, p, sizeof p) && (c).t[0].engine == 0u && !memcmp((c).fm6[0], FM6_INIT, FM6_PACKED))
        ok = proj_pack_vb(&st, &a) && proj_import(&c, &st, sizeof st);
        bad += check("FUN8 with a DIGITAL track: FM6, the converted patch its own, PTCH / preset FM6 PAD", ok && FM4_OK(c));
        bad += check("  its motion on INDEX / OP LEVEL / E7 goes; a send's and the other track's stay",
                     c.motion.count == 2u && c.motion.event[0].param == P_CHOR && c.motion.event[1].place == 3u &&
                     c.motion.event[1].param == P_E4 && motion_valid(&c.motion) && proj_ok(&c));
        bad += check("  imported again: as it is (FM6 now)", proj_import(&d, &c, sizeof c) && !memcmp(&d, &c, sizeof d));
        memcpy(v7, st.raw, PROJ_STORE_V7 - 16u);       /* the same as FUN7 (as the FUN8 block above) */
        memset(v7 + PROJ_FM6_OFF, 0, PROJ_STORE_V7 - 16u - PROJ_FM6_OFF);
        memset(v7 + PROJ_STORE_V7 - 16u, 0, 12);
        ((uint32_t *)v7)[0] = 0x46554E37u;
        ((uint32_t *)v7)[1] = PROJ_STORE_V7;
        sum = proj_hash(v7, PROJ_STORE_V7 - 4u);
        memcpy(v7 + PROJ_STORE_V7 - 4u, &sum, 4);
        bad += check("FUN7 with a DIGITAL track: the same", proj_import(&c, v7, sizeof v7) && FM4_OK(c) &&
                     c.motion.count == 2u);
        memset(&a.motion, 0, sizeof a.motion);
        a.sum = proj_sum(&a);
        to_v4(&w4, &a);
        memcpy(&buf, &w4, sizeof w4);
        memcpy(p, a.t[1].p, sizeof p);                  /* (FUN4 had no OP ENV values: they load as defaults) */
        for (i = P_FM1_ATK; i <= P_FM4_LEVEL; i++)
            p[i] = TP[i].def;
        fm4_convert(p, v);
        fm6_pack(v, pk);
        ok = proj_import(&c, &buf, (int)sizeof w4) && FM4_OK(c);
        bad += check("FUN4 with a DIGITAL track (no OP ENV then: their defaults): FM6, converted", ok);
        memset(&w1, 0, sizeof w1);
        w1.magic = PROJ_MAGIC_V1;
        w1.size = sizeof w1;
        for (i = 0; i < PROJ_NG_V2; i++)
            w1.g[i] = GP[i].def;
        for (i = 0; i < PROJ_NP_V2 - 8u; i++)
            w1.t.p[i] = TP[i].def;
        for (i = 0; i < 8u; i++)
            w1.t.p[PROJ_NP_V2 - 8u + i] = DIGITAL_PRESETS[1].e[i];   /* BELL */
        w1.t.engine = ENGI_DIGITAL;
        w1.t.preset = 1;
        w1.sum = proj_hash(&w1, sizeof w1 - 4u);
        memcpy(&buf, &w1, sizeof w1);
        ok = proj_import(&c, &buf, (int)sizeof w1) && c.t[0].engine == ENGI_FM6 && c.t[0].preset == 1u &&
             c.t[1].engine == ENGI_FM6 && c.t[1].preset == PROJ_DEF_SOUND;   /* (track 2: the power-on FM6 PAD) */
        bad += check("FUN1 with DIGITAL: track 1 FM6 (BELL), track 2 the power-on sound (FM6 now)", ok);
#undef FM4_OK
    }
#endif
    {   /* SAMPLE tracks of SET 4 (PERC, the GM kit, retired after 1.0.2) in FUN8 / FUN4 projects load as DRUM with its
         * default kit: the E values the kit's, the rest of the sound, the steps and the preset byte's meaning kept; their
         * motion on the EDIT values goes, the rest stays; another SAMPLE set stays SAMPLE. Idempotent */
        project_t a, c, d;
        static project_store_t st;
        static project_v4_t w4;
        static const int16_t KIT[8] = DRUM_KIT_E;
        uint32_t i;
        memset(&a, 0, sizeof a);
        a.magic = PROJ_MAGIC; a.size = sizeof a; a.parts = NPART; a.phys = PROJ_PHYS;
        chain_defaults(&a.chain);
        for (t = 0; t < PROJ_LT; t++) {
            for (i = 0; i < P_COUNT; i++)
                a.t[t].p[i] = param_desc_of(ENGI_SAMPLE, i)->def;
            a.t[t].engine = ENGI_SAMPLE;
            memcpy(a.fm6[t], FM6_INIT, FM6_PACKED);
        }
        a.t[2].p[P_E0] = SMP_SET_PERC;                  /* track 3: a saved PERC sound (SET 4, no loop) */
        a.t[2].p[P_E3] = 0;
        a.t[2].p[P_LEVEL] = 77; a.t[2].p[P_REV] = 41; a.t[2].p[P_PAN] = -9;
        a.t[2].preset = 4;
        a.t[2].step[0] = (step_t){{36, 42, 0, 0}, 2, ST_NOTE, SF_ACCENT, 100, 0, 0};
        a.t[2].step[4] = (step_t){{38, 49, 0, 0}, 2, ST_NOTE, 0, 90, 0, 0};
        a.t[0].p[P_E0] = 2;                             /* track 1: FLUTE (SAMPLE retired in JIANT: ANALOG) */
        a.motion.count = 3;
        a.motion.event[0] = (motion_event_t){2u << 6 | 3u, P_E4, 20};          /* track 3: CUT: goes */
        a.motion.event[1] = (motion_event_t){2u << 6 | 5u, P_REV, 60};         /* track 3: a send: stays */
        a.motion.event[2] = (motion_event_t){0u << 6 | 3u, P_E4, 70};          /* track 1: stays */
        a.sum = proj_sum(&a);
#define PERC_OK(c) ((c).t[2].engine == ENGI_DRUM && (c).t[2].preset == 0u && !memcmp(&(c).t[2].p[P_E0], KIT, sizeof KIT) && \
                    !memcmp((c).t[2].p, a.t[2].p, P_E0 * sizeof(int16_t)) && \
                    !memcmp((c).t[2].step, a.t[2].step, sizeof a.t[2].step) && \
                    (c).t[0].engine == ENGI_PHYS_TO && (c).t[0].p[P_E0] == ENGINES[ENGI_PHYS_TO]->presets[0].e[0] && \
                    (c).t[1].engine == ENGI_PHYS_TO)
        ok = proj_pack_vb(&st, &a) && proj_import(&c, &st, sizeof st);
        bad += check("FUN8 with a SAMPLE PERC track: DRUM's kit, the rest of the sound and the steps kept; SAMPLE tracks ANALOG", ok && PERC_OK(c) &&
                     str_eq(ENGINES[ENGI_DRUM]->presets[0].name, "DRUM-X") &&
                     !memcmp(ENGINES[ENGI_DRUM]->presets[0].e, (int8_t[8])DRUM_KIT_E, 8));
        bad += check("  its motion on CUT goes, the retired SAMPLE track's too (ANALOG now); a send's stays",
                     c.motion.count == 1u && c.motion.event[0].param == P_REV && motion_valid(&c.motion) && proj_ok(&c));
        bad += check("  imported again: as it is (DRUM now)", proj_import(&d, &c, sizeof c) && !memcmp(&d, &c, sizeof d));
        memset(&a.motion, 0, sizeof a.motion);
        a.sum = proj_sum(&a);
        to_v4(&w4, &a);
        memcpy(&buf, &w4, sizeof w4);
        ok = proj_import(&c, &buf, (int)sizeof w4) && PERC_OK(c);
        bad += check("FUN4 with a SAMPLE PERC track: DRUM's kit, the same", ok);
#undef PERC_OK
    }
    {   /* (JIANT) GRAIN (8) and SLICE (13) tracks, retired: ANALOG's first preset, their engine motion gone, the rest kept */
        static project_t g, h;
        static project_store_t gs;
        memset(&g, 0, sizeof g);
        g.magic = PROJ_MAGIC; g.size = sizeof g; g.parts = NPART; g.phys = PROJ_PHYS;
        chain_defaults(&g.chain);
        for (t = 0; t < PROJ_LT; t++) {
            for (i = 0; i < P_COUNT; i++) g.t[t].p[i] = param_desc_of(0, i)->def;
            memcpy(g.fm6[t], FM6_INIT, FM6_PACKED);
            memcpy(g.dx, DX_KIT_DEF, sizeof g.dx);
        }
        g.t[1].engine = ENGI_SAMPLE; g.t[2].engine = ENGI_PHYS; g.t[2].p[P_LEVEL] = 66;   /* (0.9.9-snd5: GRAIN's and SLICE's numbers
                                                         * are STRING and ADD now: two other retired ones) */
        g.motion.count = 2; g.motion.on = 4;
        g.motion.event[0] = (motion_event_t){2u << 6 | 1u, P_E2, 30};
        g.motion.event[1] = (motion_event_t){2u << 6 | 2u, P_REV, 40};
        g.sum = proj_sum(&g);
        ok = proj_pack_vb(&gs, &g) && proj_import(&h, &gs, sizeof gs) && h.t[1].engine == ENGI_PHYS_TO &&
             h.t[2].engine == ENGI_PHYS_TO && h.t[2].p[P_LEVEL] == 66 && h.motion.count == 1u && h.motion.event[0].param == P_REV &&
             h.t[2].p[P_E0] == ENGINES[ENGI_PHYS_TO]->presets[0].e[0] && !eng_ok(ENGI_PHYS) && eng_ok(ENGI_STRING) && eng_ok(ENGI_ADD) &&
             !eng_ok(ENGI_SAMPLE) && eng_live(ENGI_PHYS) == ENGI_PHYS_TO;
        bad += check("JIANT: SAMPLE and PHYS tracks (retired) load as ANALOG, their engine motion gone, LEVEL and sends kept", ok);
    }
    {   /* (JIANT 0.6) FUNC: six tracks in FUNB's 3840 bytes, only the steps that are not empty stored */
        static project_t f, g, h;
        static project_store_t fs, gs;
        uint32_t k, i, n;
        memset(&f, 0, sizeof f);
        f.magic = PROJ_MAGIC; f.size = sizeof f; f.parts = NPART; f.phys = PROJ_PHYS;
        chain_defaults(&f.chain);
        proj_new_tracks(&f, 0u);
        for (k = 0; k < NTRK; k++) memcpy(f.fm6[k], FM6_INIT, FM6_PACKED);
        for (k = 0; k < G_COUNT; k++) f.g[k] = GP[k].def;
        memcpy(f.dx, DX_KIT_DEF, sizeof f.dx);
        for (k = 0; k < NTRK; k++)                      /* a typical song: 16 steps a track, T5 a chord on all 64 */
            for (i = 0; i < (k == 4u ? NSTEP : 16u); i++) {
                step_t *s = &f.t[k].step[i];
                s->time = ST_NOTE; s->n = (uint8_t)(k == 4u ? 3u : 1u); s->vel = (uint8_t)(60u + k);
                s->note[0] = (uint8_t)(40u + i); s->note[1] = 64; s->note[2] = 67;
                if (i == 3u) step_set_ratchet(s, 3u), step_set_chance(s, 40u);
            }
        f.t[5].p[P_LEVEL] = 77; f.t[4].engine = ENGI_FM6; memcpy(f.fm6[4], FM6_FACTORY[3], FM6_PACKED);
        motion_at(&f.motion.event[0], 5u << 6 | 9u, 33); f.motion.event[0].param = P_LEVEL;
        motion_at(&f.motion.event[1], 4u << 6 | 2u, -20); f.motion.event[1].param = P_PAN | MOTION_LOCK;
        f.motion.count = 2; f.motion.on = 0x30;
        memcpy(f.name, "SIX", 3);
        f.sum = proj_sum(&f);
        ok = proj_pack(&fs, &f) && ((uint32_t *)fs.raw)[0] == PROJ_MAGIC && fs.raw[67] == NTRK && proj_import(&g, &fs, sizeof fs);
        ok &= !memcmp(g.t, f.t, sizeof f.t) && !memcmp(g.fm6, f.fm6, sizeof f.fm6) && !memcmp(&g.motion, &f.motion, sizeof f.motion) &&
              !memcmp(g.name, "SIX", 3) && !memcmp(g.g, f.g, sizeof f.g) && !memcmp(g.dx, f.dx, sizeof f.dx);
        ok &= MOTION_TRK(&g.motion.event[0]) == 5u && MOTION_STEP(&g.motion.event[0]) == 9u && MOTION_VAL(&g.motion.event[0]) == 33 &&
              MOTION_TRK(&g.motion.event[1]) == 4u && MOTION_VAL(&g.motion.event[1]) == -20;
        bad += check("FUNC: six tracks round trip (steps, ratchet, chance, patches, T5 / T6 motion, name, globals)", ok);
        ok = proj_pack_vb(&gs, &f) && proj_import(&h, &gs, sizeof gs) && !memcmp(h.t, f.t, PROJ_LT * sizeof f.t[0]) &&
             h.t[4].engine == trk_old_engine(4u) && h.t[5].engine == trk_old_engine(5u) && h.t[5].step[0].time == ST_REST;
        bad += check("FUNB (0.5) -> six tracks: its four kept, T5 / T6 at 0.9.8's power-on sounds (TRK_OLD), empty", ok);
        {   /* (JIANT 0.8.5) VOICE's lyrics: 'L' 'Y' blocks for the tracks that have one; none before */
            static project_t l1, l2;
            static project_store_t ls;
            static const char *const LY[NTRK] = {"la noche es azul", "", "", "you are my sunshine - oh",
                                                 "ABCDEFGHIJKLMNOPQRSTUVWXYZABCDEFGHIJKLMNOPQRSTUVWXYZABCDEFGHIJK", "sakura"};
            uint32_t hv, nl = 0;
            int oky = 1;
            ok = proj_import(&l2, &fs, sizeof fs);          /* the FUNC above: written without lyrics */
            for (k = 0; k < NTRK; k++)
                ok &= !l2.lyr[k][0];
            bad += check("0.8.5: a project without lyrics (FUNC of before) loads with none", ok);
            ok = proj_import(&l2, &gs, sizeof gs);
            for (k = 0; k < NTRK; k++)
                ok &= !l2.lyr[k][0];
            bad += check("0.8.5: an older format (FUNB) loads with no lyrics", ok);
            l1 = f;
            for (k = 0; k < NTRK; k++) {
                memset(l1.lyr[k], 0, LYR_LEN);
                memcpy(l1.lyr[k], LY[k], strlen(LY[k]));
                nl += LY[k][0] != 0;
            }
            l1.t[0].engine = 5; l1.t[0].p[P_VLANG] = LG_ES; l1.t[0].p[P_VLYR] = 0; l1.t[0].p[P_E0] = 3;
            l1.sum = proj_sum(&l1);
            ok = proj_pack(&ls, &l1) && proj_import(&l2, &ls, sizeof ls);
            for (k = 0; k < NTRK; k++)
                ok &= !memcmp(l2.lyr[k], l1.lyr[k], LYR_LEN);
            ok &= !memcmp(l2.t, l1.t, sizeof l1.t) && l2.t[0].p[P_VLANG] == LG_ES && l2.t[0].p[P_E0] == 3;
            bad += check("0.8.5: lyrics round trip (four tracks, 63 characters, \"-\"), LANG and POS with the sound", ok);
            l2 = l1;
            memset(l2.lyr, 0, sizeof l2.lyr);
            ok = proj_packed_size(&l1) == proj_packed_size(&l2) + 4u * nl + (uint32_t)strlen(LY[0]) + (uint32_t)strlen(LY[3]) +
                                          (uint32_t)strlen(LY[4]) + (uint32_t)strlen(LY[5]);
            bad += check("0.8.5: the lyrics' blocks counted in the section's room (4 + length a track)", ok);
            {   /* a lyric byte outside 32..126 (hash right): refused; a firmware of before stops at the block */
                static project_store_t bs;
                uint32_t p;
                bs = ls;
                for (p = 68u; p + 4u < PROJC_PFX_OFF && !(bs.raw[p] == 'L' && bs.raw[p + 1u] == 'Y' && bs.raw[p + 2u] == 0u); p++)
                    ;
                oky = p + 4u < PROJC_PFX_OFF && bs.raw[p + 3u] == strlen(LY[0]) && !memcmp(bs.raw + p + 4u, LY[0], strlen(LY[0]));
                bs.raw[p + 5u] = 7;
                hv = proj_hash(bs.raw, PROJ_STORE_SIZE - 4u);
                memcpy(bs.raw + PROJ_STORE_SIZE - 4u, &hv, 4);
                oky &= !proj_import(&l2, &bs, sizeof bs);
                bad += check("0.8.5: the block in place (track, length, text); a control byte in it: refused", oky);
            }
            for (k = 0; k < NTRK; k++)                  /* lyrics on a full section: not packed, proj_full */
                memset(l1.lyr[k], 'A', LYR_LEN - 1u);
            for (k = 0; k < NTRK; k++)
                for (i = 0; i < NSTEP; i++) {
                    l1.t[k].step[i].time = ST_NOTE; l1.t[k].step[i].n = 2; l1.t[k].step[i].note[0] = 60; l1.t[k].step[i].vel = 90;
                }
            l1.sum = proj_sum(&l1);
            ok = proj_packed_size(&l1) <= PROJC_PFX_OFF ? proj_pack(&ls, &l1) && proj_import(&l2, &ls, sizeof ls) :
                                                       !proj_pack(&ls, &l1) && proj_full;
            bad += check("0.8.5: long lyrics on a full section: packed whole, or SECTION FULL (never cut)", ok);
        }
        {   /* (JIANT 0.6.3) the SLICER's bus: its globals round trip; a project of before it: from the tracks' SLICERs */
            static project_t s1, s2;
            s1 = f;
            s1.g[G_SLMODE] = 2; s1.g[G_SLPAT] = 9; s1.g[G_SLRATE] = 4; s1.g[G_SLDEP] = 77;
            for (k = 0; k <= NTRK; k++) s1.g[G_SLT1 + k] = (int16_t)(k & 1u);
            s1.g[G_SLPLEN] = 5; s1.g[G_SLPQ] = 0;
            for (k = 0; k < 16u; k++) s1.g[G_SLP0 + k] = (int16_t)((int)k * 3 - 24);
            s1.sum = proj_sum(&s1);
            ok = proj_pack(&fs, &s1) && proj_import(&s2, &fs, sizeof fs) &&
                 !memcmp(&s2.g[G_SLMODE], &s1.g[G_SLMODE], G_SL_N * sizeof s1.g[0]);
            bad += check("FUNC: the SLICER bus (mode, pattern, rate, depth, T1..T6 FX, pitch LEN QNT, 16 steps) round trips", ok);
            s1 = f;
            for (k = 0; k < NTRK; k++) s1.t[k].p[P_SLCR] = 0;
            s1.t[1].p[P_SLCR] = SL_STUT; s1.t[1].p[P_SLPAT] = 12; s1.t[1].p[P_SLRATE] = 2; s1.t[1].p[P_SLDEPTH] = 90;
            s1.t[3].p[P_SLCR] = SL_GATE;
            s1.sum = proj_sum(&s1);
            ok = proj_pack_vb(&gs, &s1) && proj_import(&s2, &gs, sizeof gs) && s2.g[G_SLMODE] == SL_STUT && s2.g[G_SLPAT] == 12 &&
                 s2.g[G_SLRATE] == 2 && s2.g[G_SLDEP] == 90 && !s2.g[G_SLT1] && s2.g[G_SLT1 + 1] && !s2.g[G_SLT1 + 2] &&
                 s2.g[G_SLT1 + 3] && !s2.g[G_SLT1 + 4] && !s2.g[G_SLFX];
            bad += check("FUNB of 0.5: the tracks' SLICERs become the bus (T2's settings, T2 and T4 in, FX out)", ok);
            for (k = 0, ok = 1; k < NTRK; k++)                /* (0.6.9: their slots are AUTO PAN's: off) */
                ok &= s2.t[k].p[P_APDEP] == 0 && s2.t[k].p[P_APRATE] == TP[P_APRATE].def &&
                      s2.t[k].p[P_APWAVE] == TP[P_APWAVE].def && s2.t[k].p[P_APSYNC] == TP[P_APSYNC].def;
            bad += check("  .. and AUTO PAN, in the old SLICER's slots, off on every track (its defaults)", ok);
        }
        {   /* damaged on purpose with the hash made right: refused or loaded, never read past the room (ASan) */
            static project_store_t z;
            uint32_t r = 12345u, it, okz = 1, loaded = 0;
            for (it = 0; it < 20000u; it++) {
                uint32_t hz, nfl = 1u + (r >> 29), q2i;
                z = fs;
                for (q2i = 0; q2i < nfl; q2i++) {
                    r = r * 1664525u + 1013904223u;
                    z.raw[68u + (r >> 8) % (PROJC_PFX_OFF - 68u)] = (uint8_t)(r >> 24);
                }
                hz = proj_hash(z.raw, PROJ_STORE_SIZE - 4u); memcpy(z.raw + PROJ_STORE_SIZE - 4u, &hz, 4);
                if (proj_import(&h, &z, sizeof z)) {
                    loaded++;
                    okz &= proj_ok(&h);
                }
            }
            bad += check("FUNC: 20000 damaged sections (hash right): refused or loaded valid, nothing read past", okz && loaded < 20000u);
        }
        fs.raw[100] ^= 1u;
        bad += check("FUNC: a byte changed: the hash refuses it", !proj_import(&g, &fs, sizeof fs));
        for (k = 0, n = 1; k < NTRK; k++)               /* every step of every track a chord: too much for one slot */
            for (i = 0; i < NSTEP; i++) {
                f.t[k].step[i].time = ST_NOTE; f.t[k].step[i].n = 4; f.t[k].step[i].hit = 3; f.t[k].step[i].acc = 1;
                step_set_chance(&f.t[k].step[i], 50u); step_set_ratchet(&f.t[k].step[i], 4u);
            }
        f.sum = proj_sum(&f);
        ok = !proj_pack(&fs, &f) && proj_full;
        f.t[0].step[0].n = 9;                           /* (a value out of range: not for room) */
        ok &= !proj_pack(&fs, &f) && !proj_full;
        bad += check("FUNC: six full tracks do not fit: not packed, proj_full (SECTION FULL); a bad value: not proj_full", ok && n);
    }
    printf("%s\n", bad ? "PROJECT FORMAT TEST FAILED" : "project format test passed");
    return bad != 0;
}
