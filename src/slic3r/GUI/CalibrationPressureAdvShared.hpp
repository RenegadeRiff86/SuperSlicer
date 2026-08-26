#pragma once

#include "CalibrationPressureAdvDialog.hpp"

#include "libslic3r/CustomGCode.hpp"
#include "libslic3r/Model.hpp"

#include <array>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>
#include <vector>

namespace Slic3r::GUI::CalibrationPressureAdvDetail {

constexpr int  pa_label_max_decimals                      = 6;
constexpr char kExtrusionSpacingKey[]                      = "extrusion_spacing";
constexpr char kFirstLayerSpeedKey[]                       = "first_layer_speed";
constexpr char kCalibrationResourceDirectory[]             = "calibration";
constexpr char kFilamentPressureResourceDirectory[]        = "filament_pressure";
constexpr char kLayerHeightKey[]                            = "layer_height";
constexpr char kPressureAdvanceBorderResource[]             = "pa_border.3mf";
constexpr size_t kPaControlDecimalPlaces                    = 2;
constexpr double kPercentScale                              = 100.0;
constexpr int    kDefaultFlowPercent                        = 100;
constexpr double kGeometryCenterDivisor                     = 2.0;
constexpr size_t kMiddleIndexDivisor                        = 2;
constexpr int    kParityDivisor                             = 2;
constexpr double kMaximumCalibrationNozzleDiameter          = 2.0;
constexpr int    kActiveCalibrationLayerCount               = 5;
constexpr int    kControlBorder                             = 5;
constexpr int    kRowSpacer                                 = 15;
constexpr int    kExtrusionRoleChoiceCount                  = 15;
constexpr double kRoundCapCount                             = 2.0;
constexpr double kDoubleNozzleDiameterScale                 = 2.0;
constexpr double kNozzleSnapStepMm                          = 0.1;
constexpr double kMinimumSnappedNozzleMm                    = 0.10;
constexpr size_t kNozzleDiameterBufSize                     = 16;
constexpr int    kPaControlStreamPrecision                  = 4;
constexpr int    kSolidInfillOverlapCapPercent              = 80;
constexpr size_t kThinWallExtrusionRoleIndex                = 11;
constexpr double kPaVolumeLayerHeightMm                     = 0.3;
constexpr double kErWidthPercentThresholdMultiplier         = 3.0;
constexpr int    kPanelInnerPaddingPx                       = 10;
constexpr int    kGenerateCloseSpacerPx                     = 50;
constexpr double kMicrosecondsPerSecond                     = 1000000.0;

inline std::string format_pa_label_value(double value)
{
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::fixed << std::setprecision(pa_label_max_decimals) << value;

    std::string label = stream.str();
    const size_t decimal_pos = label.find('.');
    if (decimal_pos != std::string::npos) {
        while (label.size() > decimal_pos + 1 && label.back() == '0')
            label.pop_back();
        if (!label.empty() && label.back() == '.')
            label.pop_back();
    }

    return label == "-0" ? "0" : label;
}

inline std::string pressure_advance_prefix(GCodeFlavor flavor, bool smooth_time)
{
    switch (flavor) {
    case gcfKlipper:
        return smooth_time ? "SET_PRESSURE_ADVANCE SMOOTH_TIME=" : "SET_PRESSURE_ADVANCE ADVANCE=";
    case gcfMarlinFirmware:
        return "M900 K";
    case gcfRepRap:
        return "M572 D0 S"; // D0 is the single-extruder default; custom multi-tool commands are tracked in #42.
    default:
        return "";
    }
}

struct PaValueHint
{
    bool valid = false;
    double value = 0.0;
};

struct PaControlDefaults
{
    wxString first_layer = "0.040";
    wxString start = "0.0";
    wxString end = "0.10";
    wxString increment = "0.005";
    bool from_filament_pa = false;
};

inline std::string format_pa_control_value(double value)
{
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::fixed << std::setprecision(kPaControlStreamPrecision) << value;

    std::string text = stream.str();
    const size_t decimal_pos = text.find('.');
    if (decimal_pos != std::string::npos) {
        while (text.size() > decimal_pos + kPaControlDecimalPlaces && text.back() == '0')
            text.pop_back();
    }

    return text == "-0.0" ? "0.0" : text;
}

inline PaValueHint enabled_positive_pa_value(const DynamicPrintConfig* config, const char* key)
{
    const ConfigOptionFloats* option = config ? config->option<ConfigOptionFloats>(key) : nullptr;
    if (!option || option->size() == 0 || !option->is_enabled(0))
        return {};

    const double value = option->get_at(0);
    return value > 0.0 ? PaValueHint{ true, value } : PaValueHint{};
}

inline PaControlDefaults pa_control_defaults_from_filament(const DynamicPrintConfig* filament_config)
{
    PaControlDefaults defaults;
    const PaValueHint base_pa = enabled_positive_pa_value(filament_config, "filament_pressure_advance");
    if (!base_pa.valid)
        return defaults;

    const PaValueHint first_layer_pa = enabled_positive_pa_value(filament_config, "filament_first_layer_pa");
    const double range_half_width = base_pa.value > 0.1 ? base_pa.value * 0.5 : 0.05;
    const double start = (base_pa.value > range_half_width) ? base_pa.value - range_half_width : 0.0;
    double end = base_pa.value + range_half_width;
    if (end > 1.0)
        end = 1.0;
    if (end <= start)
        end = start + 0.05;

    const double span = end - start;
    const double increment = span <= 0.1 ? 0.005 : (span <= 0.3 ? 0.01 : 0.05);

    defaults.first_layer = wxString(format_pa_control_value(first_layer_pa.valid ? first_layer_pa.value : base_pa.value));
    defaults.start = wxString(format_pa_control_value(start));
    defaults.end = wxString(format_pa_control_value(end));
    defaults.increment = wxString(format_pa_control_value(increment));
    defaults.from_filament_pa = true;
    return defaults;
}

// Stamps each bend's PA value onto its matching digit volumes as region_gcode. Parked behind
// enable_region_gcode_for_numbers in create_geometry: the labels do come out, but the first layer
// ends up with messed up surfaces, so that has to be understood before this can be switched on.
// num_part is advanced exactly the way the caller's own volume walk advances it.
inline void apply_pa_region_gcode_to_numbers(ModelObject& object,
                                      const std::vector<double>& pa_values,
                                      size_t number_position_count,
                                      int count_numbers,
                                      int count_borders,
                                      int extra_vol,
                                      GCodeFlavor flavor,
                                      bool smooth_time,
                                      size_t& num_part)
{
    const std::string set_advance_prefix = pressure_advance_prefix(flavor, smooth_time);

    int pa_index = 0;
    int nb_number = 0;

    while (nb_number < int(number_position_count)) {

        // Odd pa_index slots hold no number set, so only the index moves on.
        if (pa_index % kParityDivisor == 1) {
            pa_index++; // increment pa_index to match how numbers are loaded
            continue;
        }

        // Borders and out-of-range volumes are stepped over one at a time.
        if ((nb_number >= count_numbers && nb_number < count_numbers + count_borders) ||
            num_part >= object.volumes.size()) {
            num_part++;
            nb_number++;
            continue; // Skip to the next iteration same way numbers get loaded.
        }

        // Apply the PA value to the number set stays inline with 90_bend models
        for (int number_set = 0; number_set < count_numbers; number_set++) {
            object.volumes[number_set + num_part + extra_vol]->config.set_key_value(
                "region_gcode",
                std::make_unique<ConfigOptionString>(set_advance_prefix + std::to_string(pa_values[pa_index]) + " ; "));

            nb_number++;
        }
        pa_index++;
        num_part += count_numbers;
    }
}

// Role name constants extracted to eliminate repeated string literal warnings.
// All former "RoleName" literals throughout this file now reference these single-definition constants.
inline const std::string ROLE_INTERNAL_INFILL            = "InternalInfill";
inline const std::string ROLE_BRIDGE_INFILL              = "BridgeInfill";
inline const std::string ROLE_EXTERNAL_PERIMETER         = "ExternalPerimeter";
inline const std::string ROLE_GAP_FILL                   = "GapFill";
inline const std::string ROLE_INTERNAL_BRIDGE_INFILL     = "InternalBridgeInfill";
inline const std::string ROLE_IRONING                    = "Ironing";
inline const std::string ROLE_OVERHANG_PERIMETER         = "OverhangPerimeter";
inline const std::string ROLE_PERIMETER                  = "Perimeter";
inline const std::string ROLE_SOLID_INFILL               = "SolidInfill";
inline const std::string ROLE_SUPPORT_MATERIAL           = "SupportMaterial";
inline const std::string ROLE_SUPPORT_MATERIAL_INTERFACE = "SupportMaterialInterface";
inline const std::string ROLE_THIN_WALL                  = "ThinWall";
inline const std::string ROLE_TOP_SOLID_INFILL           = "TopSolidInfill";
inline const std::string ROLE_FIRST_LAYER                = "FirstLayer";
inline const std::string ROLE_CHECK_ALL                  = "CheckAll";

// The extrusion-role dropdown's flow-bearing roles. CheckAll is handled on its own path, so it is
// not one of these.
using CalibrationRoleChoices = std::array<std::string, 14>;

} // namespace Slic3r::GUI::CalibrationPressureAdvDetail
