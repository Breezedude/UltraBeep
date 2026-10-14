/*
 * Port of the barometer-only filter of the firmware (firmware/src/app/vario.c, vario_filter_*):
 * 2-state Kalman filter (altitude, climb rate), the vertical acceleration as white noise.
 * The device uses it while the IMU has no attitude yet and as the fallback without IMU.
 * Since 0.21 with the measured interval between samples and R per sample scaled with the rate.
 */

const params = [
  { key: "Q", value: 2.0, min: 0.01, max: 50, step: 0.1, unit: "(m/s²)²·s", doc: "Acceleration noise density (VARIO_FILTER_Q)" },
  { key: "R", value: 0.01, min: 0.0001, max: 2, step: 0.001, unit: "m²", doc: "Altitude measurement variance (VARIO_FILTER_R)" },
];

function create(p) {
  const f = { started: false, h: 0, v: 0, p00: 0, p01: 0, p11: 0, lastMs: null };
  const inst = {
    vz: 0,
    h: NaN,
    baroDt: 1 / 32,
    imu() {},
    fix() {},
    event() {},
    baro(tMs, alt) {
      const dt = f.lastMs == null ? inst.baroDt : Math.min(0.1, Math.max(0.005, (tMs - f.lastMs) / 1000));
      f.lastMs = tMs;
      const R = p.R / (32 * inst.baroDt); // per sample at 32 Hz
      if (!f.started) {
        f.h = alt; f.v = 0; f.p00 = R; f.p01 = 0; f.p11 = 1; f.started = true;
      } else {
        const dt2 = dt * dt;
        f.h += f.v * dt;
        const p00 = f.p00 + dt * 2 * f.p01 + dt2 * f.p11 + p.Q * dt2 * dt2 * 0.25;
        const p01 = f.p01 + dt * f.p11 + p.Q * dt2 * dt * 0.5;
        const p11 = f.p11 + p.Q * dt2;
        const s = p00 + R;
        const k0 = p00 / s, k1 = p01 / s;
        const y = alt - f.h;
        f.h += k0 * y;
        f.v += k1 * y;
        f.p00 = (1 - k0) * p00;
        f.p01 = (1 - k0) * p01;
        f.p11 = p11 - k1 * p01;
      }
      inst.vz = f.v;
      inst.h = f.h;
    },
    diag() {},
  };
  return inst;
}

export default {
  id: "baro_kf",
  name: "Barometer only (Kalman)",
  description: "2-state Kalman filter on the barometric altitude, as vario.c (no IMU)",
  params,
  diagnostics: [],
  create,
};
