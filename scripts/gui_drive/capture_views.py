"""Capture the running SuperSlicer's Gcode preview from multiple camera angles.

The SuperSlicer canvas accepts plain digit keys for camera presets when
focused (these are handled in `GLCanvas3D::on_char`):
  0 = iso       1 = top       2 = bottom
  3 = front     4 = rear      5 = left      6 = right

This script:
  1. Maximizes SuperSlicer (full-resolution screenshots)
  2. Optionally presses Ctrl+3 and selects Fan speed in G-code preview
  3. Clicks once in the 3D canvas to give SS keyboard focus
  4. Sends each requested digit, waits for the redraw, captures a PNG
  5. Restores the original window placement

Load the model before running this script. Use --prepare-fan-speed when the
model is on the plater and the capture should switch to sliced fan-speed view.

Usage:
  conda run -n superslicer python -m gui_drive.capture_views \\
      --out-dir test_models/slice_output/fan_view_overhang_test \\
      --views iso front left right
"""
from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

_REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(_REPO_ROOT.parent / "tools" / "scripts"))
sys.path.insert(0, str(_REPO_ROOT))

import superslicer_auto as auto  # noqa: E402
import win32gui, win32con          # noqa: E402
import pyautogui                    # noqa: E402

pyautogui.FAILSAFE = False
pyautogui.PAUSE = 0.10

VIEW_KEYS = {
    "iso":    "0",
    "top":    "1",
    "bottom": "2",
    "front":  "3",
    "rear":   "4",
    "left":   "5",
    "right":  "6",
}

FAN_SPEED_ARROW_OFFSET = (362, 126)
FAN_SPEED_ROW_OFFSET = (72, 224)


def prepare_fan_speed_preview(hwnd: int, slice_wait: float = 45.0) -> None:
    print("[views] Ctrl+3 -> G-code preview / auto-slice")
    pyautogui.hotkey("ctrl", "3")
    time.sleep(slice_wait)

    l, t, r, b = win32gui.GetWindowRect(hwnd)
    arrow_x = l + FAN_SPEED_ARROW_OFFSET[0]
    arrow_y = t + FAN_SPEED_ARROW_OFFSET[1]
    row_x = l + FAN_SPEED_ROW_OFFSET[0]
    row_y = t + FAN_SPEED_ROW_OFFSET[1]
    print(f"[views] Feature type arrow click: {arrow_x},{arrow_y}")
    pyautogui.click(arrow_x, arrow_y)
    time.sleep(1.5)
    print(f"[views] Fan speed row click: {row_x},{row_y}")
    pyautogui.click(row_x, row_y)
    time.sleep(1.8)

def capture_views(
    out_dir: Path,
    views: list[str],
    focus_x: float = 0.70,
    focus_y: float = 0.72,
    redraw_delay: float = 1.0,
    prepare_fan_speed: bool = False,
    slice_wait: float = 45.0,
) -> list[Path]:
    if not 0.0 <= focus_x <= 1.0 or not 0.0 <= focus_y <= 1.0:
        raise ValueError("focus_x and focus_y must be normalized window coordinates from 0.0 to 1.0")

    window_match = auto.find_ss_window(timeout=5.0)
    if not window_match:
        raise RuntimeError("SuperSlicer window not found.")
    hwnd, title = window_match
    print(f"[views] window: {title!r}")

    placement = win32gui.GetWindowPlacement(hwnd)
    prev_show = placement[1]
    written: list[Path] = []

    try:
        win32gui.ShowWindow(hwnd, win32con.SW_MAXIMIZE)
        time.sleep(0.6)
        try:
            win32gui.SetForegroundWindow(hwnd)
        except Exception:
            pass
        time.sleep(0.4)

        if prepare_fan_speed:
            prepare_fan_speed_preview(hwnd, slice_wait=slice_wait)

        l, t, r, b = win32gui.GetWindowRect(hwnd)
        focus_px = l + int((r - l) * focus_x)
        focus_py = t + int((b - t) * focus_y)
        print(f"[views] focus click: {focus_px},{focus_py} ({focus_x:.2f},{focus_y:.2f})")
        pyautogui.click(focus_px, focus_py)
        time.sleep(0.4)

        out_dir.mkdir(parents=True, exist_ok=True)
        for name in views:
            key = VIEW_KEYS.get(name)
            if not key:
                print(f"[views] skip unknown view: {name}")
                continue
            pyautogui.press(key)
            time.sleep(redraw_delay)
            path = out_dir / f"{name}.png"
            auto.screenshot_window(hwnd, path)
            size_kb = round(path.stat().st_size / 1024)
            print(f"[views] {name:6s} -> {path}  ({size_kb} KB)")
            written.append(path)
    finally:
        new_p = list(placement)
        new_p[1] = prev_show
        win32gui.SetWindowPlacement(hwnd, tuple(new_p))

    return written


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out-dir", type=Path,
                   default=Path.home() / "AppData" / "Local" / "Temp" / "ss_views",
                   help="Directory for the captured PNGs")
    parser.add_argument("--views", nargs="+",
                   default=["iso", "front", "left", "right"],
                   choices=list(VIEW_KEYS.keys()),
                   help="Camera angles to capture (default: iso front left right)")
    parser.add_argument("--focus-x", type=float, default=0.70,
                        help="Normalized window X coordinate for the canvas focus click")
    parser.add_argument("--focus-y", type=float, default=0.72,
                        help="Normalized window Y coordinate for the canvas focus click")
    parser.add_argument("--redraw-delay", type=float, default=1.0,
                        help="Seconds to wait after each camera key before capture")
    parser.add_argument("--prepare-fan-speed", action="store_true",
                        help="Press Ctrl+3, wait for slicing, and select Fan speed before capture")
    parser.add_argument("--slice-wait", type=float, default=45.0,
                        help="Seconds to wait after Ctrl+3 before selecting Fan speed")
    args = parser.parse_args()
    capture_views(
        args.out_dir,
        args.views,
        focus_x=args.focus_x,
        focus_y=args.focus_y,
        redraw_delay=args.redraw_delay,
        prepare_fan_speed=args.prepare_fan_speed,
        slice_wait=args.slice_wait,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
