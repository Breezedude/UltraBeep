#!/usr/bin/env node
/*
 * Command line of the raw log analyzer: the same modules as the browser UI (index.html).
 *
 *   node cli.mjs algos                                     algorithms and their parameters
 *   node cli.mjs info LOG [--json]                         header, streams, sensor health
 *   node cli.mjs run LOG [--algo ID[:K=V,...]]... [opts]   replay and metrics
 *   node cli.mjs sweep LOG --algo ID[:K=V,...] --grid K=v1,v2,... [--grid ...] [opts]
 *   node cli.mjs export LOG OUTDIR                         baro.csv, imu.csv, nmea.txt, events.csv
 *   node cli.mjs states LOG|FLIGHT.igc [--json]            flight states, derived values and announcements
 *
 * opts: --range T0-T1 (seconds since the log start), --flat-ref (reference window not widened
 *       to whole rotations in turns), --ref S (half window of the reference,
 *       default 2), --csv FILE (run: series on the barometer grid), --sort KEY (sweep, default
 *       score), --top N, --json (machine-readable output)
 */
import { mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { G0, parseRawLog } from "./js/core/rawlog.js";
import { buildContext, loggedRun, runAlgorithms, setReference } from "./js/core/replay.js";
import { computeMetrics, score } from "./js/core/metrics.js";
import { analyzeHealth } from "./js/core/health.js";
import { loadAlgorithms, parseParamText } from "./js/core/registry.js";
import { parseIgc } from "./js/core/igc.js";
import { buildTrackContext, flightStatesOf } from "./js/core/track.js";

function parseArgs(argv) {
  const a = { _: [], algo: [], grid: [] };
  for (let i = 0; i < argv.length; i++) {
    const x = argv[i];
    if (x === "--json") a.json = true;
    else if (x === "--algo") a.algo.push(argv[++i]);
    else if (x === "--grid") a.grid.push(argv[++i]);
    else if (x.startsWith("--")) a[x.slice(2)] = argv[++i];
    else a._.push(x);
  }
  return a;
}

function load(path, args) {
  const log = parseRawLog(readFileSync(path), path);
  const ctx = buildContext(log, { refHalf: args.ref ? Number(args.ref) : undefined, turnRef: !args["flat-ref"] });
  return { log, ctx };
}

function range(args) {
  if (!args.range) return {};
  const [t0, t1] = args.range.split("-").map(Number);
  return { t0, t1 };
}

function specFor(algos, text, label) {
  const [id, p] = text.split(":");
  const algo = algos.find((x) => x.id === id);
  if (!algo) throw new Error(`unknown algorithm ${id} (known: ${algos.map((x) => x.id).join(", ")})`);
  return { algo, params: parseParamText(algo, p), label: label || text };
}

const f = (v, d = 3) => (Number.isFinite(v) ? v.toFixed(d) : "-");

function printMetrics(rows) {
  const cols = [["label", 28], ["lag s", 7], ["corr", 6], ["rms al", 7], ["noise", 7], ["bias", 7],
    ["falseL%", 8], ["missL%", 7], ["gnss", 6], ["switch/m", 9], ["handl", 6], ["score", 6]];
  console.log(cols.map(([c, w]) => c.padEnd(w)).join(" "));
  for (const m of rows) {
    const v = [m.label.slice(0, 28), f(m.lag, 2), f(m.corr, 3), f(m.rmsAligned), f(m.noiseHF), f(m.bias),
      f(m.falseLiftPct, 2), f(m.missedLiftPct, 2), f(m.rmsGnss, 2), f(m.toneSwitchesPerMin, 1),
      f(m.handlingMean, 2), f(score(m), 3)];
    console.log(v.map((x, i) => String(x).padEnd(cols[i][1])).join(" "));
  }
}

async function main() {
  const args = parseArgs(process.argv.slice(2));
  const cmd = args._[0];
  const algos = await loadAlgorithms();

  if (cmd === "algos") {
    const out = algos.map((a) => ({ id: a.id, name: a.name, private: !!a.private, description: a.description,
      params: a.params.map((p) => ({ key: p.key, value: p.value, unit: p.unit, doc: p.doc })) }));
    console.log(JSON.stringify(out, null, 2));
    return;
  }
  const path = args._[1];
  if (!path) throw new Error("usage: node cli.mjs info|run|sweep|states LOG ... (see the head of cli.mjs)");

  if (cmd === "states") {
    const isIgc = /\.igc$/i.test(path);
    const ctx = isIgc ? buildTrackContext(parseIgc(readFileSync(path, "latin1"), path)) : load(path, args).ctx;
    const { series, states } = flightStatesOf(ctx);
    const fmt = (t) => { const s = Math.round(t); return `${Math.floor(s / 3600)}:${String(Math.floor(s / 60) % 60).padStart(2, "0")}:${String(s % 60).padStart(2, "0")}`; };
    if (args.json) {
      console.log(JSON.stringify({ source: series.source, summary: states.summary, segments: states.segments, events: states.events }, null, 1));
      return;
    }
    const sm = states.summary;
    console.log(`${path}: ${series.source} series, ${series.n} s; IMU ${sm.imu ? "yes" : "no"}; takeoff ${sm.takeoffT == null ? "-" : fmt(sm.takeoffT)}, ` +
      `landing ${sm.landingT == null ? "-" : fmt(sm.landingT)}; thermals ${sm.thermals} (${(sm.thermalSeconds / 60).toFixed(0)} min, +${sm.thermalGainM.toFixed(0)} m, ` +
      `best ${f(sm.bestThermalClimb, 1)} m/s), glides ${sm.glides} (${(sm.glideSeconds / 60).toFixed(0)} min, median L/D ${f(sm.medianLD, 1)}), ` +
      `spirals ${sm.spirals}, wingovers ${sm.wingovers}, max load ${f(sm.maxLoad, 2)} g, wind ${sm.wind ? `${sm.wind.kmh.toFixed(0)} km/h from ${sm.wind.deg.toFixed(0)}°` : "-"}`);
    console.log("segments (start, end, kind, duration s, gain m, mean climb m/s, mean L/D):");
    for (const g of states.segments) {
      if (g.i1 - g.i0 < 5 && g.kind !== "spiral" && g.kind !== "wingover") continue;
      console.log(`  ${fmt(g.t0)} ${fmt(g.t1)} ${g.kind.padEnd(9)} ${String(g.i1 - g.i0 + 1).padStart(5)} ${f(g.gainM, 0).padStart(6)} ${f(g.meanClimb, 2).padStart(6)} ${f(g.meanLD, 1).padStart(6)}`);
    }
    console.log(`announcements (${states.events.length}):`);
    for (const e of states.events) console.log(`  ${fmt(e.t)} ${e.text}${e.rules ? ` (${e.rules.join(", ")}; detected ${fmt(e.detectedT)})` : ""}`);
    return;
  }
  const { log, ctx } = load(path, args);

  if (cmd === "export") {
    const dir = args._[2];
    if (!dir) throw new Error("usage: node cli.mjs export LOG OUTDIR");
    mkdirSync(dir, { recursive: true });
    const t = (ms) => ((ms - log.t0) / 1000).toFixed(3);
    const b = log.baro;
    const baro = ["t_s,p_raw,t_raw,pressure_pa,temp_c,filter_alt_m,filter_vz_ms"];
    for (let i = 0; i < b.n; i++) baro.push(`${t(b.t[i])},${b.pRaw[i]},${b.tRaw[i]},${b.p[i].toFixed(2)},${b.temp[i].toFixed(3)},${b.alt[i].toFixed(3)},${b.vz[i].toFixed(3)}`);
    writeFileSync(join(dir, "baro.csv"), baro.join("\n"));
    const h = log.header, ka = G0 / h.accelLsbG, kg = 1 / h.gyroLsbDps, r = log.imu.raw;
    const imu = ["t_s,ax_ms2,ay_ms2,az_ms2,gx_dps,gy_dps,gz_dps"];
    for (let i = 0; i < log.imu.n; i++) {
      const o = i * 6;
      imu.push(`${t(log.imu.t[i])},${(r[o] * ka).toFixed(4)},${(r[o + 1] * ka).toFixed(4)},${(r[o + 2] * ka).toFixed(4)},` +
        `${(r[o + 3] * kg).toFixed(3)},${(r[o + 4] * kg).toFixed(3)},${(r[o + 5] * kg).toFixed(3)}`);
    }
    writeFileSync(join(dir, "imu.csv"), imu.join("\n"));
    writeFileSync(join(dir, "nmea.txt"), log.nmea.map((x) => `${t(x.t)} ${x.text}`).join("\n"));
    writeFileSync(join(dir, "events.csv"), ["t_s,event", ...log.events.map((x) => `${t(x.t)},"${x.text.replace(/"/g, '""')}"`)].join("\n"));
    console.log(`written to ${dir}`);
    return;
  }

  if (cmd === "info") {
    const h = analyzeHealth(ctx);
    if (args.json) {
      delete h.summary.baro.noiseSeries;
      delete h.summary.imu.rests;
      console.log(JSON.stringify({ header: log.header, health: h }, null, 2));
      return;
    }
    console.log(`${path}: firmware ${log.header.firmware}, ${(h.summary.log.durationS / 60).toFixed(1)} min, ` +
      `baro ${log.baro.n}, IMU ${log.imu.n}, GNSS epochs ${ctx.fixes.n} (${ctx.fixRate}/s), events ${log.events.length}`);
    for (const x of h.findings) console.log(`[${x.level}] ${x.sensor}: ${x.title}${x.detail ? " - " + x.detail : ""}`);
    return;
  }

  if (cmd === "run") {
    if (args.ref) setReference(ctx, Number(args.ref));
    const specs = (args.algo.length ? args.algo : ["fusion", "baro_kf"]).map((t) => specFor(algos, t));
    const runs = [loggedRun(ctx), ...runAlgorithms(ctx, specs)];
    const rows = runs.map((r) => computeMetrics(ctx, r, range(args)));
    rows.forEach((m) => { m.score = score(m); });
    if (args.csv) {
      const head = ["t_s", "ref", "gnss", ...runs.map((r) => r.label)];
      const lines = [head.join(",")];
      for (let i = 0; i < ctx.tS.length; i++) {
        lines.push([ctx.tS[i].toFixed(3), f(ctx.ref[i]), f(ctx.gnssClimb[i]), ...runs.map((r) => f(r.vz[i]))].join(","));
      }
      writeFileSync(args.csv, lines.join("\n"));
    }
    if (args.json) console.log(JSON.stringify({ log: path, range: range(args), metrics: rows }, null, 2));
    else printMetrics(rows);
    return;
  }

  if (cmd === "sweep") {
    if (!args.algo.length || !args.grid.length) throw new Error("sweep needs --algo and at least one --grid");
    const base = specFor(algos, args.algo[0]);
    const grid = args.grid.map((g) => {
      const [k, v] = g.split("=");
      if (!(k in base.params)) throw new Error(`unknown parameter ${k}`);
      return { k, values: v.split(",").map(Number) };
    });
    let combos = [{}];
    for (const g of grid) combos = combos.flatMap((c) => g.values.map((v) => ({ ...c, [g.k]: v })));
    // all combinations in one replay pass (the IMU/baro loop is shared)
    const specs = combos.map((c) => ({
      algo: base.algo, params: { ...base.params, ...c },
      label: Object.entries(c).map(([k, v]) => `${k}=${v}`).join(","),
    }));
    const runs = runAlgorithms(ctx, specs);
    const rows = runs.map((r, i) => {
      const m = computeMetrics(ctx, r, range(args));
      m.score = score(m);
      m.params = combos[i];
      return m;
    });
    const key = args.sort || "score";
    rows.sort((a, b) => (a[key] ?? Infinity) - (b[key] ?? Infinity));
    const top = rows.slice(0, Number(args.top) || rows.length);
    if (args.json) console.log(JSON.stringify({ log: path, algo: base.algo.id, range: range(args), results: top }, null, 2));
    else printMetrics(top);
    return;
  }
  throw new Error(`unknown command ${cmd}`);
}

main().catch((e) => {
  console.error(e.message);
  process.exit(1);
});
