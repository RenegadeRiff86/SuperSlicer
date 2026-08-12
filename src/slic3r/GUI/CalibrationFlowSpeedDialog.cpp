#include "CalibrationFlowSpeedDialog.hpp"
#include "I18N.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/LocalesUtils.hpp"
#include "libslic3r/Model.hpp"
#include "Jobs/ArrangeJob.hpp"
#include "GLCanvas3D.hpp"
#include "GUI.hpp"
#include "GUI_ObjectList.hpp"
#include "Plater.hpp"
#include "Tab.hpp"
#include <wx/scrolwin.h>
#include <wx/display.h>
#include <wx/file.h>
#include "MsgDialog.hpp"

#include <string>

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
// Dialog layout (em units / spacing).
constexpr int kFieldWidthEm = 6;
constexpr int kButtonWidthEm = 18;
constexpr int kLabelWidthEm = 15;
constexpr int kShortLabelWidthEm = 8;
constexpr int kUnitWidthEm = 4;
constexpr int kRowSpacerPx = 20;
// ComboBox item counts and default selections.
constexpr int kGramChoiceCount = 5;
constexpr int kGramDefaultSelection = 2; // "1" g
constexpr int kStepsChoiceCount = 8;
constexpr int kStepsDefaultSelection = 4; // "5" patches
constexpr int kMinOverlapDefaultSelection = 6; // "60"%
// Speed / flow math.
constexpr float kSpeedHeadroomFactor = 10.f;
constexpr float kPercentScale = 100.f;
constexpr float kDensityGPerCm3ToGPerMm3 = 1000.f;
constexpr int kMaxSizeIterations = 100;
constexpr int kDecimalPlaces = 4;
constexpr int kSectionSpacerPx = 5;
constexpr int kHalveSizeDivisor = 2;
constexpr float kMaxOverlapCapPercent = 80.f;
constexpr int kFlowAverageDivisor = 2;

void append_cube_name_suffix(std::string& name, size_t i, int nb_steps,
    float min_overlap, float max_overlap, float min_speed, float max_speed,
    float min_flow, float max_flow)
{
    if (nb_steps <= 1)
        return;
    if (min_overlap < max_overlap) {
        const float overlap = min_overlap + i * (max_overlap - min_overlap) / (nb_steps - 1);
        name += std::string("_") + std::to_string(int(overlap));
        return;
    }
    if (min_speed < max_speed) {
        const float speed = min_speed + i * (max_speed - min_speed) / (nb_steps - 1);
        name += std::string("_") + std::to_string(int(speed));
        return;
    }
    if (min_flow < max_flow) {
        const float flow = min_flow + i * (max_flow - min_flow) / (nb_steps - 1);
        name += std::string("_") + std::to_string(int(flow * kPercentScale + EPSILON));
    }
}

float flow_multiplier_percent(size_t i, int nb_steps, float min_flow, float max_flow, float extrusion_mult)
{
    if (nb_steps == 1)
        return kPercentScale * (min_flow + max_flow) / (kFlowAverageDivisor * extrusion_mult);
    return kPercentScale * (min_flow + i * (max_flow - min_flow) / (nb_steps - 1)) / extrusion_mult;
}
} // namespace

void CalibrationFlowSpeedDialog::create_buttons(wxStdDialogButtonSizer* buttons){
    const wxSize size(kFieldWidthEm * em_unit(), wxDefaultCoord);
    const wxSize bt_size(kButtonWidthEm * em_unit(), wxDefaultCoord);

    wxString choices_gram[] = { "0.2","0.5","1","2","5","10" };
    //cmb_gram = new wxComboBox(this, wxID_ANY, wxString{ "1" }, wxDefaultPosition, wxDefaultSize, kGramChoiceCount, choices_gram);
    cmb_gram = new ComboBox(this, wxID_ANY, wxString{"1"}, wxDefaultPosition, size, kGramChoiceCount, choices_gram);
    cmb_gram->SetToolTip(_L("Choose the size of the patch to print (in gramme). A bigger weight allow to have more precision but it takes longer to print"));
    cmb_gram->SetSelection(kGramDefaultSelection);
    
    wxString choices_nb[] = { "1","2","3","4","5","6","7","8" };
    //cmb_nb_steps = new wxComboBox(this, wxID_ANY, wxString{ "4" }, wxDefaultPosition, wxDefaultSize, kStepsChoiceCount, choices_nb);
    cmb_nb_steps = new ComboBox(this, wxID_ANY, wxString{"4"}, wxDefaultPosition, size, kStepsChoiceCount, choices_nb);
    cmb_nb_steps->SetToolTip(_L("Select the number of patches."));
    cmb_nb_steps->SetSelection(kStepsDefaultSelection);

    // check max speed (constrained by filament max volumetric flow)

    const DynamicPrintConfig* print_config = this->gui_app->get_tab(Preset::TYPE_FFF_PRINT)->get_config();
    const DynamicPrintConfig* printer_config = this->gui_app->get_tab(Preset::TYPE_PRINTER)->get_config();
    const DynamicPrintConfig* filament_config = this->gui_app->get_tab(Preset::TYPE_FFF_FILAMENT)->get_config();
    float max_vol_flow = filament_config->option("filament_max_volumetric_speed")->get_float(0);
    float max_speed = filament_config->option("filament_max_speed")->get_float(0);
    float curr_speed = print_config->get_computed_value("solid_infill_speed", 0);

    // compute max speed
    if (max_vol_flow == 0 && max_speed == 0)
        max_speed = curr_speed * kSpeedHeadroomFactor;
    if (max_vol_flow > 0) {
        float layer_height = print_config->option("layer_height")->get_float();
        float nz = printer_config->option("nozzle_diameter")->get_float(0);
        layer_height = std::max(layer_height, float(print_config->get_abs_value("first_layer_height", nz)));
        float filament_max_overlap = filament_config->option("filament_max_overlap")->get_float();
        Flow  flow                 = Flow::new_from_config(FlowRole::frSolidInfill, *print_config, nz, layer_height,
                                          filament_max_overlap / kPercentScale, false);
        float current_flow = flow.mm3_per_mm();
        if (max_speed > 0) {
            max_speed = std::min(max_speed, curr_speed * max_vol_flow / current_flow);
        } else {
            max_speed = curr_speed * max_vol_flow / current_flow;
        }
        max_speed = std::min(max_speed, curr_speed * kSpeedHeadroomFactor);
        if (printer_config->option<ConfigOptionEnum<MachineLimitsUsage>>("machine_limits_usage")->value != MachineLimitsUsage::Ignore) {
            float max_feedarete_x = printer_config->option("machine_max_feedrate_x")->get_float(0);
            max_speed             = std::min(max_speed, max_feedarete_x);
        }
    }

    float min_speed = std::max(filament_config->option("min_print_speed")->get_float(0), 0.);
    if (min_speed <= 0)
        min_speed = curr_speed / kSpeedHeadroomFactor;
    txt_min_speed = new wxTextCtrl(this, wxID_ANY, Slic3r::from_dot_to_local(Slic3r::to_string_nozero(min_speed, kDecimalPlaces)), wxDefaultPosition, size);
    txt_min_speed->SetToolTip(_L("Speed of the first patch."));

    txt_max_speed = new wxTextCtrl(this, wxID_ANY, Slic3r::from_dot_to_local(Slic3r::to_string_nozero(max_speed, kDecimalPlaces)), wxDefaultPosition, size);
    txt_max_speed->SetToolTip(_L("Speed of the first patch."));

    constexpr double kDefaultMinFlowMultiplier = 0.9;
    constexpr double kDefaultMaxFlowMultiplier = 1.3;
    txt_min_flow = new wxTextCtrl(this, wxID_ANY, Slic3r::from_dot_to_local(Slic3r::to_string_nozero(kDefaultMinFlowMultiplier, kDecimalPlaces)), wxDefaultPosition, size);
    txt_min_flow->SetToolTip(_L("Minimum extrusion multiplier."));

    txt_max_flow = new wxTextCtrl(this, wxID_ANY, Slic3r::from_dot_to_local(Slic3r::to_string_nozero(kDefaultMaxFlowMultiplier, kDecimalPlaces)), wxDefaultPosition, size);
    txt_max_flow->SetToolTip(_L("Maximum extrusion multiplier."));

    wxString choices_min_overlap[] = { "0","10","20","30","40","50","60","70","80","90"};
    //cmb_min_overlap = new wxComboBox(this, wxID_ANY, wxString{ "4" }, wxDefaultPosition, wxDefaultSize, kStepsChoiceCount, choices_min_overlap);
    cmb_min_overlap = new ComboBox(this, wxID_ANY, wxString{ "4" }, wxDefaultPosition, size, kStepsChoiceCount, choices_min_overlap);
    cmb_min_overlap->SetToolTip(_L("Minimum overlap (0%: llnes don't touch (bad but easy to print)"
        " ; 100%: no empty spaces (almost impossible, the filament isn't liquid enough)."));
    cmb_min_overlap->SetSelection(kMinOverlapDefaultSelection);
    
    wxString choices_max_overlap[] = { "100","90","80","70","60","50","30", "10"};
    //cmb_max_overlap = new wxComboBox(this, wxID_ANY, wxString{ "4" }, wxDefaultPosition, wxDefaultSize, kStepsChoiceCount, choices_max_overlap);
    cmb_max_overlap = new ComboBox(this, wxID_ANY, wxString{ "4" }, wxDefaultPosition, size, kStepsChoiceCount, choices_max_overlap);
    cmb_max_overlap->SetToolTip(_L("Maximum overlap (0%: llnes don't touch (bad but easy to print)"
        " ; 100%: no empty spaces (almost impossible, the filament isn't liquid enough)."));
    cmb_max_overlap->SetSelection(0);
    
    wxBoxSizer* vertical =new wxBoxSizer(wxVERTICAL);
    wxBoxSizer* hsizer_common =new wxBoxSizer(wxHORIZONTAL);
    wxBoxSizer* hsizer_overlap =new wxBoxSizer(wxHORIZONTAL);
    wxBoxSizer* hsizer_flow =new wxBoxSizer(wxHORIZONTAL);
    wxBoxSizer* hsizer_speed =new wxBoxSizer(wxHORIZONTAL);

    hsizer_common->Add(new wxStaticText(this, wxID_ANY, _L("Patch weight:"), wxDefaultPosition, {kLabelWidthEm * em_unit(), -1}, wxALIGN_RIGHT));
    hsizer_common->Add(cmb_gram);
    hsizer_common->Add(new wxStaticText(this, wxID_ANY, wxString(" ") + _L("g"), wxDefaultPosition, {kUnitWidthEm * em_unit(), -1}, wxALIGN_LEFT));
    hsizer_common->AddSpacer(kRowSpacerPx);
    hsizer_common->Add(new wxStaticText(this, wxID_ANY, _L("steps:"), wxDefaultPosition, {kShortLabelWidthEm * em_unit(), -1}, wxALIGN_RIGHT));
    hsizer_common->Add(cmb_nb_steps);
    //hsizer_common->AddSpacer(kRowSpacerPx);
    
    hsizer_flow->Add(new wxStaticText(this, wxID_ANY, _L("Extrusion multiplier:") + wxString(" ") + _L("min:"), wxDefaultPosition, {kLabelWidthEm * em_unit(), -1}, wxALIGN_RIGHT));
    hsizer_flow->Add(txt_min_flow);
    hsizer_flow->Add(new wxStaticText(this, wxID_ANY, wxString(" "), wxDefaultPosition, {kUnitWidthEm * em_unit(), -1}, wxALIGN_LEFT));
    hsizer_flow->AddSpacer(kRowSpacerPx);
    hsizer_flow->Add(new wxStaticText(this, wxID_ANY, _L("max:"), wxDefaultPosition, {kShortLabelWidthEm * em_unit(), -1}, wxALIGN_RIGHT));
    hsizer_flow->Add(txt_max_flow);
    hsizer_flow->Add(new wxStaticText(this, wxID_ANY, wxString(" "), wxDefaultPosition, {kUnitWidthEm * em_unit(), -1}, wxALIGN_LEFT));
    hsizer_flow->AddSpacer(kRowSpacerPx);
    wxButton* bt_flow = new wxButton(this, wxID_FILE2, _L("Generate for multiple flows"), wxDefaultPosition, bt_size);
    bt_flow->Bind(wxEVT_BUTTON, &CalibrationFlowSpeedDialog::create_flow, this);
    hsizer_flow->Add(bt_flow);

    hsizer_overlap->Add(new wxStaticText(this, wxID_ANY, _L("min overlap:"), wxDefaultPosition, {kLabelWidthEm * em_unit(), -1}, wxALIGN_RIGHT));
    hsizer_overlap->Add(cmb_min_overlap);
    hsizer_overlap->Add(new wxStaticText(this, wxID_ANY, wxString(" ") + _L("%"), wxDefaultPosition, {kUnitWidthEm * em_unit(), -1}, wxALIGN_LEFT));
    hsizer_overlap->AddSpacer(kRowSpacerPx);
    hsizer_overlap->Add(new wxStaticText(this, wxID_ANY, _L("max overlap:"), wxDefaultPosition, {kShortLabelWidthEm * em_unit(), -1}, wxALIGN_RIGHT));
    hsizer_overlap->Add(cmb_max_overlap);
    hsizer_overlap->Add(new wxStaticText(this, wxID_ANY, wxString(" ") + _L("%"), wxDefaultPosition, {kUnitWidthEm * em_unit(), -1}, wxALIGN_LEFT));
    hsizer_overlap->AddSpacer(kRowSpacerPx);
    wxButton* bt_overlap = new wxButton(this, wxID_FILE3, _L("Generate for multiple overlaps"), wxDefaultPosition, bt_size); 
    bt_overlap->Bind(wxEVT_BUTTON, &CalibrationFlowSpeedDialog::create_overlap, this);
    hsizer_overlap->Add(bt_overlap);

    hsizer_speed->Add(new wxStaticText(this, wxID_ANY, _L("min speed:"), wxDefaultPosition, {kLabelWidthEm * em_unit(), -1}, wxALIGN_RIGHT));
    hsizer_speed->Add(txt_min_speed);
    hsizer_speed->Add(new wxStaticText(this, wxID_ANY, wxString(" ") + _L("mm/s"), wxDefaultPosition, {kUnitWidthEm * em_unit(), -1}, wxALIGN_LEFT));
    hsizer_speed->AddSpacer(kRowSpacerPx);
    hsizer_speed->Add(new wxStaticText(this, wxID_ANY, _L("max speed:"), wxDefaultPosition, {kShortLabelWidthEm * em_unit(), -1}, wxALIGN_RIGHT));
    hsizer_speed->Add(txt_max_speed);
    hsizer_speed->Add(new wxStaticText(this, wxID_ANY, wxString(" ") + _L("mm/s"), wxDefaultPosition, {kUnitWidthEm * em_unit(), -1}, wxALIGN_LEFT));
    hsizer_speed->AddSpacer(kRowSpacerPx);
    wxButton* bt_speed = new wxButton(this, wxID_FILE1, _L("Generate for multiple speeds"), wxDefaultPosition, bt_size);
    bt_speed->Bind(wxEVT_BUTTON, &CalibrationFlowSpeedDialog::create_speed, this);
    hsizer_speed->Add(bt_speed);

    vertical->Add(hsizer_common);
    vertical->AddSpacer(kSectionSpacerPx);
    vertical->Add(hsizer_flow);
    vertical->AddSpacer(kSectionSpacerPx);
    vertical->Add(hsizer_overlap);
    vertical->AddSpacer(kSectionSpacerPx);
    vertical->Add(hsizer_speed);

    buttons->Add(vertical);
}


std::tuple<float, float, Flow> CalibrationFlowSpeedDialog::get_cube_size(float overlap) {
    const DynamicPrintConfig* print_config = this->gui_app->get_tab(Preset::TYPE_FFF_PRINT)->get_config();
    const DynamicPrintConfig* printer_config = this->gui_app->get_tab(Preset::TYPE_PRINTER)->get_config();
    const DynamicPrintConfig* filament_config = this->gui_app->get_tab(Preset::TYPE_FFF_FILAMENT)->get_config();

    // compute patch size

    float density = filament_config->option("filament_density")->get_float(0);
    if (density<=0)
        density = 1.25;
    // g/cm3 to g/mm3
    density = density / kDensityGPerCm3ToGPerMm3;

    std::string str = cmb_gram->GetValue().ToStdString();
    float weight = parse_float_all_locale(str);

    float nz = printer_config->option("nozzle_diameter")->get_float(0);
    // get max height
    float max_height = print_config->option("extruder_clearance_height")->get_float();
    // multiple of layer height
    float layer_height = print_config->option("layer_height")->get_float();
    layer_height = std::max(layer_height, float(print_config->get_abs_value("first_layer_height", nz)));
    max_height = int(max_height / layer_height) * layer_height;

    //compute flow
    Flow flow;
    float mult_size_for_overlap = 1;
    if (overlap < kPercentScale) {
        //Flow flow = Flow::new_from_config(FlowRole::frSolidInfill, *print_config, nz, layer_height, filament_max_overlap / kPercentScale, false);
        flow = Flow::Flow::new_from_config_width(FlowRole::frSolidInfill, 
            *print_config->option<ConfigOptionFloatOrPercent>("solid_infill_extrusion_width"),
            *print_config->option<ConfigOptionFloatOrPercent>("solid_infill_extrusion_spacing"),
            nz, layer_height, overlap / kPercentScale);
        float width = flow.width();
        Flow flow_full = Flow::new_from_width(width, nz, layer_height, 1);
        //same width, -> flow has higher spacing, same flow
        assert(std::abs(flow.mm3_per_mm() - flow_full.mm3_per_mm()) < EPSILON);
        assert(flow.spacing() > flow_full.spacing());
        mult_size_for_overlap = std::sqrt(flow.spacing() / flow_full.spacing());
    } else {
        float filament_max_overlap = filament_config->option("filament_max_overlap")->get_float();
        flow = Flow::new_from_config(FlowRole::frSolidInfill, *print_config, nz, layer_height, filament_max_overlap / kPercentScale, false);
    }

    //compute square size
    for(size_t max_iter = 0; max_iter < kMaxSizeIterations; max_iter++) {
        // get cube size
        // x*y*z*density = weight
        // x*y = weight / (z*density)
        float size_x = std::sqrt(weight / (density * max_height));
        // enlarge if overlap < 1
        if (mult_size_for_overlap != 1) {
            size_x -= flow.width() * 2.09; // remove external perimeter
            size_x *= mult_size_for_overlap;
            size_x += flow.width() * 2.09; // re-add external perimeter
        }

        // add external perimeter width/spacing diff
        float spacing_diff = layer_height * float(1. - 0.25 * PI);
        size_x += spacing_diff;

        if (size_x > max_height / kHalveSizeDivisor)
            return {size_x, max_height, flow};

        max_height = max_height / kHalveSizeDivisor;
        max_height = int(max_height / layer_height) * layer_height;
    }
    assert(false);
    return { 0, 0 , Flow::bridging_flow(0.2f, 0.2f)};
}

void CalibrationFlowSpeedDialog::create_overlap(wxCommandEvent &event_args) 
{
    std::string str_parse = cmb_min_overlap->GetValue().ToStdString();
    float min_overlap = parse_float_all_locale(str_parse);
    str_parse = cmb_max_overlap->GetValue().ToStdString();
    float max_overlap = parse_float_all_locale(str_parse);

    //const DynamicPrintConfig* print_config = this->gui_app->get_tab(Preset::TYPE_FFF_PRINT)->get_config();
    //const DynamicPrintConfig* printer_config = this->gui_app->get_tab(Preset::TYPE_PRINTER)->get_config();
    //const DynamicPrintConfig* filament_config = this->gui_app->get_tab(Preset::TYPE_FFF_FILAMENT)->get_config();
    //DynamicPrintConfig full_config = *print_config;
    //full_config.apply(*printer_config);
    //full_config.apply(*filament_config);
    //float speed = full_config.get_computed_value("solid_infill_speed", 0);

    const DynamicPrintConfig* filament_config = this->gui_app->get_tab(Preset::TYPE_FFF_FILAMENT)->get_config();
    float extrusion_mult = filament_config->option("extrusion_multiplier")->get_float(0);

    create_geometry(extrusion_mult, extrusion_mult, 0, 0, min_overlap, max_overlap);
}

void CalibrationFlowSpeedDialog::create_flow(wxCommandEvent &event_args) 
{
    std::string str_parse_min = txt_min_flow->GetValue().ToStdString();
    float min_flow = parse_float_all_locale(str_parse_min);
    std::string str_parse_max = txt_max_flow->GetValue().ToStdString();
    float max_flow = parse_float_all_locale(str_parse_max);

    //const DynamicPrintConfig* print_config = this->gui_app->get_tab(Preset::TYPE_FFF_PRINT)->get_config();
    //const DynamicPrintConfig* printer_config = this->gui_app->get_tab(Preset::TYPE_PRINTER)->get_config();
    //const DynamicPrintConfig* filament_config = this->gui_app->get_tab(Preset::TYPE_FFF_FILAMENT)->get_config();
    //DynamicPrintConfig full_config = *print_config;
    //full_config.apply(*printer_config);
    //full_config.apply(*filament_config);
    //float speed = full_config.get_computed_value("solid_infill_speed", 0);

    const DynamicPrintConfig* print_config = this->gui_app->get_tab(Preset::TYPE_FFF_PRINT)->get_config();
    const DynamicPrintConfig* filament_config = this->gui_app->get_tab(Preset::TYPE_FFF_FILAMENT)->get_config();
    float overlap      = print_config->option("solid_infill_overlap")->get_float();
    float filament_max_overlap = filament_config->option("filament_max_overlap")->get_float();
    overlap = std::min(overlap, filament_max_overlap);

    create_geometry(min_flow, max_flow, 0, 0, std::min(kMaxOverlapCapPercent, overlap), std::min(kMaxOverlapCapPercent, overlap));
}

void CalibrationFlowSpeedDialog::create_speed(wxCommandEvent &event_args) 
{
    std::string str_parse = txt_min_speed->GetValue().ToStdString();
    float min_speed = parse_float_all_locale(str_parse);
    str_parse = txt_max_speed->GetValue().ToStdString();
    float max_speed = parse_float_all_locale(str_parse);

    const DynamicPrintConfig* print_config = this->gui_app->get_tab(Preset::TYPE_FFF_PRINT)->get_config();
    const DynamicPrintConfig* filament_config = this->gui_app->get_tab(Preset::TYPE_FFF_FILAMENT)->get_config();
    float overlap      = print_config->option("solid_infill_overlap")->get_float();
    float filament_max_overlap = filament_config->option("filament_max_overlap")->get_float();
    overlap = std::min(overlap, filament_max_overlap);

    float extrusion_mult = filament_config->option("extrusion_multiplier")->get_float(0);

    create_geometry(extrusion_mult, extrusion_mult, min_speed, max_speed, overlap, overlap);
}

void CalibrationFlowSpeedDialog::create_geometry(
    float min_flow, //0-2
    float max_flow, //0-2
    float min_speed, //0-150+ mm/s
    float max_speed, //0-150+ mm/s
    float min_overlap, //0-100 %
    float max_overlap //0-100 %
    ) {

    Plater* plat = this->main_frame->plater();
    Model& model = plat->model();
    if (!plat->new_project(L("Flow calibration")))
        return;
    // wait for slicing end if needed
    wxGetApp().Yield();

    //GLCanvas3D::set_warning_freeze(true);
    bool autocenter = gui_app->app_config->get("autocenter") == "1";
    if (autocenter) {
        //disable auto-center for this calibration.
        gui_app->app_config->set("autocenter", "0");
    }

    std::string str_parse = cmb_nb_steps->GetValue().ToStdString();
    // The combo is editable, so this can parse as 0 or negative. Comparing
    // `size_t i < nb_steps` then converts the negative to ~1.8e19 and the loop
    // spins forever building objects, so clamp to at least one step.
    int         nb_steps  = std::stoi(str_parse);
    if (nb_steps < 1)
        nb_steps = 1;

    const DynamicPrintConfig* print_config = this->gui_app->get_tab(Preset::TYPE_FFF_PRINT)->get_config();
    const DynamicPrintConfig* printer_config = this->gui_app->get_tab(Preset::TYPE_PRINTER)->get_config();
    const DynamicPrintConfig* filament_config = this->gui_app->get_tab(Preset::TYPE_FFF_FILAMENT)->get_config();

    model.clear_objects();
    std::vector<ModelObject*> objs;
    std::vector<Flow> objs_flow;
    for (size_t i = 0; i < size_t(nb_steps); i++) {
        // if overlap, compute the size
        float overlap = max_overlap;
        if (nb_steps > 1 && min_overlap < max_overlap)
            overlap   = min_overlap + i * (max_overlap - min_overlap) / (nb_steps - 1);
        auto [cube_xy,cube_z, flow] = get_cube_size(overlap);
        // create name
        std::string name = "cube";
        append_cube_name_suffix(name, i, nb_steps, min_overlap, max_overlap, min_speed, max_speed, min_flow, max_flow);
        // create object
        objs.push_back(model.add_object(name.c_str(), "", Slic3r::make_cube(cube_xy, cube_xy, cube_z)));
        objs.back()->add_instance();
        objs_flow.push_back(flow);
    }

    /// --- main config, please modify object config when possible ---
    DynamicPrintConfig new_print_config = *print_config; //make a copy
    new_print_config.set_key_value("complete_objects", std::make_unique<ConfigOptionBool>(true));
    //if skirt, use only one
    //if (print_config->option<ConfigOptionInt>("skirts")->get_int() > 0 && print_config->option<ConfigOptionInt>("skirt_height")->get_int() > 0) {
    //    new_print_config.set_key_value("complete_objects_one_skirt", std::make_unique<ConfigOptionBool>(true));
    //}

    // same for printer config
    DynamicPrintConfig new_printer_config = *printer_config; //make a copy
    new_printer_config.option<ConfigOptionFloatsOrPercents>("seam_gap")->set_at(FloatOrPercent{0, false}, 0);

    // same for filament config
    DynamicPrintConfig new_filament_config = *filament_config; //make a copy
    new_filament_config.option<ConfigOptionFloats>("slowdown_below_layer_time")->set_at(0, 0);
    
    /// --- custom config ---
    float nz = printer_config->option("nozzle_diameter")->get_float(0);
    float layer_height = print_config->option("layer_height")->get_float();
    layer_height = std::max(layer_height, float(print_config->get_abs_value("first_layer_height", nz)));
    const float extrusion_mult = filament_config->option("extrusion_multiplier")->get_float(0);
    assert(objs_flow.size() == objs.size());
    assert(nb_steps == objs.size());
    for (size_t i = 0; i < size_t(nb_steps); i++) {

        if (min_flow < max_flow) {
            objs[i]->config.set_key_value("print_extrusion_multiplier",
                std::make_unique<ConfigOptionPercent>(flow_multiplier_percent(i, nb_steps, min_flow, max_flow, extrusion_mult)));
        }

        float overlap = max_overlap;
        if (nb_steps > 1 && min_overlap < max_overlap)
            overlap = min_overlap + i * (max_overlap - min_overlap) / (nb_steps - 1);
        objs[i]->config.set_key_value("perimeter_overlap", std::make_unique<ConfigOptionPercent>(overlap));
        objs[i]->config.set_key_value("external_perimeter_overlap", std::make_unique<ConfigOptionPercent>(overlap));
        objs[i]->config.set_key_value("solid_infill_overlap", std::make_unique<ConfigOptionPercent>(overlap));
        objs[i]->config.set_key_value("top_solid_infill_overlap", std::make_unique<ConfigOptionPercent>(overlap));

        Flow flow = objs_flow[i];
        objs[i]->config.set_key_value("solid_infill_extrusion_width", std::make_unique<ConfigOptionFloatOrPercent>(flow.width(), false));
        objs[i]->config.set_key_value("top_infill_extrusion_width", std::make_unique<ConfigOptionFloatOrPercent>(flow.width(), false));
        objs[i]->config.set_key_value("perimeter_extrusion_width", std::make_unique<ConfigOptionFloatOrPercent>(flow.width(), false));
        objs[i]->config.set_key_value("external_perimeter_extrusion_width", std::make_unique<ConfigOptionFloatOrPercent>(flow.width(), false));
        // keep first_layer_extrusion_width, it doesn't change the weight.
        //objs[i]->config.set_key_value("first_layer_extrusion_width", std::make_unique<ConfigOptionFloatOrPercent>(flow.width(), false));

        objs[i]->config.set_key_value("first_layer_size_compensation", std::make_unique<ConfigOptionFloat>(0));

        // no brim (but a skirt for primming)
        objs[i]->config.set_key_value("brim_ears", std::make_unique<ConfigOptionBool>(false));
        objs[i]->config.set_key_value("brim_width", std::make_unique<ConfigOptionFloat>(0));

        objs[i]->config.set_key_value("enforce_full_fill_volume", std::make_unique<ConfigOptionBool>(true));
        objs[i]->config.set_key_value("solid_infill_every_layers", std::make_unique<ConfigOptionInt>(1));
        objs[i]->config.set_key_value("layer_height", std::make_unique<ConfigOptionFloat>(layer_height));
        objs[i]->config.set_key_value("first_layer_height", std::make_unique<ConfigOptionFloatOrPercent>(layer_height, false));
        objs[i]->config.set_key_value("solid_fill_pattern", std::make_unique<ConfigOptionEnum<InfillPattern>>(ipRectilinear));
        objs[i]->config.set_key_value("top_fill_pattern", std::make_unique<ConfigOptionEnum<InfillPattern>>(ipRectilinear));
        objs[i]->config.set_key_value("perimeter_generator", std::make_unique<ConfigOptionEnum<PerimeterGeneratorType>>(PerimeterGeneratorType::Classic));
        //disable ironing post-process
        objs[i]->config.set_key_value("ironing", std::make_unique<ConfigOptionBool>(false));
        //set speed
        if (nb_steps > 1 && min_speed < max_speed) {
            float speed = float(min_speed + i * double(max_speed - min_speed) / (nb_steps - 1));
            objs[i]->config.set_key_value("perimeter_speed", std::make_unique<ConfigOptionFloatOrPercent>(speed, false));
            objs[i]->config.set_key_value("external_perimeter_speed", std::make_unique<ConfigOptionFloatOrPercent>(speed, false));
            objs[i]->config.set_key_value("solid_infill_speed", std::make_unique<ConfigOptionFloatOrPercent>(speed, false));
            objs[i]->config.set_key_value("top_solid_infill_speed", std::make_unique<ConfigOptionFloatOrPercent>(speed, false));
        }
        // keep first_layer_speed.
    }

    //update plater
    //GLCanvas3D::set_warning_freeze(false);
    this->gui_app->get_tab(Preset::TYPE_FFF_PRINT)->load_config(new_print_config);
    plat->on_config_change(new_print_config);
    this->gui_app->get_tab(Preset::TYPE_PRINTER)->load_config(new_printer_config);
    plat->on_config_change(new_printer_config);
    this->gui_app->get_tab(Preset::TYPE_FFF_FILAMENT)->load_config(new_filament_config);
    plat->on_config_change(new_filament_config);
    //plat->changed_objects(objs_idx);
    this->gui_app->get_tab(Preset::TYPE_FFF_PRINT)->update_dirty();

    //update everything, easier to code.
    ObjectList* obj = this->gui_app->obj_list();
    obj->update_after_undo_redo();

    // arrange if needed, after new settings, to take them into account
    if (true) { //has_to_arrange) {
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
