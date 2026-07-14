#!/usr/bin/env python3
"""Launch or attach to SuperSlicer and control its integrated or standalone viewer."""

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


def default_main_exe() -> Path:
    return default_viewer_exe().with_name("superslicer.exe")


def wait_for_viewer(process_id: int, timeout: float):
    deadline = time.monotonic() + timeout
    desktop = Desktop(backend="uia")
    while time.monotonic() < deadline:
        for window in desktop.windows(process=process_id):
            title = window.window_text()
            if "SuperSlicer " in title and window.element_info.control_type == "Window":
                return window
        time.sleep(0.25)
    raise TimeoutError(f"SuperSlicer window did not appear within {timeout:g} seconds")


def window_button(window, title: str):
    button = next(
        (
            control
            for control in window.descendants(control_type="Button")
            if control.window_text() == title
        ),
        None,
    )
    if button is None:
        raise RuntimeError(f"Could not locate button: {title}")
    return button


def set_window_state(window, state: str) -> None:
    if state == "normal":
        window.restore()
    elif state == "maximized":
        window.maximize()
    elif state == "minimized":
        window.minimize()


def resize_window(window, x: int | None, y: int | None, width: int | None, height: int | None) -> None:
    win32gui = import_module("win32gui")
    win32con = import_module("win32con")
    left, top, right, bottom = win32gui.GetWindowRect(window.handle)
    target_x = left if x is None else x
    target_y = top if y is None else y
    target_width = right - left if width is None else width
    target_height = bottom - top if height is None else height
    win32gui.SetWindowPos(
        window.handle,
        0,
        target_x,
        target_y,
        target_width,
        target_height,
        win32con.SWP_NOACTIVATE | win32con.SWP_NOZORDER,
    )


def select_main_view(window, view: str, slice_model: bool, timeout: float) -> None:
    if slice_model:
        window_button(window, "Slice now").click_input()
    title = {
        "3d": "3D view",
        "sliced": "Sliced preview",
        "gcode": "Gcode preview",
    }[view]
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            window_button(window, title).click_input()
            if view == "3d":
                return
            viewer_sliders(window)
            return
        except (RuntimeError, LookupError):
            time.sleep(0.5)
    raise TimeoutError(f"Integrated {title} did not become ready within {timeout:g} seconds")


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


def manipulation_edits(window):
    rows: dict[int, list] = {}
    for control in window.descendants(control_type="Edit"):
        top = control.rectangle().top
        key = next((row_top for row_top in rows if abs(row_top - top) <= 4), top)
        rows.setdefault(key, []).append(control)
    grid = [
        sorted(rows[top], key=lambda control: control.rectangle().left)
        for top in sorted(rows)
    ]
    grid = [row for row in grid if len(row) == 3]
    if len(grid) < 4:
        raise RuntimeError(
            "Could not locate the object manipulation Position/Rotation/Scale/Size fields"
        )
    return grid


def set_rotation(window, axis: int, degrees: float) -> None:
    # Select every object so the sidebar manipulation fields are active.
    window.type_keys("^a", pause=0.05)
    time.sleep(0.5)
    # Sidebar row order: Position, Rotation, Scale factors, Size [World].
    field = manipulation_edits(window)[1][axis]
    field.click_input()
    field.type_keys("^a", pause=0.05)
    field.type_keys(f"{degrees:g}", with_spaces=False, pause=0.05)
    field.type_keys("{ENTER}", pause=0.05)
    # Give the plater time to apply the transform and re-arrange.
    time.sleep(1.5)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Launch or attach to SuperSlicer, select its integrated or standalone viewer, "
            "control window state and sliders, and capture the resulting window."
        )
    )
    parser.add_argument(
        "input",
        type=Path,
        nargs="?",
        help="G-code to open, or an FFF model to slice and view (omit with --process-id)",
    )
    parser.add_argument("--viewer-exe", type=Path, default=default_viewer_exe())
    parser.add_argument(
        "--process-id",
        type=int,
        help="Attach to SuperSlicer already launched by Visual Studio instead of starting one",
    )
    parser.add_argument(
        "--main-window",
        action="store_true",
        help="Use the normal plater window and its integrated Preview controls",
    )
    parser.add_argument(
        "--slice",
        action="store_true",
        help="Click Slice now before selecting an integrated preview",
    )
    parser.add_argument(
        "--select-view",
        choices=("3d", "sliced", "gcode"),
        help="Select a view in the main window",
    )
    parser.add_argument(
        "--window-state",
        choices=("normal", "maximized", "minimized"),
        help="Restore, maximize, or minimize the controlled window",
    )
    parser.add_argument("--window-x", type=int, help="Set the HWND left edge in screen pixels")
    parser.add_argument("--window-y", type=int, help="Set the HWND top edge in screen pixels")
    parser.add_argument("--window-width", type=int, help="Set the HWND width in pixels")
    parser.add_argument("--window-height", type=int, help="Set the HWND height in pixels")
    parser.add_argument(
        "--rotate-x",
        type=float,
        help="Select all objects and set the sidebar Rotation X field to this many degrees before slicing",
    )
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
    viewer_process = None
    if args.process_id is None:
        if args.input is None:
            raise ValueError("input is required unless --process-id is supplied")
        executable = (
            default_main_exe() if args.main_window else args.viewer_exe
        ).resolve()
        input_path = args.input.resolve()
        if not executable.is_file():
            raise FileNotFoundError(f"SuperSlicer executable not found: {executable}")
        if not input_path.is_file():
            raise FileNotFoundError(f"Input file not found: {input_path}")

        slicer_args = list(args.slicer_args)
        if slicer_args and slicer_args[0] == "--":
            slicer_args.pop(0)
        if args.main_window:
            command = [str(executable), *slicer_args, str(input_path)]
        else:
            command = [str(executable), *slicer_args, "--gcodeviewer", str(input_path)]
        viewer_process = subprocess.Popen(
            command,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        process_id = viewer_process.pid
    else:
        if args.input is not None or args.slicer_args:
            raise ValueError("input and slicer arguments cannot be used with --process-id")
        process_id = args.process_id

    if (args.slice or args.select_view or args.rotate_x is not None) and not args.main_window:
        raise ValueError("--slice, --select-view, and --rotate-x require --main-window")

    window = None
    try:
        window = wait_for_viewer(process_id, args.timeout)
        window.set_focus()
        resize_requested = any(
            value is not None
            for value in (args.window_x, args.window_y, args.window_width, args.window_height)
        )
        if resize_requested:
            window.restore()
            resize_window(
                window,
                args.window_x,
                args.window_y,
                args.window_width,
                args.window_height,
            )
            time.sleep(0.5)
        if args.window_state is not None and args.window_state != "minimized":
            set_window_state(window, args.window_state)
            time.sleep(0.5)

        if args.main_window and args.rotate_x is not None:
            set_rotation(window, 0, args.rotate_x)

        if args.main_window and args.select_view is not None:
            select_main_view(window, args.select_view, args.slice, args.timeout)

        vertical = horizontal = None
        if (
            not args.main_window
            or args.select_view in ("sliced", "gcode")
            or args.layer_ratio is not None
            or args.move_ratio is not None
        ):
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
        if args.window_state == "minimized":
            set_window_state(window, "minimized")
        print(f"viewer_pid={process_id}")
        print(f"window_handle={window.handle}")
        print(f"window_rect={window.rectangle()}")
        if args.keep_open:
            return 0
        window.close()
        if viewer_process is not None:
            viewer_process.wait(timeout=10)
            return viewer_process.returncode or 0
        return 0
    finally:
        if (
            not args.keep_open
            and viewer_process is not None
            and viewer_process.poll() is None
        ):
            if window is not None:
                window.close()
            viewer_process.terminate()


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"gcode_viewer_control: {exc}", file=sys.stderr)
        raise SystemExit(1)
