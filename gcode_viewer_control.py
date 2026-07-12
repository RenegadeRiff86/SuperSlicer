#!/usr/bin/env python3
"""Launch and control SuperSlicer's wxWidgets G-code viewer on Windows."""

from __future__ import annotations

import argparse
import subprocess
import sys
import time
from importlib import import_module
from pathlib import Path


CENTER_DIVISOR = 2
Desktop = import_module("pywinauto").Desktop


def ratio(value: str) -> float:
    parsed = float(value)
    if not 0.0 <= parsed <= 1.0:
        raise argparse.ArgumentTypeError("ratio must be between 0 and 1")
    return parsed


def default_viewer_exe() -> Path:
    return (
        Path(__file__).resolve().parent
        / "build-default"
        / "src"
        / "RelWithDebInfo"
        / "superslicer_console.exe"
    )


def wait_for_viewer(process_id: int, timeout: float):
    deadline = time.monotonic() + timeout
    desktop = Desktop(backend="uia")
    while time.monotonic() < deadline:
        for window in desktop.windows(process=process_id):
            title = window.window_text()
            if "SuperSlicer " in title and window.element_info.control_type == "Window":
                return window
        time.sleep(0.25)
    raise TimeoutError(f"SuperSlicer viewer window did not appear within {timeout:g} seconds")


def viewer_sliders(window):
    controls = [
        control
        for control in window.descendants(control_type="Pane")
        if control.window_text() == "control"
    ]
    vertical = next(
        (control for control in controls if control.rectangle().height() > control.rectangle().width()),
        None,
    )
    horizontal = next(
        (control for control in controls if control.rectangle().width() > control.rectangle().height()),
        None,
    )
    if vertical is None or horizontal is None:
        raise RuntimeError("Could not locate the G-code viewer layer and move sliders")
    return vertical, horizontal


def set_layer_ratio(control, value: float) -> None:
    rect = control.rectangle()
    margin = min(10, max(1, rect.height() // 20))
    y = round((1.0 - value) * (rect.height() - CENTER_DIVISOR * margin)) + margin
    control.click_input(coords=(rect.width() // CENTER_DIVISOR, y))


def set_move_ratio(control, value: float) -> None:
    rect = control.rectangle()
    margin = min(10, max(1, rect.width() // 100))
    x = round(value * (rect.width() - CENTER_DIVISOR * margin)) + margin
    control.click_input(coords=(x, rect.height() // CENTER_DIVISOR))


def zoom_view(window, notches: float, focus_x: float, focus_y: float) -> None:
    mouse = import_module("pywinauto").mouse
    rect = window.rectangle()
    x = rect.left + round(focus_x * rect.width())
    y = rect.top + round(focus_y * rect.height())
    mouse.move(coords=(x, y))
    time.sleep(0.2)
    step = 1 if notches > 0 else -1
    for _ in range(int(abs(notches))):
        # The G-code viewer zooms toward the cursor, so keep the pointer on the focus point.
        mouse.scroll(coords=(x, y), wheel_dist=step)
        time.sleep(0.05)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Launch SuperSlicer's G-code viewer, optionally move its custom layer/move "
            "sliders, and capture the resulting window."
        )
    )
    parser.add_argument("input", type=Path, help="G-code to open, or an FFF model to slice and view")
    parser.add_argument("--viewer-exe", type=Path, default=default_viewer_exe())
    parser.add_argument("--layer-ratio", type=ratio, help="Visible upper layer, 0=bottom and 1=top")
    parser.add_argument("--move-ratio", type=ratio, help="Visible toolpath progress, 0=start and 1=end")
    parser.add_argument("--zoom", type=float, default=None, help="Zoom the 3D view by N mouse-wheel notches (positive=in, negative=out)")
    parser.add_argument("--zoom-x", type=ratio, default=0.5, help="Horizontal focus point for zoom within the window, 0=left 1=right (default 0.5)")
    parser.add_argument("--zoom-y", type=ratio, default=0.5, help="Vertical focus point for zoom within the window, 0=top 1=bottom (default 0.5)")
    parser.add_argument("--screenshot", type=Path, help="Save the controlled viewer window as PNG")
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument("--keep-open", action="store_true")
    parser.add_argument(
        "slicer_args",
        nargs=argparse.REMAINDER,
        help="Additional slicer arguments after '--', for example -- --load profile.ini --output out.gcode",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    viewer_exe = args.viewer_exe.resolve()
    input_path = args.input.resolve()
    if not viewer_exe.is_file():
        raise FileNotFoundError(f"Viewer executable not found: {viewer_exe}")
    if not input_path.is_file():
        raise FileNotFoundError(f"Input file not found: {input_path}")

    slicer_args = list(args.slicer_args)
    if slicer_args and slicer_args[0] == "--":
        slicer_args.pop(0)
    command = [str(viewer_exe), *slicer_args, "--gcodeviewer", str(input_path)]
    viewer_process = subprocess.Popen(
        command,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    window = None
    try:
        window = wait_for_viewer(viewer_process.pid, args.timeout)
        window.set_focus()
        vertical, horizontal = viewer_sliders(window)
        if args.layer_ratio is not None:
            set_layer_ratio(vertical, args.layer_ratio)
        if args.move_ratio is not None:
            set_move_ratio(horizontal, args.move_ratio)
        if args.zoom is not None:
            zoom_view(window, args.zoom, args.zoom_x, args.zoom_y)
        time.sleep(1.0)
        if args.screenshot is not None:
            screenshot = args.screenshot.resolve()
            screenshot.parent.mkdir(parents=True, exist_ok=True)
            window.capture_as_image().save(screenshot)
            print(screenshot)
        print(f"viewer_pid={viewer_process.pid}")
        if args.keep_open:
            return 0
        window.close()
        viewer_process.wait(timeout=10)
        return viewer_process.returncode or 0
    finally:
        if not args.keep_open and viewer_process.poll() is None:
            if window is not None:
                window.close()
            viewer_process.terminate()


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"gcode_viewer_control: {exc}", file=sys.stderr)
        raise SystemExit(1)
