"""Maximize the running SuperSlicer window, capture a PNG of it, then restore
the original window placement. Use when SuperSlicer is partially covered or
rendered at a small size and you want a full-resolution screenshot.

Why maximize-then-restore: SuperSlicer's PrintWindow capture pulls the actual
window content. If the window is partly off-screen or under another app, the
covered pixels come back grey/black. Maximizing forces the OS to lay out the
full window, after which the capture is clean. We then restore the prior
placement so we don't disrupt the user's layout.

Usage:
  conda run -n superslicer python -m gui_drive.snap_maximized [--out PATH]
"""
from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

# Pull in the bigger pyautogui-based helpers that already exist in the repo.
_REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(_REPO_ROOT.parent / "tools" / "scripts"))  # superslicer_auto.py
sys.path.insert(0, str(_REPO_ROOT))                               # capture_slicer_window.py

import superslicer_auto as auto  # noqa: E402
import win32gui, win32con          # noqa: E402


def snap(out: Path) -> Path:
    window_match = auto.find_ss_window(timeout=5.0)
    if not window_match:
        raise RuntimeError("SuperSlicer window not found. Is the GUI running?")
    hwnd, title = window_match
    print(f"[snap] window: {title!r}")

    placement = win32gui.GetWindowPlacement(hwnd)
    prev_show = placement[1]

    win32gui.ShowWindow(hwnd, win32con.SW_MAXIMIZE)
    time.sleep(0.6)
    try:
        win32gui.SetForegroundWindow(hwnd)
    except Exception:
        pass
    time.sleep(0.3)

    out.parent.mkdir(parents=True, exist_ok=True)
    auto.screenshot_window(hwnd, out)

    # Restore previous placement
    new_p = list(placement)
    new_p[1] = prev_show
    win32gui.SetWindowPlacement(hwnd, tuple(new_p))

    size_kb = out.stat().st_size / 1024
    print(f"[snap] wrote {out}  ({size_kb:.0f} KB)")
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path,
                        default=Path.home() / "AppData" / "Local" / "Temp" / "ss_max.png",
                        help="PNG output path")
    args = parser.parse_args()
    snap(args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
