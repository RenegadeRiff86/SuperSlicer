#pragma once

#include "CreateMMUTiledCanvasRPlaceData.hpp"
#include "CreateMMUTiledCanvasShared.hpp"
#include "GUI_Utils.hpp"
#include "I18N.hpp"
#include "wxExtensions.hpp"

#include <algorithm>
#include <wx/gbsizer.h>

namespace Slic3r::GUI {

using namespace CreateMMUTiledCanvasDetail;

class GetRPlaceDialog : public DPIDialog
    {
    public:
        wxSpinCtrl* day;
        wxSpinCtrl* hour;
        wxSpinCtrl* minute;
        wxSpinCtrl* second;

        wxButton* bt_previous;
        wxButton* bt_next;

        long timestamp = 0;

        static inline long last_timestamp = 1649112424;

        void check_file(int delta = 0) {
            timestamp = kRPlaceEpochUnix + kSecondsPerDay * (day->GetValue() - 1);
            timestamp += kSecondsPerHour * (hour->GetValue());
            timestamp += kSecondsPerMinute * (minute->GetValue());
            timestamp += (second->GetValue());
            timestamp += delta;
            //it's ordered
            int idx = 0;
            while (idx < int(rplace_timestamps().size()) && timestamp > rplace_timestamps()[idx]) { idx++; }
            //if after the last or ( if not found, and it not before the begin, and only if we don't want the next.)
            if (idx >= int(rplace_timestamps().size()) || (timestamp != rplace_timestamps()[idx] && idx > 0 && delta<=0)) idx--;
            //enforce a good file
            timestamp = rplace_timestamps()[idx];
            day->SetValue(1 + (timestamp - kRPlaceEpochUnix) / kSecondsPerDay);
            hour->SetValue(((timestamp - kRPlaceEpochUnix) / kSecondsPerHour) % kHoursPerDay);
            minute->SetValue(((timestamp - kRPlaceEpochUnix) / kSecondsPerMinute) % kSecondsPerMinute);
            second->SetValue((timestamp - kRPlaceEpochUnix) % kSecondsPerMinute);
        }


        GetRPlaceDialog(wxWindow* p, wxWindowID id, const wxString& title) : DPIDialog(p, id, title, wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE, "getrplace"){
            int spin_options = wxTE_PROCESS_ENTER | wxSP_ARROW_KEYS
#ifdef _WIN32
                | wxBORDER_SIMPLE
#endif
                ;
            // last pic before white 1649112424 ~05/04/2022 00:47:04 
            wxStaticText* lbl_day = new wxStaticText(this, wxID_ANY, "Day");
            wxStaticText* lbl_hour = new wxStaticText(this, wxID_ANY, "Hour");
            wxStaticText* lbl_minute = new wxStaticText(this, wxID_ANY, "Minute");
            wxStaticText* lbl_second = new wxStaticText(this, wxID_ANY, "Second");
            day = new wxSpinCtrl(this, wxID_ANY, "Day", wxDefaultPosition, wxSize(kSpinCtrlWidthPx, kSpinCtrlHeightPx), spin_options, 1, kMaximumDay, kDefaultDay);
            hour = new wxSpinCtrl(this, wxID_ANY, "H", wxDefaultPosition, wxSize(kSpinCtrlWidthPx, kSpinCtrlHeightPx), spin_options, 0, kHoursPerDay, 0);
            minute = new wxSpinCtrl(this, wxID_ANY, "M", wxDefaultPosition, wxSize(kSpinCtrlWidthPx, kSpinCtrlHeightPx), spin_options, 0, kSecondsPerMinute, 47);
            second = new wxSpinCtrl(this, wxID_ANY, "S", wxDefaultPosition, wxSize(kSpinCtrlWidthPx, kSpinCtrlHeightPx), spin_options, 0, kSecondsPerMinute, 35);
            timestamp = last_timestamp;
            day->SetValue(1 + (timestamp - kRPlaceEpochUnix) / kSecondsPerDay);
            hour->SetValue(((timestamp - kRPlaceEpochUnix) / kSecondsPerHour) % kHoursPerDay);
            minute->SetValue(((timestamp - kRPlaceEpochUnix) / kSecondsPerMinute) % kSecondsPerMinute);
            second->SetValue((timestamp - kRPlaceEpochUnix) % kSecondsPerMinute);
            day->Bind(wxEVT_SPINCTRL, ([this](wxCommandEvent e) {  check_file();  }), day->GetId());
            hour->Bind(wxEVT_SPINCTRL, ([this](wxCommandEvent e) {  check_file();  }), hour->GetId());
            minute->Bind(wxEVT_SPINCTRL, ([this](wxCommandEvent e) {  check_file();  }), minute->GetId());
            second->Bind(wxEVT_SPINCTRL, ([this](wxCommandEvent e) {  check_file();  }), second->GetId());
            bt_previous = new wxButton(this, wxID_ANY, _L("Previous"));
            bt_previous->Bind(wxEVT_BUTTON, ([this](wxCommandEvent e) {  check_file(-1);  }), bt_previous->GetId());
            bt_next = new wxButton(this, wxID_ANY, _L("Next"));
            bt_next->Bind(wxEVT_BUTTON, ([this](wxCommandEvent e) {  check_file(1);  }), bt_next->GetId());
            wxStdDialogButtonSizer* buttons = this->CreateStdDialogButtonSizer(wxCANCEL | wxOK);
            std::sort(rplace_timestamps().begin(), rplace_timestamps().end());

            wxGridBagSizer* gb_sizer = new wxGridBagSizer(1, 1); //(int vgap, int hgap)
            int x=1, y=1;
            x = kDialogFirstInputColumn;
            gb_sizer->Add(lbl_day, wxGBPosition(y, x++), wxGBSpan(1, 1));
            gb_sizer->Add(lbl_hour, wxGBPosition(y, x++), wxGBSpan(1, 1));
            gb_sizer->Add(lbl_minute, wxGBPosition(y, x++), wxGBSpan(1, 1));
            gb_sizer->Add(lbl_second, wxGBPosition(y, x++), wxGBSpan(1, 1));
            x = 1; y++;
            gb_sizer->Add(bt_previous, wxGBPosition(y, x++), wxGBSpan(1, 1));
            gb_sizer->Add(day, wxGBPosition(y, x++), wxGBSpan(1, 1));
            gb_sizer->Add(hour, wxGBPosition(y, x++), wxGBSpan(1, 1));
            gb_sizer->Add(minute, wxGBPosition(y, x++), wxGBSpan(1, 1));
            gb_sizer->Add(second, wxGBPosition(y, x++), wxGBSpan(1, 1));
            gb_sizer->Add(bt_next, wxGBPosition(y, x++), wxGBSpan(1, 1));
            x = 1; y++;
            gb_sizer->Add(buttons, wxGBPosition(y, x), wxGBSpan(1, kButtonColumnSpan));
            
            SetSizerAndFit(gb_sizer);
            //this->Fit();
            //wxSize dialog_size((80*6+20) * this->scale_factor(), 150 * this->scale_factor());
            //this->SetSize(dialog_size);
            //this->Fit();

            check_file();
        }
        virtual ~GetRPlaceDialog() {}

        void on_dpi_changed(const wxRect& suggested_rect) override
        {
            msw_buttons_rescale(this, em_unit(), { wxID_APPLY, wxID_CLOSE });

            wxSize oldSize = this->GetSize();
            Layout();
            this->SetSize(oldSize.x * this->scale_factor() / this->prev_scale_factor(), oldSize.y * this->scale_factor() / this->prev_scale_factor());
            Refresh();
        }

    };

} // namespace Slic3r::GUI
