#!/usr/bin/env python3
"""Reproducible SuperSlicer CLI slicing, and before/after G-code comparison.

WHY THIS EXISTS
---------------
Proving a libslic3r change is output-neutral means slicing the same model before
and after and diffing the G-code. That is easy to get *silently* wrong:

  * Most interesting code is gated OFF by the default profile. Slice without the
    right switch and you get a confident "identical" result that proves nothing,
    because the code under test never ran. This has happened twice.
  * Some models are not reproducible run to run, so a diff against them is noise.
  * The CLI arranges the model differently from the GUI, so a raw diff of a CLI
    run against a GUI export never matches even when the toolpath is identical.

Everything known about those traps is encoded here so nobody has to remember it.
See doc/slice-verification.md for the reasoning and the measurements behind it.

QUICK START
-----------
    python scripts/slice_verify.py models
    python scripts/slice_verify.py slice --model wedge --out before.gcode
    # ... rebuild with your change ...
    python scripts/slice_verify.py slice --model wedge --out after.gcode
    python scripts/slice_verify.py compare before.gcode after.gcode --require gapfill

`--require` is the guard against a vacuous pass: compare exits non-zero if the
named feature is absent from the G-code, i.e. if the code you changed never ran.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable, Sequence

REPO_ROOT = Path(__file__).resolve().parent.parent


# ---------------------------------------------------------------------------
# Feature gates: which profile switch makes a given source file actually run.
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class FeatureGate:
    """A profile setting that must be forced on for some code to execute."""

    name: str
    option: str            # CLI option, e.g. "--arc-fitting=emit_center"
    default_state: str     # what the stock profile does
    gates: str             # what silently does not run without it
    fingerprint: str       # key into FINGERPRINTS proving it really ran


FEATURE_GATES: dict[str, FeatureGate] = {
    "arcs": FeatureGate(
        name="arcs",
        option="--arc-fitting=emit_center",
        default_state="arc_fitting = disabled",
        gates="src/libslic3r/Geometry/ArcWelder.cpp (all arc fitting)",
        fingerprint="arcs",
    ),
    "classic": FeatureGate(
        name="classic",
        option="--perimeter-generator=classic",
        default_state="perimeter_generator = arachne",
        gates="src/libslic3r/Geometry/MedialAxis.cpp (thin walls, gap fill); "
              "Arachne does variable-width perimeters itself and bypasses it",
        fingerprint="gapfill",
    ),
}

# How to prove, from the G-code alone, that a feature actually executed.
FINGERPRINTS = {
    "arcs": lambda g: g.arc_count,
    "gapfill": lambda g: g.feature_blocks.get("Gap fill", 0),
    "support": lambda g: g.feature_blocks.get("Support material", 0),
    "thinwall": lambda g: g.feature_blocks.get("Thin wall", 0),
    "overhang": lambda g: g.feature_blocks.get("Overhang perimeter", 0),
}


# ---------------------------------------------------------------------------
# Test models
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class TestModel:
    key: str
    filename: str
    deterministic: bool
    exercises: str
    notes: str = ""
    needs_supports: bool = False

    @property
    def path(self) -> Path:
        return REPO_ROOT / "build-default" / "Test Models" / self.filename


MODELS: dict[str, TestModel] = {
    "wedge": TestModel(
        key="wedge",
        filename="wedge_wall_arachne.stl",
        deterministic=True,
        exercises="thin walls / gap fill / variable width (MedialAxis)",
        notes="100mm x 2mm wall tapering 2mm -> 0mm. Best MedialAxis target: it "
              "sweeps the whole width range in one model. Needs --perimeter-generator=classic.",
    ),
    "support": TestModel(
        key="support",
        filename="Support_test.stl",
        deterministic=True,
        exercises="supports, gap fill, arcs",
        notes="Byte-reproducible. The safest general before/after target.",
        needs_supports=True,
    ),
    "overhang": TestModel(
        key="overhang",
        filename="overhang test.stl",
        deterministic=False,
        exercises="overhangs, 244 layers",
        notes="Reproducible EXCEPT one known flaky line (~44093): the 5th decimal "
              "of an E value flips between runs of the same build. A lone 1-digit "
              "E delta there is noise -- confirm by re-running the same build.",
    ),
    "handle": TestModel(
        key="handle",
        filename="handle test.stl",
        deterministic=False,
        exercises="curved surfaces, supports, arcs",
        notes="Support island ORDER varies run to run, which cascades into the "
              "whole file. Use only with compare's translation/ordering awareness.",
        needs_supports=True,
    ),
    "cube": TestModel(
        key="cube",
        filename="All in 1 Calibration Cube 40x40x40 V1.3.stl",
        deterministic=False,
        exercises="everything (perimeters, infill, bridges, gap fill)",
        notes="DO NOT use as a diff target. Three runs of one build gave 481836 / "
              "481824 / 481800 lines with arc fitting on. Two layers are bistable "
              "on last-bit float rounding.",
    ),
}


# ---------------------------------------------------------------------------
# Slice options
# ---------------------------------------------------------------------------

@dataclass
class SliceOptions:
    """Everything needed to drive the CLI, with the traps pre-answered.

    Defaults are chosen so that the two files most refactor work touches
    (MedialAxis.cpp and ArcWelder.cpp) actually execute.
    """

    load_user_profiles: bool = True
    gates: Sequence[str] = ("classic", "arcs")
    support_material: bool | None = None   # None = follow the model's default
    center: tuple[float, float] | None = None
    slice_report: Path | None = None
    extra: list[str] = field(default_factory=list)

    def to_args(self, model: TestModel) -> list[str]:
        args: list[str] = []
        if self.load_user_profiles:
            # Reproduces the user's currently-selected preset set -- the same
            # PresetBundle::full_config() the GUI slices with (632/632 keys match).
            args.append("--load-user-profiles")
        for key in self.gates:
            args.append(FEATURE_GATES[key].option)
        want_supports = model.needs_supports if self.support_material is None else self.support_material
        if want_supports:
            args.append("--support-material")
        if self.center is not None:
            args.append("--center={0},{1}".format(*self.center))
        if self.slice_report is not None:
            args += ["--slice-report", str(self.slice_report)]
        args += self.extra
        return args


# ---------------------------------------------------------------------------
# The slicer
# ---------------------------------------------------------------------------

class Slicer:
    """Locates and runs superslicer_console.exe."""

    def __init__(self, repo_root: Path = REPO_ROOT, config: str = "RelWithDebInfo"):
        self.repo_root = Path(repo_root)
        self.config = config

    @property
    def exe(self) -> Path:
        # NOTE: the copy in build-default/Test Models is a launcher that fails
        # with "Slic3r.dll was not loaded" -- always use the build output.
        exe = self.repo_root / "build-default" / "src" / self.config / "superslicer_console.exe"
        if not exe.exists():
            raise FileNotFoundError(
                "slicer not found at {0}\nBuild the solution first, or pass "
                "--config Debug/Release.".format(exe))
        return exe

    def slice(self, model: TestModel, out: Path, options: SliceOptions | None = None,
              timeout: int = 900) -> "GCode":
        options = options or SliceOptions()
        if not model.path.exists():
            raise FileNotFoundError("model not found: {0}".format(model.path))
        out = Path(out)
        out.parent.mkdir(parents=True, exist_ok=True)
        cmd = [str(self.exe)] + options.to_args(model) + [
            "-g", str(model.path), "-o", str(out)]
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        if proc.returncode != 0 or not out.exists():
            raise RuntimeError("slice failed (exit {0})\n{1}\n{2}".format(
                proc.returncode, proc.stdout[-2000:], proc.stderr[-2000:]))
        gcode = GCode.from_file(out)
        # Fail loudly if a requested gate did not actually produce its feature.
        for key in options.gates:
            gate = FEATURE_GATES[key]
            if FINGERPRINTS[gate.fingerprint](gcode) == 0:
                print("WARNING: gate '{0}' was requested but no '{1}' appears in the "
                      "output -- {2} may not have executed for this model.".format(
                          key, gate.fingerprint, gate.gates), file=sys.stderr)
        return gcode


# ---------------------------------------------------------------------------
# G-code
# ---------------------------------------------------------------------------

# Lines that legitimately differ between two runs of identical code.
_VOLATILE = ("; generated by", "M73", "M117", ";TIME", "; estimated", "; total filament")
_MOVE = re.compile(r"^G[123] ")
_X = re.compile(r" X(-?[\d.]+)")
_Y = re.compile(r" Y(-?[\d.]+)")
_E = re.compile(r" E(-?[\d.]+)")
_CFG = re.compile(r"^; ([a-z0-9_]+) = (.*)$")


@dataclass
class Move:
    cmd: str
    x: float | None
    y: float | None
    e: float | None
    comment: str


class GCode:
    """A parsed G-code file, with the volatile parts identified."""

    def __init__(self, path: Path, lines: list[str]):
        self.path = Path(path)
        self.lines = lines

    @classmethod
    def from_file(cls, path: Path) -> "GCode":
        text = Path(path).read_text(encoding="utf-8", errors="replace")
        return cls(path, text.split("\n"))

    @property
    def stable_lines(self) -> list[str]:
        """Lines that must match between two runs of identical code."""
        return [l for l in self.lines if not l.startswith(_VOLATILE)]

    @property
    def config(self) -> dict[str, str]:
        """The `; key = value` footer SuperSlicer appends (~632 keys)."""
        out: dict[str, str] = {}
        for line in self.lines:
            m = _CFG.match(line)
            if m:
                out[m.group(1)] = m.group(2)
        return out

    @property
    def moves(self) -> list[Move]:
        out: list[Move] = []
        for line in self.lines:
            if not _MOVE.match(line):
                continue
            mx, my, me = _X.search(line), _Y.search(line), _E.search(line)
            comment = line.split(";", 1)[1].strip() if ";" in line else ""
            out.append(Move(line[:2],
                            float(mx.group(1)) if mx else None,
                            float(my.group(1)) if my else None,
                            float(me.group(1)) if me else None,
                            comment))
        return out

    @property
    def arc_count(self) -> int:
        return sum(1 for l in self.lines if l.startswith(("G2 ", "G3 ")))

    @property
    def feature_blocks(self) -> dict[str, int]:
        c: Counter = Counter()
        for line in self.lines:
            if ";TYPE:" in line:
                c[line.split(";TYPE:", 1)[1].strip()] += 1
        return dict(c)

    def summary(self) -> str:
        feats = ", ".join("{0}={1}".format(k, v) for k, v in sorted(self.feature_blocks.items()))
        return "{0}: {1} lines, {2} moves, {3} arcs\n  features: {4}".format(
            self.path.name, len(self.lines), len(self.moves), self.arc_count, feats or "none")


# ---------------------------------------------------------------------------
# Comparison
# ---------------------------------------------------------------------------

@dataclass
class Comparison:
    identical: bool
    config_diffs: list[str]
    line_count: tuple[int, int]
    move_count: tuple[int, int]
    offsets: dict[tuple[float, float], int]
    first_diffs: list[tuple[str, str]]
    missing_features: list[str]

    @property
    def translated_only(self) -> bool:
        """True when the two files differ only by a constant XY translation.

        A constant offset means the same toolpath placed elsewhere on the bed --
        which is exactly how a CLI run differs from a GUI export.
        """
        real = {k for k in self.offsets if k != (0.0, 0.0)}
        if not real or len(self.move_count) != 2 or self.move_count[0] != self.move_count[1]:
            return False
        xs = {round(k[0], 2) for k in real}
        ys = {round(k[1], 2) for k in real}
        return len(xs) == 1 and len(ys) == 1

    @property
    def ok(self) -> bool:
        return self.identical and not self.missing_features

    def report(self) -> str:
        out = []
        if self.config_diffs:
            out.append("SETTINGS DIFFER ({0}) -- fix this before reading anything else:".format(
                len(self.config_diffs)))
            out += ["    " + d for d in self.config_diffs[:20]]
        else:
            out.append("settings: identical")
        out.append("lines: {0} vs {1}   moves: {2} vs {3}".format(*self.line_count, *self.move_count))
        if self.identical:
            out.append("RESULT: IDENTICAL (ignoring timestamps and time estimates)")
        elif self.translated_only:
            dx, dy = next(iter(k for k in self.offsets if k != (0.0, 0.0)))
            out.append("RESULT: SAME TOOLPATH, TRANSLATED by dx={0:+.3f} dy={1:+.3f} mm".format(dx, dy))
            out.append("   (a constant offset = same print, different bed position)")
        else:
            out.append("RESULT: DIFFERENT")
            for a, b in self.first_diffs[:8]:
                out.append("   - {0}".format(a[:100]))
                out.append("   + {0}".format(b[:100]))
        if self.missing_features:
            out.append("")
            out.append("!! VACUOUS COMPARISON: required feature(s) absent: {0}".format(
                ", ".join(self.missing_features)))
            out.append("   The code under test did not run. This result proves nothing.")
        return "\n".join(out)


def compare(a: GCode, b: GCode, require: Iterable[str] = ()) -> Comparison:
    missing = [f for f in require if FINGERPRINTS[f](a) == 0 or FINGERPRINTS[f](b) == 0]

    ca, cb = a.config, b.config
    cfg_diffs = ["{0}: {1!r} vs {2!r}".format(k, ca.get(k), cb.get(k))
                 for k in sorted(set(ca) | set(cb)) if ca.get(k) != cb.get(k)]

    sa, sb = a.stable_lines, b.stable_lines
    identical = sa == sb

    offsets: Counter = Counter()
    for ma, mb in zip(a.moves, b.moves):
        if None in (ma.x, mb.x, ma.y, mb.y):
            continue
        offsets[(round(ma.x - mb.x, 3), round(ma.y - mb.y, 3))] += 1

    first = [(x, y) for x, y in zip(sa, sb) if x != y][:8]
    return Comparison(
        identical=identical,
        config_diffs=cfg_diffs,
        line_count=(len(sa), len(sb)),
        move_count=(len(a.moves), len(b.moves)),
        offsets=dict(offsets),
        first_diffs=first,
        missing_features=missing,
    )


# ---------------------------------------------------------------------------
# Command line
# ---------------------------------------------------------------------------

def _cmd_models(args) -> int:
    for m in MODELS.values():
        mark = "deterministic" if m.deterministic else "NOT deterministic"
        print("{0:9s} {1:45s} [{2}]".format(m.key, m.filename, mark))
        print("          exercises: {0}".format(m.exercises))
        if m.notes:
            print("          {0}".format(m.notes))
        print("          {0}".format("present" if m.path.exists() else "MISSING from disk"))
    print("\nFeature gates (off by default -- without these the code does not run):")
    for g in FEATURE_GATES.values():
        print("  {0:9s} {1:34s} default: {2}".format(g.name, g.option, g.default_state))
        print("            gates {0}".format(g.gates))
    return 0


def _cmd_slice(args) -> int:
    model = MODELS[args.model]
    opts = SliceOptions(gates=tuple(args.gates), slice_report=args.slice_report)
    if args.center:
        opts.center = tuple(float(v) for v in args.center.split(","))
    gcode = Slicer(config=args.config).slice(model, Path(args.out), opts)
    print(gcode.summary())
    if not model.deterministic:
        print("\nNOTE: {0} is not reproducible run to run -- see `models` for what "
              "varies before treating any diff as real.".format(model.key))
    return 0


def _cmd_compare(args) -> int:
    result = compare(GCode.from_file(args.before), GCode.from_file(args.after),
                     require=args.require)
    print(result.report())
    return 0 if result.ok or result.translated_only else 1


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    m = sub.add_parser("models", help="list test models and feature gates")
    m.set_defaults(func=_cmd_models)

    s = sub.add_parser("slice", help="slice one model with the gates forced on")
    s.add_argument("--model", required=True, choices=sorted(MODELS))
    s.add_argument("--out", required=True)
    s.add_argument("--gates", nargs="*", default=["classic", "arcs"], choices=sorted(FEATURE_GATES))
    s.add_argument("--center", help="x,y -- only needed to match a GUI export")
    s.add_argument("--slice-report", type=Path)
    s.add_argument("--config", default="RelWithDebInfo")
    s.set_defaults(func=_cmd_slice)

    c = sub.add_parser("compare", help="compare two G-code files")
    c.add_argument("before")
    c.add_argument("after")
    c.add_argument("--require", nargs="*", default=[], choices=sorted(FINGERPRINTS),
                   help="fail if these features are absent (guards against a vacuous pass)")
    c.set_defaults(func=_cmd_compare)

    args = p.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
