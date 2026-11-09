"""Serve only the browser preview, reloading it when preview files change."""

from __future__ import annotations

import hashlib
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1] / "web-preview"


class PreviewHandler(SimpleHTTPRequestHandler):
    def do_GET(self) -> None:
        if self.path == "/__version":
            digest = hashlib.sha256()
            for name in ("index.html", "style.css", "app.js", "launcher-bg.jpg", "desktop-bg.jpg"):
                digest.update((ROOT / name).read_bytes())
            body = digest.hexdigest().encode("ascii")
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        super().do_GET()

    def end_headers(self) -> None:
        self.send_header("Cache-Control", "no-store")
        super().end_headers()


if __name__ == "__main__":
    handler = partial(PreviewHandler, directory=str(ROOT))
    print("Launcher preview: http://0.0.0.0:8080", flush=True)  # noqa: T201
    ThreadingHTTPServer(("0.0.0.0", 8080), handler).serve_forever()  # noqa: S104
