# Adaptive Pressure Advance — standalone test harness

Fast, dependency-free unit tests for `AdaptivePAModel`
(`src/libslic3r/GCode/AdaptivePressureAdvance.{hpp,cpp}`, issue #39).

The evaluator only uses the C++ standard library, so this harness compiles **just**
`AdaptivePressureAdvance.cpp` + `run_tests.cpp` — no libslic3r, no CMake reconfigure,
builds and runs in a couple of seconds.

## Run

```
tests\adaptive_pa\build.bat
```

It sets up the MSVC environment (VS 2026 / VS 18 `vcvars64.bat`), compiles
`run_tests.exe`, runs it, and exits non-zero if any check fails.

## Add tests

In `run_tests.cpp`, write another `test_*()` function and call it from `main()`.
Assertions:

- `CHECK(cond)` — boolean condition.
- `CHECK_NEAR(got, want)` — floating-point compare (1e-6 tolerance).

## Coverage today

Grid-point recovery, linear interpolation across flow and acceleration, full bilinear
interpolation, edge clamping (both axes), result rounding, and the degenerate cases
(empty model → caller fallback, single point, single acceleration level).

## Note

There is also a Catch2 version at `tests/libslic3r/test_adaptive_pressure_advance.cpp`
wired into the in-solution `libslic3r_tests` target, for when the main build is
configured with tests enabled. This standalone harness is the quick path for iterating
on the evaluator without rebuilding the solution.
