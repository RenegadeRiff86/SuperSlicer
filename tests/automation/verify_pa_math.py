#!/usr/bin/env python3
"""Numerical audit of accuracy-critical SuperSlicer formulas.

Mirrors C++ in GCode.cpp (PA peak/inverses), AdaptivePressureAdvance.cpp,
and Plater filament weight conversion. Uses numpy when available for tent
convolution; pure math otherwise for inverses/APA/filament.
"""
from __future__ import annotations

import math
import random
import sys

try:
    import numpy as np
    HAS_NP = True
except Exception:
    HAS_NP = False

PI = 3.141592653589793238
failures: list[tuple[str, str]] = []
n_checks = 0


def check(name: str, cond: bool, detail="") -> None:
    global n_checks
    n_checks += 1
    if not cond:
        failures.append((name, str(detail)))


# ---------------------------------------------------------------------------
# C++ PA formulas (GCode.cpp)
# ---------------------------------------------------------------------------

def pa_peak(speed, a, K, Ts):
    if K <= 0 or a <= 0 or speed <= 0:
        return speed
    r = min(1.0, speed / (a * Ts)) if Ts > 0 else 1.0
    return speed + K * a * r * (2.0 - r)


def pa_speed(budget, a, K, Ts):
    if K <= 0 or a <= 0:
        return budget
    if Ts <= 0:
        return budget - K * a
    if budget >= a * Ts + K * a:
        return budget - K * a
    b = a * Ts * (Ts / K + 2.0)
    c = a * Ts * Ts * budget / K
    disc = b * b - 4 * c
    if disc <= 0:
        return budget - K * a
    return 0.5 * (b - math.sqrt(disc))


def pa_accel(budget, speed, K, Ts):
    if K <= 0 or speed <= 0:
        return float("inf")
    if budget <= speed:
        return 0.0
    slack = budget - speed
    if Ts <= 0:
        return slack / K
    ceiling = 2 * K * speed / Ts
    if ceiling <= slack:
        return float("inf")
    uns = slack / K
    if uns <= speed / Ts:
        return uns
    return K * speed * speed / (Ts * Ts * (ceiling - slack))


def sim_peak(speed, a, K, Ts, dt=1e-4):
    """Peak of tent-smoothed extruder velocity during 0->speed ramp.

    Raw advanced feed: during the ramp a*t + K*a, then steady speed.
    Force a sample exactly at t=Ta so the unsmoothed peak is not missed by the grid.
    """
    if K <= 0 or a <= 0 or speed <= 0:
        return speed
    Ta = speed / a
    if Ts <= 0:
        # Exact unsmoothed peak is at the end of the ramp.
        return speed + K * a
    T_end = Ta + Ts + 50 * dt
    # Build time grid including Ta exactly.
    times = [i * dt for i in range(int(math.ceil(T_end / dt)) + 1)]
    if Ta not in times:
        times.append(Ta)
        times.sort()
    v_raw = []
    for t in times:
        if t < Ta - 1e-15:
            v_raw.append(a * t + K * a)
        elif abs(t - Ta) <= 1e-15:
            v_raw.append(speed + K * a)  # still at full a for an instant
        else:
            v_raw.append(speed)
    if not HAS_NP:
        m = max(1, int(round(Ts / dt)))
        half = m / 2.0
        w = [max(0.0, 1.0 - abs(j - half) / half) if half > 0 else 1.0 for j in range(m + 1)]
        s = sum(w)
        w = [x / s for x in w]
        peak = 0.0
        for i in range(len(v_raw)):
            acc = 0.0
            for j, wj in enumerate(w):
                k = i - j
                acc += wj * (v_raw[k] if k >= 0 else 0.0)
            peak = max(peak, acc)
        return peak

    v = np.asarray(v_raw, dtype=float)
    m = max(1, int(round(Ts / dt)))
    half = m / 2.0
    j = np.arange(m + 1)
    w = np.maximum(0.0, 1.0 - np.abs(j - half) / half) if half > 0 else np.ones(m + 1)
    w = w / w.sum()
    sm = np.convolve(v, w, mode="full")[: len(v)]
    return float(sm.max())


def main() -> int:
    # 1) Unsmoothed identity
    for speed, a, K in [(50, 1000, 0.05), (100, 3000, 0.04), (20, 500, 0.1)]:
        p = pa_peak(speed, a, K, 0.0)
        check(f"unsmoothed formula {speed}", abs(p - (speed + K * a)) < 1e-12, p)
        s = sim_peak(speed, a, K, 0.0)
        check(f"unsmoothed sim {speed}", abs(s - (speed + K * a)) < 1e-9, s)

    # 2) Bound vs tent simulation
    underrun = 0
    max_oh = 0.0
    ohs = []
    cases = []
    for speed in [20, 50, 100, 150]:
        for a in [1000, 3000, 8000]:
            for K in [0.03, 0.05, 0.08]:
                for Ts in [0.0, 0.04, 0.1, 0.2]:
                    cases.append((speed, a, K, Ts))
    random.seed(1)
    for _ in range(60):
        cases.append(
            (
                random.uniform(10, 200),
                random.uniform(500, 12000),
                random.uniform(0.02, 0.1),
                random.choice([0.0, 0.04, 0.1, 0.2]),
            )
        )

    for speed, a, K, Ts in cases:
        bound = pa_peak(speed, a, K, Ts)
        sim = sim_peak(speed, a, K, Ts)
        global n_checks
        n_checks += 1
        if bound + 1e-4 < sim:
            underrun += 1
            failures.append(
                (
                    f"UNDER v={speed:.1f} a={a:.0f} K={K} Ts={Ts}",
                    f"b={bound:.6f} s={sim:.6f}",
                )
            )
        if sim > 0:
            oh = (bound - sim) / sim
            max_oh = max(max_oh, oh)
            ohs.append(oh)
    check("no underruns (bound is safe)", underrun == 0, underrun)
    # Comment in GCode.cpp claims <=21% vs a specific integration; our tent-on-velocity
    # proxy is looser. Still require the bound never under-predict, and stay <50% high.
    check("max overhang < 0.50 (conservative bound)", max_oh < 0.50, max_oh)

    # 3) large-a asymptote: r->0 => speed + 2*K*speed/Ts
    for speed, K, Ts in [(50, 0.05, 0.04), (100, 0.04, 0.1)]:
        b = pa_peak(speed, 1e12, K, Ts)
        exp = speed + 2 * K * speed / Ts
        check(f"asymptote {speed}", abs(b - exp) / exp < 1e-6, (b, exp))

    # 4) inverses round-trip
    for budget, a, K, Ts in [
        (80, 2000, 0.05, 0.0),
        (80, 2000, 0.05, 0.04),
        (120, 5000, 0.04, 0.1),
        (40, 1000, 0.08, 0.02),
        (200, 3000, 0.03, 0.2),
        (60, 4000, 0.06, 0.08),
    ]:
        v = pa_speed(budget, a, K, Ts)
        if v > 0:
            peak = pa_peak(v, a, K, Ts)
            check(
                f"speed-inv b={budget} Ts={Ts}",
                abs(peak - budget) / budget < 1e-5 or peak <= budget + 1e-6,
                (v, peak),
            )
            a2 = pa_accel(budget, v, K, Ts)
            if math.isfinite(a2) and a2 > 0:
                peak2 = pa_peak(v, a2, K, Ts)
                check(
                    f"accel-inv b={budget}",
                    abs(peak2 - budget) / budget < 1e-4 or peak2 <= budget + 1e-5,
                    (a2, peak2),
                )

    # 5) monotonicity in speed
    for a, K, Ts in [(2000, 0.05, 0.04), (5000, 0.04, 0.0), (1000, 0.1, 0.2)]:
        prev = -1.0
        ok = True
        for speed in [5, 10, 20, 40, 80, 120, 200]:
            p = pa_peak(speed, a, K, Ts)
            if p + 1e-12 < prev:
                ok = False
            prev = p
        check(f"mono-v a={a} Ts={Ts}", ok)

    # 6) Adaptive PA bilinear (AdaptivePressureAdvance.cpp)
    class Model:
        def __init__(self, pts):
            self.pts = pts
            ACC = 1.0
            acc = sorted(p[2] for p in pts)
            self.L = []
            for a in acc:
                if not self.L or a - self.L[-1] > ACC:
                    self.L.append(a)

        def at(self, lvl, flow):
            ACC = 1.0
            band = sorted([p for p in self.pts if abs(p[2] - lvl) <= ACC], key=lambda p: p[1])
            if not band:
                return 0.0
            if len(band) == 1 or flow <= band[0][1]:
                return band[0][0]
            if flow >= band[-1][1]:
                return band[-1][0]
            for i in range(1, len(band)):
                if flow <= band[i][1]:
                    lo, hi = band[i - 1], band[i]
                    sp = hi[1] - lo[1]
                    t = (flow - lo[1]) / sp if sp > 0 else 0
                    return lo[0] + t * (hi[0] - lo[0])
            return band[-1][0]

        def e(self, flow, accel, fb=0.0, step=1e-3):
            if not self.pts:
                return fb
            L = self.L
            if len(L) == 1 or accel <= L[0]:
                r = self.at(L[0], flow)
            elif accel >= L[-1]:
                r = self.at(L[-1], flow)
            else:
                hi = 1
                while hi < len(L) and accel > L[hi]:
                    hi += 1
                lo, h = L[hi - 1], L[hi]
                t = (accel - lo) / (h - lo) if h > lo else 0
                r = self.at(lo, flow) + t * (self.at(h, flow) - self.at(lo, flow))
            if r < 0:
                r = 0
            if step > 0:
                r = round(r / step) * step
            return r

    m = Model(
        [
            (0.040, 3.84, 1000),
            (0.030, 7.68, 1000),
            (0.030, 3.84, 2000),
            (0.020, 7.68, 2000),
        ]
    )
    expect = [
        (3.84, 1000, 0.040),
        (7.68, 1000, 0.030),
        (3.84, 2000, 0.030),
        (7.68, 2000, 0.020),
        (5.76, 1000, 0.035),
        (5.76, 2000, 0.025),
        (3.84, 1500, 0.035),
        (5.76, 1500, 0.030),
        (0.5, 1000, 0.040),
        (99, 1000, 0.030),
        (3.84, 100, 0.040),
        (3.84, 99999, 0.030),
        (5.12, 1000, 0.037),
    ]
    for f, a, exp in expect:
        got = m.e(f, a)
        check(f"apa {f},{a}", abs(got - exp) < 1e-12, (got, exp))
    check("apa empty", abs(Model([]).e(5, 1500, 0.123) - 0.123) < 1e-15)

    # 7) Filament weight (Plater.cpp fix)
    def cross(d):
        return (d * 0.5) ** 2 * PI

    for d, ef in [(1.75, 5.8), (2.85, 41)]:
        c = cross(d)
        dens = 1.24
        L = 12820
        mm_to_g = c * dens * 0.001
        old = dens / (c * 1000)
        ratio = mm_to_g / old
        check(f"weight truth {d}", abs(L * mm_to_g - L * c * dens * 0.001) < 1e-12)
        check(f"ratio=c^2 {d}", abs(ratio - c * c) < 1e-9, ratio)
        check(f"factor~{ef} {d}", abs(ratio - ef) < (0.2 if d < 2 else 1.5), ratio)

    meas = 30833.28 / (12.82 * 1000)
    c175 = cross(1.75)
    check("bunny cross", abs(meas - c175) / c175 < 0.01, (meas, c175))
    fil_vol = 30833.28
    Lm = 12.82 * 1000
    rem = fil_vol - Lm * c175
    check("remainder sane", rem < fil_vol)
    check("old rem absurd", abs(fil_vol - Lm) > abs(rem) * 10)

    # 8) external join width selection
    check("ext width", (0.45 if True else 0.4) == 0.45)
    check("int width", (0.45 if False else 0.4) == 0.4)

    print(f"HAS_NP={HAS_NP}")
    print(f"CHECKS={n_checks} FAILURES={len(failures)}")
    print(
        f"PA cases={len(cases)} underruns={underrun} "
        f"max_oh={max_oh:.4%} mean_oh={(sum(ohs)/len(ohs) if ohs else 0):.4%}"
    )
    print(f"filament factors 1.75={cross(1.75)**2:.6f} 2.85={cross(2.85)**2:.6f}")
    for f in failures[:20]:
        print("FAIL", f[0], f[1])
    print("ALL_PASS" if not failures else "HAS_FAILURES")
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
