/* Side panels: metrics, sensor health, spectrum, events, automation help. */
/* global uPlot */
import { score, TURN_BINS, SPEED_BINS, ACC_BINS } from "../core/metrics.js";
import { indexRange, spectrum } from "../core/dsp.js";
import { KIND_COLORS, KIND_LABELS } from "../core/flightstate.js";

const f = (v, d = 2) => (Number.isFinite(v) ? v.toFixed(d) : "–");

function el(tag, attrs = {}, ...children) {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (k === "class") e.className = v;
    else if (k.startsWith("on")) e.addEventListener(k.slice(2), v);
    else e.setAttribute(k, v);
  }
  for (const c of children.flat(Infinity)) if (c != null) e.append(c instanceof Node ? c : document.createTextNode(String(c)));
  return e;
}

const METRIC_COLS = [
  { key: "lag", label: "lag s", d: 2, best: "min", doc: "delay against the reference (best correlation)" },
  { key: "corr", label: "corr", d: 3, best: "max", doc: "correlation with the reference at that lag" },
  { key: "rmsAligned", label: "rms", d: 3, best: "min", doc: "RMS error after removing the lag [m/s]" },
  { key: "noiseHF", label: "noise", d: 3, best: "min", doc: "jitter: RMS of the climb minus its 1 s mean [m/s]" },
  { key: "bias", label: "bias", d: 3, best: "abs", doc: "mean error [m/s]" },
  { key: "falseLiftPct", label: "false lift %", d: 2, best: "min", doc: "share of quiet time (ref < lift − 0.2) with climb ≥ lift" },
  { key: "missedLiftPct", label: "missed %", d: 2, best: "min", doc: "share of lift (ref ≥ lift + 0.2) with climb < lift" },
  { key: "rmsGnss", label: "vs GNSS", d: 2, best: "min", doc: "RMS against the GNSS vertical speed (both 1 s means)" },
  { key: "toneSwitchesPerMin", label: "tone sw/min", d: 1, best: "min", doc: "lift tone on/off switches per minute (cadence of the device)" },
  { key: "toneLiftPct", label: "lift tone %", d: 1, best: null, doc: "share of time with lift beeps" },
  { key: "handlingMean", label: "handling", d: 2, best: "min", doc: "largest deviation within 3 s after taps and button presses (mean) [m/s]" },
  { key: "score", label: "score", d: 3, best: "min", doc: "lag + 2·rms + 2·noise + 0.05·false lift % + 0.2·handling (lower is better)" },
];

function trackNote(pane, what) {
  pane.replaceChildren(el("h2", {}, what), el("p", { class: "muted" }, "Not available for an IGC track: it has no sensor samples. Open a raw log (0:/raw/RAWnnnnn.BIN) for the filter metrics, the sensor health and the spectra."));
}

export function renderFlight(pane, app) {
  const ctx = app.state.ctx;
  const st = ctx.states;
  if (!st) { pane.replaceChildren(el("h2", {}, "Flight states"), el("p", { class: "muted" }, "No flight states: the file has no usable GNSS track.")); return; }
  const s = st.summary;
  const kv = (obj) => el("dl", { class: "kv" }, Object.entries(obj).map(([k, v]) => [el("dt", {}, k), el("dd", {}, v)]));
  const sw = (kind) => el("span", { class: "swatch", style: `display:inline-block;margin-right:6px;background:${KIND_COLORS[kind]}` });
  /* takeoffs and landings: dated time, the rules that held, when the device would have noticed */
  const flightEvents = (kind) => st.events.filter((e) => e.kind === kind)
    .map((e) => `${app.fmtTime(e.t, true)} (${(e.rules || []).join(", ")}; detected ${app.fmtTime(e.detectedT ?? e.t, true)})`).join(" · ");
  const noLanding = s.motion ? "not detected" : "not detected (without IMU peaks, as in an IGC file, only the calm rule applies; the device also lands by the impact and ground handling rules)";
  const segs = st.segments.filter((g) => g.i1 - g.i0 >= 5 || g.kind === "spiral" || g.kind === "wingover");
  const head = el("tr", {}, el("th", {}, "start"), el("th", {}, "state"), el("th", {}, "length"), el("th", {}, "gain m"), el("th", {}, "climb m/s"), el("th", {}, "L/D"));
  const rows = segs.map((g) => el("tr", { onclick: () => app.jumpTo(g.t0 + 1), style: "cursor:pointer" },
    el("td", { class: "mono" }, app.fmtTime(g.t0, true)), el("td", {}, sw(g.kind), KIND_LABELS[g.kind]),
    el("td", { class: "mono" }, app.fmtTime(g.t1 - g.t0)), el("td", { class: "mono" }, f(g.gainM, 0)),
    el("td", { class: "mono" }, f(g.meanClimb, 2)), el("td", { class: "mono" }, f(g.meanLD, 1))));
  pane.replaceChildren(
    el("h2", {}, "Flight"),
    kv({
      takeoff: flightEvents("takeoff") || "not detected",
      landing: flightEvents("landing") || noLanding,
      thermals: `${s.thermals} (${(s.thermalSeconds / 60).toFixed(0)} min, +${f(s.thermalGainM, 0)} m, best ${f(s.bestThermalClimb, 1)} m/s mean)`,
      "straight flight": `${s.glides} legs, ${(s.glideSeconds / 60).toFixed(0)} min, median L/D ${f(s.medianLD, 1)}`,
      manoeuvres: `${s.spirals} spirals, ${s.wingovers} wingovers${s.imu ? "" : " (load factor estimated from the track: no wingovers without an IMU)"}`,
      "max load": `${f(s.maxLoad, 2)} g${s.imu ? "" : " (est.)"}`,
      wind: s.wind ? `${f(s.wind.kmh, 0)} km/h from ${f(s.wind.deg, 0)}° (last estimate)` : "no estimate",
      announcements: `${s.announcements} with the default settings`,
    }),
    el("p", { class: "muted" }, "The firmware's detectors replayed on the track with its default settings: takeoff and landing (flight.c; the time they are dated to, the rules that held and the second the device notices them; lines in the charts, markers on the map), circling, climb phases, thermal exit, glide ratio and wind (thermal.c), spirals and wingovers (maneuver.c). Click a row to zoom there."),
    el("h2", {}, `Segments (${segs.length})`),
    el("div", { class: "tbl" }, el("table", {}, el("thead", {}, head), el("tbody", {}, rows))),
    el("h2", {}, `What the device would say (${st.events.length})`),
    el("div", { class: "evlist" }, st.events.map((e) => el("div", { onclick: () => app.jumpTo(e.t) },
      el("span", { class: "t" }, app.fmtTime(e.t, true)), el("span", { class: "muted" }, e.kind), el("span", {}, e.text)))),
    el("button", { onclick: () => app.copy(JSON.stringify({ summary: s, segments: st.segments, events: st.events }, null, 2)) }, "Copy as JSON"),
  );
}

export function renderMetrics(pane, app, rows, range) {
  if (app.state.ctx.kind === "track") { trackNote(pane, "Metrics"); return; }
  rows.forEach((m) => { m.score = score(m); });
  const valid = rows.filter((m) => !m.tooShort);
  const best = {};
  for (const c of METRIC_COLS) {
    if (!c.best) continue;
    const vals = valid.map((m) => m[c.key]).filter(Number.isFinite);
    if (!vals.length) continue;
    best[c.key] = c.best === "max" ? Math.max(...vals) : c.best === "abs" ? vals.reduce((a, b) => (Math.abs(b) < Math.abs(a) ? b : a)) : Math.min(...vals);
  }
  const head = el("tr", {}, el("th", {}, "run"), METRIC_COLS.map((c) => el("th", { title: c.doc }, c.label)));
  const body = rows.map((m) => el("tr", {},
    el("td", {}, el("span", { class: "swatch", style: `display:inline-block;margin-right:6px;background:${(app.runByLabel(m.label) || {}).color || "#9aa4ad"}` }), m.label),
    METRIC_COLS.map((c) => el("td", { class: `mono${best[c.key] !== undefined && m[c.key] === best[c.key] ? " best" : ""}` }, m.tooShort ? "" : f(m[c.key], c.d)))));
  const rangeText = range ? `${app.fmtTime(range[0], true)} – ${app.fmtTime(range[1], true)} (${f(range[1] - range[0], 0)} s, zoomed)` : "whole log";
  const binTable = (title, key, edges, unit) => {
    const hdr = el("tr", {}, el("th", {}, `${title}`), edges.map(([lo, hi]) => el("th", {}, `${lo}${hi === Infinity ? "+" : `–${hi}`} ${unit}`)));
    const rws = valid.map((m) => el("tr", {}, el("td", {}, m.label), m[key].map((b) => el("td", { class: "mono", title: `n = ${b.n}, mean ${f(b.mean, 3)}` }, b.n > 30 ? f(b.rms, 3) : "–"))));
    return el("div", { class: "tbl" }, el("table", {}, el("thead", {}, hdr), el("tbody", {}, rws)));
  };
  pane.replaceChildren(
    el("h2", {}, `Metrics: ${rangeText}`),
    el("p", { class: "muted" }, `Reference: least-squares slope of the barometric altitude ±${app.state.ctx.refHalf} s. Windows around taps and button presses (−1 … +4 s) are left out (${f(valid[0]?.excludedSeconds, 0)} s).`),
    el("div", { class: "tbl" }, el("table", {}, el("thead", {}, head), el("tbody", {}, body))),
    el("h2", {}, "RMS error (lag removed) by condition"),
    binTable("rotation", "turnBins", TURN_BINS, "°/s"),
    binTable("ground speed", "speedBins", SPEED_BINS, "m/s"),
    binTable("| |a| − g |", "accBins", ACC_BINS, "m/s²"),
    el("dl", { class: "metric-doc" }, METRIC_COLS.map((c) => [el("dt", {}, c.label), el("dd", {}, c.doc)])),
    el("button", { onclick: () => app.copy(JSON.stringify({ range, metrics: rows }, null, 2)) }, "Copy as JSON"),
  );
}

export function renderHealth(pane, app, health) {
  if (!health) { trackNote(pane, "Sensor health"); return; }
  const s = health.summary;
  const kv = (obj) => el("dl", { class: "kv" }, Object.entries(obj).map(([k, v]) => [el("dt", {}, k), el("dd", {}, v)]));
  const findings = health.findings.map((x) => el("div", { class: `finding ${x.level}` },
    el("div", {}, el("span", { class: "lvl" }, x.level), el("b", {}, `${x.sensor}: ${x.title}`)),
    x.detail ? el("div", { class: "muted" }, x.detail) : null,
    x.times.length ? el("div", { class: "chips" }, x.times.slice(0, 40).map((t) =>
      el("button", { class: "chip", onclick: () => app.jumpTo(t) }, app.fmtTime(t, true))), x.times.length > 40 ? el("span", { class: "muted" }, `+${x.times.length - 40}`) : null) : null));
  const bySpeed = s.baro.noiseBySpeed.map((b) => `${b.lo}${b.hi === Infinity ? "+" : "–" + b.hi} m/s: ${f(b.noise * 100, 1)} cm`).join(" · ");
  const gyroBias = s.imu.restGyroBias ? s.imu.restGyroBias.map((v) => f(v, 2)).join(" / ") + " °/s" : "–";
  pane.replaceChildren(
    el("h2", {}, "Findings"),
    ...(findings.length ? findings : [el("p", { class: "muted" }, "Nothing found.")]),
    el("h2", {}, "Log"),
    kv({ firmware: s.log.firmware, duration: `${f(s.log.durationS / 60, 1)} min`, size: `${f(s.log.sizeBytes / 1e6, 1)} MB`, records: s.log.records, "clock set": s.log.clockSet ? "yes" : "no" }),
    el("h2", {}, "Streams"),
    kv({
      barometer: `${f(s.baroStream.rate, 2)} Hz, ${s.baroStream.gaps.length} gaps, jitter p99 ${f(s.baroStream.jitterP99, 1)} ms`,
      IMU: `${f(s.imuStream.rate, 2)} Hz, ${s.imuStream.gaps.length} gaps, jitter p99 ${f(s.imuStream.jitterP99, 1)} ms`,
      GNSS: `${s.gnss.epochs} epochs, ${s.gnss.rate}/s, ${s.gnss.valid} valid`,
    }),
    el("h2", {}, "Barometer"),
    kv({
      "noise per sample": `median ${f(s.baro.noiseMedian * 100, 1)} cm, p95 ${f(s.baro.noiseP95 * 100, 1)} cm`,
      "noise by ground speed": bySpeed,
      "spikes / steps": `${s.baro.spikes} / ${s.baro.steps}`,
      temperature: `${f(s.baro.tempMin, 1)} … ${f(s.baro.tempMax, 1)} °C`,
      "vs GNSS": `drift ${f(s.gnss.baroGnssDriftM, 1)} m, offset sd ${f(s.gnss.baroGnssOffsetSd, 1)} m`,
    }),
    el("h2", {}, "IMU"),
    kv({
      "rest windows": s.imu.restWindows,
      "|a| at rest": `${f(s.imu.restG, 3)} m/s² (${f((s.imu.restG / 9.80665 - 1) * 100, 2)} %)`,
      "gyro bias at rest": gyroBias,
      "largest |a|": `${f(s.imu.accMax / 9.80665, 2)} g`,
      clipping: `accel ${s.imu.clipping.accel.join("/")}, gyro ${s.imu.clipping.gyro.join("/")}`,
      "stuck runs": s.imu.stuckRuns,
    }),
    el("h2", {}, "GNSS"),
    kv({
      "first 3D fix": Number.isFinite(s.gnss.ttffS) ? `${f(s.gnss.ttffS, 1)} s after the start` : "none",
      "fix losses": s.gnss.losses, "jumps (pos / height)": `${s.gnss.jumps} / ${s.gnss.altJumps}`,
      HDOP: `median ${f(s.gnss.hdopMedian, 1)}, p95 ${f(s.gnss.hdopP95, 1)}`,
      satellites: `median ${f(s.gnss.satsMedian, 0)}, min ${f(s.gnss.satsMin, 0)}`,
      "≥ 30 dB-Hz": `median ${f(s.gnss.goodSatsMedian, 0)}`, "top-4 C/N0": `${f(s.gnss.top4SnrMedian, 1)} dB-Hz`,
      sentences: Object.entries(s.gnss.sentences).map(([k, v]) => `${k} ${v}`).join(", "),
      "checksum errors": s.gnss.checksumErrors,
    }),
    el("h2", {}, "Events"),
    kv({ total: s.events.total, taps: s.events.taps, "button presses": s.events.buttons, "voice underruns": s.events.underruns }),
  );
}

let specPlot = null;
export function renderSpectrum(pane, app) {
  const ctx = app.state.ctx;
  if (ctx.kind === "track") { trackNote(pane, "Spectrum"); return; }
  const sel = el("select", {},
    ["accel |a| (IMU 100 Hz)", "rotation |ω| (IMU 100 Hz)", "barometric altitude (32 Hz)", ...app.state.runs.filter((r) => r.result).map((r) => `climb: ${r.label}`)]
      .map((t, i) => el("option", { value: i }, t)));
  sel.value = pane.dataset.sel || "0";
  const plotDiv = el("div");
  const info = el("p", { class: "muted" });
  const draw = () => {
    pane.dataset.sel = sel.value;
    const k = Number(sel.value);
    const range = app.state.range || [ctx.tS[0], ctx.tS[ctx.tS.length - 1]];
    let x, fs, unit;
    if (k <= 1) {
      const [i0, i1] = indexRange(ctx.imuT, range[0], range[1]);
      x = (k === 0 ? ctx.imuAcc : ctx.imuGyro).subarray(i0, i1);
      fs = ctx.imuRate;
      unit = k === 0 ? "m/s²/√Hz" : "°/s/√Hz";
    } else {
      const [i0, i1] = indexRange(ctx.tS, range[0], range[1]);
      fs = ctx.baroRate;
      if (k === 2) { x = ctx.altIsa.subarray(i0, i1); unit = "m/√Hz"; } else {
        const r = app.state.runs.filter((q) => q.result)[k - 3];
        x = r.result.vz.subarray(i0, i1);
        unit = "m/s/√Hz";
      }
    }
    const seg = Math.min(4096, 1 << Math.floor(Math.log2(Math.max(64, x.length / 4))));
    const sp = spectrum(x, fs, seg);
    const fr = Array.from(sp.f).slice(1), amp = Array.from(sp.amp).slice(1);
    info.textContent = `Welch, ${sp.segments} segments of ${seg} samples (${f(seg / fs, 1)} s), resolution ${f(fs / seg, 3)} Hz; unit ${unit}. Range: the zoomed part of the charts.`;
    if (specPlot) specPlot.destroy();
    specPlot = new uPlot({
      width: Math.max(300, pane.clientWidth - 24), height: 300,
      scales: { x: { time: false, distr: 3 }, y: { distr: 3 } },
      axes: [{ stroke: "#8796a3", grid: { stroke: "#1f2a33" }, label: "Hz" }, { stroke: "#8796a3", grid: { stroke: "#1f2a33" }, size: 60 }],
      series: [{ label: "Hz", value: (u, v) => f(v, 3) }, { label: unit, stroke: "#4fb3ff", width: 1, value: (u, v) => (v == null ? "–" : v.toExponential(2)) }],
    }, [fr, amp], plotDiv);
  };
  sel.addEventListener("change", draw);
  pane.replaceChildren(el("h2", {}, "Amplitude spectral density"), sel, plotDiv, info,
    el("p", { class: "muted" }, "Look for vibration peaks (engine, airflow, harness), the pendulum mode (≈ 0.2 Hz in flight) and noise floors (ICM-42605 accel ≈ 70 µg/√Hz = 7·10⁻⁴ m/s²/√Hz)."));
  draw();
}

export function renderEvents(pane, app) {
  const ctx = app.state.ctx;
  const all = [
    ...ctx.events.map((e) => ({ ...e, kind: "event" })),
    ...ctx.messages.map((m) => ({ ...m, kind: "gps" })),
    ...(ctx.states ? ctx.states.events.map((e) => ({ t: e.t, text: e.text, kind: "flight" })) : []),
  ].sort((a, b) => a.t - b.t);
  pane.replaceChildren(el("h2", {}, `Events and GPS module messages (${all.length})`),
    el("div", { class: "evlist" }, all.map((e) => el("div", { onclick: () => app.jumpTo(e.t) },
      el("span", { class: "t" }, app.fmtTime(e.t, true)), el("span", { class: "muted" }, e.kind), el("span", {}, e.text)))));
}

export function renderApi(pane) {
  pane.replaceChildren(
    el("h2", {}, "Automation"),
    el("p", {}, "The page offers window.UBA for scripts (browser console, test tools); the same modules run on the command line."),
    el("pre", { class: "code" }, `// browser
await UBA.whenReady()                         // page set up, ?log= loaded
await UBA.load("/tools/Auto.BIN")            // URL or ArrayBuffer
UBA.algorithms()                              // ids and parameters
UBA.runs()                                    // current runs
await UBA.addRun({ algo: "fusion", params: { KP: 0.5 }, label: "KP 0.5" })
await UBA.updateRun("KP 0.5", { ACCEL_GATE: 0.1 })
UBA.setRange(300, 900)                        // seconds since the log start
UBA.metrics()                                 // all runs, current range
UBA.health()                                  // sensor findings
await UBA.sweep({ algo: "fusion", grid: { KP: [0.5, 1, 2] }, range: [300, 900] })
UBA.flightStates()                            // segments, events (announcements) and summary; also for IGC tracks
UBA.stateAt(600)                              // the state and derived values at a time
UBA.play(600, 2); UBA.setToneSource("KP 0.5"); UBA.pause()

// command line (Node 18+), from tools/analyzer
node cli.mjs info LOG
node cli.mjs run LOG --algo fusion --algo fusion:KP=0.5,ACCEL_GATE=0.1 --range 300-900 --json
node cli.mjs sweep LOG --algo fusion --grid KP=0.5,1,2 --grid FUSION_ACCEL_NOISE=0.3,0.5 --top 5
node cli.mjs states FLIGHT.igc --json`),
    el("p", { class: "muted" }, "Page parameters: ?log=URL loads a log at start (with a local server), e.g. ?log=/tools/Auto.BIN."),
    el("h2", {}, "Adding an algorithm"),
    el("p", {}, "Write a module in js/algorithms/ (see js/core/registry.js for the interface: imu, baro, fix, event, vz, h, diag) and add it to the builtin list; private ones go to private/index.js (git-ignored)."),
  );
}
