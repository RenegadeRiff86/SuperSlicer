///|/ Copyright (c) 2026 Stan Elston (RenegadeRiff86)
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher.
///|/
#include "CalibrationPressureAdvAdaptiveDialog.hpp"
#include "I18N.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/LocalesUtils.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/Utils.hpp"
#include "Jobs/ArrangeJob.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GUI_ObjectList.hpp"
#include "Plater.hpp"
#include "Tab.hpp"
#include "wxExtensions.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include <wx/scrolwin.h>
#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <wx/msgdlg.h>

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

    // In results mode, prefill the fields with the last grid that was generated so the results
    // grid matches the printed plate without re-typing. (Editable in case you printed an older grid.)
    if (m_results_mode) {
        AppConfig* ac = gui_app->app_config.get();
        auto restore_combo = [](ComboBox* cmb, const std::string& v) {
            if (v.empty()) return;
            try { int n = std::stoi(v); if (n >= 1 && n <= 8) cmb->SetSelection(n - 1); } catch (...) {}
        };
        auto restore_text = [&ac](const std::string& key, wxTextCtrl* ctrl) {
            const std::string v = ac->get(key);
            if (!v.empty()) ctrl->SetValue(Slic3r::from_dot_to_local(v));
        };
        restore_combo(cmb_nb_speed, ac->get("pa_adaptive_cal_nb_speed"));
        restore_combo(cmb_nb_accel, ac->get("pa_adaptive_cal_nb_accel"));
        restore_text("pa_adaptive_cal_min_speed", txt_min_speed);
        restore_text("pa_adaptive_cal_max_speed", txt_max_speed);
        restore_text("pa_adaptive_cal_min_accel", txt_min_accel);
        restore_text("pa_adaptive_cal_max_accel", txt_max_accel);
        restore_text("pa_adaptive_cal_pa", txt_pa);
    }

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

    if (m_results_mode) {
        // Results mode: the fields above are prefilled from the printed grid; this opens the grid
        // that mirrors the plate so the user can identify each box and enter the best PA per cell.
        wxButton* bt = new wxButton(this, wxID_ANY, _L("Show results grid"), wxDefaultPosition, bt_size);
        bt->SetToolTip(_L("Open a grid that mirrors the printed plate. Each cell shows its speed, volumetric flow "
                          "and acceleration; type the PA that printed best for that box, then copy the model rows "
                          "or write them straight into the filament's adaptive PA model."));
        bt->Bind(wxEVT_BUTTON, &CalibrationPressureAdvAdaptiveDialog::show_results_grid, this);
        vertical->Add(bt);
    } else {
        wxButton* bt = new wxButton(this, wxID_FILE1, _L("Generate flow x acceleration grid"), wxDefaultPosition, bt_size);
        bt->Bind(wxEVT_BUTTON, &CalibrationPressureAdvAdaptiveDialog::create_geometry, this);
        vertical->Add(bt);
    }

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

    // Remember the grid we just generated (C-locale, dot decimals) so the results dialog can
    // rebuild the same layout later without the user re-typing it.
    AppConfig* ac = gui_app->app_config.get();
    ac->set("pa_adaptive_cal_nb_speed",  std::to_string(nb_speed));
    ac->set("pa_adaptive_cal_nb_accel",  std::to_string(nb_accel));
    ac->set("pa_adaptive_cal_min_speed", Slic3r::to_string_nozero(min_speed, 4));
    ac->set("pa_adaptive_cal_max_speed", Slic3r::to_string_nozero(max_speed, 4));
    ac->set("pa_adaptive_cal_min_accel", Slic3r::to_string_nozero(min_accel, 1));
    ac->set("pa_adaptive_cal_max_accel", Slic3r::to_string_nozero(max_accel, 1));
    ac->set("pa_adaptive_cal_pa",        Slic3r::to_string_nozero(test_pa, 4));
    // Flush to disk now: the results dialog is usually opened in a *later* session (after the
    // part has printed), and under the debugger the process is killed rather than exited
    // cleanly, so AppConfig's normal save-on-exit never runs and these keys would be lost.
    ac->save();

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

    // Tight grid layout (closer than arrange()) centred on the bed, so a single wide brim can
    // wrap the whole set.
    const double cube_xy = 10.0, cube_z = 4.0, gap = 4.0, pitch = cube_xy + gap;
    Vec2d bed_center(100.0, 100.0);
    if (const ConfigOptionPoints* bs = printer_config->option<ConfigOptionPoints>("bed_shape")) {
        if (!bs->get_values().empty())
            bed_center = BoundingBoxf(bs->get_values()).center();
    }
    const double grid_w = nb_speed * pitch;
    const double grid_h = nb_accel * pitch;

    model.clear_objects();
    std::vector<ModelObject*> objs;
    for (int ja = 0; ja < nb_accel; ja++) {
        for (int is = 0; is < nb_speed; is++) {
            const float speed = axis_value(is, nb_speed, min_speed, max_speed);
            const float accel = axis_value(ja, nb_accel, min_accel, max_accel);
            const std::string name = "s" + std::to_string(int(speed + 0.5f)) + "_a" + std::to_string(int(accel + 0.5f));
            ModelObject* obj = model.add_object(name.c_str(), "", Slic3r::make_cube(cube_xy, cube_xy, cube_z));
            obj->center_around_origin();
            obj->add_instance();
            const double x = bed_center.x() - grid_w / 2 + is * pitch + pitch / 2;
            const double y = bed_center.y() - grid_h / 2 + ja * pitch + pitch / 2;
            obj->instances[0]->set_offset(Vec3d(x, y, 0));
            obj->ensure_on_bed();
            objs.push_back(obj);
        }
    }

    /// --- main config (modify per-object where possible) ---
    DynamicPrintConfig new_print_config = *print_config;
    new_print_config.set_key_value("complete_objects", std::make_unique<ConfigOptionBool>(false));
    // Wide brim around the whole grid; with the tight spacing the per-object brims merge into
    // one mat that holds the thin single-wall boxes down.
    new_print_config.set_key_value("brim_width", std::make_unique<ConfigOptionFloat>(6));

    DynamicPrintConfig new_printer_config = *printer_config;

    DynamicPrintConfig new_filament_config = *filament_config;
    // Print the whole grid at one fixed PA; disable adaptive PA so the static value is what's emitted.
    new_filament_config.set_key_value("filament_adaptive_pressure_advance", std::make_unique<ConfigOptionBools>(std::initializer_list<bool>{ false }));
    auto test_pa_opt = std::make_unique<ConfigOptionFloats>(std::initializer_list<double>{ double(test_pa) });
    test_pa_opt->set_can_be_disabled(false);
    new_filament_config.set_key_value("filament_pressure_advance", std::move(test_pa_opt));
    // Force a single uniform PA across the whole grid: disable every per-role PA override so all
    // roles fall back to filament_pressure_advance (= test_pa). Otherwise a preset's per-role values
    // (e.g. external_perimeter_pa, overhangs_pa, first_layer_pa) leak through and the cubes print at
    // mixed PA, so cells differ by more than just speed/acceleration and aren't comparable.
    for (const char* pa_key : { "filament_perimeter_pa", "filament_external_perimeter_pa",
            "filament_overhangs_pa", "filament_first_layer_pa", "filament_first_layer_pa_over_raft",
            "filament_infill_pa", "filament_solid_infill_pa", "filament_top_solid_infill_pa",
            "filament_bridge_pa", "filament_bridge_internal_pa", "filament_gap_fill_pa",
            "filament_thin_walls_pa", "filament_ironing_pa", "filament_brim_pa", "filament_travel_pa",
            "filament_support_material_pa", "filament_support_material_interface_pa" }) {
        auto role_pa_opt = std::make_unique<ConfigOptionFloats>(std::initializer_list<double>{ 0. });
        role_pa_opt->set_can_be_disabled(true);
        new_filament_config.set_key_value(pa_key, std::move(role_pa_opt));
    }

    /// --- per-object config: one cube per (speed, acceleration) cell ---
    int idx = 0;
    for (int ja = 0; ja < nb_accel; ja++) {
        for (int is = 0; is < nb_speed; is++, idx++) {
            const float  speed = axis_value(is, nb_speed, min_speed, max_speed);
            const float  accel = axis_value(ja, nb_accel, min_accel, max_accel);
            ModelObject* o     = objs[idx];

            o->config.set_key_value("layer_height", std::make_unique<ConfigOptionFloat>(layer_height));
            o->config.set_key_value("first_layer_height", std::make_unique<ConfigOptionFloatOrPercent>(layer_height, false));
            o->config.set_key_value("ironing", std::make_unique<ConfigOptionBool>(false));
            // Hollow wall test: perimeters only, no top/bottom skins, no infill, so pressure
            // advance shows up at the corners instead of being masked by solid fill.
            o->config.set_key_value("perimeters", std::make_unique<ConfigOptionInt>(2));
            o->config.set_key_value("top_solid_layers", std::make_unique<ConfigOptionInt>(0));
            o->config.set_key_value("bottom_solid_layers", std::make_unique<ConfigOptionInt>(0));
            o->config.set_key_value("fill_density", std::make_unique<ConfigOptionPercent>(0));
            o->config.set_key_value("thin_walls", std::make_unique<ConfigOptionBool>(false));

            // speed -> volumetric flow (one axis of the grid)
            o->config.set_key_value("perimeter_speed", std::make_unique<ConfigOptionFloatOrPercent>(speed, false));
            o->config.set_key_value("external_perimeter_speed", std::make_unique<ConfigOptionFloatOrPercent>(speed, false));
            o->config.set_key_value("solid_infill_speed", std::make_unique<ConfigOptionFloatOrPercent>(speed, false));
            o->config.set_key_value("top_solid_infill_speed", std::make_unique<ConfigOptionFloatOrPercent>(speed, false));
            o->config.set_key_value("infill_speed", std::make_unique<ConfigOptionFloatOrPercent>(speed, false));

            // acceleration (the other axis)
            o->config.set_key_value("default_acceleration", std::make_unique<ConfigOptionFloatOrPercent>(accel, false));
            o->config.set_key_value("perimeter_acceleration", std::make_unique<ConfigOptionFloatOrPercent>(accel, false));
            o->config.set_key_value("external_perimeter_acceleration", std::make_unique<ConfigOptionFloatOrPercent>(accel, false));
            o->config.set_key_value("solid_infill_acceleration", std::make_unique<ConfigOptionFloatOrPercent>(accel, false));
            o->config.set_key_value("top_solid_infill_acceleration", std::make_unique<ConfigOptionFloatOrPercent>(accel, false));
            o->config.set_key_value("infill_acceleration", std::make_unique<ConfigOptionFloatOrPercent>(accel, false));
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

    // Objects are already placed in a tight grid; apply config and slice (no arrange, which
    // would scatter the grid).
    if (plat->printer_technology() == ptFFF)
        plat->fff_print().apply(plat->model(), *plat->config());
    plat->reslice();

    if (autocenter)
        gui_app->app_config->set("autocenter", "1");

    close_dialog();
}

void CalibrationPressureAdvAdaptiveDialog::show_results_grid(wxCommandEvent& /*event_args*/)
{
    // --- read the same grid parameters the generator uses (live from the form) ---
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

    // Volumetric flow per cell = speed * mm3_per_mm of the external perimeter (the wall whose
    // corners reveal pressure advance). Width and layer height are constant across the grid, so
    // only speed varies the flow. This mirrors what the engine feeds the adaptive model.
    float nz           = printer_config->option("nozzle_diameter")->get_float(0);
    float layer_height = print_config->option("layer_height")->get_float();
    layer_height = std::max(layer_height, float(print_config->get_abs_value("first_layer_height", nz)));
    float max_overlap  = filament_config->option("filament_max_overlap")->get_float();
    Flow  perim_flow   = Flow::new_from_config(FlowRole::frExternalPerimeter, *print_config, nz, layer_height,
                                               max_overlap / 100.f, false);
    const double mm3_per_mm = perim_flow.mm3_per_mm();

    auto axis_value = [](int idx, int count, float lo, float hi) -> float {
        return (count > 1) ? (lo + idx * (hi - lo) / (count - 1)) : lo;
    };

    // --- build the dialog ---
    wxDialog dlg(this, wxID_ANY, _L("Adaptive PA - identify boxes & enter results"),
                 wxDefaultPosition, wxSize(720, 520), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    wxBoxSizer* root = new wxBoxSizer(wxVERTICAL);

    wxStaticText* help = new wxStaticText(&dlg, wxID_ANY,
        _L("Each cell is one box on your plate, arranged the same way: start at the top-left box, work your way "
           "right, then drop down to the next row and repeat. Moving right means faster speed (higher flow); "
           "moving down means lower acceleration. For each box, type the pressure-advance value that printed best "
           "into its cell, then copy the rows or write them into the filament's adaptive PA model."));
    help->Wrap(680);
    root->Add(help, 0, wxALL, 8);

    wxScrolledWindow* scroll = new wxScrolledWindow(&dlg, wxID_ANY);
    scroll->SetScrollRate(10, 10);
    wxFlexGridSizer* grid = new wxFlexGridSizer(nb_accel + 1, nb_speed + 1, 4, 4);

    grid->Add(new wxStaticText(scroll, wxID_ANY, _L("accel \\ flow")), 0, wxALIGN_CENTER);
    for (int is = 0; is < nb_speed; is++) {
        const float  speed = axis_value(is, nb_speed, min_speed, max_speed);
        const double cflow = speed * mm3_per_mm;
        grid->Add(new wxStaticText(scroll, wxID_ANY, wxString::Format("%.0f mm/s\n%.2f mm3/s", speed, cflow),
                                   wxDefaultPosition, wxDefaultSize, wxALIGN_CENTRE_HORIZONTAL), 0, wxALIGN_CENTER);
    }

    // top row = highest acceleration (back of bed), bottom row = lowest (front of bed).
    struct Cell { wxTextCtrl* tc; double flow; double accel; };
    std::vector<Cell> cells;
    cells.reserve(size_t(nb_speed) * size_t(nb_accel));
    const wxString pa_prefill = Slic3r::from_dot_to_local(Slic3r::to_string_nozero(test_pa, 4));
    for (int dr = 0; dr < nb_accel; dr++) {
        const int   ja    = nb_accel - 1 - dr;
        const float accel = axis_value(ja, nb_accel, min_accel, max_accel);
        grid->Add(new wxStaticText(scroll, wxID_ANY, wxString::Format("%.0f mm/s2", accel)), 0,
                  wxALIGN_CENTER_VERTICAL | wxALIGN_RIGHT);
        for (int is = 0; is < nb_speed; is++) {
            const float  speed = axis_value(is, nb_speed, min_speed, max_speed);
            const double cflow = speed * mm3_per_mm;
            wxTextCtrl*  tc    = new wxTextCtrl(scroll, wxID_ANY, pa_prefill, wxDefaultPosition,
                                                wxSize(7 * em_unit(), -1));
            tc->SetToolTip(wxString::Format("box \"s%d_a%d\": speed %.0f mm/s, flow %.3f mm3/s, accel %.0f mm/s2",
                                            int(speed + 0.5f), int(accel + 0.5f), speed, cflow, accel));
            grid->Add(tc, 0, wxALIGN_CENTER);
            cells.push_back(Cell{ tc, cflow, double(accel) });
        }
    }
    scroll->SetSizer(grid);
    root->Add(scroll, 1, wxEXPAND | wxLEFT | wxRIGHT, 8);

    // Assemble "PA, flow, accel" rows (dot decimals, C locale) for every cell with a valid PA.
    auto build_rows = [&cells]() -> std::string {
        std::vector<std::array<double, 3>> pts; // pa, flow, accel
        for (const Cell& c : cells) {
            const double pa = parse_float_all_locale(c.tc->GetValue().ToStdString());
            if (pa >= 0.)
                pts.push_back({ pa, c.flow, c.accel });
        }
        std::sort(pts.begin(), pts.end(), [](const std::array<double, 3>& a, const std::array<double, 3>& b) {
            return a[2] != b[2] ? a[2] < b[2] : a[1] < b[1];
        });
        std::string out;
        for (const auto& p : pts)
            out += Slic3r::to_string_nozero(p[0], 4) + ", " + Slic3r::to_string_nozero(p[1], 4) + ", " +
                   Slic3r::to_string_nozero(p[2], 1) + "\n";
        return out;
    };

    // --- buttons ---
    wxBoxSizer* btns   = new wxBoxSizer(wxHORIZONTAL);
    wxButton*   bt_copy  = new wxButton(&dlg, wxID_ANY, _L("Copy model rows"));
    wxButton*   bt_apply = new wxButton(&dlg, wxID_ANY, _L("Write to filament model"));
    wxButton*   bt_close = new wxButton(&dlg, wxID_OK, _L("Close"));
    btns->Add(bt_copy, 0, wxRIGHT, 6);
    btns->Add(bt_apply, 0, wxRIGHT, 6);
    btns->AddStretchSpacer();
    btns->Add(bt_close);
    root->Add(btns, 0, wxEXPAND | wxALL, 8);

    bt_copy->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) {
        const std::string rows = build_rows();
        if (rows.empty()) {
            wxMessageBox(_L("No cells have a pressure-advance value."), _L("Adaptive PA"), wxOK | wxICON_INFORMATION, &dlg);
            return;
        }
        if (wxTheClipboard->Open()) {
            wxTheClipboard->SetData(new wxTextDataObject(wxString::FromUTF8(rows.c_str())));
            wxTheClipboard->Close();
            wxMessageBox(_L("Model rows copied to the clipboard."), _L("Adaptive PA"), wxOK | wxICON_INFORMATION, &dlg);
        }
    });

    bt_apply->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) {
        const std::string rows = build_rows();
        if (rows.empty()) {
            wxMessageBox(_L("No cells have a pressure-advance value."), _L("Adaptive PA"), wxOK | wxICON_INFORMATION, &dlg);
            return;
        }
        Tab* ftab = this->gui_app->get_tab(Preset::TYPE_FFF_FILAMENT);
        DynamicPrintConfig new_filament_config = *ftab->get_config();
        new_filament_config.set_key_value("filament_adaptive_pressure_advance", std::make_unique<ConfigOptionBools>(std::initializer_list<bool>{ true }));
        new_filament_config.set_key_value("filament_adaptive_pressure_advance_model", std::make_unique<ConfigOptionStrings>(std::initializer_list<std::string>{ rows }));
        ftab->load_config(new_filament_config);
        ftab->update_dirty();
        wxMessageBox(_L("Adaptive PA enabled and the model written to the current filament preset. "
                        "Review it in the Filament tab and save the preset to keep it."),
                     _L("Adaptive PA"), wxOK | wxICON_INFORMATION, &dlg);
    });

    dlg.SetSizer(root);
    dlg.Layout();
    dlg.ShowModal();
}

} // namespace GUI
} // namespace Slic3r
