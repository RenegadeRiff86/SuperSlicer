// This file is part of SuperSlicer.
//
// SuperSlicer is free software: you can redistribute it and/or modify it under
// the terms of the GNU Affero General Public License as published by the Free
// Software Foundation, version 3 of the License.

#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace Slic3r::GUI {

class WaylandInput
{
public:
    struct Request
    {
        std::string type;
        std::string text;
        double x {};
        double y {};
        double end_x {};
        double end_y {};
        double scroll_x {};
        double scroll_y {};
        std::uint32_t button {};
        std::uint32_t keycode {};
    };

    struct Result
    {
        bool ok { false };
        std::string code;
        std::string message;
    };

    WaylandInput();
    ~WaylandInput();

    WaylandInput(const WaylandInput&) = delete;
    WaylandInput& operator=(const WaylandInput&) = delete;

    Result prepare(const Request& request, std::chrono::milliseconds connection_timeout);
    Result perform(const Request& request, std::chrono::milliseconds connection_timeout);
    void stop();
    bool active() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace Slic3r::GUI
