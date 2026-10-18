# TTS relay

A small Node.js service that speaks a few words with Microsoft Edge's text-to-speech (free,
no account) and returns MP3. Browsers cannot call that service directly, so the configurator
talks to this relay (default `https://voice.breezedude.de`) to generate voice clips, e.g. a
custom profile name. The request shape follows the OpenAI speech API, so the configurator can
also use other services that speak it.

It also offers the voices of a TTS worker at home (`tools/tts-worker`, Chatterbox): the worker
connects out to the relay over a WebSocket with a shared secret, announces its voices and takes
the jobs for them. Its voices are listed while it is connected, with the provider `chatterbox`.

Shared voice packs: anyone can publish a whole pack from the configurator (Custom voicepack). The
relay keeps the pack descriptions in SQLite (`node:sqlite`, `DB_PATH`), the worker keeps the files;
uploads and downloads pass through the relay in 1 MB parts. Packs and voice templates are public at
once; only the operator deletes them (the admin API with `ADMIN_TOKEN`, used by the worker's lab).

Works (1.4): what a device's voice pack is made of - the voice, the audio settings and every clip the
configurator made for the device (its text, language and take, and whether it is in the pack file or a
single file in `voices/custom`) - so that a clip added or changed later sounds like the rest. A work is
read by its id and never listed; the device's configuration names it (`voice_generator_work_<pack>_string`)
with its owner key (`voice_generator_workkey_<pack>_string`). The key overwrites the work (no history);
without it the configurator starts a new work from it (`parent`). A pack shared from a work belongs to it:
its owner shares it again in place (the same pack id; the old file is served until the new one is complete).

## API

| Request | Answer |
|---|---|
| `POST /v1/audio/speech` with JSON `{"input": "Ozone Rush", "voice": "de-DE-KatjaNeural", "pitch": 0, "rate": 0}` | `audio/mpeg` (24 kHz mono MP3); `pitch` and `rate` in percent (-50..50) are our extension (SSML prosody, Edge voices only); `"priority": "bulk"` puts a worker clip behind single clips (a whole pack) |
| `GET /v1/voices?locale=de` | `{"voices": [{"name", "locale", "gender", "friendly", "provider", "seconds", "defaults"?}], "own": {"available", "languages", "seconds"}}`; `provider` is `edge` or `chatterbox`, `seconds` a worker voice's time per new clip, `defaults` a template's pitch, rate and band-pass |
| `POST /v1/voices/own?consent=yes` with a 16-bit mono WAV (at most `OWN_VOICE_MAX_BYTES`) | `201 {"id", "seconds"}`: an own voice, cloned by the worker, usable as `cb:<locale>-own-<id>` in every language of `own.languages`, never listed |
| `DELETE /v1/voices/own/<id>` | `{"deleted": true}`, or 404 |
| `POST /v1/voices/public` with JSON `{"voice_id", "name", "locale", "gender", "settings", "audio", "consent": true}` | `201 {"voice": "cb:de-DE-<name>"}`: the own voice becomes a template for everyone (409 for a name taken in that language) |
| `POST /v1/audio/speech` with `"settings": {"exaggeration", "cfg_weight", "temperature"}` | a worker clip with these Chatterbox settings (Custom speaker), cached apart from the plain one |
| `POST /v1/audio/speech` with `"language": "en"` and/or `"variant": 1..99` | a worker clip spoken in another language than the voice's, and/or another take of the same text (each take reproducible, cached apart; 0 is the default take) |
| `GET /v1/packs?lang=de` | `{"packs": [{"id", "name", "lang", "voice", "voice_label", "settings", "clips", "size", "created", "downloads", "base", "work", "changed_texts"}]}` (the voice pack id as `lang`; `work`: the work it was shared from, or null) |
| `GET /v1/packs/<id>` | the same with `"overrides": {clip: text}`, `"languages": {clip: language}` and `"variants": {clip: take}` |
| `GET /v1/packs/<id>/file` | the `.ubpack` (`Content-Disposition: <lang>.ubpack`) |
| `POST /v1/packs` with JSON `{"name", "lang", "voice", "voice_label", "clips", "size", "settings", "overrides", "languages", "variants", "base", "consent": true}` | `201 {"id", "upload_token", "part_size", "replaces"}` (409: name taken in the language; own voices refused). With `"work"` and that work's owner `"key"` (403 otherwise) the pack belongs to the work, and a pack the work shared before is replaced in place: the same id, `"replaces": true` |
| `POST /v1/works` with JSON `{"lang", "name", "voice", "locale", "settings", "clips": {clip: {"t", "l"?, "v"?, "f"?}}, "built", "parent"?}` | `201 {"id", "key", "version": 1}`: a new work; the key is shown only here (the relay keeps its hash) |
| `GET /v1/works/<id>` | the work: `{"id", "version", "lang", "name", "voice", "locale", "settings", "clips", "built", "parent", "pack", "created", "updated"}` |
| `PUT /v1/works/<id>` with the same JSON and `"key"` | `{"id", "version"}`: overwritten, the version counted up (403: not its key, 404: no such work) |
| `PUT /v1/packs/<id>/file?part=<n>&parts=<m>` with `X-Upload-Token` | `{"done"}`; the pack is listed after its last part |
| `GET /v1/admin/packs`, `DELETE /v1/admin/packs/<id>` with `Authorization: Bearer <ADMIN_TOKEN>` | all packs with their state; deleting removes the file too |
| `GET /v1/worker` (WebSocket upgrade, `Authorization: Bearer <WORKER_SECRET>`) | the worker's connection; protocol in `tools/tts-worker/README.md` |
| `GET /health` | `{"ok": true, "workers": n, "packs": n, "works": n}` |

Errors come as `{"error": {"message": ...}}` with 400 (input, voice, recording), 404 (own voice
gone), 429 (rate limit, with `Retry-After`), 502 (Edge or the worker failed), 503 (the worker is
offline, or still at a clip: with `Retry-After`, the clip is made anyway and served from the cache
when asked again; nginx gives up after 30 s, so a request waits at most `WORKER_WAIT_MS`).

## Limits (`.env`, see `.env.example`)

* `MAX_CHARS` (80): a voice line is a few words.
* `RATE_LIMIT` per `RATE_WINDOW_S` and client IP (3000 per hour in the example, 30 per 10 min
  without a `.env`); repeated texts are served
  from a memory cache and do not count. Generating a whole voice pack from the configurator is
  about 1000 requests, which the example values allow twice an hour; `CACHE_ENTRIES` (2000) keeps a
  whole pack, so building it again with the same voice costs nothing.
* `VOICE_FILTER`: a regular expression on the voice name (the example: the languages of the
  manufacturer's voice packs and a few more European ones; empty: every Edge voice).
* `ALLOWED_ORIGINS`: browser origins (`*` by default).
* `EDGE=0` turns Edge's voices off.
* `WORKER_SECRET` (at least 16 characters, the same as in the worker's `/etc/tts/worker.env`): without
  it no worker may connect. `WORKER_WAIT_MS` (25000), `WORKER_JOB_TIMEOUT_MS` (180000) and
  `WORKER_QUEUE_MAX` (300) bound the waiting; requests for one clip share one job and count once
  against the rate limit.
* `OWN_VOICE_LIMIT` (10) own voices per IP and day, `OWN_VOICE_MAX_BYTES` (2000000) per recording
  (the configurator sends at most 20 s, about 1 MB).
* `WORK_LIMIT` (50) new works per IP and day; their owners' updates do not count.
* `TEMPLATE_LIMIT` (5) voice templates and `PACK_LIMIT` (5) shared packs per IP and day,
  `PACK_MAX_BYTES` (40 MB) per pack (a whole pack is about 16 MB). Unfinished uploads free their
  name after two hours.
* `DB_PATH` (`data/relay.db`): the pack database. `ADMIN_TOKEN` (at least 16 characters): the
  operator's key for listing and deleting packs; empty: no admin API.

## Setup

```bash
cd tools/tts-relay
npm install
cp .env.example .env      # adjust; TRUST_PROXY=1 behind nginx
pm2 start ecosystem.config.cjs
pm2 save
```

nginx: `nginx.conf.example` (TLS as usual; the `limit_req_zone` line goes into the `http`
context). The relay listens on 127.0.0.1 only. For a worker, nginx must pass the WebSocket
upgrade on `/v1/worker` (and keep the quiet connection open: the relay pings every 30 s), and
accept the recordings on `/v1/voices/own` and the pack parts on `/v1/packs/<id>/file` (the 8 kB
body limit of `/` is too small for them).

Test:

```bash
curl -s -X POST http://127.0.0.1:5055/v1/audio/speech -H 'Content-Type: application/json' \
  -d '{"input":"Ozone Rush","voice":"de-DE-KatjaNeural"}' -o test.mp3
```

## Troubleshooting

* **Browser: "CORS preflight ... redirect"** - the browser sends an `OPTIONS` request first, and
  something in front of the relay answers it with a redirect instead of passing it on. Check
  with `curl -sI https://voice.breezedude.de/health`: it must be `200`, not `301`. A `301` to the
  same `https://` address means the HTTPS vhost itself redirects (a "force SSL" rule behind
  another TLS terminator, or a `return 301` in the wrong server block); a `301` from `http://`
  means the configurator has the service without `https://`. The relay answers `OPTIONS` with
  the CORS headers itself; nginx must not add its own.
* **502 from the relay** - Edge refused the connection or closed it without audio; the relay tries a
  clip three times on fresh connections before it gives up. If every request fails, Edge has
  changed its protocol again: update `msedge-tts` (`npm update msedge-tts`, `pm2 restart tts-relay`),
  that package tracks the token scheme and the browser version Edge expects.
* **429** - the per-IP limit; repeated texts are served from the cache and do not count.
* **No home-server voices** - `curl -s https://voice.breezedude.de/health` shows `"workers": 0`: the
  worker is down (`journalctl -u tts-worker` at home), its secret differs (the relay logs "worker
  refused: bad secret"), or nginx does not pass the upgrade (the worker logs HTTP 400 or 404).

## How it works

`server.js` speaks through the `msedge-tts` package (MIT), which opens Edge's read-aloud WebSocket
(`speech.platform.bing.com`) with the trusted client token and the `Sec-MS-GEC` token of the
current 5-minute window and multiplexes requests on it. The relay keeps one connection per voice
open (closed after 5 min idle) and shares it between clips, so a whole pack needs one handshake
instead of one per clip; pitch and rate go into the SSML prosody. The voice list comes from the
same service and is cached for a day.

Worker voices (`cb:...`) go to the connected worker through a queue: single clips first, then the
clips of whole packs (`priority: bulk`), at most as many at a time as the worker has model
processes; a job whose worker drops goes back to the queue. The relay keeps the results in the same
memory cache as Edge's; the worker keeps every clip on disk as well.
