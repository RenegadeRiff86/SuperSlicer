#ifndef slic3r_GUI_CalibrationRerfDialog_hpp_
#define slic3r_GUI_CalibrationRerfDialog_hpp_

#include "CalibrationAbstractDialog.hpp"

class wxChoice;
class wxStaticText;

namespace Slic3r {
class TriangleMesh;

namespace GUI {

enum class RerfTarget { Normal, FirstLayer };

class CalibrationRerfDialog : public CalibrationAbstractDialog
{
public:
    CalibrationRerfDialog(GUI_App* app, MainFrame* mainframe, RerfTarget target);
    virtual ~CalibrationRerfDialog() {}

protected:
    void create_buttons(wxStdDialogButtonSizer* buttons) override;

private:
    void create_tiles(wxCommandEvent& event_args);
    void use_model(wxCommandEvent& event_args);
    void place_models(const TriangleMesh* model_mesh, const std::string& source_path);
    void apply_exposure(wxCommandEvent& event_args);
    bool photon_mono4_selected() const;
    double current_exposure_s() const;
    const char* exposure_key() const;

    RerfTarget    m_target = RerfTarget::Normal;
    wxChoice*     m_zone = nullptr;
    wxStaticText* m_result = nullptr;
};

} // namespace GUI
} // namespace Slic3r

#endif
