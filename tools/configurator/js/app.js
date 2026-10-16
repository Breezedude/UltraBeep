"use strict";

const EXT_HEADER =
  "# UltraBeep: settings the stock tools do not know (profiles 4-10, volumes, ...).\r\n" +
  "# Format: docs/config-ext-format.md. BipLink and the stock configurator ignore this file.\r\n";

const state = {
  stock: null, /* IniDoc of config.ultrabip */
  ext: null, /* IniDoc of config_ext.ultrabip */
  source: null, /* {kind: "dir"|"new"|"ble", ...} */
  view: "profile",
  profile: 1,
  mode: "dev", /* view mode: basic, expert or dev (header selector) */
  navEdit: false, /* the profile list in its edit mode: reorder by drag, delete */
  voiceLineFor: null, /* a profile number: Custom voicepack opens with that profile's voice line in the editor */
  ifFound: { text: "", saved: "" }, /* the owner's note for a finder (ultrabeep/if_found.txt) */
  clipMap: new Map(), /* profile number now -> number its custom voice line has on the device (reorder, delete) */
  clipDeleted: new Set(), /* device numbers whose profile was deleted: their voice line goes at the next save */
};

const $ = (id) => document.getElementById(id);

function el(tag, attrs = {}, ...children) {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (k === "class") e.className = v;
    else if (k.startsWith("on")) e.addEventListener(k.slice(2), v);
    else if (v !== undefined && v !== null && v !== false) e.setAttribute(k, v === true ? "" : v);
  }
  for (const c of children.flat(Infinity)) {
    if (c !== null && c !== undefined) e.append(c instanceof Node ? c : document.createTextNode(String(c)));
  }
  return e;
}

/* ---------------------------------------------------------------- model */

const section = (n) => `profile${n}`;
const docFor = (n) => (n <= STOCK_PROFILES ? state.stock : state.ext);
const profileExists = (n) => docFor(n).hasSection(section(n));
const isTrue = (v) => v === "true" || v === "1" || v === "TRUE" || v === "True";

/* View modes: a field or view with `level` (expert, dev) shows from that mode on; the rest everywhere. */
const MODE_KEY = "cfg.mode";
const LEVELS = ["basic", "expert", "dev"];
const VIEW_LEVELS = { status: "dev", console: "dev", files: "dev" };
const levelOk = (level) => LEVELS.indexOf(level || "basic") <= LEVELS.indexOf(state.mode);
const shown = (fields) => fields.filter((f) => levelOk(f.level));

function getP(n, key, ext) {
  return (ext ? state.ext : docFor(n)).get(section(n), key);
}

function setP(n, key, value, ext) {
  (ext ? state.ext : docFor(n)).set(section(n), key, value);
  refreshStatus();
}

/* ---- announcements: trigger lists (schema.js TRIGGERS, BLOCKS) ---- */

const itemsKey = (trig) => `announce_${trig.id}_items_string`;

/* The list while announce_<trigger>_items_string is missing: from the stock keys BipLink writes
 * (voicetap_<item>_bool, voiceauto_<item>_bool, flightsummary_*) or the trigger's default. */
function legacyItems(n, trig) {
  const lg = trig.legacy;
  if (lg.kind === "summary") {
    const map = { duration: "duration", maxaltitude: "altitude", maxspeed: "speed", maxclimb: "climbrate" };
    return trig.pool.filter((id) => map[id] && isTrue(getP(n, `flightsummary_${map[id]}_bool`) ?? "true"))
      .map((id) => ({ id, ble: BLOCK_BY_ID[id].ble }));
  }
  if (lg.kind === "matrix") {
    return STOCK_MATRIX_ITEMS.filter((id) => isTrue(getP(n, `${lg.prefix}${id}_bool`)))
      .map((id) => ({ id, ble: BLOCK_BY_ID[id].ble }));
  }
  return lg.items.map((id) => ({ id, ble: BLOCK_BY_ID[id].ble }));
}

function parseItems(text) {
  return text.split(",").map((s) => s.trim()).filter(Boolean).map((s) => {
    const [id, ...opts] = s.split(":");
    return { id, ble: opts.includes("ble") };
  }).filter((it) => BLOCK_BY_ID[it.id]);
}

function readItems(n, trig) {
  const v = getP(n, itemsKey(trig), true);
  return v === undefined ? legacyItems(n, trig) : parseItems(v);
}

const itemsText = (items) => items.map((it) => (it.ble ? `${it.id}:ble` : it.id)).join(",");

function writeItems(n, trig, items) {
  setP(n, itemsKey(trig), itemsText(items), true);
}

/* Enabled state of a trigger: the stock key, or an extension key whose default may come from a stock key. */
function triggerEnabled(n, trig) {
  const e = trig.enable;
  const v = getP(n, e.key, !!e.ext || n > STOCK_PROFILES);
  if (v !== undefined) return isTrue(v);
  const from = e.defFrom ? getP(n, e.defFrom) : undefined;
  return isTrue(from === undefined ? e.def : from);
}

function addProfile(n) {
  const ext = state.ext;
  for (const key of state.stock.keys(section(1))) {
    ext.set(section(n), key, state.stock.get(section(1), key));
  }
  for (const key of ext.keys(section(n))) {
    if (key.startsWith("identity_")) ext.set(section(n), key, "");
  }
  ext.set(section(n), "identity_profilename_string", "paragliding");
  ext.set(section(n), "enabled_bool", "true");
  state.profile = n;
  state.view = "profile";
  render();
}

/* Keys of a profile that live in config_ext.ultrabip even for profiles 1-3 (the firmware's ext_profile_key). */
const isExtProfileKey = (key) => key.startsWith("announce_") || key.startsWith("identity_customname") ||
  key.startsWith("identity_voicetext") || key.startsWith("identity_voiceclip");

/* A profile's settings as two maps, the stock keys and the extension keys, wherever they are stored. */
function snapshotProfile(n) {
  const stock = new Map(), ext = new Map();
  if (n <= STOCK_PROFILES) {
    for (const k of state.stock.keys(section(n))) stock.set(k, state.stock.get(section(n), k));
    for (const k of state.ext.keys(section(n))) ext.set(k, state.ext.get(section(n), k));
  } else {
    for (const k of state.ext.keys(section(n))) (isExtProfileKey(k) ? ext : stock).set(k, state.ext.get(section(n), k));
  }
  return { stock, ext };
}

/* Writes a snapshot as profile n: the stock keys into config.ultrabip for 1-3 and into config_ext.ultrabip
 * from 4 on, the extension keys always into config_ext.ultrabip; keys the snapshot lacks are removed. */
function storeProfile(n, snap) {
  const put = (doc, map) => {
    for (const k of doc.keys(section(n))) if (!map.has(k)) doc.remove(section(n), k);
    for (const [k, v] of map) doc.set(section(n), k, v);
  };
  if (n <= STOCK_PROFILES) {
    put(state.stock, snap.stock);
    put(state.ext, snap.ext);
  } else {
    put(state.ext, new Map([...snap.stock, ...snap.ext]));
  }
}

/*
 * Renumbers the profiles: `order` lists the current numbers in their new order (a deleted one left out),
 * positions 1..N take them, the extension sections beyond are removed. The selected profile and
 * system_profilelast_int follow. The custom voice lines on the device belong to the numbers, so the moves
 * are noted (clipMap, clipDeleted) and applied at the next save.
 */
function reorderProfiles(order) {
  const before = [];
  for (let n = 1; n <= PROFILE_MAX; n++) if (profileExists(n)) before.push(n);
  const snaps = order.map((n) => [n, snapshotProfile(n)]);
  const last = parseInt(state.ext.get("global", "system_profilelast_int") || "1", 10) || 1;
  const oldMap = state.clipMap;
  const newMap = new Map();
  let newLast = 1, newSelected = 1;
  snaps.forEach(([from, snap], i) => {
    const to = i + 1;
    storeProfile(to, snap);
    newMap.set(to, oldMap.has(from) ? oldMap.get(from) : from);
    if (from === last) newLast = to;
    if (from === state.profile) newSelected = to;
  });
  for (let n = snaps.length + 1; n <= PROFILE_MAX; n++) if (n > STOCK_PROFILES) state.ext.removeSection(section(n));
  const kept = new Set(newMap.values());
  for (const n of before) {
    const dev = oldMap.has(n) ? oldMap.get(n) : n;
    if (!kept.has(dev)) state.clipDeleted.add(dev);
  }
  state.clipMap = newMap;
  state.ext.set("global", "system_profilelast_int", String(newLast));
  state.profile = newSelected;
  refreshStatus();
}

/*
 * After a reorder or a delete: the custom voice lines (voices/custom/profile_custom_<n>.qoa) are renamed on
 * the device (Bluetooth) or on the card (folder), the ones of deleted profiles removed. Runs at the save;
 * returns a note for the message, "" when there was nothing to do.
 */
async function applyClipRenames(src) {
  const moves = [...state.clipMap].filter(([cur, dev]) => cur !== dev);
  const current = new Set(state.clipMap.values());
  const gone = [...state.clipDeleted].filter((dev) => !current.has(dev));
  state.clipMap = new Map();
  state.clipDeleted = new Set();
  if (!moves.length && !gone.length) return "";
  const name = (n) => `profile_custom_${n}.qoa`;
  let renamed = 0, removed = 0;
  try {
    if (src.kind === "ble") {
      const client = src.client;
      const have = new Map();
      for (const [, dev] of moves) {
        try { have.set(dev, await client.getTrack("voices/custom", name(dev))); } catch (e) { /* no voice line there */ }
      }
      for (const dev of [...have.keys(), ...gone]) {
        try { await client.deleteFile(`voices/custom/${name(dev)}`); if (!have.has(dev)) removed++; } catch (e) { /* none */ }
      }
      for (const [cur, dev] of moves) {
        if (have.has(dev)) { await client.putBytes(`voices/custom/${name(cur)}`, have.get(dev)); renamed++; }
      }
    } else if (src.kind === "dir") {
      let dir = null;
      try { dir = await (await src.dir.getDirectoryHandle("voices")).getDirectoryHandle("custom"); } catch (e) { dir = null; }
      if (dir) {
        const have = new Map();
        for (const [, dev] of moves) {
          try {
            have.set(dev, new Uint8Array(await (await (await dir.getFileHandle(name(dev))).getFile()).arrayBuffer()));
          } catch (e) { /* no voice line there */ }
        }
        for (const dev of [...have.keys(), ...gone]) {
          try { await dir.removeEntry(name(dev)); if (!have.has(dev)) removed++; } catch (e) { /* none */ }
        }
        for (const [cur, dev] of moves) {
          if (have.has(dev)) {
            const w = await (await dir.getFileHandle(name(cur), { create: true })).createWritable();
            await w.write(have.get(dev));
            await w.close();
            renamed++;
          }
        }
      }
    }
  } catch (e) {
    return ` Voice lines: ${e.message}`;
  }
  return renamed || removed ? ` Voice lines renumbered: ${renamed} moved, ${removed} removed.` : "";
}

/* ---------------------------------------------------------------- inputs */

function input(field, value, onChange) {
  const def = field.def !== undefined ? field.def : "";
  if (field.type === "bool") {
    const box = el("input", { type: "checkbox" });
    box.checked = value === undefined ? isTrue(def) : isTrue(value);
    box.addEventListener("change", () => onChange(box.checked ? "true" : "false"));
    return box;
  }
  if (field.type === "select") {
    const sel = el("select", {}, field.options.map(([v, t]) => el("option", { value: v }, t)));
    const raw = value === undefined ? def : value;
    const current = field.aliases && field.aliases[raw] !== undefined ? field.aliases[raw] : raw; /* a value the device reads as another */
    if (current !== "" && !field.options.some(([v]) => v === current)) {
      sel.append(el("option", { value: current }, current));
    }
    sel.value = current;
    sel.addEventListener("change", () => onChange(sel.value));
    return sel;
  }
  const inp = el("input", {
    type: field.type === "int" ? "number" : "text",
    min: field.min,
    max: field.max,
    maxlength: field.maxlength,
    placeholder: field.placeholder ?? def,
  });
  inp.value = value === undefined ? "" : value;
  inp.addEventListener("change", () => {
    let v = inp.value.trim();
    if (field.type === "int") {
      if (v === "") v = def;
      let n = parseInt(v, 10);
      if (Number.isNaN(n)) n = parseInt(def || "0", 10);
      if (field.min !== undefined) n = Math.max(field.min, n);
      if (field.max !== undefined) n = Math.min(field.max, n);
      v = String(n);
      inp.value = v;
    }
    onChange(v);
  });
  return inp;
}

/* Nearest piezo step for a percent value, rounded like the device (firmware sound_piezo_step). */
function piezoStep(v) {
  const n = parseInt(v, 10);
  return Number.isNaN(n) ? 0 : Math.min(100, Math.floor((Math.max(0, n) + 10) / 20) * 20);
}

/* Tick marks for the piezo sliders (one datalist for the page). */
function piezoStepList() {
  if (!$("piezo-steps")) {
    document.body.append(el("datalist", { id: "piezo-steps" }, PIEZO_STEPS.map(([v, t]) => el("option", { value: v, label: t }))));
  }
  return "piezo-steps";
}

/*
 * Slider 0-100 % with its value; test(value) adds a play button (device connected).
 * steps (piezo): detents at the step values, the step name instead of the percent.
 */
function slider(value, def, onChange, test, steps = null) {
  let current = value === undefined || value === "" ? def : value;
  if (steps) current = String(piezoStep(current));
  const range = el("input", { type: "range", min: 0, max: 100, step: steps ? 20 : 1, "aria-label": "volume",
    list: steps ? piezoStepList() : null });
  range.value = current;
  const text = (v) => (steps ? steps.find(([s]) => s === parseInt(v, 10))[1] : `${v} %`);
  const out = el("output", { class: steps ? "steps" : "" }, text(current));
  range.addEventListener("input", () => { out.textContent = text(range.value); });
  range.addEventListener("change", () => onChange(range.value));
  const box = el("div", { class: "slider" }, range, out);
  if (test) {
    box.append(el("button", { class: "play", title: "Play on the device", "aria-label": "Play on the device",
      disabled: !deviceClient(), onclick: () => test(parseInt(range.value, 10)) }, "▶"));
  }
  return box;
}

/* The connected device, or null (tests and live values need Bluetooth). */
function deviceClient() {
  const src = state.source;
  return src && src.kind === "ble" && src.client.connected ? src.client : null;
}

/* Runs a device test command; shows the device's answer if it refuses (e.g. in flight). */
async function deviceTest(text) {
  const c = deviceClient();
  if (!c) return false;
  try {
    await c.test(text);
    return true;
  } catch (e) {
    setMessage(e.message.includes("not switched on")
      ? "The device must be switched on (after the profile selection) for tests."
      : e.message.includes("in flight") ? "No tests in flight." : e.message, true);
    return false;
  }
}

let uid = 0;

/* Small info circle with a popup text (hover or click; Escape or a click elsewhere closes it). */
function info(text, about = "") {
  const btn = el("button", { type: "button", class: "info", "aria-label": about ? `Help: ${about}` : "Help",
    "aria-expanded": "false" }, "i");
  btn.addEventListener("mouseenter", () => showPop(btn, text, false));
  btn.addEventListener("mouseleave", () => { if (!pop.pinned) hidePop(); });
  btn.addEventListener("focus", () => showPop(btn, text, false));
  btn.addEventListener("blur", () => { if (!pop.pinned) hidePop(); });
  btn.addEventListener("click", (e) => {
    e.preventDefault();
    e.stopPropagation();
    if (pop.pinned && pop.anchor === btn) hidePop();
    else showPop(btn, text, true);
  });
  return btn;
}

const pop = { el: null, anchor: null, pinned: false };

function showPop(anchor, text, pinned) {
  if (!pop.el) {
    pop.el = el("div", { class: "popover", role: "tooltip", id: "popover" });
    document.body.append(pop.el);
  }
  if (pop.anchor && pop.anchor !== anchor) pop.anchor.setAttribute("aria-expanded", "false");
  pop.anchor = anchor;
  pop.pinned = pinned;
  pop.el.textContent = text;
  pop.el.hidden = false;
  anchor.setAttribute("aria-expanded", "true");
  anchor.setAttribute("aria-describedby", "popover");
  const r = anchor.getBoundingClientRect();
  const w = Math.min(340, window.innerWidth - 24);
  pop.el.style.width = `${w}px`;
  const left = Math.max(12, Math.min(window.innerWidth - w - 12, r.left + r.width / 2 - 24));
  pop.el.style.left = `${left + window.scrollX}px`;
  const below = r.bottom + 8;
  const h = pop.el.offsetHeight;
  const top = below + h > window.innerHeight - 8 && r.top - h - 8 > 8 ? r.top - h - 8 : below;
  pop.el.style.top = `${top + window.scrollY}px`;
  pop.el.dataset.above = String(top < r.top);
}

function hidePop() {
  if (!pop.el) return;
  pop.el.hidden = true;
  if (pop.anchor) {
    pop.anchor.setAttribute("aria-expanded", "false");
    pop.anchor.removeAttribute("aria-describedby");
  }
  pop.anchor = null;
  pop.pinned = false;
}

document.addEventListener("click", (e) => { if (pop.el && !pop.el.contains(e.target)) hidePop(); });
document.addEventListener("keydown", (e) => { if (e.key === "Escape") hidePop(); });
window.addEventListener("scroll", () => { if (!pop.pinned) hidePop(); }, true);

/* Label (with stock badge and info circle), control, unit. */
function row(field, value, onChange) {
  const id = `f${++uid}`;
  const control = input(field, value, onChange);
  control.id = id;
  return el("div", { class: "row" + (field.stock ? " stock" : "") },
    el("div", { class: "label" }, el("label", { for: id }, field.label),
      field.stock ? el("span", { class: "badge", title: "Only the stock firmware uses this" }, "stock") : null,
      field.help ? info(field.help, field.label) : null),
    el("div", { class: "control" }, control, field.unit ? el("span", { class: "unit" }, field.unit) : null));
}

/* A row whose control is built elsewhere (sliders, buttons). */
function customRow(label, control, help = null, extra = null) {
  return el("div", { class: "row" },
    el("div", { class: "label" }, el("span", {}, label), help ? info(help, label) : null),
    el("div", { class: "control" }, control, extra));
}

/* ---------------------------------------------------------------- views */

const ICONS = {
  profiles: "M12 12a4 4 0 1 0 0-8 4 4 0 0 0 0 8zM5 21a7 7 0 0 1 14 0",
  system: "M4 7h9M17 7h3M4 17h3M11 17h9M15 9a2 2 0 1 0 0-4 2 2 0 0 0 0 4zM9 19a2 2 0 1 0 0-4 2 2 0 0 0 0 4z",
  volume: "M4 9v6h4l5 4V5L8 9H4zM16.5 9.5a3.5 3.5 0 0 1 0 5M19 7a7 7 0 0 1 0 10",
  recording: "M6 19a2 2 0 1 0 0-4 2 2 0 0 0 0 4zM18 9a2 2 0 1 0 0-4 2 2 0 0 0 0 4zM7.5 15.5C11 12 10 9.5 16 7.5",
  status: "M3 12h4l3-7 4 14 3-7h4",
  console: "M4 5h16v14H4zM7.5 9.5 10 12l-2.5 2.5M12 15h4.5",
  tracks: "M12 4v11M7.5 10.5 12 15l4.5-4.5M5 20h14",
  files: "M7 3h7l5 5v13H7zM14 3v5h5",
  firmware: "M12 16V5M8 9l4-4 4 4M4 15v4h16v-4",
  voice: "M12 15a3 3 0 0 0 3-3V6a3 3 0 0 0-6 0v6a3 3 0 0 0 3 3zM7 11a5 5 0 0 0 10 0M12 16v4M9 20h6",
  chevron: "M9 6l6 6-6 6",
  pencil: "M4 20h4L18.5 9.5a2.1 2.1 0 0 0 0-3L17.5 5.5a2.1 2.1 0 0 0-3 0L4 16z M13 7l4 4",
  trash: "M4 7h16M9 7V4h6v3M6 7l1 13h10l1-13M10 11v6M14 11v6",
};

function icon(name) {
  const svgEl = document.createElementNS("http://www.w3.org/2000/svg", "svg");
  svgEl.setAttribute("viewBox", "0 0 24 24");
  svgEl.setAttribute("class", `icon icon-${name}`);
  svgEl.setAttribute("aria-hidden", "true");
  const path = document.createElementNS("http://www.w3.org/2000/svg", "path");
  path.setAttribute("d", ICONS[name]);
  svgEl.append(path);
  return svgEl;
}

function profilesOpen() {
  try { return localStorage.getItem("cfg.profilesOpen") !== "0"; } catch (e) { return true; }
}

function renderNav() {
  const nav = $("nav");
  nav.replaceChildren();
  const open = profilesOpen();
  const edit = state.navEdit;
  const existing = [];
  for (let n = 1; n <= PROFILE_MAX; n++) if (profileExists(n)) existing.push(n);
  const list = el("div", { class: "subtree" + (edit ? " editing" : ""), hidden: !open });
  const label = (n) => {
    const type = getP(n, CUSTOM_NAME_KEY, true) || getP(n, "identity_profilename_string") || "";
    const wing = getP(n, "identity_wingname_string") || "";
    return [type, wing].filter(Boolean).join(" · ") || "-";
  };
  const finishOrder = () => {
    const order = [...list.children].map((r) => parseInt(r.dataset.n, 10)).filter((n) => !Number.isNaN(n));
    reorderProfiles(order);
    render();
  };
  for (const n of existing) {
    const enabled = isTrue(getP(n, "enabled_bool"));
    const item = el("button", {
      class: "navitem sub" + (state.view === "profile" && state.profile === n ? " active" : "") + (enabled ? "" : " disabled"),
      "aria-current": state.view === "profile" && state.profile === n ? "page" : null,
      onclick: () => { state.view = "profile"; state.profile = n; render(); },
    }, el("span", { class: "navtext" }, el("b", {}, `Profile ${n}`), el("small", {}, label(n), enabled ? "" : " (off)")));
    if (!edit) {
      list.append(item);
      continue;
    }
    /* edit mode: a grip to drag the row (arrow keys move it too), a trash can from profile 4 on */
    const row = el("div", { class: "navrow", "data-n": String(n) });
    const grip = el("button", { type: "button", class: "grip", title: "Drag to reorder", "aria-label": `Move profile ${n}` }, gripIcon());
    grip.addEventListener("pointerdown", (e) => {
      if (e.button !== 0) return;
      e.preventDefault();
      dragRow(list, row, e.pointerId, finishOrder);
    });
    grip.addEventListener("keydown", (e) => {
      const d = e.key === "ArrowUp" ? -1 : e.key === "ArrowDown" ? 1 : 0;
      const i = existing.indexOf(n);
      if (!d || i + d < 0 || i + d >= existing.length) return;
      e.preventDefault();
      const order = existing.slice();
      [order[i], order[i + d]] = [order[i + d], order[i]];
      reorderProfiles(order);
      render();
      const h = document.querySelector(`.navrow[data-n="${i + d + 1}"] .grip`);
      if (h) h.focus();
    });
    row.append(grip, item);
    if (n > STOCK_PROFILES) {
      row.append(el("button", { type: "button", class: "trash", title: "Delete this profile", "aria-label": `Delete profile ${n}`,
        onclick: () => {
          if (!window.confirm(`Delete profile ${n} (${label(n)})? The profiles after it move up one number; ` +
            "its custom voice line on the device is removed at the next save.")) return;
          reorderProfiles(existing.filter((x) => x !== n));
          render();
        } }, icon("trash")));
    } else {
      row.append(el("span", { class: "trash-space", title: "Profiles 1 to 3 always exist" }));
    }
    list.append(row);
  }
  if (!edit && existing.length < PROFILE_MAX) {
    const next = existing.length + 1;
    list.append(el("button", { class: "navitem sub add", onclick: () => addProfile(next) }, `+ Add profile ${next}`));
  }
  const toggle = el("button", { class: "navitem group" + (state.view === "profile" ? " within" : ""), "aria-expanded": String(open),
    title: open ? "Fold the profiles" : "Show the profiles",
    onclick: () => {
      try { localStorage.setItem("cfg.profilesOpen", open ? "0" : "1"); } catch (e) { /* only for this page */ }
      renderNav();
    } }, icon("profiles"), el("span", { class: "navtext" }, "Profiles"), el("span", { class: "count" }, String(existing.length)),
  icon("chevron"));
  const pencil = el("button", { type: "button", class: "navedit" + (edit ? " on" : ""), "aria-pressed": String(edit),
    title: edit ? "Done" : "Edit the list: reorder, delete", "aria-label": "Edit the profile list",
    onclick: () => {
      state.navEdit = !state.navEdit;
      if (state.navEdit) { try { localStorage.setItem("cfg.profilesOpen", "1"); } catch (e) { /* only for this page */ } }
      renderNav();
    } }, icon("pencil"));
  nav.append(el("div", { class: "navgroup" }, toggle, pencil), list);
  const views = [["system", "System"], ["volume", "Volume"], ["recording", "Recording"], ["voice", "Voice"]];
  /* the device views: connected, or a new configuration that shows every view */
  if (state.source && (state.source.kind === "ble" || state.source.kind === "new")) {
    views.push(["status", "Status and tests"], ["console", "Console"], ["tracks", "Tracks"], ["firmware", "Firmware"]);
  }
  views.push(["files", "Files"]);
  const navButton = (view, label, sub) => el("button", {
    class: "navitem" + (sub ? " sub" : "") + (state.view === view ? " active" : ""),
    "aria-current": state.view === view ? "page" : null,
    onclick: () => { state.view = view; render(); },
  }, sub ? null : icon(view), el("span", { class: "navtext" }, label));
  for (const [view, label] of views.filter(([v]) => levelOk(VIEW_LEVELS[v]))) {
    nav.append(navButton(view, label));
    /* Voice: the packs; below it the pages that make voices and packs */
    if (view === "voice") {
      nav.append(el("div", { class: "subtree" }, navButton("speaker", "Custom speaker", true),
        navButton("voicepack", "Custom voicepack", true)));
    }
  }
}

/* A card; title may be {title, help} for an info circle next to it. */
function fieldset(title, ...content) {
  const t = typeof title === "string" ? { title } : title;
  return el("fieldset", {}, el("legend", {}, t.title, t.help ? info(t.help, t.title) : null), ...content);
}

function groupFieldset(n, g) {
  if (g.title === "Identity") return identityFieldset(n, g); /* voice.js: custom name and voice line */
  return fieldset({ title: g.title, help: g.help },
    shown(g.fields).map((f) => row(f, getP(n, f.key), (v) => {
      setP(n, f.key, v);
      if (f.key.startsWith("identity_")) renderNav();
      if (f.key.startsWith("vario_")) state.onToneChange && state.onToneChange();
    })));
}

/* Called before another view is shown: polling timers, previews and device tests end. */
function leaveView() {
  if (state.cleanup) {
    const c = state.cleanup;
    state.cleanup = null;
    c();
  }
  state.onToneChange = null;
}

function renderProfile(n) {
  const main = $("main");
  const head = el("div", { class: "head" }, el("h2", {}, `Profile ${n}`),
    el("p", { class: "sub" }, n <= STOCK_PROFILES
      ? "Stored in config.ultrabip - BipLink and the stock configurator can edit it too."
      : "Stored in config_ext.ultrabip - only this configurator and UltraBeep know it."),
    fieldset("Profile", row({ label: "Enabled", type: "bool", help: "Disabled profiles are skipped in the profile " +
      "selection after switching on." }, getP(n, "enabled_bool"), (v) => { setP(n, "enabled_bool", v); renderNav(); })));
  const before = PROFILE_GROUPS_BEFORE_TONES.map((g) => groupFieldset(n, g));
  const after = PROFILE_GROUPS_AFTER_TONES.map((g) => groupFieldset(n, g));
  main.replaceChildren(head, ...before, toneSection(n), announceSection(n), ...after);
}

/* ---- announcements: one card per trigger with its settings, a pool of blocks and the ordered list ---- */

const GRIP_PATH = "M9 6h.01M15 6h.01M9 12h.01M15 12h.01M9 18h.01M15 18h.01";

function gripIcon() {
  const svgEl = document.createElementNS("http://www.w3.org/2000/svg", "svg");
  svgEl.setAttribute("viewBox", "0 0 24 24");
  svgEl.setAttribute("class", "icon icon-grip");
  svgEl.setAttribute("aria-hidden", "true");
  const path = document.createElementNS("http://www.w3.org/2000/svg", "path");
  path.setAttribute("d", GRIP_PATH);
  svgEl.append(path);
  return svgEl;
}

/*
 * Drags `row` within `list`: the row follows the pointer and is moved in the DOM when it passes
 * the middle of a neighbour. done() runs on release. The listeners sit on the document: moving the
 * row in the DOM releases a pointer capture, so the handle cannot keep the events itself.
 */
function dragRow(list, row, pointerId, done) {
  row.classList.add("dragging");
  list.classList.add("sorting");
  const handle = document;
  const move = (ev) => {
    if (ev.pointerId !== pointerId) return;
    const y = ev.clientY;
    let before = null;
    for (const r of list.children) {
      if (r === row) continue;
      const b = r.getBoundingClientRect();
      if (y < b.top + b.height / 2) { before = r; break; }
    }
    if (before ? row.nextElementSibling !== before : list.lastElementChild !== row) list.insertBefore(row, before);
    row.style.transform = "";
    const b = row.getBoundingClientRect();
    row.style.transform = `translateY(${y - (b.top + b.height / 2)}px)`;
  };
  const up = (ev) => {
    if (ev.pointerId !== pointerId) return;
    handle.removeEventListener("pointermove", move);
    handle.removeEventListener("pointerup", up);
    handle.removeEventListener("pointercancel", up);
    row.style.transform = "";
    row.classList.remove("dragging");
    list.classList.remove("sorting");
    done();
  };
  handle.addEventListener("pointermove", move);
  handle.addEventListener("pointerup", up);
  handle.addEventListener("pointercancel", up);
}

function triggerCard(n, trig) {
  const enabled = triggerEnabled(n, trig);
  const card = el("section", { class: "trigger" + (enabled ? "" : " off") });
  const items = readItems(n, trig);
  const rerender = (focus) => {
    const next = triggerCard(n, trig);
    card.replaceWith(next);
    if (focus) {
      const f = next.querySelector(focus);
      if (f) f.focus();
    }
  };
  const commit = (focus) => { writeItems(n, trig, items); rerender(focus); };
  const settingValue = (f) => {
    const v = getP(n, f.key, !!f.ext);
    if (v !== undefined || !f.defFrom) return v;
    return getP(n, f.defFrom); /* shown as the value, so the user sees what applies */
  };
  /* The ordered list is a table: grip | block | mute with BLE | remove; the tbody is what gets sorted. */
  const list = el("tbody", {});
  const table = el("table", { class: "blocks", "aria-label": `${trig.label}: blocks in order` },
    el("thead", {}, el("tr", {}, el("th", { class: "col-grip" }, el("span", { class: "sr-only" }, "Order")),
      el("th", { class: "col-name" }, "Spoken, in this order"),
      el("th", { class: "col-ble" }, "Mute with BLE",
        info("Not spoken while a phone is connected over Bluetooth: the flight app shows the value.", "Mute with BLE")),
      el("th", { class: "col-remove" }, el("span", { class: "sr-only" }, "Remove")))),
    list);
  const itemRow = (it, i) => {
    const block = BLOCK_BY_ID[it.id];
    const grip = el("button", { type: "button", class: "grip", title: "Drag to reorder (arrow keys: move)",
      "aria-label": `Move ${block.label}` }, gripIcon());
    const box = el("input", { type: "checkbox", "aria-label": `Mute ${block.label} with BLE` });
    box.checked = it.ble;
    box.addEventListener("change", () => { it.ble = box.checked; writeItems(n, trig, items); });
    const li = el("tr", { class: "block" },
      el("td", { class: "col-grip" }, grip),
      el("td", { class: "col-name" }, block.label, block.help ? info(block.help, block.label) : null),
      el("td", { class: "col-ble" }, box),
      el("td", { class: "col-remove" }, el("button", { type: "button", class: "remove", title: "Remove",
        "aria-label": `Remove ${block.label}`, onclick: () => { items.splice(i, 1); commit(); } }, "×")));
    grip.addEventListener("pointerdown", (e) => {
      if (e.button !== 0) return;
      e.preventDefault();
      dragRow(list, li, e.pointerId, () => {
        const order = [...list.children].map((r) => r.dataset.id);
        items.sort((a, b) => order.indexOf(a.id) - order.indexOf(b.id));
        commit(`.block[data-id="${it.id}"] .grip`);
      });
    });
    grip.addEventListener("keydown", (e) => {
      const d = e.key === "ArrowUp" ? -1 : e.key === "ArrowDown" ? 1 : 0;
      if (!d || i + d < 0 || i + d >= items.length) return;
      e.preventDefault();
      [items[i], items[i + d]] = [items[i + d], items[i]];
      commit(`.block[data-id="${it.id}"] .grip`);
    });
    li.dataset.id = it.id;
    return li;
  };
  list.append(...items.map(itemRow));
  table.hidden = items.length === 0;
  const empty = el("p", { class: "hint blocks-empty" }, items.length ? "" : "No blocks: this trigger stays silent. Add blocks from the row above.");

  /* The pool: blocks not placed yet; a click appends, a drag places. */
  const placed = new Set(items.map((it) => it.id));
  const chips = trig.pool.filter((id) => !placed.has(id)).map((id) => {
    const block = BLOCK_BY_ID[id];
    const chip = el("button", { type: "button", class: "chip", title: block.help || "" }, "+ ", block.label);
    let dragged = false;
    chip.addEventListener("click", () => {
      if (dragged) { dragged = false; return; }
      items.push({ id, ble: block.ble });
      commit();
    });
    chip.addEventListener("pointerdown", (e) => {
      if (e.button !== 0) return;
      const startY = e.clientY, startX = e.clientX, pid = e.pointerId;
      const arm = (ev) => {
        if (ev.pointerId !== pid) return;
        if (Math.abs(ev.clientY - startY) + Math.abs(ev.clientX - startX) < 6) return;
        disarm(ev);
        dragged = true;
        const it = { id, ble: block.ble };
        items.push(it);
        const li = itemRow(it, items.length - 1);
        list.append(li);
        table.hidden = false;
        empty.textContent = "";
        dragRow(list, li, pid, () => {
          const order = [...list.children].map((r) => r.dataset.id);
          items.sort((a, b) => order.indexOf(a.id) - order.indexOf(b.id));
          commit();
        });
      };
      const disarm = (ev) => {
        if (ev.pointerId !== pid) return;
        document.removeEventListener("pointermove", arm);
        document.removeEventListener("pointerup", disarm);
        document.removeEventListener("pointercancel", disarm);
      };
      document.addEventListener("pointermove", arm);
      document.addEventListener("pointerup", disarm);
      document.addEventListener("pointercancel", disarm);
    });
    return chip;
  });
  const pool = el("div", { class: "pool" }, el("span", { class: "pool-label" }, chips.length ? "Add" : "All blocks placed"), chips);

  const sw = input({ type: "bool" }, String(enabled), (v) => {
    setP(n, trig.enable.key, v, !!trig.enable.ext);
    rerender(".trigger-head input");
  });
  sw.setAttribute("aria-label", `${trig.label} enabled`);
  const settings = trig.settings.map((f) => row(f, settingValue(f), (v) => setP(n, f.key, v, !!f.ext)));
  /* play button: the list as it is now (unsaved) with demo values on the connected device */
  const play = deviceClient() ? el("button", { type: "button", class: "play trigger-play", disabled: !items.length,
    title: "Play a demo of this list on the device (fixed example values)", "aria-label": `Play a demo of ${trig.label}`,
    onclick: () => deviceTest(`ANNOUNCE ${trig.id} ${itemsText(items)}`) }, "▶") : null;
  card.append(
    el("div", { class: "trigger-head" }, sw, el("h3", {}, trig.label), trig.help ? info(trig.help, trig.label) : null,
      el("span", { class: "when" }, trig.when), play),
    el("div", { class: "trigger-body" },
      settings.length ? el("div", { class: "trigger-settings" }, settings) : null,
      pool, table, empty));
  return card;
}

/* The pause between automatic announcements (firmware 0.54), per profile. */
const ANNOUNCE_GAP_FIELD = { key: "announce_mingap_int", label: "Pause between announcements", type: "select", def: "15",
  options: [["0", "none"], ["10", "10 s"], ["15", "15 s"], ["20", "20 s"], ["30", "30 s"], ["45", "45 s"], ["60", "1 min"],
    ["90", "90 s"], ["120", "2 min"]],
  help: "Automatic announcements keep at least this pause after the last one ended, a double tap's included. " +
    "A time, distance or altitude announcement that falls due meanwhile comes right after the pause; a thermal, " +
    "wind, glide or manoeuvre announcement within it is left out. A double tap also restarts the time, distance " +
    "and altitude triggers, so none of them follows right after it." };

function announceSection(n) {
  const k = ANNOUNCE_GAP_FIELD.key;
  return fieldset({ title: "Announcements", help: "Each trigger speaks its own list of blocks, in the order " +
      "shown. Add blocks from the pool of a trigger, drag the handle to reorder, and set per block whether it " +
      "stays silent while a phone is connected (the flight app shows the value then). Blocks without data " +
      "(no fix, no wind estimate yet) are left out." },
  row(ANNOUNCE_GAP_FIELD, getP(n, k, true) ?? ANNOUNCE_GAP_FIELD.def, (v) => setP(n, k, v, true)),
  TRIGGERS.map((t) => triggerCard(n, t)));
}

/* Latest value wins: while a command is on its way, only the newest next one is kept. */
function latestSender(send) {
  let busy = false, pending = null;
  return async function push(value) {
    pending = value;
    if (busy) return;
    busy = true;
    while (pending !== null) {
      const v = pending;
      pending = null;
      await send(v);
    }
    busy = false;
  };
}

/* Tone curve: graphical editor, JSON export and import, preview in the browser and tests on the device. */
function toneSection(n) {
  let curve = readTones(n);
  let mode = "off"; /* off | browser | sim | live (sim and live run on the device) */
  let climb = 1.5;  /* test climb rate: follows the pointer over the chart */
  let liveTimer = null;
  let volume = 40;
  const preview = new TonePreview();
  const sendTones = latestSender((c) => deviceTest(tonesCommand(c)));
  const sendSim = latestSender((v) => deviceTest(`SIM ${Math.round(v * 100)}`));
  const sendVolume = latestSender((v) => deviceTest(`VARIO ${v}`));
  const readout = el("span", { class: "tone-readout" }, "");
  const status = el("p", { class: "hint" }, "");
  const alarmBox = el("input", { type: "checkbox" });

  const editor = createToneEditor({
    curve,
    onEdit: (c, final) => {
      curve = c;
      if (final) writeTones(n, c);
    },
    onClimb: (v) => {
      climb = v;
      if (mode === "sim") sendSim(v);
    },
    onSelect: (text) => {
      readout.textContent = text || "Drag the points; arrow keys move the selected one (Shift: larger steps).";
    },
  });
  const setCurve = (c, write) => {
    curve = { ...c, ...toneFlags(n) }; /* the weak-lift, sweep and integration switches ride along */
    if (write) writeTones(n, c);
    alarmBox.checked = c.alarm;
    editor.setCurve(c);
    if (mode === "sim" || mode === "live") sendTones(c);
  };
  state.onToneChange = () => setCurve(readTones(n), false);
  alarmBox.checked = curve.alarm;
  alarmBox.addEventListener("change", () => setCurve({ ...cloneCurve(curve), alarm: alarmBox.checked }, true));

  /* JSON export and import */
  const exportText = el("textarea", { class: "json", readonly: true, rows: 14, spellcheck: "false",
    "aria-label": "tone curve as JSON" });
  const exportPanel = el("div", { class: "json-panel", hidden: true }, exportText,
    el("div", { class: "buttons" }, el("button", { onclick: async () => {
      try {
        await navigator.clipboard.writeText(exportText.value);
        setMessage("Tone curve copied.");
      } catch (e) {
        exportText.select();
        setMessage("Select the text and copy it (the browser did not allow it).", true);
      }
    } }, "Copy")));
  const importText = el("textarea", { class: "json", rows: 14, spellcheck: "false",
    placeholder: "Paste a tone curve (JSON, as from \"Export\") here.", "aria-label": "tone curve to import" });
  const importError = el("p", { class: "error" }, "");
  const importPanel = el("div", { class: "json-panel", hidden: true }, importText, importError,
    el("div", { class: "buttons" }, el("button", { class: "primary", onclick: () => {
      try {
        setCurve(tonesFromJson(importText.value), true);
        importError.textContent = "";
        importPanel.hidden = true;
        setMessage("Tone curve imported - save to keep it.");
      } catch (e) {
        importError.textContent = e.message;
      }
    } }, "Apply")));
  const toggle = (panel, other, fill) => () => {
    other.hidden = true;
    panel.hidden = !panel.hidden;
    if (!panel.hidden && fill) fill();
  };
  const toolbar = el("div", { class: "tone-toolbar" },
    el("label", { class: "check" }, alarmBox, " Sink alarm"),
    el("span", { class: "legend" }, el("i", { class: "key freq" }), "pitch", el("i", { class: "key rate" }), "beeps/s",
      el("i", { class: "key duty" }), "tone length"),
    el("span", { class: "spacer" }),
    el("button", { onclick: toggle(exportPanel, importPanel, () => { exportText.value = tonesToJson(curve); }) }, "Export"),
    el("button", { onclick: toggle(importPanel, exportPanel) }, "Import"),
    el("button", { onclick: () => setCurve(stockCurve(), true), title: "The stock curve and thresholds" }, "Stock curve"));

  /* tests */
  const volBox = slider(String(volume), "40", (v) => {
    volume = parseInt(v, 10);
    if (mode === "sim" || mode === "live") sendVolume(volume);
  }, null, PIEZO_STEPS);
  const modes = el("div", { class: "segmented", role: "group", "aria-label": "Test" });
  const stop = async () => {
    const was = mode;
    mode = "off";
    preview.stop();
    if (liveTimer) clearInterval(liveTimer);
    liveTimer = null;
    if (was === "sim" || was === "live") await deviceTest("TEST off");
    status.textContent = "";
    editor.setMode("edit");
    drawModes();
  };
  const start = async (m) => {
    await stop();
    if (m === "browser") {
      mode = "browser";
      preview.start(() => curve, () => climb);
      status.textContent = "Move the pointer over the chart: the bar sets the climb rate. The device sounds different (piezo).";
      editor.setMode("hover", climb);
    } else {
      if (!deviceClient()) return;
      const ok = (await deviceTest(tonesCommand(curve))) && (await deviceTest(`VARIO ${volume}`)) &&
        (await deviceTest(m === "sim" ? `SIM ${Math.round(climb * 100)}` : "SIM off"));
      if (!ok) {
        await deviceTest("TEST off");
        return;
      }
      mode = m;
      if (m === "sim") {
        status.textContent = "The device plays this curve (not saved yet). Move the pointer over the chart to set the climb rate.";
        editor.setMode("hover", climb);
      } else {
        status.textContent = "The device plays this curve with its own sensors: lift and lower it. The bar shows the measured climb rate.";
        editor.setMode("live", null);
        liveTimer = setInterval(async () => {
          try {
            const v = await deviceClient().live();
            editor.setMarker(parseFloat(v.vz));
          } catch (e) { /* next poll */ }
        }, 400);
      }
    }
    drawModes();
  };
  const drawModes = () => {
    const dev = !!deviceClient();
    const b = (m, label, needsDevice) => el("button", {
      class: mode === m ? "on" : "",
      "aria-pressed": mode === m ? "true" : "false",
      disabled: needsDevice && !dev,
      title: needsDevice && !dev ? "Connect via Bluetooth first" : "",
      onclick: () => (m === "off" ? stop() : mode === m ? stop() : start(m)),
    }, label);
    modes.replaceChildren(b("off", "Edit", false), b("browser", "Test: browser", false), b("sim", "Test: device", true),
      b("live", "Device sensors", true));
  };
  drawModes();
  state.cleanup = async () => {
    editor.destroy();
    await stop();
  };

  return fieldset("Vario tone curve", toolbar, exportPanel, importPanel, editor.el,
    el("div", { class: "tone-footer" }, readout),
    el("div", { class: "tone-test" }, modes, el("span", { class: "spacer" }),
      el("span", { class: "label" }, "Volume"), volBox),
    status);
}

/* ---- status and tests (Bluetooth) ---- */

const STATE_NAMES = {
  ground: "on the ground", flight: "in flight", "profile_selection": "profile selection",
  "power-off_melody": "switching off", track_sync: "after the flight (track download)",
};

function renderStatus() {
  const client = deviceClient();
  const cells = {};
  const cell = (id, label, unit = "") => {
    cells[id] = el("b", {}, "–");
    return el("div", { class: "live-cell" + (id === "vz" ? " big" : "") }, el("span", { class: "muted" }, label),
      el("div", {}, cells[id], unit ? el("small", {}, ` ${unit}`) : ""));
  };
  /* hardware strip (baro, imu, gpsmod, sd, pack in the LIVE line) */
  const hw = el("div", { class: "hwstrip", hidden: true });
  const HW = [["baro", "Barometer"], ["imu", "IMU"], ["gpsmod", "GNSS module"], ["sd", "SD card"], ["pack", "Voice pack"]];
  const showHw = (v) => {
    if (v.baro === undefined) return;
    hw.hidden = false;
    hw.replaceChildren(el("span", { class: "muted" }, "Hardware:"), ...HW.map(([k, label]) => {
      const val = v[k];
      const ok = val === "1", off = val === "-" || val === undefined;
      let text = ok ? "ok" : off ? "off" : k === "gpsmod" ? "SILENT" : k === "sd" ? "not mounted" : k === "pack" ? "missing" : "NO ANSWER";
      if (k === "gpsmod" && !ok && !off && parseInt(v.gpsr, 10) > 0) text += ` (${v.gpsr} restarts)`;
      return el("span", { class: "badge" + (ok ? " ok" : off ? "" : " warn"),
        title: k === "gpsmod" ? "The module sent a sentence within the last 5 s" : "Answered at the last check" }, `${label}: ${text}`);
    }));
  };
  const grid = el("div", { class: "live" },
    cell("st", "Device"), cell("vz", "Climb rate", "m/s"), cell("alt", "Altitude", "m"), cell("p", "Pressure", "hPa"),
    cell("t", "Temperature", "°C"), cell("acc", "Acceleration", "m/s²"), cell("filter", "Filter"),
    cell("gps", "GPS"), cell("gs", "Ground speed", "km/h"), cell("pos", "Position"), cell("vbat", "Battery", "V"),
    cell("rec", "Recording"), cell("taps", "Double taps"));
  const fmt = (v, d) => (v === undefined || Number.isNaN(parseFloat(v)) ? "–" : parseFloat(v).toFixed(d));
  const show = (v) => {
    showHw(v);
    cells.st.textContent = STATE_NAMES[v.st] || (v.st || "–").replace(/_/g, " ");
    const vz = parseFloat(v.vz);
    cells.vz.textContent = Number.isNaN(vz) ? "–" : `${vz >= 0 ? "+" : ""}${vz.toFixed(2)}`;
    cells.vz.className = vz >= 0.1 ? "lift" : vz <= -1 ? "sink" : "";
    cells.alt.textContent = fmt(v.alt, 1);
    cells.p.textContent = fmt(parseFloat(v.p) / 100, 2);
    cells.t.textContent = fmt(v.t, 1);
    cells.acc.textContent = fmt(v.acc, 2);
    cells.filter.textContent = `${v.fused === "1" ? "baro + IMU" : "baro only"}${v.rest === "1" ? ", at rest" : ""}`;
    cells.gps.textContent = v.fix === "1" ? `3D fix, ${v.sats} sat., HDOP ${v.hdop}` : `no fix (${v.sats || 0} sat.)`;
    cells.gs.textContent = v.fix === "1" ? fmt(v.gs, 1) : "–";
    cells.pos.textContent = v.fix === "1" ? `${fmt(v.lat, 5)}, ${fmt(v.lon, 5)}` : "–";
    cells.vbat.textContent = parseInt(v.vbat, 10) ? (parseInt(v.vbat, 10) / 1000).toFixed(2) : "–";
    cells.rec.textContent = v.rec === "1" ? "IGC running" : "off (starts with the takeoff)";
    cells.taps.textContent = v.taps === undefined ? "–"
      : `${v.taps}${parseFloat(v.tapg) > 0 ? ` (last tap ${parseFloat(v.tapg).toFixed(1)} g)` : ""}`;
    if (v.tsens !== undefined && document.activeElement !== tapSel) tapSel.value = v.tsens;
    if (v.pump && document.activeElement !== pumpRange) {
      const i = PUMP_FREQS.indexOf(parseInt(v.pump, 10));
      if (i >= 0) {
        pumpRange.value = i;
        showPump(PUMP_FREQS[i]);
      }
    }
    if (v.piezo && document.activeElement !== driveSel) driveSel.value = v.piezo;
    if (v.quiet && document.activeElement !== pulseRange) {
      pulseRange.value = v.quiet;
      showPulse(parseInt(v.quiet, 10));
    }
  };
  let timer = null;
  if (client) {
    const poll = async () => {
      try {
        show(await client.live());
      } catch (e) { /* next poll */ }
    };
    poll();
    timer = setInterval(poll, 1000);
  }

  /* piezo drive (experiment) and test tone */
  const driveSel = input({ type: "select", options: [["classic", "Classic (as stock)"],
    ["pwm", "Pulse width (experimental)"]] }, "classic", async (v) => {
    if (!(await deviceTest(`PIEZO ${v}`))) driveSel.value = v === "pwm" ? "classic" : "pwm";
  });
  driveSel.disabled = !client;
  let freq = 700, ms = 400, vol = 40;
  const freqOut = el("output", {}, "700 Hz");
  const freqRange = el("input", { type: "range", min: 100, max: 4000, step: 10, value: freq, "aria-label": "frequency" });
  freqRange.addEventListener("input", () => { freq = parseInt(freqRange.value, 10); freqOut.textContent = `${freq} Hz`; });
  const msSel = input({ type: "select", options: [["150", "0.15 s"], ["400", "0.4 s"], ["1000", "1 s"], ["2000", "2 s"]] },
    "400", (v) => { ms = parseInt(v, 10); });
  const volPiezo = slider("40", "40", (v) => { vol = parseInt(v, 10); }, null, PIEZO_STEPS);
  /* pulse of the step "very quiet" */
  const pulseOut = el("output", {}, "–");
  const pulseRange = el("input", { type: "range", min: 500, max: 16000, step: 250, "aria-label": "pulse of the step very quiet" });
  const showPulse = (ns) => { pulseOut.textContent = `${(ns / 1000).toFixed(2)} µs`; };
  const sendPulse = latestSender((ns) => deviceTest(`PIEZO pulse ${ns}`));
  pulseRange.disabled = !client;
  pulseRange.addEventListener("input", () => showPulse(parseInt(pulseRange.value, 10)));
  pulseRange.addEventListener("change", async () => {
    await sendPulse(parseInt(pulseRange.value, 10));
    deviceTest(`TONE ${freq} ${ms} 20`);
  });
  /* charge pump frequency of the step "loud" */
  const pumpOut = el("output", {}, "–");
  const pumpRange = el("input", { type: "range", min: 0, max: PUMP_FREQS.length - 1, step: 1,
    "aria-label": "charge pump frequency of the step loud" });
  const showPump = (hz) => { pumpOut.textContent = hz >= 1000 ? `${hz / 1000} kHz` : `${hz} Hz`; };
  const sendPump = latestSender((hz) => deviceTest(`PIEZO pump ${hz}`));
  pumpRange.disabled = !client;
  pumpRange.addEventListener("input", () => showPump(PUMP_FREQS[parseInt(pumpRange.value, 10)]));
  pumpRange.addEventListener("change", async () => {
    await sendPump(PUMP_FREQS[parseInt(pumpRange.value, 10)]);
    deviceTest(`TONE ${freq} ${ms} 80`);
  });
  /* double tap: sensitivity for the test */
  const tapSel = input({ type: "select", options: TAP_SENSITIVITY }, "3", (v) => deviceTest(`TAPSENS ${v}`));
  tapSel.disabled = !client;
  /* speaker */
  let spkVol = 50, clip = "gps_ok";
  const volSpk = slider("50", "50", (v) => { spkVol = parseInt(v, 10); });
  const clipSel = input({ type: "select", options: [["gps_ok", "GPS ok"], ["takeoff", "Takeoff"], ["landing", "Landing"],
    ["battery", "Battery"], ["vol_2", "Level 2"], ["al/al_1230", "1230 m"], ["sp/sp_42", "42 km/h"]] },
  "gps_ok", (v) => { clip = v; });
  /* vario */
  let varioVol = 40, simClimb = 1.5, varioMode = "off";
  const varioStatus = el("p", { class: "hint" }, "");
  const simOut = el("output", {}, "+1.5 m/s");
  const simRange = el("input", { type: "range", min: -5, max: 8, step: 0.1, value: simClimb, "aria-label": "simulated climb" });
  const sendSim = latestSender((v) => deviceTest(`SIM ${Math.round(v * 100)}`));
  simRange.addEventListener("input", () => {
    simClimb = parseFloat(simRange.value);
    simOut.textContent = `${simClimb >= 0 ? "+" : ""}${simClimb.toFixed(1)} m/s`;
    if (varioMode === "sim") sendSim(simClimb);
  });
  const varioVolBox = slider("40", "40", (v) => {
    varioVol = parseInt(v, 10);
    if (varioMode !== "off") deviceTest(`VARIO ${varioVol}`);
  }, null, PIEZO_STEPS);
  const varioButtons = el("div", { class: "buttons" });
  const drawVario = () => {
    const b = (m, label) => el("button", { class: varioMode === m ? "primary" : "", disabled: !client,
      onclick: async () => {
        if (varioMode === m) {
          varioMode = "off";
          await deviceTest("TEST off");
        } else if ((await deviceTest(`VARIO ${varioVol}`)) &&
                   (await deviceTest(m === "sim" ? `SIM ${Math.round(simClimb * 100)}` : "SIM off"))) {
          varioMode = m;
        }
        varioStatus.textContent = varioMode === "off" ? ""
          : "Uses the tone curve of the selected profile on the device (saved values). To try unsaved changes use " +
            "the test buttons in the profile's tone curve.";
        drawVario();
      } }, varioMode === m ? `■ ${label}` : `▶ ${label}`);
    varioButtons.replaceChildren(b("live", "Vario with the device's sensors"), b("sim", "Simulated climb"));
  };
  drawVario();
  state.cleanup = () => {
    if (timer) clearInterval(timer);
    if (varioMode !== "off") deviceTest("TEST off");
  };

  const r = (label, control, extra = null) => customRow(label, control, null, extra);
  $("main").replaceChildren(
    el("div", { class: "head" }, el("h2", {}, "Status and tests"),
      el("p", { class: "hint" }, client
        ? "Live values from the device, once per second. Tests work while the device is switched on and on the ground."
        : "Connect via Bluetooth to see live values and run tests.")),
    fieldset("Live", hw, grid),
    fieldset("Piezo test tone", r("Frequency", freqRange, freqOut), r("Length", msSel), r("Volume", volPiezo),
      el("div", { class: "buttons" }, el("button", { disabled: !client,
        onclick: () => deviceTest(`TONE ${freq} ${ms} ${vol}`) }, "▶ Play")),
      el("p", { class: "hint" }, "The piezo has six volume steps, no continuous volume: only its voltage changes " +
        "the loudness (battery, one or both charge pump legs). The step \"very quiet\" charges it only partly with " +
        "a short pulse; shorter is quieter. Tune it here: the value applies until the device is switched off - " +
        "report the one that fits, it becomes the default."),
      r("Very quiet: pulse", pulseRange, pulseOut),
      el("p", { class: "hint" }, "The step \"loud\" runs both charge pump legs; the frequency sets how much voltage " +
        "they reach (the stock 7 kHz is no louder than \"medium\"). Each change plays the step: report the " +
        "frequency that lies between medium and very loud."),
      r("Loud: pump frequency", pumpRange, pumpOut),
      r("Drive", driveSel),
      el("p", { class: "hint" }, "Experiment: \"Pulse width\" shorts the piezo between the pulses. It is a little " +
        "quieter, uses a little less current and sounds different. Applies to all piezo sounds until the device " +
        "is switched off.")),
    fieldset("Double tap", r("Sensitivity (test)", tapSel),
      el("p", { class: "hint" }, "Knock twice on the device. The live values count the double taps and show how hard " +
        "the last tap was. The sensitivity chosen here applies until the device is switched off; the profile's " +
        "setting (System) applies at the next start.")),
    fieldset("Speaker", r("Announcement", clipSel), r("Volume", volSpk),
      el("div", { class: "buttons" },
        el("button", { disabled: !client, onclick: () => deviceTest(`SAY ${clip} ${spkVol}`) }, "▶ Announcement"),
        el("button", { disabled: !client, onclick: () => deviceTest(`SPK 800 600 ${spkVol}`) }, "▶ Test tone 800 Hz"))),
    fieldset("Vario", r("Volume", varioVolBox), r("Simulated climb", simRange, simOut), varioButtons, varioStatus),
    el("div", { class: "buttons" }, el("button", { disabled: !client, onclick: async () => {
      varioMode = "off";
      drawVario();
      await deviceTest("TEST off");
      setMessage("All tests stopped.");
    } }, "Stop all tests")));
}

/* ---- system ---- */

/* Stock per-profile keys of the device as a whole: profile 1's value, written into every profile. */
function systemValue(key) {
  return getP(1, key);
}

function setSystemValue(key, value) {
  for (let n = 1; n <= PROFILE_MAX; n++) if (profileExists(n)) docFor(n).set(section(n), key, value);
  refreshStatus();
}

function systemDiffers(key) {
  const v = systemValue(key);
  const other = [];
  for (let n = 2; n <= PROFILE_MAX; n++) {
    if (profileExists(n) && getP(n, key) !== undefined && getP(n, key) !== v) other.push(n);
  }
  return other;
}

/* Rows whose field has `requires` are dimmed and disabled while that switch is off; read(field) gives its value. */
function applyRequires(fields, rows, read) {
  const on = (f) => {
    if (!f.requires) return true;
    const dep = fields.find((x) => x.key === f.requires);
    return isTrue(read(dep) ?? dep.def) && on(dep); /* a switched-off dependency switches off its chain */
  };
  for (const f of fields) {
    if (!f.requires || !rows[f.key]) continue;
    rows[f.key].classList.toggle("off", !on(f));
    rows[f.key].querySelector("input, select").disabled = !on(f);
  }
}

/* "differs in profile" badge for a stock key that is written into every profile. */
function profileBadge(r, key) {
  const differs = systemDiffers(key);
  if (differs.length) {
    r.querySelector(".control").append(el("span", { class: "badge warn",
      title: "Saving a change here sets the value in every profile." }, `differs in profile ${differs.join(", ")}`));
  }
}

function renderSystem() {
  leaveView(); /* a running calibration of the previous rendering stops */
  const g = (key) => state.ext.get("global", key);
  /* global: a [global] key of config_ext.ultrabip; otherwise a stock key written into every profile */
  const read = (f) => (f.global ? g(f.key) : systemValue(f.key));
  const write = (f, v) => {
    if (f.global) { state.ext.set("global", f.key, v); refreshStatus(); } else setSystemValue(f.key, v);
  };
  /* time zone: the place is stored globally, the offset in every profile (stock key), the rule globally and
   * the switch dates in the stock file's [global] section (the BipLink app stores the offset in force, e.g.
   * 120 in summer, so the standard offset comes from the table when there is one) */
  const zoneRead = () => {
    const off = dstTableStandardOffset(state.stock.get("global", "global_timechanges_longstring")) ??
      parseInt(systemValue("system_timeoffsetminutes_int") ?? "60", 10);
    const rule = g("system_dstrule_string") || "eu";
    const id = g("system_timezone_string");
    const match = (z) => z.offset === off && z.rule === rule;
    const z = TIME_ZONES.find((x) => x.id === id && match(x)) || TIME_ZONES.find(match);
    return z ? z.id : `custom:${off}:${rule}`;
  };
  const zoneField = (f) => {
    const cur = zoneRead();
    if (!cur.startsWith("custom:")) return f;
    const [, off, rule] = cur.split(":");
    const label = `Custom: ${utcOffsetText(parseInt(off, 10))}, ${DST_RULE_NAMES[rule] || rule} (from the file)`;
    return { ...f, options: [...f.options, [cur, label]] };
  };
  const zoneWrite = (id) => {
    const z = TIME_ZONES.find((x) => x.id === id);
    if (!z) return;
    state.ext.set("global", "system_timezone_string", id);
    state.ext.set("global", "system_dstrule_string", z.rule);
    setSystemValue("system_timeoffsetminutes_int", String(z.offset));
    state.stock.set("global", "global_timechanges_longstring", dstTable(z.rule, z.offset));
  };
  const groups = SYSTEM_GROUPS.filter((grp) => shown(grp.fields).length).map((grp) => {
    const rows = {};
    const card = fieldset(grp.title, shown(grp.fields).map((f) => {
      const r = f.zone ? row(zoneField(f), zoneRead(), (v) => { zoneWrite(v); renderSystem(); })
        : row(f, read(f), (v) => { write(f, v); renderSystem(); });
      rows[f.key] = r;
      if (f.zone) profileBadge(r, "system_timeoffsetminutes_int");
      else if (!f.global) profileBadge(r, f.key);
      if (f.test === "melody" && deviceClient()) {
        r.querySelector(".control").append(el("button", { class: "play", title: "Play on the device", "aria-label": "Play on the device",
          onclick: () => deviceTest(`MELODY ${read(f) || f.def} ${g("volume_buzzerground_int") || 20}`) }, "▶"));
      }
      return r;
    }));
    applyRequires(grp.fields, rows, read);
    return card;
  });
  const sensors = fieldset({ title: "Sensors", help: "Device-wide sensor settings in config_ext.ultrabip; they " +
      "apply at the next power on." },
  shown(SENSOR_FIELDS).map((f) => row(f, g(f.key), (v) => { state.ext.set("global", f.key, v); refreshStatus(); })),
  calibrationCard());
  /* the note for a finder: free text, a file on the card */
  const note = el("textarea", { rows: 8, maxlength: String(IF_FOUND_MAX), spellcheck: "false", "aria-label": "If found",
    placeholder: ifFoundTemplate() });
  note.value = state.ifFound.text;
  note.addEventListener("input", () => { state.ifFound.text = note.value; refreshStatus(); });
  const templateBtn = el("button", { type: "button", onclick: () => {
    if (note.value.trim() && !window.confirm("Replace the text with the template?")) return;
    note.value = ifFoundTemplate();
    state.ifFound.text = note.value;
    refreshStatus();
    note.focus();
  } }, "Insert template");
  const ifFound = fieldset({ title: "If found", help: "A note for whoever finds the device: your name, address and how " +
      "to reach you. Saved as ultrabeep/if_found.txt; the device keeps a copy as IF_FOUND_PLEASE_READ.txt in the top " +
      "folder of the SD card, the first file a finder sees when the device is connected over USB. The copy comes " +
      "back after the original firmware removed it. With no text there is no file. English reaches most people." },
  el("p", { class: "sub" }, "Shown to a finder as IF_FOUND_PLEASE_READ.txt in the top folder of the SD card."),
  note, el("div", { class: "buttons" }, templateBtn));
  $("main").replaceChildren(
    el("div", { class: "head" }, el("h2", {}, "System"),
      el("p", { class: "sub" }, "Settings for the whole device. The stock file keeps some of them per profile; " +
        "a change here is written into every profile.")),
    ...groups, sensors, ifFound);
}

/* ---- accelerometer calibration (six sides, over Bluetooth) ---- */

const G0 = 9.80665;
/*
 * The device has no display: its sides are named by what points up. The sensor axes behind them
 * are unknown until measured, so the wizard asks for the sides one after the other, takes
 * whichever sensor axis is up and learns the mapping (saved as imu_axes_string for later use).
 */
const CAL_FACES = [
  { id: "solar", label: "Solar cell up", hint: "flat on the table, velcro down" },
  { id: "velcro", label: "Velcro up", hint: "flat on the table, upside down" },
  { id: "usb", label: "USB socket up", hint: "button side down - the button must stay free: let it hang over " +
      "the table edge, or lean the device against a box (within 20° of upright)" },
  { id: "button", label: "Button up", hint: "standing on the USB side" },
  { id: "speaker", label: "Speaker up", hint: "standing on the eyelet side" },
  { id: "eyelet", label: "Eyelet up", hint: "standing on the speaker side" },
];
const SENSOR_FACES = ["+X", "−X", "+Y", "−Y", "+Z", "−Z"]; /* index = axis * 2 + (negative ? 1 : 0) */

/* The sensor axis pointing up (within about 25°), or -1 when the device is tilted. */
function sensorFace(m) {
  const n = Math.hypot(...m);
  for (let k = 0; k < 3; k++) {
    if (m[k] / n > 0.9) return k * 2;
    if (m[k] / n < -0.9) return k * 2 + 1;
  }
  return -1;
}

/* Least-squares offset and scale per axis: |(m - o) * s| = g for every captured side. */
function fitCalibration(means) {
  let o = [0, 0, 0], sc = [1, 1, 1];
  for (let it = 0; it < 25; it++) {
    const A = Array.from({ length: 6 }, () => new Array(7).fill(0));
    for (const m of means) {
      const u = m.map((v, k) => (v - o[k]) * sc[k]);
      const nu = Math.hypot(...u);
      const r = nu - G0;
      const J = [0, 1, 2].map((k) => -sc[k] * u[k] / nu).concat([0, 1, 2].map((k) => (m[k] - o[k]) * u[k] / nu));
      for (let i = 0; i < 6; i++) {
        for (let j = 0; j < 6; j++) A[i][j] += J[i] * J[j];
        A[i][6] -= J[i] * r;
      }
    }
    for (let i = 0; i < 6; i++) A[i][i] += 1e-9;
    for (let c = 0; c < 6; c++) {
      let piv = c;
      for (let r2 = c + 1; r2 < 6; r2++) if (Math.abs(A[r2][c]) > Math.abs(A[piv][c])) piv = r2;
      [A[c], A[piv]] = [A[piv], A[c]];
      for (let r2 = 0; r2 < 6; r2++) {
        if (r2 === c) continue;
        const f = A[r2][c] / A[c][c];
        for (let k = c; k < 7; k++) A[r2][k] -= f * A[c][k];
      }
    }
    const dx = A.map((rw, i) => rw[6] / rw[i]);
    o = o.map((v, k) => v + dx[k]);
    sc = sc.map((v, k) => v + dx[k + 3]);
  }
  const res = means.map((m) => Math.hypot(...m.map((v, k) => (v - o[k]) * sc[k])) - G0);
  return { offset: o, scale: sc, rms: Math.sqrt(res.reduce((a, b) => a + b * b, 0) / res.length), residuals: res };
}

/* "solar:+Z,usb:+X,speaker:+Y" when opposite sides gave opposite sensor axes, else null. */
function axesMapping(captured) {
  const pairs = [["solar", "velcro"], ["usb", "button"], ["speaker", "eyelet"]];
  const face = (id) => captured[CAL_FACES.findIndex((f) => f.id === id)].sensor;
  if (!pairs.every(([a, b]) => (face(a) ^ 1) === face(b))) return null;
  return pairs.map(([a]) => `${a}:${SENSOR_FACES[face(a)]}`).join(",");
}

function calibrationCard() {
  const g = (key) => state.ext.get("global", key);
  const stored = () => {
    const o = ["x", "y", "z"].map((a) => parseInt(g(`imu_accoffset_${a}_int`) || "0", 10));
    const sc = ["x", "y", "z"].map((a) => parseInt(g(`imu_accscale_${a}_int`) || "1000000", 10));
    const none = o.every((v) => v === 0) && sc.every((v) => v === 1000000);
    const axes = g("imu_axes_string") ? `; sensor axes ${g("imu_axes_string").replace(/,/g, ", ")}` : "";
    return (none ? "not calibrated"
      : `offset ${o.map((v) => (v / 1000).toFixed(3)).join(" / ")} m/s², scale ${sc.map((v) => (v / 1e4).toFixed(2)).join(" / ")} %`) + axes;
  };
  const status = el("p", { class: "sub" }, `In the file: ${stored()}.`);
  const box = el("div", { class: "cal" });
  const card = el("div", { class: "subcard" },
    el("div", { class: "subcard-head" }, el("b", {}, "Accelerometer calibration"),
      info("Measures the offset and the scale of each accelerometer axis. The wizard asks for the six sides " +
        "one after the other; each is taken after 2 s without movement, in any position within about 20° " +
        "of upright. It also finds out which sensor axis points out of which side. Needs Bluetooth and the " +
        "switched-on device; the result is saved on the device at once.", "Accelerometer calibration")),
    status, box);
  const client = deviceClient();
  if (!client) {
    box.append(el("p", { class: "muted" }, "Connect via Bluetooth (device switched on) to calibrate."));
    return card;
  }
  let timer = null;
  const captured = new Array(6).fill(null); /* {mean, sensor} per side */
  let still = 0, lastSeq = -1, lastSensor = -1, acc = [];
  const step = () => captured.findIndex((c) => !c);
  const tiles = CAL_FACES.map((f, i) => el("button", { class: "face", type: "button",
    title: "Click to take this side again", onclick: () => { captured[i] = null; still = 0; acc = []; draw(); } },
  el("span", { class: "face-label" }, f.label), el("span", { class: "face-hint" }, f.hint),
  el("span", { class: "face-state" }, "–")));
  const live = el("p", { class: "cal-live mono" }, "");
  const note = el("p", { class: "warn-text" }, "");
  const result = el("div", {});
  const startBtn = el("button", { class: "primary", type: "button" }, "Start calibration");
  const resetBtn = el("button", { type: "button" }, "Reset calibration");
  const draw = () => {
    const cur = step();
    tiles.forEach((t, i) => {
      t.classList.toggle("done", !!captured[i]);
      t.classList.toggle("current", timer && i === cur);
      t.querySelector(".face-state").textContent = captured[i] ? `✓ sensor ${SENSOR_FACES[captured[i].sensor]}`
        : timer && i === cur ? (lastSensor >= 0 ? `${Math.min(2, still)}/2 s` : "now") : "–";
    });
    result.replaceChildren();
    if (cur < 0) {
      const fit = fitCalibration(captured.map((c) => c.mean));
      const okRange = fit.offset.every((v) => Math.abs(v) < 1.5) && fit.scale.every((v) => v > 0.9 && v < 1.1);
      const axes = axesMapping(captured);
      result.append(
        el("p", {}, `Offset ${fit.offset.map((v) => v.toFixed(3)).join(" / ")} m/s² · scale ` +
          `${fit.scale.map((v) => (v * 100).toFixed(2)).join(" / ")} % · residual ${(fit.rms * 1000).toFixed(0)} mm/s²`),
        el("p", {}, axes ? `Sensor axes: ${axes.replace(/,/g, ", ")} (the opposite sides are the negative axes).`
          : "The sides do not fit together: opposite sides should give opposite sensor axes. The calibration is " +
            "still valid when all six sides differ, but the axis mapping is not saved - take the doubtful sides again."),
        ...(okRange ? [] : [el("p", { class: "warn-text" }, "Values out of the plausible range - repeat the sides.")]),
        el("button", { class: "primary", type: "button", disabled: !okRange, onclick: async () => {
          const args = [...fit.offset.map((v) => Math.round(v * 1000)), ...fit.scale.map((v) => Math.round(v * 1e6))];
          try {
            await client.calSet(args);
            ["x", "y", "z"].forEach((a, k) => {
              state.ext.set("global", `imu_accoffset_${a}_int`, String(args[k]));
              state.ext.set("global", `imu_accscale_${a}_int`, String(args[3 + k]));
            });
            if (axes) state.ext.set("global", "imu_axes_string", axes);
            stop();
            setMessage("Calibration saved on the device; it applies at once." +
              (axes ? " Save the file for the sensor axes." : ""));
            renderSystem();
          } catch (e) { setMessage(e.message, true); }
        } }, "Save on the device"));
    } else if (timer) {
      result.append(el("p", { class: "sub" }, `${6 - captured.filter((c) => !c).length} of 6 sides. Now: ` +
        `${CAL_FACES[cur].label} (${CAL_FACES[cur].hint}) - then keep it still.`));
    }
  };
  const poll = async () => {
    try {
      const v = await client.cal();
      const m = [parseFloat(v.ax), parseFloat(v.ay), parseFloat(v.az)];
      const seq = parseInt(v.n, 10);
      const sensor = sensorFace(m);
      live.textContent = `|a| ${Math.hypot(...m).toFixed(3)} m/s² · x ${m[0].toFixed(2)} y ${m[1].toFixed(2)} ` +
        `z ${m[2].toFixed(2)} · ${v.still === "1" ? "still" : "moving"}`;
      if (seq === lastSeq) return;
      lastSeq = seq;
      const cur = step();
      if (cur < 0) return;
      const taken = captured.findIndex((c) => c && c.sensor === sensor);
      if (sensor !== lastSensor || v.still !== "1") { still = 0; acc = []; }
      lastSensor = sensor;
      if (sensor < 0) {
        note.textContent = "Tilted: no side points up.";
      } else if (taken >= 0) {
        note.textContent = `This looks like "${CAL_FACES[taken].label}" again (sensor ${SENSOR_FACES[sensor]}) - ` +
          `turn the device so that the ${CAL_FACES[cur].label.toLowerCase().replace(" up", "")} points up.`;
        still = 0;
        acc = [];
      } else {
        note.textContent = "";
        if (v.still === "1") {
          still++;
          acc.push(m);
          if (still >= 2) {
            captured[cur] = { mean: [0, 1, 2].map((k) => acc.reduce((a, x) => a + x[k], 0) / acc.length), sensor };
            still = 0;
            acc = [];
            lastSensor = -1;
          }
        }
      }
      draw();
    } catch (e) {
      live.textContent = e.message;
    }
  };
  const stop = () => {
    if (timer) clearInterval(timer);
    timer = null;
    note.textContent = "";
    startBtn.textContent = "Start calibration";
  };
  startBtn.addEventListener("click", () => {
    if (timer) { stop(); draw(); return; }
    captured.fill(null);
    still = 0;
    acc = [];
    lastSensor = -1;
    timer = setInterval(poll, 500);
    poll();
    startBtn.textContent = "Stop";
    draw();
  });
  resetBtn.addEventListener("click", async () => {
    try {
      await client.calReset();
      ["x", "y", "z"].forEach((a) => {
        state.ext.set("global", `imu_accoffset_${a}_int`, "0");
        state.ext.set("global", `imu_accscale_${a}_int`, "1000000");
      });
      setMessage("Calibration removed on the device.");
      renderSystem();
    } catch (e) { setMessage(e.message, true); }
  });
  const prev = state.cleanup;
  state.cleanup = () => { stop(); if (prev) prev(); };
  box.append(el("div", { class: "faces" }, tiles), live, note, result, el("div", { class: "buttons" }, startBtn, resetBtn));
  draw();
  return card;
}

/* ---- volume ---- */

function renderVolume() {
  const g = (key) => state.ext.get("global", key);
  const set = (key) => (v) => { state.ext.set("global", key, v); refreshStatus(); };
  const piezoTest = (vol) => deviceTest(`TONE 700 400 ${vol}`);  const speakerTest = (clip) => (vol) => deviceTest(`SAY ${clip} ${vol}`);
  const piezoHelp = "The piezo has six steps, no continuous volume: only its voltage changes the loudness. " +
    "\"Very loud\" is only a little louder than \"loud\" and can sound harsh.";
  const speakerHelp = "Perceived loudness: 100 % sounds twice as loud as 50 %. 100 % is the loudest level before " +
    "the voice clips distort.";
  const levelRows = (lv, buzzerLabel, speakerLabel) => [
    customRow(buzzerLabel, slider(g(`volume_buzzer${lv.id}_int`), lv.buzzer, set(`volume_buzzer${lv.id}_int`), piezoTest,
      PIEZO_STEPS), piezoHelp),
    customRow(speakerLabel, slider(g(`volume_speaker${lv.id}_int`), lv.speaker, set(`volume_speaker${lv.id}_int`),
      speakerTest(lv.clip)), speakerHelp),
  ];
  const last = g("system_profilelast_int") || "1";
  $("main").replaceChildren(
    el("div", { class: "head" }, el("h2", {}, "Volume"),
      el("p", { class: "sub" }, "Vario tones come from the piezo, announcements from the speaker. In flight a " +
        "short press cycles low → medium → high → mute (mute silences vario and announcements). " +
        (deviceClient() ? "▶ plays a sample on the device." : "Connect via Bluetooth to try the volumes on the device."))),
    ...FLIGHT_LEVELS.map((lv) => fieldset(`Flight: ${lv.label.toLowerCase()}`,
      ...levelRows(lv, "Vario tones", "Announcements"))),
    fieldset({ title: "On the ground", help: "Automatic before the takeoff and after a landing. No vario tones " +
        "on the ground; these levels are for button feedback, melodies and system messages such as \"GPS ok\"." },
      ...levelRows(GROUND_LEVEL, "Button feedback and melodies", "System messages")),
    fieldset("Remembered level",
      row(FLIGHT_LEVEL_FIELD, g(FLIGHT_LEVEL_FIELD.key), set(FLIGHT_LEVEL_FIELD.key)),
      el("p", { class: "sub" }, `Last selected profile: ${last} (also stored by the device).`)));
}

/* ---- recording ---- */

function renderRecording() {
  const g = (key) => state.ext.get("global", key);
  /* profile: a stock per-profile key (takeoff detection), shown once and written into every profile */
  const read = (f) => (f.profile ? systemValue(f.key) : g(f.key));
  const write = (f, v) => {
    if (f.profile) setSystemValue(f.key, v); else { state.ext.set("global", f.key, v); refreshStatus(); }
  };
  const rows = {};
  const update = () => {
    applyRequires(RECORDING_FIELDS, rows, read);
    const igc = isTrue(g("flight_recordigc_bool") ?? "true"), kml = isTrue(g("flight_recordkml_bool") ?? "true");
    note.textContent = !igc && !kml ? "Nothing is recorded: tick at least one file type." : "";
  };
  const note = el("p", { class: "warn-text" });
  const field = (f) => {
    rows[f.key] = row(f, read(f), (v) => { write(f, v); update(); });
    if (f.profile) profileBadge(rows[f.key], f.key);
    return rows[f.key];
  };
  const files = shown(RECORDING_FIELDS.filter((f) => f.key.startsWith("flight_record")));
  const takeoff = shown(RECORDING_FIELDS.filter((f) => f.profile || f.key.startsWith("flight_takeoff")));
  const landing = shown(RECORDING_FIELDS.filter((f) => f.key.startsWith("flight_landing")));
  const development = shown(RECORDING_FIELDS.filter((f) => f.key === "system_rawlog_bool"));
  const cards = [fieldset("Files", files.map(field), note), fieldset("Takeoff detection", takeoff.map(field)),
    fieldset("Landing detection", landing.map(field))];
  if (development.length) {
    cards.push(fieldset({ title: "Development", help: "Not a track: the raw sensor samples for the log analyzer in " +
      "tools/analyzer, with which the vario filter is tuned." }, development.map(field)));
  }
  $("main").replaceChildren(
    el("div", { class: "head" }, el("h2", {}, "Recording"),
      el("p", { class: "sub" }, "The track starts with the takeoff detection (with the 15 s before it) and ends " +
        "when the device is switched off; a landing does not split it.")),
    ...cards);
  update();
}

/* ---- firmware update over Bluetooth (PUT update, UPDATE) ---- */

/* Version-like strings in the image ("0.26.0"), for a look before sending. */
function imageVersions(bytes) {
  const text = new TextDecoder("latin1").decode(bytes);
  const found = new Set();
  for (const m of text.matchAll(/\x00(\d+\.\d+\.\d+)\x00/g)) found.add(m[1]);
  return [...found];
}

function renderFirmware() {
  const client = deviceClient();
  const info = client ? client.info : {};
  let image = null; /* {name, bytes} */
  let sent = false;
  const fileInput = el("input", { type: "file", accept: ".update" });
  const fileInfo = el("p", { class: "sub" }, "No file chosen.");
  const bar = el("progress", { max: 100, value: 0, hidden: true });
  const status = el("p", { class: "sub" }, "");
  const sendBtn = el("button", { class: "primary", type: "button", disabled: true }, "Send to device");
  const installBtn = el("button", { class: "primary", type: "button", disabled: true }, "Install");
  fileInput.addEventListener("change", async () => {
    const f = fileInput.files[0];
    image = null;
    sent = false;
    installBtn.disabled = true;
    if (!f) { fileInfo.textContent = "No file chosen."; sendBtn.disabled = true; return; }
    const bytes = new Uint8Array(await f.arrayBuffer());
    image = { name: f.name, bytes };
    const versions = imageVersions(bytes);
    fileInfo.textContent = `${f.name}: ${(bytes.length / 1024).toFixed(0)} kB` +
      (versions.length ? `, version ${versions.join(" / ")}` : "") +
      (info.version ? ` (device: ${info.version})` : "");
    sendBtn.disabled = !client;
    status.textContent = "";
  });
  sendBtn.addEventListener("click", async () => {
    if (!image || !deviceClient()) return;
    sendBtn.disabled = true;
    bar.hidden = false;
    bar.value = 0;
    const t0 = Date.now();
    status.textContent = "Sending …";
    try {
      await deviceClient().putBytes("update", image.bytes, (done, total) => {
        bar.value = Math.round(100 * done / total);
        const s = (Date.now() - t0) / 1000;
        status.textContent = `Sending … ${(done / 1024).toFixed(0)} of ${(total / 1024).toFixed(0)} kB` +
          (s > 2 ? `, ${(done / 1024 / s).toFixed(1)} kB/s` : "");
      });
      sent = true;
      status.textContent = `Stored on the SD card as ultrabip_ultrabeep.update (${((Date.now() - t0) / 1000).toFixed(0)} s). ` +
        "Install starts the update: the device restarts, the LED flickers red and green, then it says " +
        "\"update ok\" and switches off.";
      installBtn.disabled = false;
    } catch (e) {
      status.textContent = e.message.includes("in flight") ? "Not in flight." : e.message;
      sendBtn.disabled = false;
    }
    bar.hidden = true;
  });
  installBtn.addEventListener("click", async () => {
    if (!sent || !deviceClient()) return;
    installBtn.disabled = true;
    try {
      const client = deviceClient();
      await client.installUpdate();
      status.textContent = "Installing: the device restarts into the bootloader and disconnects. Wait for the LED to " +
        "finish and the spoken result (about a minute), then switch on and connect again.";
      setMessage("Update started; the device disconnects now.");
      setTimeout(() => { /* the device checks the image first: still connected means it was refused */
        if (client.connected && state.view === "firmware") {
          status.textContent = "The device is still connected: it refused the image (see its log). Send the file again or " +
            "use the USB way.";
          installBtn.disabled = false;
        }
      }, 15000);
    } catch (e) {
      status.textContent = e.message.includes("battery") ? "The battery is too low for an update (below 3.64 V): charge first."
        : e.message.includes("in flight") ? "Not in flight." : e.message;
      installBtn.disabled = false;
    }
  });
  $("main").replaceChildren(
    el("div", { class: "head" }, el("h2", {}, "Firmware"),
      el("p", { class: "sub" }, client ? `Connected device: UltraBeep ${info.version || "?"}.`
        : "Connect via Bluetooth to update the firmware.")),
    fieldset({ title: "Update over Bluetooth", help: "Choose the .update file from the firmware build, send it to " +
        "the SD card (about 100 kB per minute) and install. The device checks the image itself; a damaged file is " +
        "refused. Needs a charged battery (3.64 V or more); never in flight. The USB way (copy the file onto the " +
        "drive and unplug) keeps working." },
    customRow("Update file", fileInput), fileInfo, bar, el("div", { class: "buttons" }, sendBtn, installBtn), status));
}

function renderFiles() {
  const block = (name, doc) => fieldset(`${name}${doc.changed ? " (changed)" : ""}`,
    el("pre", { class: "file" }, doc.text || "(empty)"));
  $("main").replaceChildren(el("div", { class: "head" }, el("h2", {}, "Files")),
    block("config.ultrabip", state.stock), block("config_ext.ultrabip", state.ext));
}

/* Track download over Bluetooth (device on, or the 60 s after switching off a flight). */
function renderTracks() {
  const client = state.source && state.source.client;
  const list = el("div", {}, el("p", { class: "muted" }, "Loading the track list..."));
  const progress = el("p", { class: "hint" }, "");
  $("main").replaceChildren(
    el("div", { class: "head" }, el("h2", {}, "Tracks"),
      el("p", { class: "hint" }, "IGC and KML files on the device. After a flight Bluetooth stays on for 60 s " +
        "after switching off (the LED flashes blue every 4 s); connect within that time to download.")),
    list, progress);
  if (!client || !client.connected) {
    list.replaceChildren(el("p", {}, "Connect via Bluetooth first."));
    return;
  }
  const kb = (n) => `${(n / 1024).toFixed(n < 10240 ? 1 : 0)} KiB`;
  const base = (name) => name.split("/").pop(); /* without the year folder */
  const fetchFile = async (dir, name, type) => {
    const file = base(name);
    try {
      const t0 = Date.now();
      const bytes = await client.getTrack(dir, name, (got, size) => {
        const rate = got / Math.max(1, (Date.now() - t0) / 1000);
        progress.textContent = `${file}: ${kb(got)} of ${kb(size)} (${kb(rate)}/s)`;
      });
      const a = el("a", { href: URL.createObjectURL(new Blob([bytes], { type })), download: file });
      a.click();
      setTimeout(() => URL.revokeObjectURL(a.href), 1000);
      progress.textContent = `${file}: downloaded (${kb(bytes.length)}).`;
    } catch (e) {
      progress.textContent = `${file}: ${e.message}`;
    }
  };
  (async () => {
    try {
      const igc = await client.listTracks("tracks_IGC");
      const kml = await client.listTracks("tracks_KML");
      const kmlNames = new Set(kml.map((f) => f.name));
      if (!igc.length) {
        list.replaceChildren(el("p", {}, "No tracks yet."));
        return;
      }
      list.replaceChildren(el("table", { class: "tracks" },
        el("thead", {}, el("tr", {}, ["Flight", "Size", ""].map((t) => el("th", {}, t)))),
        el("tbody", {}, igc.map((f) => {
          const kmlName = f.name.replace(/\.IGC$/i, ".kml");
          return el("tr", {}, el("td", {}, base(f.name)), el("td", {}, kb(f.size)), el("td", {},
            el("button", { onclick: () => fetchFile("tracks_IGC", f.name, "application/octet-stream") }, "IGC"),
            kmlNames.has(kmlName)
              ? el("button", { onclick: () => fetchFile("tracks_KML", kmlName, "application/vnd.google-earth.kml+xml") }, "KML")
              : ""));
        }))));
    } catch (e) {
      list.replaceChildren(el("p", { class: "error" }, e.message));
    }
  })();
}

/* The device's debug console over Bluetooth: the log so far, then new lines live; commands as on USB. */
function renderConsole() {
  const client = deviceClient();
  const MAX_LINES = 5000;
  let lines = [""]; /* the last one is still open */
  let drawPending = false;
  const out = el("pre", { class: "console" });
  const note = el("p", { class: "hint" }, "");
  const draw = () => {
    if (drawPending) return;
    drawPending = true;
    requestAnimationFrame(() => {
      drawPending = false;
      const atEnd = out.scrollHeight - out.scrollTop - out.clientHeight < 24;
      out.textContent = lines.join("\n");
      if (atEnd) out.scrollTop = out.scrollHeight;
    });
  };
  const add = (text, lineEnds) => {
    let cur = lines.pop() + text;
    while (cur.includes("\x08")) cur = cur.replace(/[^\x08]?\x08/, ""); /* backspace from the USB console's line editing */
    lines.push(cur);
    if (lineEnds) lines.push("");
    if (lines.length > MAX_LINES) lines = lines.slice(-MAX_LINES);
    draw();
  };
  const send = async (cmd) => {
    try {
      await client.consoleSend(cmd);
    } catch (e) {
      note.textContent = e.message;
    }
  };

  const history = [];
  let historyPos = 0;
  const input = el("input", { type: "text", class: "mono console-input", maxlength: 95, placeholder: "Command, e.g. help",
    autocomplete: "off", spellcheck: "false", disabled: !client, "aria-label": "Console command" });
  const submit = () => {
    const cmd = input.value.trim();
    input.value = "";
    if (!cmd) return;
    if (history[history.length - 1] !== cmd) history.push(cmd);
    if (history.length > 50) history.shift();
    historyPos = history.length;
    send(cmd);
  };
  input.addEventListener("keydown", (e) => {
    if (e.key === "Enter") {
      submit();
    } else if (e.key === "ArrowUp" && historyPos > 0) {
      input.value = history[--historyPos];
      e.preventDefault();
    } else if (e.key === "ArrowDown" && historyPos < history.length) {
      input.value = history[++historyPos] || "";
      e.preventDefault();
    }
  });
  const quick = (cmd, title) => el("button", { disabled: !client, title, onclick: () => send(cmd) }, cmd);

  $("main").replaceChildren(
    el("div", { class: "head" }, el("h2", {}, "Console"),
      el("p", { class: "hint" }, client
        ? "The device's debug console over Bluetooth, as on the USB serial port: the log since switching on, then " +
          "new lines live. 'help' lists the commands. When Bluetooth cannot keep up, log lines are dropped (a line " +
          "says how many); answers to commands arrive complete. The console runs while this view is open."
        : "Connect via Bluetooth to use the console.")),
    out,
    el("div", { class: "console-line" }, input, el("button", { class: "primary", disabled: !client, onclick: submit }, "Send")),
    el("div", { class: "buttons" },
      quick("status", "Device, battery, GPS summary"), quick("gps", "GPS details and start milestones"),
      quick("gps sats", "Satellites heard and their signal strength (C/N0)"),
      quick("rtc", "Clock and the last GPS synchronisation"), quick("sensors", "Barometer, altitude, climb rate"),
      quick("log", "The device's log since switching on, again"), quick("help", "All commands"),
      el("span", { class: "spacer" }),
      el("button", { onclick: async () => {
        try {
          await navigator.clipboard.writeText(lines.join("\n"));
          note.textContent = "Copied to the clipboard.";
        } catch (e) {
          note.textContent = e.message;
        }
      } }, "Copy"),
      el("button", { onclick: () => download(`ultrabeep-console-${new Date().toISOString().slice(0, 19).replace(/:/g, "-")}.txt`,
        lines.join("\n")) }, "Save"),
      el("button", { onclick: () => { lines = [""]; draw(); } }, "Clear")),
    note);
  if (!client) return;

  let open = true;
  client.consoleOpen(add).then(() => input.focus()).catch((e) => {
    open = false;
    note.textContent = /unknown command/.test(e.message)
      ? "The firmware on the device has no Bluetooth console yet (UltraBeep 0.38 or newer)."
      : e.message;
  });
  state.cleanup = () => {
    if (open) client.consoleClose();
  };
}

function render() {
  const loaded = !!state.stock;
  $("welcome").hidden = loaded;
  $("editor").hidden = !loaded;
  if (!loaded) return;
  leaveView();
  if (!levelOk(VIEW_LEVELS[state.view])) state.view = "system"; /* hidden by the view mode */
  $("editor").classList.toggle("wide", state.view === "console");
  renderNav();
  if (state.view === "system") renderSystem();
  else if (state.view === "volume") renderVolume();
  else if (state.view === "recording") renderRecording();
  else if (state.view === "status") renderStatus();
  else if (state.view === "console") renderConsole();
  else if (state.view === "files") renderFiles();
  else if (state.view === "tracks") renderTracks();
  else if (state.view === "firmware") renderFirmware();
  else if (state.view === "voice") renderVoice();          /* js/voicepacks.js */
  else if (state.view === "speaker") renderSpeaker();      /* js/speaker.js */
  else if (state.view === "voicepack") renderVoicepack();  /* js/voicegen.js */
  else renderProfile(profileExists(state.profile) ? state.profile : 1);
  refreshStatus();
}

const IF_FOUND_FILE = "if_found.txt"; /* in the extension folder; the device copies it to IF_FOUND_PLEASE_READ.txt */
const IF_FOUND_MAX = 2000;
const ifFoundChanged = () => state.ifFound.text.trim() !== state.ifFound.saved.trim();

/* A template for the note, with the pilot's name from profile 1. */
function ifFoundTemplate() {
  const pilot = (state.stock && getP(1, "identity_pilotname_string")) || "<your name>";
  return `This paragliding vario belongs to:\n${pilot}\n<street, city, country>\nPhone: <number>   E-mail: <address>\n\n` +
    "If you found it, please get in touch. Thank you!\n";
}

function refreshStatus() {
  const dirty = state.stock && (state.stock.changed || state.ext.changed || ifFoundChanged());
  $("save").disabled = !dirty;
  const src = state.source;
  const pill = $("source");
  if (src && src.kind === "ble") {
    /* the device's name and firmware, a green dot while connected, a red one once the link dropped */
    const on = src.client.connected;
    pill.replaceChildren(el("span", { class: `dot ${on ? "on" : "off"}`, title: on ? "connected" : "disconnected" }),
      `${src.client.device.name} (${src.client.info.version || "?"})`);
  } else {
    pill.textContent = !src ? "nothing loaded"
      : src.kind === "dir" ? `SD card folder "${src.dir.name}"`
      : "new configuration (saving downloads it)";
  }
  $("dirty").textContent = dirty ? "unsaved changes" : "";
}

/* A short notice sliding in from the top, gone after 4 s. */
function toast(text) {
  const old = $("toast");
  if (old) old.remove();
  const t = el("div", { id: "toast", class: "toast", role: "status" }, text);
  document.body.append(t);
  setTimeout(() => t.classList.add("hide"), 3500);
  setTimeout(() => t.remove(), 4000);
}

function setMessage(text, isError) {
  const m = $("message");
  m.textContent = text;
  m.className = isError ? "error" : "";
}

/* ---------------------------------------------------------------- sources */

function loadTexts(stockText, extText) {
  state.stock = new IniDoc(stockText);
  state.ext = new IniDoc(extText);
  if (!state.ext.text) {
    state.ext.text = EXT_HEADER;
    state.ext.original = "";
  }
  state.view = "profile";
  state.profile = parseInt(state.ext.get("global", "system_profilelast_int") || "1", 10) || 1;
  state.navEdit = false;
  state.clipMap = new Map();
  state.clipDeleted = new Set();
  state.ifFound = { text: "", saved: "" };
  render();
}

async function readHandle(dir, name) {
  try {
    const h = await dir.getFileHandle(name);
    return await (await h.getFile()).text();
  } catch (e) {
    return null;
  }
}

/* Our extension file lives in the folder "ultrabeep" (the stock firmware deletes unknown
 * files in the top folder). */
const EXT_DIR = "ultrabeep";

async function subdir(dir, name, create = false) {
  try {
    return await dir.getDirectoryHandle(name, { create });
  } catch (e) {
    return null;
  }
}

async function openFolder() {
  try {
    const dir = await window.showDirectoryPicker({ mode: "readwrite" });
    const stock = await readHandle(dir, "config.ultrabip");
    const sub = await subdir(dir, EXT_DIR);
    const ext = sub ? await readHandle(sub, "config_ext.ultrabip") : null;
    const note = (sub && await readHandle(sub, IF_FOUND_FILE)) || "";
    if (stock === null) setMessage("No config.ultrabip in this folder - choose the top folder of the SD card.", true);
    else setMessage("Loaded from the SD card folder.");
    state.source = { kind: "dir", dir };
    loadTexts(stock || "", ext || "");
    state.ifFound = { text: note, saved: note };
  } catch (e) {
    if (e.name !== "AbortError") setMessage(e.message, true);
  }
}

/* A configuration from the defaults (the neutral example with three profiles), without a device:
 * every view is shown; saving downloads both files. */
async function newConfig() {
  if (state.source && state.source.kind === "ble" && state.source.client.connected) {
    setMessage("Disconnect the device first: a new configuration is for trying the configurator without one.", true);
    return;
  }
  if (state.stock && (state.stock.changed || state.ext.changed) &&
      !confirm("Discard the unsaved changes and start a new configuration?")) return;
  try {
    const stock = await fetch("examples/config.ultrabip").then((r) => {
      if (!r.ok) throw new Error(`examples/config.ultrabip: ${r.status}`);
      return r.text();
    });
    state.source = { kind: "new" };
    loadTexts(stock, "");
    state.stock.original = ""; /* nothing saved yet: both files count as changed */
    refreshStatus();
    setMessage("New configuration with the defaults. Saving downloads config.ultrabip (top folder of the SD " +
      "card) and config_ext.ultrabip (folder ultrabeep).");
  } catch (e) {
    setMessage(`New configuration failed: ${e.message}`, true);
  }
}

/* ---- Bluetooth: connect on request, and again by itself once the browser knows the device ---- */

const AUTO_BLE_KEY = "cfg.autoble";
let manualDisconnect = false;
let autoWatch = null; /* the AbortController of a running advertisement watch, or a poll timer */

function autoBleEnabled() {
  try { return localStorage.getItem(AUTO_BLE_KEY) === "1"; } catch (e) { return false; }
}

function stopAutoReconnect() {
  if (autoWatch && autoWatch.abort) autoWatch.abort();
  if (autoWatch && autoWatch.timer) clearInterval(autoWatch.timer);
  autoWatch = null;
}

/*
 * Chrome keeps the permission for a device chosen once: the page connects again by itself when
 * the device advertises (switched on, in range). Without watchAdvertisements it tries every 5 s.
 */
async function autoReconnect() {
  stopAutoReconnect();
  if (!autoBleEnabled() || deviceClient() || manualDisconnect) return;
  const devices = await BleFileClient.knownDevices();
  if (!devices.length) {
    $("source").textContent = "auto-connect: connect once via the button";
    return;
  }
  const device = devices[0];
  $("source").textContent = `waiting for ${device.name} …`;
  const tryConnect = async () => {
    if (deviceClient() || !autoBleEnabled() || manualDisconnect) return;
    try { await connectBle(device); } catch (e) { /* not in range yet */ }
  };
  if (device.watchAdvertisements) {
    try {
      const ctrl = new AbortController();
      autoWatch = ctrl;
      device.addEventListener("advertisementreceived", () => { ctrl.abort(); tryConnect(); }, { once: true });
      await device.watchAdvertisements({ signal: ctrl.signal });
      return;
    } catch (e) { /* not supported on this platform: poll instead */ }
  }
  const timer = setInterval(() => {
    if (deviceClient() || !autoBleEnabled() || manualDisconnect) { clearInterval(timer); return; }
    tryConnect();
  }, 5000);
  autoWatch = { timer };
  tryConnect();
}

/* The device asks for a button press before it lets a new browser in (firmware 0.45): a prompt with a
 * picture of the device, shown while the AUTH waits for the press; Cancel drops the connection. */
const PAIRING_SVG = `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 260 200" role="img" aria-label="A finger pressing the red button on the side of the device">
 <g transform="rotate(-10 130 90)">
  <rect x="48" y="44" width="164" height="108" rx="22" fill="#5f6a75"/>
  <rect x="48" y="30" width="164" height="108" rx="22" fill="#9aa4ae" stroke="#434c56" stroke-width="3"/>
  <rect x="68" y="44" width="124" height="68" rx="7" fill="#182341" stroke="#0c1326" stroke-width="2"/>
  <g stroke="#3b4d78" stroke-width="2">
   <line x1="86" y1="46" x2="86" y2="110"/><line x1="104" y1="46" x2="104" y2="110"/><line x1="122" y1="46" x2="122" y2="110"/>
   <line x1="140" y1="46" x2="140" y2="110"/><line x1="158" y1="46" x2="158" y2="110"/><line x1="176" y1="46" x2="176" y2="110"/>
  </g>
  <rect x="68" y="76" width="124" height="2" fill="#3b4d78"/>
  <circle cx="84" cy="150" r="11" fill="#d7263d" stroke="#8a1021" stroke-width="2.5"/>
  <circle cx="84" cy="150" r="5" fill="#ef5468"/>
 </g>
 <path d="M22 196 C 34 164, 54 152, 80 150 L 88 158 C 66 166, 50 184, 40 200 Z" fill="#fff" stroke="#2b2b2b" stroke-width="2.5" stroke-linejoin="round"/>
 <path d="M78 151 q 7 2 9 7" fill="none" stroke="#2b2b2b" stroke-width="2" stroke-linecap="round"/>
 <path d="M112 148 q 8 -6 16 -2 M114 162 q 10 -2 16 5" fill="none" stroke="#d7263d" stroke-width="2.5" stroke-linecap="round"/>
</svg>`;

let pairingClient = null;
function pairingPrompt(show, name, client) {
  const old = $("pairing");
  if (old) old.remove();
  if (!show) { pairingClient = null; return; }
  pairingClient = client;
  const picture = el("div", { class: "picture" });
  picture.innerHTML = PAIRING_SVG; /* a fixed drawing, no user text */
  document.body.append(el("div", { id: "pairing", class: "modal", role: "dialog", "aria-modal": "true", "aria-labelledby": "pairing-title" },
    el("div", { class: "modal-card" }, picture,
      el("h2", { id: "pairing-title" }, "Press the button on the device"),
      el("p", {}, `${name} does not know this browser yet. The device plays two tones and asks for a short press of ` +
        "its button, which allows this browser from now on. You have 60 s."),
      el("button", { type: "button", onclick: () => { const c = pairingClient; pairingPrompt(false); if (c) c.disconnect(); } }, "Cancel"))));
}

async function connectBle(device = null) {
  const client = new BleFileClient(() => {
    toast(`Bluetooth disconnected: ${client.device ? client.device.name : "the device"}`);
    setMessage("");
    $("ble").textContent = "Connect via Bluetooth";
    if (["status", "console", "tracks", "firmware"].includes(state.view)) state.view = "system";
    render(); /* device tests and live values are gone */
    if (autoBleEnabled() && !manualDisconnect) setTimeout(autoReconnect, 2000);
  });
  try {
    setMessage("");
    $("source").textContent = device ? `connecting to ${device.name} …` : "connecting …";
    await client.connect(device);
    /* a device that does not know this browser asks for a button press (firmware 0.45) */
    try {
      await client.authorize(bleSecret(), () => pairingPrompt(true, client.device.name, client));
    } finally {
      pairingPrompt(false);
    }
    manualDisconnect = false;
    stopAutoReconnect();
    const stock = await client.getFile("config.ultrabip");
    let ext = "", note = "";
    try {
      ext = await client.getFile("config_ext.ultrabip");
    } catch (e) {
      ext = ""; /* not created yet */
    }
    try {
      note = await client.getFile(IF_FOUND_FILE);
    } catch (e) {
      note = ""; /* none yet, or firmware before 0.48 */
    }
    state.source = { kind: "ble", client };
    $("ble").textContent = "Disconnect";
    loadTexts(stock, ext); /* the header shows the device and the green dot */
    state.ifFound = { text: note, saved: note };
  } catch (e) {
    if (e.name !== "NotFoundError") setMessage(e.message, true);
    client.disconnect();
  }
}

function download(name, text) {
  const a = el("a", { href: URL.createObjectURL(new Blob([text], { type: "text/plain" })), download: name });
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 1000);
}

/* Settings the device reads only at power on: what a change of them still needs after "Apply now". */
function powerOnOnlyChanges() {
  const was = { stock: new IniDoc(state.stock.original), ext: new IniDoc(state.ext.original) };
  const differs = (doc, section, key) => was[doc].get(section, key) !== state[doc].get(section, key);
  const out = [];
  const pilots = () => { for (let n = 1; n <= PROFILE_MAX; n++) if (differs(n <= STOCK_PROFILES ? "stock" : "ext", section(n), "identity_pilotname_string")) return true; return false; };
  if (differs("ext", "global", "system_blename_string") || (!state.ext.get("global", "system_blename_string") && pilots())) out.push("the Bluetooth name");
  for (let n = 1; n <= PROFILE_MAX; n++) {
    if (differs(n <= STOCK_PROFILES ? "stock" : "ext", section(n), "bluetooth_remoteenable_bool")) { out.push("the remote control"); break; }
  }
  if (differs("ext", "global", "system_rawlog_bool")) out.push("the raw log");
  return out;
}

/* "Apply now" in the message after a save over Bluetooth: the device reloads its configuration at once. */
function applyNowLink(client, later) {
  const a = el("a", { href: "#", class: "msg-action", onclick: async (e) => {
    e.preventDefault();
    a.textContent = "Applying …";
    try {
      await client.reloadConfig();
      const list = later.join(", ").replace(/, ([^,]*)$/, " and $1");
      const rest = later.length ? ` ${list.charAt(0).toUpperCase()}${list.slice(1)} ${later.length > 1 ? "follow" : "follows"} at the next power on.` : "";
      setMessage(`Applied: the device reloaded its configuration.${rest}`);
    } catch (err) {
      setMessage(/unknown command/.test(err.message) ? "This firmware takes new settings at the next power on (Apply now needs 0.49)."
        : /in flight/.test(err.message) ? "Not in flight: the settings apply after the landing at the next power on, or with Apply now on the ground."
          : `Apply now: ${err.message}`, true);
    }
  } }, "Apply now");
  return a;
}

async function save() {
  const changed = [["config.ultrabip", state.stock], ["config_ext.ultrabip", state.ext]].filter(([, d]) => d.changed);
  const later = state.stock && state.ext && changed.length ? powerOnOnlyChanges() : [];
  try {
    for (const [name, doc] of changed) {
      const src = state.source;
      if (src.kind === "dir") {
        const target = name === "config_ext.ultrabip" ? await src.dir.getDirectoryHandle(EXT_DIR, { create: true }) : src.dir;
        const h = await target.getFileHandle(name, { create: true });
        const w = await h.createWritable();
        await w.write(doc.text);
        await w.close();
      } else if (src.kind === "ble") {
        await src.client.putFile(name, doc.text);
      } else {
        download(name, doc.text);
      }
      doc.markSaved();
    }
    if (ifFoundChanged()) {
      /* the note goes to ultrabeep/if_found.txt; the device copies it to the top folder, or removes the copy
       * when the note is empty (an empty upload is refused, so a bare line break stands for "no text") */
      const src = state.source;
      const text = state.ifFound.text.trim() ? state.ifFound.text.replace(/\r\n/g, "\n").replace(/\n*$/, "\n") : "";
      if (src.kind === "ble") {
        await src.client.putFile(IF_FOUND_FILE, text || "\n");
      } else if (src.kind === "dir") {
        const sub = await subdir(src.dir, EXT_DIR, true);
        if (text) {
          const w = await (await sub.getFileHandle(IF_FOUND_FILE, { create: true })).createWritable();
          await w.write(text);
          await w.close();
        } else {
          try { await sub.removeEntry(IF_FOUND_FILE); } catch (e) { /* none */ }
        }
      } else if (text) {
        download(IF_FOUND_FILE, text);
      }
      state.ifFound.saved = state.ifFound.text;
      changed.push([IF_FOUND_FILE]);
    }
    const clips = await applyClipRenames(state.source); /* the voice lines follow renumbered profiles */
    setMessage((state.source.kind === "new"
      ? `Downloaded: ${changed.map(([n]) => n).join(", ")}. Copy config.ultrabip to the top folder of the SD card ` +
        "and config_ext.ultrabip into its folder ultrabeep."
      : `Saved: ${changed.map(([n]) => n).join(", ")}. New settings apply at the next power on.`) + clips);
    const configSaved = changed.some(([n]) => n === "config.ultrabip" || n === "config_ext.ultrabip");
    if (configSaved && state.source.kind === "ble" && state.source.client.connected) {
      $("message").append(" ", applyNowLink(state.source.client, later));
    }
  } catch (e) {
    setMessage(`Saving failed: ${e.message}`, true);
  }
  render();
}

/* ---------------------------------------------------------------- start */

window.addEventListener("DOMContentLoaded", () => {
  $("open-folder").addEventListener("click", openFolder);
  $("new-config").addEventListener("click", newConfig);
  $("ble").addEventListener("click", () => {
    if (state.source && state.source.kind === "ble" && state.source.client.connected) {
      manualDisconnect = true; /* no automatic reconnect until the next connect by hand */
      stopAutoReconnect();
      state.source.client.disconnect();
    } else {
      manualDisconnect = false;
      connectBle();
    }
  });
  const mode = $("mode");
  try {
    const saved = localStorage.getItem(MODE_KEY);
    if (LEVELS.includes(saved)) state.mode = saved;
  } catch (e) { /* page only */ }
  mode.value = state.mode;
  mode.addEventListener("change", () => {
    state.mode = mode.value;
    try { localStorage.setItem(MODE_KEY, state.mode); } catch (e) { /* page only */ }
    render();
  });
  $("save").addEventListener("click", save);
  if (!window.showDirectoryPicker) $("open-folder").hidden = true;
  if (!navigator.bluetooth) $("ble").title = "Needs Chrome or Edge";
  /* auto-connect: only where the browser remembers permitted devices (Chrome, Edge) */
  const auto = $("auto-ble");
  if (navigator.bluetooth && navigator.bluetooth.getDevices) {
    auto.checked = autoBleEnabled();
    auto.addEventListener("change", () => {
      try { localStorage.setItem(AUTO_BLE_KEY, auto.checked ? "1" : "0"); } catch (e) { /* page only */ }
      manualDisconnect = false;
      if (auto.checked) autoReconnect(); else { stopAutoReconnect(); if (!deviceClient()) refreshStatus(); }
    });
    autoReconnect();
  } else {
    auto.parentElement.hidden = true;
  }
  window.addEventListener("beforeunload", (e) => {
    if (state.stock && (state.stock.changed || state.ext.changed)) e.preventDefault();
  });
  render();
});
