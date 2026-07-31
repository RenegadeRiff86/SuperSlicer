///|/ Modified 2026 by Stan Elston (RenegadeRiff86) -- see git history.
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace Slic3r::GUI {

class GUI_App;

// Loopback-only HTTP/MCP server used by developer automation. The implementation
// owns its network thread; every GUI access is marshalled back to wx's main thread.
class AutomationServer final
{
public:
    struct Config
    {
        std::uint16_t port { 43127 };
        std::string token;
    };

    explicit AutomationServer(GUI_App& app);
    ~AutomationServer();

    AutomationServer(const AutomationServer&) = delete;
    AutomationServer& operator=(const AutomationServer&) = delete;

    bool start(Config config, std::string& error);
    void stop();

    bool running() const noexcept;
    std::uint16_t port() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace Slic3r::GUI
