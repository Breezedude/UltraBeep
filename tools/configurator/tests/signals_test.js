"use strict";

/*
 * The signal tones of the announcement block "tone": the configurator's SIGNAL_TONES (schema.js) against the
 * firmware's names (announce.c announce_signal_names) and notes (sound.c sig_<name>), so the preview in the
 * browser plays what the device plays and the names written are the ones the device knows.
 *   node tools/configurator/tests/signals_test.js
 */
const fs = require("node:fs");
const path = require("node:path");
global.VOICEPACKS = require(path.join(__dirname, "..", "js", "voicepack.js")).VOICEPACKS;
const { SIGNAL_TONES } = require(path.join(__dirname, "..", "js", "schema.js"));

let failed = 0;
const check = (ok, what) => {
  console.log(`${ok ? "ok  " : "FAIL"} ${what}`);
  if (!ok) failed++;
};

const fw = path.join(__dirname, "..", "..", "..", "firmware", "src", "app");
const announce = fs.readFileSync(path.join(fw, "announce.c"), "utf8");
const sound = fs.readFileSync(path.join(fw, "sound.c"), "utf8");

const table = announce.match(/announce_signal_names\[ANN_SIGNALS\]\s*=\s*\{([^}]*)\}/);
const names = table ? [...table[1].matchAll(/"([a-z_]+)"/g)].map((m) => m[1]) : [];
check(names.length === SIGNAL_TONES.length && names.every((n, i) => SIGNAL_TONES[i].id === n),
  `the same names in the same order (the first is the default): ${names.join(" ")}`);

for (const s of SIGNAL_TONES) {
  const m = sound.match(new RegExp(`sig_${s.id}\\[\\]\\s*=\\s*\\{([\\s\\S]*?)\\};`));
  const notes = m ? [...m[1].matchAll(/\{(\d+),\s*(\d+),\s*\d+\}/g)].map((x) => [+x[1], +x[2]]).filter(([, ms]) => ms > 0) : null;
  check(notes && JSON.stringify(notes) === JSON.stringify(s.notes), `${s.id}: the notes of sound.c`);
}

console.log(failed ? `${failed} FAILED` : "all passed");
process.exitCode = failed ? 1 : 0;
