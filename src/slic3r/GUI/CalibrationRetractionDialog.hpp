#ifndef slic3r_GUI_CalibrationRetractionDialog_hpp_
#define slic3r_GUI_CalibrationRetractionDialog_hpp_

#include "CalibrationAbstractDialog.hpp"
#include "Widgets/ComboBox.hpp"

namespace Slic3r { 
namespace GUI {

class CalibrationRetractionDialog : public CalibrationAbstractDialog
{

public:
    CalibrationRetractionDialog(GUI_App* app, MainFrame* mainframe) : CalibrationAbstractDialog(app, mainframe, "Retraction calibration") { create(boost::filesystem::path("calibration") / "retraction", "retraction.html", wxSize(900, 500));  }
    virtual ~CalibrationRetractionDialog() {}
    
protected:
    void create_buttons(wxStdDialogButtonSizer* sizer) override;
    void remove_slowdown(wxCommandEvent& event_args);
    void create_geometry(wxCommandEvent& event_args);

    ComboBox* steps = nullptr;
    ComboBox* nb_steps = nullptr;
    //wxComboBox* start_step;
    wxTextCtrl* temp_start = nullptr;
    ComboBox* decr_temp = nullptr;
};

} // namespace GUI
} // namespace Slic3r

#endif
