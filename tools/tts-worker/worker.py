"""TTS worker for the UltraBeep voice relay.

Runs at home next to the models and connects OUT to the relay (wss://voice.breezedude.de/v1/worker,
shared secret), so the home network needs no open port. The relay sends jobs over that socket; the
worker answers from its disk cache or synthesizes with Chatterbox in a process pool:

  relay -> worker  {"type": "speak", "id", "voice": "cb:de-DE-florian", "text", "settings"?, "language"?}
  worker -> relay  {"type": "audio", "id", "mime": "audio/mpeg", "data": base64, "cached", "ms", "spoken"}
  relay -> worker  {"type": "clone", "id", "wav": base64, "consent": true}
  worker -> relay  {"type": "cloned", "id", "voice_id", "seconds"}
  relay -> worker  {"type": "forget", "id", "voice_id"}
  worker -> relay  {"type": "forgot", "id", "deleted"}
  relay -> worker  {"type": "publish", "id", "voice_id", "name", "locale", "gender", "settings", "audio",
                    "consent": true}
  worker -> relay  {"type": "published", "id", "voice"}
  relay -> worker  {"type": "pack_put", "id", "pack", "part", "parts", "data": base64}
  worker -> relay  {"type": "pack_stored", "id", "done", "size", "sha256"}
  relay -> worker  {"type": "pack_read", "id", "pack", "offset", "length"}
  worker -> relay  {"type": "pack_data", "id", "data": base64, "size"}
  relay -> worker  {"type": "pack_sample", "id", "pack", "paths": ["voices/takeoff.qoa", ...]}
  worker -> relay  {"type": "pack_clips", "id", "clips": {path: base64}}
  relay -> worker  {"type": "pack_delete", "id", "pack"}
  worker -> relay  {"type": "pack_deleted", "id", "deleted"}
  worker -> relay  {"type": "error", "id", "status", "message"}   (400 bad input, 404 not found, 409, 500)
  worker -> relay  {"type": "hello" | "voices", "voices": [...], "slots", "seconds", "languages"}

"settings" of a speak are Chatterbox settings to try (the configurator's Custom speaker page); they
override the voice's own and are part of the cache key, like "language": a line spoken in another
Chatterbox language than the voice's own (a pack's "GPS ok" in English). When idle, the worker renders the voice pack
texts of every public voice into the cache (PRERENDER=1), so a pack in those voices comes back at
once. Settings come from the environment (worker.env.example).
"""
import asyncio
import base64
import json
import logging
import multiprocessing
import os
import random
import socket
import time
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import websockets
from websockets.asyncio.client import connect

import engine
from cache import Cache
from packs import PackError, Packs
from textnorm import LANGUAGES, prepare
from voices import UnknownVoice, VoiceRefused, Voices, clean_settings

log = logging.getLogger("tts-worker")
MAX_TEXT = 300


def env(name, default):
    return type(default)(os.environ.get(name, default))


class Worker:
    def __init__(self, pool=None):
        self.relay_url = os.environ.get("RELAY_URL", "wss://voice.breezedude.de/v1/worker")
        self.secret = os.environ["WORKER_SECRET"]
        self.name = os.environ.get("WORKER_NAME", socket.gethostname())
        self.engines = env("ENGINES", 1)
        self.voices = Voices(os.environ.get("VOICE_DIR", "/var/lib/tts/voices"),
                             os.environ.get("PRIVATE_DIR", "/var/lib/tts/private"))
        self.cache = Cache(os.environ.get("CACHE_DIR", "/var/lib/tts/cache/clips"),
                           env("CACHE_MAX_MB", 4000) * 1024 * 1024, engine.TAG)
        self.packs = Packs(os.environ.get("PACK_DIR", "/var/lib/tts/packs"), env("PACK_MAX_MB", 40) * 2**20,
                           env("PACK_STORAGE_MB", 5000) * 2**20)
        self.catalogue = self.load_catalogue(os.environ.get("CATALOGUE", ""))
        self.prerender = env("PRERENDER", 1) == 1 and bool(self.catalogue)
        # one model per process: Chatterbox swaps parts of itself per clip and is not thread-safe
        self.pool = pool or ProcessPoolExecutor(self.engines, mp_context=multiprocessing.get_context("spawn"),
                                                initializer=engine.init_process,
                                                initargs=(os.environ.get("MODEL_DIR", "/opt/tts/models"), env("THREADS", 8)))
        self.seconds = 3.5     # running mean of a synthesized clip, for the configurator's estimate
        self.busy = 0          # jobs in the pool
        self.last_user = 0.0   # last request from the relay (prerendering waits for a quiet spell)
        self.ws = None

    @staticmethod
    def load_catalogue(path):
        """{"packs": [{"lang", "tts"}], "texts": {lang: [text]}} from export_catalogue.js."""
        if not path or not Path(path).is_file():
            return None
        return json.loads(Path(path).read_text(encoding="utf-8"))

    def hello(self, kind="hello"):
        voices = [{**v, "seconds": round(self.seconds, 1)} for v in self.voices.catalogue()]
        return {"type": kind, "name": self.name, "version": 1, "slots": self.engines, "voices": voices,
                "seconds": round(self.seconds, 1), "languages": sorted(LANGUAGES)}

    async def send(self, msg):
        """Sends to the relay; an answer for a connection that dropped meanwhile is lost (the relay
        queues the job again when the worker is back, and the clip is in the cache by then)."""
        try:
            if self.ws is not None:
                await self.ws.send(json.dumps(msg, ensure_ascii=False))
        except websockets.exceptions.ConnectionClosed:
            log.info("answer to %s lost: connection closed", msg.get("id"))

    async def run_job(self, fn, *args):
        self.busy += 1
        try:
            return await asyncio.get_running_loop().run_in_executor(self.pool, fn, *args)
        finally:
            self.busy -= 1

    # ---- jobs from the relay

    async def speak(self, msg):
        text = " ".join(str(msg.get("text", "")).split())
        if not text or len(text) > MAX_TEXT:
            return await self.send({"type": "error", "id": msg["id"], "status": 400, "message": "text empty or too long"})
        try:
            voice = self.voices.resolve(msg.get("voice"))
        except UnknownVoice as e:
            return await self.send({"type": "error", "id": msg["id"], "status": 404, "message": str(e)})
        trial = clean_settings(msg.get("settings"))  # settings tried in the configurator
        # a line of a pack spoken in another language than the voice's ("GPS ok" in English by a German voice)
        language = msg.get("language") if msg.get("language") in LANGUAGES else voice.language
        key = {**trial, "language": language} if language != voice.language else trial
        t0 = self.last_user = time.monotonic()
        data = self.cache.get(voice, text, key)
        cached = data is not None
        if not cached:
            data, info = await self.run_job(engine.job_speak, str(voice.conds), language, text,
                                            {**voice.settings, **trial})
            self.cache.put(voice, text, data, key)
            self.seconds = 0.8 * self.seconds + 0.2 * info["ms"] / 1000
            if info["attempts"] > 1:
                log.info("%s %r: %d attempts (ran on past its text)", voice.name, text, info["attempts"])
        self.voices.touch(voice)
        ms = int((time.monotonic() - t0) * 1000)
        log.info("%s%s %r %d B %d ms%s", voice.name, "" if language == voice.language else f" [{language}]", text,
                 len(data), ms, " (cache)" if cached else "")
        await self.send({"type": "audio", "id": msg["id"], "mime": "audio/mpeg", "cached": cached, "ms": ms,
                         "spoken": prepare(text, language), "data": base64.b64encode(data).decode()})

    async def clone(self, msg):
        if msg.get("consent") is not True:
            return await self.send({"type": "error", "id": msg["id"], "status": 400, "message": "consent missing"})
        try:
            wav = base64.b64decode(msg.get("wav", ""), validate=True)
        except ValueError:
            return await self.send({"type": "error", "id": msg["id"], "status": 400, "message": "wav is not base64"})
        self.last_user = time.monotonic()
        private_id, staging = self.voices.stage_private()
        try:
            seconds = await self.run_job(engine.job_clone, wav, str(staging / "conds.pt"))
            self.voices.commit_private(private_id, staging, {"seconds": round(seconds, 1), "consent": True})
        except engine.ReferenceError as e:
            self.voices.discard(staging)
            return await self.send({"type": "error", "id": msg["id"], "status": 400, "message": str(e)})
        except Exception:
            self.voices.discard(staging)
            raise
        log.info("own voice created (%.1f s reference)", seconds)
        await self.send({"type": "cloned", "id": msg["id"], "voice_id": private_id, "seconds": round(seconds, 1)})

    async def forget(self, msg):
        try:
            deleted = self.voices.delete_private(msg.get("voice_id"))
        except UnknownVoice as e:
            return await self.send({"type": "error", "id": msg["id"], "status": 400, "message": str(e)})
        self.cache.drop(f"own-{msg['voice_id']}")
        log.info("own voice deleted" if deleted else "own voice to delete did not exist")
        await self.send({"type": "forgot", "id": msg["id"], "deleted": deleted})

    async def publish(self, msg):
        """An own voice becomes a template for everyone (its speaker ticked the consent)."""
        if msg.get("consent") is not True:
            return await self.send({"type": "error", "id": msg["id"], "status": 400, "message": "consent missing"})
        try:
            name = await asyncio.to_thread(self.voices.publish, msg.get("voice_id"), msg.get("name"), msg.get("locale"),
                                           msg.get("gender"), msg.get("settings"), msg.get("audio"))
        except VoiceRefused as e:
            return await self.send({"type": "error", "id": msg["id"], "status": 409, "message": str(e)})
        except UnknownVoice as e:
            return await self.send({"type": "error", "id": msg["id"], "status": 404, "message": str(e)})
        log.info("template published: %s", name)
        await self.send(self.hello("voices"))  # listed at once, before the answer
        await self.send({"type": "published", "id": msg["id"], "voice": name})

    async def pack_put(self, msg):
        data = base64.b64decode(msg.get("data", ""), validate=True)
        done = await asyncio.to_thread(self.packs.put_part, msg.get("pack"), int(msg.get("part", -1)),
                                       int(msg.get("parts", 0)), data)
        reply = {"type": "pack_stored", "id": msg["id"], "done": done is not None}
        if done:
            reply["size"], reply["sha256"] = done
            log.info("pack %s stored: %d bytes", msg["pack"], done[0])
        await self.send(reply)

    async def pack_read(self, msg):
        data, size = await asyncio.to_thread(self.packs.read, msg.get("pack"), int(msg.get("offset", 0)),
                                             int(msg.get("length", 2**20)))
        await self.send({"type": "pack_data", "id": msg["id"], "size": size, "data": base64.b64encode(data).decode()})

    async def pack_sample(self, msg):
        paths = [str(p) for p in (msg.get("paths") or [])][:20]
        clips = await asyncio.to_thread(self.packs.clips, msg.get("pack"), paths)
        await self.send({"type": "pack_clips", "id": msg["id"],
                         "clips": {p: base64.b64encode(b).decode() for p, b in clips.items()}})

    async def pack_delete(self, msg):
        deleted = await asyncio.to_thread(self.packs.delete, msg.get("pack"))
        log.info("pack %s %s", msg.get("pack"), "deleted" if deleted else "to delete not found")
        await self.send({"type": "pack_deleted", "id": msg["id"], "deleted": deleted})

    async def handle(self, msg):
        kinds = {"speak": self.speak, "clone": self.clone, "forget": self.forget, "publish": self.publish,
                 "pack_put": self.pack_put, "pack_read": self.pack_read, "pack_sample": self.pack_sample,
                 "pack_delete": self.pack_delete}
        try:
            await kinds[msg["type"]](msg)
        except PackError as e:
            await self.send({"type": "error", "id": msg.get("id"), "status": e.status, "message": str(e)})
        except (ValueError, KeyError) as e:
            await self.send({"type": "error", "id": msg.get("id"), "status": 400, "message": f"bad request: {e}"})
        except Exception as e:
            log.exception("%s failed", msg.get("type"))
            await self.send({"type": "error", "id": msg.get("id"), "status": 500, "message": f"worker: {type(e).__name__}"})

    # ---- background: voice list changes, idle prerendering, cache size

    async def watch_voices(self):
        last = None
        while True:
            current = self.hello("voices")
            if last is not None and current["voices"] != last["voices"]:
                await self.send(current)
            last = current
            await asyncio.sleep(30)

    def prerender_todo(self):
        """(voice, text) pairs of the public voices' pack texts that are not cached yet."""
        packs = self.catalogue["packs"]
        for entry in self.voices.catalogue():
            locale = entry["locale"]
            pack = next((p for p in packs if p["tts"] == locale), None) or \
                next((p for p in packs if p["tts"].split("-")[0] == locale.split("-")[0]), None)
            if not pack:
                continue
            voice = self.voices.resolve(entry["name"])
            for text in self.catalogue["texts"].get(pack["lang"], []):
                if not self.cache.has(voice, text):
                    yield voice, text

    async def prerender_loop(self):
        """One clip at a time, only after 20 s without requests: users never wait for more than the
        clip in progress."""
        while True:
            if self.busy or time.monotonic() - self.last_user < 20:
                await asyncio.sleep(5)
                continue
            todo = await asyncio.to_thread(lambda: next(self.prerender_todo(), None))
            if todo is None:
                await asyncio.sleep(600)
                continue
            voice, text = todo
            data, info = await self.run_job(engine.job_speak, str(voice.conds), voice.language, text, voice.settings)
            self.cache.put(voice, text, data)
            log.info("prerendered %s %r in %d ms", voice.name, text, info["ms"])

    async def prune_loop(self):
        while True:
            before, removed = await asyncio.to_thread(self.cache.prune)
            if removed:
                log.info("cache: %d clips removed (%d MB before)", removed, before // 2**20)
            await asyncio.sleep(3600)

    # ---- the connection

    async def serve(self):
        backoff = 1
        while True:
            try:
                async with connect(self.relay_url, additional_headers={"Authorization": f"Bearer {self.secret}"},
                                   max_size=8 * 2**20, ping_interval=30, ping_timeout=30, open_timeout=20) as ws:
                    self.ws = ws
                    await self.send(self.hello())
                    log.info("connected to %s with %d public voices", self.relay_url, len(self.hello()["voices"]))
                    backoff = 1
                    async for raw in ws:
                        msg = json.loads(raw)
                        asyncio.create_task(self.handle(msg))
            except (OSError, websockets.exceptions.WebSocketException, asyncio.TimeoutError) as e:
                log.warning("relay connection: %s; again in %d s", e, backoff)
            finally:
                self.ws = None
            await asyncio.sleep(backoff + random.random())
            backoff = min(backoff * 2, 60)

    async def main(self):
        log.info("loading %d engine(s) ...", self.engines)
        loop = asyncio.get_running_loop()
        await asyncio.gather(*(loop.run_in_executor(self.pool, engine.job_ready) for _ in range(self.engines)))
        log.info("engines ready (cache tag %s)", engine.TAG)
        tasks = [self.watch_voices(), self.prune_loop(), self.serve()]
        if self.prerender:
            tasks.append(self.prerender_loop())
        await asyncio.gather(*tasks)


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    asyncio.run(Worker().main())
