"""Background execution for slow backend calls.

Rule for the whole GUI: the event loop never waits on a subprocess. Anything
that shells out (start/stop, doctor, app list, property writes) runs here and
returns through a Qt signal, so the window keeps painting and input keeps
flowing (stage 16: "GUI must not freeze on system operations").
"""

from __future__ import annotations

import traceback
from collections.abc import Callable
from typing import Any

from PySide6.QtCore import QObject, QRunnable, QThreadPool, Signal, Slot

from lynxdroid.core.logging_setup import get

log = get("gui.workers")


class TaskSignals(QObject):
    finished = Signal(object)  # result
    failed = Signal(str, str)  # message, traceback
    progressed = Signal(int, str)  # percent, message


class Task(QRunnable):
    """One unit of work executed on the thread pool."""

    def __init__(self, func: Callable[..., Any], *args: Any, **kwargs: Any) -> None:
        super().__init__()
        self.func = func
        self.args = args
        self.kwargs = kwargs
        self.signals = TaskSignals()
        self.setAutoDelete(True)

    @Slot()
    def run(self) -> None:
        try:
            result = self.func(*self.args, **self.kwargs)
        except Exception as exc:
            detail = traceback.format_exc(limit=6)
            log.error("background task failed: %s", exc)
            self.signals.failed.emit(str(exc) or exc.__class__.__name__, detail)
            return
        self.signals.finished.emit(result)


class TaskRunner:
    """Owns the thread pool and keeps tasks referenced until they finish."""

    def __init__(self, max_threads: int = 3) -> None:
        self._pool = QThreadPool()
        self._pool.setMaxThreadCount(max_threads)
        self._running: list[Task] = []

    def submit(
        self,
        func: Callable[..., Any],
        *args: Any,
        on_done: Callable[[Any], None] | None = None,
        on_error: Callable[[str, str], None] | None = None,
        **kwargs: Any,
    ) -> Task:
        task = Task(func, *args, **kwargs)
        if on_done is not None:
            task.signals.finished.connect(on_done)
        if on_error is not None:
            task.signals.failed.connect(on_error)
        task.signals.finished.connect(lambda _result, t=task: self._forget(t))
        task.signals.failed.connect(lambda _m, _d, t=task: self._forget(t))
        self._running.append(task)
        self._pool.start(task)
        return task

    def _forget(self, task: Task) -> None:
        if task in self._running:
            self._running.remove(task)

    def wait(self, timeout_ms: int = 5_000) -> bool:
        return self._pool.waitForDone(timeout_ms)

    @property
    def active(self) -> int:
        return len(self._running)

    def shutdown(self) -> None:
        self._pool.clear()
        self._pool.waitForDone(3_000)


class Poller(QObject):
    """Periodic backend status polling on the GUI thread, cheap by design.

    ``snapshot()`` for Waydroid costs one short ``waydroid status`` call, so
    the poll interval is conservative (2 s) and paused whenever the window is
    hidden or the backend is in a steady state with no actions in flight.
    """

    tick = Signal()

    def __init__(self, interval_ms: int = 2_000, parent: QObject | None = None) -> None:
        super().__init__(parent)
        from PySide6.QtCore import QTimer

        self._timer = QTimer(self)
        self._timer.setInterval(interval_ms)
        self._timer.timeout.connect(self.tick)
        self._paused = False

    def start(self) -> None:
        if not self._paused:
            self._timer.start()

    def stop(self) -> None:
        self._timer.stop()

    def pause(self) -> None:
        self._paused = True
        self._timer.stop()

    def resume(self) -> None:
        self._paused = False
        self._timer.start()

    @property
    def is_active(self) -> bool:
        return self._timer.isActive()
