#ifndef slic3r_GUI_CalibrationBedDialog_hpp_
#define slic3r_GUI_CalibrationBedDialog_hpp_

#include "CalibrationAbstractDialog.hpp"

namespace Slic3r { 
namespace GUI {

class CalibrationBedDialog : public CalibrationAbstractDialog
{

public:
    enum class Mode {
        BedLeveling,
        ZOffsetGenerate,
        ZOffsetResult
    };

    CalibrationBedDialog(GUI_App* app, MainFrame* mainframe, Mode mode = Mode::BedLeveling)
        : CalibrationAbstractDialog(app, mainframe,
            mode == Mode::BedLeveling ? "Bed leveling calibration" :
            mode == Mode::ZOffsetGenerate ? "Z offset calibration" : "Apply Z offset calibration result")
        , m_mode(mode)
    {
        create(boost::filesystem::path("calibration") / "bed_leveling",
            mode == Mode::BedLeveling ? "bed_leveling.html" : "z_offset.html");
    }
    virtual ~CalibrationBedDialog() {}
protected:
    void create_buttons(wxStdDialogButtonSizer* sizer) override;
private:
    void create_geometry(wxCommandEvent& event_args);
    void create_z_offset_geometry(wxCommandEvent& event_args);
    void apply_z_offset_result(wxCommandEvent& event_args);
    void recalled_grid_parameters(std::string& center, std::string& step, std::string& layer_height) const;
    double profile_first_layer_height() const;

    const Mode  m_mode;
    wxTextCtrl* txt_z_center = nullptr;
    wxTextCtrl* txt_z_step = nullptr;
    wxTextCtrl* txt_layer_height = nullptr;
    wxTextCtrl* txt_outer_walls = nullptr;
    wxTextCtrl* txt_z_result_pad = nullptr;
    wxTextCtrl* txt_measured_height = nullptr;

};

} // namespace GUI
} // namespace Slic3r

#endif
