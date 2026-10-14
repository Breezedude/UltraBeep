"use strict";

/*
 * Self-test of js/ble.js under Node with a mocked device: the upload paths (the windowed
 * writes without response of firmware 0.31, PUT ... ack; the fallback to writes with response
 * for older firmware; an error from the device in the middle; a device that stops
 * acknowledging) and the console lines on Status.
 *   node tools/configurator/tests/ble_test.js
 */
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");

const context = vm.createContext({
  console, setTimeout, clearTimeout, TextEncoder, TextDecoder, Promise, Error, Object, Math, Uint8Array,
  module: {}, navigator: {},
});
for (const f of ["crc32.js", "ble.js"]) {
  vm.runInContext(fs.readFileSync(path.join(__dirname, "..", "js", f), "utf8"), context, { filename: f });
}
const { BleFileClient } = context.module.exports;
const crc32Hex = vm.runInContext("crc32Hex", context);

let failed = 0;
const check = (ok, what) => {
  console.log(`${ok ? "ok  " : "FAIL"} ${what}`);
  if (!ok) failed++;
};

/* A device with the file service. opts: fast (the Data characteristic takes writes without
 * response and the firmware knows "ack"), plainReady (answers READY without parameters although
 * asked for ACKs), step/window, ackDelay (ms), errAt (ERR write once this many bytes arrived),
 * stallAt (no ACK after this many bytes). */
function makeDevice(opts) {
  const client = new BleFileClient(() => {});
  client.chunk = 244;
  const log = { cmds: [], noResp: 0, withResp: 0, maxInFlight: 0, received: [] };
  const emit = (text) => {
    client.onStatus({ target: { value: { buffer: new TextEncoder().encode(text + "\n").buffer } } });
  };
  let xf = null;
  client.cmd = {
    writeValueWithResponse: async (bytes) => {
      const cmd = new TextDecoder().decode(bytes);
      log.cmds.push(cmd);
      const m = cmd.match(/^PUT (\S+) (\d+) ([0-9A-F]{8})( ack)?$/);
      if (!m) {
        emit("ERR unknown command");
        return;
      }
      xf = { size: +m[2], crc: m[3], pos: 0, ack: !!m[4] && opts.fast && !opts.plainReady, lastAck: 0 };
      setTimeout(() => emit(xf.ack ? `READY ack=${opts.step} window=${opts.window}` : "READY"), 1);
    },
  };
  const receive = async (bytes) => {
    if (!xf) return;
    log.received.push(bytes);
    const before = xf.pos;
    xf.pos += bytes.length;
    log.maxInFlight = Math.max(log.maxInFlight, xf.pos - xf.lastAck);
    if (opts.errAt && xf.pos >= opts.errAt) {
      xf = null;
      emit("ERR write");
      return;
    }
    if (xf.ack && xf.pos < xf.size && Math.floor(xf.pos / opts.step) !== Math.floor(before / opts.step)) {
      if (opts.stallAt && xf.pos >= opts.stallAt) return;
      xf.lastAck = xf.pos;
      const p = xf.pos;
      setTimeout(() => emit(`ACK ${p}`), opts.ackDelay || 1);
    }
    if (xf.pos >= xf.size) {
      const all = new Uint8Array(xf.pos);
      let off = 0;
      for (const p of log.received) {
        all.set(p, off);
        off += p.length;
      }
      const crc = crc32Hex(all);
      const line = crc === xf.crc ? `DONE ${crc}` : "ERR crc";
      if (opts.doneDelay) setTimeout(() => emit(line), opts.doneDelay); else emit(line);
      xf = null;
    }
  };
  client.data = {
    properties: { writeWithoutResponse: !!opts.fast },
    writeValueWithoutResponse: async (bytes) => { log.noResp++; await receive(bytes); },
    writeValueWithResponse: async (bytes) => { log.withResp++; await receive(bytes); },
  };
  return { client, log };
}

function payload(n) {
  const b = new Uint8Array(n);
  let seed = 7;
  for (let i = 0; i < n; i++) {
    seed = (seed * 1103515245 + 12345) & 0x7fffffff;
    b[i] = seed >> 16;
  }
  return b;
}

async function main() {
  const size = 10000;
  const chunks = Math.ceil(size / 244);

  /* firmware 0.31: writes without response, paced by ACKs */
  {
    const { client, log } = makeDevice({ fast: true, step: 1024, window: 4096, ackDelay: 3 });
    const progress = [];
    await client.putBytes("update", payload(size), (done) => progress.push(done));
    check(log.cmds[0] === `PUT update ${size} ${crc32Hex(payload(size))} ack`, "fast: PUT asks for ACKs");
    check(log.noResp === chunks && log.withResp === 0, `fast: ${log.noResp} writes without response, none with`);
    check(log.maxInFlight <= 4096 + 244, `fast: at most ${log.maxInFlight} bytes beyond the last ACK`);
    check(progress.length === chunks && progress[progress.length - 1] === size, "fast: progress up to the size");
    check(client.onAck === null, "fast: ACK handler removed afterwards");
  }

  /* the device acknowledges the last chunks after the sender is done and reports DONE later still */
  {
    const { client, log } = makeDevice({ fast: true, step: 1024, window: 4096, ackDelay: 15, doneDelay: 60 });
    let err = null;
    await client.putBytes("update", payload(size)).catch((e) => { err = e; });
    check(!err, `late ACKs: ${err ? err.message : "upload completed"}`);
    check(log.noResp === chunks, "late ACKs: every chunk sent");
  }

  /* the ACKs pace the sender: slow ACKs make the upload wait */
  {
    const { client, log } = makeDevice({ fast: true, step: 1024, window: 4096, ackDelay: 30 });
    const t0 = Date.now();
    await client.putBytes("update", payload(20000));
    const ms = Date.now() - t0;
    check(ms >= 100, `pacing: 20 kB with 30 ms ACKs took ${ms} ms`);
    check(log.maxInFlight <= 4096 + 244, `pacing: at most ${log.maxInFlight} bytes beyond the last ACK`);
  }

  /* older firmware: no write without response, plain READY */
  {
    const { client, log } = makeDevice({ fast: false });
    await client.putBytes("config.ultrabip", payload(size));
    check(log.cmds[0] === `PUT config.ultrabip ${size} ${crc32Hex(payload(size))}`, "old: PUT without ack");
    check(log.withResp === chunks && log.noResp === 0, `old: ${log.withResp} writes with response`);
  }

  /* the characteristic takes writes without response but the firmware ignores "ack" */
  {
    const { client, log } = makeDevice({ fast: true, plainReady: true });
    await client.putBytes("update", payload(size));
    check(log.withResp === chunks && log.noResp === 0, "plain READY: falls back to writes with response");
  }

  /* an error from the device in the middle */
  {
    const { client, log } = makeDevice({ fast: true, step: 1024, window: 4096, ackDelay: 3, errAt: 3000 });
    let err = null;
    await client.putBytes("update", payload(size)).catch((e) => { err = e; });
    check(err && err.message === "PUT update: ERR write", `error: rejected with "${err && err.message}"`);
    check(log.noResp < chunks, `error: stopped after ${log.noResp} of ${chunks} chunks`);
  }

  /* the device stops acknowledging */
  {
    BleFileClient.ACK_TIMEOUT_MS = 50;
    const { client, log } = makeDevice({ fast: true, step: 1024, window: 4096, ackDelay: 3, stallAt: 2048 });
    let err = null;
    const t0 = Date.now();
    await client.putBytes("update", payload(size)).catch((e) => { err = e; });
    check(err && err.message.includes("stopped taking data"), `stall: rejected with "${err && err.message}"`);
    check(Date.now() - t0 < 1000, "stall: gave up after the ACK timeout");
    check(log.noResp < chunks, `stall: ${log.noResp} of ${chunks} chunks sent`);
  }

  /* AUTH (firmware 0.45): a known secret is accepted at once, a new one waits for the button; old
   * firmware has no AUTH; another configurator on the device refuses */
  {
    const { bleSecret } = context.module.exports;
    const device = (answers) => {
      const client = new BleFileClient(() => {});
      const emit = (text) => client.onStatus({ target: { value: { buffer: new TextEncoder().encode(text + "\n").buffer } } });
      const cmds = [];
      client.cmd = {
        writeValueWithResponse: async (bytes) => {
          cmds.push(new TextDecoder().decode(bytes));
          for (const [delay, line] of answers) setTimeout(() => emit(line), delay);
        },
      };
      return { client, cmds };
    };
    let pend = 0;
    let d = device([[1, "OK"]]);
    const known = await d.client.authorize("abcd1234", () => pend++);
    check(known === "ok" && pend === 0 && d.cmds[0] === "AUTH abcd1234", "AUTH: a known secret is accepted at once");
    d = device([[1, "PENDING"], [20, "AUTH ok"]]);
    const fresh = await d.client.authorize("abcd1234", () => pend++);
    check(fresh === "ok" && pend === 1, "AUTH: a new secret waits for the button, then ok");
    d = device([[1, "PENDING"], [20, "AUTH denied"]]);
    let err = null;
    await d.client.authorize("abcd1234", () => {}).catch((e) => { err = e; });
    check(!!err && /button/.test(err.message), `AUTH: denied rejects (${err && err.message})`);
    d = device([[1, "ERR unknown command"]]);
    check(await d.client.authorize("abcd1234") === "old", "AUTH: firmware without AUTH");
    d = device([[1, "ERR busy"]]);
    err = null;
    await d.client.authorize("abcd1234").catch((e) => { err = e; });
    check(!!err && /another configurator/.test(err.message), "AUTH: busy rejects with the reason");
    check(/^[a-z0-9_]{8}$/.test(bleSecret()), `secret: 8 plain characters (${bleSecret()})`);
  }

  /* console output on Status (CONSOLE on): CON/CONP lines go to onConsole with their leading spaces,
   * never to a waiting request; a character split over two notifications arrives whole */
  {
    const client = new BleFileClient(() => {});
    const notify = (bytes) => client.onStatus({ target: { value: { buffer: bytes.buffer } } });
    const enc = (text) => new TextEncoder().encode(text);
    const got = [];
    client.onConsole = (text, ends) => got.push([text, ends]);
    const reply = client.nextStatus(1000);
    notify(enc("CON   status   device\nCONP > \n"));
    notify(enc("OK\n"));
    const line = await reply;
    check(line === "OK", `console: the request gets its reply, not console output ("${line}")`);
    check(JSON.stringify(got) === JSON.stringify([["  status   device", true], ["> ", false]]),
      `console: text with its spaces and line ends: ${JSON.stringify(got)}`);
    const umlauts = enc("CON grüße\n");
    notify(umlauts.slice(0, 7)); /* between the two bytes of the u-umlaut */
    notify(umlauts.slice(7));
    check(got.length === 3 && got[2][0] === "grüße", `console: a split character arrives whole: ${JSON.stringify(got[2])}`);
  }

  console.log(failed ? `${failed} FAILED` : "all ok");
  process.exit(failed ? 1 : 0);
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
