"use strict";

/*
 * The Custom voicepack view (Voice → Custom voicepack): the voice pack of the device (the same key as
 * System → Language and time; the texts and the voices follow it), where the pack starts from (the
 * catalogue's texts, or a shared pack: its voice, settings and changed texts), the generator settings
 * (voice, pitch, rate, band-pass - heard live on the sample), the clip to generate (any clip of the
 * pack or an own one; as a single file in voices/custom it replaces the pack's clip of that name; a
 * text changed there stays with the pack, e.g. "Haik änd Flai" where a voice misreads "Hike and Fly"),
 * the whole pack generated in the browser and packed into a .ubpack image (js/fatimage.js), shared
 * with everyone if wanted, the clips on the device (play, edit, delete) and the catalogue of the
 * pack's clips (js/voicepack.js) with play and edit.
 */

const genClips = new Map(); /* clip name -> entry {raw, vol, maxS, text, bytes...}, generated in this session */
let genCancel = false;

/* The clip's file in the pack and in voices/custom: "al/al_10" -> "al/al_10.qoa", "takeoff" -> "voices/takeoff.qoa" */
const packPath = (name) => (name.includes("/") ? `${name}.qoa` : `voices/${name}.qoa`);
const customPath = (name) => `voices/custom/${name}.qoa`;
const clipNameOk = (name) => /^([a-z0-9_]{1,8}\/)?[a-z0-9_]{1,24}$/.test(name);

function groundSpeakerVolume() {
  return state.ext.get("global", "volume_speakerground_int") || "50";
}

/* The volume every "play on the device" in Custom voicepack and Custom speaker asks for (SAY <clip> <volume>): set by the
 * slider at the top of the view for this session, starting at the ground volume; the configuration's
 * volumes stay as they are. */
let deviceVolume = null;
const playVolume = () => deviceVolume ?? groundSpeakerVolume();

/* The languages the generator has texts for, as a sentence. */
function voicepackTextLanguages() {
  const names = [...new Set(VOICEPACKS.filter((p) => p.lang).map((p) => p.label.replace(/ \(.*\)$/, "")))];
  return names.slice(0, -1).join(", ") + " and " + names[names.length - 1];
}

function renderVoicepack() {
  leaveView();
  const settings = ttsSettings();
  /* the settings saved on the device win over this browser's once per configuration, so every browser
   * makes the same sound (a shared pack opened as a start later still brings its own) */
  const stored = ttsSettingsFromDoc(state.ext);
  if (stored && state.ttsFromDoc !== state.ext) {
    Object.assign(settings, stored);
    saveTtsSettings(settings);
    state.ttsFromDoc = state.ext;
  }
  const packId = systemValue("system_language_string") || "de";
  const pack = voicepackInfo(packId);
  const canGenerate = !!pack.lang; /* texts exist in the pack's language */
  const catalogLang = pack.lang || "en";
  const packName = pack.label.replace(/ \(.*\)$/, "");
  const textsName = canGenerate ? packName : "English"; /* the language of the texts shown and spoken */
  /* the clips generated in this session, per language of the texts: a pack built after a switch of the
   * voice pack never mixes languages (the German "takeoff" is not the French one) */
  const gKey = (name) => `${catalogLang}|${name}`;
  const G = { has: (name) => genClips.has(gKey(name)), get: (name) => genClips.get(gKey(name)), set: (name, c) => genClips.set(gKey(name), c) };
  const main = $("main");
  const deviceClips = new Set(); /* clip names in voices/custom on the device (refreshDevice) */
  /* the pack's own spellings of catalogue texts (js/voicepacks.js packDraft); a clip whose text was
   * changed is generated with it, and a shared pack passes it on */
  const draft = packDraft(packId);
  const catalogueText = (name) => voicepackText(catalogLang, name);
  const textFor = (name) => (canGenerate && draft.overrides[name]) || catalogueText(name);
  /* a line spoken in another language than the pack's ("GPS ok" in English): Chatterbox voices only,
   * the voice stays the same */
  const packLanguage = ttsLanguage(pack.tts || catalogLang);
  const languageFor = (name) => (canGenerate && draft.languages[name]) || "";
  const langOpt = (language) => (language && language !== packLanguage && homeVoice(settings.voice) ? { language } : {});
  const noteLanguage = (name, language) => {
    if (!canGenerate || !catalogueText(name)) return;
    if (language && language !== packLanguage) draft.languages[name] = language;
    else delete draft.languages[name];
    savePackDraft(packId, draft);
  };
  /* the take chosen for a line ("New variant"): Chatterbox voices only, Edge's speak a text the same every time */
  const variantFor = (name) => (canGenerate && draft.variants[name]) || 0;
  const clipOpt = (name, language = languageFor(name), variant = variantFor(name)) =>
    ({ ...langOpt(language), ...(variant && homeVoice(settings.voice) ? { variant } : {}) });
  const noteVariant = (name, variant) => {
    if (!canGenerate || !catalogueText(name)) return;
    if (variant > 0) draft.variants[name] = variant;
    else delete draft.variants[name];
    savePackDraft(packId, draft);
  };
  const langTag = (name) => {
    const l = languageFor(name);
    return l ? el("span", { class: "lang-tag", title: `spoken in ${languageName(l)}` +
      (homeVoice(settings.voice) ? "" : " (with a Breezedude voice; Edge's voices speak their own language)") }, l.toUpperCase()) : null;
  };
  const noteText = (name, text) => {
    if (!canGenerate || !catalogueText(name)) return;
    if (text && text !== catalogueText(name)) draft.overrides[name] = text;
    else delete draft.overrides[name];
    savePackDraft(packId, draft);
  };

  /* ---- the voice pack ---- */
  const packField = SYSTEM_GROUPS.flatMap((g) => g.fields).find((f) => f.key === "system_language_string");
  const packSel = input(packField, systemValue("system_language_string"), (v) => {
    setSystemValue("system_language_string", v);
    renderVoicepack(); /* the texts and the voices follow the pack */
  });
  /* where the pack starts from: the catalogue's texts, or a shared pack */
  const startSel = el("select", { "aria-label": "Start from" }, el("option", { value: "" }, "the catalogue's texts"));
  const startNote = el("span", { class: "unit" }, "");
  const showStart = () => {
    const n = Object.keys(draft.overrides).length, l = Object.keys(draft.languages).length, v = Object.keys(draft.variants).length;
    startNote.textContent = [n ? `${n} text${n > 1 ? "s" : ""} changed` : "", l ? `${l} in another language` : "",
      v ? `${v} with another variant` : ""].filter(Boolean).join(", ");
  };
  if (draft.base) {
    startSel.append(el("option", { value: draft.base }, draft.baseName || "a shared pack"));
    startSel.value = draft.base;
  }
  showStart();
  fetchPacks(packId).then((list) => {
    for (const p of list) {
      if (p.id !== draft.base) startSel.append(el("option", { value: p.id }, `${p.name} (${p.voice_label})`));
    }
  }).catch(() => { /* the service is away: the catalogue only */ });
  startSel.addEventListener("change", async () => {
    const id = startSel.value;
    if ((Object.keys(draft.overrides).length || Object.keys(draft.variants).length) &&
      !window.confirm("Replace the changed texts and variants of this pack with those of the new start?")) {
      startSel.value = draft.base || "";
      return;
    }
    if (!id) {
      savePackDraft(packId, { base: null, baseName: "", overrides: {}, languages: {}, variants: {} });
    } else {
      try {
        const full = await fetchPack(id);
        savePackDraft(packId, { base: id, baseName: full.name, overrides: full.overrides, languages: full.languages || {},
          variants: full.variants || {} });
        Object.assign(settings, full.settings, { voice: full.voice, locale: "" });
        saveTtsSettings(settings);
      } catch (e) {
        setMessage(e.message, true);
        return;
      }
    }
    renderVoicepack();
  });
  const packNote = el("p", { class: canGenerate ? "sub" : "warn-text" }, canGenerate
    ? `Clips are generated with the ${textsName} texts of the pack; the voices below speak ` +
      `${pack.language === "en" ? "English" : `${packName} or English`}.`
    : `Generating clips in ${packName} is not possible yet: texts exist in ${voicepackTextLanguages()}. ` +
      `The manufacturer's ${pack.id}.ubpack works on the device as it is; a single clip with your own words can ` +
      "still be made below (the catalogue shows the English texts).");

  /* ---- generator settings, the sample (heard live) ---- */
  const status = el("p", { class: "sub voice-status" }, "");
  const picker = voicePicker(settings, pack, (msg) => { status.textContent = msg; });
  let sample = []; /* clip names of the sample, generated */
  const sampleBytes = (names) => names.filter((n) => G.has(n)).map((n) => clipBytes(G.get(n)));
  /* a slider change: the first clips of the sample again with the new sound */
  let liveTimer = null;
  const live = () => {
    saveTtsSettings(settings);
    if (!sample.length) return;
    clearTimeout(liveTimer);
    liveTimer = setTimeout(() => playClips(sampleBytes(sample.slice(0, 4))), 150);
  };
  const slider = (key, min, max, unit) => {
    const range = el("input", { type: "range", min, max, step: 1, "aria-label": key });
    range.value = settings[key];
    const out = el("output", {}, `${settings[key] > 0 ? "+" : ""}${settings[key]} ${unit}`);
    range.addEventListener("input", () => {
      out.textContent = `${range.value > 0 ? "+" : ""}${range.value} ${unit}`;
      settings[key] = parseInt(range.value, 10);
      live();
    });
    return el("div", { class: "slider" }, range, out);
  };
  const bandBox = el("input", { type: "checkbox" });
  bandBox.checked = !!settings.bandpass;
  const lo = el("input", { type: "number", min: 20, max: 2000, value: settings.lo, "aria-label": "lower corner" });
  const hi = el("input", { type: "number", min: 1000, max: 11000, value: settings.hi, "aria-label": "upper corner" });
  const bandChange = () => {
    settings.bandpass = bandBox.checked;
    settings.lo = parseInt(lo.value, 10) || 300;
    settings.hi = parseInt(hi.value, 10) || 8000;
    live();
  };
  for (const x of [bandBox, lo, hi]) x.addEventListener("change", bandChange);
  const sampleBtn = el("button", { class: "primary", type: "button", disabled: !canGenerate }, "Create a sample");
  const samplePlay = el("button", { type: "button", disabled: true }, "▶ Play the sample");
  const sampleStop = el("button", { type: "button" }, "Stop");
  /* a Chatterbox clip still in the making: the status says so while fetchTts asks again */
  const waiting = (target, what) => (s) => { target.textContent = `${what}: the voice server is still at it, asking again in ${s} s …`; };
  sampleBtn.addEventListener("click", async () => {
    if (!picker.voiceSel.value) { setMessage("Choose a voice first.", true); return; }
    sampleBtn.disabled = true;
    sample = [];
    const names = voicepackSample();
    try {
      for (let i = 0; i < names.length; i++) {
        status.textContent = `Sample: ${i + 1} of ${names.length} (${names[i]}) …`;
        G.set(names[i], await generateClip(textFor(names[i]), settings, 1, 5,
          { ...clipOpt(names[i]), onWait: waiting(status, `Sample ${i + 1} of ${names.length}`) }));
        sample.push(names[i]);
      }
      status.textContent = `Sample: ${names.length} clips as the device plays them - "${names.map(textFor).join(" · ")}". ` +
        "Move pitch, rate or the band-pass: the first clips play again with the new sound.";
      playClips(sampleBytes(sample));
      samplePlay.disabled = false;
    } catch (e) {
      status.textContent = e.message;
    }
    sampleBtn.disabled = false;
    packInfo();
  });
  samplePlay.addEventListener("click", () => playClips(sampleBytes(sample)));
  sampleStop.addEventListener("click", stopPlaying);

  /* a voice with defaults (a template from Custom speaker) brings its pitch, rate and band-pass along */
  picker.voiceSel.addEventListener("change", () => {
    const v = picker.current();
    if (v && v.defaults) {
      Object.assign(settings, v.defaults);
      saveTtsSettings(settings);
      renderVoicepack();
      return;
    }
    packInfo();
    edRefresh();        /* "Spoken in" works with Chatterbox voices only */
    renderCatalogue();
  });
  picker.ready.then(() => { packInfo(); edRefresh(); });

  /* ---- the clip to generate ---- */
  /* the name is never typed: the catalogue (✎) or a profile's voice line sets it, so every clip made here is one
   * the firmware knows and plays */
  const nameInput = el("input", { type: "text", readonly: true, placeholder: "from the catalogue (✎) or a profile's voice line",
    "aria-label": "Clip name" });
  const textInput = el("input", { type: "text", placeholder: "the words to speak", "aria-label": "Text" });
  const edLang = el("select", { "aria-label": "Spoken in" }, el("option", { value: "" }, `${languageName(packLanguage)} (the pack's language)`),
    ["en", ...CHATTERBOX_LANGUAGES.filter((l) => l !== "en")].filter((l) => l !== packLanguage)
      .map((l) => el("option", { value: l }, languageName(l))));
  /* the take of the line: ◀ ▶ step through the variants, each generated at once (one made before comes from the cache) */
  let edVariant = 0, edBusy = false;
  const varPrev = el("button", { type: "button", class: "play", title: "The previous variant", "aria-label": "Previous variant" }, "◀");
  const varNext = el("button", { type: "button", class: "play", title: "The next variant: another take of the same text",
    "aria-label": "Next variant" }, "▶");
  const varOut = el("output", { class: "unit" }, "");
  const showVariant = () => { varOut.textContent = edVariant ? `variant ${edVariant}` : "the default take"; };
  showVariant();
  const edWhat = el("p", { class: "sub" }, "");
  let edProfile = null; /* the profile whose voice line is in the editor */
  const edStatus = el("p", { class: "sub voice-status" }, "Pick a clip from the catalogue below (✎), or open a profile's voice line from its Identity card.");
  const edGen = el("button", { class: "primary", type: "button" }, "Generate");
  const edPlay = el("button", { type: "button", disabled: true }, "▶ Play here");
  const edSend = el("button", { type: "button", disabled: true }, "Send to device");
  const edDevice = el("button", { type: "button", disabled: true }, "▶ Play on the device");
  /* the device has a clip when the pack has it (every clip the original packs know) or it is in voices/custom */
  const onDevice = (name) => {
    const c = voicepackClips(catalogLang).find((x) => x.name === name);
    return (!!c && c.stock !== false) || deviceClips.has(name);
  };
  const edRefresh = () => {
    const c = G.get(nameInput.value.trim());
    edPlay.disabled = !c;
    edSend.disabled = !c || !deviceClient();
    edDevice.disabled = !deviceClient() || !onDevice(nameInput.value.trim());
    edLang.disabled = !homeVoice(settings.voice);
    edLang.title = edLang.disabled ? "Edge's voices speak their own language: choose a Breezedude (Chatterbox) voice for this" : "";
    varPrev.disabled = !homeVoice(settings.voice) || edBusy || edVariant <= 0;
    varNext.disabled = !homeVoice(settings.voice) || edBusy || edVariant >= TTS_MAX_VARIANT;
    varNext.title = homeVoice(settings.voice) ? "The next variant: another take of the same text"
      : "Edge's voices speak a text the same every time: choose a Breezedude (Chatterbox) voice for variants";
  };
  nameInput.addEventListener("input", edRefresh);
  const edit = (name, text, profile = null) => {
    edProfile = profile;
    nameInput.value = name;
    textInput.value = text || (canGenerate ? textFor(name) : "") || "";
    edLang.value = profile ? "" : languageFor(name);
    edVariant = profile ? 0 : variantFor(name);
    showVariant();
    edWhat.textContent = profile
      ? `The voice line of profile ${profile} (${getP(profile, CUSTOM_NAME_KEY, true) || "custom name"}): stored as voices/custom/${name}.qoa ` +
        "and noted in the profile when you send it."
      : "";
    edStatus.textContent = G.has(name) ? "Generated in this session: play, send or generate again."
      : canGenerate || text ? "" : `No ${packName} text for this clip yet: type the words to speak.`;
    edRefresh();
    clipHead.scrollIntoView({ block: "start", behavior: "smooth" });
    textInput.focus();
  };
  const generateLine = async () => {
    const name = nameInput.value.trim(), text = textInput.value.trim();
    if (!clipNameOk(name)) { edStatus.textContent = "Pick a clip from the catalogue (✎) or open a profile's voice line first."; return; }
    if (!text) { edStatus.textContent = "Enter the text to speak."; return; }
    if (!picker.voiceSel.value) { setMessage("Choose a voice first.", true); return; }
    edBusy = edGen.disabled = true;
    edRefresh();
    const variant = homeVoice(settings.voice) ? edVariant : 0;
    edStatus.textContent = `Asking the voice service${variant ? ` for variant ${variant}` : ""} …`;
    try {
      const c = await generateClip(text, settings, 1, CLIP_MAX_S, { ...clipOpt(name, edLang.value, variant),
        onWait: waiting(edStatus, "Generating") });
      G.set(name, c);
      /* text, language and take stay with the pack */
      if (!edProfile) {
        noteText(name, text);
        noteLanguage(name, edLang.value);
        if (homeVoice(settings.voice)) noteVariant(name, variant); /* an Edge voice leaves the chosen take alone */
        showStart();
      }
      edStatus.textContent = `Generated${variant ? ` (variant ${variant})` : ""}${c.spoken && c.spoken !== text ? `: "${c.spoken}"` : ""}, ` +
        `${c.seconds.toFixed(2)} s, ${(c.bytes.length / 1024).toFixed(1)} kB${c.ms < 500 ? " (cached)" : ""}.`;
      playClip(c.bytes);
      renderCatalogue();
    } catch (e) {
      edStatus.textContent = e.message;
    }
    edBusy = edGen.disabled = false;
    edRefresh();
  };
  edGen.addEventListener("click", generateLine);
  const stepVariant = (d) => {
    edVariant = Math.max(0, Math.min(TTS_MAX_VARIANT, edVariant + d));
    showVariant();
    generateLine();
  };
  varPrev.addEventListener("click", () => stepVariant(-1));
  varNext.addEventListener("click", () => stepVariant(1));
  edPlay.addEventListener("click", () => playClip(clipBytes(G.get(nameInput.value.trim()))));
  edSend.addEventListener("click", async () => {
    const name = nameInput.value.trim();
    const client = deviceClient();
    if (!client || !G.has(name)) return;
    edSend.disabled = true;
    edStatus.textContent = "Sending …";
    try {
      await client.putBytes(customPath(name), clipBytes(G.get(name)));
      if (edProfile) {
        /* the profile now names this clip; an older one under another name goes */
        const old = getP(edProfile, VOICE_CLIP_KEY, true);
        if (old && old !== name) client.deleteFile(customPath(old)).catch(() => {});
        setP(edProfile, VOICE_CLIP_KEY, name, true);
        setP(edProfile, VOICE_TEXT_KEY, textInput.value.trim(), true);
        edStatus.textContent = `Stored on the device as ${customPath(name)} and set as the voice line of profile ${edProfile} - save the configuration.`;
      } else {
        edStatus.textContent = `Stored on the device as ${customPath(name)}; the device uses it instead of the pack's clip.`;
      }
      refreshDevice();
    } catch (e) {
      edStatus.textContent = e.message;
    }
    edRefresh();
  });
  edDevice.addEventListener("click", () => deviceTest(`SAY ${nameInput.value.trim()} ${playVolume()}`));

  /* ---- the whole pack ---- */
  const bar = el("progress", { max: 100, value: 0, hidden: true });
  const packStatus = el("p", { class: "sub" }, "");
  const errors = el("p", { class: "warn-text" }, "");
  const allBtn = el("button", { class: "primary", type: "button", disabled: !canGenerate }, "Generate all clips");
  const stopBtn = el("button", { type: "button", hidden: true }, "Stop");
  const buildBtn = el("button", { type: "button", disabled: !canGenerate }, "Build the .ubpack");
  /* language, voice, pitch, rate and band-pass into config_ext.ultrabip, saved like any setting */
  const saveGen = el("button", { type: "button", title: "Store language, voice, pitch, rate and band-pass in the configuration " +
    "on the device: every browser that connects uses them, so all clips sound the same" }, "Save audio generator settings");
  saveGen.addEventListener("click", async () => {
    if (!picker.voiceSel.value) { setMessage("Choose a voice first.", true); return; }
    ttsSettingsToDoc(state.ext, settings);
    await save();
    setMessage(`Audio generator settings saved${state.source && state.source.kind === "ble" ? " on the device" : ""}: ` +
      "every browser that opens this configuration uses them.");
  });
  const countGenerated = () => voicepackClips(catalogLang).filter((c) => G.has(c.name)).length;
  const packInfo = () => {
    if (!canGenerate) { packStatus.textContent = `No ${packName} texts yet: the whole pack cannot be generated.`; return; }
    const all = voicepackClips(catalogLang).length;
    const have = countGenerated();
    const ext = voicepackClips(catalogLang).filter((c) => c.stock === false);
    const v = picker.current();
    const slow = v && v.provider === "chatterbox" && have < all;
    packStatus.textContent = `${have} of ${all} clips of the ${textsName} pack generated in this session ` +
      `(${ext.filter((c) => G.has(c.name)).length} of the ${ext.length} extension clips the original pack lacks).` +
      (slow ? ` A Chatterbox voice takes about ${v.seconds} s per clip not made before - up to ` +
        `${Math.max(1, Math.round(((all - have) * v.seconds) / 60))} min for the rest; clips made already come at once.` : "");
    buildBtn.textContent = have === all ? `Build ${pack.id}.ubpack` : `Build ${pack.id}.ubpack (${all - have} missing)`;
  };
  /* Generates a list of clips, two at a time: the whole pack, or the extension clips from the catalogue. */
  const runGeneration = async (clips, what) => {
    if (!picker.voiceSel.value) { setMessage("Choose a voice first.", true); return; }
    if (!clips.length) { packStatus.textContent = `Nothing to do: ${what} are generated.`; return; }
    const count = clips.length;
    genCancel = false;
    allBtn.disabled = true;
    stopBtn.hidden = false;
    bar.hidden = false;
    errors.textContent = "";
    const failed = [];
    const retried = new Set(); /* a clip the service refused once goes to the end of the queue once more */
    let done = 0;
    const t0 = Date.now();
    const worker = async () => {
      while (clips.length && !genCancel) {
        const clip = clips.shift();
        try {
          /* pack clips may be longer than Bluetooth ones; on the voice server they wait behind single clips */
          G.set(clip.name, await generateClip(textFor(clip.name), settings, 1, 5, { ...clipOpt(clip.name), bulk: true }));
        } catch (e) {
          if (e.status === 429) { genCancel = true; failed.push("Rate limit of the service reached: raise RATE_LIMIT in the relay's .env for a whole pack."); }
          else if (e.status !== 400 && !retried.has(clip.name)) { retried.add(clip.name); clips.push(clip); continue; }
          failed.push(`${clip.name}: ${e.message}`);
        }
        done++;
        bar.value = Math.round(100 * done / count);
        const s = (Date.now() - t0) / 1000;
        packStatus.textContent = `Generating ${what}: ${done} of ${count} (${(done / Math.max(s, 1) * 60).toFixed(0)} per minute)`;
      }
    };
    await Promise.all([worker(), worker()]);
    bar.hidden = true;
    stopBtn.hidden = true;
    allBtn.disabled = false;
    if (failed.length) errors.textContent = `${failed.length} failed: ${failed.slice(0, 5).join("; ")}${failed.length > 5 ? " …" : ""}`;
    packInfo();
    renderCatalogue();
  };
  allBtn.addEventListener("click", () => runGeneration(voicepackClips(catalogLang).filter((c) => !G.has(c.name)), "the pack's clips"));
  /* Sends generated clips to the device, as single files in voices/custom. */
  const sendClips = async (clips) => {
    const client = deviceClient();
    if (!client || !clips.length) return;
    let n = 0;
    try {
      for (const c of clips) {
        extNote.textContent = `Sending ${c.name} (${n + 1} of ${clips.length}) …`;
        await client.putBytes(customPath(c.name), clipBytes(G.get(c.name)));
        n++;
      }
      setMessage(`${n} clips stored in voices/custom on the device; it uses them instead of the pack's.`);
    } catch (e) {
      setMessage(`${e.message} (${n} of ${clips.length} sent)`, true);
    }
    refreshDevice();
    renderCatalogue();
  };
  stopBtn.addEventListener("click", () => { genCancel = true; });
  /* The .ubpack of the clips generated in this session, with the settings as they are now: {image, clips}. */
  const buildImage = () => {
    const files = [];
    for (const c of voicepackClips(catalogLang)) {
      const g = G.get(c.name);
      if (g) files.push({ path: packPath(c.name), bytes: clipBytes(g) });
    }
    if (!files.length) throw new Error("Nothing generated yet.");
    const date = new Date().toISOString().slice(0, 10);
    files.push({ path: "version.txt", bytes: new TextEncoder().encode(`${date}\n`) });
    return { image: buildFat16Image(files, "UBVOICES"), clips: files.length - 1 };
  };
  buildBtn.addEventListener("click", () => {
    try {
      const { image, clips } = buildImage();
      downloadBytes(`${pack.id}.ubpack`, image);
      packStatus.textContent = `${pack.id}.ubpack: ${clips} clips, ${(image.length / 1024 / 1024).toFixed(1)} MB. Copy it to voices/ on the SD card ` +
        `(over USB); the device loads it as the ${packName} pack.`;
    } catch (e) {
      packStatus.textContent = e.message;
    }
  });

  /* ---- sharing the pack with everyone ---- */
  const shareName = el("input", { type: "text", maxlength: 40, placeholder: "e.g. Katja calm", "aria-label": "Pack name" });
  const shareNote = el("span", { class: "unit" }, "");
  const shareOk = el("input", { type: "checkbox", id: `pack-consent-${++uid}` });
  const shareBtn = el("button", { type: "button", class: "primary" }, "Share this pack");
  const shareStatus = el("p", { class: "sub voice-status" }, "");
  let sharedNames = [];
  const checkShareName = () => {
    const n = shareName.value.trim();
    const taken = sharedNames.includes(n.toLowerCase());
    shareNote.textContent = !n ? "" : n.length < 2 ? "at least 2 characters" : taken ? `taken among the ${packName} packs` : "free";
    shareNote.className = `unit${taken ? " warn-text" : ""}`;
    return n.length >= 2 && !taken;
  };
  fetchPacks(packId).then((list) => { sharedNames = list.map((p) => p.name.toLowerCase()); checkShareName(); }).catch(() => { /* away */ });
  shareName.addEventListener("input", checkShareName);
  shareBtn.addEventListener("click", async () => {
    const all = voicepackClips(catalogLang).length, have = countGenerated();
    const v = picker.current();
    if (!canGenerate) { shareStatus.textContent = `No ${packName} texts yet: only whole packs can be shared.`; return; }
    if (have < all) { shareStatus.textContent = `Generate the whole pack first: ${all - have} clips are missing.`; return; }
    if (!v) { shareStatus.textContent = "Choose a voice first."; return; }
    if (!checkShareName()) { shareStatus.textContent = "Choose a free name of 2 to 40 characters."; return; }
    if (!shareOk.checked) { shareStatus.textContent = "Tick the box to publish the pack for everyone."; return; }
    shareBtn.disabled = true;
    try {
      shareStatus.textContent = "Building the pack …";
      const { image, clips } = buildImage();
      const name = shareName.value.trim();
      await sharePack({ name, lang: packId, voice: v.name, voice_label: v.friendly || v.name, clips,
        settings: { pitch: settings.pitch, rate: settings.rate, bandpass: settings.bandpass, lo: settings.lo, hi: settings.hi },
        overrides: draft.overrides, languages: draft.languages, variants: draft.variants, base: draft.base }, image,
      (n, total) => { shareStatus.textContent = `Uploading: ${Math.round((100 * n) / total)} % of ${(image.length / 1024 / 1024).toFixed(1)} MB …`; });
      sharedNames.push(name.toLowerCase());
      shareStatus.textContent = `Shared: "${name}" is in the list of the ${packName} packs (Voice) for everyone. ` +
        "To have it removed, write to info@breezedude.de.";
      shareOk.checked = false;
      checkShareName();
    } catch (e) {
      shareStatus.textContent = e.message;
    }
    shareBtn.disabled = false;
  });
  const shareCard = fieldset({ title: "Share this pack", help: "Publishes the whole pack generated above - its clips, voice, " +
      "settings and changed texts - for everyone: it appears under Voice in the list of this language, where others hear, " +
      "download or start from it. The file is kept on the voice server at home." },
  customRow("Name", shareName, "The name others see in the list of this language's packs: 2 to 40 characters, not yet taken.",
    shareNote),
  el("label", { class: "consent", for: shareOk.id }, shareOk,
    " I publish this pack for everyone. Its voice is one I may use, and its texts are fine for all to hear. It is removed " +
    "on request only: write to info@breezedude.de."),
  el("div", { class: "buttons" }, shareBtn), shareStatus);

  const clipHead = el("div", { class: "subhead" }, "Clip", info("One clip: the pack's text or your own. Generate, listen here, " +
    "send it to the device - it lands in voices/custom and replaces the pack's clip of that name - and play it there.", "Clip"));
  const generationCard = fieldset({ title: "Voice generation", help: "Clips from text-to-speech for the device: single clips, " +
      "the profile voice lines, or a whole voice pack. The voice pack at the top is the device's (System → Language and " +
      "time): its language picks the texts of the clips and the voices offered. The service speaks the words; pitch, rate " +
      "and the band-pass are applied in the browser, so you hear a change at once on the sample and the clips that go " +
      "to the device are exactly what you heard. \"Create a sample\" speaks a few typical lines as single clips, as the " +
      "device strings them together." },
  customRow("Voice pack", packSel, packField.help), packNote,
  customRow("Start from", el("div", { class: "band" }, startSel, startNote), "The catalogue texts, or a pack someone shared: " +
    "its voice, settings and changed texts come along (Voice lists the shared packs)."),
  customRow("Language", picker.langSel, "The voices of the pack's language and English."),
  customRow("Voice", picker.voiceSel, "Breezedude (Chatterbox): the voices people shared as templates. They sound more " +
    "natural than Microsoft Edge's, but take a few seconds per clip not made before - their pack clips are made in " +
    "advance - and are missing while Breezedude's voice server is offline. Your own voice appears here once you share " +
    "it as a template (Voice → Custom speaker)."),
  customRow("Pitch", slider("pitch", -50, 50, "%"), "Higher or lower, in percent; large shifts sound artificial."),
  customRow("Rate", slider("rate", -50, 50, "%"), "Faster or slower speech, the pitch unchanged."),
  customRow("Band-pass", el("div", { class: "band" }, bandBox, lo, el("span", { class: "unit" }, "Hz to"), hi,
    el("span", { class: "unit" }, "Hz")), "Removes the lows the speaker cannot reproduce and the hiss above."),
  el("div", { class: "buttons" }, sampleBtn, samplePlay, sampleStop), status,
  clipHead,
  customRow("Name", nameInput, "The clip's name as the firmware asks for it, set by the catalogue or by a profile's voice " +
    "line - never by hand, a clip under another name would never be played."),
  edWhat,
  customRow("Text", textInput, textTips(true)),
  customRow("Spoken in", edLang, "The language this line is spoken in, the voice the same: e.g. \"GPS ok\" or \"Hike and Fly\" " +
    "in English within a German pack. Kept with the pack and shown in the catalogue (EN); Breezedude (Chatterbox) voices only."),
  customRow("Variant", el("div", { class: "variant-pick" }, varPrev, varOut, varNext), "Another take of the same text: the " +
    "voice draws its speech anew for every variant, and each one sounds the same every time (made once, then cached). When " +
    "a line comes out wrong - a word added or swallowed - try ▶; ◀ goes back. The variant stays with the pack: the whole " +
    "pack and a shared pack use it. Breezedude (Chatterbox) voices only; Edge's voices speak a text the same every time."),
  el("div", { class: "buttons" }, edGen, edPlay, edSend, edDevice), edStatus,
  el("div", { class: "subhead" }, "Whole pack", info("Every clip of the pack in its language, two at a time (the relay's " +
    "rate limit must allow it), packed into a FAT image the device mounts as the voice pack.", "Whole pack")),
  el("div", { class: "pack" }, el("div", { class: "buttons" }, allBtn, stopBtn, buildBtn, saveGen), bar, packStatus, errors));

  /* ---- the clips on the device ---- */
  const devBody = el("tbody", {});
  const devNote = el("p", { class: "sub" }, deviceClient() ? "Loading …" : "Connect via Bluetooth to see the clips on the device.");
  const refreshDevice = async () => {
    const client = deviceClient();
    if (!client) return;
    try {
      const list = (await client.listTracks("voices/custom")).sort((a, b) => a.name.localeCompare(b.name));
      deviceClips.clear();
      for (const f of list) deviceClips.add(f.name.replace(/\.qoa$/, ""));
      devBody.replaceChildren(...list.map((f) => {
        const name = f.name.replace(/\.qoa$/, "");
        return el("tr", {},
          el("td", { class: "mono" }, f.name),
          el("td", {}, `${(f.size / 1024).toFixed(1)} kB`),
          el("td", { class: "actions" },
            el("button", { class: "play", type: "button", title: "Play on the device", onclick: () => deviceTest(`SAY ${name} ${playVolume()}`) }, "▶"),
            el("button", { class: "play", type: "button", title: "Edit: generate it again", onclick: () => edit(name) }, "✎"),
            el("button", { class: "play danger", type: "button", title: "Delete from the device", onclick: async () => {
              if (!window.confirm(`Delete ${f.name} from the device? The pack's clip of that name is used again.`)) return;
              try {
                await client.deleteFile(customPath(name));
                setMessage(`${f.name} deleted.`);
                refreshDevice();
              } catch (e) { setMessage(e.message, true); }
            } }, "🗑")));
      }));
      devNote.textContent = list.length ? `${list.length} clips in voices/custom.` : "No clips in voices/custom: the device uses the pack only.";
      edRefresh();
      renderCatalogue(); /* the ▶ buttons follow what the device has */
    } catch (e) {
      devNote.textContent = e.message.includes("not allowed") ? "This firmware cannot list the clips." : e.message;
    }
  };
  const deviceCard = fieldset({ title: "Clips on the device", help: "The single files in voices/custom on the SD card. " +
      "They replace the pack's clips of the same name; a deleted one gives the pack's clip back." },
  devNote, el("div", { class: "scroll-x" }, el("table", { class: "cliplist" },
    el("thead", {}, el("tr", {}, el("th", {}, "File"), el("th", {}, "Size"), el("th", {}, ""))), devBody)),
  deviceClient() ? el("div", { class: "buttons" }, el("button", { type: "button", onclick: refreshDevice }, "Refresh")) : null);

  /* ---- the catalogue: the extension clips and the original pack's clips, two tables, one filter ---- */
  const filter = el("input", { type: "text", placeholder: "filter by name or text", "aria-label": "Filter" });
  const extBody = el("tbody", {});
  const stockBody = el("tbody", {});
  const extNote = el("p", { class: "sub" }, "");
  const stockNote = el("p", { class: "sub" }, "");
  const extGen = el("button", { type: "button", title: "Generate the missing extension clips" }, "Generate");
  const extSend = el("button", { type: "button", title: "Send the generated extension clips to the device (voices/custom)" }, "Send to device");
  /* number series ("al/al_-100 … al/al_7000", the hours, the speeds, the load factors) fold into one row each; the words come first */
  const expanded = new Set();
  const clipRow = (c) => el("tr", { class: G.has(c.name) ? "done" : "" },
    el("td", { class: "mono" }, c.name),
    el("td", {}, textFor(c.name), draft.overrides[c.name] ? el("small", { class: "unit", title: `catalogue: ${c.text}` }, " (changed)") : null,
      variantFor(c.name) ? el("small", { class: "unit", title: "another take of this text (Variant in the editor)" }, ` · variant ${variantFor(c.name)}`) : null),
    el("td", { class: "actions" }, langTag(c.name),
      deviceClient() && onDevice(c.name) ? el("button", { class: "play", type: "button", title: "Play on the device",
        onclick: () => deviceTest(`SAY ${c.name} ${playVolume()}`) }, "▶") : null,
      G.has(c.name) ? el("button", { class: "play", type: "button", title: "Play the generated clip here",
        onclick: () => playClip(clipBytes(G.get(c.name))) }, "♪") : null,
      el("button", { class: "play", type: "button", title: "Edit", onclick: () => edit(c.name, canGenerate ? textFor(c.name) : "") }, "✎")));
  const seriesKey = (name) => {
    if (/^\d+$/.test(name)) return "number";
    if (/^\d+_percent$/.test(name)) return "percent";
    const m = /^(.*?)_(-?\d+)$/.exec(name);
    return m ? m[1] : null;
  };
  const matches = (c, q) => c.name.includes(q) || textFor(c.name).toLowerCase().includes(q);
  /* the rows of a table: its words, then one row per number series (opened: its clips); {rows, words, series} */
  const folded = (clips) => {
    const groups = new Map();
    for (const c of clips) {
      const k = seriesKey(c.name);
      if (k) (groups.get(k) || groups.set(k, []).get(k)).push(c);
    }
    /* a series: at least four clips whose texts begin with a number or a minus ("23 Uhr", "ore 14", "minus 1,5 ..."), not words like vol_1 */
    const numeric = (list) => list.length >= 4 && list.every((c) => /^(-|minus |moins |meno |menos |min |ore )?\d/i.test(c.text));
    const words = clips.filter((c) => { const k = seriesKey(c.name); return !k || !numeric(groups.get(k)); });
    const series = [...groups.entries()].filter(([, list]) => numeric(list));
    const rows = words.map(clipRow);
    for (const [key, list] of series) {
      const have = list.filter((c) => G.has(c.name)).length;
      /* extension clips: how many voices/custom on the device has */
      const sent = deviceClient() && list[0].stock === false ? list.filter((c) => deviceClips.has(c.name)).length : 0;
      const open = expanded.has(key);
      rows.push(el("tr", { class: "group" + (have === list.length ? " done" : "") },
        el("td", { class: "mono" }, el("button", { class: "fold", type: "button", "aria-expanded": String(open),
          onclick: () => { if (open) expanded.delete(key); else expanded.add(key); renderCatalogue(); } }, open ? "▾" : "▸"),
        ` ${list[0].name} … ${list[list.length - 1].name}`),
        el("td", {}, `${list.length} clips: "${list[0].text}" … "${list[list.length - 1].text}"`),
        el("td", { class: "actions" }, el("span", { class: "unit" },
          [have ? `${have} of ${list.length} generated` : "", sent ? `${sent} on the device` : ""].filter(Boolean).join(" · ")))));
      if (open) rows.push(...list.map(clipRow));
    }
    return { rows, words, series };
  };
  const renderCatalogue = () => {
    const q = filter.value.trim().toLowerCase();
    const all = voicepackClips(catalogLang);
    const ext = all.filter((c) => c.stock === false);
    const stock = all.filter((c) => c.stock !== false);
    /* the extension clips */
    const extShown = q ? ext.filter((c) => matches(c, q)) : ext;
    extBody.replaceChildren(...(q ? extShown.map(clipRow) : folded(ext).rows));
    const extDone = ext.filter((c) => G.has(c.name)).length;
    const extOnDevice = ext.filter((c) => deviceClips.has(c.name)).length;
    extNote.textContent = (q ? `${extShown.length} of ${ext.length} match; ` : "") +
      `${extDone} of ${ext.length} generated in this session` +
      (deviceClient() ? `, ${extOnDevice} of ${ext.length} on the device (voices/custom).` : ".");
    extGen.disabled = !canGenerate || extDone === ext.length;
    extSend.hidden = !deviceClient();
    extSend.disabled = !deviceClient() || !extDone;
    /* the original pack's clips */
    if (q) {
      const rows = stock.filter((c) => matches(c, q));
      stockBody.replaceChildren(...rows.slice(0, 400).map(clipRow));
      stockNote.textContent = rows.length > 400 ? `${rows.length} of ${stock.length} match, the first 400 shown - narrow the filter.`
        : `${rows.length} of ${stock.length} match.`;
      return;
    }
    const { rows, words, series } = folded(stock);
    stockBody.replaceChildren(...rows);
    stockNote.textContent = `${words.length} words and ${series.length} number series (${stock.length} clips) in ` +
      `${textsName}${canGenerate ? "" : ` (no ${packName} texts yet)`}.`;
  };
  filter.addEventListener("input", renderCatalogue);
  extGen.addEventListener("click", () => runGeneration(voicepackClips(catalogLang).filter((c) => c.stock === false && !G.has(c.name)), "the extension clips"));
  extSend.addEventListener("click", () => sendClips(voicepackClips(catalogLang).filter((c) => c.stock === false && G.has(c.name))));
  const clipTable = (body) => el("div", { class: "scroll-x" }, el("table", { class: "cliplist" },
    el("thead", {}, el("tr", {}, el("th", {}, "Clip"), el("th", {}, "Text"), el("th", {}, ""))), body));
  const catalogueCard = fieldset({ title: "Clips", help: "What a voice pack has and what it says in the language of the " +
      "pack chosen above, in two tables: the clips UltraBeep adds - the original packs do not have them: the launch " +
      "readiness, the pairing request, glide ratio, load and bank in the announcements, the load factor as one clip per " +
      "tenth of a g and the bank angle in steps of 5 degrees (said instead of single numbers once the device has them); " +
      "generate them and send them to " +
      "the device, or build a whole pack - and the clips of the original pack. The filter works in both. ▶ plays a clip " +
      "the device has (the pack's, or one sent to voices/custom), ♪ a clip generated here, ✎ opens it in the editor." },
  customRow("Filter", filter),
  el("div", { class: "subhead" }, "UltraBeep extension clips", el("span", { class: "spacer" }), extGen, extSend),
  extNote, clipTable(extBody),
  el("div", { class: "subhead" }, "Clips of the original pack"),
  stockNote, clipTable(stockBody));
  renderCatalogue();
  packInfo();
  if (state.voiceLineFor) {
    const p = state.voiceLineFor;
    state.voiceLineFor = null;
    setTimeout(() => edit(voiceClipNameFor(p), getP(p, VOICE_TEXT_KEY, true) || getP(p, CUSTOM_NAME_KEY, true) || "", p), 0);
  }

  /* ---- the device's volume for the ▶ buttons of this view (fixed at the side while scrolling) ---- */
  const volRange = el("input", { type: "range", min: 0, max: 100, step: 5, "aria-label": "Device volume" });
  volRange.value = playVolume();
  const volOut = el("output", {}, `${volRange.value} %`);
  volRange.addEventListener("input", () => { deviceVolume = volRange.value; volOut.textContent = `${volRange.value} %`; });
  const volumePanel = el("div", { class: "device-volume" },
    el("div", { class: "label" }, el("span", {}, "Device volume"),
      info("The speaker volume the device uses for every ▶ in this view, for that playback only: the volumes of " +
        "the configuration stay as they are. Starts at the ground volume.", "Device volume")),
    el("div", { class: "slider" }, volRange, volOut));

  main.replaceChildren(
    el("div", { class: "head" }, el("h2", {}, "Custom voicepack"),
      el("p", { class: "sub" }, "Voice clips from text-to-speech: single clips for the device, or a whole voice pack in the language of the device's pack.")),
    volumePanel, generationCard, shareCard, deviceCard, catalogueCard);
  if (deviceClient()) refreshDevice();
  state.cleanup = () => { genCancel = true; stopPlaying(); clearTimeout(liveTimer); };
}
