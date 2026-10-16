"use strict";

/*
 * Self-test of the tone curve import in js/tones.js under Node: the XCTracer's default tone settings fitted into
 * the device's 8 points, a whole configuration file with other keys, JSON as before, and the errors.
 *   node tools/configurator/tests/tones_test.js
 */
const path = require("node:path");
global.VOICEPACKS = require(path.join(__dirname, "..", "js", "voicepack.js")).VOICEPACKS;
const schema = require(path.join(__dirname, "..", "js", "schema.js"));
global.TONE_POINTS = schema.TONE_POINTS;
global.STOCK_TONES = schema.STOCK_TONES;
const { varioTone, tonesFromText, curveValuesAt } = require(path.join(__dirname, "..", "js", "tones.js"));

let failed = 0;
const check = (ok, what) => {
  console.log(`${ok ? "ok  " : "FAIL"} ${what}`);
  if (!ok) failed++;
};

/* the XCTracer's defaults */
const XC = `ClimbToneOnThreshold=0.2
ClimbToneOffThreshold=0.15
SinkToneOnThreshold=-3
SinkToneOffThreshold=-3
tone=-10.0,200,100,100
tone=-3.0,280,100,100
tone=-0.51,300,500,100
tone=-0.50,200,800,5
tone=0.09,400,600,10
tone=0.10,400,600,50
tone=1.16,550,552,52
tone=2.67,763,483,55
tone=4.24,985,412,58
tone=6.00,1234,322,62
tone=8.00,1517,241,66
tone=10.00,1800,150,70`;
const r = tonesFromText(XC);
check(r.curve.lift === 20 && r.curve.sink === -300 && r.curve.alarm, `thresholds: lift ${r.curve.lift}, sink ${r.curve.sink} cm/s`);
check(r.curve.points.length === 8 && r.curve.points.every((p, i, a) => i === 0 || p.vz >= a[i - 1].vz), "8 points, sorted");
check(r.note === "12 points fitted into 8, the curve sounds the same", `note: ${r.note}`);
/* where it sounds the device plays what the XCTracer plays */
const xcPoints = XC.split("\n").filter((l) => l.startsWith("tone=")).map((l) => {
  const [vz, freq, duration, duty] = l.slice(5).split(",").map(Number);
  return { vz: Math.round(vz * 100), freq, duration, duty };
});
let worst = 0;
for (let c = -10; c <= 10; c += 0.05) {
  const t = varioTone(r.curve, c);
  const sounds = c * 100 >= 20 || c * 100 <= -300;
  if (t.sound !== sounds) { worst = Infinity; break; }
  if (!sounds) continue;
  const x = curveValuesAt(xcPoints, c * 100);
  worst = Math.max(worst, Math.abs(t.freq - x.freq) / x.freq * 100, Math.abs(t.period - x.period) / x.period * 100);
}
check(worst < 1, `pitch and period as on the XCTracer within ${worst.toFixed(2)} %`);
check(varioTone(r.curve, 1.16).freq === 550 && varioTone(r.curve, 10).freq === 1800 && varioTone(r.curve, -5).on === varioTone(r.curve, -5).period,
  "1.16 m/s 550 Hz, 10 m/s 1800 Hz, sink tone continuous");
check(!varioTone(r.curve, 0.15).sound && !varioTone(r.curve, -2.9).sound, "silent between -3 and +0.2 m/s");

/* a whole configuration file: other keys and comments are skipped */
const file = `# XC Tracer config\nuuid=1234\nClimbToneOnThreshold = 0.3\n${XC.split("\n").slice(4).join("\r\n")}\nvolume=4\n`;
const f = tonesFromText(file);
check(f.curve.lift === 30 && f.curve.sink === -250 && f.curve.points.length === 8, `whole file: lift ${f.curve.lift}, sink ${f.curve.sink} (default)`);

/* fewer points are filled up, other off thresholds are noted */
const few = tonesFromText("ClimbToneOnThreshold=0.1\nClimbToneOffThreshold=-0.2\ntone=0.1,400,600,50\ntone=5,1200,300,60");
check(few.curve.points.length === 8 && Math.abs(varioTone(few.curve, 2.5).freq - 792) <= 2 && /filled up to 8/.test(few.note) &&
  /off thresholds/.test(few.note), `two points filled up: ${few.note}`);

/* JSON still works, errors are readable */
const json = tonesFromText(JSON.stringify({ liftThreshold: 0.1, sinkThreshold: -2.5, sinkAlarm: false,
  points: STOCK_TONES.map((p) => ({ climb: p.vz / 100, frequency: p.freq, period: p.duration, tone: p.duty })) }));
check(json.curve.points.length === 8 && json.curve.alarm === false && json.note === "", "JSON as before");
const err = (text) => { try { tonesFromText(text); return ""; } catch (e) { return e.message; } };
check(/Neither/.test(err("hello")), "neither format");
check(/needs climb, frequency, cycle and duty/.test(err("tone=1,2,3")), "a short tone line");

console.log(failed ? `${failed} FAILED` : "all passed");
process.exitCode = failed ? 1 : 0;
