///|/ Copyright (c) 2026 Stan Elston
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_BambuLan_hpp_
#define slic3r_BambuLan_hpp_

#include <string>
#include <wx/string.h>

#include "PrintHost.hpp"

namespace Slic3r {

class DynamicPrintConfig;

// Bambu Lab printer with LAN Only and Developer Mode turned on.
// MQTT (TLS, port 8883) and implicit FTPS (port 990), user "bblp",
// password is the LAN access code. No Bambu cloud account.
class BambuLan : public PrintHost
{
public:
    explicit BambuLan(DynamicPrintConfig *config);
    ~BambuLan() override = default;

    const char *get_name() const override;
    bool test(wxString &msg) const override;
    wxString get_test_ok_msg() const override;
    bool upload(PrintHostUpload upload_data, ProgressFn progress_fn, ErrorFn error_fn, InfoFn info_fn) const override;
    bool has_auto_discovery() const override { return false; }
    bool can_test() const override { return true; }
    PrintHostPostUploadActions get_post_upload_actions() const override { return PrintHostPostUploadAction::StartPrint; }
    std::string get_host() const override { return m_host; }

private:
    std::string m_host;
    std::string m_access_code;
    std::string m_serial;
};

}

#endif
