#include "ThemeMetrics.hpp"

#include "GUI_App.hpp"
#include "libslic3r/AppConfig.hpp"
#include "wxExtensions.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace Slic3r {
namespace GUI {
namespace ThemeMetrics {

namespace {
enum class Platform : size_t { Windows = 0, Mac = 1, Linux = 2 };
enum class Density : size_t { Comfortable = 0, Compact = 1 };

struct TokenTable {
    double space_xs_em;
    double space_sm_em;
    double space_md_em;
    double space_lg_em;

    double radius_sm_em;
    double radius_md_em;
    double stroke_thin_em;

    double notebook_btn_margin_em;
    double notebook_line_margin_em;
    double notebook_min_height_em;

    double settings_row_gap_em;
    double settings_horizontal_gap_em;
    double settings_sidebar_width_em;
    double settings_group_margin_em;
    double settings_group_padding_em;
    double settings_scroll_step_em;

    float toolbar_border_px;
    float toolbar_separator_px;
    float toolbar_main_gap_px;
    float toolbar_view_gap_px;
    float toolbar_collapse_gap_px;
};

// Design tokens, one row per [platform][density]. The values are tabulated data, not
// independent constants: the same number means a different token in different rows
// (0.40 is space_lg_em on Windows but space_md_em on macOS), so they are labelled here
// rather than folded into shared named constants that would imply a coupling.
//
// Column order follows TokenTable above:
//   line 1 - space xs/sm/md/lg, radius sm/md, stroke thin,
//            notebook button margin, notebook line margin, notebook min height   (em)
//   line 2 - settings row gap, horizontal gap, sidebar width, group margin,
//            group padding, scroll step (em); toolbar border, separator,
//            main gap, view gap, collapse gap (px)
constexpr std::array<std::array<TokenTable, 2>, 3> TOKEN_TABLE = {{
    // Windows
    {{
        {0.10, 0.20, 0.30, 0.40, 0.20, 0.30, 0.10, 0.30, 0.10, 2.40,        // comfortable, em
         1.15, 0.35, 22.0, 0.75, 0.55, 2.40, 5.0f, 5.0f, 4.0f, 1.0f, 2.0f}, // comfortable, em then px
        {0.08, 0.15, 0.23, 0.30, 0.16, 0.24, 0.08, 0.23, 0.08, 1.80,        // compact, em
         0.80, 0.20, 19.0, 0.45, 0.35, 1.80, 4.0f, 4.0f, 3.0f, 1.0f, 1.0f}  // compact, em then px
    }},
    // macOS
    {{
        {0.12, 0.24, 0.40, 0.50, 0.22, 0.34, 0.10, 0.40, 0.10, 2.40,        // comfortable, em
         1.20, 0.40, 22.0, 0.80, 0.60, 2.40, 5.0f, 5.0f, 4.0f, 1.0f, 2.0f}, // comfortable, em then px
        {0.09, 0.18, 0.30, 0.38, 0.17, 0.26, 0.08, 0.30, 0.08, 1.80,        // compact, em
         0.85, 0.25, 19.0, 0.50, 0.40, 1.80, 4.0f, 4.0f, 3.0f, 1.0f, 1.0f}  // compact, em then px
    }},
    // Linux and others
    {{
        {0.12, 0.24, 0.40, 0.50, 0.22, 0.34, 0.10, 0.40, 0.10, 2.40,        // comfortable, em
         1.20, 0.40, 22.0, 0.80, 0.60, 2.40, 5.0f, 5.0f, 4.0f, 1.0f, 2.0f}, // comfortable, em then px
        {0.09, 0.18, 0.30, 0.38, 0.17, 0.26, 0.08, 0.30, 0.08, 1.80,        // compact, em
         0.85, 0.25, 19.0, 0.50, 0.40, 1.80, 4.0f, 4.0f, 3.0f, 1.0f, 1.0f}  // compact, em then px
    }}
}};

Platform current_platform()
{
#ifdef _WIN32
    return Platform::Windows;
#elif defined(__APPLE__)
    return Platform::Mac;
#else
    return Platform::Linux;
#endif
}

Density current_density()
{
    const std::string pref = ui_density_preference();
    return pref == "compact" ? Density::Compact : Density::Comfortable;
}

const TokenTable& table()
{
    return TOKEN_TABLE[static_cast<size_t>(current_platform())][static_cast<size_t>(current_density())];
}

int em_scaled(wxWindow* win, double ems)
{
    const int em = em_unit(win);
    return std::lround(ems * em);
}

int em_scaled_min1(wxWindow* win, double ems)
{
    return std::max(1, em_scaled(win, ems));
}

} // namespace

std::string ui_density_preference()
{
    AppConfig* app_config = wxGetApp().app_config.get();
    if (app_config == nullptr)
        return "comfortable";

    const std::string density = app_config->get("ui_density");
    if (density == "compact" || density == "comfortable")
        return density;

    return app_config->get_bool("tab_density_compact") ? "compact" : "comfortable";
}

int space_xs(wxWindow* win) { return em_scaled_min1(win, table().space_xs_em); }
int space_sm(wxWindow* win) { return em_scaled_min1(win, table().space_sm_em); }
int space_md(wxWindow* win) { return em_scaled_min1(win, table().space_md_em); }
int space_lg(wxWindow* win) { return em_scaled_min1(win, table().space_lg_em); }

int radius_sm(wxWindow* win) { return em_scaled_min1(win, table().radius_sm_em); }
int radius_md(wxWindow* win) { return em_scaled_min1(win, table().radius_md_em); }
int stroke_thin(wxWindow* win) { return em_scaled_min1(win, table().stroke_thin_em); }

int notebook_button_margin(wxWindow* win) { return em_scaled_min1(win, table().notebook_btn_margin_em); }
int notebook_line_margin(wxWindow* win) { return em_scaled_min1(win, table().notebook_line_margin_em); }
int notebook_min_height(wxWindow* win) { return em_scaled_min1(win, table().notebook_min_height_em); }

int settings_row_gap(wxWindow* win) { return em_scaled_min1(win, table().settings_row_gap_em); }
int settings_horizontal_gap(wxWindow* win) { return em_scaled_min1(win, table().settings_horizontal_gap_em); }
int settings_sidebar_width(wxWindow* win) { return em_scaled_min1(win, table().settings_sidebar_width_em); }
int settings_group_margin(wxWindow* win) { return em_scaled_min1(win, table().settings_group_margin_em); }
int settings_group_padding(wxWindow* win) { return em_scaled_min1(win, table().settings_group_padding_em); }
int settings_scroll_step(wxWindow* win) { return em_scaled_min1(win, table().settings_scroll_step_em); }

float toolbar_border() { return table().toolbar_border_px; }
float toolbar_separator() { return table().toolbar_separator_px; }
float toolbar_main_gap() { return table().toolbar_main_gap_px; }
float toolbar_view_gap() { return table().toolbar_view_gap_px; }
float toolbar_collapse_gap() { return table().toolbar_collapse_gap_px; }

double combo_corner_radius(wxWindow* win)
{
    return wxGetApp().suppress_round_corners() ? 0.0 : static_cast<double>(radius_md(win));
}

int combo_item_padding(wxWindow* win)
{
    return space_xs(win);
}

} // namespace ThemeMetrics
} // namespace GUI
} // namespace Slic3r
