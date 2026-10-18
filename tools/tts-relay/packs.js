/*
 * Shared voice packs: their descriptions in SQLite (node:sqlite, no extra package) next to the
 * relay; the .ubpack files live with the TTS worker at home and pass through the relay in parts.
 * A pack is published by anyone at once and deleted by the operator only (/v1/admin/packs).
 *   config: {"settings": {pitch, rate, bandpass, lo, hi}, "overrides": {clip name: spoken text},
 *            "languages": {clip name: language the line is spoken in, when not the pack's},
 *            "variants": {clip name: the take chosen for the line, 1 to 99, when not the default one}}
 * The overrides are the pack's own spellings of catalogue texts (a model reads "Hike and Fly" right
 * only as "Haik änd Flai"); a pack used as the start of another passes them on.
 * A pack shared from a work (below) belongs to it: the work's owner shares it again in place - same id,
 * the new file under a new storage name ("file"; until its last part arrives it is "pending" and the old
 * file is still served) - and the old file goes.
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
  const variants = {};
  for (const [clip, v] of Object.entries(body.variants || {})) {
    if (!CLIP_RE.test(clip) || !Number.isInteger(v) || v < 1 || v > 99) throw new Error(`bad variant for clip ${clip}`);
    variants[clip] = v;
  }
  if (Object.keys(variants).length > 2000) throw new Error("too many variants");
  const clips = clamp(body.clips, 1, 5000, 0), size = clamp(body.size, 1, 2 ** 31, 0);
  return { name, lang, voice, voiceLabel: String(body.voice_label || voice).slice(0, 60), clips, size,
    base: PACK_ID_RE.test(body.base || "") ? body.base : null, config: { settings: cleanSettings(body.settings), overrides, languages, variants } };
}

/*
 * The work behind a device's voice pack (Custom voicepack): the voice, the audio settings and every clip the
 * configurator made for the device - its text, the language it is spoken in, the take, and whether it is in the
 * pack file or a single file in voices/custom - so that a clip added or changed later sounds like the rest.
 * Kept by id and not listed; whoever holds the owner key (the relay keeps only its hash) overwrites it, anyone
 * else starts a new work from it (its parent).
 *   data: {"settings": {pitch, rate, bandpass, lo, hi}, "clips": {clip name: {"t": text, "l": language, "v": take,
 *          "f": 1 = a single file}}, "built": when the pack file was last built, or null}
 */
export const WORK_KEY_RE = /^[a-f0-9]{48}$/;

export function cleanWork(body, maxChars) {
  const lang = String(body.lang || "");
  if (!LANG_RE.test(lang)) throw new Error("unknown pack language");
  const voice = String(body.voice || "");
  if (!voice || voice.length > 80) throw new Error("a work needs its voice");
  const clips = {};
  const textMax = Math.max(maxChars, 200);
  for (const [clip, c] of Object.entries(body.clips && typeof body.clips === "object" ? body.clips : {})) {
    if (!CLIP_RE.test(clip) || !c || typeof c !== "object") throw new Error(`bad clip ${clip}`);
    const t = String(c.t || "").replace(/\s+/g, " ").trim();
    if (!t || t.length > textMax) throw new Error(`bad text for clip ${clip}`);
    const e = { t };
    if (c.l !== undefined && c.l !== "") {
      if (!LANG_RE.test(String(c.l))) throw new Error(`bad language for clip ${clip}`);
      e.l = String(c.l);
    }
    if (c.v !== undefined && c.v !== 0) {
      if (!Number.isInteger(c.v) || c.v < 1 || c.v > 99) throw new Error(`bad variant for clip ${clip}`);
      e.v = c.v;
    }
    if (c.f) e.f = 1;
    clips[clip] = e;
  }
  if (Object.keys(clips).length > 3000) throw new Error("too many clips");
  const built = body.built && Number.isFinite(Date.parse(body.built)) ? new Date(body.built).toISOString() : null;
  return { lang, voice, locale: String(body.locale || "").slice(0, 20),
    name: String(body.name || "").replace(/[\u0000-\u001f\u007f]/g, "").replace(/\s+/g, " ").trim().slice(0, 40),
    parent: PACK_ID_RE.test(body.parent || "") ? body.parent : null,
    data: { settings: cleanSettings(body.settings), clips, built } };
}

const keyHash = (key) => crypto.createHash("sha256").update(String(key)).digest("hex");

export function openPacks(file) {
  fs.mkdirSync(path.dirname(path.resolve(file)), { recursive: true });
  const db = new DatabaseSync(file);
  db.exec(`CREATE TABLE IF NOT EXISTS packs (
    id TEXT PRIMARY KEY, lang TEXT NOT NULL, name TEXT NOT NULL, voice TEXT NOT NULL, voice_label TEXT NOT NULL,
    config TEXT NOT NULL, clips INTEGER NOT NULL, size INTEGER NOT NULL, sha256 TEXT, base TEXT,
    state TEXT NOT NULL, upload_token TEXT, created TEXT NOT NULL, downloads INTEGER NOT NULL DEFAULT 0)`);
  db.exec("CREATE UNIQUE INDEX IF NOT EXISTS packs_lang_name ON packs (lang, name COLLATE NOCASE)");
  /* 1.4: the work a pack belongs to, its file's storage name when not the id, a replacement being uploaded */
  const have = db.prepare("PRAGMA table_info(packs)").all().map((c) => c.name);
  for (const c of ["work", "file", "pending"]) if (!have.includes(c)) db.exec(`ALTER TABLE packs ADD COLUMN ${c} TEXT`);
  db.exec(`CREATE TABLE IF NOT EXISTS works (
    id TEXT PRIMARY KEY, owner TEXT NOT NULL, version INTEGER NOT NULL, lang TEXT NOT NULL, name TEXT NOT NULL,
    voice TEXT NOT NULL, locale TEXT NOT NULL, data TEXT NOT NULL, parent TEXT, pack TEXT,
    created TEXT NOT NULL, updated TEXT NOT NULL)`);
  const row = (r) => r && { id: r.id, lang: r.lang, name: r.name, voice: r.voice, voice_label: r.voice_label,
    clips: r.clips, size: r.size, base: r.base, created: r.created, downloads: r.downloads, state: r.state,
    work: r.work || null, file: r.file || r.id, pending: r.pending ? JSON.parse(r.pending) : null, ...JSON.parse(r.config) };
  const q = {
    list: db.prepare("SELECT * FROM packs WHERE state = 'ready' AND (? = '' OR lang = ?) ORDER BY created DESC"),
    all: db.prepare("SELECT * FROM packs ORDER BY created DESC"),
    get: db.prepare("SELECT * FROM packs WHERE id = ?"),
    named: db.prepare("SELECT id FROM packs WHERE lang = ? AND name = ? COLLATE NOCASE"),
    insert: db.prepare(`INSERT INTO packs (id, lang, name, voice, voice_label, config, clips, size, base, state, upload_token, created, work)
      VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, 'uploading', ?, ?, ?)`),
    ready: db.prepare("UPDATE packs SET state = 'ready', sha256 = ?, size = ?, upload_token = NULL WHERE id = ?"),
    pend: db.prepare("UPDATE packs SET pending = ?, upload_token = ? WHERE id = ?"),
    swap: db.prepare(`UPDATE packs SET name = ?, voice = ?, voice_label = ?, config = ?, clips = ?, size = ?, sha256 = ?, base = ?,
      file = ?, pending = NULL, upload_token = NULL WHERE id = ?`),
    unpend: db.prepare("UPDATE packs SET pending = NULL, upload_token = NULL WHERE id = ?"),
    pendings: db.prepare("SELECT * FROM packs WHERE pending IS NOT NULL"),
    counted: db.prepare("UPDATE packs SET downloads = downloads + 1 WHERE id = ?"),
    remove: db.prepare("DELETE FROM packs WHERE id = ?"),
    stale: db.prepare("SELECT id FROM packs WHERE state = 'uploading' AND created < ?"),
    count: db.prepare("SELECT COUNT(*) AS n FROM packs WHERE state = 'ready'"),
    workGet: db.prepare("SELECT * FROM works WHERE id = ?"),
    workInsert: db.prepare(`INSERT INTO works (id, owner, version, lang, name, voice, locale, data, parent, created, updated)
      VALUES (?, ?, 1, ?, ?, ?, ?, ?, ?, ?, ?)`),
    workUpdate: db.prepare(`UPDATE works SET version = version + 1, lang = ?, name = ?, voice = ?, locale = ?, data = ?, updated = ?
      WHERE id = ?`),
    workPack: db.prepare("UPDATE works SET pack = ? WHERE id = ?"),
    workCount: db.prepare("SELECT COUNT(*) AS n FROM works"),
  };
  const workRow = (r) => r && { id: r.id, version: r.version, lang: r.lang, name: r.name, voice: r.voice, locale: r.locale,
    parent: r.parent, pack: r.pack, created: r.created, updated: r.updated, ...JSON.parse(r.data) };
  const owns = (r, key) => {
    if (!r || !WORK_KEY_RE.test(String(key || ""))) return false;
    return crypto.timingSafeEqual(Buffer.from(keyHash(key)), Buffer.from(r.owner));
  };
  const works = {
    get: (id) => workRow(q.workGet.get(id)),
    owns: (id, key) => owns(q.workGet.get(id), key),
    /* a new work: {id, key, version}; the key is shown once, the relay keeps its hash */
    create(w) {
      const id = crypto.randomBytes(10).toString("hex");
      const key = crypto.randomBytes(24).toString("hex");
      const now = new Date().toISOString();
      q.workInsert.run(id, keyHash(key), w.lang, w.name, w.voice, w.locale, JSON.stringify(w.data), w.parent, now, now);
      return { id, key, version: 1 };
    },
    /* the owner overwrites it: the new version, 0 when there is no such work, -1 when the key is not its */
    update(id, key, w) {
      const r = q.workGet.get(id);
      if (!r) return 0;
      if (!owns(r, key)) return -1;
      q.workUpdate.run(w.lang, w.name, w.voice, w.locale, JSON.stringify(w.data), new Date().toISOString(), id);
      return r.version + 1;
    },
    setPack: (id, packId) => q.workPack.run(packId, id),
    count: () => q.workCount.get().n,
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
          new Date().toISOString(), p.work || null);
      } catch (e) {
        if (/UNIQUE/.test(e.message)) return null;
        throw e;
      }
      return { id, token };
    },
    /* a new file and description for a shared pack, waiting for its parts; the old file stays served until then.
     * null when another pack of the language has the name. */
    replace(id, p) {
      const other = q.named.get(p.lang, p.name);
      if (other && other.id !== id) return null;
      const token = crypto.randomBytes(24).toString("hex");
      const file = crypto.randomBytes(10).toString("hex");
      q.pend.run(JSON.stringify({ file, name: p.name, voice: p.voice, voice_label: p.voiceLabel, config: p.config,
        clips: p.clips, size: p.size, base: p.base, at: new Date().toISOString() }), token, id);
      return { id, token };
    },
    /* the replacement is complete: it becomes the pack; returns the storage name of the file it replaced */
    replaced(id, sha256, size) {
      const r = row(q.get.get(id));
      const p = r.pending;
      q.swap.run(p.name, p.voice, p.voice_label, JSON.stringify(p.config), p.clips, size, sha256, p.base, p.file, id);
      return r.file;
    },
    ready: (id, sha256, size) => q.ready.run(sha256, size, id),
    counted: (id) => q.counted.run(id),
    remove: (id) => q.remove.run(id).changes > 0,
    /* uploads that never finished, older than ms */
    stale: (ms) => q.stale.all(new Date(Date.now() - ms).toISOString()).map((r) => r.id),
    /* replacements that never finished, older than ms: [{id, file}] (dropped from the packs here) */
    stalePending(ms) {
      const out = [];
      for (const r of q.pendings.all()) {
        const p = JSON.parse(r.pending);
        if (Date.parse(p.at) < Date.now() - ms) {
          q.unpend.run(r.id);
          out.push({ id: r.id, file: p.file });
        }
      }
      return out;
    },
    count: () => q.count.get().n,
    works,
  };
}
