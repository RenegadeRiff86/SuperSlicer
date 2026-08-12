#include "CalibrationCubeDialog.hpp"
#include "I18N.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Utils.hpp"
#include "GLCanvas3D.hpp"
#include "GUI.hpp"
#include "GUI_ObjectList.hpp"
#include "Plater.hpp"
#include <wx/scrolwin.h>
#include <wx/display.h>
#include <wx/file.h>
#include <wx/wupdlock.h>

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
constexpr int kScaleChoiceCount = 4;
constexpr int kDefaultScaleSelection = 1; // "20" mm
constexpr int kScaleFieldWidthPx = 60;
constexpr int kGoalChoiceCount = 2;
constexpr int kGoalFieldWidthPx = 240;
constexpr int kSpacerBeforeGoalPx = 40;
constexpr int kSpacerBetweenButtonsPx = 10;
constexpr float kDefaultCubeSizeMm = 30.f;
constexpr float kXyzCubeSizeMm = 20.f;
constexpr double kDefaultScaleMm = 20.;
constexpr int kEncroachmentPerimeters = 1;
constexpr int kExternalPerimeterPerimeters = 3;
constexpr int kEncroachmentGoalIndex = 1;
constexpr int kExternalPerimeterGoalIndex = 2;
} // namespace

void CalibrationCubeDialog::create_buttons(wxStdDialogButtonSizer* buttons){
    wxString choices_scale[] = { "10", "20", "30", "40" };
    //scale = new wxComboBox(this, wxID_ANY, wxString{ "20" }, wxDefaultPosition, wxDefaultSize, 4, choices_scale);
    scale = new ComboBox(this, wxID_ANY, wxString{"20"}, wxDefaultPosition, wxSize{kScaleFieldWidthPx,-1}, kScaleChoiceCount, choices_scale);
    scale->SetToolTip(_L("You can choose the dimension of the cube."
        " It's a simple scale, you can modify it in the right panel yourself if you prefer. It's just quicker to select it here."));
    scale->SetSelection(kDefaultScaleSelection);
    wxString choices_goal[] = { "Dimensional accuracy (default)" , "infill/perimeters encroachment"/*, "external perimeter overlap"*/};
    //calibrate = new wxComboBox(this, wxID_ANY, _L("Dimensional accuracy (default)"), wxDefaultPosition, wxDefaultSize, 2, choices_goal);
    calibrate = new ComboBox(this, wxID_ANY, _L("Dimensional accuracy (default)"), wxDefaultPosition,  wxSize{kGoalFieldWidthPx,-1}, kGoalChoiceCount, choices_goal);
    calibrate->SetToolTip(_L("Select a goal, this will change settings to increase the effects to search."));
    calibrate->SetSelection(0);
    //calibrate->SetEditable(false);

    buttons->Add(new wxStaticText(this, wxID_ANY, _L("Dimension:") + " "));
    buttons->Add(scale);
    buttons->Add(new wxStaticText(this, wxID_ANY, wxString(" ") + _L("mm")));
    buttons->AddSpacer(kSpacerBeforeGoalPx);
    buttons->Add(new wxStaticText(this, wxID_ANY, _L("Goal:") + " "));
    buttons->Add(calibrate);
    buttons->AddSpacer(kSpacerBeforeGoalPx);

    wxButton* bt = new wxButton(this, wxID_FILE1, _(L("Standard Cube")));
    bt->Bind(wxEVT_BUTTON, &CalibrationCubeDialog::create_geometry_standard, this);
    bt->SetToolTip(_L("Standard cubic xyz cube, with a flat top. Better for infill/perimeters encroachment calibration."));
    buttons->Add(bt);
    buttons->AddSpacer(kSpacerBetweenButtonsPx);
    bt = new wxButton(this, wxID_FILE2, _(L("Voron Cube")));
    bt->Bind(wxEVT_BUTTON, &CalibrationCubeDialog::create_geometry_voron, this);
    bt->SetToolTip(_L("Voron cubic cube with many features inside, with a bearing slot on top. Better to check dimensional accuracy."));
    buttons->Add(bt);
}

void CalibrationCubeDialog::create_geometry(const std::string& calibration_path) {
    Plater* plat = this->main_frame->plater();
    Model& model = plat->model();
    if (!plat->new_project(L("Calibration cube")))
        return;
    // wait for slicing end if needed
    wxGetApp().Yield();
    
    std::unique_ptr<wxWindowUpdateLocker> freeze_gui = std::make_unique<wxWindowUpdateLocker>(this);
    std::vector<size_t> objs_idx = plat->load_files(std::vector<std::string>{
            (boost::filesystem::path(Slic3r::resources_dir()) / "calibration"/"cube"/ calibration_path).string()}, LoadFileOption::LoadModel | LoadFileOption::DontUpdateDirs);

    assert(objs_idx.size() == 1);
    /// --- scale ---
    float cube_size = kDefaultCubeSizeMm;
    if (calibration_path == "xyzCalibration_cube.amf")
        cube_size = kXyzCubeSizeMm;
    double xyzScale = kDefaultScaleMm;
    if (!scale->GetValue().ToDouble(&xyzScale)) {
        xyzScale = kDefaultScaleMm;
    }
    xyzScale = xyzScale / cube_size;
    //do scaling
    model.objects[objs_idx[0]]->scale(xyzScale, xyzScale, xyzScale);


    /// --- translate ---

    /// --- custom config ---
    int idx_goal = calibrate->GetSelection();
    if (idx_goal == kEncroachmentGoalIndex) {
        model.objects[objs_idx[0]]->config.set_key_value("perimeters", std::make_unique<ConfigOptionInt>(kEncroachmentPerimeters));
        model.objects[objs_idx[0]]->config.set_key_value("fill_pattern", std::make_unique<ConfigOptionEnum<InfillPattern>>(ipCubic));
    } else if (idx_goal == kExternalPerimeterGoalIndex) {
        model.objects[objs_idx[0]]->config.set_key_value("perimeters", std::make_unique<ConfigOptionInt>(kExternalPerimeterPerimeters));
        //add full solid layers
    }

    //update plater
    //GLCanvas3D::set_warning_freeze(false);
    plat->changed_objects(objs_idx);
    plat->is_preview_shown();
    //update everything, easier to code.
    ObjectList* obj = this->gui_app->obj_list();
    obj->update_after_undo_redo();
    freeze_gui.reset();

    plat->reslice();
    close_dialog();
}

} // namespace GUI
} // namespace Slic3r
