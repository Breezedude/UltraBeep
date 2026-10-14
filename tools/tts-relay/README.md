# TTS relay

A small Node.js service that speaks a few words with Microsoft Edge's text-to-speech (free,
no account) and returns MP3. Browsers cannot call that service directly, so the configurator
talks to this relay (default `https://voice.breezedude.de`) to generate voice clips, e.g. a
custom profile name. The request shape follows the OpenAI speech API, so the configurator can
also use other services that speak it.

## API

| Request | Answer |
|---|---|
| `POST /v1/audio/speech` with JSON `{"input": "Ozone Rush", "voice": "de-DE-KatjaNeural", "pitch": 0, "rate": 0}` | `audio/mpeg` (24 kHz mono MP3); `pitch` and `rate` in percent (-50..50) are our extension (SSML prosody) |
| `GET /v1/voices?locale=de` | `{"voices": [{"name", "locale", "gender", "friendly"}]}` |
| `GET /health` | `{"ok": true}` |

Errors come as `{"error": {"message": ...}}` with 400 (input, voice), 429 (rate limit, with
`Retry-After`), 502 (Edge did not answer).

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

## Setup

```bash
cd tools/tts-relay
npm install
cp .env.example .env      # adjust; TRUST_PROXY=1 behind nginx
pm2 start ecosystem.config.cjs
pm2 save
```

nginx: `nginx.conf.example` (TLS as usual; the `limit_req_zone` line goes into the `http`
context). The relay listens on 127.0.0.1 only.

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

## How it works

`server.js` speaks through the `msedge-tts` package (MIT), which opens Edge's read-aloud WebSocket
(`speech.platform.bing.com`) with the trusted client token and the `Sec-MS-GEC` token of the
current 5-minute window and multiplexes requests on it. The relay keeps one connection per voice
open (closed after 5 min idle) and shares it between clips, so a whole pack needs one handshake
instead of one per clip; pitch and rate go into the SSML prosody. The voice list comes from the
same service and is cached for a day.
