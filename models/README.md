# Verification models

Models used to check that a code change did not alter slicing output. They are
here because the setup a model *requires* has to travel with the model — sliced
without it, several of these measure a completely different and much easier job
than the one they were chosen for.

Slice them with the real printer, not the implicit default config:

```
--printer-profile  "Creality Ender-3 0.6mm - Print"
--print-profile    "0.20mm NORMAL @CREALITY - Print"
--material-profile "Overture ABS Grey"
```

The default config is a generic 0.4 mm printer with a 200 mm build volume, which
is both unrepresentative and too short for `pins_tall_supports` — on the right
printer (250 mm) no height override and no rotation is needed.

## The models

| model | required args | what it exercises |
|---|---|---|
| `pins_tall_supports.stl` | `--brim-width 5 --support-material` | 19 x 116.7 x 207.8 mm, printed **standing**. 1038 layers, ~387k moves, ~130k support-material segments. Only ~30 of 7772 triangles touch the bed, so without a brim it tips. The endurance case. |
| `bed_cover_side.stl` | `--rotate-x 90 --support-material` | Must print **on its side**. Flat it gives 0 support blocks and 261 overhang perimeters; on its side 246 blocks and 1325 overhang perimeters. The strongest overhang/support case here. |
| `thin_walls_bowflex.stl` | none | The strongest MedialAxis / gap-fill exerciser. Nearly all short segments, so the head accelerates almost continuously — also the best case for anything touching acceleration or pressure advance. |
| `handle_test.stl` | none | General overhang/bridge coverage. See the warning below before using it as a gate. |

Add `--arc-fitting=emit_center` to any of these to reach ArcWelder and
`Geometry/Circle.cpp`. **Nothing reaches the arc code without it** — a default
slice emits zero G2/G3, so an arc-free panel leaves that whole subsystem untested
no matter how many models it contains.

## Reproducibility: check before trusting a diff

Slicing is not fully deterministic, and the nondeterminism is **bimodal** in
several places — two stable outcomes with a fixed, repeatable signature rather
than small random drift. Measured on an unmodified binary:

| config | mode-switch signature |
|---|---|
| `pins` | 200 of 1038 layers, 14.799970 mm, Skirt/Brim +30, Support material -132 |
| `bed_cover_side` | 95 of 396 layers, 15.477623 mm, Support material -69, Perimeter +1 |
| `thin_walls_bowflex` + arc fitting | 1 of 99 layers, 0.000869 mm, Perimeter -1 |
| `handle_test` | 12-31 of 191 layers run-to-run |

So: **a single before-run against a single after-run cannot tell a mode switch
from a regression.** Slice 3-5 times and treat a config as unchanged if the
after-run matches *any* before-run exactly. Two runs frequently land in the same
mode and look perfectly clean.

`thin_walls_bowflex` uses no supports at all, so this is broader than the support
code. It is worth investigating on its own — a slicer that emits two different
support layouts for identical input is a defect regardless of what else is going
on.
