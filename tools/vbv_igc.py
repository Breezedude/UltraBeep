#!/usr/bin/env python3
"""Decode the binary VBV data records of an IGC file (docs/igc-vbv-records.md).

    vbv_igc.py FLIGHT.IGC            samples as CSV on stdout
    vbv_igc.py --check FLIGHT.IGC    compare every B record with the sample before it
    vbv_igc.py --encode t lat lon ...   build one data record from 11 values + sats + status
    vbv_igc.py --verify KEYFILE FLIGHT.IGC   check the G record of an UltraBeep file

Only the Python standard library is needed.
"""
import argparse
import base64
import csv
import hashlib
import hmac
import math
import struct
import sys

PREFIX = "LVBV"
RECORD = struct.Struct("<I11fBB")  # 50 bytes, little endian
FIELDS = [
    "t_ms",        # recorder time since the recording start (see the spec)
    "lat_deg",     # WGS84 latitude, degrees, north positive
    "lon_deg",     # WGS84 longitude, degrees, east positive
    "h_ell_m",     # GNSS height above the WGS84 ellipsoid
    "vel_n_ms",    # GNSS velocity north
    "vel_e_ms",    # GNSS velocity east
    "vel_d_ms",    # GNSS velocity down (sinking is positive)
    "baro_alt_m",  # barometric altitude, unfiltered
    "h_msl_m",     # GNSS height above mean sea level (geoid)
    "vario_ms",    # filtered climb rate (climbing is positive)
    "alt_m",       # filtered altitude (barometric, fused)
    "accel_ms2",   # magnitude of the acceleration, 9.81 at rest
    "sats",        # satellites used in the solution
    "status",      # status byte (1 in all known files)
]


def decode_record(line):
    """Returns the sample of an LVBV data record as a dict, or None for other lines."""
    line = line.strip()
    if not line.startswith(PREFIX):
        return None
    payload = line[len(PREFIX):]
    try:
        raw = base64.b64decode(payload, validate=True)
    except ValueError:
        return None  # marker records such as LVBVTAKEOFFhhmmss
    if len(raw) != RECORD.size:
        return None
    return dict(zip(FIELDS, RECORD.unpack(raw)))


def encode_record(values):
    return PREFIX + base64.b64encode(RECORD.pack(*values)).decode("ascii")


def b_record(line):
    """Fields of a standard B record (no I-record extensions)."""
    lat = int(line[7:9]) + int(line[9:14]) / 60000.0
    lon = int(line[15:18]) + int(line[18:23]) / 60000.0
    if line[14] == "S":
        lat = -lat
    if line[23] == "W":
        lon = -lon
    return {
        "time": line[1:7],
        "lat": lat,
        "lon": lon,
        "valid": line[24],
        "pressure_alt": int(line[25:30]),
        "gnss_alt": int(line[30:35]),
    }


def read_lines(path):
    with open(path, encoding="latin-1") as f:
        return f.read().splitlines()


def cmd_csv(path):
    out = csv.writer(sys.stdout, lineterminator="\n")
    out.writerow(FIELDS)
    for line in read_lines(path):
        s = decode_record(line)
        if s:
            out.writerow([s[k] if not isinstance(s[k], float) else f"{s[k]:.9g}" for k in FIELDS])


def stats(values):
    mean = sum(values) / len(values)
    sd = math.sqrt(sum((v - mean) ** 2 for v in values) / len(values))
    return f"mean {mean:8.3f}  sd {sd:6.3f}  min {min(values):8.3f}  max {max(values):8.3f}"


def cmd_check(path):
    lines = read_lines(path)
    last = None
    pairs = []
    markers = []
    for line in lines:
        s = decode_record(line)
        if s:
            last = s
        elif line.startswith(PREFIX):
            markers.append(line[len(PREFIX):])
        elif line.startswith("B") and last:
            pairs.append((b_record(line), last))
    samples = [decode_record(l) for l in lines]
    samples = [s for s in samples if s]
    print(f"{len(samples)} samples, {len(pairs)} B records, markers: {', '.join(markers) or '-'}")
    if not pairs:
        return
    m_per_deg = 111195.0
    coslat = math.cos(math.radians(pairs[0][1]["lat_deg"]))
    print("B record minus the sample before it:")
    print("  latitude  [m]", stats([(b["lat"] - s["lat_deg"]) * m_per_deg for b, s in pairs]))
    print("  longitude [m]", stats([(b["lon"] - s["lon_deg"]) * m_per_deg * coslat for b, s in pairs]))
    print("  GNSS alt - h_ell [m]", stats([b["gnss_alt"] - s["h_ell_m"] for b, s in pairs]))
    print("  GNSS alt - h_msl [m]", stats([b["gnss_alt"] - s["h_msl_m"] for b, s in pairs]))
    print("  pressure alt - baro_alt [m]", stats([b["pressure_alt"] - s["baro_alt_m"] for b, s in pairs]))
    steps = sorted({b2["t_ms"] - b1["t_ms"] for b1, b2 in zip(samples, samples[1:])})
    print(f"sample interval(s) [ms]: {steps}")


def g_record_hmac(raw, key):
    """HMAC-SHA-256 over all bytes before the G record and the G record's value (None if absent)."""
    pos = raw.rfind(b"\nG")
    if pos < 0:
        return None, None
    signed = raw[: pos + 1]
    line = raw[pos + 1 :].splitlines()[0].strip()
    try:
        value = base64.b64decode(line[4:], validate=True)
    except ValueError:
        value = b""
    return hmac.new(key, signed, hashlib.sha256).digest(), value


def cmd_verify(path, key_path):
    with open(key_path, encoding="ascii") as f:
        key = bytes.fromhex(f.read().strip())
    with open(path, "rb") as f:
        raw = f.read()
    expected, value = g_record_hmac(raw, key)
    if expected is None:
        print("no G record")
        sys.exit(1)
    ok = hmac.compare_digest(expected, value)
    print("G record valid" if ok else "G record INVALID (file changed or other key)")
    sys.exit(0 if ok else 1)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--check", action="store_true", help="compare B records with the samples")
    p.add_argument("--verify", metavar="KEYFILE", help="check the G record (HMAC-SHA-256) with this key")
    p.add_argument("--encode", nargs=14, metavar="V", help="t lat lon h_ell vn ve vd baro h_msl vario alt accel sats status")
    p.add_argument("file", nargs="?")
    a = p.parse_args()
    if a.encode:
        v = a.encode
        print(encode_record([int(v[0])] + [float(x) for x in v[1:12]] + [int(v[12]), int(v[13])]))
    elif a.file and a.verify:
        cmd_verify(a.file, a.verify)
    elif a.file and a.check:
        cmd_check(a.file)
    elif a.file:
        cmd_csv(a.file)
    else:
        p.print_help()


if __name__ == "__main__":
    main()
