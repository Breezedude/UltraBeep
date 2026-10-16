"use strict";

/*
 * The Voice view: the device's voice pack (the same key as System → Language and time) and the
 * packs others have shared for it (the relay's /v1/packs; their files live on the voice server at
 * home). A pack can be heard (its sample sentences, made with its voice and settings), downloaded,
 * written to the SD card when that is open as a folder, or opened in Custom voicepack as the start
 * of one's own. Custom speaker (js/speaker.js) makes voices from recordings, Custom voicepack
 * (js/voicegen.js) makes and shares whole packs.
 */

const STOCK_PACKS_URL = "https://stodeus.com/de/ultrabip-voicepacks";
const PACK_DRAFT = "cfg.packDraft";

/* The pack being made in Custom voicepack, per voice pack id: the shared pack it started from, its
 * changed texts {clip name: spoken text} and the lines spoken in another language than the pack's
 * {clip name: language} ("gps_ok": "en"). */
function packDraft(id) {
  try {
    const d = JSON.parse(localStorage.getItem(PACK_DRAFT) || "{}");
    if (d.id === id) return { base: d.base || null, baseName: d.baseName || "", overrides: d.overrides || {}, languages: d.languages || {} };
  } catch (e) { /* none */ }
  return { base: null, baseName: "", overrides: {}, languages: {} };
}

function savePackDraft(id, draft) {
  try { localStorage.setItem(PACK_DRAFT, JSON.stringify({ id, ...draft })); } catch (e) { /* page only */ }
}

/* Writes a pack to voices/<id>.ubpack on the SD card; the pack there before is kept once as <id>_orig.ubpack. */
async function writePackToCard(dir, id, bytes) {
  const voices = await subdir(dir, "voices", true);
  if (!voices) throw new Error("No voices folder on the SD card.");
  const name = `${id}.ubpack`, backup = `${id}_orig.ubpack`;
  let before = null;
  try { before = await (await voices.getFileHandle(name)).getFile(); } catch (e) { /* none yet */ }
  if (before && !(await voices.getFileHandle(backup).then(() => true, () => false))) {
    const w = await (await voices.getFileHandle(backup, { create: true })).createWritable();
    await w.write(before);
    await w.close();
  }
  const w = await (await voices.getFileHandle(name, { create: true })).createWritable();
  await w.write(bytes);
  await w.close();
  return !!before;
}

/* a voice no service here speaks: the pack was made with another tool ("google:de-DE-Chirp3-HD") */
const externalVoice = (voice) => /^[a-z]+:/.test(voice || "") && !homeVoice(voice);
const providerLabel = (voice) => (homeVoice(voice) ? "Breezedude, Chatterbox" : /^google:/.test(voice || "") ? "Google Cloud TTS"
  : externalVoice(voice) ? "made elsewhere" : "Microsoft Edge");
const mb = (bytes) => `${(bytes / 1024 / 1024).toFixed(1)} MB`;

function renderVoice() {
  leaveView();
  const main = $("main");
  const packId = systemValue("system_language_string") || "de";
  const pack = voicepackInfo(packId);
  const packName = pack.label.replace(/ \(.*\)$/, "");
  const textLang = pack.lang || "en";
  const packField = SYSTEM_GROUPS.flatMap((g) => g.fields).find((f) => f.key === "system_language_string");
  const packSel = input(packField, packId, (v) => { setSystemValue("system_language_string", v); renderVoice(); });
  const status = el("p", { class: "sub voice-status" }, "Loading the shared packs …");
  const body = el("tbody", {});
  const src = state.source;
  const onCard = src && src.kind === "dir";
  let busy = false;

  /* runs one action at a time; its button shows that it works */
  const act = (btn, what, fn) => async () => {
    if (busy) return;
    busy = true;
    const label = btn.textContent;
    btn.disabled = true;
    try {
      await fn((text) => { status.textContent = `${what}: ${text}`; });
    } catch (e) {
      status.textContent = `${what}: ${e.message}`;
    }
    btn.disabled = false;
    btn.textContent = label;
    busy = false;
  };

  /* the pack's own clips of the sample sentences, as the device will play them */
  const sample = (p) => async (say) => {
    say("loading …");
    const [full, clips] = await Promise.all([fetchPack(p.id), fetchPackClips(p.id, voicepackSample())]);
    const names = voicepackSample().filter((n) => clips[n]);
    if (!names.length) throw new Error("This pack has none of the sample sentences.");
    say(`"${names.map((n) => full.overrides[n] || voicepackText(textLang, n)).join(" · ")}"`);
    playClips(names.map((n) => clips[n]));
  };
  const fetchFile = async (p, say) => {
    const bytes = await downloadPack(p.id, (got, total) => say(`${mb(got)} of ${mb(total || p.size)} …`));
    if (bytes.length !== p.size) throw new Error("The download broke off; try again.");
    return bytes;
  };
  const download = (p) => async (say) => {
    downloadBytes(`${packId}.ubpack`, await fetchFile(p, say));
    say(`saved as ${packId}.ubpack. Copy it to voices/ on the SD card (over USB): the device loads it as its ${packName} pack.`);
  };
  const toCard = (p) => async (say) => {
    const bytes = await fetchFile(p, say);
    say("writing to the SD card …");
    const kept = await writePackToCard(src.dir, packId, bytes);
    say(`written to voices/${packId}.ubpack${kept ? ` (the pack before is kept as voices/${packId}_orig.ubpack)` : ""}. ` +
      "The device loads it at the next start.");
  };
  const asStart = (p) => async (say) => {
    say("loading …");
    const full = await fetchPack(p.id);
    savePackDraft(packId, { base: p.id, baseName: p.name, overrides: full.overrides, languages: full.languages || {} });
    const s = ttsSettings();
    Object.assign(s, full.settings);
    /* a pack made elsewhere ("google:...") brings its texts and sound, not its voice */
    if (!externalVoice(full.voice)) Object.assign(s, { voice: full.voice, locale: "" });
    saveTtsSettings(s);
    state.view = "voicepack";
    render();
  };

  const stockRow = el("tr", {},
    el("td", {}, el("b", {}, "Original"), el("small", { class: "unit" }, " Stodeus")),
    el("td", {}, "the manufacturer's voice"),
    el("td", {}, "on the device"),
    el("td", { class: "actions" }, el("a", { href: STOCK_PACKS_URL, target: "_blank", rel: "noopener" }, "stodeus.com")));
  const packRow = (p) => {
    const b = (label, title, fn) => {
      const btn = el("button", { type: "button", class: "play", title }, label);
      btn.addEventListener("click", act(btn, `"${p.name}"`, fn));
      return btn;
    };
    return el("tr", {},
      el("td", {}, el("b", {}, p.name),
        p.changed_texts ? el("small", { class: "unit" }, ` ${p.changed_texts} text${p.changed_texts > 1 ? "s" : ""} changed`) : null),
      el("td", {}, `${p.voice_label} `, el("small", { class: "unit" }, `(${providerLabel(p.voice)})`)),
      el("td", {}, `${p.created.slice(0, 10)} · ${p.clips} clips · ${mb(p.size)}`, p.downloads ? ` · ${p.downloads}×` : ""),
      el("td", { class: "actions" },
        b("▶", "Hear its sample sentences", sample(p)),
        b("⬇", `Download as ${packId}.ubpack`, download(p)),
        onCard ? b("SD", `Write it to the SD card as voices/${packId}.ubpack`, toCard(p)) : null,
        b("✎", externalVoice(p.voice) ? "Start your own pack from this one's texts and sound (its voice is not offered here)"
          : "Start your own pack from this one (Custom voicepack)", asStart(p))));
  };
  fetchPacks(packId).then((list) => {
    body.replaceChildren(stockRow, ...list.map(packRow));
    status.textContent = list.length ? `${list.length} shared ${packName} pack${list.length > 1 ? "s" : ""}.`
      : `No shared ${packName} packs yet: make one in Custom voicepack and share it.`;
  }).catch((e) => {
    body.replaceChildren(stockRow);
    status.textContent = e.message;
  });

  const packsCard = fieldset({ title: "Voice packs", help: "The device speaks with the voice pack of its language " +
      "(voices/<id>.ubpack on the SD card). Besides the manufacturer's, people share packs they made with Custom " +
      "voicepack: ▶ plays its sample sentences, ⬇ downloads it, SD writes it to the SD card when you opened the card as a " +
      "folder, ✎ opens it in Custom voicepack as the start of your own (its voice, settings and changed texts)." },
  customRow("Voice pack", packSel, packField.help),
  el("div", { class: "scroll-x" }, el("table", { class: "cliplist packlist" },
    el("thead", {}, el("tr", {}, el("th", {}, "Pack"), el("th", {}, "Voice"), el("th", {}, "Made"), el("th", {}, ""))), body)),
  status,
  el("p", { class: "sub" }, "Shared packs are published by their makers, unchecked. To have one removed, write to ",
    el("a", { href: "mailto:info@breezedude.de" }, "info@breezedude.de"), "."));
  const makeCard = fieldset({ title: "Make your own" },
    el("div", { class: "buttons" },
      el("button", { type: "button", onclick: () => { state.view = "speaker"; render(); } }, "Custom speaker"),
      el("button", { type: "button", onclick: () => { state.view = "voicepack"; render(); } }, "Custom voicepack")),
    el("p", { class: "sub" }, "Custom speaker: your voice from a recording, tried out and offered to everyone as a template. " +
      "Custom voicepack: a whole pack or single clips in any voice, sent to the device or shared here."));
  main.replaceChildren(
    el("div", { class: "head" }, el("h2", {}, "Voice"), el("p", { class: "sub" }, `The voice pack of the device (${packName}) and the packs shared for it.`)),
    packsCard, makeCard);
  state.cleanup = () => stopPlaying();
}
