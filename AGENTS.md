# SuperSlicer

Open-source slicer (STL/3MF to G-code), forked from PrusaSlicer. C++20, wxWidgets, CMake. Version 2.7.63-fork. AGPLv3.

This checkout is built on Linux. The debug tree is `build-linux-x86_64-debug` (Unix Makefiles). The release tree is `build-linux-x86_64-release`. Clangd reads the debug compilation database.

Incremental debug build, from the repo root. The person working in this tree runs it; do not start it unless asked:

```
/usr/bin/cmake --build build-linux-x86_64-debug --target Slic3r --parallel 4
```

`Slic3r` is the GUI target. It also copies `src/resources` into the build and refreshes the `superslicer-gcodeviewer` symlink. Tests are Catch2 and are off in both caches (`SLIC3R_BUILD_TESTS=OFF`). Leave them off unless asked.

## Automation

Drive the running GUI from `tests/automation/`. Extend that package instead of writing a one-off script. The MCP server is `tests/automation/mcp_server.py`. It stays off until automation debug mode is turned on:

```
python3 tests/automation/gui_drive.py automation-debug on
python3 tests/automation/gui_drive.py automation-debug off
```

The slicer listens only when it was started with `--automation-api`. That is the "Debug automation" launch configuration, not a normal F5.

## Layout

`src/libslic3r/` is the slicing engine (`Print`, `PrintObject`, `PrintConfig`, `GCode`). `src/slic3r/GUI/` is the wxWidgets interface. `BackgroundSlicingProcess` is the thread boundary. Config options are declared in `src/libslic3r/PrintConfig.cpp`. Internal coordinates are `coord_t` (1 unit = 0.000001 mm).

Verification models live in `models/` and are described in `models/README.md`. Repeatable G-code diffs use `scripts/slice_verify.py`.

## Git

No global git config. When a commit is requested:

```
git -c user.name="Stan-Elston" -c user.email="elston86@hotmail.com" commit ...
```

`origin` is the fork. `upstream` is https://github.com/supermerill/SuperSlicer.git.

## Editor warnings

Do not mass-fix BP1012, BP1013, BP1015, or BP1002. BP1006 on the word "delete" in a comment is a false positive. Do not delete `#if 0` blocks or other intentionally disabled algorithms.
