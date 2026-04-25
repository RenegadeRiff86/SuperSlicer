///|/ Copyright (c) Prusa Research 2018 - 2021 Lukáš Matěna @lukasmatena, Oleksandra Iushchenko @YuSanka, Vojtěch Bubník @bubnikv
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_GUI_Utils_FixModelByWin10_hpp_
#define slic3r_GUI_Utils_FixModelByWin10_hpp_

#include <string>

#include <wx/string.h>

class wxProgressDialog;

namespace Slic3r {

class Model;
class ModelObject;
class Print;

#ifdef HAS_WIN10SDK

extern bool is_windows10();

#else /* HAS_WIN10SDK */

inline bool is_windows10() { return false; }

#endif /* HAS_WIN10SDK */

// Return false if fixing was canceled.
extern bool fix_model_by_repair_gui(ModelObject &model_object, int volume_idx, wxProgressDialog& progress_dlg, const wxString& msg_header, std::string& fix_result);

// Legacy wrapper kept for callers that still use the Windows-specific name.
inline bool fix_model_by_win10_sdk_gui(ModelObject &model_object, int volume_idx, wxProgressDialog& progress_dlg, const wxString& msg_header, std::string& fix_result)
{
    return fix_model_by_repair_gui(model_object, volume_idx, progress_dlg, msg_header, fix_result);
}

} // namespace Slic3r

#endif /* slic3r_GUI_Utils_FixModelByWin10_hpp_ */
