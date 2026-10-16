/*
 * The pilot's tilt against the true vertical as the firmware measures it: a port of firmware/src/app/tilt.c
 * (see firmware/inc/tilt.h for the method). Learns the gyro bias on the ground and the pilot's axes in flight,
 * then tracks the true vertical in the device's frame. No DOM. Change both files together.
 */
const DEG = Math.PI / 180;
const G = 9.80665;
export const TILT = {
  smoothS: 0.5, stillDps: 3, biasG: 0.03, biasS: 10, straightDps: 8, straightG: 0.08, straightS: 10,
  turnMinDps: 8, turnMaxDps: 80, turnG: 0.5, turnS: 10, minLean: 0.05,
  tauS: 3, gate: 0.25, spinDps: 30, spinCoher: 0.85, spinTauS: 0.7, spinHoldS: 1.5, spinAlign: 0.866,
};

const dot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
const norm = (a) => Math.sqrt(dot(a, a));
const cross = (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];

export class TiltEstimator {
  constructor(p = TILT) {
    this.p = p;
    this.bias = [0, 0, 0]; this.nBias = 0; this.started = false;
    this.fs = [0, 0, 0]; this.ws = [0, 0, 0]; this.rs = 0;
    this.mSum = [0, 0, 0]; this.nStraight = 0; this.lSum = [0, 0, 0]; this.nTurn = 0;
    this.M = [0, 0, 1]; this.F = [1, 0, 0]; this.axes = false;
    this.u = [0, 0, 1]; this.uOk = false; this.spinS = 0; this.tiltDeg = NaN;
  }

  learnAxes(dt) {
    const p = this.p;
    if (this.nStraight * dt < p.straightS || this.nTurn * dt < p.turnS) return;
    const mn = norm(this.mSum);
    if (mn <= 0) return;
    const m = this.mSum.map((v) => v / mn);
    const along = dot(this.lSum, m);
    const l = [0, 1, 2].map((k) => this.lSum[k] - along * m[k]);
    const ln = norm(l);
    if (ln / this.nTurn < p.minLean) return;
    this.M = m;
    this.F = cross(l.map((v) => v / ln), m);
    this.axes = true;
  }

  /* one IMU sample: a calibrated [m/s²], w as measured [rad/s]; returns whether tiltDeg is valid */
  update(a, w, dt, inFlight, speed) {
    const p = this.p;
    const wc = [w[0] - this.bias[0], w[1] - this.bias[1], w[2] - this.bias[2]];
    const wcn = norm(wc);
    if (!this.started) {
      this.fs = [a[0], a[1], a[2]]; this.ws = wc.slice(); this.rs = wcn; this.started = true;
    } else {
      const alpha = dt / (p.smoothS + dt);
      for (let k = 0; k < 3; k++) { this.fs[k] += alpha * (a[k] - this.fs[k]); this.ws[k] += alpha * (wc[k] - this.ws[k]); }
      this.rs += alpha * (wcn - this.rs);
    }
    const fsn = norm(this.fs), wsn = norm(this.ws), load = fsn / G;
    if (!inFlight) {
      if (this.rs < p.stillDps * DEG && wcn < p.stillDps * DEG && Math.abs(norm(a) / G - 1) < p.biasG) {
        this.nBias++;
        const k = Math.max(1 / this.nBias, dt / p.biasS);
        for (let i = 0; i < 3; i++) this.bias[i] += k * (w[i] - this.bias[i]);
      }
    } else if (fsn > 0) {
      if (wsn < p.straightDps * DEG && Math.abs(load - 1) < p.straightG) {
        for (let k = 0; k < 3; k++) this.mSum[k] += this.fs[k] / fsn;
        this.nStraight++;
        this.learnAxes(dt);
      } else if (wsn > p.turnMinDps * DEG && wsn < p.turnMaxDps * DEG && Math.abs(load - 1) < p.turnG) {
        for (let k = 0; k < 3; k++) this.lSum[k] -= this.ws[k] / wsn;
        this.nTurn++;
        this.learnAxes(dt);
      }
    }
    const fc = [a[0], a[1], a[2]];
    if (this.axes) {
      const cen = cross(wc, [speed * this.F[0], speed * this.F[1], speed * this.F[2]]);
      for (let k = 0; k < 3; k++) fc[k] -= cen[k];
    }
    const fcn = norm(fc);
    if (!this.uOk) {
      if (fcn > 0.5 * G) { this.u = fc.map((v) => v / fcn); this.uOk = true; }
    } else {
      const du = cross(this.u, wc);
      for (let k = 0; k < 3; k++) this.u[k] += du[k] * dt;
      if (Math.abs(fcn / G - 1) < p.gate) {
        const k = dt / p.tauS;
        for (let i = 0; i < 3; i++) this.u[i] += k * (fc[i] / fcn - this.u[i]);
      }
      const spin = wsn > p.spinDps * DEG && wsn > p.spinCoher * this.rs;
      this.spinS = spin ? this.spinS + dt : 0;
      const along = spin ? dot(this.ws, this.u) / wsn : 0;
      if (this.spinS >= p.spinHoldS && Math.abs(along) > p.spinAlign) {
        const k = dt / p.spinTauS, sg = along < 0 ? -1 : 1;
        for (let i = 0; i < 3; i++) this.u[i] += k * (sg * this.ws[i] / wsn - this.u[i]);
      }
      const un = norm(this.u);
      if (un > 0) for (let k = 0; k < 3; k++) this.u[k] /= un;
    }
    if (!this.axes || !this.uOk) return false;
    const c = dot(this.u, this.M);
    this.tiltDeg = Math.acos(Math.max(-1, Math.min(1, c))) / DEG;
    return true;
  }
}
