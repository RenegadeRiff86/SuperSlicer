#include "SwitchButton.hpp"

#include "../wxExtensions.hpp"
#include "../../Utils/MacDarkMode.hpp"

#include <wx/dcgraph.h>
#include <wx/dcmemory.h>
#include <wx/dcclient.h>

SwitchButton::SwitchButton(wxWindow* parent, const wxString& name, wxWindowID id)
    : BitmapToggleButton(parent, name, id)
    , m_on(this, "toggle_on", 28, 16)
    , m_off(this, "toggle_off", 28, 16)
    , text_color(std::pair{*wxWHITE, static_cast<int>(StateColor::Checked)}, std::pair{0x6B6B6B, static_cast<int>(StateColor::Normal)})
    , track_color(0xD9D9D9)
    , thumb_color(std::pair{0x00AE42, static_cast<int>(StateColor::Checked)}, std::pair{0xD9D9D9, static_cast<int>(StateColor::Normal)})
{
    Rescale();
}

void SwitchButton::SetLabels(wxString const& lbl_on, wxString const& lbl_off)
{
    labels[0] = lbl_on;
    labels[1] = lbl_off;
    Rescale();
}

void SwitchButton::SetTextColor(StateColor const& color)
{
    text_color = color;
}

void SwitchButton::SetTrackColor(StateColor const& color)
{
    track_color = color;
}

void SwitchButton::SetThumbColor(StateColor const& color)
{
    thumb_color = color;
}

void SwitchButton::SetValue(bool value)
{
    if (value != GetValue())
        wxBitmapToggleButton::SetValue(value);
    update();
}

void SwitchButton::Rescale()
{
    constexpr int STATE_COUNT = 2;
    constexpr int CENTER_DIVISOR = 2;
    constexpr int DOUBLE_BORDER_FACTOR = 2;

    if (!labels[0].IsEmpty()) {
#ifdef __WXOSX__
        auto scale = Slic3r::GUI::mac_max_scaling_factor();
        int BS = static_cast<int>(scale);
#else
        constexpr int BS = 1;
#endif
        wxSize thumbSize;
        wxSize trackSize;
        wxClientDC dc(this);
#ifdef __WXOSX__
        dc.SetFont(dc.GetFont().Scaled(scale));
#endif
        wxSize textSize[STATE_COUNT];
        {
            textSize[0] = dc.GetTextExtent(labels[0]);
            textSize[1] = dc.GetTextExtent(labels[1]);
        }
        {
            thumbSize = textSize[0];
            auto size = textSize[1];
            if (size.x > thumbSize.x) thumbSize.x = size.x;
            else size.x = thumbSize.x;
            thumbSize.x += BS * 12;
            thumbSize.y += BS * 6;
            trackSize.x = thumbSize.x + size.x + BS * 10;
            trackSize.y = thumbSize.y + BS * DOUBLE_BORDER_FACTOR;
            auto maxWidth = GetMaxWidth();
#ifdef __WXOSX__
            maxWidth *= scale;
#endif
            if (trackSize.x > maxWidth) {
                thumbSize.x -= (trackSize.x - maxWidth) / CENTER_DIVISOR;
                trackSize.x = maxWidth;
            }
        }
        for (int i = 0; i < STATE_COUNT; ++i) {
            wxMemoryDC memdc(&dc);
            wxBitmap bmp(trackSize.x, trackSize.y);
            memdc.SelectObject(bmp);
            memdc.SetBackground(wxBrush(GetBackgroundColour()));
            memdc.Clear();
            memdc.SetFont(dc.GetFont());
            auto state = i == 0 ? StateColor::Enabled : (StateColor::Checked | StateColor::Enabled);
            {
#ifdef __WXMSW__
                wxGCDC dc2(memdc);
#else
                wxDC &dc2(memdc);
#endif
                dc2.SetBrush(wxBrush(track_color.colorForStates(state)));
                dc2.SetPen(wxPen(track_color.colorForStates(state)));
                dc2.DrawRoundedRectangle(wxRect({0, 0}, trackSize), trackSize.y / CENTER_DIVISOR);
                dc2.SetBrush(wxBrush(thumb_color.colorForStates(StateColor::Checked | StateColor::Enabled)));
                dc2.SetPen(wxPen(thumb_color.colorForStates(StateColor::Checked | StateColor::Enabled)));
                dc2.DrawRoundedRectangle(wxRect({ i == 0 ? BS : (trackSize.x - thumbSize.x - BS), BS}, thumbSize), thumbSize.y / CENTER_DIVISOR);
            }
            memdc.SetTextForeground(text_color.colorForStates(state ^ StateColor::Checked));
            memdc.DrawText(labels[0], {BS + (thumbSize.x - textSize[0].x) / CENTER_DIVISOR, BS + (thumbSize.y - textSize[0].y) / CENTER_DIVISOR});
            memdc.SetTextForeground(text_color.colorForStates(state));
            memdc.DrawText(labels[1], {trackSize.x - thumbSize.x - BS + (thumbSize.x - textSize[1].x) / CENTER_DIVISOR, BS + (thumbSize.y - textSize[1].y) / CENTER_DIVISOR});
            memdc.SelectObject(wxNullBitmap);
#ifdef __WXOSX__
            bmp = wxBitmap(bmp.ConvertToImage(), -1, scale);
#endif
            (i == 0 ? m_off : m_on).SetBitmap(bmp);
        }
    }

    update();
}

void SwitchButton::SysColorChange()
{
    m_on.sys_color_changed();
    m_off.sys_color_changed();

    update();
}

void SwitchButton::update()
{
    SetBitmap((GetValue() ? m_on : m_off).bmp());
    update_size();
}
