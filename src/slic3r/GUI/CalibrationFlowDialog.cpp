#include "CalibrationFlowDialog.hpp"
#include "I18N.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/LocalesUtils.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"
#include "Automation/AutomationIds.hpp"
#include "GLCanvas3D.hpp"
#include "GUI.hpp"
#include "GUI_ObjectList.hpp"
#include "Plater.hpp"
#include "Tab.hpp"
#include <wx/button.h>
#include <wx/log.h>
#include <wx/wupdlock.h>

#include <algorithm>
#include <boost/filesystem.hpp>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace Slic3r {
namespace GUI {

namespace {
constexpr double kRecommendedFirstModifier = -0.05;
constexpr double kRecommendedStep          = 0.01;
constexpr size_t kRecommendedChipCount     = 11;
constexpr double kFineFirstModifier        = -0.04;
constexpr double kFineStep                 = 0.005;
constexpr size_t kFineChipCount            = 16;

constexpr double kOrcaModelHeightMm        = 2.0;
constexpr int    kOrcaLayersAboveFirst     = 9;
constexpr double kInfillDensityPercent     = 35.0;
constexpr int    kBottomSolidLayers        = 2;
constexpr int    kTopSolidLayers           = 5;
constexpr int    kPerimeterCount           = 1;
constexpr double kTopWidthNozzleFactor     = 1.2;
constexpr int    kButtonRowSpacerPx        = 20;
constexpr int    kFieldGapPx               = 5;
constexpr int    kSectionGapPx             = 8;
constexpr int    kTileGridCols             = 6;
constexpr int    kTileWidthEm              = 7;
constexpr int    kTileHeightEm             = 6;
constexpr int    kApplyButtonWidthEm       = 16;
constexpr char   kAutocenterKey[]          = "autocenter";
constexpr double kMinFilamentMultiplier    = 0.01;
constexpr double kMaxFilamentMultiplier    = 2.0;
constexpr int    kChipNameDecimals         = 3;
constexpr double kZeroModifierEpsilon      = 0.0005;
constexpr int    kZeroModifierIndexHint    = 5;
constexpr char   kOrcaLinearFlowFile[]     = "Orca-LinearFlow.3mf";
constexpr char   kOrcaLinearFlowFineFile[] = "Orca-LinearFlow_fine.3mf";
constexpr char   kOrcaFlowratePrefix[]     = "flowrate_";
constexpr size_t kOrcaFlowratePrefixLen    = 9;
constexpr size_t kLeadingZeroDotPrefixLen  = 2; // "0." in "0.05"
constexpr double kZScaleEpsilon            = 0.001;
constexpr char   kFlowCalFirstKey[]        = "flow_cal_first";
constexpr char   kFlowCalStepKey[]         = "flow_cal_step";
constexpr char   kFlowCalCountKey[]        = "flow_cal_count";
constexpr char   kFlowCalBaseEmKey[]       = "flow_cal_base_em";
constexpr char   kFlowCalFilamentKey[]     = "flow_cal_filament";
constexpr char   kFlowCalPrinterKey[]      = "flow_cal_printer";

std::string selected_filament_name(const PresetBundle& bundle)
{
    return bundle.filaments.get_selected_preset_name();
}

std::string selected_printer_name(const PresetBundle& bundle)
{
    if (bundle.physical_printers.has_selection()) {
        const std::string full = bundle.physical_printers.get_selected_full_printer_name();
        if (!full.empty())
            return full;
    }
    return bundle.printers.get_selected_preset_name();
}

wxString printed_with_text(const std::string& filament, const std::string& printer)
{
    if (filament.empty() && printer.empty())
        return _L("After printing, click the tile that matches the number on the chip tab.");
    wxString text = _L("Printed with: ");
    if (!filament.empty())
        text += wxString::FromUTF8(filament.c_str());
    if (!printer.empty()) {
        if (!filament.empty())
            text += _L(" on ");
        text += wxString::FromUTF8(printer.c_str());
    }
    text += _L(". Click the matching tab number.");
    return text;
}

std::string chip_name(int number, double modifier)
{
    std::ostringstream name;
    name << number << "  " << std::showpos << std::fixed << std::setprecision(kChipNameDecimals) << modifier;
    return name.str();
}

wxString chip_modifier_text(double modifier)
{
    if (std::abs(modifier) < kZeroModifierEpsilon)
        return "0";
    std::string body = Slic3r::to_string_nozero(std::abs(modifier), kChipNameDecimals);
    if (body.size() >= kLeadingZeroDotPrefixLen && body[0] == '0' && body[1] == '.')
        body.erase(body.begin());
    return wxString::FromUTF8(((modifier < 0.0 ? "-" : "") + body).c_str());
}

wxString chip_button_label(double modifier)
{
    return chip_modifier_text(modifier);
}

bool modifier_from_orca_object_name(const std::string& name, double& modifier)
{
    if (name.size() <= kOrcaFlowratePrefixLen ||
        name.compare(0, kOrcaFlowratePrefixLen, kOrcaFlowratePrefix) != 0)
        return false;
    std::string body = name.substr(kOrcaFlowratePrefixLen);
    if (body.empty())
        return false;
    if (body.front() == 'm')
        body.front() = '-';
    try {
        modifier = parse_float_all_locale(body);
        return std::isfinite(modifier);
    } catch (...) {
        return false;
    }
}

void apply_orca_flow_object_settings(ModelObject* object, double filament_em, double modifier,
    double nozzle)
{
    const double target_em     = filament_em + modifier;
    const double print_percent = 100.0 * target_em / filament_em;
    object->config.set_key_value("print_extrusion_multiplier",
        std::make_unique<ConfigOptionPercent>(print_percent));
    object->config.set_key_value("fill_density",
        std::make_unique<ConfigOptionPercent>(kInfillDensityPercent));
    object->config.set_key_value("fill_pattern",
        std::make_unique<ConfigOptionEnum<InfillPattern>>(ipRectilinear));
    object->config.set_key_value("top_fill_pattern",
        std::make_unique<ConfigOptionEnum<InfillPattern>>(ipArchimedeanChords));
    object->config.set_key_value("bottom_solid_layers",
        std::make_unique<ConfigOptionInt>(kBottomSolidLayers));
    object->config.set_key_value("bottom_solid_min_thickness",
        std::make_unique<ConfigOptionFloat>(0.0));
    object->config.set_key_value("top_solid_layers",
        std::make_unique<ConfigOptionInt>(kTopSolidLayers));
    object->config.set_key_value("top_solid_min_thickness",
        std::make_unique<ConfigOptionFloat>(0.0));
    object->config.set_key_value("solid_infill_every_layers",
        std::make_unique<ConfigOptionInt>(0));
    object->config.set_key_value("enforce_full_fill_volume",
        std::make_unique<ConfigOptionBool>(false));
    object->config.set_key_value("perimeters",
        std::make_unique<ConfigOptionInt>(kPerimeterCount));
    object->config.set_key_value("only_one_perimeter_top",
        std::make_unique<ConfigOptionBool>(true));
    object->config.set_key_value("ironing",
        std::make_unique<ConfigOptionBool>(false));
    object->config.set_key_value("top_infill_extrusion_width",
        std::make_unique<ConfigOptionFloatOrPercent>(nozzle * kTopWidthNozzleFactor, false));
    object->config.set_key_value("brim_ears",
        std::make_unique<ConfigOptionBool>(false));
    object->config.set_key_value("brim_width",
        std::make_unique<ConfigOptionFloat>(0.0));
}

double positive_or(double value, double fallback)
{
    return value > 0.0 ? value : fallback;
}

double abs_float_or_percent(const DynamicPrintConfig& print_config, const char* key,
    double ratio_over)
{
    const auto* opt = print_config.option<ConfigOptionFloatOrPercent>(key);
    if (opt == nullptr)
        return 0.0;
    return opt->get_abs_value(ratio_over);
}

// Official LinearFlow.3mf is 2 mm tall and drawn for a 0.4 mm nozzle. Scale is
// computed from the live print profile; layer heights are not rewritten.
struct FlowPlateScale {
    double xy = 1.0;
    double z  = 1.0;
};

FlowPlateScale flow_plate_scale_from_config(double nozzle, const DynamicPrintConfig& print_config)
{
    double layer_height = 0.0;
    if (const ConfigOption* opt = print_config.option("layer_height"))
        layer_height = opt->get_float();
    layer_height = positive_or(layer_height, nozzle / 2.0);

    double first_layer_height = abs_float_or_percent(print_config, "first_layer_height", nozzle);
    first_layer_height = positive_or(first_layer_height, layer_height);

    double first_layer_width = abs_float_or_percent(print_config,
        "first_layer_extrusion_width", nozzle);
    if (first_layer_width <= 0.0)
        first_layer_width = abs_float_or_percent(print_config, "extrusion_width", nozzle);
    first_layer_width = positive_or(first_layer_width, nozzle);

    FlowPlateScale scale;
    scale.xy = std::max(nozzle, first_layer_width) / CalibrationConstants::kDesignNozzleDiameterMm;
    scale.z  = (first_layer_height + kOrcaLayersAboveFirst * layer_height) / kOrcaModelHeightMm;
    return scale;
}

// SuperSlicer stores each chip as a local mesh plus an instance offset, so
// volume scale grows chips in place and they overlap. Scale instances around
// the plate center instead.
void scale_orca_linear_flow_plate(Model& model, const std::vector<size_t>& loaded,
    double xy_scale, double z_scale, const Vec2d& bed_center)
{
    if (std::abs(xy_scale - 1.0) <= kZScaleEpsilon &&
        std::abs(z_scale - 1.0) <= kZScaleEpsilon)
        return;

    BoundingBoxf3 plate;
    for (size_t idx : loaded)
        plate.merge(model.objects[idx]->bounding_box_exact());
    const Vec3d center = plate.center();

    for (size_t idx : loaded) {
        ModelObject* object = model.objects[idx];
        for (ModelInstance* instance : object->instances) {
            const Vec3d offset = instance->get_offset();
            const Vec3d scaling = instance->get_scaling_factor();
            instance->set_scaling_factor(Vec3d(
                scaling.x() * xy_scale, scaling.y() * xy_scale, scaling.z() * z_scale));
            instance->set_offset(Vec3d(
                center.x() + xy_scale * (offset.x() - center.x()),
                center.y() + xy_scale * (offset.y() - center.y()),
                offset.z()));
        }
        object->invalidate_bounding_box();
    }
    static_cast<void>(model.center_instances_around_point(bed_center));
}

int index_of_zero_modifier(double first_modifier, double step, size_t count)
{
    if (step == 0.0 || count == 0)
        return 0;
    const double raw = -first_modifier / step;
    int index = static_cast<int>(std::lround(raw));
    if (index < 0)
        index = 0;
    if (index >= static_cast<int>(count))
        index = static_cast<int>(count) - 1;
    return index;
}
} // namespace

void CalibrationFlowDialog::recalled_flow_parameters()
{
    m_first_modifier = kRecommendedFirstModifier;
    m_step           = kRecommendedStep;
    m_count          = kRecommendedChipCount;
    m_base_em        = 1.0;
    m_selected       = kZeroModifierIndexHint;

    const std::string first = gui_app->app_config->get(kFlowCalFirstKey);
    const std::string step  = gui_app->app_config->get(kFlowCalStepKey);
    const std::string count = gui_app->app_config->get(kFlowCalCountKey);
    const std::string base  = gui_app->app_config->get(kFlowCalBaseEmKey);
    try {
        if (!first.empty())
            m_first_modifier = parse_float_all_locale(first);
        if (!step.empty())
            m_step = parse_float_all_locale(step);
        if (!count.empty()) {
            const int parsed = std::stoi(count);
            if (parsed > 0)
                m_count = static_cast<size_t>(parsed);
        }
        if (!base.empty())
            m_base_em = parse_float_all_locale(base);
    } catch (...) {
        m_first_modifier = kRecommendedFirstModifier;
        m_step           = kRecommendedStep;
        m_count          = kRecommendedChipCount;
        m_base_em        = 1.0;
    }
    const double live = live_filament_multiplier();
    if (live >= kMinFilamentMultiplier)
        m_base_em = live;
    if (m_base_em < kMinFilamentMultiplier)
        m_base_em = 1.0;
    if (m_step == 0.0)
        m_step = kRecommendedStep;
    if (m_count == 0)
        m_count = kRecommendedChipCount;
    m_selected = index_of_zero_modifier(m_first_modifier, m_step, m_count);
}

void CalibrationFlowDialog::save_flow_parameters(
    double first_modifier, double step, size_t count, double base_em)
{
    gui_app->app_config->set(kFlowCalFirstKey, Slic3r::to_string_nozero(first_modifier, kChipNameDecimals));
    gui_app->app_config->set(kFlowCalStepKey, Slic3r::to_string_nozero(step, kChipNameDecimals));
    gui_app->app_config->set(kFlowCalCountKey, std::to_string(count));
    gui_app->app_config->set(kFlowCalBaseEmKey, Slic3r::to_string_nozero(base_em, kChipNameDecimals));
    gui_app->app_config->set(kFlowCalFilamentKey, selected_filament_name(*gui_app->preset_bundle));
    gui_app->app_config->set(kFlowCalPrinterKey, selected_printer_name(*gui_app->preset_bundle));
}

double CalibrationFlowDialog::selected_modifier() const
{
    if (m_selected < 0 || static_cast<size_t>(m_selected) >= m_count)
        return 0.0;
    return m_first_modifier + static_cast<double>(m_selected) * m_step;
}

double CalibrationFlowDialog::computed_multiplier() const
{
    const double current = live_filament_multiplier();
    if (current >= kMinFilamentMultiplier)
        return current + selected_modifier();
    return m_base_em + selected_modifier();
}

double CalibrationFlowDialog::live_filament_multiplier() const
{
    Tab* filament_tab = this->gui_app->get_tab(Preset::TYPE_FFF_FILAMENT);
    if (filament_tab == nullptr)
        return 0.0;
    const DynamicPrintConfig* filament_config = filament_tab->get_config();
    if (filament_config == nullptr)
        return 0.0;
    return filament_config->option("extrusion_multiplier")->get_float(0);
}

void CalibrationFlowDialog::update_printed_with_label()
{
    if (m_printed_with == nullptr)
        return;
    m_printed_with->SetLabel(printed_with_text(
        selected_filament_name(*gui_app->preset_bundle),
        selected_printer_name(*gui_app->preset_bundle)));
    Layout();
}

void CalibrationFlowDialog::update_result_label()
{
    if (m_result_label == nullptr)
        return;
    double current = live_filament_multiplier();
    if (current < kMinFilamentMultiplier)
        current = m_base_em;
    const double next = current + selected_modifier();
    const wxString text =
        _L("Current ") +
        wxString::FromUTF8(Slic3r::to_string_nozero(current, kChipNameDecimals).c_str()) +
        _L("  +  ") + chip_modifier_text(selected_modifier()) +
        _L("  =  ") +
        wxString::FromUTF8(Slic3r::to_string_nozero(next, kChipNameDecimals).c_str());
    m_result_label->SetLabel(text);
    Layout();
}

void CalibrationFlowDialog::select_tile(int index)
{
    if (m_tiles.empty())
        return;
    if (index < 0)
        index = 0;
    if (index >= static_cast<int>(m_tiles.size()))
        index = static_cast<int>(m_tiles.size()) - 1;
    m_selected = index;
    const wxColour selected = wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHT);
    const wxColour normal   = wxSystemSettings::GetColour(wxSYS_COLOUR_BTNFACE);
    for (size_t i = 0; i < m_tiles.size(); ++i) {
        m_tiles[i]->SetBackgroundColour(static_cast<int>(i) == m_selected ? selected : normal);
        m_tiles[i]->Refresh();
    }
    update_result_label();
}

void CalibrationFlowDialog::on_tile_clicked(wxCommandEvent& event_args)
{
    auto* clicked = dynamic_cast<wxButton*>(event_args.GetEventObject());
    int index = m_selected;
    for (size_t i = 0; i < m_tiles.size(); ++i) {
        if (m_tiles[i] == clicked) {
            index = static_cast<int>(i);
            break;
        }
    }
    select_tile(index);
}

void CalibrationFlowDialog::create_buttons(wxStdDialogButtonSizer* buttons)
{
    recalled_flow_parameters();

    wxBoxSizer* vertical = new wxBoxSizer(wxVERTICAL);

    wxButton* recommended = new wxButton(this, wxID_FILE1, _L("Generate recommended"));
    recommended->SetName(wxString::FromUTF8(AutomationIds::calibration("flow.generate_recommended").c_str()));
    recommended->Bind(wxEVT_BUTTON, &CalibrationFlowDialog::create_geometry_recommended, this);
    wxButton* fine = new wxButton(this, wxID_FILE2, _L("Generate fine"));
    fine->SetName(wxString::FromUTF8(AutomationIds::calibration("flow.generate_fine").c_str()));
    fine->Bind(wxEVT_BUTTON, &CalibrationFlowDialog::create_geometry_fine, this);

    wxBoxSizer* generate_row = new wxBoxSizer(wxHORIZONTAL);
    generate_row->Add(recommended, 0, wxRIGHT, kButtonRowSpacerPx);
    generate_row->Add(fine);
    vertical->Add(generate_row, 0, wxBOTTOM, kSectionGapPx);

    m_printed_with = new wxStaticText(this, wxID_ANY, wxEmptyString);
    update_printed_with_label();
    vertical->Add(m_printed_with, 0, wxBOTTOM, kSectionGapPx);

    wxFlexGridSizer* tile_grid = new wxFlexGridSizer(0, kTileGridCols, kFieldGapPx, kFieldGapPx);
    const wxSize tile_size(kTileWidthEm * em_unit(), kTileHeightEm * em_unit());
    m_tiles.clear();
    m_tiles.reserve(m_count);
    for (size_t i = 0; i < m_count; ++i) {
        const double modifier = m_first_modifier + static_cast<double>(i) * m_step;
        wxButton* tile = new wxButton(this, wxID_ANY, chip_button_label(modifier),
            wxDefaultPosition, tile_size);
        tile->SetName(wxString::FromUTF8(
            AutomationIds::calibration("flow.tile." + std::to_string(i)).c_str()));
        tile->SetToolTip(_L("Select this chip if it had the smoothest top. The label matches the engraved tab."));
        tile->Bind(wxEVT_BUTTON, &CalibrationFlowDialog::on_tile_clicked, this);
        tile_grid->Add(tile, 0);
        m_tiles.push_back(tile);
    }
    vertical->Add(tile_grid, 0, wxBOTTOM, kSectionGapPx);

    m_result_label = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_result_label->SetName(wxString::FromUTF8(AutomationIds::calibration("flow.preview").c_str()));
    vertical->Add(m_result_label, 0, wxBOTTOM, kSectionGapPx);

    wxButton* apply = new wxButton(this, wxID_ANY, _L("Apply to filament"),
        wxDefaultPosition, wxSize(kApplyButtonWidthEm * em_unit(), wxDefaultCoord));
    apply->SetName(wxString::FromUTF8(AutomationIds::calibration("flow.apply").c_str()));
    apply->SetToolTip(_L("Write the new extrusion multiplier on the filament this test was printed with."));
    apply->Bind(wxEVT_BUTTON, &CalibrationFlowDialog::apply_flow_result, this);
    vertical->Add(apply, 0, wxBOTTOM, kSectionGapPx);

    m_apply_status = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_apply_status->SetName(wxString::FromUTF8(AutomationIds::calibration("flow.result").c_str()));
    vertical->Add(m_apply_status);

    buttons->Add(vertical);
    select_tile(m_selected);
}

void CalibrationFlowDialog::create_geometry_recommended(wxCommandEvent&)
{
    Plater* plat = this->main_frame->plater();
    if (!plat->new_project(L("Orca YOLO flow calibration")))
        return;
    create_geometry(kRecommendedFirstModifier, kRecommendedStep, kRecommendedChipCount);
}

void CalibrationFlowDialog::create_geometry_fine(wxCommandEvent&)
{
    Plater* plat = this->main_frame->plater();
    if (!plat->new_project(L("Orca YOLO flow calibration fine")))
        return;
    create_geometry(kFineFirstModifier, kFineStep, kFineChipCount);
}

void CalibrationFlowDialog::create_geometry(double first_modifier, double step, size_t count)
{
    wxGetApp().Yield();

    Plater* plat  = this->main_frame->plater();
    Model&  model = plat->model();

    std::unique_ptr<wxWindowUpdateLocker> freeze_gui = std::make_unique<wxWindowUpdateLocker>(this);
    const bool autocenter = gui_app->app_config->get(kAutocenterKey) == "1";
    auto restore_autocenter = [this, autocenter]() {
        if (autocenter)
            gui_app->app_config->set(kAutocenterKey, "1");
    };
    if (autocenter)
        gui_app->app_config->set(kAutocenterKey, "0");

    const DynamicPrintConfig* print_config    = this->gui_app->get_tab(Preset::TYPE_FFF_PRINT)->get_config();
    const DynamicPrintConfig* printer_config  = this->gui_app->get_tab(Preset::TYPE_PRINTER)->get_config();
    const DynamicPrintConfig* filament_config = this->gui_app->get_tab(Preset::TYPE_FFF_FILAMENT)->get_config();

    double nozzle = CalibrationConstants::kDesignNozzleDiameterMm;
    if (const ConfigOptionFloats* nozzle_diameter =
            printer_config->option<ConfigOptionFloats>("nozzle_diameter")) {
        if (nozzle_diameter->size() > 0 && nozzle_diameter->get_at(0) > 0.0)
            nozzle = nozzle_diameter->get_at(0);
    }
    double filament_em = filament_config->option("extrusion_multiplier")->get_float(0);
    if (filament_em < kMinFilamentMultiplier)
        filament_em = 1.0;

    model.clear_objects();
    const char* filename = (std::abs(step - kFineStep) < 0.00025) ?
        kOrcaLinearFlowFineFile : kOrcaLinearFlowFile;
    const boost::filesystem::path path =
        boost::filesystem::path(Slic3r::resources_dir()) / "calibration" / "filament_flow" / filename;
    if (!boost::filesystem::exists(path)) {
        wxLogError("Missing Orca flow calibration model: %s", path.string().c_str());
        restore_autocenter();
        return;
    }
    const std::vector<size_t> loaded = plat->load_files(
        std::vector<std::string>{path.string()},
        LoadFileOption::LoadModel | LoadFileOption::DontUpdateDirs);
    if (loaded.empty()) {
        wxLogError("Could not load the Orca flow calibration model: %s", path.string().c_str());
        restore_autocenter();
        return;
    }

    const FlowPlateScale plate_scale = flow_plate_scale_from_config(nozzle, *print_config);
    scale_orca_linear_flow_plate(model, loaded, plate_scale.xy, plate_scale.z,
        plat->build_volume().bed_center());

    struct Chip { size_t idx; double modifier; };
    std::vector<Chip> chips;
    chips.reserve(loaded.size());
    for (size_t idx : loaded) {
        double modifier = 0.0;
        if (!modifier_from_orca_object_name(model.objects[idx]->name, modifier))
            continue;
        chips.push_back({idx, modifier});
    }
    if (chips.empty()) {
        wxLogError("The Orca flow model had no flowrate_* objects: %s", path.string().c_str());
        restore_autocenter();
        return;
    }
    std::sort(chips.begin(), chips.end(),
        [](const Chip& a, const Chip& b) { return a.modifier < b.modifier; });

    first_modifier = chips.front().modifier;
    count = chips.size();
    step = (count > 1) ?
        (chips.back().modifier - chips.front().modifier) / static_cast<double>(count - 1) :
        kRecommendedStep;

    DynamicPrintConfig new_filament_config = *filament_config;
    new_filament_config.option<ConfigOptionFloats>("slowdown_below_layer_time")->set_at(0, 0);

    std::vector<size_t> objs_idx;
    objs_idx.reserve(chips.size());
    for (size_t i = 0; i < chips.size(); ++i) {
        ModelObject* object = model.objects[chips[i].idx];
        object->name = chip_name(static_cast<int>(i) + 1, chips[i].modifier);
        apply_orca_flow_object_settings(object, filament_em, chips[i].modifier, nozzle);
        object->ensure_on_bed();
        objs_idx.push_back(chips[i].idx);
    }

    save_flow_parameters(first_modifier, step, count, filament_em);

    this->gui_app->get_tab(Preset::TYPE_FFF_FILAMENT)->load_config(new_filament_config);
    plat->on_config_change(new_filament_config);
    plat->changed_objects(objs_idx);
    this->gui_app->get_tab(Preset::TYPE_FFF_FILAMENT)->update_dirty();
    this->gui_app->obj_list()->update_after_undo_redo();
    freeze_gui.reset();

    DynamicPrintConfig new_print_config = *print_config;
    new_print_config.set_key_value("complete_objects", std::make_unique<ConfigOptionBool>(false));
    this->gui_app->get_tab(Preset::TYPE_FFF_PRINT)->load_config(new_print_config);
    plat->on_config_change(new_print_config);
    this->gui_app->get_tab(Preset::TYPE_FFF_PRINT)->update_dirty();

    if (plat->printer_technology() == ptFFF)
        plat->fff_print().apply(plat->model(), *plat->config());
    plat->reslice();

    restore_autocenter();

    close_dialog();
}

void CalibrationFlowDialog::apply_flow_result(wxCommandEvent&)
{
    const double next = computed_multiplier();
    if (!std::isfinite(next) || next < kMinFilamentMultiplier || next > kMaxFilamentMultiplier) {
        if (m_apply_status != nullptr)
            m_apply_status->SetLabel(_L("That multiplier is outside the 0.01 to 2.0 range."));
        return;
    }

    Tab* filament_tab = this->gui_app->get_tab(Preset::TYPE_FFF_FILAMENT);
    const std::string test_filament = gui_app->app_config->get(kFlowCalFilamentKey);
    if (!test_filament.empty() &&
        test_filament != gui_app->preset_bundle->filaments.get_selected_preset_name() &&
        !filament_tab->select_preset(test_filament)) {
        if (m_apply_status != nullptr) {
            m_apply_status->SetLabel(_L("Could not select the filament this test was printed with: ") +
                wxString::FromUTF8(test_filament.c_str()));
        }
        return;
    }

    const DynamicPrintConfig* filament_config = filament_tab->get_config();
    DynamicPrintConfig new_filament_config = *filament_config;
    auto new_multipliers = std::make_unique<ConfigOptionFloats>(std::initializer_list<double>{1.});
    if (const ConfigOptionFloats* current = filament_config->option<ConfigOptionFloats>("extrusion_multiplier"))
        new_multipliers->set(*current);
    new_multipliers->set_at(next, 0);
    new_filament_config.set_key_value("extrusion_multiplier", std::move(new_multipliers));

    filament_tab->load_config(new_filament_config);
    this->main_frame->plater()->on_config_change(new_filament_config);
    filament_tab->update_dirty();

    // Keep the next fine pass relative to the value we just wrote.
    save_flow_parameters(m_first_modifier, m_step, m_count, next);
    m_base_em = next;
    update_result_label();

    if (m_apply_status != nullptr) {
        m_apply_status->SetLabel(
            _L("Extrusion multiplier set to ") +
            wxString::FromUTF8(Slic3r::to_string_nozero(next, kChipNameDecimals).c_str()) +
            _L(". Check it in Filament Settings and save the preset to keep it."));
        this->Layout();
        this->Fit();
    }
}

} // namespace GUI
} // namespace Slic3r
