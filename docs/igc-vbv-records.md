# VBV data records in IGC files

IGC files that carry, next to the standard `B` fixes, binary **data records**:

* **Higher time resolution:** a sample at any interval, also several per second. A `B` record
  carries its time as `HHMMSS`, so standard IGC resolves whole seconds at best (the optional
  `TDS` extension adds tenths, but few programs read it).
* **Finer coordinates:** float32 degrees. At mid latitudes the step is about 0.4 m, against
  0.001 arc minute (1.85 m) in a `B` record.
* **More quantities:** each sample also carries GNSS velocities, barometric altitude, vario and
  acceleration.

The data records are `L` (comment) records. Every IGC reader therefore still sees a valid
track made of the `B` records and skips the data records. A reader that knows this format
decodes them as well.

This document was reconstructed from a recorded flight and checked against it: every data record
of the reference file decodes and re-encodes byte for byte. `tools/vbv_igc.py` is a decoder and
checker. Points that the reference file cannot settle are marked as **open**.

## 1. Lines of a file

| Line | Example | Content |
|---|---|---|
| A record | `AVBV00000` | Manufacturer code `VBV`, logger id |
| H records | `HFDTEDATE:240721` | Standard IGC headers |
| Takeoff marker | `LVBVTAKEOFF100549` | `LVBV` + `TAKEOFF` + UTC `hhmmss` |
| **Data record** | `LVBVSO4AAKQMOkLm…DAE=` | `LVBV` + Base64 of one 50-byte sample (§2) |
| B record | `B1005514727326N01317474EA0152401617` | Standard fix, repeats the sample before it (§4) |
| Landing marker | `LVBVLANDING101303` | `LVBV` + `LANDING` + UTC `hhmmss` |
| Wind marker | `LVBVWIND103015 12.3 270` | `LVBV` + `WIND` + UTC `hhmmss`, the wind speed in km/h and the direction it comes from in degrees: written whenever the thermal detector has a new estimate (after circling) |
| G record | `GVBV…` | `GVBV` + Base64 of 32 bytes (§6) |

Order in a file:
1. A and H records.
2. The takeoff marker.
3. From then on, data records with a `B` record after every 4th data record.
4. The landing marker after the sample of the landing. Recording continues after the landing.
5. The G record as the last line.

Lines end with CR LF.

A data record is always 72 characters long: `LVBV` plus 68 Base64 characters with one `=`. A marker
is shorter and is not valid Base64 for 50 bytes. That is enough to tell the two apart.

## 2. Data record

`LVBV` followed by the 50 bytes of one sample, Base64-encoded with the standard alphabet
(RFC 4648: `A–Z a–z 0–9 + /`) and `=` padding. All values are **little endian**; `float32` is
IEEE 754 single precision.

| Offset | Type | Unit | Field | Meaning |
|---|---|---|---|---|
| 0 | uint32 | ms | `t` | Time of the sample on the recorder clock (§3) |
| 4 | float32 | degree | `lat` | Latitude WGS84, north positive |
| 8 | float32 | degree | `lon` | Longitude WGS84, east positive |
| 12 | float32 | m | `h_ell` | GNSS height above the WGS84 **ellipsoid** |
| 16 | float32 | m/s | `vel_n` | GNSS velocity north |
| 20 | float32 | m/s | `vel_e` | GNSS velocity east |
| 24 | float32 | m/s | `vel_d` | GNSS velocity **down** (sinking is positive) |
| 28 | float32 | m | `baro_alt` | Barometric altitude, unfiltered, referenced to the GNSS altitude (QNH-like, §4) |
| 32 | float32 | m | `h_msl` | GNSS height above **mean sea level** (geoid) |
| 36 | float32 | m/s | `vario` | Filtered climb rate (climbing is positive) |
| 40 | float32 | m | `alt` | Filtered altitude (barometric, fused) |
| 44 | float32 | m/s² | `accel` | Magnitude of the measured acceleration (9.81 at rest) |
| 48 | uint8 | – | `sats` | Satellites used in the GNSS solution |
| 49 | uint8 | – | `status` | Status byte, `1` in every sample of the reference file (**open**: meaning; probably "GNSS fix valid") |

`h_ell − h_msl` is the geoid separation of the GNSS receiver at that position, about 44.5 m in
the Alps.

### Example
The sample

| t | lat | lon | h_ell | vel_n | vel_e | vel_d | baro_alt | h_msl | vario | alt | accel | sats | status |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 61000 | 46.512345 | 11.345678 | 2104.512 | 1.234 | −8.765 | 1.321 | 2051.73 | 2056.3 | −1.35 | 2051.9 | 9.95 | 12 | 1 |

is stored as these bytes:

```
48 ee 00 00 a4 0c 3a 42 e6 87 35 41 31 88 03 45 b6 f3 9d 3f 71 3d 0c c1
87 16 a9 3f ae 3b 00 45 cd 84 00 45 cd cc ac bf 66 3e 00 45 33 33 1f 41 0c 01
```

and is written as:

```
LVBVSO4AAKQMOkLmhzVBMYgDRbbznT9xPQzBhxapP647AEXNhABFzcysv2Y+AEUzMx9BDAE=
```

Decoding in Python:

```python
import base64, struct
t, lat, lon, h_ell, vn, ve, vd, baro, h_msl, vario, alt, accel, sats, status = \
    struct.unpack("<I11fBB", base64.b64decode(line[4:]))
```

## 3. Time
`t` counts milliseconds on the **recorder clock**, which is not synchronised to GNSS time.

* **Interval:** samples are written every 1000 ms of recorder time.
* **First sample:** it has `t = 1000`. Its UTC time is the time of the takeoff marker.
* **Clock error:** the recorder clock runs freely. In the reference file it was 1.3 % slow:
  900 s of `t` covered 912 s of GNSS time. Every ~76 s two consecutive `B` records are therefore
  5 s apart instead of 4 s.

**Getting the UTC time of a sample:**
1. Every `B` record repeats the sample written directly before it (§4) and carries that fix's UTC
   time. Take these as anchors.
2. Between two anchors, interpolate linearly in `t`.
3. Before the first anchor, count from the takeoff marker: `UTC = takeoff + (t − 1000) ms`.

The UTC date comes from `HFDTEDATE`. A flight across midnight UTC continues in the next day.

## 4. B records
A `B` record is derived from the data record directly before it, using the standard IGC rules:

| B field | Source |
|---|---|
| time | `HHMMSS`: UTC time of the GNSS fix of that sample in whole seconds (hours, minutes, seconds) |
| latitude, longitude | `lat`, `lon` cut (not rounded) to 0.001 arc minute |
| validity | `A` |
| pressure altitude | `baro_alt` converted to the standard atmosphere (1013.25 hPa), whole metres |
| GNSS altitude | `h_ell` cut to whole metres |

`baro_alt` follows the GNSS altitude (a QNH-like reference): at takeoff it is within about
1.5 m of `h_msl`. The `B` pressure altitude uses the standard atmosphere, as IGC requires. In the reference file the
two differ by a constant 47 m (± 0.4 m).

## 5. Resolution
float32 has a 24-bit mantissa, so the step of `lat` / `lon` depends on the value:

| abs(degree) | Step | Latitude step | Longitude step at 47° |
|---|---|---|---|
| 8 – 16 | 9.5 × 10⁻⁷° | 0.11 m | 0.07 m |
| 16 – 32 | 1.9 × 10⁻⁶° | 0.21 m | 0.14 m |
| 32 – 64 | 3.8 × 10⁻⁶° | 0.42 m | 0.29 m |
| 64 – 128 | 7.6 × 10⁻⁶° | 0.85 m | 0.58 m |
| 128 – 180 | 1.5 × 10⁻⁵° | 1.70 m | 1.16 m |
| `B` record (0.001') | 1.7 × 10⁻⁵° | 1.85 m | 1.26 m |

The rounding error is at most half a step. In the reference file the velocities and both GNSS
heights lie on a 1 mm(/s) grid: the receiver (u-blox, per the header) reports millimetres, as its
NAV-PVT message does.

## 6. G record
`GVBV` + Base64 of 32 bytes, the size of a SHA-256 value. It is a keyed integrity value: the
recorder computes it with a **secret key**, and only the recorder's validation program, which
holds the key, can check it. This is the usual IGC scheme, where the validating body runs the
validation program of each logger maker. It is not a plain SHA-256, SHA3-256 or BLAKE2s hash of
the file or of parts of it. The exact construction (most likely HMAC-SHA-256 over the lines before
the G record) is not part of this document. Readers that do not know the key treat the file as not
validated, and all data stays readable.

### G record of UltraBeep files
* `G` + manufacturer code + Base64 of **HMAC-SHA-256** over every byte of the file before the G
  record (from the first byte of the A record up to and including the CR LF of the line before the
  G record).
* The key is a 32-byte secret of whoever builds the firmware (`firmware/keys/igc_signing.key`,
  created by `tools/make_signing_key.py`, never in the repository). A build without a key writes
  no G record, nor does a file that was not closed (power loss).
* `python tools/vbv_igc.py --verify KEYFILE FLIGHT.IGC` checks it.
* The signature shows that the file was not changed after recording. It does not prove where the
  file came from: anyone with the firmware binary can extract the key.

## 7. Files written by UltraBeep
The firmware writes the same records, with these choices:

| Item | UltraBeep |
|---|---|
| File | `0:/tracks_IGC/YYYY/YYYY-MM-DD-XSD-UBSSSS-NN.igc` (one folder per year; the stock firmware's naming, `SSSS` = the last four characters of the device's serial number, `NN` = flight of the day). The BipLink app downloads only files named like this. The file times are local time from the GNSS-set clock. |
| A record | `AXSDUBSSSS`, then the H records in the stock's wording (`HFFTYFRTYPE:STODEUS,ULTRABIP`, firmware string in the stock's form); `LXSDB<16 bytes base64>` identifies the track for the app, `LXUBFIRMWARE:UltraBeep <version>` names this firmware |
| H records | date with flight number, pilot, glider and competition id from the profile, firmware version, receiver, pressure sensor, `HFALGALTGPS:GEO`, `HFALPALTPRESSURE:ISA` |
| I record | `I013637SIU`: satellites in use at bytes 36–37 of every B record |
| Rate | data records at `flight_recordrate_int` (default 4 Hz, the first GNSS fix in each quarter second of the 5 Hz GPS); one B record per second right after the data record of the first fix of that second. The 15 s before the takeoff come from RAM with one data record per second. |
| Start | at the takeoff detection, with the 15 s before it from a RAM buffer; the takeoff marker stands before the first record at or after the estimated takeoff time |
| `t` | 1000 ms at the first data record, then the GNSS time of each fix minus that of the first one: no clock drift, the UTC time of a record is the one of the first B record + (`t` − 1000) ms |
| Landing | the landing marker carries the time the pilot came to rest; the file goes on, a further takeoff adds a takeoff marker (top landings do not split the track) |
| End | at the power off (or before a firmware update), then the G record |
| `h_ell` | height above mean sea level + the receiver's geoid separation (GGA) |
| `vel_n`, `vel_e` | from the ground speed and course; `vel_d` from the change of `h_ell` since the previous fix (0 when unknown) |
| `baro_alt`, `alt` | unfiltered and fused barometric altitude, set to the GNSS altitude at the first good fix and then following it with a 10 min time constant (weather drift) |
| `vario` | climb rate of the barometer + IMU fusion at the time of the fix |
| `accel` | magnitude of the acceleration, 0.5 s mean |
| `status` | bit 0: valid 3D fix (otherwise the record repeats the last position) |
| B record | pressure altitude = standard atmosphere; GNSS altitude = above mean sea level (as the header says) |

## 8. Notes on the reference file
* **A record:** the first line reads `VBV00000`, without the leading `A`. A correct A record is
  `AVBV00000`; loggers without IGC approval should use a manufacturer code beginning with `X`.
* **GNSS altitude in B records:** the header says `HFALGALTGPS:GEO` (geoid), but the B records
  carry the ellipsoid height `h_ell`, about 44 m above `h_msl`. A writer should either put
  `h_msl` into the B record or declare `ELL`.
* **Rates:** there are no data records before the takeoff marker. The `B` interval (4 s) and the
  sample interval (1 s) are those of this recorder; nothing in the format ties them.

## 9. Tool
```
python tools/vbv_igc.py FLIGHT.IGC             # samples as CSV
python tools/vbv_igc.py --check FLIGHT.IGC     # compare B records with the samples
python tools/vbv_igc.py --verify KEYFILE FLIGHT.IGC   # check an UltraBeep G record
python tools/vbv_igc.py --encode 61000 46.512345 11.345678 2104.512 1.234 -8.765 1.321 \
       2051.73 2056.3 -1.35 2051.9 9.95 12 1   # build one data record
```
