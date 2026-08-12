#ifndef slic3r_GUI_CalibrationAbstractDialog_hpp_
#define slic3r_GUI_CalibrationAbstractDialog_hpp_

#include <wx/wx.h>
#include <map>
#include <vector>

#include "Jobs/ProgressIndicator.hpp"
#include "GUI_App.hpp"
#include "GUI_Utils.hpp"
#include "MainFrame.hpp"
#include "wxExtensions.hpp"
#include <wx/html/htmlwin.h>

namespace Slic3r { 
namespace GUI {

// Shared design tokens for calibration dialogs (pad models, UI defaults, scale gates).
namespace CalibrationConstants {
constexpr int    kDefaultDialogWidthPx       = 850;
constexpr int    kDefaultDialogHeightPx      = 550;
constexpr int    kCompactDialogHeightPx      = 400;
constexpr int    kWideDialogWidthPx          = 900;
constexpr int    kWideDialogHeightPx         = 500;
constexpr int    kPressureDialogWidthPx      = 1600;
constexpr int    kPressureDialogHeightPx     = 600;
constexpr int    kComboFieldWidthEm          = 6;
constexpr int    kStepChoiceCount            = 3;
constexpr int    kDefaultStepSelection       = 1;  // middle of 5/10/15 step choices
constexpr int    kDefaultNbTestsSelection    = 4;  // "5" in the 1..6 list
constexpr int    kSpacerAfterStepsPx         = 15;
constexpr int    kSpacerBeforeActionPx       = 40;
constexpr int    kDefaultStepPercent         = 10;
constexpr double kDesignNozzleDiameterMm     = 0.4;
constexpr double kDesignFirstLayerHeightMm   = 0.2;
constexpr double kXyScaleMinFactor           = 0.9;
constexpr double kXyScaleMaxFactor           = 1.2;
constexpr double kHalfTurnDegrees            = 180.0;
constexpr int    kArrangeJobTimeoutMs        = 20000;
constexpr int    kPercentFull                = 100;
constexpr int    kButtonRowSpacerPx          = 20;
} // namespace CalibrationConstants

class CalibrationAbstractDialog : public DPIDialog
{

public:
    CalibrationAbstractDialog(GUI_App* app, MainFrame* mainframe, const std::string& name);
    virtual ~CalibrationAbstractDialog(){ if(gui_app!=nullptr) gui_app->change_calibration_dialog(this, nullptr);}
    
private:
    wxPanel* create_header(wxWindow* parent, const wxFont& bold_font);
protected:
    void create(boost::filesystem::path html_path, const std::string& html_name,
        wxSize dialogsize = wxSize(CalibrationConstants::kDefaultDialogWidthPx, CalibrationConstants::kDefaultDialogHeightPx),
        bool include_close_button = false);
    void apply_html_theme(const boost::filesystem::path& full_file_path);
    void fit_to_content();
    virtual void create_buttons(wxStdDialogButtonSizer*) = 0;
    void on_dpi_changed(const wxRect& suggested_rect) override;
    void close_me(wxCommandEvent& event_args);
    void close_dialog();
    void add_part(ModelObject* model_object, const std::string& input_file, Vec3d move, Vec3d scale = Vec3d{ 1,1,1 }, bool rotate = true);

    wxHtmlWindow* html_viewer;
    MainFrame* main_frame;
    GUI_App* gui_app;

};


class HtmlDialog : public CalibrationAbstractDialog
{

public:
    HtmlDialog(GUI_App* app, MainFrame* mainframe, const std::string& title, const std::string& html_path, const std::string& html_name) : CalibrationAbstractDialog(app, mainframe, title) { create(html_path, html_name); }
    virtual ~HtmlDialog() {}
protected:
    void create_buttons(wxStdDialogButtonSizer* sizer) override {}

};

class ProgressIndicatorStub : public ProgressIndicator {
public:

    virtual ~ProgressIndicatorStub() override = default;

    virtual void set_range(int range) override {}
    virtual void set_cancel_callback(CancelFn = CancelFn()) override {}
    virtual void set_progress(int pr) override {}
    virtual void set_status_text(const char*) override {}
    virtual int  get_range() const override { return 0; }
};

} // namespace GUI
} // namespace Slic3r

#endif
