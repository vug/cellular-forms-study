"""Shows the page on this PC and keeps what is entered on it.

    python scripts/atlas serve          then open http://localhost:8765

    GET  /               the page (scripts/atlas/page/index.html)
    GET  /data.json      every run of the database, built on request (export.build)
    GET  /img/...        the screenshots and contact sheets
    POST /api/label      {"run": id, "rating": 1..5 or 0 to clear, "category": text, "note": text}
    POST /api/kind       {"id": kind, "name": text}

Only the Python standard library is used. It listens on this PC only.
"""
import http.server
import json
import mimetypes
import os

from . import export
from .store import Store


FRAME = (b'<!doctype html><html><head><meta charset=utf8><meta name=viewport content="width=device-width,initial-scale=1,viewport-fit=cover">'
         b"<style>:root{color-scheme:light;padding:env(safe-area-inset-top,0px) 0 env(safe-area-inset-bottom,0px)}body{margin:0;font:14px system-ui,sans-serif}"
         b"img{max-width:100%}[hidden]{display:none!important}</style></head><body>")


def make_handler(root):
    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, fmt, *a):
            pass

        def _send(self, code, body, kind="application/json"):
            self.send_response(code)
            self.send_header("Content-Type", kind)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store" if kind == "application/json" else "max-age=600")
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            path = self.path.split("?")[0]
            if path in ("/", "/index.html"):
                # The page is written as it is published (content only); here it gets the same
                # frame around it that publishing gives it.
                with open(export.PAGE, "rb") as f:
                    return self._send(200, FRAME + f.read() + b"</body></html>", "text/html; charset=utf-8")
            if path == "/data.json":
                return self._send(200, json.dumps(export.build(Store(root)), separators=(",", ":")).encode())
            if path.startswith("/img/") and ".." not in path:
                file = os.path.join(root, path[1:].replace("/", os.sep))
                if os.path.isfile(file):
                    with open(file, "rb") as f:
                        return self._send(200, f.read(), mimetypes.guess_type(file)[0] or "application/octet-stream")
            self._send(404, b"{}")

        def do_POST(self):
            try:
                body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", "0"))) or b"{}")
                store = Store(root)
                if self.path == "/api/label":
                    rating = body.get("rating")
                    store.set_label(int(body["run"]), rating=None if rating is None else int(rating), category=body.get("category"), note=body.get("note"))
                elif self.path == "/api/kind":
                    names = store.meta("kind_names", {})
                    names[str(int(body["id"]))] = str(body.get("name", ""))[:80]
                    store.set_meta("kind_names", names)
                else:
                    return self._send(404, b"{}")
                self._send(200, b'{"ok":true}')
            except (ValueError, KeyError) as e:
                self._send(400, json.dumps({"error": str(e)}).encode())

    return Handler


def cmd_serve(args):
    root = Store(args.root).root
    server = http.server.ThreadingHTTPServer(("127.0.0.1", args.port), make_handler(root))
    print(f"atlas: http://localhost:{args.port}  ({root}); Ctrl+C to stop", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


def register(sub):
    p = sub.add_parser("serve", help="show the page on this PC and keep ratings and names")
    p.add_argument("--port", type=int, default=8765)
    p.set_defaults(run=cmd_serve)
