# Raw log analyzer

Browser tool and command line for the raw sensor logs of UltraBeep
(`0:/raw/RAWnnnnn.BIN`, [format](../../docs/raw-log-format.md)) and for IGC files (any logger;
the enhanced files of UltraBeep with their LVBV data records too). It is used to tune
the vario filter, to find sensor problems and to see the flight states the firmware's detectors
derive from a flight. The browser UI and the command line share the same modules in `js/core`
and `js/algorithms`. Everything runs locally; no log leaves the computer.

## Start

Open a raw log or an IGC file with the button or by dropping it on the page; `?log=` takes
either, e.g. `?log=/tools/data/flight.igc`.

The page uses ES modules, so it needs a local web server; `file://` does not work. From the
repository root:

```bash
python tools/analyzer/serve.py
```

Then open <http://localhost:8766/tools/analyzer/>, and open a log or drop it on the page.
`?log=/tools/Auto.BIN` loads a log from the served folder at start.

Requirements:
* Internet, for the chart library (uPlot), deck.gl, the satellite imagery (Esri World Imagery)
  and the terrain (Mapzen terrain tiles on AWS).
* Node.js 18 or later, only for the command line.

## Flight states (IGC files and raw logs)

`js/core/flightstate.js` replays the firmware's detectors on a per-second series of the track:
takeoff and landing (`flight.c`), circling, climb phases, thermal exit, glide ratio and the wind
(`thermal.c`), spirals and wingovers (`maneuver.c`). For a raw log the manoeuvres come from a
replay at the device's own rate (every 50 ms on |a| and |ω| smoothed over 0.1 s, between takeoff
and landing), and the flight chart offers the load, the bank angle and the signed gyro turn rate
per barometer sample next to the per-second values the other detectors see: a wingover's swings
show there, the 1 s means average them away. Without an IMU (a plain IGC file) the bank
angle and the load factor are estimated from the turn rate and the airspeed (ground velocity
minus the wind estimate) as in a coordinated turn; wingovers need the unloaded moment and are
only found with the acceleration of the enhanced files or a raw log. In a tight spiral the
1 Hz positions of a plain IGC file jitter around its centre, so the turn rate collapses there;
a plain file gets its spirals from the sink instead (6 m/s or more for 3 s), the enhanced files
and raw logs from the measured load factor and the GNSS velocities as the firmware does.

* Takeoff and landing exactly as the firmware detects them with its default settings: the same
  rules, the GNSS fix with its HDOP, the barometer and the IMU peaks of every second (raw logs),
  dated as the device dates them. Green (takeoff) and red (landing) lines run through every chart,
  markers with the time sit on the 3D map; the pane "Flight" names the rules that held and the
  second the device notices it. An IGC file has neither HDOP nor IMU peaks: its fixes count as
  good, and of the landing rules only the calm one can apply.
* Bands in every chart: thermaling (orange), circling without lift (yellow), spiral (red),
  wingover (purple), on the ground (grey); straight flight stays plain. Yellow ticks at the top
  mark what the device would announce with the default settings (climb reports, thermal exit,
  glide ratio, wind, manoeuvres).
* The line above the charts follows the cursor: state, length and gain of the segment, glide
  ratio over the last 20 s, the 30 s mean climb, bank angle and load, wind, airspeed, and the
  announcement at that moment.
* The chart "Flight": glide ratio, load, bank angle, turn rate and wind over time.
* The 3D map coloured by flight state (colour "flight state"), the tooltip with the same values.
* The pane "Flight": summary, the segments (click to zoom) and every announcement.
* Command line: `node cli.mjs states FLIGHT.igc` (or a raw log), `--json` for scripts;
  browser: `UBA.flightStates()`, `UBA.stateAt(t)`.

An IGC file has no sensor samples: the runs, the metrics, the sensor health and the spectra are
not available for it; the climb rate comes from the file's data records or, for a plain file,
from the pressure altitude (±2 s slope).

## What it shows

* **Runs:** each run replays one algorithm with its own parameters over the same raw samples:
  * the firmware fusion (port of `firmware/src/app/fusion.c`), with its manoeuvre modes: a spiral
    mode (the climb rate as the altitude change over one rotation, which cancels the pressure
    modulation a spiral puts on the barometer; the IMU acceleration and the bias state frozen), the
    IMU acceleration off in fast or unsteady rotation (launch run, landing, handling) and an attitude
    re-acquisition afterwards. The `mode` diagnostic shows the state; the switches `SPIRAL_G=0`,
    `FAST_RATE=0`, `HANDLING_RATE=0`, `FREEZE_ON_HOLD=0`, `REACQ_S=0`, `BC_MAX=100`, `REST_VS_GREF=1`
    give the behaviour of firmware 0.42 and earlier (for logs recorded with it)
  * the barometer-only Kalman filter (`vario.c`)
  * private algorithms, if any (see below)
  * `device (logged)`: the climb rate the device itself computed while recording
  
  Parameters can be changed live; the run replays at once. Runs can be added, duplicated and
  hidden.
* **Charts** share the zoom and the cursor:
  * Interactions:
    * Drag to zoom; double-click or `Esc` for the whole log.
    * Click to set the replay position.
  * Charts:
    * climb rate (with the reference and GNSS)
    * error against the reference
    * altitude (barometer, GNSS shifted onto it)
    * motion (| |a| − g |, rotation)
    * GNSS (speed, satellites, HDOP)
    * barometer noise and temperature
    * the diagnostics of a run (fusion: tilt, turn rate, vertical acceleration, accelerometer
      correction share, barometer noise, bias, rest, hold)
* **3D map:**
  * Satellite imagery on terrain; the GNSS track in 3D as a thin outlined line, coloured by a
    run's climb rate, GNSS vertical speed, ground speed, altitude or flight state.
  * The terrain model differs from the GNSS altitude by 10-30 m in the mountains. The flight data
    stay as they are; the terrain is shifted by one offset for the whole track, the lowest ground
    offset before a takeoff or after a landing, so that this end lies on the surface and the rest
    at or above it. Where the track would still run under the terrain it is drawn on the surface;
    no line cuts the terrain.
  * Takeoff and landing markers (see above); a click on one sets the cursor there.
  * Click on the track sets the cursor. The replay position is a paraglider with altitude and climb,
    its size set by the "glider" slider (times the true size, about 9 m span):
    * Heading: the GNSS course; between two fixes it turns with the gyro's turn rate, so a spiral
      turns smoothly at 1 Hz fixes.
    * Bank and pitch from the IMU (`js/core/attitude.js`). The device can sit anywhere on the pilot
      in any orientation, so the pilot's axes are found in the log first: the vertical from the
      mean acceleration in straight, calm flight, the side from how the true vertical (the
      rotation axis in a turn) leans against the pilot in right and left turns. A complementary
      filter then tracks the true vertical: the gyro turns it, the accelerometer pulls it towards
      the acceleration less the centripetal part (GNSS speed), a fast steady rotation towards its
      axis. The firmware's own attitude cannot be used here: it holds the apparent vertical, which
      leaves gentle turns without bank.
    * An IGC file, or a raw log without enough turns to find the side (at least 15 s of them),
      banks as in a coordinated turn (the flight states' estimate) and stays level.
    * The model is `models/glider.glb` (glTF 2.0), built from code by `node models/make-glider.mjs`.
  * With "follow" ticked the camera glides behind the pilot during the replay (along the heading,
    holding its bearing while circling) and flies to a clicked position.
  * Left drag rotates the view around the position (at flight altitude), right (or ctrl) drag
    moves the map, the wheel zooms; the zoom and pitch you set are kept while following.
* **Metrics:** for the zoomed range, per run:
  * lag against the reference, correlation, RMS error after the lag, jitter, bias
  * false and missed lift
  * error against the GNSS vertical speed
  * tone switches per minute (the firmware's beep cadence)
  * deviation after taps and button presses, and a combined score
  
  The RMS error is also broken down by rotation rate, ground speed and | |a| − g |.
* **Sensor health:**
  * sample rates, gaps and jitter
  * barometer noise (also by ground speed), spikes, steps and temperature
  * IMU clipping, stuck values, |a| and gyro bias at rest
  * GNSS time to fix, fix losses, jumps, HDOP, satellites, C/N0 and checksum errors
  * barometer vs. GNSS drift, refused module commands, voice underruns
  
  The time stamps of findings jump to the place in the charts.
* **Spectrum:** amplitude spectral density of |a|, |ω|, barometric altitude or a run's climb rate
  over the zoomed range (vibration, pendulum, noise floors).
* **Events:** raw log events and GPS module answers; click to jump.
* **Tone replay:**
  * ▶ or space plays the vario tone of a chosen run from the cursor, at 0.5–8×, with the
    firmware's tone curve and beep cadence.
  * Keys `1`–`9` switch the run while playing, to compare the same air with different
    algorithms.
  * A configurator tone curve (JSON) can be pasted.

### Reference and metrics

There is no ground truth in a log. The yardstick is a **non-causal least-squares slope of the
barometric altitude** (±2 s by default): no lag, but smooth. In turns (gyro magnitude above
30 °/s, averaged over a second) the altitude is first averaged, centred, over exactly one
rotation period, because the pressure port sees a modulation synchronous with the rotation (±5 m
at 2-3 g in a spiral) that the slope alone would keep; `--flat-ref` turns this off. Lag is the shift with the best
correlation against it. Noise and error are measured after removing that lag. Windows around taps
and button presses (−1 … +4 s) are left out, because pressure pulses there show in the reference
too. Those windows get their own figure ("handling") against a robust reference (moving median, ±10 s).

## Command line

From `tools/analyzer`:

```bash
node cli.mjs algos
```

```bash
node cli.mjs info LOG
```

```bash
node cli.mjs run LOG --algo fusion --algo fusion:KP=0.5,ACCEL_GATE=0.1 --range 300-900
```

```bash
node cli.mjs sweep LOG --algo fusion --grid KP=0.5,1,2 --grid FUSION_ACCEL_NOISE=0.3,0.5 --top 5
```

```bash
node cli.mjs export LOG OUTDIR
```

* `info` gives the header, streams and health findings.
* `run` gives the metrics. With `--csv FILE` it also writes the series, with `--json` it prints
  machine-readable output.
* `sweep` replays every parameter combination in one pass.
* `export` writes `baro.csv`, `imu.csv`, `nmea.txt` and `events.csv`.
* Common options: `--ref S` (reference half window), `--flat-ref` (no one-rotation averaging in
  turns) and `--json`.

Self-test on a synthetic log:

```bash
node test.mjs
```

## Scripting the page

`window.UBA` in the browser console:

| Call | Effect |
|---|---|
| `whenReady()` | Wait until the page is set up and a log given by `?log=` is loaded |
| `load(urlOrBuffer)` | Load a log |
| `algorithms()` | Algorithms and their parameters |
| `runs()` | Current runs |
| `addRun({algo, params, label})` | Add a run |
| `updateRun(label, params)` | Change a run's parameters |
| `setRange(t0, t1)` | Zoom (seconds since the log start) |
| `metrics()` | Metrics for the zoomed range |
| `health()` | Sensor health findings |
| `sweep({algo, grid: {KEY: [values]}, range})` | Parameter sweep |
| `series(name)` | One series as an array |
| `play(t, speed)`, `pause()` | Tone replay |
| `setToneSource(label)` | Run that drives the tone |

The "Automation" tab lists them with examples.

## Adding an algorithm

An algorithm is a module in `js/algorithms/`. The interface is described in
`js/core/registry.js`:
* `params` with defaults and ranges, and `diagnostics`.
* `create(params)` returns an instance that receives:
  * `imu(t, w, a, dt)` at 100 Hz
  * `baro(t, altitude, pressure)` at 32 Hz; after it, `vz` and `h` are read as the outputs
  * `fix(t, fix)` for every GNSS epoch
  * `event(t, text)` for raw log events

Add it to `builtin` in `registry.js`. Keep ports of firmware code in step with the C files: the
fusion port carries the names of `fusion.c`.

**Private algorithms** live in `private/index.js`, which default-exports a list of modules. The
folder is git-ignored and meant for reconstructions of other firmware. The page and the command
line load it if it exists; otherwise the browser console shows a harmless 404 for it.

## Credits

* Charts: [uPlot](https://github.com/leeoniya/uPlot) (MIT).
* Map: [deck.gl](https://deck.gl) (MIT).
* Glider model: generated by `models/make-glider.mjs`, no third-party asset.
* Imagery: Esri, Maxar, Earthstar Geographics and the GIS User Community.
* Terrain: Mapzen terrain tiles on AWS Open Data.
