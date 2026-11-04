#!/usr/bin/env python3
"""On/off switch for the SuperSlicer automation API.

Off by default. The MCP server and gui_drive share this file, so turning the
API on in one place turns it on for the other. The slicer itself still listens
only when it was started with --automation-api.
"""

from __future__ import annotations

import os
from pathlib import Path

MODE_PATH = Path.home() / ".config" / "SuperSlicer" / "automation-debug-mode"
ENV_OVERRIDE = "SUPERSLICER_AUTOMATION_MCP"


def debug_enabled() -> bool:
    override = os.environ.get(ENV_OVERRIDE)
    if override == "1":
        return True
    if override == "0":
        return False
    try:
        return MODE_PATH.read_text(encoding="utf-8").strip() == "on"
    except OSError:
        return False


def set_debug_enabled(enabled: bool) -> None:
    MODE_PATH.parent.mkdir(parents=True, exist_ok=True)
    payload = b"on\n" if enabled else b"off\n"
    fd = os.open(MODE_PATH, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    try:
        os.write(fd, payload)
    finally:
        os.close(fd)


def status() -> dict[str, object]:
    return {"enabled": debug_enabled(), "path": str(MODE_PATH)}
