/*
 * TTS relay for the UltraBeep configurator: a small HTTP server that turns a few
 * words into MP3 with Microsoft Edge's text-to-speech (free, no account), which browsers
 * cannot call directly, and with the voices of TTS workers at home (tools/tts-worker,
 * Chatterbox), which connect out to the relay over an authenticated WebSocket.
 * OpenAI-compatible request shape, so the configurator can also talk to other services:
 *   POST   /v1/audio/speech   {"input": "Ozone Rush", "voice": "de-DE-KatjaNeural"}  -> audio/mpeg
 *   GET    /v1/voices?locale=de                                                        -> JSON list
 *   POST   /v1/voices/own?consent=yes  (16-bit mono WAV)  -> {"id"}: a private cloned voice
 *   DELETE /v1/voices/own/<id>                                     -> {"deleted"}
 *   POST   /v1/voices/public  {"voice_id", "name", "locale", ...}  -> {"voice"}: an own voice for everyone
 *   GET    /v1/packs?lang=de, GET /v1/packs/<id>, GET /v1/packs/<id>/file   shared voice packs
 *   POST   /v1/packs, PUT /v1/packs/<id>/file?part=&parts=          publishing one (packs.js)
 *   GET|DELETE /v1/admin/packs[/<id>]  (Authorization: Bearer ADMIN_TOKEN)   the operator's
 *   GET    /v1/worker  (WebSocket, Authorization: Bearer WORKER_SECRET): a worker's connection
 *   GET    /health
 * Settings come from .env (see .env.example); run behind nginx (nginx.conf.example) with pm2
 * (ecosystem.config.cjs). Abuse limits: text length, requests per IP and window, voice filter.
 */
import crypto from "node:crypto";
import { once } from "node:events";
import http from "node:http";
import msedge from "msedge-tts"; /* the Edge read-aloud protocol, maintained there (token scheme, versions) */
import { WebSocketServer } from "ws";
import { PACK_ID_RE, cleanPack, openPacks } from "./packs.js";

const { MsEdgeTTS, OUTPUT_FORMAT } = msedge;

try { process.loadEnvFile(".env"); } catch (e) { /* no .env: defaults */ }

const cfg = {
  host: process.env.HOST || "127.0.0.1",
  port: parseInt(process.env.PORT || "5055", 10),
  maxChars: parseInt(process.env.MAX_CHARS || "80", 10),
  rateLimit: parseInt(process.env.RATE_LIMIT || "30", 10),
  rateWindowS: parseInt(process.env.RATE_WINDOW_S || "600", 10),
  allowedOrigins: (process.env.ALLOWED_ORIGINS || "*").split(",").map((s) => s.trim()).filter(Boolean),
  trustProxy: process.env.TRUST_PROXY === "1",
  cacheEntries: parseInt(process.env.CACHE_ENTRIES || "300", 10),
  voiceFilter: process.env.VOICE_FILTER ? new RegExp(process.env.VOICE_FILTER) : null,
  defaultVoice: process.env.DEFAULT_VOICE || "de-DE-KatjaNeural",
  upstreamTimeoutMs: parseInt(process.env.UPSTREAM_TIMEOUT_MS || "15000", 10),
  edge: process.env.EDGE !== "0",
  workerSecret: process.env.WORKER_SECRET || "", /* empty: no worker may connect */
  /* a request waits this long for a worker clip, then gets 503 + Retry-After (nginx gives up at 30 s);
   * the clip is made anyway and served from the cache when the client asks again */
  workerWaitMs: parseInt(process.env.WORKER_WAIT_MS || "25000", 10),
  workerJobTimeoutMs: parseInt(process.env.WORKER_JOB_TIMEOUT_MS || "180000", 10),
  workerQueueMax: parseInt(process.env.WORKER_QUEUE_MAX || "300", 10),
  ownMaxBytes: parseInt(process.env.OWN_VOICE_MAX_BYTES || "2000000", 10),
  ownLimit: parseInt(process.env.OWN_VOICE_LIMIT || "10", 10), /* uploads per IP and day */
  templateLimit: parseInt(process.env.TEMPLATE_LIMIT || "5", 10), /* public voices per IP and day */
  packLimit: parseInt(process.env.PACK_LIMIT || "5", 10),         /* shared packs per IP and day */
  packMaxBytes: parseInt(process.env.PACK_MAX_BYTES || String(40 * 2 ** 20), 10),
  packPartBytes: 2 ** 20,
  dbPath: process.env.DB_PATH || "data/relay.db",
  adminToken: process.env.ADMIN_TOKEN || "", /* empty: no admin API */
};
const packs = openPacks(cfg.dbPath);

/* ---------------------------------------------------------------- Edge TTS */

const TOKEN = "6A5AA1D4EAFF4E9FB37E23D68491D6F4";
const VOICES_URL = "https://speech.platform.bing.com/consumer/speech/synthesize/readaloud/voices/list";
const HEADERS = {
  "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/143.0.0.0 Safari/537.36 Edg/143.0.0.0",
  "Accept-Language": "en-US,en;q=0.9",
};
const ENGINE_IDLE_MS = 5 * 60 * 1000;

const escapeXml = (s) => s.replace(/[<>&'"]/g, (c) => ({ "<": "&lt;", ">": "&gt;", "&": "&amp;", "'": "&apos;", "\"": "&quot;" }[c]));

/*
 * One connection per voice, kept open and shared by every clip of that voice (msedge-tts multiplexes
 * requests on it): a whole pack of about 1000 clips needs one handshake instead of one each, which
 * matters since Edge refuses a share of the handshakes (403) now and then. Idle connections close
 * after ENGINE_IDLE_MS; a failed one is dropped so the next request connects afresh.
 */
const engines = new Map(); /* voice -> {tts, ready, last} */

function engineFor(voice) {
  let e = engines.get(voice);
  if (!e) {
    const tts = new MsEdgeTTS({ connectionTimeoutMs: 10000, synthesisTimeoutMs: cfg.upstreamTimeoutMs });
    e = { tts, last: Date.now(), ready: tts.setMetadata(voice, OUTPUT_FORMAT.AUDIO_24KHZ_48KBITRATE_MONO_MP3) };
    e.ready.catch(() => dropEngine(voice, e));
    engines.set(voice, e);
  }
  e.last = Date.now();
  return e;
}

function dropEngine(voice, e) {
  if (engines.get(voice) === e) engines.delete(voice);
  try { e.tts.close(); } catch (err) { /* already closed */ }
}

setInterval(() => {
  const now = Date.now();
  for (const [voice, e] of engines) if (now - e.last > ENGINE_IDLE_MS) dropEngine(voice, e);
}, 60 * 1000).unref();

/* Speaks `text` with `voice`; pitch and rate in percent (-50..50); resolves to the MP3 bytes. */
async function synthesize(text, voice, pitch = 0, rate = 0) {
  const e = engineFor(voice);
  try {
    await e.ready;
  } catch (err) {
    throw new Error(`upstream: ${err.message}`);
  }
  const pct = (v) => `${v >= 0 ? "+" : ""}${v}%`;
  return new Promise((resolve, reject) => {
    const chunks = [];
    let stream;
    try {
      stream = e.tts.toStream(escapeXml(text), { pitch: pct(pitch), rate: pct(rate), volume: "+0%" }).audioStream;
    } catch (err) {
      dropEngine(voice, e);
      return reject(new Error(`upstream: ${err.message}`));
    }
    stream.on("data", (d) => chunks.push(d));
    stream.on("error", (err) => { dropEngine(voice, e); reject(new Error(`upstream: ${err.message}`)); });
    stream.on("close", () => {
      if (chunks.length) resolve(Buffer.concat(chunks));
      else { dropEngine(voice, e); reject(new Error("no audio from upstream")); }
    });
  });
}

let voicesCache = { at: 0, list: [] };
async function voices() {
  if (Date.now() - voicesCache.at < 24 * 3600 * 1000 && voicesCache.list.length) return voicesCache.list;
  const res = await fetch(`${VOICES_URL}?trustedclienttoken=${TOKEN}`, { headers: HEADERS });
  if (!res.ok) throw new Error(`voices: ${res.status}`);
  const all = await res.json();
  voicesCache = { at: Date.now(), list: all.map((v) => ({ name: v.ShortName, locale: v.Locale, gender: v.Gender,
    friendly: (v.FriendlyName || "").replace(/^Microsoft /, "").replace(/ Online \(Natural\).*$/, "") })) };
  return voicesCache.list;
}

/* ---------------------------------------------------------------- home workers */

/*
 * A worker (tools/tts-worker) connects with the shared secret and announces its voices; the relay
 * lists them next to Edge's while it is connected and hands it jobs, at most `slots` at a time.
 * Its voice names carry the provider: "cb:de-DE-florian" (public), "cb:de-DE-own-<id>" (a private
 * voice someone uploaded: never listed, the id is the key). Requests for the same clip share one job;
 * single clips go before the bulk of a whole pack. A job whose worker drops goes back to the queue.
 */
const WORKER_VOICE_RE = /^cb:([a-z]{2,3})-[A-Z]{2}-(?:own-[a-z2-7]{26}|[a-z0-9]+(?:-[a-z0-9]+)*)$/;
const OWN_ID_RE = /^[a-z2-7]{26}$/;
const workers = new Set(); /* {ws, name, slots, inflight: Map id -> job, voices, languages: Set, seconds} */
const jobs = new Map();    /* speech key -> job, while it is queued or running (shared by its requests) */
const queues = { interactive: [], bulk: [] };

const httpError = (status, message, extra = {}) => Object.assign(new Error(message), { status }, extra);

function workerVoices() {
  const seen = new Set(), out = [];
  for (const w of workers) for (const v of w.voices) {
    if (seen.has(v.name)) continue;
    seen.add(v.name);
    const d = v.defaults && typeof v.defaults === "object" ? v.defaults : null;
    out.push({ name: v.name, locale: v.locale, gender: v.gender || "Unknown", friendly: String(v.friendly || v.name).slice(0, 40),
      provider: "chatterbox", seconds: v.seconds || w.seconds,
      ...(d ? { defaults: { pitch: d.pitch | 0, rate: d.rate | 0, bandpass: d.bandpass !== false, lo: d.lo | 0 || 300, hi: d.hi | 0 || 8000 } } : {}) });
  }
  return out;
}

/* Chatterbox settings a speech request may try (the configurator's Custom speaker page). */
function trialSettings(s) {
  if (!s || typeof s !== "object") return null;
  const out = {};
  for (const [k, lo, hi] of [["exaggeration", 0.25, 2], ["cfg_weight", 0, 1], ["temperature", 0.05, 2]]) {
    const v = Number(s[k]);
    if (s[k] !== undefined && Number.isFinite(v)) out[k] = Math.round(Math.max(lo, Math.min(hi, v)) * 100) / 100;
  }
  return Object.keys(out).length ? out : null;
}

/* A request that needs no model time (files, publishing): straight to a worker, past the queue. */
function ask(msg, timeoutMs = 30000) {
  const w = [...workers][0];
  if (!w) return Promise.reject(httpError(503, "the voice server at home is offline: try again later"));
  return new Promise((resolve, reject) => {
    const id = crypto.randomUUID();
    const timer = setTimeout(() => { w.pending.delete(id); reject(httpError(504, "the voice server did not answer")); }, timeoutMs);
    w.pending.set(id, { resolve, reject, timer });
    w.ws.send(JSON.stringify({ ...msg, id }));
  });
}

/* Whether worker w can speak with `voice`: a public voice it lists, or any own voice in a language it has. */
function canSpeak(w, voice) {
  const m = WORKER_VOICE_RE.exec(voice);
  if (!m) return false;
  return voice.includes("-own-") ? w.languages.has(m[1] === "nb" ? "no" : m[1]) : w.voices.some((v) => v.name === voice);
}

const queued = () => queues.interactive.length + queues.bulk.length;

/* Seconds until a new bulk job would be done: the queue ahead of it, spread over the free slots. */
function estimateSeconds() {
  let slots = 0, seconds = 4;
  for (const w of workers) { slots += w.slots; seconds = w.seconds || seconds; }
  return Math.ceil(((queued() + 1) / Math.max(slots, 1)) * seconds);
}

function settle(job, err, result) {
  clearTimeout(job.timer);
  if (job.key) jobs.delete(job.key);
  if (job.worker) job.worker.inflight.delete(job.id);
  for (const w of job.waiters) w(err, result);
  job.waiters.clear();
  Object.assign(job, { done: true, error: err, result });
}

/* Hands queued jobs to workers with free slots; a job no connected worker can do fails. */
function pump() {
  for (const q of [queues.interactive, queues.bulk]) {
    for (let i = 0; i < q.length;) {
      const job = q[i];
      const able = [...workers].filter((w) => !job.voice || canSpeak(w, job.voice));
      if (!able.length) {
        q.splice(i, 1);
        settle(job, workers.size ? httpError(400, "voice not available") : httpError(503, "the voice server at home is offline"));
        continue;
      }
      const w = able.filter((x) => x.inflight.size < x.slots).sort((a, b) => a.inflight.size - b.inflight.size)[0];
      if (!w) { i++; continue; }
      q.splice(i, 1);
      job.worker = w;
      w.inflight.set(job.id, job);
      job.timer = setTimeout(() => settle(job, httpError(504, "the voice server did not answer")), cfg.workerJobTimeoutMs);
      w.ws.send(JSON.stringify({ ...job.msg, id: job.id }));
    }
  }
}

/* Queues a job: {key?, voice?, msg, bulk}; returns it (with waiters for its result). */
function enqueue(spec) {
  if (!workers.size) throw httpError(503, "the voice server at home is offline");
  if (queued() >= cfg.workerQueueMax) throw httpError(503, "the voice server is busy", { retryAfter: estimateSeconds() });
  const job = { ...spec, id: crypto.randomUUID(), waiters: new Set(), worker: null, timer: null, done: false };
  if (job.key) jobs.set(job.key, job);
  (spec.bulk ? queues.bulk : queues.interactive).push(job);
  pump();
  return job;
}

/* The job's result, or {pending: true} when it takes longer than ms (the job goes on). */
function waitFor(job, ms) {
  return new Promise((resolve, reject) => {
    if (job.done) return job.error ? reject(job.error) : resolve(job.result); /* settled right in enqueue */
    let timer = null;
    const waiter = (err, result) => { clearTimeout(timer); if (err) reject(err); else resolve(result); };
    timer = setTimeout(() => { job.waiters.delete(waiter); resolve({ pending: true }); }, ms);
    job.waiters.add(waiter);
  });
}

function onWorkerMessage(w, raw) {
  let msg;
  try { msg = JSON.parse(raw); } catch (e) { return; }
  if (msg.type === "hello" || msg.type === "voices") {
    w.name = String(msg.name || w.name).slice(0, 40);
    w.slots = Math.max(1, Math.min(8, parseInt(msg.slots, 10) || 1));
    w.seconds = Number(msg.seconds) || w.seconds;
    w.languages = new Set(Array.isArray(msg.languages) ? msg.languages.map(String) : []);
    w.voices = (Array.isArray(msg.voices) ? msg.voices : []).filter((v) => v && WORKER_VOICE_RE.test(v.name) && !v.name.includes("-own-"));
    console.log(`worker ${w.name}: ${w.voices.length} voices, ${w.slots} slots, ~${w.seconds} s per clip`);
    pump();
    return;
  }
  const asked = w.pending.get(msg.id);
  if (asked) {
    w.pending.delete(msg.id);
    clearTimeout(asked.timer);
    if (msg.type !== "error") return asked.resolve(msg);
    const status = [400, 404, 409, 413, 507].includes(msg.status) ? msg.status : 502;
    return asked.reject(httpError(status, `voice server: ${String(msg.message || "failed").slice(0, 200)}`));
  }
  const job = w.inflight.get(msg.id);
  if (!job) return;
  if (msg.type === "error") {
    const status = msg.status === 400 || msg.status === 404 ? msg.status : 502;
    settle(job, httpError(status, `voice server: ${String(msg.message || "failed").slice(0, 200)}`));
  } else if (msg.type === "audio") {
    if (!msg.cached && msg.ms > 0) w.seconds = Math.round((0.8 * w.seconds + 0.2 * msg.ms / 1000) * 10) / 10;
    const audio = Buffer.from(String(msg.data || ""), "base64");
    if (job.key) cachePut(job.key, audio); /* also when every request gave up waiting: the retry finds it */
    settle(job, null, audio);
  } else {
    settle(job, null, msg); /* cloned, forgot */
  }
  pump();
}

const wss = new WebSocketServer({ noServer: true, maxPayload: 4 * 1024 * 1024 });

function sameSecret(given) {
  const a = crypto.createHash("sha256").update(given).digest(), b = crypto.createHash("sha256").update(cfg.workerSecret).digest();
  return cfg.workerSecret.length >= 16 && crypto.timingSafeEqual(a, b);
}

function acceptWorker(req, socket, head) {
  const auth = String(req.headers.authorization || "");
  if (!auth.startsWith("Bearer ") || !sameSecret(auth.slice(7))) {
    console.log(`${clientIp(req)} worker refused: bad secret`);
    socket.end("HTTP/1.1 401 Unauthorized\r\nConnection: close\r\n\r\n");
    return;
  }
  wss.handleUpgrade(req, socket, head, (ws) => {
    const w = { ws, name: clientIp(req), slots: 1, inflight: new Map(), pending: new Map(), voices: [], languages: new Set(),
      seconds: 4, alive: true };
    workers.add(w);
    console.log(`worker connected from ${clientIp(req)}`);
    ws.on("message", (raw) => onWorkerMessage(w, raw));
    ws.on("pong", () => { w.alive = true; });
    ws.on("close", () => {
      workers.delete(w);
      /* its running jobs go back to the front of the queue, for this worker when it is back */
      for (const job of w.inflight.values()) {
        clearTimeout(job.timer);
        job.worker = null;
        (job.bulk ? queues.bulk : queues.interactive).unshift(job);
      }
      w.inflight.clear();
      for (const p of w.pending.values()) { clearTimeout(p.timer); p.reject(httpError(503, "the voice server went offline")); }
      w.pending.clear();
      console.log(`worker ${w.name} disconnected`);
      if (!workers.size) setTimeout(pump, 30000); /* queued jobs fail if it stays away */
      else pump();
    });
  });
}

/* nginx and NAT routers drop quiet connections: ping every 30 s, drop a worker that stopped answering */
setInterval(() => {
  for (const w of workers) {
    if (!w.alive) { w.ws.terminate(); continue; }
    w.alive = false;
    w.ws.ping();
  }
}, 30 * 1000).unref();

/* ---------------------------------------------------------------- HTTP */

const cache = new Map(); /* `${voice}|${text}` -> Buffer, insertion order = age */
const hits = new Map();  /* ip -> [timestamps] */
const ownUploads = new Map(); /* ip -> [timestamps] of own voice uploads */

function clientIp(req) {
  if (cfg.trustProxy) {
    const xf = req.headers["x-forwarded-for"];
    if (xf) return xf.split(",")[0].trim();
  }
  return req.socket.remoteAddress || "?";
}

function rateLimited(ip, map = hits, limit = cfg.rateLimit, windowS = cfg.rateWindowS) {
  const now = Date.now();
  const list = (map.get(ip) || []).filter((t) => now - t < windowS * 1000);
  if (list.length >= limit) { map.set(ip, list); return true; }
  list.push(now);
  map.set(ip, list);
  if (map.size > 10000) map.clear(); /* never grows without bound */
  return false;
}

function cachePut(key, audio) {
  cache.set(key, audio);
  while (cache.size > cfg.cacheEntries) cache.delete(cache.keys().next().value);
}

function cors(req, res) {
  const origin = req.headers.origin;
  const allow = cfg.allowedOrigins.includes("*") ? "*" : (origin && cfg.allowedOrigins.includes(origin) ? origin : null);
  if (allow) {
    res.setHeader("Access-Control-Allow-Origin", allow);
    res.setHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    res.setHeader("Access-Control-Expose-Headers", "Retry-After");
    res.setHeader("Access-Control-Allow-Headers", "Content-Type, Authorization, X-Upload-Token");
    res.setHeader("Access-Control-Max-Age", "600");
    if (allow !== "*") res.setHeader("Vary", "Origin");
  }
  return !!allow || !origin;
}

function send(res, status, body, type = "application/json") {
  const data = type === "application/json" ? JSON.stringify(body) : body;
  res.writeHead(status, { "Content-Type": type, "Content-Length": Buffer.byteLength(data), "Cache-Control": "no-store" });
  res.end(data);
}

const fail = (res, status, message) => {
  if (status !== 404) console.log(`refused ${status}: ${message}`); /* pm2 logs show why a client was turned away */
  return send(res, status, { error: { message } });
};

function readBody(req, limit = 4096) {
  return new Promise((resolve, reject) => {
    const parts = [];
    let size = 0;
    req.on("data", (c) => {
      size += c.length;
      if (size > limit) { reject(new Error("body too large")); req.destroy(); return; }
      parts.push(c);
    });
    req.on("end", () => resolve(Buffer.concat(parts)));
    req.on("error", reject);
  });
}

const VOICE_RE = /^[A-Za-z]{2,3}-[A-Za-z0-9]{2,8}-[A-Za-z0-9]{2,40}$/;

/* A worker voice: the clip from the cache, a running job for it, or a new job; a clip that takes
 * longer than workerWaitMs is answered with 503 + Retry-After and served from the cache later. */
async function workerSpeech(res, ip, voice, text, key, bulk, settings) {
  const t0 = Date.now();
  let audio = cache.get(key);
  const cached = !!audio;
  if (!audio) {
    let job = jobs.get(key);
    try {
      if (!job) {
        if (rateLimited(ip)) {
          res.setHeader("Retry-After", String(cfg.rateWindowS));
          return fail(res, 429, `rate limit: ${cfg.rateLimit} requests per ${cfg.rateWindowS} s`);
        }
        job = enqueue({ key, voice, bulk, msg: { type: "speak", voice, text, ...(settings ? { settings } : {}) } });
      }
      audio = await waitFor(job, cfg.workerWaitMs);
    } catch (e) {
      if (e.retryAfter) res.setHeader("Retry-After", String(e.retryAfter));
      console.log(`${ip} ${voice} "${text}" FAILED ${e.message}`);
      return fail(res, e.status || 502, e.message);
    }
    if (audio.pending) {
      res.setHeader("Retry-After", String(Math.min(300, estimateSeconds())));
      return fail(res, 503, "the voice server is still working on it: ask again");
    }
  }
  console.log(`${ip} ${voice} "${text}" ${audio.length} B ${Date.now() - t0} ms${cached ? " (cache)" : ""}`);
  send(res, 200, audio, "audio/mpeg");
}

async function speech(req, res, ip) {
  let body;
  try { body = JSON.parse((await readBody(req)).toString()); } catch (e) { return fail(res, 400, "invalid JSON body"); }
  const text = String(body.input || "").replace(/\s+/g, " ").trim();
  const voice = String(body.voice || cfg.defaultVoice);
  const format = String(body.response_format || "mp3");
  const clampPct = (v) => Math.max(-50, Math.min(50, Math.round(Number(v) || 0)));
  const pitch = clampPct(body.pitch), rate = clampPct(body.rate); /* prosody in percent, our extension */
  if (!text) return fail(res, 400, "input is empty");
  if (text.length > cfg.maxChars) return fail(res, 400, `input longer than ${cfg.maxChars} characters`);
  if (format !== "mp3") return fail(res, 400, "only response_format mp3");
  if (voice.startsWith("cb:")) {
    if (!WORKER_VOICE_RE.test(voice)) return fail(res, 400, "voice not allowed");
    if (pitch || rate) return fail(res, 400, "pitch and rate are not available for this voice");
    const settings = trialSettings(body.settings);
    return workerSpeech(res, ip, voice, text, `${voice}|0|0|${settings ? JSON.stringify(settings) : ""}|${text}`,
      body.priority === "bulk", settings);
  }
  if (!cfg.edge || !VOICE_RE.test(voice) || (cfg.voiceFilter && !cfg.voiceFilter.test(voice))) return fail(res, 400, "voice not allowed");
  const key = `${voice}|${pitch}|${rate}|${text}`;
  const t0 = Date.now();
  let audio = cache.get(key);
  const cached = !!audio;
  if (!audio) {
    if (rateLimited(ip)) {
      res.setHeader("Retry-After", String(cfg.rateWindowS));
      return fail(res, 429, `rate limit: ${cfg.rateLimit} requests per ${cfg.rateWindowS} s`);
    }
    try {
      /* Edge refuses a share of the handshakes (403) now and then: a failed connection is dropped and
       * the clip tried again on a fresh one, up to three times */
      for (let attempt = 1; ; attempt++) {
        try {
          audio = await synthesize(text, voice, pitch, rate);
          break;
        } catch (e) {
          if (attempt >= 3 || !/upstream|no audio/.test(e.message)) throw e;
          await new Promise((r) => setTimeout(r, 300 * attempt));
        }
      }
    } catch (e) {
      console.log(`${ip} ${voice} "${text}" FAILED ${e.message}`);
      return fail(res, 502, e.message);
    }
    cachePut(key, audio);
  }
  console.log(`${ip} ${voice} "${text}" ${audio.length} B ${Date.now() - t0} ms${cached ? " (cache)" : ""}`);
  send(res, 200, audio, "audio/mpeg");
}

/* Edge's voices (when enabled) and the workers' public ones; `own` tells the configurator whether
 * an own voice can be made and in which languages. */
async function voiceList(locale) {
  let edge = [];
  if (cfg.edge) {
    try {
      edge = (await voices()).filter((v) => !cfg.voiceFilter || cfg.voiceFilter.test(v.name)).map((v) => ({ ...v, provider: "edge" }));
    } catch (e) {
      if (!workers.size) throw e;
      console.log(`edge voices: ${e.message}`);
    }
  }
  const list = [...edge, ...workerVoices()].filter((v) => !locale || v.locale.toLowerCase().startsWith(locale));
  const languages = new Set();
  for (const w of workers) for (const l of w.languages) languages.add(l);
  return { voices: list, own: { available: languages.size > 0, languages: [...languages].sort(), seconds: estimateSeconds() } };
}

/* An own voice from a recording: 16-bit mono WAV (the configurator converts), consent required. */
async function ownVoiceCreate(req, res, ip, url) {
  if (url.searchParams.get("consent") !== "yes") return fail(res, 400, "consent is required: only your own voice, or one you may use");
  let wav;
  try { wav = await readBody(req, cfg.ownMaxBytes); } catch (e) { return fail(res, 413, `recording larger than ${cfg.ownMaxBytes} bytes`); }
  if (wav.length < 44 || wav.toString("latin1", 0, 4) !== "RIFF" || wav.toString("latin1", 8, 12) !== "WAVE") return fail(res, 400, "expected a WAV file");
  if (rateLimited(ip, ownUploads, cfg.ownLimit, 24 * 3600)) return fail(res, 429, `at most ${cfg.ownLimit} own voices per day`);
  try {
    const job = enqueue({ msg: { type: "clone", wav: wav.toString("base64"), consent: true } });
    const result = await waitFor(job, cfg.workerWaitMs);
    if (result.pending) return fail(res, 503, "the voice server is busy: try again in a minute");
    console.log(`${ip} own voice created (${result.seconds} s)`);
    return send(res, 201, { id: result.voice_id, seconds: result.seconds });
  } catch (e) {
    return fail(res, e.status || 502, e.message);
  }
}

async function ownVoiceDelete(res, ip, id) {
  if (!OWN_ID_RE.test(id)) return fail(res, 400, "not an own voice id");
  try {
    const result = await ask({ type: "forget", voice_id: id });
    /* the relay's cache may still hold clips of it: they go too */
    for (const key of cache.keys()) if (key.includes(`-own-${id}|`)) cache.delete(key);
    console.log(`${ip} own voice ${result.deleted ? "deleted" : "to delete not found"}`);
    return send(res, result.deleted ? 200 : 404, { deleted: !!result.deleted });
  } catch (e) {
    return fail(res, e.status || 502, e.message);
  }
}

const templates = new Map(); /* ip -> [timestamps] of published voices */
const packUploads = new Map(); /* ip -> [timestamps] of shared packs */

async function readJson(req, limit = 64 * 1024) {
  try { return JSON.parse((await readBody(req, limit)).toString()); } catch (e) { throw httpError(400, "invalid JSON body"); }
}

/* An own voice made public by its speaker ("Custom speaker" → Create template). */
async function publishVoice(req, res, ip) {
  const body = await readJson(req);
  if (body.consent !== true) return fail(res, 400, "consent is required");
  if (!OWN_ID_RE.test(String(body.voice_id || ""))) return fail(res, 400, "not an own voice id");
  if (rateLimited(ip, templates, cfg.templateLimit, 24 * 3600)) return fail(res, 429, `at most ${cfg.templateLimit} voices per day`);
  try {
    const r = await ask({ type: "publish", voice_id: body.voice_id, name: String(body.name || "").slice(0, 60),
      locale: String(body.locale || ""), gender: String(body.gender || ""), settings: trialSettings(body.settings) || {},
      audio: body.audio && typeof body.audio === "object" ? body.audio : {}, consent: true });
    console.log(`${ip} template published: ${r.voice}`);
    return send(res, 201, { voice: r.voice });
  } catch (e) {
    return fail(res, e.status || 502, e.message);
  }
}

/* A shared pack: its description first (201 {id, upload_token, part_size}), then the file in parts. */
async function packCreate(req, res, ip) {
  const body = await readJson(req, 256 * 1024);
  if (body.consent !== true) return fail(res, 400, "consent is required");
  let pack;
  try { pack = cleanPack(body, cfg.maxChars); } catch (e) { return fail(res, 400, e.message); }
  if (pack.size > cfg.packMaxBytes) return fail(res, 413, `a pack may have ${Math.floor(cfg.packMaxBytes / 2 ** 20)} MB at most`);
  if (!workers.size) return fail(res, 503, "the voice server at home is offline: try again later");
  if (rateLimited(ip, packUploads, cfg.packLimit, 24 * 3600)) return fail(res, 429, `at most ${cfg.packLimit} shared packs per day`);
  const made = packs.create(pack);
  if (!made) return fail(res, 409, `there is a pack called "${pack.name}" in this language already`);
  console.log(`${ip} pack ${made.id} "${pack.name}" (${pack.lang}, ${pack.voice}) announced, ${pack.size} B`);
  return send(res, 201, { id: made.id, upload_token: made.token, part_size: cfg.packPartBytes });
}

async function packPart(req, res, id, url) {
  const token = packs.token(id);
  const given = String(req.headers["x-upload-token"] || "");
  if (!token || given.length !== token.length || !crypto.timingSafeEqual(Buffer.from(given), Buffer.from(token))) {
    return fail(res, 403, "no upload open for this pack");
  }
  const part = parseInt(url.searchParams.get("part"), 10), parts = parseInt(url.searchParams.get("parts"), 10);
  let data;
  try { data = await readBody(req, cfg.packPartBytes); } catch (e) { return fail(res, 413, "part too large"); }
  const pack = packs.get(id);
  if (!(parts >= 1 && part >= 0 && part < parts) || parts > Math.ceil(pack.size / cfg.packPartBytes) + 1) return fail(res, 400, "bad part");
  try {
    const r = await ask({ type: "pack_put", pack: id, part, parts, data: data.toString("base64") });
    if (r.done) {
      packs.ready(id, r.sha256, r.size);
      console.log(`pack ${id} "${pack.name}" published: ${r.size} B`);
    }
    return send(res, 200, { done: !!r.done });
  } catch (e) {
    return fail(res, e.status || 502, e.message);
  }
}

/* The file, read from the worker a piece at a time while the client takes it. */
async function packDownload(req, res, id) {
  const pack = packs.get(id);
  if (!pack || pack.state !== "ready") return fail(res, 404, "no such pack");
  if (!workers.size) return fail(res, 503, "the voice server at home is offline: try again later");
  let first;
  try { first = await ask({ type: "pack_read", pack: id, offset: 0, length: cfg.packPartBytes }); } catch (e) { return fail(res, e.status || 502, e.message); }
  res.writeHead(200, { "Content-Type": "application/octet-stream", "Content-Length": first.size, "Cache-Control": "no-store",
    "Content-Disposition": `attachment; filename="${pack.lang}.ubpack"` });
  packs.counted(id);
  let chunk = Buffer.from(first.data, "base64"), offset = 0;
  try {
    while (chunk.length) {
      if (!res.write(chunk)) await once(res, "drain");
      offset += chunk.length;
      if (offset >= first.size || res.destroyed) break;
      chunk = Buffer.from((await ask({ type: "pack_read", pack: id, offset, length: cfg.packPartBytes })).data, "base64");
    }
    res.end();
  } catch (e) {
    console.log(`pack ${id} download broke off: ${e.message}`);
    res.destroy();
  }
}

const packSummary = (p) => ({ id: p.id, lang: p.lang, name: p.name, voice: p.voice, voice_label: p.voice_label, clips: p.clips,
  size: p.size, created: p.created, downloads: p.downloads, base: p.base, settings: p.settings,
  changed_texts: Object.keys(p.overrides || {}).length });

function isAdmin(req) {
  const auth = String(req.headers.authorization || "");
  if (cfg.adminToken.length < 16 || !auth.startsWith("Bearer ")) return false;
  const a = crypto.createHash("sha256").update(auth.slice(7)).digest(), b = crypto.createHash("sha256").update(cfg.adminToken).digest();
  return crypto.timingSafeEqual(a, b);
}

async function packDelete(res, id) {
  const pack = packs.get(id);
  if (!pack) return fail(res, 404, "no such pack");
  try { await ask({ type: "pack_delete", pack: id }); } catch (e) { return fail(res, e.status || 502, e.message); }
  packs.remove(id);
  console.log(`pack ${id} "${pack.name}" deleted by the operator`);
  return send(res, 200, { deleted: true });
}

/* uploads that never finished free their name after two hours */
setInterval(async () => {
  for (const id of packs.stale(2 * 3600 * 1000)) {
    try { await ask({ type: "pack_delete", pack: id }); packs.remove(id); console.log(`pack ${id}: unfinished upload dropped`); } catch (e) { /* worker away: next time */ }
  }
}, 10 * 60 * 1000).unref();

const server = http.createServer(async (req, res) => {
  const ip = clientIp(req);
  const url = new URL(req.url, "http://localhost");
  if (!cors(req, res)) return fail(res, 403, "origin not allowed");
  if (req.method === "OPTIONS") { res.writeHead(204); return res.end(); }
  try {
    const p = url.pathname, m = req.method;
    if (m === "GET" && p === "/health") return send(res, 200, { ok: true, workers: workers.size, packs: packs.count() });
    if (m === "GET" && p === "/v1/voices") return send(res, 200, await voiceList((url.searchParams.get("locale") || "").toLowerCase()));
    if (m === "POST" && p === "/v1/audio/speech") return await speech(req, res, ip);
    if (m === "POST" && p === "/v1/voices/own") return await ownVoiceCreate(req, res, ip, url);
    if (m === "POST" && p === "/v1/voices/public") return await publishVoice(req, res, ip);
    const own = /^\/v1\/voices\/own\/([^/]+)$/.exec(p);
    if (m === "DELETE" && own) return await ownVoiceDelete(res, ip, own[1]);
    if (m === "GET" && p === "/v1/packs") return send(res, 200, { packs: packs.list(url.searchParams.get("lang") || "").map(packSummary) });
    if (m === "POST" && p === "/v1/packs") return await packCreate(req, res, ip);
    const pk = /^\/v1\/packs\/([a-z0-9]+)(\/file)?$/.exec(p);
    if (pk && PACK_ID_RE.test(pk[1])) {
      if (m === "GET" && pk[2]) return await packDownload(req, res, pk[1]);
      if (m === "PUT" && pk[2]) return await packPart(req, res, pk[1], url);
      const pack = packs.get(pk[1]);
      if (m === "GET" && pack && pack.state === "ready") return send(res, 200, { ...packSummary(pack), overrides: pack.overrides });
    }
    const adm = /^\/v1\/admin\/packs(?:\/([a-z0-9]+))?$/.exec(p);
    if (adm) {
      if (!isAdmin(req)) return fail(res, 403, "admin only");
      if (m === "GET" && !adm[1]) return send(res, 200, { packs: packs.all().map((x) => ({ ...packSummary(x), state: x.state })) });
      if (m === "DELETE" && adm[1]) return await packDelete(res, adm[1]);
    }
    return fail(res, 404, "not found");
  } catch (e) {
    console.log(`${ip} ${req.method} ${url.pathname} ERROR ${e.message}`);
    return fail(res, 500, "internal error");
  }
});

server.on("upgrade", (req, socket, head) => {
  if (new URL(req.url, "http://localhost").pathname !== "/v1/worker") {
    socket.end("HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n");
    return;
  }
  acceptWorker(req, socket, head);
});

server.listen(cfg.port, cfg.host, () => {
  console.log(`tts-relay on http://${cfg.host}:${cfg.port} (max ${cfg.maxChars} chars, ${cfg.rateLimit}/${cfg.rateWindowS} s per IP, ` +
    `origins ${cfg.allowedOrigins.join(" ")}, Edge ${cfg.edge ? "on" : "off"}, workers ${cfg.workerSecret.length >= 16 ? "accepted" : "off (no WORKER_SECRET)"})`);
});
