"use strict";

/*
 * The announcement blocks of the configurator (schema.js BLOCKS, BLOCK_OPTIONS, SIGNAL_TONES, TRIGGERS) against the
 * firmware (announce.c, sound.c, summary.c), so the names written into announce_<trigger>_items_string are the ones
 * the device knows, the option words and spans are its, the signal tone preview in the browser plays what the
 * device plays, and the glide ratio clips of the catalogue (voicepack.js) are the ones summary_glide_clip asks for.
 *   node tools/configurator/tests/announce_test.js
 */
const fs = require("node:fs");
const path = require("node:path");
const vp = require(path.join(__dirname, "..", "js", "voicepack.js"));
global.VOICEPACKS = vp.VOICEPACKS;
const { SIGNAL_TONES, BLOCKS, BLOCK_OPTIONS, TRIGGERS } = require(path.join(__dirname, "..", "js", "schema.js"));

let failed = 0;
const check = (ok, what) => {
  console.log(`${ok ? "ok  " : "FAIL"} ${what}`);
  if (!ok) failed++;
};

const fw = path.join(__dirname, "..", "..", "..", "firmware", "src", "app");
const announce = fs.readFileSync(path.join(fw, "announce.c"), "utf8");
const sound = fs.readFileSync(path.join(fw, "sound.c"), "utf8");
const summary = fs.readFileSync(path.join(fw, "summary.c"), "utf8");
const table = (name) => {
  const m = announce.match(new RegExp(`${name}\\[[A-Z_]+\\]\\s*=\\s*\\{([^}]*)\\}`));
  return m ? [...m[1].matchAll(/"([a-z_]*)"/g)].map((x) => x[1]) : null;
};

/* block names: the same set (the order is the configurator's own) */
const blocks = table("announce_block_names");
const ids = BLOCKS.map((b) => b.id);
check(blocks && blocks.length === ids.length && blocks.every((b) => ids.includes(b)) && ids.every((b) => blocks.includes(b)),
  `the same ${ids.length} block names: ${(blocks || []).join(" ")}`);
check(TRIGGERS.every((t) => t.pool.every((id) => ids.includes(id)) && (t.legacy.items || []).every((id) => ids.includes(id))),
  "every trigger's pool and default list use known blocks");
check(TRIGGERS.map((t) => t.id).join(" ") === (table("announce_trigger_names") || []).join(" "),
  "the same trigger names in the same order");

/* signal tones: names in the same order (the first is the default), the notes of sound.c */
const names = table("announce_signal_names");
check(names && names.length === SIGNAL_TONES.length && names.every((n, i) => SIGNAL_TONES[i].id === n),
  `the same signal names in the same order (the first is the default): ${(names || []).join(" ")}`);
for (const s of SIGNAL_TONES) {
  const m = sound.match(new RegExp(`sig_${s.id}\\[\\]\\s*=\\s*\\{([\\s\\S]*?)\\};`));
  const notes = m ? [...m[1].matchAll(/\{(\d+),\s*(\d+),\s*\d+\}/g)].map((x) => [+x[1], +x[2]]).filter(([, ms]) => ms > 0) : null;
  check(notes && JSON.stringify(notes) === JSON.stringify(s.notes), `${s.id}: the notes of sound.c`);
}
const tone = BLOCK_OPTIONS.tone[0];
check(tone.key === "sound" && tone.play && tone.def === SIGNAL_TONES[0].id && tone.choices.map(([v]) => v).join(" ") === SIGNAL_TONES.map((s) => s.id).join(" "),
  "the tone's option offers the signals, the first as the default");

/* the option words: spans "<n>s" from announce_span_s, words from announce_word_names; defaults from the firmware */
const spanTable = announce.match(/announce_span_s\[ANN_SPANS\]\s*=\s*\{([^}]*)\}/);
const spans = spanTable ? spanTable[1].split(",").map((x) => +x.trim()).filter((x) => x > 0).map((x) => `${x}s`) : [];
const words = table("announce_word_names") || [];
check(spans.length === 5 && words.length === 4 && words[0] === "", `the firmware's spans ${spans.join(" ")} and words ${words.slice(1).join(" ")}`);
for (const [id, options] of Object.entries(BLOCK_OPTIONS)) {
  check(ids.includes(id), `${id}: a known block`);
  for (const o of options) {
    if (o.key === "span") {
      check(o.choices.every(([v]) => v === "" || spans.includes(v)) && (o.def === "" || spans.includes(o.def)),
        `${id}: the spans ${o.choices.map(([v]) => v || "(default)").join(" ")} exist in the firmware`);
    } else if (o.key === "word") {
      check(o.choices.every(([v]) => words.includes(v)) && o.def === "" && o.choices[0][0] === "",
        `${id}: the words ${o.choices.map(([v]) => v || "(none)").join(" ")} exist in the firmware`);
    }
    const values = o.choices.map(([v]) => v);
    check(new Set(values).size === values.length && values.includes(o.def), `${id}.${o.key}: distinct values, the default among them`);
  }
  const all = options.flatMap((o) => o.choices.map(([v]) => v)).filter(Boolean);
  check(new Set(all).size === all.length, `${id}: no value belongs to two options`);
}
const defaults = { climbrate: "SPAN_CLIMB_DEFAULT 1", glide: "SPAN_GLIDE_DEFAULT 3" };
for (const [id, def] of Object.entries(defaults)) {
  const idx = +def.split(" ")[1];
  const span = BLOCK_OPTIONS[id].find((o) => o.key === "span");
  check(announce.includes(`#define ${def}`) && span.def === spans[idx - 1], `${id}: the default span ${span.def} is the firmware's`);
}
check(BLOCK_OPTIONS.thermalgain.find((o) => o.key === "span").def === "" && announce.includes("in->thermal_gain_m"),
  "thermalgain: the default span is the climb phase");
/* the words the firmware speaks for them are in the catalogue in every language */
const clips = announce.match(/word_clips\[ANN_WORDS\]\s*=\s*\{([^}]*)\}/);
const wordClips = clips ? [...clips[1].matchAll(/"([a-z_]+)"/g)].map((x) => x[1]) : [];
check(wordClips.length === 3 && Object.keys(vp.VOICEPACK_LANGUAGES).every((l) => wordClips.every((c) => vp.voicepackText(l, c))),
  `the word clips ${wordClips.join(" ")} have texts in every language`);
check(["wind", "nav/from_n", "nav/from_ne", "nav/from_e", "nav/from_se", "nav/from_s", "nav/from_sw", "nav/from_w", "nav/from_nw"]
  .every((c) => Object.keys(vp.VOICEPACK_LANGUAGES).every((l) => vp.voicepackText(l, c))) && announce.includes('"nav/from_%s"'),
"the wind words have texts in every language");

/* the glide ratio clips: summary_glide_clip's ranges make the catalogue's series */
const ranges = summary.match(/summary_glide_clip[\s\S]*?clip_range_t r\[\] = \{([^;]*)\};/);
const steps = [];
for (const m of (ranges ? ranges[1] : "").matchAll(/\{(\d+),\s*(\d+),\s*(\d+)\}/g)) {
  for (let t = +m[1]; t <= +m[2]; t += +m[3]) if (!steps.includes(t)) steps.push(t);
}
steps.sort((a, b) => a - b);
check(steps.length === 117 && JSON.stringify(steps) === JSON.stringify(vp.voicepackGlideSteps()),
  `the glide ratio clips of the catalogue are the firmware's ${steps.length} steps`);
check(announce.includes('"glide/glide_%d"') && announce.includes('"glide/glide_50"') === false,
  "the firmware asks for glide/glide_<tenths>");

console.log(failed ? `${failed} FAILED` : "all passed");
process.exitCode = failed ? 1 : 0;
