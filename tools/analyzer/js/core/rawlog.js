/*
 * Reader of the raw sensor logs (docs/raw-log-format.md). No DOM: the browser UI and cli.mjs share it.
 *
 * A log becomes typed arrays per stream (times in ms of the device clock) plus the text records:
 *   baro: t, pRaw, tRaw, p [Pa], temp [°C], alt and vz of the filter the device used
 *   imu:  t, raw (6 int16 per sample: accel x y z, gyro x y z)
 *   nmea, events: [{t, text}]
 *   calib: [{t, offset[3] m/s², scale[3]}] accelerometer calibration in use
 *   settings: [{t, group, values: {key: value}, text}] the settings the device wrote (firmware 0.51)
 */

export const RECORD = { HEADER: 0, BARO: 1, IMU: 2, NMEA: 3, EVENT: 4, CALIB: 5, SETTINGS: 6 };
export const G0 = 9.80665;

const HEADER_SIZE = 84;
const BARO_SIZE = 28;
const IMU_SIZE = 16;
const CALIB_SIZE = 28;

const decoder = new TextDecoder("latin1");

function cstring(bytes, offset, length) {
  let end = offset;
  while (end < offset + length && bytes[end] !== 0) end++;
  return decoder.decode(bytes.subarray(offset, end));
}

function parseHeader(dv, bytes, o) {
  const coef = [];
  for (let i = 0; i < 9; i++) coef.push(dv.getInt32(o + 24 + 4 * i, true));
  return {
    magic: cstring(bytes, o, 4),
    format: dv.getUint8(o + 4),
    baroRate: dv.getUint8(o + 5),
    imuRate: dv.getUint16(o + 6, true),
    accelLsbG: dv.getFloat32(o + 8, true),
    gyroLsbDps: dv.getFloat32(o + 12, true),
    kp: dv.getFloat32(o + 16, true),
    kt: dv.getFloat32(o + 20, true),
    coef,
    unixTime: dv.getUint32(o + 60, true),
    tMs: dv.getUint32(o + 64, true),
    firmware: cstring(bytes, o + 68, 16),
  };
}

/*
 * Parses a whole file (ArrayBuffer or Uint8Array). Damaged records are counted, not fatal: a log
 * cut by a power loss ends with a partial record.
 */
export function parseRawLog(input, name = "") {
  const bytes = input instanceof Uint8Array ? input : new Uint8Array(input);
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const len = bytes.length;

  // pass 1: count, so the streams get typed arrays of the right size
  let nBaro = 0;
  let nImu = 0;
  const stats = { records: 0, headers: 0, unknown: 0, badLength: 0, truncatedBytes: 0 };
  let pos = 0;
  while (pos + 2 <= len) {
    const type = bytes[pos];
    const n = bytes[pos + 1];
    if (pos + 2 + n > len) {
      stats.truncatedBytes = len - pos;
      break;
    }
    if (type === RECORD.BARO && n === BARO_SIZE) nBaro++;
    else if (type === RECORD.IMU && n === IMU_SIZE) nImu++;
    pos += 2 + n;
  }

  const baro = {
    n: nBaro,
    t: new Float64Array(nBaro),
    pRaw: new Int32Array(nBaro),
    tRaw: new Int32Array(nBaro),
    p: new Float64Array(nBaro),
    temp: new Float32Array(nBaro),
    alt: new Float32Array(nBaro),
    vz: new Float32Array(nBaro),
  };
  const imu = { n: nImu, t: new Float64Array(nImu), raw: new Int16Array(nImu * 6) };
  const nmea = [];
  const events = [];
  const calib = [];
  const settings = [];
  let header = null;

  // pass 2: fill
  let ib = 0;
  let ii = 0;
  pos = 0;
  while (pos + 2 <= len) {
    const type = bytes[pos];
    const n = bytes[pos + 1];
    if (pos + 2 + n > len) break;
    const o = pos + 2;
    stats.records++;
    if (type === RECORD.HEADER) {
      if (n === HEADER_SIZE) {
        stats.headers++;
        if (!header) header = parseHeader(dv, bytes, o);
      } else stats.badLength++;
    } else if (type === RECORD.BARO) {
      if (n === BARO_SIZE) {
        baro.t[ib] = dv.getUint32(o, true);
        baro.pRaw[ib] = dv.getInt32(o + 4, true);
        baro.tRaw[ib] = dv.getInt32(o + 8, true);
        baro.p[ib] = dv.getFloat32(o + 12, true);
        baro.temp[ib] = dv.getFloat32(o + 16, true);
        baro.alt[ib] = dv.getFloat32(o + 20, true);
        baro.vz[ib] = dv.getFloat32(o + 24, true);
        ib++;
      } else stats.badLength++;
    } else if (type === RECORD.IMU) {
      if (n === IMU_SIZE) {
        imu.t[ii] = dv.getUint32(o, true);
        for (let k = 0; k < 6; k++) imu.raw[ii * 6 + k] = dv.getInt16(o + 4 + 2 * k, true);
        ii++;
      } else stats.badLength++;
    } else if (type === RECORD.CALIB) {
      if (n === CALIB_SIZE) {
        const offset = [0, 1, 2].map((k) => dv.getFloat32(o + 4 + 4 * k, true));
        const scale = [0, 1, 2].map((k) => dv.getFloat32(o + 16 + 4 * k, true));
        calib.push({ t: dv.getUint32(o, true), offset, scale });
      } else stats.badLength++;
    } else if (type === RECORD.SETTINGS) {
      if (n >= 4) {
        const text = decoder.decode(bytes.subarray(o + 4, o + n));
        const [group, ...pairs] = text.split(" ");
        const values = {};
        for (const p of pairs) {
          const i = p.indexOf("=");
          if (i > 0) values[p.slice(0, i)] = p.slice(i + 1);
        }
        settings.push({ t: dv.getUint32(o, true), group, values, text });
      } else stats.badLength++;
    } else if (type === RECORD.NMEA || type === RECORD.EVENT) {
      if (n >= 4) {
        const rec = { t: dv.getUint32(o, true), text: decoder.decode(bytes.subarray(o + 4, o + n)) };
        (type === RECORD.NMEA ? nmea : events).push(rec);
      } else stats.badLength++;
    } else {
      stats.unknown++;
    }
    pos += 2 + n;
  }

  if (!header) {
    throw new Error("No header record: not a raw log of UltraBeep (0:/raw/RAWnnnnn.BIN)");
  }
  if (header.magic !== "UBRW") {
    throw new Error(`Unknown header magic "${header.magic}"`);
  }

  // time origin: the header's start stamp, or the first record if that is earlier
  let t0 = header.tMs;
  if (baro.n && baro.t[0] < t0) t0 = baro.t[0];
  if (imu.n && imu.t[0] < t0) t0 = imu.t[0];
  let tEnd = t0;
  if (baro.n) tEnd = Math.max(tEnd, baro.t[baro.n - 1]);
  if (imu.n) tEnd = Math.max(tEnd, imu.t[imu.n - 1]);
  for (const r of nmea) if (r.t > tEnd) tEnd = r.t;

  return { name, size: len, header, baro, imu, nmea, events, calib, settings, stats, t0, tEnd };
}

/*
 * Accelerometer [m/s²] and gyro [rad/s] of IMU sample i. With cal ({offset, scale}) the
 * accelerometer is calibrated as the firmware does before its filters: a = (raw - offset) * scale.
 */
export function imuSample(log, i, a, w, cal = null) {
  const h = log.header;
  const ka = G0 / h.accelLsbG;
  const kg = Math.PI / 180 / h.gyroLsbDps;
  const r = log.imu.raw;
  const o = i * 6;
  a[0] = r[o] * ka;
  a[1] = r[o + 1] * ka;
  a[2] = r[o + 2] * ka;
  w[0] = r[o + 3] * kg;
  w[1] = r[o + 4] * kg;
  w[2] = r[o + 5] * kg;
  if (cal) {
    for (let k = 0; k < 3; k++) a[k] = (a[k] - cal.offset[k]) * cal.scale[k];
  }
}

/* The calibration in use at device time tMs (the last record before it), or null. */
export function calibrationAt(log, tMs) {
  let c = null;
  for (const x of log.calib) {
    if (x.t <= tMs) c = x;
    else break;
  }
  return c;
}

/* Pressure altitude of the standard atmosphere, as firmware/src/app/sensors.c (isa_altitude). */
export function isaAltitude(pressurePa) {
  return 44330.77 * (1 - Math.pow(pressurePa / 101325, 0.190263));
}

/* UTC of a device time stamp, if the log knows the clock (header unix time at header.tMs). */
export function utcOf(log, tMs) {
  if (!log.header.unixTime) return null;
  return log.header.unixTime * 1000 + (tMs - log.header.tMs);
}
