"""Voices, cache and the relay protocol of the worker, with a fake engine (no model needed).
python -m unittest discover -s tests   (from tools/tts-worker, in the worker's venv)"""
import asyncio
import base64
import json
import os
import sys
import tempfile
import unittest
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import websockets  # noqa: E402
from websockets.asyncio.server import serve  # noqa: E402

import engine  # noqa: E402
from cache import Cache  # noqa: E402
from packs import PackError, Packs  # noqa: E402
from voices import UnknownVoice, VoiceRefused, Voices  # noqa: E402

SECRET = "test-secret-0123456789"


def public_voice(root, slug, public=True, locales=("de-DE",), settings=None):
    folder = Path(root) / slug
    folder.mkdir(parents=True)
    (folder / "conds.pt").write_bytes(b"conds")
    (folder / "voice.json").write_text(json.dumps({"name": slug.title(), "gender": "Female", "locales": list(locales),
                                                   "public": public, "settings": settings or {}}))


class VoicesAndCache(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        public_voice(root / "voices", "anna", locales=("de-DE", "en-GB", "cs-CZ"), settings={"exaggeration": 0.6})
        public_voice(root / "voices", "hidden", public=False)
        self.voices = Voices(root / "voices", root / "private")
        self.cache = Cache(root / "cache", 1000, "tag")

    def tearDown(self):
        self.tmp.cleanup()

    def test_catalogue_lists_public_voices_in_spoken_locales(self):
        names = [v["name"] for v in self.voices.catalogue()]
        self.assertEqual(names, ["cb:de-DE-anna", "cb:en-GB-anna"])  # cs-CZ: Chatterbox has no Czech

    def test_resolve(self):
        v = self.voices.resolve("cb:en-GB-anna")
        self.assertEqual((v.language, v.key, v.settings), ("en", "anna", {"exaggeration": 0.6}))
        for bad in ("cb:fr-FR-anna", "cb:de-DE-hidden", "de-DE-KatjaNeural", "cb:de-DE-../x", f"cb:de-DE-own-{'a' * 26}"):
            with self.assertRaises(UnknownVoice, msg=bad):
                self.voices.resolve(bad)

    def test_private_voice_lifecycle(self):
        private_id, staging = self.voices.stage_private()
        (staging / "conds.pt").write_bytes(b"x")
        self.voices.commit_private(private_id, staging, {"consent": True})
        v = self.voices.resolve(f"cb:fr-FR-own-{private_id}")
        self.assertEqual((v.language, v.key), ("fr", f"own-{private_id}"))
        self.assertEqual(self.voices.catalogue()[0]["name"], "cb:de-DE-anna")  # never listed
        self.assertTrue(self.voices.delete_private(private_id))
        self.assertFalse(self.voices.delete_private(private_id))
        with self.assertRaises(UnknownVoice):
            self.voices.delete_private("../voices")

    def test_publish(self):
        private_id, staging = self.voices.stage_private()
        (staging / "conds.pt").write_bytes(b"x")
        self.voices.commit_private(private_id, staging, {"consent": True})
        with self.assertRaises(VoiceRefused):  # a public voice "Anna" speaks German already
            self.voices.publish(private_id, " anna ", "de-AT", "Female", {}, {})
        name = self.voices.publish(private_id, "Jörg  Groß", "de-DE", "Male", {"exaggeration": 9, "x": 1},
                                   {"pitch": 80, "rate": -5, "bandpass": False})
        self.assertEqual(name, "cb:de-DE-jorg-gross")
        entry = next(v for v in self.voices.catalogue() if v["name"] == name)
        self.assertEqual((entry["friendly"], entry["gender"]), ("Jörg Groß", "Male"))
        self.assertEqual(entry["defaults"], {"pitch": 50, "rate": -5, "bandpass": False})
        self.assertEqual(self.voices.resolve(name).settings, {"exaggeration": 2.0})
        self.assertEqual(self.voices.publish(private_id, "Anna", "fr-FR", "Female", {}, {}), "cb:fr-FR-anna-2")  # no French Anna yet
        for bad in ("A", "x" * 40, "bell\x07"):
            with self.assertRaises(VoiceRefused, msg=bad):
                self.voices.publish(private_id, bad, "de-DE", "Male", {}, {})

    def test_packs(self):
        packs = Packs(Path(self.tmp.name) / "packs", 3000, 10000)
        head = bytearray(1024)
        head[54:62], head[510:512] = b"FAT16   ", b"\x55\xaa"
        pid = "a" * 20
        with self.assertRaises(PackError):
            packs.put_part(pid, 0, 2, b"junk" * 200)  # no FAT16 image
        self.assertIsNone(packs.put_part(pid, 0, 2, bytes(head)))
        with self.assertRaises(PackError):
            packs.put_part(pid, 2, 3, b"x")  # part 1 expected (a part 0 would start the upload anew)
        size, sha = packs.put_part(pid, 1, 2, b"rest")
        self.assertEqual(size, 1028)
        self.assertEqual(packs.read(pid, 1024, 100), (b"rest", 1028))
        with self.assertRaises(PackError):
            packs.put_part(pid, 0, 1, bytes(head))  # stored already
        with self.assertRaises(PackError):
            packs.put_part("b" * 20, 0, 1, bytes(head) * 3)  # larger than 3000 bytes
        with self.assertRaises(PackError):
            packs.read("../etc/passwd", 0, 10)
        self.assertTrue(packs.delete(pid))
        with self.assertRaises(PackError):
            packs.read(pid, 0, 10)

    def test_cache(self):
        v = self.voices.resolve("cb:de-DE-anna")
        self.assertIsNone(self.cache.get(v, "Thermik"))
        self.cache.put(v, "Thermik", b"mp3" * 100)
        self.assertEqual(self.cache.get(v, "Thermik"), b"mp3" * 100)
        self.assertIsNone(self.cache.get(v, "Thermik", {"temperature": 0.5}))  # tried settings: their own clip
        self.assertNotEqual(self.cache.path(v, "Thermik"), self.cache.path(self.voices.resolve("cb:en-GB-anna"), "Thermik"))
        for i in range(5):
            self.cache.put(v, f"t{i}", b"x" * 300)
        before, removed = self.cache.prune()
        self.assertGreater(before, 1000)
        self.assertGreater(removed, 0)
        self.cache.drop("anna")
        self.assertIsNone(self.cache.get(v, "t4"))


def fake_speak(conds_path, language, text, settings):
    extra = json.dumps(settings, sort_keys=True) if settings else ""
    return f"MP3:{language}:{text}{extra}".encode(), {"ms": 50, "attempts": 1, "spoken": text, "seconds": 1.0}


def fake_clone(wav_bytes, out_path):
    if not wav_bytes.startswith(b"RIFF"):
        raise engine.ReferenceError("Not a WAV file.")
    Path(out_path).write_bytes(b"conds")
    return 9.0


class Protocol(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        public_voice(root / "voices", "anna")
        self.inbox = asyncio.Queue()
        self.conn = asyncio.get_running_loop().create_future()

        async def relay(ws):
            self.auth = ws.request.headers.get("Authorization")
            if not self.conn.done():
                self.conn.set_result(ws)
            async for raw in ws:
                await self.inbox.put(json.loads(raw))

        self.server = await serve(relay, "127.0.0.1", 0)
        port = self.server.sockets[0].getsockname()[1]
        os.environ.update({"RELAY_URL": f"ws://127.0.0.1:{port}/v1/worker", "WORKER_SECRET": SECRET,
                           "VOICE_DIR": str(root / "voices"), "PRIVATE_DIR": str(root / "private"),
                           "CACHE_DIR": str(root / "cache"), "PACK_DIR": str(root / "packs"), "PRERENDER": "0"})
        self.patches = (engine.job_speak, engine.job_clone)
        engine.job_speak, engine.job_clone = fake_speak, fake_clone
        import worker
        self.worker = worker.Worker(pool=ThreadPoolExecutor(1))
        self.task = asyncio.create_task(self.worker.serve())
        self.ws = await asyncio.wait_for(self.conn, 10)

    async def asyncTearDown(self):
        self.task.cancel()
        self.server.close()
        engine.job_speak, engine.job_clone = self.patches
        self.tmp.cleanup()

    async def ask(self, msg):
        await self.ws.send(json.dumps(msg))
        while True:
            reply = await asyncio.wait_for(self.inbox.get(), 10)
            if reply.get("id") == msg["id"]:
                return reply

    async def test_session(self):
        hello = await asyncio.wait_for(self.inbox.get(), 10)
        self.assertEqual(self.auth, f"Bearer {SECRET}")
        self.assertEqual(hello["type"], "hello")
        self.assertEqual([v["name"] for v in hello["voices"]], ["cb:de-DE-anna"])
        self.assertIn("de", hello["languages"])

        r = await self.ask({"type": "speak", "id": "1", "voice": "cb:de-DE-anna", "text": "Thermik"})
        self.assertEqual((r["type"], base64.b64decode(r["data"]), r["cached"]), ("audio", b"MP3:de:Thermik", False))
        r = await self.ask({"type": "speak", "id": "2", "voice": "cb:de-DE-anna", "text": "Thermik"})
        self.assertTrue(r["cached"])
        r = await self.ask({"type": "speak", "id": "3", "voice": "cb:de-DE-nobody", "text": "Thermik"})
        self.assertEqual((r["type"], r["status"]), ("error", 404))
        r = await self.ask({"type": "speak", "id": "4", "voice": "cb:de-DE-anna", "text": " "})
        self.assertEqual(r["status"], 400)

        wav = base64.b64encode(b"RIFF....WAVE").decode()
        r = await self.ask({"type": "clone", "id": "5", "wav": wav})
        self.assertEqual((r["type"], r["status"]), ("error", 400))  # no consent
        r = await self.ask({"type": "clone", "id": "6", "wav": base64.b64encode(b"junk").decode(), "consent": True})
        self.assertEqual((r["type"], r["message"]), ("error", "Not a WAV file."))
        self.assertEqual(list(Path(os.environ["PRIVATE_DIR"]).iterdir()), [])  # staging cleaned up
        r = await self.ask({"type": "clone", "id": "7", "wav": wav, "consent": True})
        self.assertEqual(r["type"], "cloned")
        own = f"cb:en-GB-own-{r['voice_id']}"
        r = await self.ask({"type": "speak", "id": "8", "voice": own, "text": "Lift"})
        self.assertEqual(base64.b64decode(r["data"]), b"MP3:en:Lift")
        clips = Path(os.environ["CACHE_DIR"]) / f"own-{own.rsplit('-', 1)[1]}"
        self.assertTrue(clips.is_dir())
        r = await self.ask({"type": "speak", "id": "8b", "voice": own, "text": "Lift", "settings": {"temperature": 0.5}})
        self.assertEqual(base64.b64decode(r["data"]), b'MP3:en:Lift{"temperature": 0.5}')
        self.assertFalse(r["cached"])

        r = await self.ask({"type": "publish", "id": "p1", "voice_id": own.rsplit("-", 1)[1], "name": "Lisa",
                            "locale": "de-DE", "gender": "Female", "audio": {"rate": 4}})
        self.assertEqual(r["status"], 400)  # no consent
        await self.ws.send(json.dumps({"type": "publish", "id": "p2", "voice_id": own.rsplit("-", 1)[1], "name": "Lisa",
                                       "locale": "de-DE", "gender": "Female", "audio": {"rate": 4}, "consent": True}))
        update = await asyncio.wait_for(self.inbox.get(), 10)  # the voice list comes before the answer
        self.assertEqual(update["type"], "voices")
        self.assertIn("cb:de-DE-lisa", [v["name"] for v in update["voices"]])
        r = await asyncio.wait_for(self.inbox.get(), 10)
        self.assertEqual((r["type"], r["voice"]), ("published", "cb:de-DE-lisa"))
        r = await self.ask({"type": "publish", "id": "p3", "voice_id": own.rsplit("-", 1)[1], "name": "lisa",
                            "locale": "de-DE", "gender": "Female", "consent": True})
        self.assertEqual(r["status"], 409)

        r = await self.ask({"type": "forget", "id": "9", "voice_id": own.rsplit("-", 1)[1]})
        self.assertEqual((r["type"], r["deleted"]), ("forgot", True))
        self.assertFalse(clips.exists())
        r = await self.ask({"type": "speak", "id": "10", "voice": own, "text": "Lift"})
        self.assertEqual(r["status"], 404)
        r = await self.ask({"type": "speak", "id": "11", "voice": "cb:de-DE-lisa", "text": "Lift"})
        self.assertEqual(r["type"], "audio")  # the template keeps its own copy

        head = bytearray(2048)
        head[54:62], head[510:512] = b"FAT16   ", b"\x55\xaa"
        pid = "p" * 20
        for i in range(2):
            r = await self.ask({"type": "pack_put", "id": f"u{i}", "pack": pid, "part": i, "parts": 2,
                                "data": base64.b64encode(bytes(head[i * 1024:(i + 1) * 1024])).decode()})
        self.assertEqual((r["type"], r["done"], r["size"]), ("pack_stored", True, 2048))
        r = await self.ask({"type": "pack_read", "id": "r", "pack": pid, "offset": 1024, "length": 4096})
        self.assertEqual((base64.b64decode(r["data"]), r["size"]), (bytes(head[1024:]), 2048))
        r = await self.ask({"type": "pack_put", "id": "u9", "pack": "q" * 20, "part": 0, "parts": 1,
                            "data": base64.b64encode(b"no image").decode()})
        self.assertEqual(r["status"], 400)
        r = await self.ask({"type": "pack_delete", "id": "d", "pack": pid})
        self.assertTrue(r["deleted"])


if __name__ == "__main__":
    unittest.main()
