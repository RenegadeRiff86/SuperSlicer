# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

SuperSlicer is an open-source 3D slicer (STL/3MF → G-code) forked from PrusaSlicer, built in C++17 with wxWidgets GUI and CMake. Version 2.7.62-beta2. Licensed AGPLv3.

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

- C++17, `#pragma once` for headers
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
