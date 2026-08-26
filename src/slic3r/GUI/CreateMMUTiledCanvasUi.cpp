#include "CreateMMUTiledCanvas.hpp"

#include "I18N.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Utils.hpp"
#include "GUI.hpp"
#include "GUI_Utils.hpp"
#include "Automation/AutomationFileDialog.hpp"
#include "GUI_ObjectList.hpp"
#include "Plater.hpp"
#include "slic3r/Utils/Http.hpp"
#include "../Utils/Http.hpp"
#include <wx/notebook.h>

#include "MainFrame.hpp"
#include "wxExtensions.hpp"

#include <ctime>
#include <cstdio>
#include <cstdlib>

#include <wx/wx.h>
#include <wx/brush.h>
#include <wx/scrolwin.h>
#include <wx/file.h>
#include <wx/mimetype.h>
#include <wx/odcombo.h>
#include <wx/rawbmp.h>
#include <wx/textctrl.h>
#include <wx/wrapsizer.h>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/ini_parser.hpp>


#include "CreateMMUTiledCanvasRPlaceDialog.hpp"
#include "CreateMMUTiledCanvasShared.hpp"

namespace Slic3r {
namespace GUI {

using namespace CreateMMUTiledCanvasDetail;

CreateMMUTiledCanvas::CreateMMUTiledCanvas(GUI_App* app, MainFrame* mainframe)
    : DPIDialog(NULL, wxID_ANY, wxString(SLIC3R_APP_NAME) + " - " + _L("Creating Mosaic tiled canvas"),
//#if ENABLE_SCROLLABLE
        wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER, "createmmu")
//#else
//    wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
//#endif // ENABLE_SCROLLABLE
{

    this->m_gui_app = app;
    this->m_main_frame = mainframe;
    SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));
    //SetPosition(wxSize(0, 0));

    load_config();

#ifdef _MSW_DARK_MODE
    wxBookCtrlBase* tabs;
    //	if (wxGetApp().dark_mode())
    tabs = new Notebook(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxNB_TOP | wxTAB_TRAVERSAL | wxNB_DEFAULT);
    /*	else {
            tabs = new wxNotebook(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxNB_TOP | wxTAB_TRAVERSAL | wxNB_DEFAULT);
            tabs->SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));
        }*/
#else
    wxNotebook* tabs = new wxNotebook(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxNB_TOP | wxTAB_TRAVERSAL | wxNB_DEFAULT);
    tabs->SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));
#endif

    // fonts
    const wxFont& font = wxGetApp().normal_font();
    //const wxFont& bold_font = wxGetApp().bold_font();
    SetFont(font);

    wxPanel* tab_main = new wxPanel(tabs, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBK_LEFT | wxTAB_TRAVERSAL);
    tabs->AddPage(tab_main, "Preview & settings");
    tab_main->SetFont(font);
    create_main_tab(tab_main);

    wxPanel* tab_color = new wxPanel(tabs, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBK_LEFT | wxTAB_TRAVERSAL);
    tabs->AddPage(tab_color, "Colors");
    tab_color->SetFont(font);
    create_color_tab(tab_color);



    wxGridBagSizer* main_sizer = new wxGridBagSizer(1,1);
    main_sizer->Add(tabs, wxGBPosition(1, 1), wxGBSpan(1, kTabsColumnSpan), wxEXPAND | wxTOP | wxLEFT | wxRIGHT);

    wxButton* bt_create_geometry = new wxButton(this, wxID_APPLY, _(L("Generate")));
    bt_create_geometry->Bind(wxEVT_BUTTON, &CreateMMUTiledCanvas::create_geometry, this);
    wxGetApp().UpdateDarkUI(bt_create_geometry);
    main_sizer->Add(bt_create_geometry, wxGBPosition(kDialogActionRow, 1), wxGBSpan(1, 1), wxEXPAND | wxALIGN_LEFT);

    wxButton* bt_close = new wxButton(this, wxID_CLOSE, _(L("Close")));
    bt_close->Bind(wxEVT_BUTTON, &CreateMMUTiledCanvas::close_me, this);
    SetAffirmativeId(wxID_CLOSE);
    wxGetApp().UpdateDarkUI(bt_close);
    main_sizer->Add(bt_close, wxGBPosition(kDialogActionRow, kCloseButtonColumn), wxGBSpan(1, 1), wxEXPAND | wxALIGN_RIGHT, kCloseButtonBorder);

    main_sizer->AddGrowableCol(kExpandableColumn);
    main_sizer->AddGrowableRow(1);

    SetSizer(main_sizer);
    main_sizer->SetSizeHints(this);
    //wxSize dialog_size(800 * this->scale_factor(), 600 * this->scale_factor());
    wxDisplay display(wxDisplay::GetFromWindow(m_main_frame));
    wxRect screen = display.GetClientArea();
    //dialog_size.x = std::min(dialog_size.x, screen.width - 50);
    //dialog_size.y = std::min(dialog_size.y, screen.height - 50);
    if (screen.width > kMediumScreenMinWidth && screen.height > kMediumScreenMinHeight)
        this->SetSize(wxSize(kMediumDialogWidthPx, kMediumDialogHeightPx));
    else if (screen.width > kLargeScreenMinWidth && screen.height > kLargeScreenMinHeight)
        this->SetSize(wxSize(kLargeDialogWidthPx, kLargeDialogHeightPx));
    else
        Fit();

    //set keyboard shortcut
    wxAcceleratorEntry entries[kDialogShortcutCount];
    //entries[0].Set(wxACCEL_CTRL, (int) 'X', bt_create_geometry->GetId());
    //entries[2].Set(wxACCEL_SHIFT, (int) 'W', wxID_FILE1);
    //entries[0].Set(wxACCEL_CTRL, WXK_ESCAPE, wxID_CLOSE);
    //entries[1].Set(wxACCEL_NORMAL, WXK_F5, bt_create_geometry->GetId()); // wxID_FILE1);
    entries[0].Set(wxACCEL_CTRL, (int) 'G', bt_create_geometry->GetId());
    //entries[3].Set(wxACCEL_CTRL, (int) 'N', bt_new->GetId());
    //entries[4].Set(wxACCEL_CTRL | wxACCEL_SHIFT, (int) 'S', bt_save->GetId());
    //entries[5].Set(wxACCEL_CTRL, (int) 'S', bt_quick_save->GetId());
    entries[1].Set(wxACCEL_CTRL, WXK_F4, wxID_CLOSE);
    this->SetAcceleratorTable(wxAcceleratorTable(kAcceleratorEntryCount, entries));


    this->CenterOnParent();
    //DoCentre(wxVERTICAL);
}


void CreateMMUTiledCanvas::create_main_tab(wxPanel* tab)
{
    wxGridBagSizer* main_sizer = new wxGridBagSizer(1, 1); //(int vgap, int hgap)

    wxBoxSizer* horiSizer = new wxBoxSizer(wxHORIZONTAL);
    wxButton* bt_open = new wxButton(tab, wxID_ANY, _L("Open") + dots);
    wxGetApp().UpdateDarkUI(bt_open);
    horiSizer->Add(bt_open, 0, wxALIGN_CENTER_VERTICAL);
    bt_open->Bind(wxEVT_BUTTON, ([this](wxCommandEvent& e) {
        if (!boost::filesystem::exists(m_filename_ctrl->GetValue().ToStdString())) {
            return;
        }
        //open default program
        std::unique_ptr<wxFileType> ftype(wxTheMimeTypesManager->GetFileTypeFromExtension("png"));
        std::unique_ptr<wxFileType> ftypejpg(wxTheMimeTypesManager->GetFileTypeFromExtension("jpg"));
        if (ftype) {
            boost::filesystem::path path(m_filename_ctrl->GetValue().ToStdString());
            path.make_preferred();
            try {
                wxString command = ftype->GetOpenCommand(path.string());
                wxString commandjpg;
                if (ftypejpg) commandjpg = ftypejpg->GetOpenCommand(path.string());
                if (!command.empty()) {
                    wxExecute(command);
                } else if (!commandjpg.empty()) {
                    wxExecute(commandjpg);
                } else {
#ifdef _WIN32
                    command = "explorer \"" + path.string() + "\"";
                    std::system(command.data());
#elif __APPLE__
                    const char* argv[] = { "open", path.string().data(), nullptr };
                    ::wxExecute(const_cast<char**>(argv), wxEXEC_ASYNC, nullptr);
#else
                    const char* argv[] = { "xdg-open", path.string().data(), nullptr };

                    // Check if we're running in an AppImage container, if so, we need to remove AppImage's env vars,
                    // because they may mess up the environment expected by the file manager.
                    // Mostly this is about LD_LIBRARY_PATH, but we remove a few more too for good measure.
                    if (wxGetEnv("APPIMAGE", nullptr)) {
                        // We're running from AppImage
                        wxEnvVariableHashMap env_vars;
                        wxGetEnvMap(&env_vars);

                        env_vars.erase("APPIMAGE");
                        env_vars.erase("APPDIR");
                        env_vars.erase("LD_LIBRARY_PATH");
                        env_vars.erase("LD_PRELOAD");
                        env_vars.erase("UNION_PRELOAD");

                        wxExecuteEnv exec_env;
                        exec_env.env = std::move(env_vars);

                        wxString owd;
                        if (wxGetEnv("OWD", &owd)) {
                            // This is the original work directory from which the AppImage image was run,
                            // set it as CWD for the child process:
                            exec_env.cwd = std::move(owd);
                        }

                        ::wxExecute(const_cast<char**>(argv), wxEXEC_ASYNC, nullptr, &exec_env);
                    } else {
                        // Looks like we're NOT running from AppImage, we'll make no changes to the environment.
                        ::wxExecute(const_cast<char**>(argv), wxEXEC_ASYNC, nullptr, nullptr);
                    }
#endif
                }
            }
            catch (const Exception&) {}
        }
        }));
    m_filename_ctrl = new wxTextCtrl(tab, wxID_ANY, "");
    m_filename_ctrl->SetMinSize(wxSize(kFilenameFieldMinWidthPx, 0));
    horiSizer->Add(m_filename_ctrl, 0, wxEXPAND | wxALL);
    wxButton* bt_file = new wxButton(tab, wxID_ANY, _L("File") + dots);
    wxGetApp().UpdateDarkUI(bt_file);
    horiSizer->Add(bt_file, 0, wxALIGN_CENTER_VERTICAL);
    bt_file->Bind(wxEVT_BUTTON, ([this](wxCommandEvent& e) {
        FileDialog openFileDialog(this, _("Open png file"), "", "",
            "png files (*.png)|*.png", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (openFileDialog.ShowModal() == wxID_CANCEL)
            return;
        m_config.set_key_value("offset", std::make_unique<ConfigOptionPoint>(Vec2d(0, 0)));

        this->m_filename_ctrl->SetValue(openFileDialog.GetPath());
        this->get_canvas()->loadImage(openFileDialog.GetPath().ToStdString());
        this->get_canvas()->Refresh();
        }));
    wxButton* bt_rplace = new wxButton(tab, wxID_ANY, _L("r/place") + dots);
    bt_rplace->SetToolTip("Download a pixel art image from reddit/place 2022");
    wxGetApp().UpdateDarkUI(bt_rplace);
    horiSizer->Add(bt_rplace, 0, wxALIGN_CENTER_VERTICAL);
    bt_rplace->Bind(wxEVT_BUTTON, ([this](wxCommandEvent& e) {
        GetRPlaceDialog dialog(this, wxID_ANY, _L("Choose your r/place moment"));
        int result = dialog.ShowModal();
        if (result == wxID_OK) {
            GetRPlaceDialog::last_timestamp = dialog.timestamp;
            boost::filesystem::path object_path(Slic3r::data_dir());
            object_path = object_path / "temp" / (std::to_string(dialog.timestamp) + ".png");
            if (!exists(object_path)) {
                get_file_from_web("https://rplace.space/combined/" + std::to_string(dialog.timestamp) + ".png", object_path);
            }

            this->m_filename_ctrl->SetValue(object_path.string());
            this->get_canvas()->loadImage(object_path.string());
            this->get_canvas()->Refresh();
        }
        }));

    //load default file
    {
        boost::filesystem::path object_path(Slic3r::data_dir());
        object_path = object_path / "temp" / "1649112424.png";
        if (!boost::filesystem::exists(object_path)) {
            if (!boost::filesystem::exists(boost::filesystem::path(Slic3r::data_dir()) / "temp"))
                boost::filesystem::create_directories(boost::filesystem::path(Slic3r::data_dir()) / "temp");
            get_file_from_web("https://rplace.space/combined/1649112424.png", object_path);
        }

        this->m_filename_ctrl->SetValue(object_path.string());
    }
    main_sizer->Add(horiSizer, wxGBPosition(1, 1), wxGBSpan(1, kWideColumnSpan), wxEXPAND | wxALL, kGridCellBorder);

    Line line = { "", "" };

    group_size = std::make_shared<ConfigOptionsGroup>(tab, "Options", &m_config);
    group_size->m_on_change = [this](const OptionKeyIdx &opt_key_idx, bool enabled, const boost::any &value) {
        assert(enabled);
        m_dirty = true;
        this->get_canvas()->Refresh();// paintNow();
        this->save_config();
    };
    group_size->title_width = 15;

    //group_size->append_single_option_line(option);

    //line = { L("Size"), "" };
    group_size->append_single_option_line(group_size->create_option_from_def("size"));
    //line.append_option(Option(def, "size"));

    group_size->append_single_option_line(group_size->create_option_from_def("size_px"));
    //line.append_option(Option(def, "size_px"));

    //group_size->append_line(line);

    group_size->append_single_option_line(group_size->create_option_from_def("height"));

    group_size->append_single_option_line(group_size->create_option_from_def("offset"));

    line = { L("Gap"), "" };

    line.append_option(group_size->create_option_from_def(kSeparationXyKey));

    line.append_option(group_size->create_option_from_def(kSeparationZKey));

    group_size->append_line(line);

    group_size->append_single_option_line(group_size->create_option_from_def("bezel"));
    group_size->append_single_option_line(group_size->create_option_from_def("border"));
    

    //group_size->append_single_option_line(group_size->create_option_from_def("bump"));

    group_size->activate([]() {}, wxALIGN_RIGHT);
    group_size->reload_config();
    group_size->update_visibility(comSimple);
    main_sizer->Add(group_size->sizer, wxGBPosition(kSizeOptionsRow, kOptionsColumn), wxGBSpan(1, 1), wxEXPAND | wxALL, kGridCellBorder);
    group_size->parent()->Layout();


    group_colors = std::make_shared<ConfigOptionsGroup>(tab, "Colors", &m_config);
    group_colors->m_on_change = [this](const OptionKeyIdx &opt_key_idx, bool enabled, const boost::any &value) {
        assert(enabled);
    //    if (kExtrudersKey == opt_key_idx.key) {
    //        dynamic_cast<TabPrinter*>(this->m_gui_app->get_tab(Preset::TYPE_PRINTER))->extruders_count_changed(boost::any_cast<int>(value));
    //    }
        m_dirty = true;
        this->get_canvas()->Refresh();// paintNow();
        this->save_config();
    };
    group_colors->title_width = 15;

    group_colors->append_single_option_line(group_colors->create_option_from_def(kSpoolColorsKey));

    //line = { L("Separation"), "" };
    //line.append_option(group_colors->create_option_from_def(kNearColorKey));
    //line.append_option(group_colors->create_option_from_def(kColorComponentKey));
    //group_colors->append_line(line);
    group_colors->append_single_option_line(group_colors->create_option_from_def(kNearColorKey));

    group_colors->append_single_option_line(group_colors->create_option_from_def(kColorComponentKey));

    group_colors->append_single_option_line(group_colors->create_option_from_def(kOrderDarkKey));

    group_colors->append_single_option_line(group_colors->create_option_from_def(kOriginalKey));

    group_colors->append_single_option_line(group_colors->create_option_from_def(kExtrudersKey));

    group_colors->append_single_option_line(group_colors->create_option_from_def(kBackgroundColorKey));


    //group_colors->append_single_option_line(group_colors->get_option(kNearColorKey));

    //#TODO
    //def.label = L("Colors");
    //def.type = coPoint;
    //def.tooltip = L("number of colors.");
    //def.set_default_value(std::make_unique<ConfigOptionPoint>(ConfigOptionPoint{ Vec2d{ 0,0 } }));


    group_colors->activate([]() {}, wxALIGN_RIGHT);
    group_colors->reload_config();
    group_colors->update_visibility(comSimple);
    main_sizer->Add(group_colors->sizer, wxGBPosition(kColorSettingsRow, kOptionsColumn), wxGBSpan(1, 1), wxEXPAND | wxALL, kGridCellBorder);

    //line = { "", "" };
    //line.full_width = 1;
    m_txt_extruder_count = new wxStaticText(tab, wxID_ANY, "test", wxDefaultPosition, wxDefaultSize);
    //line.widget = [this](wxWindow* parent) {
    //    auto sizer = new wxBoxSizer(wxHORIZONTAL);
    //    sizer->Add(nbExtruders, 1, wxEXPAND | wxALL, 0);
    //    return sizer;
    //};
    //group_colors->append_line(line);
    // Extruder count label is refreshed when printer presets change.
    main_sizer->Add(m_txt_extruder_count, wxGBPosition(kExtruderCountRow, kOptionsColumn), wxGBSpan(1, 1), wxEXPAND | wxALL, kGridCellBorder);

    // create canvas
    m_canvas = new BasicDrawPane(tab, &this->m_config);
    m_canvas->parent = this;
    this->get_canvas()->loadImage(m_filename_ctrl->GetValue().ToStdString());
    main_sizer->Add(m_canvas, wxGBPosition(kCanvasStartRow, 1), wxGBSpan(kCanvasRowSpan, 1), wxEXPAND | wxALL, kGridCellBorder);

    // set growable
    main_sizer->AddGrowableCol(1);
    main_sizer->AddGrowableRow(kExtruderCountRow);

    tab->SetSizer(main_sizer);
}


class MywxColourPickerCtrl : public wxColourPickerCtrl {
public:
    static inline CreateMMUTiledCanvas* s_main_app = nullptr;

    MywxColourPickerCtrl(const std::string& col) : wxColourPickerCtrl(s_main_app->m_color_tab, wxID_ANY, wxColour(wxString(col))) { }
    static void save_all_colors() {
        std::vector<std::string> colors;
        for (ColorEntrySpool& ces : s_main_app->m_spools) colors.push_back(wxString::Format(wxT("#%02X%02X%02X"), ces.get_printed_color().Red(), ces.get_printed_color().Green(), ces.get_printed_color().Blue()).ToStdString());
        s_main_app->m_config.set_key_value(kAvailableColorsKey, std::make_unique<ConfigOptionStrings>(colors));
        s_main_app->save_config();
    }

    static MywxColourPickerCtrl* add_color_bt(const std::string& color, wxSizer* sizer) {
        MywxColourPickerCtrl* clr_bt = new MywxColourPickerCtrl(color);
        s_main_app->m_spools.emplace_back(clr_bt);
        clr_bt->Bind(wxEVT_COLOURPICKER_CHANGED, ([](wxCommandEvent& e) {
            s_main_app->refresh_color_conversion(-1, false);
            MywxColourPickerCtrl::save_all_colors();
        }));
        clr_bt->GetPickerCtrl()->Bind(wxEVT_RIGHT_DOWN, ([clr_bt, sizer](wxMouseEvent& e) {
            //remove this color
            for (int i = 0; i < int(s_main_app->m_spools.size()); i++) {
                if (s_main_app->m_spools[i].widget == clr_bt && sizer->GetItem(i)->GetWindow() == clr_bt) {
                    s_main_app->m_spools.erase(s_main_app->m_spools.begin() + i);
                    s_main_app->refresh_color_conversion(i, false);
                    sizer->Detach(i);
                    break;
                }
            }
            //save alls
            MywxColourPickerCtrl::save_all_colors();
            clr_bt->Destroy();
            s_main_app->m_color_tab->GetSizer()->Layout();
            s_main_app->m_color_tab->Refresh();
        }));
        sizer->Add(clr_bt, wxSizerFlags().Left());
        if(s_main_app->m_color_tab->GetSizer())
            s_main_app->refresh_color_conversion(s_main_app->m_spools.size()-1, true);
        return clr_bt;
    }

};

class MywxOwnerDrawnComboBox : public wxOwnerDrawnComboBox, public CmbColorAssoc {
public:
    int idx = 0;
    CreateMMUTiledCanvas* m_main_app;
    wxBoxSizer* m_line;
    bool is_attached = false;

    bool is_auto() override {
        return GetSelection() == 0;
    }
    ColorEntry* get_print_color() override {
        if (is_auto()) {
            return nullptr;
        } else {
            int item = GetSelection();
            return &m_main_app->m_spools[item - 1];
        }
    }
    wxWindow* get_widget() override {
        return this;
    }
    wxSizer* get_sizer() override {
        return m_line;
    }
    bool is_detached() override {
        return !is_attached;
    }

    void detach() override {
        m_main_app->all_lines_conversion->Detach(m_line);
        is_attached = false;
    }
    void attach() override {
        m_main_app->all_lines_conversion->Add(m_line, 0, wxEXPAND);
        is_attached = true;
    }

    void set_color_index(int new_idx) override {
        idx = new_idx;
    }

    MywxOwnerDrawnComboBox(CreateMMUTiledCanvas* main, wxBoxSizer* line, wxString label, int idx) : wxOwnerDrawnComboBox(main->m_color_tab, wxID_ANY, label), m_line(line), m_main_app(main), idx(idx){}

    void OnDrawItem(wxDC& dc, const wxRect& rect, int item, int flags) const override {
        if (item == 0) {
            dc.SetBrush(*wxWHITE_BRUSH);
            dc.DrawRectangle(rect);
            dc.SetBrush(*wxBLACK_BRUSH);
            dc.DrawLabel(_L("Automatic"), rect, wxALIGN_CENTER_HORIZONTAL | wxALIGN_CENTER_VERTICAL);
        } else {
            if (item >= 1 && size_t(item - 1) < m_main_app->m_spools.size()) {
                dc.SetBrush(*wxTheBrushList->FindOrCreateBrush(m_main_app->m_spools[item-1].get_printed_color()));
                dc.DrawRectangle(rect);
            } else {
                dc.SetBrush(*wxBLACK_BRUSH);
                dc.DrawRectangle(rect);
            }
        }
    }
    wxCoord OnMeasureItem(size_t item) const override {
        return wxCoord(kComboItemHeightPx);
    }
    wxCoord OnMeasureItemWidth(size_t item) const override {
        return wxCoord(kComboItemWidthPx);
    }

    void refresh_auto_color() {
        if (is_auto()) {
            wxColour colour_real = m_main_app->m_used_colors[m_main_app->find_extruder(m_main_app->m_pixel_colors[idx].get_printed_color())]->get_printed_color();
            SetBackgroundColour(colour_real);
        } else {
            int item = GetSelection();
            wxColour colour_real = m_main_app->m_spools[item - 1].get_printed_color();
            SetBackgroundColour(colour_real);
        }
    }

};

int CreateMMUTiledCanvas::find_extruder(wxColour color) {
    //int nb_extruders = dynamic_cast<TabPrinter*>(this->m_gui_app->get_tab(Preset::TYPE_PRINTER))->m_extruders_count;
    int nb_extruders = m_config.option<ConfigOptionInt>(kExtrudersKey)->value;;
    const int color_algo = m_config.option<ConfigOptionInt>(kColorComponentKey)->value;
    bool use_near_color = m_config.option<ConfigOptionBool>(kNearColorKey)->value;
    int idx_extruder = 0;
    for (int i = 0; i < int(m_used_colors.size()) && i < nb_extruders; i++) {
        if (m_used_colors[i]->get_printed_color() == color) {
            idx_extruder = 1 + i;
            break;
        } else if (use_near_color && (idx_extruder <= 0 || (color_dist(color_algo, m_used_colors[i]->get_printed_color(), color) < color_dist(color_algo, m_used_colors[idx_extruder - 1]->get_printed_color(), color)))) {
            idx_extruder = 1 + i;
        }
    }
    return idx_extruder;
}

void CreateMMUTiledCanvas::recreate_color_conversion()
{
    auto create_line = [this](ColorEntry* col_entry, int index) {
        wxBoxSizer* line = new wxBoxSizer(wxHORIZONTAL);
        wxStaticText* col_start = new wxStaticText(m_color_tab, wxID_ANY, "");
        col_start->SetSize(kSpinCtrlWidthPx, kSpinCtrlHeightPx);
        col_start->SetMinSize(wxSize(kSpinCtrlWidthPx, kSpinCtrlHeightPx));
        col_start->SetBackgroundColour(col_entry->real_color);
        line->Add(col_start);
        //wxCheckBox* chk_auto = new wxCheckBox(tab, wxID_ANY, "Auto");
        //chk_auto->SetValue(true);
        //line->Add(chk_auto);
        line->Add(new wxStaticText(m_color_tab, wxID_ANY, _L("Choose specific spool color:")));
        
        MywxOwnerDrawnComboBox* clr_set = new MywxOwnerDrawnComboBox(this, line, _L("Automatic"), index);
        clr_set->Append(_L("Automatic"));
        for (size_t i = 0; i < this->m_spools.size(); i++) {
            clr_set->Append(std::to_string(i + 1));
        }
        clr_set->SetSelection(0);
        clr_set->Bind(wxEVT_COMBOBOX, [this, clr_set](wxCommandEvent&) {
            if (clr_set->is_auto()) {
                clr_set->SetBackgroundColour(*wxWHITE);
            } else {
                int id = clr_set->GetSelection();
                clr_set->SetBackgroundColour(this->m_spools[id - 1].get_printed_color());
            }
            clr_set->Refresh();
            });
        line->Add(clr_set);
        col_entry->widget_spool = clr_set;
    };

    std::vector<ColorEntry*> used;
    for (int idx = 0; idx < int(m_pixel_colors.size()); idx++) {
        ColorEntry& c = m_pixel_colors[idx];
        const bool is_used = c.nb_pixels_real > 0;
        if (is_used)
            used.push_back(&c);
        if (is_used && !c.widget_spool)
            create_line(&c, idx);
        // detach all
        if (c.widget_spool && !c.widget_spool->is_detached())
            c.widget_spool->detach();
    }
    
    // order
    std::sort(used.begin(), used.end(), [](ColorEntry* e1, ColorEntry* e2) { return e1->nb_pixels_real > e2->nb_pixels_real; });

    // attach/create
    for (ColorEntry* c : used) {
        c->widget_spool->attach();
    }

}

//refresh color from comboboxes & destroyed links
void CreateMMUTiledCanvas::refresh_color_conversion(int del_idx, bool is_add_not_del) {
    const auto refresh_background = [this](MywxOwnerDrawnComboBox* clr_set) {
        if (clr_set->is_auto())
            clr_set->SetBackgroundColour(*wxWHITE);
        else
            clr_set->SetBackgroundColour(m_spools[clr_set->GetSelection() - 1].get_printed_color());
        clr_set->Refresh();
    };

    for (ColorEntry& c : m_pixel_colors) {
        if (!c.widget_spool || c.widget_spool->is_detached())
            continue;

        auto* clr_set = static_cast<MywxOwnerDrawnComboBox*>(c.widget_spool->get_widget());
        clr_set->m_line->GetChildren().front()->GetWindow()->SetBackgroundColour(c.real_color);
        if (del_idx < 0) {
            refresh_background(clr_set);
            continue;
        }
        if (is_add_not_del) {
            clr_set->Append(std::to_string(this->m_spools.size()));
            continue;
        }
        if (clr_set->is_auto())
            continue;

        const int id = clr_set->GetSelection();
        if (id - 1 == del_idx)
            clr_set->Select(0);
        else if (id - 1 > del_idx)
            clr_set->Select(id - 1);
    }
    if (m_color_tab->GetSizer()) {
        m_color_tab->GetSizer()->Layout();
        m_color_tab->Refresh();
    }
}

void CreateMMUTiledCanvas::create_color_tab(wxPanel* tab)
{
    m_color_tab = tab;
    MywxColourPickerCtrl::s_main_app = this;

    wxGridBagSizer* color_sizer = new wxGridBagSizer(1, 1); //(int vgap, int hgap)

    wxWrapSizer* color_row_sizer = new wxWrapSizer(wxHORIZONTAL);
    wxBoxSizer* first_line = new wxBoxSizer(wxHORIZONTAL);
    wxButton* bt_new_color = new wxButton(tab, wxID_ANY, _L("Add one") + dots);
    wxGetApp().UpdateDarkUI(bt_new_color);
    bt_new_color->Bind(wxEVT_BUTTON, ([this, color_row_sizer](wxCommandEvent& e) {
        MywxColourPickerCtrl::add_color_bt("#000000", color_row_sizer);
        m_color_tab->GetSizer()->Layout();
        m_color_tab->Refresh();
        MywxColourPickerCtrl::save_all_colors();

    }));
    first_line->Add(new wxStaticText(tab, wxID_ANY, _L("Available filament colors in your spools (right clic to delete)")), wxSizerFlags().Left());
    first_line->Add(bt_new_color, wxSizerFlags().Left());
    color_sizer->Add(first_line, wxGBPosition(1, 1), wxGBSpan(1, kWideColumnSpan), wxEXPAND | wxALL, kGridCellBorder);

    //row of available colors
    //group_colors->append_single_option_line(group_colors->get_option(kAvailableColorsKey));
    ConfigOptionStrings* available_colors = m_config.option<ConfigOptionStrings>(kAvailableColorsKey);
    for (int i = 0; i < int(available_colors->size()); i++) {
        MywxColourPickerCtrl::add_color_bt(available_colors->get_at(i), color_row_sizer);
    }
    tab->Refresh();
    color_sizer->Add(color_row_sizer, wxGBPosition(kColorPickerRow, 1), wxGBSpan(1, kWideColumnSpan), wxEXPAND | wxALL, kGridCellBorder);


    color_sizer->Add(new wxStaticText(tab, wxID_ANY, _L("For each needed color, chose the spool color (or let the algorithm choose)")), wxGBPosition(kColorSettingsRow, 1), wxGBSpan(1, kWideColumnSpan), wxEXPAND | wxALL, kGridCellBorder);

    //color convertion
    all_lines_conversion = new wxBoxSizer(wxVERTICAL);
    color_sizer->Add(all_lines_conversion, wxGBPosition(kColorConversionRow, 1), wxGBSpan(1, kWideColumnSpan), wxEXPAND | wxALIGN_TOP | wxALIGN_LEFT, kGridCellBorder);


    color_sizer->AddGrowableCol(kExpandableColumn);
    color_sizer->AddGrowableRow(kColorSettingsRow);

    tab->SetSizer(color_sizer);

}

void CreateMMUTiledCanvas::refresh_description() {
    std::string descrp = "To print all colors, you need " + std::to_string(m_used_colors.size()) + " extruders";
    m_txt_extruder_count->SetLabelText(descrp);
}

void CreateMMUTiledCanvas::on_dpi_changed(const wxRect& suggested_rect)
{
    msw_buttons_rescale(this, em_unit(), { wxID_APPLY, wxID_CLOSE });

    wxSize oldSize = this->GetSize();
    Layout();
    this->SetSize(oldSize.x * this->scale_factor() / this->prev_scale_factor(), oldSize.y * this->scale_factor() / this->prev_scale_factor());
    Refresh();
}

void CreateMMUTiledCanvas::close_me(wxCommandEvent& event_args) {
    //save conf

    this->m_gui_app->change_calibration_dialog(this, nullptr);
    this->Destroy();
}

} // namespace GUI
} // namespace Slic3r
