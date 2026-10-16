"""Shared voice packs: the .ubpack files people publish from the configurator ("Custom voicepack").

The relay keeps each pack's description in its database (on the VPS); the file itself lives here
(PACK_DIR/<id>.ubpack) and goes through the relay in parts: uploads arrive part by part in order, a
download is read piece by piece, so neither side holds a whole pack (10 to 20 MB) in memory. The
sample sentences others hear come out of the pack itself (fat_files), whatever voice made it.
Only the operator deletes packs.
"""
import hashlib
import os
import re
from pathlib import Path

PACK_ID = re.compile(r"^[a-z0-9]{16,40}$")


class PackError(Exception):
    def __init__(self, status, message):
        super().__init__(message)
        self.status = status


def looks_like_pack(head):
    """A .ubpack is a FAT image: FAT16 from the configurator (js/fatimage.js), FAT12 from other tools."""
    return len(head) >= 512 and head[510:512] == b"\x55\xaa" and head[54:62] in (b"FAT16   ", b"FAT12   ")


def fat_files(image):
    """{path: bytes} of every file in a FAT12/16 image (long names included), paths lower case with "/"."""
    u16 = lambda o: int.from_bytes(image[o:o + 2], "little")  # noqa: E731
    bps, spc, reserved, nfats = u16(11), image[13], u16(14), image[16]
    root_entries, fat_sectors = u16(17), u16(22)
    total = u16(19) or int.from_bytes(image[32:36], "little")
    if not (bps in (512, 1024, 2048, 4096) and spc and nfats and fat_sectors):
        raise PackError(400, "not a FAT image")
    fat = image[reserved * bps:(reserved + fat_sectors) * bps]
    root = (reserved + nfats * fat_sectors) * bps
    data = root + -(-root_entries * 32 // bps) * bps
    clusters = (total * bps - data) // (spc * bps)
    fat12 = clusters < 4085

    def chain(first):
        c, seen = first, set()
        while 2 <= c < (0xFF8 if fat12 else 0xFFF8) and c not in seen:
            seen.add(c)
            yield c
            if fat12:
                v = int.from_bytes(fat[c + c // 2:c + c // 2 + 2], "little")
                c = v >> 4 if c & 1 else v & 0xFFF
            else:
                c = int.from_bytes(fat[2 * c:2 * c + 2], "little")

    def read(first, size=None):
        out = b"".join(image[data + (c - 2) * spc * bps:data + (c - 1) * spc * bps] for c in chain(first))
        return out if size is None else out[:size]

    def entries(raw):
        lfn = []
        for o in range(0, len(raw) - 31, 32):
            e = raw[o:o + 32]
            if e[0] == 0:
                break
            if e[0] == 0xE5:
                lfn = []
                continue
            if e[11] == 0x0F:  # a long-name part, stored last part first
                lfn.insert(0, (e[1:11] + e[14:26] + e[28:32]).decode("utf-16-le", "ignore").split("\x00")[0])
                continue
            base, ext = e[0:8].decode("latin1").rstrip(), e[8:11].decode("latin1").rstrip()
            short = (base.lower() if e[12] & 0x08 else base) + ("." + (ext.lower() if e[12] & 0x10 else ext) if ext else "")
            name = "".join(lfn) or short
            lfn = []
            if e[11] & 0x08 or name in (".", ".."):
                continue
            yield name, e[11], int.from_bytes(e[26:28], "little"), int.from_bytes(e[28:32], "little")

    out = {}

    def walk(raw, prefix):
        for name, attr, first, size in entries(raw):
            path = f"{prefix}{name}".lower()
            if attr & 0x10:
                if first >= 2 and path.count("/") < 4:
                    walk(read(first), f"{path}/")
            elif first >= 2:
                out[path] = read(first, size)
            else:
                out[path] = b""

    walk(image[root:root + root_entries * 32], "")
    return out


class Packs:
    def __init__(self, root, max_bytes, max_total):
        self.root = Path(root)
        self.max_bytes = max_bytes
        self.max_total = max_total
        self.root.mkdir(parents=True, exist_ok=True)
        self.uploads = {}  # id -> {"next": part, "size": bytes, "sha": hash}

    def path(self, pack_id):
        if not PACK_ID.match(pack_id or ""):
            raise PackError(400, "not a pack id")
        return self.root / f"{pack_id}.ubpack"

    def used(self):
        return sum(p.stat().st_size for p in self.root.glob("*.ubpack"))

    def put_part(self, pack_id, part, parts, data):
        """Stores part `part` of `parts`; returns (size, sha256 hex) after the last one, else None."""
        final = self.path(pack_id)
        tmp = final.with_name(f".{pack_id}.part")
        if final.exists():
            raise PackError(409, "this pack is stored already")
        if part == 0:
            if not looks_like_pack(data):
                raise PackError(400, "not a voice pack image")
            self.uploads[pack_id] = {"next": 0, "size": 0, "sha": hashlib.sha256()}
            tmp.unlink(missing_ok=True)
        up = self.uploads.get(pack_id)
        if up is None or part != up["next"] or not 0 <= part < parts:
            raise PackError(409, f"expected part {up['next'] if up else 0}")
        if up["size"] + len(data) > self.max_bytes:
            self.abort(pack_id)
            raise PackError(413, f"a pack may have {self.max_bytes // 2**20} MB at most")
        if part == 0 and self.used() + self.max_bytes > self.max_total:
            self.abort(pack_id)
            raise PackError(507, "the pack storage is full")
        with open(tmp, "ab") as f:
            f.write(data)
        up["sha"].update(data)
        up["size"] += len(data)
        up["next"] += 1
        if up["next"] < parts:
            return None
        os.replace(tmp, final)
        del self.uploads[pack_id]
        return up["size"], up["sha"].hexdigest()

    def abort(self, pack_id):
        self.uploads.pop(pack_id, None)
        self.path(pack_id).with_name(f".{pack_id}.part").unlink(missing_ok=True)

    def clips(self, pack_id, paths):
        """{path: bytes} of some files of a stored pack (its sample sentences), missing ones left out."""
        try:
            files = fat_files(self.path(pack_id).read_bytes())
        except FileNotFoundError:
            raise PackError(404, "no such pack")
        return {p: files[p.lower()] for p in paths[:20] if p.lower() in files and len(files[p.lower()]) <= 256 * 1024}

    def read(self, pack_id, offset, length):
        """(bytes, total size) of the stored pack from offset."""
        p = self.path(pack_id)
        try:
            size = p.stat().st_size
            with open(p, "rb") as f:
                f.seek(max(0, offset))
                return f.read(max(0, min(length, 4 * 2**20))), size
        except FileNotFoundError:
            raise PackError(404, "no such pack")

    def delete(self, pack_id):
        self.abort(pack_id)
        p = self.path(pack_id)
        if not p.exists():
            return False
        p.unlink()
        return True
