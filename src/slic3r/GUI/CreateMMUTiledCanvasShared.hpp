#pragma once

#include "libslic3r/Color.hpp"

#include <wx/colour.h>

namespace Slic3r::GUI::CreateMMUTiledCanvasDetail {

constexpr char kColorComponentKey[]  = "color_comp";
constexpr char kNearColorKey[]       = "near_color";
constexpr char kBackgroundColorKey[] = "background_color";
constexpr char kExtruderKey[]        = "extruder";
constexpr char kExtrudersKey[]       = "extruders";
constexpr char kSeparationXyKey[]    = "separation_xy";
constexpr char kSpoolColorsKey[]     = "spool_colors";
constexpr char kOriginalKey[]        = "original";
constexpr char kAvailableColorsKey[] = "available_colors";
constexpr char kOrderDarkKey[]       = "order_dark";
constexpr char kSeparationZKey[]     = "separation_z";

// r/place dialog + canvas layout (shared meanings, not identity arithmetic)
constexpr long   kRPlaceEpochUnix        = 1648764000; // 2022-04-01 00:00:00 UTC
constexpr int    kSecondsPerMinute       = 60;
constexpr int    kSecondsPerHour         = 3600;
constexpr int    kSecondsPerDay          = 86400;
constexpr int    kHoursPerDay            = 24;
constexpr int    kSpinCtrlWidthPx        = 80;
constexpr int    kSpinCtrlHeightPx       = 20;
constexpr int    kCanvasBorderPx         = 4;  // panel margin reserved around the tile grid
constexpr int    kCanvasOriginOffsetPx   = 2;  // DrawRectangle origin inset from panel edge
constexpr int    kConfigFieldWidthChars  = 4;
constexpr int    kBluePerceptualHalf     = 2;  // blue channel weighted half as much as R/G
constexpr int    kRoundingHalfDivisor    = 2;  // half-up residual for pixel conversion
constexpr double kMinSeparationZMm       = 0.01;
constexpr double kPlaceholderCubeEdgeMm  = 0.01;
constexpr int    kPyramidVertex2          = 2;
constexpr int    kPyramidVertex3          = 3;
constexpr int    kPyramidVertex4          = 4;
constexpr int    kPyramidVertex5          = 5;
constexpr int    kPyramidVertex6          = 6;
constexpr int    kPyramidVertex7          = 7;
constexpr float  kGeometryCenterDivisor   = 2.0f;
constexpr int    kGridCellBorder          = 2;
constexpr int    kWideColumnSpan          = 2;
constexpr int    kExpandableColumn        = 2;
constexpr int    kBorderSideCount         = 2;
constexpr int    kTabsColumnSpan          = 3;
constexpr int    kCloseButtonColumn       = 3;
constexpr int    kCanvasRowSpan           = 3;
constexpr int    kColorSettingsRow        = 3;
constexpr int    kExtruderCountRow        = 4;
constexpr int    kColorConversionRow       = 4;
constexpr int    kDefaultDay              = 5;
constexpr int    kMaximumDay              = 6;
constexpr double kDefaultPointCoordinate  = 5.0;
constexpr int    kCloseButtonBorder       = 5;
constexpr int    kButtonColumnSpan        = 6;
constexpr int    kAcceleratorEntryCount   = 6;
constexpr int    kHueDistanceMethod       = 2;
constexpr int    kDialogFirstInputColumn  = 2;
constexpr int    kDialogActionRow         = 2;
constexpr int    kDialogShortcutCount     = 2;
constexpr int    kOptionsColumn           = 2;
constexpr int    kSizeOptionsRow          = 2;
constexpr int    kCanvasStartRow          = 2;
constexpr int    kColorPickerRow          = 2;
constexpr int    kHueDegrees              = 360;
constexpr int    kPercentScale            = 100;
constexpr int    kDrawPaneMinSizePx       = 404;
constexpr int    kDefaultTilePixels       = 32;
constexpr int    kDefaultOffsetXPx        = 140;
constexpr int    kDefaultOffsetYPx        = 330;
constexpr double kDefaultSeparationZMm    = 0.6;
constexpr int    kMediumScreenMinWidth    = 1500;
constexpr int    kMediumScreenMinHeight   = 1000;
constexpr int    kLargeScreenMinWidth     = 2000;
constexpr int    kLargeScreenMinHeight    = 1400;
constexpr int    kMediumDialogWidthPx     = 1200;
constexpr int    kMediumDialogHeightPx    = 700;
constexpr int    kLargeDialogWidthPx      = 1600;
constexpr int    kLargeDialogHeightPx     = 1000;
constexpr int    kFilenameFieldMinWidthPx = 500;
constexpr int    kComboItemHeightPx       = 40;
constexpr int    kComboItemWidthPx        = 120;

inline int color_dist_hue(wxColour col1, wxColour col2) {

        uint32_t int_color = col1.GetRGB();
        ColorRGB rgb_color = int2rgb(int_color);
        hsv hsv_color1 = rgb2hsv(rgb_color);

        int_color = col2.GetRGB();
        rgb_color = int2rgb(int_color);
        hsv hsv_color2 = rgb2hsv(rgb_color);

        int dist = 0;
        if (hsv_color1.h > hsv_color2.h)
            dist = std::min(hsv_color1.h - hsv_color2.h, hsv_color2.h - hsv_color1.h + kHueDegrees);
        else
            dist = std::min(hsv_color2.h - hsv_color1.h, hsv_color1.h - hsv_color2.h + kHueDegrees);
        dist += std::abs(hsv_color1.s - hsv_color2.s) * kPercentScale;
        dist += std::abs(hsv_color1.v - hsv_color2.v) * kPercentScale;

        return dist;
    }

    inline int color_dist_std(wxColour col1, wxColour col2) {
        int dist = std::abs(col1.Red() - col2.Red());
        dist += std::abs(col1.Green() - col2.Green());
        dist += std::abs(col1.Blue() - col2.Blue());
        return dist;
    }

    inline int color_dist_not_blue(wxColour col1, wxColour col2) {
        int dist = std::abs(col1.Red() - col2.Red());
        dist += std::abs(col1.Green() - col2.Green());
        //blue is less perceptible by humans
        dist += std::abs(col1.Blue() - col2.Blue()) / kBluePerceptualHalf;
        return dist;
    }

    inline int color_dist(int method, wxColour col1, wxColour col2) {
        if (method == 1)
            return color_dist_not_blue(col1, col2);
        if (method == kHueDistanceMethod)
            return color_dist_hue(col1, col2);
        return color_dist_std(col1,col2);
    }

} // namespace Slic3r::GUI::CreateMMUTiledCanvasDetail
