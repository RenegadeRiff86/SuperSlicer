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
    CalibrationPressureAdvAdaptiveDialog(GUI_App* app, MainFrame* mainframe)
        : CalibrationAbstractDialog(app, mainframe, "Adaptive pressure advance calibration")
    {
        create(boost::filesystem::path("calibration") / "adaptive_pressure", "adaptive_pressure.html", wxSize(900, 500));
    }
    virtual ~CalibrationPressureAdvAdaptiveDialog() {}

protected:
    void create_buttons(wxStdDialogButtonSizer* sizer) override;
    void create_geometry(wxCommandEvent& event_args);

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
