# Raw sensor log

While the device is switched on it records every sensor sample into `0:/raw/RAWnnnnn.BIN`, one
file per power-on. These logs are for developing the vario filter offline: a recorded flight can
be replayed with other filter settings (`tools/analyzer`, browser and command line). Switch it off with
`system_rawlog_bool=false` in `[global]` of `config_ext.ultrabip`. It takes about 3.5 kB/s
(13 MB per hour) with the barometer at 32 Hz and the GPS at 1 Hz, about 6.5 kB/s (23 MB per hour)
with the barometer at 100 Hz and the GPS at 10 Hz (logs of earlier firmware; 5 Hz now).

## Records
The file is a sequence of records, little endian:

| Bytes | Content |
|---|---|
| 1 | type |
| 1 | payload length n |
| n | payload |

A record cut off at the end of the file (power loss) is ignored. Up to 2 s of data can be
missing after a power loss; the file is synchronised every 2 s.

### Type 0: header (first record, 84 bytes)
| Offset | Type | Content |
|---|---|---|
| 0 | char[4] | `UBRW` |
| 4 | uint8 | format version (1) |
| 5 | uint8 | barometer rate [Hz] (`system_barorate_int`: 32, 64, 100) |
| 6 | uint16 | IMU rate [Hz] (100) |
| 8 | float32 | accelerometer LSB per g (4096, ±8 g) |
| 12 | float32 | gyro LSB per °/s (16.4, ±2000 °/s) |
| 16 | float32 | DPS310 pressure scale factor kP of the oversampling (253952 at 16×, 7864320 at 8×, 3670016 at 4×) |
| 20 | float32 | DPS310 temperature scale factor kT (7864320 at 8×, 524288 single) |
| 24 | int32[9] | DPS310 coefficients c0, c1, c00, c10, c01, c11, c20, c21, c30 |
| 60 | uint32 | Unix time at the start, 0 if the clock was not set |
| 64 | uint32 | time stamp of the start (same clock as the records) |
| 68 | char[16] | firmware version |

### Type 1: barometer (28 bytes, 32, 64 or 100 Hz: the header's rate)
| Offset | Type | Content |
|---|---|---|
| 0 | uint32 | time stamp [ms] |
| 4 | int32 | raw pressure (24-bit, sign-extended) |
| 8 | int32 | raw temperature of the last temperature measurement |
| 12 | float32 | compensated pressure [Pa] (temperature low-pass filtered, 5 s) |
| 16 | float32 | temperature [°C] |
| 20 | float32 | altitude of the filter in use [m, standard atmosphere] |
| 24 | float32 | climb rate of the filter in use [m/s] |

### Type 2: IMU (16 bytes, 100 Hz)
| Offset | Type | Content |
|---|---|---|
| 0 | uint32 | time stamp [ms] |
| 4 | int16[3] | accelerometer x, y, z (raw, see header) |
| 10 | int16[3] | gyro x, y, z (raw) |

### Type 5: accelerometer calibration (28 bytes)
Right after the header and on every change (configurator calibration). The firmware feeds its
filters with `(raw − offset) × scale`; the raw IMU records stay uncalibrated.

| Offset | Type | Content |
|---|---|---|
| 0 | uint32 | time stamp [ms] |
| 4 | float32[3] | offset x, y, z [m/s²] |
| 16 | float32[3] | scale x, y, z |

### Type 3: GPS line, type 4: event
`uint32` time stamp [ms], then the text without line end. GPS lines are what the module sends
(NMEA sentences and command answers); at 5 Hz the GSA and GSV sentences only of the first epoch
of each second (GGA and RMC of every epoch). Events, as text:
* button presses, the selected profile, the vario test, the volume level
* takeoff and landing
* spiral mode on and off (the fusion's manoeuvre mode, `fusion.c`)
* GNSS diagnostics (0.50): `gps stats` once a minute (sentences, bad checksums, UART bytes, framing and
  overrun errors, skipped epochs, baud rate, cycle, good/visible satellites); `gps quiet` every 2 s
  without a sentence (what the UART received since the last one, battery mV) with the first bytes after
  it as hex; `gps talks again after ... ms`; when the silence watchdog fires `gps silent ... probing`,
  `gps probe: answers at our rate / answers at 115200 baud (it restarted itself) / no answer`, then
  `gps set up again without a power cycle` or `gps power cycle`; `gps sends every ... ms instead of
  200 ms` when the module fell back to its factory 1 Hz
* single taps (jerk and threshold) and double taps
* automatic announcements
* the thermal events (thermal climb, thermal exit, wind, glide ratio)
* low battery
* voice underruns (`voice underrun <clip> late <n> starved <n>`: blocks played again because they
  were filled too late, or silence because the clip data was not read yet)

The analyzer lists them and jumps to them. Its replay holds the barometer off after button and
tap events, as the firmware does.

Time stamps are milliseconds of the device clock, the same for all records.
