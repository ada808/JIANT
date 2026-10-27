// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 hahahaudio
// Co-Operator's Vave module in an AudioWorklet: vave.wasm is JIANT FM as a sound module (web/coop/vave_coop.c), one
// track of it a node. After JIANT's browser emulator (web/emu/worklet.js). The host sends it messages, never code:
//   {t: "load", wasm}                     the module's bytes; it answers {t: "ready"} or {t: "error"}
//   {t: "n", at, note, v, hold, bpm}      a note at the context's time `at`, velocity 0..1, held `hold` seconds; the tempo
//   {t: "midi", at, d: [status, d1, d2]}  any channel message on channel 1, at `at`
//   {t: "sound", at, e, k, set}           engine e's preset k into the track, then `set` {param id: value} over it
//   {t: "set", at, set}                   `set` {param id: value} into the track as it is
//   {t: "bpm", v}                         the tempo
//   {t: "panic"}                          every note off, now
// JIANT runs at 44.1 kHz; its frames are resampled to the context's rate (four-point Hermite). Its MIDI is taken
// at the start of each 128-frame half, as on the FM-1; an event goes into the half nearest its time.
const DEV = 128;
if (typeof registerProcessor === "function") {
  class Vave extends AudioWorkletProcessor {
    constructor() {
      super();
      this.ex = null;
      this.q = [];                       // timed events, by `at`
      this.base = -1;                    // the context's time of device frame 0
      this.dev = 0;                      // device frames made
      this.pos = 0;                      // the read position, in device frames
      this.buf = new Float32Array(2 * 4096);   // device frames, a ring: [l, r]
      this.held = 0;
      this.quiet = 0;                    // device frames since the last sound or event
      this.port.onmessage = (e) => this.on(e.data);
    }
    async on(m) {
      if (m.t === "load") {
        try {
          const got = await WebAssembly.instantiate(m.wasm, {});
          const ex = (got.instance || got).exports;   // bytes give {module, instance}; a Module gives the instance
          ex._initialize();
          ex.web_nor_erase();
          ex.web_master(1023);
          if (ex.web_seed) ex.web_seed((Math.random() * 4294967296) >>> 0);
          ex.web_boot();
          ex.web_render(DEV);             // the boot's first millisecond: the main loop running
          this.ex = ex;
          this.port.postMessage({ t: "ready", rate: ex.web_sample_rate() });
        } catch (err) {
          this.port.postMessage({ t: "error", message: String((err && err.message) || err) });
        }
        return;
      }
      if (m.t === "n") {
        if (m.bpm) this.bpm = m.bpm;
        const v = Math.max(1, Math.min(127, Math.round((m.v == null ? 1 : m.v) * 127)));
        this.push({ at: m.at, d: [0x90, m.note & 127, v] });
        this.push({ at: m.at + Math.max(0.005, m.hold || 0.1), d: [0x80, m.note & 127, 0] });
      } else if (m.t === "midi") this.push({ at: m.at, d: m.d });
      else if (m.t === "sound") this.push({ at: m.at || 0, sound: m });
      else if (m.t === "set") this.push({ at: m.at || 0, set: m.set });
      else if (m.t === "bpm") this.bpm = m.v;
      else if (m.t === "panic") { this.q = this.q.filter((e) => e.sound); this.push({ at: 0, d: [0xB0, 123, 0] }); this.push({ at: 0, d: [0xB0, 120, 0] }); }
    }
    push(e) {
      let i = this.q.length;
      while (i > 0 && this.q[i - 1].at > e.at) i--;
      this.q.splice(i, 0, e);
    }
    apply(e) {
      const ex = this.ex;
      this.quiet = 0;
      if (e.set) {
        for (const id in e.set) if (typeof e.set[id] === "number") ex.coop_set(+id, Math.round(e.set[id]));
        return;
      }
      if (e.sound) {
        ex.coop_load(e.sound.e >>> 0, e.sound.k >>> 0);
        const s = e.sound.set || {};
        for (const id in s) if (typeof s[id] === "number") ex.coop_set(+id, Math.round(s[id]));
        return;
      }
      const st = e.d[0] & 0xF0;
      if (st === 0x90 && e.d[2] > 0) this.held++;
      else if (st === 0x80 || (st === 0x90 && !e.d[2])) this.held = Math.max(0, this.held - 1);
      else if (st === 0xB0 && (e.d[1] === 123 || e.d[1] === 120)) this.held = 0;
      ex.web_midi(st, e.d[1] | 0, e.d[2] | 0);
    }
    // one half of the device: the events due by its middle first
    half() {
      const ex = this.ex, mid = this.base + (this.dev + DEV / 2) / 44100;
      while (this.q.length && this.q[0].at <= mid) this.apply(this.q.shift());
      ex.web_render(DEV);
      const l = new Float32Array(ex.memory.buffer, ex.web_out_l(), DEV), r = new Float32Array(ex.memory.buffer, ex.web_out_r(), DEV);
      let pk = 0;
      for (let i = 0; i < DEV; i++) {
        const j = ((this.dev + i) & 4095) * 2;
        this.buf[j] = l[i]; this.buf[j + 1] = r[i];
        const a = Math.abs(l[i]) + Math.abs(r[i]);
        if (a > pk) pk = a;
      }
      this.dev += DEV;
      this.quiet = pk > 1e-5 ? 0 : this.quiet + DEV;
    }
    process(inputs, outputs) {
      const out = outputs[0], L = out[0], R = out[1] || out[0], n = L.length;
      if (!this.ex) return true;
      if (this.bpm && this.bpm !== this.bpmNow) { this.ex.coop_bpm(Math.round(this.bpm)); this.bpmNow = this.bpm; }
      if (this.base < 0) this.base = currentTime;
      const step = 44100 / sampleRate, end = currentTime + n / sampleRate;
      /* Idle: nothing held, nothing due this quantum, two seconds of silence: the device is not run (nothing it would
       * play), and its clock is moved on with the context's so the next event keeps its time. */
      if (!this.held && this.quiet > 88200 && !(this.q.length && this.q[0].at < end + 0.02)) {
        const adv = n * step;
        this.pos += adv;
        this.dev = Math.max(this.dev, Math.ceil(this.pos) + 2);
        return true;
      }
      for (let i = 0; i < n; i++) {
        const p = this.pos, f = Math.floor(p), t = p - f;
        while (this.dev < f + 3) this.half();
        const a = ((f - 1) & 4095) * 2, b = (f & 4095) * 2, c = ((f + 1) & 4095) * 2, d = ((f + 2) & 4095) * 2;
        for (let ch = 0; ch < 2; ch++) {
          const y0 = this.buf[a + ch], y1 = this.buf[b + ch], y2 = this.buf[c + ch], y3 = this.buf[d + ch];
          const c1 = 0.5 * (y2 - y0), c2 = y0 - 2.5 * y1 + 2 * y2 - 0.5 * y3, c3 = 0.5 * (y3 - y0) + 1.5 * (y1 - y2);
          const v = ((c3 * t + c2) * t + c1) * t + y1;
          if (ch) R[i] = v; else L[i] = v;
        }
        this.pos += step;
      }
      return true;
    }
  }
  registerProcessor("vave", Vave);
}
