#include "CalibrationFlowDialog.hpp"
#include "I18N.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/AppConfig.hpp"
// #include "Jobs/ArrangeJob2.hpp"
#include "GLCanvas3D.hpp"
#include "GUI.hpp"
#include "GUI_ObjectList.hpp"
#include "Plater.hpp"
#include "Tab.hpp"
#include <wx/scrolwin.h>
#include <wx/display.h>
#include <wx/file.h>
#include <wx/wupdlock.h>

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

namespace {
constexpr char kCalibrationResourceDirectory[] = "calibration";
constexpr char kFilamentFlowResourceDirectory[] = "filament_flow";
constexpr char   kFlowTestCubeFilename[]       = "filament_flow_test_cube.amf";
constexpr float  kCoarseFlowStartPercent       = 80.0f;
constexpr float  kCoarseFlowStepPercent        = 10.0f;
constexpr float  kFineBelowFlowStartPercent    = 92.0f;
constexpr float  kFineAboveFlowStartPercent    = 100.0f;
constexpr float  kFineFlowStepPercent          = 2.0f;
constexpr size_t kFlowCubeCount                = 5;
constexpr int    kAdditionalLayerCount         = 5;
constexpr double kModelHeightCenterDivisor     = 2.0;
constexpr double kIndicatorHorizontalOffset    = 10.0;
constexpr size_t kCenterCubeIndex              = 2;
constexpr size_t kFourthCubeIndex              = 3;
constexpr int    kCalibrationPerimeterCount    = 3;
constexpr int    kPerimeterOverlapPercent      = 80;
constexpr int    kButtonRowSpacerPx            = 20;
constexpr double kIndicatorZDeltaMm            = 0.3; // add_part vertical delta for flow glyphs
constexpr int    kIndicatorMidpointDivisor     = 2;   // mid of two-layer tall indicator
constexpr size_t kFifthCubeIndex               = 4;
constexpr int    kThinWallsMinWidthPercent     = 50;
constexpr int    kExternalInfillMarginPercent  = 100;
constexpr int    kSolidInfillEveryLayer        = 1;

boost::filesystem::path flow_resource_path(const char* filename)
{
    return boost::filesystem::path(Slic3r::resources_dir()) /
           kCalibrationResourceDirectory / kFilamentFlowResourceDirectory / filename;
}
} // namespace

void CalibrationFlowDialog::create_buttons(wxStdDialogButtonSizer* buttons){
    wxButton* bt = new wxButton(this, wxID_FILE1, _L("Generate 10% intervals around current value"));
    bt->Bind(wxEVT_BUTTON, &CalibrationFlowDialog::create_geometry_10, this);
    buttons->Add(bt);
    buttons->AddSpacer(kButtonRowSpacerPx);
    bt = new wxButton(this, wxID_FILE2, _L("Generate 2% intervals below current value"));
    bt->Bind(wxEVT_BUTTON, &CalibrationFlowDialog::create_geometry_2_5, this);
    buttons->Add(bt);
    buttons->AddSpacer(kButtonRowSpacerPx);
    bt = new wxButton(this, wxID_FILE3, _L("Generate 2% intervals above current value"));
    bt->Bind(wxEVT_BUTTON, &CalibrationFlowDialog::create_geometry_2_5_above, this);
    buttons->Add(bt);
}

void CalibrationFlowDialog::create_geometry_10(wxCommandEvent &event_args)
{
    Plater *plat = this->main_frame->plater();
    if (!plat->new_project(L("Flow 10 percent calibration")))
        return;
    create_geometry(kCoarseFlowStartPercent, kCoarseFlowStepPercent);
}

void CalibrationFlowDialog::create_geometry_2_5(wxCommandEvent &event_args)
{
    Plater *plat = this->main_frame->plater();
    if (!plat->new_project(L("Flow 2 percent calibration")))
        return;
    create_geometry(kFineBelowFlowStartPercent, kFineFlowStepPercent);
}

void CalibrationFlowDialog::create_geometry_2_5_above(wxCommandEvent &event_args)
{
    Plater *plat = this->main_frame->plater();
    if (!plat->new_project(L("Flow 2 percent above calibration")))
        return;
    create_geometry(kFineAboveFlowStartPercent, kFineFlowStepPercent);
}

void CalibrationFlowDialog::create_geometry(float start, float delta) {
    // wait for slicing end if needed
    wxGetApp().Yield();

    Plater* plat = this->main_frame->plater();
    Model& model = plat->model();
    
    std::unique_ptr<wxWindowUpdateLocker> freeze_gui = std::make_unique<wxWindowUpdateLocker>(this);
    bool autocenter = gui_app->app_config->get("autocenter") == "1";
    if (autocenter) {
        //disable auto-center for this calibration.
        gui_app->app_config->set("autocenter", "0");
    }

    std::vector<size_t> objs_idx = plat->load_files(std::vector<std::string>{
            flow_resource_path(kFlowTestCubeFilename).string(),
            flow_resource_path(kFlowTestCubeFilename).string(),
            flow_resource_path(kFlowTestCubeFilename).string(),
            flow_resource_path(kFlowTestCubeFilename).string(),
            flow_resource_path(kFlowTestCubeFilename).string()},
        LoadFileOption::LoadModel | LoadFileOption::DontUpdateDirs);


    assert(objs_idx.size() == kFlowCubeCount);
    const DynamicPrintConfig* print_config = this->gui_app->get_tab(Preset::TYPE_FFF_PRINT)->get_config();
    const DynamicPrintConfig* printerConfig = this->gui_app->get_tab(Preset::TYPE_PRINTER)->get_config();
    
    /// --- scale ---
    // model is created for a 0.4 nozzle, scale xy with nozzle size.
    const ConfigOptionFloats* nozzle_diameter_config = printerConfig->option<ConfigOptionFloats>("nozzle_diameter");
    assert(nozzle_diameter_config->size() > 0);
    float nozzle_diameter = nozzle_diameter_config->get_at(0);
    float xyScale = nozzle_diameter / CalibrationConstants::kDesignNozzleDiameterMm;
    //scale z to have 6 layers
    const ConfigOptionFloatOrPercent* first_layer_height_setting = print_config->option<ConfigOptionFloatOrPercent>("first_layer_height");
    double first_layer_height = first_layer_height_setting->get_abs_value(nozzle_diameter);
    double layer_height = nozzle_diameter / 2.;
    layer_height = check_z_step(layer_height, printerConfig->option<ConfigOptionFloat>("z_step")->value); // If z_step is not 0 the slicer will scale to the nearest multiple of z_step so account for that here
    first_layer_height = std::max(first_layer_height, nozzle_diameter / 2.);

    // (zscale / 2) represents the midpoint of the filament_flow_test_cube. Note: we ned to use the height of filament_flow_test_cube before scaling. 
    float z_origin = 0.5f;
    float zscale = first_layer_height + kAdditionalLayerCount * layer_height;
    //do scaling
    if (xyScale < CalibrationConstants::kXyScaleMinFactor || CalibrationConstants::kXyScaleMaxFactor < xyScale) {
        for (size_t i = 0; i < kFlowCubeCount; i++)
            model.objects[objs_idx[i]]->scale(xyScale, xyScale, zscale); // base: 10 10 1
    } else {
        for (size_t i = 0; i < kFlowCubeCount; i++)
            model.objects[objs_idx[i]]->scale(1, 1, zscale);
    }
    //// move to bed
    //for (size_t i = 0; i < kFlowCubeCount; i++)
    //    model.objects[objs_idx[i]]->translate(0, 0, ); // base: 10 10 1


    //add sub-part after scale
    float zscale_number = (first_layer_height + layer_height) / CalibrationConstants::kDesignNozzleDiameterMm;
    /* zshift is calculated using the following:
    z_origin: we go back to 0 by removing it.
    ((first_layer_height + layer_height) / 2) represents the midpoint of our indicator tab (it is scaled to be 2 layers tall)
    The 0.3 constant is the same as the delta calculated in add_part below, this should probably be calculated per the model object
    */
    float zshift_number = -z_origin + ((first_layer_height + layer_height) / kIndicatorMidpointDivisor) + kIndicatorZDeltaMm;
    
    // it's rotated but not around the good origin: correct that
    double init_z_rotate_angle = Geometry::deg2rad(plat->config()->opt_float("init_z_rotate"));
    Matrix3d rot_matrix = Eigen::Quaterniond(Eigen::AngleAxisd(init_z_rotate_angle, Vec3d{0,0,1})).toRotationMatrix();
    auto     translate_from_rotation = [&rot_matrix, &model, &objs_idx](int idx, Vec3d &&translation) {
            ModelVolume *vol = model.objects[objs_idx[idx]]->volumes[model.objects[objs_idx[idx]]->volumes.size()-1];
            Geometry::Transformation trsf = vol->get_transformation();
            trsf.set_offset(rot_matrix * translation - translation + trsf.get_offset());
            vol->set_transformation(trsf);
        };
    
    if (delta == kCoarseFlowStepPercent && start == kCoarseFlowStartPercent) {
        add_part(model.objects[objs_idx[0]], flow_resource_path("m20.amf").string(), Vec3d{ kIndicatorHorizontalOffset * xyScale,0,zshift_number }, Vec3d{ xyScale , xyScale, zscale_number});
        add_part(model.objects[objs_idx[1]], flow_resource_path("m10.amf").string(), Vec3d{ kIndicatorHorizontalOffset * xyScale,0,zshift_number }, Vec3d{ xyScale , xyScale, zscale_number });
        add_part(model.objects[objs_idx[kCenterCubeIndex]], flow_resource_path("_0.amf").string(), Vec3d{ kIndicatorHorizontalOffset * xyScale,0,zshift_number }, Vec3d{ xyScale , xyScale, zscale_number });
        add_part(model.objects[objs_idx[kFourthCubeIndex]], flow_resource_path("p10.amf").string(), Vec3d{ kIndicatorHorizontalOffset * xyScale,0,zshift_number }, Vec3d{ xyScale , xyScale, zscale_number });
        add_part(model.objects[objs_idx[kFifthCubeIndex]], flow_resource_path("p20.amf").string(), Vec3d{ kIndicatorHorizontalOffset * xyScale,0,zshift_number }, Vec3d{ xyScale , xyScale, zscale_number });
    } else if (delta == kFineFlowStepPercent && start == kFineBelowFlowStartPercent) {
        add_part(model.objects[objs_idx[0]], flow_resource_path("m8.amf").string(), Vec3d{ kIndicatorHorizontalOffset * xyScale,0,zshift_number }, Vec3d{ xyScale , xyScale, zscale_number});
        add_part(model.objects[objs_idx[1]], flow_resource_path("m6.amf").string(), Vec3d{ kIndicatorHorizontalOffset * xyScale,0,zshift_number }, Vec3d{ xyScale , xyScale, zscale_number});
        add_part(model.objects[objs_idx[kCenterCubeIndex]], flow_resource_path("m4.amf").string(), Vec3d{ kIndicatorHorizontalOffset * xyScale,0,zshift_number }, Vec3d{ xyScale , xyScale, zscale_number});
        add_part(model.objects[objs_idx[kFourthCubeIndex]], flow_resource_path("m2.amf").string(), Vec3d{ kIndicatorHorizontalOffset * xyScale,0,zshift_number }, Vec3d{ xyScale , xyScale, zscale_number});
        add_part(model.objects[objs_idx[kFifthCubeIndex]], flow_resource_path("_0.amf").string(), Vec3d{ kIndicatorHorizontalOffset * xyScale,0,zshift_number }, Vec3d{ xyScale , xyScale, zscale_number});
    } else if (delta == kFineFlowStepPercent && start == kFineAboveFlowStartPercent) {
        add_part(model.objects[objs_idx[0]], flow_resource_path("_0.amf").string(), Vec3d{ kIndicatorHorizontalOffset * xyScale,0,zshift_number }, Vec3d{ xyScale , xyScale, zscale_number});
        add_part(model.objects[objs_idx[1]], flow_resource_path("p2.amf").string(), Vec3d{ kIndicatorHorizontalOffset * xyScale,0,zshift_number }, Vec3d{ xyScale , xyScale, zscale_number});
        add_part(model.objects[objs_idx[kCenterCubeIndex]], flow_resource_path("p4.amf").string(), Vec3d{ kIndicatorHorizontalOffset * xyScale,0,zshift_number }, Vec3d{ xyScale , xyScale, zscale_number});
        add_part(model.objects[objs_idx[kFourthCubeIndex]], flow_resource_path("p6.amf").string(), Vec3d{ kIndicatorHorizontalOffset * xyScale,0,zshift_number }, Vec3d{ xyScale , xyScale, zscale_number});
        add_part(model.objects[objs_idx[kFifthCubeIndex]], flow_resource_path("p8.amf").string(), Vec3d{ kIndicatorHorizontalOffset * xyScale,0,zshift_number }, Vec3d{ xyScale , xyScale, zscale_number});
    }
    for (size_t i = 0; i < kFlowCubeCount; i++) {
        translate_from_rotation(i, Vec3d{ kIndicatorHorizontalOffset * xyScale, 0, zscale / kModelHeightCenterDivisor - z_origin });
        add_part(model.objects[objs_idx[i]], flow_resource_path("O.amf").string(),
          Vec3d{ 0,0, zscale / 2.0 + z_origin + layer_height / 2.0 }, Vec3d{xyScale , xyScale, layer_height / CalibrationConstants::kDesignFirstLayerHeightMm}); // base: 0.2mm height
    }

    
    /// --- translate ---;
    bool has_to_arrange = true;
    const double brim_width = nozzle_diameter * 3.5;

    /// --- main config, please modify object config when possible ---
    DynamicPrintConfig new_print_config = *print_config; //make a copy
    new_print_config.set_key_value("complete_objects", std::make_unique<ConfigOptionBool>(true));
    //if skirt, use only one
    if (print_config->option<ConfigOptionInt>("skirts")->get_int() > 0 && print_config->option<ConfigOptionInt>("skirt_height")->get_int() > 0) {
        new_print_config.set_key_value("complete_objects_one_skirt", std::make_unique<ConfigOptionBool>(true));
    }

    /// --- custom config ---
    for (size_t i = 0; i < kFlowCubeCount; i++) {
        //brim to have some time to build up pressure in the nozzle
        model.objects[objs_idx[i]]->config.set_key_value("brim_width", std::make_unique<ConfigOptionFloat>(brim_width));
        model.objects[objs_idx[i]]->config.set_key_value("thin_perimeters", std::make_unique<ConfigOptionPercent>(0));
        model.objects[objs_idx[i]]->config.set_key_value("external_perimeter_overlap", std::make_unique<ConfigOptionPercent>(kPerimeterOverlapPercent));
        model.objects[objs_idx[i]]->config.set_key_value("perimeter_overlap", std::make_unique<ConfigOptionPercent>(kPerimeterOverlapPercent));
        model.objects[objs_idx[i]]->config.set_key_value("brim_ears", std::make_unique<ConfigOptionBool>(false));
        model.objects[objs_idx[i]]->config.set_key_value("perimeters", std::make_unique<ConfigOptionInt>(kCalibrationPerimeterCount));
        model.objects[objs_idx[i]]->config.set_key_value("only_one_perimeter_top", std::make_unique<ConfigOptionBool>(true));
        model.objects[objs_idx[i]]->config.set_key_value("enforce_full_fill_volume", std::make_unique<ConfigOptionBool>(true));
        model.objects[objs_idx[i]]->config.set_key_value("solid_infill_every_layers", std::make_unique<ConfigOptionInt>(kSolidInfillEveryLayer));
        model.objects[objs_idx[i]]->config.set_key_value("thin_walls", std::make_unique<ConfigOptionBool>(true));
        model.objects[objs_idx[i]]->config.set_key_value("thin_walls_min_width", std::make_unique<ConfigOptionFloatOrPercent>(kThinWallsMinWidthPercent,true));
        model.objects[objs_idx[i]]->config.set_key_value("gap_fill_enabled", std::make_unique<ConfigOptionBool>(true)); 
        model.objects[objs_idx[i]]->config.set_key_value("layer_height", std::make_unique<ConfigOptionFloat>(layer_height));
        model.objects[objs_idx[i]]->config.set_key_value("first_layer_height", std::make_unique<ConfigOptionFloatOrPercent>(first_layer_height, false));
        model.objects[objs_idx[i]]->config.set_key_value("external_infill_margin", std::make_unique<ConfigOptionFloatOrPercent>(kExternalInfillMarginPercent, true));
        model.objects[objs_idx[i]]->config.set_key_value("solid_fill_pattern", std::make_unique<ConfigOptionEnum<InfillPattern>>(ipRectilinear));
        model.objects[objs_idx[0]]->config.set_key_value("infill_filled_solid", std::make_unique<ConfigOptionBool>(true));
        model.objects[objs_idx[i]]->config.set_key_value("top_fill_pattern", std::make_unique<ConfigOptionEnum<InfillPattern>>(ipMonotonic));
        //disable ironing post-process
        model.objects[objs_idx[i]]->config.set_key_value("ironing", std::make_unique<ConfigOptionBool>(false));
        //set extrusion mult: 80 90 100 110 120
        model.objects[objs_idx[i]]->config.set_key_value("print_extrusion_multiplier", std::make_unique<ConfigOptionPercent>(start + static_cast<float>(i) * delta));
    }

    //update plater
    //GLCanvas3D::set_warning_freeze(false);
    this->gui_app->get_tab(Preset::TYPE_FFF_PRINT)->load_config(new_print_config);
    plat->on_config_change(new_print_config);
    plat->changed_objects(objs_idx);
    this->gui_app->get_tab(Preset::TYPE_FFF_PRINT)->update_dirty();
    //update everything, easier to code.
    ObjectList* obj = this->gui_app->obj_list();
    obj->update_after_undo_redo();
    freeze_gui.reset();
    // arrange if needed, after new settings, to take them into account
    if (has_to_arrange) {
        //update print config (done at reslice but we need it here)
        if (plat->printer_technology() == ptFFF)
            plat->fff_print().apply(plat->model(), *plat->config());
        Worker &ui_job_worker = plat->get_ui_job_worker();
        plat->arrange(ui_job_worker, false);
        ui_job_worker.wait_for_current_job(CalibrationConstants::kArrangeJobTimeoutMs);
    }

    plat->reslice();

    if (autocenter) {
        //re-enable auto-center after this calibration.
        gui_app->app_config->set("autocenter", "1");
    }

    close_dialog();
}

} // namespace GUI
} // namespace Slic3r
