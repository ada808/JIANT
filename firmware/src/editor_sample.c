/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Juan Ignacio Antonucci (Juan Jiant) */
/* Editor protocol: SAMPLE's bank (JIANT 0.9.9-par13; EDITOR_PROTOCOL.md "SAMPLE bank", cmds 78..81; sample_bank.c).
 * The web editor builds the bank (its header and the ADPCM data, SMP_BANK bytes) and sends it whole: BEGIN, WRITEs of
 * up to 64 bytes (each byte as two 7-bit nibbles, low first), END with the bank's CRC-32; LIST answers its slots. */
enum { ED_SMPB_BEGIN = 78, ED_SMPB_WRITE, ED_SMPB_END, ED_SMPB_LIST };

static int ed_smp_handle(uint32_t cmd, const uint8_t *a, uint32_t n)
{
    uint8_t d[64];
    uint32_t i, rc;
    switch (cmd) {
    case ED_SMPB_BEGIN:                                    /* -> rc */
        ed_b(n ? 1u : smp_begin());
        return 1;
    case ED_SMPB_WRITE:                                    /* offset (3 x 7 bits), 2 x k nibbles (k <= 64) -> rc */
        rc = n < 5u || n > 3u + 2u * sizeof d || (n - 3u) & 1u ? 1u : 0u;
        for (i = 0; !rc && i < (n - 3u) / 2u; i++) {
            if ((a[3 + 2 * i] | a[4 + 2 * i]) > 15u)
                rc = 1;
            d[i] = (uint8_t)(a[3 + 2 * i] | a[4 + 2 * i] << 4);
        }
        ed_b(rc ? rc : smp_write((uint32_t)a[0] | (uint32_t)a[1] << 7 | (uint32_t)a[2] << 14, d, (n - 3u) / 2u));
        return 1;
    case ED_SMPB_END:                                      /* CRC-32 (5 x 7 bits) -> rc, slots */
        rc = n != 5u || a[4] > 15u ? 1u : ed_flash_stop() ? 3u :
             smp_end((uint32_t)a[0] | (uint32_t)a[1] << 7 | (uint32_t)a[2] << 14 | (uint32_t)a[3] << 21 |
                     (uint32_t)a[4] << 28);
        ed_b(rc);
        ed_b(smp_ix.count);
        return 1;
    case ED_SMPB_LIST:                                     /* -> slots (15), the bank's bytes / 64, per slot: used,
                                                            * name, samples (3 x 7 bits), rate / 100 */
        if (n)
            return 0;
        ed_b(SMP_SLOTS);
        ed_b(SMP_BANK / 64u);
        for (i = 0; i < SMP_SLOTS; i++) {
            const smp_ent_t *e = &smp_bank.h.e[i];
            uint32_t u = i < smp_ix.count && smp_ix.c0[i] != 255u;
            ed_b(u);
            ed_str(u ? smp_names[i] : "", 9);
            ed_b(u ? e->n & 127u : 0); ed_b(u ? (e->n >> 7) & 127u : 0); ed_b(u ? e->n >> 14 : 0);
            ed_b(u ? e->rate / 100u : 0);
        }
        return 1;
    }
    return 0;
}
