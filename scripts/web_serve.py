"""Static file server for the WebGPU page: serves the repository root on localhost only.

    python scripts/web_serve.py [port]      then open http://localhost:8123/web/ (the app), or
                                            http://localhost:8123/web/check.html?task=verify (the check)

The page needs http (ES modules and fetch do not work from file://). Nothing is served to other
machines: the server binds to 127.0.0.1.
"""
import http.server
import os
import sys

port = int(sys.argv[1]) if len(sys.argv) > 1 else 8123
os.chdir(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map, ".js": "text/javascript", ".cfs": "application/octet-stream"}

    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def log_message(self, fmt, *args):
        pass


http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
