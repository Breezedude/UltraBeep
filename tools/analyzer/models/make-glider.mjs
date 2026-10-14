#!/usr/bin/env node
/*
 * Builds the paraglider model of the 3D map (models/glider.glb, glTF 2.0) from code, so it needs
 * no third-party asset: an arched wing of 52 inflated cells with an airfoil section and a printed
 * pattern (lime with navy and green stripes, texture generated here), the lines as line
 * primitives, and a pilot in a pod harness.
 *
 *     node models/make-glider.mjs      (from tools/analyzer)
 *
 * Axes as glTF has them: +Y up, +Z forward (the pilot looks along +Z), +X to the pilot's left.
 * Units are metres, true size (about 9 m projected span). The origin is the pilot's chest, where
 * the vario sits, so the model turns about the measured position.
 */
import { writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { deflateSync } from "node:zlib";

const OUT = join(dirname(fileURLToPath(import.meta.url)), "glider.glb");

/* ---------------------------------------------------------------- wing shape */
const CELLS = 52;            /* both sides together */
const SEG_PER_CELL = 4;      /* span segments per cell: the inflation bulge */
const CHORD_N = 24;          /* points per surface along the chord */
const HALF_SPAN = 5.6;       /* m, flat */
const ROOT_CHORD = 2.55;     /* m */
const ARC_MAX = 1.55;        /* rad: slope of the arc at the tip */
const WING_Y = 7.2;          /* m: the wing's centre above the pilot */
const LE_Z = 0.85;           /* m: the leading edge ahead of the pilot at the centre */
const THICK = 0.16;          /* thickness / chord */
const CAMBER = 0.035;
const BULGE = 0.07;          /* inflation of a cell, share of the local thickness */

/* along the half span, u = 0 (centre) .. 1 (tip): slope, chord, leading edge */
const slope = (u) => ARC_MAX * (0.45 * u + 0.55 * u ** 4);
const chordAt = (u) => ROOT_CHORD * Math.sqrt(Math.max(0.05, 1 - 0.95 * Math.pow(u, 2.2)));
const leAt = (u) => LE_Z - 0.75 * u * u * u;

/* the arc in the X-Y plane, integrated along the flat span */
const ARC_STEPS = 2000;
const arc = [[0, WING_Y]];
for (let k = 1; k <= ARC_STEPS; k++) {
  const u = (k - 0.5) / ARC_STEPS, ds = HALF_SPAN / ARC_STEPS;
  const [x, y] = arc[k - 1];
  arc.push([x + Math.cos(slope(u)) * ds, y - Math.sin(slope(u)) * ds]);
}
function spanPoint(s) { /* s in -1 (right tip) .. 1 (left tip) */
  const u = Math.abs(s), f = u * ARC_STEPS, k = Math.min(ARC_STEPS - 1, Math.floor(f)), r = f - k;
  const x = arc[k][0] + (arc[k + 1][0] - arc[k][0]) * r, y = arc[k][1] + (arc[k + 1][1] - arc[k][1]) * r;
  const a = slope(u), sg = Math.sign(s) || 1;
  return { p: [sg * x, y], n: [sg * Math.sin(a), Math.cos(a)] }; /* n: the section's "up" */
}

/* airfoil: thickness (NACA 4-digit form) and a parabolic camber, t = 0 at the leading edge */
const thickness = (t) => 5 * THICK * (0.2969 * Math.sqrt(t) - 0.126 * t - 0.3516 * t * t + 0.2843 * t ** 3 - 0.1036 * t ** 4);
const camber = (t) => (t < 0.3 ? CAMBER * (2 * 0.3 * t - t * t) / 0.09 : CAMBER * ((1 - 2 * 0.3) + 2 * 0.3 * t - t * t) / 0.49);
/* chord stations, denser at the leading edge */
const stations = Array.from({ length: CHORD_N }, (_, i) => (1 - Math.cos(Math.PI * i / (CHORD_N - 1))) / 2);

/* ---------------------------------------------------------------- mesh builder */
const meshes = []; /* {name, positions, normals, uvs?, indices, material, mode} */

function wingSurface(upper) {
  const pos = [], uv = [], idx = [];
  const nSpan = CELLS * SEG_PER_CELL;
  for (let j = 0; j <= nSpan; j++) {
    const s = -1 + 2 * j / nSpan;
    const cellFrac = (j % SEG_PER_CELL) / SEG_PER_CELL;
    const bulge = 1 + BULGE * Math.sin(Math.PI * cellFrac);
    const { p, n } = spanPoint(s);
    const u = Math.abs(s), c = chordAt(u), le = leAt(u);
    for (const t of stations) {
      const th = thickness(t) * bulge, cm = camber(t);
      const off = (upper ? cm + th / 2 : cm - th / 2 * 0.6) * c; /* the lower surface is flatter */
      pos.push(p[0] + n[0] * off, p[1] + n[1] * off, le - t * c);
      uv.push((1 - s) / 2, upper ? 0.01 + 0.48 * t : 0.51 + 0.48 * t);
    }
  }
  const row = CHORD_N;
  for (let j = 0; j < nSpan; j++) {
    for (let i = 0; i < row - 1; i++) {
      const a = j * row + i, b = a + row;
      if (upper) idx.push(a, b, a + 1, a + 1, b, b + 1);
      else idx.push(a, a + 1, b, a + 1, b + 1, b);
    }
  }
  return { pos, uv, idx };
}

/* the tip caps close the section at both ends */
function tipCap(s) {
  const { p, n } = spanPoint(s);
  const u = Math.abs(s), c = chordAt(u), le = leAt(u);
  const pos = [], uv = [], idx = [];
  const ring = [];
  for (const t of stations) ring.push((camber(t) + thickness(t) / 2) * c, t);
  for (let i = stations.length - 1; i >= 0; i--) ring.push((camber(stations[i]) - thickness(stations[i]) / 2 * 0.6) * c, stations[i]);
  for (let k = 0; k < ring.length; k += 2) {
    const off = ring[k], t = ring[k + 1];
    pos.push(p[0] + n[0] * off, p[1] + n[1] * off, le - t * c);
    uv.push((1 - s) / 2, 0.75);
  }
  const m = pos.length / 3;
  for (let k = 1; k < m - 1; k++) idx.push(0, k, k + 1);
  return { pos, uv, idx };
}

function ellipsoid(center, radii, tiltX = 0, nu = 24, nv = 16) {
  const pos = [], nrm = [], idx = [];
  const ct = Math.cos(tiltX), st = Math.sin(tiltX);
  for (let i = 0; i <= nv; i++) {
    const th = Math.PI * i / nv;
    for (let j = 0; j <= nu; j++) {
      const ph = 2 * Math.PI * j / nu;
      const ux = Math.sin(th) * Math.cos(ph), uy = Math.cos(th), uz = Math.sin(th) * Math.sin(ph);
      let x = radii[0] * ux, y = radii[1] * uy, z = radii[2] * uz;
      let nx = ux / radii[0], ny = uy / radii[1], nz = uz / radii[2];
      [y, z] = [y * ct - z * st, y * st + z * ct];
      [ny, nz] = [ny * ct - nz * st, ny * st + nz * ct];
      const l = Math.hypot(nx, ny, nz);
      pos.push(center[0] + x, center[1] + y, center[2] + z);
      nrm.push(nx / l, ny / l, nz / l);
    }
  }
  for (let i = 0; i < nv; i++) {
    for (let j = 0; j < nu; j++) {
      const a = i * (nu + 1) + j, b = a + nu + 1;
      idx.push(a, a + 1, b, a + 1, b + 1, b);
    }
  }
  return { pos, nrm, idx };
}

/* smooth normals; vertices at the same place (the seams at the leading and trailing edge) share one */
function smoothNormals(pos, idx) {
  const n = new Float64Array(pos.length);
  for (let k = 0; k < idx.length; k += 3) {
    const [a, b, c] = [idx[k] * 3, idx[k + 1] * 3, idx[k + 2] * 3];
    const e1 = [pos[b] - pos[a], pos[b + 1] - pos[a + 1], pos[b + 2] - pos[a + 2]];
    const e2 = [pos[c] - pos[a], pos[c + 1] - pos[a + 1], pos[c + 2] - pos[a + 2]];
    const f = [e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]];
    for (const v of [a, b, c]) for (let d = 0; d < 3; d++) n[v + d] += f[d];
  }
  const key = (i) => `${pos[i].toFixed(4)},${pos[i + 1].toFixed(4)},${pos[i + 2].toFixed(4)}`;
  const sum = new Map();
  for (let i = 0; i < pos.length; i += 3) {
    const k = key(i), s = sum.get(k) || [0, 0, 0];
    s[0] += n[i]; s[1] += n[i + 1]; s[2] += n[i + 2];
    sum.set(k, s);
  }
  const out = [];
  for (let i = 0; i < pos.length; i += 3) {
    const s = sum.get(key(i)), l = Math.hypot(...s) || 1;
    out.push(s[0] / l, s[1] / l, s[2] / l);
  }
  return out;
}

function merge(parts) {
  const pos = [], uv = [], idx = [];
  for (const p of parts) {
    const base = pos.length / 3;
    pos.push(...p.pos);
    uv.push(...p.uv);
    for (const i of p.idx) idx.push(base + i);
  }
  return { pos, uv, idx };
}

/* ---------------------------------------------------------------- wing texture */
const TEX_W = 1024, TEX_H = 512;
const LIME = [214, 240, 58], LIME_LOW = [178, 204, 46], NAVY = [38, 66, 98], GREEN = [26, 128, 40];

function wingPixel(s, t, upper) {
  /* s: -1 (right tip) .. 1 (left tip), t: 0 leading edge .. 1 trailing edge */
  const a = Math.abs(s);
  let col = upper ? LIME : LIME_LOW;
  if (upper) {
    // a broad navy band over the outer wing, drifting back towards the tip, notched at every other
    // cell, with a gap and an oval in it
    const navyC = 0.24 + 0.2 * Math.max(0, a - 0.34);
    const notch = Math.floor((a * CELLS) / 2) % 2 === 0 ? 0.015 : 0;
    if (a > 0.34 && a < 0.95 && Math.abs(t - navyC) < 0.075 - notch && !(a > 0.6 && a < 0.66)) col = NAVY;
    if (Math.hypot((a - 0.63) / 0.026, (t - navyC) / 0.07) < 1) col = NAVY;
    // a green step in the centre: a broad band that jumps back at |s| = 0.13 and turns forward at 0.34
    const step = a < 0.13 ? 0.3 : 0.5;
    if (a < 0.34 && Math.abs(t - step) < 0.085) col = GREEN;
    if (Math.abs(a - 0.13) < 0.04 && t > 0.215 && t < 0.585) col = GREEN;
    if (a > 0.3 && a < 0.36 && t > navyC && t < 0.585) col = GREEN;
    // green chevrons at the tips
    if (a > 0.95 && a < 0.985 && t > 0.1 && t < 0.6 && Math.floor(t * 12) % 2 === 0) col = GREEN;
  }
  // the ribs: a slightly darker seam at every cell boundary, the stitching along the chord
  const cell = (s + 1) / 2 * CELLS, fr = cell - Math.floor(cell);
  const rib = Math.min(fr, 1 - fr) < 0.06 ? 0.86 : 1;
  const leShade = upper ? 1 - 0.1 * Math.max(0, 0.08 - t) / 0.08 : 0.92;
  return col.map((v) => Math.round(v * rib * leShade));
}

function wingTexture() {
  const px = new Uint8Array(TEX_W * TEX_H * 3);
  for (let y = 0; y < TEX_H; y++) {
    const v = (y + 0.5) / TEX_H;
    const upper = v < 0.5;
    const t = Math.max(0, Math.min(1, ((upper ? v : v - 0.5) - 0.01) / 0.48));
    for (let x = 0; x < TEX_W; x++) {
      const s = 1 - 2 * (x + 0.5) / TEX_W;
      const c = wingPixel(s, t, upper);
      px.set(c, (y * TEX_W + x) * 3);
    }
  }
  return png(TEX_W, TEX_H, px);
}

/* PNG, 8-bit RGB */
const CRC = new Uint32Array(256).map((_, n) => {
  let c = n;
  for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
  return c >>> 0;
});
function crc32(buf) {
  let c = 0xffffffff;
  for (const b of buf) c = CRC[(c ^ b) & 255] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}
function png(w, h, rgb) {
  const raw = Buffer.alloc((w * 3 + 1) * h);
  for (let y = 0; y < h; y++) Buffer.from(rgb.buffer, y * w * 3, w * 3).copy(raw, y * (w * 3 + 1) + 1);
  const chunk = (type, data) => {
    const b = Buffer.alloc(12 + data.length);
    b.writeUInt32BE(data.length, 0);
    b.write(type, 4, "latin1");
    data.copy(b, 8);
    b.writeUInt32BE(crc32(b.subarray(4, 8 + data.length)), 8 + data.length);
    return b;
  };
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4);
  ihdr[8] = 8; ihdr[9] = 2;
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk("IHDR", ihdr), chunk("IDAT", deflateSync(raw, { level: 9 })), chunk("IEND", Buffer.alloc(0))]);
}

/* ---------------------------------------------------------------- lines and pilot */
const RISER_TOP = [0.24, 0.55, 0.05];   /* left riser end; the right one mirrored */
const CARABINER = [0.2, 0.0, 0.0];

function wingLowerPoint(s, t) {
  const { p, n } = spanPoint(s);
  const u = Math.abs(s), c = chordAt(u), le = leAt(u);
  const off = (camber(t) - thickness(t) / 2 * 0.6) * c;
  return [p[0] + n[0] * off, p[1] + n[1] * off, le - t * c];
}

function linesMesh() {
  const pos = [];
  const seg = (a, b) => pos.push(...a, ...b);
  for (const side of [1, -1]) {
    const riser = [side * RISER_TOP[0], RISER_TOP[1], RISER_TOP[2]];
    seg([side * CARABINER[0], CARABINER[1], CARABINER[2]], riser);
    // A, B and C rows on every fourth rib, the brake lines to the trailing edge
    for (let r = 1; r <= CELLS / 2; r += 4) {
      const s = side * (r / (CELLS / 2)) * 0.97;
      for (const t of [0.12, 0.38, 0.66]) seg(riser, wingLowerPoint(s, t));
    }
    for (let r = 6; r <= CELLS / 2; r += 4) seg([side * 0.26, 0.45, -0.25], wingLowerPoint(side * (r / (CELLS / 2)) * 0.97, 0.99));
  }
  return pos;
}

/* ---------------------------------------------------------------- glTF */
function glb(parts, texturePng) {
  const chunks = [];
  let length = 0;
  const bufferViews = [], accessors = [];
  const add = (bytes, target) => {
    const pad = (4 - (length % 4)) % 4;
    if (pad) { chunks.push(Buffer.alloc(pad)); length += pad; }
    bufferViews.push({ buffer: 0, byteOffset: length, byteLength: bytes.byteLength, ...(target ? { target } : {}) });
    chunks.push(Buffer.from(bytes.buffer, bytes.byteOffset, bytes.byteLength));
    length += bytes.byteLength;
    return bufferViews.length - 1;
  };
  const accessor = (arr, type, comps, target, minmax) => {
    const isIdx = target === 34963;
    const data = isIdx ? new Uint16Array(arr) : new Float32Array(arr);
    const acc = { bufferView: add(data, target), componentType: isIdx ? 5123 : 5126, count: arr.length / comps, type };
    if (minmax) {
      acc.min = [Infinity, Infinity, Infinity];
      acc.max = [-Infinity, -Infinity, -Infinity];
      for (let i = 0; i < arr.length; i += 3) for (let d = 0; d < 3; d++) {
        acc.min[d] = Math.min(acc.min[d], arr[i + d]);
        acc.max[d] = Math.max(acc.max[d], arr[i + d]);
      }
    }
    accessors.push(acc);
    return accessors.length - 1;
  };
  const materials = [], meshesOut = [];
  for (const p of parts) {
    if (p.pos.length / 3 > 65535) throw new Error(`${p.name}: too many vertices for 16-bit indices`);
    const attributes = { POSITION: accessor(p.pos, "VEC3", 3, 34962, true) };
    if (p.nrm) attributes.NORMAL = accessor(p.nrm, "VEC3", 3, 34962);
    if (p.uv) attributes.TEXCOORD_0 = accessor(p.uv, "VEC2", 2, 34962);
    const prim = { attributes, material: materials.length, mode: p.mode ?? 4 };
    if (p.idx) prim.indices = accessor(p.idx, "SCALAR", 1, 34963);
    materials.push(p.material);
    meshesOut.push({ name: p.name, primitives: [prim] });
  }
  const image = add(texturePng);
  const json = {
    asset: { version: "2.0", generator: "UltraBeep make-glider.mjs" },
    scene: 0,
    scenes: [{ nodes: meshesOut.map((_, i) => i) }],
    nodes: meshesOut.map((m, i) => ({ name: m.name, mesh: i })),
    meshes: meshesOut, materials, accessors, bufferViews,
    buffers: [{ byteLength: length }],
    images: [{ bufferView: image, mimeType: "image/png" }],
    samplers: [{ magFilter: 9729, minFilter: 9987, wrapS: 33071, wrapT: 33071 }],
    textures: [{ sampler: 0, source: 0 }],
  };
  const bin = Buffer.concat(chunks);
  const binPad = Buffer.concat([bin, Buffer.alloc((4 - (bin.length % 4)) % 4)]);
  json.buffers[0].byteLength = binPad.length;
  let js = Buffer.from(JSON.stringify(json), "utf8");
  js = Buffer.concat([js, Buffer.alloc((4 - (js.length % 4)) % 4, 0x20)]);
  const header = Buffer.alloc(12);
  header.writeUInt32LE(0x46546c67, 0); header.writeUInt32LE(2, 4);
  header.writeUInt32LE(12 + 8 + js.length + 8 + binPad.length, 8);
  const ch = (type, data) => { const h = Buffer.alloc(8); h.writeUInt32LE(data.length, 0); h.writeUInt32LE(type, 4); return Buffer.concat([h, data]); };
  return Buffer.concat([header, ch(0x4e4f534a, js), ch(0x004e4942, binPad)]);
}

/* deck.gl lights a model without an environment map, which leaves it dark: an emission of a share of
   the base colour lifts it, the lights still shade the cells */
const GLOW = 0.7;
const mat = (name, rgb) => {
  const lin = rgb.map((v) => (v / 255) ** 2.2);
  return {
    name, doubleSided: true, emissiveFactor: lin.map((v) => v * GLOW),
    pbrMetallicRoughness: { baseColorFactor: [...lin, 1], metallicFactor: 0, roughnessFactor: 0.85 },
  };
};

const wing = merge([wingSurface(true), wingSurface(false), tipCap(-1), tipCap(1)]);
wing.nrm = smoothNormals(wing.pos, wing.idx);
const parts = [
  {
    name: "wing", ...wing,
    material: {
      name: "canopy", doubleSided: true, emissiveTexture: { index: 0 }, emissiveFactor: [GLOW, GLOW, GLOW],
      pbrMetallicRoughness: { baseColorTexture: { index: 0 }, metallicFactor: 0, roughnessFactor: 0.7 },
    },
  },
];
{
  // luma.gl's glTF loader draws only indexed primitives
  const pos = linesMesh(), n = pos.length / 3;
  parts.push({ name: "lines", pos, nrm: Array.from({ length: n }, () => [0, 1, 0]).flat(), idx: Array.from({ length: n }, (_, i) => i), mode: 1, material: mat("lines", [175, 175, 175]) });
}
const body = (name, e, rgb) => parts.push({ name, pos: e.pos, nrm: e.nrm, idx: e.idx, material: mat(name, rgb) });
body("pod", ellipsoid([0, -0.2, 0.3], [0.27, 0.26, 0.78], -0.12), [40, 44, 52]);
body("back", ellipsoid([0, 0.0, -0.22], [0.26, 0.32, 0.22]), [40, 44, 52]);
body("torso", ellipsoid([0, 0.28, -0.04], [0.22, 0.36, 0.16], 0.15), [38, 66, 98]);
body("helmet", ellipsoid([0, 0.78, 0.02], [0.14, 0.15, 0.16]), [235, 235, 235]);
body("visor", ellipsoid([0, 0.76, 0.1], [0.12, 0.07, 0.1]), [25, 25, 30]);

const out = glb(parts, wingTexture());
writeFileSync(OUT, out);
console.log(`${OUT}: ${(out.length / 1024).toFixed(0)} KB`);
