/*
 * Flight states from a per-second series: ports of the firmware's takeoff and landing detection
 * (flight.c), of the thermal detector (thermal.c: circling, climb phases, thermal exit, glide
 * ratio, wind) and of the manoeuvre detector (maneuver.c), plus a GNSS-only estimate of bank
 * angle and load factor for tracks without an IMU, and the announcements the device would make
 * with the default settings. Works on IGC tracks and on raw logs. No DOM.
 *
 * Input (one entry per whole second, gaps as valid = 0):
 *   {n, t[] (s), valid[] (GNSS fix), lat[], lon[], alt[] (m, barometric), vz[] (m/s),
 *    ve[], vn[] (m/s ground velocity), accelG[] (|a|/g from the IMU, NaN without one),
 *    hdop[] (NaN: unknown), motion[], accMax[], accMin[] (|a| extremes in g), rateMax[] (deg/s):
 *    the IMU peaks since the previous second, as the firmware's flight step takes them}
 * Takeoff and landing with the firmware's default settings; the events carry the rules that held
 * and the second of the detection (detectedT), their t is the time the device dates them to.
 * Output:
 *   {segments: [{t0, t1, kind, i0, i1, gainM, meanClimb, meanLD}],
 *    events: [{t, kind, text}], per: {state, glide, bank, load, turn, climb, windKmh, windDeg,
 *    airspeed}, summary, kinds: KINDS}
 */

export const KINDS = ["ground", "glide", "thermal", "circling", "spiral", "wingover"];
export const KIND_COLORS = {
  ground: "#6b7680", glide: "#4fb3ff", thermal: "#ff8c42", circling: "#f2e863", spiral: "#ff4d6d", wingover: "#c792ea",
};
export const KIND_LABELS = {
  ground: "on the ground", glide: "straight flight", thermal: "thermaling", circling: "circling without lift",
  spiral: "spiral", wingover: "wingover",
};

/* announce.c defaults */
export const DEFAULT_CONFIG = { climbReportS: 20, glideMinGainM: 100, exitMinS: 60, exitMinGainM: 50, glideStraightS: 30, glideRepeatMin: 0 };

const G0 = 9.80665;
const RAD2DEG = 57.29578;

/* ---------------------------------------------------------------- flight.c */
const FLIGHT_WINDOW_S = 30;
/* The firmware's default settings (config.c): takeoff thresholds and landing rules. */
export const TAKEOFF_DEFAULTS = { speedMs: 15 / 3.6, speedS: 5, distanceM: 50, altitudeM: 15, climbMs: 1.5, climbS: 5 };
export const LANDING_DEFAULTS = { enabled: true, calmS: 60, impact: true, impactG: 4, slowMs: 10 / 3.6, impactS: 10, handling: true };
const LAUNCH = { stillMs: 2 / 3.6, hdopGood: 3.0, anchorN: 30, awayShare: 0.7, awayHoldS: 2, altOnsetM: 2.0 };
const LANDED = {
  calmMs: 3 / 3.6, calmClimbMs: 0.3,  /* the calm rule */
  climb5Ms: 0.5,                      /* on the ground: no constant climb or sink, barometer over 5 s */
  shockLowG: 1.0, shockRecentS: 30,   /* a touchdown shock: the load also drops below 1 g in that second */
  handlingDps: 250, handlingLowG: 0.5, handlingCount: 3, handlingGroundS: 20,
};
const M_PER_DEG = 111319;

const popcount = (v) => { let n = 0; for (; v; v &= v - 1) n++; return n; };

/*
 * flight.c step for step, called once a second. Input {t (s, from 1), fix, lat, lon, speed (m/s), alt
 * (barometric, m), vz (m/s), hdop (undefined = unknown, taken as good: an IGC file has none), motion
 * (the IMU peaks of the second are known: accMaxG and accMinG = extremes of |a| in g, rateMaxDps)}.
 * Returns "takeoff", "landing" or null; takeoffS and landingS date the event, rules names the rules
 * that held when it was detected.
 */
export class FlightDetector {
  constructor(takeoff = TAKEOFF_DEFAULTS, landing = LANDING_DEFAULTS) {
    this.takeoff = takeoff;
    this.landing = landing;
    this.hist = [];
    this.state = "ground";
    this.fastSince = 0; this.varioSince = 0;
    this.anchor = null; this.anchorN = 0; this.stillS = 0; this.awayM = 0; this.awayS = 0;
    this.resetGround();
    this.takeoffS = 0; this.landingS = 0;
    this.rules = [];
  }
  resetGround() {
    this.calmSince = 0; this.groundSince = 0; this.shockS = 0; this.firstShockS = 0; this.handlingBits = 0;
  }
  oldest(now, needFix) {
    for (const s of this.hist) if (now - s.t <= FLIGHT_WINDOW_S && (!needFix || s.fix)) return s;
    return null;
  }
  /* the launch point: averaged standing position before the first takeoff; leaving it backed by the ground speed */
  leftLaunchPoint(inp, good) {
    if (this.state !== "ground" || !good) { this.awayS = 0; return false; }
    if (inp.speed < LAUNCH.stillMs) {
      if (this.anchorN < LAUNCH.anchorN) this.anchorN++;
      if (this.anchorN === 1) this.anchor = { lat: inp.lat, lon: inp.lon };
      else {
        this.anchor.lat += (inp.lat - this.anchor.lat) / this.anchorN;
        this.anchor.lon += (inp.lon - this.anchor.lon) / this.anchorN;
      }
      this.stillS = inp.t;
      this.awayM = 0;
    } else {
      this.awayM += inp.speed; /* one second at this ground speed */
    }
    if (!this.anchorN) return false;
    const dy = (inp.lat - this.anchor.lat) * M_PER_DEG;
    const dx = (inp.lon - this.anchor.lon) * M_PER_DEG * Math.cos(inp.lat / RAD2DEG);
    const dist = Math.hypot(dx, dy);
    this.awayS = dist >= this.takeoff.distanceM && this.awayM >= LAUNCH.awayShare * dist ? this.awayS + 1 : 0;
    return this.awayS >= LAUNCH.awayHoldS;
  }
  /* the start of the window's altitude change: after the last sample within altOnsetM of its start */
  altitudeOnset(o, now) {
    let onset = o.t;
    for (const s of this.hist) if (s.t >= o.t && Math.abs(s.alt - o.alt) < LAUNCH.altOnsetM) onset = s.t;
    return onset + 1 < now ? onset + 1 : now;
  }
  /* barometric climb over the last `span` seconds (the sample closest to it, newest first), NaN without one */
  climbOver(inp, span) {
    for (let k = this.hist.length - 1; k >= 0; k--) {
      const age = inp.t - this.hist[k].t;
      if (age >= span && age <= span + 2) return (inp.alt - this.hist[k].alt) / age;
    }
    return NaN;
  }
  update(inp) {
    const now = inp.t, T = this.takeoff, L = this.landing;
    const good = inp.fix && (inp.hdop === undefined || (inp.hdop > 0 && inp.hdop <= LAUNCH.hdopGood));
    const fast = good && inp.speed >= T.speedMs;
    this.fastSince = fast ? (this.fastSince || now) : 0;
    const strong = Math.abs(inp.vz) >= T.climbMs;
    this.varioSince = strong ? (this.varioSince || now) : 0;
    const speed = !!this.fastSince && now - this.fastSince + 1 >= T.speedS;
    const varioHeld = !!this.varioSince && now - this.varioSince + 1 >= T.climbS;
    const o = this.oldest(now, false);
    const altitude = !!o && Math.abs(inp.alt - o.alt) >= T.altitudeM;
    const altitudeFrom = altitude ? this.altitudeOnset(o, now) : 0;
    const away = this.leftLaunchPoint(inp, good);
    this.hist.push({ t: now, fix: inp.fix, lat: inp.lat, lon: inp.lon, alt: inp.alt, speed: inp.speed });
    while (this.hist.length > FLIGHT_WINDOW_S + 1) this.hist.shift();

    let ev = null;
    if (this.state !== "airborne") {
      if (inp.fix && (away || speed || varioHeld || altitude)) {
        /* when it began: the end of standing at the launch point (if the device stood there within the
         * window), the start of the fast phase or of the climb or sink */
        const earliest = (a, b) => (a === 0 || (b !== 0 && b < a) ? b : a);
        const stood = this.state === "ground" && this.stillS && now - this.stillS <= FLIGHT_WINDOW_S;
        let first = 0;
        first = earliest(first, away || stood ? this.stillS + 1 : 0);
        first = earliest(first, speed ? this.fastSince : 0);
        first = earliest(first, varioHeld ? this.varioSince : 0);
        first = earliest(first, altitudeFrom);
        this.rules = [away && "launch point", speed && "speed", varioHeld && "climb rate", altitude && "altitude"].filter(Boolean);
        this.state = "airborne";
        this.resetGround();
        this.takeoffS = first && first <= now ? first : now;
        ev = "takeoff";
      }
    } else if (L.enabled) {
      const calm = inp.fix && inp.speed < LANDED.calmMs && Math.abs(inp.vz) < LANDED.calmClimbMs;
      this.calmSince = calm ? (this.calmSince || now) : 0;
      /* on the ground: slow (when the speed is known) and no constant climb or sink */
      const c5 = this.climbOver(inp, 5);
      const ground = Math.abs(c5) < LANDED.climb5Ms && (!inp.fix || inp.speed < L.slowMs);
      this.groundSince = ground ? (this.groundSince || now) : 0;
      const groundS = this.groundSince ? now - this.groundSince + 1 : 0;
      const shock = !!inp.motion && inp.accMaxG >= L.impactG && inp.accMinG < LANDED.shockLowG;
      if (shock) {
        if (!this.shockS || now - this.shockS > LANDED.shockRecentS) this.firstShockS = now;
        this.shockS = now;
      }
      const handling = !!inp.motion && inp.rateMaxDps >= LANDED.handlingDps && inp.accMinG < LANDED.handlingLowG;
      this.handlingBits = ((this.handlingBits << 1) | (handling ? 1 : 0)) >>> 0;
      let at = 0, rule = "";
      if (this.calmSince && now - this.calmSince >= L.calmS) { at = this.calmSince; rule = "calm"; }
      else if (L.impact && this.shockS && now - this.shockS <= LANDED.shockRecentS && groundS >= L.impactS) { at = this.firstShockS; rule = "impact"; }
      else if (L.handling && popcount(this.handlingBits & 0x3fffffff) >= LANDED.handlingCount && groundS >= LANDED.handlingGroundS) {
        at = this.groundSince;
        rule = "ground handling";
      }
      if (at) {
        this.state = "landed";
        this.landingS = at;
        this.resetGround();
        this.hist = []; /* the window starts afresh: the approach in it is no altitude change for a takeoff */
        this.rules = [rule];
        ev = "landing";
      }
    }
    return ev;
  }
}

/* ---------------------------------------------------------------- thermal.c */
const WINDOW = 32;
const MIN_TRACK_MS = 2.0, MAX_TURN_DEG = 90, CIRCLE_WINDOW_S = 30, STRAIGHT_S = 10, STRAIGHT_DEG = 60;
const THERMALING_MS = 0.1, EXIT_AVG_S = 30, EXIT_CLIMB_MS = 0.2, GLIDE_AVG_S = 20, GLIDE_MIN_SINK = 0.3, GLIDE_MAX = 60;
const FIT_TURN_DEG = 330, FIT_MIN_POINTS = 8, FIT_AIRSPEED_MIN = 5.5, FIT_AIRSPEED_MAX = 16.7, FIT_MAX_RMS = 1.5;
const WIND_NEW_MS = 1.39, WIND_NEW_DEG = 45;

const wrap180 = (d) => { while (d > 180) d -= 360; while (d < -180) d += 360; return d; };

class ThermalDetector {
  constructor(cfg) {
    this.cfg = cfg;
    this.turn = new Float64Array(WINDOW); this.gturn = new Float64Array(WINDOW);
    this.ve = new Float64Array(WINDOW); this.vn = new Float64Array(WINDOW);
    this.velOk = new Uint8Array(WINDOW); this.alt = new Float64Array(WINDOW);
    this.head = 0; this.count = 0;
    this.lastCourse = 0; this.haveCourse = false;
    this.circling = false; this.sinceEnd = WINDOW;
    this.phaseT = 0; this.phaseAlt = 0; this.nextReport = 0; this.exitDone = false; this.thermalMs = 0;
    this.exitClimbMs = 0; this.exitGainM = 0;
    this.glideArmed = false; this.straightT = 0; this.glideSaid = false; this.glideLastS = 0; this.glide = NaN;
    this.fitOk = false; this.fitE = 0; this.fitN = 0;
    this.windValid = false; this.windE = 0; this.windN = 0;
    this.windSaid = false; this.saidE = 0; this.saidN = 0;
  }
  idx(k) { return (this.head + WINDOW - 1 - k) % WINDOW; }
  sum(arr, n) { let s = 0; for (let k = 0; k < n && k < this.count; k++) s += arr[this.idx(k)]; return s; }
  windFit() {
    let n = 0, turned = 0;
    while (n < this.count && turned < FIT_TURN_DEG) {
      const i = this.idx(n);
      if (!this.velOk[i]) return null;
      turned += Math.abs(this.turn[i]);
      n++;
    }
    if (turned < FIT_TURN_DEG || n < FIT_MIN_POINTS) return null;
    let mx = 0, my = 0;
    for (let k = 0; k < n; k++) { mx += this.ve[this.idx(k)]; my += this.vn[this.idx(k)]; }
    mx /= n; my /= n;
    let suu = 0, svv = 0, suv = 0, suz = 0, svz = 0, sz = 0;
    for (let k = 0; k < n; k++) {
      const u = this.ve[this.idx(k)] - mx, v = this.vn[this.idx(k)] - my, z = u * u + v * v;
      suu += u * u; svv += v * v; suv += u * v; suz += u * z; svz += v * z; sz += z;
    }
    const det = suu * svv - suv * suv;
    if (det < 1e-3) return null;
    const uc = 0.5 * (suz * svv - svz * suv) / det, vc = 0.5 * (svz * suu - suz * suv) / det;
    const r = Math.sqrt(sz / n + uc * uc + vc * vc);
    let res = 0;
    for (let k = 0; k < n; k++) {
      const e = Math.hypot(this.ve[this.idx(k)] - mx - uc, this.vn[this.idx(k)] - my - vc) - r;
      res += e * e;
    }
    if (r < FIT_AIRSPEED_MIN || r > FIT_AIRSPEED_MAX || Math.sqrt(res / n) > FIT_MAX_RMS) return null;
    return { we: mx + uc, wn: my + vc, airspeed: r };
  }
  circlingEnded(now, ev) {
    const c = this.cfg;
    this.circling = false;
    this.sinceEnd = 0;
    const back = STRAIGHT_S < this.count ? STRAIGHT_S : this.count - 1;
    const gain = this.alt[this.idx(back)] - this.phaseAlt;
    const duration = now - back - this.phaseT;
    if (duration >= c.exitMinS && gain >= c.exitMinGainM && !this.exitDone) {
      ev.exit = true;
      this.exitClimbMs = this.thermalMs;
      this.exitGainM = gain;
    }
    this.glideArmed = c.glideMinGainM === 0 || gain >= c.glideMinGainM;
    this.straightT = now;
    this.glideSaid = false;
    if (this.fitOk) {
      if (this.windValid) { this.windE = 0.5 * (this.windE + this.fitE); this.windN = 0.5 * (this.windN + this.fitN); }
      else { this.windE = this.fitE; this.windN = this.fitN; this.windValid = true; }
      const de = this.windE - this.saidE, dn = this.windN - this.saidN;
      const dir = Math.abs(wrap180(Math.atan2(this.windE, this.windN) * RAD2DEG - Math.atan2(this.saidE, this.saidN) * RAD2DEG));
      if (!this.windSaid || Math.hypot(de, dn) > WIND_NEW_MS || dir > WIND_NEW_DEG) {
        ev.wind = true;
        this.windSaid = true;
        this.saidE = this.windE; this.saidN = this.windN;
      }
    }
  }
  update(now, velValid, ve, vn, alt, gyroValid = false, gyroDps = 0) {
    const c = this.cfg;
    const ev = { climb: false, exit: false, glide: false, wind: false };
    const ok = velValid && Math.hypot(ve, vn) >= MIN_TRACK_MS;
    let turn = 0;
    if (ok) {
      const course = Math.atan2(ve, vn) * RAD2DEG;
      if (this.haveCourse) {
        turn = wrap180(course - this.lastCourse);
        if (Math.abs(turn) > MAX_TURN_DEG) turn = 0;
      }
      this.lastCourse = course;
    }
    this.haveCourse = ok;
    const i = this.head;
    this.turn[i] = turn;
    this.gturn[i] = gyroValid ? gyroDps : 0;
    this.ve[i] = ve; this.vn[i] = vn; this.velOk[i] = ok ? 1 : 0; this.alt[i] = alt;
    this.head = (this.head + 1) % WINDOW;
    if (this.count < WINDOW) this.count++;
    if (!this.circling && this.sinceEnd < WINDOW) this.sinceEnd++;
    const window = this.sinceEnd < CIRCLE_WINDOW_S ? this.sinceEnd : CIRCLE_WINDOW_S;
    const fullTrack = !this.circling && Math.abs(this.sum(this.turn, window)) >= 360;
    const fullGyro = !this.circling && !fullTrack && Math.abs(this.sum(this.gturn, window)) >= 360;
    if (fullTrack || fullGyro) {
      const series = fullTrack ? this.turn : this.gturn;
      let back = 0, acc = 0;
      while (back + 1 < this.count && back < window && Math.abs(acc) < 360) { acc += series[this.idx(back)]; back++; }
      this.circling = true;
      this.phaseT = now - back;
      this.phaseAlt = this.alt[this.idx(back)];
      this.nextReport = now + c.climbReportS;
      this.exitDone = false;
      this.fitOk = false;
      this.glideArmed = false;
    } else if (this.circling && this.count > STRAIGHT_S && Math.abs(this.sum(this.turn, STRAIGHT_S)) < STRAIGHT_DEG &&
               Math.abs(this.sum(this.gturn, STRAIGHT_S)) < STRAIGHT_DEG) {
      this.circlingEnded(now, ev);
    }
    if (this.circling) {
      if (now > this.phaseT) this.thermalMs = (alt - this.phaseAlt) / (now - this.phaseT);
      if (c.climbReportS && now >= this.nextReport) {
        this.nextReport = now + c.climbReportS;
        if (this.thermalMs >= THERMALING_MS) ev.climb = true;
      }
      if (!this.exitDone && now - this.phaseT >= c.exitMinS && alt - this.phaseAlt >= c.exitMinGainM && this.count > EXIT_AVG_S &&
          (alt - this.alt[this.idx(EXIT_AVG_S)]) / EXIT_AVG_S < EXIT_CLIMB_MS) {
        ev.exit = true;
        this.exitDone = true;
        this.exitClimbMs = this.thermalMs;
        this.exitGainM = alt - this.phaseAlt;
      }
      const fit = this.windFit();
      if (fit) { this.fitOk = true; this.fitE = fit.we; this.fitN = fit.wn; }
    } else {
      if (this.straightT === 0) {
        this.straightT = now;
        this.glideArmed = c.glideMinGainM === 0;
        this.glideSaid = false;
      }
      if (this.glideArmed) {
        const due = !this.glideSaid ? now - this.straightT >= c.glideStraightS
          : c.glideRepeatMin && now - this.glideLastS >= c.glideRepeatMin * 60;
        const ratio = due ? this.glideNow() : NaN;
        if (Number.isFinite(ratio)) {
          ev.glide = true;
          this.glide = ratio;
          this.glideSaid = true;
          this.glideLastS = now;
          if (!c.glideRepeatMin) this.glideArmed = false;
        }
      }
    }
    return ev;
  }
  /* glide ratio over the last 20 s of straight flight, NaN otherwise */
  glideNow() {
    if (this.circling || this.count <= GLIDE_AVG_S || Math.abs(this.sum(this.turn, GLIDE_AVG_S)) >= 90) return NaN;
    let speed = 0;
    for (let k = 0; k < GLIDE_AVG_S; k++) {
      const i = this.idx(k);
      if (!this.velOk[i]) return NaN;
      speed += Math.hypot(this.ve[i], this.vn[i]);
    }
    speed /= GLIDE_AVG_S;
    const sink = (this.alt[this.idx(GLIDE_AVG_S)] - this.alt[this.idx(0)]) / GLIDE_AVG_S;
    if (sink < GLIDE_MIN_SINK || speed / sink > GLIDE_MAX) return NaN;
    return speed / sink;
  }
  wind() {
    if (!this.windValid) return null;
    let from = Math.atan2(-this.windE, -this.windN) * RAD2DEG;
    if (from < 0) from += 360;
    return { kmh: Math.hypot(this.windE, this.windN) * 3.6, deg: from, e: this.windE, n: this.windN };
  }
}

/* ---------------------------------------------------------------- maneuver.c (per second) */
const MAN = { lowG: 0.5, highG: 2.0, lowWindowS: 4, spiralG: 1.6, spiralDps: 50, spiralS: 4, endG: 1.3, endS: 2 };
/* Without an IMU the turn rate from 1 Hz positions collapses in a tight spiral (the fixes jitter around its
 * centre), but the sink does not: this much for this long is a spiral, until the sink eases for endS. */
const SINK = { spiralMs: -6, spiralS: 3, endMs: -3 };

export class ManeuverDetector {
  constructor() {
    this.sinceLow = Infinity; this.highS = 0; this.calmS = 0; this.sinkS = 0;
    this.active = false; this.kind = null; this.bySink = false; this.nMax = 0; this.sinkMax = 0; this.gMaxFlight = 0; this.startI = -1;
  }
  /* returns the kind of a manoeuvre that just ended, with its peak load and bank (or peak sink when found by the sink) */
  update(i, n, rateDps, sinkMs, measured) {
    let done = null;
    if (Number.isFinite(n) && n > this.gMaxFlight) this.gMaxFlight = n;
    if (n <= MAN.lowG) this.sinceLow = 0; else this.sinceLow += 1;
    const loaded = Number.isFinite(n) && n >= MAN.spiralG && Math.abs(rateDps) >= MAN.spiralDps;
    if (!this.active) {
      /* a wingover needs the unloaded moment first; without an IMU the load is estimated from the turn, so
       * the unload is never seen and wingovers stay undetected */
      if (measured && n >= MAN.highG && this.sinceLow <= MAN.lowWindowS) this.begin(i, "wingover", n, false);
      else if (loaded) {
        this.highS += 1;
        if (this.highS >= MAN.spiralS) this.begin(i - MAN.spiralS + 1, "spiral", n, false);
      } else this.highS = 0;
      if (!this.active && !measured && sinkMs <= SINK.spiralMs) {
        this.sinkS += 1;
        if (this.sinkS >= SINK.spiralS) this.begin(i - SINK.spiralS + 1, "spiral", n, true);
      } else this.sinkS = 0;
    } else {
      if (n > this.nMax) this.nMax = n;
      if (sinkMs < this.sinkMax) this.sinkMax = sinkMs;
      if (this.kind === "wingover" && loaded) {
        this.highS += 1;
        if (this.highS >= MAN.spiralS) this.kind = "spiral";
      } else this.highS = 0;
      const calm = this.bySink ? sinkMs > SINK.endMs && !loaded : n < MAN.endG;
      if (calm) {
        this.calmS += 1;
        if (this.calmS >= MAN.endS) {
          done = { kind: this.kind, nMax: this.nMax, bank: bankOfLoad(this.nMax), sinkMax: this.sinkMax, bySink: this.bySink, i0: this.startI, i1: i - MAN.endS };
          this.active = false;
          this.kind = null;
          this.highS = 0;
          this.sinkS = 0;
        }
      } else this.calmS = 0;
    }
    return done;
  }
  begin(i, kind, n, bySink) {
    this.active = true; this.kind = kind; this.bySink = bySink; this.nMax = Number.isFinite(n) ? n : 1; this.sinkMax = 0;
    this.calmS = 0; this.highS = 0; this.sinkS = 0; this.startI = i;
  }
}

export function bankOfLoad(n) {
  return n <= 1 ? 0 : Math.acos(1 / n) * RAD2DEG;
}

/* ---------------------------------------------------------------- maneuver.c at the device's rate */
/*
 * The device feeds maneuver.c every 50 ms with |a| and |ω| smoothed over 0.1 s (sensors.c), and the
 * detector smooths the load over another 0.2 s. The per-second detector above sees 1 s means, which
 * average a wingover's unloaded moment away; a raw log is replayed at the device's rate with this port
 * (track.js fineManeuvers), the per-second one serves IGC files.
 */
const MAN_SMOOTH_S = 0.2;
export class FineManeuverDetector {
  constructor() {
    this.n = 1; this.started = false; this.sinceLowS = MAN.lowWindowS + 1; this.highS = 0; this.calmS = 0;
    this.active = false; this.kind = null; this.nMax = 0; this.gMaxFlight = 0; this.startT = 0;
  }
  /* one tick: the smoothed load [g] and rotation rate [°/s], dt in seconds; returns a finished manoeuvre or null */
  update(t, loadG, rateDps, dt) {
    if (!this.started) { this.n = loadG; this.started = true; } else this.n += dt / (MAN_SMOOTH_S + dt) * (loadG - this.n);
    const n = this.n;
    if (n > this.gMaxFlight) this.gMaxFlight = n;
    this.sinceLowS = n <= MAN.lowG ? 0 : this.sinceLowS + dt;
    this.highS = n >= MAN.spiralG && rateDps >= MAN.spiralDps ? this.highS + dt : 0;
    if (!this.active) {
      if (n >= MAN.highG && this.sinceLowS <= MAN.lowWindowS) { this.active = true; this.kind = "wingover"; this.nMax = n; this.startT = t; }
      else if (this.highS >= MAN.spiralS) { this.active = true; this.kind = "spiral"; this.nMax = n; this.startT = t - MAN.spiralS; }
      return null;
    }
    if (n > this.nMax) this.nMax = n;
    if (this.kind === "wingover" && this.highS >= MAN.spiralS) this.kind = "spiral"; /* the wingover went on into a spiral */
    if (n < MAN.endG) {
      this.calmS += dt;
      if (this.calmS >= MAN.endS) {
        const done = { kind: this.kind, nMax: this.nMax, bank: bankOfLoad(this.nMax), t0: this.startT, t1: t - MAN.endS };
        this.active = false; this.kind = null; this.highS = 0; this.calmS = 0;
        return done;
      }
    } else this.calmS = 0;
    return null;
  }
}

/* ---------------------------------------------------------------- the whole */
export function detectFlightStates(series, cfg = DEFAULT_CONFIG, opts = {}) {
  const n = series.n;
  const per = {
    state: new Uint8Array(n), glide: new Float32Array(n).fill(NaN), bank: new Float32Array(n).fill(NaN),
    load: new Float32Array(n).fill(NaN), turn: new Float32Array(n).fill(NaN), climb: new Float32Array(n).fill(NaN),
    windKmh: new Float32Array(n).fill(NaN), windDeg: new Float32Array(n).fill(NaN), airspeed: new Float32Array(n).fill(NaN),
    text: new Array(n).fill(""),
  };
  const events = [];
  const flight = new FlightDetector();
  const thermal = new ThermalDetector(cfg);
  const man = new ManeuverDetector();
  const fine = opts.maneuvers || null; /* manoeuvres from the replay at the device's rate (track.js) */
  const hasImu = series.accelG && Array.from(series.accelG.subarray(0, Math.min(n, 600))).some(Number.isFinite);
  const kindIndex = Object.fromEntries(KINDS.map((k, i) => [k, i]));
  let airborne = opts.forceAirborne || false;
  let lastCourse = NaN;
  const climbWin = [];
  let climbSum = 0;
  const say = (i, kind, text) => { events.push({ t: series.t[i], i, kind, text }); per.text[i] = per.text[i] ? `${per.text[i]}; ${text}` : text; };
  const maneuverSeconds = new Int8Array(n).fill(-1); /* kind index of a manoeuvre, filled in when it ends */

  for (let i = 0; i < n; i++) {
    const valid = !!series.valid[i];
    const ve = series.ve[i], vn = series.vn[i];
    const speed = valid ? Math.hypot(ve, vn) : 0;
    const alt = series.alt[i], vz = Number.isFinite(series.vz[i]) ? series.vz[i] : 0;
    // 30 s mean climb
    climbWin.push(vz); climbSum += vz;
    if (climbWin.length > 30) climbSum -= climbWin.shift();
    per.climb[i] = climbSum / climbWin.length;
    // turn rate from the course
    let turn = NaN;
    if (valid && speed >= MIN_TRACK_MS) {
      const course = Math.atan2(ve, vn) * RAD2DEG;
      if (Number.isFinite(lastCourse)) turn = wrap180(course - lastCourse);
      lastCourse = course;
    } else lastCourse = NaN;
    per.turn[i] = turn;

    // takeoff and landing
    const hdop = series.hdop ? series.hdop[i] : NaN;
    const fev = flight.update({
      t: i + 1, fix: valid, lat: series.lat[i], lon: series.lon[i], speed, alt, vz,
      hdop: Number.isFinite(hdop) ? hdop : undefined,
      motion: !!(series.motion && series.motion[i]),
      accMaxG: series.accMax ? series.accMax[i] : NaN, accMinG: series.accMin ? series.accMin[i] : NaN,
      rateMaxDps: series.rateMax ? series.rateMax[i] : NaN,
    });
    if (fev === "takeoff") {
      airborne = true;
      const at = Math.max(0, i - (i + 1 - flight.takeoffS));
      events.push({ t: series.t[at], i: at, kind: "takeoff", text: "takeoff", rules: flight.rules, detectedT: series.t[i] });
      per.text[at] = "takeoff";
      Object.assign(thermal, new ThermalDetector(cfg));
    } else if (fev === "landing") {
      airborne = false;
      const at = Math.max(0, i - (i + 1 - flight.landingS));
      events.push({ t: series.t[at], i: at, kind: "landing", text: "landing", rules: flight.rules, detectedT: series.t[i] });
      per.text[at] = "landing";
    }
    if (!airborne) { per.state[i] = kindIndex.ground; continue; }

    // thermal detector
    const ev = thermal.update(i + 1, valid, ve, vn, alt);
    const wind = thermal.wind();
    if (wind) { per.windKmh[i] = wind.kmh; per.windDeg[i] = wind.deg; }
    if (ev.climb) say(i, "climb", `thermal ${thermal.thermalMs >= 0 ? "+" : ""}${thermal.thermalMs.toFixed(1)} m/s`);
    if (ev.exit) say(i, "exit", `thermal exit ${thermal.exitClimbMs >= 0 ? "+" : ""}${thermal.exitClimbMs.toFixed(1)} m/s, ${thermal.exitGainM >= 0 ? "+" : ""}${thermal.exitGainM.toFixed(0)} m`);
    if (ev.glide) say(i, "glide", `glide ratio ${thermal.glide.toFixed(1)}`);
    if (ev.wind && wind) say(i, "wind", `wind ${wind.kmh.toFixed(0)} km/h from ${wind.deg.toFixed(0)}°`);
    per.glide[i] = thermal.glideNow();

    // bank angle and load factor: measured (IMU) or from a coordinated turn at the estimated airspeed
    const airspeed = valid ? Math.hypot(ve - (wind ? wind.e : 0), vn - (wind ? wind.n : 0)) : NaN;
    per.airspeed[i] = airspeed;
    const measured = hasImu && Number.isFinite(series.accelG[i]);
    let load;
    if (measured) {
      load = series.accelG[i];
      per.bank[i] = bankOfLoad(load);
    } else if (Number.isFinite(turn) && Number.isFinite(airspeed)) {
      const bank = Math.atan(airspeed * Math.abs(turn) / RAD2DEG / G0);
      per.bank[i] = bank * RAD2DEG;
      load = 1 / Math.cos(bank);
    } else load = NaN;
    per.load[i] = load;

    // manoeuvres: from the replay at the device's rate when given (raw logs), else per second
    if (fine) {
      for (const done of fine) {
        const i1 = Math.floor(done.t1 - series.t[0]);
        if (i1 !== i) continue;
        const i0 = Math.max(0, Math.floor(done.t0 - series.t[0]));
        for (let k = i0; k <= i1 && k < n; k++) maneuverSeconds[k] = kindIndex[done.kind];
        say(i1, done.kind, `${done.kind} ${done.nMax.toFixed(1)} g, bank ${done.bank.toFixed(0)}°`);
      }
    } else {
      const done = man.update(i, load, Number.isFinite(turn) ? turn : 0, vz, measured);
      if (done) {
        for (let k = Math.max(0, done.i0); k <= done.i1 && k < n; k++) maneuverSeconds[k] = kindIndex[done.kind];
        if (done.bySink) say(done.i1, done.kind, `${done.kind}: sink ${(-done.sinkMax).toFixed(0)} m/s (from the sink; no IMU)`);
        else say(done.i1, done.kind, `${done.kind} ${done.nMax.toFixed(1)} g, bank ${done.bank.toFixed(0)}°${measured ? "" : " (estimated from the track)"}`);
      }
    }
    per.state[i] = thermal.circling ? (thermal.thermalMs >= THERMALING_MS ? kindIndex.thermal : kindIndex.circling) : kindIndex.glide;
  }
  for (let i = 0; i < n; i++) if (maneuverSeconds[i] >= 0) per.state[i] = maneuverSeconds[i];

  // segments
  const segments = [];
  let i0 = 0;
  for (let i = 1; i <= n; i++) {
    if (i === n || per.state[i] !== per.state[i0]) {
      const kind = KINDS[per.state[i0]];
      const i1 = i - 1;
      let ld = 0, c = 0;
      for (let k = i0; k <= i1; k++) if (Number.isFinite(per.glide[k])) { ld += per.glide[k]; c++; }
      segments.push({
        kind, i0, i1, t0: series.t[i0], t1: series.t[i1] + 1,
        gainM: series.alt[i1] - series.alt[i0], meanClimb: (series.alt[i1] - series.alt[i0]) / Math.max(1, i1 - i0),
        meanLD: c ? ld / c : NaN,
      });
      i0 = i;
    }
  }
  events.sort((a, b) => a.t - b.t);

  const thermals = segments.filter((s) => s.kind === "thermal");
  const glides = segments.filter((s) => s.kind === "glide");
  const lds = Array.from(per.glide).filter(Number.isFinite).sort((a, b) => a - b);
  const loads = Array.from(per.load).filter(Number.isFinite);
  const summary = {
    imu: hasImu,
    motion: !!(series.motion && series.motion.some((v) => v)), /* IMU peaks: the impact and handling rules can apply */
    takeoffT: events.find((e) => e.kind === "takeoff")?.t ?? null,
    landingT: events.find((e) => e.kind === "landing")?.t ?? null,
    thermals: thermals.length,
    thermalSeconds: thermals.reduce((a, s) => a + (s.i1 - s.i0 + 1), 0),
    thermalGainM: thermals.reduce((a, s) => a + Math.max(0, s.gainM), 0),
    bestThermalClimb: thermals.length ? Math.max(...thermals.map((s) => s.meanClimb)) : NaN,
    glides: glides.length,
    glideSeconds: glides.reduce((a, s) => a + (s.i1 - s.i0 + 1), 0),
    medianLD: lds.length ? lds[lds.length >> 1] : NaN,
    spirals: segments.filter((s) => s.kind === "spiral").length,
    wingovers: segments.filter((s) => s.kind === "wingover").length,
    maxLoad: Number.isFinite(opts.maxLoad) ? opts.maxLoad : loads.length ? Math.max(...loads) : NaN,
    wind: thermal.wind(),
    announcements: events.length,
  };
  return { segments, events, per, summary, kinds: KINDS };
}

/* the state (kind name) at a time, or null */
export function stateAt(states, series, t) {
  if (!states || !series.n) return null;
  let lo = 0, hi = series.n - 1;
  while (lo < hi) { const m = (lo + hi + 1) >> 1; if (series.t[m] <= t) lo = m; else hi = m - 1; }
  if (t < series.t[0] - 1 || t > series.t[lo] + 2) return null;
  return { i: lo, kind: KINDS[states.per.state[lo]] };
}
