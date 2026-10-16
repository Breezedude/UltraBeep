# TTS worker

Better voices for the configurator, made on a computer at home: the worker runs
[Chatterbox Multilingual](https://github.com/resemble-ai/chatterbox) (Resemble AI, MIT) on the CPU
and offers its voices through the public relay (`tools/tts-relay`), next to Microsoft Edge's. It
connects **out** to the relay over an authenticated WebSocket, so the home network needs no open
port, no VPN and no fixed address; while it is offline, its voices just disappear from the list.

```
browser ──https──> voice.breezedude.de (nginx, tts-relay) <──wss, secret── tts-worker (home)
                     Edge voices, limits, cache                            Chatterbox, disk cache
```

* **Voices** are reference clips turned into Chatterbox's conditioning. Public ones are made in the
  lab (below) and listed as `cb:<locale>-<name>` (e.g. `cb:de-DE-florian`) in the locales they are
  offered in; the locale picks the language of the text.
* **Own voices**: anyone can clone their own voice in the configurator (Voice → Custom speaker): the
  browser converts the recording to WAV and uploads it; the worker keeps only the derived
  conditioning (not the recording) under a random id, as `cb:<locale>-own-<id>`. It is never listed,
  works in every Chatterbox language and stays until its owner deletes it there. Tried with
  Chatterbox settings of its own (`settings` of a speak, cached apart), it can be published as a
  **template**: a copy becomes a public voice of the chosen name (unique per language), with those
  settings and the configurator's pitch, rate and band-pass as its defaults (`"source":
  "configurator"` in its voice.json). Templates are public at once; only the operator deletes them
  (in the lab).
* **Shared packs**: whole packs published from the configurator (Voice → Custom voicepack → Share).
  The relay keeps their descriptions; the worker keeps the files in `PACK_DIR` (`PACK_MAX_MB` each,
  `PACK_STORAGE_MB` in all), takes them in 1 MB parts (the first must be a FAT16 boot sector) and
  reads them out piece by piece for downloads.
* **Cache**: a clip depends only on voice and text (pitch, rate and the band-pass are applied in the
  browser), so each is made once and served from disk afterwards (`CACHE_MAX_MB`, least recently
  used go first). While nobody asks for clips, the worker renders the voice pack texts of every
  public voice in its locales (`PRERENDER=1`): a whole pack in those voices then comes back at once.
* **Numbers** are spelled out per language before synthesis (num2words): Chatterbox reads digits
  unreliably ("47" came out as "740").

## Speed (Ryzen 7 8745HS, Debian 13 LXC, 2026-10-15)

| Setup | per catalogue clip | real-time factor |
|---|---|---|
| Chatterbox as shipped (fp32, 10 flow steps) | 7.8 s | 3.9 |
| + Turbo's distilled "meanflow" decoder (2 steps) | 4.8 s | 2.5 |
| + T3 in bfloat16 (what the worker runs) | 3.5 s | 1.7 |

Speech recognition (faster-whisper) heard the same words in all three; T3 in dynamic int8 was faster
but gibberish. One clip does not get faster beyond 6 to 8 threads (16 were 35 % slower): more cores
only help through a second model process (`ENGINES=2`, about 7 GB of RAM each). A whole pack of 979
clips takes about an hour per voice and language. A clip that runs on past its text (it happens) is
made again with another seed.

## Setup

On a Debian 13 host or container with about 16 GB of RAM, 15 GB of disk and outbound internet:

```bash
tools/tts-worker/deploy.sh root@<host> --install
```

copies this folder to `/opt/tts/worker` and runs `install.sh` there: user `tts`, a venv in
`/opt/tts/venv` (`requirements.txt`, torch for the CPU), Chatterbox at a pinned commit, the model
files at pinned revisions (about 4 GB in `/opt/tts/models`: only what the multilingual loader and
the meanflow decoder need, not the 14 GB repository), `/etc/tts/worker.env` from
`worker.env.example`, and the systemd units `tts-lab` and `tts-worker`. Then put the same random
secret (`openssl rand -hex 32`) into `/etc/tts/worker.env` and the relay's `.env` (`WORKER_SECRET`)
and start the worker:

```bash
systemctl enable --now tts-worker
journalctl -u tts-worker -f      # "connected to wss://voice.breezedude.de/v1/worker with N public voices"
```

Later changes: `tools/tts-worker/deploy.sh root@<host>` (copies, exports the pack texts to
`/opt/tts/catalogue.json`, restarts). A Docker image is not needed: Docker in an LXC would need
nesting, and the venv is all there is.

Data: `/var/lib/tts/voices` (public voices, templates included), `/var/lib/tts/private` (own voices,
mode 700), `/var/lib/tts/packs` (shared packs), `/var/lib/tts/cache/clips`.

## The lab

`http://<host>:7860` (`tts-lab.service`, Gradio, LAN only, no login): speaks any text with the
worker's engine, so what you hear is what the relay serves, and makes the public voices. Upload or
record a reference (8 to 12 s of clean speech, one speaker, in the language the voice shall speak;
WAV or FLAC at 44.1/48 kHz; recording in the browser needs https or localhost, e.g.
`ssh -N -L 7860:127.0.0.1:7860 root@<host>`), try it, then "Save as a voice of the worker": name,
gender, the locales it is offered in, whether the configurator lists it, and the generation
settings (exaggeration, CFG/pace, temperature). The worker picks changes up within 30 s; clips of a
voice whose reference or settings changed are made anew. Only use your own voice, or one whose
speaker agreed - never clone a real person without consent. The page loads nothing from outside:
the theme uses system fonts, and a middleware removes the Google Fonts preconnect and the cdnjs
script Gradio's template carries. "Shared packs" lists the packs people published (with
`RELAY_HTTP` and the relay's `ADMIN_TOKEN` in `/etc/tts/worker.env`) and deletes one by its id;
templates from the configurator are deleted like every voice, from the voice list.

## Files

| File | |
|---|---|
| `worker.py` | the connection to the relay, jobs, cache, prerendering |
| `engine.py` | Chatterbox on the CPU (T3 bf16, meanflow decoder), MP3, references |
| `voices.py` | public and own voices, their names |
| `textnorm.py` | numbers as words, plausible clip lengths |
| `cache.py` | the clip cache on disk |
| `packs.py` | the files of the shared packs |
| `lab.py` | the lab |
| `export_catalogue.js` | the configurator's pack texts as JSON, for prerendering |
| `install.sh`, `deploy.sh`, `systemd/`, `worker.env.example` | setup |

Tests, in the worker's venv (`test_engine` loads the model, about a minute):

```bash
cd /opt/tts/worker && /opt/tts/venv/bin/python -m unittest discover -s tests
```

## Protocol (relay ⇄ worker)

JSON text frames on `wss://<relay>/v1/worker`, `Authorization: Bearer <WORKER_SECRET>`:

| From | Message |
|---|---|
| worker | `{"type": "hello" \| "voices", "name", "slots", "seconds", "languages": [...], "voices": [{name, locale, gender, friendly}]}` |
| relay | `{"type": "speak", "id", "voice", "text", "settings"?}` → `{"type": "audio", "id", "mime": "audio/mpeg", "data": base64, "cached", "ms"}` |
| relay | `{"type": "clone", "id", "wav": base64, "consent": true}` → `{"type": "cloned", "id", "voice_id", "seconds"}` |
| relay | `{"type": "forget", "id", "voice_id"}` → `{"type": "forgot", "id", "deleted"}` |
| relay | `{"type": "publish", "id", "voice_id", "name", "locale", "gender", "settings", "audio", "consent": true}` → a `voices` update, then `{"type": "published", "id", "voice"}` |
| relay | `{"type": "pack_put", "id", "pack", "part", "parts", "data"}` → `{"type": "pack_stored", "id", "done", "size"?, "sha256"?}` |
| relay | `{"type": "pack_read", "id", "pack", "offset", "length"}` → `{"type": "pack_data", "id", "data", "size"}` |
| relay | `{"type": "pack_delete", "id", "pack"}` → `{"type": "pack_deleted", "id", "deleted"}` |
| worker | `{"type": "error", "id", "status": 400 \| 404 \| 409 \| 413 \| 507 \| 500, "message"}` |

The relay sends at most `slots` speech and clone jobs at a time, single clips before the bulk of a
pack, and gives a job back to the queue when the worker drops; publishing, forgetting and the pack
files go past that queue (they need no model). One worker is the expected setup: own voices,
templates and pack files live on the worker that made them.

## Licences

Chatterbox and its model weights (Resemble AI) are MIT; every clip carries Resemble's inaudible Perth
watermark. The meanflow decoder comes from Chatterbox-Turbo (MIT). Python dependencies keep their own
licences (among them num2words LGPL-2.1, PyAV BSD with FFmpeg under LGPL, Gradio Apache-2.0); the
weights are downloaded by `install.sh` and never go into this repository.
