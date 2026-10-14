/*
 * Algorithms the analyzer can replay. An algorithm module default-exports
 *
 *   { id, name, description,
 *     params: [{key, value, min, max, step, unit, doc, advanced}],
 *     diagnostics: [{key, label, unit}],
 *     create(params, context) -> instance }
 *
 * and the instance offers (all times in ms of the device clock, inputs read-only):
 *
 *   imu(tMs, w[3] rad/s, a[3] m/s², dt s)    every IMU sample (100 Hz)
 *   baro(tMs, altitudeM, pressurePa)          every barometer sample (32 Hz); afterwards
 *   vz, h                                     are read as the outputs of this sample
 *   fix(tMs, fix)                             every completed GNSS epoch (see nmea.js)
 *   event(tMs, text)                          raw log events ("tap ...", "button ...", ...)
 *   diag(out)                                 fills out[key] for its diagnostics, once per barometer sample
 *   baroDt                                    set by the replay: 1 / barometer rate
 *
 * Private algorithms (e.g. reconstructions of other firmware) live in tools/analyzer/private/
 * (git-ignored): private/index.js default-exports a list of such modules.
 */
import fusion from "../algorithms/fusion.js";
import baroKf from "../algorithms/baro_kf.js";

export const builtin = [fusion, baroKf];

export async function loadAlgorithms() {
  const list = [...builtin];
  try {
    const mod = await import("../../private/index.js");
    for (const a of mod.default || []) list.push({ ...a, private: true });
  } catch {
    /* no private algorithms */
  }
  return list;
}

export function defaultParams(algo) {
  const p = {};
  for (const d of algo.params) p[d.key] = d.value;
  return p;
}

/* "KP=0.5,ACCEL_GATE=0.1" -> {KP: 0.5, ACCEL_GATE: 0.1} (unknown keys are an error). */
export function parseParamText(algo, text) {
  const p = defaultParams(algo);
  if (!text) return p;
  for (const part of text.split(",")) {
    const [k, v] = part.split("=").map((x) => x.trim());
    if (!k) continue;
    if (!(k in p)) throw new Error(`${algo.id}: unknown parameter ${k} (known: ${Object.keys(p).join(", ")})`);
    const x = Number(v);
    if (!Number.isFinite(x)) throw new Error(`${algo.id}: ${k} needs a number`);
    p[k] = x;
  }
  return p;
}
