/*
 * Shared voice packs: their descriptions in SQLite (node:sqlite, no extra package) next to the
 * relay; the .ubpack files live with the TTS worker at home and pass through the relay in parts.
 * A pack is published by anyone at once and deleted by the operator only (/v1/admin/packs).
 *   config: {"settings": {pitch, rate, bandpass, lo, hi}, "overrides": {clip name: spoken text},
 *            "languages": {clip name: language the line is spoken in, when not the pack's}}
 * The overrides are the pack's own spellings of catalogue texts (a model reads "Hike and Fly" right
 * only as "Haik änd Flai"); a pack used as the start of another passes them on.
 */
import crypto from "node:crypto";
import fs from "node:fs";
import path from "node:path";
import { DatabaseSync } from "node:sqlite";

export const PACK_ID_RE = /^[a-z0-9]{20}$/;
const CLIP_RE = /^([a-z0-9_]{1,8}\/)?[a-z0-9_]{1,24}$/;
const LANG_RE = /^[a-z]{2}$/;

const clamp = (v, lo, hi, d) => { const n = Math.round(Number(v)); return Number.isFinite(n) ? Math.max(lo, Math.min(hi, n)) : d; };

/* The audio settings a pack was made with (applied in the browser, js/voice.js processRaw). */
export function cleanSettings(s = {}) {
  return { pitch: clamp(s.pitch, -50, 50, 0), rate: clamp(s.rate, -50, 50, 0), bandpass: s.bandpass !== false,
    lo: clamp(s.lo, 20, 2000, 300), hi: clamp(s.hi, 1000, 11000, 8000) };
}

/* A pack description from a browser, checked; throws Error(message) for the user. */
export function cleanPack(body, maxChars) {
  const name = String(body.name || "").replace(/\s+/g, " ").trim();
  if (name.length < 2 || name.length > 40 || /[\u0000-\u001f\u007f]/.test(name)) throw new Error("the name needs 2 to 40 characters");
  const lang = String(body.lang || "");
  if (!LANG_RE.test(lang)) throw new Error("unknown pack language");
  const voice = String(body.voice || "");
  if (!voice || voice.length > 80 || voice.includes("-own-")) throw new Error("a pack needs a voice others can use too (no own voice)");
  const overrides = {};
  for (const [clip, text] of Object.entries(body.overrides || {})) {
    const t = String(text).replace(/\s+/g, " ").trim();
    if (!CLIP_RE.test(clip) || !t || t.length > maxChars) throw new Error(`bad text for clip ${clip}`);
    overrides[clip] = t;
  }
  if (Object.keys(overrides).length > 2000) throw new Error("too many changed texts");
  const languages = {};
  for (const [clip, lang] of Object.entries(body.languages || {})) {
    if (!CLIP_RE.test(clip) || !/^[a-z]{2}$/.test(String(lang))) throw new Error(`bad language for clip ${clip}`);
    languages[clip] = String(lang);
  }
  if (Object.keys(languages).length > 2000) throw new Error("too many languages");
  const clips = clamp(body.clips, 1, 5000, 0), size = clamp(body.size, 1, 2 ** 31, 0);
  return { name, lang, voice, voiceLabel: String(body.voice_label || voice).slice(0, 60), clips, size,
    base: PACK_ID_RE.test(body.base || "") ? body.base : null, config: { settings: cleanSettings(body.settings), overrides, languages } };
}

export function openPacks(file) {
  fs.mkdirSync(path.dirname(path.resolve(file)), { recursive: true });
  const db = new DatabaseSync(file);
  db.exec(`CREATE TABLE IF NOT EXISTS packs (
    id TEXT PRIMARY KEY, lang TEXT NOT NULL, name TEXT NOT NULL, voice TEXT NOT NULL, voice_label TEXT NOT NULL,
    config TEXT NOT NULL, clips INTEGER NOT NULL, size INTEGER NOT NULL, sha256 TEXT, base TEXT,
    state TEXT NOT NULL, upload_token TEXT, created TEXT NOT NULL, downloads INTEGER NOT NULL DEFAULT 0)`);
  db.exec("CREATE UNIQUE INDEX IF NOT EXISTS packs_lang_name ON packs (lang, name COLLATE NOCASE)");
  const row = (r) => r && { id: r.id, lang: r.lang, name: r.name, voice: r.voice, voice_label: r.voice_label,
    clips: r.clips, size: r.size, base: r.base, created: r.created, downloads: r.downloads, state: r.state,
    ...JSON.parse(r.config) };
  const q = {
    list: db.prepare("SELECT * FROM packs WHERE state = 'ready' AND (? = '' OR lang = ?) ORDER BY created DESC"),
    all: db.prepare("SELECT * FROM packs ORDER BY created DESC"),
    get: db.prepare("SELECT * FROM packs WHERE id = ?"),
    insert: db.prepare(`INSERT INTO packs (id, lang, name, voice, voice_label, config, clips, size, base, state, upload_token, created)
      VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, 'uploading', ?, ?)`),
    ready: db.prepare("UPDATE packs SET state = 'ready', sha256 = ?, size = ?, upload_token = NULL WHERE id = ?"),
    counted: db.prepare("UPDATE packs SET downloads = downloads + 1 WHERE id = ?"),
    remove: db.prepare("DELETE FROM packs WHERE id = ?"),
    stale: db.prepare("SELECT id FROM packs WHERE state = 'uploading' AND created < ?"),
    count: db.prepare("SELECT COUNT(*) AS n FROM packs WHERE state = 'ready'"),
  };
  return {
    list: (lang = "") => q.list.all(lang, lang).map(row),
    all: () => q.all.all().map(row),
    get: (id) => row(q.get.get(id)),
    token: (id) => q.get.get(id)?.upload_token || null,
    /* a new pack waiting for its file; null when the name is taken in that language */
    create(p) {
      const id = crypto.randomBytes(10).toString("hex");
      const token = crypto.randomBytes(24).toString("hex");
      try {
        q.insert.run(id, p.lang, p.name, p.voice, p.voiceLabel, JSON.stringify(p.config), p.clips, p.size, p.base, token,
          new Date().toISOString());
      } catch (e) {
        if (/UNIQUE/.test(e.message)) return null;
        throw e;
      }
      return { id, token };
    },
    ready: (id, sha256, size) => q.ready.run(sha256, size, id),
    counted: (id) => q.counted.run(id),
    remove: (id) => q.remove.run(id).changes > 0,
    /* uploads that never finished, older than ms */
    stale: (ms) => q.stale.all(new Date(Date.now() - ms).toISOString()).map((r) => r.id),
    count: () => q.count.get().n,
  };
}
