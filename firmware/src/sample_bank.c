/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Juan Ignacio Antonucci (Juan Jiant) */
/* SAMPLE's bank (JIANT 0.9.9-par13, eng_sample.c): SMP_BANK bytes, the same in RAM (smp_bank, what plays) and in flash
 * at SMP_HOME, 0xDA000..0xDBFFF (6 KiB of two sectors the songs left free: storage.c's map). Power-on copies the flash's into RAM
 * and indexes it; a bank that is none (erased, a CRC that fails: a power cut while it was written) leaves SAMPLE
 * silent, its slots "-".
 *
 * The web editor sends a bank whole (EDITOR_PROTOCOL.md "SAMPLE bank", cmds 78..81, editor_sample.c): BEGIN (the RAM
 * bank cleared, SAMPLE silent), WRITE its bytes into RAM, END with its CRC: checked, indexed (SAMPLE plays it at once)
 * and written to flash (both sectors erased, the bank, read back). Included by upreset.c. */
#define SMP_HOME 0xDA000u

static struct {
    uint8_t on;                                /* a transfer is open */
} smp_tx __attribute__((section(".pool")));

/* the bank in RAM checked and indexed: the ADPCM state every SMP_CHK samples of each slot (after a load or a
 * transfer); no slot when it is no bank */
static void smp_index(void)
{
    const smp_head_t *h = &smp_bank.h;
    uint32_t s, k, c = 0, n = 0, ok;
    smp_ix.count = 0;
    smp_ix.gen++;
    ok = h->magic == SMP_MAGIC && h->ver == 1u && h->count <= SMP_SLOTS && h->len <= SMP_DATA &&
         ~st_crc32_add(0xFFFFFFFFu, smp_bank.b + 16, SMP_HEAD - 16u + h->len) == h->crc;
    for (s = 0; s < SMP_SLOTS; s++) {
        const smp_ent_t *e = &h->e[s];
        smp_names[s] = "-";
        smp_ix.c0[s] = 255u;
        if (!ok || s >= h->count || !e->n || (uint32_t)e->off + ((uint32_t)e->n + 1u) / 2u > h->len ||
            e->rate < 4000u || e->rate > 16000u || c + ((uint32_t)e->n + SMP_CHK - 1u) / SMP_CHK > SMP_NCHK)
            continue;
        {
            int32_t pred = 0, idx = 0;
            uint32_t b = (uint32_t)e->off * 2u;
            smp_ix.c0[s] = (uint8_t)c;
            for (k = 0; k < e->n; k++) {
                if (!(k % SMP_CHK)) {
                    smp_ix.pred[c] = (int16_t)pred;
                    smp_ix.idx[c++] = (uint8_t)idx;
                }
                smp_dec(b + k, &pred, &idx);
            }
        }
        smp_names[s] = e->name[0] ? e->name : "?";
        n = s + 1u;
    }
    smp_ix.count = (uint8_t)n;
}

/* up_boot: the bank from flash */
static __attribute__((noinline)) void smp_boot(void)
{
#if FELUCCA_FLASH
    if (!flash_ok || st_read(SMP_HOME, smp_bank.b, SMP_BANK))
        memset(smp_bank.b, 0xFF, SMP_BANK);
#endif
    smp_index();
}

/* SMPB_BEGIN: the RAM bank cleared, SAMPLE silent. rc 0 */
static uint32_t smp_begin(void)
{
    smp_ix.count = 0;
    smp_ix.gen++;
    memset(smp_bank.b, 0, SMP_BANK);
    smp_tx.on = 1;
    return 0;
}

/* SMPB_WRITE: n bytes at off into the RAM bank. rc 0, 1 out of the bank, 5 no transfer */
static uint32_t smp_write(uint32_t off, const uint8_t *d, uint32_t n)
{
    if (!smp_tx.on)
        return 5;
    if (off > SMP_BANK || n > SMP_BANK - off)
        return 1;
    memcpy(smp_bank.b + off, d, n);
    return 0;
}

/* SMPB_END: crc, the CRC-32 of the whole bank as sent. Indexed (it plays), then written to flash. rc 0, 2 the CRC or
 * no bank (none kept: SAMPLE silent until a good one), 4 flash (it plays until power-off), 5 no transfer */
static uint32_t smp_end(uint32_t crc)
{
    uint32_t k;
    if (!smp_tx.on)
        return 5;
    smp_tx.on = 0;
    if (st_crc32(smp_bank.b, SMP_BANK) != crc)
        return 2;
    smp_index();
    if (!smp_ix.count)
        return 2;
#if FELUCCA_FLASH
    if (!flash_ok)
        return 4;
    for (k = 0; k < SMP_BANK; k += ST_SECTOR)
        if (st_erase(SMP_HOME + k))
            return 4;
    for (k = 0; k < SMP_BANK; k += 256u)
        if (st_prog(SMP_HOME + k, smp_bank.b + k, 256u))
            return 4;
    {
        uint8_t b[256];
        uint32_t c = 0xFFFFFFFFu;
        for (k = 0; k < SMP_BANK; k += sizeof b) {
            if (st_read(SMP_HOME + k, b, sizeof b))
                return 4;
            c = st_crc32_add(c, b, sizeof b);
        }
        if (~c != crc)
            return 4;
    }
#else
    (void)k;
#endif
    return 0;
}
