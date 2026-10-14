"use strict";

/* Vario tone curve of a profile: the values with the device's defaults, the device's interpolation
 * (firmware src/app/vario.c, vario_tone), JSON export and import, an interactive editor, and a preview
 * in the browser (Web Audio). Climb values are cm/s in the configuration and m/s on screen. */

const TONE_DEFAULT_LIFT = 10;   /* cm/s */
const TONE_DEFAULT_SINK = -250; /* cm/s */
const TONE_WEAK_BAND = 40;      /* cm/s below the lift threshold: the weak-lift tick (firmware VARIO_WEAK_BAND_CMS) */

/* The profile's switches the tone test carries along (the device tests the curve as shown, saved or not). */
function toneFlags(n) {
  const bool = (key, def) => { const v = getP(n, key); return v === undefined ? def : isTrue(v); };
  const integration = parseInt(getP(n, "vario_integrationtime_int"), 10);
  return { lifty: bool("vario_liftysound_bool", true), sweep: bool("vario_sweeptone_bool", true),
    integration: Number.isNaN(integration) ? 25 : integration };
}

function readTones(n) {
  const num = (key, def) => {
    const v = parseInt(getP(n, key), 10);
    return Number.isNaN(v) ? def : v;
  };
  const alarmText = getP(n, "vario_sinkalarm_bool");
  return {
    lift: num("vario_liftstart_int", TONE_DEFAULT_LIFT),
    sink: num("vario_sinkstart_int", TONE_DEFAULT_SINK),
    alarm: alarmText === undefined ? true : isTrue(alarmText),
    ...toneFlags(n),
    points: STOCK_TONES.map((d, i) => ({
      vz: num(`vario_tones_vz_${i}`, d.vz),
      freq: num(`vario_tones_freq_${i}`, d.freq),
      duration: num(`vario_tones_duration_${i}`, d.duration),
      duty: num(`vario_tones_duty_${i}`, d.duty),
    })),
  };
}

/* Writes the keys whose values changed. */
function writeTones(n, curve) {
  const put = (key, value) => {
    if (getP(n, key) !== String(value)) setP(n, key, String(value));
  };
  put("vario_liftstart_int", curve.lift);
  put("vario_sinkstart_int", curve.sink);
  put("vario_sinkalarm_bool", curve.alarm ? "true" : "false");
  curve.points.forEach((p, i) => {
    put(`vario_tones_vz_${i}`, p.vz);
    put(`vario_tones_freq_${i}`, p.freq);
    put(`vario_tones_duration_${i}`, p.duration);
    put(`vario_tones_duty_${i}`, p.duty);
  });
}

function stockCurve() {
  return {
    lift: TONE_DEFAULT_LIFT, sink: TONE_DEFAULT_SINK, alarm: true,
    points: STOCK_TONES.map((p) => ({ vz: p.vz, freq: p.freq, duration: p.duration, duty: p.duty })),
  };
}

function cloneCurve(c) {
  return { lift: c.lift, sink: c.sink, alarm: c.alarm, lifty: c.lifty, sweep: c.sweep, integration: c.integration,
    points: c.points.map((p) => ({ ...p })) };
}

/* Same result as the firmware: {sound, freq, period, on} for a climb rate in m/s. */
function varioTone(curve, climb) {
  const vz = climb * 100;
  const lift = vz >= curve.lift;
  const sink = curve.alarm && vz <= curve.sink;
  if (!lift && !sink) return { sound: false };
  const p = curve.points;
  let freq, period, duty;
  if (vz <= p[0].vz) {
    ({ freq, duration: period, duty } = p[0]);
  } else if (vz >= p[p.length - 1].vz) {
    ({ freq, duration: period, duty } = p[p.length - 1]);
  } else {
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
    on: duty >= 99.5 ? Math.round(period) : Math.round((period * duty) / 100),
  };
}

/* Text form of the Bluetooth command TONES (firmware vario_tones_parse). */
function tonesCommand(curve) {
  const pts = curve.points.map((q) => `${q.vz},${q.freq},${q.duration},${q.duty}`).join(" ");
  /* the weak-lift, sweep and integration settings as edited, so the test plays what is shown (firmware 0.46.1) */
  const flags = curve.lifty === undefined ? "" : ` ${curve.lifty ? 1 : 0} ${curve.sweep ? 1 : 0} ${curve.integration}`;
  return `TONES ${curve.lift} ${curve.sink} ${curve.alarm ? 1 : 0} ${pts}${flags}`;
}

/* ---- JSON: m/s, Hz, ms and %, one point per line ---- */

function tonesToJson(curve) {
  const pts = curve.points.map((p) =>
    `    { "climb": ${(p.vz / 100).toFixed(2)}, "frequency": ${p.freq}, "period": ${p.duration}, "tone": ${p.duty} }`);
  return `{\n  "format": "ultrabip-tones/1",\n  "liftThreshold": ${(curve.lift / 100).toFixed(2)},\n` +
    `  "sinkAlarm": ${curve.alarm},\n  "sinkThreshold": ${(curve.sink / 100).toFixed(2)},\n` +
    `  "points": [\n${pts.join(",\n")}\n  ]\n}`;
}

/* Parses and checks a pasted curve; throws an Error with a readable message. */
function tonesFromJson(text) {
  let j;
  try {
    j = JSON.parse(text);
  } catch (e) {
    throw new Error(`Not valid JSON: ${e.message}`);
  }
  const num = (v, what, min, max) => {
    const x = Number(v);
    if (!Number.isFinite(x)) throw new Error(`${what} is missing or not a number`);
    return Math.min(max, Math.max(min, x));
  };
  if (!j || !Array.isArray(j.points) || j.points.length !== TONE_POINTS) {
    throw new Error(`"points" must be a list of exactly ${TONE_POINTS} points`);
  }
  const points = j.points.map((p, i) => ({
    vz: Math.round(num(p.climb, `Point ${i + 1}: climb`, -20, 20) * 100),
    freq: Math.round(num(p.frequency, `Point ${i + 1}: frequency`, 0, 5000)),
    duration: Math.round(num(p.period, `Point ${i + 1}: period`, 20, 5000)),
    duty: Math.round(num(p.tone, `Point ${i + 1}: tone`, 0, 100)),
  })).sort((a, b) => a.vz - b.vz);
  const lift = Math.round(num(j.liftThreshold ?? TONE_DEFAULT_LIFT / 100, "liftThreshold", 0, 2) * 100);
  const sink = Math.round(num(j.sinkThreshold ?? TONE_DEFAULT_SINK / 100, "sinkThreshold", -20, 0) * 100);
  return { lift, sink, alarm: j.sinkAlarm === undefined ? true : !!j.sinkAlarm, points };
}

/* ---- interactive editor ---- */

const SVG_NS = "http://www.w3.org/2000/svg";

function svg(tag, attrs = {}, ...children) {
  const e = document.createElementNS(SVG_NS, tag);
  for (const [k, v] of Object.entries(attrs)) e.setAttribute(k, v);
  for (const c of children) e.append(c instanceof Node ? c : document.createTextNode(String(c)));
  return e;
}

const TONE_X_MIN = -6;    /* m/s shown at least */
const TONE_X_MAX = 10;
const TONE_RATE_MAX = 10; /* beeps per second at the top */
const TONE_GAP = 0.05;    /* m/s between neighbouring points */

const fmtClimb = (v) => `${v >= 0 ? "+" : "−"}${Math.abs(v).toFixed(2)} m/s`;
const snap = (v, step) => Math.round(v / step) * step;
const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));

/*
 * Editor for the tone curve. Edit mode: drag a point's circle (climb and frequency), its diamond (beeps
 * per second), its handle in the lower lane (tone length) or the two flags (lift threshold, sink alarm);
 * arrow keys move the focused handle. Hover mode (test): a bar follows the pointer, onClimb gets its
 * climb rate. Live mode: setMarker() places the bar.
 *   opts.curve, opts.onEdit(curve, final), opts.onClimb(climb), opts.onSelect(text)
 */
function createToneEditor(opts) {
  let curve = cloneCurve(opts.curve);
  let mode = "edit";
  let marker = null;
  let selected = -1;
  let width = 0;
  let dom = null;
  let drag = null;
  const P = {};
  const root = document.createElement("div");
  root.className = "tone-editor";

  const domain = () => {
    const xs = curve.points.map((p) => p.vz / 100).concat([curve.lift / 100, curve.alarm ? curve.sink / 100 : 0]);
    /* four grid divisions with round values, shared with the beeps per second (0, 2.5, ... 10) */
    const top = Math.max(...curve.points.map((p) => p.freq)) * 1.1;
    const fmax = [1000, 1200, 1600, 2000, 2400, 3200, 4000, 6000].find((f) => f >= top) || 6000;
    return { xmin: Math.min(TONE_X_MIN, Math.floor(Math.min(...xs)) - 1), xmax: Math.max(TONE_X_MAX, Math.ceil(Math.max(...xs)) + 1), fmax };
  };

  /* layout in px */
  const lay = () => {
    const small = width < 520;
    const L = small ? 38 : 48, R = small ? 34 : 44, T = 38, HM = small ? 170 : 210, GAP = 28, HL = 44;
    const lane0 = T + HM + GAP;
    return { L, R, T, HM, lane0, HL, H: lane0 + HL + 10, small };
  };
  let G;
  const X = (v) => G.L + ((v - dom.xmin) / (dom.xmax - dom.xmin)) * (width - G.L - G.R);
  const Xi = (px) => dom.xmin + ((px - G.L) / (width - G.L - G.R)) * (dom.xmax - dom.xmin);
  const Yf = (f) => G.T + (1 - Math.min(f, dom.fmax) / dom.fmax) * G.HM;
  const Yfi = (py) => (1 - (py - G.T) / G.HM) * dom.fmax;
  const Yr = (r) => G.T + (1 - Math.min(r, TONE_RATE_MAX) / TONE_RATE_MAX) * G.HM;
  const Yri = (py) => (1 - (py - G.T) / G.HM) * TONE_RATE_MAX;
  const Yd = (d) => G.lane0 + (1 - d / 100) * G.HL;
  const Ydi = (py) => (1 - (py - G.lane0) / G.HL) * 100;
  const rate = (p) => 1000 / p.duration;

  const describe = (i) => {
    const p = curve.points[i];
    return `Point ${i + 1}: ${fmtClimb(p.vz / 100)} · ${p.freq} Hz · ${rate(p).toFixed(1)} beeps/s (${p.duration} ms) · ` +
      `tone ${p.duty} %`;
  };
  const select = (i) => {
    selected = i;
    if (opts.onSelect) opts.onSelect(i >= 0 ? describe(i) : "");
    for (const [k, list] of [["fh", P.fh], ["rh", P.rh], ["dh", P.dh]]) {
      list.forEach((h, j) => h.classList.toggle("selected", j === i));
    }
  };

  function build() {
    width = Math.max(300, Math.round(root.clientWidth || 600));
    G = lay();
    dom = domain();
    const s = svg("svg", { width, height: G.H, class: "tone-svg", role: "group",
      "aria-label": "Vario tone curve editor" });
    const right = width - G.R;
    /* plot backgrounds and grid */
    s.append(svg("rect", { x: G.L, y: G.T, width: right - G.L, height: G.HM, class: "plot" }));
    s.append(svg("rect", { x: G.L, y: G.lane0, width: right - G.L, height: G.HL, class: "plot" }));
    P.silent = svg("rect", { class: "silent", y: G.T, height: G.HM });
    P.silentLane = svg("rect", { class: "silent", y: G.lane0, height: G.HL });
    s.append(P.silent, P.silentLane);
    /* the weak-lift tick: the band below the lift threshold where it plays */
    P.weak = svg("rect", { class: "weak", y: G.T, height: G.HM });
    P.weakLane = svg("rect", { class: "weak", y: G.lane0, height: G.HL });
    P.weakText = svg("text", { y: G.T + 14, class: "weak-text", "text-anchor": "middle" }, "weak lift");
    s.append(P.weak, P.weakLane, P.weakText);
    const pxPerMs = (right - G.L) / (dom.xmax - dom.xmin);
    const every = pxPerMs < 26 ? 2 : 1;
    for (let c = Math.ceil(dom.xmin); c <= dom.xmax; c++) {
      s.append(svg("line", { x1: X(c), x2: X(c), y1: G.T, y2: G.T + G.HM, class: c === 0 ? "grid zero" : "grid" }));
      s.append(svg("line", { x1: X(c), x2: X(c), y1: G.lane0, y2: G.lane0 + G.HL, class: c === 0 ? "grid zero" : "grid" }));
      if (c % every === 0) {
        s.append(svg("text", { x: X(c), y: G.T + G.HM + 17, class: "tick", "text-anchor": "middle" }, c > 0 ? `+${c}` : c));
      }
    }
    for (let k = 0; k <= 4; k++) {
      const f = (dom.fmax * k) / 4;
      s.append(svg("line", { x1: G.L, x2: right, y1: Yf(f), y2: Yf(f), class: "grid" }));
      s.append(svg("text", { x: G.L - 6, y: Yf(f) + 4, class: "tick freq", "text-anchor": "end" }, Math.round(f)));
      s.append(svg("text", { x: right + 6, y: Yr((TONE_RATE_MAX * k) / 4) + 4, class: "tick rate" },
        (TONE_RATE_MAX * k) / 4));
    }
    s.append(svg("text", { x: G.L - 6, y: G.T - 8, class: "axis-label freq", "text-anchor": "end" }, "Hz"));
    s.append(svg("text", { x: right + 6, y: G.T - 8, class: "axis-label rate" }, "/s"));
    s.append(svg("text", { x: G.L - 6, y: G.lane0 + G.HL / 2 + 4, class: "axis-label duty", "text-anchor": "end" }, "%"));
    s.append(svg("text", { x: right + 6, y: G.T + G.HM + 17, class: "axis-label" }, "m/s"));
    /* curves: faint through all points, strong where the vario sounds */
    P.fAll = svg("path", { class: "curve-all freq" });
    P.rAll = svg("path", { class: "curve-all rate" });
    P.fOn = svg("path", { class: "curve-on freq" });
    P.rOn = svg("path", { class: "curve-on rate" });
    P.dAll = svg("path", { class: "curve-on duty" });
    s.append(P.fAll, P.rAll, P.rOn, P.fOn, P.dAll);
    /* thresholds */
    const flag = (cls, label) => {
      const g = svg("g", { class: `flag ${cls}`, tabindex: 0, role: "slider", "aria-label": label });
      const line = svg("line", { y1: G.T - 6, y2: G.lane0 + G.HL, class: "flag-line" });
      const pill = svg("rect", { y: 4, height: 20, rx: 10, class: "flag-pill" });
      const text = svg("text", { y: 18, class: "flag-text", "text-anchor": "middle" }, "");
      g.append(line, pill, text);
      g.parts = { line, pill, text };
      s.append(g);
      return g;
    };
    P.sink = flag("sink", "Sink alarm threshold");
    P.lift = flag("lift", "Lift threshold");
    /* handles */
    P.fh = []; P.rh = []; P.dh = [];
    curve.points.forEach((p, i) => {
      const rh = svg("rect", { width: 10, height: 10, class: "handle rate", tabindex: 0, role: "slider",
        "aria-label": `Point ${i + 1}: beeps per second` });
      const dh = svg("circle", { r: 5.5, class: "handle duty", tabindex: 0, role: "slider",
        "aria-label": `Point ${i + 1}: tone length` });
      const fh = svg("circle", { r: 7, class: "handle freq", tabindex: 0, role: "slider",
        "aria-label": `Point ${i + 1}: climb and frequency` });
      P.rh.push(rh); P.dh.push(dh); P.fh.push(fh);
      s.append(rh, dh, fh);
      for (const [h, kind] of [[fh, "f"], [rh, "r"], [dh, "d"]]) wireHandle(h, kind, i);
    });
    wireHandle(P.lift, "lift", -1);
    wireHandle(P.sink, "sink", -1);
    /* test marker */
    P.marker = svg("g", { class: "marker" });
    P.markerLine = svg("line", { y1: G.T, y2: G.lane0 + G.HL });
    P.markerPill = svg("rect", { y: G.T + 6, height: 22, rx: 11 });
    P.markerText = svg("text", { y: G.T + 21, "text-anchor": "middle" }, "");
    P.marker.append(P.markerLine, P.markerPill, P.markerText);
    s.append(P.marker);
    /* pointer area for the test bar */
    P.hover = svg("rect", { x: G.L, y: G.T, width: right - G.L, height: G.lane0 + G.HL - G.T, class: "hover-area" });
    s.append(P.hover);
    P.hover.addEventListener("pointermove", onHover);
    P.hover.addEventListener("pointerdown", onHover);
    P.svg = s;
    root.replaceChildren(s);
    root.classList.toggle("testing", mode !== "edit");
    update();
    select(selected);
  }

  function update() {
    const right = width - G.R;
    const sinkX = curve.alarm ? X(curve.sink / 100) : G.L;
    const liftX = X(curve.lift / 100);
    for (const r of [P.silent, P.silentLane]) {
      r.setAttribute("x", sinkX);
      r.setAttribute("width", Math.max(0, liftX - sinkX));
    }
    const weakOn = !!curve.lifty;
    const weakX = X((curve.lift - TONE_WEAK_BAND) / 100);
    for (const r of [P.weak, P.weakLane]) {
      r.style.display = weakOn ? "" : "none";
      r.setAttribute("x", weakX);
      r.setAttribute("width", Math.max(0, liftX - weakX));
    }
    P.weakText.style.display = weakOn && liftX - weakX > 52 ? "" : "none";
    P.weakText.setAttribute("x", (weakX + liftX) / 2);
    /* The pills sit above their lines; when they would overlap they move apart (the lines stay). */
    const liftText = G.small ? `lift ${fmtClimb(curve.lift / 100).replace(" m/s", "")}` : `lift ${fmtClimb(curve.lift / 100)}`;
    const sinkText = G.small ? `sink ${fmtClimb(curve.sink / 100).replace(" m/s", "")}`
      : `sink alarm ${fmtClimb(curve.sink / 100)}`;
    const wl = liftText.length * 6.6 + 18, ws = sinkText.length * 6.6 + 18;
    let lc = clamp(liftX, wl / 2 + 2, width - wl / 2 - 2);
    let sc = clamp(sinkX, ws / 2 + 2, width - ws / 2 - 2);
    const need = wl / 2 + ws / 2 + 6;
    if (curve.alarm && lc - sc < need) {
      const mid = (lc + sc) / 2;
      sc = mid - need / 2;
      lc = mid + need / 2;
      if (sc < ws / 2 + 2) { lc += ws / 2 + 2 - sc; sc = ws / 2 + 2; }
      if (lc > width - wl / 2 - 2) { sc -= lc - (width - wl / 2 - 2); lc = width - wl / 2 - 2; }
    }
    const setFlag = (g, x, cx, w, text, show) => {
      g.style.display = show ? "" : "none";
      g.parts.line.setAttribute("x1", x);
      g.parts.line.setAttribute("x2", x);
      g.parts.pill.setAttribute("x", cx - w / 2);
      g.parts.pill.setAttribute("width", w);
      g.parts.text.setAttribute("x", cx);
      g.parts.text.textContent = text;
      g.setAttribute("aria-valuetext", text);
    };
    setFlag(P.lift, liftX, lc, wl, liftText, true);
    setFlag(P.sink, sinkX, sc, ws, sinkText, curve.alarm);
    /* curves */
    const pts = curve.points;
    const line = (pairs) => pairs.map(([x, y], k) => `${k ? "L" : "M"}${x.toFixed(1)},${y.toFixed(1)}`).join("");
    P.fAll.setAttribute("d", line(pts.map((p) => [X(p.vz / 100), Yf(p.freq)])));
    P.rAll.setAttribute("d", line(pts.map((p) => [X(p.vz / 100), Yr(rate(p))])));
    P.dAll.setAttribute("d", line([[G.L, Yd(pts[0].duty)], ...pts.map((p) => [X(p.vz / 100), Yd(p.duty)]),
      [right, Yd(pts[pts.length - 1].duty)]]));
    let fd = "", rd = "", pen = false;
    const steps = Math.max(200, Math.round(width / 2));
    for (let k = 0; k <= steps; k++) {
      const c = dom.xmin + ((dom.xmax - dom.xmin) * k) / steps;
      const t = varioTone(curve, c);
      if (!t.sound) { pen = false; continue; }
      fd += `${pen ? "L" : "M"}${X(c).toFixed(1)},${Yf(t.freq).toFixed(1)}`;
      rd += `${pen ? "L" : "M"}${X(c).toFixed(1)},${Yr(1000 / t.period).toFixed(1)}`;
      pen = true;
    }
    P.fOn.setAttribute("d", fd);
    P.rOn.setAttribute("d", rd);
    pts.forEach((p, i) => {
      const x = X(p.vz / 100);
      P.fh[i].setAttribute("cx", x);
      P.fh[i].setAttribute("cy", Yf(p.freq));
      P.rh[i].setAttribute("x", x - 5);
      P.rh[i].setAttribute("y", Yr(rate(p)) - 5);
      P.rh[i].setAttribute("transform", `rotate(45 ${x} ${Yr(rate(p))})`);
      P.dh[i].setAttribute("cx", x);
      P.dh[i].setAttribute("cy", Yd(p.duty));
      P.fh[i].setAttribute("aria-valuetext", `${fmtClimb(p.vz / 100)}, ${p.freq} Hz`);
      P.rh[i].setAttribute("aria-valuetext", `${rate(p).toFixed(1)} beeps per second`);
      P.dh[i].setAttribute("aria-valuetext", `tone ${p.duty} %`);
    });
    drawMarker();
  }

  function drawMarker() {
    if (marker === null || mode === "edit") {
      P.marker.style.display = "none";
      return;
    }
    const c = clamp(marker, dom.xmin, dom.xmax);
    const x = X(c);
    const t = varioTone(curve, marker);
    const text = `${fmtClimb(marker)} · ${t.sound ? `${t.freq} Hz · ${(1000 / t.period).toFixed(1)}/s` : "silent"}`;
    const w = text.length * 6.6 + 20;
    const px = clamp(x, w / 2 + 2, width - w / 2 - 2);
    P.marker.style.display = "";
    P.markerLine.setAttribute("x1", x);
    P.markerLine.setAttribute("x2", x);
    P.markerPill.setAttribute("x", px - w / 2);
    P.markerPill.setAttribute("width", w);
    P.markerText.setAttribute("x", px);
    P.markerText.textContent = text;
  }

  const local = (e) => {
    const r = P.svg.getBoundingClientRect();
    return { x: e.clientX - r.left, y: e.clientY - r.top };
  };

  function onHover(e) {
    if (mode !== "hover") return;
    marker = Math.round(clamp(Xi(local(e).x), dom.xmin, dom.xmax) * 10) / 10;
    drawMarker();
    if (opts.onClimb) opts.onClimb(marker);
  }

  /* Applies a handle position (px) or a keyboard step to the curve. */
  function apply(kind, i, pos, key) {
    const p = i >= 0 ? curve.points[i] : null;
    if (kind === "f") {
      const lo = i > 0 ? curve.points[i - 1].vz / 100 + TONE_GAP : dom.xmin;
      const hi = i < curve.points.length - 1 ? curve.points[i + 1].vz / 100 - TONE_GAP : dom.xmax;
      let vz = pos ? Xi(pos.x) : p.vz / 100 + key.dx * (key.big ? 0.5 : TONE_GAP);
      let f = pos ? Yfi(pos.y) : p.freq + key.dy * (key.big ? 50 : 5);
      p.vz = Math.round(clamp(snap(vz, TONE_GAP), lo, hi) * 100);
      p.freq = Math.round(clamp(snap(f, 5), 20, 5000));
    } else if (kind === "r") {
      const r = pos ? Yri(pos.y) : rate(p) + key.dy * (key.big ? 1 : 0.1);
      p.duration = Math.round(clamp(1000 / clamp(r, 0.2, TONE_RATE_MAX), 100, 5000) / 10) * 10;
    } else if (kind === "d") {
      const d = pos ? Ydi(pos.y) : p.duty + key.dy * (key.big ? 20 : 5);
      p.duty = Math.round(clamp(snap(d, 5), 0, 100));
    } else if (kind === "lift") {
      const v = pos ? Xi(pos.x) : curve.lift / 100 + key.dx * (key.big ? 0.5 : TONE_GAP);
      curve.lift = Math.round(clamp(snap(v, TONE_GAP), Math.max(0, curve.alarm ? curve.sink / 100 + TONE_GAP : 0), 2) * 100);
    } else if (kind === "sink") {
      const v = pos ? Xi(pos.x) : curve.sink / 100 + key.dx * (key.big ? 0.5 : TONE_GAP);
      curve.sink = Math.round(clamp(snap(v, TONE_GAP), Math.max(-20, dom.xmin), Math.min(0, curve.lift / 100 - TONE_GAP)) * 100);
    }
  }

  /* Where a handle's value sits (px): a drag keeps the offset of the grab, so nothing jumps. */
  function handlePos(kind, i) {
    const p = curve.points[i];
    if (kind === "lift") return { x: X(curve.lift / 100), y: 0 };
    if (kind === "sink") return { x: X(curve.sink / 100), y: 0 };
    const x = X(p.vz / 100);
    return { x, y: kind === "f" ? Yf(p.freq) : kind === "r" ? Yr(rate(p)) : Yd(p.duty) };
  }

  function wireHandle(h, kind, i) {
    h.addEventListener("pointerdown", (e) => {
      if (mode !== "edit") return;
      e.preventDefault();
      try {
        h.setPointerCapture(e.pointerId);
      } catch (err) { /* the moves still arrive while the pointer stays on the handle */ }
      const at = handlePos(kind, i);
      const l = local(e);
      drag = { kind, i, ox: l.x - at.x, oy: l.y - at.y };
      if (i >= 0) select(i);
      root.classList.add("dragging");
    });
    h.addEventListener("pointermove", (e) => {
      if (!drag || drag.kind !== kind || drag.i !== i) return;
      const l = local(e);
      apply(kind, i, { x: l.x - drag.ox, y: l.y - drag.oy });
      update();
      if (i >= 0) select(i);
      opts.onEdit(cloneCurve(curve), false);
    });
    const end = () => {
      if (!drag) return;
      drag = null;
      root.classList.remove("dragging");
      opts.onEdit(cloneCurve(curve), true);
      const d = domain();
      if (d.xmin !== dom.xmin || d.xmax !== dom.xmax || d.fmax !== dom.fmax) build();
    };
    h.addEventListener("pointerup", end);
    h.addEventListener("pointercancel", end);
    h.addEventListener("focus", () => { if (i >= 0) select(i); });
    h.addEventListener("keydown", (e) => {
      if (mode !== "edit") return;
      const dir = { ArrowLeft: [-1, 0], ArrowRight: [1, 0], ArrowUp: [0, 1], ArrowDown: [0, -1] }[e.key];
      if (!dir) return;
      e.preventDefault();
      apply(kind, i, null, { dx: dir[0], dy: dir[1], big: e.shiftKey });
      update();
      if (i >= 0) select(i);
      opts.onEdit(cloneCurve(curve), true);
    });
  }

  const resize = new ResizeObserver(() => {
    if (Math.round(root.clientWidth) !== width && !drag) build();
  });
  resize.observe(root);
  setTimeout(() => { if (!width) build(); }, 0); /* drawn even before the first resize notification */

  return {
    el: root,
    setCurve(c) {
      curve = cloneCurve(c);
      build();
    },
    /* "edit" | "hover" | "live" */
    setMode(m, climb = null) {
      mode = m;
      marker = m === "edit" ? null : climb;
      root.classList.toggle("testing", m !== "edit");
      if (G) drawMarker();
    },
    setMarker(climb) {
      marker = climb;
      if (G) drawMarker();
    },
    destroy() {
      resize.disconnect();
    },
  };
}

/* ---- preview in the browser: the device's beep pattern with Web Audio ---- */

class TonePreview {
  constructor() {
    this.ctx = null;
    this.timer = null;
  }

  get running() {
    return !!this.timer;
  }

  /* getCurve() and getClimb() are read for every beep, so edits and the pointer apply at once. */
  start(getCurve, getClimb) {
    this.stop();
    this.ctx = this.ctx || new AudioContext();
    this.ctx.resume();
    const osc = this.ctx.createOscillator();
    const gain = this.ctx.createGain();
    osc.type = "square"; /* closer to the piezo than a sine */
    gain.gain.value = 0;
    osc.connect(gain).connect(this.ctx.destination);
    osc.start();
    this.osc = osc;
    this.gain = gain;
    let next = this.ctx.currentTime + 0.05;
    const schedule = () => {
      const now = this.ctx.currentTime;
      while (next < now + 0.3) {
        const climb = getClimb();
        const t = climb === null ? { sound: false } : varioTone(getCurve(), climb);
        if (!t.sound) {
          gain.gain.setValueAtTime(0, next);
          next += 0.1;
          continue;
        }
        osc.frequency.setValueAtTime(t.freq, next);
        gain.gain.setValueAtTime(0.12, next);
        if (t.on < t.period) gain.gain.setValueAtTime(0, next + t.on / 1000);
        next += t.period / 1000;
      }
    };
    schedule();
    this.timer = setInterval(schedule, 100);
  }

  stop() {
    if (this.timer) clearInterval(this.timer);
    this.timer = null;
    if (this.osc) {
      this.gain.gain.cancelScheduledValues(0);
      this.gain.gain.value = 0;
      this.osc.stop();
      this.osc = null;
    }
  }
}
