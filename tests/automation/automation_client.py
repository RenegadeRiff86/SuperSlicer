#!/usr/bin/env python3
"""Client for SuperSlicer's embedded automation API.

The API is an authenticated loopback HTTP server with a REST surface under
/api/v1 and an MCP surface at /mcp. Every request carries a bearer token that
the slicer reads from SUPERSLICER_AUTOMATION_TOKEN at startup.

Both the end-to-end test and the gui_drive CLI are built on this module, so the
request plumbing lives in exactly one place.
"""

from __future__ import annotations

import http.client
import json
import os
import secrets
import subprocess
import time
from pathlib import Path
from typing import Any, Iterable

DEFAULT_PORT = 43127
TOKEN_VARIABLE = "SUPERSLICER_AUTOMATION_TOKEN"
# Anchored to the checkout rather than the working directory: a relative default silently
# picks a different binary - or none - depending on where the caller happened to stand.
# build-linux-current-release is a symlink to this same tree, so a process listing shows
# the resolved path and not whichever name was asked for.
REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_EXECUTABLE = REPO_ROOT / "build-linux-x86_64-release" / "bin" / "superslicer"


class AutomationError(RuntimeError):
    """An API call was rejected, or the slicer never became reachable."""


class ApiClient:
    def __init__(self, port: int = DEFAULT_PORT, token: str | None = None) -> None:
        resolved = token if token is not None else os.environ.get(TOKEN_VARIABLE)
        if not resolved:
            raise AutomationError(f"{TOKEN_VARIABLE} is not set")
        self.port = port
        self.token = resolved
        self.sequence = 0

    # -- transport ---------------------------------------------------------

    def request(
        self,
        method: str,
        path: str,
        body: Any | None = None,
        *,
        authorized: bool = True,
        host: str | None = None,
        origin: str | None = None,
        request_id: str | None = None,
        raw_body: bytes | None = None,
    ) -> tuple[int, dict[str, Any]]:
        timeout_seconds = 20.0
        if isinstance(body, dict) and isinstance(body.get("timeout_ms"), int):
            timeout_seconds = max(timeout_seconds, body["timeout_ms"] / 1000.0 + 5.0)
        connection = http.client.HTTPConnection(
            "127.0.0.1", self.port, timeout=timeout_seconds
        )
        payload = raw_body
        if payload is None and body is not None:
            payload = json.dumps(body).encode("utf-8")

        connection.putrequest(method, path, skip_host=True)
        connection.putheader("Host", host or f"127.0.0.1:{self.port}")
        connection.putheader("Accept", "application/json, text/event-stream")
        if authorized:
            connection.putheader("Authorization", f"Bearer {self.token}")
        if origin is not None:
            connection.putheader("Origin", origin)
        if request_id is not None:
            connection.putheader("X-Request-ID", request_id)
        if payload is not None:
            connection.putheader("Content-Type", "application/json")
            connection.putheader("Content-Length", str(len(payload)))
        connection.endheaders(payload)

        response = connection.getresponse()
        response_bytes = response.read()
        connection.close()
        if not response_bytes:
            return response.status, {}
        return response.status, json.loads(response_bytes.decode("utf-8"))

    def rest(
        self, method: str, path: str, body: Any | None = None, **kwargs: Any
    ) -> dict[str, Any]:
        status, response = self.request(method, path, body, **kwargs)
        if status != 200 or not response.get("ok"):
            raise AutomationError(f"{method} {path} failed ({status}): {response}")
        return response["result"]

    def mcp(self, method: str, params: dict[str, Any] | None = None) -> dict[str, Any]:
        self.sequence += 1
        message: dict[str, Any] = {
            "jsonrpc": "2.0",
            "id": self.sequence,
            "method": method,
        }
        if params is not None:
            message["params"] = params
        status, response = self.request("POST", "/mcp", message)
        if status != 200 or "error" in response:
            raise AutomationError(f"MCP {method} failed ({status}): {response}")
        return response["result"]

    def mcp_tool(
        self, name: str, arguments: dict[str, Any] | None = None
    ) -> dict[str, Any]:
        tool_call = self.mcp("tools/call", {"name": name, "arguments": arguments or {}})
        structured = tool_call["structuredContent"]
        if tool_call.get("isError") or not structured.get("ok"):
            raise AutomationError(f"MCP tool {name} failed: {structured}")
        return structured["result"]

    # -- state -------------------------------------------------------------

    def status(self) -> dict[str, Any]:
        return self.rest("GET", "/api/v1/status")

    def snapshot(self, include_hidden: bool = False) -> dict[str, Any]:
        path = "/api/v1/ui/snapshot"
        if include_hidden:
            path += "?include_hidden=true"
        return self.rest("GET", path)

    # -- elements ----------------------------------------------------------

    def elements(
        self,
        *,
        automation_id: str | None = None,
        name: str | None = None,
        role: str | None = None,
        prefix: str | None = None,
        snapshot: dict[str, Any] | None = None,
    ) -> list[dict[str, Any]]:
        state = snapshot if snapshot is not None else self.snapshot()
        found = []
        for element in state["elements"]:
            if automation_id is not None and element["automation_id"] != automation_id:
                continue
            if prefix is not None and not element["automation_id"].startswith(prefix):
                continue
            if name is not None and element["name"] != name:
                continue
            if role is not None and element["role"] != role:
                continue
            found.append(element)
        return found

    def element(self, **kwargs: Any) -> dict[str, Any]:
        found = self.elements(**kwargs)
        if not found:
            raise AutomationError(f"no element matched {kwargs}")
        return found[0]

    def action(self, element: dict[str, Any], action: str, **arguments: Any) -> Any:
        body = {"ref": element["ref"], "action": action}
        body.update(arguments)
        return self.rest("POST", "/api/v1/ui/action", body)

    def invoke(self, automation_id: str, **kwargs: Any) -> Any:
        return self.action(self.element(automation_id=automation_id, **kwargs), "invoke")

    def set_value(self, automation_id: str, value: Any) -> Any:
        return self.action(
            self.element(automation_id=automation_id), "set_value", value=value
        )

    def toggle(self, automation_id: str, value: bool | None = None) -> Any:
        element = self.element(automation_id=automation_id)
        if value is None:
            return self.action(element, "toggle")
        return self.action(element, "toggle", value=value)

    def focus(self, automation_id: str) -> Any:
        return self.action(self.element(automation_id=automation_id), "focus")

    def screenshot(self, ref: str | None = None) -> bytes:
        import base64

        arguments = {"ref": ref} if ref is not None else {}
        captured = self.mcp_tool("superslicer_ui_screenshot", arguments)
        return base64.b64decode(captured["data_base64"])

    # -- workflows ---------------------------------------------------------

    def load_model(self, path: Path | str) -> dict[str, Any]:
        return self.rest(
            "POST", "/api/v1/workflows/load_model", {"path": str(Path(path).resolve())}
        )

    def slice(self, timeout_ms: int = 300000) -> dict[str, Any]:
        started = self.rest("POST", "/api/v1/workflows/slice", {})
        return self.wait_operation(started["operation_id"], timeout_ms)

    def select_view(self, view: str) -> dict[str, Any]:
        return self.rest("POST", "/api/v1/workflows/select_view", {"view": view})

    def set_preview(self, **fields: Any) -> dict[str, Any]:
        """Set preview slider ranges and/or move-type visibility, and report the visibility
        of every option afterwards. Pass options={"wipe": True} to switch a move type on:
        the legend that owns those toggles is drawn by ImGui and has no element to click,
        and a move type that is off is not in the frame at all - so a screenshot comparison
        over one that was never enabled compares two pictures of nothing."""
        return self.rest("POST", "/api/v1/workflows/set_preview", fields)

    def preview_options(self) -> dict[str, bool]:
        return self.set_preview()["options"]

    def paint_supports_by_angle(self, threshold_deg: float, block: bool = False) -> dict[str, Any]:
        """Paint support enforcers (or blockers) on every facet within threshold_deg of straight
        down, and report the facet count per volume. The gizmo's panel is ImGui-drawn and has no
        element to click, so this is the only way to exercise the angle threshold."""
        return self.rest(
            "POST",
            "/api/v1/workflows/paint_supports_by_angle",
            {"threshold_deg": threshold_deg, "block": block},
        )

    def quit(self, force: bool = True) -> dict[str, Any]:
        """Ask the slicer to exit. Invoking the File > Quit menu item does not: it calls
        Close(false), which any unsaved-project or print-host prompt is free to veto, and
        nothing in an unattended run answers those."""
        try:
            return self.rest("POST", "/api/v1/workflows/quit", {"force": force})
        except (ConnectionError, OSError) as error:
            # The window can go before the reply is flushed; that is still a successful quit.
            return {"closing": True, "force": force, "detail": str(error)}

    def export_gcode(
        self, path: Path | str, overwrite: bool = True, timeout_ms: int = 300000
    ) -> dict[str, Any]:
        started = self.rest(
            "POST",
            "/api/v1/workflows/export_gcode",
            {"path": str(Path(path).resolve()), "overwrite": overwrite},
        )
        return self.wait_operation(started["operation_id"], timeout_ms)

    def new_project(self, name: str = "") -> dict[str, Any]:
        payload: dict[str, Any] = {}
        if name:
            payload["name"] = name
        return self.rest("POST", "/api/v1/workflows/new_project", payload)

    def arrange(self) -> dict[str, Any]:
        return self.rest("POST", "/api/v1/workflows/arrange", {})

    def orient(self, timeout: float = 300.0) -> dict[str, Any]:
        """Rotate objects to their optimal print orientation and wait for the job to end.
        The endpoint returns as soon as the job is queued, so a caller that slices straight
        afterwards would slice the old pose."""
        started = self.rest("POST", "/api/v1/workflows/orient", {})
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if not self.status()["job_running"]:
                return started
            time.sleep(0.1)
        raise AutomationError(f"orient did not finish within {timeout:g}s")

    # -- file dialogs ------------------------------------------------------

    def arm_file_dialog(
        self,
        answer: str = "cancel",
        paths: list[Path | str] | None = None,
        title_contains: str | None = None,
        filter_index: int = 0,
        checkbox: bool = False,
    ) -> dict[str, Any]:
        """Queue the answer for the next file dialog. Arm BEFORE triggering the
        action that opens one: ShowModal() blocks the GUI thread, so once a native
        chooser is up there is no request that can still answer it."""
        payload: dict[str, Any] = {
            "answer": answer,
            "filter_index": filter_index,
            "checkbox": checkbox,
        }
        if paths is not None:
            payload["paths"] = [str(Path(path).resolve()) for path in paths]
        if title_contains is not None:
            payload["title_contains"] = title_contains
        return self.rest("POST", "/api/v1/workflows/arm_file_dialog", payload)

    def clear_file_dialogs(self) -> dict[str, Any]:
        return self.rest("POST", "/api/v1/workflows/arm_file_dialog", {"clear": True})

    def file_dialog_status(self) -> dict[str, Any]:
        return self.rest("POST", "/api/v1/workflows/file_dialog_status", {})

    def wait_operation(self, operation_id: str, timeout_ms: int = 60000) -> dict[str, Any]:
        state = self.rest(
            "POST",
            "/api/v1/wait",
            {"operation_id": operation_id, "timeout_ms": timeout_ms},
        )
        if state["state"] != "succeeded":
            raise AutomationError(f"operation did not succeed: {state}")
        return state

    # -- polling -----------------------------------------------------------

    def wait_until(
        self, predicate, timeout: float = 20.0, description: str = "condition"
    ):
        """Poll a snapshot-derived predicate. Menu commands that open a dialog are
        dispatched asynchronously, so the caller waits on observable UI state
        rather than on the invoking request."""
        deadline = time.monotonic() + timeout
        last = None
        while time.monotonic() < deadline:
            last = self.snapshot()
            outcome = predicate(last)
            if outcome:
                return outcome
            time.sleep(0.1)
        raise AutomationError(
            f"timed out after {timeout:g}s waiting for {description}; "
            f"scope was {None if last is None else last['scope_automation_id']!r}"
        )

    def wait_for_scope(self, scope_id: str, timeout: float = 20.0) -> dict[str, Any]:
        return self.wait_until(
            lambda snapshot: snapshot if snapshot["scope_automation_id"] == scope_id else None,
            timeout,
            f"scope {scope_id!r}",
        )

    def wait_for_modal(self, timeout: float = 20.0) -> dict[str, Any]:
        return self.wait_until(
            lambda snapshot: snapshot if snapshot["modal"] else None,
            timeout,
            "a modal dialog",
        )

    def wait_for_dialog(self, title: str | None = None, timeout: float = 20.0) -> dict[str, Any]:
        """Wait for any dialog, modal or not. SuperSlicer's calibration dialogs are
        non-modal, so they never set the snapshot's modal flag."""

        def matched(snapshot: dict[str, Any]):
            candidates = self.elements(role="dialog", snapshot=snapshot)
            if title is None:
                return candidates[0] if candidates else None
            needle = title.lower()
            return next((e for e in candidates if needle in e["name"].lower()), None)

        return self.wait_until(matched, timeout, f"dialog {title or '(any)'!r}")

    def wait_for_element(self, automation_id: str, timeout: float = 20.0) -> dict[str, Any]:
        def matched(snapshot: dict[str, Any]):
            found = self.elements(automation_id=automation_id, snapshot=snapshot)
            return found[0] if found else None

        return self.wait_until(matched, timeout, f"element {automation_id!r}")


def launch(
    executable: Path | str = DEFAULT_EXECUTABLE,
    port: int = DEFAULT_PORT,
    token: str | None = None,
    extra_args: Iterable[str] = (),
) -> tuple[subprocess.Popen, ApiClient]:
    resolved_token = token or secrets.token_urlsafe(32)
    environment = os.environ.copy()
    environment[TOKEN_VARIABLE] = resolved_token
    slicer_process = subprocess.Popen(
        [
            str(Path(executable).resolve()),
            "--automation-api",
            "--automation-api-port",
            str(port),
            *extra_args,
        ],
        env=environment,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    return slicer_process, ApiClient(port, resolved_token)


def probe_status(
    client: ApiClient,
    slicer_process: subprocess.Popen | None,
    require_gui: bool,
    expect_executable: Path | str | None = None,
) -> dict[str, Any] | None:
    """One status poll. Returns the status block once the server is up, is the one
    we launched, and is ready; None while it is still coming up."""
    status, response = client.request("GET", "/api/v1/status")
    if status != 200 or not response.get("ok"):
        return None
    reported = response["result"]
    # Attaching skips the pid check below - there is no process of ours to compare against -
    # so this is the only thing standing between a caller and an hour of measuring a build
    # it did not mean to test.
    if expect_executable is not None:
        running = reported.get("executable")
        if running is None:
            raise AutomationError(
                "this slicer does not report its executable path, so it cannot be checked "
                "against the one requested; it predates the field. Rebuild it, or drop "
                "--executable to accept whatever owns the port."
            )
        if Path(running).resolve() != Path(expect_executable).resolve():
            raise AutomationError(
                f"port {client.port} is owned by {running}, not the requested "
                f"{Path(expect_executable).resolve()}. Stop that instance or use --port."
            )
    # An older slicer still holding the port answers on it, and its replies look
    # perfectly healthy - so the run silently drives the previous binary. Refuse
    # rather than report another process's state as if it were ours.
    if slicer_process is not None and reported["pid"] != slicer_process.pid:
        raise AutomationError(
            f"port {client.port} is owned by SuperSlicer pid {reported['pid']}, not the "
            f"one just launched (pid {slicer_process.pid}). Stop the other instance or "
            "use --port."
        )
    if require_gui and not reported["gui_ready"]:
        return None
    return reported


def wait_ready(
    client: ApiClient,
    slicer_process: subprocess.Popen | None = None,
    timeout: float = 60.0,
    require_gui: bool = True,
    expect_executable: Path | str | None = None,
) -> dict[str, Any]:
    deadline = time.monotonic() + timeout
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        if slicer_process is not None and slicer_process.poll() is not None:
            raise AutomationError(
                f"SuperSlicer exited during startup with code {slicer_process.returncode}"
            )
        try:
            reported = probe_status(client, slicer_process, require_gui, expect_executable)
        except (ConnectionError, OSError, json.JSONDecodeError) as error:
            last_error = error
            reported = None
        if reported is not None:
            return reported
        time.sleep(0.1)
    raise AutomationError(f"automation API did not become ready: {last_error}")


def wait_closed(client: ApiClient, timeout: float = 30.0) -> bool:
    """Poll until the API stops answering. Quitting is asynchronous - the reply comes back
    before the window is gone - so this is what tells a script the port is free again."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            client.request("GET", "/api/v1/status")
        except (ConnectionError, OSError):
            return True
        time.sleep(0.1)
    return False


def wait_process_gone(pid: int, timeout: float = 30.0) -> bool:
    """The socket closes while the process is still tearing down, and it is the process that
    matters: a slicer that never exits is what fills the machine with instances holding the
    port until the next launch fails."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            os.kill(pid, 0)
        except OSError:
            return True
        time.sleep(0.1)
    return False


def terminate(slicer_process: subprocess.Popen | None) -> None:
    if slicer_process is None or slicer_process.poll() is not None:
        return
    slicer_process.terminate()
    try:
        slicer_process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        slicer_process.kill()
        slicer_process.wait(timeout=5)
