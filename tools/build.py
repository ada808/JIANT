#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
"""Build Felucca: the app, the update loader and an installable .fwsc package.

  tools/build.py [--release X.Y[-suffix]]

Outputs in build/: felucca.bin (app), loader/ota.bin (update loader),
felucca.fwsc (package). A release build (--release X.Y) writes felucca-X.Y.fwsc and a folder
release-X.Y/ with the package, the app, SHA256SUMS, the sample attribution and the licence files.
See BUILDING.md for the toolchain and the SDK.

The JieLi toolchain is Linux x86-64 only. JIELI_TOOLCHAIN points at it; on
macOS (or with JIELI_DOCKER=1) each tool runs in a linux/amd64 container.
"""
import argparse
import hashlib
import os
import platform
import re
import shutil
import struct
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

SRC = Path(__file__).resolve().parents[1]
FW = SRC / "firmware"
OUT = SRC / "build"
GEN = OUT / "gen"
LDR = OUT / "loader"
sys.path.insert(0, str(SRC / "tools"))
import fm1pkg_make  # noqa: E402
import lz4blk  # noqa: E402

APP_XIP = 0x02000120                # app.bin offset 0 in the XIP map; the SPL jumps here
APP_SLOT = fm1pkg_make.APP_SLOT
LOADER_LOAD = 0x01C0A800
POOL_END = 0x01C6D800            # (JIANT 0.6.78) app.ld: POOL's end, RAMTEXT's start (0.6.97, 0.7.0: up 4 KiB each; 0.9.9-beta4: 9 KiB lower;
                                 # 0.9.9-stab: 1 KiB lower, the 320 MHz switch's RAM code)
RAMTEXT_END = 0x01C76000         # .. RAMTEXT's end (the user stack's guard band)
LOADER_NAME = b"usb_hid_ota.bin"    # the file name the SPL looks for
DOCKER_IMAGE = os.environ.get("JIELI_DOCKER_IMAGE", "debian:bookworm-slim")
CFLAGS = ["-Os", "-ffunction-sections", "-fno-builtin", "-Wall", "-Wno-unused-function"]
LINE = re.compile(r"^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2} )+)\s*\t(.*)$")

# SDK files of AC79NN_SDK_V1.2.1_2023-12-13 (the tested version)
SDK_SHA256 = {
    "uboot.boot": "4e3b4c220dc96641cb5a723f41e68ce41d5261ae9434bb33fbd7f2c59976ded4",
    "cfg_tool.bin": "276579954f076886a6a7694f65dc71c034a63a2c204b76749065c0ac7b010d1b",
    "cfg/eq_cfg_hw.bin": "41167491bffed4651750719c973d2758adeb9021a5670d02d6a53c85ed80ea7d",
}

PRODUCT = "FM-1_909"                # package identity; release builds are FM-1_9XY
VERSION = None                      # FELUCCA_VERSION for release builds (default: firmware/src/felucca.c)


def toolchain():
    tc = os.environ.get("JIELI_TOOLCHAIN")
    if not tc or not (Path(tc) / "pi32v2" / "bin" / "clang").exists():
        raise SystemExit("JIELI_TOOLCHAIN must point at the JieLi Linux toolchain "
                         "(the directory with pi32v2/ and common/; see tools/get_toolchain.sh)")
    return Path(tc).resolve()


def use_docker():
    native = platform.system() == "Linux" and platform.machine() in ("x86_64", "AMD64")
    return os.environ.get("JIELI_DOCKER", "0" if native else "1") == "1"


def tc(tool, *args):
    """run a toolchain binary (pi32v2/bin/..., common/bin/...) with cwd SRC; paths relative to SRC"""
    rel = [str(Path(a).resolve().relative_to(SRC)) if isinstance(a, Path) else a for a in args]
    if tool == "cc":                # the toolchain's cc wrapper needs python3; call clang directly
        tool, rel = "pi32v2/bin/clang", ["-target", "pi32v2", *rel]
    if use_docker():
        cmd = ["docker", "run", "--rm", "--platform", "linux/amd64", "-v", f"{SRC}:/work",
               "-v", f"{toolchain()}:/opt/jieli:ro", "-w", "/work", DOCKER_IMAGE, f"/opt/jieli/{tool}", *rel]
    else:
        cmd = [str(toolchain() / tool), *rel]
    r = subprocess.run(cmd, cwd=SRC, capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit(f"build: {tool} failed")
    if r.stderr.strip():
        sys.stderr.write(r.stderr)
    return r.stdout


def tc_all(*cmds):
    with ThreadPoolExecutor(len(cmds)) as ex:
        return list(ex.map(lambda c: tc(*c), cmds))


def generate():
    """generated headers (UI fonts, icons, keycaps, palettes, line art shapes, tables, samples)"""
    GEN.mkdir(parents=True, exist_ok=True)
    for old in ("felucca_font.h", "felucca_icons.h"):     # headers of the bitmap font and icon atlas
        (GEN / old).unlink(missing_ok=True)
    tools = SRC / "tools"
    cmds = [[tools / "gen_aa_font.py", GEN / "ui_fonts.h", "--preset", "jiant-lab"],   # (gen_aa_font.UI_PRESET),
            [tools / "gen_px_font.py", GEN / "ui_font_x.h"],
            [tools / "gen_aa_icons.py", GEN / "ui_icons.h"],
            [tools / "gen_aa_keycaps.py", GEN / "ui_keycaps.h"],
            [tools / "gen_ui_palettes.py", GEN / "ui_palettes.h"],
            [tools / "gen_ui_shapes.py", GEN / "ui_shapes.h"],
            [tools / "gen_tables.py", GEN / "felucca_tables.h"],
            [tools / "gen_lyrics.py", GEN / "felucca_lyrics.h"],   # (0.8.5) VOICE's words (sing.c)
            [tools / "gen_fm6_patches.py", GEN / "felucca_fm6.h"],
            [tools / "gen_drum_maps.py", FW / "src" / "gen_tables.c", FW / "src" / "song_dice.c", GEN / "gen_drum_maps.h",
             GEN / "dice_bars.h", GEN / "gen_bass.h"]]   # (0.9.7)
    procs = [subprocess.Popen([sys.executable, *map(str, c)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              text=True) for c in cmds]
    failed = []
    for c, p in zip(cmds, procs):
        sys.stdout.write(p.communicate()[0])
        if p.returncode:
            failed.append(c[0].name)
    if failed:
        raise SystemExit(f"build: {', '.join(failed)} failed")


# ---- update loader

def crc16(d, c=0):
    for b in d:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) if c & 0x8000 else c << 1
        c &= 0xFFFF
    return c


def ldr_head(dcrc, a, b, attr, name):
    body = struct.pack("<HIIBBH16s", dcrc, a, b, attr, 0, 0, name)
    return struct.pack("<H", crc16(body)) + body


def ldr_wrap(image):
    """ota.bin = outer JLFS head (32 B) + inner head (32 B, load address) +
    blocks [clen u32][dlen u32][LZ4 block], dlen 4096 except the last"""
    blocks = bytearray()
    for i in range(0, len(image), 4096):
        raw = image[i:i + 4096]
        c = lz4blk.compress(raw)
        if len(c) > 4096 or lz4blk.decompress(c) != raw:
            raise SystemExit(f"loader: block {i // 4096} does not compress below 4096 B")
        blocks += struct.pack("<II", len(c), len(raw)) + c
    inner = ldr_head(crc16(image), len(image), LOADER_LOAD, 0, LOADER_NAME)
    body = inner + bytes(blocks)
    return ldr_head(crc16(body), 0x20, len(body), 0x41, LOADER_NAME) + body


def build_loader():
    LDR.mkdir(parents=True, exist_ok=True)
    src = FW / "loader"
    flags = [*CFLAGS, "-Ifirmware/hal", "-Ifirmware/src"]
    tc_all(("cc", "-c", src / "crt0_ldr.S", "-o", LDR / "crt0_ldr.o"),
           ("cc", *flags, "-c", src / "loader.c", "-o", LDR / "loader.o"))
    elf = LDR / "loader.elf"
    tc("pi32v2/bin/ld", "--gc-sections", "-e", "_start", "-T", src / "loader.ld",
       LDR / "crt0_ldr.o", LDR / "loader.o", "-o", elf)
    _, dis, hdr, syms = tc_all(("common/bin/objcopy", "-O", "binary", "-j", ".text", elf, LDR / "loader.bin"),
                               ("common/bin/objdump", "-d", elf),
                               ("common/bin/objdump", "-h", elf),
                               ("common/bin/objdump", "-t", elf))
    (LDR / "loader.dis").write_text(dis)
    for ln in hdr.splitlines():     # the loader rewrites the flash: nothing may run from XIP
        p = ln.split()
        if len(p) > 4 and p[1].startswith(".") and int(p[3], 16) >= 0x02000000 and int(p[2], 16):
            raise SystemExit(f"loader: section {p[1]} at {p[3]} is outside RAM")
    image = (LDR / "loader.bin").read_bytes()
    if not image or len(image) > 0x14000:
        raise SystemExit("loader: image empty or too big")
    ota = ldr_wrap(image)
    (LDR / "ota.bin").write_bytes(ota)
    bss = [int(ln.split()[0], 16) for ln in syms.splitlines() if ln.rstrip().endswith("_bss_end")]
    print(f"loader: image {len(image)} B at {LOADER_LOAD:#x}" + (f", bss end {bss[0]:#x}" if bss else ""))
    print(f"loader: ota.bin {len(ota)} B")
    return ota


# ---- app

# (JIANT 0.6.78) core 0's hottest audio code, run from RAM (app.ld .hot_text, src/core.h FELUCCA_HOTRAM): the mix, the
# master, the FX layer, the SLICER, and core 0's own copies of a part's mix and the buses (core 1 runs the flash ones)
# (0.9.9-beta4) and the buses' per-block code both cores run (fx.c fx_buses, from fx_run0 on either core): the delay's
# line (dly_lo and its helpers), the reverb models and their dispatch, the chorus's admission, the DIST's shapers; the
# I-cache (32 KiB, shared by both cores and the UI) kept for the engines (Kirill's audit, LOOM-8)
HOT_FNS = ("mix_block", "mix_part_m0", "fx_run0", "perf_begin", "perf_pre", "perf_block", "perf_master", "slicer_bus",
           "dly_lo", "dly_grain", "dly_grain_adv", "dly_wow_adv", "dly_line_clear", "dly_rate", "fx_rest",
           "rev_run", "rev_idle", "rev_room", "rev_room_mod", "rev_spring", "rev_spring_side", "rev_cloud", "rev_shimmer",
           "rev_reso", "cho_admit", "dist_shape", "dist_amp")


def hot_sections(ir):
    """HOT_FNS' definitions in the optimised IR given section ".hot_text.<name>"; (the IR, how many found)"""
    found = 0

    def put(m):
        nonlocal found
        found += 1
        return f'{m.group(1)} section ".hot_text.{m.group(2)}" {{'
    ir = re.sub(r"^(define [^\n]*?@(" + "|".join(HOT_FNS) + r")\([^\n]*?)\s*\{$", put, ir, flags=re.M)
    return ir, found


def build_app():
    flags = [*CFLAGS, "-Ifirmware/hal", "-Ifirmware/src", "-Ibuild/gen"]
    for flag in ("FELUCCA_FLASH", "FELUCCA_OTA", "FELUCCA_OTA_DRYRUN", "FELUCCA_OTA_RAMONLY", "FELUCCA_CDC",
                 "FELUCCA_UART", "FELUCCA_UAC", "FELUCCA_UAC_TONE", "FELUCCA_UAC_48K", "FELUCCA_ICONS", "FELUCCA_SLICE", "FELUCCA_FM4",
                 "FELUCCA_CDC_DEFAULT"):
        v = os.environ.get(flag)    # unset: the default in firmware/src/felucca.c
        if v in ("0", "1"):
            flags.append(f"-D{flag}={v}")
    v = os.environ.get("FELUCCA_USB_LAYOUT")   # firmware/src/usb.c: how the USB composite presents itself (#67), 0..3
    if v in ("0", "1", "2", "3"):
        flags.append(f"-DFELUCCA_USB_LAYOUT={v}")
    flags.append(f'-DFELUCCA_ID="{PRODUCT}"')
    if VERSION:
        flags.append(f'-DFELUCCA_VERSION="{VERSION}"')
    # felucca.c goes to LLVM IR without the optimizer, the main-loop functions (UI, stores, editor) are
    # marked minsize (tools/size_fns.py), then the IR is compiled at -Os. FELUCCA_SIZE=0: -Os everywhere
    size = os.environ.get("FELUCCA_SIZE") != "0"
    hot = size and os.environ.get("FELUCCA_HOTRAM") != "0"   # (0.6.78) core 0's hottest audio code in RAM (HOT_FNS)
    flags.append(f"-DFELUCCA_HOTRAM={int(hot)}")
    cmain = (("cc", *flags, "-S", "-emit-llvm", "-Xclang", "-disable-llvm-optzns", "-c",
              FW / "src" / "felucca.c", "-o", OUT / "felucca.ll") if size else
             ("cc", *flags, "-c", FW / "src" / "felucca.c", "-o", OUT / "felucca.o"))
    tc_all(("cc", "-c", FW / "crt0.S", "-o", OUT / "crt0.o"),
           ("cc", "-c", FW / "hal" / "fm1_vec.S", "-o", OUT / "fm1_vec.o"),
           ("cc", "-c", FW / "hal" / "fm1_isr.S", "-o", OUT / "fm1_isr.o"),
           cmain)
    if size:
        subprocess.run([sys.executable, SRC / "tools" / "size_fns.py", OUT / "felucca.ll", OUT / "felucca_size.ll"],
                       check=True)
        cf = [f for f in flags if not f.startswith(("-I", "-D", "-W"))]
        if hot:   # (JIANT 0.6.78) optimised first, then HOT_FNS put in .hot_text, then only the code generator: a
            # section attribute before the optimiser would stop it inlining anything into them (this clang inlines
            # nothing across sections), so they come out as they were, only in RAM (app.ld)
            tc("cc", *cf, "-S", "-emit-llvm", "-c", OUT / "felucca_size.ll", "-o", OUT / "felucca_opt.ll")
            ir, n = hot_sections((OUT / "felucca_opt.ll").read_text())
            if n != len(HOT_FNS):
                raise SystemExit(f"hot RAM code: {n} of the {len(HOT_FNS)} functions found in the IR ({HOT_FNS})")
            (OUT / "felucca_hot.ll").write_text(ir)
            tc("cc", *cf, "-Xclang", "-disable-llvm-optzns", "-c", OUT / "felucca_hot.ll", "-o", OUT / "felucca.o")
        else:
            tc("cc", *cf, "-c", OUT / "felucca_size.ll", "-o", OUT / "felucca.o")
    elf = OUT / "felucca.elf"
    tc("pi32v2/bin/ld", "-T", FW / "app.ld", OUT / "crt0.o", OUT / "fm1_vec.o", OUT / "fm1_isr.o",
       OUT / "felucca.o", "-o", elf)
    for sect in ("text.bin", "data.bin", "ramtext.bin", "c1text.bin", "hottext.bin"):
        (OUT / sect).unlink(missing_ok=True)
    *_, syms, dis, rt = tc_all(("common/bin/objcopy", "-O", "binary", "-j", ".text", elf, OUT / "text.bin"),
                               ("common/bin/objcopy", "-O", "binary", "-j", ".data", elf, OUT / "data.bin"),
                               ("common/bin/objcopy", "-O", "binary", "-j", ".ram_text", elf, OUT / "ramtext.bin"),
                               ("common/bin/objcopy", "-O", "binary", "-j", ".c1_text", elf, OUT / "c1text.bin"),
                               ("common/bin/objcopy", "-O", "binary", "-j", ".hot_text", elf, OUT / "hottext.bin"),
                               ("common/bin/objdump", "-t", elf),
                               ("common/bin/objdump", "-d", elf),
                               ("common/bin/objdump", "-d", "-j", ".ram_text", elf))
    (OUT / "felucca.dis").write_text(dis)

    def symv(name):
        return int(re.search(r"^([0-9a-f]+) .*\s" + name + r"$", syms, re.M).group(1), 16)
    img = bytearray((OUT / "text.bin").read_bytes())
    # .ram_text, .c1_text (0.6.71: the second core's loop), .hot_text (0.6.78: core 0's hot audio code) and .data follow .text at their load addresses; crt0 copies
    # them by words
    for sect, lname in (("ramtext.bin", "_rt_load"), ("c1text.bin", "_c1t_load"), ("hottext.bin", "_hot_load"),
                        ("data.bin", "_data_load")):
        load = symv(lname)
        if load % 4:
            raise SystemExit(f"{lname} {load:#x} is not word aligned")
        blob = (OUT / sect).read_bytes() if (OUT / sect).exists() else b""
        if blob:
            if load - APP_XIP < len(img):
                raise SystemExit(f"{lname} overlaps the image")
            img += b"\xff" * (load - APP_XIP - len(img))
            img += blob
    img += b"\xff" * (-len(img) % 4)
    (OUT / "felucca.bin").write_bytes(img)
    return bytes(img), syms, dis, rt


# (0.9.9-stab) felucca-bsp docs/CPU.md: on the AC79 a 32 x 32 -> 64-bit multiply inlined beside loads (most of all 16-bit
# ones) now and then leaves a register a bit off; src/core.h's helpers (mul64 ...) hold the audio path's. These keep theirs:
# once a block or a frame, 64 x 64-bit products or the UI (the SDK's own 64-bit printf digits are the same case: not hot)
MUL64_SKIP = {"djf_coef", "draw_graph", "graph_adsr", "vx_arc", "fm1_main", "fugue_tick", "midi_clock_pulse", "seq_step"}


def mul64_near_loads(dis, window=4):
    """(0.9.9-stab, after fm1-base's tools/build.py, MPL-2.0) functions with a load in flight beside a 32x32->64-bit multiply
    (rN_rM = rA * rB): one up to two instructions before it, or one after it before its product is used (using it waits
    for the multiply). On the FM-1 such code now and then leaves a register a bit off, most of all beside 16-bit loads
    (felucca-bsp docs/CPU.md); a multiply in a helper of its own (noinline, src/core.h mul64) is clear of it.
    [(function, sites, the nearest load's distance in instructions, 1 = a 16-bit load among them)]"""
    found, fn, body = [], "", []

    def scan():
        sites, near, half = 0, 99, 0
        for i, ins in enumerate(body):
            m = re.match(r"(r\d+)_(r\d+) [+-]?= r\d+ \* r\d+", ins)
            if not m:
                continue
            dst, hit = {m.group(1), m.group(2)}, 0
            for d, b in enumerate(reversed(body[max(0, i - 2):i]), 1):
                if re.search(r"\bcall\b", b):
                    break
                if re.search(r"= [hbd]?\[", b):
                    hit, near = 1, min(near, d)
                    half |= b.split("=", 1)[1].lstrip().startswith("h[")
            for d, b in enumerate(body[i + 1:i + 1 + window], 1):
                if any(re.search(r"\b" + r + r"\b", b) for r in dst):
                    break                       # the product used: the multiply has finished
                if re.search(r"= [hbd]?\[", b):
                    hit, near = 1, min(near, d)
                    half |= b.split("=", 1)[1].lstrip().startswith("h[")
            sites += hit
        return sites, near, half

    for ln in dis.splitlines() + ["end:"]:
        mm = LINE.match(ln)
        if not mm:
            m2 = re.match(r"^([A-Za-z_$][\w.$]*):$", ln.strip())
            if m2:                              # (a label starting with '.': a jump table's piece, still the function)
                if fn and not fn.startswith("__") and fn.split(".")[0] not in MUL64_SKIP:
                    n, near, half = scan()
                    if n:
                        found.append((fn, n, near, half))
                fn, body = m2.group(1), []
            continue
        body.append(mm.group(3))
    return found


SOFT_DOUBLE = ("__adddf3", "__subdf3", "__muldf3", "__divdf3", "__floatsidf", "__floatunsidf", "__fixdfsi",
               "__fixunsdfsi", "__extendsfdf2", "__truncdfsf2", "__eqdf2", "__nedf2", "__gtdf2", "__gedf2", "__ltdf2", "__ledf2")


def check(img, syms, dis, rt):
    errors, notes = [], []
    mul64 = mul64_near_loads(dis)       # (0.9.9-stab) the CPU fault of felucca-bsp docs/CPU.md: a note, the code may be fine
    if mul64:
        notes.append("64-bit multiplies inlined beside loads (felucca-bsp docs/CPU.md: may corrupt a register; move them "
                     "into src/core.h's mul64 helpers): " +
                     ", ".join(f"{f} ({n} site{'s' if n > 1 else ''}, a load {d} insn{'s' if d > 1 else ''} away{', 16-bit' if h else ''})"
                               for f, n, d, h in mul64[:8]) + (" ..." if len(mul64) > 8 else ""))
    else:
        notes.append("no 64-bit multiply inlined beside a load (src/core.h mul64 helpers; felucca-bsp docs/CPU.md)")
    dbl = [n for n in SOFT_DOUBLE if re.search(r"\s" + n + r"$", syms, re.M)]
    if dbl:                             # (0.9.9-stab) the pi32v2 has no double: a soft one is a slow mistake (fm1-base's check)
        errors.append(f"soft-double routines linked (a double crept in): {dbl}")
    else:
        notes.append("no soft-double routine linked")
    m = re.search(r"^([0-9a-f]+) .*\s_start$", syms, re.M)
    if not m or int(m.group(1), 16) != APP_XIP:
        errors.append(f"_start is not at {APP_XIP:#x}")
    if img[:4] != bytes.fromhex("04818000"):
        errors.append(f"image starts with {img[:4].hex()}, not the entry stub")
    rt_calls = [ln for ln in rt.splitlines() if re.search(r"\bcall\b", ln)]
    if rt_calls:                    # RAM code runs with the flash off: no calls into XIP
        errors.append(f".ram_text contains calls: {rt_calls[:3]}")
    else:
        notes.append(f".ram_text: {len([ln for ln in rt.splitlines() if LINE.match(ln)])} insns, no calls")
    for ln in dis.splitlines():     # nothing may call or load an address in the chip ROM
        mm = LINE.match(ln)
        if not mm:
            continue
        for v in re.findall(r"= (-?\d+) <|call -?\d+ <[^:>]*: ([0-9a-f]+) >", mm.group(3)):
            val = (int(v[0]) & 0xFFFFFFFF) if v[0] else int(v[1], 16) & 0xFFFFFFFF
            if 0xFFC00000 <= val < 0xFFD00000:
                errors.append(f"reference to ROM address {val:#010x}")
    if len(img) > APP_SLOT:
        errors.append(f"image {len(img)} B exceeds the app slot")

    def sym(name):
        mm = re.search(r"^([0-9a-f]+) .*\s" + name + r"$", syms, re.M)
        return int(mm.group(1), 16) if mm else 0
    bss = sym("_bss_end") - 0x01C08000
    pool = sym("_pool_end") - sym("_pool_start")
    pool_top = max(sym("_pool_end"), sym("_c1_stack_hi"))   # (the second core's stacks after the pool)
    notes.append(f"image {len(img)} B; RAM .data+.bss {bss} B of 98304; pool {pool} B, spare {POOL_END - pool_top} B")
    if bss > 96 * 1024:
        errors.append("RAM region overflow")
    if POOL_END - pool_top < 8192:                # keep >= 8 KiB of the pool spare
        errors.append(f"pool headroom {POOL_END - pool_top} B < 8192 B")
    # (JIANT 0.6.78) the RAM code at the pool's top: a call reaches +-4 MiB (app.ld), so from the lowest RAM code
    # address the XIP image must end within 4 MiB; and the room left for RAM code
    # (0.7.5) what must be in reach is what RAM code calls, not the whole image: the calls in .ram_text (none),
    # .c1_text and .hot_text, from the disassembly (`call <rel> <name : addr>`); the sound code they call comes early
    # in the image (the TU's order: engines, voice, fx before the UI), so the UI can grow past the old whole-image
    # bound (the image's end in reach of the lowest RAM code: 12 KiB short by 0.7.5, after RAMTEXT moved up twice)
    targets, sect = [], None
    for ln in dis.splitlines():
        mm = re.match(r"Disassembly of section (\S+):", ln)
        if mm:
            sect = mm.group(1)
            continue
        if sect in (".ram_text", ".c1_text", ".hot_text"):
            mm = re.search(r"\bcall\b[^<]*<([\w.]+)\s*:\s*([0-9a-f]+)\s*>", ln)
            if mm and int(mm.group(2), 16) >= APP_XIP:
                targets.append((int(mm.group(2), 16), mm.group(1)))
    far = max(targets) if targets else (APP_XIP, "-")
    reach = sym("_rt_start") + 0x400000 - far[0]
    ramcode = sym("_hot_end") - sym("_rt_start")
    notes.append(f"RAM code {ramcode} B of {RAMTEXT_END - sym('_rt_start')}; {len(targets)} calls into the image, the farthest "
                 f"{far[1]} with {reach} B of reach to spare (the image's end: {sym('_rt_start') + 0x400000 - sym('_etext')} B)")
    if not targets and "call" in dis:
        errors.append("no call from RAM code found in the disassembly: the reach check cannot read it")
    if reach < 12288:                              # (12 KiB to spare beyond the farthest callee)
        errors.append(f"RAM code's farthest call ({far[1]}) has only {reach} B of reach to spare (< 12 KiB): "
                      "move RAMTEXT up (app.ld) or that code earlier in the image")
    if RAMTEXT_END - sym("_hot_end") < 2048:
        errors.append(f"RAM code headroom {RAMTEXT_END - sym('_hot_end')} B < 2048 B")
    return errors, notes


# Every hardware register access lives in hal/. In src/ and loader/ (comments stripped):
#  - no volatile pointer cast, except of a C object's address (`*(volatile T *)&x`: a read-once
#    of a RAM flag shared with an ISR, e.g. usb.c ota_wire_send);
#  - no literal in a register or reserved window (core SFRs 0x10000-0x13FFF, SFC 0x40000-0x43FFF,
#    GPIO/IOMAP 0x50000-0x51FFF, CPU 0x1EE0000-0x1EEFFFF, RAM top 0x01C7F000- (boot info,
#    mailbox, vectors), XIP 0x02000000-0x020FFFFF);
#  - no inline asm, except the empty compiler barrier RING_PUBLISH() (emits no instruction);
#  - no HAL register macro (a hal/ #define that is, or expands to, a volatile access).
# Linker symbols (_bss_start[], _rt_load[], ...) are plain C objects and pass.
MMIO_LIT = re.compile(r"\b0x0*(1[0-3][0-9a-f]{3}|4[0-3][0-9a-f]{3}|5[01][0-9a-f]{3}|1ee[0-9a-f]{4}|"
                      r"1c7f[0-9a-f]{3}|20[0-9a-f]{5})u?l?\b", re.I)
MMIO_CAST = re.compile(r"\(\s*(?:const\s+)?volatile\b[^()]*\*\s*\)(?!\s*&)")
MMIO_ASM = re.compile(r"\b(?:__asm__|asm)\b(?!\s+volatile\s*\(\s*\"\"\s*:::\s*\"memory\"\s*\))")


def mmio_check():
    def strip(s):
        s = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), s, flags=re.S)
        return re.sub(r"//[^\n]*", "", s)
    defs = {}
    for h in sorted((FW / "hal").glob("*.h")):
        for m in re.finditer(r"^#define\s+(\w+)(?:\([^)]*\))?\s+(.*)$", strip(h.read_text()), re.M):
            defs[m.group(1)] = m.group(2)
    regs = {n for n, b in defs.items() if re.search(r"\bvolatile\b", b)}
    while True:                                   # macros built on register macros (FM1_WR_LIMIT_H -> FM1_X2)
        more = {n for n, b in defs.items() if n not in regs and set(re.findall(r"\w+", b)) & regs}
        if not more:
            break
        regs |= more
    errors = []
    for f in sorted([*(FW / "src").glob("*.[ch]"), *(FW / "loader").glob("*.c")]):
        for no, ln in enumerate(strip(f.read_text()).splitlines(), 1):
            where = f"{f.relative_to(FW)}:{no}"
            for rx, what in ((MMIO_LIT, "register/window address"), (MMIO_CAST, "volatile pointer cast"),
                             (MMIO_ASM, "inline asm")):
                m = rx.search(ln)
                if m:
                    errors.append(f"{where}: {what} outside hal/ ({m.group(0).strip()}); add a hal/ helper")
            used = set(re.findall(r"\b[A-Z_][A-Z0-9_]*\b", ln)) & regs
            if used:
                errors.append(f"{where}: HAL register macro {sorted(used)[0]} outside hal/; use a hal/ helper")
    return errors


def main():
    global PRODUCT, VERSION
    ap = argparse.ArgumentParser()
    ap.add_argument("--release", metavar="X.Y", help="release build: identity FM-1_9XY, version string vX.Y")
    ap.add_argument("--sdk", type=Path, help="JieLi AC79 SDK checkout (default: $AC79_SDK)")
    a = ap.parse_args()
    name = "felucca.fwsc"
    if a.release:                   # one digit each: the identity has room for two (X.Y.Z and X.Y.Z.W keep X.Y's)
        m = re.fullmatch(r"(\d)\.(\d)(?:\.\d){0,2}(-[A-Za-z0-9]+)?", a.release)
        if not m:
            raise SystemExit(f"--release {a.release}: use X.Y, X.Y.Z, X.Y.Z.W or X.Y-suffix, one digit each")
        PRODUCT = "FM-1_9" + m[1] + m[2]
        VERSION = "v" + a.release.lower()      # e.g. v1.0, v1.1-rc1
        name = f"felucca-{a.release}.fwsc"
    fm1pkg_make.SDK = a.sdk
    for rel, sha in SDK_SHA256.items():          # fail early without the SDK
        if hashlib.sha256(fm1pkg_make.sdk_file(rel)).hexdigest() != sha:
            print(f"warning: SDK {rel} differs from AC79NN_SDK_V1.2.1; the package will not match the reference")
    OUT.mkdir(parents=True, exist_ok=True)
    with ThreadPoolExecutor(2) as ex:
        gen, ldr = ex.submit(generate), ex.submit(build_loader)
        gen.result()
        ota = ldr.result()
    img, syms, dis, rt = build_app()
    errors, notes = check(img, syms, dis, rt)
    hal_err = mmio_check()
    errors += hal_err
    if not hal_err:
        notes.append("register access: hal/ only (src/, loader/ clean)")
    for n in notes:
        print("  ok   ", n)
    for e in errors:
        print("  FAIL ", e)
    if errors:
        raise SystemExit("build: checks failed")
    pkg = fm1pkg_make.ufw(fm1pkg_make.flash_image(img, fm1pkg_make.KEY), ota, PRODUCT)
    (OUT / name).write_bytes(pkg)
    print(f"app      {OUT / 'felucca.bin'}  {len(img)} B")
    print(f"loader   {LDR / 'ota.bin'}  {len(ota)} B")
    print(f"package  {OUT / name}  {len(pkg)} B, identity {PRODUCT}")
    if a.release:                   # what a release carries: the package, the app and every licence they need
        rel = OUT / f"release-{a.release}"
        shutil.rmtree(rel, ignore_errors=True)
        (rel / "LICENSES").mkdir(parents=True)
        app = f"felucca-{a.release}-app.bin"
        (rel / name).write_bytes(pkg)
        (rel / app).write_bytes(img)
        (rel / "SHA256SUMS").write_text("".join(f"{hashlib.sha256(b).hexdigest()}  {n}\n"
                                                for n, b in ((name, pkg), (app, bytes(img)))))
        for f in sorted((SRC / "LICENSES").glob("*.txt")):
            shutil.copy(f, rel / "LICENSES" / f.name)
        for doc in ("LICENSE", "LICENSING.md"):
            shutil.copy(SRC / doc, rel / doc)
        print(f"release  {rel}/")
    return 0


if __name__ == "__main__":
    sys.exit(main())
