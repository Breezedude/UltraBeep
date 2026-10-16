# Bluetooth file service

Used by the configurator to read and write the configuration files and to download the tracks
over Bluetooth Low Energy. The device advertises as `UltraBip🪂<pilot>` (the stock firmware's name) while it is switched on, and
after a flight for 60 s after switching it off (longer while a phone stays connected, at most
15 min; a button press ends it). Advertising runs every 100–200 ms for the first 30 s after the
start or a disconnect, then every 0.8–1 s to save current; a scanner still finds the
device within a few seconds. The device takes two connections at once (0.45): the phone with the
BipLink app or a flight app, and the configurator. While one central is connected it keeps
advertising at the slow interval on the ground (switched on and not in flight, in the profile
selection, in the sync window after switching off), never in flight and never with two links. Each
service belongs to the link that uses it; the file service and the console serve one configurator at
a time.

| Characteristic | UUID | Properties | Content |
|---|---|---|---|
| Service | `4a800001-c6ce-4362-96ab-eb24df2be84a` | | |
| Command | `4a800002-c6ce-4362-96ab-eb24df2be84a` | write | one UTF-8 command line, at most 244 bytes |
| Data | `4a800003-c6ce-4362-96ab-eb24df2be84a` | write, write without response, notify | file content in chunks of at most ATT MTU − 3 bytes |
| Status | `4a800004-c6ce-4362-96ab-eb24df2be84a` | notify | UTF-8 reply lines ending with `\n`; a long line comes in several notifications |

The client enables notifications on Data and Status before the first command and sends the next
command only after the reply to the previous one: replies carry no request id.
CRC = CRC-32 (zlib/PNG polynomial, reflected), written as 8 hex digits.

**Authorisation (0.45):** apart from `INFO` and `AUTH`, the device takes commands only on a link that
sent `AUTH <secret>` with a secret it knows, so nobody nearby can read or change the settings. The
secret is up to 8 characters `[a-z0-9_]` the client made up once and keeps (the configurator: per
browser, in its local storage). A new secret is answered with `PENDING`: the device plays two rising
tones, says "Bluetooth, press the button to confirm" (`bluetooth_pairannounce_bool`) and pulses blue, the
configurator shows a prompt with a picture of the device, and a short button press within 60 s accepts
the secret (`AUTH ok` on Status; the
secret is stored in `biplink_secrets_string` of config_ext.ultrabip next to the BipLink app's
secrets, for this session only when that list is full), otherwise `AUTH denied`. A known secret gets
`OK` at once. `ERR busy`: another configurator is connected; `ERR not authorised`: a command before
the AUTH. Firmware before 0.45 answers `ERR unknown command` and needs no authorisation.

## Commands
| Command | Replies (Status) | Notes |
|---|---|---|
| `INFO` | `INFO name=<firmware> version=<v> profile=<n> state=<state> mtu=<mtu> interval=<ms> phy=<1\|2> links=<n> auth=<0\|1>` | `interval` is the connection interval, `phy` the radio bit rate in Mbit/s, `links` the connected centrals, `auth` whether this link is authorised |
| `AUTH <secret>` | `OK`; `PENDING`, later `AUTH ok` or `AUTH denied`; `ERR busy`; `ERR secret` | see Authorisation above |
| `RELOAD` | `OK`; `ERR in flight`; `ERR not switched on` | the device takes the saved configuration at once, as after an upload from the BipLink app (0.49): it says "loading configuration"; sensors, GNSS and the voice pack restart only when their settings changed. The Bluetooth name, the remote control and the raw log follow at the next power on. Only on the ground |
| `GET <file>` | `OK <size>`, then the file on Data, then `DONE <crc>`; or `ERR <reason>` | |
| `LIST <folder>` | like `GET`: the listing, one line `<name> <size>` per file | `tracks_IGC` or `tracks_KML`, with the files of the year folders as `2026/<name>`; an empty listing if the folder does not exist yet |
| `GET <folder>/<file>` | like `GET` | a track file of any size, read from the card while it is sent |
| `PUT <file> <size> <crc> [ack]` | `READY` (with `ack`: `READY ack=<step> window=<bytes>`, then `ACK <received>` every `<step>` bytes); after `<size>` bytes on Data: `DONE <crc>` or `ERR <reason>` | the file is replaced only if the CRC matches; `ack`: see below |

Allowed files: `config.ultrabip`, `config_ext.ultrabip` (top folder of the SD card, at most 16 KiB;
new settings take effect at the next power on), `if_found.txt` (the owner's note, stored as
`ultrabeep/if_found.txt`; the device copies it to `IF_FOUND_PLEASE_READ.txt` in the top folder, or removes
that copy when the note is blank) and, for `GET` only, the files in `tracks_IGC/` and
`tracks_KML/` and their year folders, e.g. `tracks_IGC/2026/2026-10-08-XSD-UB1A2B-01.igc` (no other sub
folders, no `..`).

## Fast uploads

A write with response takes a round trip per chunk, so an upload moved one chunk per connection
interval (about 8 kB/s). The Data characteristic also takes writes without response,
and `PUT … ack` turns on the pacing for them:

* the device answers `READY ack=1024 window=4096` and then sends `ACK <received>` on Status each
  time 1024 more bytes have arrived (not for the last bytes: `DONE` follows them);
* the client writes the chunks without response and keeps at most `window` bytes beyond the
  last acknowledged position in flight; if no ACK arrives for 10 s it gives up (the device drops
  an unfinished upload after 10 s without data);
* a client that does not send `ack` gets a plain `READY` and no ACK lines, as before.

A firmware image is collected in RAM and written to the card in pieces of 4 kB (whole sectors,
one multi-block command each). After connecting the device also asks the central for a 7.5–15 ms
connection interval, 251-byte packets and the 2 Mbit/s PHY; what the central grants shows in
`INFO` and in the device's log.

## Console

Used by the configurator's console view (developer mode): the debug console of the USB serial
port, over Bluetooth.

| Command | Replies (Status) | Notes |
|---|---|---|
| `CONSOLE on` | `OK` | from now on the console output comes on Status as well: first the greeting and the log since switching on, then every new log line and the answers to commands |
| `CONSOLE off` | `OK` | ends it; so does a disconnect |
| `CON <line>` | none | a command line as typed on the USB console (`help` lists the commands); its echo and its answer come as console output |

Console output on Status:
* `CON <text>`: text that ends a line. `CONP <text>`: text whose line end has not come yet; the
  next `CON` or `CONP` continues the line. Leading spaces belong to the text, `\r` is dropped.
* These lines are never the reply to a command: the client sets them aside before it matches
  replies.
* The device buffers 2 KiB. When Bluetooth cannot keep up, log lines of other tasks are dropped and
  a line `CON (<n> console lines dropped: Bluetooth too slow)` says so; the answers to console
  commands wait for room instead and arrive complete.

## Live values and tests

Used by the configurator's "Status and tests" view and by the test buttons of the tone curve editor.

| Command | Reply | Effect |
|---|---|---|
| `LIVE` | `LIVE st=… p=… t=… alt=… vz=… acc=… rest=… fused=… fix=… sats=… hdop=… gs=… lat=… lon=… vbat=… rec=… piezo=… quiet=… pump=… taps=… tapg=… tsens=…` | one snapshot, see below |
| `TONE <Hz> <ms> <vol>` | `OK` | a piezo tone; volume as the piezo settings: 0, 20 … 100 = steps (docs/config-ext-format.md) |
| `SPK <Hz> <ms> <vol>` | `OK` | the same tone on the speaker; volume 0–100 % perceived loudness |
| `SAY <clip> <vol>` | `OK` | plays a clip of the voice pack, e.g. `vol_1` |
| `TONES <lift> <sink> <alarm> <vz>,<f>,<p>,<d> ×8 [<weak> [<sweep> [<integration>]]]` | `OK` | sets the vario tone curve for the test (see below); it replaces the profile's curve until `TEST off` |
| `VARIO <vol>` | `OK` | vario tones on the ground at this piezo step; 0 = off |
| `SIM <cm/s>` / `SIM off` | `OK` | the vario plays this climb rate instead of the measured one (−1000…1000) |
| `PIEZO classic` / `PIEZO pwm` | `OK` | piezo drive, see below; stays until the device is switched off, `TEST off` keeps it |
| `PIEZO pulse <ns>` | `OK` | pulse of the piezo step "very quiet" (200–16000 ns); stays until the device is switched off |
| `PIEZO pump <Hz>` | `OK` | charge pump frequency of the piezo step "loud" (5000–500000 Hz); stays until the device is switched off |
| `TAPSENS <0..4>` | `OK` | double-tap sensitivity (`system_tapsensitivity_int`); stays until the device is switched off |
| `MELODY <scale\|chromatic\|sighs\|triplet> <vol>` | `OK` | plays one of the power-off melodies on the piezo |
| `ANNOUNCE <trigger> <blocks>` | `OK` | speaks a block list (`altitude:ble,climbrate,...`, the format of `announce_<trigger>_items_string`) with fixed demo values at the ground volume: the configurator's play button next to a trigger. `summary` speaks it as the flight summary. |
| `PUT voices/custom/[<folder>/]<name>.qoa <size> <crc32>` | `READY` … `DONE <crc32>` | a user-made voice clip (mono 22.05 kHz QOA, at most 24 kB); `<name>` is `[a-z0-9_]`, up to 24 characters, `<folder>` (up to 8) the clip's folder in the pack (`al`, `sp`, `nav`, `time`, `g`, `bank`); folders are created. |
| `DEL voices/custom/[<folder>/]<name>.qoa` | `OK` | removes such a clip |
| `LIST voices/custom` | `OK <size>` … `DONE <crc32>` | the clips on the card, one `[<folder>/]<name>.qoa <size>` per line |
| `PUT update <size> <crc32>` | `READY` … `DONE <crc32>` | a firmware image (`.update`, up to 400 kB), streamed to `0:/ultrabip_ultrabeep.update`; not in flight |
| `UPDATE` | `OK` | installs `0:/ultrabip*.update`: the device stops the GPS, closes the track, resets into the bootloader and disconnects; afterwards it says the result and switches off. Refused in flight and below 3.64 V (the device would not switch on afterwards) |
| `CAL` | `CAL n=… still=… ax=… ay=… az=… ox=… oy=… oz=… sx=… sy=… sz=…` | accelerometer calibration, see below |
| `CAL set <ox> <oy> <oz> <sx> <sy> <sz>` | `OK` | offsets in mm/s², scales in ppm of 1: applied at once and saved in `config_ext.ultrabip` |
| `CAL reset` | `OK` | no calibration (offsets 0, scales 1), saved |
| `TEST off` | `OK` | ends all tests: the profile's curve again, no vario on the ground, no simulation |

`TONES` arguments:
* the lift threshold in cm/s
* the sink alarm threshold in cm/s
* the sink alarm switch: 0 = off, 1 = on
* then eight points, each as climb rate in cm/s, frequency in Hz, period in ms and tone ratio in %
* optionally, after the points: the weak-lift tone switch (0/1), the sweep tone switch (0/1) and the
  integration time in % - the test then uses them as sent (the configurator sends what is edited, saved or
  not); left out, the profile's values stay (0.46.1; before, the defaults applied, which switched the
  weak-lift tone on for the test)

A test only runs while the device is switched on (or in the profile selection) and not in flight.
Otherwise the device refuses it with `ERR in flight` or `ERR not switched on`. A running test ends:
* at the takeoff
* when the phone disconnects

Tests change nothing on the SD card.

Accelerometer calibration (`CAL`):
* `ax`, `ay`, `az`: the raw (uncalibrated) mean of the last second in m/s². `n` counts the seconds,
  `still` is 1 when the device lay still in that second.
* `ox`…`sz`: the calibration in use.
* The configurator asks for the six sides of the case one after the other (solar cell, velcro,
  USB socket, button, speaker, eyelet up), takes two still seconds each from whichever sensor
  axis points up (a side already taken is rejected) and fits offset and scale per axis so that
  |(a − offset) × scale| = g on every side. It then sends `CAL set`, and notes which sensor
  axis belongs to which side (`imu_axes_string` in config_ext.ultrabip).
* Like the tests, `CAL` only works while the device is switched on and not in flight.

Piezo drive:
* `classic`: as the stock firmware. The bridge is off between the pulses, so the volume changes only
  where the charge pump switches, at about 52 % and 78 %.
* `pwm`: experimental. The piezo is shorted between the pulses, so the pulse width sets the volume
  in between too. The bridge keeps 20 µs between switching off one transistor of a leg and
  switching on the other.

`LIVE` fields:

| Field | Meaning |
|---|---|
| `st` | state name |
| `p` | pressure in Pa |
| `t` | temperature in °C |
| `alt` | barometric altitude in m |
| `vz` | climb rate in m/s; the vario plays this value |
| `acc` | acceleration in m/s² |
| `rest` | 1 while the device lies still |
| `fused` | 1 while the IMU fusion is active; 0 = barometer only |
| `fix` | 1 with a 3D fix |
| `sats` | number of satellites |
| `hdop` | horizontal dilution of precision |
| `gs` | ground speed in km/h |
| `lat`, `lon` | position in degrees |
| `vbat` | battery voltage in mV |
| `rec` | 1 while recording |
| `piezo` | piezo drive: `classic` or `pwm` |
| `quiet` | pulse of the piezo step "very quiet" in ns |
| `pump` | charge pump frequency of the piezo step "loud" in Hz |
| `taps` | double taps accepted since the device was switched on |
| `tapg` | jerk of the last single tap in g (0 = none yet) |
| `tsens` | double-tap sensitivity in use (0..4) |
| `baro`, `imu` | `1` the sensor answers, `0` it does not, `-` the sensors are not running |
| `gpsmod` | `1` the GNSS module sent a sentence within the last 5 s, `0` it is silent, `-` it is off; `gpsr` counts the restarts of a silent module |
| `sd`, `pack` | SD card mounted, voice pack open (`1`/`0`) |
