# TTS relay

A small Node.js service that speaks a few words with Microsoft Edge's text-to-speech (free,
no account) and returns MP3. Browsers cannot call that service directly, so the configurator
talks to this relay (default `https://voice.breezedude.de`) to generate voice clips, e.g. a
custom profile name. The request shape follows the OpenAI speech API, so the configurator can
also use other services that speak it.

It also offers the voices of a TTS worker at home (`tools/tts-worker`, Chatterbox): the worker
connects out to the relay over a WebSocket with a shared secret, announces its voices and takes
the jobs for them. Its voices are listed while it is connected, with the provider `chatterbox`.

## API

| Request | Answer |
|---|---|
| `POST /v1/audio/speech` with JSON `{"input": "Ozone Rush", "voice": "de-DE-KatjaNeural", "pitch": 0, "rate": 0}` | `audio/mpeg` (24 kHz mono MP3); `pitch` and `rate` in percent (-50..50) are our extension (SSML prosody, Edge voices only); `"priority": "bulk"` puts a worker clip behind single clips (a whole pack) |
| `GET /v1/voices?locale=de` | `{"voices": [{"name", "locale", "gender", "friendly", "provider", "seconds"}], "own": {"available", "languages", "seconds"}}`; `provider` is `edge` or `chatterbox`, `seconds` a worker voice's time per new clip |
| `POST /v1/voices/own?consent=yes` with a 16-bit mono WAV (at most `OWN_VOICE_MAX_BYTES`) | `201 {"id", "seconds"}`: an own voice, cloned by the worker, usable as `cb:<locale>-own-<id>` in every language of `own.languages`, never listed |
| `DELETE /v1/voices/own/<id>` | `{"deleted": true}`, or 404 |
| `GET /v1/worker` (WebSocket upgrade, `Authorization: Bearer <WORKER_SECRET>`) | the worker's connection; protocol in `tools/tts-worker/README.md` |
| `GET /health` | `{"ok": true, "workers": n}` |

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
accept the recordings on `/v1/voices/own` (the 8 kB body limit of `/` is too small for them).

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
