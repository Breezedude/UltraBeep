#!/usr/bin/env python3
"""Build and verify UltraBip/BlueBip `.update` firmware images.

Image layout (application linked at 0x0800F000):
    0x000 .. 0x13B  vector table (79 entries)
    0x13C           u32 image_len   (bytes from 0x144 up to the trailer)
    0x140           u32 checksum    (POSIX cksum over [0x144, 0x144 + image_len))
    0x144 ..        application code/data
    end - 8         trailer: copy of image_len + checksum

The total file size must be a multiple of 8. The stock bootloader verifies the
checksum when it installs an image (not at every start).

Usage:
    mkupdate.py build  <app.bin> <out.update>
    mkupdate.py verify <file.update|file.install>
"""
import struct
import sys

HEADER_OFFSET = 0x13C
PAYLOAD_OFFSET = 0x144
TRAILER_SIZE = 8


def _make_table():
    table = []
    for i in range(256):
        c = i << 24
        for _ in range(8):
            c = ((c << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if c & 0x80000000 else (c << 1) & 0xFFFFFFFF
        table.append(c)
    return table


_TABLE = _make_table()


def posix_cksum(data: bytes) -> int:
    """CRC-32/CKSUM as computed by the POSIX `cksum` utility."""
    crc = 0
    for byte in data:
        crc = ((crc << 8) & 0xFFFFFFFF) ^ _TABLE[((crc >> 24) ^ byte) & 0xFF]
    length = len(data)
    while length:
        crc = ((crc << 8) & 0xFFFFFFFF) ^ _TABLE[((crc >> 24) ^ (length & 0xFF)) & 0xFF]
        length >>= 8
    return ~crc & 0xFFFFFFFF


def build(app: bytes) -> bytes:
    if len(app) <= PAYLOAD_OFFSET:
        raise ValueError("input too small to contain a vector table and header slot")
    payload = bytearray(app[PAYLOAD_OFFSET:])
    # Pad so that the total file size (header part + payload + trailer) is a multiple of 8.
    while (PAYLOAD_OFFSET + len(payload) + TRAILER_SIZE) % 8:
        payload.append(0)
    image_len = len(payload)
    checksum = posix_cksum(bytes(payload))
    info = struct.pack("<II", image_len, checksum)
    return bytes(app[:HEADER_OFFSET]) + info + bytes(payload) + info


def verify(blob: bytes) -> bool:
    image_len, checksum = struct.unpack_from("<II", blob, HEADER_OFFSET)
    end = PAYLOAD_OFFSET + image_len
    if end + TRAILER_SIZE > len(blob):
        print(f"FAIL: image_len {image_len:#x} exceeds file size {len(blob):#x}")
        return False
    trailer = struct.unpack_from("<II", blob, end)
    computed = posix_cksum(blob[PAYLOAD_OFFSET:end])
    extra = len(blob) - end - TRAILER_SIZE
    sp, reset = struct.unpack_from("<II", blob, 0)
    print(f"initial SP      {sp:#010x}")
    print(f"reset handler   {reset:#010x}")
    print(f"image_len       {image_len:#x}")
    print(f"checksum header {checksum:#010x}  trailer {trailer[1]:#010x}  computed {computed:#010x}")
    print(f"update size     {end + TRAILER_SIZE:#x} (multiple of 8: {(end + TRAILER_SIZE) % 8 == 0})")
    if extra:
        print(f"appended data   {extra} bytes (.install: configurator HTML)")
    ok = checksum == computed and trailer == (image_len, checksum) and (end + TRAILER_SIZE) % 8 == 0
    print("OK" if ok else "FAIL")
    return ok


def main(argv):
    if len(argv) == 4 and argv[1] == "build":
        with open(argv[2], "rb") as f:
            out = build(f.read())
        with open(argv[3], "wb") as f:
            f.write(out)
        return 0 if verify(out) else 1
    if len(argv) == 3 and argv[1] == "verify":
        with open(argv[2], "rb") as f:
            return 0 if verify(f.read()) else 1
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
