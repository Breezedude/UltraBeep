"""Persistent clip cache of the worker.

The audio of a clip depends only on the voice and the text (pitch, rate and the band-pass are applied
in the configurator), so every clip is synthesized once and served from disk afterwards, for every
user. Files live in one folder per voice key (CACHE_DIR/<key>/<hash>.mp3), so deleting an own voice
deletes its clips too; the hash covers the engine tag, so changing the model or its settings starts a
fresh set. Reading a clip touches it; above the size limit the least recently used clips go.
"""
import hashlib
import os
import shutil
from pathlib import Path


class Cache:
    def __init__(self, root, max_bytes, tag):
        self.root = Path(root)
        self.max_bytes = max_bytes
        self.tag = tag
        self.root.mkdir(parents=True, exist_ok=True)

    def path(self, voice, text):
        digest = hashlib.sha256(f"{self.tag}|{voice.name}|{voice.stamp}|{text}".encode()).hexdigest()[:40]
        return self.root / voice.key / f"{digest}.mp3"

    def get(self, voice, text):
        p = self.path(voice, text)
        try:
            data = p.read_bytes()
        except FileNotFoundError:
            return None
        try:
            os.utime(p)
        except OSError:
            pass
        return data

    def has(self, voice, text):
        return self.path(voice, text).is_file()

    def put(self, voice, text, data):
        p = self.path(voice, text)
        p.parent.mkdir(parents=True, exist_ok=True)
        tmp = p.with_suffix(f".{os.getpid()}.tmp")
        tmp.write_bytes(data)
        os.replace(tmp, p)

    def drop(self, key):
        """Deletes every clip of a voice key."""
        shutil.rmtree(self.root / key, ignore_errors=True)

    def prune(self):
        """Removes the least recently used clips until the cache is below 90 % of its limit.
        Returns (bytes before, files removed)."""
        files = []
        total = 0
        for p in self.root.glob("*/*.mp3"):
            try:
                st = p.stat()
            except FileNotFoundError:
                continue
            files.append((st.st_mtime, st.st_size, p))
            total += st.st_size
        before, removed = total, 0
        if total > self.max_bytes:
            files.sort()
            for _, size, p in files:
                if total <= self.max_bytes * 0.9:
                    break
                p.unlink(missing_ok=True)
                total -= size
                removed += 1
        return before, removed
