"""Shared fixtures. Every test runs against a temporary XDG root."""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

SRC = Path(__file__).resolve().parents[1] / "src"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

from lynxdroid.core.paths import AppPaths  # noqa: E402
from lynxdroid.display.geometry import Size, ViewportGeometry  # noqa: E402
from lynxdroid.profiles.model import (  # noqa: E402
    Element,
    ElementType,
    Profile,
    Target,
)


@pytest.fixture()
def app_paths(tmp_path: Path) -> AppPaths:
    paths = AppPaths.resolve(root=tmp_path / "xdg")
    paths.ensure()
    return paths


@pytest.fixture(autouse=True)
def _isolate_environment(monkeypatch: pytest.MonkeyPatch, tmp_path: Path) -> None:
    """Keep tests away from the real home directory and session variables."""
    monkeypatch.setenv("HOME", str(tmp_path / "home"))
    (tmp_path / "home").mkdir(exist_ok=True)
    for name in ("XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME", "XDG_STATE_HOME"):
        monkeypatch.delenv(name, raising=False)


@pytest.fixture()
def target() -> Target:
    return Target(package="com.example.game", package_version="1.2.3")


@pytest.fixture()
def geometry() -> ViewportGeometry:
    return ViewportGeometry(buffer=Size(1920, 1080), surface=Size(960, 540))


@pytest.fixture()
def tap_element() -> Element:
    return Element(id="tap-a", type=ElementType.TAP, label="A", x=0.75, y=0.5, keys=("j",))


def make_profile(target: Target, *elements: Element) -> Profile:
    return Profile.new("Test layout", target, elements=elements)


@pytest.fixture()
def profile(target: Target, tap_element: Element) -> Profile:
    dpad = Element(
        id="dpad-move",
        type=ElementType.DPAD,
        label="MOVE",
        x=0.2,
        y=0.75,
        dpad_keys=("w", "d", "s", "a"),
        dpad_radius=0.08,
    )
    hold = Element(
        id="hold-fire",
        type=ElementType.HOLD,
        label="FIRE",
        x=0.85,
        y=0.8,
        keys=("k",),
    )
    return Profile.new("Shooter", target, elements=(tap_element, dpad, hold))
