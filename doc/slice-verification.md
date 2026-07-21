# Verifying that a libslic3r change is output-neutral

Most work in `libslic3r` is meant to change *nothing* the printer sees: refactors,
warning cleanups, extractions. The way to prove that is to slice the same model
before and after and diff the G-code.

That sounds trivial. It is not, and it fails **silently** in three different ways.
This document and `scripts/slice_verify.py` exist so nobody has to rediscover them.

---

## TL;DR

```
python scripts/slice_verify.py models                    # what to slice, and why
python scripts/slice_verify.py slice --model wedge --out before.gcode
#  ... rebuild with your change ...
python scripts/slice_verify.py slice --model wedge --out after.gcode
python scripts/slice_verify.py compare before.gcode after.gcode --require gapfill
```

`--require` is the important part. It makes the comparison fail if the feature you
changed is absent from the output, which is the difference between a real pass and
a meaningless one.

---

## Failure mode 1: the code under test never ran

**The stock profile turns off most of the interesting code.** Slice without the
right switch and you get a clean "identical" result that proves nothing.

| Gate | CLI option | Profile default | What silently does not run |
|---|---|---|---|
| `arcs` | `--arc-fitting=emit_center` | `arc_fitting = disabled` | all of `Geometry/ArcWelder.cpp` |
| `classic` | `--perimeter-generator=classic` | `perimeter_generator = arachne` | `Geometry/MedialAxis.cpp` — thin walls and gap fill |

Measured, same model and build:

* arachne → **0** `Gap fill` blocks; classic → **9**. On `Support_test` it is 1 → 80.
* `arc_fitting=disabled` → **0** `G2`/`G3` moves anywhere in the file.

Arachne generates variable-width perimeters itself via `SkeletalTrapezoidation`, so
it bypasses `MedialAxis` almost entirely. Note `--arc-fitting=enabled` is **not**
a valid value: it is `disabled` / `bambu` / `emit_center`, and `emit_center` is the
ArcWelder path (`ArcFittingType::ArcWelder`).

**Rule: find the switch that gates the code you touched, force it on, and confirm
the feature's fingerprint appears in the output before believing any result.**
`slice` warns when a requested gate produces nothing; `compare --require` fails.

## Failure mode 2: the model is not reproducible

Some models differ between two runs of the *same binary*, so any diff against them
is noise. Run `slice_verify.py models` for the current list. In short:

| Model | Reproducible | Use for |
|---|---|---|
| `wedge` | yes | thin walls / gap fill / variable width — best `MedialAxis` target |
| `support` | yes | supports, gap fill, arcs — safest general target |
| `overhang` | almost | overhangs; one known flaky `E` digit (see below) |
| `handle` | no | curved surfaces; support island **order** varies per run |
| `cube` | no | do not use as a diff target |

The calibration cube is the worst offender: three runs of one build produced
481836 / 481824 / 481800 lines with arc fitting on, and two layers are bistable on
last-bit float rounding. `overhang test` is reproducible except one line (~44093)
where the 5th decimal of an `E` value flips; a lone 1-digit `E` delta there is
noise — confirm by re-running the same build before investigating.

## Failure mode 3: comparing against the GUI

The CLI and the GUI **use the same slicing engine and the same options**. This was
verified directly, not assumed:

* Both build config from `PresetBundle::full_config()` — GUI `Plater.cpp`, CLI
  `PrusaSlicer.cpp`. A GUI export and a CLI run agree on **632/632 config keys**.
* Both call `Print::apply(model, config)` then `Print::process()` then
  `export_gcode` — GUI `BackgroundSlicingProcess.cpp:613`, CLI `PrusaSlicer.cpp:1237`.
  There is no separate "CLI slicer".
* A GUI export and a CLI run of the same model produced **573/573 identical moves,
  with identical command types and identical `E` values**.

The only difference is **where the part sits on the bed**: the CLI arranges it, and
its arrange lands a constant `(+0.116, +0.238) mm` away from where the GUI put it.
Every object move carries exactly that offset. That is why a naive byte diff of a
CLI run against a GUI export never matches — every X/Y is off by a fraction of a
millimetre — and it is almost certainly the origin of any past distrust of the CLI.

`compare` recognises this and reports **SAME TOOLPATH, TRANSLATED**, which is a pass.

If you need coordinate-for-coordinate equality with a GUI export, derive the centre
empirically — slice once, read the reported `dx`/`dy`, then re-slice with
`--center`. Solving it that way once got the two within **1 micron**. Do not guess:
the CLI's default arrange centre is *not* the bed centre, and `--dont-arrange` is
*not* the GUI-matching mode (it uses the raw STL coordinates, far from where the
GUI places the part). `autocenter = 0` in `SuperSlicer.ini` does not mean the GUI
leaves a loaded model at its file coordinates.

---

## Reading `compare`

| Verdict | Meaning | Exit |
|---|---|---|
| `settings: identical` | the two runs used the same config. Always check this first — a settings difference explains any geometry difference and nothing below it is meaningful. | — |
| `IDENTICAL` | same output, ignoring timestamps and time estimates. | 0 |
| `SAME TOOLPATH, TRANSLATED` | identical moves at a constant offset — same print, different bed position. | 0 |
| `DIFFERENT` | real difference; the first differing lines are printed. | 1 |
| `VACUOUS COMPARISON` | a `--require`d feature is missing, so the code under test never ran. **The result proves nothing.** | 1 |

Volatile lines (`; generated by`, `M73`, `M117`, `;TIME`, `; estimated`,
`; total filament`) are excluded automatically — they carry timestamps and time
estimates that differ between runs of identical code. An inserted `M73` also shifts
index-based alignment, so never compare two G-code files with a naive line zip.

---

## Full workflow for an A/B against a previous commit

```bat
REM 1. baseline: restore the pre-change file(s), build, slice
git show <ref>~1:src/libslic3r/Geometry/MedialAxis.cpp > %TEMP%\pre.cpp
copy %TEMP%\pre.cpp src\libslic3r\Geometry\MedialAxis.cpp
REM    (build the solution)
python scripts/slice_verify.py slice --model wedge --out before.gcode

REM 2. candidate: restore your work, build, slice
git checkout -- src/libslic3r/Geometry/MedialAxis.cpp
REM    (build the solution)
python scripts/slice_verify.py slice --model wedge --out after.gcode

REM 3. compare
python scripts/slice_verify.py compare before.gcode after.gcode --require gapfill
```

Use `<ref>~1`, **not** `<ref>^` — under `cmd` the `^` is an escape character and
silently resolves to `<ref>` itself, giving you a "baseline" that already contains
the change you are testing. Verify the exported file does not contain your new
symbols before building it.

An incremental solution build after a single `.cpp` edit is about 70 seconds, so a
full build → slice → compare cycle costs roughly three minutes. There is no
legitimate "too slow" argument for skipping this.

---

## GUI slicing

When the question is *how the slicer behaves* rather than *did my refactor change
anything* — supports, overhangs, anything you would look at — slice in the GUI and
export from the plater. The CLI is for controlled A/B comparisons, which the GUI
cannot do because it cannot run two binaries.

To drive the GUI: `set_startup_project Slic3r_app_gui`, `set_debug_arguments` with
the model path, `debug_start`, then `tools/scripts/ss_drive.py`.

**Known bug:** `ss_drive.py --export` does not honour its path argument. The GUI
writes to its own default instead —
`build-default/src/<config>/resources/shapes/<model>_<profile>.gcode`. The script
races the native Save dialog; it needs to wait for that window handle to *appear*,
activate it, act, and then wait for the handle to be *gone* before continuing.
Until that is fixed, look for the file at the default path.
