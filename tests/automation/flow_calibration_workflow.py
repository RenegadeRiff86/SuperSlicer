#!/usr/bin/env python3
"""Reusable GUI workflow: open Filament Flow calibration and optionally generate cubes.

The help page used to abort SuperSlicer on open because wxHtmlWindow decoded
JPEG screenshots through a mixed libjpeg (library 80 vs caller 62). This command
is how later sessions prove the dialog still opens.
"""

from __future__ import annotations

from typing import Any

from automation_client import ApiClient

MENU_FLOW = "superslicer.menu.calibration.filament_flow_calibration"
DIALOG_TITLE = "Flow calibration"

GENERATE_IDS = {
    "recommended": "superslicer.calibration.flow.generate_recommended",
    "fine": "superslicer.calibration.flow.generate_fine",
}


def open_dialog(client: ApiClient, *, timeout: float = 20.0) -> dict[str, Any]:
    """Open Calibration > Filament Flow calibration and wait for the dialog."""
    client.invoke(MENU_FLOW)
    dialog = client.wait_for_dialog(DIALOG_TITLE, timeout)
    return {
        "dialog": dialog.get("name"),
        "automation_id": dialog.get("automation_id"),
        "menu": MENU_FLOW,
    }


def generate_test(
    client: ApiClient,
    *,
    interval: str = "recommended",
    timeout: float = 60.0,
) -> dict[str, Any]:
    """Open the dialog and generate the recommended or fine chip set."""
    if interval not in GENERATE_IDS:
        raise ValueError(f"interval must be one of {sorted(GENERATE_IDS)}, got {interval!r}")
    opened = open_dialog(client, timeout=timeout)
    client.invoke(GENERATE_IDS[interval])
    client.wait_until(
        lambda snapshot: snapshot["scope_automation_id"] != opened.get("automation_id", ""),
        timeout,
        "the flow calibration generate dialog to close",
    )
    return {"open": opened, "interval": interval, "generate": GENERATE_IDS[interval]}


def apply_result(
    client: ApiClient,
    *,
    tile: int = 5,
    timeout: float = 30.0,
) -> dict[str, Any]:
    """Open the flow dialog, select a tile by index, and apply it to the filament."""
    opened = open_dialog(client, timeout=timeout)
    client.invoke(f"superslicer.calibration.flow.tile.{tile}")
    client.invoke("superslicer.calibration.flow.apply")
    preview = client.elements(automation_id="superslicer.calibration.flow.preview")
    status = client.elements(automation_id="superslicer.calibration.flow.result")
    return {
        "open": opened,
        "tile": tile,
        "preview": preview[0]["name"] if preview else None,
        "result": status[0]["name"] if status else None,
    }
