///|/ Copyright (c) 2026 Stan Elston
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_GUI_BambuLanControlDialog_hpp_
#define slic3r_GUI_BambuLanControlDialog_hpp_

#include <atomic>
#include <memory>
#include <vector>

#include <wx/timer.h>

#include "GUI_Utils.hpp"

class wxButton;
class wxRadioButton;
class wxSpinCtrl;
class wxTextCtrl;

namespace Slic3r {

class BambuLanCamera;
class BambuLanSession;
class PresetBundle;

namespace GUI {

class BambuCameraPanel;

// Live Developer LAN controls for a Bambu Lab printer. Opened from Printer
// Settings when the host type is Bambu Lab (Developer LAN).
class BambuLanControlDialog : public DPIDialog
{
public:
    BambuLanControlDialog(wxWindow *parent, PresetBundle *bundle);
    ~BambuLanControlDialog() override;

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    struct FrameSlot;

    void note(const wxString &line);
    void send(const wxString &label, const std::string &payload, bool await_reply);
    void send_move(const char *axis, int dir);
    void send_flow_calibration();
    std::string next_sequence();
    void on_timer(wxTimerEvent &event);
    void set_commands_enabled(bool enabled);

    PresetBundle *m_bundle = nullptr;
    std::shared_ptr<std::atomic<bool>> m_alive = std::make_shared<std::atomic<bool>>(true);
    std::shared_ptr<FrameSlot> m_frames;
    std::unique_ptr<BambuLanSession> m_session;
    std::unique_ptr<BambuLanCamera> m_camera;
    std::atomic<unsigned> m_sequence{1000};

    BambuCameraPanel *m_camera_panel = nullptr;
    wxTextCtrl *m_log = nullptr;
    wxRadioButton *m_step_10 = nullptr;
    wxSpinCtrl *m_nozzle_temp = nullptr;
    wxSpinCtrl *m_bed_temp = nullptr;
    wxTimer m_timer;
    std::vector<wxButton *> m_commands;
};

} // namespace GUI
} // namespace Slic3r

#endif
