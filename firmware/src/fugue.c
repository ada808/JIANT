/* SPDX-License-Identifier: GPL-3.0-only */
/* (JIANT 0.6.52) FUGUE (SEQ > FUGUE), after the Fugue Machine: three more playheads on each track, reading the same
 * steps as the sequencer, each with its RATE (OFF, 1/4 .. x4 of the track's), its DIRECTION (FWD, REV, PING, RND) and
 * its INTERVAL (-14 .. +14 degrees of the scale; a kit keeps its lanes): canons, counterpoint, phasing out of one
 * pattern. A head plays a step's notes (or hits) with the step's velocity and accent, the track's GATE and its own
 * chance roll; no components, locks or ratchets (those are the sequencer's). The settings are the section's (fg_set,
 * project.c FUNC 'F' 'G'); the heads start with the transport and stop with it. Cost: a few operations a block a head */
#define FG_HEADS 3u
#define FG_NRATE 10u
typedef struct {
    uint8_t rate, dir;                                   /* 0 OFF .. FG_NRATE - 1; FWD REV PING RND */
    int8_t ivl;                                          /* degrees, -14 .. 14 */
} fg_set_t;
typedef struct {
    uint32_t pos, off;                                   /* samples into its step; to its notes' end */
    uint16_t idx;                                        /* the step it is on */
    int8_t way;                                          /* PING's way: 1 / -1 */
    uint8_t n, note[4];                                  /* what it sounds */
} fg_head_t;
static fg_set_t fg_set[NTRK][FG_HEADS];
static fg_head_t fg_head[NTRK][FG_HEADS];
static const uint8_t FG_NUM[FG_NRATE] = {0, 1, 1, 1, 2, 1, 3, 2, 3, 4}, FG_DEN[FG_NRATE] = {1, 4, 3, 2, 3, 1, 2, 1, 1, 1};
static const char *const N_FGRATE[FG_NRATE] = {"OFF", "1/4", "1/3", "1/2", "2/3", "x1", "3/2", "x2", "x3", "x4"};
static const char *const N_FGDIR[4] = {"FWD", "REV", "PING", "RND"};
enum { FG_FWD, FG_REV, FG_PING, FG_RND };

static int fg_on(uint32_t ti)
{
    uint32_t h;
    for (h = 0; h < FG_HEADS; h++)
        if (fg_set[ti % NTRK][h].rate)
            return 1;
    return 0;
}
static void fg_release(track_t *t, fg_head_t *h)
{
    uint32_t i;
    for (i = 0; i < h->n; i++)
        seq_note_off(t, h->note[i]);
    h->n = 0;
}
/* the transport starts (seq_start) or stops (seq_stop): every head silent, back before its first step */
static void fg_reset(void)
{
    uint32_t ti, k;
    for (ti = 0; ti < NTRK; ti++)
        for (k = 0; k < FG_HEADS; k++) {
            fg_head_t *h = &fg_head[ti][k];
            fg_release(&trk[ti], h);
            h->pos = 0x7FFFFFFFu;                        /* (its first step on the first block) */
            h->idx = 0xFFFFu;
            h->way = 1;
        }
}
/* the next step a head reads, from idx (0xFFFF: before the first) */
static uint32_t fg_next(fg_head_t *h, uint32_t dir, uint32_t len)
{
    uint32_t i = h->idx;
    if (len < 2u)
        return 0;
    if (i == 0xFFFFu)
        return dir == FG_REV ? len - 1u : dir == FG_RND ? rng() % len : 0u;
    switch (dir) {
    case FG_REV: return (i + len - 1u) % len;
    case FG_PING:
        if ((int32_t)i + h->way < 0 || i + (uint32_t)(int32_t)h->way >= len)
            h->way = (int8_t)-h->way;
        return (uint32_t)((int32_t)i + h->way) % len;
    case FG_RND: return rng() % len;
    default: return (i + 1u) % len;
    }
}
/* a head plays step i of t */
static void fg_play(track_t *t, fg_head_t *h, const fg_set_t *f, uint32_t i, uint32_t steplen)
{
    const step_t *s = &seq_steps(t)[seq_src(t, i)];
    uint32_t k, vel, kit = (uint32_t)trk_kit(t), c;
    if (s->time == ST_TIE)
        return;                                          /* (a TIE: what it sounds goes on) */
    fg_release(t, h);
    if (s->time != ST_NOTE || (!s->n && !s->hit))
        return;
    c = trk_chance(t, s);
    if (c < 100u && rng() % 100u >= c)
        return;
    vel = (s->flags & SF_ACCENT) ? 127u : (s->vel ? s->vel : 96u);
    for (k = 0; k < s->n && k < 4u && !kit; k++) {
        int32_t x = s->note[k] + t->p[P_POFS];
        if (f->ivl)
            x = scale_steps(t, x, f->ivl);
        if (song.g[G_STRN])
            x = scale_steps(t, x, song.g[G_STRN]);
        h->note[h->n++] = (uint8_t)clamp(x, 0, 127);
    }
    for (k = 0; k < NLANE && h->n < 4u; k++)             /* (a kit: its lanes, as the step has them) */
        if ((s->hit >> k) & 1u)
            h->note[h->n++] = DRUM_LANE_NOTE[k];
    for (k = 0; k < h->n; k++)
        seq_note_on(t, h->note[k], vel);
    h->off = (uint32_t)((uint64_t)steplen * (uint32_t)clamp(t->p[P_SGATE], 1, 127) / 128u);
    if (h->off < CTL)
        h->off = CTL;
}
/* once a block a track (seq_tick, after the sequencer): each head on */
static __attribute__((noinline)) void fugue_tick(track_t *t, uint32_t n, uint32_t period, uint32_t len)
{
    uint32_t ti = (uint32_t)(t - trk) % NTRK, k;
    for (k = 0; k < FG_HEADS; k++) {
        const fg_set_t *f = &fg_set[ti][k];
        fg_head_t *h = &fg_head[ti][k];
        uint32_t steplen, guard;
        if (h->n) {
            if (h->off <= n) {                           /* (ctl64) the gate ends at its sample */
                ev_at(h->off);
                fg_release(t, h);
                ev_off = 0;
            } else
                h->off -= n;
        }
        if (!f->rate || f->rate >= FG_NRATE || !len)
            continue;
        steplen = (uint32_t)((uint64_t)period * FG_DEN[f->rate] / FG_NUM[f->rate]);
        if (steplen < CTL)
            steplen = CTL;
        if (h->pos == 0x7FFFFFFFu) {                     /* (the start: its first step now) */
            h->pos = 0;
            h->idx = (uint16_t)fg_next(h, f->dir, len);
            fg_play(t, h, f, h->idx, steplen);
            continue;
        }
        h->pos += n;
        for (guard = 0; h->pos >= steplen && guard < 4u; guard++) {
            h->pos -= steplen;
            ev_at(h->pos >= n ? 0u : n - h->pos);        /* (ctl64) each step at its sample */
            h->idx = (uint16_t)fg_next(h, f->dir, len);
            fg_play(t, h, f, h->idx, steplen);
            ev_off = 0;
        }
    }
}
