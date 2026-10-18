/*
 * Self-test of the relay's worker side: starts server.js (Edge off) and plays the worker with a fake.
 *   cd tools/tts-relay && npm install && node test/relay_test.mjs
 */
import { spawn } from "node:child_process";
import fs from "node:fs";
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
const ADMIN = "admin-token-0123456789";
const dbPath = path.join(os.tmpdir(), `relay-test-${port}.db`);
const relay = spawn(process.execPath, [path.join(here, "..", "server.js")], {
  cwd: os.tmpdir(), /* no .env there */
  env: { ...process.env, PORT: String(port), EDGE: "0", WORKER_SECRET: SECRET, WORKER_WAIT_MS: "800", RATE_LIMIT: "1000",
    OWN_VOICE_LIMIT: "2", DB_PATH: dbPath, ADMIN_TOKEN: ADMIN, MAX_CHARS: "120" },
  stdio: ["ignore", "pipe", "pipe"],
});
let log = "";
relay.stdout.on("data", (d) => { log += d; });
relay.stderr.on("data", (d) => { log += d; });
for (let i = 0; i < 50 && !log.includes("tts-relay on"); i++) await sleep(100);

/* A fake worker: answers speak with "MP3:<text>[settings]" after `delay` ms, clone with OWN_ID, forget with
 * deleted, publish with a voice name, and keeps pack files in memory. */
const stored = new Map(); /* pack id -> Buffer */
function fakeWorker(secret = SECRET, opts = {}) {
  const w = { got: [], delay: 0, ws: null };
  const send = (m) => w.ws.send(JSON.stringify(m));
  const parts = new Map();
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
        if (msg.voice.endsWith("missing")) send({ type: "error", id: msg.id, status: 404, message: "no such voice" });
        else send({ type: "audio", id: msg.id, mime: "audio/mpeg", ms: 1500, cached: false, spoken: `gesagt: ${msg.text}`,
          data: Buffer.from(`MP3:${msg.text}${msg.settings ? JSON.stringify(msg.settings) : ""}${msg.language ? `[${msg.language}]` : ""}${msg.variant ? `#${msg.variant}` : ""}`).toString("base64") });
      } else if (msg.type === "clone") {
        send({ type: "cloned", id: msg.id, voice_id: OWN_ID, seconds: 9.5 });
      } else if (msg.type === "forget") {
        send({ type: "forgot", id: msg.id, deleted: msg.voice_id === OWN_ID });
      } else if (msg.type === "publish") {
        if (msg.name === "Test") send({ type: "error", id: msg.id, status: 409, message: "There is a voice called Test" });
        else send({ type: "published", id: msg.id, voice: `cb:${msg.locale}-${msg.name.toLowerCase()}` });
      } else if (msg.type === "pack_put") {
        const list = parts.get(msg.pack) || [];
        list.push(Buffer.from(msg.data, "base64"));
        parts.set(msg.pack, list);
        const done = list.length === msg.parts;
        if (done) stored.set(msg.pack, Buffer.concat(list));
        send({ type: "pack_stored", id: msg.id, done, ...(done ? { size: stored.get(msg.pack).length, sha256: "x" } : {}) });
      } else if (msg.type === "pack_read") {
        const file = stored.get(msg.pack);
        if (!file) send({ type: "error", id: msg.id, status: 404, message: "no such pack" });
        else send({ type: "pack_data", id: msg.id, size: file.length, data: file.subarray(msg.offset, msg.offset + msg.length).toString("base64") });
      } else if (msg.type === "pack_sample") {
        const clips = {};
        for (const p of msg.paths) if (p !== "voices/missing.qoa") clips[p] = Buffer.from(`QOA:${p}`).toString("base64");
        send({ type: "pack_clips", id: msg.id, clips });
      } else if (msg.type === "pack_delete") {
        send({ type: "pack_deleted", id: msg.id, deleted: stored.delete(msg.pack) });
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
  check(decodeURIComponent(r.headers.get("x-spoken-text") || "") === "gesagt: Hallo Welt", "the spoken text comes along, from the cache too");

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

  /* trial settings are passed on and keep their own cache entry */
  r = await speak(VOICE, "Hallo Welt", { settings: { exaggeration: 0.9, bogus: 1 } });
  check(r.ok && (await r.text()) === 'MP3:Hallo Welt{"exaggeration":0.9}', "trial settings passed on, unknown keys dropped");
  r = await speak(VOICE, "Hallo Welt");
  check((await r.text()) === "MP3:Hallo Welt", "the plain clip stays separate in the cache");
  r = await speak(VOICE, "GPS ok", { language: "en" });
  check(r.ok && (await r.text()) === "MP3:GPS ok[en]", "a line in another language: passed on");
  r = await speak(VOICE, "GPS ok", { language: "English" });
  check(r.ok && (await r.text()) === "MP3:GPS ok", "a malformed language is ignored, and cached apart from the English one");
  r = await speak(VOICE, "GPS ok", { variant: 2 });
  check(r.ok && (await r.text()) === "MP3:GPS ok#2", "a variant: passed on, cached apart from the default take");
  r = await speak(VOICE, "GPS ok", { language: "en", variant: 2 });
  check(r.ok && (await r.text()) === "MP3:GPS ok[en]#2", "a variant of a line in another language");
  let plain = true;
  for (const v of [0, -1, 100, "2", 2.5, true]) {
    r = await speak(VOICE, "GPS ok", { variant: v });
    plain = plain && r.ok && (await r.text()) === "MP3:GPS ok";
  }
  check(plain, "variant 0 and malformed variants are the default take");

  /* a template for everyone */
  const pub = (body) => fetch(`${base}/v1/voices/public`, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) });
  r = await pub({ voice_id: OWN_ID, name: "Anna", locale: "de-DE", gender: "Female" });
  check(r.status === 400, "template without consent: 400");
  r = await pub({ voice_id: OWN_ID, name: "Test", locale: "de-DE", gender: "Female", consent: true });
  check(r.status === 409, "template with a taken name: 409 from the worker");
  r = await pub({ voice_id: OWN_ID, name: "Anna", locale: "de-DE", gender: "Female", consent: true, settings: { temperature: 0.7 }, audio: { pitch: 5 } });
  j = await r.json();
  check(r.status === 201 && j.voice === "cb:de-DE-anna" && w.got.some((m) => m.type === "publish" && m.settings.temperature === 0.7),
    "template published");

  /* a shared pack: announce, upload in parts, list, details, download, operator deletes */
  const file = Buffer.alloc(2.5 * 2 ** 20, 7);
  const packBody = { name: "Mein Pack", lang: "de", voice: VOICE, voice_label: "Test", clips: 979, size: file.length, consent: true,
    settings: { pitch: 4, rate: -3, bandpass: true, lo: 250, hi: 9000 }, overrides: { "profile_hike": "Haik änd Flai" }, languages: { gps_ok: "en" },
    variants: { no_gps: 3 } };
  const createPack = (b) => fetch(`${base}/v1/packs`, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(b) });
  r = await createPack({ ...packBody, consent: false });
  check(r.status === 400, "pack without consent: 400");
  r = await createPack({ ...packBody, languages: { gps_ok: "English" } });
  check(r.status === 400, "pack with a malformed language: 400");
  r = await createPack({ ...packBody, variants: { no_gps: 100 } });
  check(r.status === 400, "pack with a variant out of range: 400");
  r = await createPack({ ...packBody, voice: `cb:de-DE-own-${OWN_ID}` });
  check(r.status === 400, "pack with an own voice: 400");
  r = await createPack(packBody);
  const made = await r.json();
  check(r.status === 201 && /^[a-z0-9]{20}$/.test(made.id) && made.part_size === 2 ** 20, "pack announced");
  r = await createPack({ ...packBody, name: "mein pack" });
  check(r.status === 409, "pack name taken in the language (any case): 409");
  r = await fetch(`${base}/v1/packs?lang=de`);
  check((await r.json()).packs.length === 0, "an announced pack is not listed before its file is there");
  const nParts = Math.ceil(file.length / made.part_size);
  r = await fetch(`${base}/v1/packs/${made.id}/file?part=0&parts=${nParts}`, { method: "PUT", headers: { "X-Upload-Token": "wrong".padEnd(48, "x") },
    body: file.subarray(0, made.part_size) });
  check(r.status === 403, "upload with a wrong token: 403");
  let done = false;
  for (let i = 0; i < nParts; i++) {
    r = await fetch(`${base}/v1/packs/${made.id}/file?part=${i}&parts=${nParts}`, { method: "PUT",
      headers: { "X-Upload-Token": made.upload_token }, body: file.subarray(i * made.part_size, (i + 1) * made.part_size) });
    done = (await r.json()).done;
  }
  check(done && stored.get(made.id)?.equals(file), "pack file uploaded in parts");
  r = await fetch(`${base}/v1/packs/${made.id}/file?part=0&parts=1`, { method: "PUT", headers: { "X-Upload-Token": made.upload_token }, body: "x" });
  check(r.status === 403, "no more uploads once the pack is complete");
  r = await fetch(`${base}/v1/packs?lang=de`);
  j = await r.json();
  check(j.packs.length === 1 && j.packs[0].name === "Mein Pack" && j.packs[0].settings.pitch === 4 && j.packs[0].changed_texts === 1,
    "pack listed with settings");
  r = await fetch(`${base}/v1/packs?lang=fr`);
  check((await r.json()).packs.length === 0, "pack list filtered by language");
  r = await fetch(`${base}/v1/packs/${made.id}`);
  j = await r.json();
  check(j.overrides.profile_hike === "Haik änd Flai" && j.languages.gps_ok === "en" && j.variants.no_gps === 3,
    "pack details with its changed texts, languages and variants");
  r = await fetch(`${base}/v1/packs/${made.id}/sample?names=takeoff,al/al_1230,missing,../x`);
  j = await r.json();
  check(r.ok && Buffer.from(j.clips.takeoff, "base64").toString() === "QOA:voices/takeoff.qoa" &&
    Buffer.from(j.clips["al/al_1230"], "base64").toString() === "QOA:al/al_1230.qoa" && Object.keys(j.clips).length === 2,
  "sample clips out of the pack, by catalogue name; odd names dropped");
  r = await fetch(`${base}/v1/packs/${made.id}/file`);
  const got = Buffer.from(await r.arrayBuffer());
  check(r.ok && got.equals(file) && /de\.ubpack/.test(r.headers.get("content-disposition")), "pack downloaded whole, as de.ubpack");
  r = await fetch(`${base}/v1/admin/packs/${made.id}`, { method: "DELETE" });
  check(r.status === 403, "deleting needs the admin token");
  r = await fetch(`${base}/v1/admin/packs`, { headers: { Authorization: `Bearer ${ADMIN}` } });
  j = await r.json();
  check(j.packs.length === 1 && j.packs[0].downloads === 1, "admin list counts downloads");
  r = await fetch(`${base}/v1/admin/packs/${made.id}`, { method: "DELETE", headers: { Authorization: `Bearer ${ADMIN}` } });
  check(r.ok && !stored.has(made.id), "operator deletes a pack, file included");
  r = await fetch(`${base}/v1/packs/${made.id}/file`);
  check(r.status === 404, "a deleted pack is gone");

  /* works: what a device's pack is made of; its owner overwrites it, the key never comes back */
  const json = (method, url, body) => fetch(`${base}${url}`, { method, headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) });
  const workBody = { lang: "de", name: "Mein Vario", voice: VOICE, locale: "de-DE", settings: { pitch: -1, rate: 8, bandpass: true, lo: 300, hi: 8000 },
    clips: { takeoff: { t: "Abflug" }, gps_ok: { t: "GPS ok", l: "en" }, no_gps: { t: "Suche GPS", v: 3 }, "glide/glide_87": { t: "8.7", f: 1 } },
    built: "2026-10-18T12:00:00Z" };
  r = await json("POST", "/v1/works", { ...workBody, lang: "deu" });
  check(r.status === 400, "work with a bad language: 400");
  r = await json("POST", "/v1/works", { ...workBody, clips: { "../x": { t: "a" } } });
  check(r.status === 400, "work with a bad clip name: 400");
  r = await json("POST", "/v1/works", { ...workBody, clips: { takeoff: { t: "A", v: 100 } } });
  check(r.status === 400, "work with a take out of range: 400");
  r = await json("POST", "/v1/works", workBody);
  const wk = await r.json();
  check(r.status === 201 && /^[a-z0-9]{20}$/.test(wk.id) && /^[a-f0-9]{48}$/.test(wk.key) && wk.version === 1, "work created with its owner key");
  r = await fetch(`${base}/v1/works/${wk.id}`);
  j = await r.json();
  check(r.ok && j.version === 1 && j.voice === VOICE && j.settings.rate === 8 && j.clips.gps_ok.l === "en" && j.clips.no_gps.v === 3 &&
    j.clips["glide/glide_87"].f === 1 && !j.clips.takeoff.f && j.built === "2026-10-18T12:00:00.000Z" && !("key" in j) && !("owner" in j),
  "work read back: clips, languages, takes, single files, the build time; no key");
  r = await json("PUT", `/v1/works/${wk.id}`, { ...workBody, key: "0".repeat(48), clips: {} });
  check(r.status === 403, "overwriting with another key: 403");
  r = await json("PUT", `/v1/works/${"z".repeat(20)}`, { ...workBody, key: wk.key });
  check(r.status === 404, "overwriting a work that is not there: 404");
  r = await json("PUT", `/v1/works/${wk.id}`, { ...workBody, key: wk.key, clips: { takeoff: { t: "Start!" } }, built: null });
  j = await r.json();
  check(r.ok && j.version === 2, "the owner overwrites: version 2");
  r = await fetch(`${base}/v1/works/${wk.id}`);
  j = await r.json();
  check(j.version === 2 && Object.keys(j.clips).join() === "takeoff" && j.clips.takeoff.t === "Start!" && j.built === null,
    "the old state is not kept");
  r = await json("POST", "/v1/works", { ...workBody, parent: wk.id });
  const fork = await r.json();
  r = await fetch(`${base}/v1/works/${fork.id}`);
  check(fork.id !== wk.id && fork.key !== wk.key && (await r.json()).parent === wk.id, "a new work from it: its own id and key, the parent noted");
  r = await json("POST", "/v1/works", { ...workBody, parent: "q".repeat(20) });
  r = await fetch(`${base}/v1/works/${(await r.json()).id}`);
  check((await r.json()).parent === null, "an unknown parent is dropped");

  /* a pack shared from a work: shared again by its owner in place, the old file served until the new one is there */
  const upload = async (id, token, bytes, size = 2 ** 20) => {
    let fin = false;
    for (let i = 0, n = Math.ceil(bytes.length / size); i < n; i++) {
      const u = await fetch(`${base}/v1/packs/${id}/file?part=${i}&parts=${n}`, { method: "PUT", headers: { "X-Upload-Token": token },
        body: bytes.subarray(i * size, (i + 1) * size) });
      fin = (await u.json()).done;
    }
    return fin;
  };
  const fileA = Buffer.alloc(1.5 * 2 ** 20, 1), fileB = Buffer.alloc(1.2 * 2 ** 20, 2);
  const workPack = { ...packBody, name: "Vario Pack", size: fileA.length, work: wk.id, key: wk.key };
  r = await createPack({ ...workPack, key: fork.key });
  check(r.status === 403, "sharing for a work with another key: 403");
  r = await createPack(workPack);
  const shared = await r.json();
  check(r.status === 201 && !shared.replaces && (await upload(shared.id, shared.upload_token, fileA)), "pack shared from a work");
  r = await fetch(`${base}/v1/works/${wk.id}`);
  j = await r.json();
  r = await fetch(`${base}/v1/packs/${shared.id}`);
  check(j.pack === shared.id && (await r.json()).work === wk.id, "the work knows its shared pack and the pack its work");
  r = await createPack({ ...workPack, name: "Vario Pack 2", size: fileB.length, settings: { pitch: 9 } });
  const again = await r.json();
  check(r.status === 201 && again.id === shared.id && again.replaces, "shared again by the owner: the same pack id");
  r = await fetch(`${base}/v1/packs/${shared.id}/file`);
  check(Buffer.from(await r.arrayBuffer()).equals(fileA), "the old file is served while the new one comes");
  check(await upload(shared.id, again.upload_token, fileB), "the new file uploaded");
  r = await fetch(`${base}/v1/packs/${shared.id}/file`);
  const nowFile = Buffer.from(await r.arrayBuffer());
  r = await fetch(`${base}/v1/packs?lang=de`);
  j = await r.json();
  check(nowFile.equals(fileB) && !stored.has(shared.id) && j.packs.length === 1 && j.packs[0].name === "Vario Pack 2" &&
    j.packs[0].settings.pitch === 9 && j.packs[0].size === fileB.length, "replaced in place: new file, name and settings; the old file deleted");
  r = await createPack({ ...packBody, name: "vario pack 2", size: fileA.length });
  check(r.status === 409, "its name stays taken for others");
  r = await fetch(`${base}/v1/admin/packs/${shared.id}`, { method: "DELETE", headers: { Authorization: `Bearer ${ADMIN}` } });
  check(r.ok && stored.size === 0, "the operator deletes it with its current file");

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
  await sleep(200);
  for (const f of [dbPath, `${dbPath}-wal`, `${dbPath}-shm`]) { try { fs.unlinkSync(f); } catch (e) { /* not there */ } }
}
if (failures) console.log(`\n--- relay log ---\n${log}`);
console.log(failures ? `${failures} FAILED` : "all passed");
process.exit(failures ? 1 : 0);
