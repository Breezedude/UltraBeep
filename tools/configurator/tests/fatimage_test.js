"use strict";

/* Self-test of js/fatimage.js: build an image with folders, read every file back, check the geometry. */
const path = require("node:path");
const { buildFat16Image, readFat16File, FAT_SECTOR } = require(path.join(__dirname, "..", "js", "fatimage.js"));

let failed = 0;
const check = (ok, what) => { console.log(`${ok ? "ok  " : "FAIL"} ${what}`); if (!ok) failed++; };

const files = [];
const content = (i, len) => { const b = new Uint8Array(len); for (let k = 0; k < len; k++) b[k] = (i * 31 + k) & 255; return b; };
for (let i = 0; i < 300; i++) files.push({ path: `al/al_${i * 10}.qoa`, bytes: content(i, 1000 + i * 37) });
for (let i = 0; i < 100; i++) files.push({ path: `voices/clip_${i}.qoa`, bytes: content(500 + i, 3000 + i * 11) });
files.push({ path: "version.txt", bytes: new TextEncoder().encode("2026-10-09\n") });
files.push({ path: "nav/n.qoa", bytes: content(7, 12345) });
files.push({ path: "time/min_59.qoa", bytes: content(9, 0) });
/* long names as the stock packs have them (VFAT entries) */
for (let i = 0; i < 40; i++) files.push({ path: `voices/vario_lift_${100 + i * 50}.qoa`, bytes: content(900 + i, 2000 + i) });
files.push({ path: "voices/profile_paragliding.qoa", bytes: content(11, 7777) });
files.push({ path: "time/duration_10.qoa", bytes: content(12, 4321) });
files.push({ path: "voices/bluetooth_disconnected.qoa", bytes: content(13, 999) });

const t0 = Date.now();
const img = buildFat16Image(files, "UBVOICES");
check(img.length % FAT_SECTOR === 0 && img[510] === 0x55 && img[511] === 0xaa, `image ${(img.length / 1024 / 1024).toFixed(1)} MB built in ${Date.now() - t0} ms`);
const view = new DataView(img.buffer);
const clusters = (view.getUint16(19, true) || view.getUint32(32, true)) / view.getUint8(13);
check(clusters > 4085 && clusters < 65525, `FAT16 cluster count ${Math.floor(clusters)}`);
let allOk = true;
for (const f of files) {
  const back = readFat16File(img, f.path);
  const same = back && back.length === f.bytes.length && back.every((v, k) => v === f.bytes[k]);
  if (!same) { allOk = false; console.log("  mismatch", f.path, back && back.length); }
}
check(allOk, `all ${files.length} files read back identical`);
check(readFat16File(img, "voices/missing.qoa") === null, "missing file is null");
check(new TextDecoder().decode(readFat16File(img, "VERSION.TXT")) === "2026-10-09\n", "names are case-insensitive");
let threw = false;
try { buildFat16Image([{ path: "voices/bad name.qoa", bytes: new Uint8Array(1) }]); } catch (e) { threw = true; }
check(threw, "a name with a space is refused");
/* the short names behind long ones are unique: two long names with the same first characters */
const twin = buildFat16Image([{ path: "voices/vario_lift_100.qoa", bytes: content(1, 100) }, { path: "voices/vario_lift_150.qoa", bytes: content(2, 100) }]);
const a = readFat16File(twin, "voices/vario_lift_100.qoa"), b = readFat16File(twin, "voices/vario_lift_150.qoa");
check(a && b && a[0] === content(1, 100)[0] && b[0] === content(2, 100)[0], "two long names with the same prefix");
console.log(failed ? `${failed} FAILED` : "all passed");
process.exit(failed ? 1 : 0);
