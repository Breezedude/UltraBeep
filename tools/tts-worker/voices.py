"""The worker's voices and their names.

A Chatterbox voice is the conditioning computed from a reference clip (conds.pt). The same voice can
speak every Chatterbox language, so the relay sees one name per voice and locale, and the locale in
the name picks the language of the text:

  cb:de-DE-florian        a public voice: VOICE_DIR/florian/{conds.pt, voice.json}
  cb:de-DE-own-<26 chars> a private voice someone uploaded: PRIVATE_DIR/<id>/{conds.pt, meta.json}

Public voices are offered in the locales their voice.json lists (where they sound native) and only
with "public": true; voice.json may carry generation settings (exaggeration, cfg_weight,
temperature). Private voices are never listed; the random id is the key to them, and they stay until
their owner deletes them.

  voice.json: {"name": "Florian", "gender": "Male", "locales": ["de-DE"], "public": true,
               "settings": {"exaggeration": 0.5}, "note": "own recording, 2026-10-15"}
"""
import base64
import json
import os
import re
import secrets
import shutil
import time
from dataclasses import dataclass, field
from pathlib import Path

from textnorm import language_of

PROVIDER = "cb"
SLUG = r"[a-z0-9]+(?:-[a-z0-9]+)*"
PRIVATE_ID = r"[a-z2-7]{26}"
NAME = re.compile(rf"^{PROVIDER}:([a-z]{{2,3}}-[A-Z]{{2}})-(own-{PRIVATE_ID}|{SLUG})$")


class UnknownVoice(Exception):
    pass


@dataclass(frozen=True)
class Voice:
    name: str          # cb:de-DE-florian
    locale: str        # de-DE
    language: str      # de (Chatterbox language id)
    key: str           # florian, or own-<id>
    conds: Path        # conds.pt
    settings: dict = field(default_factory=dict)

    @property
    def stamp(self):
        """Changes when the voice sounds different: a new reference or other settings."""
        return f"{self.conds.stat().st_mtime_ns}|{json.dumps(self.settings, sort_keys=True)}"


def new_private_id():
    """26 characters of base32: 130 random bits."""
    return base64.b32encode(secrets.token_bytes(17)).decode().lower().rstrip("=")[:26]


class Voices:
    def __init__(self, public_dir, private_dir):
        self.public_dir = Path(public_dir)
        self.private_dir = Path(private_dir)
        self.private_dir.mkdir(parents=True, exist_ok=True)

    def _public(self):
        """{slug: voice.json} of the public voices with their conditioning."""
        out = {}
        if not self.public_dir.is_dir():
            return out
        for folder in sorted(self.public_dir.iterdir()):
            meta = folder / "voice.json"
            if folder.name.startswith(("own", ".")) or not re.fullmatch(SLUG, folder.name):
                continue
            if not (folder / "conds.pt").is_file() or not meta.is_file():
                continue
            try:
                info = json.loads(meta.read_text(encoding="utf-8"))
            except ValueError:
                continue
            if info.get("public"):
                out[folder.name] = info
        return out

    def catalogue(self):
        """The public voices as the relay lists them: one entry per voice and locale."""
        out = []
        for slug, info in self._public().items():
            for locale in info.get("locales", []):
                if language_of(locale):
                    out.append({"name": f"{PROVIDER}:{locale}-{slug}", "locale": locale,
                                "gender": info.get("gender", "Unknown"), "friendly": info.get("name", slug)})
        return out

    def resolve(self, name):
        """The Voice behind a name, or UnknownVoice."""
        m = NAME.match(name or "")
        if not m:
            raise UnknownVoice(f"not a voice name: {name}")
        locale, key = m.groups()
        language = language_of(locale)
        if not language:
            raise UnknownVoice(f"Chatterbox does not speak {locale}")
        settings = {}
        if key.startswith("own-"):
            conds = self.private_dir / key[4:] / "conds.pt"
            if not conds.is_file():
                raise UnknownVoice("this own voice does not exist (deleted?)")
        else:
            info = self._public().get(key)
            if not info or locale not in info.get("locales", []):
                raise UnknownVoice(f"no voice {name}")
            conds = self.public_dir / key / "conds.pt"
            settings = {k: float(v) for k, v in (info.get("settings") or {}).items()
                        if k in ("exaggeration", "cfg_weight", "temperature")}
        return Voice(name, locale, language, key, conds, settings)

    def private_folder(self, private_id):
        if not re.fullmatch(PRIVATE_ID, private_id or ""):
            raise UnknownVoice("not an own voice id")
        return self.private_dir / private_id

    def stage_private(self):
        """A fresh id and an empty staging folder for its conds.pt (see commit_private)."""
        private_id = new_private_id()
        staging = self.private_dir / f".{private_id}.tmp"
        staging.mkdir()
        return private_id, staging

    def commit_private(self, private_id, staging, meta):
        """Makes a staged voice usable; meta goes to meta.json (never the recording itself)."""
        stamp = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
        (staging / "meta.json").write_text(json.dumps({**meta, "created": stamp}, indent=1), encoding="utf-8")
        os.replace(staging, self.private_folder(private_id))

    @staticmethod
    def discard(staging):
        shutil.rmtree(staging, ignore_errors=True)

    def delete_private(self, private_id):
        """Removes a private voice; False when there was none."""
        folder = self.private_folder(private_id)
        if not folder.is_dir():
            return False
        shutil.rmtree(folder)
        return True

    def touch(self, voice):
        """Notes the last use of a private voice (shown to the operator, not used for deletion)."""
        if voice.key.startswith("own-"):
            try:
                os.utime(voice.conds.parent / "meta.json")
            except OSError:
                pass
