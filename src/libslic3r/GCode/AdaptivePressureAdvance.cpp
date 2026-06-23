///|/ Adaptive Pressure Advance model (issue #39)
///|/ Copyright (c) 2026 Stan Elston (RenegadeRiff86)
///|/
///|/ PrusaSlicer/SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "AdaptivePressureAdvance.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace Slic3r {

// Acceleration levels within this distance (mm/s^2) are treated as the same band.
static constexpr double ACCEL_LEVEL_EPS = 1.0;

// Parse one C-locale floating point number from [b, e); returns false if no number is read.
static bool parse_double_c(const char *b, const char *e, double &out)
{
    // Skip leading whitespace.
    while (b < e && (*b == ' ' || *b == '\t' || *b == '\r'))
        ++b;
    if (b >= e)
        return false;
    char *end = nullptr;
    // strtod uses the C locale, so '.' is always the decimal separator regardless of the
    // user's system locale (the same locale-independence lesson as #38, engine side).
    const double v = std::strtod(b, &end);
    if (end == b)
        return false;
    out = v;
    return true;
}

bool AdaptivePAModel::parse(const std::string &measurements)
{
    m_points.clear();
    m_accel_levels.clear();

    size_t pos = 0;
    const size_t n = measurements.size();
    while (pos <= n) {
        size_t eol = measurements.find('\n', pos);
        if (eol == std::string::npos)
            eol = n;
        const char *line_b = measurements.data() + pos;
        const char *line_e = measurements.data() + eol;
        pos = eol + 1;

        // Split the line into up to three comma-separated fields.
        double vals[3];
        int    count = 0;
        const char *field_b = line_b;
        for (const char *p = line_b; count < 3 && field_b <= line_e; ++p) {
            if (p == line_e || *p == ',') {
                double v;
                if (parse_double_c(field_b, p, v))
                    vals[count++] = v;
                else if (count > 0 || p != field_b)
                    // A malformed field after a good one (or a non-empty non-number) invalidates the row.
                    { count = -1; break; }
                field_b = p + 1;
                if (p == line_e)
                    break;
            }
        }
        if (count == 3) {
            const double pa = vals[0], flow = vals[1], accel = vals[2];
            // Reject obviously invalid rows (header text parses to count<3 and is skipped above).
            if (pa >= 0. && flow > 0. && accel >= 0.)
                m_points.push_back(Point{ pa, flow, accel });
        }
        if (eol == n)
            break;
    }

    rebuild_levels();
    return !m_points.empty();
}

void AdaptivePAModel::rebuild_levels()
{
    m_accel_levels.clear();
    std::vector<double> accels;
    accels.reserve(m_points.size());
    for (const Point &p : m_points)
        accels.push_back(p.accel);
    std::sort(accels.begin(), accels.end());
    for (double a : accels) {
        if (m_accel_levels.empty() || a - m_accel_levels.back() > ACCEL_LEVEL_EPS)
            m_accel_levels.push_back(a);
    }
}

double AdaptivePAModel::pa_at_accel_level(double accel_level, double flow) const
{
    // Collect points at this acceleration band, sorted by flow.
    std::vector<const Point*> band;
    for (const Point &p : m_points)
        if (std::abs(p.accel - accel_level) <= ACCEL_LEVEL_EPS)
            band.push_back(&p);
    if (band.empty())
        return 0.;
    std::sort(band.begin(), band.end(), [](const Point *a, const Point *b) { return a->flow < b->flow; });

    if (band.size() == 1 || flow <= band.front()->flow)
        return band.front()->pa;
    if (flow >= band.back()->flow)
        return band.back()->pa;
    for (size_t i = 1; i < band.size(); ++i) {
        if (flow <= band[i]->flow) {
            const Point *lo = band[i - 1];
            const Point *hi = band[i];
            const double span = hi->flow - lo->flow;
            const double t = span > 0. ? (flow - lo->flow) / span : 0.;
            return lo->pa + t * (hi->pa - lo->pa);
        }
    }
    return band.back()->pa;
}

double AdaptivePAModel::evaluate(double flow, double accel, double fallback, double step_round) const
{
    if (m_points.empty())
        return fallback;

    double result;
    if (m_accel_levels.size() == 1) {
        result = pa_at_accel_level(m_accel_levels.front(), flow);
    } else if (accel <= m_accel_levels.front()) {
        result = pa_at_accel_level(m_accel_levels.front(), flow);
    } else if (accel >= m_accel_levels.back()) {
        result = pa_at_accel_level(m_accel_levels.back(), flow);
    } else {
        size_t hi = 1;
        while (hi < m_accel_levels.size() && accel > m_accel_levels[hi])
            ++hi;
        const double lo_lvl = m_accel_levels[hi - 1];
        const double hi_lvl = m_accel_levels[hi];
        const double span = hi_lvl - lo_lvl;
        const double t = span > 0. ? (accel - lo_lvl) / span : 0.;
        const double pa_lo = pa_at_accel_level(lo_lvl, flow);
        const double pa_hi = pa_at_accel_level(hi_lvl, flow);
        result = pa_lo + t * (pa_hi - pa_lo);
    }

    if (result < 0.)
        result = 0.;
    if (step_round > 0.)
        result = std::round(result / step_round) * step_round;
    return result;
}

} // namespace Slic3r
