"use strict";

/*
 * Self-test of js/voicepack.js under Node: every language has the same clips as the German pack
 * the notebook defined, no text is empty, the manufacturer's pack list is consistent, and the
 * pack lookup behind system_language_string works for known and own names.
 *   node tools/configurator/tests/voicepack_test.js
 */
const path = require("node:path");
const { voicepackClips, voicepackText, voicepackInfo, voicepackExtension, voicepackGlideSteps, VOICEPACK_LANGUAGES, VOICEPACKS,
  VOICEPACK_EXTENSION } = require(path.join(__dirname, "..", "js", "voicepack.js"));

let failed = 0;
const check = (ok, what) => {
  console.log(`${ok ? "ok  " : "FAIL"} ${what}`);
  if (!ok) failed++;
};

const de = voicepackClips("de");
check(de.length > 900, `German pack: ${de.length} clips`);
const names = de.map((c) => c.name);
for (const [lang, L] of Object.entries(VOICEPACK_LANGUAGES)) {
  const clips = voicepackClips(lang);
  const same = clips.length === de.length && clips.every((c, i) => c.name === names[i]);
  check(same, `${lang} (${L.label}): the same ${clips.length} clips as the German pack`);
  const empty = clips.filter((c) => !c.text || !c.text.trim());
  check(empty.length === 0, `${lang}: no empty text${empty.length ? ` (${empty.slice(0, 3).map((c) => c.name).join(", ")})` : ""}`);
  const wordKeys = Object.keys(L.words).sort().join(",");
  check(wordKeys === Object.keys(VOICEPACK_LANGUAGES.de.words).sort().join(","), `${lang}: the same words as German`);
}

check(voicepackText("de", "vario_sink_150") === "minus 1.5 Meter pro Sekunde" && voicepackText("en", "vario_lift_150") === "1.5 metres per second" &&
  voicepackText("fr", "vario_lift_150") === "1.5 mètres par seconde" && voicepackText("it", "sp/sp_1") === "1 chilometro all'ora" &&
  voicepackText("us", "al/al_1230") === "1230 meters" && voicepackText("br", "sp/sp_42") === "42 quilômetros por hora",
"numbers and units: decimal points and singulars");
check(Object.keys(VOICEPACK_LANGUAGES).every((l) => voicepackClips(l).every((c) => !/\d,\d/.test(c.text))),
  "no decimal comma in any language (the voice server and Edge read the point as the language's decimal mark)");
check(voicepackText("pt", "takeoff") === "Descolagem" && voicepackText("br", "takeoff") === "Decolagem" && voicepackText("br", "gps_ok") === "GPS ok",
  "the Brazilian variant overrides a few words and keeps the rest");

const series = de.filter((c) => c.name.startsWith("g/"));
const bank = de.filter((c) => c.name.startsWith("bank/"));
const glide = de.filter((c) => c.name.startsWith("glide/"));
const from = de.filter((c) => c.name.startsWith("nav/from_"));
const sink = de.filter((c) => /^vario_sink_\d+$/.test(c.name));
check(VOICEPACK_EXTENSION.size === 11 && de.filter((c) => !c.stock).length === 11 + 101 + 37 + 117 + 8 &&
  de.filter((c) => c.stock).length === de.length - 274 && [...series, ...bank, ...glide, ...from].every((c) => !c.stock) &&
  voicepackExtension("g/g_23") && voicepackExtension("bank/bank_60") && voicepackExtension("glide/glide_87") && voicepackExtension("nav/from_w") &&
  !voicepackExtension("al/al_1230") && !voicepackExtension("gps_ok") && !voicepackExtension("nav/w") && !voicepackExtension("nav/wind") &&
  voicepackExtension("bank_angle") && voicepackExtension("wind") && voicepackExtension("height_gain") &&
  Object.keys(VOICEPACK_LANGUAGES).every((l) => [...VOICEPACK_EXTENSION].every((n) => voicepackText(l, n))),
"the 11 extension words, the 101 load factor, 37 bank angle, 117 glide ratio and 8 wind direction clips are flagged and have texts in every language");
check(glide.length === 117 && voicepackGlideSteps().length === 117 && glide[0].name === "glide/glide_0" && glide[116].name === "glide/glide_300" &&
  voicepackText("de", "glide/glide_87") === "8.7" && voicepackText("de", "glide/glide_50") === "5" && voicepackText("fr", "glide/glide_135") === "13.5" &&
  voicepackText("en", "glide/glide_300") === "30" && !voicepackText("en", "glide/glide_132") && !voicepackText("en", "glide/glide_225") &&
  voicepackText("en", "glide/glide_51") === "5.1" && voicepackText("en", "glide/glide_130") === "13" && voicepackText("en", "glide/glide_45") === "4.5",
"the glide ratio series: halves to 5, tenths to 13, halves to 22, whole to 30, as plain numbers with a decimal point");
check(from.length === 8 && voicepackText("de", "nav/from_w") === "aus Westen" && voicepackText("en", "nav/from_ne") === "from the north-east" &&
  voicepackText("fr", "nav/from_e") === "de l'est" && voicepackText("pt", "nav/from_e") === "de este" && voicepackText("br", "nav/from_e") === "de leste" &&
  voicepackText("br", "nav/from_n") === "de norte" && voicepackText("nl", "nav/from_sw") === "uit het zuidwesten" &&
  voicepackText("de", "wind") === "Wind:" && voicepackText("it", "climb") === "Salita:" && voicepackText("es", "height_gain") === "Ganancia de altura:",
"the wind direction clips and the words before a number in every language");
check(sink.length === 54 && sink[sink.length - 1].name === "vario_sink_2500" && sink.every((c) => c.stock) &&
  de.filter((c) => c.stock).length === 993 && voicepackText("de", "vario_sink_1200") === "minus 12 Meter pro Sekunde" &&
  voicepackText("en", "vario_sink_2450") === "minus 24.5 metres per second" && voicepackText("de", "sp/sp_80") === "80 Kilometer pro Stunde",
"the pack's clips: the sinks -0.1 to -25 m/s in halves and 80 km/h with the others (972 + 20 + 1)");
check(bank.length === 37 && bank[0].name === "bank/bank_0" && bank[18].name === "bank/bank_90" && bank[36].name === "bank/bank_180" &&
  voicepackText("de", "bank/bank_60") === "60 Grad" && voicepackText("us", "bank/bank_45") === "45 degrees" &&
  voicepackText("fr", "bank/bank_0") === "0 degré" && voicepackText("fr", "bank/bank_5") === "5 degrés" &&
  voicepackText("br", "bank/bank_30") === "30 graus" && voicepackText("nl", "bank/bank_85") === "85 graden" &&
  voicepackText("de", "bank/bank_135") === "135 Grad" && voicepackText("it", "bank/bank_180") === "180 gradi",
"the bank angle series: 0 to 180 degrees in steps of 5");
check(series.length === 101 && series[0].name === "g/g_0" && series[100].name === "g/g_100" &&
  voicepackText("de", "g/g_23") === "2.3 G" && voicepackText("de", "g/g_10") === "1 G" && voicepackText("en", "g/g_23") === "2.3 G" &&
  voicepackText("us", "g/g_100") === "10 G" && voicepackText("fr", "g/g_5") === "0.5 G",
"the load factor series: 0.0 to 10.0 g in tenths, with a decimal point");

const ids = VOICEPACKS.map((p) => p.id);
check(new Set(ids).size === VOICEPACKS.length && VOICEPACKS.length === 24, `${VOICEPACKS.length} manufacturer packs, ids unique`);
check(VOICEPACKS.every((p) => p.lang === null || VOICEPACK_LANGUAGES[p.lang]) && VOICEPACKS.every((p) => /^[a-z]{2}-[A-Z]{2}$/.test(p.tts)),
  "every pack names existing texts (or none) and a voice locale");
const de2 = voicepackInfo("de"), us = voicepackInfo("us"), pl = voicepackInfo("pl"), own = voicepackInfo("frenchvoice"), odd = voicepackInfo("xx");
check(de2.lang === "de" && de2.language === "de" && de2.tts === "de-DE", "lookup: de");
check(us.lang === "us" && us.language === "en" && us.tts === "en-US", "lookup: us (English texts, US voices)");
check(pl.lang === null && pl.language === "pl" && pl.label === "Polish", "lookup: pl (a pack without texts)");
check(own.lang === "fr" && own.language === "fr" && own.tts === "fr-FR", "lookup: an own pack named after its language");
check(odd.lang === null && odd.language === "xx", "lookup: an unknown name");
check(voicepackInfo("").id === "de" && voicepackInfo(" DE ").id === "de", "lookup: empty and padded values");

/* the work behind a device's pack: the draft taken from it, the state of a clip against it, its entries */
const { voicepackWorkKeys, voicepackWorkDraft, voicepackWorkState, voicepackWorkEntry } = require(path.join(__dirname, "..", "js", "voicepack.js"));
const devWork = { built: "2026-10-18T12:00:00.000Z", clips: {
  takeoff: { t: "Abflug" }, profile_hike: { t: "Haik änd Flai" }, gps_ok: { t: "GPS ok", l: "en" }, no_gps: { t: "Suche GPS", v: 3 },
  "glide/glide_87": { t: "8.7", f: 1 }, gone_clip: { t: "weg" } } };
const wd = voicepackWorkDraft(devWork, "de");
check(JSON.stringify(wd.overrides) === JSON.stringify({ profile_hike: "Haik änd Flai" }) && wd.languages.gps_ok === "en" &&
  wd.variants.no_gps === 3 && !("gone_clip" in wd.overrides) && Object.keys(wd.languages).length === 1,
"work draft: only the texts that differ from the catalogue, the languages and takes; clips the catalogue lacks left out");
check(voicepackWorkState(devWork, "takeoff", "Abflug") === "pack" && voicepackWorkState(devWork, "glide/glide_87", "8.7") === "file" &&
  voicepackWorkState(devWork, "takeoff", "Start") === "changed" && voicepackWorkState(devWork, "gps_ok", "GPS ok") === "changed" &&
  voicepackWorkState(devWork, "gps_ok", "GPS ok", { language: "en" }) === "pack" &&
  voicepackWorkState(devWork, "no_gps", "Suche GPS", { variant: 3 }) === "pack" && voicepackWorkState(devWork, "no_gps", "Suche GPS") === "changed" &&
  voicepackWorkState(devWork, "landing", "Landung") === "new" && voicepackWorkState({ ...devWork, built: null }, "landing", "Landung") === "" &&
  voicepackWorkState(null, "takeoff", "Abflug") === "",
"clip states against the work: in pack, single file, changed (text, language, take), new in the catalogue, nothing without a pack file");
check(JSON.stringify(voicepackWorkEntry("GPS ok", { language: "en" })) === '{"t":"GPS ok","l":"en"}' &&
  JSON.stringify(voicepackWorkEntry("8.7", {}, true)) === '{"t":"8.7","f":1}' &&
  JSON.stringify(voicepackWorkEntry("Suche GPS", { variant: 2 })) === '{"t":"Suche GPS","v":2}',
"work entries: language, take and the single-file mark only when there");
check(voicepackWorkKeys("de").id === "voice_generator_work_de_string" && voicepackWorkKeys("us").key === "voice_generator_workkey_us_string",
  "the work's id and owner key in config_ext.ultrabip, per voice pack");

/* the work file of Custom voicepack: saved and loaded again; unknown clips and bad values are left out */
const { voicepackWorkFile, voicepackWorkFromFile } = require(path.join(__dirname, "..", "js", "voicepack.js"));
const work = voicepackWorkFile("de", { base: "08cad7bea59ed4e777c6", baseName: "Google Chirp3-HD",
  overrides: { no_gps: "Suche Dschi Pi Es" }, languages: { gps_ok: "en" }, variants: { no_gps: 3 } },
{ voice: "cb:de-DE-tts-2023-version1", pitch: 4, rate: -2, bandpass: true, lo: 300, hi: 8000 });
const back = voicepackWorkFromFile(JSON.stringify(work));
check(back.pack === "de" && back.draft.overrides.no_gps === "Suche Dschi Pi Es" && back.draft.languages.gps_ok === "en" &&
  back.draft.variants.no_gps === 3 && back.draft.base === "08cad7bea59ed4e777c6" && back.voice === "cb:de-DE-tts-2023-version1" &&
  back.audio.pitch === 4 && back.audio.rate === -2 && back.dropped === 0, "work file: saved and loaded again");
const messy = voicepackWorkFromFile(JSON.stringify({ ...work, overrides: { no_gps: "  Suche   GPS. ", nonsense: "x", takeoff: "" },
  languages: { gps_ok: "English" }, variants: { no_gps: 100, takeoff: 2 }, audio: { pitch: 99 } }));
check(messy.draft.overrides.no_gps === "Suche GPS." && Object.keys(messy.draft.overrides).length === 1 &&
  !Object.keys(messy.draft.languages).length && messy.draft.variants.takeoff === 2 && !messy.draft.variants.no_gps &&
  messy.dropped === 4 && messy.audio.pitch === 50 && messy.audio.hi === 8000, "work file: unknown clips and bad values left out");
let refused = 0;
for (const t of ["not json", JSON.stringify({ format: "other" }), JSON.stringify({ ...work, pack: "../x" })]) {
  try { voicepackWorkFromFile(t); } catch (e) { refused++; }
}
check(refused === 3, "work file: other files refused");

console.log(failed ? `${failed} failed` : "all passed");
process.exit(failed ? 1 : 0);
