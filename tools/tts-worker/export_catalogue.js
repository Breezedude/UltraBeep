/*
 * The voice pack texts for the worker's prerendering, from the configurator's catalogue:
 *   node tools/tts-worker/export_catalogue.js > catalogue.json
 * {"packs": [{id, lang, tts}], "texts": {lang: [text, ...]}}; texts with their whitespace collapsed,
 * exactly as the relay passes them on, so the cached clips match the configurator's requests.
 */
const path = require("path");
const { voicepackClips, VOICEPACKS, VOICEPACK_LANGUAGES } = require(path.join(__dirname, "..", "configurator", "js", "voicepack.js"));

const texts = {};
for (const lang of Object.keys(VOICEPACK_LANGUAGES)) {
  texts[lang] = [...new Set(voicepackClips(lang).map((c) => c.text.replace(/\s+/g, " ").trim()))];
}
const packs = VOICEPACKS.filter((p) => p.lang).map(({ id, lang, tts }) => ({ id, lang, tts }));
process.stdout.write(`${JSON.stringify({ packs, texts }, null, 1)}\n`);
