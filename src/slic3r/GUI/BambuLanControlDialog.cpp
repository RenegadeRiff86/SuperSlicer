///|/ Copyright (c) 2026 Stan Elston
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "BambuLanControlDialog.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>

#include <wx/button.h>
#include <wx/dcclient.h>
#include <wx/dcmemory.h>
#include <wx/radiobut.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "Tab.hpp"

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

size_t skip_ws(const std::string &s, size_t i)
{
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
        ++i;
    return i;
}

// Index of the value after a JSON key, or npos. The key must be a quoted name.
size_t json_value_at(const std::string &s, const char *key, size_t from)
{
    const std::string pat = std::string("\"") + key + "\"";
    size_t i = from;
    while (i < s.size()) {
        i = s.find(pat, i);
        if (i == std::string::npos)
            return std::string::npos;
        const size_t j = skip_ws(s, i + pat.size());
        if (j < s.size() && s[j] == ':')
            return skip_ws(s, j + 1);
        i += pat.size();
    }
    return std::string::npos;
}

size_t json_span_end(const std::string &s, size_t open)
{
    if (open >= s.size())
        return std::string::npos;
    int depth = 0;
    bool in_str = false;
    bool esc = false;
    for (size_t i = open; i < s.size(); ++i) {
        const char c = s[i];
        if (in_str) {
            if (esc)
                esc = false;
            else if (c == '\\')
                esc = true;
            else if (c == '"')
                in_str = false;
            continue;
        }
        if (c == '"') {
            in_str = true;
            continue;
        }
        if (c == '{' || c == '[')
            ++depth;
        else if (c == '}' || c == ']') {
            --depth;
            if (depth == 0)
                return i;
        }
    }
    return std::string::npos;
}

std::string json_field(const std::string &obj, const char *key)
{
    const size_t v = json_value_at(obj, key, 0);
    if (v == std::string::npos || v >= obj.size())
        return {};
    if (obj[v] == '"') {
        std::string out;
        for (size_t i = v + 1; i < obj.size(); ++i) {
            if (obj[i] == '\\' && i + 1 < obj.size()) {
                out.push_back(obj[i + 1]);
                ++i;
                continue;
            }
            if (obj[i] == '"')
                break;
            out.push_back(obj[i]);
        }
        return out;
    }
    if (obj[v] == '{' || obj[v] == '[')
        return {};
    size_t e = v;
    while (e < obj.size() && obj[e] != ',' && obj[e] != '}' && obj[e] != ']'
           && !std::isspace(static_cast<unsigned char>(obj[e])))
        ++e;
    return obj.substr(v, e - v);
}

bool json_number(const std::string &text, double &out)
{
    if (text.empty())
        return false;
    char *end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end == text.c_str())
        return false;
    out = value;
    return true;
}

int json_int(const std::string &text)
{
    double value = 0;
    if (!json_number(text, value))
        return 0;
    return static_cast<int>(std::lround(value));
}

std::vector<std::string> json_objects(const std::string &s, size_t open)
{
    std::vector<std::string> out;
    if (open >= s.size() || s[open] != '[')
        return out;
    const size_t end = json_span_end(s, open);
    if (end == std::string::npos)
        return out;
    for (size_t i = open + 1; i < end;) {
        i = skip_ws(s, i);
        if (i >= end)
            break;
        if (s[i] == '{') {
            const size_t obj_end = json_span_end(s, i);
            if (obj_end == std::string::npos || obj_end > end)
                break;
            out.emplace_back(s, i, obj_end - i + 1);
            i = obj_end + 1;
        } else
            ++i;
    }
    return out;
}

void fill_tray(BambuLanControlDialog::AmsSlot &slot, const std::string &tray)
{
    const std::string type = json_field(tray, "tray_type");
    if (!type.empty())
        slot.material = type;
    const std::string color = json_field(tray, "tray_color");
    if (!color.empty())
        slot.color = color;
    slot.nozzle_min = json_int(json_field(tray, "nozzle_temp_min"));
    slot.nozzle_max = json_int(json_field(tray, "nozzle_temp_max"));
    slot.bed_temp = json_int(json_field(tray, "bed_temp"));
    double k = 0;
    if (json_number(json_field(tray, "k"), k)) {
        slot.has_k = true;
        slot.k = k;
    }
    double k_value = 0;
    if (json_number(json_field(tray, "k_value"), k_value)) {
        slot.has_k = true;
        slot.k = k_value;
    }
    // flow_ratio is Orca's flow-calibration result. Tray "n" is the K coefficient, not the flow ratio.
    double flow = 0;
    if (json_number(json_field(tray, "flow_ratio"), flow)) {
        slot.has_flow = true;
        slot.flow = flow;
    }
}

std::string slot_picture(const BambuLanControlDialog::AmsSlot &slot)
{
    std::ostringstream os;
    os << (slot.external ? 1 : 0) << '|' << slot.ams_id << '|' << slot.slot_id << '|'
       << slot.material << '|' << slot.color << '|' << slot.humidity << '|'
       << slot.temperature << '|' << slot.nozzle_min << '|' << slot.nozzle_max << '|'
       << slot.bed_temp << '|' << (slot.has_k ? 1 : 0) << '|' << slot.k << '|'
       << (slot.has_flow ? 1 : 0) << '|' << slot.flow << ';';
    return os.str();
}

std::string shape_of(const std::vector<BambuLanControlDialog::AmsSlot> &slots)
{
    std::string shape;
    for (const auto &slot : slots) {
        shape += slot.external ? "E" : "A";
        shape += std::to_string(slot.ams_id);
        shape += ':';
        shape += std::to_string(slot.slot_id);
        shape += ';';
    }
    return shape;
}

// DevFilaSystemParser::ParseV1_0 reads print.ams.ams[] and vt_tray / vir_slot.
bool parse_ams_slots(const std::string &report, std::vector<BambuLanControlDialog::AmsSlot> &slots)
{
    if (report.find("\"ams\"") == std::string::npos
        && report.find("\"vt_tray\"") == std::string::npos
        && report.find("\"vir_slot\"") == std::string::npos)
        return false;

    bool found_units = false;
    bool found_external = false;
    BambuLanControlDialog::AmsSlot external;
    external.external = true;
    external.ams_id = 255;
    external.slot_id = 0;
    std::vector<BambuLanControlDialog::AmsSlot> parsed;

    size_t ams_value = json_value_at(report, "ams", 0);
    if (ams_value != std::string::npos && ams_value < report.size() && report[ams_value] == '{') {
        const size_t inner = json_value_at(report, "ams", ams_value + 1);
        if (inner != std::string::npos && inner < report.size() && report[inner] == '[')
            ams_value = inner;
    }
    if (ams_value != std::string::npos && ams_value < report.size() && report[ams_value] == '[') {
        found_units = true;
        for (const std::string &unit : json_objects(report, ams_value)) {
            const int ams_id = json_int(json_field(unit, "id"));
            std::string humidity = json_field(unit, "humidity");
            const std::string humidity_raw = json_field(unit, "humidity_raw");
            if (!humidity_raw.empty()) {
                if (!humidity.empty())
                    humidity += "  ";
                humidity += humidity_raw + "%";
            }
            const std::string temperature = json_field(unit, "temp");
            const size_t tray_at = json_value_at(unit, "tray", 0);
            if (tray_at == std::string::npos || tray_at >= unit.size() || unit[tray_at] != '[')
                continue;
            for (const std::string &tray : json_objects(unit, tray_at)) {
                if (json_field(tray, "id").empty())
                    continue;
                BambuLanControlDialog::AmsSlot slot;
                slot.external = false;
                slot.ams_id = ams_id;
                slot.slot_id = json_int(json_field(tray, "id"));
                slot.humidity = humidity;
                slot.temperature = temperature;
                fill_tray(slot, tray);
                parsed.push_back(slot);
            }
        }
    }

    const size_t vir = json_value_at(report, "vir_slot", 0);
    if (vir != std::string::npos && vir < report.size() && report[vir] == '[') {
        const std::vector<std::string> trays = json_objects(report, vir);
        size_t chosen = trays.empty() ? std::string::npos : 0;
        for (size_t i = 0; i < trays.size(); ++i) {
            if (json_int(json_field(trays[i], "id")) == 255) {
                chosen = i;
                break;
            }
        }
        if (chosen != std::string::npos) {
            fill_tray(external, trays[chosen]);
            found_external = true;
        }
    } else {
        const size_t vt = json_value_at(report, "vt_tray", 0);
        if (vt != std::string::npos && vt < report.size() && report[vt] == '{') {
            const size_t end = json_span_end(report, vt);
            if (end != std::string::npos) {
                fill_tray(external, report.substr(vt, end - vt + 1));
                found_external = true;
            }
        }
    }
    if (!found_units && !found_external)
        return false;
    // AMS units first, external spool last. That is Orca's row. The external
    // spool stays the selected default until the user picks a slot.
    parsed.push_back(external);
    slots = std::move(parsed);
    return true;
}

bool edited_host_is_bambu(PresetBundle *bundle)
{
    if (!bundle)
        return false;
    const auto *host = bundle->printers.get_edited_preset().config.option<ConfigOptionEnum<PrintHostType>>("host_type");
    return host && host->value == htBambuLan;
}

void set_int_opt(DynamicPrintConfig &cfg, const char *key, int value)
{
    if (auto *opt = dynamic_cast<ConfigOptionInts *>(cfg.optptr(key, true)))
        opt->set_at(value, 0);
}

void set_float_opt(DynamicPrintConfig &cfg, const char *key, double value)
{
    if (auto *opt = dynamic_cast<ConfigOptionFloats *>(cfg.optptr(key, true)))
        opt->set_at(value, 0);
}

void set_bool_opt(DynamicPrintConfig &cfg, const char *key, bool value)
{
    if (auto *opt = dynamic_cast<ConfigOptionBools *>(cfg.optptr(key, true)))
        opt->set_at(static_cast<unsigned char>(value ? 1 : 0), 0);
}

void set_string_opt(DynamicPrintConfig &cfg, const char *key, const std::string &value)
{
    if (auto *opt = dynamic_cast<ConfigOptionStrings *>(cfg.optptr(key, true)))
        opt->set_at(value, 0);
}

void mark_filament_dirty(PresetBundle *bundle)
{
    if (!bundle)
        return;
    bundle->filaments.update_dirty();
    if (Tab *tab = wxGetApp().get_tab(Preset::TYPE_FFF_FILAMENT))
        tab->update_dirty();
}

wxString slot_label(const BambuLanControlDialog::AmsSlot &slot)
{
    if (slot.external) {
        wxString label = _L("External spool");
        if (!slot.material.empty())
            label += "  " + wxString::FromUTF8(slot.material);
        return label;
    }
    wxString label = wxString::Format("Slot %d", slot.slot_id);
    if (!slot.material.empty())
        label += "  " + wxString::FromUTF8(slot.material);
    return label;
}

wxString unit_caption(const BambuLanControlDialog::AmsSlot &slot)
{
    wxString text = wxString::Format("AMS %d", slot.ams_id);
    if (!slot.humidity.empty())
        text += "    humidity " + wxString::FromUTF8(slot.humidity);
    if (!slot.temperature.empty())
        text += "    " + wxString::FromUTF8(slot.temperature) + " °C";
    return text;
}

wxColour color_from_tray(const std::string &text)
{
    std::string hex;
    for (char c : text) {
        if (std::isxdigit(static_cast<unsigned char>(c)))
            hex.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (hex.size() < 6)
        return wxColour();
    const auto channel = [&](size_t i) {
        return static_cast<unsigned char>(std::strtoul(hex.substr(i, 2).c_str(), nullptr, 16));
    };
    return wxColour(channel(0), channel(2), channel(4));
}

void paint_slot(wxRadioButton *radio, const BambuLanControlDialog::AmsSlot &slot)
{
    if (!radio)
        return;
    radio->SetLabel(slot_label(slot));
    const wxColour color = color_from_tray(slot.color);
    if (!color.IsOk()) {
        if (wxWindow *parent = radio->GetParent()) {
            radio->SetBackgroundColour(parent->GetBackgroundColour());
            radio->SetForegroundColour(parent->GetForegroundColour());
        }
        return;
    }
    radio->SetBackgroundColour(color);
    const int luma = (color.Red() * 30 + color.Green() * 59 + color.Blue() * 11) / 100;
    radio->SetForegroundColour(luma < 140 ? *wxWHITE : *wxBLACK);
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
    m_nozzle_temp = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(em * 8, -1), wxSP_ARROW_KEYS, 0, 300, nozzle_seed);
    temps->Add(m_nozzle_temp, 0, wxRIGHT, 4);
    auto *set_nozzle = new wxButton(this, wxID_ANY, _L("Set"));
    set_nozzle->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        save_temperature(true);
        const std::string seq = next_sequence();
        const int temp = m_nozzle_temp->GetValue();
        send(wxString::Format(_L("Set nozzle temperature to %d"), temp),
             std::string("{\"print\":{\"sequence_id\":\"") + seq + "\",\"command\":\"set_nozzle_temp\",\"extruder_index\":0,\"target_temp\":" + std::to_string(temp) + "}}",
             false);
    });
    m_commands.push_back(set_nozzle);
    temps->Add(set_nozzle, 0, wxRIGHT, em);
    temps->Add(new wxStaticText(this, wxID_ANY, _L("Bed")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    m_bed_temp = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(em * 8, -1), wxSP_ARROW_KEYS, 0, 120, bed_seed);
    temps->Add(m_bed_temp, 0, wxRIGHT, 4);
    auto *set_bed = new wxButton(this, wxID_ANY, _L("Set"));
    set_bed->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        save_temperature(false);
        const std::string seq = next_sequence();
        const int temp = m_bed_temp->GetValue();
        send(wxString::Format(_L("Set bed temperature to %d"), temp),
             std::string("{\"print\":{\"sequence_id\":\"") + seq + "\",\"command\":\"set_bed_temp\",\"temp\":" + std::to_string(temp) + "}}",
             false);
    });
    m_commands.push_back(set_bed);
    temps->Add(set_bed, 0);
    root->Add(temps, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, em);

    // Orca's AMS row: one group per unit, humidity and temperature beside the
    // colored slots, and the external spool after the units. Load and Unload
    // stay enabled whenever the printer is connected. Nothing here is hidden by a mode.
    auto *ams_box = new wxStaticBoxSizer(wxVERTICAL, this, _L("AMS"));
    m_ams_list = new wxBoxSizer(wxVERTICAL);
    ams_box->Add(m_ams_list, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 4);
    auto *ams_buttons = new wxBoxSizer(wxHORIZONTAL);
    auto *load = new wxButton(this, wxID_ANY, _L("Load"));
    load->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { send_ams_filament(true); });
    auto *unload = new wxButton(this, wxID_ANY, _L("Unload"));
    unload->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { send_ams_filament(false); });
    auto *save_slot = new wxButton(this, wxID_ANY, _L("Save slot to filament"));
    save_slot->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { save_selected_slot(); });
    m_commands.push_back(load);
    m_commands.push_back(unload);
    ams_buttons->Add(load, 0, wxRIGHT, 4);
    ams_buttons->Add(unload, 0, wxRIGHT, 4);
    ams_buttons->Add(save_slot, 0);
    ams_box->Add(ams_buttons, 0, wxLEFT | wxRIGHT | wxTOP, 4);
    ams_box->Add(new wxStaticText(this, wxID_ANY, _L("The next print starts from the selected spool.")), 0, wxALL, 4);
    root->Add(ams_box, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, em);
    AmsSlot external_slot;
    external_slot.external = true;
    external_slot.ams_id = 255;
    m_ams_slots.push_back(external_slot);

    m_log = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(-1, em * 8), wxTE_MULTILINE | wxTE_READONLY);
    root->Add(m_log, 0, wxEXPAND | wxALL, em);

    auto *close = new wxButton(this, wxID_CLOSE, _L("Close"));
    close->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CLOSE); });
    root->Add(close, 0, wxALIGN_RIGHT | wxLEFT | wxRIGHT | wxBOTTOM, em);

    SetSizer(root);
    rebuild_ams();
    publish_ams_choice();
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
    m_session->set_on_report([alive, this](const std::string &report) {
        if (!wxTheApp)
            return;
        wxTheApp->CallAfter([alive, this, report] {
            if (!alive->load())
                return;
            this->apply_ams_report(report);
        });
    });
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
                if (ok && await_reply)
                    this->store_flow_reply(reply_w);
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
    AmsSlot slot;
    slot.external = true;
    slot.ams_id = 255;
    if (m_ams_selected >= 0 && m_ams_selected < static_cast<int>(m_ams_slots.size()))
        slot = m_ams_slots[m_ams_selected];
    const int ams_id = slot.external ? 255 : slot.ams_id;
    const int slot_id = slot.external ? 0 : slot.slot_id;
    const int tray_id = slot.external ? 255 : (ams_id < 16 ? ams_id * 4 + slot_id : slot_id);
    // mode 0 starts the automatic extrusion calibration. No Bambu filament catalog id is sent.
    // The selected spool is the one Orca would pass. Temperatures are also at the top level.
    const std::string payload =
        std::string("{\"print\":{\"sequence_id\":\"") + seq
        + "\",\"command\":\"extrusion_cali\",\"nozzle_diameter\":\"" + diameter_text
        + "\",\"mode\":0,\"nozzle_temp\":" + std::to_string(nozzle_temp)
        + ",\"bed_temp\":" + std::to_string(bed_temp)
        + ",\"filaments\":[{\"ams_id\":" + std::to_string(ams_id)
        + ",\"slot_id\":" + std::to_string(slot_id)
        + ",\"tray_id\":" + std::to_string(tray_id)
        + ",\"extruder_id\":0,\"nozzle_temp\":"
        + std::to_string(nozzle_temp) + ",\"bed_temp\":" + std::to_string(bed_temp)
        + ",\"nozzle_diameter\":\"" + diameter_text + "\"}]}}";
    const wxString where = slot.external
        ? _L("the external spool")
        : wxString::Format("AMS %d slot %d", slot.ams_id, slot.slot_id);
    note(_L("Flow calibration on ") + where + _L(", mode 0. Nozzle ") + wxString::FromUTF8(diameter_text)
         + wxString::Format(" mm, nozzle temperature %d (first layer %d, temperature %d), bed temperature %d (first layer %d, bed %d).",
                            nozzle_temp, first_nozzle, nozzle, bed_temp, first_bed, bed));
    send(_L("Start flow calibration"), payload, true);
}

void BambuLanControlDialog::send_ams_filament(bool load)
{
    AmsSlot slot;
    slot.external = true;
    slot.ams_id = 255;
    if (m_ams_selected >= 0 && m_ams_selected < static_cast<int>(m_ams_slots.size()))
        slot = m_ams_slots[m_ams_selected];
    int temp = m_nozzle_temp ? m_nozzle_temp->GetValue() : 0;
    if (slot.nozzle_min > 0 && slot.nozzle_max > 0)
        temp = (slot.nozzle_min + slot.nozzle_max) / 2;
    else if (slot.nozzle_max > 0)
        temp = slot.nozzle_max;
    const std::string ams_id = slot.external ? "255" : std::to_string(slot.ams_id);
    const std::string slot_id = load ? (slot.external ? "0" : std::to_string(slot.slot_id)) : "255";
    const wxString label = load
        ? (slot.external ? _L("Load external spool") : wxString::Format(_L("Load AMS %d slot %d"), slot.ams_id, slot.slot_id))
        : (slot.external ? _L("Unload external spool") : wxString::Format(_L("Unload AMS %d"), slot.ams_id));
    send(label, bambu_ams_change_filament_json(load, ams_id, slot_id, temp, temp, next_sequence()), false);
}

void BambuLanControlDialog::apply_ams_report(const std::string &report)
{
    std::vector<AmsSlot> parsed;
    if (!parse_ams_slots(report, parsed))
        return;
    std::string picture;
    for (const AmsSlot &slot : parsed)
        picture += slot_picture(slot);
    if (picture == m_ams_picture)
        return;
    const std::string old_shape = shape_of(m_ams_slots);
    const std::string new_shape = shape_of(parsed);
    bool keep_ext = true;
    int keep_ams = 255;
    int keep_slot = 0;
    if (m_ams_selected >= 0 && m_ams_selected < static_cast<int>(m_ams_slots.size())) {
        keep_ext = m_ams_slots[m_ams_selected].external;
        keep_ams = m_ams_slots[m_ams_selected].ams_id;
        keep_slot = m_ams_slots[m_ams_selected].slot_id;
    }
    m_ams_slots = std::move(parsed);
    m_ams_picture = std::move(picture);
    m_ams_selected = 0;
    for (int i = 0; i < static_cast<int>(m_ams_slots.size()); ++i) {
        const AmsSlot &slot = m_ams_slots[i];
        if (slot.external == keep_ext && slot.ams_id == keep_ams && slot.slot_id == keep_slot) {
            m_ams_selected = i;
            break;
        }
    }
    if (old_shape == new_shape && m_ams_radios.size() == m_ams_slots.size()) {
        size_t radio_i = 0;
        size_t caption_i = 0;
        size_t index = 0;
        while (index < m_ams_slots.size()) {
            if (m_ams_slots[index].external) {
                if (radio_i < m_ams_radios.size())
                    paint_slot(m_ams_radios[radio_i], m_ams_slots[index]);
                ++radio_i;
                ++index;
                continue;
            }
            const int ams_id = m_ams_slots[index].ams_id;
            if (caption_i < m_ams_captions.size())
                m_ams_captions[caption_i]->SetLabel(unit_caption(m_ams_slots[index]));
            ++caption_i;
            while (index < m_ams_slots.size() && !m_ams_slots[index].external && m_ams_slots[index].ams_id == ams_id) {
                if (radio_i < m_ams_radios.size())
                    paint_slot(m_ams_radios[radio_i], m_ams_slots[index]);
                ++radio_i;
                ++index;
            }
        }
        if (m_ams_selected >= 0 && m_ams_selected < static_cast<int>(m_ams_radios.size()) && m_ams_radios[m_ams_selected])
            m_ams_radios[m_ams_selected]->SetValue(true);
        Layout();
    } else {
        rebuild_ams();
    }
    publish_ams_choice();
}

void BambuLanControlDialog::rebuild_ams()
{
    if (!m_ams_list)
        return;
    m_ams_list->Clear(true);
    m_ams_radios.clear();
    m_ams_captions.clear();
    const int gap = std::max(4, em_unit() / 3);
    size_t index = 0;
    bool first_radio = true;
    while (index < m_ams_slots.size()) {
        auto *row = new wxBoxSizer(wxHORIZONTAL);
        if (m_ams_slots[index].external) {
            auto *radio = new wxRadioButton(this, wxID_ANY, slot_label(m_ams_slots[index]), wxDefaultPosition, wxDefaultSize, first_radio ? wxRB_GROUP : 0);
            first_radio = false;
            const int slot_index = static_cast<int>(index);
            radio->Bind(wxEVT_RADIOBUTTON, [this, slot_index](wxCommandEvent &) {
                m_ams_selected = slot_index;
                publish_ams_choice();
            });
            paint_slot(radio, m_ams_slots[index]);
            m_ams_radios.push_back(radio);
            row->Add(radio, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, gap);
            ++index;
        } else {
            const int ams_id = m_ams_slots[index].ams_id;
            auto *caption = new wxStaticText(this, wxID_ANY, unit_caption(m_ams_slots[index]));
            m_ams_captions.push_back(caption);
            row->Add(caption, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, gap * 2);
            while (index < m_ams_slots.size() && !m_ams_slots[index].external && m_ams_slots[index].ams_id == ams_id) {
                auto *radio = new wxRadioButton(this, wxID_ANY, slot_label(m_ams_slots[index]), wxDefaultPosition, wxDefaultSize, first_radio ? wxRB_GROUP : 0);
                first_radio = false;
                const int slot_index = static_cast<int>(index);
                radio->Bind(wxEVT_RADIOBUTTON, [this, slot_index](wxCommandEvent &) {
                    m_ams_selected = slot_index;
                    publish_ams_choice();
                });
                paint_slot(radio, m_ams_slots[index]);
                m_ams_radios.push_back(radio);
                row->Add(radio, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, gap);
                ++index;
            }
        }
        m_ams_list->Add(row, 0, wxEXPAND | wxBOTTOM, gap);
    }
    if (m_ams_selected >= 0 && m_ams_selected < static_cast<int>(m_ams_radios.size()) && m_ams_radios[m_ams_selected])
        m_ams_radios[m_ams_selected]->SetValue(true);
    wxGetApp().UpdateDlgDarkUI(this);
    if (m_camera_panel) {
        m_camera_panel->SetBackgroundColour(*wxBLACK);
        m_camera_panel->SetForegroundColour(wxColour(220, 220, 220));
    }
    for (size_t i = 0; i < m_ams_radios.size() && i < m_ams_slots.size(); ++i)
        paint_slot(m_ams_radios[i], m_ams_slots[i]);
    Layout();
    Fit();
}

void BambuLanControlDialog::publish_ams_choice()
{
    AmsSlot slot;
    slot.external = true;
    slot.ams_id = 255;
    if (m_ams_selected >= 0 && m_ams_selected < static_cast<int>(m_ams_slots.size()))
        slot = m_ams_slots[m_ams_selected];
    BambuAmsPrintChoice choice;
    if (slot.external) {
        choice.use_ams = false;
        choice.ams_mapping = "[-1]";
        choice.ams_mapping2 = "[{\"ams_id\":255,\"slot_id\":255}]";
    } else {
        const int tray = slot.ams_id < 16 ? slot.ams_id * 4 + slot.slot_id : slot.slot_id;
        choice.use_ams = true;
        choice.ams_mapping = "[" + std::to_string(tray) + "]";
        choice.ams_mapping2 = "[{\"ams_id\":" + std::to_string(slot.ams_id) + ",\"slot_id\":" + std::to_string(slot.slot_id) + "}]";
    }
    bambu_lan_set_ams_print_choice(choice);
}

void BambuLanControlDialog::save_selected_slot()
{
    if (!edited_host_is_bambu(m_bundle)) {
        note(_L("Filament values from the AMS are saved only for a Bambu Lab (Developer LAN) host."));
        return;
    }
    if (m_ams_selected < 0 || m_ams_selected >= static_cast<int>(m_ams_slots.size()))
        return;
    const AmsSlot &slot = m_ams_slots[m_ams_selected];
    DynamicPrintConfig &filament = m_bundle->filaments.get_edited_preset().config;
    if (!slot.material.empty())
        set_string_opt(filament, "filament_type", slot.material);
    std::string hex;
    for (char c : slot.color) {
        if (std::isxdigit(static_cast<unsigned char>(c)))
            hex.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (hex.size() >= 6)
        set_string_opt(filament, "filament_colour", "#" + hex.substr(0, 6));
    if (slot.nozzle_min > 0 || slot.nozzle_max > 0) {
        const int temp = slot.nozzle_max > 0 ? slot.nozzle_max : slot.nozzle_min;
        const int first = slot.nozzle_min > 0 ? slot.nozzle_min : temp;
        set_int_opt(filament, "temperature", temp);
        set_int_opt(filament, "first_layer_temperature", first);
    }
    if (slot.bed_temp > 0) {
        set_int_opt(filament, "bed_temperature", slot.bed_temp);
        set_int_opt(filament, "first_layer_bed_temperature", slot.bed_temp);
    }
    if (slot.has_k) {
        set_float_opt(filament, "pressure_advance", slot.k);
        set_bool_opt(filament, "enable_pressure_advance", true);
    }
    if (slot.has_flow && slot.flow > 0.)
        set_float_opt(filament, "filament_flow_ratio", slot.flow);
    mark_filament_dirty(m_bundle);
    note(_L("Saved the selected spool onto the filament preset."));
}

void BambuLanControlDialog::save_temperature(bool nozzle)
{
    if (!edited_host_is_bambu(m_bundle))
        return;
    DynamicPrintConfig &filament = m_bundle->filaments.get_edited_preset().config;
    if (nozzle) {
        const int temp = m_nozzle_temp ? m_nozzle_temp->GetValue() : 0;
        set_int_opt(filament, "temperature", temp);
        set_int_opt(filament, "first_layer_temperature", temp);
        note(wxString::Format(_L("Saved nozzle temperature %d on the filament preset."), temp));
    } else {
        const int temp = m_bed_temp ? m_bed_temp->GetValue() : 0;
        set_int_opt(filament, "bed_temperature", temp);
        set_int_opt(filament, "first_layer_bed_temperature", temp);
        note(wxString::Format(_L("Saved bed temperature %d on the filament preset."), temp));
    }
    mark_filament_dirty(m_bundle);
}

void BambuLanControlDialog::store_flow_reply(const wxString &reply)
{
    if (!edited_host_is_bambu(m_bundle))
        return;
    const auto encoded = reply.ToUTF8();
    const std::string text = encoded.data() ? std::string(encoded.data()) : std::string();
    const std::string result = json_field(text, "result");
    if (result == "fail" || result == "failed") {
        note(_L("The printer did not accept the calibration command. Filament K and flow ratio were not changed."));
        return;
    }
    double k = 0;
    double flow = 0;
    const bool has_k = json_number(json_field(text, "k_value"), k);
    const bool has_flow = json_number(json_field(text, "flow_ratio"), flow);
    if (!has_k && !has_flow) {
        note(_L("The printer accepted the command and did not return a K or flow ratio. The filament fields are unchanged."));
        return;
    }
    DynamicPrintConfig &filament = m_bundle->filaments.get_edited_preset().config;
    if (has_k) {
        set_float_opt(filament, "pressure_advance", k);
        set_bool_opt(filament, "enable_pressure_advance", true);
        note(wxString::Format(_L("Saved flow-dynamics K %.4f on the filament preset."), k));
    }
    if (has_flow) {
        set_float_opt(filament, "filament_flow_ratio", flow);
        note(wxString::Format(_L("Saved flow ratio %.4f on the filament preset."), flow));
    }
    mark_filament_dirty(m_bundle);
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
