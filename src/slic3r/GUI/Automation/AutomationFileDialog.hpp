// This file is part of SuperSlicer.
// SuperSlicer is released under the terms of the AGPLv3 or higher.
#pragma once

#include <wx/filedlg.h>

#include <cstddef>
#include <string>
#include <vector>

namespace Slic3r::GUI {

// What a file dialog was asked to show, captured as it opens. Auditing a
// "File > ..." menu item is mostly a question of whether it raised the RIGHT
// dialog, so the title, the wildcard and the default name are the answer --
// reading those back beats a screenshot nobody can assert on.
struct FileDialogRecord
{
    std::string title;
    std::string wildcard;
    std::string directory;
    std::string filename;
    bool        save { false };        // wxFD_SAVE was set
    bool        multiple { false };    // wxFD_MULTIPLE was set
    bool        intercepted { false }; // an armed response answered it
    int         return_code { 0 };
    std::vector<std::string> paths;
};

// An answer queued for a dialog that has not opened yet. The next dialog whose
// title contains title_contains -- empty matches whichever opens first --
// returns return_code and reports paths instead of opening.
struct FileDialogResponse
{
    std::string title_contains;
    int         return_code { wxID_CANCEL };
    std::vector<std::string> paths;
    int         filter_index { 0 };
    bool        checkbox { false }; // for dialogs carrying an extra checkbox
};

// wxFileDialog with an automation seam.
//
// The GTK file chooser is not a wx widget tree, so the automation API cannot
// see inside it or click its buttons: an unattended run that reaches one stops
// dead at a dialog nothing can answer. So this does not drive the native
// dialog, it answers it -- when a response has been armed the dialog never
// opens and the armed values come back through the normal getters.
//
// With nothing armed this is exactly wxFileDialog, which is why every file
// dialog in the app should be one of these: a plain wxFileDialog is invisible
// to automation and silently blocks it.
class FileDialog : public wxFileDialog
{
public:
    using wxFileDialog::wxFileDialog;

    int      ShowModal() override;
    wxString GetPath() const override;
    void     GetPaths(wxArrayString& paths) const override;
    wxString GetFilename() const override;
    void     GetFilenames(wxArrayString& files) const override;
    wxString GetDirectory() const override;
    int      GetFilterIndex() const override;

    // True when an armed response answered this dialog instead of a person.
    bool automation_intercepted() const { return m_intercepted; }
    // The extra-control checkbox value that response asked for.
    bool automation_checkbox() const { return m_response.checkbox; }

    // Queue one answer. Armed responses are consumed in the order they match,
    // so a test can queue several before triggering a multi-dialog action.
    static void        arm(FileDialogResponse response);
    static void        disarm_all();
    static std::size_t armed_count();

    // The dialog that opened most recently, if one has since the last clear.
    static bool last_record(FileDialogRecord& record);
    static void clear_record();

private:
    bool               m_intercepted { false };
    FileDialogResponse m_response;
};

} // namespace Slic3r::GUI
