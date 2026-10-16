"use strict";

/*
 * The Custom speaker view: one's own voice from a recording. The recording becomes a private voice on
 * the voice server at home (only this browser knows its id); it can be tried with the catalogue's lines
 * or one's own, with Chatterbox's settings (on the server) and pitch, rate and band-pass (here, live),
 * played here or on the device; once it sounds right it can be published as a template: a voice of
 * that name in the list of everyone (js/voice.js publishVoice), with these settings as its defaults.
 */

const SPEAKER_STORAGE = "cfg.speaker";
const SPEAKER_TEST_CLIP = "speaker_test"; /* voices/custom/speaker_test.qoa for "play on the device" */
const SPEAKER_LOCALES = ["de-DE", "de-AT", "de-CH", "en-GB", "en-US", "fr-FR", "it-IT", "es-ES", "pt-PT", "pt-BR", "nl-NL",
  "pl-PL", "fi-FI", "sv-SE", "da-DK", "nb-NO", "el-GR", "tr-TR", "ru-RU", "ja-JP", "ko-KR", "zh-CN", "ar-SA", "he-IL", "hi-IN",
  "ms-MY", "sw-KE"];
const SPEAKER_PRESETS = ["profile_select", "profile_paragliding", "profile_hike", "gps_ok", "takeoff", "landing", "al/al_1230",
  "sp/sp_42", "vario_lift_150", "vario_sink_250", "thermal_exit", "bat_low", "time/duration_1", "press_button_to_confirm"];
const SPEAKER_DEFAULTS = { locale: "", exaggeration: 0.5, cfg_weight: 0.5, temperature: 0.8, pitch: 0, rate: 0, bandpass: true,
  lo: 300, hi: 8000 };

function speakerSettings() {
  try { return { ...SPEAKER_DEFAULTS, ...JSON.parse(localStorage.getItem(SPEAKER_STORAGE) || "{}") }; } catch (e) { return { ...SPEAKER_DEFAULTS }; }
}

function saveSpeakerSettings(s) {
  const keep = {};
  for (const k of Object.keys(SPEAKER_DEFAULTS)) keep[k] = s[k];
  try { localStorage.setItem(SPEAKER_STORAGE, JSON.stringify(keep)); } catch (e) { /* page only */ }
}

/* The catalogue language (js/voicepack.js) whose texts fit a locale, or null. */
function textLanguageFor(locale) {
  const exact = VOICEPACKS.find((p) => p.lang && p.tts === locale);
  const same = VOICEPACKS.find((p) => p.lang && p.tts.split("-")[0] === locale.split("-")[0]);
  return (exact || same || {}).lang || null;
}

function renderSpeaker() {
  leaveView();
  const main = $("main");
  const tts = ttsSettings();
  const sp = speakerSettings();
  const pack = voicepackInfo(systemValue("system_language_string") || "de");
  let catalog = { voices: [], own: { available: false, languages: [] } };
  let entry = null; /* the last clip: raw audio, rebuilt live with pitch, rate and band-pass */

  /* ---- 1. the recording ---- */
  const recStatus = el("p", { class: "sub voice-status" }, "");
  const consent = el("input", { type: "checkbox", id: `own-consent-${++uid}` });
  const recBtn = el("button", { type: "button" }, "● Record");
  const fileInput = el("input", { type: "file", accept: "audio/*", hidden: true });
  const openBtn = el("button", { type: "button" }, "Open a recording …");
  const delBtn = el("button", { type: "button" }, "Delete my recording voice");
  const readAloud = el("p", { class: "sub" }, "");
  const ownId = () => ownVoiceId(tts);
  const refreshOwn = () => {
    delBtn.hidden = !ownId();
    if (!recStatus.textContent) recStatus.textContent = ownId() ? "Your recording voice is on the voice server: try it below." : "";
    publishBtn.disabled = !ownId();
  };
  let left = false; /* the view was left: a recording stopped by that is not sent */
  const sendOwn = async (encoded) => {
    if (left) return;
    if (!consent.checked) { recStatus.textContent = "Tick the box first: only your own voice, or one you may use."; return; }
    recBtn.disabled = openBtn.disabled = true;
    try {
      recStatus.textContent = "Converting …";
      const wav = await recordingToWav(encoded);
      if (wav.seconds < 6) throw new Error(`The recording has ${wav.seconds.toFixed(1)} s of sound; 10 to 15 s are needed.`);
      recStatus.textContent = `Sending ${wav.seconds.toFixed(0)} s; the voice server makes the voice from it (a few seconds) …`;
      const made = await createOwnVoice(wav.bytes);
      const old = ownId();
      tts.own = made.id;
      saveTtsSettings(tts);
      if (old && old !== made.id) deleteOwnVoice(old).catch(() => { /* gone already */ });
      recStatus.textContent = `Your recording voice is ready (${made.seconds} s of speech used): try it below.`;
      entry = null;
    } catch (e) {
      recStatus.textContent = e.name === "EncodingError" ? "The browser cannot read this file as audio." : e.message;
    }
    recBtn.disabled = openBtn.disabled = false;
    refreshOwn();
  };
  let recording = null;
  recBtn.addEventListener("click", async () => {
    if (recording) { recording.stop(); return; }
    if (!consent.checked) { recStatus.textContent = "Tick the box first: only your own voice, or one you may use."; return; }
    try {
      recording = await startRecording(OWN_MAX_S);
    } catch (e) {
      recStatus.textContent = `No microphone: ${e.message}`;
      return;
    }
    const t0 = Date.now();
    const tick = setInterval(() => { recBtn.textContent = `■ Stop (${Math.round((Date.now() - t0) / 1000)} s)`; }, 250);
    recStatus.textContent = `Recording: read the text above, then stop (after ${OWN_MAX_S} s it stops by itself).`;
    recording.done.then((encoded) => {
      clearInterval(tick);
      recBtn.textContent = "● Record";
      recording = null;
      sendOwn(encoded);
    });
  });
  openBtn.addEventListener("click", () => fileInput.click());
  fileInput.addEventListener("change", async () => {
    const f = fileInput.files[0];
    fileInput.value = "";
    if (f) sendOwn(await f.arrayBuffer());
  });
  delBtn.addEventListener("click", async () => {
    if (!ownId() || !window.confirm("Delete your recording voice on the voice server? Templates made from it stay.")) return;
    try {
      const was = await deleteOwnVoice(ownId());
      tts.own = "";
      if (OWN_VOICE_RE.test(tts.voice || "")) tts.voice = "";
      saveTtsSettings(tts);
      entry = null;
      recStatus.textContent = was ? "Your recording voice is deleted." : "The voice server did not have it anymore.";
    } catch (e) {
      recStatus.textContent = e.message;
    }
    refreshOwn();
  });
  const recordCard = fieldset({ title: "1. Your recording", help: "10 to 15 s of you reading calmly, in a quiet room, in " +
      "the language the voice shall speak - every recording program works (WAV or FLAC at 44.1/48 kHz is best; a phone's " +
      "voice memo works too). It becomes a voice only you can use until you make it a template below." },
  readAloud,
  el("label", { class: "consent", for: consent.id }, consent, " This is my own voice, or the speaker has allowed me to use it."),
  el("div", { class: "buttons" }, recBtn, openBtn, fileInput, delBtn), recStatus);

  /* ---- 2. trying it ---- */
  const localeSel = el("select", { "aria-label": "Language" });
  const textInput = el("input", { type: "text", placeholder: "your own sentence, or a line from the list", "aria-label": "Text" });
  const presets = el("div", { class: "chips" });
  const tryStatus = el("p", { class: "sub voice-status" }, "");
  const genBtn = el("button", { type: "button", class: "primary" }, "▶ Generate");
  const againBtn = el("button", { type: "button", disabled: true }, "Play again");
  const deviceBtn = el("button", { type: "button", disabled: true, title: "Sends the clip to voices/custom and plays it on the device" },
    "▶ Play on the device");
  const audio = () => ({ pitch: sp.pitch, rate: sp.rate, bandpass: sp.bandpass, lo: sp.lo, hi: sp.hi });
  const chatter = () => ({ exaggeration: sp.exaggeration, cfg_weight: sp.cfg_weight, temperature: sp.temperature });
  const fillPresets = () => {
    const lang = textLanguageFor(localeSel.value);
    const lines = lang ? SPEAKER_PRESETS.map((n) => voicepackText(lang, n)).filter(Boolean) : [];
    presets.replaceChildren(...lines.map((t) => el("button", { type: "button", class: "chip",
      onclick: () => { textInput.value = t; generate(); } }, t)));
    if (!lines.length) presets.append(el("span", { class: "unit" }, "No catalogue lines in this language yet: type your own."));
    readAloud.textContent = `Read this aloud: "${(lang ? ["reset_hold", "reset_wait", "voice_update", "press_button_to_confirm", "bluetooth_pair"]
      .map((n) => voicepackText(lang, n)).filter(Boolean).join(". ") : "a few sentences of your own") || "a few sentences of your own"}."`;
  };
  const generate = async () => {
    const text = textInput.value.trim();
    if (!ownId()) { tryStatus.textContent = "Make a recording first (1)."; return; }
    if (!text) { tryStatus.textContent = "Type a sentence or pick a line."; return; }
    genBtn.disabled = true;
    tryStatus.textContent = "Generating …";
    const t0 = Date.now();
    try {
      const mp3 = await fetchTts(ownVoiceName(localeSel.value, ownId()), text, { settings: chatter(),
        onWait: (s) => { tryStatus.textContent = `The voice server is still at it, asking again in ${s} s …`; } });
      entry = { raw: await decodeRaw(mp3, 5), vol: 1, maxS: 5, text };
      const b = buildClip(entry, audio());
      tryStatus.textContent = `"${text}": ${b.seconds.toFixed(1)} s, made in ${((Date.now() - t0) / 1000).toFixed(1)} s ` +
        "(the same text with the same settings comes from the cache).";
      playClip(b.bytes);
    } catch (e) {
      tryStatus.textContent = e.message;
    }
    genBtn.disabled = false;
    againBtn.disabled = deviceBtn.disabled = !entry;
    if (!deviceClient()) deviceBtn.disabled = true;
  };
  genBtn.addEventListener("click", generate);
  textInput.addEventListener("keydown", (e) => { if (e.key === "Enter") generate(); });
  againBtn.addEventListener("click", () => entry && playClip(buildClip(entry, audio()).bytes));
  deviceBtn.addEventListener("click", async () => {
    const client = deviceClient();
    if (!client || !entry) return;
    deviceBtn.disabled = true;
    try {
      await client.putBytes(customPath(SPEAKER_TEST_CLIP), buildClip(entry, audio()).bytes);
      await deviceTest(`SAY ${SPEAKER_TEST_CLIP} ${playVolume()}`);
    } catch (e) {
      tryStatus.textContent = e.message;
    }
    deviceBtn.disabled = false;
  });
  /* pitch, rate, band-pass: heard at once on the last clip; Chatterbox's settings: generated anew */
  let liveTimer = null;
  const live = () => {
    saveSpeakerSettings(sp);
    if (!entry) return;
    clearTimeout(liveTimer);
    liveTimer = setTimeout(() => playClip(buildClip(entry, audio()).bytes), 150);
  };
  const slider = (key, min, max, step, fmt, onDone) => {
    const range = el("input", { type: "range", min, max, step, "aria-label": key });
    range.value = sp[key];
    const out = el("output", {}, fmt(sp[key]));
    range.addEventListener("input", () => { sp[key] = parseFloat(range.value); out.textContent = fmt(sp[key]); if (!onDone) live(); });
    if (onDone) range.addEventListener("change", () => { saveSpeakerSettings(sp); onDone(); });
    return el("div", { class: "slider" }, range, out);
  };
  const pct = (v) => `${v > 0 ? "+" : ""}${v} %`;
  const regen = () => { if (entry) generate(); };
  const bandBox = el("input", { type: "checkbox" });
  bandBox.checked = !!sp.bandpass;
  const lo = el("input", { type: "number", min: 20, max: 2000, value: sp.lo, "aria-label": "lower corner" });
  const hi = el("input", { type: "number", min: 1000, max: 11000, value: sp.hi, "aria-label": "upper corner" });
  for (const x of [bandBox, lo, hi]) {
    x.addEventListener("change", () => {
      sp.bandpass = bandBox.checked;
      sp.lo = parseInt(lo.value, 10) || 300;
      sp.hi = parseInt(hi.value, 10) || 8000;
      live();
    });
  }
  localeSel.addEventListener("change", () => { sp.locale = localeSel.value; saveSpeakerSettings(sp); fillPresets(); checkName(); });
  const tryCard = fieldset({ title: "2. Try it", help: "Generates with your recording voice on the voice server. Expression, " +
      "pace and variation are Chatterbox's own settings (a change generates the clip anew); pitch, rate and the band-pass " +
      "are applied here, as for every clip of a pack, and heard at once on the last clip." },
  customRow("Language", localeSel, "The language the voice speaks; it sounds most natural in the language of the recording."),
  customRow("Text", textInput), presets,
  el("div", { class: "buttons" }, genBtn, againBtn, deviceBtn), tryStatus,
  customRow("Expression", slider("exaggeration", 0.25, 2, 0.05, (v) => v.toFixed(2), regen), "0.5 is neutral; more is livelier and often faster."),
  customRow("Pace", slider("cfg_weight", 0, 1, 0.05, (v) => v.toFixed(2), regen), "How closely it follows the text's pace; lower is " +
    "slower and more deliberate, 0 also reduces the accent of a recording in another language."),
  customRow("Variation", slider("temperature", 0.05, 2, 0.05, (v) => v.toFixed(2), regen), "Higher is more varied, lower more even."),
  customRow("Pitch", slider("pitch", -50, 50, 1, pct), "Higher or lower, in percent; large shifts sound artificial."),
  customRow("Rate", slider("rate", -50, 50, 1, pct), "Faster or slower speech, the pitch unchanged."),
  customRow("Band-pass", el("div", { class: "band" }, bandBox, lo, el("span", { class: "unit" }, "Hz to"), hi, el("span", { class: "unit" }, "Hz")),
    "Removes the lows the speaker cannot reproduce and the hiss above."));

  /* ---- 3. the template ---- */
  const nameInput = el("input", { type: "text", maxlength: 32, placeholder: "the name others see in the voice list", "aria-label": "Name" });
  const pubLocale = el("select", { "aria-label": "Language of the template" });
  const gender = el("select", { "aria-label": "Gender" }, el("option", { value: "Female" }, "female"), el("option", { value: "Male" }, "male"));
  const nameNote = el("span", { class: "unit" }, "");
  const share = el("input", { type: "checkbox", id: `share-consent-${++uid}` });
  const publishBtn = el("button", { type: "button", class: "primary", disabled: true }, "Create template");
  const pubStatus = el("p", { class: "sub voice-status" }, "");
  const taken = () => {
    const name = nameInput.value.trim().toLowerCase();
    const lang = ttsLanguage(pubLocale.value);
    return !!name && catalog.voices.some((v) => ttsLanguage(v.locale) === lang && (v.friendly || "").trim().toLowerCase() === name);
  };
  const checkName = () => {
    const n = nameInput.value.trim();
    nameNote.textContent = !n ? "" : n.length < 2 ? "at least 2 characters" : taken() ? "taken in this language: choose another name" : "free";
    nameNote.className = `unit${taken() ? " warn-text" : ""}`;
  };
  nameInput.addEventListener("input", checkName);
  pubLocale.addEventListener("change", checkName);
  publishBtn.addEventListener("click", async () => {
    const name = nameInput.value.trim();
    if (!ownId()) { pubStatus.textContent = "Make a recording first (1)."; return; }
    if (name.length < 2 || taken()) { pubStatus.textContent = "Choose a free name of 2 to 32 characters."; return; }
    if (!share.checked) { pubStatus.textContent = "Tick the box to publish the voice for everyone."; return; }
    publishBtn.disabled = true;
    pubStatus.textContent = "Creating the template …";
    try {
      const r = await publishVoice({ voice_id: ownId(), name, locale: pubLocale.value, gender: gender.value,
        settings: chatter(), audio: audio() });
      resetVoiceCatalog();
      catalog = await fetchVoiceCatalog();
      tts.voice = r.voice;
      tts.locale = pubLocale.value;
      Object.assign(tts, audio());
      saveTtsSettings(tts);
      pubStatus.textContent = `"${name}" is in the voice list now, for everyone, with your settings as its defaults - and selected ` +
        "for you in Custom voicepack. The voice server makes the pack clips of it while it is idle, so a whole pack in it " +
        "comes quickly after a while. To have it removed, write to info@breezedude.de.";
      share.checked = false;
      checkName();
    } catch (e) {
      pubStatus.textContent = e.message;
    }
    publishBtn.disabled = !ownId();
  });
  const publishCard = fieldset({ title: "3. Create a template", help: "Makes your recording voice a voice for everyone: it " +
      "appears under this name in the voice list of the configurator (Custom voicepack) in this language, with the settings " +
      "above as its defaults." },
  customRow("Name", el("div", { class: "band" }, nameInput, nameNote)),
  customRow("Language", pubLocale), customRow("Gender", gender),
  el("label", { class: "consent", for: share.id }, share,
    " This is my own voice, or its speaker agreed, and I agree that everyone can use it here. It is removed on request " +
    "only: write to info@breezedude.de."),
  el("div", { class: "buttons" }, publishBtn), pubStatus);

  /* ---- privacy ---- */
  const privacy = el("details", { class: "privacy" }, el("summary", {}, "Privacy: what happens with your recording"),
    el("p", {}, "The recording you make or open here is converted to a WAV file in your browser and sent to the voice " +
      "service voice.breezedude.de, which passes it on to the voice server run privately by the maintainer of this " +
      "configurator. There, the voice features Chatterbox needs to speak in your voice are computed from it. The recording " +
      "itself is not stored, only these features, under a random ID that only your browser knows (and your configuration, " +
      "once you save the generator settings there). Clips generated with it are kept on that server as a cache."),
    el("p", {}, "As long as you only try it, the voice is yours alone: nobody else can find or use it, and \"Delete my " +
      "recording voice\" removes it at once. A template is a copy of the voice features under the name you chose: everyone " +
      "using this configurator can then make clips with it, and its pack clips are made in advance. A template is removed on " +
      "request: write to ", el("a", { href: "mailto:info@breezedude.de" }, "info@breezedude.de"), " with its name."),
    el("p", {}, "The legal basis is your consent (Art. 6 (1) (a) and Art. 9 (2) (a) GDPR), which you can withdraw at any " +
      "time for the future. The voice service records the IP addresses of requests to limit abuse. Every clip made with " +
      "Chatterbox carries an inaudible watermark (Resemble AI's Perth) that marks it as synthetic."));

  /* ---- the lists, once the service answered ---- */
  fetchVoiceCatalog().then((c) => {
    catalog = c;
    const locales = SPEAKER_LOCALES.filter((l) => c.own.languages.includes(ttsLanguage(l)));
    if (!c.own.available || !locales.length) {
      recStatus.textContent = "The voice server at home is offline right now: try again later.";
      recBtn.disabled = openBtn.disabled = genBtn.disabled = publishBtn.disabled = true;
      return;
    }
    const opts = () => locales.map((l) => el("option", { value: l }, localeLabel(l)));
    localeSel.replaceChildren(...opts());
    pubLocale.replaceChildren(...opts());
    localeSel.value = locales.includes(sp.locale) ? sp.locale : locales.includes(pack.tts) ? pack.tts : locales[0];
    pubLocale.value = localeSel.value;
    fillPresets();
    refreshOwn();
    checkName();
  }).catch((e) => { recStatus.textContent = e.message; });
  fillPresets();
  refreshOwn();

  main.replaceChildren(
    el("div", { class: "head" }, el("h2", {}, "Custom speaker"),
      el("p", { class: "sub" }, "Your own voice for the clips: record it, try it, and offer it to everyone as a template.")),
    recordCard, tryCard, publishCard, privacy);
  state.cleanup = () => { left = true; stopPlaying(); clearTimeout(liveTimer); if (recording) recording.stop(); };
}
