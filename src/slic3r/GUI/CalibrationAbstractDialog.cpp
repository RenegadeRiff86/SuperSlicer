#include "CalibrationAbstractDialog.hpp"
#include "I18N.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Utils.hpp"
#include "GUI.hpp"
#include "GUI_ObjectList.hpp"
#include "Plater.hpp"

#include <wx/scrolwin.h>
#include <wx/file.h>

#include <algorithm>
#include <cstring>
#include <sstream>

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/system/error_code.hpp>


namespace Slic3r {
namespace GUI {

// Parented to the main frame rather than NULL: an unparented top-level dialog is not a
// descendant of mainframe, and the automation server's snapshot_roots() only picks up
// non-modal dialogs that are, so every calibration dialog was invisible to UI automation.
CalibrationAbstractDialog::CalibrationAbstractDialog(GUI_App* app, MainFrame* mainframe, const std::string& name)
        : DPIDialog(mainframe, wxID_ANY, wxString(SLIC3R_APP_NAME) + " - " + _(L(name)),
#if ENABLE_SCROLLABLE
        wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER
#else
        wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER
#endif // ENABLE_SCROLLABLE
        , "calibration")
    {
        this->gui_app = app;
        this->main_frame = mainframe;
        SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));

        // fonts
        const wxFont& font = wxGetApp().normal_font();
        SetFont(font);

    }

namespace {
boost::filesystem::path resolve_calibration_html_path(
    const boost::filesystem::path& html_path, const std::string& html_name)
{
    const boost::filesystem::path base = boost::filesystem::path(Slic3r::resources_dir()) / html_path;
    wxString language = wxGetApp().current_language_code();
    if (language == "en")
        return base / html_name;

    boost::filesystem::path candidate = base / (into_u8(language) + "_" + html_name);
    if (boost::filesystem::exists(candidate))
        return candidate;

    language = wxGetApp().current_language_code_safe();
    candidate = base / (into_u8(language) + "_" + html_name);
    if (boost::filesystem::exists(candidate))
        return candidate;

    language = language.IsEmpty() ? "en" : language.BeforeFirst('_');
    candidate = base / (into_u8(language) + "_" + html_name);
    if (boost::filesystem::exists(candidate))
        return candidate;

    return base / html_name;
}

bool html_has_jpeg_ref(const std::string& source)
{
    return source.find(".jpg") != std::string::npos
        || source.find(".jpeg") != std::string::npos
        || source.find(".JPG") != std::string::npos
        || source.find(".JPEG") != std::string::npos;
}

// wxHtmlWindow decodes JPEG through whatever libjpeg is already mapped in the
// process. SuperSlicer/wx is built against JPEG 62; GTK may have loaded JPEG 80.
// jpeg_CreateDecompress then aborts: "Wrong JPEG library version: library is 80,
// caller expects 62". Help images ship as PNG; leftover .jpg/.jpeg refs are
// rewritten so a stale resource copy cannot take that path.
void rewrite_jpeg_image_refs(std::string& source)
{
    boost::algorithm::replace_all(source, ".jpeg", ".png");
    boost::algorithm::replace_all(source, ".JPEG", ".png");
    boost::algorithm::replace_all(source, ".jpg", ".png");
    boost::algorithm::replace_all(source, ".JPG", ".png");
}

void load_calibration_html(wxHtmlWindow* viewer, const boost::filesystem::path& full_file_path)
{
    boost::nowide::ifstream file(full_file_path.string().c_str());
    std::string source;
    if (file.good()) {
        std::stringstream buffer;
        buffer << file.rdbuf();
        source = buffer.str();
    }
    const bool had_jpeg = html_has_jpeg_ref(source);
    rewrite_jpeg_image_refs(source);
    if (!had_jpeg || source.empty()) {
        viewer->LoadPage(GUI::from_u8(full_file_path.string()));
        return;
    }
    const boost::filesystem::path tmp =
        full_file_path.parent_path() / (full_file_path.stem().string() + ".ss-htmlwin.html");
    {
        boost::nowide::ofstream out(tmp.string().c_str());
        if (!out.good()) {
            viewer->SetPage(GUI::from_u8(source));
            return;
        }
        out << source;
    }
    viewer->LoadPage(GUI::from_u8(tmp.string()));
    boost::system::error_code ec;
    boost::filesystem::remove(tmp, ec);
}
} // namespace

void CalibrationAbstractDialog::create(boost::filesystem::path html_path, const std::string& html_name, wxSize dialog_size, bool include_close_button){
    // include_close_button is leftover from when these dialogs added their own Close.
    // The window chrome is the closer now.
    static_cast<void>(include_close_button);

    // Create a panel for the entire content
    wxPanel* main_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxFULL_REPAINT_ON_RESIZE);

    // Create the sizer for the panel's content
    wxBoxSizer* panel_sizer = new wxBoxSizer(wxVERTICAL);
    gui_app->app_config->set("autocenter", "1");
    
    //language — prefer localized HTML, then language-family, then English default.
    boost::filesystem::path full_file_path = resolve_calibration_html_path(html_path, html_name);

    // Create the HTML viewer and load the page
    html_viewer = new wxHtmlWindow(main_panel, wxID_ANY,
        wxDefaultPosition, wxDefaultSize, wxHW_SCROLLBAR_AUTO);
    load_calibration_html(html_viewer, full_file_path);
    // when using hyperlink, open the browser.
    html_viewer->Bind(wxEVT_HTML_LINK_CLICKED, [](wxHtmlLinkEvent& evt) {
        wxLaunchDefaultBrowser(evt.GetLinkInfo().GetHref());
    });
    constexpr int kHtmlPanelBorderPx = 5;
    constexpr int kDialogScreenMarginPx = 50;
    panel_sizer->Add(html_viewer, 1, wxEXPAND | wxALL, kHtmlPanelBorderPx);

    // Adjust the dialog size
    wxDisplay display(wxDisplay::GetFromWindow(main_frame));
    wxRect screen = display.GetClientArea();
    dialog_size.x = std::min(int(dialog_size.x * this->scale_factor()), screen.width - kDialogScreenMarginPx);
    dialog_size.y = std::min(int(dialog_size.y * this->scale_factor()), screen.height - kDialogScreenMarginPx);

    // Action buttons only (Generate, Apply, ...). There is no Close button;
    // the window chrome already has one.
    wxStdDialogButtonSizer* buttons = new wxStdDialogButtonSizer();
    create_buttons(buttons);
    if (buttons->GetItemCount() > 0) {
        buttons->Realize();
        panel_sizer->Add(buttons, 0, wxEXPAND | wxALL, kHtmlPanelBorderPx);
    }

    // Set the panel's sizer and add the panel to the dialog
    main_panel->SetSizer(panel_sizer);
    wxBoxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);
    main_sizer->Add(main_panel, 1, wxEXPAND | wxALL, 0);

    SetSizer(main_sizer);
    this->SetSize(dialog_size.x, dialog_size.y);
    gui_app->app_config->set("autocenter", "0");
    main_panel->Lower();// this may break some calibration windows... willl have to call Raise() on other calibration windows that have a panel

    wxGetApp().UpdateDlgDarkUI(this);
    // The surface follows the system window colour, which is dark under a dark theme.
    {
        wxColour window_colour = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);
        if (!window_colour.IsOk())
            window_colour = wxColour(
                CalibrationConstants::kDarkFallbackBgChannel,
                CalibrationConstants::kDarkFallbackBgChannel,
                CalibrationConstants::kDarkFallbackBgChannel);
        html_viewer->SetHTMLBackgroundColour(window_colour);
    }
    apply_html_theme(full_file_path);

    fit_to_content();
}

// wxHtmlWinParser::InitParser hardcodes the text colour to black (wx src/html/winpars.cpp),
// and wxHtml ignores <style> blocks, so a dark background on its own leaves the help page
// as black text on a near-black surface. The page's own <body> attributes are the one lever
// wxHtml honours, so the colour is injected there. LoadPage() has already pointed the virtual
// file system at the page's directory, so re-rendering with SetPage() keeps relative images
// resolving.
void CalibrationAbstractDialog::apply_html_theme(const boost::filesystem::path& full_file_path)
{
    if (!wxGetApp().dark_mode())
        return;

    boost::nowide::ifstream file(full_file_path.string().c_str());
    if (!file.good())
        return;
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string source = buffer.str();
    rewrite_jpeg_image_refs(source);

    const std::string lowered = boost::algorithm::to_lower_copy(source);
    const std::size_t body_tag = lowered.find("<body");
    if (body_tag == std::string::npos)
        return;

    wxColour text = wxGetApp().get_style_role_color("tab.text.default");
    wxColour background = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);
    if (!text.IsOk())
        text = wxColour(
            CalibrationConstants::kDarkFallbackTextChannel,
            CalibrationConstants::kDarkFallbackTextChannel,
            CalibrationConstants::kDarkFallbackTextChannel);
    if (!background.IsOk())
        background = wxColour(
            CalibrationConstants::kDarkFallbackBgChannel,
            CalibrationConstants::kDarkFallbackBgChannel,
            CalibrationConstants::kDarkFallbackBgChannel);
    const std::string attributes =
        " text=\"" + into_u8(text.GetAsString(wxC2S_HTML_SYNTAX)) + "\"" +
        " bgcolor=\"" + into_u8(background.GetAsString(wxC2S_HTML_SYNTAX)) + "\"";
    source.insert(body_tag + std::strlen("<body"), attributes);

    html_viewer->SetPage(from_u8(source));
}

// Best effort: resize the dialog so the whole help page is visible without scrolling.
// The dialog will not grow beyond the main frame's footprint (or the screen work area
// when the frame is minimized or degenerate); content that still does not fit at that
// size keeps its scrollbars.
void CalibrationAbstractDialog::fit_to_content()
{
    Layout();
    wxHtmlContainerCell* content = html_viewer->GetInternalRepresentation();
    if (content == nullptr)
        return;

    const int display_idx = wxDisplay::GetFromWindow(main_frame != nullptr ? static_cast<wxWindow*>(main_frame) : this);
    const wxDisplay display(display_idx != wxNOT_FOUND ? display_idx : 0u);
    const wxRect screen = display.GetClientArea();
    // below this the frame is likely minimized or degenerate; use the screen instead
    constexpr int min_usable_bound_width = 400;
    constexpr int min_usable_bound_height = 300;
    wxRect bound = main_frame != nullptr ? main_frame->GetScreenRect() : screen;
    if (bound.width < min_usable_bound_width || bound.height < min_usable_bound_height)
        bound = screen;

    // Widen first: fixed-width content (e.g. tables) would otherwise force a horizontal
    // scrollbar, and a wider page also rewraps to a shorter one. The controls row is a floor
    // as well as the page - it is laid out horizontally and does not wrap, so a row wider than
    // the help page would otherwise have its right-hand controls clipped by the dialog edge.
    const int decorations_x = GetSize().x - GetClientSize().x;
    const int controls_width = GetSizer()->CalcMin().x + decorations_x;
    const int extra_width = std::max(content->GetWidth() - html_viewer->GetClientSize().x,
                                     controls_width - GetSize().x);
    if (extra_width > 0) {
        const int target_width = std::min(GetSize().x + extra_width, bound.width);
        if (target_width != GetSize().x) {
            SetSize(target_width, GetSize().y);
            Layout();
            content = html_viewer->GetInternalRepresentation();
            if (content == nullptr)
                return;
        }
    }

    // Then fit the height to the page at its final width, growing or shrinking, but
    // never below what the controls row needs nor above the bound.
    const int height_delta = content->GetHeight() - html_viewer->GetClientSize().y;
    if (height_delta != 0) {
        const int decorations = GetSize().y - GetClientSize().y;
        int target_height = GetSize().y + height_delta;
        target_height = std::max(target_height, GetSizer()->CalcMin().y + decorations);
        target_height = std::min(target_height, bound.height);
        if (target_height != GetSize().y)
            SetSize(GetSize().x, target_height);
    }

    // Keep the dialog centered over the bound and fully on-screen.
    constexpr int kCenterDivisor = 2;
    wxPoint pos(bound.x + (bound.width - GetSize().x) / kCenterDivisor,
                bound.y + (bound.height - GetSize().y) / kCenterDivisor);
    pos.x = std::clamp(pos.x, screen.x, std::max(screen.x, screen.x + screen.width - GetSize().x));
    pos.y = std::clamp(pos.y, screen.y, std::max(screen.y, screen.y + screen.height - GetSize().y));
    SetPosition(pos);
}

void CalibrationAbstractDialog::close_me(wxCommandEvent& /*event_args*/)
{
    close_dialog();
}

void CalibrationAbstractDialog::close_dialog()
{
    if (gui_app != nullptr)
        gui_app->change_calibration_dialog(this, nullptr);
    Destroy();
}

void CalibrationAbstractDialog::add_part(ModelObject* model_object, const std::string& input_file, Vec3d move, Vec3d scale, bool rotate) {
    Model model;
    try {
        model = Model::read_from_file(input_file);
    }
    catch (std::exception & e) {
        auto msg = _L("Error!") + " " + input_file + " : " + e.what() + ".";
        show_error(this, msg);
        exit(1);
    }

    for (ModelObject* object : model.objects) {
        Vec3d delta = Vec3d::Zero();
        if (model_object->origin_translation != Vec3d::Zero())
        {
            object->center_around_origin();
            delta = model_object->origin_translation - object->origin_translation;
        }
        for (ModelVolume* volume : object->volumes) {
            volume->translate(delta + move);
            if (scale != Vec3d{ 1,1,1 }) {
                volume->scale(scale);
            }
            if(rotate)
                volume->rotate(Geometry::deg2rad(this->main_frame->plater()->config()->opt_float("init_z_rotate")), Axis::Z);
            ModelVolume* new_volume = model_object->add_volume(*volume);
            new_volume->set_type(ModelVolumeType::MODEL_PART);
            new_volume->name = boost::filesystem::path(input_file).filename().string();

            //volumes_info.push_back(std::make_pair(from_u8(new_volume->name), new_volume->get_mesh_errors_count() > 0));

            // set a default extruder value, since user can't add it manually
            new_volume->config.set_key_value("extruder", std::make_unique<ConfigOptionInt>(0));
            new_volume->config.set_key_value("first_layer_extruder", std::make_unique<ConfigOptionInt>(0));

            //move to bed
            /* const TriangleMesh& hull = new_volume->get_convex_hull();
            float min_z = std::numeric_limits<float>::max();
            for (const stl_facet& facet : hull.stl.facet_start) {
                for (int i = 0; i < 3; ++i)
                    min_z = std::min(min_z, Vec3f::UnitZ().dot(facet.vertex[i]));
            }
            volume->translate(Vec3d(0,0,-min_z));*/
        }
    }
    assert(model.objects.size() == 1);
}

void CalibrationAbstractDialog::on_dpi_changed(const wxRect& suggested_rect)
{
    msw_buttons_rescale(this, em_unit(), { wxID_OK });

    wxSize oldSize = this->GetSize();
    Layout();
    this->SetSize(oldSize.x * this->scale_factor() / this->prev_scale_factor(), oldSize.y * this->scale_factor() / this->prev_scale_factor());
    Refresh();
}

wxPanel* CalibrationAbstractDialog::create_header(wxWindow* parent, const wxFont& bold_font)
{
    wxPanel* panel = new wxPanel(parent);
    wxBoxSizer* sizer = new wxBoxSizer(wxHORIZONTAL);

    wxFont header_font = bold_font;
#ifdef __WXOSX__
    constexpr int kOsxHeaderPointSize = 14;
    header_font.SetPointSize(kOsxHeaderPointSize);
#else
    constexpr int kHeaderPointSizeBoost = 2;
    header_font.SetPointSize(bold_font.GetPointSize() + kHeaderPointSizeBoost);
#endif // __WXOSX__

    sizer->AddStretchSpacer();

    // text
    wxStaticText* text = new wxStaticText(panel, wxID_ANY, _L("Keyboard shortcuts"));
    text->SetFont(header_font);
    sizer->Add(text, 0, wxALIGN_CENTER_VERTICAL);

    sizer->AddStretchSpacer();

    panel->SetSizer(sizer);
    return panel;
}

} // namespace GUI
} // namespace Slic3r
