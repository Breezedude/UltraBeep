#ifndef DUMP_H
#define DUMP_H

#include <stdbool.h>

/*
 * Console command "dump": copies what tells two devices apart to 0:/ultrabeep/dump/ on the SD card,
 * to compare them on a PC:
 *   bootloader_08000000.bin  the stock bootloader, 0x08000000-0x0800EFFF (60 KB, up to the application)
 *   otp_1FFF7000.bin         the OTP area (1 KB; the factory accelerometer calibration lives here)
 *   option_bytes.txt         the option bytes, in the format of the earlier dumps
 *   device.txt               chip id and revision, flash size, package, serial number, bootloader
 *                            report and version, checksums, crystal tuning, wireless stack
 * Only reads: nothing on the chip changes. A bootloader under read protection (PCROP) is skipped.
 * The answer goes to the console.
 */
bool dump_device(void);

#endif
