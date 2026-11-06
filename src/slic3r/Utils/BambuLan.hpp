///|/ Copyright (c) 2026 Stan Elston
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_BambuLan_hpp_
#define slic3r_BambuLan_hpp_

#include <functional>
#include <memory>
#include <string>
#include <wx/string.h>

#include "PrintHost.hpp"

namespace Slic3r {

class DynamicPrintConfig;

// Host, access code, and serial as the Developer LAN connection uses them.
// Host is a bare IP: a pasted scheme or trailing slash is removed.
struct BambuLanCredentials
{
    std::string host;
    std::string access_code;
    std::string serial;

    bool complete() const { return !host.empty() && !access_code.empty() && !serial.empty(); }
};

BambuLanCredentials bambu_lan_credentials(const DynamicPrintConfig *config);

// Print-start AMS choice. Defaults to Orca's external spool: use_ams false,
// ams_mapping [-1], ams_mapping2 ams_id 255 / slot_id 255.
struct BambuAmsPrintChoice
{
    bool use_ams = false;
    std::string ams_mapping = "[-1]";
    std::string ams_mapping2 = "[{\"ams_id\":255,\"slot_id\":255}]";
};

void bambu_lan_set_ams_print_choice(BambuAmsPrintChoice choice);
BambuAmsPrintChoice bambu_lan_ams_print_choice();

// Orca MachineObject::command_ams_change_filament (DeviceManager.cpp).
// sequence_id is supplied by the caller. extruder_id is omitted, matching
// every Orca caller that does not pass one.
std::string bambu_ams_change_filament_json(bool load, const std::string &ams_id, const std::string &slot_id, int old_temp, int new_temp, const std::string &sequence_id);

// One Developer LAN MQTT session. Network calls run on a private thread so the
// dialog can jog without opening a new connection per click. Callbacks run on
// that thread.
class BambuLanSession
{
public:
    using ResultFn = std::function<void(bool ok, const std::string &reply, const std::string &error)>;

    BambuLanSession(std::string host, std::string access_code, std::string serial);
    ~BambuLanSession();

    BambuLanSession(const BambuLanSession &) = delete;
    BambuLanSession &operator=(const BambuLanSession &) = delete;

    void start(ResultFn on_connect);
    void stop();

    // Printer status JSON, including the AMS block. Called on the session thread.
    void set_on_report(std::function<void(const std::string &report)> on_report);

    // Publish JSON to device/<serial>/request. When await_containing is not
    // empty, wait up to timeout_ms for a report that contains it.
    void publish(std::string json, std::string await_containing, int timeout_ms, ResultFn on_done);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

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
