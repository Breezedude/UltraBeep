"use strict";

/*
 * INI text that is edited in place: only the changed values are replaced,
 * every other line (comments, unknown keys, order, line endings) stays.
 * Same rules as the firmware (firmware/src/app/ini.c).
 */
class IniDoc {
  constructor(text) {
    this.text = text || "";
    this.original = this.text;
  }

  get changed() {
    return this.text !== this.original;
  }

  markSaved() {
    this.original = this.text;
  }

  get newline() {
    return this.text.includes("\r\n") ? "\r\n" : "\n";
  }

  /* [{start, end, kind: "section"|"kv"|"other", section, key, value, valueStart, valueEnd}] */
  lines() {
    const out = [];
    let section = "";
    let pos = 0;
    const text = this.text;
    while (pos <= text.length) {
      let eol = text.indexOf("\n", pos);
      const end = eol < 0 ? text.length : eol;
      const next = eol < 0 ? text.length + 1 : eol + 1;
      let raw = text.slice(pos, end);
      if (raw.endsWith("\r")) raw = raw.slice(0, -1);
      const lead = raw.length - raw.trimStart().length;
      const trimmed = raw.trim();
      const line = { start: pos, end: next, kind: "other", section };
      if (trimmed.startsWith("[") && trimmed.includes("]")) {
        section = trimmed.slice(1, trimmed.indexOf("]"));
        line.kind = "section";
        line.section = section;
      } else if (trimmed && !trimmed.startsWith("#") && !trimmed.startsWith(";") && trimmed.includes("=")) {
        const eq = raw.indexOf("=");
        line.kind = "kv";
        line.key = raw.slice(0, eq).trim();
        const afterEq = raw.slice(eq + 1);
        const valueLead = afterEq.length - afterEq.trimStart().length;
        line.value = afterEq.trim();
        line.valueStart = pos + eq + 1 + valueLead;
        line.valueEnd = line.valueStart + line.value.length;
      }
      if (lead && line.kind === "other" && !trimmed) line.kind = "other";
      out.push(line);
      if (eol < 0) break;
      pos = next;
    }
    return out;
  }

  sections() {
    return [...new Set(this.lines().filter((l) => l.kind === "section").map((l) => l.section))];
  }

  hasSection(section) {
    return this.sections().includes(section);
  }

  /* Drops a key's line; nothing happens when it is not there. */
  remove(section, key) {
    for (const l of this.lines()) {
      if (l.kind === "kv" && l.section === section && l.key === key) {
        this.text = this.text.slice(0, l.start) + this.text.slice(l.end);
        return;
      }
    }
  }

  /* Drops a section with everything in it, up to the next section header. */
  removeSection(section) {
    const lines = this.lines();
    let from = -1, to = -1;
    for (const l of lines) {
      if (l.kind === "section" && l.section === section && from < 0) from = l.start;
      else if (from >= 0 && l.kind === "section") { to = l.start; break; }
    }
    if (from < 0) return;
    this.text = this.text.slice(0, from) + (to < 0 ? "" : this.text.slice(to));
  }

  get(section, key) {
    let value;
    for (const l of this.lines()) {
      if (l.kind === "kv" && l.section === section && l.key === key) value = l.value;
    }
    return value;
  }

  keys(section) {
    return this.lines().filter((l) => l.kind === "kv" && l.section === section).map((l) => l.key);
  }

  set(section, key, value) {
    value = String(value);
    const nl = this.newline;
    const lines = this.lines();
    let found = false;
    let insertAt = -1;
    let inSection = false;
    for (const l of lines) {
      if (l.kind === "section") {
        if (inSection) break;
        inSection = l.section === section;
        if (inSection) {
          found = true;
          insertAt = Math.min(l.end, this.text.length);
        }
      } else if (inSection && l.kind === "kv") {
        if (l.key === key) {
          this.text = this.text.slice(0, l.valueStart) + value + this.text.slice(l.valueEnd);
          return;
        }
        insertAt = Math.min(l.end, this.text.length);
      }
    }
    if (found) {
      const needNl = insertAt === this.text.length && this.text.length > 0 && !this.text.endsWith("\n");
      const add = (needNl ? nl : "") + `${key}=${value}${nl}`;
      this.text = this.text.slice(0, insertAt) + add + this.text.slice(insertAt);
      return;
    }
    let prefix = "";
    if (this.text.length > 0) {
      prefix = (this.text.endsWith("\n") ? "" : nl) + nl;
    }
    this.text += `${prefix}[${section}]${nl}${key}=${value}${nl}`;
  }
}
