#!/usr/bin/env python3
"""Local web server for the analyzer: serves the repository root without caching, on 127.0.0.1.

    python tools/analyzer/serve.py [PORT]      then open http://localhost:8766/tools/analyzer/
"""
import functools
import http.server
import os
import sys


class NoCache(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        super().end_headers()


def main():
    root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8766
    handler = functools.partial(NoCache, directory=root)
    print(f"http://localhost:{port}/tools/analyzer/  (serving {root})", flush=True)
    http.server.ThreadingHTTPServer(("127.0.0.1", port), handler).serve_forever()


if __name__ == "__main__":
    main()
