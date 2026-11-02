#!/usr/bin/env python3
"""Drive a running SuperSlicer through its embedded automation API.

Typical session - start the slicer once in this terminal, then attach for each step.
Do not background launch. Klipper Z-offset through to a print host is
`z-offset-workflow` (see tests/automation/README.md).

    export SUPERSLICER_AUTOMATION_TOKEN=$(openssl rand -hex 16)
    ./tests/automation/gui_drive.py launch        # stays in this terminal
    ./tests/automation/gui_drive.py --attach menus
    ./tests/automation/gui_drive.py --attach tab print_settings
    ./tests/automation/gui_drive.py --attach options --filter perimeter
    ./tests/automation/gui_drive.py --attach set superslicer.option.perimeters 4
    ./tests/automation/gui_drive.py --attach menu superslicer.menu.calibration... --wait-modal

Preview hides most move types; enable the one under test before a screenshot compare.

    ./tests/automation/gui_drive.py --attach slice
    ./tests/automation/gui_drive.py --attach view preview
    ./tests/automation/gui_drive.py --attach preview-options wipe=true
    ./tests/automation/gui_drive.py --attach screenshot before.png --ref superslicer.canvas.preview

Every command prints plain lines rather than JSON so the output is readable in a
terminal and greppable in a script.
"""

from __future__ import annotations

import argparse
import os
import sys
import time
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from automation_client import (  # noqa: E402  (path set up above)
    DEFAULT_EXECUTABLE,
    DEFAULT_PORT,
    TOKEN_VARIABLE,
    ApiClient,
    AutomationError,
    launch,
    terminate,
    wait_closed,
    wait_process_gone,
    wait_ready,
)
from flow_calibration_workflow import (  # noqa: E402
    apply_result as apply_flow_result,
    generate_test as generate_flow_test,
    open_dialog as open_flow_dialog,
)
from z_offset_workflow import (  # noqa: E402
    apply_result,
    generate_test,
    inspect_gcode,
    regression_check,
    run_workflow,
    send_gcode,
)

MENU_PREFIX = "superslicer.menu."
OPTION_PREFIX = "superslicer.option."
TAB_PREFIX = "superslicer.tab."

# "Export plate as ..." picks its format from the file dialog's FILTER INDEX, not from the file
# name, so driving it means arming the index. This order is the wildcard Plater::export_platter()
# builds, and it must stay in step with ExportPlatterFilter in Plater.cpp.
EXPORT_PLATE_MENU = "file.export.export_plate"
EXPORT_PLATE_FILTERS = {"stl": 0, "obj": 1, "3mf": 2, "amf": 3}


def describe(element: dict) -> str:
    state = element["state"]
    flags = "".join(
        letter if state[key] else "-"
        for key, letter in (("shown", "s"), ("enabled", "e"), ("focused", "f"))
    )
    value = element["value"]
    suffix = "" if value is None else f"  = {value!r}"
    return (
        f"{element['automation_id'] or '(unnamed)':<52} {element['role']:<15} "
        f"[{flags}] {element['name']!r}{suffix}"
    )


def print_elements(elements: list[dict]) -> None:
    for element in elements:
        print(describe(element))
    print(f"{len(elements)} element(s)")


def command_snapshot(client: ApiClient, args: argparse.Namespace) -> None:
    snapshot = client.snapshot(include_hidden=args.hidden)
    print(f"scope={snapshot['scope_automation_id']} modal={snapshot['modal']}")
    elements = snapshot["elements"]
    if args.filter:
        needle = args.filter.lower()
        elements = [
            element
            for element in elements
            if needle in element["automation_id"].lower()
            or needle in element["name"].lower()
            or needle in element["role"].lower()
        ]
    print_elements(elements)


def command_menus(client: ApiClient, args: argparse.Namespace) -> None:
    elements = client.elements(prefix=MENU_PREFIX)
    if args.filter:
        needle = args.filter.lower()
        elements = [e for e in elements if needle in e["automation_id"].lower()]
    print_elements(elements)


def command_options(client: ApiClient, args: argparse.Namespace) -> None:
    elements = client.elements(prefix=OPTION_PREFIX)
    if args.filter:
        needle = args.filter.lower()
        elements = [e for e in elements if needle in e["automation_id"].lower()]
    print_elements(elements)


def command_tabs(client: ApiClient, args: argparse.Namespace) -> None:
    print_elements(client.elements(prefix=TAB_PREFIX))


def command_tab(client: ApiClient, args: argparse.Namespace) -> None:
    automation_id = args.name
    if not automation_id.startswith(TAB_PREFIX):
        automation_id = TAB_PREFIX + automation_id
    print(client.invoke(automation_id))


def command_invoke(client: ApiClient, args: argparse.Namespace) -> None:
    # Automation ids are not unique: an unnamed control falls back to its wx class
    # name, so a dialog routinely exposes several elements all called "button" and
    # the bare id hits whichever comes first. --label picks by visible text, which
    # is the only thing that actually distinguishes them.
    if args.label is not None:
        print(client.invoke(args.automation_id, name=args.label))
    else:
        print(client.invoke(args.automation_id))
    settle(client, args)


def command_menu(client: ApiClient, args: argparse.Namespace) -> None:
    automation_id = args.automation_id
    if not automation_id.startswith(MENU_PREFIX):
        automation_id = MENU_PREFIX + automation_id
    print(client.invoke(automation_id))
    settle(client, args)


def command_set(client: ApiClient, args: argparse.Namespace) -> None:
    automation_id = args.automation_id
    if not automation_id.startswith(OPTION_PREFIX) and "." not in automation_id:
        automation_id = OPTION_PREFIX + automation_id
    print(client.set_value(automation_id, parse_value(args.value)))


def command_toggle(client: ApiClient, args: argparse.Namespace) -> None:
    automation_id = args.automation_id
    if not automation_id.startswith(OPTION_PREFIX) and "." not in automation_id:
        automation_id = OPTION_PREFIX + automation_id
    print(client.toggle(automation_id))


def command_screenshot(client: ApiClient, args: argparse.Namespace) -> int:
    ref = None
    if args.ref is not None:
        ref = client.element(automation_id=args.ref)["ref"]
    png = client.screenshot(ref)
    if args.path is not None:
        Path(args.path).write_bytes(png)
        print(f"{args.path} ({len(png)} bytes)")
    if args.compare is None:
        return 0
    region = parse_region(args.region) if args.region else None
    report, identical = compare_png(Path(args.compare).read_bytes(), png, region)
    print(report)
    return 0 if identical else 1


def parse_region(text: str) -> tuple[int, int, int, int]:
    values = text.split(",")
    if len(values) != 4:
        raise AutomationError(f"--region wants x,y,width,height; got {text!r}")
    try:
        x, y, width, height = (int(value) for value in values)
    except ValueError:
        raise AutomationError(f"--region wants four integers; got {text!r}") from None
    if width <= 0 or height <= 0:
        raise AutomationError(f"--region width and height must be positive; got {text!r}")
    return x, y, width, height


def compare_png(
    baseline: bytes, captured: bytes, region: tuple[int, int, int, int] | None = None
) -> tuple[str, bool]:
    """Compare two captures of the same view. The toolpath rendering is bit-deterministic
    even across separate processes, so two runs of an unchanged build return identical bytes
    - which makes a byte comparison a sound first test, and a free one. Only a real
    difference is worth the pixel arithmetic.

    The canvas capture also holds whatever the notification manager is drawing in its bottom
    right corner, and that is not deterministic: it fades on its own clock. Pass a region to
    compare the scene alone rather than chasing a toast that has nothing to do with the
    change under test."""
    if baseline == captured:
        return f"identical ({len(captured)} bytes)", True
    try:
        import io

        import numpy
        from PIL import Image
    except ImportError:
        return (
            f"differs: {len(baseline)} vs {len(captured)} bytes "
            "(install numpy and pillow to see which pixels)",
            False,
        )

    before = numpy.asarray(Image.open(io.BytesIO(baseline)).convert("RGB"), dtype=numpy.int16)
    after = numpy.asarray(Image.open(io.BytesIO(captured)).convert("RGB"), dtype=numpy.int16)
    if before.shape != after.shape:
        return f"differs: {before.shape} vs {after.shape}", False

    left, top = 0, 0
    if region is not None:
        left, top, width, height = region
        whole = captured_shape(after)
        before = before[top:top + height, left:left + width]
        after = after[top:top + height, left:left + width]
        if before.size == 0:
            return f"region {region} lies outside the {whole} capture", False

    delta = numpy.abs(after - before)
    changed = delta.any(axis=2)
    count = int(changed.sum())
    if count == 0:
        return f"identical inside region {region}", True

    # Reported in whole-image coordinates even when a region was compared, so the numbers
    # can be fed straight back in as the next --region.
    rows, columns = numpy.nonzero(changed)
    return (
        f"differs: {count} of {changed.size} pixels "
        f"({100.0 * count / changed.size:.4f}%), max channel delta {int(delta.max())}, "
        f"bounds x {left + columns.min()}-{left + columns.max()} "
        f"y {top + rows.min()}-{top + rows.max()}",
        False,
    )


def captured_shape(image) -> str:
    return f"{image.shape[1]}x{image.shape[0]}"


def command_load(client: ApiClient, args: argparse.Namespace) -> None:
    print(client.load_model(args.model))


def command_slice(client: ApiClient, args: argparse.Namespace) -> None:
    print(client.slice())


def command_view(client: ApiClient, args: argparse.Namespace) -> None:
    print(client.select_view(args.view))


def command_preview_options(client: ApiClient, args: argparse.Namespace) -> None:
    requested: dict[str, bool] = {}
    for assignment in args.option or ():
        name, separator, text = assignment.partition("=")
        value = parse_value(text) if separator else None
        if not isinstance(value, bool):
            raise AutomationError(f"expected name=true or name=false, got {assignment!r}")
        requested[name] = value
    for name, visible in sorted(client.set_preview(options=requested)["options"].items()):
        print(f"{'on ' if visible else 'off'} {name}")


def command_quit(client: ApiClient, args: argparse.Namespace) -> int:
    pid = client.status()["pid"]
    print(client.quit())
    if not wait_closed(client, args.timeout):
        print(f"still answering on port {client.port} after {args.timeout:g}s")
        return 1
    if not wait_process_gone(pid, args.timeout):
        print(f"port {client.port} is free but pid {pid} is still alive")
        return 1
    print(f"closed: port {client.port} free, pid {pid} gone")
    return 0


def command_export(client: ApiClient, args: argparse.Namespace) -> None:
    print(client.export_gcode(args.path))


def sniff_mesh_format(path: Path) -> str:
    """Name the format a file actually holds, from its bytes rather than its extension.

    Plate export chooses the format by filter index, so a mismatch there writes a perfectly
    well-formed file of the wrong format under the right name. Only the content catches that.
    """
    size = path.stat().st_size
    with path.open("rb") as handle:
        head = handle.read(512)

    if head[:4] == b"PK\x03\x04":
        # 3MF and AMF are both zip containers; only the entries tell them apart.
        with zipfile.ZipFile(path) as archive:
            names = archive.namelist()
        if any(name.endswith("3dmodel.model") for name in names):
            return "3mf"
        if any(name.lower().endswith(".amf") for name in names):
            return "amf"
        return "zip:" + ",".join(names[:3])

    # Test the binary layout before the "solid" prefix: a binary STL's 80-byte header is free
    # text and writers do put the word "solid" in it, which is the classic misdetection.
    if size >= 84 and size == 84 + 50 * int.from_bytes(head[80:84], "little"):
        return "stl-binary"

    text = head.decode("ascii", "replace")
    if text.lstrip().lower().startswith("solid"):
        return "stl-ascii"
    if any(line.startswith(("v ", "vn ", "f ", "o ", "mtllib ")) for line in text.splitlines()):
        return "obj"
    return "unknown"


def command_export_plate(client: ApiClient, args: argparse.Namespace) -> int:
    """Drive File > Export > Export plate as STL/OBJ/3MF/AMF and report what was written.

    The answer has to be armed before the menu item runs, because ShowModal() blocks the very
    thread that would answer it. store_amf() also rewrites the name to '.zip.amf', so the file
    that appears is not always the one asked for - hence the search for what actually changed.
    """
    requested = Path(args.path).expanduser().resolve()
    # Compare against a stamp taken first, so a stale file left by an earlier run is not
    # mistaken for this one's output. Nothing is deleted; the export overwrites in place.
    candidates = [requested, requested.with_name(requested.stem + ".zip.amf")]
    before = {path: (path.stat().st_mtime_ns, path.stat().st_size) if path.exists() else None
              for path in candidates}

    client.clear_file_dialogs()
    client.arm_file_dialog(
        answer="ok", paths=[requested], filter_index=EXPORT_PLATE_FILTERS[args.format]
    )
    client.invoke(MENU_PREFIX + EXPORT_PLATE_MENU)
    settle(client, args)

    # Invoking a menu item only queues the command, so the export is still running when the
    # call returns. Wait on the file itself - and then on its size holding still, since a
    # half-written archive sniffs as garbage rather than as the format it will end up being.
    def stamp(path: Path) -> tuple[int, int] | None:
        return (path.stat().st_mtime_ns, path.stat().st_size) if path.exists() else None

    deadline = time.monotonic() + args.timeout
    written: list[Path] = []
    while time.monotonic() < deadline:
        changed = [path for path in candidates if stamp(path) != before[path]]
        if not changed:
            time.sleep(0.1)
            continue
        settled = {path: stamp(path) for path in changed}
        time.sleep(0.1)
        if all(stamp(path) == settled[path] for path in changed):
            written = changed
            break

    if not written:
        print(f"asked for {args.format}: nothing was written to {requested}")
        return 1
    matched = True
    for path in written:
        found = sniff_mesh_format(path)
        # "stl-ascii" and "stl-binary" are both stl; the rest of the names have no variants.
        ok = found.split("-")[0] == args.format
        matched = matched and ok
        print(f"asked for {args.format}: wrote {found} ({path.stat().st_size} bytes)"
              f" to {path}  [{'ok' if ok else 'MISMATCH'}]")
    return 0 if matched else 1


def command_new_project(client: ApiClient, args: argparse.Namespace) -> None:
    print(client.new_project(args.name or ""))


def command_arrange(client: ApiClient, args: argparse.Namespace) -> None:
    print(client.arrange())


def command_orient(client: ApiClient, args: argparse.Namespace) -> None:
    print(client.orient())


def _print_mapping(prefix: str, value: object) -> None:
    if not isinstance(value, dict):
        print(f"{prefix}: {value}")
        return
    for key, item in value.items():
        _print_mapping(f"{prefix}.{key}" if prefix else str(key), item)


def command_flow_calibration(client: ApiClient, args: argparse.Namespace) -> None:
    if args.generate:
        _print_mapping(
            "flow-calibration",
            generate_flow_test(client, interval=args.generate, timeout=args.timeout),
        )
        return
    if args.apply:
        _print_mapping(
            "flow-calibration",
            apply_flow_result(client, tile=args.tile, timeout=args.timeout),
        )
        return
    _print_mapping("flow-calibration", open_flow_dialog(client, timeout=args.timeout))


def command_z_offset_generate(client: ApiClient, args: argparse.Namespace) -> None:
    _print_mapping(
        "z-offset-generate",
        generate_test(
            client,
            center=args.center,
            step=args.step,
            layer_height=args.layer_height,
            outer_walls=args.outer_walls,
            export_path=args.export,
            timeout=args.timeout,
        ),
    )


def command_z_offset_apply(client: ApiClient, args: argparse.Namespace) -> None:
    _print_mapping(
        "z-offset-apply",
        apply_result(
            client,
            pad=args.pad,
            measured_height=args.measured,
            timeout=args.timeout,
        ),
    )


def command_z_offset_workflow(client: ApiClient, args: argparse.Namespace) -> None:
    _print_mapping(
        "z-offset-workflow",
        run_workflow(
            client,
            export_path=args.export,
            center=args.center,
            step=args.step,
            layer_height=args.layer_height,
            outer_walls=args.outer_walls,
            moonraker=args.moonraker,
            start_print=args.start_print,
            timeout=args.timeout,
        ),
    )


def command_send_gcode(client: ApiClient, args: argparse.Namespace) -> None:
    del client
    _print_mapping(
        "send-gcode",
        send_gcode(
            args.path,
            args.moonraker,
            start=args.start_print,
            remote_name=args.remote_name,
        ),
    )


def command_inspect_gcode(client: ApiClient, args: argparse.Namespace) -> None:
    del client
    _print_mapping("inspect-gcode", inspect_gcode(args.path))


def command_z_offset_regression(client: ApiClient, args: argparse.Namespace) -> None:
    del client
    outcome = regression_check(
        args.moonraker,
        filament=args.filament,
        nozzle=args.nozzle,
        surface=args.surface,
        measured=args.measured,
        preset_offset=args.preset_offset,
    )
    _print_mapping("z-offset-regression", outcome)
    if not outcome["passed"]:
        raise AutomationError(
            f"END_PRINT overwrote {outcome['context']}: measured "
            f"{outcome['measured_bias']} is now {outcome['saved_after_print']}"
        )


def command_window(client: ApiClient, args: argparse.Namespace) -> None:
    _print_mapping(
        "window",
        client.window(x=args.x, y=args.y, width=args.width, height=args.height),
    )


def command_scroll(client: ApiClient, args: argparse.Namespace) -> None:
    _print_mapping(
        "scroll",
        client.scroll(dx=args.dx, dy=args.dy, lines=args.lines),
    )


def command_paint_supports(client: ApiClient, args: argparse.Namespace) -> None:
    painted = client.paint_supports_by_angle(args.threshold_deg, args.block)
    kind = "blockers" if args.block else "enforcers"
    for volume, count in enumerate(painted[kind]):
        print(f"volume {volume}: {count} {kind}")
    print(f"{sum(painted[kind])} {kind} at {painted['threshold_deg']:g} deg")


def command_arm_file_dialog(client: ApiClient, args: argparse.Namespace) -> None:
    if args.clear:
        print(client.clear_file_dialogs())
        return
    print(
        client.arm_file_dialog(
            answer=args.answer,
            paths=args.path or None,
            title_contains=args.title,
            filter_index=args.filter_index,
            checkbox=args.checkbox,
        )
    )


def command_file_dialog(client: ApiClient, args: argparse.Namespace) -> None:
    status = client.file_dialog_status()
    last = status.get("last")
    if last is None:
        print(f"armed={status['armed']} last=none")
        return
    kind = "save" if last["save"] else "open"
    print(
        f"armed={status['armed']} {kind}"
        f" {'answered' if last['intercepted'] else 'shown'}"
        f" accepted={last['accepted']}"
    )
    print(f"  title:    {last['title']}")
    print(f"  wildcard: {last['wildcard']}")
    print(f"  dir:      {last['directory']}")
    print(f"  filename: {last['filename']}")
    for path in last["paths"]:
        print(f"  path:     {path}")


def command_status(client: ApiClient, args: argparse.Namespace) -> None:
    for key, value in sorted(client.status().items()):
        print(f"{key}: {value}")


def command_wait(client: ApiClient, args: argparse.Namespace) -> None:
    if args.dialog is not None:
        print(describe(client.wait_for_dialog(args.dialog or None, args.timeout)))
    elif args.modal:
        snapshot = client.wait_for_modal(args.timeout)
        print(f"scope={snapshot['scope_automation_id']} modal=True")
    elif args.scope is not None:
        client.wait_for_scope(args.scope, args.timeout)
        print(f"scope={args.scope}")
    elif args.element is not None:
        print(describe(client.wait_for_element(args.element, args.timeout)))
    else:
        raise AutomationError("wait needs --modal, --scope, or --element")


def settle(client: ApiClient, args: argparse.Namespace) -> None:
    """A menu command that opens a dialog is dispatched asynchronously, so the
    caller decides what to wait for rather than guessing a sleep."""
    if getattr(args, "wait_dialog", None) is not None:
        print(describe(client.wait_for_dialog(args.wait_dialog or None, args.timeout)))
    elif getattr(args, "wait_modal", False):
        snapshot = client.wait_for_modal(args.timeout)
        print(f"scope={snapshot['scope_automation_id']} modal=True")
    elif getattr(args, "wait_element", None):
        print(describe(client.wait_for_element(args.wait_element, args.timeout)))


def parse_value(text: str):
    lowered = text.lower()
    if lowered in ("true", "false"):
        return lowered == "true"
    try:
        return int(text)
    except ValueError:
        pass
    try:
        return float(text)
    except ValueError:
        return text


def add_global_options(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument(
        "--attach",
        action="store_true",
        help="talk to a slicer that is already running (needs the token in the environment)",
    )
    parser.add_argument(
        "--executable",
        type=Path,
        default=None,
        help=f"binary to launch (default {DEFAULT_EXECUTABLE}); with --attach it is checked "
             "against the one already on the port instead of launching anything",
    )
    parser.add_argument(
        "--slicer-arg",
        action="append",
        metavar="ARG",
        help="extra argument for the launched slicer; repeat it, and write it as "
             "--slicer-arg=--load --slicer-arg=config.ini so argparse keeps the dashes",
    )
    parser.add_argument(
        "--keep-open",
        action="store_true",
        help="leave a launched slicer running after this command (implied by launch)",
    )
    parser.add_argument("--timeout", type=float, default=30.0)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    add_global_options(parser)

    commands = parser.add_subparsers(dest="command", required=True)

    # Launching only to shut down again is useless, so this one keeps the slicer up.
    launch_parser = commands.add_parser("launch", help="start the slicer, print its token, and leave it running")
    launch_parser.set_defaults(handler=command_status, keep_open=True)

    status_parser = commands.add_parser("status", help="print the API status block")
    status_parser.set_defaults(handler=command_status)

    snapshot_parser = commands.add_parser("snapshot", help="dump the UI snapshot")
    snapshot_parser.add_argument("--filter")
    snapshot_parser.add_argument("--hidden", action="store_true")
    snapshot_parser.set_defaults(handler=command_snapshot)

    menus_parser = commands.add_parser("menus", help="list menu items")
    menus_parser.add_argument("--filter")
    menus_parser.set_defaults(handler=command_menus)

    options_parser = commands.add_parser("options", help="list settings fields on the current page")
    options_parser.add_argument("--filter")
    options_parser.set_defaults(handler=command_options)

    tabs_parser = commands.add_parser("tabs", help="list notebook tab buttons")
    tabs_parser.set_defaults(handler=command_tabs)

    tab_parser = commands.add_parser("tab", help="switch to a notebook tab")
    tab_parser.add_argument("name")
    tab_parser.set_defaults(handler=command_tab)

    invoke_parser = commands.add_parser("invoke", help="invoke any element")
    invoke_parser.add_argument("automation_id")
    invoke_parser.add_argument(
        "--label",
        help="disambiguate by the element's visible text, e.g. --label '&Cancel'; "
             "needed whenever several elements share an automation_id",
    )
    add_wait_flags(invoke_parser)
    invoke_parser.set_defaults(handler=command_invoke)

    menu_parser = commands.add_parser("menu", help="invoke a menu item")
    menu_parser.add_argument("automation_id")
    add_wait_flags(menu_parser)
    menu_parser.set_defaults(handler=command_menu)

    set_parser = commands.add_parser("set", help="set a settings field")
    set_parser.add_argument("automation_id")
    set_parser.add_argument("value")
    set_parser.set_defaults(handler=command_set)

    toggle_parser = commands.add_parser("toggle", help="toggle a checkbox field")
    toggle_parser.add_argument("automation_id")
    toggle_parser.set_defaults(handler=command_toggle)

    add_capture_commands(commands)
    add_workflow_commands(commands)
    add_file_dialog_commands(commands)

    wait_parser = commands.add_parser("wait", help="wait for a UI state")
    wait_parser.add_argument(
        "--dialog",
        nargs="?",
        const="",
        help="wait for a dialog, modal or not; optionally matching this title",
    )
    wait_parser.add_argument("--modal", action="store_true")
    wait_parser.add_argument("--scope")
    wait_parser.add_argument("--element")
    wait_parser.set_defaults(handler=command_wait)

    return parser


def add_capture_commands(commands) -> None:
    screenshot_parser = commands.add_parser("screenshot", help="capture a PNG")
    screenshot_parser.add_argument("path", nargs="?", help="where to write the capture")
    screenshot_parser.add_argument("--ref", help="automation_id of the element to capture")
    screenshot_parser.add_argument(
        "--compare",
        metavar="BASELINE",
        help="diff the capture against this PNG and exit 1 if they differ",
    )
    screenshot_parser.add_argument(
        "--region",
        metavar="X,Y,WIDTH,HEIGHT",
        help="compare only this rectangle; use it to leave out the bottom right corner, "
             "where notification toasts fade on their own clock and differ run to run",
    )
    screenshot_parser.set_defaults(handler=command_screenshot)

    quit_parser = commands.add_parser(
        "quit", help="exit the slicer and wait for the port to go quiet"
    )
    quit_parser.set_defaults(handler=command_quit, keep_open=True)


def add_workflow_commands(commands) -> None:
    load_parser = commands.add_parser("load", help="load a model")
    load_parser.add_argument("model")
    load_parser.set_defaults(handler=command_load)

    slice_parser = commands.add_parser("slice", help="slice and wait")
    slice_parser.set_defaults(handler=command_slice)

    view_parser = commands.add_parser("view", help="select 3d or preview")
    view_parser.add_argument("view", choices=("3d", "preview"))
    view_parser.set_defaults(handler=command_view)

    preview_options_parser = commands.add_parser(
        "preview-options",
        help="show or set which move types the preview draws (travel, wipe, ...)",
    )
    preview_options_parser.add_argument(
        "option",
        nargs="*",
        metavar="NAME=VALUE",
        help="e.g. wipe=true; with no arguments it just reports every option. These are the "
             "legend's toggles, which ImGui draws with no clickable element behind them",
    )
    preview_options_parser.set_defaults(handler=command_preview_options)

    export_parser = commands.add_parser("export", help="export G-code and wait")
    export_parser.add_argument("path")
    export_parser.set_defaults(handler=command_export)

    export_plate_parser = commands.add_parser(
        "export-plate",
        help="export the plate as a mesh and report the format actually written",
    )
    export_plate_parser.add_argument("path")
    export_plate_parser.add_argument(
        "--format", choices=tuple(EXPORT_PLATE_FILTERS), required=True,
        help="which file dialog filter to pick; the format follows the filter, not the name",
    )
    add_wait_flags(export_plate_parser)
    export_plate_parser.set_defaults(handler=command_export_plate)

    new_project_parser = commands.add_parser("new-project", help="reset to an empty project")
    new_project_parser.add_argument("name", nargs="?", default="")
    new_project_parser.set_defaults(handler=command_new_project)

    arrange_parser = commands.add_parser("arrange", help="arrange objects on the bed")
    arrange_parser.set_defaults(handler=command_arrange)

    orient_parser = commands.add_parser(
        "orient",
        help="rotate objects to their best print orientation and wait for the job to finish",
    )
    orient_parser.set_defaults(handler=command_orient)

    paint_parser = commands.add_parser(
        "paint-supports",
        help="paint support enforcers on facets within an angle of straight down, and count them",
    )
    paint_parser.add_argument("threshold_deg", type=float)
    paint_parser.add_argument(
        "--block", action="store_true", help="paint blockers instead of enforcers"
    )
    paint_parser.set_defaults(handler=command_paint_supports)

    add_z_offset_commands(commands)
    add_flow_calibration_commands(commands)

    window_parser = commands.add_parser(
        "window", help="print or move the SuperSlicer window in screen pixels"
    )
    window_parser.add_argument("x", type=int, nargs="?")
    window_parser.add_argument("y", type=int, nargs="?")
    window_parser.add_argument("--width", type=int)
    window_parser.add_argument("--height", type=int)
    window_parser.set_defaults(handler=command_window)

    scroll_parser = commands.add_parser(
        "scroll", help="scroll the current settings page"
    )
    scroll_parser.add_argument("--dx", type=int, default=0, help="horizontal pixels")
    scroll_parser.add_argument("--dy", type=int, default=0, help="vertical pixels; positive shows content below")
    scroll_parser.add_argument("--lines", type=int, default=0, help="wxScrolledWindow units; overrides --dy")
    scroll_parser.set_defaults(handler=command_scroll)


def add_flow_calibration_commands(commands) -> None:
    flow_parser = commands.add_parser(
        "flow-calibration",
        help="open Calibration > Filament Flow calibration and optionally generate cubes",
    )
    flow_parser.add_argument(
        "--generate",
        choices=("recommended", "fine"),
        help="click Generate recommended or Generate fine after the dialog opens",
    )
    flow_parser.add_argument(
        "--apply",
        action="store_true",
        help="select a printed chip and write the new extrusion multiplier",
    )
    flow_parser.add_argument(
        "--tile",
        type=int,
        default=5,
        help="0-based chip index to apply (default 5, the 0.000 modifier on the recommended set)",
    )
    flow_parser.set_defaults(handler=command_flow_calibration)


def add_z_offset_commands(commands) -> None:
    """The Klipper Z-offset family: generate the pads, apply a measurement, and
    ship or check the resulting G-code."""
    generate_parser = commands.add_parser(
        "z-offset-generate",
        help="open Calibration > Klipper Z offset calibration and generate the nine pads",
    )
    generate_parser.add_argument("--center", type=float)
    generate_parser.add_argument("--step", type=float)
    generate_parser.add_argument("--layer-height", type=float)
    generate_parser.add_argument("--outer-walls", type=int)
    generate_parser.add_argument("--export", type=Path, help="export G-code after generate")
    generate_parser.set_defaults(handler=command_z_offset_generate)

    apply_parser = commands.add_parser(
        "z-offset-apply",
        help="open Apply Z offset and write the filament preset",
    )
    apply_parser.add_argument("--pad", type=int, required=True)
    apply_parser.add_argument("--measured", type=float, required=True)
    apply_parser.set_defaults(handler=command_z_offset_apply)

    workflow_parser = commands.add_parser(
        "z-offset-workflow",
        help="generate the nine-pad test, export G-code, and optionally upload to Moonraker",
    )
    workflow_parser.add_argument("--export", type=Path, required=True)
    workflow_parser.add_argument("--center", type=float)
    workflow_parser.add_argument("--step", type=float)
    workflow_parser.add_argument("--layer-height", type=float)
    workflow_parser.add_argument("--outer-walls", type=int)
    workflow_parser.add_argument("--moonraker", help="Moonraker base URL, e.g. http://192.168.122.147:7125")
    workflow_parser.add_argument(
        "--start-print", action="store_true", help="start the uploaded file on the printer"
    )
    workflow_parser.set_defaults(handler=command_z_offset_workflow)

    send_parser = commands.add_parser(
        "send-gcode", help="upload an already-exported G-code file to Moonraker"
    )
    send_parser.add_argument("path", type=Path)
    send_parser.add_argument("--moonraker", required=True)
    send_parser.add_argument("--start-print", action="store_true")
    send_parser.add_argument("--remote-name")
    send_parser.set_defaults(handler=command_send_gcode, skip_slicer=True)

    inspect_parser = commands.add_parser(
        "inspect-gcode",
        help="report START_PRINT / SET_GCODE_OFFSET order in an exported file",
    )
    inspect_parser.add_argument("path", type=Path)
    inspect_parser.set_defaults(handler=command_inspect_gcode, skip_slicer=True)

    regression_parser = commands.add_parser(
        "z-offset-regression",
        help="prove a measured per-context Z offset survives a print (needs the real macros)",
    )
    regression_parser.add_argument("--moonraker", required=True)
    regression_parser.add_argument("--filament", default="Regression Test Filament")
    regression_parser.add_argument("--nozzle", default="0.6")
    regression_parser.add_argument("--surface", default="Textured_PEI")
    regression_parser.add_argument(
        "--measured", type=float, default=-0.057, help="bias the user measured by hand"
    )
    regression_parser.add_argument(
        "--preset-offset", type=float, default=0.0, help="filament_z_offset the slicer emits"
    )
    regression_parser.set_defaults(handler=command_z_offset_regression, skip_slicer=True)


def add_file_dialog_commands(commands) -> None:
    arm_parser = commands.add_parser(
        "arm-file-dialog",
        help="queue the answer for the next file dialog; arm before the action that opens it",
    )
    arm_parser.add_argument("--answer", choices=("cancel", "ok"), default="cancel")
    arm_parser.add_argument(
        "--path",
        action="append",
        help="file the dialog should report; repeat it for a multi-select dialog",
    )
    arm_parser.add_argument("--title", help="only answer a dialog whose title contains this")
    arm_parser.add_argument("--filter-index", type=int, default=0)
    arm_parser.add_argument("--checkbox", action="store_true")
    arm_parser.add_argument(
        "--clear", action="store_true", help="drop every armed answer and the last record"
    )
    arm_parser.set_defaults(handler=command_arm_file_dialog)

    file_dialog_parser = commands.add_parser(
        "file-dialog", help="report the file dialog the app raised most recently"
    )
    file_dialog_parser.set_defaults(handler=command_file_dialog)


def add_wait_flags(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--wait-dialog",
        nargs="?",
        const="",
        help="wait for a dialog, modal or not; optionally matching this title. "
             "SuperSlicer's calibration dialogs are non-modal, so --wait-modal never sees them",
    )
    parser.add_argument(
        "--wait-modal",
        action="store_true",
        help="wait until a modal dialog becomes the active scope",
    )
    parser.add_argument(
        "--wait-element",
        help="wait until this automation_id appears in the snapshot",
    )


def main() -> int:
    args = build_parser().parse_args()
    if getattr(args, "skip_slicer", False):
        return args.handler(None, args) or 0
    slicer_process = None
    # An --executable given while attaching is a claim about what should already be on the
    # port, and wait_ready refuses to talk to anything else. Launching has no one to check
    # against yet, so there it is simply which binary to start.
    expect_executable = args.executable if args.attach else None
    if args.attach:
        client = ApiClient(args.port)
    else:
        # Honour a token already in the environment so a scripted session can
        # launch once and attach for every step afterwards.
        slicer_process, client = launch(
            args.executable or DEFAULT_EXECUTABLE,
            args.port,
            os.environ.get(TOKEN_VARIABLE),
            args.slicer_arg or (),
        )
        print(f"{TOKEN_VARIABLE}={client.token}", file=sys.stderr)
    try:
        wait_ready(
            client, slicer_process, timeout=args.timeout, expect_executable=expect_executable
        )
        return args.handler(client, args) or 0
    finally:
        if not args.keep_open:
            terminate(slicer_process)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AutomationError as error:
        print(f"gui_drive: {error}", file=sys.stderr)
        sys.exit(1)
