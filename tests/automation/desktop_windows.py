#!/usr/bin/env python3
"""Place SuperSlicer and Cursor side by side through KWin.

The slicer's own window command moves its frame and then raises it, which covers
the chat whenever the program is large or fullscreen. This talks to the compositor
instead, so Cursor can stay in front while the slicer remains on screen.
"""

from __future__ import annotations

import os
import subprocess
import textwrap
import time
from pathlib import Path

SCRIPT_NAME = "superslicer-desktop-windows"
BUS_NAME = "org.kde.KWin"


class DesktopWindowError(RuntimeError):
    """KWin did not accept a window placement request."""


def _session_env() -> dict[str, str]:
    env = os.environ.copy()
    runtime = env.get("XDG_RUNTIME_DIR") or f"/run/user/{os.getuid()}"
    env["XDG_RUNTIME_DIR"] = runtime
    env.setdefault("DBUS_SESSION_BUS_ADDRESS", f"unix:path={runtime}/bus")
    return env


def _qdbus(env: dict[str, str], *args: str, required: bool = True) -> str:
    result = subprocess.run(
        ["qdbus6", BUS_NAME, *args],
        env=env,
        check=False,
        text=True,
        capture_output=True,
    )
    if result.returncode != 0 and required:
        detail = (result.stderr or result.stdout).strip()
        raise DesktopWindowError(f"qdbus6 {' '.join(args)} failed: {detail}")
    return result.stdout.strip()


def place(cursor_fraction: float = 0.56) -> str:
    """Unfullscreen both programs, put Cursor on the left, SuperSlicer on the right."""
    if not 0.3 <= cursor_fraction <= 0.8:
        raise DesktopWindowError(f"cursor fraction must be between 0.3 and 0.8, got {cursor_fraction}")

    marker = f"KWINPLACE{int(time.time())}"
    script = textwrap.dedent(
        f"""\
        const area = workspace.clientArea(KWin.MaximizeArea, workspace.activeScreen, workspace.currentDesktop);
        const cursorWidth = Math.round(area.width * {cursor_fraction});
        const slicerX = area.x + cursorWidth;
        const slicerWidth = area.width - cursorWidth;
        let cursor = null;
        workspace.windowList().forEach((window) => {{
            const klass = String(window.resourceClass).toLowerCase();
            if (klass !== "cursor" && klass !== "superslicer")
                return;
            window.fullScreen = false;
            window.frameGeometry = {{
                x: klass === "cursor" ? area.x : slicerX,
                y: area.y,
                width: klass === "cursor" ? cursorWidth : slicerWidth,
                height: area.height,
            }};
            const geo = window.frameGeometry;
            print(["{marker}", klass, window.pid, geo.x, geo.y, geo.width, geo.height, JSON.stringify(String(window.caption))].join("\\t"));
            if (klass === "cursor")
                cursor = window;
        }});
        if (cursor)
            workspace.activeWindow = cursor;
        """
    )
    path = Path("/tmp") / f"{SCRIPT_NAME}.js"
    path.write_text(script, encoding="utf-8")
    env = _session_env()
    _qdbus(env, "/Scripting", "org.kde.kwin.Scripting.unloadScript", SCRIPT_NAME, required=False)
    _qdbus(env, "/Scripting", "org.kde.kwin.Scripting.loadScript", str(path), SCRIPT_NAME)
    _qdbus(env, "/Scripting", "org.kde.kwin.Scripting.start")
    time.sleep(0.4)
    log = subprocess.run(
        ["journalctl", "--user", "-n", "40", "--no-pager", "-g", marker],
        env=env,
        check=False,
        text=True,
        capture_output=True,
    )
    _qdbus(env, "/Scripting", "org.kde.kwin.Scripting.unloadScript", SCRIPT_NAME, required=False)
    placed = []
    for line in (log.stdout or "").splitlines():
        if marker not in line:
            continue
        fields = line.split(marker, 1)[1].split()
        if len(fields) >= 6:
            placed.append(" ".join(fields[:6]))
    classes = {line.split()[0] for line in placed}
    if "cursor" not in classes:
        raise DesktopWindowError("KWin placed no Cursor window")
    if "superslicer" not in classes:
        raise DesktopWindowError("KWin placed no SuperSlicer window")
    return "\n".join(placed)
