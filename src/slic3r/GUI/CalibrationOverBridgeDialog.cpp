#include "CalibrationOverBridgeDialog.hpp"
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
constexpr char kOverBridgeResourceDirectory[] = "over-bridge_tuning";
constexpr char kOverBridgeTestFilename[] = "over-bridge_flow_ratio_test.amf";
} // namespace

void CalibrationOverBridgeDialog::create_buttons(wxStdDialogButtonSizer* buttons){
    wxButton* bt1 = new wxButton(this, wxID_FILE1, _L("'Above the Bridges' flow calibration"));
    wxButton* bt2 = new wxButton(this, wxID_FILE2, _L("'Top Fill' flow calibration"));
    bt1->Bind(wxEVT_BUTTON, &CalibrationOverBridgeDialog::create_geometry1, this);
    bt2->Bind(wxEVT_BUTTON, &CalibrationOverBridgeDialog::create_geometry2, this);
    buttons->Add(bt1);
    buttons->AddSpacer(20);
    buttons->Add(bt2);
}

void CalibrationOverBridgeDialog::create_geometry1(wxCommandEvent& event_args) {
    Plater* plat = this->main_frame->plater();
    if (!plat->new_project(L("Above the Bridges flow calibration")))
        return;
    create_geometry(true);
}
void CalibrationOverBridgeDialog::create_geometry2(wxCommandEvent& event_args) {
    Plater* plat = this->main_frame->plater();
    if (!plat->new_project(L("Top Fill flow calibration")))
        return;
    create_geometry(false);
}
void CalibrationOverBridgeDialog::create_geometry(bool over_bridge) {
    // wait for slicing end if needed
    wxGetApp().Yield();

    Plater* plat = this->main_frame->plater();
    Model& model = plat->model();

    std::unique_ptr<wxWindowUpdateLocker> freeze_gui = std::make_unique<wxWindowUpdateLocker>(this);
    bool autocenter = gui_app->app_config->get("autocenter") == "1";
    if (autocenter) {
        //disable aut-ocenter for this calibration.
        gui_app->app_config->set("autocenter", "0");
    }

    std::vector<size_t> objs_idx = plat->load_files(std::vector<std::string>{
            (boost::filesystem::path(Slic3r::resources_dir()) / kCalibrationResourceDirectory / kOverBridgeResourceDirectory / kOverBridgeTestFilename).string(),
            (boost::filesystem::path(Slic3r::resources_dir()) / kCalibrationResourceDirectory / kOverBridgeResourceDirectory / kOverBridgeTestFilename).string(),
            (boost::filesystem::path(Slic3r::resources_dir()) / kCalibrationResourceDirectory / kOverBridgeResourceDirectory / kOverBridgeTestFilename).string(),
            (boost::filesystem::path(Slic3r::resources_dir()) / kCalibrationResourceDirectory / kOverBridgeResourceDirectory / kOverBridgeTestFilename).string(),
            (boost::filesystem::path(Slic3r::resources_dir()) / kCalibrationResourceDirectory / kOverBridgeResourceDirectory / kOverBridgeTestFilename).string(),
            (boost::filesystem::path(Slic3r::resources_dir()) / kCalibrationResourceDirectory / kOverBridgeResourceDirectory / kOverBridgeTestFilename).string()},
        LoadFileOption::LoadModel | LoadFileOption::DontUpdateDirs);

    assert(objs_idx.size() == 6);
    const DynamicPrintConfig* print_config = this->gui_app->get_tab(Preset::TYPE_FFF_PRINT)->get_config();
    const DynamicPrintConfig* printer_config = this->gui_app->get_tab(Preset::TYPE_PRINTER)->get_config();

    /// --- scale ---
    // model is created for a 0.4 nozzle, scale xy with nozzle size.
    const ConfigOptionFloats* nozzle_diameter_config = printer_config->option<ConfigOptionFloats>("nozzle_diameter");
    assert(nozzle_diameter_config->size() > 0);
    float nozzle_diameter = nozzle_diameter_config->get_at(0);
    float xyz_scale = (0.2 + nozzle_diameter) / 0.6;
    //do scaling
    if (xyz_scale < 0.9 || 1.2 < xyz_scale) {
    } else {
        xyz_scale = 1;
    }
    for (size_t i = 0; i < 6; i++)
        model.objects[objs_idx[i]]->scale(xyz_scale * 1.5f, xyz_scale * 1.5f, xyz_scale);

    // it's rotated but not around the good origin: correct that
    double init_z_rotate_angle = Geometry::deg2rad(plat->config()->opt_float("init_z_rotate"));
    Matrix3d rot_matrix = Eigen::Quaterniond(Eigen::AngleAxisd(init_z_rotate_angle, Vec3d{0,0,1})).toRotationMatrix();
    auto     translate_from_rotation = [&rot_matrix, &model, &objs_idx](int idx, Vec3d &&translation) {
            ModelVolume *vol = model.objects[objs_idx[idx]]->volumes[model.objects[objs_idx[idx]]->volumes.size()-1];
            Geometry::Transformation trsf = vol->get_transformation();
            trsf.set_offset(rot_matrix * translation - translation + trsf.get_offset());
            vol->set_transformation(trsf);
        };

    //add sub-part after scale
    const ConfigOptionFloatOrPercent* first_layer_height = print_config->option<ConfigOptionFloatOrPercent>("first_layer_height");
    float patch_zscale = (first_layer_height->get_abs_value(nozzle_diameter) + nozzle_diameter / 2) / 0.4;
    float zshift =  0.8 * (1 - xyz_scale);
    const boost::filesystem::path bridge_flow_dir =
        boost::filesystem::path(Slic3r::resources_dir()) / kCalibrationResourceDirectory / "bridge_flow";
    for (size_t i = 0; i < 6; i++) {
        model.objects[objs_idx[i]]->rotate(PI / 2, { 0, 0, 1 });
        add_part(model.objects[objs_idx[i]],
                 (bridge_flow_dir / ("f" + std::to_string(100 + i * 5) + ".amf")).string(),
                 Vec3d{ 0, 10 * xyz_scale, zshift }, Vec3d{ 1, 1, patch_zscale });
        translate_from_rotation(i, Vec3d{ 0, 10 * xyz_scale, zshift });
    }

    /// --- translate ---;
    bool has_to_arrange = true;
    /// --- main config, please modify object config when possible ---
    DynamicPrintConfig new_print_config = *print_config; //make a copy
    new_print_config.set_key_value("complete_objects", std::make_unique<ConfigOptionBool>(true));
    //if skirt, use only one
    if (print_config->option<ConfigOptionInt>("skirts")->get_int() > 0 && print_config->option<ConfigOptionInt>("skirt_height")->get_int() > 0) {
        new_print_config.set_key_value("complete_objects_one_skirt", std::make_unique<ConfigOptionBool>(true));
    }

    /// --- custom config ---
    for (size_t i = 0; i < 6; i++) {
        model.objects[objs_idx[i]]->config.set_key_value("perimeters", std::make_unique<ConfigOptionInt>(2));
        model.objects[objs_idx[i]]->config.set_key_value("bottom_solid_layers", std::make_unique<ConfigOptionInt>(1)); // at least the first, to prevent adhesion issues.
        model.objects[objs_idx[i]]->config.set_key_value("top_solid_layers", std::make_unique<ConfigOptionInt>(3));
        model.objects[objs_idx[i]]->config.set_key_value("fill_density", std::make_unique<ConfigOptionPercent>(5.5));
        model.objects[objs_idx[i]]->config.set_key_value("fill_pattern", std::make_unique<ConfigOptionEnum<InfillPattern>>(ipRectilinear));
        model.objects[objs_idx[i]]->config.set_key_value("infill_dense", std::make_unique<ConfigOptionBool>(false));
        model.objects[objs_idx[i]]->config.set_key_value("ironing", std::make_unique<ConfigOptionBool>(false));
        //calibration setting. Use 100 & 5 step as it's the numbers printed on the samples
        if (over_bridge) {
            model.objects[objs_idx[i]]->config.set_key_value("over_bridge_flow_ratio", std::make_unique<ConfigOptionPercent>(/*print_config->option<ConfigOptionPercent>("over_bridge_flow_ratio")->get_abs_value(100)*/100 + i * 5));
        } else {
            model.objects[objs_idx[i]]->config.set_key_value("fill_top_flow_ratio", std::make_unique<ConfigOptionPercent>(/*print_config->option<ConfigOptionPercent>("fill_top_flow_ratio")->get_abs_value(100)*/100 + i * 5));
        }
        model.objects[objs_idx[i]]->config.set_key_value("layer_height", std::make_unique<ConfigOptionFloat>(nozzle_diameter / 2));
        model.objects[objs_idx[i]]->config.set_key_value("external_infill_margin", std::make_unique<ConfigOptionFloatOrPercent>(400,true));
        model.objects[objs_idx[i]]->config.set_key_value("top_fill_pattern", std::make_unique<ConfigOptionEnum<InfillPattern>>(ipSmooth));
        model.objects[objs_idx[i]]->config.set_key_value("fill_angle", std::make_unique<ConfigOptionFloat>(45));
    }

    //update plater
    //GLCanvas3D::set_warning_freeze(false);
    this->gui_app->get_tab(Preset::TYPE_FFF_PRINT)->load_config(new_print_config);
    plat->on_config_change(new_print_config);
    plat->changed_objects(objs_idx);
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
        ui_job_worker.wait_for_current_job(20000);
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
