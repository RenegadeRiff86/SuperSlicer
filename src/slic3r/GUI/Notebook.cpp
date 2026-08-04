///|/ Copyright (c) Prusa Research 2021 - 2022 Oleksandra Iushchenko @YuSanka, Lukáš Hejl @hejllukas
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "Notebook.hpp"

#include "libslic3r/AppConfig.hpp"

#include "Automation/AutomationIds.hpp"
#include "GUI_App.hpp"
#include "GUI_Tags.hpp"
#include "ThemeMetrics.hpp"
#include "wxExtensions.hpp"

#include <wx/button.h>
#include <wx/dcgraph.h>
#include <wx/dcbuffer.h>
#include <wx/sizer.h>

#include <algorithm>
#include <cmath>
#include <memory>

namespace
{
constexpr const char* ROLE_TAB_BG_DEFAULT = "tab.bg.default";
constexpr const char* ROLE_TAB_BG_HOVER = "tab.bg.hover";
constexpr const char* ROLE_TAB_BG_SELECTED = "tab.bg.selected";
constexpr const char* ROLE_TAB_BORDER_DEFAULT = "tab.border.default";
constexpr const char* ROLE_TAB_BORDER_ACTIVE = "tab.border.active";
constexpr const char* ROLE_TAB_BORDER_FOCUS = "tab.border.focus";
constexpr const char* ROLE_TAB_TEXT_DEFAULT = "tab.text.default";
constexpr const char* ROLE_TAB_TEXT_HOVER = "tab.text.hover";
constexpr const char* ROLE_TAB_TEXT_SELECTED = "tab.text.selected";

void draw_tab_chrome(wxDC& dc, const wxRect& button_rect, const wxRect& client_rect, const wxColour& background, const wxColour& border, const wxColour* focus_ring, int radius, int bottom_line_height)
{
    wxRect chrome = button_rect;
    chrome.SetHeight(client_rect.GetBottom() - button_rect.GetTop() - bottom_line_height + 1);

    dc.SetPen(wxPen(border, 1));
    dc.SetBrush(wxBrush(background));
    dc.DrawRoundedRectangle(chrome, radius);

    if (focus_ring != nullptr) {
        wxRect ring = chrome;
        ring.Inflate(1);
        dc.SetPen(wxPen(*focus_ring, 2));
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawRoundedRectangle(ring, radius + 1);
    }
}
}

wxDEFINE_EVENT(wxCUSTOMEVT_NOTEBOOK_SEL_CHANGED, wxCommandEvent);
wxDEFINE_EVENT(wxCUSTOMEVT_NOTEBOOK_BT_PRESSED, wxCommandEvent);

ButtonsListCtrl::ButtonsListCtrl(wxWindow *parent, bool add_mode_buttons/* = false*/) :
    wxControl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxTAB_TRAVERSAL)
{
#ifdef __WINDOWS__
    SetDoubleBuffered(true);
#endif //__WINDOWS__

    m_btn_margin = Slic3r::GUI::ThemeMetrics::notebook_button_margin(this);
    m_line_margin = Slic3r::GUI::ThemeMetrics::notebook_line_margin(this);

    SetBackgroundStyle(wxBG_STYLE_PAINT);

    m_sizer = new wxBoxSizer(wxHORIZONTAL);
    this->SetSizer(m_sizer);

    // Create the buttons sizer with adjustable gaps
    m_buttons_sizer = new wxFlexGridSizer(1, m_btn_margin, m_btn_margin);
#ifdef __APPLE__
    m_buttons_sizer->SetHGap(m_btn_margin);  // Horizontal gap between buttons
    m_buttons_sizer->SetVGap(m_btn_margin);  // Vertical gap between buttons
#endif
    m_sizer->Add(m_buttons_sizer, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxBOTTOM, m_btn_margin);

    if (add_mode_buttons) {
        m_mode_sizer = new Slic3r::GUI::ModeSizer(this, m_btn_margin, 0);
        m_sizer->AddStretchSpacer(20);  // Adjust the stretch spacer to ensure buttons align correctly
        m_sizer->Add(m_mode_sizer, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT | wxBOTTOM, m_btn_margin);
    }

    this->Bind(wxEVT_PAINT, &ButtonsListCtrl::OnPaint, this);
}

void ButtonsListCtrl::OnPaint(wxPaintEvent&)
{
    auto &app = Slic3r::GUI::wxGetApp();
    app.UpdateDarkUI(this);

    wxAutoBufferedPaintDC buffered_dc(this);
    wxGCDC dc(buffered_dc);
    dc.SetBackground(wxBrush(GetBackgroundColour()));
    dc.Clear();

    const wxRect client_rect(wxPoint(0, 0), GetClientSize());
    const int radius = Slic3r::GUI::ThemeMetrics::radius_sm(this);

    for (int idx = 0; idx < int(m_pageButtons.size()); ++idx) {
        if (ScalableButton *button = m_pageButtons[idx]) {
            const TabVisualState state = get_tab_state(button, idx);
            apply_tab_state(button, state);

            const bool is_focused = state == TabVisualState::Focused;
            const wxColour& background = app.get_style_role_color(
                state == TabVisualState::Selected ? ROLE_TAB_BG_SELECTED :
                state == TabVisualState::Hovered  ? ROLE_TAB_BG_HOVER    :
                                                    ROLE_TAB_BG_DEFAULT);
            const wxColour& border = app.get_style_role_color(
                (state == TabVisualState::Selected || state == TabVisualState::Focused) ? ROLE_TAB_BORDER_ACTIVE : ROLE_TAB_BORDER_DEFAULT);
            const wxColour* focus_ring = is_focused ? &app.get_style_role_color(ROLE_TAB_BORDER_FOCUS) : nullptr;
            draw_tab_chrome(dc, button->GetRect(), client_rect, background, border, focus_ring, radius, m_line_margin);
        }
    }

    if (m_mode_sizer) {
        const auto &mode_btns = m_mode_sizer->get_btns();
        for (Slic3r::GUI::ModeButton* mode_btn : mode_btns) {
            if (!mode_btn)
                continue;
            const bool selected = mode_btn->is_selected();
            const wxColour& bg = app.get_style_role_color(selected ? ROLE_TAB_BG_SELECTED : ROLE_TAB_BG_DEFAULT);
            const wxColour& border = app.get_style_role_color(selected ? ROLE_TAB_BORDER_ACTIVE : ROLE_TAB_BORDER_DEFAULT);
            draw_tab_chrome(dc, mode_btn->GetRect(), client_rect, bg, border, nullptr, radius, m_line_margin);
            apply_tab_state(mode_btn, selected ? TabVisualState::Selected : TabVisualState::Default);
#ifdef __APPLE__
            mode_btn->SetWindowStyle(wxBORDER_SUNKEN | wxBORDER_SIMPLE);
#endif
        }
    }

    dc.SetPen(wxPen(app.get_style_role_color(ROLE_TAB_BORDER_ACTIVE), m_line_margin));
    dc.DrawLine(client_rect.GetLeft(), client_rect.GetBottom() - m_line_margin / 2,
                client_rect.GetRight(), client_rect.GetBottom() - m_line_margin / 2);
}

ButtonsListCtrl::TabVisualState ButtonsListCtrl::get_tab_state(const ScalableButton* button, int idx) const
{
    if (button == m_focused_button)
        return TabVisualState::Focused;
    if (idx == m_selection)
        return TabVisualState::Selected;
    if (button == m_hovered_button)
        return TabVisualState::Hovered;
    return TabVisualState::Default;
}

void ButtonsListCtrl::apply_tab_state(ScalableButton* button, TabVisualState state) const
{
    auto &app = Slic3r::GUI::wxGetApp();
    const wxColour& text_color = app.get_style_role_color(
        state == TabVisualState::Selected ? ROLE_TAB_TEXT_SELECTED :
        state == TabVisualState::Hovered  ? ROLE_TAB_TEXT_HOVER    :
                                            ROLE_TAB_TEXT_DEFAULT);

    button->SetForegroundColour(text_color);
#ifdef __APPLE__
    button->SetWindowStyle(wxBORDER_SUNKEN | wxBORDER_SIMPLE);
#endif
}

void ButtonsListCtrl::UpdateMode()
{
    if (m_mode_sizer)
        m_mode_sizer->SetMode(Slic3r::GUI::wxGetApp().get_mode());
}

void ButtonsListCtrl::Rescale()
{
    m_btn_margin = Slic3r::GUI::ThemeMetrics::notebook_button_margin(this);
    m_line_margin = Slic3r::GUI::ThemeMetrics::notebook_line_margin(this);

    m_buttons_sizer->SetVGap(m_btn_margin);  // Adjust vertical gap here
    m_buttons_sizer->SetHGap(m_btn_margin);  // Adjust horizontal gap here

    m_sizer->Layout();
}

void ButtonsListCtrl::OnColorsChanged()
{
    for (size_t idx = 0; idx < m_pageButtons.size(); ++idx) {
        ScalableButton* btn = m_pageButtons[idx];
        btn->sys_color_changed();
        apply_tab_state(btn, get_tab_state(btn, int(idx)));
    }

    if (m_mode_sizer)
        m_mode_sizer->sys_color_changed();

    m_sizer->Layout();
    Refresh();
}

void ButtonsListCtrl::UpdateModeMarkers()
{
    if (m_mode_sizer)
        m_mode_sizer->update_mode_markers();
}

void ButtonsListCtrl::SetSelection(int sel)
{
    if (m_selection == sel)
        return;
    m_selection = sel;
    Refresh();
}

bool ButtonsListCtrl::InsertPage(size_t n, const wxString& text, bool bSelect/* = false*/, const std::string& bmp_name/* = ""*/, const int bmp_size)
{

    ScalableButton* btn = new ScalableButton(this, wxID_ANY, bmp_name, text, wxDefaultSize, wxDefaultPosition,
#ifdef __APPLE__
        wxBU_EXACTFIT | wxBORDER_SIMPLE | (bmp_name.empty() ? 0 : wxBU_LEFT),
#else
        wxBU_EXACTFIT | wxNO_BORDER | (bmp_name.empty() ? 0 : wxBU_LEFT),
#endif //__APPLE__
        false, bmp_size);

    // The automation API addresses controls by wxWindow::GetName(); every tab
    // button would otherwise report the wx default "button" and be indistinguishable.
    btn->SetName(Slic3r::GUI::AutomationIds::tab(text));

    if (Slic3r::GUI::ThemeMetrics::ui_density_preference() == "compact")
        btn->SetMinSize(wxSize(-1, Slic3r::GUI::ThemeMetrics::notebook_min_height(this)));

    apply_tab_state(btn, bSelect ? TabVisualState::Selected : TabVisualState::Default);

    btn->Bind(wxEVT_ENTER_WINDOW, [this, btn](wxMouseEvent& event) {
        m_hovered_button = btn;
        Refresh();
        event.Skip();
    });
    btn->Bind(wxEVT_LEAVE_WINDOW, [this, btn](wxMouseEvent& event) {
        if (m_hovered_button == btn)
            m_hovered_button = nullptr;
        Refresh();
        event.Skip();
    });
    btn->Bind(wxEVT_SET_FOCUS, [this, btn](wxFocusEvent& event) {
        m_focused_button = btn;
        Refresh();
        event.Skip();
    });
    btn->Bind(wxEVT_KILL_FOCUS, [this, btn](wxFocusEvent& event) {
        if (m_focused_button == btn)
            m_focused_button = nullptr;
        Refresh();
        event.Skip();
    });

    btn->Bind(wxEVT_BUTTON, [this, btn](wxCommandEvent& event) {
        if (auto it = std::find(m_pageButtons.begin(), m_pageButtons.end(), btn); it != m_pageButtons.end()) {
            m_selection = (it - m_pageButtons.begin());
            wxCommandEvent evt = wxCommandEvent(wxCUSTOMEVT_NOTEBOOK_SEL_CHANGED);
            evt.SetId(m_selection);
            wxPostEvent(this->GetParent(), evt);
            Refresh();
        }
    });
    Slic3r::GUI::wxGetApp().UpdateDarkUI(btn);
    m_pageButtons.insert(m_pageButtons.begin() + n, btn);
    m_spacers.insert(m_spacers.begin() + n, false);
    size_t idx = n;
    for (size_t i = 0; i < n; i++) {
        if (m_spacers[i]) idx++;
    }
    m_buttons_sizer->Insert(idx, new wxSizerItem(btn));
    m_buttons_sizer->SetCols(m_buttons_sizer->GetCols() + 1);
    m_sizer->Layout();
    return true;
}

bool ButtonsListCtrl::InsertSpacer(size_t n, int size)
{
    if (m_spacers.size() <= n) {
        assert(false);
        return false; // error
    }
    m_spacers[n] = true;
    size_t idx = n;
    for (size_t i = 0; i < n; i++) {
        if (m_spacers[i]) idx++;
    }
    m_buttons_sizer->Insert(idx, size, 1);
    m_buttons_sizer->SetCols(m_buttons_sizer->GetCols() + 1);
    m_sizer->Layout();
    return true;
}

bool ButtonsListCtrl::HasSpacer(size_t n)
{
    if (m_spacers.size() <= n) {
        assert(false);
        return false; // error
    }
    return m_spacers[n];
}

void ButtonsListCtrl::RemovePage(size_t n)
{
    ScalableButton* btn = m_pageButtons[n];
    if (m_hovered_button == btn)
        m_hovered_button = nullptr;
    if (m_focused_button == btn)
        m_focused_button = nullptr;
    m_pageButtons.erase(m_pageButtons.begin() + n);
    size_t idx = n;
    for (size_t i = 0; i < n; i++) {
        if (m_spacers[i]) idx++;
    }
    if (m_spacers[n])
        m_buttons_sizer->Remove(idx);
    m_buttons_sizer->Remove(idx);
    m_spacers.erase(m_spacers.begin() + n);
    btn->Reparent(nullptr);
    btn->Destroy();
    m_sizer->Layout();
}

void ButtonsListCtrl::RemoveSpacer(size_t n)
{
    if (m_spacers[n]) {
        size_t idx = n;
        for (size_t i = 0; i < n; i++) {
            if (m_spacers[i]) idx++;
        }
        m_buttons_sizer->Remove(idx);
        m_sizer->Layout();
    }
}

bool ButtonsListCtrl::SetPageImage(size_t n, const std::string& bmp_name, const int bmp_size) const
{
    if (n >= m_pageButtons.size())
        return false;
    return m_pageButtons[n]->SetBitmap_(bmp_name, bmp_size);
}

bool ButtonsListCtrl::SetPageImage(size_t n, const wxBitmap& bmp) const
{
    if (n >= m_pageButtons.size())
        return false;
    m_pageButtons[n]->SetBitmap_(bmp);
    return true;
}

void ButtonsListCtrl::SetPageText(size_t n, const wxString& strText)
{
    ScalableButton* btn = m_pageButtons[n];
    btn->SetLabel(strText);
}

wxString ButtonsListCtrl::GetPageText(size_t n) const
{
    ScalableButton* btn = m_pageButtons[n];
    return btn->GetLabel();
}

ScalableButton* ButtonsListCtrl::GetPageButton(size_t n)
{
    if (n < m_pageButtons.size())
        return m_pageButtons[n];
    return nullptr;
}

void Notebook::EmitEventSelChanged(int16_t new_sel) {

    //emit event for changed tab
    if (new_sel >=0 && GetBtnsListCtrl() && this->GetPageCount() > size_t(new_sel)) {
        ScalableButton* btn = GetBtnsListCtrl()->GetPageButton(new_sel);
        if (btn) {
            auto evt = std::make_unique<wxCommandEvent>(wxCUSTOMEVT_NOTEBOOK_BT_PRESSED);
            evt->SetId(new_sel);
            wxQueueEvent(btn, evt.release());
        }
    }
}
 
