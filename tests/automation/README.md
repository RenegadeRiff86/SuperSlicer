# SuperSlicer GUI automation

This directory is the reusable way to drive the **running SuperSlicer GUI**.
Do not write one-off scripts in `/tmp`. Extend `gui_drive.py` and the modules
it imports so the next session can find the same command.

## Start the slicer once

```
export SUPERSLICER_AUTOMATION_TOKEN=$(openssl rand -hex 16)
python3 tests/automation/gui_drive.py --keep-open launch
```

Every later step uses `--attach` and that same token. Launch stays in the
foreground of the terminal that started it. Do not background it.

## Klipper Z-offset calibration through to a print

The filament Z offset now lives in SuperSlicer (`filament_z_offset`). The
printer `SET_CONTEXT` macros are obsolete for that job. This workflow is how
you prove the GUI still generates the nine-pad test, writes `SET_GCODE_OFFSET`
after `START_PRINT`, and can hand the file to Moonraker.

Stable control ids (set in `CalibrationBedDialog.cpp` via `AutomationIds::calibration`):

| Id | Control |
|---|---|
| `superslicer.calibration.z_offset.center` | Center offset field |
| `superslicer.calibration.z_offset.step` | Step field |
| `superslicer.calibration.z_offset.layer_height` | Layer height field |
| `superslicer.calibration.z_offset.outer_walls` | Outer walls field |
| `superslicer.calibration.z_offset.generate` | Generate button |
| `superslicer.calibration.z_offset.measured_pad` | Apply pad number |
| `superslicer.calibration.z_offset.measured_height` | Apply measured height |
| `superslicer.calibration.z_offset.apply` | Apply to filament |

Menus:

- `superslicer.menu.calibration.klipper_z_offset`
- `superslicer.menu.calibration.apply_z_offset`
- `superslicer.menu.calibration.filament_flow_calibration`

### Filament Flow calibration

Opening this dialog used to abort the process when `wxHtmlWindow` decoded the
help-page JPEGs through a mixed libjpeg (`library is 80, caller expects 62`).
The help images are PNG now. Prove the dialog still opens:

```
python3 tests/automation/gui_drive.py --attach flow-calibration
```

`--generate recommended` (or `fine`) clicks one of the generate buttons. Recommended is the Orca YOLO pass (±5% at 1%). After printing, `--apply --tile 6` selects that chip and writes `old + modifier` to the filament extrusion multiplier.

### Generate, export, optionally upload

```
python3 tests/automation/gui_drive.py --attach z-offset-workflow \
    --export /tmp/z-offset-pads.gcode \
    --center -0.057 --step 0.027 \
    --moonraker http://192.168.122.147:7125
```

`--start-print` starts the uploaded file. The rehearsal VM is a virtual
printer; use it to accept G-code, not to run real motion.

### Apply a measured pad to the active filament

```
python3 tests/automation/gui_drive.py --attach z-offset-apply --pad 5 --measured 0.35
```

That dirties the filament preset. Save it in the GUI if you want it kept.

Two things to expect while driving it. The Apply button ends in a modal
`wxMessageBox`, which blocks the GUI thread, so the HTTP call returns 504 even
though the apply itself succeeds - check `filament_z_offset` rather than the
exit code. And once a preset is dirty, the next generate raises a modal
"Creating a new project: Unsaved Changes"; clear it with
`invoke button --label Keep` before carrying on.

### Prove a measured offset survives a print

```
python3 tests/automation/gui_drive.py z-offset-regression \
    --moonraker http://192.168.122.147:7125
```

No slicer needed - this one talks only to the printer. It saves a measured bias
for a context, replays START_PRINT / the slicer's own SET_GCODE_OFFSET /
END_PRINT, and fails if the saved value did not survive. It refuses to run
unless the printer really defines START_PRINT, END_PRINT, SET_CONTEXT and
`[save_variables]`, because a stub rehearsal config passes this test by doing
nothing at all.

### Inspect or re-upload an existing file (no slicer required)

```
python3 tests/automation/gui_drive.py inspect-gcode /tmp/z-offset-pads.gcode
python3 tests/automation/gui_drive.py send-gcode /tmp/z-offset-pads.gcode \
    --moonraker http://192.168.122.147:7125
```

`inspect-gcode` must show `START_PRINT` first, then `; Filament preset Z offset`,
then nine pad `SET_GCODE_OFFSET` lines.

## Window move and scroll

```
python3 tests/automation/gui_drive.py --attach window
python3 tests/automation/gui_drive.py --attach window 80 40
python3 tests/automation/gui_drive.py --attach scroll --dy 400
python3 tests/automation/gui_drive.py --attach scroll --lines 20
```

`window` without numbers prints the current screen position and size. `scroll`
moves the current settings page; positive `--dy` shows content below.

## Other durable commands

See the module docstring in `gui_drive.py` and `doc/gui-menu-audit.md`.
`automation_client.py` is the HTTP/MCP client. `moonraker.py` is the printer-host
client. `z_offset_workflow.py` is the Z-offset sequence those commands call.
