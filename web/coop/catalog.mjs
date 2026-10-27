// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 hahahaudio
// The Vave module's catalogue (web/coop/build.sh): the engines in PRESETS' order, each one's presets (browsing order:
// n -> its preset k, name, sound type) and its EDIT 1 / EDIT 2 knobs (label, range, default, an enum's names), read
// from the built module itself, so the host shows what the module holds.
//   node web/coop/catalog.mjs build/coop/vave.wasm > build/coop/vave-catalog.json
import fs from "node:fs";
const { instance } = await WebAssembly.instantiate(fs.readFileSync(process.argv[2]), {});
const ex = instance.exports;
ex._initialize();
ex.web_nor_erase();
ex.web_boot();
const str = (p) => { if (!p) return null; const m = new Uint8Array(ex.memory.buffer); let e = p; while (m[e]) e++; return new TextDecoder().decode(m.subarray(p, e)); };
const TYPES = ["", "BASS", "LEAD", "PAD", "KEYS", "PLUCK", "BELL", "VOX", "DRUM", "FX"];
const FMT = ["int", "pct", "bipct", "time", "lfohz", "cutoff", "db", "semi", "enum", "bpm", "note", "onoff", "oct", "steps", "pctinv"];
const engines = [];
for (let r = 0; r < ex.coop_neng(); r++) {
  const e = ex.coop_eng_at(r), presets = [], knobs = [];
  for (let n = 0; n < ex.coop_npre(e); n++) {
    const k = ex.coop_pre_k(e, n);
    ex.coop_load(e, k);                  // its EDIT values as the preset sets them
    const v = []; for (let i = 0; i < 8; i++) v.push(ex.coop_get(ex.coop_p_e0() + i));
    presets.push({ k, name: str(ex.coop_pre_name(e, k)), type: TYPES[ex.coop_pre_type(e, k)] || "", v });
  }
  for (let i = 0; i < 8; i++) {
    const kb = { label: str(ex.coop_knob(e, i)), fmt: FMT[ex.coop_knob_fmt(e, i)] || "int",
                 min: ex.coop_knob_min(e, i), max: ex.coop_knob_max(e, i), def: ex.coop_knob_def(e, i) };
    if (kb.fmt === "enum") { kb.names = []; for (let v = kb.min; v <= kb.max; v++) kb.names.push(str(ex.coop_knob_enum(e, i, v))); }
    knobs.push(kb);
  }
  engines.push({ e, name: str(ex.coop_eng_name(e)), pages: [str(ex.coop_page(e, 0)), str(ex.coop_page(e, 1))], knobs, presets });
}
const ver = "JIANT FM (GPL-3.0-only), Co-Operator's Vave module";
process.stdout.write(JSON.stringify({ about: ver, rate: ex.web_sample_rate(), pe0: ex.coop_p_e0(), engines }) + "\n");
