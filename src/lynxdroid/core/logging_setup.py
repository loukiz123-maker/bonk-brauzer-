"""Structured logging with redaction.

Logs go to ``$XDG_STATE_HOME/lynxdroid/lynxdroid.log`` and, optionally, to a
caller-provided sink (the GUI console). Three rules:

1. no secrets: tokens/cookies/home paths are redacted before formatting;
2. no unbounded growth: the handler rotates at 1 MiB, 3 files;
3. logs never block the UI thread (the handler is a plain file handler, and
   nothing in the app logs inside a paint event).
"""

from __future__ import annotations

import logging
import logging.handlers
import re
from pathlib import Path

_REDACTIONS: tuple[tuple[re.Pattern[str], str], ...] = (
    (re.compile(r"(?i)(password|passwd|token|secret|api[_-]?key)=([^\s&]+)"), r"\1=<redacted>"),
    (re.compile(r"(?i)(authorization:\s*bearer\s+)(\S+)"), r"\1<redacted>"),
    (re.compile(r"/home/([^/\s]+)/"), r"/home/<user>/"),
)

_LOG_FORMAT = "%(asctime)s %(levelname)-7s %(name)-22s %(message)s"
_configured = False


class RedactingFormatter(logging.Formatter):
    """Format and scrub well-known secret patterns from the final message."""

    def format(self, record: logging.LogRecord) -> str:
        text = super().format(record)
        return redact(text)


def redact(text: str) -> str:
    for pattern, replacement in _REDACTIONS:
        text = pattern.sub(replacement, text)
    return text


def configure(
    log_file: Path | None = None,
    *,
    level: int = logging.INFO,
    console_level: int | None = None,
) -> logging.Logger:
    """Configure the root ``lynxdroid`` logger once. Idempotent."""
    global _configured
    root = logging.getLogger("lynxdroid")
    root.setLevel(level)
    if _configured:
        return root

    formatter = RedactingFormatter(_LOG_FORMAT)

    if log_file is not None:
        log_file = Path(log_file)
        log_file.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        file_handler = logging.handlers.RotatingFileHandler(
            log_file, maxBytes=1_048_576, backupCount=3, encoding="utf-8"
        )
        file_handler.setFormatter(formatter)
        file_handler.setLevel(level)
        root.addHandler(file_handler)

    if console_level is not None:
        stream = logging.StreamHandler()
        stream.setFormatter(formatter)
        stream.setLevel(console_level)
        root.addHandler(stream)

    root.propagate = False
    _configured = True
    return root


def get(name: str) -> logging.Logger:
    """Child logger, e.g. ``get("backend.waydroid")``."""
    return logging.getLogger(f"lynxdroid.{name}")
