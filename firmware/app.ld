/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* FELUCCA memory map.
 * Code + constants execute in place from XIP (app.bin offset 0 = 0x02000120).
 * RAMTEXT (JIANT 0.6.78: at the top of the pool, was 0x01C00000): .ram_text (code that must run while the flash is
 * busy: the flash driver), .c1_text (the second core's loop), .hot_text (core 0's hottest audio code). A call reaches
 * +-4 MiB (pi32v2 imm23 x 2, no long calls in this compiler): RAM code there reaches the whole XIP image (build.py
 * checks the margin); at 0x01C00000 it reached none of it.
 * LOWBUF 0x01C00000..0x01C08000 (0.6.78; 0.6.82: 4 KiB more, the steps grew): main-loop buffers (undo, the song
 * section's stage; the project scratch until 0.8.4 and the fonts' decode tables in 0.8.4, both in the pool since
 * 0.8.5; 0.9.9-beta4: the fonts' tables and the user presets' banks back here, 9 KiB of the pool given to RAMTEXT),
 * zeroed at boot. 0x01C07000..0x01C08000 was kept for RAM-run debug until 0.9.9-beta4 (LOWBUF took it).
 * RAM  : .data/.bss (96 KiB)
 * POOL : big zero-initialised buffers (FX lines, canvases), section .pool
 * STACKS with 256-byte guard bands; NOINIT survives resets and UBOOT entry.
 * 0x01C7FD50.. is reserved: boot_info, boot-state mailbox, vector table. */
MEMORY
{
    XIP    (rx)  : ORIGIN = 0x02000120, LENGTH = 0x8DFBC
    LOWBUF (rw)  : ORIGIN = 0x01C00000, LENGTH = 0x8000
    RAM    (rwx) : ORIGIN = 0x01C08000, LENGTH = 96K
    POOL   (rw)  : ORIGIN = 0x01C20000, LENGTH = 0x4D800
    RAMTEXT (rwx): ORIGIN = 0x01C6D800, LENGTH = 0x8800   /* (0.6.97, 0.7.0: up 4 KiB each, the image grew: a call from here must
                                                           * reach its end; the user stack 4 KiB shorter each time, 15.75 KiB;
                                                           * 0.9.9-stab, as 0.9.12: 1 KiB lower, from the pool: the 320 MHz switch;
                                                           * 0.9.9-beta4: 9 KiB lower, from the pool (8 KiB of its main-loop
                                                           * buffers moved to LOWBUF): the delay's, the reverbs' and the
                                                           * chorus's block code in RAM, out of the I-cache the engines share) */
    NOINIT (rw)  : ORIGIN = 0x01C7C000, LENGTH = 0x3D50
}
SECTIONS
{
    .text : { KEEP(*(.text.crt0)) *(.text*) *(.rodata*) . = ALIGN(4); _etext = .; } > XIP
    .ram_text : { _rt_start = .; KEEP(*(.ram_text*)) . = ALIGN(4); _rt_end = .; } > RAMTEXT AT > XIP
    _rt_load = LOADADDR(.ram_text);
    /* (JIANT 0.6.71) the second core's waiting loop (hal/fm1_multicore.h): in RAM, after the flash driver, inside the
     * guard's RAM code window (_rt_start .. _c1t_end); its own section, as it calls the jobs (the build's check that
     * .ram_text calls nothing still holds) */
    .c1_text : { _c1t_start = .; KEEP(*(.c1_text*)) . = ALIGN(4); _c1t_end = .; } > RAMTEXT AT > XIP
    _c1t_load = LOADADDR(.c1_text);
    /* (JIANT 0.6.78) core 0's hottest audio code (tools/build.py HOT_FNS): in RAM after the second core's loop, inside the
     * same RAM code window (_rt_start .. _hot_end) */
    .hot_text : {
        _hot_start = .;
        KEEP(*(.hot_text*))   /* (tools/build.py HOT_FNS: their sections renamed in the optimised IR) */
        . = ALIGN(4);
        _hot_end = .;
    } > RAMTEXT AT > XIP
    _hot_load = LOADADDR(.hot_text);
    .data : { . = ALIGN(4); _data_start = .; *(.data*) *(.sdata*) . = ALIGN(4); _data_end = .; } > RAM AT > XIP
    _data_load = LOADADDR(.data);
    .bss (NOLOAD) : { _bss_start = .; *(.bss*) *(.sbss*) *(COMMON) . = ALIGN(4); _bss_end = .; } > RAM
    .pool (NOLOAD) : { _pool_start = .; *(.pool*) . = ALIGN(4); _pool_end = .; } > POOL
    /* (JIANT 0.6.71) the second core's stacks (hal/fm1_multicore.h): 6 KiB for the render, 1 KiB for an exception */
    .c1_stacks (NOLOAD) : { . = ALIGN(32); _c1_stack_lo = .; . += 7168; _c1_stack_hi = .; } > POOL
    _c1_ustack_top = _c1_stack_lo + 6144;
    _c1_sstack_top = _c1_stack_hi;
    .lowbuf (NOLOAD) : { _low_start = .; *(.lowbuf*) . = ALIGN(4); _low_end = .; } > LOWBUF
    .noinit (NOLOAD) : { *(.noinit*) } > NOINIT
    _guard0 = 0x01C76000; _ustack_lo = 0x01C76100; _ustack_top = 0x01C7A000;
    _guard1 = 0x01C7A000; _sstack_lo = 0x01C7A100; _sstack_top = 0x01C7C000;
    /DISCARD/ : { *(.comment) *(.note*) }
}
