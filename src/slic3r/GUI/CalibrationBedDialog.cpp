#include "CalibrationBedDialog.hpp"
#include "I18N.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/LocalesUtils.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/AppConfig.hpp"
#include "GLCanvas3D.hpp"
#include "GUI.hpp"
#include "GUI_ObjectList.hpp"
#include "Plater.hpp"
#include "Tab.hpp"
#include <wx/scrolwin.h>
#include <wx/display.h>
#include <wx/file.h>
#include <wx/wupdlock.h>

#include <array>
#include <cmath>
#include <string>
#include <vector>

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
constexpr char kAutocenterConfigKey[]             = "autocenter";
constexpr char kBedLevelingResourceDirectory[]    = "bed_leveling";
constexpr char kCalibrationResourceDirectory[]    = "calibration";
constexpr char kCalibrationPatchFilename[]        = "patch.amf";
constexpr char kFillAngleConfigKey[]              = "fill_angle";

constexpr int    kZOffsetDecimalPlaces     = 4;   // to_string_nozero precision for Z cal values
constexpr int    kFieldGapPx               = 5;   // horizontal sizer spacing between fields
constexpr int    kSectionGapPx             = 6;   // vertical spacing between form sections
constexpr int    kBedPadMarginBase         = 10;  // base mm margin (scaled by xyScale) around pads
constexpr int    kBedLevelPadCount         = 5;   // center + four corner pads for bed leveling
constexpr int    kCenterPadNumber          = 5;   // 1-based center pad label in the 3x3 Z grid
constexpr int    kZOffsetGridSize          = 3;   // 3x3 Klipper Z-offset pad grid
constexpr int    kZOffsetPadCount          = 9;   // total pads in the 3x3 grid
constexpr int    kRectangularBedCorners    = 4;
constexpr int    kCalibPerimeters          = 2;
constexpr int    kCalibBottomSolidLayers   = 2;
constexpr int    kSingleSolidLayer         = 1;   // top/bottom solid layers on single-layer pads
constexpr int    kFieldWidthEm             = 8;   // text field width in em units
constexpr int    kButtonWidthEm            = 24;  // primary button width in em units
constexpr int    kDefaultOuterWalls        = 5;   // default outer walls on Z-offset pads
constexpr int    kMaxOuterWalls            = 20;
constexpr int    kFirstLayerExtrusionWidthPercent = 140;
constexpr int    kSolidFillPercent         = 100;
constexpr int    kArrangeJobTimeoutMs      = 20000;
constexpr double kZOffsetAbsLimitMm        = 2.0;
constexpr double kQuarterTurnDivisor       = 4.0;
constexpr double kDiagonalProjectionDivisor = 4.0;
constexpr double kDesignNozzleDiameterMm   = 0.4; // bed pad models are authored for this nozzle
constexpr double kDesignFirstLayerHeightMm = 0.2; // authored first-layer height of the pad model
constexpr double kXyScaleMinFactor         = 0.9; // skip XY rescale when near design nozzle
constexpr double kXyScaleMaxFactor         = 1.2;
constexpr double kDiagonalSqrt2Factor      = 1.414; // sqrt(2) for circular-bed pad placement
constexpr double kMaxZStepMm               = 0.25;
constexpr double kZeroOffsetEpsilonMm      = 1e-6;
constexpr double kFillAngleDeg90           = 90.0;
constexpr double kFillAngleDeg45           = 45.0;
constexpr double kFillAngleDeg0            = 0.0;
constexpr double kFillAngleDeg135          = 135.0;
constexpr double kRotateEighthsTopLeft     = 5.0; // 5/4 turn for rectangular-bed pad orientation
constexpr double kRotateEighthsBottomLeft  = 3.0;
constexpr double kRotateEighthsBottomRight = 7.0;
constexpr int    kZOffsetCenterIndex       = 4;
constexpr double kZOffsetRadiusSteps       = 4.0;
constexpr size_t kFourthLoadedObjectIndex  = 4;
constexpr size_t kThirdLoadedObjectIndex   = 3;
constexpr size_t kSecondLoadedObjectIndex  = 2;
constexpr double kHalfTurnDivisor          = 2.0;
constexpr double kCenterDivisor            = 2.0;
constexpr double kDoubleMarginFactor       = 2.0; // supported filament Z offset range ±mm
} // namespace

// First-layer height of the active print profile for nozzle 1, the default pad height.
double CalibrationBedDialog::profile_first_layer_height() const
{
    const DynamicPrintConfig* print_config = gui_app->get_tab(Preset::TYPE_FFF_PRINT)->get_config();
    const DynamicPrintConfig* printer_config = gui_app->get_tab(Preset::TYPE_PRINTER)->get_config();
    double default_layer_height = 0.30;
    if (const ConfigOptionFloats* nozzle_diameters = printer_config->option<ConfigOptionFloats>("nozzle_diameter");
        nozzle_diameters != nullptr && !nozzle_diameters->get_values().empty())
        default_layer_height = print_config->get_abs_value("first_layer_height", nozzle_diameters->get_at(0));
    return default_layer_height;
}

// Grid parameters recalled from the last generated test, with the same defaults the
// generate dialog shows, so the generate dialog, the result summary, and the apply
// math always agree.
void CalibrationBedDialog::recalled_grid_parameters(std::string& center, std::string& step, std::string& layer_height) const
{
    center = gui_app->app_config->get("z_offset_cal_center");
    step = gui_app->app_config->get("z_offset_cal_step");
    layer_height = gui_app->app_config->get("z_offset_cal_layer_height");
    if (center.empty())
        center = "0.08";
    if (step.empty())
        step = "0.027";
    if (layer_height.empty())
        layer_height = Slic3r::to_string_nozero(profile_first_layer_height(), kZOffsetDecimalPlaces);
}

void CalibrationBedDialog::create_buttons(wxStdDialogButtonSizer* buttons)
{
    if (m_mode == Mode::BedLeveling) {
        wxButton* bt = new wxButton(this, wxID_FILE1, _L("Generate"));
        bt->Bind(wxEVT_BUTTON, &CalibrationBedDialog::create_geometry, this);
        buttons->Add(bt);
        return;
    }

    const wxSize field_size(kFieldWidthEm * em_unit(), wxDefaultCoord);
    const wxSize button_size(kButtonWidthEm * em_unit(), wxDefaultCoord);
    std::string center, step, layer_height;
    recalled_grid_parameters(center, step, layer_height);
    std::string outer_walls = gui_app->app_config->get("z_offset_cal_outer_walls");
    if (outer_walls.empty())
        outer_walls = std::to_string(kDefaultOuterWalls);

    wxBoxSizer* vertical = new wxBoxSizer(wxVERTICAL);

    if (m_mode == Mode::ZOffsetGenerate) {
        txt_z_center = new wxTextCtrl(this, wxID_ANY, Slic3r::from_dot_to_local(center),
            wxDefaultPosition, field_size, wxBORDER_SIMPLE);
        txt_z_center->SetToolTip(_L("Z offset at the center pad. The other eight pads are spaced around it by the step value."));
        txt_z_step = new wxTextCtrl(this, wxID_ANY, Slic3r::from_dot_to_local(step),
            wxDefaultPosition, field_size, wxBORDER_SIMPLE);
        txt_z_step->SetToolTip(_L("Z offset difference between neighboring pads."));
        txt_layer_height = new wxTextCtrl(this, wxID_ANY, Slic3r::from_dot_to_local(layer_height),
            wxDefaultPosition, field_size, wxBORDER_SIMPLE);
        txt_layer_height->SetToolTip(_L("Height of the single-layer pads. Defaults to the active print profile's first-layer height for nozzle 1."));

        wxBoxSizer* offset_fields = new wxBoxSizer(wxHORIZONTAL);
        offset_fields->Add(new wxStaticText(this, wxID_ANY, _L("Center offset:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, kFieldGapPx);
        offset_fields->Add(txt_z_center, 0, wxRIGHT, kFieldGapPx);
        offset_fields->Add(new wxStaticText(this, wxID_ANY, _L("mm   Step:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, kFieldGapPx);
        offset_fields->Add(txt_z_step, 0, wxRIGHT, kFieldGapPx);
        offset_fields->Add(new wxStaticText(this, wxID_ANY, _L("mm")), 0, wxALIGN_CENTER_VERTICAL);
        vertical->Add(offset_fields, 0, wxBOTTOM, kSectionGapPx);

        wxBoxSizer* layer_fields = new wxBoxSizer(wxHORIZONTAL);
        layer_fields->Add(new wxStaticText(this, wxID_ANY, _L("Layer height:")),
            0, wxALIGN_CENTER_VERTICAL | wxRIGHT, kFieldGapPx);
        layer_fields->Add(txt_layer_height, 0, wxRIGHT, kFieldGapPx);
        txt_outer_walls = new wxTextCtrl(this, wxID_ANY, outer_walls,
            wxDefaultPosition, field_size, wxBORDER_SIMPLE);
        txt_outer_walls->SetToolTip(_L("Number of perimeter walls around each pad. More walls make edge thickness measurements more repeatable."));
        layer_fields->Add(new wxStaticText(this, wxID_ANY, _L("mm   Outer walls:")),
            0, wxALIGN_CENTER_VERTICAL | wxRIGHT, kFieldGapPx);
        layer_fields->Add(txt_outer_walls, 0);
        vertical->Add(layer_fields, 0, wxBOTTOM, kSectionGapPx);

        wxButton* generate = new wxButton(this, wxID_FILE1, _L("Generate nine-pad Z offset test"),
            wxDefaultPosition, button_size);
        generate->SetToolTip(_L("Generate a 3 by 3 single-layer Klipper test. The dialog closes after the project is created."));
        generate->Bind(wxEVT_BUTTON, &CalibrationBedDialog::create_z_offset_geometry, this);
        vertical->Add(generate);
    } else {
        // Measurement-only dialog: the grid parameters are recalled from the last
        // generated test, so the user only reports what they measured.
        vertical->Add(new wxStaticText(this, wxID_ANY,
            _L("Printed test grid: center offset ") + Slic3r::from_dot_to_local(center) +
            _L(" mm, step ") + Slic3r::from_dot_to_local(step) +
            _L(" mm, target height ") + Slic3r::from_dot_to_local(layer_height) + _L(" mm")),
            0, wxBOTTOM, kSectionGapPx);

        txt_z_result_pad = new wxTextCtrl(this, wxID_ANY, std::to_string(kCenterPadNumber),
            wxDefaultPosition, field_size, wxBORDER_SIMPLE);
        txt_z_result_pad->SetToolTip(_L("Number printed on the measured pad, from 1 through 9."));
        txt_measured_height = new wxTextCtrl(this, wxID_ANY, Slic3r::from_dot_to_local(layer_height),
            wxDefaultPosition, field_size, wxBORDER_SIMPLE);
        txt_measured_height->SetToolTip(_L("Measured outside-edge thickness of the selected pad."));

        wxBoxSizer* result_fields = new wxBoxSizer(wxHORIZONTAL);
        result_fields->Add(new wxStaticText(this, wxID_ANY, _L("Measured pad:")),
            0, wxALIGN_CENTER_VERTICAL | wxRIGHT, kFieldGapPx);
        result_fields->Add(txt_z_result_pad, 0, wxRIGHT, kFieldGapPx);
        result_fields->Add(new wxStaticText(this, wxID_ANY, _L("Measured height:")),
            0, wxALIGN_CENTER_VERTICAL | wxRIGHT, kFieldGapPx);
        result_fields->Add(txt_measured_height, 0, wxRIGHT, kFieldGapPx);
        result_fields->Add(new wxStaticText(this, wxID_ANY, _L("mm")),
            0, wxALIGN_CENTER_VERTICAL);
        vertical->Add(result_fields, 0, wxBOTTOM, kSectionGapPx);

        wxButton* apply_result = new wxButton(this, wxID_ANY, _L("Apply result to filament preset"),
            wxDefaultPosition, button_size);
        apply_result->SetToolTip(_L("Calculate the corrected Z offset from the recalled test parameters and write it to the active filament preset."));
        apply_result->Bind(wxEVT_BUTTON, &CalibrationBedDialog::apply_z_offset_result, this);
        vertical->Add(apply_result);
    }

    buttons->Add(vertical);
}

void CalibrationBedDialog::apply_z_offset_result(wxCommandEvent& /*event_args*/)
{
    std::string center_str, step_str, target_str;
    recalled_grid_parameters(center_str, step_str, target_str);

    long pad_number = 0;
    double center = 0.;
    double step = 0.;
    double target_height = 0.;
    double measured_height = 0.;
    try {
        center = parse_float_all_locale(center_str);
        step = parse_float_all_locale(step_str);
        target_height = parse_float_all_locale(target_str);
        measured_height = parse_float_all_locale(txt_measured_height->GetValue().ToStdString());
    } catch (...) {
        show_error(this, _L("The recalled test parameters and the measured height must be valid numbers."));
        return;
    }

    if (!txt_z_result_pad->GetValue().ToLong(&pad_number) || pad_number < 1 || pad_number > kZOffsetPadCount) {
        show_error(this, _L("Measured pad must be a whole number from 1 through 9."));
        return;
    }
    if (!std::isfinite(center) || !std::isfinite(step) || step <= 0. ||
        !std::isfinite(target_height) || target_height <= 0. ||
        !std::isfinite(measured_height) || measured_height <= 0.) {
        show_error(this, _L("Offsets and heights must be finite, and step and heights must be greater than zero."));
        return;
    }

    const double tested_offset = center + (pad_number - kCenterPadNumber) * step;
    double corrected_offset = tested_offset - (measured_height - target_height);
    if (std::abs(corrected_offset) < kZeroOffsetEpsilonMm)
        corrected_offset = 0.;
    if (!std::isfinite(corrected_offset) || corrected_offset < -kZOffsetAbsLimitMm || corrected_offset > kZOffsetAbsLimitMm) {
        show_error(this, _L("The calculated filament Z offset is outside the supported -2 to 2 mm range."));
        return;
    }

    Tab* filament_tab = this->gui_app->get_tab(Preset::TYPE_FFF_FILAMENT);
    const DynamicPrintConfig* filament_config = filament_tab->get_config();
    DynamicPrintConfig new_filament_config = *filament_config;
    auto new_offsets = std::make_unique<ConfigOptionFloats>(std::initializer_list<double>{0.});
    if (const ConfigOptionFloats* current_offsets = filament_config->option<ConfigOptionFloats>("filament_z_offset"))
        new_offsets->set(*current_offsets);
    new_offsets->set_at(corrected_offset, 0);
    new_filament_config.set_key_value("filament_z_offset", std::move(new_offsets));

    filament_tab->load_config(new_filament_config);
    this->main_frame->plater()->on_config_change(new_filament_config);
    filament_tab->update_dirty();

    const std::string corrected_text = Slic3r::to_string_nozero(corrected_offset, kZOffsetDecimalPlaces);
    gui_app->app_config->set("z_offset_cal_center", corrected_text);
    gui_app->app_config->set("z_offset_cal_step", Slic3r::to_string_nozero(step, kZOffsetDecimalPlaces));
    gui_app->app_config->set("z_offset_cal_layer_height", Slic3r::to_string_nozero(target_height, kZOffsetDecimalPlaces));

    wxMessageBox(
        _L("Calculated filament Z offset: ") + wxString::FromUTF8(corrected_text.c_str()) +
            _L(" mm.\n\nThe active filament preset has been updated. Review it in Filament Settings and save the preset to keep it."),
        _L("Z offset calibration"), wxOK | wxICON_INFORMATION, this);
    close_dialog();
}

void CalibrationBedDialog::create_geometry(wxCommandEvent& event_args) {
    Plater* plat = this->main_frame->plater();
    Model& model = plat->model();
    if (!plat->new_project(L("First layer calibration")))
        return;
    // wait for slicing end if needed
    wxGetApp().Yield();
    
    std::unique_ptr<wxWindowUpdateLocker> freeze_gui = std::make_unique<wxWindowUpdateLocker>(this);
    bool autocenter = gui_app->app_config->get(kAutocenterConfigKey) == "1";
    if(autocenter) {
        //disable aut-ocenter for this calibration.
        gui_app->app_config->set(kAutocenterConfigKey, "0");
    }
    std::vector<size_t> objs_idx = plat->load_files(std::vector<std::string>{
            (boost::filesystem::path(Slic3r::resources_dir()) / kCalibrationResourceDirectory / kBedLevelingResourceDirectory / kCalibrationPatchFilename).string(),
            (boost::filesystem::path(Slic3r::resources_dir()) / kCalibrationResourceDirectory / kBedLevelingResourceDirectory / kCalibrationPatchFilename).string(),
            (boost::filesystem::path(Slic3r::resources_dir()) / kCalibrationResourceDirectory / kBedLevelingResourceDirectory / kCalibrationPatchFilename).string(),
            (boost::filesystem::path(Slic3r::resources_dir()) / kCalibrationResourceDirectory / kBedLevelingResourceDirectory / kCalibrationPatchFilename).string(),
            (boost::filesystem::path(Slic3r::resources_dir()) / kCalibrationResourceDirectory / kBedLevelingResourceDirectory / kCalibrationPatchFilename).string()},
        LoadFileOption::LoadModel | LoadFileOption::DontUpdateDirs);

    assert(objs_idx.size() == kBedLevelPadCount);
    const DynamicPrintConfig* printConfig = this->gui_app->get_tab(Preset::TYPE_FFF_PRINT)->get_config();
    const DynamicPrintConfig* printerConfig = this->gui_app->get_tab(Preset::TYPE_PRINTER)->get_config();
    
    /// --- scale ---
    //model is created for a 0.4 nozzle, scale xy with nozzle size.
    const ConfigOptionFloats* nozzle_diameter = printerConfig->option<ConfigOptionFloats>("nozzle_diameter");
    assert(nozzle_diameter->size() > 0);
    float xyScale = nozzle_diameter->get_at(0) / kDesignNozzleDiameterMm;
    //scale z with the first_layer_height
    const ConfigOptionFloatOrPercent* first_layer_height = printConfig->option<ConfigOptionFloatOrPercent>("first_layer_height");
    float zscale = first_layer_height->get_abs_value(nozzle_diameter->get_at(0)) / kDesignFirstLayerHeightMm;
    //do scaling
    if (xyScale < kXyScaleMinFactor || kXyScaleMaxFactor < xyScale) {
        for (size_t i = 0; i < kBedLevelPadCount; i++)
            model.objects[objs_idx[i]]->scale(xyScale, xyScale, zscale);
    } else {
        for (size_t i = 0; i < kBedLevelPadCount; i++)
            model.objects[objs_idx[i]]->scale(1, 1, zscale);
    }

    /// --- rotate ---
    const ConfigOptionPoints* bed_shape = printerConfig->option<ConfigOptionPoints>("bed_shape");
    if (bed_shape->size() == kRectangularBedCorners) {
        model.objects[objs_idx[0]]->rotate(PI / kQuarterTurnDivisor, { 0,0,1 });
        model.objects[objs_idx[1]]->rotate(kRotateEighthsTopLeft * PI / kQuarterTurnDivisor, { 0,0,1 });
        model.objects[objs_idx[kThirdLoadedObjectIndex]]->rotate(kRotateEighthsBottomLeft * PI / kQuarterTurnDivisor, { 0,0,1 });
        model.objects[objs_idx[kFourthLoadedObjectIndex]]->rotate(kRotateEighthsBottomRight * PI / kQuarterTurnDivisor, { 0,0,1 });
    } else {
        model.objects[objs_idx[kThirdLoadedObjectIndex]]->rotate(PI / kHalfTurnDivisor, { 0,0,1 });
        model.objects[objs_idx[kFourthLoadedObjectIndex]]->rotate(PI / kHalfTurnDivisor, { 0,0,1 });
    }

    /// --- translate ---
    //three first will stay with this orientation (top left, middle, bottom right)
    //last two with 90deg (top left, middle, bottom right)
    //get position for patches
    const BoundingBoxf bed_bounds(bed_shape->get_values());
    const Vec2d bed_size = bed_bounds.size();
    const Vec2d bed_min = bed_bounds.min;
    float offsetx = kBedPadMarginBase + kBedPadMarginBase * xyScale;
    float offsety = kBedPadMarginBase + kBedPadMarginBase * xyScale;
    if (bed_shape->size() > kRectangularBedCorners) {
        offsetx = bed_size.x() / kCenterDivisor - bed_size.x() * kDiagonalSqrt2Factor / kDiagonalProjectionDivisor + kBedPadMarginBase * xyScale;
        offsety = bed_size.y() / kCenterDivisor - bed_size.y() * kDiagonalSqrt2Factor / kDiagonalProjectionDivisor + kBedPadMarginBase * xyScale;
    }
    bool large_enough = bed_shape->size() == kRectangularBedCorners ?
        (bed_size.x() > offsetx * kZOffsetGridSize && bed_size.y() > offsety * kZOffsetGridSize) :
        (bed_size.x() > offsetx * kDoubleMarginFactor + kBedPadMarginBase * xyScale && bed_size.y() > offsety * kDoubleMarginFactor + kBedPadMarginBase * xyScale);
    // note: objects are loaded around bed_size center (because of load_model bool : center_instances_around_point(this->bed.build_volume().bed_center());)
    if (large_enough) {
        ModelInstance *instance = model.objects[objs_idx[0]]->instances.front();
        instance->set_offset({ bed_min.x() + offsetx,               bed_min.y() + bed_size.y() - offsety, instance->get_offset().z() + 1 * zscale });
        instance = model.objects[objs_idx[1]]->instances.front();
        instance->set_offset({ bed_min.x() + bed_size.x() - offsetx,bed_min.y() + offsety ,               instance->get_offset().z() + 1 * zscale });
        instance = model.objects[objs_idx[kSecondLoadedObjectIndex]]->instances.front();
        instance->set_offset({ bed_min.x() + bed_size.x() / kCenterDivisor, bed_min.y() + bed_size.y() / kCenterDivisor, instance->get_offset().z() + 1 * zscale });
        instance = model.objects[objs_idx[kThirdLoadedObjectIndex]]->instances.front();
        instance->set_offset({ bed_min.x() + offsetx,               bed_min.y() + offsety,                instance->get_offset().z() + 1 * zscale });
        instance = model.objects[objs_idx[kFourthLoadedObjectIndex]]->instances.front();
        instance->set_offset({ bed_min.x() + bed_size.x() - offsetx,bed_min.y() + bed_size.y() - offsety, instance->get_offset().z() + 1 * zscale });
    }

    /// --- main config, please modify object config when possible ---
    DynamicPrintConfig new_print_config = *printConfig; //make a copy
    new_print_config.set_key_value("complete_objects", std::make_unique<ConfigOptionBool>(true));

    /// --- custom config ---
    for (size_t i = 0; i < kBedLevelPadCount; i++) {
        model.objects[objs_idx[i]]->config.set_key_value("perimeters", std::make_unique<ConfigOptionInt>(kCalibPerimeters));
        model.objects[objs_idx[i]]->config.set_key_value("bottom_solid_layers", std::make_unique<ConfigOptionInt>(kCalibBottomSolidLayers));
        model.objects[objs_idx[i]]->config.set_key_value("gap_fill_enabled", std::make_unique<ConfigOptionBool>(false));
        model.objects[objs_idx[i]]->config.set_key_value("first_layer_extrusion_width", std::make_unique<ConfigOptionFloatOrPercent>(kFirstLayerExtrusionWidthPercent, true));
        auto first_layer_infill_width = std::make_unique<ConfigOptionFloatOrPercent>(kFirstLayerExtrusionWidthPercent, true);
        first_layer_infill_width->set_can_be_disabled(true);
        model.objects[objs_idx[i]]->config.set_key_value("first_layer_infill_extrusion_width", std::move(first_layer_infill_width));
        model.objects[objs_idx[i]]->config.set_key_value("bottom_fill_pattern", std::make_unique<ConfigOptionEnum<InfillPattern>>(ipRectilinear));
        model.objects[objs_idx[i]]->config.set_key_value("infill_filled_bottom", std::make_unique<ConfigOptionBool>(true));
        //disable ironing post-process
        model.objects[objs_idx[i]]->config.set_key_value("ironing", std::make_unique<ConfigOptionBool>(false));
    }
    if (bed_shape->size() == kRectangularBedCorners) {
        model.objects[objs_idx[0]]->config.set_key_value(kFillAngleConfigKey, std::make_unique<ConfigOptionFloat>(kFillAngleDeg90));
        model.objects[objs_idx[1]]->config.set_key_value(kFillAngleConfigKey, std::make_unique<ConfigOptionFloat>(kFillAngleDeg90));
        model.objects[objs_idx[kSecondLoadedObjectIndex]]->config.set_key_value(kFillAngleConfigKey, std::make_unique<ConfigOptionFloat>(kFillAngleDeg45));
        model.objects[objs_idx[kThirdLoadedObjectIndex]]->config.set_key_value(kFillAngleConfigKey, std::make_unique<ConfigOptionFloat>(kFillAngleDeg0));
        model.objects[objs_idx[kFourthLoadedObjectIndex]]->config.set_key_value(kFillAngleConfigKey, std::make_unique<ConfigOptionFloat>(kFillAngleDeg0));
    } else {
        for (size_t i = 0; i < kZOffsetGridSize; i++)
        for (size_t i = kZOffsetGridSize; i < kBedLevelPadCount; i++)
            model.objects[objs_idx[i]]->config.set_key_value(kFillAngleConfigKey, std::make_unique<ConfigOptionFloat>(kFillAngleDeg135));
    }

    //update plater
    //GLCanvas3D::set_warning_freeze(false);
    this->gui_app->get_tab(Preset::TYPE_FFF_PRINT)->load_config(new_print_config);
    plat->on_config_change(new_print_config);
    plat->changed_objects(objs_idx);
    this->gui_app->get_tab(Preset::TYPE_FFF_PRINT)->update_dirty();
    //update everything, easier to code.
    this->gui_app->obj_list()->update_after_undo_redo();
    freeze_gui.reset();
    if (!large_enough) {
        //problem : too small, use arrange instead and let the user place them.
        Worker &ui_job_worker = plat->get_ui_job_worker();
        plat->arrange(ui_job_worker, false);
        ui_job_worker.wait_for_current_job(kArrangeJobTimeoutMs);
        show_info(this,
            _L("Calibration objects were auto-arranged because the bed was too small for the default layout."),
            _L("Bed calibration"));
    }
    //if(!plat->is_background_process_update_scheduled())
    //    plat->schedule_background_process();
    plat->reslice();

    if (autocenter) {
        //re-enable auto-center after this calibration.
        gui_app->app_config->set(kAutocenterConfigKey, "1");
    }

    close_dialog();
}

void CalibrationBedDialog::create_z_offset_geometry(wxCommandEvent& /*event_args*/)
{
    const DynamicPrintConfig* print_config = this->gui_app->get_tab(Preset::TYPE_FFF_PRINT)->get_config();
    const DynamicPrintConfig* printer_config = this->gui_app->get_tab(Preset::TYPE_PRINTER)->get_config();

    if (printer_config->option<ConfigOptionEnum<GCodeFlavor>>("gcode_flavor")->value != gcfKlipper) {
        show_error(this, _L("The Z offset calibration generator requires a Klipper printer profile."));
        return;
    }

    double center = 0.;
    double step = 0.;
    double layer_height = 0.;
    long outer_walls = 0;
    try {
        center = parse_float_all_locale(txt_z_center->GetValue().ToStdString());
        step = parse_float_all_locale(txt_z_step->GetValue().ToStdString());
        layer_height = parse_float_all_locale(txt_layer_height->GetValue().ToStdString());
    } catch (...) {
        show_error(this, _L("Center offset, step, and layer height must be valid numbers."));
        return;
    }

    if (!txt_outer_walls->GetValue().ToLong(&outer_walls) || outer_walls < kCalibPerimeters || outer_walls > kMaxOuterWalls) {
        show_error(this, _L("Outer walls must be a whole number between 2 and 20."));
        return;
    }

    if (!std::isfinite(center) || !std::isfinite(step) || step <= 0. || step > kMaxZStepMm ||
        center - kZOffsetRadiusSteps * step < -kZOffsetAbsLimitMm || center + kZOffsetRadiusSteps * step > kZOffsetAbsLimitMm) {
        show_error(this, _L("Use a positive step no greater than 0.25 mm, with all nine offsets between -2 and 2 mm."));
        return;
    }

    const ConfigOptionFloats* nozzle_diameters = printer_config->option<ConfigOptionFloats>("nozzle_diameter");
    if (nozzle_diameters == nullptr || nozzle_diameters->get_values().empty()) {
        show_error(this, _L("The active printer profile does not define a nozzle diameter."));
        return;
    }
    const double nozzle_diameter = nozzle_diameters->get_at(0);
    if (!std::isfinite(layer_height) || layer_height <= 0. || layer_height > nozzle_diameter) {
        show_error(this, _L("Layer height must be greater than zero and no greater than the active nozzle diameter."));
        return;
    }

    constexpr double pad_xy = 30.;
    constexpr double gap = 3.; // spacing between pads in the 3x3 grid (mm)
    constexpr double pitch = pad_xy + gap;
    constexpr double grid_span = double(kZOffsetGridSize) * pad_xy + kDoubleMarginFactor * gap;

    const ConfigOptionPoints* bed_shape = printer_config->option<ConfigOptionPoints>("bed_shape");
    if (bed_shape == nullptr || bed_shape->get_values().empty()) {
        show_error(this, _L("The active printer profile does not define a usable bed shape."));
        return;
    }

    const BoundingBoxf bed_box(bed_shape->get_values());
    if (bed_box.size().x() < grid_span || bed_box.size().y() < grid_span) {
        show_error(this, _L("The active bed is too small for the 96 by 96 mm Z offset calibration grid."));
        return;
    }

    Plater* plat = this->main_frame->plater();
    if (!plat->new_project(L("Klipper Z offset calibration")))
        return;
    wxGetApp().Yield();

    const bool autocenter = gui_app->app_config->get(kAutocenterConfigKey) == "1";
    if (autocenter)
        gui_app->app_config->set(kAutocenterConfigKey, "0");

    const std::string center_text = Slic3r::to_string_nozero(center, kZOffsetDecimalPlaces);
    const std::string step_text = Slic3r::to_string_nozero(step, kZOffsetDecimalPlaces);
    const std::string layer_height_text = Slic3r::to_string_nozero(layer_height, kZOffsetDecimalPlaces);
    gui_app->app_config->set("z_offset_cal_center", center_text);
    gui_app->app_config->set("z_offset_cal_step", step_text);
    gui_app->app_config->set("z_offset_cal_layer_height", layer_height_text);
    gui_app->app_config->set("z_offset_cal_outer_walls", std::to_string(outer_walls));

    Model& model = plat->model();
    model.clear_objects();

    const Vec2d bed_center = bed_box.center();
    const std::array<const char*, 9> positions = {
        "Top left", "Top center", "Top right",
        "Middle left", "Center", "Middle right",
        "Bottom left", "Bottom center", "Bottom right"
    };
    std::vector<size_t> object_indices;
    object_indices.reserve(positions.size());

    for (size_t index = 0; index < positions.size(); ++index) {
        const int row = int(index / kZOffsetGridSize);
        const int column = int(index % kZOffsetGridSize);
        // Center of 0..8 is index 4 (middle of 3x3).
        const double offset = center + (int(index) - kZOffsetCenterIndex) * step;
        const std::string offset_text = Slic3r::to_string_nozero(offset, kZOffsetDecimalPlaces);
        const std::string object_name = "Z" + std::to_string(index + 1) + " " +
            positions[index] + " " + offset_text + " mm";

        ModelObject* object = model.add_object(object_name.c_str(), "",
            Slic3r::make_cube(pad_xy, pad_xy, layer_height));
        object->center_around_origin();
        object->add_instance();
        const double x = bed_center.x() + (column - 1) * pitch;
        const double y = bed_center.y() + (1 - row) * pitch;
        object->instances.front()->set_offset(Vec3d(x, y, 0.));
        object->ensure_on_bed();

        object->config.set_key_value("layer_height", std::make_unique<ConfigOptionFloat>(layer_height));
        object->config.set_key_value("first_layer_height", std::make_unique<ConfigOptionFloatOrPercent>(layer_height, false));
        object->config.set_key_value("perimeters", std::make_unique<ConfigOptionInt>(static_cast<int>(outer_walls)));
        object->config.set_key_value("top_solid_layers", std::make_unique<ConfigOptionInt>(kSingleSolidLayer));
        object->config.set_key_value("bottom_solid_layers", std::make_unique<ConfigOptionInt>(kSingleSolidLayer));
        object->config.set_key_value("fill_density", std::make_unique<ConfigOptionPercent>(kSolidFillPercent));
        object->config.set_key_value("infill_filled_bottom", std::make_unique<ConfigOptionBool>(true));
        object->config.set_key_value("bottom_fill_pattern", std::make_unique<ConfigOptionEnum<InfillPattern>>(ipRectilinear));
        object->config.set_key_value("gap_fill_enabled", std::make_unique<ConfigOptionBool>(false));
        object->config.set_key_value("ironing", std::make_unique<ConfigOptionBool>(false));

        const std::string object_gcode =
            "; SuperSlicer-generated Z offset calibration pad " + std::to_string(index + 1) + "\n" +
            "; Calibration target layer height: " + layer_height_text + " mm\n" +
            "SET_GCODE_OFFSET Z=" + offset_text + " MOVE=0\n" +
            "RESPOND MSG=\"Z offset pad " + std::to_string(index + 1) + ": " +
                positions[index] + ", offset " + offset_text + " mm, target " + layer_height_text + " mm\"\n";
        object->config.set_key_value("object_gcode", std::make_unique<ConfigOptionString>(object_gcode));
        object_indices.push_back(index);
    }

    DynamicPrintConfig new_print_config = *print_config;
    new_print_config.set_key_value("complete_objects", std::make_unique<ConfigOptionBool>(false));
    new_print_config.set_key_value("brim_width", std::make_unique<ConfigOptionFloat>(0.));
    new_print_config.set_key_value("skirts", std::make_unique<ConfigOptionInt>(0));

    // The generated project already injects each Klipper offset. Remove only the legacy script
    // that performed the same injection, while preserving unrelated post-processing scripts.
    if (const ConfigOptionStrings* scripts = print_config->option<ConfigOptionStrings>("post_process")) {
        std::vector<std::string> filtered_scripts;
        filtered_scripts.reserve(scripts->size());
        for (const std::string& script : scripts->get_values())
            if (script.find("z_offset_test_postprocess.py") == std::string::npos)
                filtered_scripts.push_back(script);
        new_print_config.set_key_value("post_process", std::make_unique<ConfigOptionStrings>(std::move(filtered_scripts)));
    }

    this->gui_app->get_tab(Preset::TYPE_FFF_PRINT)->load_config(new_print_config);
    plat->on_config_change(new_print_config);
    plat->changed_objects(object_indices);
    this->gui_app->get_tab(Preset::TYPE_FFF_PRINT)->update_dirty();
    this->gui_app->obj_list()->update_after_undo_redo();

    if (plat->printer_technology() == ptFFF)
        plat->fff_print().apply(plat->model(), *plat->config());
    plat->reslice();

    if (autocenter)
        gui_app->app_config->set(kAutocenterConfigKey, "1");

    close_dialog();
}

} // namespace GUI
} // namespace Slic3r
