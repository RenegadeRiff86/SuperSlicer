// This file is part of SuperSlicer.
// SuperSlicer is released under the terms of the AGPLv3 or higher.
#include "AutomationFileDialog.hpp"

#include <wx/filename.h>

#include <deque>
#include <mutex>
#include <optional>
#include <utility>

namespace Slic3r::GUI {

namespace {

// The queue is process-wide because a file dialog is raised from wherever the
// action happens to live, and only the automation server ever fills it. Guarded
// because the server's network thread arms while the GUI thread consumes.
std::mutex                      g_mutex;
std::deque<FileDialogResponse>  g_armed;
std::optional<FileDialogRecord> g_last;

std::string to_utf8(const wxString& text)
{
    return std::string(text.ToUTF8().data());
}

// Takes the first response whose filter matches, not strictly the first queued,
// so a test can arm answers for several dialogs without having to know the order
// the app happens to raise them in.
std::optional<FileDialogResponse> take_response(const std::string& title)
{
    std::lock_guard lock(g_mutex);
    for (auto it = g_armed.begin(); it != g_armed.end(); ++it) {
        if (it->title_contains.empty() || title.find(it->title_contains) != std::string::npos) {
            FileDialogResponse response = std::move(*it);
            g_armed.erase(it);
            return response;
        }
    }
    return std::nullopt;
}

} // namespace

int FileDialog::ShowModal()
{
    FileDialogRecord record;
    record.title     = to_utf8(GetMessage());
    record.wildcard  = to_utf8(GetWildcard());
    record.directory = to_utf8(wxFileDialog::GetDirectory());
    record.save      = HasFdFlag(wxFD_SAVE);
    record.multiple  = HasFdFlag(wxFD_MULTIPLE);
    if (!record.multiple)
        record.filename = to_utf8(wxFileDialog::GetFilename());

    std::optional<FileDialogResponse> response = take_response(record.title);
    if (response.has_value()) {
        // Answering instead of showing also skips wx's extension fixup and its
        // overwrite prompt. That is deliberate: the armed path is used exactly as
        // given, so a test gets the file it named rather than one wx adjusted.
        m_response         = std::move(*response);
        m_intercepted      = true;
        record.intercepted = true;
        record.return_code = m_response.return_code;
        record.paths       = m_response.paths;
    } else {
        record.return_code = wxFileDialog::ShowModal();
        if (record.return_code == wxID_OK) {
            wxArrayString paths;
            wxFileDialog::GetPaths(paths);
            for (const wxString& path : paths)
                record.paths.push_back(to_utf8(path));
        }
    }

    {
        std::lock_guard lock(g_mutex);
        g_last = record;
    }
    return record.return_code;
}

wxString FileDialog::GetPath() const
{
    if (!m_intercepted)
        return wxFileDialog::GetPath();
    wxCHECK_MSG(!HasFlag(wxFD_MULTIPLE), wxString(),
        "When using wxFD_MULTIPLE, must call GetPaths() instead");
    return m_response.paths.empty() ? wxString() : wxString::FromUTF8(m_response.paths.front());
}

void FileDialog::GetPaths(wxArrayString& paths) const
{
    if (!m_intercepted) {
        wxFileDialog::GetPaths(paths);
        return;
    }
    paths.Empty();
    for (const std::string& path : m_response.paths)
        paths.Add(wxString::FromUTF8(path));
}

wxString FileDialog::GetFilename() const
{
    if (!m_intercepted)
        return wxFileDialog::GetFilename();
    wxCHECK_MSG(!HasFlag(wxFD_MULTIPLE), wxString(),
        "When using wxFD_MULTIPLE, must call GetFilenames() instead");
    return m_response.paths.empty()
        ? wxString()
        : wxFileName(wxString::FromUTF8(m_response.paths.front())).GetFullName();
}

void FileDialog::GetFilenames(wxArrayString& files) const
{
    if (!m_intercepted) {
        wxFileDialog::GetFilenames(files);
        return;
    }
    files.Empty();
    for (const std::string& path : m_response.paths)
        files.Add(wxFileName(wxString::FromUTF8(path)).GetFullName());
}

wxString FileDialog::GetDirectory() const
{
    // Callers remember this as the next dialog's starting folder, so an armed
    // answer has to report the folder it actually chose, not the default.
    if (!m_intercepted || m_response.paths.empty())
        return wxFileDialog::GetDirectory();
    return wxFileName(wxString::FromUTF8(m_response.paths.front())).GetPath();
}

int FileDialog::GetFilterIndex() const
{
    return m_intercepted ? m_response.filter_index : wxFileDialog::GetFilterIndex();
}

void FileDialog::arm(FileDialogResponse response)
{
    std::lock_guard lock(g_mutex);
    g_armed.push_back(std::move(response));
}

void FileDialog::disarm_all()
{
    std::lock_guard lock(g_mutex);
    g_armed.clear();
}

std::size_t FileDialog::armed_count()
{
    std::lock_guard lock(g_mutex);
    return g_armed.size();
}

bool FileDialog::last_record(FileDialogRecord& record)
{
    std::lock_guard lock(g_mutex);
    if (!g_last.has_value())
        return false;
    record = *g_last;
    return true;
}

void FileDialog::clear_record()
{
    std::lock_guard lock(g_mutex);
    g_last.reset();
}

} // namespace Slic3r::GUI
