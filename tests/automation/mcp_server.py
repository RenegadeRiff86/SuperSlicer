#!/usr/bin/env python3
"""MCP server for the SuperSlicer GUI automation API.

Cursor starts this process, but the API is off until automation debug mode is
turned on. While it is off, the only tool is automation_debug. Turning it on
publishes the rest and tells the client to refresh its tool list.

The slicer still has to be launched with --automation-api. This server never
prints the bearer token.
"""

from __future__ import annotations

import json
import os
import sys
from pathlib import Path
from typing import Any, Callable

sys.path.insert(0, str(Path(__file__).resolve().parent))

from automation_client import (  # noqa: E402
    DEFAULT_EXECUTABLE,
    DEFAULT_PORT,
    TOKEN_VARIABLE,
    ApiClient,
    AutomationError,
    launch,
    wait_ready,
)
from automation_mode import debug_enabled, set_debug_enabled, status as mode_status  # noqa: E402
from desktop_windows import DesktopWindowError, place as place_desktop  # noqa: E402
from flow_calibration_workflow import apply_result as apply_flow_result  # noqa: E402
from flow_calibration_workflow import generate_test as generate_flow_test  # noqa: E402
from flow_calibration_workflow import open_dialog as open_flow_dialog  # noqa: E402
from gui_drive import (  # noqa: E402
    MENU_PREFIX,
    OPTION_PREFIX,
    TAB_PREFIX,
    parse_value,
)
from z_offset_workflow import apply_result as apply_z_offset  # noqa: E402
from z_offset_workflow import generate_test as generate_z_offset  # noqa: E402
from z_offset_workflow import inspect_gcode  # noqa: E402
from z_offset_workflow import run_workflow as run_z_offset  # noqa: E402
from z_offset_workflow import send_gcode  # noqa: E402

SESSION_ENV = Path("/tmp/superslicer-automation-session.env")
PROTOCOL = "2024-11-05"
SUPPORTED_PROTOCOLS = (
    "2025-11-25",
    "2025-06-18",
    "2025-03-26",
    "2024-11-05",
    "2024-10-07",
)


def _read_token_file() -> str | None:
    try:
        lines = SESSION_ENV.read_text(encoding="utf-8").splitlines()
    except OSError:
        return None
    prefix = TOKEN_VARIABLE + "="
    for line in lines:
        if line.startswith(prefix):
            token = line[len(prefix):].strip().strip('"').strip("'")
            return token or None
    return None


def _token() -> str | None:
    env = os.environ.get(TOKEN_VARIABLE)
    if env:
        return env
    return _read_token_file()


def _client() -> ApiClient:
    if not debug_enabled():
        raise AutomationError(
            "Automation debug mode is off. Call automation_debug with enabled=true, "
            "or run: python3 tests/automation/gui_drive.py automation-debug on"
        )
    token = _token()
    if not token:
        raise AutomationError(
            "No automation token. Start SuperSlicer with the "
            "'Debug automation' launch configuration, or call the launch tool."
        )
    return ApiClient(DEFAULT_PORT, token)


def _require_debug() -> None:
    if not debug_enabled():
        raise AutomationError("Automation debug mode is off.")


def _filter_elements(elements: list[dict], needle: str | None) -> list[dict]:
    if not needle:
        return elements
    folded = needle.lower()
    return [
        element
        for element in elements
        if folded in element.get("automation_id", "").lower()
        or folded in element.get("name", "").lower()
        or folded in element.get("role", "").lower()
    ]


def _summarize(elements: list[dict]) -> list[dict]:
    return [
        {
            "automation_id": element.get("automation_id"),
            "name": element.get("name"),
            "role": element.get("role"),
            "value": element.get("value"),
        }
        for element in elements
    ]


def _with_prefix(value: str, prefix: str) -> str:
    if value.startswith(prefix) or "." in value:
        return value
    return prefix + value


def _settle(client: ApiClient, arguments: dict[str, Any]) -> dict[str, Any] | None:
    timeout = float(arguments.get("timeout") or 30)
    if arguments.get("wait_dialog") is not None:
        title = arguments.get("wait_dialog") or None
        return client.wait_for_dialog(title, timeout)
    if arguments.get("wait_modal"):
        return client.wait_for_modal(timeout)
    if arguments.get("wait_scope"):
        return client.wait_for_scope(arguments["wait_scope"], timeout)
    if arguments.get("wait_element"):
        return client.wait_for_element(arguments["wait_element"], timeout)
    return None


def tool_automation_debug(arguments: dict[str, Any]) -> dict[str, Any]:
    if "enabled" in arguments:
        set_debug_enabled(bool(arguments["enabled"]))
        _notify_tools_changed()
    reported = mode_status()
    reported["listening"] = False
    if reported["enabled"]:
        try:
            reported["slicer"] = _client().status()
            reported["listening"] = True
        except (AutomationError, ConnectionError, OSError) as error:
            reported["slicer_error"] = str(error)
    return reported


def tool_status(_arguments: dict[str, Any]) -> dict[str, Any]:
    return _client().status()


def tool_snapshot(arguments: dict[str, Any]) -> dict[str, Any]:
    snapshot = _client().snapshot(include_hidden=bool(arguments.get("hidden")))
    elements = _filter_elements(snapshot.get("elements", []), arguments.get("filter"))
    return {
        "scope_automation_id": snapshot.get("scope_automation_id"),
        "modal": snapshot.get("modal"),
        "count": len(elements),
        "elements": _summarize(elements),
    }


def tool_menus(arguments: dict[str, Any]) -> dict[str, Any]:
    elements = _filter_elements(_client().elements(prefix=MENU_PREFIX), arguments.get("filter"))
    return {"count": len(elements), "elements": _summarize(elements)}


def tool_options(arguments: dict[str, Any]) -> dict[str, Any]:
    elements = _filter_elements(_client().elements(prefix=OPTION_PREFIX), arguments.get("filter"))
    return {"count": len(elements), "elements": _summarize(elements)}


def tool_tabs(_arguments: dict[str, Any]) -> dict[str, Any]:
    elements = _client().elements(prefix=TAB_PREFIX)
    return {"count": len(elements), "elements": _summarize(elements)}


def tool_tab(arguments: dict[str, Any]) -> Any:
    return _client().invoke(_with_prefix(arguments["name"], TAB_PREFIX))


def tool_invoke(arguments: dict[str, Any]) -> dict[str, Any]:
    client = _client()
    kwargs = {"name": arguments["label"]} if arguments.get("label") else {}
    result = client.invoke(arguments["automation_id"], **kwargs)
    waited = _settle(client, arguments)
    return {"result": result, "wait": waited}


def _pointer(client: ApiClient, element: dict[str, Any], kind: str, x: float, y: float) -> dict[str, Any]:
    if kind not in ("move", "click"):
        raise AutomationError("pointer type must be move or click")
    if not 0.0 <= x <= 1.0 or not 0.0 <= y <= 1.0:
        raise AutomationError(
            "pointer coordinates must stay inside the SuperSlicer window, from 0 through 1"
        )
    return client.rest(
        "POST",
        "/api/v1/input",
        {
            "backend": "wayland",
            "type": kind,
            "ref": element["ref"],
            "x": x,
            "y": y,
            "consent_timeout_ms": 3000,
        },
    )


def _bounds(element: dict[str, Any]) -> dict[str, Any]:
    bounds = element.get("bounds") or {}
    return {
        "x": bounds.get("x"),
        "y": bounds.get("y"),
        "width": bounds.get("width"),
        "height": bounds.get("height"),
    }


def tool_pointer(arguments: dict[str, Any]) -> dict[str, Any]:
    target = arguments.get("automation_id") or "superslicer.main_window"
    client = _client()
    return _pointer(
        client,
        client.element(automation_id=target),
        arguments["type"],
        float(arguments["x"]),
        float(arguments["y"]),
    )


def tool_hover(arguments: dict[str, Any]) -> dict[str, Any]:
    automation_id = arguments["automation_id"]
    x = float(arguments.get("x", 0.5))
    y = float(arguments.get("y", 0.5))
    client = _client()
    # Park off the control first. A pointer that is already inside it does not
    # enter, so the hover handlers never run.
    window = client.element(automation_id="superslicer.main_window")
    _pointer(client, window, "move", 0.01, 0.01)
    before = client.element(automation_id=automation_id)
    moved = _pointer(client, before, "move", x, y)
    after = client.element(automation_id=automation_id)
    return {
        "automation_id": automation_id,
        "point": moved.get("point"),
        "before": _bounds(before),
        "after": _bounds(after),
    }


def tool_menu(arguments: dict[str, Any]) -> dict[str, Any]:
    client = _client()
    result = client.invoke(_with_prefix(arguments["automation_id"], MENU_PREFIX))
    return {"result": result, "wait": _settle(client, arguments)}


def tool_set_option(arguments: dict[str, Any]) -> Any:
    automation_id = _with_prefix(arguments["automation_id"], OPTION_PREFIX)
    return _client().set_value(automation_id, parse_value(str(arguments["value"])))


def tool_toggle(arguments: dict[str, Any]) -> Any:
    automation_id = _with_prefix(arguments["automation_id"], OPTION_PREFIX)
    value = arguments.get("value")
    if value is None:
        return _client().toggle(automation_id)
    return _client().toggle(automation_id, bool(value))


def tool_load(arguments: dict[str, Any]) -> Any:
    return _client().load_model(arguments["model"])


def tool_slice(arguments: dict[str, Any]) -> Any:
    return _client().slice(int(arguments.get("timeout_ms") or 300000))


def tool_view(arguments: dict[str, Any]) -> Any:
    return _client().select_view(arguments["view"])


def tool_preview_options(arguments: dict[str, Any]) -> Any:
    options = arguments.get("options") or {}
    return _client().set_preview(options=options) if options else _client().preview_options()


def tool_export(arguments: dict[str, Any]) -> Any:
    return _client().export_gcode(arguments["path"], timeout_ms=int(arguments.get("timeout_ms") or 300000))


def tool_new_project(arguments: dict[str, Any]) -> Any:
    return _client().new_project(arguments.get("name") or "")


def tool_arrange(_arguments: dict[str, Any]) -> Any:
    return _client().arrange()


def tool_orient(arguments: dict[str, Any]) -> Any:
    return _client().orient(timeout=float(arguments.get("timeout") or 300))


def tool_paint_supports(arguments: dict[str, Any]) -> Any:
    return _client().paint_supports_by_angle(
        float(arguments["threshold_deg"]), bool(arguments.get("block"))
    )


def tool_window(arguments: dict[str, Any]) -> Any:
    return _client().window(
        x=arguments.get("x"),
        y=arguments.get("y"),
        width=arguments.get("width"),
        height=arguments.get("height"),
    )


def tool_scroll(arguments: dict[str, Any]) -> Any:
    return _client().scroll(
        dx=int(arguments.get("dx") or 0),
        dy=int(arguments.get("dy") or 0),
        lines=int(arguments.get("lines") or 0),
    )


def tool_desktop(arguments: dict[str, Any]) -> dict[str, Any]:
    _require_debug()
    try:
        placed = place_desktop(float(arguments.get("cursor_fraction") or 0.56))
    except DesktopWindowError as error:
        raise AutomationError(str(error)) from error
    return {"placed": placed}


def tool_screenshot(arguments: dict[str, Any]) -> dict[str, Any]:
    client = _client()
    ref = None
    if arguments.get("ref"):
        ref = client.element(automation_id=arguments["ref"])["ref"]
    png = client.screenshot(ref)
    path = Path(arguments["path"]).expanduser()
    path.write_bytes(png)
    return {"path": str(path), "bytes": len(png)}


def tool_wait(arguments: dict[str, Any]) -> Any:
    client = _client()
    timeout = float(arguments.get("timeout") or 30)
    if arguments.get("dialog") is not None:
        title = arguments.get("dialog") or None
        return client.wait_for_dialog(title, timeout)
    if arguments.get("modal"):
        return client.wait_for_modal(timeout)
    if arguments.get("scope"):
        return client.wait_for_scope(arguments["scope"], timeout)
    if arguments.get("element"):
        return client.wait_for_element(arguments["element"], timeout)
    raise AutomationError("wait needs dialog, modal, scope, or element")


def tool_quit(arguments: dict[str, Any]) -> Any:
    return _client().quit(force=arguments.get("force", True))


def tool_launch(arguments: dict[str, Any]) -> dict[str, Any]:
    _require_debug()
    executable = Path(arguments.get("executable") or DEFAULT_EXECUTABLE)
    port = int(arguments.get("port") or DEFAULT_PORT)
    token = _token()
    if token is None:
        token = os.urandom(32).hex()
        SESSION_ENV.write_text(f"{TOKEN_VARIABLE}={token}\n", encoding="utf-8")
        os.chmod(SESSION_ENV, 0o600)
    process, client = launch(executable, port, token)
    reported = wait_ready(client, process, timeout=float(arguments.get("timeout") or 60))
    return {
        "pid": reported.get("pid"),
        "gui_ready": reported.get("gui_ready"),
        "executable": reported.get("executable"),
        "port": port,
    }


def tool_arm_file_dialog(arguments: dict[str, Any]) -> Any:
    client = _client()
    if arguments.get("clear"):
        return client.clear_file_dialogs()
    return client.arm_file_dialog(
        answer=arguments.get("answer") or "cancel",
        paths=arguments.get("paths"),
        title_contains=arguments.get("title"),
        filter_index=int(arguments.get("filter_index") or 0),
        checkbox=bool(arguments.get("checkbox")),
    )


def tool_file_dialog_status(_arguments: dict[str, Any]) -> Any:
    return _client().file_dialog_status()


def tool_flow_calibration(arguments: dict[str, Any]) -> Any:
    client = _client()
    timeout = float(arguments.get("timeout") or 60)
    if arguments.get("tile") is not None:
        return apply_flow_result(client, tile=int(arguments["tile"]), timeout=timeout)
    if arguments.get("generate"):
        return generate_flow_test(client, interval=arguments["generate"], timeout=timeout)
    return open_flow_dialog(client, timeout=timeout)


def tool_z_offset_generate(arguments: dict[str, Any]) -> Any:
    return generate_z_offset(
        _client(),
        center=arguments.get("center"),
        step=arguments.get("step"),
        layer_height=arguments.get("layer_height"),
        outer_walls=arguments.get("outer_walls"),
        export_path=arguments.get("export"),
        timeout=float(arguments.get("timeout") or 60),
    )


def tool_z_offset_apply(arguments: dict[str, Any]) -> Any:
    return apply_z_offset(
        _client(),
        pad=int(arguments["pad"]),
        measured_height=float(arguments["measured"]),
        timeout=float(arguments.get("timeout") or 30),
    )


def tool_z_offset_workflow(arguments: dict[str, Any]) -> Any:
    return run_z_offset(
        _client(),
        export_path=arguments["export"],
        center=arguments.get("center"),
        step=arguments.get("step"),
        layer_height=arguments.get("layer_height"),
        outer_walls=arguments.get("outer_walls"),
        moonraker=arguments.get("moonraker"),
        start_print=bool(arguments.get("start_print")),
        timeout=float(arguments.get("timeout") or 60),
    )


def tool_inspect_gcode(arguments: dict[str, Any]) -> Any:
    _require_debug()
    return inspect_gcode(arguments["path"])


def tool_send_gcode(arguments: dict[str, Any]) -> Any:
    _require_debug()
    return send_gcode(
        arguments["path"],
        arguments["moonraker"],
        start=bool(arguments.get("start_print")),
        remote_name=arguments.get("remote_name"),
    )


def _schema(properties: dict[str, Any], required: list[str] | None = None) -> dict[str, Any]:
    schema: dict[str, Any] = {
        "type": "object",
        "properties": properties,
        "additionalProperties": False,
    }
    if required:
        schema["required"] = required
    return schema


def _string(description: str) -> dict[str, str]:
    return {"type": "string", "description": description}


def _number(description: str) -> dict[str, str]:
    return {"type": "number", "description": description}


def _integer(description: str) -> dict[str, str]:
    return {"type": "integer", "description": description}


def _boolean(description: str) -> dict[str, str]:
    return {"type": "boolean", "description": description}


WAIT = {
    "wait_dialog": _string("After the action, wait for a dialog whose title contains this. Empty waits for any dialog."),
    "wait_modal": _boolean("After the action, wait until a modal dialog is the active scope."),
    "wait_scope": _string("After the action, wait until this scope automation id is active."),
    "wait_element": _string("After the action, wait until this automation id appears."),
    "timeout": _number("Seconds to wait. Default 30."),
}


TOOLS: list[tuple[str, str, dict[str, Any], Callable[[dict[str, Any]], Any]]] = [
    (
        "automation_debug",
        "Turn the SuperSlicer automation API on or off. It is off by default, like a debug mode. "
        "Other automation tools are published only while it is on. Omit enabled to report the current mode.",
        _schema({"enabled": _boolean("True turns the API on. False turns it off.")}),
        tool_automation_debug,
    ),
    ("status", "Report whether the slicer automation API is up, including pid and gui_ready.", _schema({}), tool_status),
    (
        "snapshot",
        "List the UI elements in the current scope.",
        _schema({"filter": _string("Substring matched against id, name, and role."), "hidden": _boolean("Include hidden elements.")}),
        tool_snapshot,
    ),
    ("menus", "List menu items.", _schema({"filter": _string("Substring of the menu automation id.")}), tool_menus),
    ("options", "List settings fields on the current page.", _schema({"filter": _string("Substring of the option automation id.")}), tool_options),
    ("tabs", "List notebook tabs.", _schema({}), tool_tabs),
    ("tab", "Switch to a notebook tab. A short name such as print_settings is prefixed.", _schema({"name": _string("Tab name or automation id.")}, ["name"]), tool_tab),
    (
        "invoke",
        "Invoke a button or other element by automation id. This posts the command event and does not move the pointer. Pass label when several elements share that id.",
        _schema({"automation_id": _string("Element automation id."), "label": _string("Visible text, used when the id is shared."), **WAIT}, ["automation_id"]),
        tool_invoke,
    ),
    (
        "pointer",
        "Move or click the real pointer inside a SuperSlicer window. x and y are fractions of that window, from 0 through 1. "
        "A click follows the GTK mouse path and needs SuperSlicer in front. Use hover to enter a control.",
        _schema({
            "type": {"type": "string", "enum": ["move", "click"], "description": "move repositions the pointer. click presses the left button there."},
            "x": _number("Fraction of the target width, from 0 through 1."),
            "y": _number("Fraction of the target height, from 0 through 1."),
            "automation_id": _string("Window or control that defines the range. Defaults to the main window."),
        }, ["type", "x", "y"]),
        tool_pointer,
    ),
    (
        "hover",
        "Move the real pointer onto a control so its hover handlers run. Invoke and click do not enter the control. "
        "Reports the control's screen bounds before and after, which is how a hover-only size change shows up.",
        _schema({
            "automation_id": _string("Control to enter, such as superslicer.mode.simple."),
            "x": _number("Fraction of the control width, from 0 through 1. Default 0.5."),
            "y": _number("Fraction of the control height, from 0 through 1. Default 0.5."),
        }, ["automation_id"]),
        tool_hover,
    ),
    (
        "menu",
        "Invoke a menu item. A short id is prefixed with superslicer.menu.",
        _schema({"automation_id": _string("Menu automation id."), **WAIT}, ["automation_id"]),
        tool_menu,
    ),
    (
        "set_option",
        "Set a settings field. A short name is prefixed with superslicer.option.",
        _schema({"automation_id": _string("Option automation id."), "value": _string("New value. true/false and numbers are parsed.")}, ["automation_id", "value"]),
        tool_set_option,
    ),
    (
        "toggle",
        "Toggle a checkbox, or set it when value is given.",
        _schema({"automation_id": _string("Option automation id."), "value": _boolean("Force the checkbox on or off.")}, ["automation_id"]),
        tool_toggle,
    ),
    ("load_model", "Load a model onto the platter.", _schema({"model": _string("Path to STL, 3MF, or another model.")}, ["model"]), tool_load),
    ("slice", "Slice the current platter and wait until it finishes.", _schema({"timeout_ms": _integer("Wait timeout in milliseconds. Default 300000.")}), tool_slice),
    ("view", "Select the 3D editor or the preview.", _schema({"view": {"type": "string", "enum": ["3d", "preview"]}}, ["view"]), tool_view),
    (
        "preview_options",
        "Show or set which preview move types are drawn. These toggles are ImGui and have no element to click.",
        _schema({"options": {"type": "object", "description": "Move type name to boolean, for example {\"wipe\": true}."}}),
        tool_preview_options,
    ),
    ("export_gcode", "Export G-code to a path and wait.", _schema({"path": _string("Output path."), "timeout_ms": _integer("Wait timeout in milliseconds.")}, ["path"]), tool_export),
    ("new_project", "Reset the platter to an empty project.", _schema({"name": _string("Optional project name.")}), tool_new_project),
    ("arrange", "Arrange objects on the bed.", _schema({}), tool_arrange),
    ("orient", "Rotate objects to their optimal orientation and wait for the job.", _schema({"timeout": _number("Seconds. Default 300.")}), tool_orient),
    (
        "paint_supports",
        "Paint support enforcers, or blockers, on facets within an angle of straight down.",
        _schema({"threshold_deg": _number("Angle from straight down, in degrees."), "block": _boolean("Paint blockers instead of enforcers.")}, ["threshold_deg"]),
        tool_paint_supports,
    ),
    (
        "window",
        "Read or move the SuperSlicer window, in screen pixels. Omit every field to read it.",
        _schema({"x": _integer("Left edge."), "y": _integer("Top edge."), "width": _integer("Width."), "height": _integer("Height.")}),
        tool_window,
    ),
    (
        "scroll",
        "Scroll the current settings page. Positive dy shows content below.",
        _schema({"dx": _integer("Horizontal pixels."), "dy": _integer("Vertical pixels."), "lines": _integer("wxScrolledWindow units. Overrides dy.")}),
        tool_scroll,
    ),
    (
        "desktop",
        "Tile Cursor on the left and SuperSlicer on the right, and leave Cursor in front.",
        _schema({"cursor_fraction": _number("Share of the work area for Cursor. Default 0.56.")}),
        tool_desktop,
    ),
    (
        "screenshot",
        "Capture a PNG to a file path. Does not return the image bytes.",
        _schema({"path": _string("Where to write the PNG."), "ref": _string("Automation id of one element. Omit for the whole window.")}, ["path"]),
        tool_screenshot,
    ),
    (
        "wait",
        "Wait for a dialog, a modal, a scope, or an element.",
        _schema({
            "dialog": _string("Dialog title substring. Empty waits for any dialog."),
            "modal": _boolean("Wait for a modal scope."),
            "scope": _string("Scope automation id."),
            "element": _string("Element automation id."),
            "timeout": _number("Seconds. Default 30."),
        }),
        tool_wait,
    ),
    ("quit", "Close SuperSlicer. Forced by default so unsaved-change prompts cannot veto it.", _schema({"force": _boolean("Skip prompts. Default true.")}), tool_quit),
    (
        "launch",
        "Start SuperSlicer with the automation API and leave it running. Does not return the token.",
        _schema({
            "executable": _string("Binary to start. Defaults to the release build."),
            "port": _integer("Loopback port. Default 43127."),
            "timeout": _number("Seconds to wait until the GUI is ready."),
        }),
        tool_launch,
    ),
    (
        "arm_file_dialog",
        "Queue the answer for the next file dialog. Arm it before the action that opens the dialog.",
        _schema({
            "clear": _boolean("Drop a previously armed answer."),
            "answer": _string("ok or cancel. Default cancel."),
            "paths": {"type": "array", "items": {"type": "string"}, "description": "Paths to return."},
            "title": _string("Only answer a dialog whose title contains this."),
            "filter_index": _integer("File-type combo index."),
            "checkbox": _boolean("Checkbox state to return."),
        }),
        tool_arm_file_dialog,
    ),
    ("file_dialog_status", "Report the file dialog SuperSlicer raised most recently.", _schema({}), tool_file_dialog_status),
    (
        "flow_calibration",
        "Open filament flow calibration. Pass generate as recommended or fine, or tile to apply a measured chip.",
        _schema({
            "generate": _string("recommended or fine."),
            "tile": _integer("Chip index to apply."),
            "timeout": _number("Seconds."),
        }),
        tool_flow_calibration,
    ),
    (
        "z_offset_generate",
        "Generate the nine-pad Klipper Z offset test and optionally export it.",
        _schema({
            "center": _number("Center offset."),
            "step": _number("Pad step."),
            "layer_height": _number("Layer height."),
            "outer_walls": _integer("Outer wall count."),
            "export": _string("G-code path. Omit to generate without exporting."),
            "timeout": _number("Seconds."),
        }),
        tool_z_offset_generate,
    ),
    (
        "z_offset_apply",
        "Apply a measured pad height to the active filament Z offset.",
        _schema({"pad": _integer("Pad number."), "measured": _number("Measured height."), "timeout": _number("Seconds.")}, ["pad", "measured"]),
        tool_z_offset_apply,
    ),
    (
        "z_offset_workflow",
        "Generate the nine-pad test, export G-code, and optionally upload it to Moonraker.",
        _schema({
            "export": _string("G-code path."),
            "center": _number("Center offset."),
            "step": _number("Pad step."),
            "layer_height": _number("Layer height."),
            "outer_walls": _integer("Outer wall count."),
            "moonraker": _string("Moonraker base URL."),
            "start_print": _boolean("Start the uploaded file."),
            "timeout": _number("Seconds."),
        }, ["export"]),
        tool_z_offset_workflow,
    ),
    ("inspect_gcode", "Check that a G-code file has START_PRINT, the filament Z offset, and the nine pad offsets.", _schema({"path": _string("G-code path.")}, ["path"]), tool_inspect_gcode),
    (
        "send_gcode",
        "Upload a G-code file to Moonraker.",
        _schema({
            "path": _string("G-code path."),
            "moonraker": _string("Moonraker base URL."),
            "start_print": _boolean("Start the upload."),
            "remote_name": _string("Filename to store on the printer."),
        }, ["path", "moonraker"]),
        tool_send_gcode,
    ),
]


def _visible_tools() -> list[tuple[str, str, dict[str, Any], Callable[[dict[str, Any]], Any]]]:
    if debug_enabled():
        return TOOLS
    return [tool for tool in TOOLS if tool[0] == "automation_debug"]


def _tools_list() -> dict[str, Any]:
    return {
        "tools": [
            {"name": name, "description": description, "inputSchema": schema}
            for name, description, schema, _handler in _visible_tools()
        ]
    }


def _call(name: str, arguments: dict[str, Any] | None) -> dict[str, Any]:
    if name != "automation_debug" and not debug_enabled():
        raise AutomationError("Automation debug mode is off.")
    handler = next((item[3] for item in TOOLS if item[0] == name), None)
    if handler is None:
        raise AutomationError(f"unknown tool {name}")
    result = handler(arguments or {})
    if not isinstance(result, dict):
        result = {"result": result}
    return {
        "content": [{"type": "text", "text": json.dumps(result, indent=2, default=str)}],
        "structuredContent": result,
        "isError": False,
    }


def _error(message: str) -> dict[str, Any]:
    return {
        "content": [{"type": "text", "text": message}],
        "structuredContent": {"ok": False, "error": message},
        "isError": True,
    }


_stdout = sys.stdout.buffer
# Cursor's stdio client writes one JSON object per line. Content-Length is the
# older framing; a client that sends it still gets that framing back.
_framing = "ndjson"


def _write(message: dict[str, Any]) -> None:
    payload = json.dumps(message, separators=(",", ":")).encode("utf-8")
    if _framing == "content-length":
        header = f"Content-Length: {len(payload)}\r\n\r\n".encode("ascii")
        _stdout.write(header)
        _stdout.write(payload)
    else:
        _stdout.write(payload)
        _stdout.write(b"\n")
    _stdout.flush()


def _notify_tools_changed() -> None:
    _write({"jsonrpc": "2.0", "method": "notifications/tools/list_changed"})


def _read_headers(first_line: bytes) -> dict[str, str]:
    headers: dict[str, str] = {}
    line: bytes | None = first_line
    while line:
        if line in (b"\r\n", b"\n"):
            break
        decoded = line.decode("ascii", "replace").strip()
        if ":" in decoded:
            name, value = decoded.split(":", 1)
            headers[name.strip().lower()] = value.strip()
        line = sys.stdin.buffer.readline()
    return headers


def _read_message() -> dict[str, Any] | None:
    global _framing
    while True:
        line = sys.stdin.buffer.readline()
        if not line:
            return None
        stripped = line.strip()
        if not stripped:
            continue
        if stripped[:1] in (b"{", b"["):
            _framing = "ndjson"
            return json.loads(stripped.decode("utf-8"))
        _framing = "content-length"
        headers = _read_headers(line)
        length = int(headers.get("content-length", "0"))
        if length <= 0:
            return None
        body = sys.stdin.buffer.read(length)
        if len(body) < length:
            return None
        return json.loads(body.decode("utf-8"))


def _handle(message: dict[str, Any]) -> None:
    method = message.get("method")
    if "id" not in message:
        return
    request_id = message["id"]
    try:
        if method == "initialize":
            requested = (message.get("params") or {}).get("protocolVersion")
            protocol = requested if requested in SUPPORTED_PROTOCOLS else PROTOCOL
            result = {
                "protocolVersion": protocol,
                "capabilities": {"tools": {"listChanged": True}},
                "serverInfo": {"name": "superslicer-automation", "version": "1.0.0"},
                "instructions": (
                    "The SuperSlicer automation API is off until automation_debug is called "
                    "with enabled=true. Launch the slicer with --automation-api before the other tools."
                ),
            }
        elif method == "tools/list":
            result = _tools_list()
        elif method == "tools/call":
            params = message.get("params") or {}
            try:
                result = _call(params.get("name") or "", params.get("arguments") or {})
            except (AutomationError, KeyError, TypeError, ValueError) as error:
                result = _error(str(error))
        elif method == "ping":
            result = {}
        else:
            _write({
                "jsonrpc": "2.0",
                "id": request_id,
                "error": {"code": -32601, "message": f"unknown method {method}"},
            })
            return
    except Exception as error:  # noqa: BLE001 - the client must get the failure, not a dead pipe
        _write({
            "jsonrpc": "2.0",
            "id": request_id,
            "error": {"code": -32603, "message": str(error)},
        })
        return
    _write({"jsonrpc": "2.0", "id": request_id, "result": result})


def main() -> int:
    while True:
        message = _read_message()
        if message is None:
            return 0
        _handle(message)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except BrokenPipeError:
        sys.exit(0)
