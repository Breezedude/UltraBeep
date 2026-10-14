/*
 * IGC files: the standard B records (one fix per second with the pressure and GNSS altitudes),
 * the headers, and the records UltraBeep adds (LVBV data records and markers,
 * docs/igc-vbv-records.md). The stock firmware's LXSD lines are counted and skipped. No DOM.
 *
 * Result: {name, kind: "igc", header, n, utc[] (ms), t[] (s since the first fix), lat[], lon[],
 *   altPress[], altGps[], valid[] (1 = 'A'), fxa[], siu[] (NaN where absent),
 *   vbv: {n, t, utc, lat, lon, hEll, velN, velE, velD, baroAlt, hMsl, vario, alt, accel, sats,
 *   status} or null, markers: [{t, utc, kind, text, kmh, deg}], stats}
 */

const VBV_PREFIX = "LVBV";
const VBV_RECORD_CHARS = 72; /* "LVBV" + 68 Base64 characters of a 50-byte sample */

function base64Bytes(s) {
  const bin = atob(s);
  const out = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
  return out;
}

function dateFromHeader(line) {
  // HFDTE220726 or HFDTEDATE:220726,01
  const m = line.match(/(\d{2})(\d{2})(\d{2})/);
  if (!m) return null;
  return Date.UTC(2000 + Number(m[3]), Number(m[2]) - 1, Number(m[1]));
}

function headerValue(line) {
  const i = line.indexOf(":");
  return i >= 0 ? line.slice(i + 1).trim() : line.slice(5).trim();
}

/* I record: "I023638FXA3940SIU" -> [{code, start, end}] (1-based, inclusive, as in the file) */
function parseExtensions(line) {
  const n = Number(line.slice(1, 3));
  const out = [];
  for (let k = 0; k < n; k++) {
    const o = 3 + k * 7;
    const start = Number(line.slice(o, o + 2)), end = Number(line.slice(o + 2, o + 4));
    const code = line.slice(o + 4, o + 7);
    if (start && end) out.push({ code, start, end });
  }
  return out;
}

function secondsOfDay(hhmmss) {
  return Number(hhmmss.slice(0, 2)) * 3600 + Number(hhmmss.slice(2, 4)) * 60 + Number(hhmmss.slice(4, 6));
}

export function isIgcText(text) {
  const head = text.slice(0, 200);
  return /^A[A-Z0-9]{3}/.test(head) || /\nB\d{6}\d{7}[NS]\d{8}[EW]/.test(head) || /\nHFDTE/.test(head);
}

export function parseIgc(text, name = "") {
  const lines = text.split(/\r?\n/);
  const header = { manufacturer: "", loggerId: "", date: null, pilot: "", glider: "", firmware: "", hardware: "", frType: "", gps: "", timezone: null, gpsPeriodMs: null };
  const stats = { b: 0, vbv: 0, lxsd: 0, bad: 0, markers: 0 };
  let ext = [];
  let count = 0;
  for (const l of lines) if (l[0] === "B" && l.length >= 35) count++;
  const n = count;
  const utc = new Float64Array(n), lat = new Float64Array(n), lon = new Float64Array(n);
  const altPress = new Float32Array(n), altGps = new Float32Array(n);
  const valid = new Uint8Array(n);
  const fxa = new Float32Array(n).fill(NaN), siu = new Float32Array(n).fill(NaN);

  const vb = { tRec: [], lat: [], lon: [], hEll: [], velN: [], velE: [], velD: [], baroAlt: [], hMsl: [], vario: [], alt: [], accel: [], sats: [], status: [] };
  const anchors = []; /* [vbv index, utc ms]: the B record after a sample carries its UTC */
  const markers = [];
  let dayMs = null;
  let lastTod = -1;
  let dayAdd = 0;
  let ib = 0;
  let pendingVbv = -1;

  const utcOfTod = (tod) => {
    if (lastTod >= 0 && tod < lastTod - 43200) dayAdd += 86400000;
    lastTod = tod;
    return (dayMs ?? 0) + dayAdd + tod * 1000;
  };

  for (const line of lines) {
    const c = line[0];
    if (c === "B") {
      if (line.length < 35) { stats.bad++; continue; }
      const tod = secondsOfDay(line.slice(1, 7));
      const la = Number(line.slice(7, 9)) + Number(line.slice(9, 14)) / 60000;
      const lo = Number(line.slice(15, 18)) + Number(line.slice(18, 23)) / 60000;
      utc[ib] = utcOfTod(tod);
      lat[ib] = line[14] === "S" ? -la : la;
      lon[ib] = line[23] === "W" ? -lo : lo;
      valid[ib] = line[24] === "A" ? 1 : 0;
      altPress[ib] = Number(line.slice(25, 30));
      altGps[ib] = Number(line.slice(30, 35));
      for (const e of ext) {
        const v = Number(line.slice(e.start - 1, e.end));
        if (e.code === "FXA") fxa[ib] = v;
        else if (e.code === "SIU") siu[ib] = v;
      }
      if (pendingVbv >= 0) anchors.push([pendingVbv, utc[ib]]);
      pendingVbv = -1;
      ib++;
      stats.b++;
    } else if (c === "L") {
      if (line.startsWith(VBV_PREFIX)) {
        const body = line.slice(4);
        if (line.length === VBV_RECORD_CHARS && /^[A-Za-z0-9+/]+=*$/.test(body)) {
          let bytes;
          try { bytes = base64Bytes(body); } catch { stats.bad++; continue; }
          if (bytes.length !== 50) { stats.bad++; continue; }
          const dv = new DataView(bytes.buffer);
          vb.tRec.push(dv.getUint32(0, true));
          vb.lat.push(dv.getFloat32(4, true));
          vb.lon.push(dv.getFloat32(8, true));
          vb.hEll.push(dv.getFloat32(12, true));
          vb.velN.push(dv.getFloat32(16, true));
          vb.velE.push(dv.getFloat32(20, true));
          vb.velD.push(dv.getFloat32(24, true));
          vb.baroAlt.push(dv.getFloat32(28, true));
          vb.hMsl.push(dv.getFloat32(32, true));
          vb.vario.push(dv.getFloat32(36, true));
          vb.alt.push(dv.getFloat32(40, true));
          vb.accel.push(dv.getFloat32(44, true));
          vb.sats.push(bytes[48]);
          vb.status.push(bytes[49]);
          pendingVbv = vb.tRec.length - 1;
          stats.vbv++;
        } else {
          const m = body.match(/^(TAKEOFF|LANDING|WIND)(\d{6})(?:\s+([\d.]+)\s+(\d+))?/);
          if (m) {
            const mk = { kind: m[1].toLowerCase(), utc: utcOfTod(secondsOfDay(m[2])), text: m[1].toLowerCase() };
            if (m[1] === "WIND") {
              mk.kmh = Number(m[3]);
              mk.deg = Number(m[4]);
              mk.text = `wind ${mk.kmh} km/h from ${mk.deg}°`;
            }
            markers.push(mk);
            stats.markers++;
          }
        }
      } else if (line.startsWith("LXSD")) stats.lxsd++;
      else if (/^LMMMGPSPERIOD(\d+)MSEC/.test(line)) header.gpsPeriodMs = Number(line.match(/(\d+)MSEC/)[1]);
    } else if (c === "H") {
      const code = line.slice(2, 5);
      if (code === "DTE") { header.date = dateFromHeader(line); dayMs = header.date; }
      else if (code === "PLT") header.pilot = headerValue(line);
      else if (code === "GTY") header.glider = headerValue(line);
      else if (code === "RFW") header.firmware = headerValue(line);
      else if (code === "RHW") header.hardware = headerValue(line);
      else if (code === "FTY") header.frType = headerValue(line);
      else if (code === "GPS") header.gps = headerValue(line);
      else if (code === "TZN") header.timezone = Number(headerValue(line));
    } else if (c === "I") {
      ext = parseExtensions(line);
    } else if (c === "A") {
      header.manufacturer = line.slice(1, 4);
      header.loggerId = line.slice(4).trim();
    }
  }
  if (!n) throw new Error("No B records: not an IGC file");

  // VBV samples: UTC from the anchors (the B record after a sample), interpolated in recorder time
  let vbv = null;
  if (vb.tRec.length) {
    const m = vb.tRec.length;
    const vutc = new Float64Array(m);
    if (anchors.length) {
      let a = 0;
      for (let i = 0; i < m; i++) {
        while (a + 1 < anchors.length && anchors[a + 1][0] <= i) a++;
        const [ia, ua] = anchors[a];
        const next = anchors[a + 1];
        let slope = 1;
        if (next && next[0] > ia && vb.tRec[next[0]] !== vb.tRec[ia]) slope = (next[1] - ua) / (vb.tRec[next[0]] - vb.tRec[ia]);
        else if (!next && a > 0) {
          const [ip, up] = anchors[a - 1];
          if (vb.tRec[ia] !== vb.tRec[ip]) slope = (ua - up) / (vb.tRec[ia] - vb.tRec[ip]);
        }
        vutc[i] = ua + (vb.tRec[i] - vb.tRec[ia]) * slope;
      }
    } else {
      const take = markers.find((k) => k.kind === "takeoff");
      const base = take ? take.utc : utc[0];
      for (let i = 0; i < m; i++) vutc[i] = base + (vb.tRec[i] - 1000);
    }
    vbv = { n: m, tRec: Float64Array.from(vb.tRec), utc: vutc };
    for (const k of ["lat", "lon", "hEll", "velN", "velE", "velD", "baroAlt", "hMsl", "vario", "alt", "accel"]) vbv[k] = Float32Array.from(vb[k]);
    vbv.sats = Uint8Array.from(vb.sats);
    vbv.status = Uint8Array.from(vb.status);
  }

  const utc0 = Math.min(utc[0], vbv ? vbv.utc[0] : Infinity);
  const t = new Float64Array(n);
  for (let i = 0; i < n; i++) t[i] = (utc[i] - utc0) / 1000;
  if (vbv) {
    vbv.t = new Float64Array(vbv.n);
    for (let i = 0; i < vbv.n; i++) vbv.t[i] = (vbv.utc[i] - utc0) / 1000;
  }
  for (const mk of markers) mk.t = (mk.utc - utc0) / 1000;

  return { name, kind: "igc", header, n, utc0, utc, t, lat, lon, altPress, altGps, valid, fxa, siu, vbv, markers, stats };
}
