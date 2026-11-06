#include "CalibrationRerfDialog.hpp"

#include "Automation/AutomationFileDialog.hpp"
#include "Automation/AutomationIds.hpp"
#include "GUI.hpp"
#include "GUI_ObjectList.hpp"
#include "I18N.hpp"
#include "Plater.hpp"
#include "Tab.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/SLA/Rerf.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <algorithm>
#include <exception>

#include <wx/choice.h>
#include <wx/stattext.h>
#include <wx/wupdlock.h>

namespace Slic3r {
namespace GUI {

namespace {
constexpr double kTileMaxMm = 12.;
constexpr double kTileHeightMm = 3.;
constexpr double kTileZoneFraction = 0.45;
constexpr double kZoneEdgeGapMm = 1.;

double fit_in_zone(const TriangleMesh& mesh, const Vec2d& zone_size)
{
    const Vec3d size = mesh.bounding_box().size();
    const double room_x = std::max(0.5, zone_size.x() - 2. * kZoneEdgeGapMm);
    const double room_y = std::max(0.5, zone_size.y() - 2. * kZoneEdgeGapMm);
    if (size.x() <= 1e-6 || size.y() <= 1e-6)
        return 1.;
    if (size.x() <= room_x && size.y() <= room_y)
        return 1.;
    return std::min(room_x / size.x(), room_y / size.y());
}
} // namespace

CalibrationRerfDialog::CalibrationRerfDialog(GUI_App* app, MainFrame* mainframe, RerfTarget target)
    : CalibrationAbstractDialog(app, mainframe, target == RerfTarget::FirstLayer ? "First layer exposure" : "Normal exposure")
    , m_target(target)
{
    const char* page = target == RerfTarget::FirstLayer ? "rerf_first_layer.html" : "rerf.html";
    create(boost::filesystem::path("calibration") / "rerf", page,
        wxSize(CalibrationConstants::kWideDialogWidthPx, CalibrationConstants::kCompactDialogHeightPx));
}

bool CalibrationRerfDialog::photon_mono4_selected() const
{
    const DynamicPrintConfig* printer = this->gui_app->get_tab(Preset::TYPE_PRINTER)->get_config();
    const auto* format = printer == nullptr ? nullptr : printer->option<ConfigOptionEnum<OutputFormat>>("output_format");
    return format != nullptr && format->value == OutputFormat::ofAnycubicMono4;
}

const char* CalibrationRerfDialog::exposure_key() const
{
    return m_target == RerfTarget::FirstLayer ? "initial_exposure_time" : "exposure_time";
}

double CalibrationRerfDialog::current_exposure_s() const
{
    const DynamicPrintConfig* material = this->gui_app->get_tab(Preset::TYPE_SLA_MATERIAL)->get_config();
    const auto* exposure = material == nullptr ? nullptr : material->option<ConfigOptionFloat>(exposure_key());
    return exposure == nullptr ? 0. : exposure->value;
}

void CalibrationRerfDialog::create_buttons(wxStdDialogButtonSizer* buttons)
{
    wxBoxSizer* vertical = new wxBoxSizer(wxVERTICAL);

    const wxString fixed_text = m_target == RerfTarget::FirstLayer
        ? _L("The printer only varies exposure for a file named R_E_R_F.pm4n. The step is fixed at 0.25 s. Area 1 is this resin's bottom exposure.")
        : _L("The printer only varies exposure for a file named R_E_R_F.pm4n. The step is fixed at 0.25 s. Area 1 is this resin's normal exposure.");
    wxStaticText* fixed = new wxStaticText(this, wxID_ANY, fixed_text);
    fixed->SetName(wxString::FromUTF8(AutomationIds::calibration("rerf.fixed").c_str()));
    fixed->Wrap(520);
    vertical->Add(fixed, 0, wxEXPAND | wxBOTTOM, 8);

    wxButton* generate = new wxButton(this, wxID_FILE1, _L("Generate tiles"));
    generate->SetName(wxString::FromUTF8(AutomationIds::calibration("rerf.generate").c_str()));
    generate->SetToolTip(_L("Place one tile in each area and export the plate as R_E_R_F.pm4n. The 0.25 s steps are fixed by the printer."));
    generate->Bind(wxEVT_BUTTON, &CalibrationRerfDialog::create_tiles, this);
    vertical->Add(generate, 0, wxBOTTOM, 8);

    wxButton* mine = new wxButton(this, wxID_FILE2, _L("Use my model"));
    mine->SetName(wxString::FromUTF8(AutomationIds::calibration("rerf.model").c_str()));
    mine->SetToolTip(_L("Place a copy of your model in each area. A model larger than an area is scaled down so it stays inside that exposure."));
    mine->Bind(wxEVT_BUTTON, &CalibrationRerfDialog::use_model, this);
    vertical->Add(mine, 0, wxBOTTOM, CalibrationConstants::kButtonRowSpacerPx);

    wxBoxSizer* apply_row = new wxBoxSizer(wxHORIZONTAL);
    apply_row->Add(new wxStaticText(this, wxID_ANY, _L("Best area:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);

    const double base_s = current_exposure_s();
    wxArrayString zones;
    for (int zone = 1; zone <= sla::RERF_ZONE_COUNT; ++zone) {
        const std::string seconds = sla::format_rerf_seconds(base_s + (zone - 1) * sla::RERF_EXPOSURE_STEP_S);
        zones.Add(wxString::Format("%d — %s s", zone, wxString::FromUTF8(seconds)));
    }
    m_zone = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, zones);
    m_zone->SetSelection(0);
    m_zone->SetName(wxString::FromUTF8(AutomationIds::calibration("rerf.zone").c_str()));
    apply_row->Add(m_zone, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);

    wxButton* apply = new wxButton(this, wxID_APPLY, _L("Apply exposure"));
    apply->SetName(wxString::FromUTF8(AutomationIds::calibration("rerf.apply").c_str()));
    apply->SetToolTip(m_target == RerfTarget::FirstLayer
        ? _L("Save the chosen area's time as this resin's bottom exposure.")
        : _L("Save the chosen area's time as this resin's normal exposure."));
    apply->Bind(wxEVT_BUTTON, &CalibrationRerfDialog::apply_exposure, this);
    apply_row->Add(apply, 0, wxALIGN_CENTER_VERTICAL);
    vertical->Add(apply_row, 0, wxBOTTOM, 8);

    m_result = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_result->SetName(wxString::FromUTF8(AutomationIds::calibration("rerf.result").c_str()));
    vertical->Add(m_result, 0, wxEXPAND);

    buttons->Add(vertical);
}

void CalibrationRerfDialog::create_tiles(wxCommandEvent& /*event_args*/)
{
    place_models(nullptr, {});
}

void CalibrationRerfDialog::use_model(wxCommandEvent& /*event_args*/)
{
    if (!photon_mono4_selected()) {
        show_error(this, _L("This test is for the Anycubic Photon Mono 4."));
        return;
    }

    FileDialog dialog(this, _L("Choose the test model"), "", "", file_wildcards(FT_MODEL),
        wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dialog.ShowModal() != wxID_OK)
        return;

    TriangleMesh mesh;
    const std::string path = into_u8(dialog.GetPath());
    try {
        const Model loaded = Model::read_from_file(path);
        for (const ModelObject* object : loaded.objects) {
            TriangleMesh part = object->mesh();
            if (mesh.its.indices.empty())
                mesh = std::move(part);
            else
                mesh.merge(part);
        }
    } catch (const std::exception& ex) {
        show_error(this, wxString::FromUTF8(ex.what()));
        return;
    }
    if (mesh.its.indices.empty()) {
        show_error(this, _L("That file has no model to place."));
        return;
    }
    place_models(&mesh, path);
}

void CalibrationRerfDialog::place_models(const TriangleMesh* model_mesh, const std::string& source_path)
{
    if (!photon_mono4_selected()) {
        show_error(this, _L("This test is for the Anycubic Photon Mono 4."));
        return;
    }

    Plater* plat = this->main_frame->plater();
    const bool started = m_target == RerfTarget::FirstLayer
        ? plat->new_project(L("First layer exposure"))
        : plat->new_project(L("Normal exposure"));
    if (!started)
        return;
    wxGetApp().Yield();

    const DynamicPrintConfig* printer = this->gui_app->get_tab(Preset::TYPE_PRINTER)->get_config();
    const ConfigOptionPoints* bed_shape = printer->option<ConfigOptionPoints>("bed_shape");
    if (bed_shape == nullptr || bed_shape->get_values().empty()) {
        show_error(this, _L("The printer has no bed shape, so the exposure areas cannot be placed."));
        return;
    }

    bool autocenter = gui_app->app_config->get("autocenter") == "1";
    if (autocenter)
        gui_app->app_config->set("autocenter", "0");

    std::unique_ptr<wxWindowUpdateLocker> freeze_gui = std::make_unique<wxWindowUpdateLocker>(this);
    Model& model = plat->model();
    model.clear_objects();

    const BoundingBoxf bed(bed_shape->get_values());
    const auto zones = sla::rerf_zones(bed);
    const double model_scale = (model_mesh != nullptr && !zones.empty() && zones.front().box.defined)
        ? fit_in_zone(*model_mesh, zones.front().box.size()) : 1.;
    const char* name_prefix = m_target == RerfTarget::FirstLayer ? "First layer " : "Normal ";
    for (const sla::RerfZone& zone : zones) {
        if (!zone.box.defined)
            continue;
        const Vec2d zone_size = zone.box.size();
        const double seconds = current_exposure_s() + (zone.index - 1) * sla::RERF_EXPOSURE_STEP_S;
        const std::string name = std::string(name_prefix) + std::to_string(zone.index) + " " + sla::format_rerf_seconds(seconds) + " s";
        ModelObject* object = nullptr;
        if (model_mesh != nullptr) {
            object = model.add_object(name.c_str(), source_path.c_str(), *model_mesh);
            object->center_around_origin();
            if (model_scale != 1.)
                object->scale(model_scale);
        } else {
            const double tile = std::min(kTileMaxMm, std::min(zone_size.x(), zone_size.y()) * kTileZoneFraction);
            if (tile <= 0.)
                continue;
            object = model.add_object(name.c_str(), "", make_cube(tile, tile, kTileHeightMm));
            object->center_around_origin();
        }
        object->add_instance();
        const Vec2d center = zone.box.center();
        object->instances.front()->set_offset(Vec3d(center.x(), center.y(), 0.));
        object->ensure_on_bed();
    }

    if (model.objects.empty()) {
        freeze_gui.reset();
        show_error(this, _L("The exposure areas could not be placed on this bed."));
        if (autocenter)
            gui_app->app_config->set("autocenter", "1");
        return;
    }

    DynamicPrintConfig new_print = *this->gui_app->get_tab(Preset::TYPE_SLA_PRINT)->get_config();
    new_print.set_key_value("rerf_calibration", std::make_unique<ConfigOptionBool>(true));
    new_print.set_key_value("supports_enable", std::make_unique<ConfigOptionBool>(false));
    new_print.set_key_value("pad_enable", std::make_unique<ConfigOptionBool>(false));
    new_print.set_key_value("support_object_elevation", std::make_unique<ConfigOptionFloat>(0.));
    new_print.set_key_value("hollowing_enable", std::make_unique<ConfigOptionBool>(false));

    this->gui_app->get_tab(Preset::TYPE_SLA_PRINT)->load_config(new_print);
    plat->on_config_change(new_print);
    this->gui_app->get_tab(Preset::TYPE_SLA_PRINT)->update_dirty();
    this->gui_app->obj_list()->update_after_undo_redo();
    freeze_gui.reset();

    plat->reslice();
    if (autocenter)
        gui_app->app_config->set("autocenter", "1");
    close_dialog();
}

void CalibrationRerfDialog::apply_exposure(wxCommandEvent& /*event_args*/)
{
    if (!photon_mono4_selected()) {
        show_error(this, _L("This test is for the Anycubic Photon Mono 4."));
        return;
    }
    if (m_zone == nullptr || m_zone->GetSelection() == wxNOT_FOUND)
        return;

    const int zone = m_zone->GetSelection() + 1;
    const double exposure_s = current_exposure_s() + (zone - 1) * sla::RERF_EXPOSURE_STEP_S;

    Tab* material_tab = this->gui_app->get_tab(Preset::TYPE_SLA_MATERIAL);
    DynamicPrintConfig material = *material_tab->get_config();
    material.set_key_value(exposure_key(), std::make_unique<ConfigOptionFloat>(exposure_s));
    material_tab->load_config(material);
    this->main_frame->plater()->on_config_change(material);
    material_tab->update_dirty();

    Tab* print_tab = this->gui_app->get_tab(Preset::TYPE_SLA_PRINT);
    DynamicPrintConfig print = *print_tab->get_config();
    print.set_key_value("rerf_calibration", std::make_unique<ConfigOptionBool>(false));
    print_tab->load_config(print);
    this->main_frame->plater()->on_config_change(print);
    print_tab->update_dirty();

    const std::string seconds = sla::format_rerf_seconds(exposure_s);
    if (m_result != nullptr) {
        const wxString which = m_target == RerfTarget::FirstLayer
            ? _L("Bottom exposure time set to ")
            : _L("Normal exposure set to ");
        m_result->SetLabel(
            which + wxString::FromUTF8(seconds) +
            _L(" s. Save the material preset to keep it. The next export uses a normal file name."));
        this->Layout();
        this->Fit();
    }
}

} // namespace GUI
} // namespace Slic3r
