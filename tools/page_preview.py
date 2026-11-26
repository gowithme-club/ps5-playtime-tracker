#!/usr/bin/env python3
"""The tile's page (assets/page.html) off the console, with a stub /state.

    python3 tools/page_preview.py [port]     # 7532, or the next free one

Open the address it prints: the page sits in a 1540x700 frame, the console
browser's window, scaled to fit, with links to every state (welcome, code,
stopped, connecting, linked, late, offline, down; add &nick=Name for linked). /page
is the page alone. The QR codes are stand-ins: they only fill the space.
The page asks /state without its query, so the state is read from Referer.

The welcome screen frames gowithme.club/embed/flow; GWM_FLOW_ORIGIN=http://
localhost:7600 frames a server run locally instead."""

import json
import os
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
PAGE = os.path.join(ROOT, "assets", "page.html")
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 7532
FLOW_ORIGIN = os.environ.get("GWM_FLOW_ORIGIN", "")


def fake_qr(n=29):
    # A pattern with the three finder squares, close enough for layout.
    rows = []
    for y in range(n):
        r = ""
        for x in range(n):
            finder = any(
                ox <= x < ox + 7 and oy <= y < oy + 7 and (x - ox in (0, 6) or y - oy in (0, 6) or (2 <= x - ox <= 4 and 2 <= y - oy <= 4))
                for ox, oy in ((0, 0), (n - 7, 0), (0, n - 7))
            )
            near = any(ox - 1 <= x <= ox + 7 and oy - 1 <= y <= oy + 7 for ox, oy in ((0, 0), (n - 7, 0), (0, n - 7)))
            r += "1" if finder or (not near and (x * 7 + y * 13 + x * y) % 5 < 2) else "0"
        rows.append(r)
    return rows


def state_for(q):
    s = q.get("state", ["code"])[0]
    nick = q.get("nick", ["Example"])[0]
    v = {"version": "1.2.0"}
    if s in ("code", "welcome"):
        v.update(state="code", code="217301", qr=fake_qr())
        if s == "welcome":
            v.update(welcome=True)
    elif s in ("linked", "late"):
        v.update(state="linked", syncAgo=40 if s == "linked" else 900, queued=0 if s == "linked" else 12,
                 profileUrl="https://gowithme.club/player/" + nick, profileQr=fake_qr(33),
                 recorded={"seconds": 261240, "games": 42, "trophies": 318})
    elif s in ("claim", "claiming"):
        v.update(state="claim", profile=q.get("profile", ["Mira"])[0], syncAgo=20, queued=0,
                 recorded={"seconds": 0, "games": 0, "trophies": 0})
        if s == "claim":
            v.update(code="042137", qr=fake_qr())
    else:
        v.update(state=s)
    return v


STATES = ["welcome", "code", "stopped", "connecting", "linked", "late", "claim", "claiming", "offline", "down"]


def frame(query):
    current = parse_qs(query).get("state", ["code"])[0]
    links = "".join(
        f'<a href="/?state={s}"{" class=on" if s == current else ""}>{s}</a>' for s in STATES
    )
    return f"""<!doctype html><meta charset="utf-8"><title>tile page preview</title>
<style>
  html, body {{ margin: 0; height: 100%; background: #0b0c0d; color: #9a9ea3; font: 13px -apple-system, system-ui, sans-serif; }}
  nav {{ position: fixed; top: 0; left: 0; right: 0; height: 36px; display: flex; align-items: center; gap: 4px; padding: 0 12px; }}
  nav a {{ color: inherit; text-decoration: none; padding: 4px 8px; border-radius: 6px; }}
  nav a.on, nav a:hover {{ background: #202225; color: #f2f3f4; }}
  nav span {{ margin-left: auto; white-space: nowrap; }}
  .stage {{ position: fixed; top: 36px; left: 0; right: 0; bottom: 0; overflow: hidden; }}
  iframe {{ position: absolute; left: 50%; top: 50%; width: 1540px; height: 700px; border: 0; border-radius: 10px; background: #16181a; }}
</style>
<nav>{links}<span>1540×700, the console's browser window</span></nav>
<div class="stage"><iframe src="/page?{query or 'state=code'}"></iframe></div>
<script>
  var f = document.querySelector("iframe"), st = document.querySelector(".stage");
  function fit() {{ f.style.transform = "translate(-50%, -50%) scale(" + Math.min(1, st.clientWidth / 1560, st.clientHeight / 720) + ")"; }}
  addEventListener("resize", fit); fit();
</script>"""


class Handler(BaseHTTPRequestHandler):
    def send(self, code, kind, body):
        self.send_response(code)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        url = urlparse(self.path)
        if url.path == "/":
            self.send(200, "text/html; charset=utf-8", frame(url.query).encode())
        elif url.path == "/page":
            with open(PAGE, "rb") as f:
                page = f.read()
            if FLOW_ORIGIN:
                page = page.replace(b'var FLOW_ORIGIN = "https://gowithme.club"', f'var FLOW_ORIGIN = "{FLOW_ORIGIN}"'.encode())
            self.send(200, "text/html; charset=utf-8", page)
        elif url.path == "/state":
            ref = parse_qs(urlparse(self.headers.get("Referer", "")).query)
            self.send(200, "application/json", json.dumps(state_for(ref)).encode())
        else:
            self.send(404, "text/plain", b"")

    def do_POST(self):
        self.send(202, "application/json", b"{}")

    def log_message(self, *args):
        pass


# A busy port is usually another copy of this preview: take the next one.
for port in range(PORT, PORT + 20):
    try:
        server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    except OSError:
        continue
    print(f"http://localhost:{port}/?state=code", flush=True)
    server.serve_forever()
sys.exit(f"no free port from {PORT} to {PORT + 19}")
