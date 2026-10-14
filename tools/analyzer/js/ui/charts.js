/*
 * Stacked uPlot charts on the barometer grid (seconds since the log start): shared cursor, shared
 * zoom (drag to zoom, double-click to reset), click to set the replay position, a play line. Takeoff
 * (green) and landing (red) as the firmware detects them with its default settings run as lines
 * through every chart, at the time the device dates them to.
 */
/* global uPlot */
import { KINDS, KIND_COLORS } from "../core/flightstate.js";

const SYNC_KEY = "uba";
const BAND_ALPHA = { ground: 0.18, thermal: 0.22, circling: 0.2, spiral: 0.3, wingover: 0.3 }; /* glide stays plain */
export const FLIGHT_MARK_COLORS = { takeoff: "#3ddc84", landing: "#ff5c5c" };

function hexRgba(hex, a) {
  const v = parseInt(hex.slice(1), 16);
  return `rgba(${(v >> 16) & 255},${(v >> 8) & 255},${v & 255},${a})`;
}

/* typed array with NaN -> plain array with null (uPlot gaps) */
export function col(arr, scale = 1, offset = 0) {
  const out = new Array(arr.length);
  for (let i = 0; i < arr.length; i++) {
    const v = arr[i];
    out[i] = Number.isFinite(v) ? v * scale + offset : null;
  }
  return out;
}

function rollingBaroNoise(alt, w) {
  const n = alt.length;
  const out = new Float64Array(n).fill(NaN);
  let s = 0, s2 = 0, c = 0;
  const d = new Float64Array(n);
  for (let i = 1; i < n; i++) d[i] = alt[i] - alt[i - 1];
  for (let i = 1; i < n; i++) {
    s += d[i]; s2 += d[i] * d[i]; c++;
    if (i - w >= 1) { s -= d[i - w]; s2 -= d[i - w] ** 2; c--; }
    if (c > 4) out[i] = Math.sqrt(Math.max(0, (s2 - s * s / c) / (c - 1)) / 2);
  }
  return out;
}

export const CHART_DEFS = [
  { id: "vario", title: "Climb rate [m/s]", on: true, height: 230 },
  { id: "flight", title: "Flight: glide ratio, load [g] | bank angle, turn rate [°/s]", on: true, height: 160, states: true },
  { id: "error", title: "Climb rate minus reference [m/s]", on: true, height: 150, track: false },
  { id: "alt", title: "Altitude [m] (standard atmosphere; GNSS shifted onto it)", on: true, height: 170 },
  { id: "motion", title: "Motion: | |a| − g | [m/s²], rotation [°/s]", on: true, height: 150, track: false },
  { id: "gnss", title: "GNSS: ground speed [km/h], satellites, HDOP×10", on: true, height: 140 },
  { id: "baro", title: "Barometer: noise per sample [cm], temperature [°C]", on: false, height: 130, track: false },
  { id: "diag", title: "Diagnostics", on: true, height: 170, track: false },
];

export class Charts {
  constructor(container, app) {
    this.el = container;
    this.app = app;
    this.plots = [];
    this.visible = Object.fromEntries(CHART_DEFS.map((d) => [d.id, d.on]));
    this.diagRun = null;
    this.diagKeys = null;
    this.range = null;
    this.playT = null;
    this._syncing = false;
    this._resizeObs = new ResizeObserver(() => this.resize());
    this._resizeObs.observe(container);
  }

  /* All series of one chart: [{label, stroke, width, dash, values, scale}] */
  seriesFor(id, ctx, runs) {
    const shown = runs.filter((r) => r.visible && r.result);
    const S = [];
    if (id === "vario") {
      S.push({ label: `reference ±${ctx.refHalf}s`, stroke: "#e8e8e8", width: 1.5, dash: [6, 4], values: col(ctx.ref) });
      S.push({ label: "GNSS", stroke: "#a58bff", width: 1, values: col(ctx.gnssClimb), show: false });
      for (const r of [...shown].reverse()) S.push({ label: r.label, stroke: r.color, width: 1.4, values: col(r.result.vz) }); // first run drawn last (on top)
    } else if (id === "error") {
      const ref = ctx.ref;
      for (const r of shown) {
        const e = new Float64Array(ref.length);
        for (let i = 0; i < e.length; i++) e[i] = ctx.handlingMask[i] ? NaN : r.result.vz[i] - ref[i];
        S.push({ label: r.label, stroke: r.color, width: 1, values: col(e) });
      }
    } else if (id === "alt") {
      S.push({ label: "barometer (raw)", stroke: "#e8e8e8", width: 1, values: col(ctx.altIsa) });
      // GNSS altitude moved onto the barometric one (median offset): shows drift and GNSS noise
      const d = [];
      for (let i = 0; i < ctx.altIsa.length; i += 16) if (Number.isFinite(ctx.gnssAlt[i])) d.push(ctx.gnssAlt[i] - ctx.altIsa[i]);
      d.sort((a, b) => a - b);
      const off = d.length ? d[d.length >> 1] : 0;
      S.push({ label: `GNSS ${off >= 0 ? "−" : "+"}${Math.abs(off).toFixed(0)} m`, stroke: "#a58bff", width: 1, values: col(ctx.gnssAlt, 1, -off) });
      for (const r of shown) S.push({ label: r.label, stroke: r.color, width: 1, values: col(r.result.h), show: false });
    } else if (id === "motion") {
      const acc = new Float64Array(ctx.accNorm.length);
      for (let i = 0; i < acc.length; i++) acc[i] = Math.abs(ctx.accNorm[i] - 9.80665);
      S.push({ label: "| |a| − g |", stroke: "#ffb347", width: 1, values: col(acc), scale: "a" });
      S.push({ label: "rotation (mean)", stroke: "#5ce1e6", width: 1, values: col(ctx.gyroNorm), scale: "w" });
      if (ctx.yawRate) S.push({ label: "turn rate (gyro, ± = right/left)", stroke: "#7ddc7a", width: 1, values: col(ctx.yawRate), scale: "w", show: false });
      S.push({ label: "GNSS track turn", stroke: "#a58bff", width: 1, values: col(ctx.trackRate), scale: "w", show: false });
    } else if (id === "flight" && ctx.states) {
      // per-second values onto the grid
      const per = ctx.states.per, ser = ctx.series;
      const at = (arr) => {
        const out = new Float64Array(ctx.tS.length).fill(NaN);
        for (let i = 0; i < ctx.tS.length; i++) {
          const k = Math.floor(ctx.tS[i] - ser.t[0]);
          if (k >= 0 && k < ser.n) out[i] = arr[k];
        }
        return out;
      };
      const ld = at(per.glide);
      for (let i = 0; i < ld.length; i++) if (ld[i] > 40) ld[i] = 40;
      S.push({ label: "glide ratio (20 s)", stroke: "#7ddc7a", width: 1.2, values: col(ld), scale: "y" });
      // the per-second values are what the device's detectors see (1 s means); a raw log also gives
      // the load, the bank angle and the gyro turn rate per sample, which show a wingover's swings
      const perSec = ctx.states.summary.imu ? " (1 s)" : " (est.)";
      S.push({ label: `load [g]${perSec}`, stroke: "#ff6b8b", width: 1, values: col(at(per.load)), scale: "y" });
      S.push({ label: `bank [°]${perSec}`, stroke: "#c792ea", width: 1, values: col(at(per.bank)), scale: "d" });
      S.push({ label: "turn rate [°/s] (GNSS course, 1 s)", stroke: "#5ce1e6", width: 1, values: col(at(per.turn)), scale: "d", show: false });
      if (ctx.yawRate) {
        const bank = new Float64Array(ctx.accNorm.length);
        for (let i = 0; i < bank.length; i++) {
          const n = ctx.accNorm[i] / 9.80665;
          bank[i] = n > 1 ? Math.acos(1 / n) * 180 / Math.PI : 0;
        }
        S.push({ label: "bank [°] (IMU, per sample)", stroke: "#e0b0ff", width: 1, values: col(bank), scale: "d", show: false });
        S.push({ label: "turn rate [°/s] (gyro, per sample)", stroke: "#9fe8eb", width: 1, values: col(ctx.yawRate), scale: "d", show: false });
      }
      S.push({ label: "wind [km/h]", stroke: "#ffb347", width: 1, values: col(at(per.windKmh)), scale: "y", show: false });
    } else if (id === "gnss") {
      const fx = ctx.fixes;
      const at = (arr) => {
        const out = new Float64Array(ctx.tS.length).fill(NaN);
        let j = 0;
        for (let i = 0; i < ctx.tS.length; i++) {
          while (j + 1 < fx.n && ctx.fixT[j + 1] <= ctx.tS[i]) j++;
          if (fx.n && ctx.fixT[j] <= ctx.tS[i] && ctx.tS[i] - ctx.fixT[j] < 2.5) out[i] = arr[j];
        }
        return out;
      };
      S.push({ label: "speed", stroke: "#7ddc7a", width: 1, values: col(ctx.gnssSpeed, 3.6) });
      S.push({ label: "satellites used", stroke: "#ffb347", width: 1, values: col(at(fx.sats)) });
      S.push({ label: "HDOP×10", stroke: "#ff6b8b", width: 1, values: col(at(fx.hdop), 10) });
      S.push({ label: "fix 3D", stroke: "#4fb3ff", width: 1, values: col(at(fx.valid), 5), show: false });
    } else if (id === "baro") {
      S.push({ label: "noise (1 s)", stroke: "#ffb347", width: 1, values: col(rollingBaroNoise(ctx.altIsa, Math.round(ctx.baroRate)), 100), scale: "n" });
      S.push({ label: "temperature", stroke: "#5ce1e6", width: 1, values: col(ctx.log.baro.temp), scale: "T" });
    } else if (id === "diag") {
      const r = this.diagRunFor(runs);
      if (r && r.result) {
        const keys = this.diagKeys || r.result.diagnostics.slice(0, 4).map((d) => d.key);
        const palette = ["#ffb347", "#5ce1e6", "#7ddc7a", "#ff6b8b", "#c792ea", "#f2e863", "#4fb3ff", "#ff9f6b", "#9fd3ff", "#e0e0e0"];
        r.result.diagnostics.forEach((d, k) => {
          S.push({
            label: `${d.label}${d.unit ? ` [${d.unit}]` : ""}`, stroke: palette[k % palette.length], width: 1,
            values: col(r.result.diag[d.key]), show: keys.includes(d.key),
          });
        });
      }
    }
    return S;
  }

  diagRunFor(runs) {
    const withDiag = runs.filter((r) => r.result && r.result.diagnostics.length);
    return withDiag.find((r) => r.id === this.diagRun) || withDiag[0] || null;
  }

  build(ctx, runs) {
    this.destroy();
    this.el.innerHTML = "";
    const tools = document.createElement("div");
    tools.className = "chart-tools";
    tools.append("Charts:");
    const isTrack = ctx.kind === "track";
    const defs = CHART_DEFS.filter((d) => !(isTrack && d.track === false) && !(d.states && !ctx.states));
    for (const d of defs) {
      const l = document.createElement("label");
      const c = document.createElement("input");
      c.type = "checkbox";
      c.checked = this.visible[d.id];
      c.addEventListener("change", () => { this.visible[d.id] = c.checked; this.build(ctx, runs); });
      l.append(c, d.id);
      tools.append(l);
    }
    const dr = this.diagRunFor(runs);
    if (dr) {
      const sel = document.createElement("select");
      for (const r of runs.filter((x) => x.result && x.result.diagnostics.length)) {
        const o = new Option(`diagnostics: ${r.label}`, r.id);
        if (r.id === dr.id) o.selected = true;
        sel.append(o);
      }
      sel.addEventListener("change", () => { this.diagRun = sel.value; this.build(ctx, runs); });
      tools.append(sel);
    }
    const reset = document.createElement("button");
    reset.textContent = isTrack ? "Whole flight" : "Whole log";
    reset.addEventListener("click", () => this.app.setRange(null));
    tools.append(reset);
    if (ctx.states) {
      const key = document.createElement("span");
      key.className = "statekey";
      for (const k of KINDS) {
        if (k === "glide") continue;
        const sw = document.createElement("i");
        sw.style.background = hexRgba(KIND_COLORS[k], 0.5);
        const lab = document.createElement("span");
        lab.append(sw, k);
        key.append(lab);
      }
      for (const k of Object.keys(FLIGHT_MARK_COLORS)) {
        const sw = document.createElement("i");
        sw.style.background = FLIGHT_MARK_COLORS[k];
        sw.style.width = "3px";
        const lab = document.createElement("span");
        lab.append(sw, k);
        key.append(lab);
      }
      key.title = "Flight states as bands; straight flight stays plain. Takeoff and landing as the firmware detects them " +
        "with its default settings. Hover a chart for the values.";
      tools.append(key);
    }
    this.el.append(tools);
    const info = document.createElement("div");
    info.className = "stateinfo";
    info.id = "stateinfo";
    info.textContent = ctx.states ? "Hover a chart: flight state, glide ratio, bank angle, wind and what the device would say." : "";
    if (ctx.states) this.el.append(info);

    const x = Array.from(ctx.tS);
    const width = Math.max(300, this.el.clientWidth - 24);
    const bands = ctx.states ? ctx.states.segments.filter((g) => BAND_ALPHA[g.kind]) : [];
    const events = ctx.states ? ctx.states.events : [];
    const marks = events.filter((e) => !FLIGHT_MARK_COLORS[e.kind]);
    const flightMarks = events.filter((e) => FLIGHT_MARK_COLORS[e.kind]);
    const drawFlightMarks = (u, labelled) => {
      const c = u.ctx, bb = u.bbox, px = uPlot.pxRatio || 1;
      c.save();
      c.font = `${Math.round(10 * px)}px IBM Plex Mono, Consolas, monospace`;
      c.textBaseline = "top";
      for (const e of flightMarks) {
        const xm = u.valToPos(e.t, "x", true);
        if (xm < bb.left || xm > bb.left + bb.width) continue;
        c.fillStyle = FLIGHT_MARK_COLORS[e.kind];
        c.fillRect(xm - px, bb.top, 2 * px, bb.height);
        if (labelled) c.fillText(e.kind, xm + 4 * px, bb.top + 10 * px);
      }
      c.restore();
    };
    let labelled = true; /* the labels in the top chart only */
    const drawBands = (u) => {
      const c = u.ctx, bb = u.bbox;
      c.save();
      for (const g of bands) {
        const x0 = Math.max(bb.left, u.valToPos(g.t0, "x", true)), x1 = Math.min(bb.left + bb.width, u.valToPos(g.t1, "x", true));
        if (x1 <= bb.left || x0 >= bb.left + bb.width) continue;
        c.fillStyle = hexRgba(KIND_COLORS[g.kind], BAND_ALPHA[g.kind]);
        c.fillRect(x0, bb.top, Math.max(1, x1 - x0), bb.height);
      }
      c.fillStyle = "rgba(255, 214, 10, 0.8)";
      for (const e of marks) {
        const xm = u.valToPos(e.t, "x", true);
        if (xm < bb.left || xm > bb.left + bb.width) continue;
        c.fillRect(xm - 1, bb.top, 2, 7);
      }
      c.restore();
    };
    for (const d of defs) {
      if (!this.visible[d.id]) continue;
      const S = this.seriesFor(d.id, ctx, runs);
      if (!S.length) continue;
      const label = labelled;
      labelled = false;
      const wrap = document.createElement("div");
      wrap.className = "chart";
      const title = document.createElement("div");
      title.className = "title";
      title.textContent = d.id === "diag" && dr ? `Diagnostics: ${dr.label}` : d.title;
      wrap.append(title);
      this.el.append(wrap);
      const scales = { x: { time: false } };
      const axes = [
        { stroke: "#8796a3", grid: { stroke: "#1f2a33" }, ticks: { stroke: "#26323c" }, values: (u, v) => v.map((s) => this.app.fmtTime(s)) },
        { stroke: "#8796a3", grid: { stroke: "#1f2a33" }, ticks: { stroke: "#26323c" }, size: 50 },
      ];
      const usedScales = [...new Set(S.map((s) => s.scale).filter(Boolean))];
      if (usedScales.length === 2) {
        axes[1].scale = usedScales[0];
        axes.push({ scale: usedScales[1], side: 1, stroke: "#8796a3", grid: { show: false }, size: 50 });
      } else if (usedScales.length === 1) axes[1].scale = usedScales[0];
      const opts = {
        width, height: d.height,
        scales,
        axes,
        series: [{ label: "t", value: (u, v) => (v == null ? "–" : this.app.fmtTime(v, true)) },
          ...S.map((s) => ({
            label: s.label, stroke: s.stroke, width: s.width, dash: s.dash, show: s.show !== false, scale: s.scale || "y",
            value: (u, v) => (v == null ? "–" : v.toFixed(Math.abs(v) < 10 ? 2 : 1)), points: { show: false },
          }))],
        cursor: { sync: { key: SYNC_KEY, setSeries: false }, drag: { x: true, y: false, setScale: false } },
        legend: { live: true },
        hooks: {
          setSelect: [(u) => {
            if (u.select.width < 3) return;
            const a = u.posToVal(u.select.left, "x"), b = u.posToVal(u.select.left + u.select.width, "x");
            u.setSelect({ left: 0, width: 0 }, false);
            this.app.setRange([a, b]);
          }],
          drawClear: ctx.states ? [drawBands] : [],
          draw: flightMarks.length ? [(u) => drawFlightMarks(u, label)] : [],
          setCursor: ctx.states ? [(u) => {
            if (this._syncing) return;
            const i = u.cursor.idx;
            this.app.onChartHover(i == null ? null : u.data[0][i]);
          }] : [],
        },
      };
      const data = [x, ...S.map((s) => s.values)];
      const u = new uPlot(opts, data, wrap);
      u.over.addEventListener("dblclick", () => this.app.setRange(null));
      u.over.addEventListener("click", (ev) => {
        if (u.select.width > 2) return;
        const rect = u.over.getBoundingClientRect();
        this.app.setCursor(u.posToVal(ev.clientX - rect.left, "x"));
      });
      const line = document.createElement("div");
      line.className = "playline";
      u.over.append(line);
      this.plots.push({ u, line, id: d.id });
    }
    this.applyRange();
    this.setPlayT(this.playT);
  }

  setRange(range) {
    this.range = range;
    this.applyRange();
  }

  applyRange() {
    for (const { u } of this.plots) {
      const xs = u.data[0];
      const [a, b] = this.range || [xs[0], xs[xs.length - 1]];
      u.setScale("x", { min: a, max: b });
    }
    this.setPlayT(this.playT);
  }

  setPlayT(t) {
    this.playT = t;
    for (const { u, line } of this.plots) {
      if (t == null) { line.style.display = "none"; continue; }
      const x = u.valToPos(t, "x");
      if (x < 0 || x > u.over.clientWidth) { line.style.display = "none"; continue; }
      line.style.display = "block";
      line.style.left = `${x}px`;
      line.style.height = "100%";
    }
  }

  resize() {
    const width = Math.max(300, this.el.clientWidth - 24);
    for (const { u } of this.plots) u.setSize({ width, height: u.height });
    this.setPlayT(this.playT);
  }

  destroy() {
    for (const { u } of this.plots) u.destroy();
    this.plots = [];
  }
}
