"""Chatterbox lab: the operator's test bench and voice editor in the home LAN (Gradio).

Speaks with the same engine as the worker, so what you hear is what the relay serves, and turns
reference clips into the public voices of the worker (VOICE_DIR/<slug>/{conds.pt, reference.wav,
voice.json}): name, gender, the locales it is offered in, whether it is offered at all, and its
generation settings. The worker picks up changes within 30 s; clips of a voice whose reference or
settings changed are made anew.

Run: /opt/tts/venv/bin/python lab.py   (listens on 0.0.0.0:7860; no login: LAN only)
"""
import json
import os
import random
import re
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path

os.environ.setdefault("GRADIO_ANALYTICS_ENABLED", "False")

import gradio as gr
import soundfile as sf
from chatterbox.mtl_tts import Conditionals

import engine
from textnorm import language_of

MODEL_DIR = Path(os.environ.get("MODEL_DIR", "/opt/tts/models"))
VOICE_DIR = Path(os.environ.get("VOICE_DIR", "/var/lib/tts/voices"))
PRIVATE_DIR = Path(os.environ.get("PRIVATE_DIR", "/var/lib/tts/private"))
PORT = int(os.environ.get("LAB_PORT", "7860"))
THREADS = int(os.environ.get("THREADS") or min(8, len(os.sched_getaffinity(0))))
MAX_CHARS = 300
BUILTIN = "builtin"
LOCALES = ["de-DE", "de-AT", "de-CH", "en-GB", "en-US", "fr-FR", "it-IT", "es-ES", "pt-PT", "pt-BR", "nl-NL", "pl-PL",
           "fi-FI", "sv-SE", "da-DK", "nb-NO", "el-GR", "tr-TR", "ru-RU", "ja-JP", "ko-KR", "zh-CN", "ar-SA", "he-IL",
           "hi-IN", "ms-MY", "sw-KE"]

ENGINE = engine.Engine(MODEL_DIR, THREADS)
LOCK = threading.Lock()  # one synthesis at a time: the model swaps parts per clip

# ---- voices


def voice_names():
    VOICE_DIR.mkdir(parents=True, exist_ok=True)
    return [BUILTIN] + sorted(p.name for p in VOICE_DIR.iterdir() if (p / "conds.pt").is_file() and not p.name.startswith("."))


def voice_meta(slug):
    path = VOICE_DIR / slug / "voice.json"
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def voice_conds(slug):
    return ENGINE.builtin if slug == BUILTIN else ENGINE.conds(VOICE_DIR / slug / "conds.pt")


_clip = {}


def clip_conds(path):
    """Conditioning and reference audio of an uploaded clip (any format: the operator's own files)."""
    key = (path, os.path.getmtime(path))
    if key not in _clip:
        _clip.clear()
        conds, ref = ENGINE.conds_from_wav(engine.decode_any(path))
        _clip[key] = (Conditionals(conds.t3, conds.gen), ref)
    return _clip[key]


def describe(slug):
    if slug == BUILTIN:
        return "Built-in reference of the model (an English speaker); not offered to the configurator."
    m = voice_meta(slug)
    offered = ", ".join(m.get("locales", [])) or "no locale"
    state = f"**offered** in {offered}" if m.get("public") else f"not offered (would be: {offered})"
    settings = ", ".join(f"{k} {v}" for k, v in (m.get("settings") or {}).items()) or "default settings"
    return (f"{m.get('name', slug)} · {m.get('gender', '?')} · {state} · {settings} · "
            f"{m.get('seconds', '?')} s reference · {m.get('note', '')}")


# ---- actions


def synthesize(text, locale, voice, clip, exaggeration, cfg_weight, temperature, seed):
    text = (text or "").strip()[:MAX_CHARS]
    if not text:
        raise gr.Error("Enter a text.")
    language = language_of(locale)
    seed = int(seed or 0) or random.randrange(1, 2**31)
    settings = {"exaggeration": exaggeration, "cfg_weight": cfg_weight, "temperature": temperature}
    with LOCK:
        try:
            conds = clip_conds(clip)[0] if clip else voice_conds(voice)
        except engine.ReferenceError as e:
            raise gr.Error(str(e))
        wav, info = ENGINE.speak(conds, language, text, settings, seed=seed)
    tries = f" · {info['attempts']} attempts (ran on past the text)" if info["attempts"] > 1 else ""
    report = (f"**{info['ms'] / 1000:.1f} s** for {info['seconds']:.1f} s of audio "
              f"(real-time factor {info['ms'] / 1000 / max(info['seconds'], 0.1):.2f}){tries} · seed {seed}\n\n"
              f"Spoken text: `{info['spoken']}`  \nVoice: {'uploaded clip' if clip else voice}")
    return (engine.SR, wav), report


def save_voice(name, clip, voice, gender, locales, public, note, exaggeration, cfg_weight, temperature):
    """Saves the uploaded clip as a new voice, or - without a clip - the form's details for the selected one."""
    slug = re.sub(r"[^a-z0-9]+", "-", (name or "").lower()).strip("-")[:40]
    if not clip:
        if voice == BUILTIN:
            raise gr.Error("Upload a reference clip to make a voice, or pick a saved voice to change its details.")
        slug = voice
    if not slug or slug == BUILTIN or slug.startswith("own"):
        raise gr.Error("Give the voice a name (letters, digits).")
    meta = voice_meta(slug)
    folder = VOICE_DIR / slug
    with LOCK:
        if clip:
            try:
                conds, ref = clip_conds(clip)
            except engine.ReferenceError as e:
                raise gr.Error(str(e))
            folder.mkdir(parents=True, exist_ok=True)
            conds.save(folder / "conds.pt")
            sf.write(folder / "reference.wav", ref, engine.SR)
            meta["seconds"] = round(len(ref) / engine.SR, 1)
            meta["created"] = time.strftime("%Y-%m-%d %H:%M:%S")
        meta.update({"name": (name or meta.get("name") or slug).strip(), "gender": gender, "locales": list(locales or []),
                     "public": bool(public), "note": (note or "").strip(),
                     "settings": {"exaggeration": exaggeration, "cfg_weight": cfg_weight, "temperature": temperature}})
        (folder / "voice.json").write_text(json.dumps(meta, ensure_ascii=False, indent=1), encoding="utf-8")
    return gr.update(choices=voice_names(), value=slug), None, f"Saved **{slug}**: {describe(slug)}"


def delete_voice(voice):
    if voice == BUILTIN:
        raise gr.Error("Pick a saved voice; the built-in one stays.")
    trash = VOICE_DIR / ".trash"
    trash.mkdir(exist_ok=True)
    (VOICE_DIR / voice).rename(trash / f"{voice}-{time.strftime('%Y%m%d-%H%M%S')}")
    return gr.update(choices=voice_names(), value=BUILTIN), f"Moved **{voice}** to .trash."


def pick_voice(voice):
    """The form follows the selected voice: its details, settings and reference."""
    m = {} if voice == BUILTIN else voice_meta(voice)
    s = {**engine.DEFAULT_SETTINGS, **(m.get("settings") or {})}
    ref = VOICE_DIR / voice / "reference.wav"
    return (describe(voice), m.get("name", ""), m.get("gender", "Female"), m.get("locales", []), bool(m.get("public")),
            m.get("note", ""), s["exaggeration"], s["cfg_weight"], s["temperature"],
            str(ref) if voice != BUILTIN and ref.is_file() else None)


def private_count():
    n = sum(1 for p in PRIVATE_DIR.iterdir() if (p / "conds.pt").is_file()) if PRIVATE_DIR.is_dir() else 0
    return f"{n} private voices uploaded through the configurator (kept until their owners delete them)."


# ---- the packs shared from the configurator (their descriptions live in the relay's database)

RELAY_HTTP = os.environ.get("RELAY_HTTP", "https://voice.breezedude.de")
ADMIN_TOKEN = os.environ.get("ADMIN_TOKEN", "")


def relay_admin(method, path):
    req = urllib.request.Request(f"{RELAY_HTTP}{path}", method=method, headers={"Authorization": f"Bearer {ADMIN_TOKEN}"})
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            return json.loads(r.read())
    except urllib.error.HTTPError as e:
        raise gr.Error(f"relay: HTTP {e.code} {e.read()[:200].decode(errors='replace')}")
    except OSError as e:
        raise gr.Error(f"relay: {e}")


def list_packs():
    if not ADMIN_TOKEN:
        return "Set RELAY_HTTP and ADMIN_TOKEN (the relay's) in /etc/tts/worker.env to see the shared packs here."
    packs = relay_admin("GET", "/v1/admin/packs")["packs"]
    if not packs:
        return "No shared packs."
    rows = ["| id | pack | name | voice | made | clips | MB | downloads | changed texts | state |", "|" + "---|" * 10]
    for p in packs:
        rows.append(f"| `{p['id']}` | {p['lang']} | {p['name']} | {p['voice_label']} (`{p['voice']}`) | {p['created'][:16]} | "
                    f"{p['clips']} | {p['size'] / 2**20:.1f} | {p['downloads']} | {p['changed_texts']} | {p['state']} |")
    return "\n".join(rows)


def delete_pack(pack_id):
    pack_id = (pack_id or "").strip()
    if not re.fullmatch(r"[a-z0-9]{20}", pack_id):
        raise gr.Error("Paste the pack's id (20 characters) from the list.")
    relay_admin("DELETE", f"/v1/admin/packs/{pack_id}")
    return list_packs(), ""


EXAMPLES = [
    ["1230 Meter", "de-DE"], ["-100 Meter", "de-DE"], ["minus 1,5 Meter pro Sekunde", "de-DE"],
    ["47 Kilometer pro Stunde", "de-DE"], ["GPS ok", "de-DE"], ["Thermikausstieg", "de-DE"],
    ["Taste zum Bestätigen drücken", "de-DE"], ["Halte die Taste zum Zurücksetzen auf Werkseinstellungen gedrückt", "de-DE"],
    ["1230 metres", "en-GB"], ["minus 1.5 metres per second", "en-GB"], ["Press the button to confirm", "en-GB"],
]

THEME = gr.themes.Default(
    font=["system-ui", "-apple-system", "Segoe UI", "Roboto", "Helvetica Neue", "Arial", "sans-serif"],
    font_mono=["ui-monospace", "SFMono-Regular", "Consolas", "Liberation Mono", "monospace"],
)

with gr.Blocks(title="Chatterbox lab", analytics_enabled=False) as demo:
    gr.Markdown(f"## Chatterbox lab\nThe worker's engine (Chatterbox Multilingual V3, T3 bf16, meanflow decoder) on "
                f"`{os.uname().nodename}`, {THREADS} CPU threads, one clip at a time.")
    with gr.Row():
        with gr.Column():
            text = gr.Textbox(label=f"Text (max. {MAX_CHARS} characters; numbers are spelled out)",
                              value="minus 1,5 Meter pro Sekunde", lines=2)
            with gr.Row():
                locale = gr.Dropdown(LOCALES, value="de-DE", label="Language")
                voice = gr.Dropdown(voice_names(), value=BUILTIN, label="Voice")
            voice_info = gr.Markdown(describe(BUILTIN))
            clip = gr.Audio(sources=["upload", "microphone"], type="filepath",
                            label="Reference clip (optional: replaces the voice for a try, or becomes a new one)")
            gr.Markdown("8 to 12 s of clean speech, one speaker, in the language the voice shall speak; WAV or FLAC at "
                        "44.1/48 kHz is best. Only your own voice or one you have permission for. Recording in the "
                        "browser needs https or localhost (SSH tunnel); over the LAN address, upload a file.")
            with gr.Accordion("Generation settings (saved with the voice)", open=False):
                exaggeration = gr.Slider(0.25, 2.0, value=0.5, step=0.05, label="Exaggeration (0.5 = neutral)")
                cfg_weight = gr.Slider(0.0, 1.0, value=0.5, step=0.05,
                                       label="CFG / pace (0 reduces the accent of a foreign-language reference)")
                temperature = gr.Slider(0.05, 2.0, value=0.8, step=0.05, label="Temperature")
                seed = gr.Number(value=0, precision=0, label="Seed (0 = random; the worker derives it from the text)")
            run = gr.Button("Generate", variant="primary")
            with gr.Accordion("Save as a voice of the worker", open=False):
                with gr.Row():
                    voice_name = gr.Textbox(label="Name (shown in the configurator)", placeholder="e.g. Florian")
                    gender = gr.Radio(["Female", "Male"], value="Female", label="Gender")
                locales = gr.Dropdown(LOCALES, value=["de-DE"], multiselect=True, label="Offered in")
                public = gr.Checkbox(value=False, label="Offer it in the configurator")
                voice_note = gr.Textbox(label="Note (source, consent)", placeholder="own recording")
                gr.Markdown("With a reference clip above: a new voice from it. Without: the details of the selected voice "
                            "are updated (clips made with other settings are made anew).")
                with gr.Row():
                    save_btn = gr.Button("Save voice")
                    delete_btn = gr.Button("Delete selected voice", variant="stop")
                voice_status = gr.Markdown()
            gr.Markdown(private_count())
            with gr.Accordion("Shared packs (published from the configurator; only you delete them)", open=False):
                packs_list = gr.Markdown()
                with gr.Row():
                    packs_btn = gr.Button("Load the list")
                    pack_id = gr.Textbox(label="Pack id", placeholder="20 characters from the list")
                    pack_del = gr.Button("Delete pack", variant="stop")
                gr.Markdown("Templates published from the configurator are voices like the others: pick them above "
                            "(\"source\": configurator in their voice.json) and delete them there.")
        with gr.Column():
            output = gr.Audio(label="Result", type="numpy", autoplay=True)
            info = gr.Markdown()
            stored = gr.Audio(label="Reference of the selected voice", interactive=False)
            gr.Examples(EXAMPLES, inputs=[text, locale], label="Catalogue lines")

    synth_inputs = [text, locale, voice, clip, exaggeration, cfg_weight, temperature, seed]
    run.click(synthesize, synth_inputs, [output, info])
    text.submit(synthesize, synth_inputs, [output, info])
    voice.change(pick_voice, voice, [voice_info, voice_name, gender, locales, public, voice_note,
                                     exaggeration, cfg_weight, temperature, stored])
    save_btn.click(save_voice, [voice_name, clip, voice, gender, locales, public, voice_note, exaggeration, cfg_weight,
                                temperature], [voice, clip, voice_status])
    delete_btn.click(delete_voice, voice, [voice, voice_status])
    packs_btn.click(list_packs, None, packs_list)
    pack_del.click(delete_pack, pack_id, [packs_list, pack_id])


# Gradio's page template preconnects to Google Fonts and loads iframe-resizer from cdnjs; both would
# hand the visitor's address to third parties, and neither is needed with system fonts and no iframe.
EXTERNAL = re.compile(rb'<link\s+rel="preconnect"[^>]*fonts\.(?:googleapis|gstatic)\.com[^>]*>'
                      rb'|<script\s+src="https://cdnjs\.cloudflare\.com/[^"]*"[^>]*>\s*</script>')


class StripExternal:
    """ASGI middleware that removes EXTERNAL from uncompressed HTML responses."""

    def __init__(self, app):
        self.app = app

    async def __call__(self, scope, receive, send):
        if scope["type"] != "http":
            return await self.app(scope, receive, send)
        start, body = None, []

        async def capture(message):
            nonlocal start
            if message["type"] == "http.response.start":
                headers = dict(message.get("headers", []))
                if headers.get(b"content-type", b"").startswith(b"text/html") and b"content-encoding" not in headers:
                    start = message
                    return
            elif message["type"] == "http.response.body" and start is not None:
                body.append(message.get("body", b""))
                if message.get("more_body"):
                    return
                data = EXTERNAL.sub(b"", b"".join(body))
                headers = [(k, v) for k, v in start.get("headers", []) if k != b"content-length"]
                await send({**start, "headers": headers + [(b"content-length", str(len(data)).encode())]})
                message = {"type": "http.response.body", "body": data}
            await send(message)

        await self.app(scope, receive, capture)


if __name__ == "__main__":
    import uvicorn
    from fastapi import FastAPI

    app = gr.mount_gradio_app(
        FastAPI(), demo.queue(default_concurrency_limit=1), path="/", theme=THEME, ssr_mode=False,
        max_file_size="20mb", show_error=True, footer_links=[], allowed_paths=[str(VOICE_DIR)],
    )
    uvicorn.run(StripExternal(app), host="0.0.0.0", port=PORT, log_level="warning")
