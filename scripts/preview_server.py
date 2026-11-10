"""Serve only the browser preview, reloading it when preview files change."""

from __future__ import annotations

import argparse
import hashlib
import sys
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1] / "web-preview"
DEFAULT_PORT = 8080
REQUIRED_FILES = ("index.html", "style.css", "app.js")
OPTIONAL_FILES = ("launcher-bg.jpg", "desktop-bg.jpg")


def version_digest(directory: Path = ROOT) -> str:
    """Hash preview sources; optional artwork may be removed without errors."""
    digest = hashlib.sha256()
    for name in (*REQUIRED_FILES, *OPTIONAL_FILES):
        path = directory / name
        digest.update(name.encode("utf-8"))
        if path.is_file():
            digest.update(path.read_bytes())
    return digest.hexdigest()


class PreviewHandler(SimpleHTTPRequestHandler):
    def do_GET(self) -> None:
        if self.path == "/__version":
            body = version_digest(Path(self.directory or ROOT)).encode("ascii")
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if self.path == "/favicon.ico":
            # No icon is shipped; an empty response avoids a noisy 404.
            self.send_response(204)
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            return
        super().do_GET()

    def end_headers(self) -> None:
        self.send_header("Cache-Control", "no-store")
        super().end_headers()


def make_server(port: int = DEFAULT_PORT, directory: Path = ROOT) -> ThreadingHTTPServer:
    handler = partial(PreviewHandler, directory=str(directory))
    return ThreadingHTTPServer(("0.0.0.0", port), handler)  # noqa: S104


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Serve the browser preview with auto-reload.")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    arguments = parser.parse_args(argv)
    try:
        server = make_server(arguments.port)
    except OSError as exc:
        sys.stderr.write(
            f"Не удалось запустить предпросмотр на порту {arguments.port}: "
            f"{exc.strerror or exc}. Закройте другой сервер или укажите --port.\n"
        )
        return 1
    sys.stdout.write(f"Launcher preview: http://0.0.0.0:{arguments.port}\n")
    sys.stdout.flush()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
