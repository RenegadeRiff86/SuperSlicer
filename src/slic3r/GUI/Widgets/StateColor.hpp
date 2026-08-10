#ifndef slic3r_GUI_StateColor_hpp_
#define slic3r_GUI_StateColor_hpp_

#include <wx/colour.h>

class StateColor
{
    static constexpr int NegatedStateBitOffset = 16;

public:
    enum State {
        Normal     = 0,
        Enabled    = 1 << 0,
        Checked    = 1 << 1,
        Focused    = 1 << 2,
        Hovered    = 1 << 3,
        Pressed    = 1 << 4,
        Disabled   = Enabled << NegatedStateBitOffset,
        NotChecked = Checked << NegatedStateBitOffset,
        NotFocused = Focused << NegatedStateBitOffset,
        NotHovered = Hovered << NegatedStateBitOffset,
        NotPressed = Pressed << NegatedStateBitOffset,
    };

public:
    template<typename ...Colors>
    StateColor(std::pair<Colors, int>... colors) {
        fill(colors...);
    }

    // single color
    StateColor(wxColour const & color);

    // single color
    StateColor(wxString const &color);

    // single color
    StateColor(unsigned long color);

public:
    void append(wxColour const & color, int states);

    void append(wxString const &color, int states);

    void append(unsigned long color, int states);

    void clear();

public:
    int count() const { return statesList_.size(); }

    int states() const;

public:
    wxColour defaultColor();

    wxColour colorForStates(int states);

    int colorIndexForStates(int states);

    bool setColorForStates(wxColour const & color, int states);

    void setTakeFocusedAsHovered(bool set);

private:
    template<typename Color, typename ...Colors>
    void fill(std::pair<Color, int> color, std::pair<Colors, int>... colors) {
        fillOne(color);
        fill(colors...);
    }

    template<typename Color>
    void fillOne(std::pair<Color, int> color) {
        append(color.first, color.second);
    }

    void fill() {
    }

private:
    std::vector<int> statesList_;
    std::vector<wxColour> colors_;
    bool takeFocusedAsHovered_ = true;
};

#endif // !slic3r_GUI_StateColor_hpp_
