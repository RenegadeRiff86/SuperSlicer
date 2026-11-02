// This file is part of SuperSlicer.
// SuperSlicer is released under the terms of the AGPLv3 or higher.
#pragma once

#include <string>

namespace Slic3r::GUI::AutomationSecurity {

std::string lower_ascii(const std::string& input);
bool constant_time_equal(const std::string& lhs, const std::string& rhs);
bool local_authority(const std::string& input);
bool local_origin(const std::string& origin);
bool bearer_authorized(const std::string& authorization, const std::string& token);

} // namespace Slic3r::GUI::AutomationSecurity
