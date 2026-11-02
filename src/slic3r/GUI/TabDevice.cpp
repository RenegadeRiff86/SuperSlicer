#include "TabDevice.hpp"

#include <wx/button.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/utils.h>
#if wxUSE_WEBVIEW
#include <wx/webview.h>
#endif

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "libslic3r/PresetBundle.hpp"

namespace Slic3r {
namespace GUI {

namespace {

wxString webview_unavailable_message()
{
#if defined(__WXGTK__)
    return _L("The embedded printer view is unavailable because this build does not have WebKitGTK 4.1 support.\nUse Open in Browser to open the printer interface.");
#elif defined(_WIN32)
    return _L("The embedded printer view is unavailable. The Microsoft Edge WebView2 runtime may not be installed.\nUse Open in Browser to open the printer interface.");
#else
    return _L("The embedded printer view is unavailable in this build.\nUse Open in Browser to open the printer interface.");
#endif
}

} // namespace

TabDevice::TabDevice(wxWindow* parent)
    : wxPanel(parent, wxID_ANY)
{
    auto* main_sizer = new wxBoxSizer(wxVERTICAL);

    // -- Toolbar: URL bar + embedded-view and external-browser controls --
    auto* toolbar_sizer = new wxBoxSizer(wxHORIZONTAL);

    m_url_bar = new wxTextCtrl(this, wxID_ANY, wxEmptyString,
                               wxDefaultPosition, wxDefaultSize, wxTE_READONLY);
    toolbar_sizer->Add(m_url_bar, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);

#if wxUSE_WEBVIEW
    auto* btn_refresh = new wxButton(this, wxID_ANY, _L("Refresh"),
                                     wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    btn_refresh->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_webview)
            m_webview->Reload();
    });
    toolbar_sizer->Add(btn_refresh, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
#endif

    m_open_browser_button = new wxButton(this, wxID_ANY, _L("Open in Browser"),
                                         wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    m_open_browser_button->Disable();
    m_open_browser_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        open_in_browser();
    });
    toolbar_sizer->Add(m_open_browser_button, 0, wxALIGN_CENTER_VERTICAL);

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

#if wxUSE_WEBVIEW
#if defined(_WIN32)
    const wxString backend = wxWebViewBackendEdge;
#elif defined(__WXGTK__)
    const wxString backend = wxWebViewBackendWebKit;
#else
    const wxString backend = wxWebViewBackendDefault;
#endif

    if (wxWebView::IsBackendAvailable(backend))
        m_webview = wxWebView::New(this, wxID_ANY, "about:blank",
                                   wxDefaultPosition, wxDefaultSize, backend);

    if (m_webview) {
        main_sizer->Add(m_webview, 1, wxEXPAND);
        m_webview->Bind(wxEVT_WEBVIEW_ERROR, &TabDevice::on_webview_error, this);
        m_webview->Bind(wxEVT_WEBVIEW_NAVIGATED, &TabDevice::on_webview_navigated, this);
        m_webview->Hide();
    }
#endif

    SetSizerAndFit(main_sizer);
    show_message(_L("Select a physical printer with a host address to view its web interface."));
}

void TabDevice::load_printer_url()
{
    if (!wxGetApp().preset_bundle) {
        clear_printer_url();
        show_message(_L("Printer configuration is not available yet."));
        return;
    }

    DynamicPrintConfig* cfg = wxGetApp().preset_bundle->physical_printers.get_selected_printer_config();
    if (!cfg) {
        clear_printer_url();
        show_message(_L("No physical printer is selected.\nGo to Printer Settings and select a physical printer with a host address."));
        return;
    }

    wxString url = wxString::FromUTF8(cfg->opt_string("print_host"));
    url.Trim(true).Trim(false);
    if (url.empty()) {
        clear_printer_url();
        show_message(_L("The selected physical printer has no host address configured.\nEdit the physical printer and set the hostname or IP address."));
        return;
    }

    if (url.Find("://") == wxNOT_FOUND)
        url.Prepend("http://");

    m_url_bar->SetValue(url);
    m_open_browser_button->Enable();

#if wxUSE_WEBVIEW
    if (m_webview) {
        // Switching to this tab re-enters here. Reloading the same host cancels the
        // in-flight request and WebKit reports that as a connection failure.
        if (m_target_url == url && m_webview->IsShown())
            return;
        m_target_url = url;
        show_webview();
        m_webview->LoadURL(url);
        return;
    }
#endif

    show_message(webview_unavailable_message());
}

void TabDevice::clear_printer_url()
{
    m_url_bar->Clear();
    m_open_browser_button->Disable();
#if wxUSE_WEBVIEW
    m_target_url.clear();
#endif
}

void TabDevice::open_in_browser()
{
    const wxString url = m_url_bar->GetValue();
    if (url.empty()) {
        show_message(_L("Select a physical printer with a host address first."));
        return;
    }

    if (!wxLaunchDefaultBrowser(url))
        show_message(wxString::Format(_L("Could not open the printer interface in the default browser.\n\nURL: %s"), url));
}

#if wxUSE_WEBVIEW
void TabDevice::on_webview_error(wxWebViewEvent& evt)
{
    const wxString url = evt.GetURL();
    const wxString detail = evt.GetString();
    // The view is created on about:blank. Replacing that URL, or calling LoadURL
    // again, cancels the previous request. WebKit reports the destination URL
    // with "Load request cancelled" — that is not a failed connection.
    if (url.empty() || url == "about:blank")
        return;
    if (detail.Lower().Find("cancel") != wxNOT_FOUND)
        return;
#ifdef wxWEBVIEW_NAV_ERR_USER_CANCELLED
    if (evt.GetInt() == wxWEBVIEW_NAV_ERR_USER_CANCELLED)
        return;
#endif
    wxString msg = wxString::Format(
        _L("Failed to connect to printer web interface.\n\nURL: %s\nError: %s\n\nYou can also use Open in Browser."),
        url, detail);
    show_message(msg);
}

void TabDevice::on_webview_navigated(wxWebViewEvent& evt)
{
    const wxString url = evt.GetURL();
    if (url.empty() || url == "about:blank")
        return;
    m_url_bar->SetValue(url);
    m_open_browser_button->Enable();
}
#endif

void TabDevice::show_message(const wxString& msg)
{
    m_message->SetLabel(msg);
    m_message_panel->Show();
#if wxUSE_WEBVIEW
    if (m_webview)
        m_webview->Hide();
#endif
    Layout();
}

#if wxUSE_WEBVIEW
void TabDevice::show_webview()
{
    m_message_panel->Hide();
    if (m_webview)
        m_webview->Show();
    Layout();
}
#endif

} // namespace GUI
} // namespace Slic3r
