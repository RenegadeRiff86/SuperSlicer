///|/ Adaptive Pressure Advance model (issue #39)
///|/ Copyright (c) 2026 Stan Elston (RenegadeRiff86)
///|/
///|/ PrusaSlicer/SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#pragma once

#include <string>
#include <vector>

namespace Slic3r {

// Adaptive pressure advance: pressure advance (PA) expressed as a function of a move's
// volumetric flow (mm^3/s) and acceleration (mm/s^2), interpolated from per-filament
// calibration measurements.
//
// The measurement string is OrcaSlicer-compatible: one record per line, three
// comma-separated values "PA, volumetric_flow(mm^3/s), acceleration(mm/s^2)", e.g.
//     0.040, 3.84, 1000
//     0.030, 7.68, 2000
//     0.024, 15.35, 4000
// Blank lines and non-numeric/header lines are ignored.
class AdaptivePAModel
{
public:
    struct Point { double pa = 0.; double flow = 0.; double accel = 0.; };

    AdaptivePAModel() = default;
    explicit AdaptivePAModel(const std::string &measurements) { parse(measurements); }

    // Parse the measurement string. Returns true if at least one valid point was found.
    // Replaces any previously parsed data.
    bool parse(const std::string &measurements);

    bool   empty() const { return m_points.empty(); }
    size_t size()  const { return m_points.size(); }
    const std::vector<Point>& points() const { return m_points; }

    // Interpolate PA for the given volumetric flow (mm^3/s) and acceleration (mm/s^2).
    // Bilinear-style: PA is interpolated vs flow within each measured acceleration band,
    // then between the two bracketing bands vs acceleration. Inputs outside the measured
    // range are clamped to the nearest edge. With an empty model the supplied fallback is
    // returned unchanged. The result is rounded to step_round to avoid churning the output
    // command on near-identical consecutive moves.
    double evaluate(double flow, double accel, double fallback = 0., double step_round = 1e-3) const;

private:
    std::vector<Point>  m_points;       // all parsed measurements
    std::vector<double> m_accel_levels; // sorted unique acceleration levels

    void   rebuild_levels();
    // PA vs flow at one acceleration level: linear interpolation over the points whose
    // acceleration matches accel_level, clamped to the flow range at that level.
    double pa_at_accel_level(double accel_level, double flow) const;
};

} // namespace Slic3r
