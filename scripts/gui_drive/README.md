# gui_drive

Small scripts that drive the **running** SuperSlicer GUI for diagnosis and
screenshotting. They use `pyautogui` + `pywin32` against an already-launched
`superslicer.exe`, building on the larger helpers in:
- `tools/scripts/superslicer_auto.py` (find window, screenshot, slice clicks)
- `SuperSlicer/capture_slicer_window.py` (low-level capture)

## When to use this vs `superslicer_mcp` / `preset_slice`

| Path | What it produces | Faithful to GUI? |
|---|---|---|
| `superslicer_console --load *.ini` | Gcode | **No** — segfaults on real 3-profile loads |
| `preset_slice.exe` (this repo) | Gcode via PresetBundle | **Mostly** — doesn't replicate unsaved preset edits, project state |
| **GUI + `gui_drive` scripts** | Screenshots / live gcode export | **Yes** — it IS the GUI |

For fan/feature-coloring debugging, **always** use the GUI path. The CLI
tools miss the GUI's modified-preset state, which is often the root cause
(see `scripts/superslicer_mcp/README.md` § Bug 1).

## Setup

The conda env already has pyautogui, pywin32, and Pillow:
```
conda run -n superslicer python -c "import pyautogui, win32gui, PIL"
```

SuperSlicer must be running (launch it manually, or via the VS IDE Bridge's
`debug_start`).

## Scripts

### `snap_maximized.py`

Maximizes SS, captures a full-resolution PNG of the whole window, restores
the original size. Use when the GUI is partly hidden behind VS or another
window — PrintWindow captures the actual content only when it's fully
laid out.

```
conda run -n superslicer python -m gui_drive.snap_maximized \
    --out C:/path/to/out.png
```

### `capture_views.py`

Maximizes SS, optionally switches to sliced fan-speed preview, clicks once in
the canvas to grab keyboard focus, then sends the SS digit-key camera shortcuts
(`0`=iso, `1`=top, ..., `6`=right) and screenshots each angle, then restores the
original window placement.

```
conda run -n superslicer python -m gui_drive.capture_views \
    --out-dir test_models/slice_output/fan_view_overhang_test \
    --views iso front left right \
    --prepare-fan-speed \
    --focus-x 0.70 --focus-y 0.72
```

Load the model first. `--prepare-fan-speed` presses **Ctrl+3**, waits for the
auto-slice, then opens the G-code preview legend selector labeled **Feature
type** and clicks **Fan speed (%)** before capturing.

The focus coordinates are normalized to the maximized SS window, not absolute
screen pixels. If the camera keys do not take effect, rerun with a canvas point
that is clearly inside the preview area, for example `--focus-x 0.78 --focus-y
0.66`. Use `--redraw-delay` if a slow slice or GPU needs more time before each
screenshot.

## Workflow for fan-emission diagnosis (the one that works)

1. Launch SS through VS (`debug_start`) or directly
2. `superslicer.exe --single-instance test_models/overhang test.stl` to load
3. Run `python -m gui_drive.capture_views --prepare-fan-speed --out-dir <dir> --views iso front left right`
4. Inspect the four PNGs. Cross-check against:
   - the on-disk filament profile values (`grep ... \*.ini`)
   - the gcode tail (the slicer dumps the merged config there as `; key = value`)

## Fan-speed selection note

The reliable path is the visible Legend dropdown in the upper-left of the
G-code preview after **Ctrl+3**. The older bottom-toolbar coordinate guess can
leave the preview on `Feature type`, so `--prepare-fan-speed` targets the Legend
dropdown instead. Do not use **Alt+Down** / **Alt+Up** for the selector; those
keys move the layer preview slider.
