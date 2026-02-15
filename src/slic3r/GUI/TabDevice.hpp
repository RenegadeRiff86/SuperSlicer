#ifndef slic3r_TabDevice_hpp_
#define slic3r_TabDevice_hpp_

#include <wx/panel.h>
#include <wx/webview.h>

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
    wxWebView*    m_webview{nullptr};
    wxTextCtrl*   m_url_bar{nullptr};
    wxStaticText* m_message{nullptr};
    wxPanel*      m_message_panel{nullptr};

    void on_webview_error(wxWebViewEvent& evt);
    void on_webview_navigated(wxWebViewEvent& evt);
    void show_message(const wxString& msg);
    void show_webview();
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_TabDevice_hpp_
