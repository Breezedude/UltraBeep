"use strict";

/*
 * Self-test of the time zone helpers in js/schema.js under Node: the time change table the configurator
 * writes into config.ultrabip (global_timechanges_longstring) matches the one the BipLink app wrote on a
 * device, the rules give the right Sundays, and the standard offset comes back out of a table.
 *   node tools/configurator/tests/timezone_test.js
 */
const path = require("node:path");
global.VOICEPACKS = require(path.join(__dirname, "..", "js", "voicepack.js")).VOICEPACKS;
const { TIME_ZONES, dstTable, dstTableStandardOffset } = require(path.join(__dirname, "..", "js", "schema.js"));

let failed = 0;
const check = (ok, what) => {
  console.log(`${ok ? "ok  " : "FAIL"} ${what}`);
  if (!ok) failed++;
};

/* From a device's config.ultrabip, written by the app on 2026-03-28 (global_timestamp 1774690499). */
const APP_TABLE = "251026  60,260329 120,261025  60,270328 120,271031  60,280326 120,281029  60,290325 120,291028  60," +
  "300331 120,301027  60,310330 120,311026  60,320328 120,321031  60,330327 120,331030  60,340326 120,341029  60," +
  "350325 120,351028  60,360330 120,361026  60,370329 120,371025  60,380328 120,381031  60,390327 120,391030  60," +
  "400325 120,401028  60,";
const ours = dstTable("eu", 60, new Date(1774690499 * 1000));
check(ours === APP_TABLE, `EU table from 2026-03-28 equals the app's (${ours.length} characters)`);
check(/^(\d{6} +-?\d+,)+$/.test(ours), "the app's entry format");

const eu2026 = dstTable("eu", 60, new Date(Date.UTC(2026, 0, 15)));
check(eu2026.startsWith("251026  60,260329 120,261025  60,"), "in January the previous autumn's entry leads");
const us = dstTable("us", -300, new Date(Date.UTC(2026, 6, 1)));
check(us.startsWith("260308 -240,261101 -300,"), `US 2026: second Sunday of March, first Sunday of November (${us.slice(0, 24)})`);
const au = dstTable("au", 600, new Date(Date.UTC(2026, 6, 1)));
check(au.startsWith("260405 600,261004 660,"), `AU 2026: first Sunday of April back, first Sunday of October on (${au.slice(0, 22)})`);
const nz = dstTable("nz", 720, new Date(Date.UTC(2026, 6, 1)));
check(nz.startsWith("260405 720,260927 780,"), `NZ 2026: first Sunday of April, last Sunday of September (${nz.slice(0, 22)})`);
const cl = dstTable("cl", -240, new Date(Date.UTC(2026, 6, 1)));
check(cl.startsWith("260405 -240,260906 -180,"), `Chile 2026: first Sunday of April, first Sunday of September (${cl.slice(0, 24)})`);
check(dstTable("none", 60) === "" && dstTable("x", 60) === "", "no table without summer time");
check(dstTable("eu", 60).endsWith("401028  60,"), "the table runs to 2040");

check(dstTableStandardOffset(APP_TABLE) === 60 && dstTableStandardOffset(us) === -300 && dstTableStandardOffset(au) === 600,
  "the standard offset is the table's smallest");
check(dstTableStandardOffset("") === null && dstTableStandardOffset(undefined) === null, "no entries: null");

const berlin = TIME_ZONES.find((z) => z.id === "berlin");
check(berlin && berlin.offset === 60 && berlin.rule === "eu", "Berlin: UTC+1, EU rule");
check(TIME_ZONES.every((z) => z.rule === "none" || dstTable(z.rule, z.offset).length > 100), "every rule in the list yields a table");

console.log(failed ? `${failed} failed` : "all passed");
process.exit(failed ? 1 : 0);
