#include "CalibrationRetractionDialog.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/AppConfig.hpp"
#include "GLCanvas3D.hpp"
#include "GUI.hpp"
#include "GUI_ObjectList.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
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

static constexpr int STEP_CHOICE_COUNT = 5;
static constexpr int DEFAULT_STEP_COUNT_SELECTION = 5;
static constexpr int TEMPERATURE_INCREMENT = 5;
static constexpr int TEMPERATURE_ROUNDING_OFFSET = 2;
static constexpr int TWO_ITEM_SELECTION_INDEX = 2;
static constexpr int FIVE_ITEM_SELECTION_INDEX = 5;
static constexpr size_t TWO_ITEM_COUNT = 2;
static constexpr size_t FIVE_ITEM_COUNT = 5;
static constexpr size_t SLOWDOWN_OPTION_COUNT = 5;
static constexpr double HALF_DIVISOR = 2.0;
static constexpr size_t EXPECTED_VOLUME_COUNT = 2;
static constexpr int PERIMETER_COUNT = 2;
static constexpr int BOTTOM_SOLID_LAYER_COUNT = 2;
static constexpr int THIN_WALL_MIN_WIDTH_PERCENT = 2;
static constexpr int EXTRA_VOLUME_COUNT = 2;

void CalibrationRetractionDialog::create_buttons(wxStdDialogButtonSizer* buttons){
    const wxSize size(6 * em_unit(), wxDefaultCoord);
    wxString choices_steps[] = { "0.1","0.2","0.5","1","2" };
    //steps = new wxComboBox(this, wxID_ANY, wxString{ "0.2" }, wxDefaultPosition, wxDefaultSize, 5, choices_steps);
    steps = new ComboBox(this, wxID_ANY, wxString{ "0.2" }, wxDefaultPosition, size, STEP_CHOICE_COUNT, choices_steps);
    steps->SetToolTip(_L("Each militer add this value to the retraction value."));
    steps->SetSelection(1);
    wxString choices_nb[] = { "2","4","6","8","10","15","20","25" };
    //nb_steps = new wxComboBox(this, wxID_ANY, wxString{ "15" }, wxDefaultPosition, wxDefaultSize, 8, choices_nb);
    nb_steps = new ComboBox(this, wxID_ANY, wxString{ "15" }, wxDefaultPosition, size, 8, choices_nb);
    nb_steps->SetToolTip(_L("Select the number milimeters for the tower."));
    nb_steps->SetSelection(DEFAULT_STEP_COUNT_SELECTION);
    //wxString choices_start[] = { "current","260","250","240","230","220","210" };
    //start_step = new wxComboBox(this, wxID_ANY, wxString{ "current" }, wxDefaultPosition, wxDefaultSize, 7, choices_start);
    //start_step->SetToolTip(_(L("Select the highest temperature to test for.")));
    //start_step->SetSelection(0);
    const DynamicPrintConfig* filament_config = this->gui_app->get_tab(Preset::TYPE_FFF_FILAMENT)->get_config();
    int temp = int((TEMPERATURE_ROUNDING_OFFSET + filament_config->option<ConfigOptionInts>("temperature")->get_at(0)) / TEMPERATURE_INCREMENT) * TEMPERATURE_INCREMENT;
    temp_start = new wxTextCtrl(this, wxID_ANY, std::to_string(temp), wxDefaultPosition, size);
    temp_start->SetToolTip(_L("Note that only Multiple of 5 can be engraved in the part"));
    wxString choices_decr[] = { _L("one test"),_L("2x10°"),_L("3x10°"), _L("4x10°"), _L("3x5°"), _L("5x5°") };
    //decr_temp = new wxComboBox(this, wxID_ANY, wxString{ "current" }, wxDefaultPosition, wxDefaultSize, 6, choices_decr);
    decr_temp = new ComboBox(this, wxID_ANY, wxString{"current"}, wxDefaultPosition, {15 * em_unit(), wxDefaultCoord}, 6, choices_decr);
    decr_temp->SetToolTip(_L("Select the number tower to print, and by how many degrees C to decrease each time."));
    decr_temp->SetSelection(0);
    //decr_temp->SetEditable(false);

    buttons->Add(new wxStaticText(this, wxID_ANY, _L("Step:")));
    buttons->Add(steps);
    buttons->AddSpacer(15);
    buttons->Add(new wxStaticText(this, wxID_ANY, _L("Height:")));
    buttons->Add(nb_steps);
    buttons->AddSpacer(20);

    buttons->Add(new wxStaticText(this, wxID_ANY, _L("Start temp:")));
    buttons->Add(temp_start);
    buttons->AddSpacer(15);
    buttons->Add(new wxStaticText(this, wxID_ANY, _L("Temp decr:")));
    buttons->Add(decr_temp);
    buttons->AddSpacer(20);

    wxButton* bt = new wxButton(this, wxID_SETUP, _L("Remove fil. slowdown"));
    bt->Bind(wxEVT_BUTTON, &CalibrationRetractionDialog::remove_slowdown, this);
    buttons->Add(bt);

    buttons->AddSpacer(30);

    bt = new wxButton(this, wxID_FILE1, _L("Generate"));
    bt->Bind(wxEVT_BUTTON, &CalibrationRetractionDialog::create_geometry, this);
    buttons->Add(bt);
}

void CalibrationRetractionDialog::remove_slowdown(wxCommandEvent& event_args) {

    const DynamicPrintConfig* filament_config = this->gui_app->get_tab(Preset::TYPE_FFF_FILAMENT)->get_config();
    DynamicPrintConfig new_filament_config = *filament_config; //make a copy

    const ConfigOptionFloats *fil_conf = filament_config->option<ConfigOptionFloats>("slowdown_below_layer_time");
    auto new_slowdown = std::make_unique<ConfigOptionFloats>(SLOWDOWN_OPTION_COUNT);
    new_slowdown->set(*fil_conf);
    new_slowdown->set_at(0, 0);
    new_filament_config.set_key_value("slowdown_below_layer_time", std::move(new_slowdown));

    fil_conf = filament_config->option<ConfigOptionFloats>("fan_below_layer_time");
    auto new_fan_time = std::make_unique<ConfigOptionFloats>(60);
    new_fan_time->set(*fil_conf);
    new_fan_time->set_at(0, 0);
    new_filament_config.set_key_value("fan_below_layer_time", std::move(new_fan_time));

    this->gui_app->get_tab(Preset::TYPE_FFF_FILAMENT)->load_config(new_filament_config);
    this->main_frame->plater()->on_config_change(new_filament_config);
    this->gui_app->get_tab(Preset::TYPE_FFF_FILAMENT)->update_dirty();

}

void CalibrationRetractionDialog::create_geometry(wxCommandEvent& event_args) {
    Plater* plat = this->main_frame->plater();
    Model& model = plat->model();
    if (!plat->new_project(L("Retraction calibration")))
        return;
    // wait for slicing end if needed
    wxGetApp().Yield();
    
    std::unique_ptr<wxWindowUpdateLocker> freeze_gui = std::make_unique<wxWindowUpdateLocker>(this);
    bool autocenter = gui_app->app_config->get("autocenter") == "1";
    if (autocenter) {
        //disable aut-ocenter for this calibration.
        gui_app->app_config->set("autocenter", "0");
    }

    long nb_retract = 1;
    if (!nb_steps->GetValue().ToLong(&nb_retract)) {
        nb_retract = 15;
    }
    size_t nb_items = 1;
    if (decr_temp->GetSelection() == 1) {
        nb_items = TWO_ITEM_COUNT;
    } else if (decr_temp->GetSelection() == TWO_ITEM_SELECTION_INDEX || decr_temp->GetSelection() == 4) {
        nb_items = 3;
    } else if (decr_temp->GetSelection() == 3) {
        nb_items = 4;
    } else if (decr_temp->GetSelection() == FIVE_ITEM_SELECTION_INDEX) {
        nb_items = FIVE_ITEM_COUNT;
    }
    int temp_decr = (decr_temp->GetSelection() < 4) ? 10 : TEMPERATURE_INCREMENT;


    std::vector<std::string> items;
    for (size_t i = 0; i < nb_items; i++)
        items.emplace_back((boost::filesystem::path(Slic3r::resources_dir()) / "calibration" / "retraction" / "retraction_calibration.amf").string());
    std::vector<size_t> objs_idx = plat->load_files(items, LoadFileOption::LoadModel | LoadFileOption::DontUpdateDirs);


    assert(objs_idx.size() == nb_items);
    const DynamicPrintConfig* print_config = this->gui_app->get_tab(Preset::TYPE_FFF_PRINT)->get_config();
    const DynamicPrintConfig* printer_config = this->gui_app->get_tab(Preset::TYPE_PRINTER)->get_config();
    const DynamicPrintConfig* filament_config = this->gui_app->get_tab(Preset::TYPE_FFF_FILAMENT)->get_config();
    DynamicPrintConfig full_print_config;
    full_print_config.apply(*print_config);
    full_print_config.apply(*printer_config);
    full_print_config.apply(*filament_config);

    // check if the printer has use_firmware_retraction
    if (printer_config->opt_bool("use_firmware_retraction")) {
        MessageDialog dialog(this, _L("The current printer profile has the firmware retraction enabled. This calibration can't work with this setting enabled."), _L("Firmware retraction enabled"),
            wxICON_WARNING | wxOK);
        dialog.Show();
        return;
    }

    double retraction_start = 0;
    std::string str = temp_start->GetValue().ToStdString();
    int temp = int((TEMPERATURE_ROUNDING_OFFSET + filament_config->option<ConfigOptionInts>("temperature")->get_at(0)) / TEMPERATURE_INCREMENT) * TEMPERATURE_INCREMENT;
    int first_layer_temp = filament_config->option<ConfigOptionInts>("first_layer_temperature")->get_at(0);
    if (str.find_first_not_of("0123456789") == std::string::npos)
        temp = std::atoi(str.c_str());

    double retraction_steps = 0.01;
    if (!steps->GetValue().ToDouble(&retraction_steps)) {
        retraction_steps = 0.1;
    }

    /// --- scale ---
    // model is created for a 0.4 nozzle, scale xy with nozzle size.
    const ConfigOptionFloats* nozzle_diameter_config = printer_config->option<ConfigOptionFloats>("nozzle_diameter");
    assert(nozzle_diameter_config->size() > 0);
    float nozzle_diameter = nozzle_diameter_config->get_at(0);
    //scale z to have 6 layers
    const ConfigOptionFloatOrPercent* first_layer_height_setting = print_config->option<ConfigOptionFloatOrPercent>("first_layer_height");
    double first_layer_height = first_layer_height_setting->get_abs_value(nozzle_diameter);
    // Keep a printable first layer for tiny nozzles while honouring the user's first_layer_height when larger.
    first_layer_height = std::max(first_layer_height, nozzle_diameter / HALF_DIVISOR);

    float scale = nozzle_diameter / 0.4;
    //do scaling
    if (scale < 0.9 || 1.2 < scale) {
        for (size_t i = 0; i < nb_items; i++)
            model.objects[objs_idx[i]]->scale(scale, scale, scale);
    }

    //add sub-part after scale
    std::vector<std::string> filament_temp_item_name;
    for (size_t id_item = 0; id_item < nb_items; id_item++) {
        int mytemp = temp - temp_decr * id_item;
        if (mytemp <= 285 && mytemp >= 180 && mytemp % TEMPERATURE_INCREMENT == 0) {
            filament_temp_item_name.push_back("t" + std::to_string(mytemp) + ".amf");
            assert(model.objects[objs_idx[id_item]]->volumes.size() == 1);
            add_part(model.objects[objs_idx[id_item]], (boost::filesystem::path(Slic3r::resources_dir()) / "calibration" / "filament_temp" / filament_temp_item_name.back()).string(),
                Vec3d{ 0,0, scale * 0.0 - 4.8 }, Vec3d{ scale,scale,scale });
            assert(model.objects[objs_idx[id_item]]->volumes.size() == EXPECTED_VOLUME_COUNT);
            model.objects[objs_idx[id_item]]->volumes[1]->rotate(PI / HALF_DIVISOR, Vec3d(0, 0, 1));
            model.objects[objs_idx[id_item]]->volumes[1]->rotate(-PI / HALF_DIVISOR, Vec3d(1, 0, 0));
            //model.objects[objs_idx[id_item]]->volumes[1]->rotate(Geometry::deg2rad(plat->config()->opt_float("init_z_rotate")), Axis::Z);
        } else {
            filament_temp_item_name.push_back("");
        }
        for (int num_retract = 0; num_retract < nb_retract; num_retract++) {
            add_part(model.objects[objs_idx[id_item]], 
                (boost::filesystem::path(Slic3r::resources_dir()) / "calibration" / "retraction" / "retraction_calibration_pillar.amf").string(),
                Vec3d{ 0,0,scale * 0.7 - 0.3 + scale * num_retract }, Vec3d{ scale,scale,scale });
        }
    }

    /// --- translate ---;
    bool has_to_arrange = true;

    /// --- custom config ---
    assert(filament_temp_item_name.size() == nb_items);
    assert(model.objects.size() == nb_items);
    for (size_t i = 0; i < nb_items; i++) {
        ModelObject *current_obj = model.objects[objs_idx[i]];
        //speed
        double perimeter_speed = full_print_config.get_computed_value("perimeter_speed");
        double external_perimeter_speed = full_print_config.get_computed_value("external_perimeter_speed");
        //brim to have some time to build up pressure in the nozzle
        current_obj->config.set_key_value("brim_width", std::make_unique<ConfigOptionFloat>(0));
        current_obj->config.set_key_value("perimeters", std::make_unique<ConfigOptionInt>(PERIMETER_COUNT));
        current_obj->config.set_key_value("external_perimeters_first", std::make_unique<ConfigOptionBool>(false));
        current_obj->config.set_key_value("bottom_solid_layers", std::make_unique<ConfigOptionInt>(0));
        for(auto& volume : current_obj->volumes)
            if( volume->name == filament_temp_item_name[i] || volume->name.empty()) // if temperature patch or the main retraction patch (empty name because it's the initial volume)
                volume->config.set_key_value("bottom_solid_layers", std::make_unique<ConfigOptionInt>(BOTTOM_SOLID_LAYER_COUNT));
        current_obj->config.set_key_value("top_solid_layers", std::make_unique<ConfigOptionInt>(0));
        current_obj->config.set_key_value("fill_density", std::make_unique<ConfigOptionPercent>(0));
        //current_obj->config.set_key_value("fill_pattern", std::make_unique<ConfigOptionEnum<InfillPattern>>(ipRectilinear));
        current_obj->config.set_key_value("only_one_perimeter_top", std::make_unique<ConfigOptionBool>(false));
        auto overhangs_width_speed = std::make_unique<ConfigOptionFloatOrPercent>(0, false);
        overhangs_width_speed->set_can_be_disabled(true);
        current_obj->config.set_key_value("overhangs_width_speed", std::move(overhangs_width_speed));
        current_obj->config.set_key_value("thin_walls", std::make_unique<ConfigOptionBool>(true));
        current_obj->config.set_key_value("thin_walls_min_width", std::make_unique<ConfigOptionFloatOrPercent>(THIN_WALL_MIN_WIDTH_PERCENT, true));
        current_obj->config.set_key_value("gap_fill_enabled", std::make_unique<ConfigOptionBool>(false));
        current_obj->config.set_key_value("first_layer_height", std::make_unique<ConfigOptionFloatOrPercent>(nozzle_diameter / HALF_DIVISOR, false));
        current_obj->config.set_key_value("layer_height", std::make_unique<ConfigOptionFloat>(nozzle_diameter / HALF_DIVISOR));
        //temp
        current_obj->config.set_key_value("print_temperature", std::make_unique<ConfigOptionInt>(int(temp - temp_decr * i)));
        current_obj->config.set_key_value("print_first_layer_temperature", std::make_unique<ConfigOptionInt>(first_layer_temp));
        //set retraction override
        
        const int mytemp = temp - temp_decr * i;
        const int extra_vol = (mytemp <= 285 && mytemp >= 180 && mytemp % TEMPERATURE_INCREMENT == 0) ? EXTRA_VOLUME_COUNT : 1;
        for (size_t num_part = extra_vol; num_part < current_obj->volumes.size(); num_part++) {
            current_obj->volumes[num_part]->config.set_key_value("print_retract_length", std::make_unique<ConfigOptionFloat>(retraction_start + num_part * retraction_steps));
            current_obj->volumes[num_part]->config.set_key_value("small_perimeter_speed", std::make_unique<ConfigOptionFloatOrPercent>(external_perimeter_speed, false));
            current_obj->volumes[num_part]->config.set_key_value("perimeter_speed", std::make_unique<ConfigOptionFloatOrPercent>(std::min(external_perimeter_speed, perimeter_speed), false));
            current_obj->volumes[num_part]->config.set_key_value("external_perimeter_speed", std::make_unique<ConfigOptionFloatOrPercent>(external_perimeter_speed, false));
            //current_obj->volumes[num_part + extra_vol]->config.set_key_value("small_perimeter_speed", std::make_unique<ConfigOptionFloatOrPercent>(external_perimeter_speed, false));
            //current_obj->volumes[num_part + extra_vol]->config.set_key_value("infill_speed", std::make_unique<ConfigOptionFloatOrPercent>(std::min(print_config->option<ConfigOptionFloatOrPercent>("infill_speed")->value, 10.*scale)), false);
            
        }
    }

    /// --- main config, please modify object config when possible ---
    if (nb_items > 1) {
        DynamicPrintConfig new_print_config = *print_config; //make a copy
        new_print_config.set_key_value("complete_objects", std::make_unique<ConfigOptionBool>(true));
        //if skirt, use only one
        if (print_config->option<ConfigOptionInt>("skirts")->get_int() > 0 && print_config->option<ConfigOptionInt>("skirt_height")->get_int() > 0) {
            new_print_config.set_key_value("complete_objects_one_skirt", std::make_unique<ConfigOptionBool>(true));
        }
        this->gui_app->get_tab(Preset::TYPE_FFF_PRINT)->load_config(new_print_config);
        this->gui_app->get_tab(Preset::TYPE_FFF_PRINT)->update_dirty();
        plat->on_config_change(new_print_config);
    }

    //update plater
    //GLCanvas3D::set_warning_freeze(false);
    plat->changed_objects(objs_idx);
    //if (plat->printer_technology() == ptFFF)
        //plat->fff_print().full_print_config().apply(plat->config());
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
