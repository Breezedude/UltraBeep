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

/*
 * The recording outlives the view that started it: a redraw (a Bluetooth disconnect or reconnect
 * redraws the page) keeps it going, and leaving the view stops it but keeps it to send or download.
 * The functions below work on whatever Custom speaker view is on screen now (spEl, by id).
 */
const speakerSession = { recording: null, t0: 0, left: false, wav: null, unsent: false, consent: false, busy: false,
  offline: false, status: "" };
const spEl = (id) => document.getElementById(`sp-${id}`);

function speakerStatus(text) {
  speakerSession.status = text;
  const s = spEl("rec-status");
  if (s) s.textContent = text;
}

/* Buttons and steps as the session stands; steps 2 and 3 stay greyed out until there is a recording voice. */
function speakerRefresh() {
  const ss = speakerSession;
  const id = ownVoiceId(ttsSettings());
  const rec = spEl("rec"), open = spEl("open"), send = spEl("send"), dl = spEl("dl"), del = spEl("del");
  if (rec) {
    rec.textContent = ss.recording ? `■ Stop (${Math.round((Date.now() - ss.t0) / 1000)} s)` : "● Record";
    rec.disabled = !ss.recording && (ss.busy || ss.offline);
  }
  if (open) open.disabled = !!ss.recording || ss.busy || ss.offline;
  if (send) { send.hidden = !ss.unsent; send.disabled = ss.busy || ss.offline; }
  if (dl) dl.disabled = !ss.wav;
  if (del) del.hidden = !id;
  for (const card of [spEl("try"), spEl("publish")]) {
    if (!card) continue;
    card.disabled = !id || ss.offline;
    card.classList.toggle("inactive", card.disabled);
  }
  const status = spEl("rec-status");
  if (status && !ss.status && id) status.textContent = "Your recording voice is on the voice server: try it below.";
}

async function speakerRecord() {
  const ss = speakerSession;
  const rec = await startRecording(OWN_MAX_S);
  Object.assign(ss, { recording: rec, t0: Date.now(), left: false });
  speakerStatus(`Recording: read the text above, then stop (after ${OWN_MAX_S} s it stops by itself).`);
  const tick = setInterval(speakerRefresh, 250);
  speakerRefresh();
  rec.done.then((encoded) => {
    clearInterval(tick);
    ss.recording = null;
    speakerTake(encoded, !ss.left); /* stopped by leaving the view: kept, not sent */
  });
}

/* A recording or an opened file: converted to WAV here and kept (for the download), sent when `send`. */
async function speakerTake(encoded, send) {
  const ss = speakerSession;
  ss.busy = true;
  speakerRefresh();
  speakerStatus("Converting …");
  try {
    const wav = await recordingToWav(encoded);
    if (wav.seconds < 6) throw new Error(`The recording has ${wav.seconds.toFixed(1)} s of sound; 10 to 15 s are needed.`);
    Object.assign(ss, { wav, unsent: true, busy: false });
    if (send) return await speakerSend(wav);
    speakerStatus(`Your recording (${wav.seconds.toFixed(0)} s) is kept here but not sent yet: "Send it" makes your voice ` +
      "from it, or download it.");
  } catch (e) {
    speakerStatus(e.name === "EncodingError" ? "The browser cannot read this file as audio." : e.message);
  }
  ss.busy = false;
  speakerRefresh();
}

async function speakerSend(wav) {
  const ss = speakerSession;
  ss.busy = true;
  speakerRefresh();
  speakerStatus(`Sending ${wav.seconds.toFixed(0)} s; the voice server makes the voice from it (a few seconds) …`);
  try {
    const made = await createOwnVoice(wav.bytes);
    const t = ttsSettings();
    const old = ownVoiceId(t);
    t.own = made.id;
    saveTtsSettings(t);
    if (old && old !== made.id) deleteOwnVoice(old).catch(() => { /* gone already */ });
    ss.unsent = false;
    speakerStatus(`Your recording voice is ready (${made.seconds} s of speech used): try it below.`);
  } catch (e) {
    speakerStatus(e.message);
  }
  ss.busy = false;
  speakerRefresh();
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
  const sp = speakerSettings();
  const pack = voicepackInfo(systemValue("system_language_string") || "de");
  const ss = speakerSession;
  let catalog = { voices: [], own: { available: false, languages: [] } };
  let entry = null; /* the last clip: raw audio, rebuilt live with pitch, rate and band-pass */

  /* ---- 1. the recording (its state lives in speakerSession: a redraw of the view keeps it) ---- */
  const recStatus = el("p", { class: "sub voice-status", id: "sp-rec-status" }, ss.status);
  const consent = el("input", { type: "checkbox", id: "sp-consent" });
  consent.checked = ss.consent;
  consent.addEventListener("change", () => { ss.consent = consent.checked; });
  const recBtn = el("button", { type: "button", id: "sp-rec" }, "● Record");
  const fileInput = el("input", { type: "file", accept: "audio/*", hidden: true });
  const openBtn = el("button", { type: "button", id: "sp-open" }, "Open a recording …");
  const sendBtn = el("button", { type: "button", id: "sp-send", class: "primary", hidden: true }, "Send it");
  const dlBtn = el("button", { type: "button", id: "sp-dl", title: "The recording as converted here (WAV): open it here again " +
    "later; it is kept nowhere else" }, "⬇ Download my recording");
  const delBtn = el("button", { type: "button", id: "sp-del" }, "Delete my recording voice");
  const readAloud = el("p", { class: "sub" }, "");
  const needConsent = () => { speakerStatus("Tick the box first: only your own voice, or one you may use."); return false; };
  recBtn.addEventListener("click", async () => {
    if (ss.recording) { ss.recording.stop(); return; }
    if (!consent.checked) return needConsent();
    try {
      await speakerRecord();
    } catch (e) {
      speakerStatus(`No microphone: ${e.message}`);
    }
  });
  openBtn.addEventListener("click", () => fileInput.click());
  fileInput.addEventListener("change", async () => {
    const f = fileInput.files[0];
    fileInput.value = "";
    if (!f) return;
    if (!consent.checked) return needConsent();
    speakerTake(await f.arrayBuffer(), true);
  });
  sendBtn.addEventListener("click", () => {
    if (!consent.checked) return needConsent();
    if (ss.wav) speakerSend(ss.wav);
  });
  dlBtn.addEventListener("click", () => { if (ss.wav) downloadBytes("my-voice-recording.wav", ss.wav.bytes); });
  delBtn.addEventListener("click", async () => {
    const id = ownVoiceId(ttsSettings());
    if (!id || !window.confirm("Delete your recording voice on the voice server? Templates made from it stay.")) return;
    try {
      const was = await deleteOwnVoice(id);
      const t = ttsSettings();
      t.own = "";
      if (OWN_VOICE_RE.test(t.voice || "")) t.voice = "";
      saveTtsSettings(t);
      speakerStatus(was ? "Your recording voice is deleted." : "The voice server did not have it anymore.");
    } catch (e) {
      speakerStatus(e.message);
    }
    speakerRefresh();
  });
  const recordCard = fieldset({ title: "1. Your recording", help: "10 to 15 s of you reading calmly, in a quiet room, in " +
      "the language the voice shall speak - every recording program works (WAV or FLAC at 44.1/48 kHz is best; a phone's " +
      "voice memo works too). It becomes a voice only you can use until you make it a template below. The voice server " +
      "keeps no recording: download it to open it here again later." },
  readAloud,
  el("label", { class: "consent", for: consent.id }, consent, " This is my own voice, or the speaker has allowed me to use it."),
  el("div", { class: "buttons" }, recBtn, openBtn, fileInput, sendBtn, dlBtn, delBtn), recStatus);
  const ownId = () => ownVoiceId(ttsSettings()); /* read anew each time: a recording sent meanwhile counts */

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
  /* while a clip is made, the controls of this step are locked (the text field stays free for the next
   * sentence): one click, one clip, and nothing made earlier plays after it */
  let generating = false;
  let genSeq = 0;
  let left = false; /* this view was replaced (redrawn or left): its clips no longer play */
  const lockTry = (on) => {
    for (const x of tryCard.querySelectorAll("input, select, button")) {
      if (x !== textInput && !x.classList.contains("info")) x.disabled = on;
    }
    if (!on) {
      againBtn.disabled = !entry;
      deviceBtn.disabled = !entry || !deviceClient();
    }
  };
  const generate = async () => {
    if (generating) return;
    const text = textInput.value.trim();
    if (!ownId()) { tryStatus.textContent = "Make a recording first (1)."; return; }
    if (!text) { tryStatus.textContent = "Type a sentence or pick a line."; return; }
    generating = true;
    const mine = ++genSeq;
    lockTry(true);
    tryStatus.textContent = "Generating …";
    try {
      const voiceId = ownId();
      entry = await generateClip(text, { ...TTS_DEFAULTS, ...audio(), voice: ownVoiceName(localeSel.value, voiceId) }, 1, 5,
        { settings: chatter(), onWait: (s) => { tryStatus.textContent = `The voice server is still at it, asking again in ${s} s …`; } });
      entry.voiceId = voiceId;
      const b = buildClip(entry, audio());
      /* the text as the model got it (numbers as words), and "cached" for a clip that came back too fast to be made */
      tryStatus.textContent = `"${text}"${entry.spoken && entry.spoken !== text ? ` → "${entry.spoken}"` : ""}, ` +
        `${b.seconds.toFixed(1)} s${entry.ms < 500 ? " (cached)" : ""}`;
      if (mine === genSeq && !left) playClip(b.bytes);
    } catch (e) {
      tryStatus.textContent = e.message;
    }
    generating = false;
    lockTry(false);
  };
  genBtn.addEventListener("click", generate);
  textInput.addEventListener("keydown", (e) => { if (e.key === "Enter") generate(); });
  /* a clip of a recording voice replaced meanwhile is made anew */
  const current = () => entry && entry.voiceId === ownId();
  againBtn.addEventListener("click", () => (current() ? playClip(buildClip(entry, audio()).bytes) : generate()));
  deviceBtn.addEventListener("click", async () => {
    const client = deviceClient();
    if (!client || !current()) return;
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
    if (!current()) return;
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
  /* top to bottom: what the voice server generates with (language, its settings, the text), what this
   * browser does to the clip (pitch, rate, band-pass), then the buttons */
  const tryCard = fieldset({ title: "2. Try it", help: "Generates with your recording voice on the voice server. Language, " +
      "expression, pace and variation go to the voice server and shape the generation (a change generates the clip anew); " +
      "pitch, rate and the band-pass are applied here, as for every clip of a pack, and heard at once on the last clip." },
  customRow("Language", localeSel, "The language the voice speaks; it sounds most natural in the language of the recording."),
  customRow("Expression", slider("exaggeration", 0.25, 2, 0.05, (v) => v.toFixed(2), regen), "0.5 is neutral; more is livelier and often faster."),
  customRow("Pace", slider("cfg_weight", 0, 1, 0.05, (v) => v.toFixed(2), regen), "How closely it follows the text's pace; lower is " +
    "slower and more deliberate, 0 also reduces the accent of a recording in another language."),
  customRow("Variation", slider("temperature", 0.05, 2, 0.05, (v) => v.toFixed(2), regen), "Higher is more varied, lower more even."),
  customRow("Text", textInput, "Numbers are spoken as words; the status below shows the text as the voice server got it."),
  presets,
  customRow("Pitch", slider("pitch", -50, 50, 1, pct), "Higher or lower, in percent; large shifts sound artificial."),
  customRow("Rate", slider("rate", -50, 50, 1, pct), "Faster or slower speech, the pitch unchanged."),
  customRow("Band-pass", el("div", { class: "band" }, bandBox, lo, el("span", { class: "unit" }, "Hz to"), hi, el("span", { class: "unit" }, "Hz")),
    "Removes the lows the speaker cannot reproduce and the hiss above."),
  el("div", { class: "buttons" }, genBtn, againBtn, deviceBtn), tryStatus);

  /* ---- 3. the template ---- */
  const nameInput = el("input", { type: "text", maxlength: 32, placeholder: "e.g. Anna", "aria-label": "Name" });
  const pubLocale = el("select", { "aria-label": "Language of the template" });
  const gender = el("select", { "aria-label": "Gender" }, el("option", { value: "Female" }, "female"), el("option", { value: "Male" }, "male"));
  const nameNote = el("span", { class: "unit" }, "");
  const share = el("input", { type: "checkbox", id: `share-consent-${++uid}` });
  const publishBtn = el("button", { type: "button", class: "primary" }, "Create template");
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
      const t = ttsSettings();
      Object.assign(t, audio(), { voice: r.voice, locale: pubLocale.value });
      saveTtsSettings(t);
      pubStatus.textContent = `"${name}" is in the voice list now, for everyone, with your settings as its defaults - and selected ` +
        "for you in Custom voicepack. The voice server makes the pack clips of it while it is idle, so a whole pack in it " +
        "comes quickly after a while. To have it removed, write to info@breezedude.de.";
      share.checked = false;
      checkName();
    } catch (e) {
      pubStatus.textContent = e.message;
    }
    publishBtn.disabled = false;
  });
  const publishCard = fieldset({ title: "3. Create a template", help: "Makes your recording voice a voice for everyone: it " +
      "appears under this name in the voice list of the configurator (Custom voicepack) in this language, with the settings " +
      "above as its defaults." },
  customRow("Name", nameInput, "The name others see in the voice list: 2 to 32 characters, not yet taken in this language.",
    nameNote),
  customRow("Language", pubLocale), customRow("Gender", gender),
  el("label", { class: "consent", for: share.id }, share,
    " This is my own voice, or its speaker agreed, and I agree that everyone can use it here. It is removed on request " +
    "only: write to info@breezedude.de."),
  el("div", { class: "buttons" }, publishBtn), pubStatus);

  /* ---- privacy ---- */
  const privacy = el("details", { class: "privacy" }, el("summary", {}, "Privacy: what happens with your recording"),
    el("p", {}, "The recording you make or open here is converted to a WAV file in your browser and sent to the voice " +
      "service voice.breezedude.de, which passes it on to the voice server run privately and locally by Breezedude. " +
      "There, the voice features Chatterbox needs to speak in your voice are computed from it. The recording " +
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
      speakerSession.offline = true;
      speakerStatus("The voice server at home is offline right now: try again later.");
      speakerRefresh();
      return;
    }
    const opts = () => locales.map((l) => el("option", { value: l }, localeLabel(l)));
    localeSel.replaceChildren(...opts());
    pubLocale.replaceChildren(...opts());
    localeSel.value = locales.includes(sp.locale) ? sp.locale : locales.includes(pack.tts) ? pack.tts : locales[0];
    pubLocale.value = localeSel.value;
    speakerSession.offline = false;
    fillPresets();
    speakerRefresh();
    checkName();
  }).catch((e) => { speakerStatus(e.message); });
  fillPresets();
  tryCard.id = "sp-try";
  publishCard.id = "sp-publish";

  main.replaceChildren(
    el("div", { class: "head" }, el("h2", {}, "Custom speaker"),
      el("p", { class: "sub" }, "Your own voice for the clips: record it, try it, and offer it to everyone as a template.")),
    recordCard, tryCard, publishCard, privacy);
  speakerRefresh();
  /* a redraw of this view lets a recording go on; leaving it stops the recording and keeps it */
  state.cleanup = () => {
    left = true;
    stopPlaying();
    clearTimeout(liveTimer);
    if (ss.recording && state.view !== "speaker") { ss.left = true; ss.recording.stop(); }
  };
}
