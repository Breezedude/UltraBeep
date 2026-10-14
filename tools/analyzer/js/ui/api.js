/*
 * window.UBA: scripting interface of the analyzer page (browser console, automated tests,
 * Claude via the browser tools). All times in seconds since the log start.
 */
import { runAlgorithms } from "../core/replay.js";
import { computeMetrics, score } from "../core/metrics.js";
import { defaultParams } from "../core/registry.js";

export function installApi(app) {
  const st = app.state;
  const runInfo = (r) => ({ id: r.id, label: r.label, algo: r.algo ? r.algo.id : "logged", params: { ...r.params }, visible: r.visible });

  window.UBA = {
    version: 1,
    /* url (with a local server) or ArrayBuffer */
    async load(src, name = "log.bin") {
      if (typeof src === "string") await app.loadUrl(src);
      else await app.loadBuffer(src, name);
      return app.summary();
    },
    summary: () => app.summary(),
    /* waits until the page is set up and a log given by ?log= is loaded; returns its summary */
    whenReady: () => app.whenReady(),
    algorithms: () => st.algos.map((a) => ({
      id: a.id, name: a.name, private: !!a.private, description: a.description,
      params: a.params.map((p) => ({ key: p.key, value: p.value, min: p.min, max: p.max, unit: p.unit, doc: p.doc })),
      diagnostics: (a.diagnostics || []).map((d) => d.key),
    })),
    runs: () => st.runs.map(runInfo),
    async addRun({ algo, params = {}, label } = {}) { return runInfo(await app.addRun(algo, params, label)); },
    async updateRun(idOrLabel, params) { return runInfo(await app.updateRun(idOrLabel, params)); },
    removeRun: (idOrLabel) => app.removeRun(idOrLabel),
    setVisible(idOrLabel, on) {
      const r = st.runs.find((x) => x.id === idOrLabel || x.label === idOrLabel);
      if (r) { r.visible = !!on; app.renderRuns(); app.refresh({ charts: true, metrics: true }); }
    },
    setRange: (t0, t1) => app.setRange(t0 == null ? null : [t0, t1]),
    range: () => st.range,
    /* metrics of the visible runs over a range ([t0, t1] or the current one) */
    metrics(range) {
      const rows = app.metrics(range === undefined ? st.range : range);
      rows.forEach((m) => { m.score = score(m); });
      return rows;
    },
    health: () => st.health,
    /*
     * Parameter sweep without touching the page's runs: {algo, base: {params}, grid: {KEY: [values]},
     * range: [t0, t1], sort: "score"}. Returns the results sorted (best first).
     */
    async sweep({ algo, base = {}, grid = {}, range = st.range, sort = "score", top } = {}) {
      const a = st.algos.find((x) => x.id === algo);
      if (!a) throw new Error(`unknown algorithm ${algo}`);
      let combos = [{}];
      for (const [k, values] of Object.entries(grid)) combos = combos.flatMap((c) => values.map((v) => ({ ...c, [k]: v })));
      const specs = combos.map((c) => ({ algo: a, params: { ...defaultParams(a), ...base, ...c }, label: JSON.stringify(c) }));
      await app.busy(true, `Sweep: ${specs.length} replays…`);
      try {
        const runs = runAlgorithms(st.ctx, specs);
        const opts = { curve: st.curve };
        if (range) { opts.t0 = range[0]; opts.t1 = range[1]; }
        const rows = runs.map((r, i) => {
          const m = computeMetrics(st.ctx, r, opts);
          m.score = score(m);
          m.params = combos[i];
          return m;
        });
        rows.sort((x, y) => (x[sort] ?? Infinity) - (y[sort] ?? Infinity));
        return top ? rows.slice(0, top) : rows;
      } finally {
        await app.busy(false);
      }
    },
    /* a series on the barometer grid as a plain array: "t", "ref", "gnss", "altitude" or a run label */
    series(name, i0 = 0, i1) {
      const ctx = st.ctx;
      const pick = name === "t" ? ctx.tS : name === "ref" ? ctx.ref : name === "gnss" ? ctx.gnssClimb
        : name === "altitude" ? ctx.altIsa : (st.runs.find((r) => r.label === name || r.id === name) || {}).result?.vz;
      if (!pick) throw new Error(`no series ${name}`);
      return Array.from(pick.subarray(i0, i1 ?? pick.length));
    },
    /* flight states (IGC tracks and raw logs): {summary, segments, events} */
    flightStates: () => (st.ctx && st.ctx.states ? { summary: st.ctx.states.summary, segments: st.ctx.states.segments, events: st.ctx.states.events } : null),
    stateAt: (t) => app.stateText(t),
    setCursor: (t) => app.setCursor(t),
    play(t, speed) {
      if (speed) { document.getElementById("speed").value = String(speed); app.player.setSpeed(speed); }
      if (t != null) app.state.cursor = t;
      if (!app.player.playing) app.togglePlay();
    },
    pause() { if (app.player.playing) app.togglePlay(); },
    setToneSource: (idOrLabel) => app.setToneRun(idOrLabel),
    showPane: (id) => app.showPane(id),
  };
}
