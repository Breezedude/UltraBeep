/*
 * The flight path on the terrain of the 3D map (no DOM). The terrain model never matches the GNSS
 * altitude exactly (DEM errors of 10-30 m in the mountains, a few metres of GNSS altitude error), so
 * the track would hang in the air or run under the ground at takeoff and landing. The flight data
 * stay as they are; the terrain moves instead, by one offset for the whole track: the lowest of the
 * ground offsets (GNSS altitude minus terrain) before the takeoffs and after the landings. That ground
 * phase lies on the surface, every other one at or above it (2026-10-12: +14 m at the launch, -14 m
 * at the landing; the landing lies on the ground, the launch 27 m above it). Where a point would still
 * lie under the shifted terrain (DEM errors along the way), it is drawn on the surface; the map draws
 * every point at its height above its own terrain mesh (deck.gl terrain extension, offset mode), so no
 * point lies under the drawn surface at any zoom, and segments that come close to the terrain between
 * their ends are split, so that no line cuts through a crest between two fixes.
 *
 * dem(lon, lat) returns the terrain height in metres (NaN where unknown).
 */

const M_PER_DEG = 111319.49;
const GROUND_S = 10;       /* seconds of fixes before a takeoff and after a landing that count */
const MIN_FIXES = 3;       /* fewer fixes in a ground phase: no offset from it */
const AIRBORNE_GAP_M = 60; /* the end of a track without a landing further off: it ends in the air */
const PLAUSIBLE_M = 100;   /* a ground offset beyond this is no ground phase */

const median = (v) => {
  if (!v.length) return NaN;
  const s = [...v].sort((a, b) => a - b);
  return s.length % 2 ? s[(s.length - 1) / 2] : (s[s.length / 2 - 1] + s[s.length / 2]) / 2;
};

/* Median of (GNSS altitude - terrain) over the fixes from t0 to t1 ({t, lon, lat, alt}, sorted by t). */
export function groundOffset(fixes, t0, t1, dem) {
  const d = [];
  for (const f of fixes) {
    if (f.t < t0 || f.t > t1) continue;
    const h = dem(f.lon, f.lat);
    if (Number.isFinite(h) && Number.isFinite(f.alt)) d.push(f.alt - h);
  }
  return d.length >= MIN_FIXES ? median(d) : NaN;
}

/*
 * The terrain offset of the whole track: the lowest offset of its ground phases, GROUND_S seconds
 * before every takeoff and after every landing (times in s, as the fixes). Without a takeoff the start
 * counts; when the track does not end with a landing, its end counts too unless it lies far off the
 * other ground phases (the track ends in the air). Returns {offset, phases: [{kind, t0, t1, offset}],
 * ground: the phase that sets the offset, or null (offset 0)}.
 */
export function terrainOffset(fixes, takeoffs, landings, dem) {
  const phases = [];
  if (!fixes.length) return { offset: 0, phases, ground: null };
  const first = fixes[0].t, last = fixes[fixes.length - 1].t;
  const phase = (kind, t0, t1) => {
    const offset = groundOffset(fixes, Math.max(first, t0), Math.min(last, t1), dem);
    return Number.isFinite(offset) && Math.abs(offset) <= PLAUSIBLE_M ? { kind, t0, t1, offset } : null;
  };
  const add = (p) => { if (p) phases.push(p); };
  if (!takeoffs.length) add(phase("start", first, first + GROUND_S));
  for (const t of takeoffs) add(phase("takeoff", t - GROUND_S, t));
  for (const t of landings) add(phase("landing", t, t + GROUND_S));
  const lastTakeoff = takeoffs.length ? Math.max(...takeoffs) : -Infinity;
  if (!landings.some((t) => t > lastTakeoff)) {
    const end = phase("end", last - GROUND_S, last);
    if (end && (!phases.length || phases.some((p) => Math.abs(p.offset - end.offset) <= AIRBORNE_GAP_M))) phases.push(end);
  }
  let ground = null;
  for (const p of phases) if (!ground || p.offset < ground.offset) ground = p;
  return { offset: ground ? ground.offset : 0, phases, ground };
}

function metresBetween(a, b) {
  const dy = (b[1] - a[1]) * M_PER_DEG;
  const dx = (b[0] - a[0]) * M_PER_DEG * Math.cos(((a[1] + b[1]) / 2) * Math.PI / 180);
  return Math.hypot(dx, dy);
}

/*
 * A path of [lon, lat, alt] as [lon, lat, height above the shifted terrain], at least minAgl. Where the
 * straight line between two points comes closer than `clearance` to the terrain (or below it), the
 * segment is split every stepM metres and each new point lifted the same way.
 */
export function liftPath(path, dem, offset, { minAgl = 2, stepM = 8, clearance = 10 } = {}) {
  const agl = (lon, lat, alt) => {
    const h = dem(lon, lat);
    return Math.max(minAgl, alt - (Number.isFinite(h) ? h + offset : alt));
  };
  const out = [];
  for (let k = 0; k < path.length; k++) {
    const b = path[k];
    if (k > 0) {
      const a = path[k - 1];
      const n = Math.ceil(metresBetween(a, b) / stepM);
      if (n > 1) {
        const inner = [];
        let close = false;
        for (let j = 1; j < n; j++) {
          const f = j / n;
          const p = [a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f, a[2] + (b[2] - a[2]) * f];
          const h = dem(p[0], p[1]);
          if (Number.isFinite(h) && p[2] - (h + offset) < clearance) close = true;
          inner.push(p);
        }
        if (close) for (const p of inner) out.push([p[0], p[1], agl(p[0], p[1], p[2])]);
      }
    }
    out.push([b[0], b[1], agl(b[0], b[1], b[2])]);
  }
  return out;
}

/* The altitude the map draws a point at: on the shifted terrain at least minAgl above it. */
export function drawnAltitude(lon, lat, alt, dem, offset, minAgl = 2) {
  const h = dem(lon, lat);
  return Number.isFinite(h) ? Math.max(alt, h + offset + minAgl) : alt;
}
