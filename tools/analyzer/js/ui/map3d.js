/*
 * 3D map (deck.gl, loaded on first use): Esri World Imagery on Mapzen/AWS terrain tiles, the GNSS
 * track in 3D as a thin outlined line coloured by a chosen value, the replay position as a marker
 * with altitude and climb, and a follow mode: the camera glides behind the pilot during the replay
 * (heading, pitch 60°) and holds its bearing while circling. The view's focal point sits at the
 * pilot's altitude (view state `position`), so the left mouse button rotates around the pilot in
 * 3D, not around the ground below; the right one (or ctrl) moves the map. Click on the track sets
 * the cursor.
 *
 * Terrain fit (core/terrain.js): the flight data stay as they are; the terrain is shifted by one
 * offset for the whole track, the lowest ground offset at a takeoff or landing, so that this ground
 * phase lies on the surface and the rest of the track at or above it. The track is drawn as height
 * above the drawn terrain (deck.gl terrain extension, offset mode), on the surface where it would
 * still run under it, so no line cuts the terrain at any zoom. Until the heights along the track are
 * loaded, or if a tile fails, the track keeps its GNSS altitudes.
 *
 * Takeoff (green) and landing (red) as the firmware detects them with its default settings
 * (core/flightstate.js), labelled with the time they are dated to; a click moves the cursor there.
 *
 * The replay position is a paraglider (models/glider.glb, built by models/make-glider.mjs), scaled
 * by the "glider" slider: placed between the fixes, its heading the GNSS course (between two fixes
 * shaped by the gyro's turn rate, so a spiral turns smoothly at 1 Hz fixes), bank and pitch from the
 * IMU (core/attitude.js). Without an IMU, or when the log has no turns to find the device's mounting
 * from, it banks as in a coordinated turn (the flight states' bank estimate) and stays level.
 */
/* global deck */
import { KINDS, KIND_COLORS, stateAt } from "../core/flightstate.js";
import { drawnAltitude, liftPath, terrainOffset } from "../core/terrain.js";
import { attitudeAt, pilotAttitude } from "../core/attitude.js";
import { Dem } from "./dem.js";

const DECK_URLS = ["vendor/deckgl/dist.min.js"]; /* deck.gl 9.1.14, served with the page */
const IMAGERY = "https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}";
const TERRAIN = "https://s3.amazonaws.com/elevation-tiles-prod/terrarium/{z}/{x}/{y}.png";

const FOLLOW_ZOOM = 15.2;
const FOLLOW_PITCH = 62;
const FOLLOW_STEP_DEG = 2.5;   /* bearing change per update (10 per second while playing): no spinning */
const CIRCLING = new Set(["thermal", "circling", "spiral"]);
const MARK_COLORS = { takeoff: [61, 220, 132], landing: [255, 92, 92] };
const GLIDER_URL = "models/glider.glb";
export const GLIDER_SCALE = 10;  /* default of the size slider: times the true size (about 9 m span) */
const COURSE_MIN_MS = 2;         /* the GNSS course counts from this ground speed */
const FIX_GAP_S = 3;             /* no interpolation of the position across longer gaps */
const COURSE_GAP_S = 10;         /* ... of the heading */

function loadScript(src) {
  return new Promise((resolve, reject) => {
    const s = document.createElement("script");
    s.src = src;
    s.onload = resolve;
    s.onerror = () => reject(new Error(`cannot load ${src}`));
    document.head.append(s);
  });
}

/* sink blue -> grey -> lift orange/red, for ±range */
export function climbColor(v, range = 3) {
  if (!Number.isFinite(v)) return [120, 120, 120];
  const x = Math.max(-1, Math.min(1, v / range));
  if (x < 0) {
    const k = -x;
    return [Math.round(200 - 170 * k), Math.round(205 - 90 * k), Math.round(210 + 45 * k)];
  }
  return [Math.round(200 + 55 * x), Math.round(205 - 140 * x), Math.round(210 - 190 * x)];
}

function rampColor(x) { // 0..1 viridis-like
  const stops = [[68, 1, 84], [59, 82, 139], [33, 145, 140], [94, 201, 98], [253, 231, 37]];
  const p = Math.max(0, Math.min(1, x)) * (stops.length - 1);
  const i = Math.min(stops.length - 2, Math.floor(p)), f = p - i;
  return stops[i].map((c, k) => Math.round(c + (stops[i + 1][k] - c) * f));
}

const wrap180 = (d) => { while (d > 180) d -= 360; while (d < -180) d += 360; return d; };

/* last index k with key(arr[k]) <= x in an ascending array, or -1 */
function lowerIndex(arr, x, key = (v) => v) {
  let lo = 0, hi = arr.length - 1;
  if (hi < 0 || key(arr[0]) > x) return -1;
  while (lo < hi) { const m = (lo + hi + 1) >> 1; if (key(arr[m]) <= x) lo = m; else hi = m - 1; }
  return lo;
}

/*
 * Column-major model matrix of the glider in the map's local metres (x east, y north, z up): the
 * model's axes as glTF has them (+X left, +Y up, +Z forward) turned to the heading (degrees clockwise
 * from north), then pitched (nose up positive) and banked (right wing down positive).
 */
export function modelMatrix(heading, pitch, bank) {
  const h = heading * Math.PI / 180, th = pitch * Math.PI / 180, ph = bank * Math.PI / 180;
  const f0 = [Math.sin(h), Math.cos(h), 0], r0 = [Math.cos(h), -Math.sin(h), 0];
  const ct = Math.cos(th), st = Math.sin(th), cp = Math.cos(ph), sp = Math.sin(ph);
  const f = [f0[0] * ct, f0[1] * ct, st];
  const u1 = [-f0[0] * st, -f0[1] * st, ct];
  const r = [0, 1, 2].map((k) => r0[k] * cp - u1[k] * sp);
  const u = [0, 1, 2].map((k) => u1[k] * cp + r0[k] * sp);
  return [-r[0], -r[1], -r[2], 0, u[0], u[1], u[2], 0, f[0], f[1], f[2], 0, 0, 0, 0, 1];
}

export class MapView {
  constructor(el, app) {
    this.el = el;
    this.app = app;
    this.deck = null;
    this.loading = null;
    this.segments = [];
    this.marker = null;
    this.markerText = "";
    this.terrain = true;
    this.colorBy = "run:0";
    this.error = null;
    this.follow = true;
    this.followZoom = FOLLOW_ZOOM;
    this.followPitch = FOLLOW_PITCH;
    this.bearing = 0;
    this.userRotatedUntil = 0;
    this.lastView = null;
    this.fit = null;      /* {offset, phases, ground, height(lon, lat)} once the terrain heights are in */
    this.marks = [];      /* takeoff and landing */
    this.fitToken = 0;
    this.terrainExt = null;
    this.gliderScale = GLIDER_SCALE;
    this.gliderFailed = false;
    this.pose = null;     /* the glider: {p: [lon, lat, alt], heading, bank, pitch, m: model matrix} */
  }

  async ensure() {
    if (this.deck) return true;
    if (!this.loading) {
      this.loading = (async () => {
        for (const url of DECK_URLS) {
          try {
            if (typeof deck === "undefined") await loadScript(url);
            if (typeof deck !== "undefined") break;
          } catch (e) { this.error = e.message; }
        }
        if (typeof deck === "undefined") throw new Error("deck.gl could not be loaded (offline?)");
        this.deck = new deck.Deck({
          parent: this.el,
          initialViewState: { longitude: 11, latitude: 47.5, zoom: 8, pitch: 55, bearing: 0, maxPitch: 85 },
          controller: { inertia: 200, dragMode: "rotate" }, /* left drag rotates, right or ctrl drag pans */
          getTooltip: ({ object }) => object && object.tip ? { text: object.tip, style: { fontSize: "12px" } } : null,
          onClick: (info) => { if (info.object && info.object.t != null) this.app.setCursor(info.object.t); },
          onViewStateChange: ({ viewState, interactionState }) => {
            this.lastView = viewState;
            const is = interactionState || {};
            if (is.isZooming || is.isPanning || is.isDragging) { this.followZoom = viewState.zoom; this.followPitch = viewState.pitch; }
            if (is.isRotating) { this.followPitch = viewState.pitch; this.bearing = viewState.bearing; this.userRotatedUntil = Date.now() + 8000; }
          },
        });
        this.terrainExt = deck._TerrainExtension ? new deck._TerrainExtension() : null;
      })();
    }
    try {
      await this.loading;
      return true;
    } catch (e) {
      this.el.querySelector(".attrib").textContent = e.message;
      return false;
    }
  }

  /* track from the valid GNSS fixes, at most every 0.5 s */
  setTrack(ctx, runs) {
    this.ctx = ctx;
    this.runs = runs;
    const fx = ctx.fixes;
    const idx = [];
    let last = -Infinity;
    for (let i = 0; i < fx.n; i++) {
      if (!fx.valid[i]) continue;
      if (ctx.fixT[i] - last < 0.5) continue;
      idx.push(i);
      last = ctx.fixT[i];
    }
    this.idx = idx;
    this.fit = null;
    this.prepareGlider();
    this.recolor();
    this.fitTerrain();
    this.fitView();
  }

  /* loads the terrain heights along the track, then shifts the terrain and lifts the track (see the top) */
  async fitTerrain() {
    const token = ++this.fitToken;
    const ctx = this.ctx, fx = ctx.fixes;
    if (!this.idx.length) return;
    const dem = new Dem();
    let ok = false;
    try {
      ok = await dem.load(this.idx.map((i) => [fx.lon[i], fx.lat[i]]));
    } catch (e) {
      ok = false;
    }
    if (token !== this.fitToken || !ok) return;
    const height = (lon, lat) => dem.height(lon, lat);
    const fixes = this.idx.map((i) => ({ t: ctx.fixT[i], lon: fx.lon[i], lat: fx.lat[i], alt: fx.altMsl[i] }));
    const events = ctx.states ? ctx.states.events : [];
    const times = (kind) => events.filter((e) => e.kind === kind).map((e) => e.t);
    this.fit = { ...terrainOffset(fixes, times("takeoff"), times("landing"), height), height };
    this.recolor();
    if (this.app.state.cursor != null) this.setPlayT(this.app.state.cursor);
  }

  /* the track is drawn on the fitted terrain: heights above it instead of GNSS altitudes */
  onTerrain() {
    return !!(this.terrain && this.fit && this.terrainExt);
  }

  valueAt(i, how) {
    const ctx = this.ctx;
    const t = ctx.fixT[i];
    if (how === "state") { const s = ctx.states ? stateAt(ctx.states, ctx.series, t) : null; return s ? KINDS.indexOf(s.kind) : NaN; }
    if (how === "speed") return ctx.fixes.speed[i] * 3.6;
    if (how === "alt") return ctx.fixes.altMsl[i];
    if (how === "gnssclimb") return -ctx.fixes.vd[i];
    if (how.startsWith("run:")) {
      const r = this.runs.find((x) => x.id === how.slice(4)) || this.runs[0];
      if (!r || !r.result) return NaN;
      // mean climb over the 0.5 s before the fix
      const tS = ctx.tS;
      let lo = 0, hi = tS.length - 1;
      while (lo < hi) { const m = (lo + hi + 1) >> 1; if (tS[m] <= t) lo = m; else hi = m - 1; }
      let s = 0, c = 0;
      for (let k = lo; k >= 0 && tS[k] > t - 0.5; k--) if (Number.isFinite(r.result.vz[k])) { s += r.result.vz[k]; c++; }
      return c ? s / c : NaN;
    }
    return NaN;
  }

  recolor() {
    if (!this.ctx) return;
    const ctx = this.ctx, fx = ctx.fixes, idx = this.idx;
    let how = this.colorBy;
    if (how === "state" && !ctx.states) how = "speed";
    const vals = idx.map((i) => this.valueAt(i, how));
    let lo = 0, hi = 1;
    if (how === "state") {
      this.legend = { lo: 0, hi: KINDS.length - 1, unit: "", ramp: false, discrete: KINDS };
    } else if (how === "speed" || how === "alt") {
      const f = vals.filter(Number.isFinite).sort((a, b) => a - b);
      lo = f[Math.floor(f.length * 0.02)] ?? 0;
      hi = f[Math.floor(f.length * 0.98)] ?? 1;
      this.legend = { lo, hi, unit: how === "speed" ? "km/h" : "m", ramp: true };
    } else this.legend = { lo: -3, hi: 3, unit: "m/s", ramp: false };
    const stateRgb = (v) => {
      const hex = KIND_COLORS[KINDS[v]] || "#808080";
      const x = parseInt(hex.slice(1), 16);
      return [(x >> 16) & 255, (x >> 8) & 255, x & 255];
    };
    const col = (v) => (how === "state" ? (Number.isFinite(v) ? stateRgb(v) : [120, 120, 120]) : this.legend.ramp ? rampColor((v - lo) / ((hi - lo) || 1)) : climbColor(v));
    this.segments = [];
    for (let k = 1; k < idx.length; k++) {
      const a = idx[k - 1], b = idx[k];
      if (ctx.fixT[b] - ctx.fixT[a] > 3) continue;
      const v = vals[k];
      const path = [[fx.lon[a], fx.lat[a], fx.altMsl[a]], [fx.lon[b], fx.lat[b], fx.altMsl[b]]];
      this.segments.push({
        path,
        agl: this.fit ? liftPath(path, this.fit.height, this.fit.offset) : null,
        c: col(v),
        t: ctx.fixT[b],
        tip: `${this.app.fmtTime(ctx.fixT[b], true)}\n${how === "state" ? (this.app.stateText(ctx.fixT[b], true) || "–") : `${Number.isFinite(v) ? v.toFixed(how === "alt" ? 0 : 2) : "–"} ${this.legend.unit}`}\n${fx.altMsl[b].toFixed(0)} m, ${(fx.speed[b] * 3.6).toFixed(0)} km/h`,
      });
    }
    this.marks = this.flightMarks();
    this.app.onMapLegend(this.legend, col);
    this.render();
  }

  /* takeoff and landing as the firmware detects them (core/flightstate.js), at the fix of their time */
  flightMarks() {
    const ctx = this.ctx, fx = ctx.fixes, fmt = (t) => this.app.fmtTime(t, true);
    const clock = (t) => fmt(t).split(" · ").pop(); /* the UTC time alone when there is one */
    const out = [];
    for (const e of ctx.states ? ctx.states.events : []) {
      if (!MARK_COLORS[e.kind]) continue;
      const i = this.fixIndexAt(e.t);
      if (i < 0) continue;
      const p = [fx.lon[i], fx.lat[i], fx.altMsl[i]];
      out.push({
        p, agl: this.fit ? liftPath([p], this.fit.height, this.fit.offset)[0] : null, c: MARK_COLORS[e.kind], t: e.t,
        text: `${e.kind} ${clock(e.t)}`,
        tip: `${e.kind} ${fmt(e.t)}\n${(e.rules || []).join(", ")}, detected ${fmt(e.detectedT ?? e.t)}`,
      });
    }
    return out;
  }

  /* fix index at or before t (valid), or -1 */
  fixIndexAt(t) {
    const fx = this.ctx.fixes, fixT = this.ctx.fixT;
    let lo = 0, hi = fx.n - 1;
    while (lo < hi) { const m = (lo + hi + 1) >> 1; if (fixT[m] <= t) lo = m; else hi = m - 1; }
    let i = lo;
    while (i > 0 && !fx.valid[i]) i--;
    return fx.valid[i] ? i : -1;
  }

  setPlayT(t) {
    if (!this.ctx || t == null) { this.marker = null; this.render(); return; }
    const fx = this.ctx.fixes;
    const i = this.fixIndexAt(t);
    this.marker = i >= 0 ? [fx.lon[i], fx.lat[i], fx.altMsl[i]] : null;
    this.markerAgl = i >= 0 && this.fit ? liftPath([this.marker], this.fit.height, this.fit.offset)[0] : null;
    this.pose = i >= 0 ? this.gliderPose(t) : null;
    if (i >= 0) {
      const tone = this.runs && this.runs.find((r) => r.id === this.app.state.toneRunId);
      const climb = tone ? this.valueAt(i, `run:${tone.id}`) : NaN;
      this.markerText = `${fx.altMsl[i].toFixed(0)} m${Number.isFinite(climb) ? ` · ${climb >= 0 ? "+" : ""}${climb.toFixed(1)} m/s` : ""}`;
      if (this.follow) this.followTo(i, t);
    }
    this.render();
  }

  /* heading over the last few seconds of the track, degrees, or NaN while slow */
  headingAt(i) {
    const fx = this.ctx.fixes, fixT = this.ctx.fixT;
    let j = i;
    while (j > 0 && fixT[i] - fixT[j] < 4) j--;
    const dy = (fx.lat[i] - fx.lat[j]) * 111319;
    const dx = (fx.lon[i] - fx.lon[j]) * 111319 * Math.cos(fx.lat[i] * Math.PI / 180);
    if (Math.hypot(dx, dy) < 8) return NaN;
    return Math.atan2(dx, dy) * 180 / Math.PI;
  }

  /* what the glider needs per log: the valid fixes, those with a course, the turn rate integrated, the attitude */
  prepareGlider() {
    const ctx = this.ctx, fx = ctx.fixes;
    if (this.gliderCtx === ctx) return;
    this.gliderCtx = ctx;
    this.validFix = [];
    this.courseFix = [];
    for (let i = 0; i < fx.n; i++) {
      if (!fx.valid[i]) continue;
      this.validFix.push(i);
      if (fx.speed[i] >= COURSE_MIN_MS && Number.isFinite(fx.course[i])) this.courseFix.push(i);
    }
    // the gyro's turn rate (clockwise positive, as the course) integrated on the barometer grid, degrees
    this.yawSum = null;
    if (ctx.yawRate && ctx.kind !== "track") {
      const n = ctx.tS.length, y = new Float64Array(n);
      for (let k = 1; k < n; k++) {
        const r = ctx.yawRate[k];
        y[k] = y[k - 1] + (Number.isFinite(r) ? r : 0) * (ctx.tS[k] - ctx.tS[k - 1]);
      }
      this.yawSum = y;
    }
    const sum = ctx.states ? ctx.states.summary : {};
    try {
      this.attitude = pilotAttitude(ctx, { takeoffT: sum.takeoffT ?? null, landingT: sum.landingT ?? null });
    } catch (e) {
      console.error("attitude", e);
      this.attitude = null;
    }
  }

  /* the turn rate integrated up to t (degrees), or NaN without a gyro */
  yawAt(t) {
    const y = this.yawSum, tS = this.ctx.tS;
    if (!y) return NaN;
    const k = lowerIndex(tS, t);
    if (k < 0) return y[0];
    if (k >= tS.length - 1) return y[tS.length - 1];
    return y[k] + (y[k + 1] - y[k]) * (t - tS[k]) / (tS[k + 1] - tS[k]);
  }

  /* heading at t, degrees clockwise from north: the GNSS course of the fixes around it, and between them
     the gyro's turn rate from either side, blended (exact at both fixes); NaN without a course nearby */
  headingAtT(t) {
    const fx = this.ctx.fixes, fixT = this.ctx.fixT, cf = this.courseFix;
    const j = lowerIndex(cf, t, (i) => fixT[i]);
    const a = j >= 0 && t - fixT[cf[j]] <= COURSE_GAP_S ? cf[j] : -1;
    const b = j + 1 < cf.length && fixT[cf[j + 1]] - t <= COURSE_GAP_S ? cf[j + 1] : -1;
    if (a < 0 && b < 0) return NaN;
    const yaw = this.yawAt(t);
    const from = (i) => fx.course[i] + (Number.isFinite(yaw) ? yaw - this.yawAt(fixT[i]) : 0);
    if (a < 0) return from(b);
    if (b < 0) return from(a);
    const f = (t - fixT[a]) / (fixT[b] - fixT[a]);
    const h = from(a);
    return h + wrap180(from(b) - h) * f;
  }

  /* bank and pitch at t, degrees: the IMU's, or the bank of a coordinated turn from the flight states */
  attitudeAtT(t) {
    const att = attitudeAt(this.attitude, t);
    if (att) return att;
    const ctx = this.ctx, st = ctx.states && stateAt(ctx.states, ctx.series, t);
    if (!st) return { bank: 0, pitch: 0 };
    const per = ctx.states.per, i = st.i;
    const at = (k) => (Number.isFinite(per.bank[k]) && Number.isFinite(per.turn[k]) ? Math.sign(per.turn[k]) * per.bank[k] : 0);
    const f = Math.max(0, Math.min(1, t - ctx.series.t[i]));
    return { bank: at(i) + (at(Math.min(i + 1, ctx.series.n - 1)) - at(i)) * f, pitch: 0 };
  }

  /* the glider at t: position between the fixes, the model matrix from heading, pitch and bank */
  gliderPose(t) {
    const ctx = this.ctx, fx = ctx.fixes, fixT = ctx.fixT, vf = this.validFix;
    const j = lowerIndex(vf, t, (i) => fixT[i]);
    if (j < 0) return null;
    const a = vf[j], b = j + 1 < vf.length ? vf[j + 1] : -1;
    let lon = fx.lon[a], lat = fx.lat[a], alt = fx.altMsl[a];
    if (b >= 0 && fixT[b] - fixT[a] <= FIX_GAP_S) {
      const f = (t - fixT[a]) / (fixT[b] - fixT[a]);
      lon += (fx.lon[b] - lon) * f;
      lat += (fx.lat[b] - lat) * f;
      alt += (fx.altMsl[b] - alt) * f;
    }
    if (this.onTerrain()) alt = drawnAltitude(lon, lat, alt, this.fit.height, this.fit.offset);
    let heading = this.headingAtT(t);
    if (!Number.isFinite(heading)) heading = this.pose ? this.pose.heading : 0;
    const { bank, pitch } = this.attitudeAtT(t);
    return { p: [lon, lat, alt], heading, bank, pitch, m: modelMatrix(heading, pitch, bank) };
  }

  setGliderScale(v) {
    this.gliderScale = v;
    this.render();
  }

  /* the camera behind the pilot: during the replay in small steps, on a click with a flight */
  followTo(i, t) {
    if (!this.deck) return;
    const fx = this.ctx.fixes;
    const playing = this.app.player.playing;
    const state = this.ctx.states ? stateAt(this.ctx.states, this.ctx.series, t) : null;
    const circling = state && CIRCLING.has(state.kind);
    const heading = this.headingAt(i);
    if (Number.isFinite(heading) && !circling && Date.now() > this.userRotatedUntil) {
      const d = wrap180(heading - this.bearing);
      const step = playing ? Math.max(-FOLLOW_STEP_DEG, Math.min(FOLLOW_STEP_DEG, d)) : d;
      this.bearing = wrap180(this.bearing + step);
    }
    // the pilot, at flight altitude, is the focal point: a left drag orbits around the position in 3D
    const alt = this.onTerrain() ? drawnAltitude(fx.lon[i], fx.lat[i], fx.altMsl[i], this.fit.height, this.fit.offset) : fx.altMsl[i];
    const view = {
      longitude: fx.lon[i], latitude: fx.lat[i], position: [0, 0, alt],
      zoom: this.followZoom, pitch: this.followPitch, bearing: this.bearing, maxPitch: 85,
      transitionDuration: playing ? 130 : 900,
      transitionInterpolator: new deck.LinearInterpolator(["longitude", "latitude", "position", "zoom", "pitch"]),
    };
    this.deck.setProps({ initialViewState: view });
  }

  setFollow(on) {
    this.follow = on;
    if (on && this.app.state.cursor != null) this.setPlayT(this.app.state.cursor);
  }

  render() {
    if (!this.deck) return;
    const layers = [];
    const tileProps = { refinementStrategy: "best-available", maxRequests: 8, onTileError: () => {} };
    const onTerrain = this.onTerrain();
    const ext = onTerrain ? { extensions: [this.terrainExt], terrainDrawMode: "offset" } : {};
    if (this.terrain) {
      layers.push(new deck.TerrainLayer({
        id: "terrain", minZoom: 0, maxZoom: 13, strategy: "no-overlap", meshMaxError: 6, ...tileProps,
        operation: "terrain+draw", /* the height map the track is drawn on */
        elevationDecoder: { rScaler: 256, gScaler: 1, bScaler: 1 / 256, offset: -32768 + (this.fit ? this.fit.offset : 0) },
        elevationData: TERRAIN, texture: IMAGERY,
      }));
    } else {
      layers.push(new deck.TileLayer({
        id: "imagery", data: IMAGERY, minZoom: 0, maxZoom: 19, tileSize: 256, ...tileProps,
        renderSubLayers: (props) => {
          const tile = props.tile;
          const bb = tile.boundingBox ? [tile.boundingBox[0][0], tile.boundingBox[0][1], tile.boundingBox[1][0], tile.boundingBox[1][1]]
            : [tile.bbox.west, tile.bbox.south, tile.bbox.east, tile.bbox.north];
          return new deck.BitmapLayer(props, { data: null, image: props.data, bounds: bb });
        },
      }));
    }
    // a thin coloured line with round joints, billboarded so the width stays in pixels when pitched
    layers.push(new deck.PathLayer({
      id: "track", data: this.segments, pickable: true, widthUnits: "pixels", getWidth: 2.2, capRounded: true, jointRounded: true, billboard: true,
      getPath: (d) => (onTerrain ? d.agl : d.path), getColor: (d) => d.c, ...ext,
      updateTriggers: { getColor: [this.colorBy, this.segments.length], getPath: [onTerrain, this.segments.length] },
    }));
    if (this.marks.length) {
      /* takeoff and landing: dots facing the camera, drawn over the track and the terrain */
      const marks = this.marks.map((m) => ({ ...m, pos: onTerrain && m.agl ? m.agl : m.p }));
      const onTop = { depthCompare: "always", depthWriteEnabled: false };
      layers.push(new deck.ScatterplotLayer({
        id: "flightmarks", data: marks, pickable: true, radiusUnits: "pixels", getRadius: 7, stroked: true, billboard: true, ...ext,
        getLineColor: [0, 0, 0], lineWidthUnits: "pixels", getLineWidth: 2, getPosition: (d) => d.pos, getFillColor: (d) => d.c,
        parameters: onTop,
      }));
      layers.push(new deck.TextLayer({
        id: "flightlabels", data: marks, getPosition: (d) => d.pos, getText: (d) => d.text, ...ext, parameters: onTop,
        getSize: 12, getColor: (d) => d.c, getPixelOffset: [0, -20], billboard: true,
        background: true, getBackgroundColor: [0, 0, 0, 170], backgroundPadding: [5, 2],
        fontFamily: "IBM Plex Mono, Consolas, monospace", fontSettings: { sdf: false },
      }));
    }
    if (this.marker) {
      const p = onTerrain && this.markerAgl ? this.markerAgl : this.marker;
      const pose = this.pose;
      if (pose && !this.gliderFailed) {
        /* the glider at its absolute drawn altitude: the terrain extension would shift a model by the ground below */
        layers.push(new deck.ScenegraphLayer({
          id: "glider", data: [pose], scenegraph: GLIDER_URL, _lighting: "pbr", sizeScale: this.gliderScale,
          getPosition: (d) => d.p, getTransformMatrix: (d) => d.m, getColor: [255, 255, 255, 255],
          updateTriggers: { getPosition: [pose], getTransformMatrix: [pose] },
          onError: (e) => { console.error("glider model", e); this.gliderFailed = true; this.render(); return true; },
        }));
      } else {
        layers.push(new deck.ScatterplotLayer({
          id: "pos", data: [{ p }], radiusUnits: "pixels", getRadius: 6, stroked: true, ...ext,
          getLineColor: [0, 0, 0], lineWidthUnits: "pixels", getLineWidth: 2,
          getPosition: (d) => d.p, getFillColor: [255, 214, 10], updateTriggers: { getPosition: [p] },
        }));
      }
      layers.push(new deck.TextLayer({
        id: "poslabel", data: [{ p, text: this.markerText }], getPosition: (d) => d.p, getText: (d) => d.text, ...ext,
        getSize: 13, getColor: [255, 255, 255], getPixelOffset: [0, -22], billboard: true,
        background: true, getBackgroundColor: [0, 0, 0, 170], backgroundPadding: [6, 3],
        fontFamily: "IBM Plex Mono, Consolas, monospace", fontSettings: { sdf: false },
        updateTriggers: { getPosition: [p], getText: [this.markerText] },
      }));
    }
    this.deck.setProps({ layers });
  }

  fitView() {
    if (!this.deck || !this.ctx || !this.idx.length) return;
    const fx = this.ctx.fixes;
    let w = Infinity, e = -Infinity, s = Infinity, n = -Infinity;
    for (const i of this.idx) {
      w = Math.min(w, fx.lon[i]); e = Math.max(e, fx.lon[i]);
      s = Math.min(s, fx.lat[i]); n = Math.max(n, fx.lat[i]);
    }
    /* fitBounds fails on a view smaller than its padding: the map may not be laid out yet while loading */
    const width = Math.max(300, this.el.clientWidth), height = Math.max(300, this.el.clientHeight);
    const pad = 0.0015;
    const vp = new deck.WebMercatorViewport({ width, height }).fitBounds([[w - pad, s - pad], [e + pad, n + pad]], { padding: 30 });
    this.bearing = 0;
    this.deck.setProps({
      initialViewState: { longitude: vp.longitude, latitude: vp.latitude, position: [0, 0, 0], zoom: Math.min(15, vp.zoom), pitch: 55, bearing: 0, maxPitch: 85, transitionDuration: 300 },
    });
  }
}
