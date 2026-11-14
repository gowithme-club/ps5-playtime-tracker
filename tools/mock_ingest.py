#!/usr/bin/env python3
"""A stub ingest server for testing the payload without gowithme.club.

Answers 200 to every request and prints it: headers, body, parsed JSON.

    python3 tools/mock_ingest.py [port]      # listens on 0.0.0.0:8080

Then in /data/gowithme.club/config.cfg on the console:

    token   = test
    host    = <this machine's IP>
    port    = 8080
"""

import functools
import json
import sys
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8080

# Unbuffered, so output redirected to a file shows up right away.
print = functools.partial(__builtins__.print, flush=True)


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass  # own format below

    def _stamp(self):
        return datetime.now(timezone.utc).strftime("%H:%M:%S")

    def _reply(self, payload, code=200):
        body = json.dumps(payload).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(length)

        print(f"\n[{self._stamp()}] POST {self.path}")
        print(f"  Authorization: {self.headers.get('Authorization', '<none>')}")
        print(f"  X-Client-Version: {self.headers.get('X-Client-Version', '<none>')}")
        print(f"  Content-Length: {length}")

        try:
            data = json.loads(raw)
        except Exception as exc:
            print(f"  !! body does not parse: {exc}")
            print(f"  raw: {raw!r}")
            self._reply({"error": "bad json"}, 400)
            return

        print(f"  body: {json.dumps(data, ensure_ascii=False)}")

        if self.path.endswith("/heartbeat"):
            current = data.get("current", [])
            for c in current:
                print(f"  -> playing: {c.get('titleName')} [{c.get('titleId')}]"
                      f" since {c.get('startedAt')}, profile {c.get('account')}")
            if not current:
                print("  -> console on, nothing playing")
            self._reply({"ok": True})

        elif self.path.endswith("/sessions"):
            sessions = data.get("sessions", [])
            for s in sessions:
                print(f"  -> session: {s.get('title')} [{s.get('titleId')}]"
                      f" {s.get('startedAt')} .. {s.get('endedAt')}"
                      f" = {s.get('seconds')} s, profile {s.get('account')}")
            self._reply({"accepted": len(sessions), "duplicates": 0, "rejected": []})

        else:
            self._reply({"ok": True})


if __name__ == "__main__":
    print(f"stub listening on 0.0.0.0:{PORT}, Ctrl-C to quit")
    ThreadingHTTPServer(("0.0.0.0", PORT), Handler).serve_forever()
