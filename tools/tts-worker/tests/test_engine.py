"""The real engine (skipped where the models are missing): a clip, the MP3, a voice from a WAV.
Slow (about a minute: the model loads). TTS_MODEL_DIR defaults to /opt/tts/models."""
import io
import os
import sys
import tempfile
import unittest
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

MODEL_DIR = Path(os.environ.get("TTS_MODEL_DIR", "/opt/tts/models"))


@unittest.skipUnless((MODEL_DIR / "chatterbox" / "t3_mtl23ls_v3.safetensors").is_file(), "no models here")
class RealEngine(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        import engine
        cls.engine = engine
        cls.e = engine.Engine(MODEL_DIR, int(os.environ.get("THREADS", "8")))

    def test_speak_and_clone(self):
        engine = self.engine
        wav, info = self.e.speak(self.e.builtin, "de", "-100 Meter")
        self.assertEqual(info["spoken"], "minus einhundert Meter")
        self.assertTrue(0.6 < info["seconds"] < 3.0, info)
        mp3 = engine.encode_mp3(wav)
        self.assertTrue(mp3[:3] == b"ID3" or mp3[0] == 0xFF, mp3[:4])
        with engine.av.open(io.BytesIO(mp3)) as c:
            self.assertEqual(c.streams.audio[0].rate, engine.SR)

        # a voice from a 16-bit WAV, as the configurator uploads it: the built-in voice speaking 6 s
        ref = []
        for text in ("Halte die Taste zum Zurücksetzen auf Werkseinstellungen gedrückt", "Bitte Sprachpaket aktualisieren",
                     "Auslieferungszustand wird geladen, bitte warten"):
            ref.append(self.e.speak(self.e.builtin, "de", text)[0])
        pcm = (engine.np.concatenate(ref) * 32767).astype("<i2")
        buf = io.BytesIO()
        with wave.open(buf, "wb") as w:
            w.setnchannels(1), w.setsampwidth(2), w.setframerate(engine.SR), w.writeframes(pcm.tobytes())
        with tempfile.TemporaryDirectory() as tmp:
            engine._engine = self.e
            seconds = engine.job_clone(buf.getvalue(), f"{tmp}/conds.pt")
            self.assertGreater(seconds, 5)
            mp3, info = engine.job_speak(f"{tmp}/conds.pt", "de", "Thermikausstieg", {})
            self.assertGreater(len(mp3), 2000)
        with self.assertRaises(engine.ReferenceError):
            engine.read_wav(b"RIFF\x00\x00\x00\x00WAVEjunk")
        short = io.BytesIO()
        with wave.open(short, "wb") as w:
            w.setnchannels(1), w.setsampwidth(2), w.setframerate(engine.SR), w.writeframes(pcm[: engine.SR * 2].tobytes())
        with self.assertRaises(engine.ReferenceError):
            self.e.conds_from_wav(engine.read_wav(short.getvalue()))


if __name__ == "__main__":
    unittest.main()
