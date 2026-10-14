"use strict";

/* Client for the device's Bluetooth file service (docs/ble-file-service.md). */
const BLE_SERVICE = "4a800001-c6ce-4362-96ab-eb24df2be84a";
const BLE_COMMAND = "4a800002-c6ce-4362-96ab-eb24df2be84a";
const BLE_DATA = "4a800003-c6ce-4362-96ab-eb24df2be84a";
const BLE_STATUS = "4a800004-c6ce-4362-96ab-eb24df2be84a";

/* The secret this browser presents to the device (AUTH, firmware 0.45): made up once and kept per
 * browser. A device that does not know it asks the pilot for a button press, then remembers it. */
function bleSecret() {
  const key = "cfg.bleSecret";
  let s = null;
  try { s = localStorage.getItem(key); } catch (e) { s = null; }
  if (!s || !/^[a-z0-9_]{8}$/.test(s)) {
    const chars = "abcdefghijklmnopqrstuvwxyz0123456789";
    const r = new Uint8Array(8);
    if (typeof crypto !== "undefined" && crypto.getRandomValues) crypto.getRandomValues(r);
    else for (let i = 0; i < r.length; i++) r[i] = Math.floor(Math.random() * 256);
    s = Array.from(r, (b) => chars[b % chars.length]).join("");
    try { localStorage.setItem(key, s); } catch (e) { /* no storage: a new secret per page load */ }
  }
  return s;
}

class BleFileClient {
  constructor(onDisconnect) {
    this.onDisconnect = onDisconnect;
    this.device = null;
    this.waiters = [];
    this.statusText = "";
    this.sink = null;
    this.chunk = 20;
    this.info = {};
    this.queue = Promise.resolve();
    this.onAck = null; /* during a PUT with ACK lines */
    this.onConsole = null; /* (text, lineEnds) while the console is on */
    this.decoder = new TextDecoder(); /* streaming: a character may be split over two notifications */
  }

  /* One operation at a time: replies on Status are matched in order, a LIVE poll must not
   * land between the OK and DONE of a download. */
  exclusive(fn) {
    const run = this.queue.then(fn, fn);
    this.queue = run.catch(() => {});
    return run;
  }

  /* Live values: {st, p, t, alt, vz, acc, rest, fused, fix, sats, hdop, gs, lat, lon, vbat, rec, piezo, quiet, taps} */
  live() {
    return this.exclusive(async () => {
      const line = await this.request("LIVE", ["LIVE"], 3000);
      return Object.fromEntries(line.split(" ").slice(1).map((kv) => kv.split("=")));
    });
  }

  /* Accelerometer calibration: {n, still, ax, ay, az (raw mean of the last second), ox..sz (in use)} */
  cal() {
    return this.exclusive(async () => {
      const line = await this.request("CAL", ["CAL"], 3000);
      return Object.fromEntries(line.split(" ").slice(1).map((kv) => kv.split("=")));
    });
  }

  /* [ox, oy, oz] mm/s² and [sx, sy, sz] ppm: applied and saved on the device. */
  calSet(values) {
    return this.exclusive(() => this.request(`CAL set ${values.join(" ")}`, ["OK"], 5000));
  }

  calReset() {
    return this.exclusive(() => this.request("CAL reset", ["OK"], 5000));
  }

  /* Test commands (TONE, SPK, SAY, VARIO, SIM, TONES, PIEZO, TEST off); rejects with the device's ERR text. */
  test(text) {
    return this.exclusive(() => this.request(text, ["OK"], 3000));
  }

  /* The device's debug console: onOutput(text, lineEnds) gets the log and the answers; the device
   * starts with the log so far. Rejects with "unknown command" on firmware without it. */
  consoleOpen(onOutput) {
    this.onConsole = onOutput;
    return this.exclusive(() => this.request("CONSOLE on", ["OK"], 3000));
  }

  consoleClose() {
    this.onConsole = null;
    if (!this.connected) return Promise.resolve();
    return this.exclusive(() => this.request("CONSOLE off", ["OK"], 3000)).catch(() => {});
  }

  /* A command line; the device echoes it and answers on the console, there is no reply. */
  consoleSend(line) {
    return this.exclusive(() => this.send(`CON ${line}`));
  }

  get connected() {
    return !!(this.device && this.device.gatt.connected);
  }

  /* The Data characteristic takes writes without response, so an upload can
   * send several chunks per connection interval; the device paces it with ACK lines. */
  get fastWrites() {
    return !!(this.data && this.data.properties && this.data.properties.writeWithoutResponse);
  }

  /* UltraBip devices the browser was allowed to use before (Chrome's persistent permissions). */
  static async knownDevices() {
    if (!navigator.bluetooth || !navigator.bluetooth.getDevices) return [];
    try {
      return (await navigator.bluetooth.getDevices()).filter((d) => d.name && d.name.startsWith("UltraBip"));
    } catch (e) {
      return [];
    }
  }

  /* Connects to a device the page already knows (knownDevices), or asks the user for one. */
  async connect(device = null) {
    if (!navigator.bluetooth) throw new Error("This browser has no Web Bluetooth (use Chrome or Edge).");
    this.device = device || await navigator.bluetooth.requestDevice({
      filters: [{ namePrefix: "UltraBip" }],
      optionalServices: [BLE_SERVICE],
    });
    this.device.addEventListener("gattserverdisconnected", () => this.onDisconnect && this.onDisconnect());
    const server = await this.device.gatt.connect();
    const service = await server.getPrimaryService(BLE_SERVICE);
    this.cmd = await service.getCharacteristic(BLE_COMMAND);
    this.data = await service.getCharacteristic(BLE_DATA);
    this.status = await service.getCharacteristic(BLE_STATUS);
    this.data.addEventListener("characteristicvaluechanged", (e) => {
      if (this.sink) this.sink(new Uint8Array(e.target.value.buffer));
    });
    this.status.addEventListener("characteristicvaluechanged", (e) => this.onStatus(e));
    await this.data.startNotifications();
    await this.status.startNotifications();
    const line = await this.request("INFO", ["INFO"]);
    this.info = Object.fromEntries(line.split(" ").slice(1).map((kv) => kv.split("=")));
    const mtu = parseInt(this.info.mtu || "23", 10);
    this.chunk = Math.max(20, Math.min(244, mtu - 3));
    return this.info;
  }

  disconnect() {
    if (this.connected) this.device.gatt.disconnect();
  }

  /* AUTH: the device takes commands only from a browser it knows. Resolves "ok" at once for a known
   * secret; for a new one the device answers PENDING, onPending() is called and the pilot has 60 s to
   * press the button ("AUTH ok"). Resolves "old" for firmware without AUTH (before 0.45, no
   * authorisation). Rejects when the device refused: another configurator is connected, or the button
   * was not pressed in time. */
  async authorize(secret, onPending) {
    let line;
    try {
      line = await this.request(`AUTH ${secret}`, ["OK", "PENDING"], 5000);
    } catch (e) {
      if (/unknown command/.test(e.message)) return "old";
      if (/busy/.test(e.message)) throw new Error("The device is already connected to another configurator.");
      throw e;
    }
    if (line.startsWith("OK")) return "ok";
    if (onPending) onPending();
    const result = await this.nextStatus(BleFileClient.AUTH_TIMEOUT_MS);
    if (result === "AUTH ok") return "ok";
    throw new Error(result === "AUTH denied"
      ? "The device did not allow this browser: no button press within 60 s."
      : `AUTH: ${result}`);
  }

  onStatus(e) {
    this.statusText += this.decoder.decode(new Uint8Array(e.target.value.buffer), { stream: true });
    let idx;
    while ((idx = this.statusText.indexOf("\n")) >= 0) {
      const raw = this.statusText.slice(0, idx);
      this.statusText = this.statusText.slice(idx + 1);
      /* console output (CONSOLE on): never the reply to a command; leading spaces count */
      if (raw.startsWith("CON ") || raw.startsWith("CONP ")) {
        const ends = raw.startsWith("CON ");
        if (this.onConsole) this.onConsole(raw.slice(ends ? 4 : 5), ends);
        continue;
      }
      const line = raw.trim();
      if (line.startsWith("ACK ")) {
        /* never the reply to a command: one that arrives after the upload's last chunk is dropped */
        if (this.onAck) this.onAck(parseInt(line.slice(4), 10) || 0);
        continue;
      }
      const w = this.waiters.shift();
      if (w) w(line);
    }
  }

  nextStatus(timeoutMs) {
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        const i = this.waiters.indexOf(done);
        if (i >= 0) this.waiters.splice(i, 1);
        reject(new Error("No answer from the device"));
      }, timeoutMs);
      const done = (line) => {
        clearTimeout(timer);
        resolve(line);
      };
      this.waiters.push(done);
    });
  }

  async send(text) {
    await this.cmd.writeValueWithResponse(new TextEncoder().encode(text));
  }

  /* Sends a command and returns the first reply line; rejects on ERR or unexpected replies. */
  async request(text, expect, timeoutMs = 5000) {
    const reply = this.nextStatus(timeoutMs);
    await this.send(text);
    const line = await reply;
    if (!expect.some((p) => line.startsWith(p))) throw new Error(`${text.split(" ")[0]}: ${line}`);
    return line;
  }

  /* Runs GET or LIST: "OK <size>", the bytes on Data, "DONE <crc>"; onProgress(received, size). */
  async transfer(command, onProgress) {
    const parts = [];
    let received = 0;
    let size = 0;
    this.sink = (bytes) => {
      parts.push(bytes);
      received += bytes.length;
      if (onProgress) onProgress(received, size);
    };
    try {
      const ok = await this.request(command, ["OK"]);
      size = parseInt(ok.split(" ")[1], 10);
      const done = await this.nextStatus(20000 + size);
      if (!done.startsWith("DONE")) throw new Error(`${command}: ${done}`);
      const bytes = new Uint8Array(received);
      let off = 0;
      for (const p of parts) {
        bytes.set(p, off);
        off += p.length;
      }
      if (bytes.length !== size || crc32Hex(bytes) !== done.split(" ")[1]) {
        throw new Error(`${command}: transfer damaged`);
      }
      return bytes;
    } finally {
      this.sink = null;
    }
  }

  async getFile(name) {
    return new TextDecoder().decode(await this.exclusive(() => this.transfer(`GET ${name}`)));
  }

  /* Files in tracks_IGC or tracks_KML and their year folders: [{name, size}] with names such as
   * "2026/2026-10-08-XSD-UB1A2B-01.igc", newest first (by the file name, which starts with the date). */
  async listTracks(dir) {
    const text = new TextDecoder().decode(await this.exclusive(() => this.transfer(`LIST ${dir}`)));
    return text.split("\n").filter(Boolean).map((line) => {
      const i = line.lastIndexOf(" ");
      return { name: line.slice(0, i), size: parseInt(line.slice(i + 1), 10) };
    }).sort((a, b) => b.name.split("/").pop().localeCompare(a.name.split("/").pop()));
  }

  async getTrack(dir, name, onProgress) {
    return this.exclusive(() => this.transfer(`GET ${dir}/${name}`, onProgress));
  }

  putFile(name, text) {
    return this.exclusive(() => this.putFileNow(name, new TextEncoder().encode(text)));
  }

  /* Binary file, e.g. a voice clip (voices/custom/<name>.qoa); onProgress(sent, total). */
  putBytes(name, bytes, onProgress) {
    return this.exclusive(() => this.putFileNow(name, bytes, onProgress));
  }

  /* With "ack" the device answers "READY ack=<step> window=<bytes>" and sends
   * "ACK <received>" every <step> bytes: the chunks go out without response, at most <window>
   * bytes beyond the last ACK. Older firmware answers a plain READY: one write with response
   * per chunk, as before. */
  async putFileNow(name, bytes, onProgress) {
    const fast = this.fastWrites;
    const ready = await this.request(`PUT ${name} ${bytes.length} ${crc32Hex(bytes)}${fast ? " ack" : ""}`, ["READY"]);
    const opt = Object.fromEntries(ready.split(" ").slice(1).map((kv) => kv.split("=")));
    const window = fast ? parseInt(opt.window || "0", 10) || 0 : 0;
    let acked = 0;
    let finished = null; /* the DONE or ERR line, once it arrived */
    let wake = () => {};
    this.onAck = (pos) => { acked = pos; wake(); };
    const done = this.nextStatus(20000 + bytes.length);
    done.then((line) => { finished = line; wake(); }, () => {});
    let line;
    try {
      for (let off = 0; off < bytes.length && !finished; off += this.chunk) {
        const piece = bytes.slice(off, off + this.chunk);
        if (window) {
          while (off - acked >= window && !finished) {
            const before = acked;
            await new Promise((resolve) => { wake = resolve; setTimeout(resolve, BleFileClient.ACK_TIMEOUT_MS); });
            if (acked === before && !finished) throw new Error(`PUT ${name}: the device stopped taking data`);
          }
          if (finished) break;
          await this.data.writeValueWithoutResponse(piece);
        } else {
          await this.data.writeValueWithResponse(piece);
        }
        if (onProgress) onProgress(Math.min(bytes.length, off + this.chunk), bytes.length);
      }
      line = await done; /* ACKs for the last chunks still arrive before DONE */
    } finally {
      this.onAck = null;
    }
    if (!line.startsWith("DONE")) throw new Error(`PUT ${name}: ${line}`);
  }

  /* The image sent as "update" is installed; the device resets and disconnects. */
  installUpdate() {
    return this.exclusive(() => this.request("UPDATE", ["OK"], 10000));
  }

  /* Removes a clip in voices/custom. */
  deleteFile(name) {
    return this.exclusive(() => this.request(`DEL ${name}`, ["OK"], 5000));
  }
}

/* How long an upload waits for the next ACK before it gives up. */
BleFileClient.ACK_TIMEOUT_MS = 10000;
/* How long a new browser waits for the pilot's button press (the device's window is 60 s). */
BleFileClient.AUTH_TIMEOUT_MS = 70000;

if (typeof module !== "undefined") module.exports = { BleFileClient, bleSecret };
