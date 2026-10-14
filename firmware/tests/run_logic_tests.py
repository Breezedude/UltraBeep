#!/usr/bin/env python3
"""Builds the hardware-independent firmware code for Cortex-M4 and runs the
tests in logic_tests.c in the Unicorn emulator (pip install unicorn).

The IGC recorder test leaves its file in emulator memory (test_igc_file);
it is then checked with tools/vbv_igc.py like a real recording: B records
against the data records, and the G record with the test key.

Usage: run_logic_tests.py [path to the arm-none-eabi bin directory]
"""
import os
import struct
import subprocess
import sys
import tempfile

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB
from unicorn.arm_const import UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_SP

HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.dirname(HERE)
REPO = os.path.dirname(FW)
SOURCES = ["src/app/ini.c", "src/app/timeconv.c", "src/app/crc32.c", "src/app/gesture.c", "src/app/vario.c", "src/app/identity.c", "src/app/console_pack.c",
           "src/app/sha256.c", "src/app/fusion.c", "src/app/nmea.c", "src/app/flight.c", "src/app/igc.c",
           "src/app/recorder.c", "src/app/battery.c", "src/app/kml.c", "src/app/summary.c", "src/app/tap.c", "src/app/announce.c", "src/app/thermal.c", "src/app/maneuver.c", "src/drivers/qoa.c", "tests/qoa_ref.c", "tests/logic_tests.c", "tests/test_stubs.c", "tests/recorder_stubs.c"]
INCLUDES = [os.path.join(HERE, "stubs"),  # before inc/: replaces board.h
            os.path.join(FW, "inc"),
            os.path.join(FW, "config"),
            os.path.join(REPO, "third_party", "STM32CubeWB", "Middlewares", "Third_Party", "FatFs", "src")]
ROM, RAM = 0x08000000, 0x20000000
RETURN_ADDR = 0x080F0000
TEST_KEY = bytes(range(32))  # tests/recorder_stubs.c


def tool(bindir, name):
    exe = "arm-none-eabi-" + name + (".exe" if os.name == "nt" else "")
    return os.path.join(bindir, exe) if bindir else exe


def build(bindir, out_dir):
    elf = os.path.join(out_dir, "tests.elf")
    cmd = [tool(bindir, "gcc"), "-mcpu=cortex-m4", "-mthumb", "-mfloat-abi=soft", "-O1", "-g",
           "-Wall", "-Wextra", "-ffunction-sections", "-fdata-sections", "-specs=nano.specs",
           "-nostartfiles", "-Wl,--gc-sections", "-Wl,--no-warn-rwx-segments",
           "-T", os.path.join(HERE, "test.ld"), "-o", elf]
    for inc in INCLUDES:
        cmd += ["-I", inc]
    cmd += [os.path.join(FW, s) for s in SOURCES] + ["-lm"]
    subprocess.run(cmd, check=True)
    binary = os.path.join(out_dir, "tests.bin")
    subprocess.run([tool(bindir, "objcopy"), "-O", "binary", "-R", ".bss", elf, binary], check=True)
    nm = subprocess.run([tool(bindir, "nm"), elf], check=True, capture_output=True, text=True).stdout
    symbols = {}
    for line in nm.splitlines():
        parts = line.split()
        if len(parts) == 3:
            symbols[parts[2]] = int(parts[0], 16)
    return open(binary, "rb").read(), symbols


def run(image, symbols):
    mu = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
    mu.mem_map(ROM, 0x100000)
    mu.mem_map(RAM, 0x40000)
    mu.mem_write(ROM, image)
    mu.reg_write(UC_ARM_REG_SP, RAM + 0x40000)
    mu.reg_write(UC_ARM_REG_LR, RETURN_ADDR | 1)
    mu.emu_start(symbols["test_main"] | 1, RETURN_ADDR, count=2_000_000_000)
    if mu.reg_read(UC_ARM_REG_PC) & ~1 != RETURN_ADDR & ~1:
        sys.exit("test_main did not return (instruction limit reached)")
    ptr = mu.reg_read(UC_ARM_REG_R0)
    report = bytes(mu.mem_read(ptr, 4096)).split(b"\0", 1)[0].decode()
    return report, mu


def check_igc(mu, symbols, out_dir):
    """The recorder's file, checked with tools/vbv_igc.py. Returns (passed, failed, text)."""
    sys.path.insert(0, os.path.join(REPO, "tools"))
    import vbv_igc  # noqa: E402

    length = struct.unpack("<I", bytes(mu.mem_read(symbols["test_igc_len"], 4)))[0]
    raw = bytes(mu.mem_read(symbols["test_igc_file"], length))
    path = os.path.join(out_dir, "recorder.igc")
    with open(path, "wb") as f:
        f.write(raw)
    lines = raw.decode("ascii").split("\r\n")
    passed, failed, text = 0, 0, ""

    def check(cond, msg):
        nonlocal passed, failed, text
        if cond:
            passed += 1
        else:
            failed += 1
            text += f"FAIL recorder file: {msg}\n"

    expected, value = vbv_igc.g_record_hmac(raw, TEST_KEY)
    check(expected is not None and expected == value, "G record does not verify")
    samples, pairs = [], []
    last = None
    for line in lines:
        s = vbv_igc.decode_record(line)
        if s:
            samples.append(s)
            last = s
        elif line.startswith("B") and last:
            pairs.append((vbv_igc.b_record(line), last))
    # the last recording of logic_tests.c: data records at 4 Hz, the 15 s before the takeoff at 1 Hz
    check(len(samples) == 15 + 35 * 4 and len(pairs) == 50, f"{len(samples)} data records, {len(pairs)} B records")
    ts = [s["t_ms"] for s in samples]
    steps = [b - a for a, b in zip(ts, ts[1:])]
    check(ts and ts[0] == 1000 and steps[:15] == [1000] * 15 and all(x == 250 for x in steps[15:]),
          f"t not 1000 .. 16000 in seconds, then every 250 ms: {ts[:3]} .. {ts[14:18]}")
    for b, s in pairs:
        dlat = (b["lat"] - s["lat_deg"]) * 60000  # thousandths of a minute
        # B cuts the exact value; the data record holds float32 (0.23 thousandths resolution at 46 deg)
        if not (-1.3 < dlat < 0.3) or b["gnss_alt"] != round(s["h_msl_m"]) or b["pressure_alt"] != round(s["baro_alt_m"] - 30.3):
            check(False, f"B record {b['time']} does not match its data record")
            break
    else:
        check(True, "")
    check(all(s["status"] == 1 and s["sats"] == 9 for s in samples), "status / satellites")
    return passed, failed, text


def main():
    bindir = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("ARM_TOOLCHAIN_DIR", "")
    with tempfile.TemporaryDirectory() as tmp:
        image, symbols = build(bindir, tmp)
        report, mu = run(image, symbols)
        passed, failed, text = check_igc(mu, symbols, tmp)
    lines = report.rstrip().split("\n")
    summary = lines[-1].split()
    total_pass = int(summary[0]) + passed
    total_fail = int(summary[2]) + failed
    print("\n".join(lines[:-1]) + ("\n" if len(lines) > 1 else ""), end="")
    print(text, end="")
    print(f"{total_pass} passed, {total_fail} failed")
    return 0 if total_fail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
