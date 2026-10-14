# UltraBeep

An alternative firmware for the **Stodeus UltraBip** paragliding vario. Installing it is drag and
drop: copy one file onto the device over USB. Going back to the original firmware works the same
way, at any time and without restrictions. Your original settings, voice packs and tracks stay on
the device, and the BipLink app keeps working.

<a href='https://ko-fi.com/P5P41RYRWK' target='_blank'><img height='36' style='border:0px;height:36px;' src='https://storage.ko-fi.com/cdn/kofi5.png?v=6' border='0' alt='Buy Me a Coffee at ko-fi.com' /></a>

> **Work in progress.** The vario algorithms still need tuning on real flights. Bugs and ideas are welcome as [issues](https://github.com/Breezedude/UltraBeep/issues).
> Some modules like custom voice generation are WIP and not fully tested yet.

**Tools:** [Configurator](https://vario.breezedude.de) (settings over Bluetooth or USB, Chrome or
Edge) · [Analyzer](https://vario.breezedude.de/analyze/) (flight and sensor logs)

## Why

The UltraBip is excellent hardware. UltraBeep exists because of small things: long, loud jingles,
only three profiles, and algorithms that could do their job better. The journey so far is on
[Hackaday.io](https://hackaday.io/project/206937-ultrabeep-an-alternative-firmware-nobody-ask-for).

## What you get

UltraBeep does what the original firmware does, with the exceptions listed below, and adds:

* **Quieter:** short sounds instead of jingles.
* **No switching on by accident:** a short press, release, then press and hold until the start
  melody.
* **Bluetooth stays on after switching off** following a flight, so the app can fetch the track.
  The time is adjustable, and it can start only once the device lies still: switch off after the
  landing, walk to the packing place, put everything down, and Bluetooth is still on when the
  phone comes out (the LED flashes blue every 4 s meanwhile).
* **Up to ten profiles**, each with its own spoken name, generated in the configurator.
* **A note for a finder:** your contact details, typed in the configurator, as
  `IF_FOUND_PLEASE_READ.txt` in the top folder of the SD card.
* **More announcements:** besides the usual ones, the device can speak at the thermal exit, while
  thermaling, on a straight glide, with a new wind estimate and after a wingover or spiral. Each
  event has its own list of what to say: wind, glide ratio, thermal climb and gain, g-load, bank
  angle and everything the original offers.
* **Thermal and wind detection**, glide ratio, wingover and spiral detection.
* **Barometer and motion sensor working together** in the vario, with filters that adapt to the
  conditions.
* **Detailed track files:** the IGC files carry extra data (finer positions, climb rate,
  acceleration, wind) that normal tools simply skip ([docs/igc-vbv-records.md](docs/igc-vbv-records.md)).
* **A web configurator** for every setting, with a tone curve editor that plays on the device, an
  announcement editor, sensor calibration, track download, firmware update over Bluetooth and a
  voice pack generator.
* **An analyzer** that replays flights and sensor logs with charts, a 3D map and the vario tones.

**Not yet in UltraBeep:** triple click for automatic announcements.

**Left out on purpose:** the demo mode and the settings menus on the button; settings are changed
in the app or the configurator. Settings that only the original firmware uses stay untouched in
the configuration file.

## Installing

1. Download `ultrabip_ultrabeep.update` from the [releases](https://github.com/Breezedude/UltraBeep/releases).
2. Switch the device off and connect it to a computer over USB. It shows up as a drive.
3. Copy the file into the top folder of the drive, eject the drive and unplug the cable.
4. If the installation does not start by itself, switch the device on. It installs the file (red
   and green LED), says "update ok" and switches off.

Later updates go the same way, or over Bluetooth from the configurator. The battery must be charged
above about 3.6 V.

## Back to the original firmware

Copy the original firmware file onto the device the same way, or let the BipLink app install it.
The current version is
[ultrabip_202606230057.update](https://www.stodeus.com/downloads/ultrabip_202606230057.update);
newer ones are listed in the catalogue the app reads,
[catalogStodeus.json](https://www.stodeus.com/downloads/catalogStodeus.json). Nothing else is needed: your original settings, voice
packs and tracks are still there. UltraBeep's own settings live in the folder `ultrabeep/`; the
original firmware ignores it, so it is still there if you come back.

If the device does not respond at all: hold the button until it resets, then connect USB. The drive
appears again, and a firmware file can be copied onto it.

## Operation

* **Switch on:** short press (beep), then press again within 1 s and hold until the start melody.
* **Profile:** a short press moves to the next profile, a long press confirms.
* **On the ground:** a short press says whether the device is ready: "ready, waiting for takeoff"
  with a GPS fix, "searching GPS" before one.
* **In flight:** a short press switches the volume level, a double tap on the case reads out the
  flight information.
* **Switch off:** hold the button through the power-off melody; releasing earlier cancels. After a
  flight the summary follows, and Bluetooth stays on for the track download (adjustable).

## Specifications

| | |
|---|---|
| Barometer | 100 Hz (64 or 32 Hz selectable) |
| Motion sensor (IMU) | 100 Hz, accelerometer ±8 g, gyroscope ±2000 °/s |
| GPS | 5 fixes per second (or 1); GPS, GLONASS, SBAS, QZSS; hot start kept across power-off |
| Profiles | up to 10 |
| IGC files | B records once a second, [extra data records](docs/igc-vbv-records.md) 1 to 5 per second (default 4), optional G record signature, KML next to it |
| Sensor log | every sample of every sensor, for the analyzer ([docs/raw-log-format.md](docs/raw-log-format.md)) |
| Bluetooth | BipLink app (settings, tracks), `$LK8EX1` and `$LXWP0` once a second for XCTrack and XCSoar, remote control (a double tap presses the space bar on the phone), the configurator; two connections at once (phone and configurator, on the ground); a new phone or computer needs one button press on the device before it may read or change anything |
| USB | drive for the SD card and a serial console while the device is off; headphones on a USB-C audio adapter (voice and vario tones, detected automatically) |
| Debug | the console over USB, optionally also on LPUART1: RX PC0, TX PC1, 1,000,000 baud 8N1, 3.3 V (test pads inside the case) |

## For developers

### Compatibility

| Part | How |
|---|---|
| Update | An ordinary `.update` image for the original bootloader, which installs only files named `ultrabip*.update`. Its recovery mode keeps working. |
| Configuration | `config.ultrabip` is read, and only changed values are written back. Everything new lives in `ultrabeep/config_ext.ultrabip`, in a folder because the original firmware deletes unknown files in the top folder ([docs/config-ext-format.md](docs/config-ext-format.md)). |
| Voice packs | The original `.ubpack` files. Extra clips go into `voices/custom/` or `voices/custom.ubpack`. |
| BipLink app | Device Information, Battery, the NMEA stream and the original file transfer protocol with the pairing by button press. A firmware update offered by the app installs the original firmware. |
| Device identity | The app accepts only devices that look like the original. UltraBeep therefore uses the original's Bluetooth address and name and the serial number from the chip, reports the manufacturer, model and firmware version string of the original firmware, and names and heads its IGC files like the original's. The files name this firmware in an extra `LXUBFIRMWARE` record. See the disclaimer below. |

### Building

Requirements: Arm GNU Toolchain 14.x, CMake ≥ 3.22, Ninja, Python 3, git.

```
tools/fetch_sdk.sh                       # STM32CubeWB into third_party/ (once)
cd firmware
cmake --preset default -DARM_TOOLCHAIN_DIR=<path to arm-none-eabi bin>
cmake --build --preset default
```

Result: `firmware/build/ultrabip_ultrabeep.update`. Tests: `python firmware/tests/run_logic_tests.py
<toolchain bin>` (logic, in an emulator), `node tools/analyzer/test.mjs`,
`node tools/configurator/tests/<name>.js`.

IGC files get a G record only if the build finds a secret key in `firmware/keys/igc_signing.key`;
`tools/make_signing_key.py` creates one. The key never goes into git.

### Repository layout

| Path | Content |
|---|---|
| `firmware/` | The firmware (STM32CubeWB HAL, FreeRTOS, FatFs) and its logic tests |
| `tools/configurator/` | Web configurator, a static page without a build step |
| `tools/analyzer/` | Raw log and IGC analyzer, browser and command line |
| `tools/tts-relay/` | Small Node.js relay for the configurator's text-to-speech |
| `tools/` | `mkupdate.py` (update image), `fetch_sdk.sh`, `vbv_igc.py` (decode and verify IGC files), `make_signing_key.py` |
| `docs/` | IGC data records, configuration extension, raw log format, the configurator's Bluetooth file service |

## License

[Mozilla Public License 2.0](LICENSE). Changed files stay open when they are passed on; the
firmware may still be combined with the parts under other licences listed below.

Third-party code: the STM32CubeWB package (HAL, BLE middleware, FreeRTOS, FatFs, USB device
library) is fetched by `tools/fetch_sdk.sh` and keeps its own licences. Two configuration headers
in `firmware/config/` come from ST's and ChaN's templates and keep their notices. The analyzer
ships uPlot and deck.gl (MIT) and the IBM Plex fonts (SIL Open Font License) in
`tools/analyzer/vendor/`, each with its licence. Only the 3D map loads data from outside: satellite
images from Esri and terrain tiles from AWS.

## Disclaimer

UltraBeep is an independent project. It is not affiliated with, endorsed by or supported by
Stodeus. "Stodeus", "UltraBip" and "BipLink" are names of their respective owners and are used
here only to say which hardware and app this firmware works with. This repository contains no
code, firmware, voice packs or other files of Stodeus.

To work with the BipLink app, UltraBeep has to present itself as the original device: it reports
the original firmware's identity over Bluetooth and writes IGC files with the original's naming
and header. This is done for compatibility only, not to pass this firmware off as the original.

**Please do not contact Stodeus support about devices running UltraBeep.**

Installing a different firmware may void the warranty. Use at your own risk.
