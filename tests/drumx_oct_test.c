/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* (JIANT 0.8.5) DRUM-X OCTAVES (eng_drum.c DXO_*, seq.c dxo_*): the kit in every octave of the keys, on the real
 * sequencer, MIDI, project and UI sources (with the stubs of tests/ui_test.c):
 *   CENTRE     OCT 0: the keys' notes and hits as before (key 7 the kick, 36), no ratchet, no TOUCH, a hit bit for bit
 *              the one a plain note-on renders; recorded: the hit alone.
 *   UP         OCT +1 / +2 / +3: the same sound ratcheted x2 / x4 / x8 a step while held, stopped (from the press, a
 *              step / n apart) and playing (on the sequencer's RATCH grid); the release ends it.
 *   DOWN       OCT -1 SHORT (half the length, its level), -2 GHOST (short, darker, under 55 % of the level), -3 FLAM (a
 *              soft grace hit, the hit itself ~20 ms later at its level): measured on the engine's output.
 *   REC        real-time recording, playing: an upper octave writes the hit and the step's RATCH x2 / x4 (x8: ECHO x8
 *              EVEN), in each step it is held into (an early release puts the last one back); the pass after plays the
 *              very hits that were heard (each at the same sample, to the block); a lower one writes the hit and the
 *              step's TOUCH, played back with it (the same length and level as live); MIDI the same; FUNC round trip.
 *   MIDI       35..81 the GM map as before; 82.. ratchet bands, ..34 TOUCH bands (by pitch class), note-offs end them.
 *   UI         OCT on a DRUM track says what the octave does; SEQ > COMP shows a recorded TOUCH step, ECHO's EVEN kept
 *              by its knob.
 * Built and run by tests/run_tests.sh. */
#define UI_TEST_NO_MAIN 1
#define PROJ_FUNB_WRITER 1                               /* (an older format to load: FUNB, as JIANT 0.2 .. 0.5 wrote it) */
#include "ui_test.c"

#define KICK_KEY 7u                                     /* the first C: the kick (36) at OCT 0 */
static track_t *dt;

static track_t *drum_setup(void)
{
    uint32_t i;
    ui_power_on();
    for (i = 0; i < NTRK; i++) {
        track_defaults_steps(&trk[i]);
        if (i != 3u)
            trk[i].p[P_MUTE] = 1;
    }
    dt = &trk[3];
    set_engine_of(dt, ENGI_DRUM);
    dt->engine = dt->eng_req;
    song.sel = 3;
    song.octave = 0;
    song.rec = 0;
    song.playing = 0;
    dt->p[P_SLEN] = 16;
    dt->p[P_SSWING] = 0;
    song.g[G_SWING] = 0;
    song.g[G_ROUTE] = 0;
    memset(dxo_n, 0, sizeof dxo_n);
    kb_prev = fm1_in.notes = 0;
    events_block(CTL);                                  /* (the sound load's panic, as the ISR takes it) */
    return dt;
}
static uint32_t step_len(uint32_t idx) { return step_samples(dt, div_samples((uint32_t)dt->p[P_SDIV]), idx); }
static uint32_t now_s;                                  /* samples since the test's clock started */
/* nb blocks of the ISR's events (keys, MIDI, the sequencer, the held octaves); at[]: the sample of each hit */
static uint32_t run_b(uint32_t nb, uint32_t *at, uint32_t nat, uint32_t *nh)
{
    uint32_t b, v0;
    static int32_t o[2u * CTL];
    for (b = 0; b < nb; b++) {
        v0 = vage;
        mix_block(o, CTL);                              /* (0.9.9 ctl64: the parts rendered too: a hit inside the block
                                                         * starts its lane in the render, voice.c track_render) */
        for (; v0 < vage; v0++) {
            if (at && *nh < nat)
                at[*nh] = now_s;
            (*nh)++;
        }
        now_s += CTL;
    }
    return *nh;
}
static void key_dn(uint32_t k) { fm1_in.notes |= 1u << k; }
static void key_upk(uint32_t k) { fm1_in.notes &= ~(1u << k); }
static const drum_lane_t *lane(uint32_t l) { return &drum_kit[3][l]; }
static uint32_t lane_modes_but(const step_t *s, uint32_t l)   /* the lanes other than l with a mode (a mask) */
{
    uint32_t m = 0, k;
    for (k = 0; k < NLANE; k++)
        if (k != l && step_lmode(s, k))
            m |= 1u << k;
    return m;
}

/* ---------------------------------------------------------------- audio --- */
/* the engine's own output of the hit on lane l (drum_render at full amplitude: no mix, no sends, no tails) */
#define AN (FS * 2u)
static int32_t abuf[AN];
static void eng_render(uint32_t l, uint32_t n)
{
    voice_t *v = 0;
    vmod_t m;
    uint32_t i, k;
    memset(&m, 0, sizeof m);
    for (i = 0; i < NVOICE; i++)
        if (dt->v[i].active && drum_lane_of(dt, &dt->v[i]) == &drum_kit[3][l])
            v = &dt->v[i];
    memset(abuf, 0, sizeof abuf);
    if (v)
        m.amp1 = v->vel * 258;                          /* (the voice's level: voice.c, the ADSR held at full) */
    for (k = 0; v && k + CTL <= n; k += CTL) {
        int32_t o[CTL];
        memset(o, 0, sizeof o);
        m.amp0 = m.amp1;
        m.amp1 = v->vel * 258;
        drum_render(dt, v, o, CTL, &m);
        for (i = 0; i < CTL; i++)
            abuf[k + i] = o[i];
    }
}
static void quiet_kit(void)                             /* every hit ended, the lanes from rest */
{
    uint32_t i;
    for (i = 0; i < NVOICE; i++)
        dt->v[i].active = 0;
    memset(drum_kit, 0, sizeof drum_kit);
}
/* the hit's length: the last 5 ms window above -40 dB of the loudest one */
static uint32_t hit_len(uint32_t n)
{
    double pk = 0, w;
    uint32_t i, j, last = 0, win = FS / 200u;
    for (i = 0; i + win <= n; i += win) {
        for (w = 0, j = 0; j < win; j++)
            w += (double)abuf[i + j] * abuf[i + j];
        w = sqrt(w / win);
        if (w > pk) pk = w;
    }
    for (i = 0; i + win <= n; i += win) {
        for (w = 0, j = 0; j < win; j++)
            w += (double)abuf[i + j] * abuf[i + j];
        if (sqrt(w / win) > pk * 0.01)
            last = i + win;
    }
    return last;
}
/* brightness of its first 30 ms: the first difference's energy over the signal's */
static double bright(void)
{
    double a = 0, d = 0;
    uint32_t i;
    for (i = 1; i < FS * 30u / 1000u; i++) {
        double x = abuf[i], y = (double)abuf[i] - abuf[i - 1];
        a += x * x;
        d += y * y;
    }
    return a > 0 ? d / a : 0;
}
static void strike_dmp(uint32_t note, uint32_t dmp)
{
    quiet_kit();
    dx_in_tch = (uint8_t)dmp;
    trk_note_on(dt, note, 100);
    dx_in_tch = 0;
    trk_note_off(dt, note);
    eng_render(drum_lane(note), AN);
}

static int centre(void)
{
    int bad = 0, ok = 1;
    uint32_t nh = 0, k;
    static int32_t a[FS / 2];
    drum_setup();
    for (k = 0; k < 27u; k++)
        for (song.octave = -3; song.octave <= 3; song.octave++)
            ok &= kb_map(dt, k) == 29u + k;
    song.octave = 0;
    bad += check("DRUM keys: every octave the same notes (key 7 the kick, 36)", ok);
    key_dn(KICK_KEY);
    run_b(FS / CTL, 0, 0, &nh);
    key_upk(KICK_KEY);
    run_b(4, 0, 0, &nh);
    bad += check("OCT 0: one hit for a held key, nothing held over (no ratchet), no TOUCH",
                 nh == 1u && !dxo_n[3] && lane(DV_KICK)->dm == 0u);
    /* the same hit through the keys and through a plain note-on: bit for bit */
    quiet_kit();
    key_dn(KICK_KEY);
    events_block(CTL);
    key_upk(KICK_KEY);
    eng_render(DV_KICK, FS / 2u);
    memcpy(a, abuf, sizeof a);
    events_block(CTL);
    quiet_kit();
    trk_note_on(dt, 36, 100);
    trk_note_off(dt, 36);
    eng_render(DV_KICK, FS / 2u);
    bad += check("OCT 0: a key's kick renders bit for bit as a plain note-on 36", !memcmp(a, abuf, sizeof a));
    /* recording at OCT 0: the hit, nothing else */
    drum_setup();
    song.rec = 1u << 3;
    seq_start();
    nh = 0;
    run_b(1, 0, 0, &nh);
    key_dn(KICK_KEY);
    run_b(2, 0, 0, &nh);
    key_upk(KICK_KEY);
    run_b(2, 0, 0, &nh);
    ok = dt->step[0].hit == 1u << DV_KICK && step_ratchet(&dt->step[0]) == 1u && !dt->step[0].comp && !dt->step[0].comp2;
    seq_stop();
    bad += check("REC at OCT 0: the hit alone (x1, no component), as before", ok);
    return bad;
}

/* hits in about `steps` steps of the track's DIV with the key held at OCT o, transport stopped */
static uint32_t held_stopped(int32_t o, uint32_t steps, uint32_t *at)
{
    uint32_t nh = 0, nb = steps * div_samples((uint32_t)dt->p[P_SDIV]) / CTL - 1u;
    drum_setup();
    song.octave = o;
    now_s = 0;
    key_dn(KICK_KEY);
    run_b(nb, at, 64, &nh);
    key_upk(KICK_KEY);
    return nh;
}
static int up(void)
{
    int bad = 0, ok = 1;
    uint32_t at[64], nh, k, per, o;
    static const uint8_t R[4] = {1, 2, 4, 8};
    for (o = 1; o <= 3u; o++) {
        nh = held_stopped((int32_t)o, 4, at);
        per = div_samples((uint32_t)dt->p[P_SDIV]) / R[o];
        ok &= nh == 4u * R[o] && dxo_n[3] == 1u;
        for (k = 1; k < nh && k < 64u; k++) {
            int32_t d = (int32_t)(at[k] - at[0]) - (int32_t)(k * per);   /* (in the block holding its sample) */
            ok &= d > -(int32_t)CTL && d <= 0;
        }
        for (k = 0; k < NVOICE; k++)
            ok &= !dt->v[k].active || dt->v[k].note == 36u;
        nh = 0;
        run_b(div_samples((uint32_t)dt->p[P_SDIV]) * 2u / CTL, 0, 0, &nh);
        ok &= nh == 0u && !dxo_n[3];
        if (!ok) printf("ui:   OCT +%u stopped: %u hits in 4 steps\n", o, held_stopped((int32_t)o, 4, at));
    }
    bad += check("OCT +1 / +2 / +3 stopped: x2 / x4 / x8 a step from the press, the kick; the release ends it", ok);
    /* playing: on the sequencer's grid (a part every step / n from each step's start) */
    ok = 1;
    for (o = 1; o <= 3u; o++) {
        uint32_t s0, i;
        drum_setup();
        song.octave = (int32_t)o;
        seq_start();
        now_s = 0;
        nh = 0;
        run_b(1, 0, 0, &nh);                             /* (step 1 begins) */
        s0 = now_s - CTL;
        key_dn(KICK_KEY);
        nh = 0;
        run_b((step_len(0) * 4u) / CTL - 1u, at, 64, &nh);
        key_upk(KICK_KEY);
        ok &= nh == 4u * R[o];
        for (i = 1; i < nh && i < 64u; i++) {           /* hit i: part i % n of step i / n */
            uint32_t want = s0 + (i / R[o]) * step_len(0) + (i % R[o]) * (step_len(0) / R[o]);
            int32_t d = (int32_t)at[i] - (int32_t)want;
            ok &= d >= 0 && d < (int32_t)CTL;
        }
        seq_stop();
    }
    bad += check("OCT +1 / +2 / +3 playing: n hits a step, each on the RATCH grid of the step (to the block)", ok);
    return bad;
}

static double rms_of(uint32_t a, uint32_t b)
{
    double w = 0;
    uint32_t i;
    for (i = a; i < b; i++)
        w += (double)abuf[i] * abuf[i];
    return sqrt(w / (b - a));
}
static int32_t peak(uint32_t a, uint32_t b)
{
    int32_t p = 0;
    uint32_t i;
    for (i = a; i < b; i++)
        p = abs(abuf[i]) > p ? abs(abuf[i]) : p;
    return p;
}
static int down(void)
{
    int bad = 0, ok = 1, okg = 1, okf = 1;
    uint32_t len[4], d, l, nh;
    int32_t pk[4];
    uint32_t ms = FS / 1000u, at = DXO_FLAM * CTL;
    double br[4], r0 = 0, g0, g1;
    static const uint8_t NOTE[3] = {36, 38, 42};       /* kick, snare, closed hat */
    drum_setup();
    for (l = 0; l < 3u; l++) {
        for (d = 0; d < 4u; d++) {
            strike_dmp(NOTE[l], d);
            len[d] = hit_len(AN);
            br[d] = bright();
            pk[d] = peak(0, AN);
            if (!d)
                r0 = rms_of(0, 5u * ms);
        }
        ok &= len[1] < len[0] * 3u / 4u && len[1] > len[0] / 5u && pk[1] * 10 > pk[0] * 8;
        okg &= len[2] < len[0] * 3u / 4u && pk[2] * 100 < pk[0] * 55 && br[2] < br[1] * 0.97;
        strike_dmp(NOTE[l], 3);                         /* FLAM: the grace hit, the hit ~20 ms on */
        g0 = rms_of(0, 5u * ms);                        /* (the first 5 ms of each, RMS) */
        g1 = rms_of(at, at + 5u * ms);
        okf &= g0 < g1 * 0.65 && g1 > r0 * 0.65 && hit_len(AN) > len[0] && at > 18u * ms && at < 23u * ms;
        printf("ui:   note %u: length %u %u %u %u samples, peak %d %d %d, brightness %.4f %.4f %.4f; FLAM grace %.0f, hit %.0f (plain %.0f)\n",
               NOTE[l], len[0], len[1], len[2], len[3], pk[0], pk[1], pk[2], br[0], br[1], br[2], g0, g1, r0);
    }
    bad += check("TOUCH SHORT: under 3/4 of the length, the same level, every lane", ok);
    bad += check("TOUCH GHOST: short, under 55 % of the level, darker", okg);
    bad += check("TOUCH FLAM: a grace hit under 65 % of the hit, the hit ~20 ms later (a retrigger)", okf);
    /* the keys: OCT -1 / -2 / -3 strike with TOUCH 1 / 2 / 3, nothing held over */
    ok = 1;
    for (d = 1; d <= 3u; d++) {
        drum_setup();
        song.octave = -(int32_t)d;
        nh = 0;
        key_dn(KICK_KEY);
        run_b(FS / 2u / CTL, 0, 0, &nh);
        key_upk(KICK_KEY);
        run_b(2, 0, 0, &nh);
        ok &= nh == 1u && lane(DV_KICK)->dm == d && !dxo_n[3] && !dx_in_tch;
    }
    bad += check("OCT -1 / -2 / -3: one hit, TOUCH SHORT / GHOST / FLAM on it", ok);
    return bad;
}

/* real-time recording, upper octave: held over steps 2..4, what plays back the pass after is what was heard */
static int rec_up(void)
{
    int bad = 0, ok = 1;
    uint32_t live[32], back[32], nl = 0, nb = 0, k, s2, loop, o;
    static const uint8_t R[4] = {1, 2, 4, 8};
    for (o = 1; o <= 3u; o++) {
        drum_setup();
        song.octave = (int32_t)o;
        song.rec = 1u << 3;
        seq_start();
        now_s = 0;
        nl = nb = 0;
        while (!(dt->seq_idx == 2u && dt->seq_pos < CTL))  /* to step 3's first block (index 2) */
            run_b(1, 0, 0, &nb);
        s2 = now_s - CTL;
        nb = 0;
        key_dn(KICK_KEY);
        while (!(dt->seq_idx == 4u && dt->seq_pos + 2u * CTL > step_len(4)))   /* held to the end of step 5 */
            run_b(1, live, 32, &nl);
        key_upk(KICK_KEY);
        while (dt->seq_idx != 1u)                         /* the rest of the pass */
            run_b(1, 0, 0, &nb);
        loop = 0;
        for (k = 0; k < 16u; k++)
            loop += step_len(k);
        for (k = 0; k < 16u; k++) {
            const step_t *s = &dt->step[k];
            int has = k >= 2u && k <= 4u;
            ok &= has ? s->time == ST_NOTE && s->hit == 1u << DV_KICK && lm_hits(step_lmode(s, DV_KICK)) == R[o] &&
                            !s->comp && step_ratchet(s) == 1u && lane_modes_but(s, DV_KICK) == 0u
                      : s->time != ST_NOTE || (!s->hit && !s->n);
        }
        nb = 0;
        while (!(dt->seq_idx == 5u))                      /* the next pass, steps 3..5 */
            run_b(1, back, 32, &nb);
        ok &= nl == 3u * R[o] && nb == nl;
        for (k = 0; k < nl && k < nb && k < 32u; k++) {
            int32_t d = (int32_t)(back[k] - loop) - (int32_t)live[k];   /* (the press: a block late, as heard) */
            ok &= d >= -(int32_t)CTL && d <= (int32_t)CTL;
        }
        (void)s2;
        if (!ok) printf("ui:   REC OCT +%u: %u hits live, %u played back\n", o, nl, nb);
        seq_stop();
    }
    bad += check("REC OCT +1 / +2 / +3: the kick and the KICK lane's x2 / x4 / x8 in each step held (the step x1); played back as heard", ok);
    /* an early release in a step it was held into puts that step back */
    drum_setup();
    song.octave = 1;
    song.rec = 1u << 3;
    seq_start();
    nb = 0;
    while (!(dt->seq_idx == 2u && dt->seq_pos < CTL))
        run_b(1, 0, 0, &nb);
    key_dn(KICK_KEY);
    while (!(dt->seq_idx == 3u && dt->seq_pos >= CTL))
        run_b(1, 0, 0, &nb);
    key_upk(KICK_KEY);
    run_b(2, 0, 0, &nb);
    ok = step_lmode(&dt->step[2], DV_KICK) == LM_X2 && dt->step[2].hit == 1u << DV_KICK && !dt->step[3].hit &&
         !step_lm_any(&dt->step[3]) && !dxo_n[3];
    seq_stop();
    bad += check("  let go early in the next step: that step back as it was (one step recorded)", ok);
    /* late in a step: into the next one (the sequencer plays its later parts, the live ones stop) */
    drum_setup();
    song.octave = 1;
    song.rec = 1u << 3;
    seq_start();
    nb = 0;
    while (!(dt->seq_idx == 6u && dt->seq_pos > step_len(6) * 3u / 4u))
        run_b(1, 0, 0, &nb);
    nl = 0;
    key_dn(KICK_KEY);
    while (!(dt->seq_idx == 7u && dt->seq_pos + 2u * CTL > step_len(7)))
        run_b(1, 0, 0, &nl);
    key_upk(KICK_KEY);
    run_b(4, 0, 0, &nl);
    ok = dt->step[7].hit == 1u << DV_KICK && step_lmode(&dt->step[7], DV_KICK) == LM_X2 && !dt->step[6].hit && !dt->step[8].hit &&
         nl == 2u;
    seq_stop();
    bad += check("  pressed late in a step: into the next (x2), 2 hits heard, as it plays back", ok);
    return bad;
}

static int rec_down(void)
{
    int bad = 0, ok = 1;
    uint32_t d, nb, len0, len1;
    int32_t pk0;
    for (d = 1; d <= 3u; d++) {
        drum_setup();
        song.octave = -(int32_t)d;
        song.rec = 1u << 3;
        seq_start();
        nb = 0;
        while (!(dt->seq_idx == 6u && dt->seq_pos < CTL))
            run_b(1, 0, 0, &nb);
        key_dn(KICK_KEY - 2u);                          /* A#: 34, the open hat's octave below (GM 46) */
        run_b(3, 0, 0, &nb);
        key_upk(KICK_KEY - 2u);
        key_dn(KICK_KEY);
        run_b(2, 0, 0, &nb);
        key_upk(KICK_KEY);
        run_b(2, 0, 0, &nb);
        ok &= lane(DV_KICK)->dm == d;
        {
            const step_t *s = &dt->step[6];
            ok &= s->hit == 1u << DV_KICK && s->n == 1u && s->note[0] == 34u && !s->comp && step_ratchet(s) == 1u &&
                  step_lmode(s, DV_KICK) == LM_SHORT + d - 1u && step_lmode(s, DV_HATO) == LM_SHORT + d - 1u &&
                  lane_modes_but(s, DV_KICK) == (1u << DV_HATO);
        }
        song.octave = 0;
        drum_kit[3][DV_KICK].dm = drum_kit[3][DV_HATO].dm = 0;
        while (dt->seq_idx != 7u)                         /* step 7 again, the next pass: TOUCH d on its hits */
            run_b(1, 0, 0, &nb);
        while (dt->seq_idx != 6u)
            run_b(1, 0, 0, &nb);
        ok &= lane(DV_KICK)->dm == d && lane(DV_HATO)->dm == d && dt->dtch == 0u;
        seq_stop();
    }
    bad += check("REC OCT -1 / -2 / -3: each lane's SHORT / GHOST / FLAM (a note on its lane too), played back with it", ok);
    /* played back, the same sound: the live hit and the sequenced one, the same length */
    drum_setup();
    strike_dmp(38, 2);
    len0 = hit_len(AN);
    pk0 = peak(0, AN);
    quiet_kit();
    dt->step[0] = (step_t){{0}, 0, ST_NOTE, 0, 100, 1u << DV_SNARE};
    step_set_lmode(&dt->step[0], DV_SNARE, LM_GHOST);
    dt->p[P_SLEN] = 64;
    seq_start();
    nb = 0;
    run_b(2, 0, 0, &nb);
    eng_render(DV_SNARE, AN);
    seq_stop();
    len1 = hit_len(AN);
    ok = nb == 1u && len1 + FS / 100u > len0 && len1 < len0 + FS / 100u && lane(DV_SNARE)->dm == 2u &&
         peak(0, AN) * 10 > pk0 * 9 && peak(0, AN) * 10 < pk0 * 11;
    bad += check("  a GHOST step plays the snare as the live OCT -2 hit (its length within 10 ms, its level 10 %)", ok);
    return bad;
}

static uint32_t mev(uint32_t st, uint32_t note, uint32_t vel)   /* a MIDI event on channel 4 (track 4): its hits */
{
    uint32_t v0 = vage;
    midi_event(st, 3, note, vel);
    return vage - v0;
}
static uint32_t cnt[128];                               /* note-ons by note over run_c */
static void run_c(uint32_t nb)
{
    uint32_t b, i, a0;
    for (b = 0; b < nb; b++) {
        a0 = vage;
        events_block(CTL);
        for (i = 0; i < NVOICE; i++)
            if (dt->v[i].active && dt->v[i].age > a0)
                cnt[dt->v[i].note & 127u]++;
        now_s += CTL;
    }
}
/* (0.8.5) per sound: each lane's mode on its own hit only */
static int per_lane(void)
{
    int bad = 0, ok;
    uint32_t nb;
    drum_setup();
    dt->p[P_SLEN] = 1;                                  /* a one-step loop: the kick plain, the closed hat x4, the snare GHOST */
    dt->step[0] = (step_t){{0}, 0, ST_NOTE, 0, 100, 1u << DV_KICK | 1u << DV_HATC | 1u << DV_SNARE};
    step_set_lmode(&dt->step[0], DV_HATC, LM_X4);
    step_set_lmode(&dt->step[0], DV_SNARE, LM_GHOST);
    seq_start();
    memset(cnt, 0, sizeof cnt);
    run_c(1);
    ok = lane(DV_KICK)->dm == 0u && lane(DV_SNARE)->dm == 2u && lane(DV_HATC)->dm == 0u;
    run_c(step_len(0) / CTL - 1u);                      /* (the rest of the pass) */
    ok &= cnt[36] == 1u && cnt[38] == 1u && cnt[42] == 4u;
    run_c(step_len(0) * 3u / CTL);                      /* three more passes */
    ok &= cnt[36] == 4u && cnt[38] == 4u && cnt[42] == 16u && lane(DV_KICK)->dm == 0u && lane(DV_SNARE)->dm == 2u;
    if (!ok) printf("ui:   per lane: kick %u snare %u hat %u\n", cnt[36], cnt[38], cnt[42]);
    seq_stop();
    bad += check("per sound: kick plain + closed hat x4 + snare GHOST on one step: 1 + 4 + 1 a pass, the kick not damped", ok);
    /* the step's own RATCH x2 still repeats it all, the hat's x4 on top of the first part */
    drum_setup();
    dt->p[P_SLEN] = 1;
    dt->step[0] = (step_t){{0}, 0, ST_NOTE, 0, 100, 1u << DV_KICK};
    step_set_ratchet(&dt->step[0], 2);
    seq_start();
    memset(cnt, 0, sizeof cnt);
    run_c(step_len(0) / CTL);
    seq_stop();
    bad += check("  a step's RATCH x2 as before (lanes without a mode)", cnt[36] == 2u);
    /* recorded live: the kick at OCT 0 and the closed hat at OCT +2 into the same step, then the snare at OCT -2 */
    drum_setup();
    song.rec = 1u << 3;
    seq_start();
    nb = 0;
    while (!(dt->seq_idx == 4u && dt->seq_pos < CTL))
        run_b(1, 0, 0, &nb);
    key_dn(KICK_KEY);
    run_b(2, 0, 0, &nb);
    key_upk(KICK_KEY);
    song.octave = 2;
    key_dn(KICK_KEY + 6u);                              /* F#: the closed hat */
    run_b(2, 0, 0, &nb);
    key_upk(KICK_KEY + 6u);
    song.octave = -2;
    key_dn(KICK_KEY + 2u);                              /* D: the snare */
    run_b(2, 0, 0, &nb);
    key_upk(KICK_KEY + 2u);
    song.octave = 0;
    run_b(2, 0, 0, &nb);
    {
        const step_t *s = &dt->step[4];
        ok = s->hit == (1u << DV_KICK | 1u << DV_HATC | 1u << DV_SNARE) && step_lmode(s, DV_KICK) == LM_NONE &&
             step_lmode(s, DV_HATC) == LM_X4 && step_lmode(s, DV_SNARE) == LM_GHOST && step_ratchet(s) == 1u && !s->comp;
    }
    while (dt->seq_idx != 3u)                           /* the next pass: step 5 plays them each its own way */
        run_b(1, 0, 0, &nb);
    memset(cnt, 0, sizeof cnt);
    while (dt->seq_idx != 5u)
        run_c(1);
    ok &= cnt[36] == 1u && cnt[42] == 4u && cnt[38] == 1u && lane(DV_KICK)->dm == 0u && lane(DV_SNARE)->dm == 2u;
    seq_stop();
    bad += check("REC per sound: kick OCT 0, hat OCT +2, snare OCT -2 on one step: the kick 1, the hat x4, the snare GHOST", ok);
    /* the grid: KNOB 3 HIT goes -- ON x2 x4 x8 SHORT GHOST FLAM on the selected lane */
    drum_setup();
    go_page(GR_ROLL);
    frame();
    ui.lane = DV_HATC;
    cursor_set(2);
    {
        uint32_t k;
        ok = grid_on() && grid_hit_of(&dt->step[2], DV_HATC) == 0u;
        for (k = 1; k <= LM_N; k++) {
            grid_edit(2, 1);
            ok &= grid_hit_of(&dt->step[2], DV_HATC) == k && ((dt->step[2].hit >> DV_HATC) & 1u) &&
                  step_lmode(&dt->step[2], DV_HATC) == k - 1u;
        }
        grid_edit(2, 1);
        ok &= step_lmode(&dt->step[2], DV_HATC) == LM_FLAM;
        frame();
        ui.lane = DV_KICK;
        grid_edit(2, 1);
        ok &= step_lmode(&dt->step[2], DV_KICK) == LM_NONE && step_lmode(&dt->step[2], DV_HATC) == LM_FLAM;
        ui.lane = DV_HATC;
        for (k = 0; k < LM_N; k++)
            grid_edit(2, -1);
        ok &= !((dt->step[2].hit >> DV_HATC) & 1u) && step_lmode(&dt->step[2], DV_HATC) == LM_NONE &&
              ((dt->step[2].hit >> DV_KICK) & 1u);
    }
    bad += check("grid KNOB 3 HIT: -- ON x2 x4 x8 SHORT GHOST FLAM, that lane only; off clears its mode", ok);
    /* projects: none of these: the steps packed as 0.8.4 packed them (x bit 6 unset); with one, bit 6 and three bytes */
    {
        project_t before, after;
        project_store_t a, b;
        drum_setup();
        dt->step[1] = (step_t){{0}, 0, ST_NOTE, 0, 100, 1u << DV_KICK};
        project_capture(&before);
        ok = proj_pack(&a, &before) && proj_step_x(&dt->step[1], &nb) == 1u;
        step_set_lmode(&dt->step[1], DV_KICK, LM_FLAM);
        project_capture(&before);
        ok &= proj_pack(&b, &before) && (proj_step_x(&dt->step[1], &nb) & 64u) && proj_step_size(&dt->step[1]) == 2u + 1u + 2u + 3u &&
              proj_import(&after, &b, sizeof b) && step_lmode(&after.t[3].step[1], DV_KICK) == LM_FLAM;
        dt->step[1].lm[0] = 7u;                         /* (a 7 reads as none) */
        project_capture(&before);
        ok &= proj_pack(&b, &before) && proj_import(&after, &b, sizeof b) && step_lmode(&after.t[3].step[1], DV_KICK) == LM_NONE;
        bad += check("FUNC: a step without lane modes as before; with them x bit 6 + 3 bytes, round trip; a 7 none", ok);
        dt = &trk[3];
        trk[0].step[3] = (step_t){{36}, 1, ST_NOTE, 0, 100, 1u << DV_SNARE};
        project_capture(&before);
        proj_pack_vb(&a, &before);
        ok = proj_import(&after, &a, sizeof a) && after.t[0].step[3].hit == 1u << DV_SNARE && !step_lm_any(&after.t[0].step[3]);
        for (nb = 0; nb < NSTEP; nb++)
            ok &= !step_lm_any(&after.t[0].step[nb]) && !step_lm_any(&after.t[3].step[nb]);
        bad += check("  an older project (FUNB) loads with no lane modes, its hits as they were", ok);
    }
    return bad;
}
static int midi(void)
{
    int bad = 0, ok = 1;
    uint32_t nb, i;
    static const uint8_t GM[6] = {35, 36, 42, 49, 56, 81};
    drum_setup();
    for (i = 0; i < 6u; i++) {
        nb = mev(0x90, GM[i], 100);
        run_b(FS / 4u / CTL, 0, 0, &nb);
        ok &= nb == 1u && !dxo_n[3] && lane(drum_lane(GM[i]))->dm == 0u;
        mev(0x80, GM[i], 0);
        run_b(1, 0, 0, &nb);
    }
    bad += check("MIDI 35..81: the GM map as before, one hit, no ratchet, no TOUCH", ok);
    ok = drum_oct_of(34) == -1 && drum_oct_of(23) == -1 && drum_oct_of(22) == -2 && drum_oct_of(11) == -2 &&
         drum_oct_of(10) == -3 && drum_oct_of(0) == -3 && drum_oct_of(82) == 1 && drum_oct_of(93) == 1 &&
         drum_oct_of(94) == 2 && drum_oct_of(105) == 2 && drum_oct_of(106) == 3 && drum_oct_of(127) == 3;
    for (i = 35; i <= 81u; i++)
        ok &= drum_oct_of(i) == 0;
    ok &= drum_oct_base(84) == 36u && drum_oct_base(24) == 36u && drum_oct_base(90) == 42u && drum_oct_base(0) == 36u;
    for (i = 0; i < 128u; i++)                          /* MIDI OUT's band note comes back as the same sound and octave */
        if (i >= 36u && i <= 47u)
            ok &= drum_oct_of(drum_oct_note(i, 1)) == 1 && drum_oct_base(drum_oct_note(i, 1)) == i &&
                  drum_oct_of(drum_oct_note(i, 3)) == 3 && drum_oct_base(drum_oct_note(i, -1)) == i &&
                  drum_oct_of(drum_oct_note(i, -1)) == -1 && drum_oct_of(drum_oct_note(i, -2)) == -2;
    bad += check("MIDI bands: ..10 -3, 11..22 -2, 23..34 -1, 35..81 the GM map, 82..93 +1, 94..105 +2, 106.. +3", ok);
    ok = 1;
    {
        static const uint8_t UPN[3] = {84, 96, 108}, R[3] = {2, 4, 8}, DN[3] = {24, 12, 0};
        for (i = 0; i < 3u; i++) {
            drum_setup();
            now_s = 0;
            nb = mev(0x90, UPN[i], 100);
            run_b(4u * div_samples((uint32_t)dt->p[P_SDIV]) / CTL - 1u, 0, 0, &nb);
            ok &= nb == 4u * R[i] && dxo_n[3] == 1u;
            mev(0x80, UPN[i], 0);
            nb = 0;
            run_b(div_samples((uint32_t)dt->p[P_SDIV]) * 2u / CTL, 0, 0, &nb);
            ok &= nb == 0u && !dxo_n[3];
            nb += mev(0x90, DN[i], 100);
            run_b(2, 0, 0, &nb);
            mev(0x80, DN[i], 0);
            run_b(2, 0, 0, &nb);
            ok &= nb == 1u && lane(DV_KICK)->dm == i + 1u;
        }
    }
    bad += check("MIDI 84 / 96 / 108: the kick x2 / x4 / x8 while held; 24 / 12 / 0: the kick SHORT / GHOST / FLAM", ok);
    /* MIDI recording */
    drum_setup();
    song.rec = 1u << 3;
    seq_start();
    nb = 0;
    while (!(dt->seq_idx == 3u && dt->seq_pos < CTL))
        run_b(1, 0, 0, &nb);
    mev(0x90, 90, 100);                       /* F#: the closed hat, +1 */
    run_b(4, 0, 0, &nb);
    mev(0x80, 90, 0);
    while (!(dt->seq_idx == 5u && dt->seq_pos < CTL))
        run_b(1, 0, 0, &nb);
    mev(0x90, 26, 100);                       /* D: the snare, -1 */
    run_b(2, 0, 0, &nb);
    mev(0x80, 26, 0);
    run_b(2, 0, 0, &nb);
    seq_stop();
    ok = dt->step[3].hit == 1u << DV_HATC && step_lmode(&dt->step[3], DV_HATC) == LM_X2 && !dt->step[4].hit &&
         dt->step[5].hit == 1u << DV_SNARE && step_lmode(&dt->step[5], DV_SNARE) == LM_SHORT && !dt->step[5].comp;
    bad += check("MIDI REC: 90 the closed hat x2, 26 the snare SHORT, each into its step", ok);
    {   /* FUNC round trip */
        project_t before, after;
        project_store_t packed;
        project_capture(&before);
        ok = proj_pack(&packed, &before) && proj_import(&after, &packed, sizeof packed) &&
             step_lmode(&after.t[3].step[3], DV_HATC) == LM_X2 && step_lmode(&after.t[3].step[5], DV_SNARE) == LM_SHORT &&
             after.t[3].step[5].hit == 1u << DV_SNARE && !memcmp(&before, &after, sizeof before);
        bad += check("  the recorded steps (the hat x2, the snare SHORT) through a project save and load, every byte", ok);
    }
    return bad;
}

static int ui_part(void)
{
    int bad = 0, ok;
    drum_setup();
    frame();
    press(B_OCTUP);
    ok = song.octave == 1 && msg_is("RATCHET x2");
    press(B_OCTUP);
    ok &= song.octave == 2 && msg_is("RATCHET x4");
    press(B_OCTDN);
    press(B_OCTDN);
    press(B_OCTDN);
    ok &= song.octave == -1 && msg_is("SHORT");
    press(B_OCTDN);
    ok &= song.octave == -2 && msg_is("GHOST");
    press(B_OCTDN);
    ok &= song.octave == -3 && msg_is("FLAM");
    song.octave = 2;
    press(B_OCTUP);
    ok &= song.octave == 3 && msg_is("RATCHET x8");
    bad += check("OCT on a DRUM track: RATCHET x2 / x4 / x8, SHORT, GHOST, FLAM", ok);
    song.octave = 0;
    dt->step[2] = (step_t){{0}, 0, ST_NOTE, 0, 100, 1u << DV_KICK};
    dt->step[2].comp = CP_TOUCH;
    dt->step[2].carg = 2;
    go_page(GR_COMP);
    cursor_set(2);
    frame();
    ok = comp_view() == COMP_TOUCH;
    bad += check("SEQ > COMP: a TOUCH step shows TOUCH", ok);
    dt->step[3] = (step_t){{0}, 0, ST_NOTE, 0, 100, 1u << DV_KICK};
    dt->step[3].comp = CP_ECHO;
    dt->step[3].carg = CP_ECHO_EVEN | 6u;
    cursor_set(3);
    frame();
    comp_knob(2, -1);
    ok = comp_view() == COMP_ECHO && dt->step[3].carg == (CP_ECHO_EVEN | 5u);
    {
        uint32_t dv;
        comp_knob(2, 1);
        ok &= dt->step[3].carg == (CP_ECHO_EVEN | 6u) && step_parts(dt, &dt->step[3], &dv) == 8u;
    }
    bad += check("SEQ > COMP: a recorded x8 shows ECHO x8 EVEN; KNOB 3 keeps EVEN", ok);
    return bad;
}

int main(void)
{
    int bad = 0;
    bad += centre();
    bad += up();
    bad += down();
    bad += rec_up();
    bad += rec_down();
    bad += midi();
    bad += per_lane();
    bad += ui_part();
    if (bad) {
        printf("drumx octaves: %d FAILED\n", bad);
        return 1;
    }
    printf("drumx octaves: all ok\n");
    return 0;
}
