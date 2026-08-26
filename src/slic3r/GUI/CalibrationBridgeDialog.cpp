#include "CalibrationBridgeDialog.hpp"
#include "I18N.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/AppConfig.hpp"
//#include "Jobs/ArrangeJob2.hpp"
#include "GLCanvas3D.hpp"
#include "GUI.hpp"
#include "GUI_ObjectList.hpp"
#include "Plater.hpp"
#include "Tab.hpp"
#include <wx/scrolwin.h>
#include <wx/file.h>
#include <wx/wupdlock.h>


namespace Slic3r {
namespace GUI {

namespace {
constexpr float  kLabelZShiftBaseMm        = 2.3f;  // vertical offset for engraved % labels before scale
constexpr float  kLabelHeightMm            = 4.6f;  // engraved label geometry height
constexpr double kLabelXOffsetMm           = -10.0; // label position relative to bridge test body
constexpr int    kEngravedStepMinPercent   = 20;    // only multiples of 5 in this range have AMF glyphs
constexpr int    kEngravedStepMaxPercent   = 180;
constexpr int    kEngravedStepMultiple     = 5;
constexpr int    kBridgePerimeters         = 2;
constexpr int    kBridgeBottomSolidLayers  = 2;
constexpr double kLayerHeightNozzleFactor  = 0.5;   // layer height as fraction of nozzle diameter
constexpr double kLayerHeightEpsilonMm     = 0.01;
constexpr int    kPercentScale             = 100;
constexpr int    kHalfPercentScale         = 50;
constexpr double kDefaultZStepMm           = 0.1;
constexpr double kDefaultMaxLayerFrac      = 0.75;
constexpr int    kArrangeJobTimeoutMs      = 20000;
constexpr int    kDefaultStepPercent       = 10;
constexpr int    kDefaultNbItemsFallback   = 10;
constexpr double kBrimNozzleMultiple       = 5.;
} // namespace

void CalibrationBridgeDialog::create_buttons(wxStdDialogButtonSizer* buttons) {
    using namespace CalibrationConstants;
    const wxSize size(kComboFieldWidthEm * em_unit(), wxDefaultCoord);
    wxString choices_steps[] = { "5","10","15" };
    //steps = new wxComboBox(this, wxID_ANY, wxString{ "10" }, wxDefaultPosition, wxDefaultSize, 3, choices_steps);
    steps = new ComboBox(this, wxID_ANY, wxString{ "10" }, wxDefaultPosition, size, kStepChoiceCount, choices_steps);
    steps->SetToolTip(_L("Select the step in % between two tests.\nNote that only multiple of 5 are engraved on the parts."));
    steps->SetSelection(kDefaultStepSelection);
    wxString choices_nb[] = { "1","2","3","4","5","6" };
    constexpr int kNbTestChoiceCount = 6;
    //nb_tests = new wxComboBox(this, wxID_ANY, wxString{ "5" }, wxDefaultPosition, wxDefaultSize, 6, choices_nb);
    nb_tests = new ComboBox(this, wxID_ANY, wxString{ "5" }, wxDefaultPosition, size, kNbTestChoiceCount, choices_nb);
    nb_tests->SetToolTip(_L("Select the number of tests"));
    nb_tests->SetSelection(kDefaultNbTestsSelection);

    buttons->Add(new wxStaticText(this, wxID_ANY,_L("Step:")));
    buttons->Add(steps);
    buttons->AddSpacer(kSpacerAfterStepsPx);
    buttons->Add(new wxStaticText(this, wxID_ANY, _L("Nb tests:")));
    buttons->Add(nb_tests);
    buttons->AddSpacer(kSpacerBeforeActionPx);
    wxButton* bt = new wxButton(this, wxID_FILE1, _L("Test Flow Ratio"));
    bt->Bind(wxEVT_BUTTON, &CalibrationBridgeDialog::create_geometry_flow_ratio, this);
    buttons->Add(bt);
    //buttons->AddSpacer(15);
    //bt = new wxButton(this, wxID_FILE2, _(L("Test Overlap")));
    //bt->Bind(wxEVT_BUTTON, &CalibrationBridgeDialog::create_geometry_overlap, this);
    //buttons->Add(bt);
}

void CalibrationBridgeDialog::create_geometry(const std::string& setting_to_test, bool add) {
    Plater* plat = this->main_frame->plater();
    Model& model = plat->model();
    if (!plat->new_project(L("Bridge calibration")))
        return;
    // wait for slicing end if needed
    wxGetApp().Yield();
    
    std::unique_ptr<wxWindowUpdateLocker> freeze_gui = std::make_unique<wxWindowUpdateLocker>(this);
    bool autocenter = gui_app->app_config->get("autocenter") == "1";
    if (autocenter) {
        //disable auto-center for this calibration.
        gui_app->app_config->set("autocenter", "0");
    }

    long step = kDefaultStepPercent;
    if (!steps->GetValue().ToLong(&step)) {
        step = kDefaultStepPercent;
    }
    long parsed_nb_items = kDefaultNbItemsFallback;
    // The count comes from a text field, so it can parse as 0 or negative. Comparing
    // `size_t i < nb_items` then converts the negative long to ~1.8e19 and the loop
    // spins forever loading objects, so treat it like the unparseable case and fall
    // back to the default.
    if (!nb_tests->GetValue().ToLong(&parsed_nb_items) || parsed_nb_items < 1) {
        parsed_nb_items = kDefaultNbItemsFallback;
    }
    const size_t nb_items = size_t(parsed_nb_items);

    std::vector<std::string> items;
    for (size_t i = 0; i < nb_items; i++)
        items.emplace_back((boost::filesystem::path(Slic3r::resources_dir()) / "calibration" / "bridge_flow" / "bridge_test.amf").string());
    std::vector<size_t> objs_idx = plat->load_files(items, LoadFileOption::LoadModel | LoadFileOption::DontUpdateDirs);

    assert(objs_idx.size() == nb_items);
    const DynamicPrintConfig* print_config = this->gui_app->get_tab(Preset::TYPE_FFF_PRINT)->get_config();
    const DynamicPrintConfig* filament_config = this->gui_app->get_tab(Preset::TYPE_FFF_FILAMENT)->get_config();
    const DynamicPrintConfig* printer_config = this->gui_app->get_tab(Preset::TYPE_PRINTER)->get_config();
    DynamicPrintConfig full_print_config;
    full_print_config.apply(*print_config);
    full_print_config.apply(*filament_config);
    full_print_config.apply(*printer_config);
    full_print_config.set_key_value("extruder_id", std::make_unique<ConfigOptionInt>(0));

    /// --- scale ---
    // model is created for a 0.4 nozzle, scale xy with nozzle size.
    const ConfigOptionFloats* nozzle_diameter_config = printer_config->option<ConfigOptionFloats>("nozzle_diameter");
    assert(nozzle_diameter_config->size() > 0);
    float nozzle_diameter = nozzle_diameter_config->get_at(0);
    float z_scale = nozzle_diameter / CalibrationConstants::kDesignNozzleDiameterMm;
    //do scaling
    if (z_scale < CalibrationConstants::kXyScaleMinFactor || CalibrationConstants::kXyScaleMaxFactor < z_scale) {
        for (size_t i = 0; i < nb_items; i++)
            model.objects[objs_idx[i]]->scale(1, 1, z_scale);
    } else {
        z_scale = 1;
    }
    
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
    const ConfigOptionPercent* bridge_flow_ratio = print_config->option<ConfigOptionPercent>(setting_to_test);
    int start = bridge_flow_ratio->value;
    float zshift = kLabelZShiftBaseMm * (1 - z_scale);
    for (size_t i = 0; i < nb_items; i++) {
        int step_num = (start + (add ? 1 : -1) * i * step);
        if (step_num < kEngravedStepMaxPercent && step_num > kEngravedStepMinPercent && step_num % kEngravedStepMultiple == 0) {
            const Vec3d label_offset{ kLabelXOffsetMm, 0, zshift + kLabelHeightMm * z_scale };
            add_part(model.objects[objs_idx[i]], (boost::filesystem::path(Slic3r::resources_dir()) / "calibration" / "bridge_flow" / ("f" + std::to_string(step_num) + ".amf")).string(), label_offset, Vec3d{ 1,1,z_scale });
            translate_from_rotation(i, Vec3d{ kLabelXOffsetMm, 0, zshift + kLabelHeightMm * z_scale });
        }
    }
    /// --- translate ---;
    bool has_to_arrange = true;
    const float brim_width = std::max(print_config->option<ConfigOptionFloat>("brim_width")->value, nozzle_diameter * kBrimNozzleMultiple);

    /// --- main config, please modify object config when possible ---
    DynamicPrintConfig new_print_config = *print_config; //make a copy
    new_print_config.set_key_value("complete_objects", std::make_unique<ConfigOptionBool>(true));
    //if skirt, use only one
    if (print_config->option<ConfigOptionInt>("skirts")->get_int() > 0 && print_config->option<ConfigOptionInt>("skirt_height")->get_int() > 0) {
        new_print_config.set_key_value("complete_objects_one_skirt", std::make_unique<ConfigOptionBool>(true));
    }

    /// --- custom config ---
    for (size_t i = 0; i < nb_items; i++) {
        model.objects[objs_idx[i]]->config.set_key_value("brim_width", std::make_unique<ConfigOptionFloat>(brim_width));
        model.objects[objs_idx[i]]->config.set_key_value("brim_ears", std::make_unique<ConfigOptionBool>(false));
        model.objects[objs_idx[i]]->config.set_key_value("perimeters", std::make_unique<ConfigOptionInt>(kBridgePerimeters));
        model.objects[objs_idx[i]]->config.set_key_value("bottom_solid_layers", std::make_unique<ConfigOptionInt>(kBridgeBottomSolidLayers));
        model.objects[objs_idx[i]]->config.set_key_value("gap_fill_enabled", std::make_unique<ConfigOptionBool>(false));
        model.objects[objs_idx[i]]->config.set_key_value(setting_to_test, std::make_unique<ConfigOptionPercent>(start + (add ? 1 : -1) * i * step));
        model.objects[objs_idx[i]]->config.set_key_value("layer_height", std::make_unique<ConfigOptionFloat>(nozzle_diameter * kLayerHeightNozzleFactor));
        model.objects[objs_idx[i]]->config.set_key_value("no_perimeter_unsupported_algo", std::make_unique<ConfigOptionEnum<NoPerimeterUnsupportedAlgo>>(npuaBridges));
        //model.objects[objs_idx[i]]->config.set_key_value("top_fill_pattern", std::make_unique<ConfigOptionEnum<InfillPattern>>(ipSmooth)); /not needed
        model.objects[objs_idx[i]]->config.set_key_value("ironing", std::make_unique<ConfigOptionBool>(false)); // not needed, and it slow down things.
    }
    /// if first ayer height is excactly at the wrong value, the text isn't drawed. Fix that by switching the first layer height just a little bit.
    double first_layer_height = full_print_config.get_computed_value("first_layer_height", 0);
    double layer_height = nozzle_diameter * kLayerHeightNozzleFactor;
    if (layer_height > kLayerHeightEpsilonMm && (int(first_layer_height * kPercentScale) % int(layer_height * kPercentScale)) == int(layer_height * kHalfPercentScale)) {
        double z_step = printer_config->option<ConfigOptionFloat>("z_step")->value;
        if (z_step == 0)
            z_step = kDefaultZStepMm;
        double max_height = full_print_config.get_computed_value("max_layer_height",0);
        if (max_height < EPSILON || !full_print_config.option("max_layer_height")->is_enabled())
            max_height = kDefaultMaxLayerFrac * nozzle_diameter;
        if (max_height > first_layer_height + z_step)
            for (size_t i = 0; i < nb_items; i++)
                model.objects[objs_idx[i]]->config.set_key_value("first_layer_height", std::make_unique<ConfigOptionFloatOrPercent>(first_layer_height + z_step, false));
        else
            for (size_t i = 0; i < nb_items; i++)
                model.objects[objs_idx[i]]->config.set_key_value("first_layer_height", std::make_unique<ConfigOptionFloatOrPercent>(first_layer_height - z_step, false));
    }

    //update plater
    //GLCanvas3D::set_warning_freeze(false);
    this->gui_app->get_tab(Preset::TYPE_FFF_PRINT)->load_config(new_print_config);
    plat->on_config_change(new_print_config);
    plat->changed_objects(objs_idx);
    //this->gui_app->get_tab(Preset::TYPE_FFF_PRINT)->update_dirty();
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
        ui_job_worker.wait_for_current_job(kArrangeJobTimeoutMs);
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
