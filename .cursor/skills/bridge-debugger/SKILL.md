---
name: bridge-debugger
description: >-
  SuperSlicer launch rules for the IDE bridge debugger. Use together with the
  bridge debugger skill when debugging this repo: which binary to launch, the
  single-process guard, and how to condition layer breakpoints.
---

# SuperSlicer debugger

The bridge debugger skill covers breakpoints, threads, stepping, and the Debug Console. These rules apply only in this repo.

## One session

`diagnostics_snapshot` says whether a debugger is running. A live SuperSlicer process has comm `slic3r_main` (zombies do not count).

Every launch configuration runs `.vscode/refuse-if-superslicer-running.sh` before the build. If that process is live, the launch fails. Do not call `debug_start` again. `debug_stop` the session that is already open, wait until the guard exits 0, then start one session.

## Which binary

`debug_start` launches the configuration selected in the Run view, not whichever configuration last received `set_debug_arguments`.

Step on the unoptimized binary (`build-linux-x86_64-debug`, `-O0`). Optimized (`-O2`) jumps over function bodies and drops locals, so a condition on `params` can fail and the breakpoint stops on the wrong layer.

The unoptimized binary asserts on some models (Clipper contours, and `MedialAxis.cpp` variable-width). For those slices, use the optimized binary to confirm the gcode, and keep the unoptimized session for the lines that still have locals.

`set_debug_arguments` writes `launch.json`. After the process is running, clear those args so the next F5 is a normal GUI launch.

## Layers

Function names include the namespace and the class when they share a name. `create_overhangs_classic` is `Slic3r::PerimeterGenerator::PerimeterGenerator::create_overhangs_classic`. A shorter name does not bind.

Layers are sliced on parallel threads. Condition the breakpoint on the layer (`params.layer->print_z > 1.25 && params.layer->print_z < 1.35`). `debug_watch` accepts field access (`params.layer->print_z`, `paths[0].m_attributes.role`). Container size comes back from the value summary (`size=0`).
