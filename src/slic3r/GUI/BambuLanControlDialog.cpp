///|/ Copyright (c) 2026 Stan Elston
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "BambuLanControlDialog.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>

#include <wx/button.h>
#include <wx/dcclient.h>
#include <wx/dcmemory.h>
#include <wx/radiobut.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"

#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/Utils/BambuLan.hpp"
#include "slic3r/Utils/BambuLanCamera.hpp"

namespace Slic3r {
namespace GUI {
namespace {

int config_int_at(const DynamicPrintConfig &cfg, const char *key)
{
    const auto *opt = cfg.option<ConfigOptionInts>(key);
    return opt ? opt->get_at(0) : 0;
}

double config_float_at(const DynamicPrintConfig &cfg, const char *key)
{
    const auto *opt = cfg.option<ConfigOptionFloats>(key);
    return opt ? opt->get_at(0) : 0.0;
}

// Bambu's calibration command wants a short diameter token such as "0.4".
std::string nozzle_token(double diameter)
{
    int milli = static_cast<int>(std::lround(diameter * 100.0));
    if (milli < 0)
        milli = 0;
    const int whole = milli / 100;
    const int frac = milli % 100;
    if (frac % 10 == 0)
        return std::to_string(whole) + "." + std::to_string(frac / 10);
    std::string frac_text = std::to_string(frac);
    if (frac < 10)
        frac_text.insert(frac_text.begin(), '0');
    return std::to_string(whole) + "." + frac_text;
}

wxString clip_reply(const wxString &text)
{
    constexpr int kMax = 4000;
    if (text.length() <= kMax)
        return text;
    return text.Left(kMax) + "...";
}

} // namespace

class BambuCameraPanel : public wxPanel
{
public:
    explicit BambuCameraPanel(wxWindow *parent)
        : wxPanel(parent, wxID_ANY)
    {
        SetBackgroundColour(*wxBLACK);
        SetForegroundColour(wxColour(220, 220, 220));
        Bind(wxEVT_PAINT, &BambuCameraPanel::on_paint, this);
        Bind(wxEVT_ERASE_BACKGROUND, [](wxEraseEvent &) {});
    }

    void set_message(const wxString &message)
    {
        m_message = message;
        m_image = wxImage();
        Refresh();
    }

    void set_frame(wxImage image)
    {
        m_image = std::move(image);
        Refresh();
    }

private:
    void on_paint(wxPaintEvent &)
    {
        wxPaintDC dc(this);
        dc.SetBackground(*wxBLACK_BRUSH);
        dc.Clear();
        const wxSize size = GetClientSize();
        if (m_image.IsOk() && m_image.GetWidth() > 0 && m_image.GetHeight() > 0 && size.x > 0 && size.y > 0) {
            wxBitmap bmp(m_image);
            wxMemoryDC mem(bmp);
            dc.StretchBlit(0, 0, size.x, size.y, &mem, 0, 0, bmp.GetWidth(), bmp.GetHeight());
            return;
        }
        dc.SetTextForeground(GetForegroundColour());
        const int text_w = size.x > 16 ? size.x - 16 : 0;
        const int text_h = size.y > 16 ? size.y - 16 : 0;
        dc.DrawLabel(m_message, wxRect(8, 8, text_w, text_h), wxALIGN_LEFT | wxALIGN_TOP);
    }

    wxImage m_image;
    wxString m_message;
};

struct BambuLanControlDialog::FrameSlot
{
    std::mutex mu;
    int width = 0;
    int height = 0;
    std::vector<unsigned char> rgb;
    bool dirty = false;
};

BambuLanControlDialog::BambuLanControlDialog(wxWindow *parent, PresetBundle *bundle)
    : DPIDialog(parent, wxID_ANY, _L("Bambu Lab (Developer LAN)"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_bundle(bundle)
    , m_frames(std::make_shared<FrameSlot>())
{
    const int em = std::max(em_unit(), 8);
    auto *root = new wxBoxSizer(wxVERTICAL);

    auto *intro = new wxStaticText(this, wxID_ANY, _L("Developer LAN controls for this printer. Commands go to the printer on your network."));
    intro->Wrap(em * 70);
    root->Add(intro, 0, wxEXPAND | wxALL, em);

    auto *top = new wxBoxSizer(wxHORIZONTAL);
    m_camera_panel = new BambuCameraPanel(this);
    m_camera_panel->SetMinSize(wxSize(em * 42, em * 24));
    top->Add(m_camera_panel, 1, wxEXPAND | wxRIGHT, em);

    auto *motion = new wxBoxSizer(wxVERTICAL);
    auto *steps = new wxBoxSizer(wxHORIZONTAL);
    auto *step_1 = new wxRadioButton(this, wxID_ANY, _L("1 mm"), wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
    m_step_10 = new wxRadioButton(this, wxID_ANY, _L("10 mm"));
    step_1->SetValue(true);
    steps->Add(step_1, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, em);
    steps->Add(m_step_10, 0, wxALIGN_CENTER_VERTICAL);
    motion->Add(steps, 0, wxBOTTOM, em);

    auto add_move = [&](wxSizer *row, const wxString &label, const char *axis, int dir) {
        auto *btn = new wxButton(this, wxID_ANY, label);
        btn->Bind(wxEVT_BUTTON, [this, axis, dir](wxCommandEvent &) { send_move(axis, dir); });
        m_commands.push_back(btn);
        row->Add(btn, 0, wxRIGHT, 4);
    };
    auto *xy = new wxBoxSizer(wxHORIZONTAL);
    add_move(xy, _L("X −"), "X", -1);
    add_move(xy, _L("X +"), "X", 1);
    add_move(xy, _L("Y −"), "Y", -1);
    add_move(xy, _L("Y +"), "Y", 1);
    motion->Add(xy, 0, wxBOTTOM, 4);
    auto *zrow = new wxBoxSizer(wxHORIZONTAL);
    add_move(zrow, _L("Z −"), "Z", -1);
    add_move(zrow, _L("Z +"), "Z", 1);
    auto *home = new wxButton(this, wxID_ANY, _L("Home"));
    home->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        const std::string seq = next_sequence();
        send(_L("Home"),
             std::string("{\"print\":{\"sequence_id\":\"") + seq + "\",\"command\":\"back_to_center\"}}",
             false);
    });
    m_commands.push_back(home);
    zrow->Add(home, 0);
    motion->Add(zrow, 0);
    top->Add(motion, 0, wxEXPAND);
    root->Add(top, 1, wxEXPAND | wxLEFT | wxRIGHT, em);

    auto *cal_note = new wxStaticText(this, wxID_ANY, _L("Each calibration button starts that routine on the printer."));
    root->Add(cal_note, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, em);
    auto *cal = new wxBoxSizer(wxHORIZONTAL);
    auto add_cal = [&](const wxString &label, int option) {
        auto *btn = new wxButton(this, wxID_ANY, label);
        btn->Bind(wxEVT_BUTTON, [this, label, option](wxCommandEvent &) {
            const std::string seq = next_sequence();
            send(label,
                 std::string("{\"print\":{\"sequence_id\":\"") + seq + "\",\"command\":\"calibration\",\"option\":" + std::to_string(option) + "}}",
                 false);
        });
        m_commands.push_back(btn);
        cal->Add(btn, 0, wxRIGHT, 4);
    };
    // Bit 0 lidar, bit 1 bed leveling, bit 2 vibration compensation.
    add_cal(_L("Start lidar calibration"), 1);
    add_cal(_L("Start bed leveling"), 2);
    add_cal(_L("Start vibration compensation"), 4);
    auto *flow = new wxButton(this, wxID_ANY, _L("Start flow calibration"));
    flow->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { send_flow_calibration(); });
    m_commands.push_back(flow);
    cal->Add(flow, 0);
    root->Add(cal, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, em);

    auto *job = new wxBoxSizer(wxHORIZONTAL);
    auto add_cmd = [&](const wxString &label, const char *command, bool confirm_stop) {
        auto *btn = new wxButton(this, wxID_ANY, label);
        btn->Bind(wxEVT_BUTTON, [this, label, command, confirm_stop](wxCommandEvent &) {
            if (confirm_stop) {
                MessageDialog dlg(this, _L("Stop the print on the printer?"), _L("Bambu Lab (Developer LAN)"), wxYES_NO | wxNO_DEFAULT | wxICON_WARNING);
                if (dlg.ShowModal() != wxID_YES)
                    return;
            }
            const std::string seq = next_sequence();
            send(label,
                 std::string("{\"print\":{\"sequence_id\":\"") + seq + "\",\"command\":\"" + command + "\",\"param\":\"\"}}",
                 false);
        });
        m_commands.push_back(btn);
        job->Add(btn, 0, wxRIGHT, 4);
    };
    auto *light_on = new wxButton(this, wxID_ANY, _L("Chamber light on"));
    light_on->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        const std::string seq = next_sequence();
        send(_L("Chamber light on"),
             std::string("{\"system\":{\"command\":\"ledctrl\",\"led_node\":\"chamber_light\",\"led_mode\":\"on\",\"led_on_time\":500,\"led_off_time\":500,\"loop_times\":0,\"interval_time\":0,\"sequence_id\":\"") + seq + "\"}}",
             false);
    });
    auto *light_off = new wxButton(this, wxID_ANY, _L("Chamber light off"));
    light_off->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        const std::string seq = next_sequence();
        send(_L("Chamber light off"),
             std::string("{\"system\":{\"command\":\"ledctrl\",\"led_node\":\"chamber_light\",\"led_mode\":\"off\",\"led_on_time\":500,\"led_off_time\":500,\"loop_times\":0,\"interval_time\":0,\"sequence_id\":\"") + seq + "\"}}",
             false);
    });
    m_commands.push_back(light_on);
    m_commands.push_back(light_off);
    job->Add(light_on, 0, wxRIGHT, 4);
    job->Add(light_off, 0, wxRIGHT, em);
    add_cmd(_L("Pause"), "pause", false);
    add_cmd(_L("Resume"), "resume", false);
    add_cmd(_L("Stop"), "stop", true);
    root->Add(job, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, em);

    int nozzle_seed = 0;
    int bed_seed = 0;
    if (m_bundle) {
        const DynamicPrintConfig &filament = m_bundle->filaments.get_edited_preset().config;
        const int first_nozzle = config_int_at(filament, "first_layer_temperature");
        const int nozzle = config_int_at(filament, "temperature");
        const int first_bed = config_int_at(filament, "first_layer_bed_temperature");
        const int bed = config_int_at(filament, "bed_temperature");
        nozzle_seed = std::clamp(first_nozzle > 0 ? first_nozzle : nozzle, 0, 300);
        bed_seed = std::clamp(first_bed > 0 ? first_bed : bed, 0, 120);
    }
    auto *temps = new wxBoxSizer(wxHORIZONTAL);
    temps->Add(new wxStaticText(this, wxID_ANY, _L("Nozzle")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    m_nozzle_temp = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(em * 10, -1), wxSP_ARROW_KEYS, 0, 300, nozzle_seed);
    temps->Add(m_nozzle_temp, 0, wxRIGHT, 4);
    auto *set_nozzle = new wxButton(this, wxID_ANY, _L("Set nozzle"));
    set_nozzle->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        const std::string seq = next_sequence();
        const int temp = m_nozzle_temp->GetValue();
        send(wxString::Format(_L("Set nozzle temperature to %d"), temp),
             std::string("{\"print\":{\"sequence_id\":\"") + seq + "\",\"command\":\"set_nozzle_temp\",\"extruder_index\":0,\"target_temp\":" + std::to_string(temp) + "}}",
             false);
    });
    m_commands.push_back(set_nozzle);
    temps->Add(set_nozzle, 0, wxRIGHT, em);
    temps->Add(new wxStaticText(this, wxID_ANY, _L("Bed")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    m_bed_temp = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(em * 10, -1), wxSP_ARROW_KEYS, 0, 120, bed_seed);
    temps->Add(m_bed_temp, 0, wxRIGHT, 4);
    auto *set_bed = new wxButton(this, wxID_ANY, _L("Set bed"));
    set_bed->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        const std::string seq = next_sequence();
        const int temp = m_bed_temp->GetValue();
        send(wxString::Format(_L("Set bed temperature to %d"), temp),
             std::string("{\"print\":{\"sequence_id\":\"") + seq + "\",\"command\":\"set_bed_temp\",\"temp\":" + std::to_string(temp) + "}}",
             false);
    });
    m_commands.push_back(set_bed);
    temps->Add(set_bed, 0);
    root->Add(temps, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, em);

    m_log = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(-1, em * 8), wxTE_MULTILINE | wxTE_READONLY);
    root->Add(m_log, 0, wxEXPAND | wxALL, em);

    auto *close = new wxButton(this, wxID_CLOSE, _L("Close"));
    close->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CLOSE); });
    root->Add(close, 0, wxALIGN_RIGHT | wxLEFT | wxRIGHT | wxBOTTOM, em);

    SetSizer(root);
    Layout();
    Fit();
    CentreOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
    m_camera_panel->SetBackgroundColour(*wxBLACK);
    m_camera_panel->SetForegroundColour(wxColour(220, 220, 220));

    set_commands_enabled(false);

    BambuLanCredentials creds;
    if (m_bundle)
        creds = bambu_lan_credentials(&m_bundle->printers.get_edited_preset().config);
    if (!creds.complete()) {
        note(_L("Enter the printer IP, the LAN access code, and the serial number."));
        m_camera_panel->set_message(_L("Enter the printer IP, the LAN access code, and the serial number."));
        return;
    }

    note(_L("Connecting to ") + wxString::FromUTF8(creds.host) + "...");
    if (!BambuLanCamera::available())
        m_camera_panel->set_message(_L("Video libraries are unavailable."));
    else
        m_camera_panel->set_message(_L("Waiting for the camera…"));

    auto alive = m_alive;
    m_session = std::make_unique<BambuLanSession>(creds.host, creds.access_code, creds.serial);
    m_session->start([alive, this](bool ok, const std::string &, const std::string &error) {
        const wxString error_w = wxString::FromUTF8(error);
        if (!wxTheApp)
            return;
        wxTheApp->CallAfter([alive, this, ok, error_w] {
            if (!alive->load())
                return;
            if (!ok) {
                this->note(error_w.empty() ? _L("Could not connect to the printer.") : error_w);
                return;
            }
            this->note(_L("Connected."));
            this->set_commands_enabled(true);
        });
    });

    if (BambuLanCamera::available()) {
        auto frames = m_frames;
        m_camera = std::make_unique<BambuLanCamera>();
        m_camera->start(creds.host, creds.access_code,
            [frames](BambuLanCamera::Frame frame) {
                std::lock_guard<std::mutex> lock(frames->mu);
                frames->width = frame.width;
                frames->height = frame.height;
                frames->rgb = std::move(frame.rgb);
                frames->dirty = true;
            },
            [alive, this](const std::string &error) {
                const wxString error_w = wxString::FromUTF8(error);
                if (!wxTheApp)
                    return;
                wxTheApp->CallAfter([alive, this, error_w] {
                    if (!alive->load())
                        return;
                    this->m_camera_panel->set_message(error_w.empty() ? _L("Could not open the camera stream.") : error_w);
                });
            });
        m_timer.SetOwner(this);
        m_timer.Bind(wxEVT_TIMER, &BambuLanControlDialog::on_timer, this);
        m_timer.Start(66);
    }
}

BambuLanControlDialog::~BambuLanControlDialog()
{
    m_alive->store(false);
    if (m_timer.IsRunning())
        m_timer.Stop();
    if (m_camera)
        m_camera->stop();
    if (m_session)
        m_session->stop();
}

void BambuLanControlDialog::on_dpi_changed(const wxRect &)
{
    SetFont(wxGetApp().normal_font());
    Layout();
    Fit();
    Refresh();
}

void BambuLanControlDialog::note(const wxString &line)
{
    if (!m_log)
        return;
    const wxString text = clip_reply(line);
    m_log->AppendText(text);
    if (!text.EndsWith("\n"))
        m_log->AppendText("\n");
    if (m_log->GetLastPosition() > 30000)
        m_log->Remove(0, 10000);
}

void BambuLanControlDialog::set_commands_enabled(bool enabled)
{
    for (wxButton *button : m_commands)
        if (button)
            button->Enable(enabled);
}

std::string BambuLanControlDialog::next_sequence()
{
    return std::to_string(m_sequence.fetch_add(1));
}

void BambuLanControlDialog::send(const wxString &label, const std::string &payload, bool await_reply)
{
    if (!m_session) {
        note(_L("Not connected."));
        return;
    }
    note(label);
    std::string needle;
    if (await_reply) {
        const auto key = payload.find("\"sequence_id\":\"");
        if (key != std::string::npos) {
            const size_t begin = key + std::strlen("\"sequence_id\":\"");
            const size_t end = payload.find('"', begin);
            if (end != std::string::npos)
                needle = payload.substr(begin, end - begin);
        }
    }
    auto alive = m_alive;
    m_session->publish(payload, needle, await_reply ? 8000 : 0,
        [alive, this, await_reply](bool ok, const std::string &reply, const std::string &error) {
            const wxString reply_w = wxString::FromUTF8(reply);
            const wxString error_w = wxString::FromUTF8(error);
            if (!wxTheApp)
                return;
            wxTheApp->CallAfter([alive, this, await_reply, ok, reply_w, error_w] {
                if (!alive->load())
                    return;
                if (!reply_w.empty())
                    this->note(reply_w);
                if (!ok)
                    this->note(error_w.empty() ? _L("The printer did not accept the command.") : error_w);
                else if (reply_w.empty())
                    this->note(await_reply ? _L("The printer did not include a reply.") : _L("Sent."));
            });
        });
}

void BambuLanControlDialog::send_move(const char *axis, int dir)
{
    const bool big = m_step_10 && m_step_10->GetValue();
    const std::string seq = next_sequence();
    const wxString label = wxString::FromUTF8(axis) + (dir > 0 ? " +" : " -") + wxString::Format("%d mm", big ? 10 : 1);
    // X1 Carbon is CoreXY and accepts xyz_ctrl. The relative G-code fallback is not sent.
    send(label,
         std::string("{\"print\":{\"sequence_id\":\"") + seq
             + "\",\"command\":\"xyz_ctrl\",\"axis\":\"" + axis
             + "\",\"dir\":" + (dir > 0 ? "1" : "-1")
             + ",\"mode\":" + (big ? "1" : "0") + "}}",
         false);
}

void BambuLanControlDialog::send_flow_calibration()
{
    if (!m_bundle) {
        note(_L("No filament preset is open."));
        return;
    }
    const DynamicPrintConfig &printer = m_bundle->printers.get_edited_preset().config;
    const DynamicPrintConfig &filament = m_bundle->filaments.get_edited_preset().config;
    const double diameter = config_float_at(printer, "nozzle_diameter");
    const int first_nozzle = config_int_at(filament, "first_layer_temperature");
    const int nozzle = config_int_at(filament, "temperature");
    const int first_bed = config_int_at(filament, "first_layer_bed_temperature");
    const int bed = config_int_at(filament, "bed_temperature");
    const int nozzle_temp = first_nozzle > 0 ? first_nozzle : nozzle;
    const int bed_temp = first_bed > 0 ? first_bed : bed;
    const std::string diameter_text = nozzle_token(diameter);
    const std::string seq = next_sequence();
    // mode 0 starts the automatic extrusion calibration. No Bambu filament catalog id is sent.
    // ams_id 255 is the external spool. Temperatures are also at the top level for firmware
    // that reads them there.
    const std::string payload =
        std::string("{\"print\":{\"sequence_id\":\"") + seq
        + "\",\"command\":\"extrusion_cali\",\"nozzle_diameter\":\"" + diameter_text
        + "\",\"mode\":0,\"nozzle_temp\":" + std::to_string(nozzle_temp)
        + ",\"bed_temp\":" + std::to_string(bed_temp)
        + ",\"filaments\":[{\"ams_id\":255,\"slot_id\":0,\"tray_id\":255,\"extruder_id\":0,\"nozzle_temp\":"
        + std::to_string(nozzle_temp) + ",\"bed_temp\":" + std::to_string(bed_temp)
        + ",\"nozzle_diameter\":\"" + diameter_text + "\"}]}}";
    note(_L("Flow calibration on the external spool, mode 0. Nozzle ") + wxString::FromUTF8(diameter_text)
         + wxString::Format(" mm, nozzle temperature %d (first layer %d, temperature %d), bed temperature %d (first layer %d, bed %d).",
                            nozzle_temp, first_nozzle, nozzle, bed_temp, first_bed, bed));
    send(_L("Start flow calibration"), payload, true);
}

void BambuLanControlDialog::on_timer(wxTimerEvent &)
{
    if (!m_alive->load() || !m_frames || !m_camera_panel)
        return;
    int width = 0;
    int height = 0;
    std::vector<unsigned char> rgb;
    {
        std::lock_guard<std::mutex> lock(m_frames->mu);
        if (!m_frames->dirty)
            return;
        width = m_frames->width;
        height = m_frames->height;
        rgb.swap(m_frames->rgb);
        m_frames->dirty = false;
    }
    if (width <= 0 || height <= 0 || rgb.size() < static_cast<size_t>(width) * static_cast<size_t>(height) * 3)
        return;
    wxImage image(width, height);
    if (!image.IsOk() || !image.GetData())
        return;
    std::memcpy(image.GetData(), rgb.data(), static_cast<size_t>(width) * static_cast<size_t>(height) * 3);
    m_camera_panel->set_frame(std::move(image));
}

} // namespace GUI
} // namespace Slic3r
