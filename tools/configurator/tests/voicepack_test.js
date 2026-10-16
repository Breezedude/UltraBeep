"use strict";

/*
 * Self-test of js/voicepack.js under Node: every language has the same clips as the German pack
 * the notebook defined, no text is empty, the manufacturer's pack list is consistent, and the
 * pack lookup behind system_language_string works for known and own names.
 *   node tools/configurator/tests/voicepack_test.js
 */
const path = require("node:path");
const { voicepackClips, voicepackText, voicepackInfo, voicepackExtension, VOICEPACK_LANGUAGES, VOICEPACKS, VOICEPACK_EXTENSION } = require(path.join(__dirname, "..", "js", "voicepack.js"));

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

check(voicepackText("de", "vario_sink_150") === "minus 1,5 Meter pro Sekunde" && voicepackText("en", "vario_lift_150") === "1.5 metres per second" &&
  voicepackText("fr", "vario_lift_150") === "1,5 mètres par seconde" && voicepackText("it", "sp/sp_1") === "1 chilometro all'ora" &&
  voicepackText("us", "al/al_1230") === "1230 meters" && voicepackText("br", "sp/sp_42") === "42 quilômetros por hora",
"numbers and units: decimal marks and singulars");
check(voicepackText("pt", "takeoff") === "Descolagem" && voicepackText("br", "takeoff") === "Decolagem" && voicepackText("br", "gps_ok") === "GPS ok",
  "the Brazilian variant overrides a few words and keeps the rest");

const series = de.filter((c) => c.name.startsWith("g/"));
const bank = de.filter((c) => c.name.startsWith("bank/"));
check(VOICEPACK_EXTENSION.size === 7 && de.filter((c) => !c.stock).length === 7 + 101 + 19 &&
  de.filter((c) => c.stock).length === de.length - 127 && [...series, ...bank].every((c) => !c.stock) &&
  voicepackExtension("g/g_23") && voicepackExtension("bank/bank_60") &&
  !voicepackExtension("al/al_1230") && !voicepackExtension("gps_ok") && voicepackExtension("bank_angle") &&
  Object.keys(VOICEPACK_LANGUAGES).every((l) => [...VOICEPACK_EXTENSION].every((n) => voicepackText(l, n))),
"the 7 extension words, the 101 load factor and the 19 bank angle clips are flagged and have texts in every language");
check(bank.length === 19 && bank[0].name === "bank/bank_0" && bank[18].name === "bank/bank_90" &&
  voicepackText("de", "bank/bank_60") === "60 Grad" && voicepackText("us", "bank/bank_45") === "45 degrees" &&
  voicepackText("fr", "bank/bank_0") === "0 degré" && voicepackText("fr", "bank/bank_5") === "5 degrés" &&
  voicepackText("br", "bank/bank_30") === "30 graus" && voicepackText("nl", "bank/bank_85") === "85 graden",
"the bank angle series: 0 to 90 degrees in steps of 5");
check(series.length === 101 && series[0].name === "g/g_0" && series[100].name === "g/g_100" &&
  voicepackText("de", "g/g_23") === "2,3 G" && voicepackText("de", "g/g_10") === "1 G" && voicepackText("en", "g/g_23") === "2.3 G" &&
  voicepackText("us", "g/g_100") === "10 G" && voicepackText("fr", "g/g_5") === "0,5 G",
"the load factor series: 0.0 to 10.0 g in tenths, with the language's decimal mark");

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

console.log(failed ? `${failed} failed` : "all passed");
process.exit(failed ? 1 : 0);
