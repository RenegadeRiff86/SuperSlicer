#!/usr/bin/env python3
"""Moonraker / Klipper host used by gui_drive.

This is the durable way to send G-code a SuperSlicer GUI session just exported.
Do not invent a new uploader in /tmp; extend this module.
"""

from __future__ import annotations

import json
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any


class MoonrakerError(RuntimeError):
    """The printer host rejected a request or never answered."""


class MoonrakerHost:
    def __init__(self, base_url: str, timeout: float = 30.0) -> None:
        self.base_url = base_url.rstrip("/")
        self.timeout = timeout

    def _url(self, path: str) -> str:
        if not path.startswith("/"):
            path = "/" + path
        return self.base_url + path

    def request(self, method: str, path: str, body: Any | None = None) -> dict[str, Any]:
        request_body = None
        headers = {"Accept": "application/json"}
        if body is not None:
            request_body = json.dumps(body).encode("utf-8")
            headers["Content-Type"] = "application/json"
        request = urllib.request.Request(
            self._url(path), data=request_body, headers=headers, method=method
        )
        try:
            with urllib.request.urlopen(request, timeout=self.timeout) as response:
                payload = response.read()
        except urllib.error.HTTPError as error:
            detail = error.read().decode("utf-8", "replace")
            raise MoonrakerError(f"{method} {path} failed ({error.code}): {detail}") from error
        except urllib.error.URLError as error:
            raise MoonrakerError(f"{method} {path} failed: {error.reason}") from error
        if not payload:
            return {}
        parsed = json.loads(payload.decode("utf-8"))
        if isinstance(parsed, dict) and "result" in parsed:
            return parsed["result"]
        if isinstance(parsed, dict):
            return parsed
        raise MoonrakerError(f"{method} {path} returned a non-object payload")

    def info(self) -> dict[str, Any]:
        return self.request("GET", "/printer/info")

    def objects_query(self, *objects: str) -> dict[str, Any]:
        # The HTTP endpoint takes bare keys - ?gcode_move&toolhead=position - not the
        # {"objects": {...}} body the websocket JSON-RPC form uses. Sending the RPC
        # shape here does not fail; Moonraker just reports back an object named
        # '{"gcode_move": null}' with a null value, so every read looks empty.
        query = "&".join(urllib.parse.quote(name, safe="=,") for name in objects)
        return self.request("GET", f"/printer/objects/query?{query}")

    def upload(self, path: Path | str, remote_name: str | None = None) -> dict[str, Any]:
        source = Path(path)
        if not source.is_file():
            raise MoonrakerError(f"G-code file does not exist: {source}")
        filename = remote_name or source.name
        boundary = "----superslicer-moonraker"
        header = (
            f"--{boundary}\r\n"
            f'Content-Disposition: form-data; name="file"; filename="{filename}"\r\n'
            "Content-Type: application/octet-stream\r\n\r\n"
        ).encode("utf-8")
        footer = f"\r\n--{boundary}--\r\n".encode("utf-8")
        body = header + source.read_bytes() + footer
        request = urllib.request.Request(
            self._url("/server/files/upload"),
            body,
            {
                "Accept": "application/json",
                "Content-Type": f"multipart/form-data; boundary={boundary}",
            },
            method="POST",
        )
        try:
            with urllib.request.urlopen(request, timeout=self.timeout) as response:
                payload = response.read()
        except urllib.error.HTTPError as error:
            detail = error.read().decode("utf-8", "replace")
            raise MoonrakerError(f"upload failed ({error.code}): {detail}") from error
        except urllib.error.URLError as error:
            raise MoonrakerError(f"upload failed: {error.reason}") from error
        parsed = json.loads(payload.decode("utf-8")) if payload else {}
        if isinstance(parsed, dict) and "result" in parsed:
            return parsed["result"]
        return parsed if isinstance(parsed, dict) else {"item": parsed}

    def start_print(self, filename: str) -> dict[str, Any]:
        quoted = urllib.parse.quote(filename)
        return self.request("POST", f"/printer/print/start?filename={quoted}")

    def gcode_script(self, script: str) -> dict[str, Any]:
        """Run one G-code line (or macro call) and wait for Klipper to finish it."""
        quoted = urllib.parse.quote(script)
        return self.request("POST", f"/printer/gcode/script?script={quoted}")

    def saved_variables(self) -> dict[str, Any]:
        """The [save_variables] store - where the printer keeps per-context Z."""
        status = self.objects_query("save_variables").get("status", {})
        return status.get("save_variables", {}).get("variables", {})

    def live_z_offset(self) -> float:
        """The offset SET_GCODE_OFFSET is currently holding, as Klipper sees it."""
        status = self.objects_query("gcode_move").get("status", {})
        origin = status.get("gcode_move", {}).get("homing_origin", (0.0, 0.0, 0.0))
        return float(origin[2])

    def send_gcode(
        self,
        path: Path | str,
        *,
        start: bool = False,
        remote_name: str | None = None,
    ) -> dict[str, Any]:
        uploaded = self.upload(path, remote_name)
        item = uploaded.get("item", uploaded)
        stored = item.get("path") if isinstance(item, dict) else None
        if not stored:
            stored = remote_name or Path(path).name
        print_job = {"uploaded": stored, "host": self.base_url, "started": False}
        if start:
            print_job["print"] = self.start_print(stored)
            print_job["started"] = True
        return print_job
