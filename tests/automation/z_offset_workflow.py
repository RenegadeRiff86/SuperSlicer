#!/usr/bin/env python3
"""Reusable GUI workflow: Klipper Z-offset calibration through to a print host.

This is the command future sessions should run. It talks to a SuperSlicer that
already has the automation API up (gui_drive.py launch / --attach). It does not
replace the printer-side START_PRINT hardware sequence; it owns the filament Z
offset SuperSlicer emits after that macro.
"""

from __future__ import annotations

import re
from pathlib import Path
from typing import Any

from automation_client import ApiClient, AutomationError
from moonraker import MoonrakerHost

MENU_GENERATE = "superslicer.menu.calibration.klipper_z_offset"
MENU_APPLY = "superslicer.menu.calibration.apply_z_offset"
ID_CENTER = "superslicer.calibration.z_offset.center"
ID_STEP = "superslicer.calibration.z_offset.step"
ID_LAYER = "superslicer.calibration.z_offset.layer_height"
ID_WALLS = "superslicer.calibration.z_offset.outer_walls"
ID_GENERATE = "superslicer.calibration.z_offset.generate"
ID_PAD = "superslicer.calibration.z_offset.measured_pad"
ID_HEIGHT = "superslicer.calibration.z_offset.measured_height"
ID_APPLY = "superslicer.calibration.z_offset.apply"

PAD_OFFSET = re.compile(
    r"; SuperSlicer-generated Z offset calibration pad \d+\n"
    r"; Calibration target layer height: [^\n]+\n"
    r"SET_GCODE_OFFSET Z=(?P<offset>-?\d+(?:\.\d+)?) MOVE=0"
)


def _invoke_id_or_label(client: ApiClient, automation_id: str, label: str) -> None:
    if client.elements(automation_id=automation_id):
        client.invoke(automation_id)
        return
    client.invoke("button", name=label)


def _dismiss_message(client: ApiClient) -> None:
    snapshot = client.snapshot()
    if not snapshot.get("modal"):
        return
    for label in ("OK", "&OK", "Ok"):
        try:
            client.invoke("button", name=label)
            return
        except AutomationError:
            continue


def generate_test(
    client: ApiClient,
    *,
    center: float | None = None,
    step: float | None = None,
    layer_height: float | None = None,
    outer_walls: int | None = None,
    export_path: Path | str | None = None,
    timeout: float = 60.0,
) -> dict[str, Any]:
    """Open Calibration > Klipper Z offset calibration, generate the nine pads,
    and optionally export G-code. The generator also starts a reslice."""
    client.new_project("Klipper Z offset calibration")
    client.invoke(MENU_GENERATE)
    dialog = client.wait_for_dialog("Z offset calibration", timeout)
    if center is not None:
        client.set_value(ID_CENTER, center)
    if step is not None:
        client.set_value(ID_STEP, step)
    if layer_height is not None:
        client.set_value(ID_LAYER, layer_height)
    if outer_walls is not None:
        client.set_value(ID_WALLS, outer_walls)
    values = {
        "center": client.element(automation_id=ID_CENTER)["value"],
        "step": client.element(automation_id=ID_STEP)["value"],
        "layer_height": client.element(automation_id=ID_LAYER)["value"],
        "outer_walls": client.element(automation_id=ID_WALLS)["value"],
        "dialog": dialog["name"],
    }
    _invoke_id_or_label(client, ID_GENERATE, "Generate")
    client.wait_until(
        lambda snapshot: snapshot["scope_automation_id"] != dialog.get("automation_id", ""),
        timeout,
        "the Z offset generate dialog to close",
    )
    result: dict[str, Any] = {"generate": values}
    if export_path is not None:
        exported = Path(export_path)
        client.export_gcode(exported)
        result["export"] = inspect_gcode(exported)
    return result


def apply_result(
    client: ApiClient,
    *,
    pad: int,
    measured_height: float,
    timeout: float = 30.0,
) -> dict[str, Any]:
    """Open Calibration > Apply Z offset and write the offset into the active
    filament preset. The preset is dirty until the user saves it."""
    client.invoke(MENU_APPLY)
    client.wait_for_dialog("Apply Z offset", timeout)
    client.set_value(ID_PAD, pad)
    client.set_value(ID_HEIGHT, measured_height)
    _invoke_id_or_label(client, ID_APPLY, "Apply to filament")
    try:
        client.wait_for_modal(timeout)
        _dismiss_message(client)
    except AutomationError:
        pass
    return {
        "pad": pad,
        "measured_height": measured_height,
        "filament_z_offset": _read_option(client, "superslicer.option.filament_z_offset"),
    }


def _read_option(client: ApiClient, automation_id: str) -> Any:
    found = client.elements(automation_id=automation_id)
    if not found:
        found = client.elements(automation_id=automation_id + "#0")
    if not found:
        return None
    return found[0]["value"]


def inspect_gcode(path: Path | str) -> dict[str, Any]:
    text = Path(path).read_text(encoding="utf-8", errors="replace")
    start_at = text.find("START_PRINT")
    filament_at = text.find("; Filament preset Z offset")
    pad_offsets = [
        match.group("offset")
        for match in PAD_OFFSET.finditer(text)
        if "Filament preset Z offset"
        not in text[max(0, match.start() - 80) : match.start()]
    ]
    filament_offsets = [
        match.group("offset")
        for match in re.finditer(
            r"; Filament preset Z offset\nSET_GCODE_OFFSET Z=(?P<offset>-?\d+(?:\.\d+)?) MOVE=0",
            text,
        )
    ]
    return {
        "path": str(Path(path)),
        "has_start_print": start_at != -1,
        "filament_offset_after_start": (
            start_at != -1 and filament_at != -1 and start_at < filament_at
        ),
        "filament_offsets": filament_offsets,
        "pad_offsets": pad_offsets,
        "pad_count": len(pad_offsets),
    }


def send_gcode(
    path: Path | str,
    host: str,
    *,
    start: bool = False,
    remote_name: str | None = None,
) -> dict[str, Any]:
    return MoonrakerHost(host).send_gcode(path, start=start, remote_name=remote_name)


REQUIRED_MACROS = ("START_PRINT", "END_PRINT", "SET_CONTEXT")


def _require_macros(printer: MoonrakerHost) -> list[str]:
    """Refuse to run against a stub config.

    A rehearsal VM whose START_PRINT is just `M117 Start print` passes every
    Z-offset assertion by doing nothing at all, so the check has to prove the
    real macros are loaded before it can mean anything.
    """
    config = printer.request("GET", "/printer/objects/query?configfile")
    settings = config.get("status", {}).get("configfile", {}).get("config", {})
    present = {
        name.split(None, 1)[1] for name in settings if name.startswith("gcode_macro ")
    }
    missing = [name for name in REQUIRED_MACROS if name not in present]
    if missing:
        raise AutomationError(
            f"printer config is missing {', '.join(missing)} - a stub rehearsal "
            "config cannot exercise the Z offset path"
        )
    if "save_variables" not in settings:
        raise AutomationError(
            "printer config has no [save_variables], so per-context Z is never stored"
        )
    return sorted(present & set(REQUIRED_MACROS))


def regression_check(
    host: str,
    *,
    filament: str = "Regression Test Filament",
    nozzle: str = "0.6",
    surface: str = "Textured_PEI",
    measured: float = -0.057,
    preset_offset: float = 0.0,
    bed: float = 110.0,
    extruder: float = 270.0,
) -> dict[str, Any]:
    """Replay one print against the printer's own macros and report whether a
    hand-measured per-context Z offset survives it.

    SuperSlicer owns the live offset now: filament_z_offset is emitted as an
    absolute SET_GCODE_OFFSET after the start G-code, so it wins over whatever
    SET_CONTEXT applied. END_PRINT then copies whatever is live back into the
    active context variable, which means the slicer's preset value overwrites
    the measured bias stored on the printer. This drives the real macros end to
    end so the rig fails when that happens.
    """
    printer = MoonrakerHost(host)
    macros = _require_macros(printer)

    context_args = f'FILAMENT="{filament}" NOZZLE="{nozzle}" SURFACE="{surface}"'
    printer.gcode_script(f"SET_CONTEXT {context_args}")
    key = str(printer.saved_variables().get("current_context", "")).replace("'", "")
    if not key:
        raise AutomationError("SET_CONTEXT did not record a current_context variable")

    # Stand in for a bias the user measured by hand and saved for this context.
    printer.gcode_script(f"SAVE_VARIABLE VARIABLE={key} VALUE={measured}")

    printer.gcode_script(f"START_PRINT BED={bed} EXTRUDER={extruder} {context_args}")
    after_start = printer.live_z_offset()
    # What the slicer writes once the user's start G-code has run.
    printer.gcode_script(f"SET_GCODE_OFFSET Z={preset_offset} MOVE=0")
    before_end = printer.live_z_offset()
    printer.gcode_script("END_PRINT")

    saved_after = printer.saved_variables().get(key)
    preserved = saved_after is not None and abs(float(saved_after) - measured) < 1e-6
    return {
        "host": host,
        "context": key,
        "macros_present": macros,
        "measured_bias": measured,
        "preset_offset": preset_offset,
        "live_after_start_print": after_start,
        "live_before_end_print": before_end,
        "saved_after_print": saved_after,
        "passed": preserved,
    }


def run_workflow(
    client: ApiClient,
    *,
    export_path: Path | str,
    center: float | None = None,
    step: float | None = None,
    layer_height: float | None = None,
    outer_walls: int | None = None,
    moonraker: str | None = None,
    start_print: bool = False,
    timeout: float = 60.0,
) -> dict[str, Any]:
    """Generate the nine-pad test, export G-code, and optionally upload it."""
    generated_test = generate_test(
        client,
        center=center,
        step=step,
        layer_height=layer_height,
        outer_walls=outer_walls,
        export_path=export_path,
        timeout=timeout,
    )
    exported = generated_test.get("export") or {}
    if exported.get("pad_count", 0) != 9:
        raise AutomationError(
            f"expected 9 pad SET_GCODE_OFFSET lines, found {exported.get('pad_count')}"
        )
    if moonraker:
        result["moonraker"] = send_gcode(
            export_path, moonraker, start=start_print
        )
    return result
