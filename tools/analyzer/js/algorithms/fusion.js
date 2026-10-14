/*
 * Port of the firmware fusion (firmware/src/app/fusion.c and its use in sensors.c): Mahony attitude
 * with a gated accelerometer correction, 3-state vertical Kalman filter (altitude, climb rate,
 * accelerometer bias), rest detection, adaptive weights, barometer hold after taps and button
 * presses, and the manoeuvre modes of firmware 0.43: spiral mode (the climb rate as the altitude
 * change over exactly one rotation period, which cancels the pressure modulation a spiral puts on
 * the barometer; the IMU acceleration and the bias state frozen; a cross-fade and an attitude
 * re-acquisition afterwards), the IMU acceleration off in fast or unsteady rotation (launch run,
 * landing, handling) and during the tap hold, the attitude filter's bias integrator clamped, and a
 * rest test against g itself so the learned g recovers from a bad first sample. Before the attitude
 * is known the barometer-only filter of vario.c runs, as on the device. Keep in step with fusion.c:
 * the parameters carry its names; the switches (SPIRAL_G 0, FAST_RATE 0, HANDLING_RATE 0,
 * FREEZE_ON_HOLD 0, REACQ_S 0, BC_MAX 100, REST_VS_GREF 1) give the behaviour of firmware 0.42 and
 * earlier. The head of fusion.h has the findings behind each part.
 * Barometer rate: the variance limits are per sample at 32 Hz and grow with the rate; the noise
 * estimate keeps its time constant (fusion_set_baro_rate).
 */

const G0 = 9.80665;
const DEG = Math.PI / 180;
const HIST_STEP_S = 0.1; // FUSION_HIST_STEP_S
const HIST_N = 84; // FUSION_HIST_N
const WINDOW_MIN_S = 0.5, WINDOW_MAX_S = 8;

const params = [
  { key: "KP", value: 1.0, min: 0, max: 5, step: 0.05, unit: "1/s", doc: "Attitude: pull towards the measured vertical" },
  { key: "KI", value: 0.02, min: 0, max: 0.5, step: 0.005, doc: "Attitude: gyro bias correction" },
  { key: "ACCEL_GATE", value: 0.05, min: 0, max: 0.5, step: 0.005, doc: "Accelerometer corrects the attitude only within ±gate of g" },
  { key: "ATT_RATE_MAX", value: 10, min: 0, max: 200, step: 1, unit: "°/s", doc: "... and below this rotation rate" },
  { key: "FUSION_ACCEL_NOISE", value: 0.5, min: 0.01, max: 5, step: 0.01, unit: "m/s²", doc: "Vertical acceleration noise in calm motion" },
  { key: "FUSION_ACCEL_NOISE_REST", value: 0.1, min: 0.01, max: 2, step: 0.01, unit: "m/s²", doc: "... at rest" },
  { key: "FUSION_ACCEL_NOISE_SPIRAL", value: 1.5, min: 0.01, max: 10, step: 0.05, unit: "m/s²", doc: "... in spiral mode (the filter follows the barometer alone)" },
  { key: "FUSION_DYN_GAIN", value: 1.0, min: 0, max: 5, step: 0.05, doc: "Added noise per m/s² of horizontal specific force" },
  { key: "FUSION_BIAS_WALK", value: 0.01, min: 0, max: 0.2, step: 0.001, unit: "m/s²/√s", doc: "Accelerometer bias random walk" },
  { key: "FUSION_BARO_VAR_MIN", value: 0.04, min: 0.001, max: 2, step: 0.005, unit: "m²", doc: "Barometer variance, lower limit" },
  { key: "FUSION_BARO_VAR_MAX", value: 2.0, min: 0.01, max: 20, step: 0.1, unit: "m²", doc: "Barometer variance, upper limit" },
  { key: "FUSION_BARO_VAR_REST", value: 1.0, min: 0, max: 10, step: 0.1, unit: "m²", doc: "Barometer variance at least this at rest" },
  { key: "FUSION_BARO_VAR_HOLD", value: 50, min: 0, max: 500, step: 1, unit: "m²", doc: "Barometer variance while held off" },
  { key: "FUSION_HOLD_S", value: 1.5, min: 0, max: 10, step: 0.1, unit: "s", doc: "Hold after a tap or button press (0 = off)" },
  { key: "FREEZE_ON_HOLD", value: 1, min: 0, max: 1, step: 1, doc: "The hold also stops the IMU integration (1, firmware) or not (0)" },
  // manoeuvre modes
  { key: "FUSION_MAN_TAU_S", value: 1.0, min: 0.1, max: 5, step: 0.1, unit: "s", doc: "Time constant of the load, rate and coherence means" },
  { key: "SPIRAL_G", value: 1.2, min: 0, max: 3, step: 0.05, unit: "g", doc: "Spiral mode starts above this load (0 = off) ..." },
  { key: "SPIRAL_RATE", value: 45, min: 0, max: 200, step: 5, unit: "°/s", doc: "... and above this rotation rate (ends below 3/4 of it)" },
  { key: "SPIRAL_COHER", value: 0.6, min: 0, max: 1, step: 0.05, doc: "... with a steady rotation axis (|mean ω| / mean |ω|)" },
  { key: "SPIRAL_ENTER_S", value: 0.5, min: 0, max: 5, step: 0.1, unit: "s", doc: "The conditions must hold this long" },
  { key: "SPIRAL_PULLOUT_G", value: 2.0, min: 1, max: 5, step: 0.1, unit: "g", doc: "The pull-out rule applies once the spiral reached this load ..." },
  { key: "SPIRAL_EXIT_LOAD", value: 0.5, min: 0, max: 1, step: 0.05, doc: "... and the load fell to this share of the spiral's highest" },
  { key: "SPIRAL_EXIT_S", value: 0.5, min: 0, max: 5, step: 0.1, unit: "s", doc: "Spiral mode ends this long after the rotation or the load drop" },
  { key: "SPIRAL_RAMP_S", value: 1.5, min: 0, max: 10, step: 0.1, unit: "s", doc: "Cross-fade from the one-rotation climb to the filter after the spiral" },
  { key: "SPIRAL_FULL_G", value: 1.5, min: 1.05, max: 3, step: 0.05, unit: "g", doc: "Full averaging window from this load on, none at 1 g" },
  { key: "SPIRAL_LOCKOUT_S", value: 3, min: 0, max: 10, step: 0.5, unit: "s", doc: "No new spiral mode this long after one ended ..." },
  { key: "SPIRAL_LOCKOUT_MIN_S", value: 3, min: 0, max: 20, step: 0.5, unit: "s", doc: "... if it lasted at least this long" },
  { key: "FAST_RATE", value: 45, min: 0, max: 200, step: 5, unit: "°/s", doc: "IMU acceleration off while the rotation (1 s) is above this (0 = off)" },
  { key: "HANDLING_RATE", value: 100, min: 0, max: 500, step: 5, unit: "°/s", doc: "Handling: rotation above this (0 = off) ..." },
  { key: "HANDLING_COHER", value: 0.5, min: 0, max: 1, step: 0.05, doc: "... with an unsteady axis (coherence below this)" },
  { key: "HANDLING_HOLD_S", value: 1.0, min: 0, max: 10, step: 0.1, unit: "s", doc: "IMU acceleration off this long after handling" },
  { key: "REACQ_S", value: 3, min: 0, max: 10, step: 0.5, unit: "s", doc: "Attitude re-acquisition after a spiral or handling (IMU acceleration still off)" },
  { key: "REACQ_GATE", value: 0.3, min: 0, max: 1, step: 0.05, doc: "... accelerometer gate meanwhile" },
  { key: "BC_MAX", value: 3, min: 0, max: 100, step: 0.5, unit: "°/s", doc: "Anti-windup of the attitude filter's bias integrator" },
  { key: "REST_VS_GREF", value: 0, min: 0, max: 1, step: 1, doc: "Rest needs |a| near g (0, firmware: g_ref recovers from a bad first sample) or near g_ref (1, firmware 0.42 and earlier)" },
  // as fusion.c
  { key: "DYN_ALPHA", value: 0.05, min: 0.001, max: 1, step: 0.005, advanced: true, doc: "Horizontal specific force mean (per IMU sample)" },
  { key: "BARO_VAR_ALPHA", value: 0.02, min: 0.001, max: 1, step: 0.005, advanced: true, doc: "Barometer variance estimate (per sample)" },
  { key: "REST_GYRO_DEV", value: 0.5, min: 0, max: 10, step: 0.1, unit: "°/s", advanced: true, doc: "Rest: gyro fluctuation around its mean" },
  { key: "REST_GYRO_MAX", value: 5, min: 0, max: 50, step: 0.5, unit: "°/s", advanced: true, doc: "Rest: rotation after the bias" },
  { key: "REST_ACCEL_DEV", value: 0.02, min: 0, max: 0.2, step: 0.005, advanced: true, doc: "Rest: relative fluctuation of |a|" },
  { key: "REST_G0_TOL", value: 0.1, min: 0, max: 0.5, step: 0.01, advanced: true, doc: "Rest: |a| within this of g (REST_VS_GREF 0)" },
  { key: "REST_ACCEL", value: 0.03, min: 0, max: 0.2, step: 0.005, advanced: true, doc: "Rest: |a| within this of g_ref (REST_VS_GREF 1)" },
  { key: "REST_MS", value: 1000, min: 0, max: 10000, step: 100, unit: "ms", advanced: true, doc: "Rest after this long still" },
  { key: "MEAN_ALPHA", value: 0.02, min: 0.001, max: 1, step: 0.005, advanced: true, doc: "Running means for the rest detection" },
  { key: "BIAS_ALPHA", value: 0.01, min: 0, max: 1, step: 0.005, advanced: true, doc: "Gyro bias learning at rest" },
  { key: "G_REF_ALPHA", value: 0.002, min: 0, max: 0.1, step: 0.001, advanced: true, doc: "g_ref learning at rest" },
  { key: "INIT_GATE", value: 0.1, min: 0, max: 0.5, step: 0.01, advanced: true, doc: "First sample taken as this sensor's g within ±gate" },
  { key: "VARIO_FILTER_Q", value: 2.0, min: 0.01, max: 50, step: 0.1, advanced: true, doc: "Barometer-only filter before the attitude is known" },
  { key: "VARIO_FILTER_R", value: 0.01, min: 0.0001, max: 1, step: 0.001, advanced: true, doc: "... altitude variance" },
];

const diagnostics = [
  { key: "mode", label: "Mode (0 normal, 1 spiral, 2 handling, 3 hold, 4 ramp, 5 re-acquisition)", unit: "" },
  { key: "period", label: "Averaging window of the one-rotation climb", unit: "s" },
  { key: "coher", label: "Rotation coherence (|mean ω| / mean |ω|)", unit: "" },
  { key: "loadMean", label: "Load (1 s)", unit: "g" },
  { key: "rateMean", label: "Rotation rate (1 s)", unit: "°/s" },
  { key: "tilt", label: "Tilt of the device", unit: "°" },
  { key: "wUp", label: "Turn rate about the vertical", unit: "°/s" },
  { key: "aUp", label: "Vertical acceleration (bias removed)", unit: "m/s²" },
  { key: "hdyn", label: "Horizontal specific force", unit: "m/s²" },
  { key: "corr", label: "Accelerometer corrects the attitude", unit: "share" },
  { key: "baroSd", label: "Measured barometer noise", unit: "m" },
  { key: "bias", label: "Vertical accelerometer bias", unit: "m/s²" },
  { key: "gRef", label: "g of this sensor", unit: "m/s²" },
  { key: "gyroBias", label: "Learned gyro bias", unit: "°/s" },
  { key: "bc", label: "Attitude filter bias integrator", unit: "°/s" },
  { key: "rest", label: "At rest", unit: "0/1" },
];

function create(p) {
  const KP = p.KP, KI = p.KI, ACCEL_GATE = p.ACCEL_GATE, ATT_RATE_MAX = p.ATT_RATE_MAX * DEG;
  const REST_GYRO_DEV = p.REST_GYRO_DEV * DEG, REST_GYRO_MAX = p.REST_GYRO_MAX * DEG;
  const SPIRAL_RATE = p.SPIRAL_RATE * DEG, HANDLING_RATE = p.HANDLING_RATE * DEG, FAST_RATE = p.FAST_RATE * DEG;
  const BC_MAX = p.BC_MAX * DEG;

  const s = {
    q: [1, 0, 0, 0], bc: [0, 0, 0], gRef: G0, attitudeOk: false,
    gyroMean: [0, 0, 0], accelMean: 0, gyroBias: [0, 0, 0], stillMs: 0, atRest: false,
    hdyn: 0, baroLast: 0, baroVar: 0, baroHaveLast: false,
    h: 0, v: 0, b: 0, P: [[1, 0, 0], [0, 1, 0], [0, 0, 0.25]], started: false,
    vz: 0,
    t: 0, holdUntil: -1, held: false,
    // barometer-only fallback (vario.c)
    kf: { started: false, h: 0, v: 0, p00: 0, p01: 0, p11: 0 },
    // diagnostics accumulated between barometer samples
    nImu: 0, nCorr: 0, sumAUp: 0, sumWUp: 0, lastTilt: 0,
    // manoeuvres
    loadMean: 1, rateMean: 0, wMean: [0, 0, 0], coher: 0,
    spiral: false, spiralOnS: 0, spiralCalmS: 0, spiralStart: 0, loadMax: 0, rampLeft: 0, reseed: false,
    handlingUntil: -1, reacqUntil: -1, lockoutUntil: -1,
    window: 0, mode: 0,
    hist: [], histLast: -1, // [t, h] of the filter's altitude, one entry per HIST_STEP_S
  };

  function up(q) {
    return [2 * (q[1] * q[3] - q[0] * q[2]), 2 * (q[0] * q[1] + q[2] * q[3]),
      q[0] * q[0] - q[1] * q[1] - q[2] * q[2] + q[3] * q[3]];
  }

  function attitudeFromAccel(a) {
    const roll = Math.atan2(a[1], a[2]);
    const pitch = Math.atan2(-a[0], Math.hypot(a[1], a[2]));
    const cr = Math.cos(roll / 2), sr = Math.sin(roll / 2), cp = Math.cos(pitch / 2), sp = Math.sin(pitch / 2);
    s.q = [cr * cp, sr * cp, cr * sp, -sr * sp];
    s.attitudeOk = true;
  }

  const clamp = (x, lim) => (x < -lim ? -lim : x > lim ? lim : x);

  function attitudeUpdate(w, a, norm, dt) {
    let gx = w[0], gy = w[1], gz = w[2];
    const rate2 = gx * gx + gy * gy + gz * gz;
    // re-acquisition after a spiral or handling: the accelerometer pulls through a wide gate at any
    // rotation rate, without feeding the bias integrator
    const reacq = s.t < s.reacqUntil;
    const gate = reacq ? p.REACQ_GATE : ACCEL_GATE;
    if (norm > 0 && Math.abs(norm / s.gRef - 1) < gate && (reacq || rate2 < ATT_RATE_MAX * ATT_RATE_MAX)) {
      const ax = a[0] / norm, ay = a[1] / norm, az = a[2] / norm;
      const v = up(s.q);
      const ex = ay * v[2] - az * v[1], ey = az * v[0] - ax * v[2], ez = ax * v[1] - ay * v[0];
      if (!reacq) {
        s.bc[0] = clamp(s.bc[0] + KI * ex * dt, BC_MAX);
        s.bc[1] = clamp(s.bc[1] + KI * ey * dt, BC_MAX);
        s.bc[2] = clamp(s.bc[2] + KI * ez * dt, BC_MAX);
      }
      gx += KP * ex + s.bc[0];
      gy += KP * ey + s.bc[1];
      gz += KP * ez + s.bc[2];
      s.nCorr++;
    } else {
      gx += s.bc[0];
      gy += s.bc[1];
      gz += s.bc[2];
    }
    const q = s.q;
    const hx = 0.5 * dt * gx, hy = 0.5 * dt * gy, hz = 0.5 * dt * gz;
    const q0 = q[0], q1 = q[1], q2 = q[2], q3 = q[3];
    q[0] = q0 - q1 * hx - q2 * hy - q3 * hz;
    q[1] = q1 + q0 * hx + q2 * hz - q3 * hy;
    q[2] = q2 + q0 * hy - q1 * hz + q3 * hx;
    q[3] = q3 + q0 * hz + q1 * hy - q2 * hx;
    const n = Math.hypot(q[0], q[1], q[2], q[3]);
    for (let i = 0; i < 4; i++) q[i] /= n;
  }

  function restUpdate(w, norm, dt) {
    let dev2 = 0, rate2 = 0;
    for (let i = 0; i < 3; i++) {
      s.gyroMean[i] += p.MEAN_ALPHA * (w[i] - s.gyroMean[i]);
      const d = w[i] - s.gyroMean[i];
      const r = w[i] - s.gyroBias[i];
      dev2 += d * d;
      rate2 += r * r;
    }
    s.accelMean += p.MEAN_ALPHA * (norm - s.accelMean);
    const gOk = p.REST_VS_GREF >= 1 ? Math.abs(norm / s.gRef - 1) < p.REST_ACCEL : Math.abs(norm / G0 - 1) < p.REST_G0_TOL;
    const still = dev2 < REST_GYRO_DEV * REST_GYRO_DEV && rate2 < REST_GYRO_MAX * REST_GYRO_MAX &&
      Math.abs(norm / s.accelMean - 1) < p.REST_ACCEL_DEV && gOk;
    s.stillMs = still ? s.stillMs + Math.round(dt * 1000) : 0;
    s.atRest = s.stillMs >= p.REST_MS;
    if (s.atRest) {
      for (let i = 0; i < 3; i++) s.gyroBias[i] += p.BIAS_ALPHA * (w[i] - s.gyroBias[i]);
      const gOld = s.gRef;
      s.gRef += p.G_REF_ALPHA * (norm - s.gRef);
      s.b -= s.gRef - gOld;
    }
  }

  /* Manoeuvre means and the modes (fusion.c maneuver_update). */
  function maneuverUpdate(w, norm, dt) {
    const alpha = dt / (p.FUSION_MAN_TAU_S + dt);
    const rate = Math.hypot(w[0], w[1], w[2]);
    s.loadMean += alpha * (norm / s.gRef - s.loadMean);
    s.rateMean += alpha * (rate - s.rateMean);
    for (let i = 0; i < 3; i++) s.wMean[i] += alpha * (w[i] - s.wMean[i]);
    s.coher = s.rateMean > 1e-3 ? Math.hypot(s.wMean[0], s.wMean[1], s.wMean[2]) / s.rateMean : 0;

    if (p.HANDLING_RATE > 0 && s.rateMean > HANDLING_RATE && s.coher < p.HANDLING_COHER) {
      s.handlingUntil = s.t + p.HANDLING_HOLD_S;
      s.reacqUntil = s.handlingUntil + p.REACQ_S;
    }
    if (p.SPIRAL_G <= 0) return;
    const loaded = s.loadMean > p.SPIRAL_G && s.rateMean > SPIRAL_RATE && s.coher > p.SPIRAL_COHER;
    if (!s.spiral) {
      s.spiralOnS = loaded && s.t >= s.lockoutUntil ? s.spiralOnS + dt : 0;
      if (s.spiralOnS >= p.SPIRAL_ENTER_S) {
        s.spiral = true;
        s.spiralCalmS = 0;
        s.rampLeft = 0;
        s.loadMax = s.loadMean;
        s.spiralStart = s.t;
      }
      return;
    }
    if (s.loadMean > s.loadMax) s.loadMax = s.loadMean;
    const calm = s.rateMean < 0.75 * SPIRAL_RATE || (s.loadMax >= p.SPIRAL_PULLOUT_G && s.loadMean < p.SPIRAL_EXIT_LOAD * s.loadMax);
    s.spiralCalmS = calm ? s.spiralCalmS + dt : 0;
    if (s.spiralCalmS >= p.SPIRAL_EXIT_S) {
      s.spiral = false;
      s.spiralOnS = 0;
      s.rampLeft = p.SPIRAL_RAMP_S;
      s.reseed = true;
      s.reacqUntil = s.t + p.REACQ_S;
      if (s.t - s.spiralStart >= p.SPIRAL_LOCKOUT_MIN_S) s.lockoutUntil = s.t + p.SPIRAL_LOCKOUT_S;
    }
  }

  function imuOff() {
    return s.spiral || s.rampLeft > 0 || s.t < s.handlingUntil || s.t < s.reacqUntil ||
      (p.FREEZE_ON_HOLD >= 1 && s.t < s.holdUntil) || (p.FAST_RATE > 0 && s.rateMean > FAST_RATE);
  }

  function predict(a, dt) {
    if (!s.started) return;
    const off = imuOff();
    const ac = off ? 0 : a - s.b;
    s.h += s.v * dt + 0.5 * ac * dt * dt;
    s.v += ac * dt;
    const P = s.P;
    const F = off ? [[1, dt, 0], [0, 1, 0], [0, 0, 1]] : [[1, dt, -0.5 * dt * dt], [0, 1, -dt], [0, 0, 1]];
    const FP = [[0, 0, 0], [0, 0, 0], [0, 0, 0]];
    for (let i = 0; i < 3; i++) for (let j = 0; j < 3; j++) FP[i][j] = F[i][0] * P[0][j] + F[i][1] * P[1][j] + F[i][2] * P[2][j];
    const g = [0.5 * dt * dt, dt, 0];
    const sa = s.spiral || s.rampLeft > 0 ? p.FUSION_ACCEL_NOISE_SPIRAL : s.atRest ? p.FUSION_ACCEL_NOISE_REST : p.FUSION_ACCEL_NOISE + p.FUSION_DYN_GAIN * s.hdyn;
    const sa2 = sa * sa;
    for (let i = 0; i < 3; i++) {
      for (let j = 0; j < 3; j++) {
        P[i][j] = FP[i][0] * F[j][0] + FP[i][1] * F[j][1] + FP[i][2] * F[j][2] + sa2 * g[i] * g[j];
      }
    }
    if (off) {
      P[0][2] = P[2][0] = P[1][2] = P[2][1] = 0; // the bias state is frozen: no correlation builds up
    } else {
      P[2][2] += p.FUSION_BIAS_WALK * p.FUSION_BIAS_WALK * dt;
    }
  }

  function kfUpdate(alt, dt) {
    const f = s.kf;
    const R = p.VARIO_FILTER_R / (32 * inst.baroDt);
    if (!f.started) {
      f.h = alt; f.v = 0; f.p00 = R; f.p01 = 0; f.p11 = 1; f.started = true;
      return;
    }
    const dt2 = dt * dt;
    f.h += f.v * dt;
    const p00 = f.p00 + dt * 2 * f.p01 + dt2 * f.p11 + p.VARIO_FILTER_Q * dt2 * dt2 * 0.25;
    const p01 = f.p01 + dt * f.p11 + p.VARIO_FILTER_Q * dt2 * dt * 0.5;
    const p11 = f.p11 + p.VARIO_FILTER_Q * dt2;
    const sum = p00 + R;
    const k0 = p00 / sum, k1 = p01 / sum;
    const y = alt - f.h;
    f.h += k0 * y;
    f.v += k1 * y;
    f.p00 = (1 - k0) * p00;
    f.p01 = (1 - k0) * p01;
    f.p11 = p11 - k1 * p01;
  }

  function histPush() {
    if (s.hist.length && s.t - s.histLast < HIST_STEP_S) return;
    s.hist.push([s.t, s.h]);
    if (s.hist.length > HIST_N) s.hist.shift();
    s.histLast = s.t;
  }

  /* Climb rate as the change of the filter altitude over the last `win` seconds (NaN without the history). */
  function periodClimb(win) {
    const hist = s.hist;
    if (win < 0.05 || hist.length < 2) return NaN;
    const tOld = s.t - win;
    let i = hist.length - 1, newer = -1;
    while (i > 0 && hist[i][0] > tOld) { newer = i; i--; }
    if (hist[i][0] > tOld + HIST_STEP_S) return NaN; // not enough history yet
    let hOld = hist[i][1];
    if (newer >= 0 && hist[newer][0] > hist[i][0]) {
      const f = Math.max(0, Math.min(1, (tOld - hist[i][0]) / (hist[newer][0] - hist[i][0])));
      hOld += (hist[newer][1] - hOld) * f;
    }
    return (s.h - hOld) / win;
  }

  const inst = {
    vz: 0,
    h: NaN,
    baroDt: 1 / 32,

    imu(tMs, w, a, dt) {
      const norm = Math.hypot(a[0], a[1], a[2]);
      if (!s.attitudeOk) {
        if (norm > 0.5 * G0) {
          attitudeFromAccel(a);
          if (Math.abs(norm / G0 - 1) < p.INIT_GATE) s.gRef = norm;
          s.accelMean = norm;
          s.gyroMean = [w[0], w[1], w[2]];
        }
        return;
      }
      s.t += dt;
      restUpdate(w, norm, dt);
      const wc = [w[0] - s.gyroBias[0], w[1] - s.gyroBias[1], w[2] - s.gyroBias[2]];
      maneuverUpdate(wc, norm, dt);
      if (s.rampLeft > 0) {
        s.rampLeft -= dt;
        if (s.rampLeft <= 0) s.window = 0;
      }
      if (s.reseed && !s.spiral && Math.abs(norm / s.gRef - 1) < ACCEL_GATE) {
        attitudeFromAccel(a); /* the gyro-only attitude of the spiral has drifted: start over */
        s.bc = [0, 0, 0];
        s.reseed = false;
      }
      attitudeUpdate(wc, a, norm, dt);
      const v = up(s.q);
      const upA = a[0] * v[0] + a[1] * v[1] + a[2] * v[2];
      const hor2 = norm * norm - upA * upA;
      s.hdyn += p.DYN_ALPHA * (Math.sqrt(hor2 > 0 ? hor2 : 0) - s.hdyn);
      const aUp = upA - s.gRef;
      predict(aUp, dt);
      s.nImu++;
      s.sumAUp += aUp - s.b;
      s.sumWUp += (wc[0] * v[0] + wc[1] * v[1] + wc[2] * v[2]) / DEG;
      s.lastTilt = Math.acos(Math.max(-1, Math.min(1, v[2]))) / DEG;
    },

    baro(tMs, alt) {
      // barometer-only filter: measured interval, altitude variance per sample scaled with the rate
      const dt = s.lastBaroMs == null ? inst.baroDt : Math.min(0.1, Math.max(0.005, (tMs - s.lastBaroMs) / 1000));
      s.lastBaroMs = tMs;
      kfUpdate(alt, dt);
      if (s.attitudeOk) {
        const scale = 32 * inst.baroDt; // FUSION_BARO_RATE_REF / rate
        const kRate = 1 / scale;
        if (s.baroHaveLast) {
          const d = alt - s.baroLast;
          const e = 0.5 * d * d;
          s.baroVar = s.baroVar > 0 ? s.baroVar + p.BARO_VAR_ALPHA * scale * (e - s.baroVar) : e;
        }
        s.baroLast = alt;
        s.baroHaveLast = true;
        if (!s.started) {
          s.h = alt;
          s.v = 0;
          s.started = true;
          histPush();
        } else {
          let r = Math.min(p.FUSION_BARO_VAR_MAX * kRate, Math.max(p.FUSION_BARO_VAR_MIN * kRate, s.baroVar));
          if (s.atRest && r < p.FUSION_BARO_VAR_REST * kRate) r = p.FUSION_BARO_VAR_REST * kRate;
          if (s.t < s.holdUntil) {
            r = p.FUSION_BARO_VAR_HOLD * kRate;
            s.held = true;
          } else if (s.held) {
            s.h = alt;
            s.held = false;
          }
          // the filter always sees the raw altitude; with the IMU off the bias state gets no gain
          const P = s.P;
          const n = imuOff() ? 2 : 3;
          const sum = P[0][0] + r;
          const k = [P[0][0] / sum, P[1][0] / sum, n > 2 ? P[2][0] / sum : 0];
          const y = alt - s.h;
          s.h += k[0] * y;
          s.v += k[1] * y;
          s.b += k[2] * y;
          const row0 = [P[0][0], P[0][1], P[0][2]];
          for (let i = 0; i < n; i++) for (let j = 0; j < n; j++) P[i][j] -= k[i] * row0[j];
          histPush();
          // spiral: the climb rate is the altitude change over exactly one rotation period, cross-faded
          // back to the filter afterwards
          let vz = s.v;
          if (s.spiral || s.rampLeft > 0) {
            if (s.spiral && s.rateMean >= 0.75 * SPIRAL_RATE) {
              s.window = Math.min(WINDOW_MAX_S, Math.max(WINDOW_MIN_S, 2 * Math.PI / s.rateMean));
            }
            const share = Math.max(0, Math.min(1, (s.loadMean - 1) / (p.SPIRAL_FULL_G - 1)));
            const vS = periodClimb(s.window * share);
            if (Number.isFinite(vS)) {
              const wgt = s.spiral ? 1 : s.rampLeft / p.SPIRAL_RAMP_S;
              vz = wgt * vS + (1 - wgt) * s.v;
            }
          }
          s.vz = vz;
          s.mode = s.spiral ? 1 : s.rampLeft > 0 ? 4 : s.t < s.handlingUntil ? 2 : s.t < s.holdUntil ? 3 : s.t < s.reacqUntil ? 5 : 0;
        }
      }
      const fused = s.attitudeOk;
      inst.vz = fused ? s.vz : s.kf.v;
      inst.h = fused ? s.h : s.kf.h;
    },

    event(tMs, text) {
      if (p.FUSION_HOLD_S > 0 && (text.startsWith("tap ") || text.startsWith("button "))) {
        if (s.t + p.FUSION_HOLD_S > s.holdUntil) s.holdUntil = s.t + p.FUSION_HOLD_S;
      }
    },

    fix() {},

    diag(out) {
      const n = s.nImu || 1;
      out.mode = s.mode;
      out.period = s.window;
      out.coher = s.coher;
      out.loadMean = s.loadMean;
      out.rateMean = s.rateMean / DEG;
      out.tilt = s.lastTilt;
      out.wUp = s.sumWUp / n;
      out.aUp = s.sumAUp / n;
      out.hdyn = s.hdyn;
      out.corr = s.nCorr / n;
      out.baroSd = Math.sqrt(s.baroVar);
      out.bias = s.b;
      out.gRef = s.gRef;
      out.gyroBias = Math.hypot(s.gyroBias[0], s.gyroBias[1], s.gyroBias[2]) / DEG;
      out.bc = Math.hypot(s.bc[0], s.bc[1], s.bc[2]) / DEG;
      out.rest = s.atRest ? 1 : 0;
      s.nImu = 0;
      s.nCorr = 0;
      s.sumAUp = 0;
      s.sumWUp = 0;
    },
  };
  return inst;
}

export default {
  id: "fusion",
  name: "Firmware fusion",
  description: "Mahony attitude (gated) + 3-state vertical Kalman filter with the manoeuvre modes (spiral, handling), as in the firmware (fusion.c)",
  params,
  diagnostics,
  create,
};
