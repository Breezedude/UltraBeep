# Configuration files

The device reads two files from the SD card, `config.ultrabip` in the top folder and
`ultrabeep/config_ext.ultrabip` (a folder, because the stock firmware deletes files it does not
know in the top folder). Both use the same INI format:
`[section]` headers, `key=value` lines, comments start with `#` or `;`. Key names follow the
stock scheme `<group>_<name>_<type>` with the types `bool` (`true`/`false`), `int` and `string`.

| File | Written by | Content |
|---|---|---|
| `config.ultrabip` | BipLink, the stock HTML configurator, the UltraBeep configurator | `[profile1]` … `[profile3]` and `[global]` in the stock format. The firmware only reads it. |
| `ultrabeep/config_ext.ultrabip` | the UltraBeep configurator and the firmware | Profiles 4–10 and every setting the stock tools do not know. BipLink and the stock configurator never touch it. |

The firmware keeps every line it does not know when it rewrites `config_ext.ultrabip`
(it only changes single values, e.g. `system_profilelast_int`).

## `[global]` in `config_ext.ultrabip`

| Key | Default | Meaning |
|---|---|---|
| `system_profilelast_int` | 1 | Profile chosen at the last power on (1–10); preselected next time. Written by the firmware. |
| `system_uartconsole_bool` | `false` | The debug console also on the serial pins of the stock firmware: LPUART1, RX PC0, TX PC1, 1,000,000 baud 8N1, 3.3 V. Log and answers go to both consoles, commands come from either. |
| `system_rawlog_bool` | `false` | Raw sensor log in `0:/raw/` while switched on (docs/raw-log-format.md), for developing the filters. |
| `system_batteryannounce_bool` | `true` | Right after the profile selection: "battery" and the charge in 25 % steps. |
| `system_blename_string` | – | The Bluetooth name after `UltraBip🪂` (the BipLink app needs the `UltraBip` prefix), up to 14 bytes to keep the emoji; empty: the first word of the pilot name, as the stock firmware. Read at power on. |
| `system_syncwindow_int` | 60 | Seconds Bluetooth stays on after switching off a flight, for the track download (0–600; the configurator offers 0, 30, 60 and 90). Longer while a phone is connected, at most 15 min; a button press ends it. |
| `system_syncwhilemoving_bool` | `true` | The time above counts only once the device lies still (the IMU keeps running); moving it starts the time again. |
| `biplink_secrets_string` | – | Written by the firmware: the secrets of the BipLink apps and configurators the pilot allowed by a button press, comma-separated, up to 8 characters each (room for about eight; later ones are allowed for their session only). Delete the key to forget them all. |
| `bluetooth_pairannounce_bool` | `true` | When a new phone or browser asks for access the device plays two rising tones and says "Bluetooth, press the button to confirm" (the extension clip `press_button_to_confirm`). `false`: the two tones only (0.46). |
| `voice_generator_*` | – | Configurator only: the Voice view's audio generator settings (`voice_generator_locale_string`, `_voice_string`, `_pitch_int`, `_rate_int`, `_bandpass_bool`, `_bandlow_int`, `_bandhigh_int`), written by "Save audio generator settings" and restored in every browser that opens the configuration, so all clips sound the same. |
| `bluetooth_announce_bool` | `false` | "Bluetooth connected" and "Bluetooth disconnected" are spoken at the link events, on the ground and in flight. The pairing confirmation by button then plays the confirm melody instead of the clip (0.44). |
| `system_batteryannounceonlylow_bool` | `true` | ... only below 60 %. The low-battery warning (3.64 V, 0 %) is a log entry only; below 3.58 V the device still says "battery" and switches off at once (no summary, no Bluetooth window), and below 3.64 V it does not switch on. |
| `system_dstrule_string` | `eu` | Summer time rule of the time zone, applied while `system_timezoneauto_bool` is set: `eu` (last Sunday of March to last Sunday of October, 01:00 UTC), `us` (second Sunday of March to first Sunday of November, 02:00 local), `au` (south-east Australia: first Sunday of October to first Sunday of April), `nz` (last Sunday of September to first Sunday of April), `cl` (Chile: first Sunday of September to first Sunday of April), `none`. Missing: `eu`. |
| `system_timezone_string` | `berlin` | Configurator only. The place chosen in the configurator's time zone list, which sets the offset in every profile, the rule above and the switch table `global_timechanges_longstring` in `config.ultrabip` (0.44); the device reads those three. |
| `system_poweroffmelody_string` | `scale` | The melody while the button is held to switch off, a falling sequence in decrescendo: `scale` (E D C B A), `chromatic` (E D♯ D C♯ C, quick), `sighs` (E–C, D–B, C–A), `triplet` (G E C and a low G). About 1 s each. |
| `flight_recordigc_bool` | `true` | IGC file in `0:/tracks_IGC/<year>`. With `flight_recordkml_bool` both files are written; with neither nothing is recorded. |
| `flight_recordkml_bool` | `true` | KML file in `0:/tracks_KML/<year>`, same name as the IGC file (also without IGC). |
| `flight_recordrate_int` | 4 | IGC data records (VBV) per second, 1–5, from the 5 Hz GNSS fixes (more is read as 5). B records and KML points stay at one per second, the 15 s before the takeoff too. About 0.4 MB per hour at 1 Hz, 1.2 MB at 4 Hz, 1.5 MB at 5 Hz. |
| `flight_landingdetect_bool` | `true` | Landing detection. Off: once airborne the device stays in flight mode until it is switched off (calm soaring in strong wind can look like a landing). |
| `flight_landingtime_int` | 60 | Seconds below 3 km/h ground speed and 0.3 m/s climb rate before a landing counts (10–600). A landing never ends the recording: after a top landing the next takeoff continues the same track. |
| `flight_landingimpact_bool` | `true` | Landing after a touchdown shock: a peak of at least `flight_landingimpactg_int` while the load also drops below 1 g in that second (a spiral's steady load never counts), then for `flight_landingimpacttime_int` seconds ground speed below `flight_landingspeed_int` (when there is a fix) and no steady climb or sink (barometer over 5 s below 0.5 m/s); dated at the shock. Works without GNSS. |
| `flight_landingimpactg_int` | 4 | The shock in g (3–15). |
| `flight_landingimpacttime_int` | 10 | Seconds of calm after the shock (3–60). |
| `flight_landinghandling_bool` | `true` | Landing from ground handling: within 30 s three seconds with a rotation over 250 °/s while the load drops below 0.5 g, and 20 s slow without steady climb or sink. |
| `flight_landingspeed_int` | 10 | Ground speed limit of the impact and handling rules in km/h (3–30). |
| `flight_takeoffspeed_int` | 15 | Takeoff by speed: GNSS ground speed in km/h (5–50) on good fixes, held for `flight_takeoffspeedtime_int` seconds (1–30, default 5). Any one of the takeoff rules starts the flight, with a GPS fix (firmware/inc/flight.h). Configurator: Recording, expert view. |
| `flight_takeoffspeedtime_int` | 5 | See above. |
| `flight_takeoffdistance_int` | 50 | Takeoff from the launch point: distance in m (10–500) from the averaged standing position after switching on (good fixes, below 2 km/h; standing again moves it along). The way out must be backed by the GNSS ground speed and the fixes must be good (HDOP ≤ 3), so GPS jumps and a wandering position never count. Before the first takeoff only: after a landing speed and altitude decide. |
| `flight_takeoffaltitude_int` | 15 | Takeoff by altitude: barometric change in m within 30 s (5–100), or … |
| `flight_takeoffclimb_int` | 150 | … a climb or sink rate in cm/s (50–500) held for `flight_takeoffclimbtime_int` seconds (1–30, default 5). |
| `flight_takeoffclimbtime_int` | 5 | See above. |
| `system_gpsrate_int` | 5 | GNSS fixes per second asked of the module: `5` or `1` (`@GSOP`). `10` is read as `5`: at 10 Hz the module sent about 9 epochs per second but computed only 5 of them, at irregular intervals. |
| `system_gpssystems_string` | `all` | Satellite systems: `all` (the largest set the module accepts: GPS, GLONASS, SBAS, QZSS), `gps+glonass`, `gps`. Fewer systems, less load on the module. |
| `system_barorate_int` | 100 | Barometer samples per second: `100` (4× oversampling, each measurement started by the processor every 10 ms, as the stock firmware), `64` (8×) or `32` (16×). The filters keep their weight per second at every rate. |
| `imu_accoffset_x_int`, `_y_`, `_z_` | 0 | Accelerometer offset per axis in mm/s² (±3000). The firmware uses `(raw − offset) × scale`. Written by the configurator's calibration (`CAL set`, docs/ble-file-service.md). Without any `imu_acc*` key the firmware takes the factory calibration the stock firmware stored in the chip's OTP area (log line "Accelerometer calibration from the factory OTP"). |
| `imu_accscale_x_int`, `_y_`, `_z_` | 1000000 | Accelerometer scale per axis in ppm of 1 (800000–1200000). |
| `imu_axes_string` | – | Configurator only. Which sensor axis points out of which side of the case, found by the calibration wizard, e.g. `solar:+Z,usb:+X,speaker:+Y` (the opposite sides velcro, button and eyelet are the negative axes). Informational for now; the firmware does not read it. |
| `volume_buzzerlow_int` | 40 | Piezo (vario tones, melodies), level **low** (step, see below) |
| `volume_buzzermed_int` | 60 | Piezo, level **med** |
| `volume_buzzerhigh_int` | 100 | Piezo, level **high** |
| `volume_speakerlow_int` | 40 | Speaker (announcements), level **low** |
| `volume_speakermed_int` | 70 | Speaker, level **med** |
| `volume_speakerhigh_int` | 100 | Speaker, level **high** |
| `volume_buzzerground_int` | 20 | Piezo on the **ground**: button feedback and melodies only |
| `volume_speakerground_int` | 50 | Speaker on the **ground**: system messages (GPS ok, profile selection) |
| `volume_flightlevel_string` | `med` | Level in flight (`low`, `med`, `high`, `mute`). Written by the device; used at the next takeoff. |

### Volume levels
* **On the ground** a short press says the launch readiness (0.44): "ready" and "waiting for takeoff"
  (the extension clips `ready` and `wait_for_takeoff`, generated in the Voice view) with a 3D fix,
  "searching GPS" before one, "GPS disabled" when the profile has no GPS.
* **In flight** a short press cycles low → med → high → mute → low. The device remembers the last
  level for the next takeoff.
* **mute** switches vario tones and announcements off; it has no settings.
* **ground** is not selectable: the device uses it automatically from power on until takeoff,
  and it has its own settings. On the ground there are no vario tones and no flight
  announcements.

**Speaker** volumes are perceived loudness in percent: 100 % sounds twice as loud as 50 % (+10 dB
per doubling). 100 % is the loudest level before the stock voice clips start to distort.

**Piezo** volumes are steps, because the hardware has no continuous volume. Only the voltage on the
piezo changes its loudness.

| Value | Step | Drive |
|---|---|---|
| 0 | off | |
| 20 | very quiet | battery voltage, short pulse: the piezo is charged only partly |
| 40 | quiet | battery voltage |
| 60 | medium | one charge pump leg |
| 80 | loud | both legs at 7 kHz (stock) |
| 100 | very loud | both legs at 500 kHz; only a little louder than loud |

Other values round to the nearest step (30 → 40, 10 → 20).

## Profiles

* Profiles 1–3 live in `config.ultrabip` (stock), profiles 4–10 in `config_ext.ultrabip` as
  `[profile4]` … `[profile10]` with the same keys as the stock profiles (e.g. `enabled_bool`,
  `identity_profilename_string`, `vario_*`, `voicetap_*`, `voiceauto_*`, `vario_tones_*`).
  The configurator reorders and deletes profiles (the pencil above the list) by renumbering them
  without gaps: a profile moved into 1–3 goes into the stock file, one moved beyond goes into the
  extension file; positions 1–3 always exist. The custom voice lines
  (`voices/custom/profile_custom_<n>.qoa`) belong to the numbers and are renamed along at the next save.
* Stock keys of the device as a whole stay per profile in the stock format. The configurator shows
  them under "System" and writes a change into every profile:
  * `system_language_string`: the voice pack (profile 1's value counts)
  * `system_timezoneauto_bool`, `system_timeoffsetminutes_int`: local time (the configurator sets both from
    its time zone list, with the summer time rule in `system_dstrule_string` and the switch table
    `global_timechanges_longstring` in the stock file's `[global]` section)
  * `system_tapsensitivity_int`: the double tap
  * `system_autopowerofftime_int`: minutes lying still on the ground before the device
    switches itself off (0 = never; default 30). Never in flight, while Bluetooth is connected or
    on USB power.
  * `led_userled_int` (stock numbering): what the LED shows once the GPS has a fix
    (before that it blinks yellow while searching). `0` off (default). `1` vario colours: green
    with every lift beep, red during the sink tone, following the tone pattern, in flight only.
    `2` vario colours only at the flight level mute. `3` power indicator: on battery a blink
    pattern every 3 s (green 3× ≥ 70 %, 2× ≥ 40 %, 1× above the warning level 3.64 V; red 1×
    below it, red 2× below 3.61 V, shortly before the device switches itself off); on USB power
    green breathing while charging and dim green when charged. `4` power indicator only at the
    flight level mute.
  * stock only: `bluetooth_*`
* Other stock profile keys the firmware uses:
  * `vario_liftysound_bool`: the weak-lift tone, a 40 ms tick every second while the climb rate is
    within 0.4 m/s below the lift threshold ("zero sink"), pitched like the curve at that climb rate. In
    flight only (and in the configurator's vario test).
  * `vario_sweeptone_bool`: the pitch follows the climb rate while a beep sounds. Off: each beep keeps
    the pitch it started with.
  * `vario_integrationtime_int` (0–100 %): the climb rate the tones follow is smoothed with a time
    constant of 0.1 / 0.5 / 1.2 / 2.6 / 5.5 s at 0 / 25 / 50 / 75 / 100 % (linear in between), the
    stock app's 0.3 / 0.7 / 1.4 / 2.8 / 5.7 s less the 0.2 s the fusion itself lags. Missing: 25 %.
  * `gps_takeoffdetection_bool`: off = flight mode (vario tones, announcements, recording) from the
    first GPS fix on, without landing detection - ground handling, or when the detection fails. The
    configurator shows it under Recording and writes it into every profile.
  * `alarm_min2_int`, `alarm_max2_int` (0 = off): altitude alerts in flight, altitudes above sea
    level (so only with the GNSS reference): the error melody and the altitude once when the limit is
    crossed, then every 5 min while beyond it, with 10 m of hysteresis on the way back.
  * `gps_enable_bool`: ignored, the GPS stays on (takeoff detection, track, announcements).
* `identity_profilename_string` selects the spoken profile name from the voice pack
  (`paragliding`, `cross`, `hike`, `bivouac`, `acro`, `paramotor`, `competition`, `speedriding`,
  `tandem`, `alpinism`).
* Custom profile name (`config_ext.ultrabip`, configurator):
  * `identity_customname_string`: an own name instead of the type, e.g. the glider. The
    configurator shows it in the profile list; `identity_profilename_string` keeps a standard
    value for BipLink and as the spoken fallback.
  * `identity_voicetext_string`: the text the voice line speaks (defaults to the name), kept so
    the clip can be made again.
  * `identity_voiceclip_string` (0.47): the voice line's clip in `0:/voices/custom/`, named after
    the profile by the configurator (`profile_kode_p_16`, plain letters, unique among the
    profiles). The key travels with the profile, so the clip stays its own through reorders and
    renames; the configurator proposes a new name when the voice line is made again and removes
    the old file. Without the key the firmware looks for the older `profile_custom_<n>.qoa`.
  * The clip is mono 22.05 kHz QOA, levelled like the stock clips (loudest 200 ms about −13 dBFS
    rms). The configurator's Voice view generates it from text-to-speech (tools/tts-relay,
    Microsoft Edge voices) in the browser and stores it over Bluetooth (`PUT voices/custom/...`).
    The device says "select profile" and this clip; without the clip, the type as before.
  * Lookup (every clip, since 0.43.1): the single file `0:/voices/custom/<clip>.qoa` first, then
    `0:/voices/custom.ubpack` when it exists (an extension pack with the same layout as the main
    pack: `voices/<clip>.qoa`, `al/al_<m>.qoa` ...), then the main pack. So a clip generated in
    the configurator replaces the pack's clip of that name, user-made clips
    can be packed later without a firmware change, and a clip the main pack lacks can be added as a file.
  * **Extension clips** - the clips UltraBeep says that the manufacturer's packs do not have (the
    configurator's Voice view lists them in their own table and generates and sends them as a group): `ready` and
    `wait_for_takeoff` (the launch readiness below), `press_button_to_confirm` (the pairing request),
    `glide_ratio`, `g_load` and `bank_angle` (the announcement blocks), `thermal_exit`, the load factor
    series `g/g_0` … `g/g_100` (0.0 to 10.0 g in tenths, one clip each: "2,3 G", since 0.52), the bank
    angle series `bank/bank_0` … `bank/bank_180` (steps of 5 degrees: "60 Grad", since 0.53), and the
    profiles' voice lines (`identity_voiceclip_string`). Everything else the firmware says is in the stock packs.

## If found

`ultrabeep/if_found.txt` (0.48) is the owner's note for whoever finds the device, typed in the
configurator under System → If found (free text, up to 2000 characters, English reaches most people).
At every start, and after the configurator sent a new note, the firmware copies it to
`0:/IF_FOUND_PLEASE_READ.txt` in the top folder of the SD card, where a finder who connects the device
over USB sees it first; a blank note removes that copy. The copy lives in the top folder, so the
original firmware deletes it with the other files it does not know - the master in the folder
survives, and the copy comes back with the next start of UltraBeep.

## Announcements

Announcements are **triggers**, each with its own ordered list of **blocks** (the configurator's
profile view). The lists live per profile in `config_ext.ultrabip` (`[profile1]` …
`[profile10]`):

| Key | Meaning |
|---|---|
| `announce_<trigger>_items_string` | The blocks in the order spoken, comma separated; a block may carry the option `:ble` (not spoken while a phone is connected over Bluetooth). Example: `tone,thermalclimb,altitude:ble`. An empty value means nothing. |
| *(missing key)* | The list is derived from the stock keys BipLink writes (below), or the trigger's default. |

Triggers, their enable key and settings. Keys without a prefix note are extension keys; the stock
keys stay where they are (profiles 1–3 in `config.ultrabip`), so BipLink changes them too.

| Trigger | Fires | Enable | Settings |
|---|---|---|---|
| `summary` | at power off after a flight | `flightsummary_enable_bool` (stock) | – |
| `tap` | double tap | `voicetap_enable_bool` (stock) | `voicetap_enablebeforetakeoff_bool`, `voicetap_enablewhensilent_bool` (stock) |
| `auto` | regularly in flight: after `voiceauto_timetrigger_int` min or `voiceauto_distancetrigger_int` m (stock), at most every 30 s | `voiceauto_enable_bool` (stock) | the two stock keys |
| `elevation` | after `announce_elevation_gain_int` m climbed or `announce_elevation_loss_int` m lost since the last one (0 = off; default: `voiceauto_altitudetrigger_int`) | `announce_elevation_enable_bool` (default: `voiceauto_enable_bool`) | the two keys |
| `thermalexit` | a climb phase ends (30 s mean below 0.2 m/s or circling stops) after at least `announce_thermalexit_mintime_int` s (60) and `announce_thermalexit_mingain_int` m (50) | `announce_thermalexit_enable_bool` (true) | the two keys |
| `thermaling` | every `announce_thermalclimb_int` s (20) while circling with a mean climb (turns without climb stay silent) | `announce_thermaling_enable_bool` (false) | the key |
| `glide` | after `announce_glide_straighttime_int` s (30) of straight flight; with `announce_glidemingain_int` m (100, 0 = always) only on the glide after a thermal of that gain; `announce_glide_repeat_int` min (0 = once per glide) | `announce_glide_enable_bool` (true) | the three keys |
| `wind` | circling ends with a wind estimate that is new or changed by 5 km/h or 45° | `announce_wind_enable_bool` (false) | – |
| `wingover` | after a wingover (turns that change direction: at least 0.5 s faster than 40°/s, the other way within 2.5 s, 1.8 g or more in the swings; swings that run into a spiral are logged, not spoken) or a spiral (1.6 g and 50°/s for 4 s), 2 s after the load is back below 1.3 g (firmware/inc/maneuver.h, 0.54) | `announce_wingover_enable_bool` (true) | – |

Blocks: `altitude`, `climbrate` (10 s mean), `speed`, `heading`, `duration` (flight time), `time`
(local time of day), `wind`, `glide` (glide ratio), `battery`, `tone` (the falling three-tone
signal, no voice), `thermalclimb` (mean climb since the circling began), `thermalgain` (height
gained since then), `maxg` (peak load factor of the flight or the manoeuvre: the one clip `g/g_23` when
the device has that series, else "2 dot 3" from single numbers), `bank`
(bank angle of a coordinated turn at that load, degrees, a size without a side, below 90: the
one clip `bank/bank_60` in steps of 5 when the device has that series, else the single number), and
for the summary `maxaltitude`, `maxspeed`, `maxclimb` (max climb, a short pause, then the max sink as a negative number).
Blocks without data (no fix, no wind estimate yet, flight time before the takeoff) are left out.

The derived list while `announce_<trigger>_items_string` is missing:
* `summary`: `duration`, `maxaltitude`, `maxspeed`, `maxclimb` for the stock keys
  `flightsummary_duration/altitude/speed/climbrate_bool` that are true.
* `tap`, `auto`, `elevation`: the stock items in the stock order (`altitude`, `climbrate`,
  `speed`, `heading`, `duration`, `time`) whose stock key `voicetap_<item>_bool` /
  `voiceauto_<item>_bool` is true; every block muted with a phone connected except `climbrate`.
* `thermalexit`: `tone` (on); `thermaling`: `thermalclimb` (off); `glide`: `glide` (on); `wind`:
  `wind` (off); `wingover`: `maxg`, `bank` (on).

Details of the triggers (firmware/src/app/announce.c):
* Automatic triggers stay silent at the flight level mute and leave out the blocks marked `:ble`
  while a phone is connected; the double tap is a request and ignores both; the summary leaves out
  `:ble` blocks when a phone is connected at power off.
* `tone` plays the falling three-tone signal on the piezo at its place in the list.
* `auto` and `elevation` fire at most every 30 s each; `elevation` measures from the altitude of
  its last announcement (the takeoff at first).
* `thermalexit` speaks the climb phase that ended (`thermalclimb`, `thermalgain` of that phase);
  `thermaling` the phase so far.
* `glide` reports after `announce_glide_straighttime_int` s of straight flight, also right after
  the takeoff when `announce_glidemingain_int` is 0; the glide ratio itself needs 20 s of straight
  flight with a fix and at least 0.3 m/s sink, else that block is left out and the others are said.
* `wingover` speaks 2 s after the manoeuvre ended; `maxg` and `bank` are its values, in the
  summary `maxg` is the peak of the flight. Extension clips `g_load` and `bank_angle` (like
  `glide_ratio`) are said first when the voice pack has them. `maxg` takes the series `g/g_<tenths>`
  when `g/g_10` and `g/g_100` are there (above 10 g: `g/g_100`; the accelerometer measures to 8 g),
  `bank` the series `bank/bank_<deg>` when `bank/bank_5` and `bank/bank_180` are there (the clips
  above 90 are for a measured roll angle; the load of a coordinated turn gives less than 90).
  The device looks the extension clips up again when the voice pack changes or a clip in
  `voices/custom` is sent or deleted over Bluetooth (0.52).
* A list holds up to 10 blocks, each once.

### Details

* **Double tap:** in flight, or before the takeoff if `voicetap_enablebeforetakeoff_bool` is set.
  At the flight level mute only with `voicetap_enablewhensilent_bool`, then at the ground volume.
  `system_tapsensitivity_int` is the sensitivity as in the stock configurator: 0 = extremely easy
  to trigger, 1 very easy, 2 easy, 3 balanced (default), 4 difficult.
* **Circling** means at least one full turn of the ground track (or of the gyro's turn rate)
  within 30 s; it ends below 60° in 10 s.
* **Wind** comes from a circle fit of the ground velocity over the last turn. It is accepted for an
  airspeed of 20–60 km/h. Spoken as "wind", the direction it comes from, and the speed.
* **Glide ratio** is spoken as "8 dot 7"; with an extension clip `glide_ratio` in the voice pack,
  that word comes first.
* **Time of day** (also the IGC header's time zone and the file dates): with `system_timezoneauto_bool`
  the offset in force by the stock's switch table `global_timechanges_longstring` in `config.ultrabip`
  ("yymmdd offset," entries, written by the BipLink app for the phone's time zone and by the configurator
  for the chosen zone, taking effect at 01:00 UTC of their day) when the table covers the date (from its
  first entry to a year after its last); otherwise UTC + `system_timeoffsetminutes_int` plus one hour
  while the summer time of `system_dstrule_string` is in force. Without the automatic: the offset as it
  is. The app stores the offset in force (120 in a European summer) in the profiles, so the table has to
  decide there; before 0.44 the summer hour was added on top (file dates and the spoken time one hour
  ahead after a configuration from the app).

Further per-profile keys in `config_ext.ultrabip`:

| Key | Default | Meaning |
|---|---|---|
| `announce_mingap_int` | 15 | Pause in seconds that automatic announcements keep after the last one ended, a double tap's included (0 = none). A time, distance or elevation announcement that falls due meanwhile comes right after; a thermal, wind, glide or manoeuvre announcement within it is left out. A double tap also restarts the time/distance and elevation triggers (0.54). |
| `announce_thermalclimb_int` | 20 | Seconds between climb announcements while circling |
| `announce_glidemingain_int` | 100 | Glide ratio only after a climb of at least this many metres |
