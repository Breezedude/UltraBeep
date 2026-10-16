"""The worker's voices and their names.

A Chatterbox voice is the conditioning computed from a reference clip (conds.pt). The same voice can
speak every Chatterbox language, so the relay sees one name per voice and locale, and the locale in
the name picks the language of the text:

  cb:de-DE-florian        a public voice: VOICE_DIR/florian/{conds.pt, voice.json}
  cb:de-DE-own-<26 chars> a private voice someone uploaded: PRIVATE_DIR/<id>/{conds.pt, meta.json}

Public voices are offered in the locales their voice.json lists (where they sound native) and only
with "public": true; voice.json may carry generation settings (exaggeration, cfg_weight,
temperature) and the configurator's audio defaults for the voice (pitch, rate, band-pass). They are
made in the lab or published from the configurator ("Custom speaker": an own voice made public by
its speaker, with consent; only the operator deletes them). Private voices are never listed; the
random id is the key to them, and they stay until their owner deletes them.

  voice.json: {"name": "Florian", "gender": "Male", "locales": ["de-DE"], "public": true,
               "settings": {"exaggeration": 0.5}, "audio": {"pitch": 0, "rate": 5, "bandpass": true,
               "lo": 300, "hi": 8000}, "note": "own recording, 2026-10-15"}
"""
import base64
import json
import os
import re
import secrets
import shutil
import time
import unicodedata
from dataclasses import dataclass, field
from pathlib import Path

from textnorm import language_of

PROVIDER = "cb"
SLUG = r"[a-z0-9]+(?:-[a-z0-9]+)*"
PRIVATE_ID = r"[a-z2-7]{26}"
NAME = re.compile(rf"^{PROVIDER}:([a-z]{{2,3}}-[A-Z]{{2}})-(own-{PRIVATE_ID}|{SLUG})$")


class UnknownVoice(Exception):
    pass


class VoiceRefused(Exception):
    """A voice that cannot be published (duplicate or bad name); the message is shown to the user."""


SETTING_RANGES = {"exaggeration": (0.25, 2.0), "cfg_weight": (0.0, 1.0), "temperature": (0.05, 2.0)}
AUDIO_RANGES = {"pitch": (-50, 50), "rate": (-50, 50), "lo": (20, 2000), "hi": (1000, 11000)}


def clean_settings(raw):
    """Chatterbox settings from outside: known keys only, clamped, two decimals."""
    out = {}
    for key, (lo, hi) in SETTING_RANGES.items():
        try:
            if raw and key in raw:
                out[key] = round(min(hi, max(lo, float(raw[key]))), 2)
        except (TypeError, ValueError):
            pass
    return out


def clean_audio(raw):
    """The configurator's audio defaults of a voice: pitch and rate in percent, the band-pass."""
    if not isinstance(raw, dict):
        return {}
    out = {}
    for key, (lo, hi) in AUDIO_RANGES.items():
        try:
            if key in raw:
                out[key] = int(min(hi, max(lo, round(float(raw[key])))))
        except (TypeError, ValueError):
            pass
    if "bandpass" in raw:
        out["bandpass"] = bool(raw["bandpass"])
    return out


def clean_name(name):
    """A display name: 2 to 32 printable characters, single spaces."""
    name = " ".join(str(name or "").split())
    if not 2 <= len(name) <= 32 or not all(c.isprintable() for c in name):
        raise VoiceRefused("The name needs 2 to 32 characters.")
    return name


def slug_of(name):
    s = unicodedata.normalize("NFKD", name.lower().replace("ß", "ss"))
    s = re.sub(r"[^a-z0-9]+", "-", s.encode("ascii", "ignore").decode()).strip("-")[:32].strip("-")
    return s if s and not s.startswith("own") else f"v-{s}".strip("-")


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
                    entry = {"name": f"{PROVIDER}:{locale}-{slug}", "locale": locale,
                             "gender": info.get("gender", "Unknown"), "friendly": info.get("name", slug)}
                    if clean_audio(info.get("audio")):
                        entry["defaults"] = clean_audio(info.get("audio"))
                    out.append(entry)
        return out

    def publish(self, private_id, name, locale, gender, settings, audio):
        """An own voice made public by its speaker: a copy of its conditioning becomes a public voice
        of that name in that locale. Returns the new voice name; VoiceRefused for a taken name."""
        name = clean_name(name)
        if not re.fullmatch(r"[a-z]{2,3}-[A-Z]{2}", locale or "") or not language_of(locale):
            raise VoiceRefused(f"Chatterbox does not speak {locale}.")
        conds = self.private_folder(private_id) / "conds.pt"
        if not conds.is_file():
            raise UnknownVoice("this own voice does not exist (deleted?)")
        lang = locale.split("-")[0]
        for info in self._public().values():
            if info.get("name", "").casefold() == name.casefold() and \
                    any(loc.split("-")[0] == lang for loc in info.get("locales", [])):
                raise VoiceRefused(f"There is a voice called {name} in this language already.")
        base = slug_of(name)
        slug, n = base, 2
        while (self.public_dir / slug).exists():
            slug, n = f"{base}-{n}", n + 1
        staging = self.public_dir / f".{slug}.tmp"
        staging.mkdir(parents=True)
        try:
            shutil.copy2(conds, staging / "conds.pt")
            info = {"name": name, "gender": gender if gender in ("Female", "Male") else "Unknown", "locales": [locale],
                    "public": True, "settings": clean_settings(settings), "audio": clean_audio(audio),
                    "source": "configurator", "consent": "published by its speaker, or with the speaker's permission",
                    "created": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}
            (staging / "voice.json").write_text(json.dumps(info, ensure_ascii=False, indent=1), encoding="utf-8")
            os.replace(staging, self.public_dir / slug)
        finally:
            shutil.rmtree(staging, ignore_errors=True)
        return f"{PROVIDER}:{locale}-{slug}"

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
