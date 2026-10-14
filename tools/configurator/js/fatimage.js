"use strict";

/*
 * Builds a FAT16 disk image in the browser: the voice pack (.ubpack) is such an image, mounted
 * by the firmware through a file on the SD card (1024-byte sectors like the stock packs). Files
 * are given as {path: "voices/vario_lift_1000.qoa", bytes}; folders are created as needed. Names
 * longer than 8.3 get VFAT long-name entries (the stock packs have them too); the short names
 * behind them are generated. Also runs under Node for the self-test
 * (tools/configurator/tests/fatimage_test.js).
 */

const FAT_SECTOR = 1024;
const FAT_ROOT_ENTRIES = 512;
const FAT_NAME_MAX = 64;

/* Splits "a/b/c.qoa" into folders and the file name; validates the characters. */
function splitPath(path) {
  const parts = path.split("/").filter(Boolean);
  const name = parts.pop();
  for (const p of parts.concat([name])) {
    const dot = p.lastIndexOf(".");
    const base = dot > 0 ? p.slice(0, dot) : p;
    const ext = dot > 0 ? p.slice(dot + 1) : "";
    if (!base || base.length > FAT_NAME_MAX || ext.length > 3 || !/^[A-Za-z0-9_\-]+$/.test(base) || (ext && !/^[A-Za-z0-9]+$/.test(ext))) {
      throw new Error(`not a usable file name: ${p}`);
    }
  }
  return { dirs: parts, name };
}

const isShort = (name) => {
  const dot = name.lastIndexOf(".");
  const base = dot > 0 ? name.slice(0, dot) : name;
  const ext = dot > 0 ? name.slice(dot + 1) : "";
  return base.length <= 8 && ext.length <= 3 && name.split(".").length <= 2;
};

/* The 11-byte 8.3 name of a short name ("TAKEOFF QOA"); "." and ".." as they are. */
function shortName(name) {
  if (name === "." || name === "..") return name.padEnd(11, " ");
  const dot = name.lastIndexOf(".");
  const base = (dot > 0 ? name.slice(0, dot) : name).toUpperCase();
  const ext = (dot > 0 ? name.slice(dot + 1) : "").toUpperCase();
  return base.padEnd(8, " ") + ext.padEnd(3, " ");
}

/* A generated short name for a long one, unique within `used`: "VARIO_~1.QOA". */
function makeShortName(name, used) {
  const dot = name.lastIndexOf(".");
  const base = (dot > 0 ? name.slice(0, dot) : name).toUpperCase().replace(/[^A-Z0-9_]/g, "_");
  const ext = (dot > 0 ? name.slice(dot + 1) : "").toUpperCase().slice(0, 3);
  for (let n = 1; n < 1000000; n++) {
    const tail = `~${n}`;
    const sn = (base.slice(0, 8 - tail.length) + tail).padEnd(8, " ") + ext.padEnd(3, " ");
    if (!used.has(sn)) { used.add(sn); return sn; }
  }
  throw new Error("no short name left");
}

function lfnChecksum(sn) {
  let sum = 0;
  for (let i = 0; i < 11; i++) sum = (((sum & 1) << 7) + (sum >> 1) + sn.charCodeAt(i)) & 0xff;
  return sum;
}

/* The directory entries of a file: the long-name entries (if needed), then the short entry. */
function entryCount(name) {
  return isShort(name) ? 1 : 1 + Math.ceil(name.length / 13);
}

function writeEntries(view, image, off, name, sn, attr, cluster, size, date) {
  if (!isShort(name)) {
    const n = Math.ceil(name.length / 13);
    const sum = lfnChecksum(sn);
    const chars = Array.from(name, (c) => c.charCodeAt(0));
    chars.push(0);
    while (chars.length < n * 13) chars.push(0xffff);
    for (let seq = n; seq >= 1; seq--) {
      const slice = chars.slice((seq - 1) * 13, seq * 13);
      view.setUint8(off, seq | (seq === n ? 0x40 : 0));
      const positions = [1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30];
      for (let i = 0; i < 13; i++) view.setUint16(off + positions[i], slice[i], true);
      view.setUint8(off + 11, 0x0f);
      view.setUint8(off + 12, 0);
      view.setUint8(off + 13, sum);
      view.setUint16(off + 26, 0, true);
      off += 32;
    }
  }
  for (let i = 0; i < 11; i++) view.setUint8(off + i, sn.charCodeAt(i));
  view.setUint8(off + 11, attr);
  const d = date;
  const fatTime = (d.getHours() << 11) | (d.getMinutes() << 5) | (d.getSeconds() >> 1);
  const fatDate = ((d.getFullYear() - 1980) << 9) | ((d.getMonth() + 1) << 5) | d.getDate();
  view.setUint16(off + 14, fatTime, true);
  view.setUint16(off + 16, fatDate, true);
  view.setUint16(off + 18, fatDate, true);
  view.setUint16(off + 22, fatTime, true);
  view.setUint16(off + 24, fatDate, true);
  view.setUint16(off + 26, cluster, true);
  view.setUint32(off + 28, size, true);
  return off + 32;
}

/*
 * files: [{path, bytes}], label: up to 11 characters. Returns the image as a Uint8Array.
 * The cluster size is chosen so the cluster count stays in the FAT16 range (4085 .. 65524).
 */
function buildFat16Image(files, label = "VOICEPACK") {
  const date = new Date();
  const root = { name: "", dirs: new Map(), files: [] };
  for (const f of files) {
    const { dirs, name } = splitPath(f.path);
    let d = root;
    for (const part of dirs) {
      const key = part.toUpperCase();
      if (!d.dirs.has(key)) d.dirs.set(key, { name: part, dirs: new Map(), files: [] });
      d = d.dirs.get(key);
    }
    if (d.files.some((x) => x.name.toUpperCase() === name.toUpperCase())) throw new Error(`duplicate file: ${f.path}`);
    d.files.push({ name, bytes: f.bytes });
  }
  /* directory entries per folder (with the long-name entries), data size, cluster size */
  const dirEntries = (d) => {
    let n = d === root ? 0 : 2;
    for (const s of d.dirs.values()) n += entryCount(s.name);
    for (const f of d.files) n += entryCount(f.name);
    return n;
  };
  let dataBytes = 0, dirBytes = 0;
  const walk = (d, fn) => { fn(d); for (const s of d.dirs.values()) walk(s, fn); };
  walk(root, (d) => {
    for (const f of d.files) dataBytes += f.bytes.length;
    if (d !== root) dirBytes += dirEntries(d) * 32;
  });
  if (dirEntries(root) + 1 > FAT_ROOT_ENTRIES) throw new Error("too many entries in the root folder");
  let clusterSectors = 1;
  const need = (cs) => Math.ceil((dataBytes + dirBytes) / (cs * FAT_SECTOR)) + files.length + 64;
  while (need(clusterSectors) > 65000 && clusterSectors < 64) clusterSectors *= 2;
  const clusterBytes = clusterSectors * FAT_SECTOR;
  const clusterCount = Math.max(4100, need(clusterSectors));
  const fatSectors = Math.ceil(((clusterCount + 2) * 2) / FAT_SECTOR);
  const rootSectors = Math.ceil((FAT_ROOT_ENTRIES * 32) / FAT_SECTOR);
  const dataStart = 1 + 2 * fatSectors + rootSectors; /* sector */
  const totalSectors = dataStart + clusterCount * clusterSectors;
  const image = new Uint8Array(totalSectors * FAT_SECTOR);
  const view = new DataView(image.buffer);

  /* boot sector */
  image.set([0xeb, 0x3c, 0x90], 0);
  image.set(new TextEncoder().encode("MSWIN4.1"), 3);
  view.setUint16(11, FAT_SECTOR, true);
  view.setUint8(13, clusterSectors);
  view.setUint16(14, 1, true); /* reserved sectors */
  view.setUint8(16, 2); /* FATs */
  view.setUint16(17, FAT_ROOT_ENTRIES, true);
  view.setUint16(19, totalSectors < 65536 ? totalSectors : 0, true);
  view.setUint8(21, 0xf8);
  view.setUint16(22, fatSectors, true);
  view.setUint16(24, 32, true);
  view.setUint16(26, 2, true);
  view.setUint32(28, 0, true);
  view.setUint32(32, totalSectors < 65536 ? 0 : totalSectors, true);
  view.setUint8(36, 0x80);
  view.setUint8(38, 0x29);
  view.setUint32(39, (Date.now() / 1000) >>> 0, true);
  image.set(new TextEncoder().encode(label.toUpperCase().padEnd(11, " ").slice(0, 11)), 43);
  image.set(new TextEncoder().encode("FAT16   "), 54);
  view.setUint16(510, 0xaa55, true);

  /* the FAT and the cluster allocation */
  const fat = new Uint16Array(clusterCount + 2);
  fat[0] = 0xfff8;
  fat[1] = 0xffff;
  let nextCluster = 2;
  const allocate = (bytes) => {
    const n = Math.max(1, Math.ceil(bytes / clusterBytes));
    if (nextCluster + n > clusterCount + 2) throw new Error("image full");
    const first = nextCluster;
    for (let i = 0; i < n; i++) fat[nextCluster + i] = i + 1 < n ? nextCluster + i + 1 : 0xffff;
    nextCluster += n;
    return first;
  };
  const clusterOffset = (c) => (dataStart + (c - 2) * clusterSectors) * FAT_SECTOR;

  /* a folder: its entries written at `off` (its allocated clusters are contiguous) */
  const place = (d, off, self, parent) => {
    const used = new Set();
    const snOf = (name) => {
      if (isShort(name)) { const sn = shortName(name); used.add(sn); return sn; }
      return makeShortName(name, used);
    };
    if (d !== root) {
      off = writeEntries(view, image, off, ".", shortName("."), 0x10, self, 0, date);
      off = writeEntries(view, image, off, "..", shortName(".."), 0x10, parent, 0, date);
    } else if (label) {
      off = writeEntries(view, image, off, label.slice(0, 8), shortName(label.slice(0, 8)), 0x08, 0, 0, date);
    }
    for (const s of d.dirs.values()) {
      const c = allocate(dirEntries(s) * 32);
      off = writeEntries(view, image, off, s.name, snOf(s.name), 0x10, c, 0, date);
      place(s, clusterOffset(c), c, self);
    }
    for (const f of d.files) {
      let c = 0;
      if (f.bytes.length) {
        c = allocate(f.bytes.length);
        image.set(f.bytes, clusterOffset(c));
      }
      off = writeEntries(view, image, off, f.name, snOf(f.name), 0x20, c, f.bytes.length, date);
    }
  };
  place(root, (1 + 2 * fatSectors) * FAT_SECTOR, 0, 0);
  const fatBytes = new Uint8Array(fat.buffer, 0, Math.min(fat.byteLength, fatSectors * FAT_SECTOR));
  image.set(fatBytes, FAT_SECTOR);
  image.set(fatBytes, FAT_SECTOR * (1 + fatSectors));
  return image;
}

/* Reads a file back out of an image built here (the self-test): bytes or null. */
function readFat16File(image, path) {
  const view = new DataView(image.buffer, image.byteOffset);
  const sector = view.getUint16(11, true), cs = view.getUint8(13), reserved = view.getUint16(14, true);
  const fats = view.getUint8(16), rootEntries = view.getUint16(17, true), fatSectors = view.getUint16(22, true);
  const rootOff = (reserved + fats * fatSectors) * sector;
  const dataStart = rootOff + rootEntries * 32;
  const clusterOffset = (c) => dataStart + (c - 2) * cs * sector;
  const fatAt = (c) => view.getUint16(reserved * sector + c * 2, true);
  const { dirs, name } = splitPath(path);
  const readChain = (first, size) => {
    const parts = [];
    let c = first, got = 0;
    while (c >= 2 && c < 0xfff8 && (size === 0 || got < size)) {
      const n = size === 0 ? cs * sector : Math.min(cs * sector, size - got);
      parts.push(image.subarray(clusterOffset(c), clusterOffset(c) + n));
      got += n;
      c = fatAt(c);
    }
    const out = new Uint8Array(got);
    let pos = 0;
    for (const p of parts) { out.set(p, pos); pos += p.length; }
    return out;
  };
  /* walks the entries, collecting long-name pieces before each short entry */
  const findIn = (entries, wanted) => {
    const v = new DataView(entries.buffer, entries.byteOffset, entries.byteLength);
    const positions = [1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30];
    let lfn = "";
    for (let e = 0; e + 32 <= entries.length; e += 32) {
      if (entries[e] === 0) return null;
      if (entries[e + 11] === 0x0f) {
        let piece = "";
        for (const p of positions) { const ch = v.getUint16(e + p, true); if (ch === 0 || ch === 0xffff) break; piece += String.fromCharCode(ch); }
        lfn = piece + lfn;
        continue;
      }
      let sn = "";
      for (let k = 0; k < 11; k++) sn += String.fromCharCode(entries[e + k]);
      const matches = !(entries[e + 11] & 0x08) && (lfn ? lfn.toUpperCase() === wanted.toUpperCase() : sn === shortName(wanted));
      lfn = "";
      if (matches) return { attr: entries[e + 11], cluster: v.getUint16(e + 26, true), size: v.getUint32(e + 28, true) };
    }
    return null;
  };
  let entries = image.subarray(rootOff, rootOff + rootEntries * 32);
  for (const d of dirs) {
    const e = findIn(entries, d);
    if (!e || !(e.attr & 0x10)) return null;
    entries = readChain(e.cluster, 0);
  }
  const e = findIn(entries, name);
  if (!e) return null;
  return e.size ? readChain(e.cluster, e.size) : new Uint8Array(0);
}

if (typeof module !== "undefined") {
  module.exports = { buildFat16Image, readFat16File, FAT_SECTOR };
}
