#include "CalibrationPressureAdvAdaptiveDialog.hpp"
#include "I18N.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/LocalesUtils.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Utils.hpp"
#include "Jobs/ArrangeJob.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GUI_ObjectList.hpp"
#include "Plater.hpp"
#include "Tab.hpp"
#include "wxExtensions.hpp"

#include <algorithm>
#include <string>

namespace Slic3r {
namespace GUI {

void CalibrationPressureAdvAdaptiveDialog::create_buttons(wxStdDialogButtonSizer* buttons)
{
    const wxSize size(6 * em_unit(), wxDefaultCoord);
    const wxSize bt_size(24 * em_unit(), wxDefaultCoord);

    wxString choices_nb[] = { "1", "2", "3", "4", "5", "6", "7", "8" };
    cmb_nb_speed = new ComboBox(this, wxID_ANY, wxString{ "4" }, wxDefaultPosition, size, 8, choices_nb);
    cmb_nb_speed->SetToolTip(_L("Number of flow steps (columns): patches printed at different speeds, i.e. volumetric flows."));
    cmb_nb_speed->SetSelection(3);

    cmb_nb_accel = new ComboBox(this, wxID_ANY, wxString{ "3" }, wxDefaultPosition, size, 8, choices_nb);
    cmb_nb_accel->SetToolTip(_L("Number of acceleration steps (rows)."));
    cmb_nb_accel->SetSelection(2);

    const DynamicPrintConfig* print_config = this->gui_app->get_tab(Preset::TYPE_FFF_PRINT)->get_config();
    float curr_speed = print_config->get_computed_value("solid_infill_speed", 0);
    if (curr_speed <= 0) curr_speed = 60;
    float curr_accel = print_config->get_computed_value("default_acceleration", 0);
    if (curr_accel <= 0) curr_accel = 2000;

    txt_min_speed = new wxTextCtrl(this, wxID_ANY, Slic3r::from_dot_to_local(Slic3r::to_string_nozero(std::max(10.f, curr_speed / 3.f), 4)), wxDefaultPosition, size);
    txt_max_speed = new wxTextCtrl(this, wxID_ANY, Slic3r::from_dot_to_local(Slic3r::to_string_nozero(std::max(20.f, curr_speed * 1.5f), 4)), wxDefaultPosition, size);
    txt_min_accel = new wxTextCtrl(this, wxID_ANY, Slic3r::from_dot_to_local(Slic3r::to_string_nozero(std::max(500.f, curr_accel / 3.f), 1)), wxDefaultPosition, size);
    txt_max_accel = new wxTextCtrl(this, wxID_ANY, Slic3r::from_dot_to_local(Slic3r::to_string_nozero(std::max(1000.f, curr_accel), 1)), wxDefaultPosition, size);
    txt_pa = new wxTextCtrl(this, wxID_ANY, Slic3r::from_dot_to_local(Slic3r::to_string_nozero(0.04, 4)), wxDefaultPosition, size);
    txt_pa->SetToolTip(_L("Fixed pressure-advance value used for the whole grid (adaptive PA is disabled for the calibration print). "
                          "Re-run with a few PA values to bracket the best one per cell."));

    wxBoxSizer* vertical = new wxBoxSizer(wxVERTICAL);
    auto add_row = [&](const wxString& lbl1, wxWindow* c1, const wxString& unit1,
                       const wxString& lbl2, wxWindow* c2, const wxString& unit2) {
        wxBoxSizer* h = new wxBoxSizer(wxHORIZONTAL);
        h->Add(new wxStaticText(this, wxID_ANY, lbl1, wxDefaultPosition, { 15 * em_unit(), -1 }, wxALIGN_RIGHT));
        h->Add(c1);
        h->Add(new wxStaticText(this, wxID_ANY, wxString(" ") + unit1, wxDefaultPosition, { 7 * em_unit(), -1 }, wxALIGN_LEFT));
        h->AddSpacer(15);
        if (c2 != nullptr) {
            h->Add(new wxStaticText(this, wxID_ANY, lbl2, wxDefaultPosition, { 9 * em_unit(), -1 }, wxALIGN_RIGHT));
            h->Add(c2);
            h->Add(new wxStaticText(this, wxID_ANY, wxString(" ") + unit2, wxDefaultPosition, { 7 * em_unit(), -1 }, wxALIGN_LEFT));
        }
        vertical->Add(h);
        vertical->AddSpacer(4);
    };

    add_row(_L("Speed steps:"), cmb_nb_speed, "", _L("Accel steps:"), cmb_nb_accel, "");
    add_row(_L("min speed:"), txt_min_speed, _L("mm/s"), _L("max speed:"), txt_max_speed, _L("mm/s"));
    add_row(_L("min accel:"), txt_min_accel, _L("mm/s2"), _L("max accel:"), txt_max_accel, _L("mm/s2"));
    add_row(_L("Fixed PA:"), txt_pa, "", "", nullptr, "");

    wxButton* bt = new wxButton(this, wxID_FILE1, _L("Generate flow x acceleration grid"), wxDefaultPosition, bt_size);
    bt->Bind(wxEVT_BUTTON, &CalibrationPressureAdvAdaptiveDialog::create_geometry, this);
    vertical->Add(bt);

    buttons->Add(vertical);
}

void CalibrationPressureAdvAdaptiveDialog::create_geometry(wxCommandEvent& event_args)
{
    Plater* plat = this->main_frame->plater();
    Model& model = plat->model();
    if (!plat->new_project(L("Adaptive PA calibration")))
        return;
    wxGetApp().Yield();

    bool autocenter = gui_app->app_config->get("autocenter") == "1";
    if (autocenter)
        gui_app->app_config->set("autocenter", "0");

    int   nb_speed  = std::max(1, std::stoi(cmb_nb_speed->GetValue().ToStdString()));
    int   nb_accel  = std::max(1, std::stoi(cmb_nb_accel->GetValue().ToStdString()));
    float min_speed = parse_float_all_locale(txt_min_speed->GetValue().ToStdString());
    float max_speed = parse_float_all_locale(txt_max_speed->GetValue().ToStdString());
    float min_accel = parse_float_all_locale(txt_min_accel->GetValue().ToStdString());
    float max_accel = parse_float_all_locale(txt_max_accel->GetValue().ToStdString());
    float test_pa   = parse_float_all_locale(txt_pa->GetValue().ToStdString());

    const DynamicPrintConfig* print_config    = this->gui_app->get_tab(Preset::TYPE_FFF_PRINT)->get_config();
    const DynamicPrintConfig* printer_config  = this->gui_app->get_tab(Preset::TYPE_PRINTER)->get_config();
    const DynamicPrintConfig* filament_config = this->gui_app->get_tab(Preset::TYPE_FFF_FILAMENT)->get_config();

    float nz           = printer_config->option("nozzle_diameter")->get_float(0);
    float layer_height = print_config->option("layer_height")->get_float();
    layer_height = std::max(layer_height, float(print_config->get_abs_value("first_layer_height", nz)));

    // helper to interpolate a grid axis value
    auto axis_value = [](int idx, int count, float lo, float hi) -> float {
        return (count > 1) ? (lo + idx * (hi - lo) / (count - 1)) : lo;
    };

    model.clear_objects();
    std::vector<ModelObject*> objs;
    for (int ja = 0; ja < nb_accel; ja++) {
        for (int is = 0; is < nb_speed; is++) {
            const float speed = axis_value(is, nb_speed, min_speed, max_speed);
            const float accel = axis_value(ja, nb_accel, min_accel, max_accel);
            const std::string name = "s" + std::to_string(int(speed + 0.5f)) + "_a" + std::to_string(int(accel + 0.5f));
            ModelObject* obj = model.add_object(name.c_str(), "", Slic3r::make_cube(10, 10, 4));
            obj->add_instance();
            objs.push_back(obj);
        }
    }

    /// --- main config (modify per-object where possible) ---
    DynamicPrintConfig new_print_config = *print_config;
    new_print_config.set_key_value("complete_objects", new ConfigOptionBool(false));

    DynamicPrintConfig new_printer_config = *printer_config;

    DynamicPrintConfig new_filament_config = *filament_config;
    // Print the whole grid at one fixed PA; disable adaptive PA so the static value is what's emitted.
    new_filament_config.set_key_value("filament_adaptive_pressure_advance", new ConfigOptionBools({ false }));
    new_filament_config.set_key_value("filament_pressure_advance",
        (new ConfigOptionFloats({ double(test_pa) }))->set_can_be_disabled(false));

    /// --- per-object config: one cube per (speed, acceleration) cell ---
    int idx = 0;
    for (int ja = 0; ja < nb_accel; ja++) {
        for (int is = 0; is < nb_speed; is++, idx++) {
            const float  speed = axis_value(is, nb_speed, min_speed, max_speed);
            const float  accel = axis_value(ja, nb_accel, min_accel, max_accel);
            ModelObject* o     = objs[idx];

            o->config.set_key_value("layer_height", new ConfigOptionFloat(layer_height));
            o->config.set_key_value("first_layer_height", new ConfigOptionFloatOrPercent(layer_height, false));
            o->config.set_key_value("brim_width", new ConfigOptionFloat(0));
            o->config.set_key_value("ironing", new ConfigOptionBool(false));

            // speed -> volumetric flow (one axis of the grid)
            o->config.set_key_value("perimeter_speed", new ConfigOptionFloatOrPercent(speed, false));
            o->config.set_key_value("external_perimeter_speed", new ConfigOptionFloatOrPercent(speed, false));
            o->config.set_key_value("solid_infill_speed", new ConfigOptionFloatOrPercent(speed, false));
            o->config.set_key_value("top_solid_infill_speed", new ConfigOptionFloatOrPercent(speed, false));
            o->config.set_key_value("infill_speed", new ConfigOptionFloatOrPercent(speed, false));

            // acceleration (the other axis)
            o->config.set_key_value("default_acceleration", new ConfigOptionFloatOrPercent(accel, false));
            o->config.set_key_value("perimeter_acceleration", new ConfigOptionFloatOrPercent(accel, false));
            o->config.set_key_value("external_perimeter_acceleration", new ConfigOptionFloatOrPercent(accel, false));
            o->config.set_key_value("solid_infill_acceleration", new ConfigOptionFloatOrPercent(accel, false));
            o->config.set_key_value("top_solid_infill_acceleration", new ConfigOptionFloatOrPercent(accel, false));
            o->config.set_key_value("infill_acceleration", new ConfigOptionFloatOrPercent(accel, false));
        }
    }

    // --- push configs to the tabs / plater ---
    this->gui_app->get_tab(Preset::TYPE_FFF_PRINT)->load_config(new_print_config);
    plat->on_config_change(new_print_config);
    this->gui_app->get_tab(Preset::TYPE_PRINTER)->load_config(new_printer_config);
    plat->on_config_change(new_printer_config);
    this->gui_app->get_tab(Preset::TYPE_FFF_FILAMENT)->load_config(new_filament_config);
    plat->on_config_change(new_filament_config);
    this->gui_app->get_tab(Preset::TYPE_FFF_PRINT)->update_dirty();
    this->gui_app->get_tab(Preset::TYPE_PRINTER)->update_dirty();
    this->gui_app->get_tab(Preset::TYPE_FFF_FILAMENT)->update_dirty();

    ObjectList* obj_list = this->gui_app->obj_list();
    obj_list->update_after_undo_redo();

    // arrange the grid on the bed, then slice
    if (plat->printer_technology() == ptFFF)
        plat->fff_print().apply(plat->model(), *plat->config());
    Worker& ui_job_worker = plat->get_ui_job_worker();
    plat->arrange(ui_job_worker, false);
    ui_job_worker.wait_for_current_job(20000);

    plat->reslice();

    if (autocenter)
        gui_app->app_config->set("autocenter", "1");
}

} // namespace GUI
} // namespace Slic3r
