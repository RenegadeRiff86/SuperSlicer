#pragma once

#include <algorithm>
#include <array>
#include <sstream>
#include <string>
#include <vector>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r::sla {

// Photon Mono 4 firmware varies exposure only for a file named exactly this.
inline constexpr const char *RERF_FILENAME = "R_E_R_F.pm4n";
inline constexpr int RERF_ZONE_COUNT = 8;
inline constexpr double RERF_EXPOSURE_STEP_S = 0.25;

// Eight equal areas of the bed. Looking at the bottom of the plate, area 1 is
// the back-left and area 2 is directly in front of it. The next columns to the
// right are 3/4, 5/6 and 7/8. The layer mask is mirrored in X, so a picture of
// the screen shows areas 1 and 2 on the right.
struct RerfZone {
    int index = 0;
    BoundingBoxf box;
};

inline std::array<RerfZone, RERF_ZONE_COUNT> rerf_zones(const BoundingBoxf &bed)
{
    std::array<RerfZone, RERF_ZONE_COUNT> zones{};
    if (!bed.defined)
        return zones;

    const double column_width = (bed.max.x() - bed.min.x()) / 4.0;
    const double row_height = (bed.max.y() - bed.min.y()) / 2.0;
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 2; ++row) {
            RerfZone &zone = zones[column * 2 + row];
            zone.index = column * 2 + row + 1;
            const double x0 = bed.min.x() + column * column_width;
            // row 0 is the back half (higher Y). row 1 is the front half.
            const double y0 = row == 0 ? bed.min.y() + row_height : bed.min.y();
            const double y1 = row == 0 ? bed.max.y() : bed.min.y() + row_height;
            zone.box = BoundingBoxf{Vec2d{x0, y0}, Vec2d{x0 + column_width, y1}};
        }
    }
    return zones;
}

inline std::string format_rerf_seconds(double seconds)
{
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(2);
    out << seconds;
    std::string text = out.str();
    while (!text.empty() && text.back() == '0')
        text.pop_back();
    if (!text.empty() && text.back() == '.')
        text.pop_back();
    return text;
}

// Exposure for every area the object actually covers. An object that only
// touches a boundary stays with the area it occupies.
inline std::string rerf_exposure_label(const BoundingBoxf &object_xy, const BoundingBoxf &bed, double base_exposure_s)
{
    if (!object_xy.defined || !bed.defined)
        return {};

    std::vector<int> zones;
    for (const RerfZone &zone : rerf_zones(bed)) {
        if (!zone.box.defined)
            continue;
        const double overlap_x = std::min(object_xy.max.x(), zone.box.max.x()) - std::max(object_xy.min.x(), zone.box.min.x());
        const double overlap_y = std::min(object_xy.max.y(), zone.box.max.y()) - std::max(object_xy.min.y(), zone.box.min.y());
        if (overlap_x > 0.05 && overlap_y > 0.05)
            zones.push_back(zone.index);
    }
    if (zones.empty())
        return {};

    std::sort(zones.begin(), zones.end());
    std::string label;
    for (int zone : zones) {
        if (!label.empty())
            label += ", ";
        label += format_rerf_seconds(base_exposure_s + (zone - 1) * RERF_EXPOSURE_STEP_S);
    }
    label += " s";
    return label;
}

inline bool photon_mono4_rerf_enabled(const DynamicPrintConfig *config)
{
    if (config == nullptr)
        return false;
    const auto *enabled = config->option<ConfigOptionBool>("rerf_calibration");
    const auto *format = config->option<ConfigOptionEnum<OutputFormat>>("output_format");
    return enabled != nullptr && enabled->value && format != nullptr && format->value == OutputFormat::ofAnycubicMono4;
}

inline BoundingBoxf rerf_bed_bbox(const DynamicPrintConfig *config)
{
    if (config == nullptr)
        return {};
    const auto *shape = config->option<ConfigOptionPoints>("bed_shape");
    if (shape == nullptr || shape->get_values().empty())
        return {};
    return BoundingBoxf(shape->get_values());
}

inline double rerf_base_exposure_s(const DynamicPrintConfig *config)
{
    if (config == nullptr)
        return 0.;
    const auto *exposure = config->option<ConfigOptionFloat>("exposure_time");
    return exposure == nullptr ? 0. : exposure->value;
}

} // namespace Slic3r::sla
