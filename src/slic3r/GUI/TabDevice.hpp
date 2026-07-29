#ifndef slic3r_TabDevice_hpp_
#define slic3r_TabDevice_hpp_

#include <wx/panel.h>
#if wxUSE_WEBVIEW
#include <wx/webview.h>
#endif

class wxButton;
class wxStaticText;
class wxTextCtrl;

namespace Slic3r {
namespace GUI {

class TabDevice : public wxPanel
{
public:
    TabDevice(wxWindow* parent);

    // Load the URL from the currently selected physical printer's print_host setting.
    void load_printer_url();

private:
#if wxUSE_WEBVIEW
    wxWebView* m_webview{nullptr};

    void on_webview_error(wxWebViewEvent& evt);
    void on_webview_navigated(wxWebViewEvent& evt);
    void show_webview();
#endif

    wxTextCtrl*  m_url_bar{nullptr};
    wxButton*    m_open_browser_button{nullptr};
    wxStaticText* m_message{nullptr};
    wxPanel*      m_message_panel{nullptr};

    void clear_printer_url();
    void open_in_browser();
    void show_message(const wxString& msg);
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_TabDevice_hpp_
