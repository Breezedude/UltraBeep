"""Chatterbox Multilingual V3 on the CPU, set up as the benchmark found best (2026-10-15, Ryzen 7 8745HS):

* T3 (text -> speech tokens) in bfloat16 under autocast: 30 % faster than fp32, same words.
* Turbo's distilled "meanflow" decoder (2 flow steps instead of 10) for the speech tokens: S3Gen
  0.6 s instead of 4 s per clip, the speech recognition hears the same.
* 6 to 8 torch threads; more make a clip slower.

One Engine per process. The worker runs it in a process pool (job_* functions), the lab in-process.
"""
import hashlib
import io
import os
import tempfile
import time
import wave
from pathlib import Path

os.environ.setdefault("HF_HUB_OFFLINE", "1")
os.environ.setdefault("TQDM_DISABLE", "1")

import av
import librosa
import numpy as np
import pyloudnorm
import soundfile as sf
import torch
from safetensors.torch import load_file

from chatterbox.models.s3gen import S3Gen
from chatterbox.models.s3gen.const import S3GEN_SIL
from chatterbox.mtl_tts import ChatterboxMultilingualTTS, Conditionals

from textnorm import plausible_seconds, prepare

SR = 24000
TOKENS_PER_S = 25
# Part of every cache key: change it when the audio for the same voice and text changes.
TAG = "chatterbox-mtl-v3/t3-bf16/meanflow-2/textnorm-1/length-2/mp3-64k"
DEFAULT_SETTINGS = {"exaggeration": 0.5, "cfg_weight": 0.5, "temperature": 0.8}
MAX_VARIANT = 99  # takes of a text besides the default one (variant 0)
MIN_REFERENCE_S = 5.0
MAX_REFERENCE_S = 15.0


class ReferenceError(ValueError):
    """A reference clip that cannot become a voice; the message is shown to the user."""


class Engine:
    def __init__(self, model_dir, threads):
        model_dir = Path(model_dir)
        try:
            torch.set_num_interop_threads(1)
        except RuntimeError:
            pass  # already set in this process
        self.threads = threads
        torch.set_num_threads(threads)
        model = ChatterboxMultilingualTTS.from_local(model_dir / "chatterbox", "cpu", t3_model="v3")

        # T3 in bfloat16; the cap on new tokens stops a clip that runs on after its text.
        model.t3.to(dtype=torch.bfloat16)
        t3_inference = model.t3.inference
        self.token_cap = 1000

        def t3_bf16(*a, **kw):
            kw["max_new_tokens"] = min(kw.get("max_new_tokens") or self.token_cap, self.token_cap)
            with torch.autocast("cpu", dtype=torch.bfloat16):
                return t3_inference(*a, **kw)
        model.t3.inference = t3_bf16

        # The original decoder stays for embedding references (the built-in voice and every saved one
        # were made with it); speech goes through the meanflow decoder.
        self.s3gen_ref = model.s3gen
        meanflow = S3Gen(meanflow=True)
        meanflow.load_state_dict(load_file(model_dir / "chatterbox-turbo" / "s3gen_meanflow.safetensors"), strict=True)
        meanflow.eval()
        meanflow_inference = meanflow.inference

        def decode(speech_tokens, ref_dict, **kw):
            # Like Turbo: trailing silence tokens keep the 2-step decoder from smearing the last word;
            # ChatterboxMultilingualTTS trims the waveform back to the real tokens.
            silence = torch.full((3,), S3GEN_SIL, dtype=speech_tokens.dtype)
            return meanflow_inference(speech_tokens=torch.cat([speech_tokens, silence]), ref_dict=ref_dict,
                                      n_cfm_timesteps=2, **kw)
        meanflow.inference = decode
        model.s3gen = meanflow
        self.model = model
        self.builtin = model.conds
        self._conds = {}

    def conds(self, path):
        """The conditioning of a voice file, kept per file and modification time."""
        path = Path(path)
        key = (str(path), path.stat().st_mtime_ns)
        if key not in self._conds:
            if len(self._conds) > 64:
                self._conds.clear()
            self._conds[key] = Conditionals.load(path)
        return self._conds[key]

    def speak(self, conds, language, text, settings=None, seed=None, variant=0):
        """Synthesizes `text` (as written; numbers are spelled out here). Returns (float32 wav, info).
        The seed comes from the language and the text, so a clip comes out the same every time; variant n
        (1 to MAX_VARIANT) gives another take of the same text, reproducible as well (the model samples
        its speech tokens, and the seed decides the draws). A clip longer than plausible for its text is
        generated again with another seed (twice at most)."""
        settings = {**DEFAULT_SETTINGS, **(settings or {})}
        spoken = prepare(text, language)
        limit = plausible_seconds(spoken)
        self.token_cap = int(limit * TOKENS_PER_S) + 10
        if seed is None:
            base = f"{language}|{text}" + (f"|{variant}" if variant else "")
            seed = int.from_bytes(hashlib.sha256(base.encode()).digest()[:4], "big")
        t3_cond = conds.t3.__class__(**vars(conds.t3)).to(dtype=torch.bfloat16)
        torch.set_num_threads(self.threads)  # per OS thread: the lab calls this from Gradio's worker threads
        best = None
        t0 = time.perf_counter()
        for attempt in range(3):
            self.model.conds = Conditionals(t3_cond, conds.gen)
            torch.manual_seed(seed + attempt)
            wav = self.model.generate(spoken, language_id=language, **settings).squeeze(0).numpy()
            if best is None or len(wav) < len(best):
                best = wav
            if len(wav) / SR < limit:
                break
        info = {"spoken": spoken, "seconds": len(best) / SR, "ms": int((time.perf_counter() - t0) * 1000),
                "attempts": attempt + 1, "seed": seed}
        return best.astype(np.float32), info

    def conds_from_wav(self, wav):
        """The conditioning of a reference (float32 mono at SR): trimmed, 5 to 15 s of sound,
        loudness-normalised like Turbo does (-27 LUFS)."""
        wav, _ = librosa.effects.trim(wav, top_db=40)
        if len(wav) < MIN_REFERENCE_S * SR:
            raise ReferenceError(f"The recording has {len(wav) / SR:.1f} s of speech; at least "
                                 f"{MIN_REFERENCE_S:.0f} s are needed (8 to 12 s work best).")
        wav = wav[: int(MAX_REFERENCE_S * SR)]
        meter = pyloudnorm.Meter(SR)
        loudness = meter.integrated_loudness(wav)
        if not np.isfinite(loudness):
            raise ReferenceError("The recording is silent.")
        wav = np.clip(pyloudnorm.normalize.loudness(wav, loudness, -27.0), -1.0, 1.0).astype(np.float32)
        with tempfile.NamedTemporaryFile(suffix=".wav") as f:
            sf.write(f.name, wav, SR)
            decoder, self.model.s3gen = self.model.s3gen, self.s3gen_ref
            try:
                self.model.prepare_conditionals(f.name, exaggeration=DEFAULT_SETTINGS["exaggeration"])
            finally:
                self.model.s3gen = decoder
        return self.model.conds, wav


def encode_mp3(wav, bitrate=64000):
    """MP3 (mono, SR) of float audio: what the relay serves for Edge voices too."""
    buf = io.BytesIO()
    with av.open(buf, "w", format="mp3") as out:
        stream = out.add_stream("libmp3lame", rate=SR)
        stream.bit_rate = bitrate
        frame = av.AudioFrame.from_ndarray(np.clip(wav, -1, 1).astype(np.float32)[None, :], format="fltp", layout="mono")
        frame.sample_rate = SR
        for packet in stream.encode(frame):
            out.mux(packet)
        for packet in stream.encode(None):
            out.mux(packet)
    return buf.getvalue()


def decode_any(path):
    """Any audio FFmpeg reads, as mono float32 at SR. Only for files of the operator (the lab):
    uploads from the public arrive as plain WAV (read_wav)."""
    with av.open(str(path)) as container:
        resampler = av.AudioResampler(format="flt", layout="mono", rate=SR)
        chunks = [o.to_ndarray().reshape(-1) for frame in container.decode(audio=0) for o in resampler.resample(frame)]
        chunks += [o.to_ndarray().reshape(-1) for o in resampler.resample(None)]
    if not chunks:
        raise ReferenceError("The file contains no audio.")
    return np.concatenate(chunks).astype(np.float32)


def read_wav(data, max_seconds=30):
    """A public upload: 16-bit PCM WAV, mono, 16 to 48 kHz (the configurator converts whatever the
    browser recorded or opened). Parsed with the standard library only; returns float32 at SR."""
    try:
        with wave.open(io.BytesIO(data)) as w:
            channels, width, rate, frames = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
            if channels != 1 or width != 2 or not 16000 <= rate <= 48000:
                raise ReferenceError("Expected 16-bit mono WAV at 16 to 48 kHz.")
            if frames > max_seconds * rate:
                raise ReferenceError(f"The recording is longer than {max_seconds} s.")
            pcm = np.frombuffer(w.readframes(frames), dtype="<i2").astype(np.float32) / 32768.0
    except (wave.Error, EOFError) as e:
        raise ReferenceError(f"Not a WAV file ({e}).")
    return pcm if rate == SR else librosa.resample(pcm, orig_sr=rate, target_sr=SR).astype(np.float32)


# ---- process pool jobs (the worker) ---------------------------------------------------------------

_engine = None


def init_process(model_dir, threads):
    global _engine
    _engine = Engine(model_dir, threads)


def job_ready():
    return _engine is not None


def job_speak(conds_path, language, text, settings, variant=0):
    wav, info = _engine.speak(_engine.conds(conds_path), language, text, settings, variant=variant)
    return encode_mp3(wav), info


def job_clone(wav_bytes, out_path):
    """Turns an uploaded WAV into a voice file at out_path; returns the seconds of reference used."""
    conds, ref = _engine.conds_from_wav(read_wav(wav_bytes))
    conds.save(Path(out_path))
    return len(ref) / SR
