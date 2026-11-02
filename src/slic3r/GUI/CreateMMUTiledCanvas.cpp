#include "CreateMMUTiledCanvas.hpp"

#include "I18N.hpp"
#include "libslic3r/Config.hpp"
#include "GUI.hpp"
#include "Plater.hpp"
#include <wx/notebook.h>

#include <ctime>
#include <cstdio>
#include <cstdlib>

#include <wx/wx.h>
#include <wx/brush.h>
#include <wx/scrolwin.h>
#include <wx/file.h>
#include <wx/mimetype.h>
#include <wx/odcombo.h>
#include <wx/rawbmp.h>
#include <wx/textctrl.h>
#include <wx/wrapsizer.h>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/ini_parser.hpp>


#include "CreateMMUTiledCanvasShared.hpp"

namespace Slic3r {
namespace GUI {

using namespace CreateMMUTiledCanvasDetail;

BEGIN_EVENT_TABLE(BasicDrawPane, wxPanel)
        // some useful events
        /*
         EVT_MOTION(BasicDrawPane::mouseMoved)
         EVT_LEFT_DOWN(BasicDrawPane::mouseDown)
         EVT_LEFT_UP(BasicDrawPane::mouseReleased)
         EVT_RIGHT_DOWN(BasicDrawPane::rightClick)
         EVT_LEAVE_WINDOW(BasicDrawPane::mouseLeftWindow)
         EVT_KEY_DOWN(BasicDrawPane::keyPressed)
         EVT_KEY_UP(BasicDrawPane::keyReleased)
         EVT_MOUSEWHEEL(BasicDrawPane::mouseWheelMoved)
         */

         // catch paint events
        EVT_PAINT(BasicDrawPane::paintEvent)

        END_EVENT_TABLE()


        // some useful events
        /*
         void BasicDrawPane::mouseMoved(wxMouseEvent& event) {}
         void BasicDrawPane::mouseDown(wxMouseEvent& event) {}
         void BasicDrawPane::mouseWheelMoved(wxMouseEvent& event) {}
         void BasicDrawPane::mouseReleased(wxMouseEvent& event) {}
         void BasicDrawPane::rightClick(wxMouseEvent& event) {}
         void BasicDrawPane::mouseLeftWindow(wxMouseEvent& event) {}
         void BasicDrawPane::keyPressed(wxKeyEvent& event) {}
         void BasicDrawPane::keyReleased(wxKeyEvent& event) {}
         */

        BasicDrawPane::BasicDrawPane(wxWindow* parent, MyDynamicConfig* config) :
        wxPanel(parent)
    {
        SetMinSize(wxSize(kDrawPaneMinSizePx, kDrawPaneMinSizePx));
    }

    /*
     * Called by the system of by wxWidgets when the panel needs
     * to be redrawn. You can also trigger this call by
     * calling Refresh()/Update().
     */

    void BasicDrawPane::paintEvent(wxPaintEvent& evt)
    {
        wxPaintDC dc(this);
        render(dc);
    }

    /*
     * Alternatively, you can use a clientDC to paint on the panel
     * at any time. Using this generally does not free you from
     * catching paint events, since it is possible that e.g. the window
     * manager throws away your drawing when the window comes to the
     * background, and expects you will redraw it when the window comes
     * back (by sending a paint event).
     *
     * In most cases, this will not be needed at all; simply handling
     * paint events and calling Refresh() when a refresh is needed
     * will do the job.
     */
    void BasicDrawPane::paintNow()
    {
        wxClientDC dc(this);
        render(dc);
    }

    void BasicDrawPane::redrawImage()
    {
    }

    // Index of the used-colour entry whose printed colour best matches `color`. An exact match
    // wins immediately; otherwise the nearest colour is taken only when use_near_color is set.
    static int nearest_used_color_index(const std::vector<ColorEntry*>& used_colors,
                                        const wxColour&                 color,
                                        int                             color_algo,
                                        bool                            use_near_color,
                                        bool                            use_spool_colors)
    {
        int nearest_idx = 0;
        for (int i = 0; i < int(used_colors.size()); i++) {
            const wxColour candidate = used_colors[i]->get_printed_color(use_spool_colors);
            if (candidate == color)
                return i;
            if (!use_near_color)
                continue;
            const wxColour nearest = used_colors[nearest_idx]->get_printed_color(use_spool_colors);
            if (color_dist(color_algo, candidate, color) < color_dist(color_algo, nearest, color))
                nearest_idx = i;
        }
        return nearest_idx;
    }

    // Index of `color` in `pixel_colors`, appending a new entry when the colour is not there yet.
    static size_t pixel_color_index(std::vector<ColorEntry>& pixel_colors, const wxColour& color)
    {
        for (size_t i = 0; i < pixel_colors.size(); i++) {
            if (pixel_colors[i].real_color == color)
                return i;
        }
        pixel_colors.emplace_back(color);
        return pixel_colors.size() - 1;
    }

    // Spool whose printed colour sits closest to `target`. `spools` must not be empty.
    static ColorEntrySpool* nearest_spool(std::vector<ColorEntrySpool>& spools,
                                          int color_algo, bool use_spool, const wxColour& target)
    {
        ColorEntrySpool* nearest = &spools[0];
        for (size_t i = 1; i < spools.size(); i++) {
            if (color_dist(color_algo, spools[i].get_printed_color(use_spool), target)
                < color_dist(color_algo, nearest->get_printed_color(use_spool), target))
                nearest = &spools[i];
        }
        return nearest;
    }

    /*
     * Here we do the actual rendering. I put it in a separate
     * method so that it can work no matter what type of DC
     * (e.g. wxPaintDC or wxClientDC) is used.
     */
    void BasicDrawPane::render(wxDC& dc)
    {
        dc.SetBrush(*wxBLACK_BRUSH);
        dc.DrawRectangle(0, 0, GetSize().x, GetSize().y);

        if (!bmp.IsOk())
            return;
        if (bmp.GetSize().x <= 0 || bmp.GetSize().y <= 0)
            return;
        if (bmp.GetWidth() == 0 || bmp.GetHeight() == 0)
            return;

        if (previous_size != GetSize()) {
            previous_size = GetSize();
            Refresh();
            return;
        }

        Vec2d offset_dbl = parent->m_config.option<ConfigOptionPoint>("offset")->value;
        wxSize offset(std::min(int(offset_dbl.x()), std::max(0, bmp.GetSize().x - 1)), std::min(int(offset_dbl.y()), std::max(0, bmp.GetSize().y - 1)));
        Vec2d size_dbl = parent->m_config.option<ConfigOptionPoint>("size")->value;
        wxSize size(std::min(int(size_dbl.x()), bmp.GetSize().x - offset.x), std::min(int(size_dbl.y()), bmp.GetSize().y - offset.y));
        Vec2d pixel_size = parent->m_config.option<ConfigOptionPoint>("size_px")->value;
        double separation = parent->m_config.opt_float(kSeparationXyKey);

        //compute pixel per mm
        double max_x_mm = (size.x * (pixel_size.x() + separation) - separation);
        double max_y_mm = (size.y * (pixel_size.y() + separation) - separation);
        float mm_per_pixel = std::max(max_x_mm / (GetSize().x - kCanvasBorderPx), max_y_mm / (GetSize().y - kCanvasBorderPx));

        //compute pixel per tile & per separation
        int pixels_separation = int(separation / mm_per_pixel) + (separation - int(separation / mm_per_pixel) > mm_per_pixel / kRoundingHalfDivisor ? 1 : 0);
        float ratiox = std::max(1.f, float(pixel_size.x() / pixel_size.y()));
        float ratioy = std::max(1.f, float(pixel_size.y() / pixel_size.x()));
        int pixels_reste_x = (GetSize().x - kCanvasBorderPx - pixels_separation * (size.x - 1));
        int pixels_reste_y = (GetSize().y - kCanvasBorderPx - pixels_separation * (size.y - 1));
        double pixels_per_tile_dbl_x = (pixels_reste_x / double(size.x * ratiox)); // (pixel_size.x() / pixel_size.y())));
        double pixels_per_tile_dbl_y = (pixels_reste_y / double(size.y * ratioy)); // (pixel_size.y() / pixel_size.x())));
        double pixels_per_tile = std::min(pixels_per_tile_dbl_x, pixels_per_tile_dbl_y);
        int pixels_per_tile_x = std::max(1, int(pixels_per_tile * ratiox));
        int pixels_per_tile_y = std::max(1, int(pixels_per_tile * ratioy));

        bool show_original = parent->m_config.option<ConfigOptionBool>(kOriginalKey)->value;
        const int color_algo = parent->m_config.option<ConfigOptionInt>(kColorComponentKey)->value;
        bool use_near_color = parent->m_config.option<ConfigOptionBool>(kNearColorKey)->value;
        bool use_spool_colors = parent->m_config.option<ConfigOptionBool>(kSpoolColorsKey)->value;
        wxColour background_color{ wxString{ parent->m_config.option<ConfigOptionString>(kBackgroundColorKey)->value } };

        parent->recompute_colors();

        std::unordered_map<int, wxBrush*> col2nearest;
        for (ColorEntry& c : parent->m_pixel_colors) {
            if (c.nb_pixels_real <= 0)
                continue;

            wxColour color = c.real_color;
            if (show_original) {
                col2nearest[c.real_color.GetRGB()] = wxTheBrushList->FindOrCreateBrush(c.real_color);
                continue;
            }

            // First check if there is an override.
            if (use_spool_colors && c.widget_spool && !c.widget_spool->is_auto())
                color = c.widget_spool->get_print_color()->get_printed_color(use_spool_colors);

            const int idx_extruder = nearest_used_color_index(
                parent->m_used_colors, color, color_algo, use_near_color, use_spool_colors);
            color = parent->m_used_colors[idx_extruder]->get_printed_color(use_spool_colors);
            col2nearest[c.real_color.GetRGB()] = wxTheBrushList->FindOrCreateBrush(color);
        }

        dc.SetBrush(*wxBLACK_BRUSH);
        dc.SetBrush(*col2nearest[background_color.GetRGB()]); //beckground color is in the m_pixel_colors list
        dc.DrawRectangle(
            kCanvasOriginOffsetPx,
            kCanvasOriginOffsetPx,
            size.x * (pixels_separation + pixels_per_tile_x) - pixels_separation,
            size.y * (pixels_separation + pixels_per_tile_y) - pixels_separation);
        
        wxNativePixelData data(bmp);
        wxNativePixelData::Iterator p(bmp, data);
        if (offset.y > 0)
            p.OffsetY(data, offset.y);
        for (int y = 0; y < size.y; ++y) {
            wxNativePixelData::Iterator rowStart = p;
            if (offset.x > 0)
                p.OffsetX(data, offset.x);
            for (int x = 0; x < size.x; x++, ++p) {
                wxColour color(p.Red(), p.Green(), p.Blue());
                dc.SetBrush(*col2nearest[color.GetRGB()]);
                dc.DrawRectangle(
                    kCanvasOriginOffsetPx + x * (pixels_separation + pixels_per_tile_x),
                    kCanvasOriginOffsetPx + y * (pixels_separation + pixels_per_tile_y),
                    pixels_per_tile_x,
                    pixels_per_tile_y);
            }
            p = rowStart;
            p.OffsetY(data, 1);
        }
    }

    void BasicDrawPane::loadImage(const std::string& path) {
        //wxImage image;
        //if (!image.LoadFile(Slic3r::GUI::from_u8(Slic3r::get_icon_file("C:/Users/VR-REMI/Downloads/1649109736.png")), wxBITMAP_TYPE_PNG) ||
        //    image.GetWidth() == 0 || image.GetHeight() == 0)
        //    return ;
        if (boost::filesystem::exists(path))
            this->bmp = wxBitmap(path, wxBITMAP_TYPE_PNG);
        else
            this->bmp = wxBitmap();
    }


void CreateMMUTiledCanvas::recompute_colors()
{

    wxBitmap& bmp = get_canvas()->bmp;
    if (!bmp.IsOk())
        return;
    if (bmp.GetSize().x <= 0 || bmp.GetSize().y <= 0)
        return;
    if (bmp.GetWidth() == 0 || bmp.GetHeight() == 0)
        return;

    Vec2d offset_dbl = m_config.option<ConfigOptionPoint>("offset")->value;
    wxSize offset(std::min(int(offset_dbl.x()), std::max(0, bmp.GetSize().x - 1)), std::min(int(offset_dbl.y()), std::max(0, bmp.GetSize().y - 1)));
    Vec2d size_dbl = m_config.option<ConfigOptionPoint>("size")->value;
    wxSize size(std::min(int(size_dbl.x()), bmp.GetSize().x - offset.x), std::min(int(size_dbl.y()), bmp.GetSize().y - offset.y));

    wxColour background_color{ wxString{ m_config.option<ConfigOptionString>(kBackgroundColorKey)->value } };

    for (ColorEntry& col : m_pixel_colors) {
        col.reset();
        col.printing_color = nullptr;
    }
    //create background color
    {
        int idx = -1;
        for (int i = 0; i < int(m_pixel_colors.size()); i++) {
            if (m_pixel_colors[i].real_color == background_color) {
                idx = i;
                break;
            }
        }
        if (idx < 0) {
            idx = m_pixel_colors.size();
            m_pixel_colors.emplace_back(background_color);
        }
        //as background, it's the most used extruder;
        m_pixel_colors[idx].nb_pixels_real = 100000;
    }
    // create colors & count pixels
    {
        wxNativePixelData data(bmp);
        wxNativePixelData::Iterator p(bmp, data);
        if (offset.y > 0)
            p.OffsetY(data, offset.y);
        for (int y = 0; y < size.y; ++y) {
            wxNativePixelData::Iterator rowStart = p;
            if (offset.x > 0)
                p.OffsetX(data, offset.x);
            for (int x = 0; x < size.x; x++, ++p) {
                wxColour color(p.Red(), p.Green(), p.Blue());
                m_pixel_colors[pixel_color_index(m_pixel_colors, color)].add_pixel();
            }
            p = rowStart;
            p.OffsetY(data, 1);
        }
    }

    // collect colors to merge
    bool use_spool = m_config.opt_bool(kSpoolColorsKey);
    std::vector<ColorEntry*> old_colors = m_used_colors;
    m_used_colors.clear();
    for (ColorEntry& col : m_pixel_colors) {
        if (col.nb_pixels_real > 0) {
            m_used_colors.push_back(&col);
        }
    }

    const int color_algo = m_config.option(kColorComponentKey)->get_int();
    bool use_near_color = m_config.option(kNearColorKey)->get_bool();
    //int nb_extruders = dynamic_cast<TabPrinter*>(m_gui_app->get_tab(Preset::TYPE_PRINTER))->m_extruders_count;
    int nb_extruders = m_config.option(kExtrudersKey)->get_int();
    // Re-order and fuse spool colors.
    auto assign_spool_colors = [&]() {
        for (ColorEntry& spool : m_spools)
            spool.reset();
        for (ColorEntry* c : m_used_colors) {
            if (c->widget_spool && !c->widget_spool->is_auto())
                c->printing_color = c->widget_spool->get_print_color();
            else
                c->printing_color = nearest_spool(m_spools, color_algo, use_spool, c->real_color);
            c->printing_color->nb_pixels_real += c->nb_pixels_sum;
        }

        m_used_colors.clear();
        for (ColorEntry& col : m_spools) {
            if (col.nb_pixels_real <= 0)
                continue;
            col.nb_pixels_sum = col.nb_pixels_real;
            m_used_colors.push_back(&col);
        }
    };
    if (use_spool)
        assign_spool_colors();

    // Merge until the configured extruder count is enough.
    std::sort(m_used_colors.begin(), m_used_colors.end(), [](ColorEntry* e1, ColorEntry* e2) { return e1->nb_pixels_sum > e2->nb_pixels_sum; });
    const auto nearest_color_index = [&]() {
        int nearest_index = 0;
        for (int i = 1; i + 1 < int(m_used_colors.size()); ++i) {
            if (color_dist(color_algo, m_used_colors[i]->real_color, m_used_colors.back()->real_color) <
                color_dist(color_algo, m_used_colors[nearest_index]->real_color, m_used_colors.back()->real_color))
                nearest_index = i;
        }
        return nearest_index;
    };
    while (use_near_color && m_used_colors.size() > size_t(nb_extruders)) {
        const int idx_extruder = nearest_color_index();
        m_used_colors[idx_extruder]->nb_pixels_sum += m_used_colors.back()->nb_pixels_sum;
        m_used_colors.back()->reset_merge();
        m_used_colors.back()->printing_color = m_used_colors[idx_extruder];
        m_used_colors.erase(m_used_colors.end() - 1);
        std::sort(m_used_colors.begin(), m_used_colors.end(), [](ColorEntry* e1, ColorEntry* e2) { return e1->nb_pixels_sum > e2->nb_pixels_sum; });
    }

    if (old_colors != m_used_colors) {
        refresh_description();
        recreate_color_conversion();
    }
}

} // namespace GUI
} // namespace Slic3r
