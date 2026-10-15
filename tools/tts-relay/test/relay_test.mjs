/*
 * Self-test of the relay's worker side: starts server.js (Edge off) and plays the worker with a fake.
 *   cd tools/tts-relay && npm install && node test/relay_test.mjs
 */
import { spawn } from "node:child_process";
import net from "node:net";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import WebSocket from "ws";

const here = path.dirname(fileURLToPath(import.meta.url));
const SECRET = "test-secret-0123456789";
const VOICE = "cb:de-DE-testvoice";
const OWN_ID = "abcdefghijklmnopqrstuvwxyz";
let failures = 0;
const check = (ok, what) => { console.log(`${ok ? "ok  " : "FAIL"} ${what}`); if (!ok) failures++; };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const port = await new Promise((resolve) => {
  const s = net.createServer().listen(0, "127.0.0.1", () => { const p = s.address().port; s.close(() => resolve(p)); });
});
const base = `http://127.0.0.1:${port}`;
const relay = spawn(process.execPath, [path.join(here, "..", "server.js")], {
  cwd: os.tmpdir(), /* no .env there */
  env: { ...process.env, PORT: String(port), EDGE: "0", WORKER_SECRET: SECRET, WORKER_WAIT_MS: "800", RATE_LIMIT: "1000", OWN_VOICE_LIMIT: "2" },
  stdio: ["ignore", "pipe", "pipe"],
});
let log = "";
relay.stdout.on("data", (d) => { log += d; });
relay.stderr.on("data", (d) => { log += d; });
for (let i = 0; i < 50 && !log.includes("tts-relay on"); i++) await sleep(100);

/* A fake worker: answers speak with "MP3:<text>" after `delay` ms, clone with OWN_ID, forget with deleted. */
function fakeWorker(secret = SECRET, opts = {}) {
  const w = { got: [], delay: 0, ws: null };
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(`ws://127.0.0.1:${port}/v1/worker`, { headers: { Authorization: `Bearer ${secret}` } });
    w.ws = ws;
    ws.on("unexpected-response", (req, res) => reject(new Error(`HTTP ${res.statusCode}`)));
    ws.on("error", reject);
    ws.on("open", () => {
      ws.send(JSON.stringify({ type: "hello", name: "fake", slots: 1, seconds: 2, languages: ["de", "en"],
        voices: [{ name: VOICE, locale: "de-DE", gender: "Female", friendly: "Test" }, { name: "cb:de-DE-own-" + OWN_ID, locale: "de-DE" }] }));
      setTimeout(() => resolve(w), 100);
    });
    ws.on("message", async (raw) => {
      const msg = JSON.parse(raw);
      w.got.push(msg);
      if (opts.silent) return;
      await sleep(w.delay);
      if (msg.type === "speak") {
        if (msg.voice.endsWith("missing")) ws.send(JSON.stringify({ type: "error", id: msg.id, status: 404, message: "no such voice" }));
        else ws.send(JSON.stringify({ type: "audio", id: msg.id, mime: "audio/mpeg", ms: 1500, cached: false,
          data: Buffer.from(`MP3:${msg.text}`).toString("base64") }));
      } else if (msg.type === "clone") {
        ws.send(JSON.stringify({ type: "cloned", id: msg.id, voice_id: OWN_ID, seconds: 9.5 }));
      } else if (msg.type === "forget") {
        ws.send(JSON.stringify({ type: "forgot", id: msg.id, deleted: msg.voice_id === OWN_ID }));
      }
    });
  });
}

const speak = (voice, input, extra = {}) => fetch(`${base}/v1/audio/speech`, {
  method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ input, voice, ...extra }) });

function wav(seconds = 1) {
  const n = 24000 * seconds, b = Buffer.alloc(44 + 2 * n);
  b.write("RIFF", 0, "latin1"); b.writeUInt32LE(36 + 2 * n, 4); b.write("WAVE", 8, "latin1"); b.write("fmt ", 12, "latin1");
  b.writeUInt32LE(16, 16); b.writeUInt16LE(1, 20); b.writeUInt16LE(1, 22); b.writeUInt32LE(24000, 24); b.writeUInt32LE(48000, 28);
  b.writeUInt16LE(2, 32); b.writeUInt16LE(16, 34); b.write("data", 36, "latin1"); b.writeUInt32LE(2 * n, 40);
  return b;
}

try {
  /* no worker yet */
  let r = await fetch(`${base}/v1/voices`);
  let j = await r.json();
  check(r.ok && j.voices.length === 0 && j.own.available === false, "voices without a worker: empty, no own voices");
  r = await speak(VOICE, "Hallo");
  check(r.status === 503, `speech without a worker: 503 (${r.status})`);

  /* a wrong secret is refused */
  let refused = "";
  try { await fakeWorker("wrong-secret-0123456789"); } catch (e) { refused = e.message; }
  check(refused === "HTTP 401", `worker with a wrong secret refused (${refused})`);

  const w = await fakeWorker();
  r = await fetch(`${base}/v1/voices?locale=de`);
  j = await r.json();
  check(j.voices.length === 1 && j.voices[0].name === VOICE && j.voices[0].provider === "chatterbox" && j.voices[0].seconds === 2,
    "worker voice listed with provider and estimate; own voices never listed");
  check(j.own.available && j.own.languages.join() === "de,en", "own voice languages announced");

  /* synthesis, then the cache */
  r = await speak(VOICE, "Hallo  Welt");
  check(r.ok && r.headers.get("content-type") === "audio/mpeg" && (await r.text()) === "MP3:Hallo Welt", "speech through the worker");
  const before = w.got.length;
  r = await speak(VOICE, "Hallo Welt");
  check(r.ok && (await r.text()) === "MP3:Hallo Welt" && w.got.length === before, "second request served from the relay cache");

  /* two requests for one new clip share a job */
  const [a, b] = await Promise.all([speak(VOICE, "Thermik"), speak(VOICE, "Thermik")]);
  check(a.ok && b.ok && w.got.filter((m) => m.text === "Thermik").length === 1, "concurrent requests for one clip: one job");

  /* refusals */
  r = await speak(VOICE, "Hallo", { pitch: 10 });
  check(r.status === 400, "pitch on a worker voice: 400");
  r = await speak("cb:de-DE-missing", "Hallo");
  check(r.status === 400, `voice no worker lists: 400 (${r.status})`);
  r = await speak("de-DE-KatjaNeural", "Hallo");
  check(r.status === 400, "Edge voice with EDGE=0: 400");
  r = await speak(`cb:de-DE-own-${OWN_ID}`, "Eigene Stimme");
  check(r.ok && (await r.text()) === "MP3:Eigene Stimme", "own voice: any worker with the language takes it");

  /* a slow clip: 503 + Retry-After, then from the cache */
  w.delay = 1500;
  r = await speak(VOICE, "Langsam");
  check(r.status === 503 && Number(r.headers.get("retry-after")) > 0, `slow clip: 503 with Retry-After ${r.headers.get("retry-after")}`);
  await sleep(1000);
  r = await speak(VOICE, "Langsam");
  check(r.ok && (await r.text()) === "MP3:Langsam" && w.got.filter((m) => m.text === "Langsam").length === 1,
    "asked again: the finished clip, made once");
  w.delay = 0;

  /* bulk waits behind single clips */
  w.delay = 200;
  const order = [];
  const bulk = ["P1", "P2", "P3"].map((t) => speak(VOICE, t, { priority: "bulk" }).then(() => order.push(t)));
  await sleep(50);
  const single = speak(VOICE, "Einzeln").then(() => order.push("Einzeln"));
  await Promise.all([...bulk, single]);
  check(order.indexOf("Einzeln") < order.indexOf("P3"), `a single clip overtakes the queued pack clips (${order.join(" ")})`);
  w.delay = 0;

  /* own voices */
  r = await fetch(`${base}/v1/voices/own`, { method: "POST", headers: { "Content-Type": "audio/wav" }, body: wav() });
  check(r.status === 400, "own voice without consent: 400");
  r = await fetch(`${base}/v1/voices/own?consent=yes`, { method: "POST", headers: { "Content-Type": "audio/wav" }, body: Buffer.from("not a wav") });
  check(r.status === 400, "own voice that is no WAV: 400");
  r = await fetch(`${base}/v1/voices/own?consent=yes`, { method: "POST", headers: { "Content-Type": "audio/wav" }, body: wav() });
  j = await r.json();
  check(r.status === 201 && j.id === OWN_ID && w.got.some((m) => m.type === "clone" && m.consent === true), "own voice created");
  r = await fetch(`${base}/v1/voices/own/${OWN_ID}`, { method: "DELETE" });
  j = await r.json();
  check(r.ok && j.deleted === true, "own voice deleted");
  r = await fetch(`${base}/v1/voices/own/bad`, { method: "DELETE" });
  check(r.status === 400, "delete with a malformed id: 400");
  await fetch(`${base}/v1/voices/own?consent=yes`, { method: "POST", body: wav() });
  r = await fetch(`${base}/v1/voices/own?consent=yes`, { method: "POST", body: wav() });
  check(r.status === 429, "own voice uploads limited per IP and day");

  /* a worker that drops with a job: the job waits for the next one */
  w.ws.close();
  await sleep(100);
  const silent = await fakeWorker(SECRET, { silent: true });
  const pending = speak(VOICE, "Wiederholt");
  await sleep(150);
  silent.ws.close();
  await sleep(100);
  const w2 = await fakeWorker();
  r = await pending;
  check(r.ok && (await r.text()) === "MP3:Wiederholt" && w2.got.some((m) => m.text === "Wiederholt"), "job of a dropped worker done by the next one");
  r = await fetch(`${base}/health`);
  j = await r.json();
  check(j.ok && j.workers === 1, "health counts the workers");
  w2.ws.close();
} catch (e) {
  failures++;
  console.log(`FAIL exception: ${e.stack}`);
} finally {
  relay.kill();
}
if (failures) console.log(`\n--- relay log ---\n${log}`);
console.log(failures ? `${failures} FAILED` : "all passed");
process.exit(failures ? 1 : 0);
