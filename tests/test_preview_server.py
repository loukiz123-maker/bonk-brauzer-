"""Preview server behaviour; no browser is required."""

from __future__ import annotations

import importlib.util
import socket
import sys
import threading
import urllib.request
from http import HTTPStatus
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "preview_server", ROOT / "scripts" / "preview_server.py"
)
assert SPEC is not None and SPEC.loader is not None
preview_server = importlib.util.module_from_spec(SPEC)
sys.modules["preview_server"] = preview_server
SPEC.loader.exec_module(preview_server)


def preview(tmp_path: Path) -> Path:
    directory = tmp_path / "web-preview"
    directory.mkdir()
    (directory / "index.html").write_text("<h1>preview</h1>", encoding="utf-8")
    (directory / "style.css").write_text("body{}", encoding="utf-8")
    (directory / "app.js").write_text("'use strict';", encoding="utf-8")
    return directory


@pytest.fixture()
def server(tmp_path: Path):
    directory = preview(tmp_path)
    instance = preview_server.make_server(0, directory)
    thread = threading.Thread(target=instance.serve_forever, daemon=True)
    thread.start()
    try:
        yield instance.server_address[1], directory
    finally:
        instance.shutdown()
        instance.server_close()
        thread.join(timeout=5)


def fetch(port: int, path: str) -> tuple[int, bytes]:
    with urllib.request.urlopen(f"http://127.0.0.1:{port}{path}", timeout=5) as response:
        return response.status, response.read()


def test_version_works_without_optional_artwork(tmp_path: Path) -> None:
    directory = preview(tmp_path)
    before = preview_server.version_digest(directory)
    (directory / "launcher-bg.jpg").write_bytes(b"art")
    assert preview_server.version_digest(directory) != before
    (directory / "launcher-bg.jpg").unlink()
    assert preview_server.version_digest(directory) == before


def test_version_and_favicon_responses(server) -> None:
    port, _ = server
    status, body = fetch(port, "/__version")
    assert status == HTTPStatus.OK and len(body) == 64
    with urllib.request.urlopen(f"http://127.0.0.1:{port}/favicon.ico", timeout=5) as response:
        assert response.status == HTTPStatus.NO_CONTENT


def test_busy_port_reports_a_useful_message(capsys) -> None:
    with socket.socket() as probe:
        probe.bind(("0.0.0.0", 0))  # noqa: S104 - matches the server bind address
        probe.listen()
        port = probe.getsockname()[1]
        assert preview_server.main(["--port", str(port)]) == 1
        error = capsys.readouterr().err
        assert "Не удалось запустить предпросмотр" in error
        assert "--port" in error
        assert "Traceback" not in error
