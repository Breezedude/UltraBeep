"use strict";

/*
 * Voice clips from text-to-speech: the relay (tools/tts-relay at TTS_URL) speaks a text, the
 * browser decodes it, filters and levels it like the stock clips and encodes it to QOA
 * (js/qoa.js). Used by the Voice view (js/voicegen.js: single clips, the profiles' voice lines,
 * whole packs). Language, voice, pitch, rate and the band-pass stay in this browser (localStorage)
 * and, once saved there, in the configuration on the device.
 */

const TTS_URL = "https://voice.breezedude.de";
const TTS_STORAGE = "cfg.tts";
const CLIP_RATE = 22050;
const CLIP_TARGET_RMS_DB = -13; /* loudest 200 ms of the stock clips: -13 dBFS rms, peaks about -2 dBFS */
const CLIP_PEAK_DB = -1.5;
const CLIP_MAX_S = 2.5; /* the device takes clips up to 24 kB over Bluetooth (about 9 kB per second) */
const CUSTOM_NAME_KEY = "identity_customname_string";
const VOICE_TEXT_KEY = "identity_voicetext_string";

let audioCtx = null;
let voiceCatalog = null; /* promise of the relay's voice list, fetched once per page */

/* The profile's voice line is the clip named here, in voices/custom; the configurator names it after the
 * profile ("profile_kode_p_16"), so it stays with the profile through reorders and renames. */
const VOICE_CLIP_KEY = "identity_voiceclip_string";
const VOICE_CLIP_MAX = 24;

const TTS_DEFAULTS = { locale: "", voice: "", pitch: 0, rate: 0, bandpass: true, lo: 300, hi: 8000 };

function ttsSettings() {
  try { return { ...TTS_DEFAULTS, ...JSON.parse(localStorage.getItem(TTS_STORAGE) || "{}") }; } catch (e) { return { ...TTS_DEFAULTS }; }
}

function saveTtsSettings(s) {
  const keep = {};
  for (const k of Object.keys(TTS_DEFAULTS)) keep[k] = s[k];
  try { localStorage.setItem(TTS_STORAGE, JSON.stringify(keep)); } catch (e) { /* page only */ }
}

/* The generator settings on the device (config_ext.ultrabip [global], written by "Save audio generator settings"
 * in the Voice view): every browser that opens the configuration then makes clips that sound the same. */
const TTS_KEYS = { locale: "voice_generator_locale_string", voice: "voice_generator_voice_string", pitch: "voice_generator_pitch_int",
  rate: "voice_generator_rate_int", bandpass: "voice_generator_bandpass_bool", lo: "voice_generator_bandlow_int",
  hi: "voice_generator_bandhigh_int" };

/* The settings stored in the document, or null when it has none (the voice is the marker). */
function ttsSettingsFromDoc(ext) {
  const get = (k) => (ext ? ext.get("global", TTS_KEYS[k]) : null);
  if (!get("voice")) return null;
  const int = (k, d) => { const v = parseInt(get(k), 10); return Number.isFinite(v) ? v : d; };
  return { locale: get("locale") || "", voice: get("voice"), pitch: int("pitch", 0), rate: int("rate", 0),
    bandpass: get("bandpass") !== "false", lo: int("lo", 300), hi: int("hi", 8000) };
}

function ttsSettingsToDoc(ext, s) {
  ext.set("global", TTS_KEYS.locale, s.locale || "");
  ext.set("global", TTS_KEYS.voice, s.voice || "");
  ext.set("global", TTS_KEYS.pitch, String(s.pitch | 0));
  ext.set("global", TTS_KEYS.rate, String(s.rate | 0));
  ext.set("global", TTS_KEYS.bandpass, s.bandpass ? "true" : "false");
  ext.set("global", TTS_KEYS.lo, String(s.lo | 0));
  ext.set("global", TTS_KEYS.hi, String(s.hi | 0));
}

/* ---- the service ---- */

/* fetch() fails with a bare TypeError when the browser's CORS preflight was refused or redirected. */
async function fetchService(url, init) {
  try {
    return await fetch(url, init);
  } catch (e) {
    throw new Error(`Voice service: the browser could not talk to ${url} (${e.message}). The service must answer ` +
      "the browser's OPTIONS request itself with CORS headers; a redirect (http → https, www, a trailing slash) " +
      "or a proxy in front that redirects breaks it. Check the address, e.g. https://voice.breezedude.de/health.");
  }
}

/* The MP3 of `text` in `voice`; pitch and rate in percent (the relay's prosody). */
async function fetchTts(voice, text, opts = {}) {
  const res = await fetchService(`${TTS_URL}/v1/audio/speech`, {
    method: "POST", headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ model: "tts-1", input: text, voice, response_format: "mp3",
      pitch: Math.round(opts.pitch || 0), rate: Math.round(opts.rate || 0) }),
  });
  if (!res.ok) {
    let msg = `HTTP ${res.status}`;
    try { msg = (await res.json()).error.message; } catch (e) { /* no JSON body */ }
    const err = new Error(`Voice service: ${msg}`);
    err.status = res.status;
    throw err;
  }
  return res.arrayBuffer();
}

/* All voices of the relay, once per page: [{name, locale, gender, friendly}]. */
function fetchVoices() {
  if (!voiceCatalog) {
    voiceCatalog = fetchService(`${TTS_URL}/v1/voices`, {}).then(async (res) => {
      if (!res.ok) throw new Error(`Voice service: HTTP ${res.status}`);
      return (await res.json()).voices;
    }).catch((e) => { voiceCatalog = null; throw e; });
  }
  return voiceCatalog;
}

/* "German (Austria)" for de-AT, from the browser's own names. */
function localeLabel(locale) {
  try {
    const [lang, region] = locale.split("-");
    const l = new Intl.DisplayNames(["en"], { type: "language" }).of(lang);
    const r = region ? new Intl.DisplayNames(["en"], { type: "region" }).of(region) : "";
    return r ? `${l} (${r})` : l;
  } catch (e) {
    return locale;
  }
}

/* The voice pack of the configuration (js/voicepack.js voicepackInfo): its language and preferred voice locale. */
function currentVoicepack() {
  return voicepackInfo(systemValue("system_language_string") || "de");
}

/*
 * Language and voice selects filled from the relay's list, limited to the pack's language and
 * English (the device strings the clips together with the pack's); the choice is saved in
 * `settings`. Returns {langSel, voiceSel, note, ready}; onError(message) when the list cannot be loaded.
 */
function voicePicker(settings, pack, onError) {
  const langSel = el("select", { "aria-label": "Language" });
  const voiceSel = el("select", { "aria-label": "Voice" });
  const note = el("span", { class: "unit" }, "loading the voices …");
  let voices = [];
  /* the "multilingual" voices often mispronounce the language's own words: listed last, never the default
   * (they are a fallback for English terms like "Hike and Fly") */
  const multilingual = (v) => /multilingual/i.test(v.name);
  const fillVoices = () => {
    const list = voices.filter((v) => v.locale === langSel.value).sort((a, b) => multilingual(a) - multilingual(b));
    voiceSel.replaceChildren(...list.map((v) => el("option", { value: v.name },
      `${v.friendly || v.name} (${v.gender.toLowerCase()}${multilingual(v) ? ", multilingual" : ""})`)));
    if (list.some((v) => v.name === settings.voice)) voiceSel.value = settings.voice;
    settings.locale = langSel.value;
    settings.voice = voiceSel.value;
    saveTtsSettings(settings);
  };
  const ready = fetchVoices().then((list) => {
    const language = (pack.language || "").toLowerCase();
    /* the pack's language in every region, English as US and UK only (a dozen more regions just
     * clutter the list), and the pack's own locale whatever it is */
    const fits = (l) => {
      const lower = l.toLowerCase(), lang = lower.split("-")[0];
      if (lower === (pack.tts || "").toLowerCase()) return true;
      if (lang === "en") return lower === "en-us" || lower === "en-gb";
      return lang === language;
    };
    voices = list.filter((v) => fits(v.locale));
    const locales = [...new Set(voices.map((v) => v.locale))].sort((a, b) => localeLabel(a).localeCompare(localeLabel(b)));
    langSel.replaceChildren(...locales.map((l) => el("option", { value: l }, localeLabel(l))));
    const lower = (s) => (s || "").toLowerCase();
    /* the saved choice stays while it speaks the pack's language; a new pack starts with its own voices */
    const saved = settings.locale && locales.includes(settings.locale) && lower(settings.locale).split("-")[0] === language ? settings.locale : null;
    const wanted = saved || locales.find((l) => lower(l) === lower(pack.tts)) || locales.find((l) => lower(l).startsWith(language)) || locales[0];
    langSel.value = wanted;
    const own = locales.filter((l) => l.split("-")[0].toLowerCase() === language).length;
    note.textContent = language === "en" ? `${voices.length} English voices in ${locales.length} regions`
      : own ? `${voices.length} voices: ${pack.label.replace(/ \(.*\)$/, "")} and English`
        : `no ${pack.label} voices at the service; English only`;
    fillVoices();
  }).catch((e) => {
    note.textContent = "";
    if (onError) onError(e.message);
  });
  langSel.addEventListener("change", fillVoices);
  voiceSel.addEventListener("change", () => { settings.voice = voiceSel.value; saveTtsSettings(settings); });
  return { langSel, voiceSel, note, ready };
}

/* ---- the clip ---- */

/*
 * The clip pipeline. The service's audio is decoded once to raw PCM (mono 22.05 kHz, silence
 * trimmed) and kept; pitch, rate and the band-pass are applied in the browser from that raw
 * audio - so a change of the sliders is heard at once and the clip that goes to the device is
 * exactly what was heard. The whole chain runs in the browser; nothing but the text leaves it.
 */

/* MP3 (or WAV) -> Float32 mono 22.05 kHz, trimmed below -45 dBFS at both ends (40 ms kept). */
async function decodeRaw(encoded, maxS = CLIP_MAX_S) {
  const probe = new OfflineAudioContext(1, 1, CLIP_RATE);
  const decoded = await probe.decodeAudioData(encoded.slice(0));
  const length = Math.min(Math.ceil(decoded.duration * CLIP_RATE), Math.ceil(maxS * 1.6 * CLIP_RATE));
  const off = new OfflineAudioContext(1, length, CLIP_RATE);
  const src = off.createBufferSource();
  src.buffer = decoded;
  src.connect(off.destination);
  src.start();
  const x = (await off.startRendering()).getChannelData(0);
  const thr = Math.pow(10, -45 / 20);
  let a = 0, b = x.length;
  while (a < b && Math.abs(x[a]) < thr) a++;
  while (b > a && Math.abs(x[b - 1]) < thr) b--;
  const pad = Math.round(0.04 * CLIP_RATE);
  const raw = x.slice(Math.max(0, a - pad), Math.min(x.length, b + pad));
  if (raw.length < CLIP_RATE / 10) throw new Error("The voice service returned silence.");
  return raw;
}

/* ---- DSP: time stretch (WSOLA), resampling, biquads ---- */

/* Stretches x to factor times its length without changing the pitch: overlap-add of 25 ms grains,
 * each aligned (within 6 ms) to the natural continuation of the previous one. */
function timeStretch(x, factor) {
  if (Math.abs(factor - 1) < 0.005) return x;
  const win = Math.round(0.025 * CLIP_RATE), hop = win >> 1, search = Math.round(0.006 * CLIP_RATE);
  const w = new Float32Array(win);
  for (let i = 0; i < win; i++) w[i] = 0.5 - 0.5 * Math.cos((2 * Math.PI * i) / win);
  const outLen = Math.round(x.length * factor);
  const y = new Float32Array(outLen + win);
  let prev = -1;
  for (let out = 0; out < outLen; out += hop) {
    const nominal = Math.round(out / factor);
    let start = Math.min(nominal, x.length - win);
    if (start < 0) break;
    if (prev >= 0) {
      /* the continuation the ear expects: the previous grain shifted by one hop */
      const ref = prev + hop;
      let best = -Infinity, bestStart = start;
      for (let d = -search; d <= search; d += 2) {
        const s = start + d;
        if (s < 0 || s + win > x.length || ref + win > x.length) continue;
        let c = 0;
        for (let i = 0; i < win; i += 2) c += x[s + i] * x[ref + i];
        if (c > best) { best = c; bestStart = s; }
      }
      start = bestStart;
    }
    for (let i = 0; i < win && out + i < y.length; i++) y[out + i] += x[start + i] * w[i];
    prev = start;
  }
  return y.subarray(0, outLen);
}

/* Resamples by `ratio` (output length = input length / ratio): ratio 1.2 raises the pitch by 20 %. */
function resampleBy(x, ratio) {
  if (Math.abs(ratio - 1) < 0.005) return x;
  const n = Math.floor(x.length / ratio);
  const y = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    const p = i * ratio, k = Math.floor(p), f = p - k;
    y[i] = x[k] * (1 - f) + (x[k + 1] || 0) * f;
  }
  return y;
}

/* Second-order Butterworth high- or low-pass, applied twice (fourth order), in place. */
function biquad(x, type, fc) {
  const w0 = (2 * Math.PI * fc) / CLIP_RATE, cosw = Math.cos(w0), alpha = Math.sin(w0) / (2 * Math.SQRT1_2);
  const a0 = 1 + alpha;
  const b0 = type === "lp" ? (1 - cosw) / 2 / a0 : (1 + cosw) / 2 / a0;
  const b1 = type === "lp" ? (1 - cosw) / a0 : -(1 + cosw) / a0;
  const b2 = b0;
  const a1 = (-2 * cosw) / a0, a2 = (1 - alpha) / a0;
  for (let pass = 0; pass < 2; pass++) {
    let x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    for (let i = 0; i < x.length; i++) {
      const v = x[i];
      const y = b0 * v + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
      x2 = x1; x1 = v; y2 = y1; y1 = y;
      x[i] = y;
    }
  }
  return x;
}

/* Pitch, rate and band-pass of the settings applied to raw audio: a new Float32Array. */
function processRaw(raw, settings) {
  const pitch = 1 + (settings.pitch || 0) / 100;  /* frequency factor */
  const rate = 1 + (settings.rate || 0) / 100;    /* speed factor */
  let x = timeStretch(raw, pitch / rate);         /* longer by the pitch factor, shorter by the rate */
  x = resampleBy(x, pitch);                        /* back to the rate's length with the pitch raised */
  if (x === raw) x = raw.slice();
  if (settings.bandpass) {
    biquad(x, "hp", Math.max(20, Math.min(2000, settings.lo || 300)));
    biquad(x, "lp", Math.max(1000, Math.min(11000, settings.hi || 8000)));
  }
  return x;
}

/* Levels like the stock clips (the loudest 200 ms to -13 dBFS rms, vol scales it, peak limited)
 * and encodes: {bytes, seconds, gainDb}. */
function encodeClip(x, vol = 1, maxS = CLIP_MAX_S) {
  if (x.length > maxS * CLIP_RATE) x = x.subarray(0, Math.floor(maxS * CLIP_RATE));
  let peak = 0;
  for (let i = 0; i < x.length; i++) peak = Math.max(peak, Math.abs(x[i]));
  const w = Math.round(0.2 * CLIP_RATE);
  let acc = 0, best = 0;
  for (let i = 0; i < x.length; i++) {
    acc += x[i] * x[i];
    if (i >= w) acc -= x[i - w] * x[i - w];
    if (i >= w - 1 && acc > best) best = acc;
  }
  const rms = Math.sqrt(best / Math.min(w, x.length)) || 1e-6;
  const target = Math.pow(10, CLIP_TARGET_RMS_DB / 20) * (vol || 1);
  const gain = Math.min(target / rms, Math.pow(10, CLIP_PEAK_DB / 20) / (peak || 1e-6));
  const pcm = new Int16Array(x.length);
  for (let i = 0; i < x.length; i++) pcm[i] = Math.max(-32768, Math.min(32767, Math.round(x[i] * gain * 32767)));
  return { bytes: qoaEncode(pcm, CLIP_RATE), seconds: x.length / CLIP_RATE, gainDb: 20 * Math.log10(gain) };
}

/* A clip entry {raw, vol, maxS, text} built with the settings: {bytes, seconds, gainDb}, cached per settings. */
function buildClip(entry, settings) {
  const sig = `${settings.pitch}|${settings.rate}|${settings.bandpass ? `${settings.lo}-${settings.hi}` : "off"}|${entry.vol}|${entry.maxS}`;
  if (!entry.built || entry.built.sig !== sig) {
    entry.built = { sig, ...encodeClip(processRaw(entry.raw, settings), entry.vol, entry.maxS) };
  }
  return entry.built;
}

/* Speaks `text` and keeps the raw audio; the returned entry also holds the clip built with the settings. */
async function generateClip(text, settings, vol = 1, maxS = CLIP_MAX_S) {
  const mp3 = await fetchTts(settings.voice, text, { pitch: 0, rate: 0 }); /* prosody is applied here, live */
  const entry = { raw: await decodeRaw(mp3, maxS), vol, maxS, text };
  const b = buildClip(entry, settings);
  return Object.assign(entry, { bytes: b.bytes, seconds: b.seconds, gainDb: b.gainDb });
}

/* The clip of an entry with the current settings (rebuilt when they changed since). */
function clipBytes(entry) {
  const b = buildClip(entry, ttsSettings());
  entry.bytes = b.bytes;
  entry.seconds = b.seconds;
  entry.gainDb = b.gainDb;
  return b.bytes;
}

/* Plays clips as the device will: decoded from the QOA bytes, one after the other with a short gap. */
let playing = [];
function playClips(list) {
  if (!audioCtx) audioCtx = new (window.AudioContext || window.webkitAudioContext)();
  stopPlaying();
  let at = audioCtx.currentTime + 0.05;
  for (const bytes of list) {
    const { sampleRate, samples } = qoaDecode(bytes);
    const buf = audioCtx.createBuffer(1, samples.length, sampleRate);
    const ch = buf.getChannelData(0);
    for (let i = 0; i < samples.length; i++) ch[i] = samples[i] / 32768;
    const src = audioCtx.createBufferSource();
    src.buffer = buf;
    src.connect(audioCtx.destination);
    src.start(at);
    playing.push(src);
    at += buf.duration + 0.2;
  }
}

function stopPlaying() {
  for (const s of playing) { try { s.stop(); } catch (e) { /* already ended */ } }
  playing = [];
}

const playClip = (bytes) => playClips([bytes]);

/* ---- the profile's voice line (custom profile name) ---- */

/* The Identity card: the type select gets "custom name", which opens the name and the voice line. */
function identityFieldset(n, g) {
  const custom = (getP(n, CUSTOM_NAME_KEY, true) || "") !== "";
  const typeField = g.fields.find((f) => f.key === "identity_profilename_string");
  const typeSelect = { ...typeField, options: [...typeField.options, ["custom", "Custom name (own voice line)"]],
    help: typeField.help + " \"Custom name\": your own name for the profile - e.g. the glider - spoken by a " +
      "voice line you generate below." };
  let card;
  const rerender = () => card.replaceWith(identityFieldset(n, g));
  const typeRow = row(typeSelect, custom ? "custom" : getP(n, "identity_profilename_string"), (v) => {
    if (v === "custom") {
      setP(n, CUSTOM_NAME_KEY, getP(n, "identity_wingname_string") || `Profile ${n}`, true);
    } else {
      setP(n, CUSTOM_NAME_KEY, "", true);
      setP(n, "identity_profilename_string", v);
    }
    renderNav();
    rerender();
  });
  const rows = [typeRow];
  if (custom) {
    rows.push(row({ key: CUSTOM_NAME_KEY, label: "Custom name", type: "text",
      help: "Shown in the profile list. The device speaks the voice line below after \"select profile\"; " +
        "without a clip on the device it says the type chosen before." },
    getP(n, CUSTOM_NAME_KEY, true), (v) => { setP(n, CUSTOM_NAME_KEY, v, true); renderNav(); }));
  }
  for (const f of g.fields) {
    if (f === typeField) continue;
    rows.push(row(f, getP(n, f.key), (v) => { setP(n, f.key, v); renderNav(); }));
  }
  card = fieldset({ title: g.title, help: g.help }, rows, custom ? voiceLineRow(n) : null);
  return card;
}

/* A clip name for a profile's voice line: "profile_" + the custom name in plain letters, unique among the profiles. */
function voiceClipNameFor(n) {
  const taken = new Set();
  for (let i = 1; i <= PROFILE_MAX; i++) {
    if (i !== n && profileExists(i)) {
      const c = getP(i, VOICE_CLIP_KEY, true);
      if (c) taken.add(c);
    }
  }
  const plain = (getP(n, CUSTOM_NAME_KEY, true) || "").toLowerCase().normalize("NFD").replace(/[\u0300-\u036f]/g, "")
    .replace(/ß/g, "ss").replace(/[^a-z0-9]+/g, "_").replace(/^_+|_+$/g, "");
  const base = `profile_${plain || `custom_${n}`}`.slice(0, VOICE_CLIP_MAX);
  let name = base;
  for (let k = 2; taken.has(name); k++) name = `${base.slice(0, VOICE_CLIP_MAX - 1 - String(k).length)}_${k}`;
  return name;
}

/* The profile's voice line: which clip, whether the device has it, and the way to the generator. */
function voiceLineRow(n) {
  const clip = getP(n, VOICE_CLIP_KEY, true) || "";
  const status = el("span", { class: "unit" }, clip ? `${clip}.qoa` : "none yet");
  const btn = el("button", { type: "button", class: clip ? "" : "primary",
    onclick: () => { state.voiceLineFor = n; state.view = "voice"; render(); } },
  clip ? "Make it again in the voice generator" : "Make it in the voice generator");
  const r = customRow("Voice line", el("div", { class: "voiceline" }, status, btn),
    "The clip the device says after \"select profile\". It is named after the profile and stays with it when " +
    "profiles are reordered or renamed; make it again when the name changes, so the spoken words match.");
  const client = deviceClient();
  if (clip && client) {
    client.listTracks("voices/custom").then((list) => {
      status.textContent = `${clip}.qoa - ${list.some((f) => f.name === `${clip}.qoa`) ? "on the device" : "not on the device yet"}`;
    }).catch(() => { /* older firmware: no listing */ });
  }
  return r;
}

/* A download of bytes under a file name. */
function downloadBytes(name, bytes) {
  const url = URL.createObjectURL(new Blob([bytes], { type: "application/octet-stream" }));
  const a = el("a", { href: url, download: name });
  document.body.append(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}
