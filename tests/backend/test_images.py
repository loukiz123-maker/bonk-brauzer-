"""Image preparation must never act without consent (stage 4)."""

from __future__ import annotations

from pathlib import Path

import pytest

from lynxdroid.backend import images
from lynxdroid.core.errors import Blocked, LynxError


class TestPlan:
    def test_plan_lists_source_size_and_licence(self, tmp_path: Path) -> None:
        plan = images.plan_for("lineage-vanilla", tmp_path)
        text = plan.describe()
        assert plan.system_url.startswith("https://ota.waydro.id/")
        assert "Licence" in text
        assert "about" in text
        assert "Nothing is downloaded" in text

    def test_plan_states_install_needs_root(self, tmp_path: Path) -> None:
        plan = images.plan_for("lineage-vanilla", tmp_path)
        assert plan.requires_root_to_install

    def test_plan_admits_missing_checksums(self, tmp_path: Path) -> None:
        plan = images.plan_for("lineage-vanilla", tmp_path)
        assert not plan.checksums_available
        assert any("SHA-256" in step for step in plan.steps)

    def test_unknown_channel_is_refused(self, tmp_path: Path) -> None:
        with pytest.raises(LynxError, match="unknown image channel"):
            images.plan_for("mystery", tmp_path)

    def test_plan_is_deterministic(self, tmp_path: Path) -> None:
        first = images.plan_for("lineage-vanilla", tmp_path).describe()
        second = images.plan_for("lineage-vanilla", tmp_path).describe()
        assert first == second


class TestSpace:
    def test_space_check_reports_numbers(self, tmp_path: Path) -> None:
        enough, message = images.check_space(tmp_path, 10 * 1024**2)
        assert isinstance(enough, bool)
        assert "GiB" in message

    def test_huge_request_fails(self, tmp_path: Path) -> None:
        enough, message = images.check_space(tmp_path, 10**15)
        assert not enough
        assert "needed" in message

    def test_required_space_has_a_margin(self) -> None:
        assert images.required_space(1000) > 2000


class TestDownload:
    def test_download_requires_consent(self, tmp_path: Path) -> None:
        with pytest.raises(Blocked, match="consent"):
            images.download("https://example.invalid/x.zip", tmp_path / "x.zip", consent=False)

    def test_refuses_to_overwrite(self, tmp_path: Path) -> None:
        target = tmp_path / "x.zip"
        target.write_bytes(b"existing")
        with pytest.raises(LynxError, match="already exists"):
            images.download("https://example.invalid/x.zip", target, consent=True)
        assert target.read_bytes() == b"existing"

    def test_checksum_mismatch_deletes_the_partial_file(self, tmp_path: Path, monkeypatch) -> None:
        payload = b"not the expected bytes"

        class FakeResponse:
            def __init__(self) -> None:
                self._sent = False

            def read(self, _size: int) -> bytes:
                if self._sent:
                    return b""
                self._sent = True
                return payload

            def __enter__(self):
                return self

            def __exit__(self, *args):
                return False

        monkeypatch.setattr("urllib.request.urlopen", lambda *a, **k: FakeResponse())
        target = tmp_path / "image.zip"
        with pytest.raises(LynxError, match="checksum mismatch"):
            images.download(
                "https://example.invalid/x", target, consent=True, expected_sha256="0" * 64
            )
        assert not target.exists()
        assert not (tmp_path / "image.zip.part").exists()

    def test_successful_download_records_digest(self, tmp_path: Path, monkeypatch) -> None:
        payload = b"PK\x03\x04fake image payload"

        class FakeResponse:
            def __init__(self) -> None:
                self._sent = False

            def read(self, _size: int) -> bytes:
                if self._sent:
                    return b""
                self._sent = True
                return payload

            def __enter__(self):
                return self

            def __exit__(self, *args):
                return False

        monkeypatch.setattr("urllib.request.urlopen", lambda *a, **k: FakeResponse())
        import hashlib

        digest = hashlib.sha256(payload).hexdigest()
        target = tmp_path / "image.zip"
        result = images.download(
            "https://example.invalid/x", target, consent=True, expected_sha256=digest
        )
        assert result.verified
        assert result.path.read_bytes() == payload
        assert result.size_bytes == len(payload)


class TestInstructions:
    def test_instructions_are_explicit_and_do_not_execute(self) -> None:
        result = images.FetchResult(
            path=Path("/home/user/.cache/lynxdroid/images/system.img"),
            size_bytes=1024**3,
            sha256="a" * 64,
            source="https://ota.waydro.id/system",
            verified=False,
        )
        text = images.render_install_instructions(result)
        assert "sudo" in text
        assert "LynxDroid does not do this for you" in text
        assert "not" in text.lower() and "verified" in text.lower()
        assert result.sha256 in text
