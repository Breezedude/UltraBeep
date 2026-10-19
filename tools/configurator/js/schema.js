"use strict";

/* Settings shown by the configurator. Stock keys of profiles 1-3 live in
 * config.ultrabip, everything else in config_ext.ultrabip (docs/config-ext-format.md).
 *
 * Field: {key, label, type: text | int | bool | select, unit, min, max, def, options,
 *         help: text of the info circle, stock: only the stock firmware uses it,
 *         ext: the key lives in config_ext.ultrabip for every profile} */

const PROFILE_MAX = 10;
const STOCK_PROFILES = 3;

const PROFILE_TYPES = [
  "paragliding", "cross", "hike", "bivouac", "acro", "paramotor",
  "competition", "speedriding", "tandem", "alpinism",
];

/* led_userled_int: the stock numbering, so BipLink shows the same choice. */
const LED_OPTIONS = [
  ["0", "Off"],
  ["1", "Vario colours"],
  ["2", "Vario colours when the vario is muted"],
  ["3", "Power indicator"],
  ["4", "Power indicator when the vario is muted"],
];
const LED_HELP = "What the LED shows once the GPS has a fix (before that it blinks yellow while searching). " +
  "Vario colours: green with every lift beep, red during the sink tone - the LED blinks like the tones, " +
  "in flight only. Power indicator: on battery a blink every 3 s - green 3× full, 2× medium, 1× ok; " +
  "red 1× nearly empty, 2× about to switch off. On USB power: green breathing while charging, dim green " +
  "when full. \"When the vario is muted\": only at the flight level mute (a short press in flight). Whenever " +
  "the LED would show nothing (Off, the vario colours on the ground, the muted choices at other levels), a very " +
  "short green flash every 4 s shows that the device is on; after switching off it flashes blue while Bluetooth " +
  "stays on for the track download.";

const STOCK_ONLY = "Used by the stock firmware only; UltraBeep ignores it for now. It stays in the file " +
  "so BipLink and the stock firmware keep working.";

/* Profile view order: identity, vario, tone curve, announcements, then the groups after it. */
const PROFILE_GROUPS_BEFORE_TONES = [
  {
    title: "Identity",
    fields: [
      { key: "identity_profilename_string", label: "Profile type", type: "select", options: PROFILE_TYPES.map((t) => [t, t]),
        help: "Announced when you choose the profile after switching on (the voice pack has a clip for each type)." },
      { key: "identity_wingname_string", label: "Wing", type: "text",
        help: "Your glider. Written into the IGC header and the KML description." },
      { key: "identity_pilotname_string", label: "Pilot name", type: "text",
        help: "Written into the IGC header (pilot in charge) and the KML title." },
      { key: "identity_compid_string", label: "Competition ID", type: "text",
        help: "Competition number, written into the IGC header." },
      { key: "identity_birthdate_string", label: "Birth date", type: "text", stock: true, help: STOCK_ONLY },
      { key: "identity_notes_string", label: "Notes", type: "text", help: "Free text for yourself; the device does not use it." },
    ],
  },
  {
    title: "Vario",
    fields: [
      { key: "vario_integrationtime_int", label: "Integration time", unit: "% (0 = fast)", type: "int", min: 0, max: 100, def: "25",
        help: "How calm the vario tones are: the climb rate they follow is smoothed over about 0.3 / 0.7 / 1.4 / " +
          "2.8 / 5.7 s at 0 / 25 / 50 / 75 / 100 % (as in the stock app). More smooths out pendulum swings and " +
          "turbulence and gives fewer false lift beeps, but the tone reacts later." },
      { key: "vario_liftysound_bool", label: "Weak-lift tone", type: "bool", def: "true",
        help: "A short tick every second while you neither climb nor sink: from 0.4 m/s below the lift " +
          "threshold up to it (\"zero sink\" - the air carries the glider), pitched like the curve there. " +
          "In flight only." },
      { key: "vario_sweeptone_bool", label: "Sweep tone", type: "bool", def: "true",
        help: "The pitch follows the climb rate while a beep sounds: a change in the climb is heard at once, " +
          "not only with the next beep. Off: each beep keeps the pitch it started with." },
    ],
  },
];

/* Double-tap sensitivity as in the stock configurator (system_tapsensitivity_int). */
const TAP_SENSITIVITY = [
  ["0", "Extremely easy to trigger"], ["1", "Very easy to trigger"], ["2", "Easy to trigger"], ["3", "Balanced"],
  ["4", "Difficult to trigger"],
];

const PROFILE_GROUPS_AFTER_TONES = [
  {
    title: "Altitude alerts",
    help: "In flight: the error melody and the altitude once when the limit is crossed, then every 5 min while " +
      "you stay beyond it (10 m of hysteresis on the way back). Altitudes above sea level.",
    fields: [
      { key: "alarm_min2_int", label: "Minimum altitude", unit: "m (0 = off)", type: "int", min: 0, max: 9000, def: "0",
        help: "Alert below this altitude, e.g. a minimum height over a valley or a lake." },
      { key: "alarm_max2_int", label: "Maximum altitude", unit: "m (0 = off)", type: "int", min: 0, max: 9000, def: "0",
        help: "Alert above this altitude, e.g. an airspace ceiling." },
    ],
  },
];

/*
 * System: settings for the whole device. The stock format keeps them per profile, so the
 * configurator shows profile 1's value and writes a change into every profile.
 */
/* Time zones: places, the standard offset from UTC in minutes and the summer time rule the firmware knows
 * (announce.h dst_rule_t). The configurator stores the place (system_timezone_string), the offset (every
 * profile, stock key) and the rule (system_dstrule_string). */
const TIME_ZONES = [
  { id: "utc", places: "UTC", offset: 0, rule: "none" },
  { id: "azores", places: "Azores", offset: -60, rule: "eu" },
  { id: "london", places: "London, Dublin, Lisbon, Canary Islands", offset: 0, rule: "eu" },
  { id: "reykjavik", places: "Reykjavik, Accra, Dakar", offset: 0, rule: "none" },
  { id: "berlin", places: "Berlin, Paris, Madrid, Rome, Vienna, Zurich", offset: 60, rule: "eu" },
  { id: "lagos", places: "Lagos, Algiers, Tunis", offset: 60, rule: "none" },
  { id: "athens", places: "Athens, Helsinki, Kyiv, Bucharest, Sofia", offset: 120, rule: "eu" },
  { id: "johannesburg", places: "Johannesburg, Cape Town, Harare", offset: 120, rule: "none" },
  { id: "istanbul", places: "Istanbul, Moscow, Riyadh, Nairobi", offset: 180, rule: "none" },
  { id: "tehran", places: "Tehran", offset: 210, rule: "none" },
  { id: "dubai", places: "Dubai, Tbilisi, Baku", offset: 240, rule: "none" },
  { id: "karachi", places: "Karachi, Tashkent", offset: 300, rule: "none" },
  { id: "india", places: "India, Sri Lanka", offset: 330, rule: "none" },
  { id: "nepal", places: "Nepal", offset: 345, rule: "none" },
  { id: "dhaka", places: "Dhaka, Almaty, Bishkek", offset: 360, rule: "none" },
  { id: "bangkok", places: "Bangkok, Jakarta, Hanoi", offset: 420, rule: "none" },
  { id: "beijing", places: "Beijing, Singapore, Manila, Perth", offset: 480, rule: "none" },
  { id: "tokyo", places: "Tokyo, Seoul", offset: 540, rule: "none" },
  { id: "darwin", places: "Darwin", offset: 570, rule: "none" },
  { id: "adelaide", places: "Adelaide", offset: 570, rule: "au" },
  { id: "brisbane", places: "Brisbane", offset: 600, rule: "none" },
  { id: "sydney", places: "Sydney, Melbourne, Hobart", offset: 600, rule: "au" },
  { id: "auckland", places: "Auckland, Wellington", offset: 720, rule: "nz" },
  { id: "sao_paulo", places: "São Paulo, Buenos Aires, Montevideo", offset: -180, rule: "none" },
  { id: "santiago", places: "Santiago de Chile", offset: -240, rule: "cl" },
  { id: "caracas", places: "Caracas, La Paz, Manaus", offset: -240, rule: "none" },
  { id: "halifax", places: "Halifax", offset: -240, rule: "us" },
  { id: "new_york", places: "New York, Toronto, Miami", offset: -300, rule: "us" },
  { id: "bogota", places: "Bogotá, Lima, Quito, Panama", offset: -300, rule: "none" },
  { id: "chicago", places: "Chicago, Dallas, Winnipeg", offset: -360, rule: "us" },
  { id: "mexico_city", places: "Mexico City, Guatemala, Costa Rica", offset: -360, rule: "none" },
  { id: "denver", places: "Denver, Salt Lake City, Calgary", offset: -420, rule: "us" },
  { id: "phoenix", places: "Phoenix", offset: -420, rule: "none" },
  { id: "los_angeles", places: "Los Angeles, Vancouver, Seattle", offset: -480, rule: "us" },
  { id: "anchorage", places: "Anchorage", offset: -540, rule: "us" },
  { id: "honolulu", places: "Honolulu", offset: -600, rule: "none" },
];
const DST_RULE_NAMES = { none: "no summer time", eu: "EU summer time", us: "US summer time", au: "Australian summer time",
  nz: "New Zealand summer time", cl: "Chilean summer time" };

function utcOffsetText(min) {
  const sign = min < 0 ? "−" : "+";
  const a = Math.abs(min);
  return `UTC${sign}${Math.floor(a / 60)}${a % 60 ? `:${String(a % 60).padStart(2, "0")}` : ""}`;
}

function zoneLabel(z) {
  return `${z.places} (${utcOffsetText(z.offset)}${z.rule !== "none" ? `, ${DST_RULE_NAMES[z.rule]}` : ""})`;
}

/*
 * The stock's time change table (global_timechanges_longstring in config.ultrabip): "yymmdd offset," entries,
 * the UTC offset in minutes from that day on, as the BipLink app writes them for the phone's time zone -
 * from the switch in force at `from` to the end of 2040. The firmware takes the offset from this table while
 * "summer time automatically" is set (the app also stores the summer offset in the profiles, which must
 * not get the summer hour on top), so the configurator writes the table of the chosen zone. Zones without
 * summer time get an empty table.
 */
const DST_SWITCHES = { /* [month, n-th Sunday (-1: last), minutes added] */
  eu: [[3, -1, 60], [10, -1, 0]], us: [[3, 2, 60], [11, 1, 0]], au: [[4, 1, 0], [10, 1, 60]],
  nz: [[4, 1, 0], [9, -1, 60]], cl: [[4, 1, 0], [9, 1, 60]],
};

function dstTable(rule, offset, from = new Date(), untilYear = 2040) {
  const sw = DST_SWITCHES[rule];
  if (!sw) return "";
  const sunday = (y, m, n) => {
    if (n < 0) {
      const d = new Date(Date.UTC(y, m, 0)); /* the month's last day */
      d.setUTCDate(d.getUTCDate() - d.getUTCDay());
      return d;
    }
    const d = new Date(Date.UTC(y, m - 1, 1));
    d.setUTCDate(1 + (7 - d.getUTCDay()) % 7 + 7 * (n - 1));
    return d;
  };
  const entries = [];
  for (let y = from.getUTCFullYear() - 1; y <= untilYear; y++) {
    for (const [m, n, add] of sw) entries.push([sunday(y, m, n), offset + add]);
  }
  entries.sort((a, b) => a[0] - b[0]);
  let first = 0;
  for (let i = 0; i < entries.length; i++) if (entries[i][0] <= from) first = i;
  const two = (v) => String(v).padStart(2, "0");
  return entries.slice(first)
    .map(([d, off]) => `${two(d.getUTCFullYear() % 100)}${two(d.getUTCMonth() + 1)}${two(d.getUTCDate())} ${String(off).padStart(3)},`)
    .join("");
}

/* The standard offset behind a time change table: its smallest offset; null without entries. */
function dstTableStandardOffset(table) {
  const offs = [...String(table || "").matchAll(/\d{6}\s+(-?\d+)/g)].map((m) => parseInt(m[1], 10));
  return offs.length ? Math.min(...offs) : null;
}

const SYSTEM_GROUPS = [
  {
    title: "Language and time",
    fields: [
      { key: "system_language_string", label: "Voice pack", type: "select", def: "de",
        options: VOICEPACKS.map((p) => [p.id, `${p.label} (${p.id}.ubpack)`]),
        help: "The voice pack in voices/ on the SD card: one of the manufacturer's packs (their file names), a pack " +
          "shared by others (Voice) or one generated in Voice → Custom voicepack. The announcements and the profile names " +
          "come from it; Custom voicepack generates clips in its language." },
      { key: "system_timezone_string", label: "Time zone", type: "select", global: true, zone: true, def: "berlin",
        options: TIME_ZONES.map((z) => [z.id, zoneLabel(z)]),
        help: "Where you fly: the standard offset from UTC and the summer time rule of that place. The device " +
          "gets the offset (system_timeoffsetminutes_int), the rule (system_dstrule_string) and the switch " +
          "dates up to 2040 (global_timechanges_longstring, as the BipLink app writes them); the GPS " +
          "delivers UTC. Used for the spoken time of day, the IGC header and the file dates." },
      { key: "system_timezoneauto_bool", label: "Summer time automatically", type: "bool", def: "true",
        help: "Switches to summer time and back by the zone's rule (EU, USA and Canada, south-east Australia, " +
          "New Zealand, Chile). Off: standard time all year. Zones without summer time ignore it." },
    ],
  },
  {
    title: "Controls",
    fields: [
      { key: "system_tapsensitivity_int", label: "Double tap", type: "select", def: "3", options: TAP_SENSITIVITY,
        help: "How hard you have to knock twice on the device. A double tap speaks the items ticked in the " +
          "column \"Double tap\" of a profile's announcements. Try it under Status and tests." },
      { key: "led_userled_int", label: "LED", type: "select", options: LED_OPTIONS, def: "0", help: LED_HELP },
    ],
  },
  {
    title: "Power",
    fields: [
      { key: "system_autopowerofftime_int", label: "Auto power off", unit: "min (0 = never)", type: "int", min: 0, max: 600, def: "30",
        help: "Switches off after the device has lain still on the ground for this long - after a landing or " +
          "when you forgot it. Never in flight, while Bluetooth is connected or on USB power. Carried up a " +
          "hill it moves, so it stays on." },
      { key: "system_batteryannounce_bool", label: "Announce the battery after switching on", type: "bool", def: "true", global: true,
        help: "Right after the profile selection: \"battery\" and the charge in 25 % steps." },
      { key: "system_batteryannounceonlylow_bool", label: "… only when it is low", type: "bool", def: "true", global: true,
        requires: "system_batteryannounce_bool",
        help: "Stays silent with a charge of 60 % or more - enough for a long flying day." },
    ],
  },
  {
    title: "Sounds",
    fields: [
      { key: "system_poweroffmelody_string", label: "Power-off melody", type: "select", def: "scale", global: true, test: "melody",
        options: [["scale", "Five steps down the scale"], ["chromatic", "Chromatic steps, quick"],
          ["sighs", "Three sighs, each lower"], ["triplet", "G E C and a low G"]],
        help: "Played while you hold the button to switch off; each gets quieter from note to note and lasts " +
          "about a second. ▶ plays it on the connected device." },
    ],
  },
  {
    title: "Bluetooth",
    fields: [
      { key: "bluetooth_enable_bool", label: "Bluetooth", type: "bool", stock: true,
        help: "Stock: switch Bluetooth off. UltraBeep keeps it on for the configurator and the track " +
          "download." },
      { key: "bluetooth_remoteenable_bool", label: "Remote control", type: "bool",
        help: "A double tap presses the space bar on the phone instead of the announcement, for an action " +
          "in the flight app (e.g. XCTrack). Pair the device once in the phone's Bluetooth settings. Takes " +
          "effect at the next power on." },
      { key: "bluetooth_announce_bool", label: "Speak Bluetooth connection", type: "bool", def: "false", global: true,
        help: "\"Bluetooth connected\" and \"Bluetooth disconnected\" are spoken when a phone or the configurator " +
          "connects or drops the link, on the ground and in flight. The pairing confirmation by button then " +
          "plays the confirm melody instead of the clip." },
      { key: "bluetooth_pairannounce_bool", label: "Speak Bluetooth pairing request", type: "bool", def: "true", global: true,
        help: "When a new phone or browser asks for access, the device plays two rising tones and says \"Bluetooth, " +
          "press the button to confirm\" (the extension clip press_button_to_confirm, generated in Voice → Custom voicepack). " +
          "Off: the two tones only." },
      { key: "system_blename_string", label: "Bluetooth name", type: "text", def: "", global: true, maxlength: 14,
        placeholder: "first word of the pilot name",
        help: "The device shows up as UltraBip🪂 followed by this text (up to 14 letters). Empty: the first word " +
          "of the pilot name, as the original firmware does. \"UltraBip\" always stays in front: the BipLink app " +
          "recognises the device by it. Takes effect at the next power on; phones may show the old name until " +
          "they scan again." },
      { key: "system_syncwindow_int", label: "Bluetooth after switching off", type: "select", def: "60", global: true,
        options: [["0", "Off"], ["30", "30 s"], ["60", "60 s"], ["90", "90 s"]],
        help: "After a flight Bluetooth stays on this long once the device is switched off, so the app can " +
          "fetch the track. Longer while a phone is connected, at most 15 min; a button press ends it." },
      { key: "system_syncwhilemoving_bool", label: "Keep on during motion", type: "bool", def: "true", global: true,
        help: "The time above starts only once the device lies still: switch off after the landing, walk to " +
          "the packing place, put everything down - Bluetooth is still on when you take out the phone." },
      { key: "bluetooth_compatibility_bool", label: "Compatibility mode", type: "bool", stock: true,
        help: "Stock option for phones that keep losing the connection (slower, more tolerant connection " +
          "settings for BipLink and XCTrack). UltraBeep does not use it yet." },
    ],
  },
];

/* Barometer sampling (config_ext.ultrabip [global], firmware dps310.h). */
const BARO_RATES = [
  ["100", "100 Hz, 4× oversampling (recommended)"],
  ["64", "64 Hz, 8× oversampling"],
  ["32", "32 Hz, 16× oversampling (up to 0.20)"],
];

const SENSOR_FIELDS = [
  { key: "system_barorate_int", label: "Barometer", type: "select", def: "100", options: BARO_RATES, level: "dev",
    help: "Pressure samples per second. 100 Hz is timed by the processor as in the stock firmware: the " +
      "freshest values and about 20 % less noise per second than 32 Hz, because more short measurements " +
      "beat fewer long ones. 64 Hz and 32 Hz are there for comparisons with the log analyzer." },
  { key: "system_gpsrate_int", label: "GPS fixes per second", type: "select", def: "5", level: "dev",
    options: [["5", "5 per second"], ["1", "1 per second"]], aliases: { 10: "5" },
    help: "The positioning rate asked of the GNSS module. 5 per second feeds the enhanced IGC data; 1 per " +
      "second as the stock firmware. There is no 10 any more: the module computed only 5 real fixes per second " +
      "there, at irregular intervals; a file with 10 runs at 5. Should the module restart itself twice at 5 per " +
      "second in one power-on (seen on a board v1.0 device), firmware 0.59 switches to 1 per second and keeps it, " +
      "with a note above, until \"Try 5 Hz again\"." },
  { key: "system_gpssystems_string", label: "Satellite systems", type: "select", def: "all", level: "dev",
    options: [["all", "All the module accepts (GPS, GLONASS, SBAS, QZSS)"], ["gps+glonass", "GPS and GLONASS"], ["gps", "GPS only"]],
    help: "Fewer systems: less load on the module, a slower first fix and a worse HDOP. Applies at the " +
      "next power on; the console \"gps systems\" tries it at once." },
];

/*
 * Announcements: triggers, each with an ordered list of blocks (docs/config-ext-format.md,
 * "Announcements"). The list is stored per profile in config_ext.ultrabip as
 * announce_<trigger>_items_string = "block[:ble],block,..." (ble = mute while a phone is
 * connected). While that key is missing, the list is derived from the stock keys BipLink
 * writes (voiceauto_*, voicetap_*, flightsummary_*), and the other triggers use their defaults.
 */
const BLOCKS = [
  { id: "altitude", label: "Altitude", ble: true, help: "Altitude above sea level (barometric, referenced to the GPS)." },
  { id: "climbrate", label: "Climb rate", ble: false,
    help: "Mean climb or sink over the time chosen next to the name (10 s unless changed), as the number alone " +
      "or after \"Climb:\" (the extension clip climb, Voice view)." },
  { id: "speed", label: "Ground speed", ble: true, help: "GPS speed over ground in km/h." },
  { id: "heading", label: "Heading", ble: true, help: "Course over ground as a compass direction (N, NE, E ...)." },
  { id: "duration", label: "Flight time", ble: true, help: "Time since the takeoff." },
  { id: "time", label: "Time of day", ble: true, help: "Local time (System → Language and time)." },
  { id: "wind", label: "Wind", ble: true,
    help: "\"Wind: 12 km/h, from the west\" - the speed and the direction it comes from, estimated from circling; " +
      "left out before the first estimate. Needs the extension clips wind and nav/from_… (Voice view); without " +
      "them the stock form \"wind direction, west, 12 km/h\"." },
  { id: "glide", label: "Glide ratio", ble: true,
    help: "Distance over ground per height lost, over the time chosen next to the name (20 s unless changed, " +
      "straight flight with a fix). \"8.7\" as one clip with the extension series glide/glide_… (Voice view: " +
      "in tenths from 5 to 13, in halves below and up to 22, whole to 30), else \"8 dot 7\"." },
  { id: "battery", label: "Battery", ble: false, help: "Charge in percent." },
  { id: "tone", label: "Signal tone", ble: false,
    help: "A short signal on the piezo, no voice; choose it next to the name (▶ plays it: on the device when " +
      "connected, else here as an approximation). Put it first to draw attention to what follows." },
  { id: "thermalgain", label: "Thermal: height gained", ble: false,
    help: "Height gained since the circling began, or over the last minute or 30 s of any flight (the choice next " +
      "to the name; a loss is said as a negative number), as the number alone or after \"Gain:\" or " +
      "\"Height gain:\" (the extension clips gain and height_gain, Voice view)." },
  { id: "maxaltitude", label: "Maximum altitude", ble: true, help: "Highest altitude of the flight." },
  { id: "maxspeed", label: "Maximum speed", ble: true, help: "Highest ground speed of the flight." },
  { id: "maxclimb", label: "Maximum climb and sink", ble: true, help: "Strongest climb and sink (5 s means) of the flight." },
  { id: "maxg", label: "Maximum g-load", ble: true, help: "Peak load factor, e.g. \"2 dot 3\" (g)." },
  { id: "bank", label: "Bank angle", ble: true,
    help: "After a wingover the largest measured tilt of the pilot, 0 to 180° (once the device has learned " +
      "how it sits on you, about a minute into the flight); after a spiral the bank of a coordinated turn at " +
      "the peak load (2 g = 60°)." },
];
const BLOCK_BY_ID = Object.fromEntries(BLOCKS.map((b) => [b.id, b]));

/*
 * The signal tones of the block "tone" (firmware 0.57: announce.c announce_signal_names, the notes in sound.c),
 * stored as the option "tone[:ble]:<id>"; the first is the default and is not written. notes: [Hz, ms], 0 Hz =
 * pause, for the preview in the browser.
 */
const SIGNAL_TONES = [
  { id: "tritone_up", label: "Tritone up", notes: [[2637, 90], [0, 25], [3729, 170]] },
  { id: "octave_leap", label: "Octave leap", notes: [[1270, 70], [0, 25], [2540, 170]] },
  { id: "bi_dup_twice", label: "Bi-dup twice", notes: [[3322, 40], [3951, 60], [0, 80], [3322, 40], [3951, 60]] },
  { id: "tick_tock", label: "Tick-tock", notes: [[3800, 25], [0, 110], [3000, 25]] },
  { id: "chirp_up", label: "Chirp up", notes: [[2400, 25], [2700, 25], [3000, 25], [3300, 25], [3650, 25], [4000, 25]] },
  { id: "trill", label: "Trill", notes: [[3322, 35], [3951, 35], [3322, 35], [3951, 35], [3322, 35], [3951, 35]] },
  { id: "low_octave", label: "Octave leap, low", notes: [[566, 70], [0, 25], [1132, 170]] },
  { id: "low_bi_dup_twice", label: "Bi-dup twice, low", notes: [[800, 40], [1200, 60], [0, 80], [800, 40], [1200, 60]] },
  { id: "low_trill", label: "Trill, low", notes: [[566, 40], [800, 40], [566, 40], [800, 40], [566, 40], [800, 40]] },
  { id: "tock_tock", label: "Tock-tock", notes: [[800, 30], [0, 110], [566, 30]] },
  { id: "low_chirp_up", label: "Chirp up, low", notes: [[566, 25], [680, 25], [800, 25], [950, 25], [1131, 25], [1270, 25]] },
];
const SIGNAL_BY_ID = Object.fromEntries(SIGNAL_TONES.map((s) => [s.id, s]));

/*
 * The options a block carries after its name and ":ble" in the items string (firmware 0.58, announce.c
 * parse_options): the span "<n>s" its value covers (announce_span_s) and the word spoken before the number
 * (announce_word_names); for "tone" its signal. Per option: key, label (the select's name), def (the firmware's
 * own default, not written) and choices [[value, label]]; play: ▶ next to it. The firmware ignores words it
 * does not know, and firmware before 0.58 still reads the ":ble".
 */
const BLOCK_OPTIONS = {
  climbrate: [
    { key: "span", label: "Averaged over", def: "10s",
      choices: [["10s", "last 10 s"], ["15s", "last 15 s"], ["20s", "last 20 s"], ["30s", "last 30 s"]] },
    { key: "word", label: "Spoken as", def: "", choices: [["", "number only"], ["climb", "“Climb:” + number"]] },
  ],
  thermalgain: [
    { key: "span", label: "Height gained", def: "",
      choices: [["", "since the thermal began"], ["60s", "in the last minute"], ["30s", "in the last 30 s"]] },
    { key: "word", label: "Spoken as", def: "",
      choices: [["", "number only"], ["gain", "“Gain:” + number"], ["heightgain", "“Height gain:” + number"]] },
  ],
  glide: [
    { key: "span", label: "Averaged over", def: "20s",
      choices: [["15s", "over 15 s"], ["20s", "over 20 s"], ["30s", "over 30 s"], ["60s", "over 60 s"]] },
  ],
  tone: [
    { key: "sound", label: "Which signal tone", def: SIGNAL_TONES[0].id, play: true,
      choices: SIGNAL_TONES.map((s, k) => [s.id, k ? s.label : `${s.label} (default)`]) },
  ],
};

/* The stock matrix items in the stock order, then the 0.16 additions: for the derived lists. */
const MATRIX_ITEMS = ["altitude", "climbrate", "speed", "heading", "duration", "time", "wind", "glide", "battery"];
/* The items of the stock keys voicetap_<item>_bool / voiceauto_<item>_bool (the derived lists). */
const STOCK_MATRIX_ITEMS = MATRIX_ITEMS.slice(0, 6);
const FLIGHT_BLOCKS = [...MATRIX_ITEMS, "tone"];

/*
 * Trigger: enable {key, stock, def} (stock: the key lives in config.ultrabip for profiles 1-3),
 * settings (fields; stock keys without ext, new keys with ext: true; defFrom: default from another
 * key), pool (blocks that make sense here, in the suggested order), legacy (how the list is derived
 * while announce_<id>_items_string is missing).
 */
const TRIGGERS = [
  { id: "summary", label: "Flight summary", when: "at power off after a flight",
    help: "Spoken when you switch the device off after a flight. Then Bluetooth stays on for 60 s so the " +
      "configurator or a phone can fetch the track.",
    enable: { key: "flightsummary_enable_bool", stock: true, def: "true" },
    settings: [],
    pool: ["duration", "maxaltitude", "maxspeed", "maxclimb", "maxg"],
    legacy: { kind: "summary" } },
  { id: "tap", label: "Double tap", when: "when you knock twice on the device",
    help: "Knock twice on the case (System → Controls sets how hard). In flight, and before the takeoff if " +
      "allowed below. A double tap is an explicit request: \"Mute with BLE\" is ignored here.",
    enable: { key: "voicetap_enable_bool", stock: true, def: "true" },
    settings: [
      { key: "voicetap_enablebeforetakeoff_bool", label: "Also before the takeoff", type: "bool", def: "false",
        help: "Double taps also work on the ground before the takeoff (e.g. to hear the altitude at launch)." },
      { key: "voicetap_enablewhensilent_bool", label: "Also when the vario is muted", type: "bool", def: "false",
        help: "Double taps also speak at the flight level \"mute\" (then at the ground volume)." },
    ],
    pool: FLIGHT_BLOCKS,
    legacy: { kind: "matrix", prefix: "voicetap_" } },
  { id: "auto", label: "Time or distance", when: "regularly in flight",
    help: "Spoken after a time or a distance flown since the last announcement of this trigger, " +
      "whichever comes first; at most every 30 s.",
    enable: { key: "voiceauto_enable_bool", stock: true, def: "true" },
    settings: [
      { key: "voiceauto_timetrigger_int", label: "Every", unit: "min (0 = off)", type: "int", min: 0, max: 240, def: "15",
        help: "At the latest after this time." },
      { key: "voiceauto_distancetrigger_int", label: "Or after a distance of", unit: "m (0 = off)", type: "int", min: 0, max: 100000, def: "1000",
        help: "... or once you have flown this far over ground." },
    ],
    pool: FLIGHT_BLOCKS,
    legacy: { kind: "matrix", prefix: "voiceauto_" } },
  { id: "elevation", label: "Height gained or lost", when: "after climbing or sinking a set height",
    help: "Spoken each time you have climbed or sunk this much since the last announcement of this trigger " +
      "(the altitude part of the stock \"automatic\" announcements).",
    enable: { key: "announce_elevation_enable_bool", ext: true, defFrom: "voiceauto_enable_bool", def: "true" },
    settings: [
      { key: "announce_elevation_gain_int", label: "After a gain of", unit: "m (0 = off)", type: "int", min: 0, max: 5000, ext: true,
        defFrom: "voiceauto_altitudetrigger_int", def: "100", help: "Height climbed since the last announcement." },
      { key: "announce_elevation_loss_int", label: "After a loss of", unit: "m (0 = off)", type: "int", min: 0, max: 5000, ext: true,
        defFrom: "voiceauto_altitudetrigger_int", def: "100", help: "Height lost since the last announcement." },
    ],
    pool: FLIGHT_BLOCKS,
    legacy: { kind: "matrix", prefix: "voiceauto_" } },
  { id: "thermalexit", label: "Thermal exit", when: "when a climb ends",
    help: "When a climb phase ends: the 30 s mean drops below 0.2 m/s or the circling stops. Short or weak " +
      "climbs are ignored (settings below).",
    enable: { key: "announce_thermalexit_enable_bool", ext: true, def: "true" },
    settings: [
      { key: "announce_thermalexit_mintime_int", label: "Only after a climb of at least", unit: "s", type: "int", min: 10, max: 600, ext: true, def: "60",
        help: "Shorter climb phases do not count as a thermal." },
      { key: "announce_thermalexit_mingain_int", label: "... and a gain of at least", unit: "m", type: "int", min: 0, max: 2000, ext: true, def: "50",
        help: "Weaker climb phases do not count as a thermal." },
    ],
    pool: ["tone", "thermalgain", "climbrate", "altitude", "wind", "speed", "heading", "duration", "time", "battery"],
    legacy: { kind: "event", item: "thermalexit", def: true, items: ["tone"] } },
  { id: "thermaling", label: "While thermaling", when: "every few seconds while you circle in lift",
    help: "While you circle in a thermal - at least one full turn within 30 s and a mean climb since the " +
      "circling began - at the interval below. Turns without climb stay silent.",
    enable: { key: "announce_thermaling_enable_bool", ext: true, def: "false" },
    settings: [
      { key: "announce_thermalclimb_int", label: "Every", unit: "s", type: "int", min: 5, max: 300, ext: true, def: "20" },
    ],
    pool: ["thermalgain", "climbrate", "altitude", "wind", "duration", "time", "battery", "tone"],
    legacy: { kind: "event", item: "climbrate", def: false, items: ["thermalgain"] } },
  { id: "glide", label: "Straight glide", when: "after flying straight for a while",
    help: "Once you have flown straight (no circling) for the time below. With \"only after a climb\" it " +
      "waits for the glide after a thermal, e.g. to hear the glide ratio towards the next one.",
    enable: { key: "announce_glide_enable_bool", ext: true, def: "true" },
    settings: [
      { key: "announce_glide_straighttime_int", label: "After flying straight for", unit: "s", type: "int", min: 10, max: 600, ext: true, def: "30" },
      { key: "announce_glidemingain_int", label: "Only after a climb of at least", unit: "m (0 = always)", type: "int", min: 0, max: 2000, ext: true, def: "100",
        help: "Only on the glide after a thermal in which you gained at least this much." },
      { key: "announce_glide_repeat_int", label: "Repeat every", unit: "min (0 = once per glide)", type: "int", min: 0, max: 60, ext: true, def: "0",
        help: "On a long glide, again at this interval." },
    ],
    pool: ["glide", "speed", "altitude", "wind", "heading", "climbrate", "duration", "time", "battery", "tone"],
    legacy: { kind: "event", item: "glide", def: true, items: ["glide"] } },
  { id: "wind", label: "New wind estimate", when: "when circling ends with a changed wind",
    help: "When the circling stops with a wind estimate that is new, or changed by more than 5 km/h or 45°.",
    enable: { key: "announce_wind_enable_bool", ext: true, def: "false" },
    settings: [],
    pool: ["wind", "speed", "heading", "altitude", "tone"],
    legacy: { kind: "event", item: "wind", def: false, items: ["wind"] } },
  { id: "wingover", label: "Wingover and spiral", when: "after a wingover or a spiral dive",
    help: "A wingover: turns that change direction - at least half a second faster than 40°/s one way, then " +
      "within 2.5 s the other way, with 1.8 g or more in the swings (a series of swings is one wingover). " +
      "Swings that run straight into a spiral are not spoken; the spiral is. A spiral: at least 1.6 g while " +
      "turning 50°/s or more for 4 s. Spoken 2 s after the load is back below 1.3 g. The bank angle of a " +
      "wingover is measured (the pilot's largest tilt against the vertical, beyond 90° over the top), that of " +
      "a spiral comes from the peak load.",
    enable: { key: "announce_wingover_enable_bool", ext: true, def: "true" },
    settings: [],
    pool: ["maxg", "bank", "tone", "altitude", "duration", "time"],
    legacy: { kind: "event", item: null, def: true, items: ["maxg", "bank"] } },
];

/* Volume levels in config_ext.ultrabip [global] (docs/config-ext-format.md): perceived loudness in percent. */
/* Piezo volume steps: the hardware has no continuous volume. The file keeps percent; the device rounds to the
 * nearest step (firmware sound_piezo_step). */
const PIEZO_STEPS = [
  [0, "Off"], [20, "Very quiet"], [40, "Quiet"], [60, "Medium"], [80, "Loud"], [100, "Very loud"],
];
/* Test values for the charge pump frequency of the step "loud" (firmware: 5 kHz .. 500 kHz). */
const PUMP_FREQS = [5000, 7000, 10000, 15000, 20000, 30000, 50000, 70000, 100000, 150000, 200000, 300000, 500000];

const FLIGHT_LEVELS = [
  { id: "low", label: "Low", buzzer: "40", speaker: "40", clip: "vol_1" },
  { id: "med", label: "Medium", buzzer: "60", speaker: "70", clip: "vol_2" },
  { id: "high", label: "High", buzzer: "100", speaker: "100", clip: "vol_3" },
];
const GROUND_LEVEL = { id: "ground", label: "On the ground", buzzer: "20", speaker: "50", clip: "gps_ok" };
const FLIGHT_LEVEL_FIELD = {
  key: "volume_flightlevel_string", label: "Level at the next takeoff", type: "select", def: "med",
  options: [["low", "Low"], ["med", "Medium"], ["high", "High"], ["mute", "Mute"]],
  help: "The device stores the level you used last in flight here and starts the next flight with it. In " +
    "flight a short press cycles low → medium → high → mute.",
};

/* Recording and flight detection, config_ext.ultrabip [global]. */
const RECORDING_FIELDS = [
  { key: "flight_recordigc_bool", label: "Record as IGC file", type: "bool", def: "true",
    help: "The standard flight log (XContest, WeGlide, DHV-XC ...), signed by the device, in " +
      "0:/tracks_IGC/<year>/. It starts with the takeoff detection, including the 15 s before it." },
  { key: "flight_recordkml_bool", label: "Record as KML file", type: "bool", def: "true",
    help: "The track for Google Earth, in 0:/tracks_KML/<year>/ with the same file name as the IGC file. " +
      "Both boxes ticked: both files are written." },
  { key: "flight_recordrate_int", label: "Enhanced IGC data", type: "select", def: "4",
    options: [["1", "1 per second"], ["2", "2 per second"], ["4", "4 per second"], ["5", "5 per second"]], aliases: { 10: "5" },
    requires: "flight_recordigc_bool",
    help: "Extra data records inside the IGC file (GNSS velocity, barometric altitude, climb rate) for " +
      "analysis tools. The standard IGC position records (B records) stay at one per second, so every IGC " +
      "reader handles the file. About 0.4 MB per hour at 1 per second, 1.2 MB at 4, 1.5 MB at 5 (at most one " +
      "per GNSS fix)." },
  /* takeoff detection (flight.c); the thresholds in the expert view */
  { key: "gps_takeoffdetection_bool", label: "Detect the takeoff", type: "bool", def: "true", profile: true,
    help: "Switch the vario on at the launch. After the first GPS fix it averages the spot where you stand (the " +
      "launch point). The flight starts when you are the distance below away from it, or move at the ground speed " +
      "below for some seconds, or the altitude changes clearly. Poor GPS fixes, jumps and a wandering position never " +
      "count: the GPS ground speed must back the way out. Vario tones, announcements and the track start with the " +
      "takeoff (the track includes the 15 s before it). Off: flight mode from the first GPS fix on - for ground " +
      "handling; then there is no landing detection either." },
  { key: "flight_takeoffdistance_int", label: "Distance from the launch point", unit: "m", type: "int", min: 10, max: 500,
    def: "50", level: "expert", requires: "gps_takeoffdetection_bool",
    help: "Moving this far from the launch point starts the flight. Standing anywhere for a while moves the launch " +
      "point along, so a few steps around the launch never add up; walking this far away in one go counts as a " +
      "takeoff. Before the first takeoff only: after a landing the speed and the altitude decide, so walking away " +
      "from the landing field does not start a new flight." },
  { key: "flight_takeoffspeed_int", label: "Ground speed", unit: "km/h", type: "int", min: 5, max: 50, def: "15",
    level: "expert", requires: "gps_takeoffdetection_bool",
    help: "Moving at least this fast (GPS ground speed, good fixes) for the time below starts the flight - running " +
      "that fast too." },
  { key: "flight_takeoffspeedtime_int", label: "… held for", unit: "s", type: "int", min: 1, max: 30, def: "5",
    level: "expert", requires: "gps_takeoffdetection_bool", help: "Seconds in a row." },
  { key: "flight_takeoffaltitude_int", label: "Altitude change within 30 s", unit: "m", type: "int", min: 5, max: 100, def: "15",
    level: "expert", requires: "gps_takeoffdetection_bool",
    help: "The barometric altitude changed this much within 30 s (climbing straight up in front of the launch in " +
      "strong wind) …" },
  { key: "flight_takeoffclimb_int", label: "Climb or sink rate", unit: "cm/s", type: "int", min: 50, max: 500, def: "150",
    level: "expert", requires: "gps_takeoffdetection_bool", help: "… or the climb or sink rate was at least this …" },
  { key: "flight_takeoffclimbtime_int", label: "… held for", unit: "s", type: "int", min: 1, max: 30, def: "5",
    level: "expert", requires: "gps_takeoffdetection_bool", help: "… for this long in a row." },
  /* landing detection (flight.c): three rules, any of them lands */
  { key: "flight_landingdetect_bool", label: "Detect the landing", type: "bool", def: "true", requires: "gps_takeoffdetection_bool",
    help: "A landing makes the device silent again (ground level) and is marked in the track; the track goes " +
      "on until you switch off, so a top landing does not split it. Any of the three rules below lands. Off: once " +
      "airborne the device stays in flight mode - calm soaring in strong wind can look like a landing." },
  { key: "flight_landingtime_int", label: "Standing still for", unit: "s", type: "int", min: 10, max: 600, def: "60",
    requires: "flight_landingdetect_bool",
    help: "Rule 1: below 3 km/h ground speed and 0.3 m/s climb or sink for this long (needs GPS). Walking away " +
      "after the landing keeps this rule from counting; the other two rules catch that." },
  { key: "flight_landingimpact_bool", label: "After the touchdown impact", type: "bool", def: "true",
    requires: "flight_landingdetect_bool",
    help: "Rule 2: a short shock (at least 4 g, while the load also drops below 1 g in that second - a spiral pulls " +
      "4 g as well, but steadily), then slow and no steady climb or sink for 10 s. The landing is dated at the " +
      "shock. Works without GPS too. On the flights of 2026-10-12 it found both landings 13-14 s after " +
      "touchdown and never fired in the air. Not every landing comes with a shock: rules 1 and 3 remain." },
  { key: "flight_landingimpactg_int", label: "Impact at least", unit: "g", type: "int", min: 3, max: 15, def: "4",
    level: "expert", requires: "flight_landingimpact_bool",
    help: "The peak of the shock. Touchdowns measured 7-8 g, walking and packing up to 6 g, the steepest spiral " +
      "4.2 g (steady, so it never counts)." },
  { key: "flight_landingimpacttime_int", label: "… then calm for", unit: "s", type: "int", min: 3, max: 60, def: "10",
    level: "expert", requires: "flight_landingimpact_bool",
    help: "How long after the shock the ground speed stays below the limit below and the barometer shows no " +
      "steady climb or sink (less than 0.5 m/s over 5 s; in flight it is almost always more)." },
  { key: "flight_landinghandling_bool", label: "From ground handling", type: "bool", def: "true",
    requires: "flight_landingdetect_bool",
    help: "Rule 3: within 30 s three moments of fast rotation (over 250 °/s) with the load dropping below 0.5 g " +
      "- packing up, carrying the device around - and 20 s slow without steady climb or sink. In flight this " +
      "never happened on the recorded flights (spirals and wingovers included). Works without GPS too." },
  { key: "flight_landingspeed_int", label: "Ground speed below", unit: "km/h", type: "int", min: 3, max: 30, def: "10",
    level: "expert", requires: "flight_landingdetect_bool",
    help: "Rules 2 and 3: the ground speed must stay below this (walking is 3-6 km/h). Without a GPS fix only " +
      "the barometer counts." },
  { key: "system_uartconsole_bool", label: "Serial console on the debug pins", type: "bool", def: "false", level: "dev",
    help: "The debug console also on LPUART1 (PC0 RX, PC1 TX, 1,000,000 baud 8N1, " +
      "3.3 V), next to the one over USB. Needs a USB-serial adapter on the test pads inside the case." },
  { key: "system_rawlog_bool", label: "Raw sensor log", type: "bool", def: "false", level: "dev",
    help: "Records every sensor sample to 0:/raw from power on to power off, flight or not (about 20 MB per " +
      "hour, and a little more current). For developing the vario filter with tools/analyzer; off by default." },
];

/* Stock "custom" tone curve: what the device uses when a profile has none. */
const STOCK_TONES = [
  { vz: -500, freq: 62, duration: 450, duty: 50 }, { vz: -350, freq: 75, duration: 450, duty: 100 },
  { vz: -50, freq: 100, duration: 450, duty: 100 }, { vz: 5, freq: 440, duration: 875, duty: 50 },
  { vz: 80, freq: 513, duration: 770, duty: 26 }, { vz: 120, freq: 675, duration: 500, duty: 27 },
  { vz: 300, freq: 845, duration: 370, duty: 30 }, { vz: 800, freq: 1000, duration: 230, duty: 45 },
];

const TONE_POINTS = 8;

if (typeof module !== "undefined") {
  module.exports = { TIME_ZONES, dstTable, dstTableStandardOffset, STOCK_TONES, TONE_POINTS, SIGNAL_TONES, SIGNAL_BY_ID,
    BLOCKS, BLOCK_OPTIONS, TRIGGERS };
}
