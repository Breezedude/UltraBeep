/*
 * Vario tone of the firmware: the tone curve (firmware/src/app/vario.c, vario_tone), the integration
 * time (vario_integration_tau_s) and the beep cadence with its threshold hysteresis and the early
 * start of lift (vario_audio.c, vario_tone_cut_short). Used by the metrics (how a climb trace
 * sounds) and by the audio replay. Curves are the configurator's JSON ("ultrabip-tones/1").
 */

export const DEFAULT_CURVE = {
  lift: 10, sink: -250, alarm: true, // cm/s
  integration: 25, // vario_integrationtime_int, %
  points: [
    { vz: -500, freq: 62, duration: 450, duty: 50 }, { vz: -350, freq: 75, duration: 450, duty: 100 },
    { vz: -50, freq: 100, duration: 450, duty: 100 }, { vz: 5, freq: 440, duration: 875, duty: 50 },
    { vz: 80, freq: 513, duration: 770, duty: 26 }, { vz: 120, freq: 675, duration: 500, duty: 27 },
    { vz: 300, freq: 845, duration: 370, duty: 30 }, { vz: 800, freq: 1000, duration: 230, duty: 45 },
  ],
};

const HYSTERESIS = 0.05; // m/s, vario_audio.c HYSTERESIS_M_S

/* vario_integration_tau_s: the stock app's 0.3/0.7/1.4/2.8/5.7 s less the fusion's own 0.2 s. */
export function integrationTau(pct) {
  const tau = [0.1, 0.5, 1.2, 2.6, 5.5];
  const p = Math.max(0, Math.min(100, Number(pct) || 0));
  if (p >= 100) return tau[4];
  const i = Math.floor(p / 25);
  return tau[i] + (tau[i + 1] - tau[i]) * ((p % 25) / 25);
}

/* The climb rate the tones follow: first-order filter with the integration time (vario_audio.c). */
export function dampClimb(tSec, vz, pct) {
  const out = new Float32Array(vz.length);
  if (pct == null) { out.set(vz); return out; }
  const tau = integrationTau(pct);
  let y = Number.isFinite(vz[0]) ? vz[0] : 0;
  for (let i = 0; i < vz.length; i++) {
    const dt = i ? tSec[i] - tSec[i - 1] : 0;
    if (Number.isFinite(vz[i])) y = dt > 1 ? vz[i] : y + dt / (tau + dt) * (vz[i] - y);
    out[i] = y;
  }
  return out;
}

/* vario_tone_cut_short: lift starts at once during a weak tick or the sink tone, and a stronger lift ends a pause early. */
function cutShort(curve, climb, liftRunning, onMs, elapsedMs) {
  if (climb * 100 < curve.lift) return false;
  const next = varioTone(curve, climb);
  if (!next.sound) return false;
  if (!liftRunning) return true;
  return elapsedMs >= onMs && elapsedMs >= next.period;
}

/* {sound, freq, period, on} for a climb rate in m/s, as vario_tone(). */
export function varioTone(curve, climb) {
  const vz = climb * 100;
  const lift = vz >= curve.lift;
  const sink = curve.alarm && vz <= curve.sink;
  if (!lift && !sink) return { sound: false, freq: 0, period: 0, on: 0 };
  const p = curve.points;
  let freq, period, duty;
  if (vz <= p[0].vz) ({ freq, duration: period, duty } = p[0]);
  else if (vz >= p[p.length - 1].vz) ({ freq, duration: period, duty } = p[p.length - 1]);
  else {
    let i = 0;
    while (i < p.length - 2 && vz > p[i + 1].vz) i++;
    const span = p[i + 1].vz - p[i].vz;
    const x = span > 0 ? (vz - p[i].vz) / span : 0;
    const lerp = (a, b) => a + (b - a) * x;
    freq = lerp(p[i].freq, p[i + 1].freq);
    period = lerp(p[i].duration, p[i + 1].duration);
    duty = lerp(p[i].duty, p[i + 1].duty);
  }
  period = Math.max(20, period);
  return {
    sound: freq >= 10,
    freq: Math.round(freq),
    period: Math.round(period),
    on: duty >= 99.5 ? Math.round(period) : Math.round(period * duty / 100),
  };
}

/*
 * Beep cadence of vario_audio_tick: a period starts with the tone of the climb rate at that
 * moment and keeps its pitch; the next period follows when it ends. Call next(climb, nowMs)
 * whenever a period may start; it returns the period to play or null (silence).
 */
export function createCadence(curve) {
  const st = { inPeriod: false, curLift: false, start: 0, cur: null };
  return {
    curve,
    next(climb, now) {
      if (st.inPeriod && now - st.start < st.cur.period &&
          !cutShort(curve, climb, st.curLift, st.cur.on, now - st.start)) return null; // the running period goes on
      const lift = curve.lift / 100, sink = curve.sink / 100;
      let c = climb;
      if (st.inPeriod && st.curLift && c < lift && c >= lift - HYSTERESIS) c = lift;
      else if (st.inPeriod && !st.curLift && curve.alarm && c > sink && c <= sink + HYSTERESIS) c = sink;
      const t = varioTone(curve, c);
      if (!t.sound) {
        st.inPeriod = false;
        return null;
      }
      st.cur = t;
      st.curLift = c >= lift;
      st.start = now;
      st.inPeriod = true;
      return { ...t, lift: st.curLift, start: now };
    },
    /* a period sounds or pauses at `now` (the pattern runs); its kind */
    active(now) { return st.inPeriod && now - st.start < st.cur.period ? (st.curLift ? "lift" : "sink") : null; },
  };
}

/*
 * How a climb trace (grid tSec, vz m/s) sounds: share of time with lift beeps, lift beep starts
 * (periods) and lift on/off switches per minute; the trace is damped with the curve's integration
 * time first, as on the device.
 */
export function toneStats(curve, tSec, vzRaw, i0 = 0, i1 = tSec.length) {
  if (i1 - i0 < 2) return { liftShare: NaN, sinkShare: NaN, switchesPerMin: NaN, periodsPerMin: NaN };
  const vz = dampClimb(tSec, vzRaw, curve.integration);
  const cad = createCadence(curve);
  let liftMs = 0, sinkMs = 0, switches = 0, periods = 0;
  let lastLift = false;
  let j = i0;
  const tStart = tSec[i0] * 1000, tEnd = tSec[i1 - 1] * 1000;
  let now = tStart;
  while (now < tEnd) {
    while (j + 1 < i1 && tSec[j + 1] * 1000 <= now) j++;
    const p = cad.next(vz[j], now); // checked every 10 ms like the device's tone tick (5 ms)
    if (p) {
      periods += p.lift ? 1 : 0;
      if (p.lift !== lastLift) switches++;
      lastLift = p.lift;
    } else if (!cad.active(now)) {
      if (lastLift) switches++;
      lastLift = false;
    }
    const kind = cad.active(now);
    if (kind === "lift") liftMs += 10; else if (kind === "sink") sinkMs += 10;
    now += 10;
  }
  const dur = (tEnd - tStart) || 1;
  return {
    liftShare: liftMs / dur,
    sinkShare: sinkMs / dur,
    switchesPerMin: switches / (dur / 60000),
    periodsPerMin: periods / (dur / 60000),
  };
}

/* Configurator JSON ("ultrabip-tones/1": m/s, Hz, ms, %) -> curve in cm/s. */
export function curveFromJson(text) {
  const j = typeof text === "string" ? JSON.parse(text) : text;
  if (!j || !Array.isArray(j.points) || j.points.length < 2) throw new Error("points missing");
  const points = j.points.map((p) => ({
    vz: Math.round(Number(p.climb) * 100), freq: Number(p.frequency), duration: Number(p.period), duty: Number(p.tone),
  })).sort((a, b) => a.vz - b.vz);
  return {
    lift: Math.round(Number(j.liftThreshold ?? 0.1) * 100),
    sink: Math.round(Number(j.sinkThreshold ?? -2.5) * 100),
    integration: j.integration == null ? DEFAULT_CURVE.integration : Number(j.integration),
    alarm: j.sinkAlarm === undefined ? true : !!j.sinkAlarm,
    points,
  };
}
