# Firmware

STM32WB55 (Cortex-M4 application core) with STM32Cube HAL, FreeRTOS, FatFs and the ST USB device
library. The radio core keeps the wireless stack that is installed on the device.

## Behaviour
* **Power on** (device off): short press (beep + LED), release, press again within 1 s and hold
  until the start melody plays. Released earlier → back to off.
* **Profile selection**: announces "profile" and the preselected profile; short press = next,
  long press (0.7 s, confirmation tone) = confirm; without input the profile is chosen after the third announcement.
  The choice is remembered (`config_ext.ultrabip`).
* **On the ground** the device stays silent (no vario tones, no announcements); the LED blinks yellow until the first GPS fix
  (number of blinks = satellites with a good signal, max. 4), then "GPS ok" is played once and the LED shows what
  `led_userled_int` selects (off, vario colours in flight, power indicator; docs/config-ext-format.md). Whenever that
  shows nothing, a very short green flash every 4 s says the device is on; in the Bluetooth window after switching off
  the flash is blue.
  The GPS starts right after power on.
* **Power off**: hold the button; the power-off melody plays and the device switches off at its
  end. Releasing the button after 90 % of the melody still switches off; releasing earlier
  cancels.
* **USB**: with USB power and the device switched off, the SD card is a USB drive; a serial
  port (debug console) is always available while USB is connected.
* **Bluetooth**: while switched on the device advertises with the stock firmware's address and
  name (`UltraBip🪂<first word of the pilot's name>`, firmware/inc/identity.h) and offers the file
  service of the configurator (`docs/ble-file-service.md`). The radio core keeps its installed
  wireless stack; once it ran, switching off goes through a reset so that standby is reached.
* **Long press reset**: holding the button for several seconds resets the board (hardware). With
  USB connected the bootloader starts its recovery drive (blue LED); a short press leaves it and
  starts this firmware, which then stays switched off. The log shows how long the button was held.

## Debug console
Any serial terminal (115200 baud, settings are ignored) on the USB serial port. With
`system_uartconsole_bool` the same console also runs on the stock firmware's serial pins (LPUART1,
RX PC0, TX PC1, 1,000,000 baud 8N1, 3.3 V), and the configurator's console view (developer mode)
reaches it over Bluetooth (`CONSOLE on`, `docs/ble-file-service.md`). `help` lists the
commands; for GPS analysis: `gps` (milestones: first NMEA, first/4 good satellites, UTC, 2D/3D
fix), `gps start auto|cold|time`, `gps monitor on`, `gps send @VER`, `rtc`. `dump` copies the stock
bootloader (60 KB), the OTP area, the option bytes and chip data to `0:/ultrabeep/dump/` (read only), to
compare devices.

## Code layout
| Path | Content |
|---|---|
| `src/main.c` | start-up, wake-up press, emergency recovery, task creation |
| `src/app/` | system state machine, GPS, sound, console, configuration, storage, LED patterns |
| `src/drivers/` | board support, audio hardware, SD card, USB, analog inputs, faults |
| `inc/`, `config/` | headers, HAL/FreeRTOS/FatFs/USB configuration |
| `tests/` | logic tests, run in an ARM emulator: `python tests/run_logic_tests.py <toolchain bin>` |

Tasks: `system` (state machine), `sound`, `gps`, `console`, `ble`, plus FreeRTOS timers (button
sampling, LED patterns, watchdog supervision). The watchdog is only fed while every task reports
alive; after a reset while switched on the device resumes without the power-on gesture.
