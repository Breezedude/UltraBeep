"""Shared voice packs: the .ubpack files people publish from the configurator ("Custom voicepack").

The relay keeps each pack's description in its database (on the VPS); the file itself lives here
(PACK_DIR/<id>.ubpack) and goes through the relay in parts: uploads arrive part by part in order, a
download is read piece by piece, so neither side holds a whole pack (10 to 20 MB) in memory.
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
    """A .ubpack is a FAT16 image (js/fatimage.js): boot signature and file system type."""
    return len(head) >= 512 and head[510:512] == b"\x55\xaa" and head[54:62] == b"FAT16   "


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
