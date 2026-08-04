#!/usr/bin/env python3
"""Drive a running SuperSlicer through its embedded automation API.

Typical session - start the slicer once, then attach for each step:

    export SUPERSLICER_AUTOMATION_TOKEN=$(openssl rand -hex 16)
    ./tests/automation/gui_drive.py launch        # stays running
    ./tests/automation/gui_drive.py --attach menus
    ./tests/automation/gui_drive.py --attach tab print_settings
    ./tests/automation/gui_drive.py --attach options --filter perimeter
    ./tests/automation/gui_drive.py --attach set superslicer.option.perimeters 4
    ./tests/automation/gui_drive.py --attach menu superslicer.menu.calibration... --wait-modal

Every command prints plain lines rather than JSON so the output is readable in a
terminal and greppable in a script.
"""

from __future__ import annotations

import argparse
import os
import sys
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
    wait_ready,
)

MENU_PREFIX = "superslicer.menu."
OPTION_PREFIX = "superslicer.option."
TAB_PREFIX = "superslicer.tab."


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


def command_screenshot(client: ApiClient, args: argparse.Namespace) -> None:
    ref = None
    if args.ref is not None:
        ref = client.element(automation_id=args.ref)["ref"]
    png = client.screenshot(ref)
    Path(args.path).write_bytes(png)
    print(f"{args.path} ({len(png)} bytes)")


def command_load(client: ApiClient, args: argparse.Namespace) -> None:
    print(client.load_model(args.model))


def command_slice(client: ApiClient, args: argparse.Namespace) -> None:
    print(client.slice())


def command_view(client: ApiClient, args: argparse.Namespace) -> None:
    print(client.select_view(args.view))


def command_export(client: ApiClient, args: argparse.Namespace) -> None:
    print(client.export_gcode(args.path))


def command_new_project(client: ApiClient, args: argparse.Namespace) -> None:
    print(client.new_project(args.name or ""))


def command_arrange(client: ApiClient, args: argparse.Namespace) -> None:
    print(client.arrange())


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


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument(
        "--attach",
        action="store_true",
        help="talk to a slicer that is already running (needs the token in the environment)",
    )
    parser.add_argument("--executable", type=Path, default=DEFAULT_EXECUTABLE)
    parser.add_argument(
        "--keep-open",
        action="store_true",
        help="leave a launched slicer running after this command (implied by launch)",
    )
    parser.add_argument("--timeout", type=float, default=30.0)

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

    screenshot_parser = commands.add_parser("screenshot", help="capture a PNG")
    screenshot_parser.add_argument("path")
    screenshot_parser.add_argument("--ref", help="automation_id of the element to capture")
    screenshot_parser.set_defaults(handler=command_screenshot)

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


def add_workflow_commands(commands) -> None:
    load_parser = commands.add_parser("load", help="load a model")
    load_parser.add_argument("model")
    load_parser.set_defaults(handler=command_load)

    slice_parser = commands.add_parser("slice", help="slice and wait")
    slice_parser.set_defaults(handler=command_slice)

    view_parser = commands.add_parser("view", help="select 3d or preview")
    view_parser.add_argument("view", choices=("3d", "preview"))
    view_parser.set_defaults(handler=command_view)

    export_parser = commands.add_parser("export", help="export G-code and wait")
    export_parser.add_argument("path")
    export_parser.set_defaults(handler=command_export)

    new_project_parser = commands.add_parser("new-project", help="reset to an empty project")
    new_project_parser.add_argument("name", nargs="?", default="")
    new_project_parser.set_defaults(handler=command_new_project)

    arrange_parser = commands.add_parser("arrange", help="arrange objects on the bed")
    arrange_parser.set_defaults(handler=command_arrange)


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
    slicer_process = None
    if args.attach:
        client = ApiClient(args.port)
    else:
        # Honour a token already in the environment so a scripted session can
        # launch once and attach for every step afterwards.
        slicer_process, client = launch(args.executable, args.port, os.environ.get(TOKEN_VARIABLE))
        print(f"{TOKEN_VARIABLE}={client.token}", file=sys.stderr)
    try:
        wait_ready(client, slicer_process, timeout=args.timeout)
        args.handler(client, args)
        return 0
    finally:
        if not args.keep_open:
            terminate(slicer_process)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AutomationError as error:
        print(f"gui_drive: {error}", file=sys.stderr)
        sys.exit(1)
