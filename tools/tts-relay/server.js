/*
 * TTS relay for the UltraBeep configurator: a small HTTP server that turns a few
 * words into MP3 with Microsoft Edge's text-to-speech (free, no account), which browsers
 * cannot call directly. OpenAI-compatible request shape, so the configurator can also talk
 * to other services:
 *   POST /v1/audio/speech   {"input": "Ozone Rush", "voice": "de-DE-KatjaNeural"}  -> audio/mpeg
 *   GET  /v1/voices?locale=de                                                        -> JSON list
 *   GET  /health
 * Settings come from .env (see .env.example); run behind nginx (nginx.conf.example) with pm2
 * (ecosystem.config.cjs). Abuse limits: text length, requests per IP and window, voice filter.
 */
import http from "node:http";
import msedge from "msedge-tts"; /* the Edge read-aloud protocol, maintained there (token scheme, versions) */

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
};

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

/* ---------------------------------------------------------------- HTTP */

const cache = new Map(); /* `${voice}|${text}` -> Buffer, insertion order = age */
const hits = new Map();  /* ip -> [timestamps] */

function clientIp(req) {
  if (cfg.trustProxy) {
    const xf = req.headers["x-forwarded-for"];
    if (xf) return xf.split(",")[0].trim();
  }
  return req.socket.remoteAddress || "?";
}

function rateLimited(ip) {
  const now = Date.now();
  const list = (hits.get(ip) || []).filter((t) => now - t < cfg.rateWindowS * 1000);
  if (list.length >= cfg.rateLimit) { hits.set(ip, list); return true; }
  list.push(now);
  hits.set(ip, list);
  if (hits.size > 10000) hits.clear(); /* never grows without bound */
  return false;
}

function cors(req, res) {
  const origin = req.headers.origin;
  const allow = cfg.allowedOrigins.includes("*") ? "*" : (origin && cfg.allowedOrigins.includes(origin) ? origin : null);
  if (allow) {
    res.setHeader("Access-Control-Allow-Origin", allow);
    res.setHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    res.setHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");
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
    req.on("end", () => resolve(Buffer.concat(parts).toString()));
    req.on("error", reject);
  });
}

const VOICE_RE = /^[A-Za-z]{2,3}-[A-Za-z0-9]{2,8}-[A-Za-z0-9]{2,40}$/;

async function speech(req, res, ip) {
  let body;
  try { body = JSON.parse(await readBody(req)); } catch (e) { return fail(res, 400, "invalid JSON body"); }
  const text = String(body.input || "").replace(/\s+/g, " ").trim();
  const voice = String(body.voice || cfg.defaultVoice);
  const format = String(body.response_format || "mp3");
  const clampPct = (v) => Math.max(-50, Math.min(50, Math.round(Number(v) || 0)));
  const pitch = clampPct(body.pitch), rate = clampPct(body.rate); /* prosody in percent, our extension */
  if (!text) return fail(res, 400, "input is empty");
  if (text.length > cfg.maxChars) return fail(res, 400, `input longer than ${cfg.maxChars} characters`);
  if (!VOICE_RE.test(voice) || (cfg.voiceFilter && !cfg.voiceFilter.test(voice))) return fail(res, 400, "voice not allowed");
  if (format !== "mp3") return fail(res, 400, "only response_format mp3");
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
    cache.set(key, audio);
    while (cache.size > cfg.cacheEntries) cache.delete(cache.keys().next().value);
  }
  console.log(`${ip} ${voice} "${text}" ${audio.length} B ${Date.now() - t0} ms${cached ? " (cache)" : ""}`);
  send(res, 200, audio, "audio/mpeg");
}

const server = http.createServer(async (req, res) => {
  const ip = clientIp(req);
  const url = new URL(req.url, "http://localhost");
  if (!cors(req, res)) return fail(res, 403, "origin not allowed");
  if (req.method === "OPTIONS") { res.writeHead(204); return res.end(); }
  try {
    if (req.method === "GET" && url.pathname === "/health") return send(res, 200, { ok: true });
    if (req.method === "GET" && url.pathname === "/v1/voices") {
      const locale = (url.searchParams.get("locale") || "").toLowerCase();
      const list = (await voices()).filter((v) => !locale || v.locale.toLowerCase().startsWith(locale))
        .filter((v) => !cfg.voiceFilter || cfg.voiceFilter.test(v.name));
      return send(res, 200, { voices: list });
    }
    if (req.method === "POST" && url.pathname === "/v1/audio/speech") return await speech(req, res, ip);
    return fail(res, 404, "not found");
  } catch (e) {
    console.log(`${ip} ${req.method} ${url.pathname} ERROR ${e.message}`);
    return fail(res, 500, "internal error");
  }
});

server.listen(cfg.port, cfg.host, () => {
  console.log(`tts-relay on http://${cfg.host}:${cfg.port} (max ${cfg.maxChars} chars, ${cfg.rateLimit}/${cfg.rateWindowS} s per IP, ` +
    `origins ${cfg.allowedOrigins.join(" ")})`);
});
