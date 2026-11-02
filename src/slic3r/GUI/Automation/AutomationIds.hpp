// This file is part of SuperSlicer.
// SuperSlicer is released under the terms of the AGPLv3 or higher.
#pragma once

#include <string>

class wxString;

namespace Slic3r::GUI {

// The identity strings the automation API reports as an element's automation_id.
// Real controls carry theirs in wxWindow::GetName(), so they are set where the
// control is created; elements that own no window (menu items, view selectors)
// are named by the snapshot builder. Both go through here so one scheme covers
// the whole surface.
namespace AutomationIds {

// Folds a user-visible label into a stable id fragment: the mnemonic and the
// accelerator ("&Slice now\tCtrl+R") are dropped, everything else is lowercased
// and runs of punctuation collapse to a single '_'. The fragment follows the
// displayed text, so a translated UI yields translated ids.
std::string slug(const wxString& label);

// A settings field: "superslicer.option.<key>", or "superslicer.option.<key>#<n>"
// for one element of a vector option. index is negative for scalars.
std::string option(const std::string& key, int index);

// A tab button in the main notebook: "superslicer.tab.<slug>".
std::string tab(const wxString& caption);

// A named control on a calibration dialog: "superslicer.calibration.<fragment>".
// fragment is a stable English id, not a translated label, so scripts keep working
// when the UI language changes.
std::string calibration(const std::string& fragment);

} // namespace AutomationIds
} // namespace Slic3r::GUI
