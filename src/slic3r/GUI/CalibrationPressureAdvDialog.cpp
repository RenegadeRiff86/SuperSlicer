///|/ PA calibration generator. Original implementation by legend069 (2024).
///|/ Modified 2026 by Stan Elston (RenegadeRiff86) -- see git history.
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher.
///|/
#include "CalibrationPressureAdvDialog.hpp"
#include "CalibrationPressureAdvShared.hpp"
#include "I18N.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/CustomGCode.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/AppConfig.hpp"
#include "GLCanvas3D.hpp"
#include "GUI.hpp"
#include "GUI_ObjectList.hpp"
#include "Plater.hpp"
#include "Tab.hpp"
#include <wx/scrolwin.h>
#include <wx/file.h>
#include <wx/choice.h>
#include <wx/msgdlg.h>
#include <wx/settings.h>
#include "Jobs/ArrangeJob.hpp"
//#include "Jobs/job.hpp" 2.7 requirement?
#include <array>
#include <iomanip>
#include <locale>
#include <sstream>
#include <unordered_map>

#undef NDEBUG
#include <cassert>


namespace Slic3r {
namespace GUI {

// Tracked in #40: confirm custom G-code ordering around extrusion-role and region changes.
// Number/point model Z scaling intentionally spans first_layer_height + base_layer_height so
// embossed labels still slice when first_layer_height <= base_layer_height. See #38.
// Tracked in #41: audit Marlin/RepRap PA calibration command semantics.
// Tracked in #42: support custom and multi-tool PA command workflows.
// PA label values are formatted compactly before digit meshes are loaded.
// Manual PA text entry is normalized to C-locale decimal text before ToCDouble parsing, so
// comma-decimal locales do not break PA calculations. See #38.


using namespace CalibrationPressureAdvDetail;

double CalibrationPressureAdvDialog::magical_scaling(
    double nozzle_diameter, double er_width, double perimeter_overlap,
    double external_perimeter_overlap, double base_layer_height)
{
    // er_width arrives as a percentage of the nozzle diameter (e.g. 112 == 112%); the guard
    // below converts it to an absolute mm width. A value already <= 3x the nozzle diameter is
    // treated as mm directly, as a fallback for any legacy callsite that passes mm.
    double extrusion_width = er_width;
    if (er_width > nozzle_diameter * kErWidthPercentThresholdMultiplier)
        extrusion_width = nozzle_diameter * (er_width / kPercentScale);

    const double model_design_width = nozzle_diameter * 4.0;
    const double round_cap = base_layer_height * (1.0 - 0.25 * M_PI);
    const double flat_width = extrusion_width - kRoundCapCount * round_cap;
    const double rounded_extrusion = round_cap + flat_width + round_cap;

    const double extrusion1 = rounded_extrusion; // 1 and 2 overlap
    const double extrusion2 = rounded_extrusion; // kGeometryCenterDivisor and 3 touch
    const double extrusion3 = rounded_extrusion;
    const double extrusion4 = rounded_extrusion; // 3 and 4 overlap

    const double spacing_external = extrusion_width - base_layer_height * (1.0 - 0.25 * M_PI) * external_perimeter_overlap;
    const double spacing_internal = extrusion_width - base_layer_height * (1.0 - 0.25 * M_PI) * perimeter_overlap;
    const double overlap_external = extrusion_width - spacing_external;
    const double overlap_internal = extrusion_width - spacing_internal;

    // The middle pair intentionally touches without contributing overlap to the total width.
    const double first_pair = extrusion1 + extrusion2 - overlap_external;
    const double second_pair = extrusion3 + extrusion4 - overlap_internal;
    const double perfect_sliced_width = first_pair + second_pair;

    return perfect_sliced_width / model_design_width;
}

void CalibrationPressureAdvDialog::create_buttons(wxStdDialogButtonSizer* buttons) {

    const DynamicPrintConfig* printer_config = this->gui_app->get_tab(Preset::TYPE_PRINTER)->get_config();
    GCodeFlavor flavor = printer_config->option<ConfigOptionEnum<GCodeFlavor>>("gcode_flavor")->value;
    // Reading "dark_color_mode" straight from the config returns false whenever the key is
    // absent, but GUI_App::dark_mode() falls back to system detection - that split painted a
    // white control strip inside a dark dialog. "color_dark" is the button-text-on-hover accent
    // (Preferences calls it "Text color template"), not a label colour; at the stock cc6429 it
    // reads 3.9:1 on white and 3.3:1 on dark grey, both under the 4.5:1 minimum.
    const wxColour text_color = wxGetApp().get_style_role_color("tab.text.default");
    const wxColour background_color = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);

    std::string prefix = (gcfMarlinFirmware == flavor) ? " LA " : ((gcfKlipper == flavor || gcfRepRap == flavor) ? " PA " : "unsupported firmware type");

    if (prefix != "unsupported firmware type") {

        wxPanel* mainPanel = new wxPanel(this, wxID_ANY);
        mainPanel->SetBackgroundColour(background_color);
        mainPanel->Raise();

        // Create a vertical sizer for the panel
        wxBoxSizer* panelSizer = new wxBoxSizer(wxVERTICAL);
        mainPanel->SetSizer(panelSizer);

        // Create the common controls sizer
        wxBoxSizer* commonSizer = new wxBoxSizer(wxHORIZONTAL);

        wxString number_of_runs[] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "10" };//setting this any higher will break loading the model for the ID
        nbRuns = new wxComboBox(mainPanel, wxID_ANY, wxString{ "1" }, wxDefaultPosition, wxDefaultSize, 10, number_of_runs, wxCB_READONLY);
        nbRuns->SetToolTip(_L("Select the number of calibration lines to generate. Max 6 is recommended due to bed size limits."));
        nbRuns->SetSelection(0);
        nbRuns->Bind(wxEVT_COMBOBOX, &CalibrationPressureAdvDialog::on_row_change, this);

        wxStaticText* text_generate_count = new wxStaticText(mainPanel, wxID_ANY, _L("Number of" + prefix + "calibration lines: "));
        text_generate_count->SetForegroundColour(text_color);
        commonSizer->Add(text_generate_count, 0, wxALIGN_CENTER_VERTICAL | wxALL, kControlBorder);
        commonSizer->Add(nbRuns, 0, wxALIGN_CENTER_VERTICAL | wxALL, kControlBorder);

        // Create a button for generating models
        wxButton* generateButton = new wxButton(mainPanel, wxID_FILE1, _L("Generate"));
        generateButton->Bind(wxEVT_BUTTON, &CalibrationPressureAdvDialog::create_geometry, this);
        commonSizer->Add(generateButton, 0, wxALIGN_CENTER_VERTICAL | wxALL, kControlBorder);

        panelSizer->Add(commonSizer, 0, wxALL, kPanelInnerPaddingPx);
        dynamicSizer = new wxBoxSizer(wxVERTICAL);
        panelSizer->Add(dynamicSizer, 1, wxEXPAND | wxALL, kControlBorder);
        buttons->Add(mainPanel, 1, wxEXPAND | wxALL, kPanelInnerPaddingPx);

        currentTestCount = wxAtoi(nbRuns->GetValue());
        create_row_controls(dynamicSizer, currentTestCount);
    } else {

        wxStaticText* incompatiable_text = new wxStaticText(this, wxID_ANY, _L(prefix));
        incompatiable_text->SetForegroundColour(*wxRED); // Set the text color to red for the incompatiable firmware tpe
        buttons->Add(incompatiable_text);
    }
}

void CalibrationPressureAdvDialog::create_row_controls(wxBoxSizer* parentSizer, int row_count) {

    // Same theme source as create_buttons - see the note there.
    const wxColour text_color = wxGetApp().get_style_role_color("tab.text.default");

    //
    //wxArrayInt
    //wxArrayDouble
    //wxArrayDouble choices_first_layerPA[] = { 0.025, 0.030, 0.035, 0.040, 0.045, 0.050 };
    wxString choices_first_layerPA[] = { "0.025", "0.030", "0.035", "0.040", "0.045", "0.050" };
    wxString choices_start_PA[] = { "0.0", "0.010", "0.020", "0.030", "0.040", "0.050" };
    wxString choices_end_PA[] = { "0.10", "0.20", "0.30", "0.40", "0.50", "0.60", "0.70", "0.80", "0.90", "1.00" };
    wxString choices_increment_PA[] = { "0.0010", "0.0025", "0.0035", "0.005", "0.006", "0.007", "0.01", "0.1" };
    wxString choices_extrusion_role[] = {
        ROLE_INTERNAL_INFILL, ROLE_BRIDGE_INFILL, ROLE_EXTERNAL_PERIMETER, ROLE_GAP_FILL, ROLE_INTERNAL_BRIDGE_INFILL,
        ROLE_IRONING, ROLE_OVERHANG_PERIMETER, ROLE_PERIMETER, ROLE_SOLID_INFILL, ROLE_SUPPORT_MATERIAL,
        ROLE_SUPPORT_MATERIAL_INTERFACE, ROLE_THIN_WALL, ROLE_TOP_SOLID_INFILL, ROLE_FIRST_LAYER, ROLE_CHECK_ALL
    };
    const DynamicPrintConfig* filament_config = this->gui_app->get_tab(Preset::TYPE_FFF_FILAMENT)->get_config();
    const DynamicPrintConfig* printer_config = this->gui_app->get_tab(Preset::TYPE_PRINTER)->get_config();
    GCodeFlavor flavor = printer_config->option<ConfigOptionEnum<GCodeFlavor>>("gcode_flavor")->value;
    std::string prefix = (gcfMarlinFirmware == flavor) ? " LA " : ((gcfKlipper == flavor || gcfRepRap == flavor) ? " PA " : "unsupported firmware type");
    const PaControlDefaults pa_defaults = pa_control_defaults_from_filament(filament_config);

    int current_selection = 2;//start selection at ExternalPerimeter

    if (!dynamicExtrusionRole.empty()) {
        const wxString last_selected_role =
            dynamicExtrusionRole[currentTestCount - 1]->GetValue();
        const auto found = std::find(
            std::begin(choices_extrusion_role),
            std::end(choices_extrusion_role),
            last_selected_role);
        if (found != std::end(choices_extrusion_role))
            current_selection = int(std::distance(
                std::begin(choices_extrusion_role), found)) + 1;
    }
    current_selection = std::min(current_selection, static_cast<int>(sizeof(choices_extrusion_role) / sizeof(choices_extrusion_role[0]) - 1));

    for (int i = 0; i < row_count; i++) {
        wxBoxSizer* rowSizer = new wxBoxSizer(wxHORIZONTAL);

        // wxDefaultSize, not a pixel width: the combos size to their longest entry, so values
        // like "0.0010" are not clipped at a larger font or display scale.
        wxComboBox* firstPaCombo = new wxComboBox(parentSizer->GetContainingWindow(), wxID_ANY, pa_defaults.first_layer, wxDefaultPosition, wxDefaultSize, 6, choices_first_layerPA);
        wxStaticText* text_first_l_prefix = new wxStaticText(parentSizer->GetContainingWindow(), wxID_ANY, _L("First Layers" + prefix + "value: "));
        text_first_l_prefix->SetForegroundColour(text_color);
        rowSizer->Add(text_first_l_prefix, 0, wxALIGN_CENTER_VERTICAL | wxALL, kControlBorder);
        firstPaCombo->SetToolTip(_L("Select the" + prefix + "value to be used for the first layer only.\n(this gets added to 'before_layer_gcode' area)"));
        rowSizer->Add(firstPaCombo, 0, wxALIGN_CENTER_VERTICAL | wxALL, kControlBorder);
        dynamicFirstPa.push_back(firstPaCombo);

        rowSizer->AddSpacer(kRowSpacer);

        wxComboBox* startPaCombo = new wxComboBox(parentSizer->GetContainingWindow(), wxID_ANY, pa_defaults.start, wxDefaultPosition, wxDefaultSize, 6, choices_start_PA);
        wxStaticText* text_start_value = new wxStaticText(parentSizer->GetContainingWindow(), wxID_ANY, _L("Start value: "));
        text_start_value->SetForegroundColour(text_color);
        rowSizer->Add(text_start_value, 0, wxALIGN_CENTER_VERTICAL | wxALL, kControlBorder);
        startPaCombo->SetToolTip(_L("Select the starting" + prefix + "value to be used.\nDefaults use the current filament PA when enabled.\n (you can manually type in values!)"));
        rowSizer->Add(startPaCombo, 0, wxALIGN_CENTER_VERTICAL);
        dynamicStartPa.push_back(startPaCombo);// can't validate input here since this is where they type it in..

        rowSizer->AddSpacer(kRowSpacer);

        wxComboBox* endPaCombo = new wxComboBox(parentSizer->GetContainingWindow(), wxID_ANY, pa_defaults.end, wxDefaultPosition, wxDefaultSize, 10, choices_end_PA);
        wxStaticText* text_end_value = new wxStaticText(parentSizer->GetContainingWindow(), wxID_ANY, _L("End value: "));
        text_end_value->SetForegroundColour(text_color);
        rowSizer->Add(text_end_value, 0, wxALIGN_CENTER_VERTICAL | wxALL, kControlBorder);
        endPaCombo->SetToolTip(_L("Select the ending" + prefix + "value to be used.\nDefaults use the current filament PA when enabled.\n (you can manually type in values!)"));
        rowSizer->Add(endPaCombo, 0, wxALIGN_CENTER_VERTICAL);
        dynamicEndPa.push_back(endPaCombo);

        rowSizer->AddSpacer(kRowSpacer);

        wxComboBox* paIncrementCombo = new wxComboBox(parentSizer->GetContainingWindow(), wxID_ANY, pa_defaults.increment, wxDefaultPosition, wxDefaultSize, 8, choices_increment_PA);
        wxStaticText* text_increment = new wxStaticText(parentSizer->GetContainingWindow(), wxID_ANY, _L("Increment by: "));
        text_increment->SetForegroundColour(text_color);
        rowSizer->Add(text_increment, 0, wxALIGN_CENTER_VERTICAL | wxALL, kControlBorder);
        paIncrementCombo->SetToolTip(_L("Select the incremental value.\nDefaults use the current filament PA when enabled.\n (you can manually type in values!)"));
        rowSizer->Add(paIncrementCombo, 0, wxALIGN_CENTER_VERTICAL);
        dynamicPaIncrement.push_back(paIncrementCombo);

        rowSizer->AddSpacer(kRowSpacer);

        wxComboBox* erPaCombo = new wxComboBox(parentSizer->GetContainingWindow(), wxID_ANY, wxString{ choices_extrusion_role[current_selection] }, wxDefaultPosition, wxDefaultSize, kExtrusionRoleChoiceCount, choices_extrusion_role, wxCB_READONLY);
            // disable user edit this one :)
        wxStaticText* text_extrusion_role = new wxStaticText(parentSizer->GetContainingWindow(), wxID_ANY, _L("Extrusion role: "));
        text_extrusion_role->SetForegroundColour(text_color);
        rowSizer->Add(text_extrusion_role, 0, wxALIGN_CENTER_VERTICAL | wxALL, kControlBorder);
        erPaCombo->SetToolTip(_L("Select the extrusion role you want to generate a calibration for"));
        erPaCombo->SetSelection(current_selection);
        rowSizer->Add(erPaCombo, 0, wxALIGN_CENTER_VERTICAL);
        dynamicExtrusionRole.push_back(erPaCombo);

        // Increment selection for the next row
        current_selection++;
        if (current_selection >= int(sizeof(choices_extrusion_role) / sizeof(choices_extrusion_role[0]))) {
            current_selection = 0; // Wrap around: SetSelection does it's own memory access checks so this shouldn't be needed. but it's a nice safe guard to have.
        }

        if (prefix == " PA ") {//klipper only feature ?
            rowSizer->AddSpacer(kRowSpacer);
            wxCheckBox* enableST = new wxCheckBox(parentSizer->GetContainingWindow(), wxID_ANY, _L("Calibrate Smooth Time instead of Advance"), wxDefaultPosition, wxDefaultSize);
            enableST->SetForegroundColour(text_color);
            enableST->SetToolTip(_L("When enabled, the start/end/increment values will sweep Klipper's SMOOTH_TIME parameter instead of ADVANCE.\n\nSmooth Time controls how long extruder velocity changes are averaged to smooth out rapid "
                                    "pressure changes.\nShorter times (e.g., 0.01s) suit fast printing; longer times (e.g., 0.4s) suit slower printing.\nKlipper default: 0.04s."));
            enableST->SetValue(false);
            enableST->Bind(wxEVT_CHECKBOX, &CalibrationPressureAdvDialog::on_smooth_time_toggle, this);
            rowSizer->Add(enableST, 1, wxALIGN_CENTER_VERTICAL);
            dynamicEnableST.push_back(enableST);
        }

        parentSizer->Add(rowSizer, 0, wxALL, 2);// change this to make each row have a larger/smaller 'gap' between them
        dynamicRowcount.push_back(rowSizer);
    }
}

void CalibrationPressureAdvDialog::on_row_change(wxCommandEvent& event) {
    int new_test_count = wxAtoi(nbRuns->GetValue());

    wxSize auto_size = GetSize();
    //wxSize auto_size = DoGetBestSize();

    const auto remove_last_smooth_time_control = [&]() {
        if (dynamicEnableST.empty())
            return;
        dynamicEnableST.pop_back();
        assert(dynamicEnableST.size() == dynamicExtrusionRole.size());
    };

    if (new_test_count > currentTestCount) {
        create_row_controls(dynamicSizer, new_test_count - currentTestCount);
    } else if (new_test_count < currentTestCount) {
        for (int i = currentTestCount - 1; i >= new_test_count; --i) {
            wxBoxSizer* row = dynamicRowcount.back();
            row->Clear(true);
            const bool removed = dynamicSizer->Remove(row);
            assert(removed);
            dynamicRowcount.pop_back();
            dynamicFirstPa.pop_back();
            dynamicStartPa.pop_back();
            dynamicEndPa.pop_back();
            dynamicPaIncrement.pop_back();
            dynamicExtrusionRole.pop_back();
            remove_last_smooth_time_control();
        }
    }

    currentTestCount = new_test_count;
    dynamicSizer->Layout();
    this->Fit();
    
    //this->SetSize(1600,600);
    this->SetSize(auto_size); //makes GUI flash on updating

}

void CalibrationPressureAdvDialog::on_smooth_time_toggle(wxCommandEvent& event) {
    // Find which row's checkbox was toggled
    wxCheckBox* cb = dynamic_cast<wxCheckBox*>(event.GetEventObject());
    if (!cb) return;

    int row = -1;
    for (size_t i = 0; i < dynamicEnableST.size(); i++) {
        if (dynamicEnableST[i] == cb) { row = static_cast<int>(i); break; }
    }
    if (row < 0 || row >= static_cast<int>(dynamicFirstPa.size())) return;

    bool enabled = cb->GetValue();

    if (enabled) {
        // Save current values before overwriting
        savedPaBeforeST[row] = {
            dynamicFirstPa[row]->GetValue(),
            dynamicStartPa[row]->GetValue(),
            dynamicEndPa[row]->GetValue(),
            dynamicPaIncrement[row]->GetValue(),
            dynamicExtrusionRole[row]->GetValue()
        };

        // Set recommended smooth time calibration values (Klipper default is 0.04s)
        // ExternalPerimeter is the standard role for smooth time tuning
        dynamicFirstPa[row]->SetValue("0.040");
        dynamicStartPa[row]->SetValue("0.010");
        dynamicEndPa[row]->SetValue("0.080");
        dynamicPaIncrement[row]->SetValue("0.005");
        dynamicExtrusionRole[row]->SetValue(ROLE_EXTERNAL_PERIMETER);

        // Disable editing — smooth time calibration uses fixed recommended values
        dynamicFirstPa[row]->Enable(false);
        dynamicStartPa[row]->Enable(false);
        dynamicEndPa[row]->Enable(false);
        dynamicPaIncrement[row]->Enable(false);
        dynamicExtrusionRole[row]->Enable(false);
    } else {
        // Restore saved values
        auto it = savedPaBeforeST.find(row);
        if (it != savedPaBeforeST.end()) {
            dynamicFirstPa[row]->SetValue(it->second.firstPa);
            dynamicStartPa[row]->SetValue(it->second.startPa);
            dynamicEndPa[row]->SetValue(it->second.endPa);
            dynamicPaIncrement[row]->SetValue(it->second.increment);
            dynamicExtrusionRole[row]->SetValue(it->second.extrusionRole);
            savedPaBeforeST.erase(it);
        }

        // Re-enable editing
        dynamicFirstPa[row]->Enable(true);
        dynamicStartPa[row]->Enable(true);
        dynamicEndPa[row]->Enable(true);
        dynamicPaIncrement[row]->Enable(true);
        dynamicExtrusionRole[row]->Enable(true);
    }
}

std::pair<std::vector<double>, int> CalibrationPressureAdvDialog::calc_PA_values(int id_item) {
    wxString firstPaValue = dynamicFirstPa[id_item]->GetValue();
    wxString startPaValue = dynamicStartPa[id_item]->GetValue();
    wxString endPaValue = dynamicEndPa[id_item]->GetValue();
    wxString paIncrementValue = dynamicPaIncrement[id_item]->GetValue();

    // Normalize comma decimal text before C-locale parsing. See #38.

    /*std::locale loc("");
    const std::numpunct<char>& np = std::use_facet<std::numpunct<char>>(loc);

    // Get the locale-specific decimal and thousands separators
    wxString decimal_sep = wxString::Format("%c", np.decimal_point());
    wxString thousands_sep = wxString::Format("%c", np.thousands_sep());

    // Replace the decimal separator with a dot
    if (!decimal_sep.IsEmpty() ) {
        firstPaValue.Replace(thousands_sep, decimal_sep);
        startPaValue.Replace(thousands_sep, decimal_sep);
        endPaValue.Replace(thousands_sep, decimal_sep);
        paIncrementValue.Replace(thousands_sep, decimal_sep);
    }
    */
    firstPaValue.Replace(",", ".");
    startPaValue.Replace(",", ".");
    endPaValue.Replace(",", ".");
    paIncrementValue.Replace(",", ".");
    auto show_input_error = [this, id_item](const wxString& details) {
        wxMessageBox(
            wxString::Format(_L("Invalid pressure advance input in row %d:\n%s"), id_item + 1, details),
            _L("Invalid calibration values"),
            wxOK | wxICON_ERROR,
            this);
    };
    
    //maybe? will need to load in the correct 'acsii' character based on localization then swap ?
    //any point idiot profing the input to stop crashing ? nothing stopping users typing in letters to force a crash...

    // Parse with ToCDouble (C locale, '.' decimal) after the comma->dot normalization above,
    // so input parses correctly regardless of the user's system locale. Plain ToDouble uses
    // the current locale and rejects "0.02" under comma-decimal locales (see #38).
    double first_pa = 0.0;
    bool first_pa_ok = firstPaValue.ToCDouble(&first_pa);
    double start_pa = 0.0;
    bool start_pa_ok = startPaValue.ToCDouble(&start_pa);
    double end_pa = 0.0;
    bool end_pa_ok = endPaValue.ToCDouble(&end_pa);
    double pa_increment = 0.0;
    bool pa_increment_ok = paIncrementValue.ToCDouble(&pa_increment);

    if (!first_pa_ok || !start_pa_ok || !end_pa_ok || !pa_increment_ok) {
        show_input_error(_L("Please enter numeric values for first PA, start PA, end PA, and PA increment."));
        return std::make_pair(std::vector<double>{}, 0);
    }

    if (pa_increment <= 0.0) {
        show_input_error(_L("PA increment must be greater than 0."));
        return std::make_pair(std::vector<double>{}, 0);
    }

    if (end_pa < start_pa) {
        show_input_error(_L("End PA must be greater than or equal to Start PA."));
        return std::make_pair(std::vector<double>{}, 0);
    }

    constexpr int max_pa_points = 500;
    int estimated_points = static_cast<int>(std::ceil((end_pa - start_pa) / pa_increment)) + 1;
    if (estimated_points > max_pa_points) {
        show_input_error(wxString::Format(
            _L("Too many PA values (%d). Increase the increment or reduce the range. Maximum allowed is %d."),
            estimated_points,
            max_pa_points));
        return std::make_pair(std::vector<double>{}, 0);
    }

    int countincrements = 0;
    int sizeofarray = estimated_points + 1;//'+1' keeps room for the end-pa failsafe branch.
    std::vector<double> pa_values(sizeofarray);

    double incremented_pa_value = start_pa;
    while (incremented_pa_value <= end_pa + pa_increment / kGeometryCenterDivisor) {
        if (incremented_pa_value <= end_pa) {
            double rounded_pa = std::round(incremented_pa_value * kMicrosecondsPerSecond) / kMicrosecondsPerSecond;
            pa_values[countincrements] = rounded_pa;
            countincrements++;
            incremented_pa_value += pa_increment;
        } else {
            pa_values[countincrements] = end_pa;
            countincrements++;//failsafe if werid input numbers are provided that can't add the "ending pa" number to the array.
            break;
        }
    }// is there a limit of how many models SS can load ? might be good to set a failsafe just so it won't load 10k+ models...

    return std::make_pair(pa_values, countincrements);
}

void CalibrationPressureAdvDialog::close_me_wrapper(wxCommandEvent& event) {// for custom location of "close" button
    this->close_me(event);
}
} // namespace GUI
} // namespace Slic3r
