/* Analyzer UI: state, runs, wiring of charts, map, panels and the tone replay; raw logs and IGC tracks. */
import { parseRawLog } from "../core/rawlog.js";
import { isIgcText, parseIgc } from "../core/igc.js";
import { buildTrackContext, flightStatesOf } from "../core/track.js";
import { KINDS, KIND_LABELS, stateAt } from "../core/flightstate.js";
import { buildContext, loggedRun, runAlgorithms, setReference } from "../core/replay.js";
import { computeMetrics } from "../core/metrics.js";
import { analyzeHealth } from "../core/health.js";
import { defaultParams, loadAlgorithms } from "../core/registry.js";
import { DEFAULT_CURVE, curveFromJson } from "../core/tones.js";
import { Charts } from "./charts.js";
import { MapView } from "./map3d.js";
import { Player } from "./audio.js";
import { renderApi, renderEvents, renderFlight, renderHealth, renderMetrics, renderSpectrum } from "./panels.js";
import { installApi } from "./api.js";

const COLORS = ["#4fb3ff", "#ffb347", "#7ddc7a", "#ff6b8b", "#c792ea", "#5ce1e6", "#f2e863", "#ff9f6b", "#9fd3ff"];
const $ = (id) => document.getElementById(id);

function debounce(fn, ms) {
  let h = null;
  return (...a) => { clearTimeout(h); h = setTimeout(() => fn(...a), ms); };
}

class App {
  constructor() {
    this.state = { log: null, ctx: null, algos: [], runs: [], range: null, cursor: null, curve: DEFAULT_CURVE, toneRunId: null, health: null };
    this.charts = new Charts($("charts"), this);
    this.map = new MapView($("map"), this);
    this.player = new Player(this);
    this.nextId = 1;
    this.activePane = "map";
    this.utcOffset = null;
    this.refreshMetricsSoon = debounce(() => this.renderActivePane(), 150);
    this.ready = false;
    this.loading = null;
    this.initialized = new Promise((res) => { this._initDone = res; });
  }

  async init() {
    this.state.algos = await loadAlgorithms();
    const add = $("addalgo");
    for (const a of this.state.algos) add.append(new Option(`${a.name}${a.private ? " (private)" : ""}`, a.id));
    $("addrun").addEventListener("click", () => this.addRun(add.value));
    $("open").addEventListener("click", () => $("file").click());
    $("file").addEventListener("change", async (e) => {
      const file = e.target.files[0];
      if (file) await this.loadBuffer(await file.arrayBuffer(), file.name);
      e.target.value = "";
    });
    document.body.addEventListener("dragover", (e) => { e.preventDefault(); document.body.classList.add("dragging"); });
    document.body.addEventListener("dragleave", (e) => { if (e.target === $("drop")) document.body.classList.remove("dragging"); });
    document.body.addEventListener("drop", async (e) => {
      e.preventDefault();
      document.body.classList.remove("dragging");
      const file = e.dataTransfer.files[0];
      if (file) await this.loadBuffer(await file.arrayBuffer(), file.name);
    });
    for (const b of document.querySelectorAll(".tabs button")) b.addEventListener("click", () => this.showPane(b.dataset.pane));
    $("play").addEventListener("click", () => this.togglePlay());
    $("speed").addEventListener("change", (e) => this.player.setSpeed(Number(e.target.value)));
    $("volume").addEventListener("input", (e) => this.player.setVolume(Number(e.target.value)));
    $("wave").addEventListener("change", (e) => this.player.setWave(e.target.value));
    $("tonesrc").addEventListener("change", (e) => this.setToneRun(e.target.value));
    $("refhalf").addEventListener("input", (e) => {
      $("refhalfval").textContent = `${Number(e.target.value).toFixed(1)} s`;
      this.setRefHalfSoon(Number(e.target.value));
    });
    this.setRefHalfSoon = debounce((v) => {
      if (!this.state.ctx) return;
      setReference(this.state.ctx, v);
      this.refresh({ charts: true, metrics: true });
    }, 200);
    $("curveapply").addEventListener("click", () => {
      try {
        this.state.curve = curveFromJson($("curvejson").value);
        $("curvemsg").textContent = "applied";
        this.player.setCurve(this.state.curve);
        this.refresh({ metrics: true });
      } catch (e) { $("curvemsg").textContent = e.message; }
    });
    $("curvereset").addEventListener("click", () => {
      this.state.curve = DEFAULT_CURVE;
      $("curvemsg").textContent = "default";
      this.player.setCurve(this.state.curve);
      this.refresh({ metrics: true });
    });
    $("mapcolor").addEventListener("change", (e) => { this.map.colorBy = e.target.value; this.map.recolor(); });
    $("terrain").addEventListener("change", (e) => { this.map.terrain = e.target.checked; this.map.render(); });
    $("follow").addEventListener("change", (e) => this.map.setFollow(e.target.checked));
    $("mapfit").addEventListener("click", () => this.map.fitView());
    const glider = $("glidersize");
    const showGlider = () => { $("glidersizeval").textContent = `${glider.value}×`; };
    glider.value = this.map.gliderScale;
    showGlider();
    glider.addEventListener("input", () => { showGlider(); this.map.setGliderScale(Number(glider.value)); });
    document.addEventListener("keydown", (e) => {
      if (/INPUT|TEXTAREA|SELECT/.test(document.activeElement.tagName)) return;
      if (e.code === "Space") { e.preventDefault(); this.togglePlay(); }
      if (/^Digit[1-9]$/.test(e.code)) {
        const r = this.state.runs[Number(e.code.slice(5)) - 1];
        if (r) this.setToneRun(r.id);
      }
      if (e.code === "Escape") this.setRange(null);
    });
    this.initResizer();
    renderApi($("pane-api"));
    installApi(this);
    const url = new URLSearchParams(location.search).get("log");
    const first = url ? this.loadUrl(url).catch(() => {}) : Promise.resolve();
    this._initDone(first);
    await first;
  }

  /* resolves when the page is set up and a log given by ?log= is loaded */
  async whenReady() {
    await this.initialized;
    if (this.loading) await this.loading.catch(() => {});
    return this.summary();
  }

  initResizer() {
    const r = $("resizer");
    r.addEventListener("pointerdown", (e) => {
      r.setPointerCapture(e.pointerId);
      const move = (ev) => {
        const w = Math.max(320, Math.min(window.innerWidth - 500, window.innerWidth - ev.clientX));
        document.querySelector(".layout").style.setProperty("--right-w", `${w}px`);
      };
      const up = () => { r.removeEventListener("pointermove", move); r.removeEventListener("pointerup", up); this.charts.resize(); };
      r.addEventListener("pointermove", move);
      r.addEventListener("pointerup", up);
    });
  }

  busy(on, text = "Working…") {
    $("busy").textContent = text;
    $("busy").classList.toggle("on", on);
    // let the browser paint the message; no requestAnimationFrame (it stalls in hidden tabs)
    return new Promise((res) => setTimeout(res, 30));
  }

  /* relative time m:ss (h:mm:ss); precise adds tenths and UTC when known */
  fmtTime(s, precise = false) {
    if (s == null || !Number.isFinite(s)) return "–";
    const neg = s < 0;
    let v = Math.abs(s);
    const h = Math.floor(v / 3600); v -= h * 3600;
    const m = Math.floor(v / 60); v -= m * 60;
    const sec = precise ? v.toFixed(1).padStart(4, "0") : String(Math.floor(v)).padStart(2, "0");
    let out = `${neg ? "−" : ""}${h ? `${h}:${String(m).padStart(2, "0")}` : m}:${sec}`;
    if (precise && this.utcOffset != null && this.state.ctx) {
      const d = new Date(this.state.ctx.t0 + s * 1000 + this.utcOffset);
      out += ` · ${d.toISOString().slice(11, 19)} UTC`;
    }
    return out;
  }

  async loadUrl(url) {
    await this.busy(true, `Loading ${url}…`);
    try {
      const res = await fetch(url);
      if (!res.ok) throw new Error(`${res.status} ${res.statusText}`);
      await this.loadBuffer(await res.arrayBuffer(), url.split("/").pop());
    } catch (e) {
      $("fileinfo").textContent = `Cannot load ${url}: ${e.message}`;
      await this.busy(false);
      throw e;
    }
  }

  async loadBuffer(buf, name) {
    this.ready = false;
    this.loading = this._load(buf, name);
    try {
      return await this.loading;
    } finally {
      this.ready = !!this.state.log;
    }
  }

  async _load(buf, name) {
    this.player.stop();
    this.onPlayState(false);
    await this.busy(true, `Reading ${name}…`);
    try {
      const bytes = buf instanceof Uint8Array ? buf : new Uint8Array(buf);
      const head = new TextDecoder("latin1").decode(bytes.subarray(0, 400));
      const isTrack = /\.igc$/i.test(name) || isIgcText(head);
      let log, ctx;
      if (isTrack) {
        const track = parseIgc(new TextDecoder("latin1").decode(bytes), name);
        ctx = buildTrackContext(track, { refHalf: Number($("refhalf").value) });
        const h = track.header;
        log = { kind: "track", name, header: { firmware: `${h.frType || h.manufacturer} ${h.firmware}`.trim(), pilot: h.pilot, glider: h.glider, date: h.date },
          t0: track.utc0, tEnd: track.utc0 + ctx.tS[ctx.tS.length - 1] * 1000, baro: { n: ctx.tS.length }, imu: { n: 0 }, events: ctx.events, track };
      } else {
        log = parseRawLog(bytes, name);
        ctx = buildContext(log, { refHalf: Number($("refhalf").value) });
      }
      try {
        const fs = flightStatesOf(ctx);
        ctx.series = fs.series;
        ctx.states = fs.states;
      } catch (e) {
        console.error("flight states", e);
        ctx.series = null;
        ctx.states = null;
      }
      this.state.log = log;
      this.state.ctx = ctx;
      this.state.range = null;
      this.state.cursor = null;
      // UTC: GNSS time if there is any, else the clock of the header
      this.utcOffset = null;
      const fx = ctx.fixes;
      for (let i = 0; i < fx.n; i++) if (Number.isFinite(fx.utc[i])) { this.utcOffset = fx.utc[i] - fx.t[i]; break; }
      if (this.utcOffset == null && log.header.unixTime) this.utcOffset = log.header.unixTime * 1000 - log.header.tMs;
      const dur = (log.tEnd - log.t0) / 1000;
      const st = ctx.states ? ` · ${ctx.states.summary.thermals} thermals, ${(ctx.states.summary.thermalSeconds / 60).toFixed(0)} min` : "";
      $("fileinfo").textContent = isTrack
        ? `${name} · ${log.header.firmware} · ${[log.header.pilot, log.header.glider].filter(Boolean).join(", ")} · ${this.fmtTime(dur)} · ${ctx.fixRate || 0} fixes/s${st}`
        : `${name} · firmware ${log.header.firmware} · ${this.fmtTime(dur)} · ${ctx.fixRate || 0} fixes/s${st}`;
      $("fileinfo").title = $("fileinfo").textContent;
      $("empty")?.remove();
      $("play").disabled = false;

      await this.busy(true, "Replaying…");
      const keep = this.state.runs.filter((r) => r.algo);
      this.state.runs = [{ id: "logged", label: isTrack ? "file (climb rate)" : "device (logged)", algo: null, color: "#9aa4ad", visible: true, result: null, params: {} }];
      $("addrun").disabled = isTrack;
      $("addalgo").disabled = isTrack;
      $("addrun").title = isTrack ? "An IGC track has no sensor samples to replay" : "";
      if (!isTrack) {
        if (keep.length) {
          for (const r of keep) this.state.runs.push({ ...r, result: null });
        } else {
          for (const a of this.state.algos) this.state.runs.push(this.newRun(a.id));
        }
      }
      this.runAll();
      this.state.health = isTrack ? null : analyzeHealth(ctx);
      if (isTrack) this.map.colorBy = "state";
      if (!this.state.toneRunId || !this.state.runs.find((r) => r.id === this.state.toneRunId)) {
        this.state.toneRunId = (this.state.runs.find((r) => r.algo && r.algo.id === "fusion") || this.state.runs[0]).id;
      }
      this.renderRuns();
      this.refresh({ charts: true, map: true, metrics: true, health: true });
    } catch (e) {
      $("fileinfo").textContent = `${name}: ${e.message}`;
      console.error(e);
      throw e;
    } finally {
      await this.busy(false);
    }
    return this.summary();
  }

  summary() {
    const { log, ctx } = this.state;
    if (!log || !this.ready) return null;
    return {
      name: log.name, kind: ctx.kind || "raw", firmware: log.header.firmware, durationS: (log.tEnd - log.t0) / 1000,
      baro: log.baro.n, imu: log.imu.n, fixes: ctx.fixes.n, fixRate: ctx.fixRate, events: log.events.length,
      states: ctx.states ? ctx.states.summary : null,
    };
  }

  newRun(algoId, params, label) {
    const algo = this.state.algos.find((a) => a.id === algoId);
    if (!algo) throw new Error(`unknown algorithm ${algoId}`);
    const n = this.nextId++;
    const used = new Set(this.state.runs.map((r) => r.label));
    let base = label || algo.id, lbl = base, k = 2;
    while (used.has(lbl)) lbl = `${base} ${k++}`;
    return {
      id: `r${n}`, label: lbl, algo, color: COLORS[(n - 1) % COLORS.length], visible: true,
      params: { ...defaultParams(algo), ...(params || {}) }, result: null,
    };
  }

  runAll() {
    const ctx = this.state.ctx;
    const logged = this.state.runs.find((r) => r.id === "logged");
    if (logged) { logged.result = loggedRun(ctx); logged.result.label = logged.label; }
    const todo = this.state.runs.filter((r) => r.algo);
    if (!todo.length) return;
    const res = runAlgorithms(ctx, todo.map((r) => ({ algo: r.algo, params: r.params, label: r.label })));
    todo.forEach((r, i) => { r.result = res[i]; r.result.label = r.label; });
  }

  rerun(run) {
    const [res] = runAlgorithms(this.state.ctx, [{ algo: run.algo, params: run.params, label: run.label }]);
    run.result = res;
  }

  async addRun(algoId, params, label) {
    if (!this.state.ctx) throw new Error("no log loaded");
    const run = this.newRun(algoId, params, label);
    this.state.runs.push(run);
    await this.busy(true, "Replaying…");
    this.rerun(run);
    await this.busy(false);
    this.renderRuns();
    this.refresh({ charts: true, map: true, metrics: true });
    return run;
  }

  async updateRun(id, params) {
    const run = this.state.runs.find((r) => r.id === id || r.label === id);
    if (!run || !run.algo) throw new Error(`no run ${id}`);
    Object.assign(run.params, params);
    this.rerun(run);
    this.renderRuns();
    this.refresh({ charts: true, map: true, metrics: true });
    return run;
  }

  removeRun(id) {
    const i = this.state.runs.findIndex((r) => (r.id === id || r.label === id) && r.algo);
    if (i < 0) return;
    this.state.runs.splice(i, 1);
    if (this.state.toneRunId === id) this.state.toneRunId = this.state.runs[0]?.id;
    this.renderRuns();
    this.refresh({ charts: true, map: true, metrics: true });
  }

  runByLabel(label) {
    return this.state.runs.find((r) => r.label === label);
  }

  setToneRun(id) {
    const r = this.state.runs.find((x) => x.id === id || x.label === id);
    if (!r) return;
    this.state.toneRunId = r.id;
    this.player.source = r;
    $("tonesrc").value = r.id;
    for (const c of document.querySelectorAll(".run")) c.classList.toggle("tone", c.dataset.id === r.id);
  }

  renderRuns() {
    const box = $("runs");
    box.replaceChildren();
    const tone = $("tonesrc");
    tone.replaceChildren();
    this.state.runs.forEach((r, idx) => {
      tone.append(new Option(`${idx + 1}: ${r.label}`, r.id));
      const card = document.createElement("div");
      card.className = "run";
      card.dataset.id = r.id;
      const head = document.createElement("div");
      head.className = "run-head";
      const vis = document.createElement("input");
      vis.type = "checkbox";
      vis.checked = r.visible;
      vis.title = "Show in charts and metrics";
      vis.addEventListener("change", () => { r.visible = vis.checked; this.refresh({ charts: true, metrics: true }); });
      const sw = document.createElement("span");
      sw.className = "swatch";
      sw.style.background = r.color;
      const name = document.createElement("input");
      name.type = "text";
      name.value = r.label;
      name.disabled = !r.algo;
      name.addEventListener("change", () => {
        r.label = name.value.trim() || r.label;
        if (r.result) r.result.label = r.label;
        this.renderRuns();
        this.refresh({ charts: true, metrics: true });
      });
      head.append(vis, sw, name);
      if (r.algo) {
        const dup = document.createElement("button");
        dup.className = "icon";
        dup.textContent = "⧉";
        dup.title = "Duplicate (same parameters)";
        dup.addEventListener("click", () => this.addRun(r.algo.id, { ...r.params }, `${r.label} copy`));
        const del = document.createElement("button");
        del.className = "icon";
        del.textContent = "✕";
        del.title = "Remove";
        del.addEventListener("click", () => this.removeRun(r.id));
        head.append(dup, del);
      }
      card.append(head);
      const algoLine = document.createElement("div");
      algoLine.className = "run-algo";
      algoLine.textContent = r.algo ? `${idx + 1} · ${r.algo.name}`
        : this.state.ctx?.kind === "track" ? `${idx + 1} · climb rate and altitude from the file (${this.state.ctx.track.vbv ? "data records" : "pressure altitude, ±2 s slope"})`
          : `${idx + 1} · climb rate and altitude the device computed (firmware ${this.state.log?.header.firmware ?? "?"})`;
      card.append(algoLine);
      if (r.algo && r.algo.params.length) card.append(this.paramEditor(r));
      if (r.result && r.result.ms) {
        const st = document.createElement("div");
        st.className = "status";
        st.textContent = `replay ${r.result.ms} ms`;
        card.append(st);
      }
      box.append(card);
    });
    this.setToneRun(this.state.toneRunId);
    const mc = $("mapcolor");
    const cur = mc.value || this.map.colorBy;
    mc.replaceChildren(...this.state.runs.map((r) => new Option(`climb: ${r.label}`, `run:${r.id}`)),
      new Option("GNSS vertical speed", "gnssclimb"), new Option("ground speed", "speed"), new Option("altitude", "alt"),
      ...(this.state.ctx?.states ? [new Option("flight state", "state")] : []));
    mc.value = [...mc.options].some((o) => o.value === cur) ? cur : `run:${this.state.toneRunId}`;
    this.map.colorBy = mc.value;
  }

  paramEditor(r) {
    const det = document.createElement("details");
    det.className = "params";
    const sum = document.createElement("summary");
    const defs = defaultParams(r.algo);
    const changed = Object.keys(r.params).filter((k) => r.params[k] !== defs[k]);
    sum.textContent = `Parameters${changed.length ? ` (${changed.length} changed)` : ""}`;
    det.append(sum);
    const adv = document.createElement("label");
    adv.className = "muted";
    adv.style.fontSize = "12px";
    const advBox = document.createElement("input");
    advBox.type = "checkbox";
    adv.append(advBox, " advanced");
    det.append(adv);
    const list = document.createElement("div");
    det.append(list);
    const rerunSoon = debounce(() => {
      this.rerun(r);
      this.refresh({ charts: true, map: true, metrics: true });
      const s = det.parentElement?.querySelector(".status");
      if (s) s.textContent = `replay ${r.result.ms} ms`;
    }, 250);
    const draw = () => {
      list.replaceChildren();
      for (const p of r.algo.params) {
        if (p.advanced && !advBox.checked) continue;
        const row = document.createElement("div");
        row.className = `param${r.params[p.key] !== p.value ? " changed" : ""}`;
        const lab = document.createElement("label");
        lab.textContent = p.key;
        lab.title = `${p.doc || ""}${p.unit ? ` [${p.unit}]` : ""} (default ${p.value})`;
        const num = document.createElement("input");
        num.type = "number";
        num.step = p.step ?? "any";
        num.value = r.params[p.key];
        const rng = document.createElement("input");
        rng.type = "range";
        rng.min = p.min ?? 0;
        rng.max = p.max ?? p.value * 4;
        rng.step = p.step ?? "any";
        rng.value = r.params[p.key];
        const set = (v) => {
          if (!Number.isFinite(v)) return;
          r.params[p.key] = v;
          num.value = v;
          rng.value = v;
          row.classList.toggle("changed", v !== p.value);
          rerunSoon();
        };
        num.addEventListener("change", () => set(Number(num.value)));
        rng.addEventListener("input", () => set(Number(rng.value)));
        lab.addEventListener("dblclick", () => set(p.value));
        row.append(lab, num, rng);
        list.append(row);
      }
    };
    advBox.addEventListener("change", draw);
    draw();
    return det;
  }

  visibleResults() {
    return this.state.runs.filter((r) => r.visible && r.result).map((r) => r.result);
  }

  metrics(range = this.state.range) {
    const ctx = this.state.ctx;
    if (!ctx || ctx.kind === "track") return [];
    const opts = { curve: this.state.curve };
    if (range) { opts.t0 = range[0]; opts.t1 = range[1]; }
    return this.visibleResults().map((r) => computeMetrics(ctx, r, opts));
  }

  refresh(what) {
    if (!this.state.ctx) return;
    if (what.charts) this.charts.build(this.state.ctx, this.state.runs);
    if (what.map) {
      this.map.setTrack(this.state.ctx, this.state.runs);
      if (this.activePane === "map") this.ensureMap();
    }
    if (what.metrics || what.health) this.renderActivePane();
  }

  async ensureMap() {
    if (await this.map.ensure()) {
      this.map.setTrack(this.state.ctx, this.state.runs);
      this.map.setPlayT(this.state.cursor);
    }
  }

  showPane(id) {
    this.activePane = id;
    for (const b of document.querySelectorAll(".tabs button")) b.setAttribute("aria-selected", String(b.dataset.pane === id));
    for (const p of document.querySelectorAll(".pane")) p.classList.toggle("active", p.id === `pane-${id}`);
    if (id === "map" && this.state.ctx) this.ensureMap();
    this.renderActivePane();
  }

  renderActivePane() {
    if (!this.state.ctx) return;
    const id = this.activePane;
    if (id === "flight") renderFlight($("pane-flight"), this);
    else if (id === "metrics") renderMetrics($("pane-metrics"), this, this.metrics(), this.state.range);
    else if (id === "health") renderHealth($("pane-health"), this, this.state.health);
    else if (id === "spectrum") renderSpectrum($("pane-spectrum"), this);
    else if (id === "events") renderEvents($("pane-events"), this);
  }

  setRange(range) {
    const ctx = this.state.ctx;
    if (!ctx) return;
    if (range) {
      const a = Math.max(ctx.tS[0], Math.min(range[0], range[1]));
      const b = Math.min(ctx.tS[ctx.tS.length - 1], Math.max(range[0], range[1]));
      range = b - a > 1 ? [a, b] : null;
    }
    this.state.range = range;
    this.charts.setRange(range);
    this.refreshMetricsSoon();
  }

  setCursor(t) {
    this.state.cursor = t;
    this.charts.setPlayT(t);
    this.map.setPlayT(t);
    $("clock").textContent = this.fmtTime(t, true);
    if (this.player.playing) this.player.start(t);
  }

  jumpTo(t) {
    this.setRange([t - 30, t + 30]);
    this.setCursor(t);
  }

  togglePlay() {
    if (!this.state.ctx) return;
    if (this.player.playing) {
      this.state.cursor = this.player.position();
      this.player.stop();
      this.onPlayState(false);
      return;
    }
    this.player.source = this.state.runs.find((r) => r.id === this.state.toneRunId);
    this.player.speed = Number($("speed").value);
    this.player.volume = Number($("volume").value);
    this.player.wave = $("wave").value;
    const ctx = this.state.ctx;
    const start = this.state.cursor ?? (this.state.range ? this.state.range[0] : ctx.tS[0]);
    this.player.start(start);
    this.player.setVolume(this.player.volume);
    this.onPlayState(true);
  }

  onPlayState(playing) {
    $("play").textContent = playing ? "❚❚ Pause" : "▶ Play";
  }

  onPlayPosition(t, beep) {
    this.state.cursor = t;
    this.charts.setPlayT(t);
    const now = performance.now();
    if (!this._lastMap || now - this._lastMap > 100) {
      this.map.setPlayT(t);
      this._lastMap = now;
    }
    $("clock").textContent = `${this.fmtTime(t, true)}${beep ? ` · ${beep.climb >= 0 ? "+" : ""}${beep.climb.toFixed(1)} m/s` : ""}`;
    // keep the play position inside the zoomed range
    const r = this.state.range;
    if (r && (t > r[1] || t < r[0])) {
      const w = r[1] - r[0];
      this.setRange([t - w * 0.1, t + w * 0.9]);
    }
  }

  /* the flight state and the derived values at a time, for the hover line above the charts and the map tips */
  stateText(t, short = false) {
    const ctx = this.state.ctx;
    if (!ctx || !ctx.states) return "";
    const s = stateAt(ctx.states, ctx.series, t);
    if (!s) return "";
    const per = ctx.states.per, i = s.i;
    const f = (v, d = 1) => (Number.isFinite(v) ? v.toFixed(d) : "–");
    const parts = [KIND_LABELS[s.kind]];
    const seg = ctx.states.segments.find((g) => i >= g.i0 && i <= g.i1);
    if (seg && !short) parts.push(`${this.fmtTime(seg.t1 - seg.t0)} long, ${seg.gainM >= 0 ? "+" : ""}${f(seg.gainM, 0)} m`);
    if (Number.isFinite(per.glide[i])) parts.push(`L/D ${f(per.glide[i])}`);
    parts.push(`climb 30 s ${per.climb[i] >= 0 ? "+" : ""}${f(per.climb[i])} m/s`);
    if (Number.isFinite(per.bank[i])) parts.push(`bank ${f(per.bank[i], 0)}°, ${f(per.load[i], 2)} g${ctx.states.summary.imu ? "" : " (est.)"}`);
    if (Number.isFinite(per.windKmh[i])) parts.push(`wind ${f(per.windKmh[i], 0)} km/h from ${f(per.windDeg[i], 0)}°`);
    if (!short && Number.isFinite(per.airspeed[i])) parts.push(`airspeed ≈ ${f(per.airspeed[i] * 3.6, 0)} km/h`);
    const said = ctx.states.events.filter((e) => Math.abs(e.t - t) <= 1.5).map((e) => e.text);
    if (said.length) parts.push(`says: “${said.join("; ")}”`);
    return parts.join(" · ");
  }

  onChartHover(t) {
    const el = $("stateinfo");
    if (!el) return;
    el.textContent = t == null ? "" : `${this.fmtTime(t, true)} · ${this.stateText(t) || "no flight state"}`;
  }

  onMapLegend(legend, col) {
    const c = $("legend").getContext("2d");
    c.clearRect(0, 0, 160, 8);
    if (legend.discrete) {
      const w = 160 / legend.discrete.length;
      legend.discrete.forEach((k, i) => { const rgb = col(i); c.fillStyle = `rgb(${rgb[0]},${rgb[1]},${rgb[2]})`; c.fillRect(i * w, 0, w - 1, 8); });
      $("legendtext").textContent = legend.discrete.join(" · ");
      return;
    }
    for (let x = 0; x < 160; x++) {
      const v = legend.lo + (legend.hi - legend.lo) * x / 159;
      const rgb = col(v);
      c.fillStyle = `rgb(${rgb[0]},${rgb[1]},${rgb[2]})`;
      c.fillRect(x, 0, 1, 8);
    }
    const d = legend.unit === "m/s" ? 0 : 0;
    $("legendtext").textContent = `${legend.lo.toFixed(d)} … ${legend.hi.toFixed(d)} ${legend.unit}`;
  }

  async copy(text) {
    try { await navigator.clipboard.writeText(text); } catch { console.log(text); }
  }
}

const app = new App();
window.__ubaApp = app;
app.init().catch((e) => console.error(e));
