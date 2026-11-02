# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

SuperSlicer is an open-source 3D slicer (STL/3MF → G-code) forked from PrusaSlicer, built in C++20 with wxWidgets GUI and CMake. Version 2.7.63-fork. Licensed AGPLv3.

## Build Commands (Windows / VS2022)

**Prerequisites**: Visual Studio 2022, CMake 3.13+, Git

**Step 1 — Build dependencies** (MSVC x64 Native Tools Command Prompt, only needed once):
```
cd deps && mkdir build && cd build
cmake .. -G "Visual Studio 17 2022"
cmake --build . --config RelWithDebInfo
```

**Step 2 — Generate solution**:
```
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022" -DCMAKE_PREFIX_PATH="deps\build\destdir\usr\local" -DCGAL_WITH_GMPXX=OFF
```
`-DCGAL_WITH_GMPXX=OFF` is required on Windows (GMPXX not available).

**Step 3 — Build & run**:
1. Open `build\Slic3r.sln`
2. Set `slic3r_app_gui` as Startup Project
3. Build (F7 for incremental, Rebuild for clean)
4. F5 to debug

**Command-line build** (after cmake generate):
```
cmake --build build --config RelWithDebInfo --target slic3r_app_gui
```

## Running Tests

Tests use Catch2. Test executables are built as part of the solution. After building:
```
cd build
ctest -C RelWithDebInfo                          # run all tests
ctest -C RelWithDebInfo -R fff_print             # run one test suite
ctest -C RelWithDebInfo -R libslic3r             # run another suite
```

Or run individual test executables directly for Catch2 filtering:
```
build\tests\fff_print\RelWithDebInfo\fff_print_tests.exe "[cooling]"   # run tests matching tag
build\tests\fff_print\RelWithDebInfo\fff_print_tests.exe "test name"   # run specific test
```

**Test suites**: `libslic3r_tests`, `superslicerlibslic3r_tests`, `fff_print_tests`, `sla_print_tests`, `arrange_tests`, `thumbnails_tests`, `slic3rutils_tests` (GUI only)

## Architecture

### Slicing Pipeline

```
Model (STL/3MF)
  → Print::apply(model, config)          # bind model + config, invalidate changed steps
  → Print::process()                     # orchestrate all steps
    → PrintObject::slice()               # mesh → 2D ExPolygons per layer
    → PrintObject::make_perimeters()     # PerimeterGenerator → wall paths
    → PrintObject::prepare_infill()      # classify surfaces (top/bottom/internal/bridge)
    → PrintObject::infill()              # Fill patterns → infill paths
    → PrintObject::generate_support_material()
  → Print::export_gcode()
    → GCodeGenerator::do_export()        # layers → G-code text
      → CoolingBuffer                    # enforce min layer time, fan control
      → SpiralVase                       # spiral/vase mode post-process
      → PressureEqualizer               # PA smoothing
      → GCodeProcessor                  # analyze for preview
```

**GUI triggers slicing** via `BackgroundSlicingProcess` (src/slic3r/GUI/), which runs `Print::process()` + `export_gcode()` on a background thread and posts wxWidgets events for progress/completion.

**Step invalidation**: When config changes, `Print::invalidate_state_by_config_options()` marks only affected steps dirty. Unchanged steps are not re-run (e.g., changing extrusion multiplier skips re-slicing the mesh).

### Configuration System

Two tiers:
- **StaticPrintConfig** (`PrintConfig`, `PrintObjectConfig`, `PrintRegionConfig`): compile-time members, used during slicing. Access: `config.first_layer_height`.
- **DynamicPrintConfig**: runtime key-value map, used by GUI/file I/O/PlaceholderParser. Access: `config.option("first_layer_height")`.

All config options are declared in `src/libslic3r/PrintConfig.cpp` (look for `optgroup` definitions). Hierarchy: `PrintConfig` (printer/bed) → `PrintObjectConfig` (per-object) → `PrintRegionConfig` (per-region infill/perimeters/speed).

**PlaceholderParser** (`src/libslic3r/PlaceholderParser.cpp`): expands custom G-code templates with `{variable}` syntax. All config options + special vars (date, layer_num, etc.) available.

### Core Source Layout

- **src/libslic3r/** — Platform-independent slicing engine
  - `Print.cpp/hpp` — Orchestrator, owns PrintObjects and PrintRegions
  - `PrintObject.cpp` — Per-object step execution (slice, perimeters, infill, support)
  - `PrintConfig.cpp/hpp` — All setting definitions
  - `GCode.cpp/hpp` — G-code generation (`_compute_speed_mm_per_sec()` for speed/volumetric logic)
  - `GCode/` — Post-processors: CoolingBuffer, SpiralVase, WipeTower, SeamPlacer, PressureEqualizer, FanMover
  - `Fill/` — Infill patterns (each pattern is a FillBase subclass)
  - `PerimeterGenerator.cpp` — Wall/perimeter generation, overhang detection
  - `Format/` — File I/O (3MF, AMF, STL, OBJ)
  - `SLA/` — SLA print processing
- **src/slic3r/GUI/** — wxWidgets GUI (Plater, tabs, 3D canvas, preview)
  - `BackgroundSlicingProcess.cpp` — Thread boundary between GUI and engine
  - `ConfigManipulation.cpp` — Config validation and auto-corrections in UI
- **src/angelscript/** — Scripting engine for custom G-code processing
- **src/PrusaSlicer.cpp** — Application entry point

### Key Data Types

- **ExPolygon**: polygon with holes (main 2D geometry type)
- **ExtrusionEntity** hierarchy: `ExtrusionPath`, `ExtrusionLoop`, `ExtrusionMultiPath`, `ExtrusionEntityCollection` — all toolpath types
- **Layer** / **LayerRegion**: per-Z-height slice data, surfaces, perimeters, fills
- **coord_t**: scaled integer (1 unit = 0.000001 mm) used internally to avoid FP errors; converted to float on G-code export

### External Dependencies

Boost 1.83, CGAL, Clipper, Eigen, libigl, TBB, wxWidgets, OpenGL, GLEW, CURL, NLopt, OpenVDB

## Code Style

- C++20, `#pragma once` for headers
- PascalCase for classes, snake_case for functions/variables

## Git

**Identity** — No global git config. Use:
```
git -c user.name="Stan-Elston" -c user.email="elston86@hotmail.com" commit ...
```

**Remotes**:
- `origin` — https://github.com/Stan-Elston/SuperSlicer.git (fork)
- `upstream` — https://github.com/supermerill/SuperSlicer.git (original)

## Known Build Issues

**CGAL GMPXX error**: `target "CGAL" contains GMPXX::libgmpxx but the target was not found`
→ Fix: `-DCGAL_WITH_GMPXX=OFF` in cmake configure (already in Step 2 above).

## VS IDE Bridge (MCP)

The user runs a custom VS IDE Bridge MCP server that connects Claude Code to the live Visual Studio instance. **Always prefer bridge tools over standard file tools when the bridge is available.**

### Workflow

1. **Start with `diagnostics_snapshot`** — returns errors + warnings + messages (linter) in one call. The `messages` tier contains `lnt-*` linter findings that are the most precision-relevant; never skip it.
2. **Use `apply_diff`** to make code changes — diffs apply live into the VS editor and are immediately visible. Do not use `Edit`/`Write` for files already open in the solution.
3. **Use bridge search tools** instead of Grep/Glob: `find_text` for content search, `search_symbols` for symbols, `find_files` for file discovery.
4. **Use `read_file`** (bridge) instead of `Read` for files in the solution — it reveals the file in the editor and shows line numbers in context.
5. **Use `build` / `build_errors`** to compile and get build diagnostics without leaving the bridge.

### Key tools
| Tool | Purpose |
|------|---------|
| `diagnostics_snapshot` | All errors + warnings + linter messages in one snapshot |
| `apply_diff` | Apply unified diff live into VS editor |
| `find_text` | Full-solution text/regex search |
| `search_symbols` | Symbol definition search |
| `read_file` | Read file slice, reveals in editor |
| `build` / `build_errors` | Build solution, return errors |
| `errors` / `warnings` | Focused error/warning queries |
| `file_outline` | Class/function outline for a file |
| `goto_definition` / `find_references` | Navigation |
| `help` | Full catalog of all 74 bridge tools |

### Important notes
- **Bridge path**: `apply_diff` resolves paths relative to `build\Slic3r.slnx`. Edits land in `build\src\...` (the build directory copy), not `src\`. Changes made this way need to be propagated back to `src\` if you want them committed.
- **`diagnostics_snapshot` messages tier**: Contains `lnt-arithmetic-overflow`, `lnt-integer-float-division`, `lnt-accidental-copy`, `VCR003` etc. These are VS linter findings, not compiler errors — but in a slicer using `coord_t` (scaled int32), they represent real precision bugs.
- **User owns the bridge**: The bridge is a custom tool owned by the user. If behavior seems wrong or a tool is missing, the user can add/modify bridge tools.
- **`BP1006` false positive**: The "best-practice" extension flags the word "delete" in comments as raw C++ `delete`. These are false positives — no code change needed.
