// This file is part of SuperSlicer.
// SuperSlicer is released under the terms of the AGPLv3 or higher.

#include "AutomationIds.hpp"

#include <wx/menuitem.h>
#include <wx/string.h>

#include <cctype>

namespace Slic3r::GUI::AutomationIds {

namespace {

constexpr char PREFIX[] = "superslicer.";

} // namespace

std::string slug(const wxString& label)
{
    const wxString text = wxMenuItem::GetLabelText(label);
    const wxScopedCharBuffer utf8 = text.utf8_str();
    std::string fragment;
    fragment.reserve(utf8.length());
    for (std::size_t i = 0; i < utf8.length(); ++i) {
        const auto character = static_cast<unsigned char>(utf8.data()[i]);
        if (std::isalnum(character) != 0)
            fragment.push_back(static_cast<char>(std::tolower(character)));
        else if (!fragment.empty() && fragment.back() != '_')
            fragment.push_back('_');
    }
    while (!fragment.empty() && fragment.back() == '_')
        fragment.pop_back();
    return fragment;
}

std::string option(const std::string& key, int index)
{
    std::string id = std::string(PREFIX) + "option." + key;
    if (index >= 0)
        id += "#" + std::to_string(index);
    return id;
}

std::string tab(const wxString& caption)
{
    return std::string(PREFIX) + "tab." + slug(caption);
}

} // namespace Slic3r::GUI::AutomationIds
