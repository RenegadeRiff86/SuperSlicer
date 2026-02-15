#include "TabDevice.hpp"

#include <wx/sizer.h>
#include <wx/button.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/webview.h>

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "libslic3r/PresetBundle.hpp"

namespace Slic3r {
namespace GUI {

TabDevice::TabDevice(wxWindow* parent)
    : wxPanel(parent, wxID_ANY)
{
    auto* main_sizer = new wxBoxSizer(wxVERTICAL);

    // -- Toolbar: URL bar + Refresh button --
    auto* toolbar_sizer = new wxBoxSizer(wxHORIZONTAL);

    m_url_bar = new wxTextCtrl(this, wxID_ANY, wxEmptyString,
                               wxDefaultPosition, wxDefaultSize, wxTE_READONLY);
    toolbar_sizer->Add(m_url_bar, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);

    auto* btn_refresh = new wxButton(this, wxID_ANY, _L("Refresh"),
                                     wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    btn_refresh->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_webview)
            m_webview->Reload();
    });
    toolbar_sizer->Add(btn_refresh, 0, wxALIGN_CENTER_VERTICAL);

    main_sizer->Add(toolbar_sizer, 0, wxEXPAND | wxALL, 4);

    // -- Message panel (shown when no printer / no host / error) --
    m_message_panel = new wxPanel(this, wxID_ANY);
    auto* msg_sizer = new wxBoxSizer(wxVERTICAL);
    m_message = new wxStaticText(m_message_panel, wxID_ANY, wxEmptyString,
                                 wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER_HORIZONTAL);
    msg_sizer->AddStretchSpacer();
    msg_sizer->Add(m_message, 0, wxALIGN_CENTER | wxALL, 20);
    msg_sizer->AddStretchSpacer();
    m_message_panel->SetSizer(msg_sizer);

    main_sizer->Add(m_message_panel, 1, wxEXPAND);

    // -- WebView (Edge backend on Windows, default elsewhere) --
    m_webview = wxWebView::New(this, wxID_ANY, "about:blank"
#ifdef _WIN32
        , wxDefaultPosition, wxDefaultSize, wxWebViewBackendEdge
#endif
    );

    if (m_webview) {
        main_sizer->Add(m_webview, 1, wxEXPAND);

        m_webview->Bind(wxEVT_WEBVIEW_ERROR, &TabDevice::on_webview_error, this);
        m_webview->Bind(wxEVT_WEBVIEW_NAVIGATED, &TabDevice::on_webview_navigated, this);

        // Start hidden until a URL is loaded
        m_webview->Hide();
    }

    SetSizerAndFit(main_sizer);

    // Show initial message
    show_message(_L("Select a physical printer with a host address to view its web interface."));
}

void TabDevice::load_printer_url()
{
    if (!m_webview) {
        show_message(_L("WebView is not available. The Edge WebView2 runtime may not be installed."));
        return;
    }

    DynamicPrintConfig* cfg = wxGetApp().preset_bundle->physical_printers.get_selected_printer_config();
    if (!cfg) {
        show_message(_L("No physical printer is selected.\nGo to Printer Settings and select a physical printer with a host address."));
        return;
    }

    std::string host = cfg->opt_string("print_host");
    if (host.empty()) {
        show_message(_L("The selected physical printer has no host address configured.\nEdit the physical printer and set the hostname or IP address."));
        return;
    }

    // Build URL — add http:// if no scheme is present
    wxString url;
    if (host.find("://") == std::string::npos)
        url = wxString::Format("http://%s", host);
    else
        url = wxString::FromUTF8(host);

    m_url_bar->SetValue(url);
    show_webview();
    m_webview->LoadURL(url);
}

void TabDevice::on_webview_error(wxWebViewEvent& evt)
{
    wxString msg = wxString::Format(
        _L("Failed to connect to printer web interface.\n\nURL: %s\nError: %s"),
        evt.GetURL(), evt.GetString());
    show_message(msg);
}

void TabDevice::on_webview_navigated(wxWebViewEvent& evt)
{
    m_url_bar->SetValue(evt.GetURL());
}

void TabDevice::show_message(const wxString& msg)
{
    m_message->SetLabel(msg);
    m_message_panel->Show();
    if (m_webview)
        m_webview->Hide();
    Layout();
}

void TabDevice::show_webview()
{
    m_message_panel->Hide();
    if (m_webview)
        m_webview->Show();
    Layout();
}

} // namespace GUI
} // namespace Slic3r
