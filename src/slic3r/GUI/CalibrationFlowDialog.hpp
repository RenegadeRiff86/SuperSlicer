#ifndef slic3r_GUI_CalibrationFlowDialog_hpp_
#define slic3r_GUI_CalibrationFlowDialog_hpp_

#include "CalibrationAbstractDialog.hpp"

#include <vector>

class wxButton;
class wxStaticText;

namespace Slic3r {
namespace GUI {

class CalibrationFlowDialog : public CalibrationAbstractDialog
{

public:
    CalibrationFlowDialog(GUI_App* app, MainFrame* mainframe)
        : CalibrationAbstractDialog(app, mainframe, "Flow calibration")
    {
        create(boost::filesystem::path("calibration") / "filament_flow", "filament_flow.html",
            wxSize(CalibrationConstants::kWideDialogWidthPx, CalibrationConstants::kWideDialogHeightPx));
    }
    virtual ~CalibrationFlowDialog() {}

protected:
    void create_buttons(wxStdDialogButtonSizer* sizer) override;
    void create_geometry(double first_modifier, double step, size_t count);
    void create_geometry_recommended(wxCommandEvent& event_args);
    void create_geometry_fine(wxCommandEvent& event_args);
    void on_tile_clicked(wxCommandEvent& event_args);
    void apply_flow_result(wxCommandEvent& event_args);

private:
    void recalled_flow_parameters();
    void save_flow_parameters(double first_modifier, double step, size_t count, double base_em);
    void select_tile(int index);
    void update_printed_with_label();
    void update_result_label();
    double selected_modifier() const;
    double computed_multiplier() const;
    double live_filament_multiplier() const;

    double m_first_modifier = -0.05;
    double m_step           = 0.01;
    size_t m_count          = 11;
    double m_base_em        = 1.0;
    int    m_selected       = 5;

    std::vector<wxButton*> m_tiles;
    wxStaticText*                m_result_label  = nullptr;
    wxStaticText*                m_apply_status  = nullptr;
    wxStaticText*                m_printed_with  = nullptr;
};

} // namespace GUI
} // namespace Slic3r

#endif
