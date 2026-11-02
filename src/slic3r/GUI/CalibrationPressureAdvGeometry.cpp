///|/ PA calibration geometry generator. Original implementation by legend069 (2024).
///|/ Modified 2026 by Stan Elston (RenegadeRiff86) -- see git history.
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher.
///|/
#include "CalibrationPressureAdvDialog.hpp"
#include "CalibrationPressureAdvShared.hpp"
#include "GLCanvas3D.hpp"
#include "GUI.hpp"
#include "GUI_ObjectList.hpp"
#include "Plater.hpp"
#include "Tab.hpp"
#include "Jobs/ArrangeJob.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/CustomGCode.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Utils.hpp"

#include <wx/file.h>
#include <wx/msgdlg.h>

#undef NDEBUG
#include <cassert>

namespace Slic3r {
namespace GUI {

using namespace CalibrationPressureAdvDetail;

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
    // overhangs_width is an unsupported-width THRESHOLD, not a line width. Overhang paths
    // use the same flow as external perimeters (FlowRole::frExternalPerimeter).
    {ROLE_OVERHANG_PERIMETER, "external_perimeter_extrusion_width"},
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
    {ROLE_IRONING, "ironing_spacing"},
    {ROLE_OVERHANG_PERIMETER, "external_perimeter_extrusion_spacing"},
    {ROLE_PERIMETER, "perimeter_extrusion_spacing"},
    {ROLE_SOLID_INFILL, "solid_infill_extrusion_spacing"},
    {ROLE_SUPPORT_MATERIAL, "support_material_spacing"},
    {ROLE_SUPPORT_MATERIAL_INTERFACE, "support_material_interface_spacing"},
    {ROLE_THIN_WALL, "external_perimeter_extrusion_spacing"},
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
    double combined_layer_height = infill_every_layers * base_layer_height;


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

        if (role_name == ROLE_SUPPORT_MATERIAL || role_name == ROLE_SUPPORT_MATERIAL_INTERFACE) {
            if (const auto *float_opt = dynamic_cast<const ConfigOptionFloat*>(opt))
                return float_opt->value;
        }

        const double spacing = print_config->get_abs_value(spacing_key.c_str(), nozzle_diameter);
        return spacing > 0 ? spacing : default_er_spacing;
    };
    // first_layer_speed is a MAX when absolute, or a scale of the current feature speed when
    // given as a percent (100% means "no first-layer modification").
    auto resolve_role_speed = [&](const std::string& role_name) {
        const auto speed_it = er_speed_ToOptionKey.find(role_name);
        const double role_speed = (speed_it != er_speed_ToOptionKey.end())
            ? full_print_config.get_computed_value(speed_it->second.c_str())
            : default_er_speed;
        if (role_name != ROLE_FIRST_LAYER)
            return role_speed > 0 ? role_speed : default_er_speed;

        const auto *first_layer_speed_option =
            dynamic_cast<const ConfigOptionFloatOrPercent*>(full_print_config.option(kFirstLayerSpeedKey));
        if (!first_layer_speed_option)
            return default_er_speed;
        if (first_layer_speed_option->percent)
            return first_layer_speed_option->get_abs_value(default_er_speed);
        return first_layer_speed_option->value > 0 ? first_layer_speed_option->value : default_er_speed;
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
    double snapped_nozzle = std::round(nozzle_diameter / kNozzleSnapStepMm) * kNozzleSnapStepMm;
    if (snapped_nozzle < kMinimumSnappedNozzleMm) snapped_nozzle = kMinimumSnappedNozzleMm;
    if (snapped_nozzle > kMaximumCalibrationNozzleDiameter) snapped_nozzle = kMaximumCalibrationNozzleDiameter;
    char nozzle_diameter_buf[kNozzleDiameterBufSize];
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

    const auto generate_calibration_item = [&](int id_item) {
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
            {
                // Everything below is derived from selected_extrusion_role alone, so walking every
                // entry of choice_extrusion_role just recomputed the same values once per role.
                if (er_width_ToOptionKey.find(selected_extrusion_role) != er_width_ToOptionKey.end()) {

                    //look at maps to match speed/width ect to the selected ER role
                    er_width = print_config->get_abs_value(er_width_ToOptionKey[selected_extrusion_role].c_str(), nozzle_diameter);
                    er_speed = resolve_role_speed(selected_extrusion_role);
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

        double xyzScale = nozzle_diameter / CalibrationConstants::kDesignNozzleDiameterMm;
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

            const auto add_tower_borders = [&]() {
                // Load borders once, after the first number positions exist.
                if (nb_bends != 1 || selected_extrusion_role == ROLE_CHECK_ALL)
                    return;
                Eigen::Vector3d bend_pos_first = bend_90_positions[0];
                Eigen::Vector3d bend_pos_mid = bend_90_positions[count_increments / kMiddleIndexDivisor];
                Eigen::Vector3d bend_pos_last = bend_90_positions[count_increments-1];
                // True vertical centre of the bend stack. The left/right borders must be centred
                // here, NOT on bend_pos_mid (the middle-INDEX bend): that index only equals the
                // centre for odd counts. For even counts it sits half a bend-pitch high, shifting
                // the side borders up and leaving the bottom border disconnected (#46).
                const double tower_center_y = (bend_pos_first.y() + bend_pos_last.y()) / kGeometryCenterDivisor;

                // Border generation runs after the first label set, so this collection is non-empty.
                const Eigen::Vector3d number_pos_first = number_positions.front();
                const Eigen::Vector3d number_pos_mid =
                    number_positions[number_positions.size() / kMiddleIndexDivisor];
                const Eigen::Vector3d number_pos_last = number_positions.back();
                count_numbers += int(number_positions.size());

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
                };
                add_tower_borders();

            // CheckAll has no per-bend labels, and alternating slots intentionally stay blank.
            if (selected_extrusion_role == ROLE_CHECK_ALL)
                continue;
            if (nb_bends % kParityDivisor == 1)
                continue;
            // Tracked in #47: validate normal-size vs thin-wall-specific label geometry.

            Eigen::Vector3d bend_90_pos = bend_90_positions[nb_bends];
            const std::string pa_values_string = format_pa_label_value(pa_values[nb_bends]);

            double xpos = bend_90_pos.x() + (xy_scaled_90_bend_x / kGeometryCenterDivisor) + (xy_scaled_number_x / kGeometryCenterDivisor) + nozzle_diameter;
            //double ypos = bend_90_pos.y() + (xy_scaled_90_bend_y / kGeometryCenterDivisor) - (xy_scaled_number_y / kGeometryCenterDivisor)
            double ypos = bend_90_pos.y() + (xy_scaled_90_bend_y / kGeometryCenterDivisor) - (xy_scaled_number_y / kGeometryCenterDivisor) + (nozzle_diameter * 3);
            double space_numbers_distance_x = (xy_scaled_number_x / kGeometryCenterDivisor) + nozzle_diameter + (xy_scaled_number_x / kGeometryCenterDivisor);//space the numbers by this amount.
            // Tracked in #47: tune label Y position so it is centered with the 90-degree bend notch.

            const auto add_label_character = [&](char label_character) {
                if (label_character == '.') {

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

                } else if (std::isdigit(label_character)) {
                    add_part(model.objects[objs_idx[id_item]],(boost::filesystem::path(Slic3r::resources_dir()) / kCalibrationResourceDirectory / kFilamentPressureResourceDirectory / (label_character + std::string(".3mf"))).string(),
                        Vec3d{ xpos, ypos, z_scaled_model_height },
                            /*scale*/Vec3d{ xyzScale * er_width_to_scale, xyzScale * er_width_to_scale, z_scale_numbers }, false);//TOCHECK: if any numbers get gapfill
                    number_positions.push_back(Eigen::Vector3d(xpos, ypos, z_scaled_model_height));
                    xpos = number_positions.back().x();
                }
            };
            for (size_t index = 0; index < pa_values_string.length(); ++index) {
                if (index != 0)
                    xpos += space_numbers_distance_x;
                add_label_character(pa_values_string[index]);
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
                base_vol->config.set_key_value("fill_density", std::make_unique<ConfigOptionPercent>(kSolidInfillOverlapCapPercent));
            }
        }
    };
    for (int id_item = 0; id_item < currentTestCount; ++id_item)
        generate_calibration_item(id_item);

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
    // Region G-code carries the PA value for each calibration bend. Feature G-code is emitted
    // later, immediately before extrusion, so a user macro there could overwrite the value being
    // calibrated. Disable it for this generated project; the user's saved preset is not modified.
    new_print_config.set_key_value("feature_gcode", std::make_unique<ConfigOptionString>(""));
    auto filament_pa = std::make_unique<ConfigOptionFloats>(std::initializer_list<double>{0.00});
    filament_pa->set_can_be_disabled(true);
    new_filament_config.set_key_value("filament_pressure_advance", std::move(filament_pa));



    //assert(filament_temp_item_name.size() == nb_runs);
    //assert(model.objects.size() == nb_runs);
    assert(objs_idx.size() == size_t(currentTestCount));
    const auto configure_calibration_item = [&](int id_item) {
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
        // first_layer_extrusion_width is PrintObjectConfig: a volume override is ignored by
        // Print::apply. Put it on the object so the first-layer lines actually change width.
        {
            double first_layer_width_percent = first_layer_width;
            if (selected_extrusion_role != ROLE_CHECK_ALL && er_width > 0 && first_layer_height > 0) {
                const double scale_role = magical_scaling(
                    nozzle_diameter, er_width, perimeter_overlap, external_perimeter_overlap,
                    role_layer_height(selected_extrusion_role));
                const double scale_first = magical_scaling(
                    nozzle_diameter, er_width, perimeter_overlap, external_perimeter_overlap,
                    first_layer_height);
                if (scale_role > 0)
                    first_layer_width_percent = er_width * (1.0 + (scale_role - scale_first) / scale_role);
            }
            model.objects[objs_idx[id_item]]->config.set_key_value(
                "first_layer_extrusion_width",
                std::make_unique<ConfigOptionFloatOrPercent>(first_layer_width_percent, true));
        }
        // changed_objects() below publishes every object-config override together, including
        // seam_position, so the plater and project dirty state stay in sync.
        model.objects[objs_idx[id_item]]->config.set_key_value("seam_position", std::make_unique<ConfigOptionEnum<SeamPosition>>(spRear));
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
                er_speed = resolve_role_speed(er_role);
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
                
                er_speed = resolve_role_speed(er_role);
                er_accel = full_print_config.get_computed_value(er_accel_ToOptionKey[er_role].c_str(), nozzle_diameter);
                //er_spacing = print_config->get_abs_value(er_spacing_ToOptionKey[er_role].c_str(), nozzle_diameter);

                //er_width = (er_width != 0) ? er_width : default_er_width;
                er_speed = (er_speed != 0) ? er_speed : default_er_speed;
                er_accel = (er_accel != 0) ? er_accel : default_er_accel;
                //er_spacing = (er_spacing != 0) ? er_spacing : default_er_spacing;

                er_role = "defaults for " + er_role + " width spacing";
            }
            if (er_role == choice_extrusion_role[kThinWallExtrusionRoleIndex] || er_role == ROLE_THIN_WALL){
                er_width = default_er_width;// since the model gets scaled to thinwall size,it should use the default modifer? if it uses the thin_wall width modifer it fails to slice "ERROR:Layer height can't be greater than perimeter extrusion
                                            // width"
                er_width = std::round((default_er_width * kPercentScale / nozzle_diameter) * kPercentScale) / kPercentScale;
            }
            else{
                er_width = std::round((er_width * kPercentScale / nozzle_diameter) * kPercentScale) / kPercentScale;
            }
            const std::string active_role = (selected_extrusion_role == ROLE_CHECK_ALL) ? er_role : selected_extrusion_role;
            if (active_role == ROLE_FIRST_LAYER)
                er_spacing = first_layer_flow.spacing();
            const std::string set_advance_prefix = pressure_advance_prefix(flavor, smooth_time);


            /// --- custom config ---
            // Region-level overrides stay on each bend volume: Check All assigns a different
            // role to every bend, which an object override would collapse. first_layer_*
            // belongs on the object (PrintObjectConfig) and is set above.
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
            model.objects[objs_idx[id_item]]->volumes[num_part + extra_vol]->config.set_key_value(kLayerHeightKey, std::make_unique<ConfigOptionFloat>(kPaVolumeLayerHeightMm));

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
    };
    for (int id_item = 0; id_item < currentTestCount; ++id_item)
        configure_calibration_item(id_item);

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
    // Use the current UI worker so arrangement participates in normal job tracking.
    if (has_to_arrange) {
        //update print config (done at reslice but we need it here)
        if (plat->printer_technology() == ptFFF)
            plat->fff_print().apply(plat->model(), *plat->config());
        Worker &ui_job_worker = plat->get_ui_job_worker();
        plat->arrange(ui_job_worker, false);
        ui_job_worker.wait_for_current_job(CalibrationConstants::kArrangeJobTimeoutMs);
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

} // namespace GUI
} // namespace Slic3r
