/*
 * NMEA of the raw log -> GNSS fixes, satellite signal summaries and module messages.
 * Epoch assembly as firmware/src/app/nmea.c: GGA starts an epoch, the GSA sentences give the best
 * fix type, the RMC of the same time completes it. The vertical speed comes from the ellipsoid
 * height over at least 0.9 s (as the firmware at 10 Hz).
 */

const KNOTS = 0.514444;
const SPAN_MS = 900;
const MAX_GAP_MS = 2500;
const GOOD_SNR = 30;

function checksumOk(s) {
  const star = s.lastIndexOf("*");
  if (star < 1) return false;
  let cs = 0;
  for (let i = 1; i < star; i++) cs ^= s.charCodeAt(i);
  return parseInt(s.slice(star + 1, star + 3), 16) === cs;
}

function todMs(v) {
  if (!v || v.length < 6) return null;
  const h = +v.slice(0, 2), m = +v.slice(2, 4), s = +v.slice(4, 6);
  if (!(h <= 23 && m <= 59 && s <= 60)) return null;
  let frac = 0;
  if (v[6] === ".") frac = Math.round(parseFloat("0" + v.slice(6)) * 1000);
  return ((h * 60 + m) * 60 + s) * 1000 + frac;
}

function coord(v, hemi) {
  if (!v) return NaN;
  const dot = v.indexOf(".");
  const intLen = dot < 0 ? v.length : dot;
  const deg = parseInt(v.slice(0, intLen - 2), 10);
  const min = parseFloat(v.slice(intLen - 2));
  const d = deg + min / 60;
  return hemi === "S" || hemi === "W" ? -d : d;
}

function dateMs(d) {
  if (!d || d.length !== 6) return null;
  const day = +d.slice(0, 2), month = +d.slice(2, 4), year = +d.slice(4, 6);
  if (!(day >= 1 && month >= 1 && month <= 12 && year >= 24 && year <= 79)) return null; // 06.01.80: no time yet
  return Date.UTC(2000 + year, month - 1, day);
}

/*
 * lines: [{t, text}] in file order. Returns
 *   fixes: column arrays (one entry per completed epoch)
 *   sat:   one entry per epoch that had GSV sentences
 *   messages: module answers and other non-NMEA lines
 *   counts, checksumErrors
 */
export function parseNmea(lines) {
  const fx = {
    t: [], utc: [], lat: [], lon: [], altMsl: [], geoid: [], speed: [], course: [], vn: [], ve: [], vd: [],
    fixType: [], sats: [], hdop: [], quality: [], valid: [],
  };
  const sat = { t: [], inView: [], good: [], top4: [], used: [] };
  const messages = [];
  const counts = {};
  let checksumErrors = 0;

  let pend = null;          // epoch in assembly
  let gsv = null;           // GSV of the current epoch: {talkers: {GP: total}, snr: []}
  const heights = [];       // [{tMs (UTC-based or device), h}] of valid fixes for the vertical speed
  let lastFixType = 0;      // GSA fix type carried over the epochs without a GSA group

  const flushGsv = (t) => {
    if (!gsv || !gsv.snr.length && !Object.keys(gsv.talkers).length) return;
    const snr = gsv.snr.filter((x) => x > 0).sort((a, b) => b - a);
    sat.t.push(t);
    sat.inView.push(Object.values(gsv.talkers).reduce((a, b) => a + b, 0));
    sat.good.push(snr.filter((x) => x >= GOOD_SNR).length);
    sat.top4.push(snr.length ? snr.slice(0, 4).reduce((a, b) => a + b, 0) / Math.min(4, snr.length) : 0);
    sat.used.push(pend ? pend.sats : 0);
  };

  for (const { t, text } of lines) {
    if (text[0] !== "$") {
      if (text.trim()) messages.push({ t, text });
      continue;
    }
    if (!checksumOk(text)) {
      checksumErrors++;
      continue;
    }
    const f = text.slice(0, text.lastIndexOf("*")).split(",");
    const type = f[0].slice(3);
    counts[type] = (counts[type] || 0) + 1;
    if (type === "GGA") {
      if (gsv) flushGsv(gsv.t);
      gsv = { t, talkers: {}, snr: [] };
      const tod = todMs(f[1]);
      if (tod === null || f.length < 12) {
        pend = null;
        continue;
      }
      const q = parseInt(f[6], 10) || 0;
      pend = {
        tod, quality: q, sats: parseInt(f[7], 10) || 0, hdop: parseFloat(f[8]) || NaN,
        lat: coord(f[2], f[3]), lon: coord(f[4], f[5]), alt: parseFloat(f[9]), geoid: parseFloat(f[11]) || 0,
        fixType: 0, gsaSeen: false,
      };
    } else if (type === "GSA") {
      /* one GSA per satellite system, once a second at 10 Hz: the best of the epoch's group counts
       * and stays for the epochs until the next group, as the firmware keeps it */
      const ft = parseInt(f[2], 10);
      if (pend && ft >= 1 && ft <= 3) {
        if (!pend.gsaSeen) { pend.gsaSeen = true; pend.fixType = 0; }
        if (ft > pend.fixType) pend.fixType = ft;
      }
    } else if (type === "GSV") {
      if (!gsv) gsv = { t, talkers: {}, snr: [] };
      const talker = f[0].slice(1, 3);
      if (f[2] === "1") gsv.talkers[talker] = parseInt(f[3], 10) || 0;
      for (let k = 4; k + 3 < f.length; k += 4) {
        const snr = parseInt(f[k + 3], 10);
        if (parseInt(f[k], 10) > 0 && snr > 0) gsv.snr.push(snr);
      }
    } else if (type === "RMC") {
      const tod = todMs(f[1]);
      if (!pend || tod === null || tod !== pend.tod || f.length < 10) continue;
      const day = dateMs(f[9]);
      const utc = day === null ? NaN : day + tod;
      if (!pend.gsaSeen) pend.fixType = lastFixType;
      lastFixType = pend.fixType;
      const valid = pend.quality > 0 && f[2] === "A" && day !== null;
      const speed = valid ? (parseFloat(f[7]) || 0) * KNOTS : NaN;
      const course = valid ? parseFloat(f[8]) || 0 : NaN;
      const c = (course * Math.PI) / 180;
      let vd = NaN;
      if (valid) {
        const h = pend.alt + pend.geoid;
        for (let k = heights.length - 1; k >= 0; k--) {
          const dt = utc - heights[k].utc;
          if (dt > MAX_GAP_MS || dt <= 0) break;
          if (dt >= SPAN_MS) {
            vd = -(h - heights[k].h) * 1000 / dt;
            break;
          }
        }
        heights.push({ utc, h });
        if (heights.length > 32) heights.shift();
      }
      fx.t.push(t);
      fx.utc.push(utc);
      fx.lat.push(valid ? pend.lat : NaN);
      fx.lon.push(valid ? pend.lon : NaN);
      fx.altMsl.push(valid ? pend.alt : NaN);
      fx.geoid.push(pend.geoid);
      fx.speed.push(speed);
      fx.course.push(course);
      fx.vn.push(valid ? speed * Math.cos(c) : NaN);
      fx.ve.push(valid ? speed * Math.sin(c) : NaN);
      fx.vd.push(vd);
      fx.fixType.push(pend.fixType);
      fx.sats.push(pend.sats);
      fx.hdop.push(pend.hdop);
      fx.quality.push(pend.quality);
      fx.valid.push(valid && pend.fixType === 3 ? 1 : 0);
      pend = { ...pend, tod: -1 }; // keeps sats for the GSV summary, but no second RMC match
    }
  }
  if (gsv) flushGsv(gsv.t);

  const fixes = {};
  for (const [k, v] of Object.entries(fx)) fixes[k] = Float64Array.from(v);
  fixes.n = fx.t.length;
  const sats = {};
  for (const [k, v] of Object.entries(sat)) sats[k] = Float64Array.from(v);
  sats.n = sat.t.length;
  return { fixes, sats, messages, counts, checksumErrors };
}

/* Fixes per second (median spacing of consecutive fixes). */
export function fixRate(fixes) {
  if (fixes.n < 3) return 0;
  const d = [];
  for (let i = 1; i < fixes.n; i++) d.push(fixes.t[i] - fixes.t[i - 1]);
  d.sort((a, b) => a - b);
  const med = d[d.length >> 1];
  return med > 0 ? Math.round(1000 / med) : 0;
}
