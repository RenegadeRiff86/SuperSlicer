#!/usr/bin/env python3
"""End-to-end smoke test for SuperSlicer's embedded automation API."""

from __future__ import annotations

import argparse
import base64
import http.client
import json
import os
from pathlib import Path
import secrets
import subprocess
import sys
import time
from typing import Any


class ApiClient:
    def __init__(self, port: int, token: str) -> None:
        self.port = port
        self.token = token
        self.sequence = 0

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
            timeout_seconds = max(
                timeout_seconds, body["timeout_ms"] / 1000.0 + 5.0
            )
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
        self,
        method: str,
        path: str,
        body: Any | None = None,
        **kwargs: Any,
    ) -> dict[str, Any]:
        status, response = self.request(method, path, body, **kwargs)
        if status != 200 or not response.get("ok"):
            raise RuntimeError(f"{method} {path} failed ({status}): {response}")
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
            raise RuntimeError(f"MCP {method} failed ({status}): {response}")
        return response["result"]

    def mcp_tool(self, name: str, arguments: dict[str, Any] | None = None) -> dict[str, Any]:
        tool_call = self.mcp(
            "tools/call",
            {"name": name, "arguments": arguments or {}},
        )
        structured = tool_call["structuredContent"]
        if tool_call.get("isError") or not structured.get("ok"):
            raise RuntimeError(f"MCP tool {name} failed: {structured}")
        return structured["result"]


def _poll_status_result(
    client: ApiClient,
) -> tuple[dict[str, Any] | None, Exception | None]:
    """Return (ready result, None) or (None, error/None) when not ready."""
    try:
        status, response = client.request("GET", "/api/v1/status")
    except (ConnectionError, OSError, json.JSONDecodeError) as error:
        return None, error
    if status == 200 and response.get("ok"):
        return response["result"], None
    return None, None


def _terminate_slicer(slicer_process: subprocess.Popen[bytes]) -> None:
    slicer_process.terminate()
    try:
        slicer_process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        slicer_process.kill()
        slicer_process.wait(timeout=5)


def wait_for_server(
    client: ApiClient,
    slicer_process: subprocess.Popen[bytes] | None,
) -> dict[str, Any]:
    deadline = time.monotonic() + 30
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        if slicer_process is not None and slicer_process.poll() is not None:
            raise RuntimeError(
                "SuperSlicer exited during startup with code "
                f"{slicer_process.returncode}"
            )
        result, error = _poll_status_result(client)
        if error is not None:
            last_error = error
        elif result is not None:
            return result
        time.sleep(0.1)
    raise RuntimeError(f"automation API did not become ready: {last_error}")


def check_transport_and_protocol(client: ApiClient, status: dict[str, Any]) -> None:
    unauthorized, _ = client.request(
        "GET", "/api/v1/status", authorized=False
    )
    assert unauthorized == 401

    rejected_host, _ = client.request(
        "GET", "/api/v1/status", host="example.com"
    )
    assert rejected_host == 403

    rejected_origin, _ = client.request(
        "GET", "/api/v1/status", origin="https://example.com"
    )
    assert rejected_origin == 403

    malformed, malformed_body = client.request(
        "POST", "/mcp", raw_body=b"{"
    )
    assert malformed == 400
    assert malformed_body["error"]["code"] == "operation_failed"

    initialized = client.mcp(
        "initialize",
        {
            "protocolVersion": "2025-06-18",
            "capabilities": {},
            "clientInfo": {"name": "superslicer-e2e", "version": "1"},
        },
    )
    assert initialized["protocolVersion"] == "2025-06-18"
    client.mcp("ping")

    tools = client.mcp("tools/list")["tools"]
    names = {tool["name"] for tool in tools}
    expected = {
        "superslicer_status",
        "superslicer_ui_snapshot",
        "superslicer_ui_screenshot",
        "superslicer_ui_action",
        "superslicer_input",
        "superslicer_wait",
        "superslicer_batch",
        "superslicer_load_model",
        "superslicer_slice",
        "superslicer_select_view",
        "superslicer_set_preview",
        "superslicer_set_transform",
        "superslicer_export_gcode",
        "superslicer_new_project",
        "superslicer_arrange",
        "superslicer_orient",
        "superslicer_paint_supports_by_angle",
        "superslicer_arm_file_dialog",
        "superslicer_file_dialog_status",
        "superslicer_quit",
    }
    assert names == expected
    assert client.mcp_tool("superslicer_status")["pid"] == status["pid"]


def check_snapshot_and_deduplication(client: ApiClient) -> None:
    first = client.rest("GET", "/api/v1/ui/snapshot")
    assert first["generation"] > 0
    assert first["elements"]
    by_automation_id = {
        element["automation_id"]: element for element in first["elements"]
    }
    for selector_id in (
        "superslicer.view.select.3d",
        "superslicer.view.select.preview",
    ):
        selector = by_automation_id[selector_id]
        assert selector["actions"] == ["invoke"]
        assert selector["bounds"]["width"] == 0
        assert selector["bounds"]["height"] == 0

    focusable = next(
        element
        for element in first["elements"]
        if element["state"]["shown"]
        and element["state"]["enabled"]
        and "focus" in element["actions"]
    )
    second = client.rest("GET", "/api/v1/ui/snapshot")
    assert second["generation"] != first["generation"]

    stale_status, stale = client.request(
        "POST",
        "/api/v1/ui/action",
        {"ref": focusable["ref"], "action": "focus"},
    )
    assert stale_status == 409
    assert stale["error"]["code"] == "stale_ref"

    current = next(
        element
        for element in second["elements"]
        if element["state"]["shown"]
        and element["state"]["enabled"]
        and "focus" in element["actions"]
    )
    dedup_id = "automation-e2e-dedup"
    first_status, first_result = client.request(
        "POST",
        "/api/v1/ui/action",
        {"ref": current["ref"], "action": "focus"},
        request_id=dedup_id,
    )
    second_status, second_result = client.request(
        "POST",
        "/api/v1/ui/action",
        {"ref": current["ref"], "action": "focus"},
        request_id=dedup_id,
    )
    assert first_status == second_status == 200
    assert first_result == second_result

    canvas = next(
        element
        for element in second["elements"]
        if element["role"] == "canvas" and element["state"]["shown"]
    )
    screenshot = client.mcp_tool(
        "superslicer_ui_screenshot", {"ref": canvas["ref"]}
    )
    png = base64.b64decode(screenshot["data_base64"])
    assert png.startswith(b"\x89PNG\r\n\x1a\n")


def wait_operation(client: ApiClient, operation_id: str) -> None:
    operation_state = client.rest(
        "POST",
        "/api/v1/wait",
        {"operation_id": operation_id, "timeout_ms": 60000},
    )
    if operation_state["state"] != "succeeded":
        raise RuntimeError(f"operation did not succeed: {operation_state}")


def run_workflow(
    client: ApiClient,
    model: Path,
    export_path: Path | None,
) -> None:
    client.rest(
        "POST", "/api/v1/workflows/load_model", {"path": str(model)}
    )
    slicing = client.rest("POST", "/api/v1/workflows/slice", {})
    wait_operation(client, slicing["operation_id"])

    client.rest(
        "POST", "/api/v1/workflows/select_view", {"view": "preview"}
    )
    snapshot = client.rest("GET", "/api/v1/ui/snapshot")
    layer_slider = next(
        (
            element
            for element in snapshot["elements"]
            if element["automation_id"] == "superslicer.preview.layer_slider"
        ),
        None,
    )
    if layer_slider is not None:
        value = layer_slider["value"]
        client.rest(
            "POST",
            "/api/v1/workflows/set_preview",
            {
                "layer_lower": value["lower"],
                "layer_upper": value["higher"],
            },
        )

    client.rest(
        "POST",
        "/api/v1/workflows/set_transform",
        {"position": {"x": 100.0}},
    )
    canvas = next(
        element
        for element in snapshot["elements"]
        if element["role"] == "canvas" and element["state"]["shown"]
    )
    screenshot = client.mcp_tool(
        "superslicer_ui_screenshot", {"ref": canvas["ref"]}
    )
    assert base64.b64decode(screenshot["data_base64"]).startswith(b"\x89PNG")

    if export_path is not None:
        export_path.parent.mkdir(parents=True, exist_ok=True)
        exported = client.rest(
            "POST",
            "/api/v1/workflows/export_gcode",
            {"path": str(export_path), "overwrite": False},
        )
        wait_operation(client, exported["operation_id"])
        assert export_path.is_file()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--executable",
        type=Path,
        default=Path("build-linux-x86_64-release/bin/superslicer"),
    )
    parser.add_argument("--port", type=int, default=43127)
    parser.add_argument("--attach", action="store_true")
    parser.add_argument("--model", type=Path)
    parser.add_argument("--export", type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    slicer_process: subprocess.Popen[bytes] | None = None
    token = os.environ.get("SUPERSLICER_AUTOMATION_TOKEN")
    if args.attach:
        if not token:
            raise RuntimeError("attach mode requires SUPERSLICER_AUTOMATION_TOKEN")
    else:
        token = secrets.token_urlsafe(32)
        environment = os.environ.copy()
        environment["SUPERSLICER_AUTOMATION_TOKEN"] = token
        slicer_process = subprocess.Popen(
            [
                str(args.executable.resolve()),
                "--automation-api",
                "--automation-api-port",
                str(args.port),
            ],
            env=environment,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

    client = ApiClient(args.port, token)
    try:
        status = wait_for_server(client, slicer_process)
        check_transport_and_protocol(client, status)
        while not status["gui_ready"]:
            time.sleep(0.1)
            status = client.rest("GET", "/api/v1/status")
        check_snapshot_and_deduplication(client)
        if args.model is not None:
            run_workflow(
                client,
                args.model.resolve(),
                args.export.resolve() if args.export is not None else None,
            )
        print("SuperSlicer automation API end-to-end checks passed")
        return 0
    finally:
        if slicer_process is not None and slicer_process.poll() is None:
            _terminate_slicer(slicer_process)


if __name__ == "__main__":
    sys.exit(main())
