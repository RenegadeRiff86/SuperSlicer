---
name: superslicer-automation
description: >-
  Drive the running SuperSlicer GUI with the superslicer-automation MCP tools.
  Use when the user says to open SuperSlicer, walk the GUI, click the
  interface, snapshot, screenshot, slice, or fix a UI issue, or when calling
  automation_debug, launch, snapshot, menu, invoke, hover, or screenshot.
---

# SuperSlicer automation

Drive the GUI through the MCP server `project-0-SuperSlicer-superslicer-automation`. Read each tool schema with GetDynamicTools before calling it. The package behind those tools is `tests/automation/`. Add a missing command there. Do not write a one-off script, and do not shell out to `gui_drive.py` to click, snapshot, or launch while this server can take the call.

## Connect

1. `automation_debug` with `enabled` true. Until that returns, the other tools are not published.
2. `status`. If `gui_ready` is true, that process is the one to drive. Do not call `launch`.
3. `launch` only when status says nothing is listening. It starts one slicer with `--automation-api` and does not return the token. A live process has comm `superslicer` or `slic3r_main` (zombies do not count). A second launch is a bug.
4. If a call fails with "MCP process client is not registered", call `mcp_auth` on that server once and retry the same call. If it still fails, stop. The server is not attached to this chat. Do not recover by reading another process's environment or by starting a second slicer.

`launch` defaults to the release binary. Pass `executable` when the change under test is only in another build. The unoptimized debug binary aborts on asserts; do not point it at a model that trips those, and do not pass `--load-user-profiles` to it.

## Look, then act

`snapshot` is the current scope. `menus`, `tabs`, and `options` list what can be driven. A short menu id is prefixed with `superslicer.menu.`, a short option with `superslicer.option.`, and a short tab name with the tab prefix the tool already applies.

`invoke` posts the command and does not move the pointer. `hover` enters a control. `pointer` clicks only when SuperSlicer is the foreground window, because the click is delivered to whatever surface is under the pointer. `screenshot` renders the SuperSlicer window this process owns and writes a PNG to the path you pass. It does not ask the compositor which window is active, and it returns no image bytes. Read the PNG after it is written. `desktop` tiles the windows and leaves Cursor in front.

`wait` with `dialog` matches a dialog title. Calibration dialogs are not modal, so `modal` never sees them. A modal `wxMessageBox` blocks the GUI thread and the HTTP call can return 504 even though the click landed; read the setting that should have changed.

## Leave these alone

Do not click the config wizard Finish button, "Remove user profiles", or desktop integration. Do not generate or apply flow calibration or the Z-offset workflow unless the user asked for that run. After a preset is dirty, the next generate raises "Creating a new project: Unsaved Changes"; clear it with `invoke` on the button labeled Keep before continuing.
