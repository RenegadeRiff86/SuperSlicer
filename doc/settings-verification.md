# Verifying that a setting does what it claims

Companion to `doc/gui-menu-audit.md`. That one asks whether a **menu item** does what its label
says; this one asks whether a **setting** actually reaches the G-code.

The two failure modes worth hunting:

1. The setting is accepted by the UI and silently never applied.
2. The setting is applied, but to the wrong thing - the wrong role, the wrong firmware command,
   or everywhere instead of only where it was scoped.

## Method

Drive the **CLI**, not the GUI. It takes every config key as `--kebab-case-name`, so a whole
matrix can be swept without touching presets, and the result is deterministic.

```
build-linux-x86_64-release/bin/superslicer --export-gcode \
    --gcode-flavor klipper --gcode-comments \
    --filament-pressure-advance 0.0111 \
    --filament-perimeter-pa 0.0222 \
    --output /tmp/out.gcode model.stl
```

Three rules that make the result trustworthy:

- **Give every scope a unique sentinel value.** `0.0222`, `0.0333`, `0.0444`… Never reuse a number
  and never use a value equal to a default - you cannot tell "applied" from "coincidence".
- **Parse state, not occurrences.** Walk the G-code tracking the current `;TYPE:` feature and the
  last value the setting emitted, and attribute every *extruding* move to that pair. Counting
  occurrences proves a value appears somewhere; pairing proves it applies to the right extrusions.
  `--gcode-comments` is required for the `;TYPE:` markers.
- **Run the negative control.** Slice again with the setting **unset** and confirm the output is
  absent entirely. Without this, a setting that is always-on looks identical to one that works.

## Verified

### Pressure advance - 2026-07-31, PASS

Base `--filament-pressure-advance 0.0111` plus five per-role overrides, sliced and attributed by
feature. Every override applied to exactly its own role, with no leakage:

| Feature | Expected | PA in force |
|---|---|---|
| Perimeter | 0.0222 | 0.0222 only (62497 moves) |
| External perimeter | 0.0333 | 0.0333 only (13706) |
| Solid infill | 0.0444 | 0.0444 only (16875) |
| Internal infill | 0.0555 | 0.0555 only (6948) |
| Top solid infill | 0.0666 | 0.0666 only (2418) |
| Bridge / Gap fill / Internal bridge / Overhang / Skirt | *(not overridden)* | base 0.0111 |

Controls:

- **Unset** → zero `SET_PRESSURE_ADVANCE` lines in the whole file. Emission is setting-driven.
- **`--filament-overhangs-pa 0.0999`** → 1203 occurrences, i.e. the un-overridden role above does
  pick up its own override when given one.
- **Firmware command form** is correct per flavor: klipper `SET_PRESSURE_ADVANCE ADVANCE=0.0111`,
  marlin2 `M900 K0.0111`, reprapfirmware `M572 D0 S0.0111`, and each flavor emits *only* its own
  form.
- **Per-role switching is not Klipper-only**: marlin2 with the same five overrides emitted all six
  distinct `M900 K` values (4075 lines). An earlier single-line result was simply a run where only
  the base was set, so there was nothing to switch.

### Speed settings - 2026-07-31, PASS (11 exact + overhangs)

**Use the slicer's own annotations, not arithmetic.** Every feedrate line names the setting that
produced it, and every extrusion line names its role:

```
G1 F771.788 ; external_perimeter_speed, reduced by small_perimeter_speed;
G1 X90.463 Y64.69 E1.73779 ; perimeter
```

That is a direct setting-to-output mapping and far stronger than dividing F by 60 and guessing.

Exact match between the named setting and the value it produced:

| Setting | Set | Produced |
|---|---|---|
| `perimeter_speed` | 11 | 11.0 |
| `external_perimeter_speed` | 12 | 12.0 |
| `bridge_speed` | 15 | 15.0 |
| `internal_bridge_speed` | 16 | 16.0 |
| `infill_speed` | 17 | 17.0 |
| `solid_infill_speed` | 18 | 18.0 |
| `top_solid_infill_speed` | 19 | 19.0 |
| `gap_fill_speed` | 21 | 21.0 |
| `thin_walls_speed` | 22 | 22.0 |
| `ironing_speed` | 23 | 23.0 |
| `brim_speed` | 26 | 26.0 |

`support_material_speed` (24) and `support_material_interface_speed` (25) produce exactly those
speeds, but the feedrate line reads `; previous speed` because F was already correct and not
re-emitted - match on the value, not the annotation, for those.

**Two settings are modulated by a second setting, and the annotation says so** - not defects:

- `small_perimeter_speed` **interpolates**, it does not replace: `perimeter_speed, reduced by
  small_perimeter_speed` yields 11.02-11.05 between perimeter (11) and small perimeter (13).
- `overhangs_speed` is blended by `overhangs_dynamic_speed`, which is **on by default**
  (`0:5:1:0x0:25x10:50x40:75x70:100x100`), so overhangs print near `perimeter_speed` at low overhang
  percentages - observed 11.3 with `overhangs_speed = 14`. Flatten the curve and it lands on exactly
  **14.0**. Any test of `overhangs_speed` must neutralise the dynamic curve first.

### Pressure advance, remaining per-role overrides - PASS

Attribute PA by the `;TYPE:` block it **precedes** - `SET_PRESSURE_ADVANCE` is emitted before the
marker for the block it applies to, ahead of the retract/travel. Verified exact for external
perimeter, perimeter, internal infill, solid infill, top solid infill, gap fill, ironing, support
material, support material interface, internal bridge infill, and brim.

**`filament_first_layer_pa` overrides every per-role PA on layer 1** - by design, stated at
`GCode.cpp:8047-8050` ("an explicit override (not a cap)"). So `filament_brim_pa` cannot show on the
brim itself, which is layer-1 only; it applies to skirt above layer 1, confirmed with
`--skirt-height 4` (L1 = 0.0266 first-layer, L2+ = 0.0255 brim). Not a defect - but worth knowing
before reporting one.

> **Sentinel values must differ across *neighbouring* roles too.** The first PA run looked clean only
> because the un-overridden roles all shared the base value; giving every role a distinct value is
> what exposed the ordering above.

### ✅ FIXED: `machine_klipper_pressure_advance` was a dead setting

Printer Settings exposes it (`Tab.cpp:3788`) and it is stored in the profile, but **no code ever
reads it**. Searched all of `src/libslic3r` and `src/slic3r`: the only hits are its declaration
(`PrintConfig.hpp:1106`), its definition (`PrintConfig.cpp:4737`), the preset list, and the UI line.
It reaches the G-code only as a config-trailer comment.

Its sibling `machine_klipper_pressure_advance_smooth_time` **is** read - `GCode.cpp:7684-7686` uses
it as the fallback when `filament_pressure_advance_smooth_time` is disabled, with the comment *"the
printer keeps whatever its own [extruder] section sets"*. The PA value has no such fallback.

That asymmetry has a consequence beyond the setting being inert. `_pressure_advance_acceleration`
(`:7704`) takes PA from `_compute_pressure_advance`, which returns 0 unless
`filament_pressure_advance.is_enabled()`, and then bails at `:7705`. So with autospeed PA headroom
enabled and filament PA disabled - i.e. the user relies on the printer's own `[extruder]
pressure_advance`, which is exactly what this setting records - the slicer reserves **no headroom at
all**, while the printer really is applying pressure advance. Either the setting should feed the
headroom calculation the way smooth time does, or it should not be offered.

**Fix applied 2026-07-31** (`GCode.cpp`, `_pressure_advance_acceleration`): when the emitted PA is 0
and the flavor is Klipper, the headroom solve falls back to `machine_klipper_pressure_advance`.

Deliberately placed in the *acceleration* path, **not** in `_compute_pressure_advance` - that
function's return value is what gets emitted, and the printer needs no `SET_PRESSURE_ADVANCE` for a
value its own `[extruder]` section already holds. This mirrors where the smooth-time fallback lives.

Verified with `--autospeed-pressure-advance-headroom`, filament PA disabled, profile acceleration
3000:

| `machine_klipper_pressure_advance` | `M204` commands | acceleration range | `SET_PRESSURE_ADVANCE` lines |
|---|---|---|---|
| 0 | 1 | flat 1500 | 0 |
| 0.08 | 46364 | 46 - 1500 | **0** |

The solver now reserves per-move headroom for the printer's own pressure advance, and the zero in
the last column is the guard: no PA commands were introduced into the output.

### Also verified 2026-07-31

| Setting | Result |
|---|---|
| `filament_travel_pa` | ✅ 0.03 emitted before travel, base 0.01 restored for extrusion |
| `travel_speed` / `travel_speed_z` | ✅ F9000 (150 mm/s) and F1200 (20 mm/s) |
| `filament_first_layer_pa_over_raft` | ✅ with `--raft-layers 3`: raft L1 = first-layer PA, first object layer over raft = 0.0288 |
| `filament_max_speed` | ✅ honoured - refuses the slice with an exact diagnostic naming the cap (`7 mm/s` → `0.956801 mm3/s`) when geometry cannot be printed within it |
| `tool_pressure_advance_mirrors` | ✅ a named mirror gets its own `SET_PRESSURE_ADVANCE … EXTRUDER=extruder_stepper1` alongside the main extruder |

**CLI caveat on mirrors:** the value is a per-extruder string list, so a comma in the CLI value is
consumed as the *per-tool* separator - `'a, b'` stored as `a;b`, meaning tool 0 → `a`, tool 1 → `b`,
not two mirrors for tool 0. `GCodeWriter.cpp:300` splits the same string on commas to find multiple
mirrors, so multi-mirror can only be exercised through the GUI field, not the CLI.

## Not yet verified

- [ ] `filament_pressure_advance_smooth_time` - Klipper `SMOOTH_TIME=`, clamped to 0.2 in
      `GCodeWriter.cpp:288`. Verify the clamp and that it only appears when enabled.
- [ ] `tool_pressure_advance_mirrors` - should repeat the command verbatim per named stepper.
- [ ] `filament_travel_pa` - applies on travel moves, suppressed above 2.0 on Klipper.
- [ ] Adaptive PA (`filament_adaptive_pressure_advance`) - interpolated per move; needs a different
      oracle than a fixed sentinel.
- [ ] `first_layer_speed` - **open question, not verified.** On layer 1 the observed speeds were
      brim 26, support 24, support interface 25, gap fill 21 - i.e. those roles kept their own
      speeds rather than taking `first_layer_speed` (27). Establish the intended precedence before
      calling it either way; `filament_first_layer_pa` *does* override per-role PA, so speed and PA
      may not follow the same rule.
- [ ] `small_perimeter_speed` - confirm the interpolation curve, not just that it moves the value.
- [ ] `travel_speed` / `travel_speed_z` - non-extruding moves, needs a different filter.
- [ ] `max_print_speed`, `max_volumetric_speed`, autospeed (speed = 0) - caps rather than settings.
- [ ] Temperature, acceleration and retraction settings - same method, different markers.
