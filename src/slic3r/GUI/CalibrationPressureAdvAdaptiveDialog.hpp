///|/ Copyright (c) 2026 Stan Elston (RenegadeRiff86)
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher.
///|/
#ifndef slic3r_GUI_CalibrationPressureAdvAdaptiveDialog_hpp_
#define slic3r_GUI_CalibrationPressureAdvAdaptiveDialog_hpp_

#include "CalibrationAbstractDialog.hpp"
#include "Widgets/ComboBox.hpp"

namespace Slic3r {
namespace GUI {

// Adaptive pressure advance calibration generator (issue #39, Phase B).
//
// Prints a grid of small test patches spanning volumetric flow (via speed) on one axis and
// acceleration on the other, all at one fixed pressure-advance value. The user prints the
// grid (re-running at a few PA values), picks the best-looking PA for each flow/acceleration
// cell, and pastes the resulting "PA, flow, accel" rows into the filament's adaptive PA model.
//
// Modeled on CalibrationFlowSpeedDialog.
class CalibrationPressureAdvAdaptiveDialog : public CalibrationAbstractDialog
{
public:
    // results_mode == false builds the generation form (one "Generate" button that creates the
    // grid and closes). results_mode == true builds the results form: the same fields prefilled
    // from the last grid that was generated, plus a button that opens the identify/enter-results
    // grid. The two modes are wired to two separate Calibration menu items.
    CalibrationPressureAdvAdaptiveDialog(GUI_App* app, MainFrame* mainframe, bool results_mode = false)
        : CalibrationAbstractDialog(app, mainframe,
              results_mode ? "Adaptive pressure advance results" : "Adaptive pressure advance calibration")
        , m_results_mode(results_mode)
    {
        create(boost::filesystem::path("calibration") / "adaptive_pressure", "adaptive_pressure.html",
               wxSize(900, 500), m_results_mode);
    }
    virtual ~CalibrationPressureAdvAdaptiveDialog() {}

protected:
    void create_buttons(wxStdDialogButtonSizer* sizer) override;
    void create_geometry(wxCommandEvent& event_args);
    // Opens a modal grid that mirrors the plate layout (one cell per speed/accel box) so the
    // user can read off which printed box is which and type the best-looking PA per cell. The
    // entered values are turned into "PA, flow, accel" model rows (clipboard or written straight
    // into the filament's adaptive PA model).
    void show_results_grid(wxCommandEvent& event_args);

    const bool  m_results_mode = false;
    ComboBox*   cmb_nb_speed = nullptr;
    ComboBox*   cmb_nb_accel = nullptr;
    wxTextCtrl* txt_min_speed = nullptr;
    wxTextCtrl* txt_max_speed = nullptr;
    wxTextCtrl* txt_min_accel = nullptr;
    wxTextCtrl* txt_max_accel = nullptr;
    wxTextCtrl* txt_pa = nullptr;
};

} // namespace GUI
} // namespace Slic3r

#endif
