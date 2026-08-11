///|/ PA calibration generator. Original implementation by legend069 (2024).
///|/ Modified 2026 by Stan Elston (RenegadeRiff86) -- see git history.
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher.
///|/
#include "CalibrationPressureAdvDialog.hpp"
#include "I18N.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/CustomGCode.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/AppConfig.hpp"
#include "GLCanvas3D.hpp"
#include "GUI.hpp"
#include "GUI_ObjectList.hpp"
#include "Plater.hpp"
#include "Tab.hpp"
#include <wx/scrolwin.h>
#include <wx/display.h>
#include <wx/file.h>
#include <wx/choice.h>
#include <wx/msgdlg.h>
#include <wx/settings.h>
#include "Jobs/ArrangeJob.hpp"
//#include "Jobs/job.hpp" 2.7 requirement?
#include <array>
#include <iomanip>
#include <locale>
#include <sstream>
#include <unordered_map>

#define enable_27_fixes

#undef NDEBUG
#include <cassert>

#if ENABLE_SCROLLABLE
static wxSize get_screen_size(wxWindow* window)
{
    const auto idx = wxDisplay::GetFromWindow(window);
    wxDisplay display(idx != wxNOT_FOUND ? idx : 0u);
    return display.GetClientArea().GetSize();
}
#endif // ENABLE_SCROLLABLE

namespace Slic3r {
namespace GUI {

// Tracked in #40: confirm custom G-code ordering around extrusion-role and region changes.
// Number/point model Z scaling intentionally spans first_layer_height + base_layer_height so
// embossed labels still slice when first_layer_height <= base_layer_height. See #38.
// Tracked in #41: audit Marlin/RepRap PA calibration command semantics.
// Tracked in #42: support custom and multi-tool PA command workflows.
// PA label values are formatted compactly before digit meshes are loaded.
// Manual PA text entry is normalized to C-locale decimal text before ToCDouble parsing, so
// comma-decimal locales do not break PA calculations. See #38.

namespace {
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
constexpr double kModelPlacementOffset                      = 5.0;
constexpr int    kControlBorder                             = 5;
constexpr int    kRowSpacer                                 = 15;
constexpr int    kExtrusionRoleChoiceCount                  = 15;
constexpr double kRoundCapCount                             = 2.0;
constexpr double kDoubleNozzleDiameterScale                 = 2.0;

std::string format_pa_label_value(double value)
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

std::string pressure_advance_prefix(GCodeFlavor flavor, bool smooth_time)
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

std::string format_pa_control_value(double value)
{
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::fixed << std::setprecision(4) << value;

    std::string text = stream.str();
    const size_t decimal_pos = text.find('.');
    if (decimal_pos != std::string::npos) {
        while (text.size() > decimal_pos + kPaControlDecimalPlaces && text.back() == '0')
            text.pop_back();
    }

    return text == "-0.0" ? "0.0" : text;
}

PaValueHint enabled_positive_pa_value(const DynamicPrintConfig* config, const char* key)
{
    const ConfigOptionFloats* option = config ? config->option<ConfigOptionFloats>(key) : nullptr;
    if (!option || option->size() == 0 || !option->is_enabled(0))
        return {};

    const double value = option->get_at(0);
    return value > 0.0 ? PaValueHint{ true, value } : PaValueHint{};
}

PaControlDefaults pa_control_defaults_from_filament(const DynamicPrintConfig* filament_config)
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
void apply_pa_region_gcode_to_numbers(ModelObject& object,
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
} // namespace

// Role name constants extracted to eliminate repeated string literal warnings.
// All former "RoleName" literals throughout this file now reference these single-definition constants.
const std::string ROLE_INTERNAL_INFILL            = "InternalInfill";
const std::string ROLE_BRIDGE_INFILL              = "BridgeInfill";
const std::string ROLE_EXTERNAL_PERIMETER         = "ExternalPerimeter";
const std::string ROLE_GAP_FILL                   = "GapFill";
const std::string ROLE_INTERNAL_BRIDGE_INFILL     = "InternalBridgeInfill";
const std::string ROLE_IRONING                    = "Ironing";
const std::string ROLE_OVERHANG_PERIMETER         = "OverhangPerimeter";
const std::string ROLE_PERIMETER                  = "Perimeter";
const std::string ROLE_SOLID_INFILL               = "SolidInfill";
const std::string ROLE_SUPPORT_MATERIAL           = "SupportMaterial";
const std::string ROLE_SUPPORT_MATERIAL_INTERFACE = "SupportMaterialInterface";
const std::string ROLE_THIN_WALL                  = "ThinWall";
const std::string ROLE_TOP_SOLID_INFILL           = "TopSolidInfill";
const std::string ROLE_FIRST_LAYER                = "FirstLayer";
const std::string ROLE_CHECK_ALL                  = "CheckAll";

FlowRole string_to_flow_role(const std::string& role_str) {
    static std::unordered_map<std::string, FlowRole> role_map = {
        {ROLE_INTERNAL_INFILL, FlowRole::frInfill},
        {ROLE_BRIDGE_INFILL, FlowRole::frSupportMaterialInterface},                     // special calc required
        {ROLE_EXTERNAL_PERIMETER, FlowRole::frExternalPerimeter},
        {ROLE_GAP_FILL, FlowRole::frSupportMaterialInterface},                          // special calc required
        {ROLE_INTERNAL_BRIDGE_INFILL, FlowRole::frSupportMaterialInterface},             // special calc required
        {ROLE_IRONING, FlowRole::frSupportMaterialInterface},                          // special calc required
        {ROLE_OVERHANG_PERIMETER, FlowRole::frExternalPerimeter},                       // Tracked in #44: confirm overhang flow-role mapping.
        {ROLE_PERIMETER, FlowRole::frPerimeter},
        {ROLE_SOLID_INFILL, FlowRole::frSolidInfill},
        {ROLE_SUPPORT_MATERIAL, FlowRole::frSupportMaterial},
        {ROLE_SUPPORT_MATERIAL_INTERFACE, FlowRole::frSupportMaterialInterface},
        {ROLE_THIN_WALL, FlowRole::frSupportMaterialInterface},                         // special calc required
        {ROLE_TOP_SOLID_INFILL, FlowRole::frTopSolidInfill},
        {ROLE_FIRST_LAYER, FlowRole::frSupportMaterialInterface}                        // special calc required
    };

    const auto it = role_map.find(role_str);
    return (it != role_map.end()) ? it->second : FlowRole::frPerimeter;
}
#ifdef enable_27_fixes
GCodeExtrusionRole string_to_er_role(const std::string& role_str) {
    static std::unordered_map<std::string, GCodeExtrusionRole> role_map = {
        {ROLE_INTERNAL_INFILL, GCodeExtrusionRole::InternalInfill},
        {ROLE_BRIDGE_INFILL, GCodeExtrusionRole::BridgeInfill},
        {ROLE_EXTERNAL_PERIMETER, GCodeExtrusionRole::ExternalPerimeter},
        {ROLE_GAP_FILL, GCodeExtrusionRole::GapFill},
        {ROLE_INTERNAL_BRIDGE_INFILL, GCodeExtrusionRole::InternalBridgeInfill},
        {ROLE_IRONING, GCodeExtrusionRole::Ironing},
        {ROLE_OVERHANG_PERIMETER, GCodeExtrusionRole::OverhangPerimeter},
        {ROLE_PERIMETER, GCodeExtrusionRole::Perimeter},
        {ROLE_SOLID_INFILL, GCodeExtrusionRole::SolidInfill},
        {ROLE_SUPPORT_MATERIAL, GCodeExtrusionRole::SupportMaterial},
        {ROLE_SUPPORT_MATERIAL_INTERFACE, GCodeExtrusionRole::SupportMaterialInterface},
        {ROLE_THIN_WALL, GCodeExtrusionRole::ThinWall},
        {ROLE_TOP_SOLID_INFILL, GCodeExtrusionRole::TopSolidInfill},
        {ROLE_FIRST_LAYER, GCodeExtrusionRole::Custom}
    };

    auto it = role_map.find(role_str);
    return (it != role_map.end()) ? it->second : GCodeExtrusionRole::ExternalPerimeter;
}
#endif
#ifndef enable_27_fixes
ExtrusionRole string_to_er_role(const std::string& role_str) {
    static std::unordered_map<std::string, ExtrusionRole> role_map = {
        {ROLE_INTERNAL_INFILL, ExtrusionRole::erInternalInfill},
        {ROLE_BRIDGE_INFILL, ExtrusionRole::erBridgeInfill},
        {ROLE_EXTERNAL_PERIMETER, ExtrusionRole::erExternalPerimeter},
        {ROLE_GAP_FILL, ExtrusionRole::erGapFill},
        {ROLE_INTERNAL_BRIDGE_INFILL, ExtrusionRole::erInternalBridgeInfill},
        {ROLE_IRONING, ExtrusionRole::erIroning},
        {ROLE_OVERHANG_PERIMETER, ExtrusionRole::erOverhangPerimeter},
        {ROLE_PERIMETER, ExtrusionRole::erPerimeter},
        {ROLE_SOLID_INFILL, ExtrusionRole::erSolidInfill},
        {ROLE_SUPPORT_MATERIAL, ExtrusionRole::erSupportMaterial},
        {ROLE_SUPPORT_MATERIAL_INTERFACE, ExtrusionRole::erSupportMaterialInterface},
        {ROLE_THIN_WALL, ExtrusionRole::erThinWall},
        {ROLE_TOP_SOLID_INFILL, ExtrusionRole::erTopSolidInfill},
        {ROLE_FIRST_LAYER, ExtrusionRole::erCustom}
    };

    const auto it = role_map.find(role_str);
    return (it != role_map.end()) ? it->second : ExtrusionRole::erExternalPerimeter;
}
#endif

// The extrusion-role dropdown's flow-bearing roles. CheckAll is handled on its own path, so it is
// not one of these.
using CalibrationRoleChoices = std::array<std::string, 14>;

// Layer heights create_geometry resolves from the active config, grouped so the per-role flow
// calculation can be handed the whole set instead of seven loose doubles.
struct CalibrationLayerHeights {
    double first_layer;
    double base;
    double combined;
    double minimum;
    double maximum;
    double support_material;
    double support_material_interface;
};

// The per-role values the flow calculation hands back.
struct CalibrationErFlow {
    double width;
    double spacing;
    double speed;
    double accel;
};

// Derives width/spacing/speed/acceleration for the selected extrusion role from the flow model,
// rather than reading them out of the er_*_ToOptionKey config maps the way create_geometry does now.
//
// Parked behind enable_switch in create_geometry - it still needs work. Several roles (bridges, gap
// fill, ironing, thin walls, internal bridges) have no correct flow calculation yet and their
// assignments are left commented out inside the switch, so those roles would keep whatever flow the
// previous iteration produced. Kept compiled so it stays honest about the types it depends on.
static CalibrationErFlow compute_er_flow_by_role(const CalibrationRoleChoices& choice_extrusion_role,
                                                 const std::string& selected_extrusion_role,
                                                 const DynamicPrintConfig& print_config,
                                                 DynamicPrintConfig& full_print_config,
                                                 std::unordered_map<std::string, std::string>& er_speed_ToOptionKey,
                                                 std::unordered_map<std::string, std::string>& er_accel_ToOptionKey,
                                                 const CalibrationLayerHeights& layer_heights,
                                                 double nozzle_diameter,
                                                 double filament_max_overlap,
                                                 double default_er_speed,
                                                 double infill_every_layers,
                                                 bool infill_dense)
{
    Flow base_flow = Flow::new_from_config(FlowRole::frExternalPerimeter, print_config, nozzle_diameter, layer_heights.base, filament_max_overlap, false);

    for (std::string role_str : choice_extrusion_role) {

        //role_str = (role_str == selected_extrusion_role) ? selected_extrusion_role : role_str;
        if (role_str != selected_extrusion_role) {
            continue;
        }
        GCodeExtrusionRole extrusion_role = string_to_er_role(role_str);
        FlowRole flow_role = string_to_flow_role(role_str);
        double modified_layer_height = layer_heights.base;
        if (infill_every_layers > 1 && role_str == ROLE_INTERNAL_INFILL && infill_dense == false){
            modified_layer_height = layer_heights.combined;
        }
        else if (role_str == ROLE_SUPPORT_MATERIAL){//this one might be tricky to do, since supports layerheight can go up/down based on config. maybe load 3 90_bend models for supports with low,high, middle layer heights?
            if (layer_heights.support_material == 0){
                double average_layer_height = (layer_heights.minimum + layer_heights.maximum) / kGeometryCenterDivisor;
                modified_layer_height = average_layer_height;
            }
            else{
                modified_layer_height = layer_heights.support_material;
            }
        }
        else if (role_str == ROLE_SUPPORT_MATERIAL_INTERFACE){
            if (layer_heights.support_material_interface == 0)
            {
                modified_layer_height = layer_heights.maximum;
            }
            else{
                modified_layer_height = layer_heights.support_material_interface;
            }
        }
        else if (role_str == ROLE_FIRST_LAYER){
            modified_layer_height = layer_heights.first_layer;
        }

        //move this later
        double bridge_flow_ratio = full_print_config.get_abs_value("bridge_flow_ratio", nozzle_diameter);

        //er_width = print_config.get_abs_value(er_width_ToOptionKey[selected_extrusion_role].c_str(), nozzle_diameter);
        switch (extrusion_role) {
            case GCodeExtrusionRole::InternalInfill:
                base_flow = Flow::new_from_config(flow_role, print_config, nozzle_diameter, modified_layer_height, filament_max_overlap, false);
                break;
            case GCodeExtrusionRole::BridgeInfill:// this will be tricky because bridges don't get any "layersquish" so the 90_bend model will have to have "empty" layers to help simulate a bridge

                //base_flow = Flow::new_from_width( bridge_flow_ratio, nozzle_diameter, base_layer_height, perimeter_overlap, true);//does this return the correct height value?
                base_flow = Flow::bridging_flow(float(sqrt(bridge_flow_ratio) * nozzle_diameter) , nozzle_diameter);
                            // or new_from_config_width ?
                //base_flow = Flow::new_from_config(flow_role, print_config, nozzle_diameter, base_layer_height, filament_max_overlap, false);
                break;
            case GCodeExtrusionRole::ExternalPerimeter:
                base_flow = Flow::new_from_config(flow_role, print_config, nozzle_diameter, layer_heights.base, filament_max_overlap, false);
                break;
            case GCodeExtrusionRole::GapFill:// i don't think i can adjust width/spacing for this one. only speed related config. unless i scale the 90_bend model wrong so it DOES get gap fill ? won't work for arachne, or will it ?
                //base_flow = Flow::new_from_config(flow_role, print_config, nozzle_diameter, base_layer_height, filament_max_overlap, false);
                break;
            case GCodeExtrusionRole::InternalBridgeInfill:
                //base_flow = Flow::new_from_config(flow_role, print_config, nozzle_diameter, modified_layer_height, filament_max_overlap, false);
                break;
            case GCodeExtrusionRole::Ironing: //ironing_flowrate ironing_spacing
                //base_flow = Flow::new_from_config(flow_role, print_config, nozzle_diameter, modified_layer_height, filament_max_overlap, false);
                break;
            case GCodeExtrusionRole::OverhangPerimeter:
                base_flow = Flow::new_from_config(flow_role, print_config, nozzle_diameter, layer_heights.base, filament_max_overlap, false);
                break;
            case GCodeExtrusionRole::Perimeter:
                base_flow = Flow::new_from_config(flow_role, print_config, nozzle_diameter, layer_heights.base, filament_max_overlap, false);
                break;
            case GCodeExtrusionRole::SolidInfill:
                base_flow = Flow::new_from_config(flow_role, print_config, nozzle_diameter, layer_heights.base, filament_max_overlap, false);
                break;
            case GCodeExtrusionRole::SupportMaterial:
                base_flow = Flow::new_from_config(flow_role, print_config, nozzle_diameter, modified_layer_height, filament_max_overlap, false);
                break;
            case GCodeExtrusionRole::SupportMaterialInterface:
                base_flow = Flow::new_from_config(flow_role, print_config, nozzle_diameter, layer_heights.base, filament_max_overlap, false);
                break;
            case GCodeExtrusionRole::ThinWall://maybe scale the 90_bend models down so they get detected as thin_walls_min_width config ? this will result in a "single wall" 90_bend model hmmm..
                //base_flow = Flow::new_from_config(flow_role, print_config, nozzle_diameter, base_layer_height, filament_max_overlap, false);
                break;
            case GCodeExtrusionRole::TopSolidInfill:
                base_flow = Flow::new_from_config(flow_role, print_config, nozzle_diameter, layer_heights.base, filament_max_overlap, false);
                break;
            case GCodeExtrusionRole::Custom://first_layer
                base_flow = Flow::new_from_config(flow_role, print_config, nozzle_diameter, modified_layer_height, 1.f, true);
                break;
            default:
                base_flow = Flow::new_from_config(FlowRole::frExternalPerimeter, print_config, nozzle_diameter, layer_heights.base, filament_max_overlap, false);//unsupported roles.
                continue;
        }
        break;
    }

    CalibrationErFlow flow_values;
    flow_values.width = base_flow.width();
    flow_values.spacing = base_flow.spacing();
    flow_values.width = std::round((flow_values.width * kPercentScale / nozzle_diameter) * kPercentScale) / kPercentScale;

    //flow_values.speed = full_print_config.get_computed_value(er_speed_ToOptionKey[selected_extrusion_role].c_str());
    const ConfigOptionFloatOrPercent* first_layer_speed_option = dynamic_cast<const ConfigOptionFloatOrPercent*>(full_print_config.option(kFirstLayerSpeedKey));
    flow_values.speed = (first_layer_speed_option && first_layer_speed_option->percent && selected_extrusion_role == ROLE_FIRST_LAYER)
                            ? default_er_speed
                            : full_print_config.get_computed_value(er_speed_ToOptionKey[selected_extrusion_role].c_str());
    flow_values.accel = full_print_config.get_computed_value(er_accel_ToOptionKey[selected_extrusion_role].c_str());
    return flow_values;
}

void CalibrationPressureAdvDialog::create_geometry(wxCommandEvent& event_args) {
   
    const CalibrationRoleChoices choice_extrusion_role = {
    ROLE_INTERNAL_INFILL,
    ROLE_BRIDGE_INFILL,
    ROLE_EXTERNAL_PERIMETER,
    ROLE_GAP_FILL,
    ROLE_INTERNAL_BRIDGE_INFILL,
    ROLE_IRONING,
    ROLE_OVERHANG_PERIMETER,
    ROLE_PERIMETER,
    ROLE_SOLID_INFILL,
    ROLE_SUPPORT_MATERIAL,
    ROLE_SUPPORT_MATERIAL_INTERFACE,
    ROLE_THIN_WALL,
    ROLE_TOP_SOLID_INFILL,
    ROLE_FIRST_LAYER//i've got added them all right?
    };

   std::unordered_map<std::string, std::string> er_width_ToOptionKey = {
    {ROLE_INTERNAL_INFILL, "infill_extrusion_width"},
    {ROLE_BRIDGE_INFILL, "extrusion_width"},
    {ROLE_EXTERNAL_PERIMETER, "external_perimeter_extrusion_width"},
    {ROLE_GAP_FILL, "extrusion_width"},
    {ROLE_INTERNAL_BRIDGE_INFILL, "extrusion_width"},
    {ROLE_IRONING, "top_infill_extrusion_width"},
    {ROLE_OVERHANG_PERIMETER, "overhangs_width"},// Tracked in #44: verify overhang width/flow handling.
    {ROLE_PERIMETER, "perimeter_extrusion_width"},
    {ROLE_SOLID_INFILL, "solid_infill_extrusion_width"},
    {ROLE_SUPPORT_MATERIAL, "support_material_extrusion_width"},// support material layer_height can go up/down depending on config.
    {ROLE_SUPPORT_MATERIAL_INTERFACE, "support_material_extrusion_width"},
    {ROLE_THIN_WALL, "thin_walls_min_width"},
    {ROLE_TOP_SOLID_INFILL, "top_infill_extrusion_width"},
    {ROLE_FIRST_LAYER, "first_layer_extrusion_width"}

    };

    std::unordered_map<std::string, std::string> er_accel_ToOptionKey = {
    {ROLE_INTERNAL_INFILL, "infill_acceleration"},
    {ROLE_BRIDGE_INFILL, "bridge_acceleration"},
    {ROLE_EXTERNAL_PERIMETER, "external_perimeter_acceleration"},
    {ROLE_GAP_FILL, "gap_fill_acceleration"},
    {ROLE_INTERNAL_BRIDGE_INFILL, "internal_bridge_acceleration"},
    {ROLE_IRONING, "ironing_acceleration"},
    {ROLE_OVERHANG_PERIMETER, "overhangs_acceleration"},
    {ROLE_PERIMETER, "perimeter_acceleration"},
    {ROLE_SOLID_INFILL, "solid_infill_acceleration"},
    {ROLE_SUPPORT_MATERIAL, "support_material_acceleration"},
    {ROLE_SUPPORT_MATERIAL_INTERFACE, "support_material_interface_acceleration"},
    {ROLE_THIN_WALL, "thin_walls_acceleration"},
    {ROLE_TOP_SOLID_INFILL, "top_solid_infill_acceleration"},
    {ROLE_FIRST_LAYER, "first_layer_acceleration"}
    };

    std::unordered_map<std::string, std::string> er_spacing_ToOptionKey = {
    {ROLE_INTERNAL_INFILL, "infill_extrusion_spacing"},
    {ROLE_BRIDGE_INFILL, kExtrusionSpacingKey}, //special calc required
    {ROLE_EXTERNAL_PERIMETER, "external_perimeter_extrusion_spacing"},
    {ROLE_GAP_FILL, kExtrusionSpacingKey},//special calc required for commented ones
    {ROLE_INTERNAL_BRIDGE_INFILL, kExtrusionSpacingKey}, //special calc required
    //{ROLE_IRONING, "ironing_spacing"}, // Tracked in #44: verify ironing spacing option type/availability.
    {ROLE_IRONING, "top_infill_extrusion_spacing"},
    {ROLE_OVERHANG_PERIMETER, kExtrusionSpacingKey},
    {ROLE_PERIMETER, "perimeter_extrusion_spacing"},
    {ROLE_SOLID_INFILL, "solid_infill_extrusion_spacing"},
    {ROLE_SUPPORT_MATERIAL, "support_material_spacing"}, // Tracked in #44: verify support spacing option availability.
    {ROLE_SUPPORT_MATERIAL_INTERFACE, "support_material_interface_spacing"}, // Tracked in #44: verify interface spacing option.
    {ROLE_THIN_WALL, "external_perimeter_extrusion_spacing"}, // Tracked in #44: verify thin-wall spacing approximation.
    {ROLE_TOP_SOLID_INFILL, "top_infill_extrusion_spacing"},
    {ROLE_FIRST_LAYER, "first_layer_extrusion_spacing"}
    };

    std::unordered_map<std::string, std::string> er_speed_ToOptionKey = {
    {ROLE_INTERNAL_INFILL, "infill_speed"},
    {ROLE_BRIDGE_INFILL, "bridge_speed"},
    {ROLE_EXTERNAL_PERIMETER, "external_perimeter_speed"},
    {ROLE_GAP_FILL, "gap_fill_speed"},
    {ROLE_INTERNAL_BRIDGE_INFILL, "internal_bridge_speed"},
    {ROLE_IRONING, "ironing_speed"},
    {ROLE_OVERHANG_PERIMETER, "overhangs_speed"},
    {ROLE_PERIMETER, "perimeter_speed"},
    {ROLE_SOLID_INFILL, "solid_infill_speed"},
    {ROLE_SUPPORT_MATERIAL, "support_material_speed"},
    {ROLE_SUPPORT_MATERIAL_INTERFACE, "support_material_interface_speed"},
    {ROLE_THIN_WALL, "thin_walls_speed"},
    {ROLE_TOP_SOLID_INFILL, "top_solid_infill_speed"},
    {ROLE_FIRST_LAYER, kFirstLayerSpeedKey}
    };

    std::vector<std::pair<std::vector<double>, int>> pa_results(currentTestCount);
    for (int id_item = 0; id_item < currentTestCount; id_item++) {
        pa_results[id_item] = calc_PA_values(id_item);
        if (pa_results[id_item].second <= 0 || pa_results[id_item].first.empty()) {
            return;
        }
    }


    Plater* plat = this->main_frame->plater();
    Model& model = plat->model();
    if (!plat->new_project(L("Pressure advance line calibration")))
        return;

    bool autocenter = gui_app->app_config->get("autocenter") == "1";
    if (autocenter) {
        //disable auto-center for this calibration.
        gui_app->app_config->set("autocenter", "0");
    }
    
    std::vector<std::string> items;
    for (int i = 0; i < currentTestCount; i++) {
        items.emplace_back((boost::filesystem::path(Slic3r::resources_dir()) / kCalibrationResourceDirectory / kFilamentPressureResourceDirectory / "base_plate.3mf").string());
    }
    std::vector<size_t> objs_idx = plat->load_files(items, LoadFileOption::LoadModel | LoadFileOption::DontUpdateDirs);
    assert(objs_idx.size() == size_t(currentTestCount));
    const DynamicPrintConfig* print_config = this->gui_app->get_tab(Preset::TYPE_FFF_PRINT)->get_config();
    const DynamicPrintConfig* filament_config = this->gui_app->get_tab(Preset::TYPE_FFF_FILAMENT)->get_config();
    const DynamicPrintConfig* printer_config = this->gui_app->get_tab(Preset::TYPE_PRINTER)->get_config();

    DynamicPrintConfig full_print_config;
    full_print_config.apply(*print_config);
    full_print_config.apply(*printer_config);
    full_print_config.apply(*filament_config);

    GCodeFlavor flavor = printer_config->option<ConfigOptionEnum<GCodeFlavor>>("gcode_flavor")->value;
    const ConfigOptionFloats* nozzle_diameter_config = printer_config->option<ConfigOptionFloats>("nozzle_diameter");
    assert(nozzle_diameter_config->size() > 0);
    double nozzle_diameter = nozzle_diameter_config->get_at(0);//get extruderID too?


    //double first_layer_height = full_print_config.get_computed_value("first_layer_height");
    double first_layer_height = full_print_config.get_abs_value("first_layer_height", nozzle_diameter);
    double first_layer_width = full_print_config.get_abs_value("first_layer_extrusion_width", nozzle_diameter);
    double first_layer_spacing = full_print_config.get_abs_value("first_layer_extrusion_spacing", nozzle_diameter);
    double infill_every_layers = full_print_config.get_computed_value("infill_every_layers");
    double support_material_layer_height = full_print_config.get_computed_value("support_material_layer_height");
    double support_material_interface_layer_height = full_print_config.get_computed_value("support_material_interface_layer_height");

    double base_layer_height = full_print_config.get_computed_value(kLayerHeightKey);
    double er_width = full_print_config.get_abs_value("solid_infill_extrusion_width", nozzle_diameter);
    double er_accel = full_print_config.get_computed_value("solid_infill_acceleration");
    double er_speed = full_print_config.get_computed_value("solid_infill_speed");
    double er_spacing = full_print_config.get_abs_value("external_perimeter_extrusion_spacing",nozzle_diameter);

    double default_er_width = full_print_config.get_abs_value("extrusion_width", nozzle_diameter);
    double default_er_speed = full_print_config.get_computed_value("default_speed");
    double default_er_accel = full_print_config.get_computed_value("default_acceleration");
    double default_er_spacing = full_print_config.get_abs_value(kExtrusionSpacingKey, nozzle_diameter);
    double perimeter_overlap = full_print_config.get_computed_value("perimeter_overlap");
    double external_perimeter_overlap = full_print_config.get_computed_value("external_perimeter_overlap");
    double filament_max_overlap = full_print_config.get_computed_value("filament_max_overlap",0);//maybe check for extruderID ?
    const int extruder_count = wxGetApp().extruders_edited_cnt();
    //const int ext_cnt = full_print_config.get_computed_value();
    double combined_layer_height = infill_every_layers * base_layer_height;
    double min_layer_height = full_print_config.get_computed_value("min_layer_height", extruder_count - extruder_count);//why is this now broken after i added multi extruder config then removed it. ??
    double max_layer_height = full_print_config.get_computed_value("max_layer_height", extruder_count - extruder_count);

    //double max_layer_heih = full_print_config.get_abs_value("min_layer_height", nozzle_diameter);
    //double min_layer_height, max_layer_height = 0.0;


    bool infill_dense = full_print_config.get_bool("infill_dense");
    if (combined_layer_height > nozzle_diameter){
        combined_layer_height = nozzle_diameter;
    }
    Flow first_layer_flow = Flow::new_from_config(FlowRole::frPerimeter, *print_config, nozzle_diameter, first_layer_height, 1.f, true);
    double default_first_layer_width = first_layer_flow.width();
    double default_first_layer_spacing = first_layer_flow.spacing();

    bool defaults_broken = false;
    if (default_er_width == 0 || default_er_spacing == 0) {
        // A missing default cannot safely seed Flow::new_from_config, so use the nozzle
        // diameter and the rounded-rectangle spacing formula as stable fallbacks.
        default_er_width = nozzle_diameter;
        default_er_spacing = default_er_width - base_layer_height * float(1. - 0.25 * PI) * external_perimeter_overlap;
        defaults_broken = true;
    }
    //what if defaults broken/not set for speed/accell too??

    auto role_layer_height = [&](const std::string& role_name) {
        if (role_name == ROLE_FIRST_LAYER)
            return first_layer_height;
        if (role_name == ROLE_INTERNAL_INFILL && !infill_dense && infill_every_layers > 1)
            return combined_layer_height;
        if (role_name == ROLE_SUPPORT_MATERIAL && support_material_layer_height > 0)
            return support_material_layer_height;
        if (role_name == ROLE_SUPPORT_MATERIAL_INTERFACE && support_material_interface_layer_height > 0)
            return support_material_interface_layer_height;
        // Every remaining role, including the bridge-like ones whose real pathing is variable,
        // is generated at the base layer height. There is no per-role override to apply here.
        return base_layer_height;
    };

    auto role_spacing_value = [&](const std::string& role_name, const std::string& spacing_key) {
        const ConfigOption *opt = print_config->option(spacing_key.c_str());
        if (opt == nullptr)
            return default_er_spacing;

        if (role_name == ROLE_SUPPORT_MATERIAL || role_name == ROLE_SUPPORT_MATERIAL_INTERFACE || role_name == ROLE_IRONING) {
            if (const auto *float_opt = dynamic_cast<const ConfigOptionFloat*>(opt))
                return float_opt->value;
        }

        const double spacing = print_config->get_abs_value(spacing_key.c_str(), nozzle_diameter);
        return spacing > 0 ? spacing : default_er_spacing;
    };


    // --- translate ---
    //bool autocenter = gui_app->app_config->get("autocenter") == "1";
    bool has_to_arrange = plat->config()->opt_float("init_z_rotate") != 0;
    has_to_arrange = true;

    
    /*if (!autocenter) {
        const ConfigOptionPoints* bed_shape = printer_config->option<ConfigOptionPoints>("bed_shape");
        Vec2d bed_size = BoundingBoxf(bed_shape->values).size();
        Vec2d bed_min = BoundingBoxf(bed_shape->values).min;
        model.objects[objs_idx[0]]->translate({ bed_min.x() + bed_size.x() / kGeometryCenterDivisor, bed_min.y() + bed_size.y() / kGeometryCenterDivisor, kModelPlacementOffset * xyzScale - kModelPlacementOffset });
    }*/
    

    std::vector < std::vector<ModelObject*>> pressure_tower;
    bool smooth_time = false;

    // The bend-90 calibration meshes exist only in 0.10 mm steps from 0.10 to 2.00
    // (resources/calibration/filament_pressure/scaled_with_nozzle_size). Snap the nozzle
    // diameter to the nearest available model and format with two decimals, so whole-mm
    // nozzles (1.0 -> "1.00") and odd nozzles (0.25 -> "0.20") resolve to a real file
    // instead of "1.0"/"0.250" which do not exist. The mesh is still XY-scaled by
    // magical_scaling() using the real nozzle diameter, so snapping only affects the
    // starting mesh, never the final geometry.
    double snapped_nozzle = std::round(nozzle_diameter / 0.1) * 0.1;
    if (snapped_nozzle < 0.10) snapped_nozzle = 0.10;
    if (snapped_nozzle > kMaximumCalibrationNozzleDiameter) snapped_nozzle = kMaximumCalibrationNozzleDiameter;
    char nozzle_diameter_buf[16];
    snprintf(nozzle_diameter_buf, sizeof(nozzle_diameter_buf), "%.2f", snapped_nozzle);
    std::string nozzle_diameter_str = nozzle_diameter_buf;

    std::string bend_90_nozzle_size_3mf = "90_bend_" + nozzle_diameter_str + ".3mf";
    std::string selected_extrusion_role = dynamicExtrusionRole[0]->GetValue().ToStdString();
    double initial_model_height = 0.2;
    //models is created per nozzles size 0.1-2mm walls are nozzle_size*4 thick
    //exported origin point is center of the model in xyz
    double initial_90_bend_x = 42.00;  //size in x= 42.0 mm, model half x=21.0
    double initial_90_bend_y = 21.0;   //size in y= 21.0 mm, model half y=10.5
    double initial_number_x  = 2.0;    //size in x= 2.0 mm , model half x=1.0
    double initial_number_y  = 4.0;    //size in y= 4.0 mm , model half y=2.0
    double initial_border_x  = 1.6;    //size in x= 1.6 mm , model half x=0.8
    double initial_border_y  = 21.0;   //size in y= 21.0 mm, model half y=10.5
    double initial_point_xy  = 0.60;   //size in xy= 0.6mm , model half xy=0.3 'point' model origin is center of the model (same as the numbers)
    double x_offset_90_bend  = 1.2;    //apex of 90° bend is offset from origin


    int count_numbers = 0;
    int count_borders = 0;
    std::vector<Eigen::Vector3d> bend_90_positions;
    std::vector<Eigen::Vector3d> number_positions;

    for (int id_item = 0; id_item < currentTestCount; id_item++) {

        count_numbers = 0;
        count_borders = 0;
        bend_90_positions.clear();
        number_positions.clear();
        
        auto pa_result = pa_results[id_item];
        std::vector<double> pa_values = pa_result.first;
        int count_increments = pa_result.second;
        selected_extrusion_role = dynamicExtrusionRole[id_item]->GetValue().ToStdString();
        if (selected_extrusion_role == ROLE_SUPPORT_MATERIAL && support_material_layer_height != 0){
            combined_layer_height = support_material_layer_height;
        }
        if (selected_extrusion_role == ROLE_INTERNAL_INFILL && infill_dense == false && infill_every_layers > 1){
            combined_layer_height = infill_every_layers * base_layer_height;
        }
        if (selected_extrusion_role == ROLE_SUPPORT_MATERIAL_INTERFACE && support_material_interface_layer_height !=0){
            combined_layer_height = support_material_interface_layer_height;
        }
        if (selected_extrusion_role == ROLE_FIRST_LAYER){
            combined_layer_height = first_layer_height;
        }
        /*
        double first_pa = wxAtof(firstPaValue);
        */

        if (selected_extrusion_role == ROLE_CHECK_ALL) {
            //count_increments = 13;
            count_increments = choice_extrusion_role.size();
            er_width = default_er_width;
            er_spacing = default_er_spacing;
            er_width = er_width * kPercentScale / nozzle_diameter;
            er_width = std::round(er_width * kPercentScale) / kPercentScale;

        }
        else{
            bool enable_switch = false;
            if(enable_switch == true){//still needs work :)
                CalibrationLayerHeights layer_heights;
                layer_heights.first_layer                = first_layer_height;
                layer_heights.base                       = base_layer_height;
                layer_heights.combined                   = combined_layer_height;
                layer_heights.minimum                    = min_layer_height;
                layer_heights.maximum                    = max_layer_height;
                layer_heights.support_material           = support_material_layer_height;
                layer_heights.support_material_interface = support_material_interface_layer_height;

                const CalibrationErFlow flow_values = compute_er_flow_by_role(choice_extrusion_role, selected_extrusion_role,
                                                                              *print_config, full_print_config,
                                                                              er_speed_ToOptionKey, er_accel_ToOptionKey,
                                                                              layer_heights, nozzle_diameter, filament_max_overlap,
                                                                              default_er_speed, infill_every_layers, infill_dense);
                er_width   = flow_values.width;
                er_spacing = flow_values.spacing;
                er_speed   = flow_values.speed;
                er_accel   = flow_values.accel;
            }

            if(enable_switch == false){
                // Everything below is derived from selected_extrusion_role alone, so walking every
                // entry of choice_extrusion_role just recomputed the same values once per role.
                if (er_width_ToOptionKey.find(selected_extrusion_role) != er_width_ToOptionKey.end()) {

                    //look at maps to match speed/width ect to the selected ER role
                    er_width = print_config->get_abs_value(er_width_ToOptionKey[selected_extrusion_role].c_str(), nozzle_diameter);
                    const ConfigOptionFloatOrPercent* first_layer_speed_option = dynamic_cast<const ConfigOptionFloatOrPercent*>(full_print_config.option(kFirstLayerSpeedKey));
                    er_speed = (first_layer_speed_option && first_layer_speed_option->percent && selected_extrusion_role == ROLE_FIRST_LAYER) ?
                        default_er_speed : full_print_config.get_computed_value(er_speed_ToOptionKey[selected_extrusion_role].c_str());
                    er_accel = full_print_config.get_computed_value(er_accel_ToOptionKey[selected_extrusion_role].c_str());
                    if (/*selected_extrusion_role == choice_extrusion_role[5] ||*/ selected_extrusion_role == choice_extrusion_role[9] || selected_extrusion_role == choice_extrusion_role[10]){//ironing, SupportMaterial, SupportMaterialInterface, 
                        er_spacing = print_config->option<ConfigOptionFloat>(er_spacing_ToOptionKey[selected_extrusion_role].c_str())->value;
                    }else{
                        er_spacing = print_config->get_abs_value(er_spacing_ToOptionKey[selected_extrusion_role].c_str(), nozzle_diameter);
                    }
                    first_layer_flow = Flow::new_from_config(FlowRole::frPerimeter, *print_config, nozzle_diameter, first_layer_height, 1.f, true);
                    first_layer_width = first_layer_flow.width();
                    first_layer_spacing = first_layer_flow.spacing();

                    // Tracked in #45: verify zero/default fallback handling for generated calibration values.
                    er_width = (er_width != 0) ? er_width : default_er_width;
                    er_speed = (er_speed != 0) ? er_speed : default_er_speed;
                    er_accel = (er_accel != 0) ? er_accel : default_er_accel;
                    er_spacing = (er_spacing != 0) ? er_spacing : default_er_spacing;
                    first_layer_width = (first_layer_width != 0) ? first_layer_width : first_layer_flow.width();
                    first_layer_spacing = (first_layer_spacing != 0) ? first_layer_spacing : first_layer_flow.spacing();

                    er_width = std::round((er_width * kPercentScale / nozzle_diameter) * kPercentScale) / kPercentScale;
                    first_layer_width = std::round((first_layer_width * kPercentScale / nozzle_diameter) * kPercentScale) / kPercentScale;

                } else {
                    er_width = print_config->get_abs_value("solid_infill_extrusion_width", nozzle_diameter); // Tracked in #44: add special flow handling for gap fill and bridges.
                    er_width = (er_width != 0) ? er_width : default_er_width;
                    er_width = std::round((er_width * kPercentScale / nozzle_diameter) * kPercentScale) / kPercentScale;
                    first_layer_width = default_first_layer_width;
                    first_layer_spacing = default_first_layer_spacing;
                    //er_width = er_width * kPercentScale / nozzle_diameter;
                    //er_width = std::round(er_width * kPercentScale) / kPercentScale;

                }
            }
            if(defaults_broken == true){//if their config is broken fix it :)
                default_er_width = nozzle_diameter;
                default_er_spacing = default_er_width - base_layer_height * float(1. - 0.25 * PI) * external_perimeter_overlap; //rounded_rectangle_extrusion_spacing
            }
        }


        //-- magical scaling is done here :)
        //the 90_bend models need to be scaled correctly so there is no 'gapfill' since gapfill will effect results.
        double adjustment_factor = first_layer_flow.width() - first_layer_flow.spacing();// Tracked in #45: verify this border adjustment calculation.

        double xyzScale = nozzle_diameter / 0.4;
        const double selected_role_layer_height = role_layer_height(selected_extrusion_role);
        double er_width_to_scale = magical_scaling(
            nozzle_diameter, er_width, perimeter_overlap, external_perimeter_overlap, selected_role_layer_height);
        double er_width_to_scale_first_layer_border = first_layer_flow.width() + 3 * first_layer_flow.spacing() + adjustment_factor;//total_width_with_overlap

        //-- magical scaling 
        pressure_tower.emplace_back();

        // Z CENTER for the embossed numbers/points. Their span is first_layer_height + base_layer_height
        // so the digits always slice into the first layer plus >= 1 base layer and render regardless of
        // the first/base layer-height relationship. Previously this was tied to first_layer_height alone,
        // which collapsed the digits and made them not render when first_layer_height <= base_layer_height
        // (e.g. both 0.02). See #38.
        double z_scaled_model_height = (first_layer_height + base_layer_height) / kGeometryCenterDivisor; //mm
        double xy_scaled_90_bend_x = initial_90_bend_x * er_width_to_scale;             // mm
        double xy_scaled_90_bend_y = initial_90_bend_y * er_width_to_scale;             // mm
        //double first_layer_xy_scaled_90_bend_x = initial_90_bend_x * er_width_to_scale_first_layer; // mm for 90_bend width scaled for first_layer prob not needed?
        //double first_layer_xy_scaled_90_bend_y = initial_90_bend_y * er_width_to_scale_first_layer; // mm for 90_bend width scaled for first_layer prob not needed?
        double xy_scaled_border_x = er_width_to_scale_first_layer_border;               // mm
        double xy_scaled_border_y = er_width_to_scale_first_layer_border;               // mm
        double xy_scaled_number_x = initial_number_x * xyzScale * er_width_to_scale;    // mm
        double xy_scaled_number_y = initial_number_y * xyzScale * er_width_to_scale;    // mm
        double xy_scaled_point_y =  initial_point_xy * xyzScale * 1.5 * er_width_to_scale;    // mm the 'point' model gets scaled a litte larger in y to help with gcode generation and actually printing it.


        double thickness_offset = 0.0;
        double bend_90_y_pos = 0.0;
        double z_scale_90_bend = (first_layer_height + (base_layer_height * 4)) / initial_model_height;//force constant 5 layer height for model
        double z_90_bend_pos = (first_layer_height + (base_layer_height * 4)) / kGeometryCenterDivisor;
        double z_scale_others = first_layer_height / initial_model_height;
        double z_others_pos = first_layer_height / kGeometryCenterDivisor;
        // Z scale that prints the number/point models (native height initial_model_height) at
        // first_layer_height + base_layer_height tall, matching z_scaled_model_height's span. See #38.
        double z_scale_numbers = (first_layer_height + base_layer_height) / initial_model_height;
        for (int nb_90_bends = 0; nb_90_bends < count_increments; nb_90_bends++) {
            std::string er_role = selected_extrusion_role;
            double y_offset = 0.0;
            bool role_found = false;

            if (selected_extrusion_role == ROLE_CHECK_ALL) {
                y_offset = 10.0;
                er_role = choice_extrusion_role[nb_90_bends];
                role_found = (er_width_ToOptionKey.find(er_role) != er_width_ToOptionKey.end());
            } else {
                role_found = (er_width_ToOptionKey.find(er_role) != er_width_ToOptionKey.end());
            }

            if (role_found == true) {
                er_width =   print_config->get_abs_value(er_width_ToOptionKey[er_role].c_str(), nozzle_diameter);
                er_spacing = role_spacing_value(er_role, er_spacing_ToOptionKey[er_role]);

                er_width = (er_width != 0) ? er_width : default_er_width;//found supported role but it has 0 value, need to give it defaults.
                er_spacing = (er_spacing != 0) ? er_spacing : default_er_spacing;

            } else {
                er_width = default_er_width;
                er_spacing = default_er_spacing;
            }

            er_width = std::round((er_width * kPercentScale / nozzle_diameter) * kPercentScale) / kPercentScale;
            const std::string active_role = (selected_extrusion_role == ROLE_CHECK_ALL) ? er_role : selected_extrusion_role;
            const double active_layer_height = role_layer_height(active_role);
            er_width_to_scale = magical_scaling(
                nozzle_diameter, er_width, perimeter_overlap, external_perimeter_overlap, active_layer_height);
            if (active_layer_height != base_layer_height) {
                z_90_bend_pos = (first_layer_height + (active_layer_height * kActiveCalibrationLayerCount)) / kGeometryCenterDivisor;
                z_scale_90_bend = (first_layer_height + (active_layer_height * kActiveCalibrationLayerCount)) / initial_model_height;
            }


            add_part(model.objects[objs_idx[id_item]], 
                (boost::filesystem::path(Slic3r::resources_dir()) / kCalibrationResourceDirectory / kFilamentPressureResourceDirectory / "scaled_with_nozzle_size" / bend_90_nozzle_size_3mf).string(),
                    Vec3d{ x_offset_90_bend, bend_90_y_pos , z_90_bend_pos }, 
                    /*scale*/Vec3d{ er_width_to_scale, er_width_to_scale, z_scale_90_bend }, false);

            pressure_tower.back().push_back(model.objects[objs_idx[id_item]]);
            Eigen::Vector3d modelPosition( x_offset_90_bend, bend_90_y_pos + y_offset , z_90_bend_pos );

            // thickness offset that moves each '90_bend' model in Y
            //thickness_offset = ((er_width / kPercentScale) * nozzle_diameter) * 4 + (nozzle_diameter * kDoubleNozzleDiameterScale);// pretty tight gap
            //thickness_offset = ((er_width / kPercentScale) * nozzle_diameter) * 4 + (nozzle_diameter * kDoubleNozzleDiameterScale.5);
            thickness_offset = ((er_width / kPercentScale) * nozzle_diameter) * 4 + (nozzle_diameter * 4);// larger gap
           
            //thickness_offset = ((er_width / kPercentScale) * nozzle_diameter * xy_scaled_90_bend_y * 4) + (nozzle_diameter * 4);
            //double scaled_thickness_offset = ((xy_scaled_90_bend_x - initial_90_bend_x) + (xy_scaled_90_bend_y - initial_90_bend_y));
            //double real_offset = thickness_offset + scaled_thickness_offset;

            /*thickness_offset = ((er_width / kPercentScale) * nozzle_diameter * 4) + (nozzle_diameter * 4);

            double scaled_thickness_offset = thickness_offset * (((xy_scaled_90_bend_x / initial_90_bend_x) + (xy_scaled_90_bend_y / initial_90_bend_y)) / kGeometryCenterDivisor - 1);
            double real_offset = thickness_offset + scaled_thickness_offset;*/

            bend_90_positions.push_back(modelPosition);
            bend_90_y_pos = modelPosition.y() + thickness_offset;
            //bend_90_y_pos = modelPosition.y() + real_offset;
            
            
        }
    
        for (int nb_bends = 0; nb_bends < count_increments;nb_bends++){

            if(nb_bends == 1 && selected_extrusion_role != ROLE_CHECK_ALL) {// only load once. this only determines when the borders get loaded, keeping at top of list makes it easier to scroll down to. it can't be '0' since it needs the numbers
                                                                            // positions!

                Eigen::Vector3d bend_pos_first = bend_90_positions[0];
                Eigen::Vector3d bend_pos_mid = bend_90_positions[count_increments / kMiddleIndexDivisor];
                Eigen::Vector3d bend_pos_last = bend_90_positions[count_increments-1];
                // True vertical centre of the bend stack. The left/right borders must be centred
                // here, NOT on bend_pos_mid (the middle-INDEX bend): that index only equals the
                // centre for odd counts. For even counts it sits half a bend-pitch high, shifting
                // the side borders up and leaving the bottom border disconnected (#46).
                const double tower_center_y = (bend_pos_first.y() + bend_pos_last.y()) / kGeometryCenterDivisor;

                Eigen::Vector3d number_pos_first = number_positions[0];
                Eigen::Vector3d number_pos_mid = number_positions[0];
                Eigen::Vector3d number_pos_last = number_positions[0];


                if (!number_positions.empty()) {
                    // The loop this replaces walked every entry only to keep the middle one, the
                    // last one, and a count. kMiddleIndexDivisor is 2, so size/2 is always in range.
                    number_pos_mid  = number_positions[number_positions.size() / kMiddleIndexDivisor];
                    number_pos_last = number_positions.back();
                    count_numbers += int(number_positions.size());
                }

                // Tracked in #46: odd/uneven PA value arrays can leave borders too short when
                // the forced final end_pa value adds an extra 90-degree bend model.

                // Scaled to include the gap between the end of the 90_bend and the first number.
                double numbers_total_width =
                    (number_pos_last.x() + (xy_scaled_number_x / kGeometryCenterDivisor)) -
                    (number_pos_first.x() - (xy_scaled_number_x / kGeometryCenterDivisor));
                double total_height = (bend_pos_last.y() + (xy_scaled_90_bend_y / kGeometryCenterDivisor)) - (bend_pos_first.y() - (xy_scaled_90_bend_y / kGeometryCenterDivisor));
                double scalred_r_border_x_mm = numbers_total_width + (nozzle_diameter * kDoubleNozzleDiameterScale);
                // The left border sits slightly inside the 90_bend model; this is that distance.
                double left_border_x_offset =
                    (bend_pos_mid.x() - (xy_scaled_90_bend_x / kGeometryCenterDivisor) - nozzle_diameter +
                        (xy_scaled_border_x / kGeometryCenterDivisor)) -
                    (bend_pos_mid.x() - (xy_scaled_90_bend_x / kGeometryCenterDivisor));
                double tb_total_width_mm = (xy_scaled_border_x - left_border_x_offset) + xy_scaled_90_bend_x + scalred_r_border_x_mm;
                
                double scaled_l_border_x_percentage  = xy_scaled_border_x / initial_border_x;
                double scaled_r_border_x_percentage  = (numbers_total_width + (nozzle_diameter * kDoubleNozzleDiameterScale)) / initial_border_x ;
                double scaled_lr_border_y_percentage = (total_height + xy_scaled_border_y) / initial_90_bend_y;
                double scaled_tb_border_x_percentage = tb_total_width_mm  / initial_border_x;
                double scaled_tb_border_y_percentage  = xy_scaled_border_y / initial_border_y;
                
                double left_border_x_pos = bend_pos_mid.x() - (xy_scaled_90_bend_x / kGeometryCenterDivisor) - nozzle_diameter;
                double right_border_x_pos = bend_pos_mid.x() + (xy_scaled_90_bend_x / kGeometryCenterDivisor) + (scalred_r_border_x_mm / kGeometryCenterDivisor);

                double left_edge_pos = bend_pos_mid.x() - (xy_scaled_90_bend_x / kGeometryCenterDivisor) - xy_scaled_border_x + left_border_x_offset;
                double right_edge_pos = (xy_scaled_90_bend_x / kGeometryCenterDivisor) + scalred_r_border_x_mm + bend_pos_mid.x();
                double center = (left_edge_pos + right_edge_pos) / kGeometryCenterDivisor;
                double tb_border_x_pos = center;


                add_part(model.objects[objs_idx[id_item]], (boost::filesystem::path(Slic3r::resources_dir()) / kCalibrationResourceDirectory / kFilamentPressureResourceDirectory / kPressureAdvanceBorderResource).string(),
                        Vec3d{ left_border_x_pos , tower_center_y, z_others_pos },
                        /*scale*/Vec3d{ scaled_l_border_x_percentage, scaled_lr_border_y_percentage, z_scale_others }, false);count_borders++;         //Left border
                
                add_part(model.objects[objs_idx[id_item]], (boost::filesystem::path(Slic3r::resources_dir()) / kCalibrationResourceDirectory / kFilamentPressureResourceDirectory / kPressureAdvanceBorderResource).string(),
                    Vec3d{ right_border_x_pos , tower_center_y, z_others_pos },
                        /*scale*/Vec3d{ scaled_r_border_x_percentage , scaled_lr_border_y_percentage , z_scale_others}, false);count_borders++;        //right border
                

                // Tracked in #46: odd increment counts can leave the bottom border disconnected.
                add_part(model.objects[objs_idx[id_item]], (boost::filesystem::path(Slic3r::resources_dir()) / kCalibrationResourceDirectory / kFilamentPressureResourceDirectory / kPressureAdvanceBorderResource).string(),
                    // Border offsets/scale need shared handling with the top and side calculations.
                    Vec3d{ tb_border_x_pos,
                           bend_pos_first.y() - (xy_scaled_90_bend_y / kGeometryCenterDivisor) -
                               (xy_scaled_border_y / kGeometryCenterDivisor) - nozzle_diameter,
                           z_others_pos },
                        /*scale*/Vec3d{ scaled_tb_border_x_percentage , scaled_tb_border_y_percentage, z_scale_others }, false);count_borders++;       //bottom border
                //----------
                add_part(model.objects[objs_idx[id_item]], (boost::filesystem::path(Slic3r::resources_dir()) / kCalibrationResourceDirectory / kFilamentPressureResourceDirectory / kPressureAdvanceBorderResource).string(),
                    Vec3d{ tb_border_x_pos , bend_pos_last.y() + (xy_scaled_90_bend_y / kGeometryCenterDivisor) + (xy_scaled_border_y / kGeometryCenterDivisor) + nozzle_diameter, z_others_pos },
                        /*scale*/Vec3d{ scaled_tb_border_x_percentage, scaled_tb_border_y_percentage, z_scale_others}, false);count_borders++;         //top border
                //  scale model in percentage from original models xy values!


                // Only label the run with its ID when there is more than one run to tell apart.
                // For a single run the ID "0" is redundant and collides with the PA=0 value label (#47).
                if (currentTestCount > 1 && id_item < 10){ //will break if max test count goes higher. ie currentTestCount
                    add_part(model.objects[objs_idx[id_item]],(boost::filesystem::path(Slic3r::resources_dir()) / kCalibrationResourceDirectory / kFilamentPressureResourceDirectory / (std::to_string(id_item) + std::string(".3mf"))).string(),
                        Vec3d{ number_pos_mid.x(), bend_pos_first.y() - (xy_scaled_90_bend_y / kGeometryCenterDivisor) + (xy_scaled_number_y / kGeometryCenterDivisor), z_scaled_model_height },
                            /*scale*/Vec3d{ xyzScale * er_width_to_scale, xyzScale * er_width_to_scale, z_scale_numbers }, false);count_borders++;      // currentTestCount identifer
                    // Record the run-ID digit so the number base plate (sized from number_positions
                    // below) extends to cover it, instead of leaving it orphaned on the border (#47).
                    number_positions.push_back(Eigen::Vector3d(number_pos_mid.x(), bend_pos_first.y() - (xy_scaled_90_bend_y / kGeometryCenterDivisor) + (xy_scaled_number_y / kGeometryCenterDivisor), z_scaled_model_height));
                }
            }


            if (selected_extrusion_role != ROLE_CHECK_ALL) {// Tracked in #47: consider role-name labels next to each 90-degree bend.

                if (nb_bends % kParityDivisor == 1){
                    continue;// Skip generating every second number
                }
                // Tracked in #47: validate normal-size vs thin-wall-specific label geometry.

                Eigen::Vector3d bend_90_pos = bend_90_positions[nb_bends];
                const std::string pa_values_string = format_pa_label_value(pa_values[nb_bends]);

                double xpos = bend_90_pos.x() + (xy_scaled_90_bend_x / kGeometryCenterDivisor) + (xy_scaled_number_x / kGeometryCenterDivisor) + nozzle_diameter;
                //double ypos = bend_90_pos.y() + (xy_scaled_90_bend_y / kGeometryCenterDivisor) - (xy_scaled_number_y / kGeometryCenterDivisor)
                double ypos = bend_90_pos.y() + (xy_scaled_90_bend_y / kGeometryCenterDivisor) - (xy_scaled_number_y / kGeometryCenterDivisor) + (nozzle_diameter * 3);
                double space_numbers_distance_x = (xy_scaled_number_x / kGeometryCenterDivisor) + nozzle_diameter + (xy_scaled_number_x / kGeometryCenterDivisor);//space the numbers by this amount.
                // Tracked in #47: tune label Y position so it is centered with the 90-degree bend notch.

                for (size_t j = 0; j < pa_values_string.length(); ++j) {//not sure how the code will respond with a positive array list? ie ; 100.2 this moves decimal point thus breaking the code from loading model since "..3mf" not a real file

                    if (j != 0  ) {//don't apply the offset for first number
                        xpos = xpos + space_numbers_distance_x;}
                    if (pa_values_string[j] == '.') { // maybe if ! isdigit(pa_values_string[j]) if it's not a '.' it could be ',' part fix for localization issue. but also this character could be anything else..(it shouldn't though..) and it
                                                      // shouldn't be 'fixed' here..

                        double right_edge_of_left_number = xpos + (xy_scaled_number_x / kGeometryCenterDivisor) - space_numbers_distance_x;//this can be simplified,values represent the inner edges of the numbers between the '.' model
                        double left_edge_of_right_number = xpos + (xy_scaled_number_x / kGeometryCenterDivisor) + (nozzle_diameter * kDoubleNozzleDiameterScale) + (xy_scaled_number_x / kGeometryCenterDivisor) - space_numbers_distance_x;
                        double point_xpos = (right_edge_of_left_number + left_edge_of_right_number) / kGeometryCenterDivisor;

                        add_part(model.objects[objs_idx[id_item]],(boost::filesystem::path(Slic3r::resources_dir()) / kCalibrationResourceDirectory / kFilamentPressureResourceDirectory / "point.3mf").string(),
                            // FIXED: the point moved to the wrong position on every nozzle_size,
                            // because its exported offset position is not scaled with the model.
                            Vec3d{ point_xpos,
                                   ypos - (xy_scaled_number_y / kGeometryCenterDivisor) +
                                       (xy_scaled_point_y / kGeometryCenterDivisor),
                                   z_scaled_model_height },
                                /*scale*/Vec3d{ xyzScale * er_width_to_scale, (xyzScale + (xyzScale / kGeometryCenterDivisor)) * er_width_to_scale, z_scale_numbers }, false);
                        number_positions.push_back(Eigen::Vector3d(point_xpos, ypos - (xy_scaled_number_y / kGeometryCenterDivisor) + (xy_scaled_point_y / kGeometryCenterDivisor), z_scaled_model_height));
                        xpos -= (xy_scaled_number_x / kGeometryCenterDivisor);

                    } else if (std::isdigit(pa_values_string[j])) {
                        add_part(model.objects[objs_idx[id_item]],(boost::filesystem::path(Slic3r::resources_dir()) / kCalibrationResourceDirectory / kFilamentPressureResourceDirectory / (pa_values_string[j] + std::string(".3mf"))).string(),
                            Vec3d{ xpos, ypos, z_scaled_model_height },
                                /*scale*/Vec3d{ xyzScale * er_width_to_scale, xyzScale * er_width_to_scale, z_scale_numbers }, false);//TOCHECK: if any numbers get gapfill
                        number_positions.push_back(Eigen::Vector3d(xpos, ypos, z_scaled_model_height));
                        xpos = number_positions.back().x();
                    }
                }
            }
        }

        // First-layer base under the number column (#47). The embossed digits otherwise
        // print as loose characters straight on the bed, with a tiny first-layer footprint, so
        // they adhere poorly and are a pain to remove. Add one flat plate sized from the actual
        // digit positions, one layer tall: it merges into the digits' own first layer, so the
        // raised upper layers still show the numbers but the whole label strip lifts off as one
        // attached piece. Sits to the right of the bends (over the number column only), so it
        // does not touch the test geometry. Skipped for CheckAll (no per-bend numbers there).
        if (selected_extrusion_role != ROLE_CHECK_ALL && number_positions.size() > 1) {
            double nx_min = number_positions[0].x(), nx_max = number_positions[0].x();
            double ny_min = number_positions[0].y(), ny_max = number_positions[0].y();
            for (const Eigen::Vector3d& np : number_positions) {
                nx_min = std::min(nx_min, np.x()); nx_max = std::max(nx_max, np.x());
                ny_min = std::min(ny_min, np.y()); ny_max = std::max(ny_max, np.y());
            }
            // Extra room to the left/right of the digits so they aren't flush with the pad edge.
            const double base_margin_x = nozzle_diameter * 8;
            const double base_margin_y = nozzle_diameter * 3;
            const double base_size_x = (nx_max - nx_min) + xy_scaled_number_x + base_margin_x;
            const double base_size_y = (ny_max - ny_min) + xy_scaled_number_y + base_margin_y;
            add_part(model.objects[objs_idx[id_item]], (boost::filesystem::path(Slic3r::resources_dir()) / kCalibrationResourceDirectory / kFilamentPressureResourceDirectory / kPressureAdvanceBorderResource).string(),
                Vec3d{ (nx_min + nx_max) / kGeometryCenterDivisor, (ny_min + ny_max) / kGeometryCenterDivisor, z_others_pos },
                /*scale*/Vec3d{ base_size_x / initial_border_x, base_size_y / initial_border_y, z_scale_others }, false);count_borders++;       //number base plate
            // The pad only needs to tie the digits together for handling, so print it with the
            // lightest fill: sparse infill, no solid top/bottom shells, a single perimeter.
            if (ModelVolume* base_vol = model.objects[objs_idx[id_item]]->volumes.back()) {
                base_vol->config.set_key_value("bottom_solid_layers", std::make_unique<ConfigOptionInt>(0));
                base_vol->config.set_key_value("top_solid_layers", std::make_unique<ConfigOptionInt>(0));
                base_vol->config.set_key_value("perimeters", std::make_unique<ConfigOptionInt>(1));
                base_vol->config.set_key_value("fill_density", std::make_unique<ConfigOptionPercent>(80));
            }
        }
    }

    /// --- main config ---
    // => settings that are for object or region should be added to the model (see below, in the for loop), not here
    DynamicPrintConfig new_print_config = *print_config;
    DynamicPrintConfig new_printer_config = *printer_config;
    DynamicPrintConfig new_filament_config = *filament_config;
    //check if setting any config values to 45° breaks it. or it might be the default value for rotation adding part?
    new_print_config.set_key_value("avoid_crossing_perimeters", std::make_unique<ConfigOptionBool>(false));
    new_print_config.set_key_value("complete_objects", std::make_unique<ConfigOptionBool>(false)); //true is required for multi tests on single plate?
    new_print_config.set_key_value("first_layer_flow_ratio", std::make_unique<ConfigOptionPercent>(kDefaultFlowPercent));
    new_print_config.set_key_value("first_layer_size_compensation", std::make_unique<ConfigOptionFloat>(0));
    new_print_config.set_key_value("xy_inner_size_compensation", std::make_unique<ConfigOptionFloat>(0));
    new_print_config.set_key_value("xy_outer_size_compensation", std::make_unique<ConfigOptionFloat>(0));
    auto filament_pa = std::make_unique<ConfigOptionFloats>(std::initializer_list<double>{0.00});
    filament_pa->set_can_be_disabled(true);
    new_filament_config.set_key_value("filament_pressure_advance", std::move(filament_pa));
    new_print_config.set_key_value("print_custom_variables", std::make_unique<ConfigOptionString>("calibration_print"));//created this as an extra check for when generating gcode to not include "feature_gcode"
                                                                                                          // unless i disable the "generate" button if the keywords are detected in the custom gcode ?


    //assert(filament_temp_item_name.size() == nb_runs);
    //assert(model.objects.size() == nb_runs);
    assert(objs_idx.size() == size_t(currentTestCount));
    for (int id_item = 0; id_item < currentTestCount; id_item++) {

        auto pa_result = pa_results[id_item];
        std::vector<double> pa_values = pa_result.first;
        int count_increments = pa_result.second;

        wxString firstPaValue = dynamicFirstPa[id_item]->GetValue();
        firstPaValue.Replace(",", ".");
        double first_pa = wxAtof(firstPaValue);
        smooth_time = dynamicEnableST.size() > size_t(id_item) ? dynamicEnableST[id_item]->GetValue() : 0;
        selected_extrusion_role = dynamicExtrusionRole[id_item]->GetValue().ToStdString();

        if (selected_extrusion_role == ROLE_CHECK_ALL) {// have to keep it in range
            count_increments = choice_extrusion_role.size();
        }
        if (selected_extrusion_role == ROLE_SUPPORT_MATERIAL && support_material_layer_height != 0){
            combined_layer_height = support_material_layer_height;
        }
        if (selected_extrusion_role == ROLE_SUPPORT_MATERIAL_INTERFACE && support_material_interface_layer_height !=0){
            combined_layer_height = support_material_interface_layer_height;
        }
        else if (selected_extrusion_role == ROLE_INTERNAL_INFILL && infill_dense == false && infill_every_layers > 1){
            combined_layer_height = infill_every_layers * base_layer_height;
        }
        else if(selected_extrusion_role == ROLE_FIRST_LAYER){
            combined_layer_height = first_layer_height;
        }

        auto last_90_bend_scale = model.objects[objs_idx[id_item]]->volumes[count_increments]->get_scaling_factor();
        Eigen::Vector3d bend_90_mesh = model.objects[objs_idx[id_item]]->volumes[count_increments]->mesh().size();
        double model_height = bend_90_mesh.z() * last_90_bend_scale.z();

        const std::string set_first_layer_prefix = pressure_advance_prefix(flavor, false);
        std::string first_layer_region_prefix = "{if layer_z <= " + std::to_string(first_layer_height) + "}" + set_first_layer_prefix + std::to_string(first_pa) + "; first layer [layer_z] {endif}";

        /*
        gcfRepRap,
        gcfSprinter,
        gcfRepetier,
        gcfTeacup,
        gcfMakerWare,
        gcfMarlinLegacy,
        gcfMarlinFirmware,
        gcfLerdge,
        gcfKlipper,
        gcfSailfish,
        gcfMach3,
        gcfMachinekit,
        gcfSmoothie,
        gcfNoExtrusion*/

        // Name each tower so they're identifiable on the build plate and in the object list
        model.objects[objs_idx[id_item]]->name = "PA Line Calibration " + std::to_string(id_item) + " - " + selected_extrusion_role;

        // config modifers for the base model
        model.objects[objs_idx[id_item]]->config.set_key_value("bottom_fill_pattern", std::make_unique<ConfigOptionEnum<InfillPattern>>(ipMonotonic));// ipConcentric or ipConcentricGapFill ?
        model.objects[objs_idx[id_item]]->config.set_key_value("infill_filled_bottom", std::make_unique<ConfigOptionBool>(true));
        model.objects[objs_idx[id_item]]->config.set_key_value("thin_walls", std::make_unique<ConfigOptionBool>(true));
        model.objects[objs_idx[id_item]]->config.set_key_value("bottom_solid_layers", std::make_unique<ConfigOptionInt>(1));
        model.objects[objs_idx[id_item]]->config.set_key_value("brim_width", std::make_unique<ConfigOptionFloat>(0));
        //model.objects[objs_idx[id_item]]->config.set_key_value("external_perimeter_overlap", std::make_unique<ConfigOptionPercent>(kDefaultFlowPercent));//
        model.objects[objs_idx[id_item]]->config.set_key_value("fill_density", std::make_unique<ConfigOptionPercent>(0));
        const bool is_check_all = (selected_extrusion_role == ROLE_CHECK_ALL);
        model.objects[objs_idx[id_item]]->config.set_key_value("gap_fill_enabled", std::make_unique<ConfigOptionBool>(!is_check_all));
        if (is_check_all) {
            // Keep geometry deterministic for role comparison runs.
            model.objects[objs_idx[id_item]]->config.set_key_value("gap_fill_extension", std::make_unique<ConfigOptionFloat>(0));
            model.objects[objs_idx[id_item]]->config.set_key_value("thin_walls", std::make_unique<ConfigOptionBool>(false));
        }
        model.objects[objs_idx[id_item]]->config.set_key_value("min_width_top_surface", std::make_unique<ConfigOptionFloatOrPercent>(0.0,false));
        model.objects[objs_idx[id_item]]->config.set_key_value("only_one_perimeter_top", std::make_unique<ConfigOptionBool>(false));
        // , if borderers - right are scaled correctly there shouldn't be any gap fill in them.
        // it would be nice to keep the *4 extrusion lines for the borders only.
        model.objects[objs_idx[id_item]]->config.set_key_value("only_one_perimeter_first_layer", std::make_unique<ConfigOptionBool>(false));
        //model.objects[objs_idx[id_item]]->config.set_key_value("perimeter_overlap", std::make_unique<ConfigOptionPercent>(kDefaultFlowPercent));//
        model.objects[objs_idx[id_item]]->config.set_key_value("seam_position", std::make_unique<ConfigOptionEnum<SeamPosition>>(spRear)); // Tracked in #48: verify seam_position dirty-state/UI marking.
        model.objects[objs_idx[id_item]]->config.set_key_value("top_solid_layers", std::make_unique<ConfigOptionInt>(0));
        model.objects[objs_idx[id_item]]->config.set_key_value("region_gcode", std::make_unique<ConfigOptionString>(first_layer_region_prefix + " \n" ));

        // Layer ranges replace the object's default {0, 2} range rather than adding a second
        // overlapping one: an added range left the object list's tree out of sync with the model,
        // which is the malformed subtree ObjectDataViewModel::Delete still guards against (#49).
        if (selected_extrusion_role != ROLE_CHECK_ALL) {//don't apply layer ranges to the main object for CheckAll mode(option isn't supported.and it needs to be!!)
            {
                if (infill_every_layers > 1 && selected_extrusion_role == ROLE_INTERNAL_INFILL && infill_dense == false) {

                    wxGetApp().obj_list()->layers_editing(id_item);//could prob use this same thing for the unsupported roles since they need a different layer_height/width
                    auto existing_range = model.objects[objs_idx[id_item]]->layer_config_ranges.find(std::pair<double, double>(0.0f, 2.0f));// Find the default existing range {0.0f, 2.0f}

                    if (existing_range != model.objects[objs_idx[id_item]]->layer_config_ranges.end()) {
                        ModelConfig new_range_conf = existing_range->second;

                        new_range_conf.set_key_value(kLayerHeightKey, std::make_unique<ConfigOptionFloatOrPercent>(combined_layer_height, false));
                        model.objects[objs_idx[id_item]]->layer_config_ranges.erase(existing_range);
                        model.objects[objs_idx[id_item]]->layer_config_ranges[std::pair<double, double>(first_layer_height, model_height + first_layer_height)] = new_range_conf;
                    }
                }
                if ((selected_extrusion_role == ROLE_SUPPORT_MATERIAL && support_material_layer_height != 0) || (selected_extrusion_role == ROLE_SUPPORT_MATERIAL_INTERFACE && support_material_interface_layer_height != 0)) {

                    wxGetApp().obj_list()->layers_editing(id_item);//could prob use this same thing for the unsupported roles since they need a different layer_height/width
                    auto existing_range = model.objects[objs_idx[id_item]]->layer_config_ranges.find(std::pair<double, double>(0.0f, 2.0f));// Find the default existing range {0.0f, 2.0f}

                    if (existing_range != model.objects[objs_idx[id_item]]->layer_config_ranges.end()) {
                        ModelConfig new_range_conf = existing_range->second;

                        new_range_conf.set_key_value(kLayerHeightKey, std::make_unique<ConfigOptionFloatOrPercent>(combined_layer_height, false));
                        model.objects[objs_idx[id_item]]->layer_config_ranges.erase(existing_range);
                        model.objects[objs_idx[id_item]]->layer_config_ranges[std::pair<double, double>(first_layer_height, model_height + first_layer_height)] = new_range_conf;
                    }
                }
                if (selected_extrusion_role == ROLE_FIRST_LAYER ) {

                    wxGetApp().obj_list()->layers_editing(id_item);//could prob use this same thing for the unsupported roles since they need a different layer_height/width
                    auto existing_range = model.objects[objs_idx[id_item]]->layer_config_ranges.find(std::pair<double, double>(0.0f, 2.0f));// Find the default existing range {0.0f, 2.0f}

                    if (existing_range != model.objects[objs_idx[id_item]]->layer_config_ranges.end()) {
                        ModelConfig new_range_conf = existing_range->second;

                        new_range_conf.set_key_value(kLayerHeightKey, std::make_unique<ConfigOptionFloatOrPercent>(combined_layer_height, false));
                        model.objects[objs_idx[id_item]]->layer_config_ranges.erase(existing_range);
                        model.objects[objs_idx[id_item]]->layer_config_ranges[std::pair<double, double>(first_layer_height, model_height + first_layer_height)] = new_range_conf;
                    }
                }
            }
        }
        size_t num_part = 0;
        const int extra_vol = 1;
        for (; num_part < pressure_tower[id_item].size(); ++num_part) {

            std::string er_role = selected_extrusion_role;
            bool role_found = false;
            if (selected_extrusion_role == ROLE_CHECK_ALL) {
                if (num_part < choice_extrusion_role.size())
                    er_role = choice_extrusion_role[num_part];
                role_found = (er_width_ToOptionKey.find(er_role) != er_width_ToOptionKey.end());
            } else {
                role_found = (er_width_ToOptionKey.find(er_role) != er_width_ToOptionKey.end());
            }

            if (role_found == true /*&& defaults_broken == false*/) {
                er_width = print_config->get_abs_value(er_width_ToOptionKey[er_role].c_str(), nozzle_diameter);
                const ConfigOptionFloatOrPercent* first_layer_speed_option = dynamic_cast<const ConfigOptionFloatOrPercent*>(full_print_config.option(kFirstLayerSpeedKey));
                er_speed = (first_layer_speed_option && first_layer_speed_option->percent && er_role == ROLE_FIRST_LAYER) ? default_er_speed : full_print_config.get_computed_value(er_speed_ToOptionKey[er_role].c_str());
                er_accel = full_print_config.get_computed_value(er_accel_ToOptionKey[er_role].c_str(), nozzle_diameter);
                er_spacing = role_spacing_value(er_role, er_spacing_ToOptionKey[er_role]);

                er_width = (er_width != 0) ? er_width : default_er_width;
                er_speed = (er_speed != 0) ? er_speed : default_er_speed;
                er_accel = (er_accel != 0) ? er_accel : default_er_accel;
                er_spacing = (er_spacing != 0) ? er_spacing : default_er_spacing;

            } else {
                //instead of loading defaults for everything only load defaults for broken/unsupported values.
                er_width = default_er_width;
                er_speed = default_er_speed;
                er_accel = default_er_accel;
                er_spacing = default_er_spacing;
                
                //er_width = print_config->get_abs_value(er_width_ToOptionKey[er_role].c_str(), nozzle_diameter);
                const ConfigOptionFloatOrPercent* first_layer_speed_option = dynamic_cast<const ConfigOptionFloatOrPercent*>(full_print_config.option(kFirstLayerSpeedKey));
                er_speed = (first_layer_speed_option && first_layer_speed_option->percent && er_role == ROLE_FIRST_LAYER) ? default_er_speed : full_print_config.get_computed_value(er_speed_ToOptionKey[er_role].c_str());
                er_accel = full_print_config.get_computed_value(er_accel_ToOptionKey[er_role].c_str(), nozzle_diameter);
                //er_spacing = print_config->get_abs_value(er_spacing_ToOptionKey[er_role].c_str(), nozzle_diameter);

                //er_width = (er_width != 0) ? er_width : default_er_width;
                er_speed = (er_speed != 0) ? er_speed : default_er_speed;
                er_accel = (er_accel != 0) ? er_accel : default_er_accel;
                //er_spacing = (er_spacing != 0) ? er_spacing : default_er_spacing;

                er_role = "defaults for " + er_role + " width spacing";
            }
            if (er_role == choice_extrusion_role[11] || er_role == ROLE_THIN_WALL){
                er_width = default_er_width;// since the model gets scaled to thinwall size,it should use the default modifer? if it uses the thin_wall width modifer it fails to slice "ERROR:Layer height can't be greater than perimeter extrusion
                                            // width"
                er_width = std::round((default_er_width * kPercentScale / nozzle_diameter) * kPercentScale) / kPercentScale;
            }
            else{
                er_width = std::round((er_width * kPercentScale / nozzle_diameter) * kPercentScale) / kPercentScale;
            }
            const std::string active_role = (selected_extrusion_role == ROLE_CHECK_ALL) ? er_role : selected_extrusion_role;
            double active_layer_height = role_layer_height(active_role);
            if (active_role == ROLE_FIRST_LAYER)
                er_spacing = first_layer_flow.spacing();
            double er_width_to_scale = magical_scaling(
                nozzle_diameter, er_width, perimeter_overlap, external_perimeter_overlap, active_layer_height);
            
            double er_width_to_scale_first_layer_match_base2 = magical_scaling(
                nozzle_diameter, er_width, perimeter_overlap, external_perimeter_overlap, first_layer_height);

            double xy_scaled_90_bend_x = initial_90_bend_x * er_width_to_scale;             // mm
            double xy_scaled_90_bend_y = initial_90_bend_y * er_width_to_scale;             // mm
            double first_layer_xy_scaled_90_bend_x_match = initial_90_bend_x * er_width_to_scale_first_layer_match_base2; // mm for 90_bend width scaled for first_layer to match er role width
            double first_layer_xy_scaled_90_bend_y_match = initial_90_bend_y * er_width_to_scale_first_layer_match_base2; // mm for 90_bend width scaled for first_layer to match er role width


            double adjusted_first_layer_width_x = er_width * (1 + (xy_scaled_90_bend_x - first_layer_xy_scaled_90_bend_x_match) / xy_scaled_90_bend_x);
            double adjusted_first_layer_width_y = er_width * (1 + (xy_scaled_90_bend_y - first_layer_xy_scaled_90_bend_y_match) / xy_scaled_90_bend_y);
            double adjusted_first_average = (adjusted_first_layer_width_x + adjusted_first_layer_width_y) / kGeometryCenterDivisor;

            const std::string set_advance_prefix = pressure_advance_prefix(flavor, smooth_time);


            /// --- custom config ---
            // config for the 90_bend model
            // Tracked in #50: move width/speed override ownership to the right modifier and validate percent vs mm inputs.
            model.objects[objs_idx[id_item]]->volumes[num_part + extra_vol]->config.set_key_value("first_layer_extrusion_width", std::make_unique<ConfigOptionFloatOrPercent>(adjusted_first_average, true));
            model.objects[objs_idx[id_item]]->volumes[num_part + extra_vol]->config.set_key_value("perimeter_extrusion_width", std::make_unique<ConfigOptionFloatOrPercent>(er_width, true));
            model.objects[objs_idx[id_item]]->volumes[num_part + extra_vol]->config.set_key_value("external_perimeter_extrusion_width", std::make_unique<ConfigOptionFloatOrPercent>(er_width, true));
            model.objects[objs_idx[id_item]]->volumes[num_part + extra_vol]->config.set_key_value("perimeter_speed", std::make_unique<ConfigOptionFloatOrPercent>(er_speed, false));
            model.objects[objs_idx[id_item]]->volumes[num_part + extra_vol]->config.set_key_value("external_perimeter_speed", std::make_unique<ConfigOptionFloatOrPercent>(er_speed, false));
            model.objects[objs_idx[id_item]]->volumes[num_part + extra_vol]->config.set_key_value("gap_fill_speed", std::make_unique<ConfigOptionFloatOrPercent>(er_speed, false));
            if(er_accel > 0){
                model.objects[objs_idx[id_item]]->volumes[num_part + extra_vol]->config.set_key_value("perimeter_acceleration", std::make_unique<ConfigOptionFloatOrPercent>(er_accel, false));
                model.objects[objs_idx[id_item]]->volumes[num_part + extra_vol]->config.set_key_value("external_perimeter_acceleration", std::make_unique<ConfigOptionFloatOrPercent>(er_accel, false));
                model.objects[objs_idx[id_item]]->volumes[num_part + extra_vol]->config.set_key_value("gap_fill_acceleration", std::make_unique<ConfigOptionFloatOrPercent>(er_accel, false));
            }
            model.objects[objs_idx[id_item]]->volumes[num_part + extra_vol]->config.set_key_value(kLayerHeightKey, std::make_unique<ConfigOptionFloat>(0.3));

            const std::string first_layer_scope_prefix = "{if layer_z <= " + std::to_string(first_layer_height) + "}" + set_first_layer_prefix + std::to_string(first_pa) + " ; first layer [layer_z]\n{endif}\n";
            const std::string next_layer_scope_prefix = "{if layer_z > " + std::to_string(first_layer_height) + "}";
            const std::string next_layer_scope_suffix = " {endif}";

            if (selected_extrusion_role == ROLE_CHECK_ALL) {
                // user manual type in values, commented out to stop errors
                model.objects[objs_idx[id_item]]->volumes[num_part + extra_vol]->config.set_key_value("region_gcode", std::make_unique<ConfigOptionString>(first_layer_scope_prefix + ";" + set_advance_prefix + " ; " + er_role ));
                //will need to adjust layerheight for infill,support, other er roles that needs a different layerheight for CheckAll mode.

                /*ModelConfig range_conf;
                range_conf.set_key_value(kLayerHeightKey, std::make_unique<ConfigOptionFloatOrPercent>(combined_layer_height, false));
                model.objects[objs_idx[id_item]]->volumes[num_part + extra_vol]->layer_config_ranges[std::pair<double,double>(first_layer_height, 8)] = range_conf;

                wxGetApp().obj_list()->layers_editing();
                auto list = wxGetApp().obj_list();*/

                    /*if (infill_every_layers > 1 && selected_extrusion_role == ROLE_INTERNAL_INFILL && infill_dense == false) {

                        wxGetApp().obj_list()->layers_editing(id_item);//could prob use this same thing for the unsupported roles since they need a different layer_height
                        auto existing_range = model.objects[objs_idx[id_item]]->volumes[num_part + extra_vol]->layer_config_ranges.find(std::pair<double, double>(0.0f, 2.0f));// Find the default existing range {0.0f, 2.0f}

                        if (existing_range != model.objects[objs_idx[id_item]]->volumes[num_part + extra_vol]->layer_config_ranges.end()) {
                            ModelConfig new_range_conf = existing_range->second;

                            new_range_conf.set_key_value(kLayerHeightKey, std::make_unique<ConfigOptionFloatOrPercent>(combined_layer_height, false));
                            model.objects[objs_idx[id_item]]->volumes[num_part + extra_vol]->layer_config_ranges.erase(existing_range);
                            model.objects[objs_idx[id_item]]->volumes[num_part + extra_vol]->layer_config_ranges[std::pair<double, double>(first_layer_height, model_height + first_layer_height)] = new_range_conf;
                        }
                    }
                    else if (selected_extrusion_role == supports /ect){
                    }*/
            }
            else{
                model.objects[objs_idx[id_item]]->volumes[num_part + extra_vol]->config.set_key_value("region_gcode", std::make_unique<ConfigOptionString>(
                    first_layer_scope_prefix +
                    next_layer_scope_prefix + set_advance_prefix + std::to_string(pa_values[num_part]) + " ; " + er_role + next_layer_scope_suffix + "\n"));
            }
            //model.objects[objs_idx[id_item]]->ensure_on_bed(); // put at the correct z (kind of arrange-z)) shouldn't be needed though.
            //model.objects[objs_idx[id_item]]->center_around_origin();
        }


        bool enable_region_gcode_for_numbers = false;// this still needa a bit of work, the first layer ends up getting messed up surfaces. might be a config thing?
        //                                              unless i need to change the numbers height and z position?
        if (enable_region_gcode_for_numbers == true){
            apply_pa_region_gcode_to_numbers(*model.objects[objs_idx[id_item]], pa_values, number_positions.size(),
                                             count_numbers, count_borders, extra_vol, flavor, smooth_time, num_part);
        }
    }

    //update plater
    this->gui_app->get_tab(Preset::TYPE_FFF_PRINT)->load_config(new_print_config);
    plat->on_config_change(new_print_config);
    this->gui_app->get_tab(Preset::TYPE_PRINTER)->load_config(new_printer_config);
    plat->on_config_change(new_printer_config);
    this->gui_app->get_tab(Preset::TYPE_FFF_FILAMENT)->load_config(new_filament_config);
    plat->on_config_change(new_filament_config);
    //enable it later as a safeguard?, shouldn't be needed though.
    //for (size_t obj_idx : objs_idx) { model.objects[obj_idx]->ensure_on_bed(); } // put at the correct z (kind of arrange-z))
    //for (size_t obj_idx : objs_idx) { model.objects[obj_idx]->center_around_origin();}
    plat->changed_objects(objs_idx);
    this->gui_app->get_tab(Preset::TYPE_FFF_PRINT)->update_dirty();
    this->gui_app->get_tab(Preset::TYPE_PRINTER)->update_dirty();
    this->gui_app->get_tab(Preset::TYPE_FFF_FILAMENT)->update_dirty();
    plat->is_preview_shown();
    //update everything, easier to code.
    ObjectList* obj = this->gui_app->obj_list();
    obj->update_after_undo_redo();

    // arrange if needed, after new settings, to take them into account
    // Historical arrange workaround left disabled; sinking-label placement was fixed elsewhere.
    /*if (has_to_arrange) {
        //update print config (done at reslice but we need it here)
        if (plat->printer_technology() == ptFFF)
            plat->fff_print().apply(plat->model(), *plat->config());
        std::shared_ptr<ProgressIndicatorStub> fake_statusbar = std::make_shared<ProgressIndicatorStub>();
        ArrangeJob arranger(std::dynamic_pointer_cast<ProgressIndicator>(fake_statusbar), plat);
        arranger.prepare_all();
        arranger.process();
        arranger.finalize();
        
    }*/

    // 2.7 change
    if (has_to_arrange) {
        //update print config (done at reslice but we need it here)
        if (plat->printer_technology() == ptFFF)
            plat->fff_print().apply(plat->model(), *plat->config());
        Worker &ui_job_worker = plat->get_ui_job_worker();
        plat->arrange(ui_job_worker, false);
        ui_job_worker.wait_for_current_job(20000);
    }

    if (selected_extrusion_role != ROLE_CHECK_ALL) {//don't auto slice so user can manual add PA values
#ifndef _DEBUG
            plat->reslice(); //forces a slice of plater.
#endif
    }

    if (autocenter) {
        //re-enable auto-center after this calibration.
        gui_app->app_config->set("autocenter", "1");
    }

    close_dialog();
}

double CalibrationPressureAdvDialog::magical_scaling(
    double nozzle_diameter, double er_width, double perimeter_overlap,
    double external_perimeter_overlap, double base_layer_height)
{
    // er_width arrives as a percentage of the nozzle diameter (e.g. 112 == 112%); the guard
    // below converts it to an absolute mm width. A value already <= 3x the nozzle diameter is
    // treated as mm directly, as a fallback for any legacy callsite that passes mm.
    double extrusion_width = er_width;
    if (er_width > nozzle_diameter * 3.0)
        extrusion_width = nozzle_diameter * (er_width / kPercentScale);

    const double model_design_width = nozzle_diameter * 4.0;
    const double round_cap = base_layer_height * (1.0 - 0.25 * M_PI);
    const double flat_width = extrusion_width - kRoundCapCount * round_cap;
    const double rounded_extrusion = round_cap + flat_width + round_cap;

    const double extrusion1 = rounded_extrusion; // 1 and 2 overlap
    const double extrusion2 = rounded_extrusion; // kGeometryCenterDivisor and 3 touch
    const double extrusion3 = rounded_extrusion;
    const double extrusion4 = rounded_extrusion; // 3 and 4 overlap

    const double spacing_external = extrusion_width - base_layer_height * (1.0 - 0.25 * M_PI) * external_perimeter_overlap;
    const double spacing_internal = extrusion_width - base_layer_height * (1.0 - 0.25 * M_PI) * perimeter_overlap;
    const double overlap_external = extrusion_width - spacing_external;
    const double overlap_internal = extrusion_width - spacing_internal;

    // The middle pair intentionally touches without contributing overlap to the total width.
    const double first_pair = extrusion1 + extrusion2 - overlap_external;
    const double second_pair = extrusion3 + extrusion4 - overlap_internal;
    const double perfect_sliced_width = first_pair + second_pair;

    return perfect_sliced_width / model_design_width;
}

void CalibrationPressureAdvDialog::create_buttons(wxStdDialogButtonSizer* buttons) {

    const DynamicPrintConfig* printer_config = this->gui_app->get_tab(Preset::TYPE_PRINTER)->get_config();
    GCodeFlavor flavor = printer_config->option<ConfigOptionEnum<GCodeFlavor>>("gcode_flavor")->value;
    // Reading "dark_color_mode" straight from the config returns false whenever the key is
    // absent, but GUI_App::dark_mode() falls back to system detection - that split painted a
    // white control strip inside a dark dialog. "color_dark" is the button-text-on-hover accent
    // (Preferences calls it "Text color template"), not a label colour; at the stock cc6429 it
    // reads 3.9:1 on white and 3.3:1 on dark grey, both under the 4.5:1 minimum.
    const wxColour text_color = wxGetApp().get_style_role_color("tab.text.default");
    const wxColour background_color = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);

    std::string prefix = (gcfMarlinFirmware == flavor) ? " LA " : ((gcfKlipper == flavor || gcfRepRap == flavor) ? " PA " : "unsupported firmware type");

    if (prefix != "unsupported firmware type") {

        wxPanel* mainPanel = new wxPanel(this, wxID_ANY);
        mainPanel->SetBackgroundColour(background_color);
        mainPanel->Raise();

        // Create a vertical sizer for the panel
        wxBoxSizer* panelSizer = new wxBoxSizer(wxVERTICAL);
        mainPanel->SetSizer(panelSizer);

        // Create the common controls sizer
        wxBoxSizer* commonSizer = new wxBoxSizer(wxHORIZONTAL);

        wxString number_of_runs[] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "10" };//setting this any higher will break loading the model for the ID
        nbRuns = new wxComboBox(mainPanel, wxID_ANY, wxString{ "1" }, wxDefaultPosition, wxDefaultSize, 10, number_of_runs, wxCB_READONLY);
        nbRuns->SetToolTip(_L("Select the number of calibration lines to generate. Max 6 is recommended due to bed size limits."));
        nbRuns->SetSelection(0);
        nbRuns->Bind(wxEVT_COMBOBOX, &CalibrationPressureAdvDialog::on_row_change, this);

        wxStaticText* text_generate_count = new wxStaticText(mainPanel, wxID_ANY, _L("Number of" + prefix + "calibration lines: "));
        text_generate_count->SetForegroundColour(text_color);
        commonSizer->Add(text_generate_count, 0, wxALIGN_CENTER_VERTICAL | wxALL, kControlBorder);
        commonSizer->Add(nbRuns, 0, wxALIGN_CENTER_VERTICAL | wxALL, kControlBorder);

        // Create a button for generating models
        wxButton* generateButton = new wxButton(mainPanel, wxID_FILE1, _L("Generate"));
        generateButton->Bind(wxEVT_BUTTON, &CalibrationPressureAdvDialog::create_geometry, this);
        commonSizer->Add(generateButton, 0, wxALIGN_CENTER_VERTICAL | wxALL, kControlBorder);

        commonSizer->AddSpacer(50);// move the close button to the right a little, or align it on the far right side?

        wxButton* closeButton = new wxButton(mainPanel, wxID_CLOSE, _L("Close"));
        closeButton->Bind(wxEVT_BUTTON, &CalibrationPressureAdvDialog::close_me_wrapper, this);
        commonSizer->Add(closeButton, 0, wxALL, kControlBorder);

        panelSizer->Add(commonSizer, 0, wxALL, 10);
        dynamicSizer = new wxBoxSizer(wxVERTICAL);
        panelSizer->Add(dynamicSizer, 1, wxEXPAND | wxALL, kControlBorder);
        buttons->Add(mainPanel, 1, wxEXPAND | wxALL, 10);

        currentTestCount = wxAtoi(nbRuns->GetValue());
        create_row_controls(dynamicSizer, currentTestCount);
    } else {

        wxStaticText* incompatiable_text = new wxStaticText(this, wxID_ANY, _L(prefix));
        incompatiable_text->SetForegroundColour(*wxRED); // Set the text color to red for the incompatiable firmware tpe
        buttons->Add(incompatiable_text);
    }
}

void CalibrationPressureAdvDialog::create_row_controls(wxBoxSizer* parentSizer, int row_count) {

    // Same theme source as create_buttons - see the note there.
    const wxColour text_color = wxGetApp().get_style_role_color("tab.text.default");

    //
    //wxArrayInt
    //wxArrayDouble
    //wxArrayDouble choices_first_layerPA[] = { 0.025, 0.030, 0.035, 0.040, 0.045, 0.050 };
    wxString choices_first_layerPA[] = { "0.025", "0.030", "0.035", "0.040", "0.045", "0.050" };
    wxString choices_start_PA[] = { "0.0", "0.010", "0.020", "0.030", "0.040", "0.050" };
    wxString choices_end_PA[] = { "0.10", "0.20", "0.30", "0.40", "0.50", "0.60", "0.70", "0.80", "0.90", "1.00" };
    wxString choices_increment_PA[] = { "0.0010", "0.0025", "0.0035", "0.005", "0.006", "0.007", "0.01", "0.1" };
    wxString choices_extrusion_role[] = {
        ROLE_INTERNAL_INFILL, ROLE_BRIDGE_INFILL, ROLE_EXTERNAL_PERIMETER, ROLE_GAP_FILL, ROLE_INTERNAL_BRIDGE_INFILL,
        ROLE_IRONING, ROLE_OVERHANG_PERIMETER, ROLE_PERIMETER, ROLE_SOLID_INFILL, ROLE_SUPPORT_MATERIAL,
        ROLE_SUPPORT_MATERIAL_INTERFACE, ROLE_THIN_WALL, ROLE_TOP_SOLID_INFILL, ROLE_FIRST_LAYER, ROLE_CHECK_ALL
    };
    const DynamicPrintConfig* filament_config = this->gui_app->get_tab(Preset::TYPE_FFF_FILAMENT)->get_config();
    const DynamicPrintConfig* printer_config = this->gui_app->get_tab(Preset::TYPE_PRINTER)->get_config();
    GCodeFlavor flavor = printer_config->option<ConfigOptionEnum<GCodeFlavor>>("gcode_flavor")->value;
    std::string prefix = (gcfMarlinFirmware == flavor) ? " LA " : ((gcfKlipper == flavor || gcfRepRap == flavor) ? " PA " : "unsupported firmware type");
    const PaControlDefaults pa_defaults = pa_control_defaults_from_filament(filament_config);

    int current_selection = 2;//start selection at ExternalPerimeter

    if (!dynamicExtrusionRole.empty()) {// If there's a previous selection, find the index of the last selected role
        std::string last_selected_er_role = dynamicExtrusionRole[currentTestCount-1]->GetValue().ToStdString();
        for (int j = 0; j < int(sizeof(choices_extrusion_role) / sizeof(choices_extrusion_role[0])); j++) {
            if (choices_extrusion_role[j] == wxString(last_selected_er_role)) {
                current_selection = j + 1;
                break;
            }
        }
    }
    current_selection = std::min(current_selection, static_cast<int>(sizeof(choices_extrusion_role) / sizeof(choices_extrusion_role[0]) - 1));

    for (int i = 0; i < row_count; i++) {
        wxBoxSizer* rowSizer = new wxBoxSizer(wxHORIZONTAL);

        // wxDefaultSize, not a pixel width: the combos size to their longest entry, so values
        // like "0.0010" are not clipped at a larger font or display scale.
        wxComboBox* firstPaCombo = new wxComboBox(parentSizer->GetContainingWindow(), wxID_ANY, pa_defaults.first_layer, wxDefaultPosition, wxDefaultSize, 6, choices_first_layerPA);
        wxStaticText* text_first_l_prefix = new wxStaticText(parentSizer->GetContainingWindow(), wxID_ANY, _L("First Layers" + prefix + "value: "));
        text_first_l_prefix->SetForegroundColour(text_color);
        rowSizer->Add(text_first_l_prefix, 0, wxALIGN_CENTER_VERTICAL | wxALL, kControlBorder);
        firstPaCombo->SetToolTip(_L("Select the" + prefix + "value to be used for the first layer only.\n(this gets added to 'before_layer_gcode' area)"));
        rowSizer->Add(firstPaCombo, 0, wxALIGN_CENTER_VERTICAL | wxALL, kControlBorder);
        dynamicFirstPa.push_back(firstPaCombo);

        rowSizer->AddSpacer(kRowSpacer);

        wxComboBox* startPaCombo = new wxComboBox(parentSizer->GetContainingWindow(), wxID_ANY, pa_defaults.start, wxDefaultPosition, wxDefaultSize, 6, choices_start_PA);
        wxStaticText* text_start_value = new wxStaticText(parentSizer->GetContainingWindow(), wxID_ANY, _L("Start value: "));
        text_start_value->SetForegroundColour(text_color);
        rowSizer->Add(text_start_value, 0, wxALIGN_CENTER_VERTICAL | wxALL, kControlBorder);
        startPaCombo->SetToolTip(_L("Select the starting" + prefix + "value to be used.\nDefaults use the current filament PA when enabled.\n (you can manually type in values!)"));
        rowSizer->Add(startPaCombo, 0, wxALIGN_CENTER_VERTICAL);
        dynamicStartPa.push_back(startPaCombo);// can't validate input here since this is where they type it in..

        rowSizer->AddSpacer(kRowSpacer);

        wxComboBox* endPaCombo = new wxComboBox(parentSizer->GetContainingWindow(), wxID_ANY, pa_defaults.end, wxDefaultPosition, wxDefaultSize, 10, choices_end_PA);
        wxStaticText* text_end_value = new wxStaticText(parentSizer->GetContainingWindow(), wxID_ANY, _L("End value: "));
        text_end_value->SetForegroundColour(text_color);
        rowSizer->Add(text_end_value, 0, wxALIGN_CENTER_VERTICAL | wxALL, kControlBorder);
        endPaCombo->SetToolTip(_L("Select the ending" + prefix + "value to be used.\nDefaults use the current filament PA when enabled.\n (you can manually type in values!)"));
        rowSizer->Add(endPaCombo, 0, wxALIGN_CENTER_VERTICAL);
        dynamicEndPa.push_back(endPaCombo);

        rowSizer->AddSpacer(kRowSpacer);

        wxComboBox* paIncrementCombo = new wxComboBox(parentSizer->GetContainingWindow(), wxID_ANY, pa_defaults.increment, wxDefaultPosition, wxDefaultSize, 8, choices_increment_PA);
        wxStaticText* text_increment = new wxStaticText(parentSizer->GetContainingWindow(), wxID_ANY, _L("Increment by: "));
        text_increment->SetForegroundColour(text_color);
        rowSizer->Add(text_increment, 0, wxALIGN_CENTER_VERTICAL | wxALL, kControlBorder);
        paIncrementCombo->SetToolTip(_L("Select the incremental value.\nDefaults use the current filament PA when enabled.\n (you can manually type in values!)"));
        rowSizer->Add(paIncrementCombo, 0, wxALIGN_CENTER_VERTICAL);
        dynamicPaIncrement.push_back(paIncrementCombo);

        rowSizer->AddSpacer(kRowSpacer);

        wxComboBox* erPaCombo = new wxComboBox(parentSizer->GetContainingWindow(), wxID_ANY, wxString{ choices_extrusion_role[current_selection] }, wxDefaultPosition, wxDefaultSize, kExtrusionRoleChoiceCount, choices_extrusion_role, wxCB_READONLY);
            // disable user edit this one :)
        wxStaticText* text_extrusion_role = new wxStaticText(parentSizer->GetContainingWindow(), wxID_ANY, _L("Extrusion role: "));
        text_extrusion_role->SetForegroundColour(text_color);
        rowSizer->Add(text_extrusion_role, 0, wxALIGN_CENTER_VERTICAL | wxALL, kControlBorder);
        erPaCombo->SetToolTip(_L("Select the extrusion role you want to generate a calibration for"));
        erPaCombo->SetSelection(current_selection);
        rowSizer->Add(erPaCombo, 0, wxALIGN_CENTER_VERTICAL);
        dynamicExtrusionRole.push_back(erPaCombo);

        // Increment selection for the next row
        current_selection++;
        if (current_selection >= int(sizeof(choices_extrusion_role) / sizeof(choices_extrusion_role[0]))) {
            current_selection = 0; // Wrap around: SetSelection does it's own memory access checks so this shouldn't be needed. but it's a nice safe guard to have.
        }

        if (prefix == " PA ") {//klipper only feature ?
            rowSizer->AddSpacer(kRowSpacer);
            wxCheckBox* enableST = new wxCheckBox(parentSizer->GetContainingWindow(), wxID_ANY, _L("Calibrate Smooth Time instead of Advance"), wxDefaultPosition, wxDefaultSize);
            enableST->SetForegroundColour(text_color);
            enableST->SetToolTip(_L("When enabled, the start/end/increment values will sweep Klipper's SMOOTH_TIME parameter instead of ADVANCE.\n\nSmooth Time controls how long extruder velocity changes are averaged to smooth out rapid "
                                    "pressure changes.\nShorter times (e.g., 0.01s) suit fast printing; longer times (e.g., 0.4s) suit slower printing.\nKlipper default: 0.04s."));
            enableST->SetValue(false);
            enableST->Bind(wxEVT_CHECKBOX, &CalibrationPressureAdvDialog::on_smooth_time_toggle, this);
            rowSizer->Add(enableST, 1, wxALIGN_CENTER_VERTICAL);
            dynamicEnableST.push_back(enableST);
        }

        parentSizer->Add(rowSizer, 0, wxALL, 2);// change this to make each row have a larger/smaller 'gap' between them
        dynamicRowcount.push_back(rowSizer);
    }
}

void CalibrationPressureAdvDialog::on_row_change(wxCommandEvent& event) {
    int new_test_count = wxAtoi(nbRuns->GetValue());

    wxSize auto_size = GetSize();
    //wxSize auto_size = DoGetBestSize();

    if (new_test_count > currentTestCount) {
        create_row_controls(dynamicSizer, new_test_count - currentTestCount);
    } else if (new_test_count < currentTestCount) {
        for (int i = currentTestCount - 1; i >= new_test_count; --i) {
            wxBoxSizer* row = dynamicRowcount.back();
            row->Clear(true);
            const bool removed = dynamicSizer->Remove(row);
            assert(removed);
            dynamicRowcount.pop_back();
            dynamicFirstPa.pop_back();
            dynamicStartPa.pop_back();
            dynamicEndPa.pop_back();
            dynamicPaIncrement.pop_back();
            dynamicExtrusionRole.pop_back();
            if (dynamicEnableST.size() > 0) {
                dynamicEnableST.pop_back();
                assert(dynamicEnableST.size() == dynamicExtrusionRole.size());
            }
        }
    }

    currentTestCount = new_test_count;
    dynamicSizer->Layout();
    this->Fit();
    
    //this->SetSize(1600,600);
    this->SetSize(auto_size); //makes GUI flash on updating

}

void CalibrationPressureAdvDialog::on_smooth_time_toggle(wxCommandEvent& event) {
    // Find which row's checkbox was toggled
    wxCheckBox* cb = dynamic_cast<wxCheckBox*>(event.GetEventObject());
    if (!cb) return;

    int row = -1;
    for (size_t i = 0; i < dynamicEnableST.size(); i++) {
        if (dynamicEnableST[i] == cb) { row = static_cast<int>(i); break; }
    }
    if (row < 0 || row >= static_cast<int>(dynamicFirstPa.size())) return;

    bool enabled = cb->GetValue();

    if (enabled) {
        // Save current values before overwriting
        savedPaBeforeST[row] = {
            dynamicFirstPa[row]->GetValue(),
            dynamicStartPa[row]->GetValue(),
            dynamicEndPa[row]->GetValue(),
            dynamicPaIncrement[row]->GetValue(),
            dynamicExtrusionRole[row]->GetValue()
        };

        // Set recommended smooth time calibration values (Klipper default is 0.04s)
        // ExternalPerimeter is the standard role for smooth time tuning
        dynamicFirstPa[row]->SetValue("0.040");
        dynamicStartPa[row]->SetValue("0.010");
        dynamicEndPa[row]->SetValue("0.080");
        dynamicPaIncrement[row]->SetValue("0.005");
        dynamicExtrusionRole[row]->SetValue(ROLE_EXTERNAL_PERIMETER);

        // Disable editing — smooth time calibration uses fixed recommended values
        dynamicFirstPa[row]->Enable(false);
        dynamicStartPa[row]->Enable(false);
        dynamicEndPa[row]->Enable(false);
        dynamicPaIncrement[row]->Enable(false);
        dynamicExtrusionRole[row]->Enable(false);
    } else {
        // Restore saved values
        auto it = savedPaBeforeST.find(row);
        if (it != savedPaBeforeST.end()) {
            dynamicFirstPa[row]->SetValue(it->second.firstPa);
            dynamicStartPa[row]->SetValue(it->second.startPa);
            dynamicEndPa[row]->SetValue(it->second.endPa);
            dynamicPaIncrement[row]->SetValue(it->second.increment);
            dynamicExtrusionRole[row]->SetValue(it->second.extrusionRole);
            savedPaBeforeST.erase(it);
        }

        // Re-enable editing
        dynamicFirstPa[row]->Enable(true);
        dynamicStartPa[row]->Enable(true);
        dynamicEndPa[row]->Enable(true);
        dynamicPaIncrement[row]->Enable(true);
        dynamicExtrusionRole[row]->Enable(true);
    }
}

std::pair<std::vector<double>, int> CalibrationPressureAdvDialog::calc_PA_values(int id_item) {
    wxString firstPaValue = dynamicFirstPa[id_item]->GetValue();
    wxString startPaValue = dynamicStartPa[id_item]->GetValue();
    wxString endPaValue = dynamicEndPa[id_item]->GetValue();
    wxString paIncrementValue = dynamicPaIncrement[id_item]->GetValue();

    // Normalize comma decimal text before C-locale parsing. See #38.

    /*std::locale loc("");
    const std::numpunct<char>& np = std::use_facet<std::numpunct<char>>(loc);

    // Get the locale-specific decimal and thousands separators
    wxString decimal_sep = wxString::Format("%c", np.decimal_point());
    wxString thousands_sep = wxString::Format("%c", np.thousands_sep());

    // Replace the decimal separator with a dot
    if (!decimal_sep.IsEmpty() ) {
        firstPaValue.Replace(thousands_sep, decimal_sep);
        startPaValue.Replace(thousands_sep, decimal_sep);
        endPaValue.Replace(thousands_sep, decimal_sep);
        paIncrementValue.Replace(thousands_sep, decimal_sep);
    }
    */
    firstPaValue.Replace(",", ".");
    startPaValue.Replace(",", ".");
    endPaValue.Replace(",", ".");
    paIncrementValue.Replace(",", ".");
    auto show_input_error = [this, id_item](const wxString& details) {
        wxMessageBox(
            wxString::Format(_L("Invalid pressure advance input in row %d:\n%s"), id_item + 1, details),
            _L("Invalid calibration values"),
            wxOK | wxICON_ERROR,
            this);
    };
    
    //maybe? will need to load in the correct 'acsii' character based on localization then swap ?
    //any point idiot profing the input to stop crashing ? nothing stopping users typing in letters to force a crash...

    // Parse with ToCDouble (C locale, '.' decimal) after the comma->dot normalization above,
    // so input parses correctly regardless of the user's system locale. Plain ToDouble uses
    // the current locale and rejects "0.02" under comma-decimal locales (see #38).
    double first_pa = 0.0;
    bool first_pa_ok = firstPaValue.ToCDouble(&first_pa);
    double start_pa = 0.0;
    bool start_pa_ok = startPaValue.ToCDouble(&start_pa);
    double end_pa = 0.0;
    bool end_pa_ok = endPaValue.ToCDouble(&end_pa);
    double pa_increment = 0.0;
    bool pa_increment_ok = paIncrementValue.ToCDouble(&pa_increment);

    if (!first_pa_ok || !start_pa_ok || !end_pa_ok || !pa_increment_ok) {
        show_input_error(_L("Please enter numeric values for first PA, start PA, end PA, and PA increment."));
        return std::make_pair(std::vector<double>{}, 0);
    }

    if (pa_increment <= 0.0) {
        show_input_error(_L("PA increment must be greater than 0."));
        return std::make_pair(std::vector<double>{}, 0);
    }

    if (end_pa < start_pa) {
        show_input_error(_L("End PA must be greater than or equal to Start PA."));
        return std::make_pair(std::vector<double>{}, 0);
    }

    constexpr int max_pa_points = 500;
    int estimated_points = static_cast<int>(std::ceil((end_pa - start_pa) / pa_increment)) + 1;
    if (estimated_points > max_pa_points) {
        show_input_error(wxString::Format(
            _L("Too many PA values (%d). Increase the increment or reduce the range. Maximum allowed is %d."),
            estimated_points,
            max_pa_points));
        return std::make_pair(std::vector<double>{}, 0);
    }

    int countincrements = 0;
    int sizeofarray = estimated_points + 1;//'+1' keeps room for the end-pa failsafe branch.
    std::vector<double> pa_values(sizeofarray);

    double incremented_pa_value = start_pa;
    while (incremented_pa_value <= end_pa + pa_increment / kGeometryCenterDivisor) {
        if (incremented_pa_value <= end_pa) {
            double rounded_pa = std::round(incremented_pa_value * 1000000.0) / 1000000.0;
            pa_values[countincrements] = rounded_pa;
            countincrements++;
            incremented_pa_value += pa_increment;
        } else {
            pa_values[countincrements] = end_pa;
            countincrements++;//failsafe if werid input numbers are provided that can't add the "ending pa" number to the array.
            break;
        }
    }// is there a limit of how many models SS can load ? might be good to set a failsafe just so it won't load 10k+ models...

    return std::make_pair(pa_values, countincrements);
}

void CalibrationPressureAdvDialog::close_me_wrapper(wxCommandEvent& event) {// for custom location of "close" button
    this->close_me(event);
}
} // namespace GUI
} // namespace Slic3r
