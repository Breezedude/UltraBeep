/*
 * Vario tone replay (Web Audio). The beep cadence runs in real time as on the device
 * (core/tones.js: period, on time, pitch fixed per beep, threshold hysteresis); the climb rate is
 * taken from the chosen run at the replay position, which advances at the replay speed.
 */
import { createCadence, dampClimb } from "../core/tones.js";

const LOOKAHEAD = 0.12; // s of audio scheduled ahead
const TICK_MS = 25;
const ATTACK = 0.003;

export class Player {
  constructor(app) {
    this.app = app;
    this.ctx = null;
    this.playing = false;
    this.speed = 1;
    this.volume = 0.4;
    this.wave = "square";
    this.source = null; // run whose vz drives the tone
  }

  ensureAudio() {
    if (this.ctx) return;
    this.ctx = new AudioContext();
    this.master = this.ctx.createGain();
    this.master.gain.value = this.volume * 0.3;
    this.master.connect(this.ctx.destination);
    this.osc = this.ctx.createOscillator();
    this.osc.type = this.wave;
    this.env = this.ctx.createGain();
    this.env.gain.value = 0;
    this.osc.connect(this.env).connect(this.master);
    this.osc.start();
  }

  /* replay position (s since the log start) for an audio time */
  posAt(audioT) {
    return this.t0 + (audioT - this.a0) * this.speed;
  }

  climbAt(t) {
    const r = this.source;
    const ctx = this.app.state.ctx;
    if (!r || !r.result || !ctx) return 0;
    const tS = ctx.tS;
    let lo = 0, hi = tS.length - 1;
    while (lo < hi) { const m = (lo + hi + 1) >> 1; if (tS[m] <= t) lo = m; else hi = m - 1; }
    /* the climb rate the tones follow on the device: damped with the integration time */
    const curve = this.app.state.curve;
    if (this.dampedFor !== r.result || this.dampedPct !== curve.integration) {
      this.damped = dampClimb(tS, r.result.vz, curve.integration);
      this.dampedFor = r.result;
      this.dampedPct = curve.integration;
    }
    const v = this.damped[lo];
    return Number.isFinite(v) ? v : 0;
  }

  start(t) {
    this.ensureAudio();
    if (this.ctx.state === "suspended") this.ctx.resume();
    this.stop(true);
    this.cadence = createCadence(this.app.state.curve);
    this.t0 = t;
    this.a0 = this.ctx.currentTime + 0.05;
    this.next = this.a0;
    this.playing = true;
    this.timer = setInterval(() => this.schedule(), TICK_MS);
    this.schedule();
    const frame = () => {
      if (!this.playing) return;
      const t = this.posAt(this.ctx.currentTime);
      if (t >= this.app.state.ctx.tS[this.app.state.ctx.tS.length - 1]) { this.stop(); this.app.onPlayState(false); return; }
      this.app.onPlayPosition(t, this.lastBeep);
      this.raf = requestAnimationFrame(frame);
    };
    this.raf = requestAnimationFrame(frame);
  }

  schedule() {
    const ac = this.ctx;
    const g = this.env.gain;
    while (this.next < ac.currentTime + LOOKAHEAD) {
      const at = Math.max(this.next, ac.currentTime);
      const climb = this.climbAt(this.posAt(at));
      const p = this.cadence.next(climb, at * 1000);
      if (!p) {
        if (this.continuous) { // a continuous tone ends
          g.setValueAtTime(1, at);
          g.linearRampToValueAtTime(0, at + ATTACK);
          this.continuous = false;
        }
        this.lastBeep = null;
        this.next = at + 0.01;
        continue;
      }
      this.osc.frequency.setValueAtTime(p.freq, at);
      const on = p.on / 1000;
      const per = p.period / 1000;
      if (!this.continuous) {
        g.setValueAtTime(0, at);
        g.linearRampToValueAtTime(1, at + ATTACK);
      }
      this.continuous = on >= per; // continuous tone: no gap to the next period (as the device)
      if (!this.continuous) {
        g.setValueAtTime(1, at + on - ATTACK);
        g.linearRampToValueAtTime(0, at + on);
      }
      this.lastBeep = { freq: p.freq, climb };
      this.next = at + per;
    }
  }

  stop(silentOnly = false) {
    if (this.timer) clearInterval(this.timer);
    if (this.raf) cancelAnimationFrame(this.raf);
    this.timer = null;
    this.raf = null;
    if (this.ctx) {
      const g = this.env.gain;
      g.cancelScheduledValues(this.ctx.currentTime);
      g.setValueAtTime(0, this.ctx.currentTime);
    }
    this.continuous = false;
    if (!silentOnly) this.playing = false;
  }

  position() {
    return this.playing && this.ctx ? this.posAt(this.ctx.currentTime) : null;
  }

  setSpeed(s) {
    if (this.playing) {
      const now = this.ctx.currentTime;
      this.t0 = this.posAt(now);
      this.a0 = now;
    }
    this.speed = s;
  }

  setVolume(v) {
    this.volume = v;
    if (this.master) this.master.gain.setTargetAtTime(v * 0.3, this.ctx.currentTime, 0.02);
  }

  setWave(w) {
    this.wave = w;
    if (this.osc) this.osc.type = w;
  }

  setCurve(curve) {
    if (this.cadence) this.cadence = createCadence(curve);
  }
}
