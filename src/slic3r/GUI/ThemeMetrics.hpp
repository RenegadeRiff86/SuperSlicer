#ifndef slic3r_GUI_ThemeMetrics_hpp_
#define slic3r_GUI_ThemeMetrics_hpp_

#include <string>

class wxWindow;

namespace Slic3r::GUI::ThemeMetrics {

int space_xs(wxWindow* win);
int space_sm(wxWindow* win);
int space_md(wxWindow* win);
int space_lg(wxWindow* win);

int radius_sm(wxWindow* win);
int radius_md(wxWindow* win);
int stroke_thin(wxWindow* win);

int notebook_button_margin(wxWindow* win);
int notebook_line_margin(wxWindow* win);
int notebook_min_height(wxWindow* win);

float toolbar_border();
float toolbar_separator();
float toolbar_main_gap();
float toolbar_view_gap();
float toolbar_collapse_gap();

double combo_corner_radius(wxWindow* win);
int combo_item_padding(wxWindow* win);

std::string ui_density_preference();

} // namespace Slic3r::GUI::ThemeMetrics

#endif
