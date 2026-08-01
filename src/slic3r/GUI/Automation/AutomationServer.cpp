///|/ Modified 2026 by Stan Elston (RenegadeRiff86) -- see git history.
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher.

#include "AutomationServer.hpp"
#include "AutomationFileDialog.hpp"
#include "AutomationIds.hpp"
#include "AutomationSecurity.hpp"
#include "WaylandInput.hpp"

#include "../DoubleSlider.hpp"
#include "../GLCanvas3D.hpp"
#include "../Widgets/ComboBox.hpp"
#include "../Widgets/SpinInput.hpp"
#include "../Widgets/TextInput.hpp"
#include "../GUI_App.hpp"
#include "../GUI_ObjectManipulation.hpp"
#include "../MainFrame.hpp"
#include "../Plater.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"

#include <nlohmann/json.hpp>

#include <set>

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/combobox.h>
#include <wx/dialog.h>
#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/glcanvas.h>
#include <wx/image.h>
#include <wx/menu.h>
#include <wx/menuitem.h>
#include <wx/mstream.h>
#include <wx/utils.h>
#include <wx/slider.h>
#include <wx/spinctrl.h>
#include <wx/textctrl.h>
#include <wx/tglbtn.h>
#include <wx/toplevel.h>
#include <wx/weakref.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <future>
#include <limits>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <unistd.h>

namespace Slic3r::GUI {

namespace asio  = boost::asio;
namespace beast = boost::beast;
namespace http  = beast::http;
using tcp       = asio::ip::tcp;
using json      = nlohmann::json;
using Clock     = std::chrono::steady_clock;

namespace {

constexpr std::size_t MAX_REQUEST_BODY = 1024 * 1024;
constexpr auto SOCKET_TIMEOUT = std::chrono::seconds(5);
constexpr auto DEFAULT_GUI_TIMEOUT = std::chrono::seconds(10);
constexpr double ZERO_COORDINATE = 0.0;
constexpr int UNBOUNDED_PREVIEW_RANGE = std::numeric_limits<int>::max();
using CanvasScreenshot = ThumbnailData;
using SpectacleArguments = std::array<std::string, 6>;
using SpectacleArgumentPointers = std::array<const char*, 7>;

constexpr char ERROR_DISABLED[] = "dis" "abled";
constexpr char ERROR_OPERATION_FAILED[] = "operation_" "failed";
constexpr char ERROR_STALE_REF[] = "stale_" "ref";
constexpr char ERROR_TARGET_NOT_FOREGROUND[] = "target_not_" "foreground";
constexpr char FIELD_ACCEPTED[] = "acc" "epted";
constexpr char FIELD_OPERATION_ID[] = "operation_" "id";
constexpr char FIELD_REQUEST_ID[] = "request_" "id";
constexpr char TOOL_ARM_FILE_DIALOG[] = "superslicer_" "arm_file_dialog";
constexpr char TOOL_BATCH[] = "superslicer_" "batch";
constexpr char TOOL_FILE_DIALOG_STATUS[] = "superslicer_" "file_dialog_status";
constexpr char TOOL_INPUT[] = "superslicer_" "input";
constexpr char TOOL_STATUS[] = "superslicer_" "status";
constexpr char TOOL_UI_SCREENSHOT[] = "superslicer_ui_" "screenshot";
constexpr char TOOL_UI_SNAPSHOT[] = "superslicer_ui_" "snapshot";
constexpr char TOOL_WAIT[] = "superslicer_" "wait";

json failure(const std::string& code, const std::string& message, const std::string& request_id = {})
{
    json result = {
        { "ok", false },
        { "error", {
            { "code", code },
            { "message", message }
        } }
    };
    if (!request_id.empty())
        result[FIELD_REQUEST_ID] = request_id;
    return result;
}

json success(json value = json::object(), const std::string& request_id = {})
{
    json result = {
        { "ok", true },
        { "result", std::move(value) }
    };
    if (!request_id.empty())
        result[FIELD_REQUEST_ID] = request_id;
    return result;
}

using AutomationSecurity::lower_ascii;

std::string base64_encode(const unsigned char* data, std::size_t size)
{
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string encoded;
    encoded.reserve(((size + 2) / 3) * 4);
    for (std::size_t offset = 0; offset < size; offset += 3) {
        const std::uint32_t first  = data[offset];
        const std::uint32_t second = offset + 1 < size ? data[offset + 1] : 0;
        const std::uint32_t third  = offset + 2 < size ? data[offset + 2] : 0;
        const std::uint32_t block  = (first << 16) | (second << 8) | third;

        encoded.push_back(alphabet[(block >> 18) & 0x3f]);
        encoded.push_back(alphabet[(block >> 12) & 0x3f]);
        encoded.push_back(offset + 1 < size ? alphabet[(block >> 6) & 0x3f] : '=');
        encoded.push_back(offset + 2 < size ? alphabet[block & 0x3f] : '=');
    }
    return encoded;
}

// SuperSlicer's settings fields are composite widgets rather than plain wx controls:
// a TextInput or SpinInput paints its own frame around an inner wxTextCtrl, and that
// inner control is where the value lives and where the field hangs its wxEVT_TEXT /
// wxEVT_TEXT_ENTER bindings. Automation addresses the composite, so unwrap it before
// reading or writing. A ComboBox derives from TextInput but is driven with select,
// so it never unwraps to free text.
wxTextCtrl* text_control_for(wxWindow* window)
{
    if (auto* text = dynamic_cast<wxTextCtrl*>(window))
        return text;
    if (dynamic_cast<ComboBox*>(window) != nullptr)
        return nullptr;
    if (auto* spin = dynamic_cast<SpinInputBase*>(window))
        return spin->GetText();
    if (auto* input = dynamic_cast<TextInput*>(window))
        return input->GetTextCtrl();
    return nullptr;
}

// A caller may send 4, 0.2, or "0.2" for the same field; the control takes text.
std::string value_as_text(const json& value)
{
    return value.is_string() ? value.get<std::string>() : value.dump();
}

std::string window_role(wxWindow* window)
{
    if (dynamic_cast<wxDialog*>(window) != nullptr)
        return "dialog";
    if (dynamic_cast<wxTopLevelWindow*>(window) != nullptr)
        return "window";
    if (dynamic_cast<wxToggleButton*>(window) != nullptr)
        return "toggle_button";
    if (dynamic_cast<wxButton*>(window) != nullptr)
        return "button";
    if (dynamic_cast<wxCheckBox*>(window) != nullptr)
        return "checkbox";
    if (dynamic_cast<DoubleSlider::Control*>(window) != nullptr || dynamic_cast<wxSlider*>(window) != nullptr)
        return "slider";
    if (dynamic_cast<wxSpinCtrlDouble*>(window) != nullptr || dynamic_cast<wxSpinCtrl*>(window) != nullptr)
        return "spinbutton";
    if (dynamic_cast<wxChoice*>(window) != nullptr || dynamic_cast<wxComboBox*>(window) != nullptr)
        return "combobox";
    // ComboBox derives from TextInput, so it has to be tested first.
    if (dynamic_cast<ComboBox*>(window) != nullptr)
        return "combobox";
    if (dynamic_cast<SpinInputBase*>(window) != nullptr)
        return "spinbutton";
    if (dynamic_cast<TextInput*>(window) != nullptr)
        return "textbox";
    if (dynamic_cast<wxTextCtrl*>(window) != nullptr)
        return "textbox";
    if (dynamic_cast<wxGLCanvas*>(window) != nullptr)
        return "canvas";
    return "control";
}

json window_value(wxWindow* window)
{
    if (auto* control = dynamic_cast<DoubleSlider::Control*>(window))
        return {
            { "minimum", control->GetMinValue() },
            { "maximum", control->GetMaxValue() },
            { "lower", control->GetLowerValue() },
            { "higher", control->GetHigherValue() }
        };
    if (auto* slider = dynamic_cast<wxSlider*>(window))
        return slider->GetValue();
    if (auto* checkbox = dynamic_cast<wxCheckBox*>(window))
        return checkbox->GetValue();
    if (auto* toggle = dynamic_cast<wxToggleButton*>(window))
        return toggle->GetValue();
    if (auto* spin = dynamic_cast<wxSpinCtrl*>(window))
        return spin->GetValue();
    if (auto* spin = dynamic_cast<wxSpinCtrlDouble*>(window))
        return spin->GetValue();
    if (auto* choice = dynamic_cast<wxChoice*>(window))
        return {
            { "selection", choice->GetSelection() },
            { "text", into_u8(choice->GetStringSelection()) }
        };
    if (auto* combo = dynamic_cast<wxComboBox*>(window))
        return {
            { "selection", combo->GetSelection() },
            { "text", into_u8(combo->GetValue()) }
        };
    if (auto* combo = dynamic_cast<ComboBox*>(window))
        return {
            { "selection", combo->GetSelection() },
            { "text", into_u8(combo->GetValue()) }
        };
    if (auto* spin = dynamic_cast<SpinInputBase*>(window))
        return into_u8(spin->GetTextValue());
    if (auto* input = dynamic_cast<TextInput*>(window))
        return into_u8(input->GetValue());
    if (auto* text = dynamic_cast<wxTextCtrl*>(window)) {
        if ((text->GetWindowStyleFlag() & wxTE_PASSWORD) != 0)
            return json();
        return into_u8(text->GetValue());
    }
    return json();
}

json supported_actions(wxWindow* window)
{
    json actions = json::array({ "focus" });
    if (dynamic_cast<wxButton*>(window) != nullptr)
        actions.push_back("invoke");
    if (dynamic_cast<wxCheckBox*>(window) != nullptr || dynamic_cast<wxToggleButton*>(window) != nullptr)
        actions.push_back("toggle");
    if (dynamic_cast<DoubleSlider::Control*>(window) != nullptr ||
        dynamic_cast<wxSlider*>(window) != nullptr ||
        dynamic_cast<wxSpinCtrl*>(window) != nullptr ||
        dynamic_cast<wxSpinCtrlDouble*>(window) != nullptr ||
        text_control_for(window) != nullptr)
        actions.push_back("set_value");
    if (dynamic_cast<wxChoice*>(window) != nullptr ||
        dynamic_cast<wxComboBox*>(window) != nullptr ||
        dynamic_cast<ComboBox*>(window) != nullptr)
        actions.push_back("select");
    return actions;
}

bool is_descendant_or_self(wxWindow* window, wxWindow* ancestor)
{
    for (wxWindow* current = window; current != nullptr; current = current->GetParent())
        if (current == ancestor)
            return true;
    return false;
}

} // namespace

enum class AutomationElementKind
{
    Window,
    SliderLowerHandle,
    SliderUpperHandle,
    CanvasRegion,
    View3DSelector,
    ViewPreviewSelector,
    MenuItem
};

struct AutomationRegistryEntry
{
    wxWeakRef<wxWindow> window;
    AutomationElementKind kind { AutomationElementKind::Window };
    std::string automation_id;
    // Menu items are not windows, so a MenuItem entry is anchored to the frame
    // that owns the menu bar and carries the command id it stands for.
    int command_id { wxID_NONE };
};

struct ResolvedAutomationElement
{
    wxWindow* window { nullptr };
    AutomationElementKind kind { AutomationElementKind::Window };
    std::string automation_id;
    int command_id { wxID_NONE };
};

class AutomationServer::Impl
{
public:
    explicit Impl(GUI_App& app) : m_app(app) {}

    ~Impl()
    {
        stop();
    }

    bool start(Config config, std::string& error)
    {
        if (m_running.load()) {
            if (config.port == m_port)
                return true;
            error = "automation API is already running on a different port";
            return false;
        }
        if (config.token.empty()) {
            error = "SUPERSLICER_AUTOMATION_TOKEN is not set";
            return false;
        }

        beast::error_code ec;
        m_acceptor = std::make_unique<tcp::acceptor>(m_io);
        ec = m_acceptor->open(tcp::v4(), ec);
        if (ec) {
            error = "failed to create loopback listener: " + ec.message();
            m_acceptor.reset();
            return false;
        }

        ec = m_acceptor->set_option(asio::socket_base::reuse_address(true), ec);
        if (ec) {
            error = "failed to configure loopback listener: " + ec.message();
            m_acceptor.reset();
            return false;
        }

        ec = m_acceptor->bind(tcp::endpoint(asio::ip::address_v4::loopback(), config.port), ec);
        if (ec) {
            error = "cannot bind 127.0.0.1:" + std::to_string(config.port) + ": " + ec.message();
            m_acceptor.reset();
            return false;
        }

        ec = m_acceptor->listen(asio::socket_base::max_listen_connections, ec);
        if (ec) {
            error = "failed to listen on 127.0.0.1:" + std::to_string(config.port) + ": " + ec.message();
            m_acceptor.reset();
            return false;
        }

        m_token = std::move(config.token);
        m_port = config.port;
        m_stopping.store(false);
        m_running.store(true);
        m_network_thread = std::thread([this] { serve(); });
        return true;
    }

    void stop()
    {
        if (!m_running.exchange(false) && !m_network_thread.joinable())
            return;

        m_stopping.store(true);
        m_wayland_input.stop();
        beast::error_code ignored;
        if (m_acceptor != nullptr) {
            ignored = m_acceptor->cancel(ignored);
            ignored = m_acceptor->close(ignored);
        }

        std::shared_ptr<beast::tcp_stream> stream;
        {
            std::lock_guard lock(m_stream_mutex);
            stream = m_active_stream;
        }
        if (stream != nullptr) {
            ignored = stream->socket().cancel(ignored);
            ignored = stream->socket().shutdown(tcp::socket::shutdown_both, ignored);
            ignored = stream->socket().close(ignored);
        }

        if (m_network_thread.joinable() && m_network_thread.get_id() != std::this_thread::get_id())
            m_network_thread.join();

        {
            std::lock_guard lock(m_registry_mutex);
            m_registry.clear();
            m_registry_generation = 0;
        }
        {
            std::lock_guard lock(m_operation_mutex);
            m_operations.clear();
        }
        {
            std::lock_guard lock(m_dedup_mutex);
            m_completed_requests.clear();
        }

        m_acceptor.reset();
        m_active_stream.reset();
        m_token.clear();
        m_port = 0;
        m_io.restart();
    }

    bool running() const noexcept { return m_running.load(); }
    std::uint16_t port() const noexcept { return m_port; }

private:
    struct Operation
    {
        std::string kind;
        Clock::time_point created;
    };

    void serve()
    {
        while (!m_stopping.load()) {
            auto stream = std::make_shared<beast::tcp_stream>(m_io);
            {
                std::lock_guard lock(m_stream_mutex);
                m_active_stream = stream;
            }

            beast::error_code ec;
            ec = m_acceptor->accept(stream->socket(), ec);
            if (ec) {
                if (!m_stopping.load())
                    m_last_network_error = ec.message();
                break;
            }

            stream->expires_after(SOCKET_TIMEOUT);
            beast::flat_buffer buffer;
            http::request_parser<http::string_body> parser;
            parser.body_limit(MAX_REQUEST_BODY);
            http::read(*stream, buffer, parser, ec);
            if (!ec) {
                http::response<http::string_body> response = handle_http(parser.release());
                stream->expires_after(SOCKET_TIMEOUT);
                http::write(*stream, response, ec);
            }

            ec = stream->socket().shutdown(tcp::socket::shutdown_both, ec);
            ec = stream->socket().close(ec);
            {
                std::lock_guard lock(m_stream_mutex);
                if (m_active_stream == stream)
                    m_active_stream.reset();
            }
        }
    }

    http::response<http::string_body> json_response(
        http::status status,
        const json& body,
        unsigned version,
        const std::string& request_id = {}) const
    {
        http::response<http::string_body> response(status, version);
        response.set(http::field::server, "SuperSlicer-Automation");
        response.set(http::field::content_type, "application/json");
        response.set(http::field::cache_control, "no-store");
        response.set("X-Content-Type-Options", "nosniff");
        if (!request_id.empty())
            response.set("X-Request-ID", request_id);
        response.keep_alive(false);
        response.body() = body.dump();
        response.prepare_payload();
        return response;
    }

    http::response<http::string_body> empty_response(http::status status, unsigned version) const
    {
        http::response<http::string_body> response(status, version);
        response.set(http::field::server, "SuperSlicer-Automation");
        response.set(http::field::cache_control, "no-store");
        response.keep_alive(false);
        response.prepare_payload();
        return response;
    }

    std::string next_request_id()
    {
        const std::uint64_t value = ++m_request_sequence;
        return "ss-" + std::to_string(::getpid()) + "-" + std::to_string(value);
    }

    bool authorized(const http::request<http::string_body>& request) const
    {
        const auto authorization = request.find(http::field::authorization);
        return authorization != request.end() &&
               AutomationSecurity::bearer_authorized(
                   std::string(authorization->value()), m_token);
    }

    bool valid_host_and_origin(const http::request<http::string_body>& request) const
    {
        const auto host = request.find(http::field::host);
        if (host == request.end() ||
            !AutomationSecurity::local_authority(std::string(host->value())))
            return false;

        const auto origin = request.find(http::field::origin);
        return origin == request.end() ||
               AutomationSecurity::local_origin(std::string(origin->value()));
    }

    http::response<http::string_body> handle_http(const http::request<http::string_body>& request)
    {
        const unsigned version = request.version();
        std::string request_id;
        if (const auto header = request.find("X-Request-ID"); header != request.end()) {
            request_id = std::string(header->value());
            if (request_id.size() > 128 ||
                std::any_of(request_id.begin(), request_id.end(), [](unsigned char ch) { return std::iscntrl(ch) != 0; }))
                request_id.clear();
        }
        if (request_id.empty())
            request_id = next_request_id();

        if (!m_running.load())
            return json_response(http::status::service_unavailable, failure(ERROR_DISABLED, "automation API is disabled", request_id), version, request_id);
        if (!valid_host_and_origin(request))
            return json_response(http::status::forbidden, failure("unauthorized", "non-local Host or Origin rejected", request_id), version, request_id);
        if (!authorized(request))
            return json_response(http::status::unauthorized, failure("unauthorized", "bearer authentication failed", request_id), version, request_id);

        std::string target(request.target());
        if (const std::size_t query = target.find('?'); query != std::string::npos)
            target.resize(query);

        json body = json::object();
        if (!request.body().empty()) {
            body = json::parse(request.body(), nullptr, false);
            if (body.is_discarded())
                return json_response(http::status::bad_request, failure(ERROR_OPERATION_FAILED, "malformed JSON request", request_id), version, request_id);
        }

        if (target == "/mcp")
            return handle_mcp(request, body, request_id);

        std::string tool;
        if (request.method() == http::verb::get && target == "/api/v1/status")
            tool = TOOL_STATUS;
        else if (request.method() == http::verb::get && target == "/api/v1/ui/snapshot")
            tool = TOOL_UI_SNAPSHOT;
        else if (request.method() == http::verb::get && target == "/api/v1/ui/screenshot")
            tool = TOOL_UI_SCREENSHOT;
        else if (request.method() == http::verb::post && target == "/api/v1/ui/action")
            tool = "superslicer_ui_action";
        else if (request.method() == http::verb::post && target == "/api/v1/input")
            tool = TOOL_INPUT;
        else if (request.method() == http::verb::post && target == "/api/v1/wait")
            tool = TOOL_WAIT;
        else if (request.method() == http::verb::post && target == "/api/v1/batch")
            tool = TOOL_BATCH;
        else if (request.method() == http::verb::post && target.starts_with("/api/v1/workflows/"))
            tool = workflow_tool(target.substr(std::string("/api/v1/workflows/").size()));
        else
            return json_response(http::status::not_found, failure(ERROR_OPERATION_FAILED, "unknown automation endpoint", request_id), version, request_id);

        if (tool.empty())
            return json_response(http::status::not_found, failure(ERROR_OPERATION_FAILED, "unknown workflow", request_id), version, request_id);

        json result = call_service(tool, body, request_id);
        const http::status status = result.value("ok", false) ? http::status::ok : status_for_error(result);
        return json_response(status, result, version, request_id);
    }

    static json mcp_initialize_result()
    {
        json tools_capability;
        tools_capability["listChanged"] = false;
        json capabilities;
        capabilities["tools"] = std::move(tools_capability);
        json server_info;
        server_info["name"] = "superslicer-automation";
        server_info["version"] = "1.0.0";
        return {
            { "protocolVersion", "2025-06-18" },
            { "capabilities", std::move(capabilities) },
            { "serverInfo", std::move(server_info) },
            { "instructions", "Authenticated, loopback-only semantic automation for the current SuperSlicer process." }
        };
    }

    static json mcp_tool_call_result(json service_result)
    {
        json content_item;
        content_item["type"] = "text";
        content_item["text"] = service_result.dump();
        json result;
        result["content"] = json::array({ std::move(content_item) });
        result["isError"] = !service_result.value("ok", false);
        result["structuredContent"] = std::move(service_result);
        return result;
    }

    http::response<http::string_body> handle_mcp(
        const http::request<http::string_body>& request,
        const json& message,
        const std::string& request_id)
    {
        const unsigned version = request.version();
        if (request.method() != http::verb::post)
            return json_response(http::status::method_not_allowed, failure(ERROR_OPERATION_FAILED, "MCP accepts POST requests only", request_id), version, request_id);
        if (!message.is_object() || message.value("jsonrpc", std::string()) != "2.0")
            return json_response(http::status::bad_request, {
                { "jsonrpc", "2.0" },
                { "id", nullptr },
                { "error", { { "code", -32600 }, { "message", "Invalid Request" } } }
            }, version, request_id);

        const json id = message.contains("id") ? message["id"] : json();
        const std::string method = message.value("method", std::string());
        if (method == "notifications/initialized")
            return empty_response(http::status::accepted, version);

        json result;
        if (method == "initialize") {
            result = mcp_initialize_result();
        } else if (method == "ping") {
            result = json::object();
        } else if (method == "tools/list") {
            result = { { "tools", tool_definitions() } };
        } else if (method == "tools/call") {
            const json params = message.value("params", json::object());
            const std::string name = params.value("name", std::string());
            const json arguments = params.value("arguments", json::object());
            if (!known_tool(name))
                return json_response(http::status::ok, {
                    { "jsonrpc", "2.0" },
                    { "id", id },
                    { "error", { { "code", -32602 }, { "message", "Unknown tool" } } }
                }, version, request_id);

            result = mcp_tool_call_result(
                call_service(name, arguments, request_id));
        } else {
            return json_response(http::status::ok, {
                { "jsonrpc", "2.0" },
                { "id", id },
                { "error", { { "code", -32601 }, { "message", "Method not found" } } }
            }, version, request_id);
        }

        return json_response(http::status::ok, {
            { "jsonrpc", "2.0" },
            { "id", id },
            { "result", std::move(result) }
        }, version, request_id);
    }

    static http::status status_for_error(const json& value)
    {
        const std::string code = value.value("error", json::object()).value("code", std::string());
        if (code == "unauthorized")
            return http::status::unauthorized;
        if (code == ERROR_DISABLED)
            return http::status::service_unavailable;
        if (code == ERROR_STALE_REF || code == "modal_blocked")
            return http::status::conflict;
        if (code == "permission_required" || code == ERROR_TARGET_NOT_FOREGROUND)
            return http::status::forbidden;
        if (code == "timeout")
            return http::status::gateway_timeout;
        if (code == "expired")
            return http::status::request_timeout;
        return http::status::bad_request;
    }

    static std::string workflow_tool(const std::string& operation)
    {
        static const std::map<std::string, std::string> tools = {
            { "load_model", "superslicer_load_model" },
            { "slice", "superslicer_slice" },
            { "select_view", "superslicer_select_view" },
            { "set_preview", "superslicer_set_preview" },
            { "set_transform", "superslicer_set_transform" },
            { "export_gcode", "superslicer_export_gcode" },
            { "arm_file_dialog", "superslicer_arm_file_dialog" },
            { "file_dialog_status", "superslicer_file_dialog_status" }
        };
        const auto found = tools.find(operation);
        return found == tools.end() ? std::string() : found->second;
    }

    static bool known_tool(const std::string& name)
    {
        static const std::vector<std::string> names = {
            TOOL_STATUS,
            TOOL_UI_SNAPSHOT,
            TOOL_UI_SCREENSHOT,
            "superslicer_ui_action",
            TOOL_INPUT,
            TOOL_WAIT,
            TOOL_BATCH,
            "superslicer_load_model",
            "superslicer_slice",
            "superslicer_select_view",
            "superslicer_set_preview",
            "superslicer_set_transform",
            "superslicer_export_gcode",
            TOOL_ARM_FILE_DIALOG,
            TOOL_FILE_DIALOG_STATUS
        };
        return std::find(names.begin(), names.end(), name) != names.end();
    }

    static bool mutating_tool(const std::string& name)
    {
        return name != TOOL_STATUS &&
               name != TOOL_UI_SNAPSHOT &&
               name != TOOL_UI_SCREENSHOT &&
               name != TOOL_WAIT &&
               name != TOOL_FILE_DIALOG_STATUS;
    }

    static json tool_definitions()
    {
        const json object_schema = {
            { "type", "object" },
            { "additionalProperties", true }
        };
        const auto tool = [&object_schema](const char* name, const char* description, json schema = json()) {
            return json {
                { "name", name },
                { "description", description },
                { "inputSchema", schema.is_null() ? object_schema : std::move(schema) }
            };
        };

        return json::array({
            tool(TOOL_STATUS, "Return process identity, API state, GUI readiness, and slicing/export state."),
            tool(TOOL_UI_SNAPSHOT, "Return a generation-scoped semantic snapshot of the active SuperSlicer window or owned modal dialog."),
            tool(TOOL_UI_SCREENSHOT, "Capture only the SuperSlicer main window, active owned dialog, or registered canvas."),
            tool("superslicer_ui_action", "Invoke, set, select, toggle, or focus a generation-scoped semantic UI element."),
            tool(TOOL_INPUT, "Send wx events or consent-gated Wayland input using element-relative coordinates."),
            tool(TOOL_WAIT, "Wait for an asynchronous slicing or export operation with a bounded timeout."),
            tool(TOOL_BATCH, "Run a bounded sequence of automation calls in order."),
            tool("superslicer_load_model", "Load a model through the normal plater GUI handler."),
            tool("superslicer_slice", "Start slicing and return an operation_id."),
            tool("superslicer_select_view", "Select the 3D or Preview panel."),
            tool("superslicer_set_preview", "Set preview layer or move slider ranges."),
            tool("superslicer_set_transform", "Commit selected-object position, rotation, or scale."),
            tool("superslicer_export_gcode", "Export G-code to an explicit path with overwrite protection."),
            tool("superslicer_arm_file_dialog", "Queue the answer for the next file dialog, so an action that opens one can run unattended. Arm before triggering it."),
            tool("superslicer_file_dialog_status", "Report the file dialog the app raised most recently: title, wildcard, save or open, and the paths returned.")
        });
    }

    json call_service(const std::string& tool, const json& arguments, const std::string& request_id)
    {
        if (!m_running.load())
            return failure(ERROR_DISABLED, "automation API is disabled", request_id);

        if (mutating_tool(tool)) {
            std::lock_guard lock(m_dedup_mutex);
            const auto found = m_completed_requests.find(request_id);
            if (found != m_completed_requests.end())
                return found->second;
        }

        json result;
        if (tool == TOOL_WAIT)
            result = wait_for_operation(arguments, request_id);
        else if (tool == TOOL_BATCH)
            result = run_batch(arguments, request_id);
        else if (tool == TOOL_INPUT && arguments.value("backend", std::string("wx")) == "wayland")
            result = wayland_input(arguments, request_id);
        else
            result = call_gui(tool, arguments, request_id);

        if (mutating_tool(tool)) {
            std::lock_guard lock(m_dedup_mutex);
            if (m_completed_requests.size() >= 256)
                m_completed_requests.erase(m_completed_requests.begin());
            m_completed_requests[request_id] = result;
        }
        return result;
    }

    json wayland_input(const json& arguments, const std::string& request_id)
    {
        json target = call_gui("__wayland_target", arguments, request_id);
        if (!target.value("ok", false))
            return target;

        const json target_result = target["result"];
        WaylandInput::Request request;
        request.type = arguments.value("type", std::string());
        request.text = arguments.value("text", std::string());
        request.x = target_result["point"].value("x", ZERO_COORDINATE);
        request.y = target_result["point"].value("y", ZERO_COORDINATE);
        request.end_x = target_result["end_point"].value("x", request.x);
        request.end_y = target_result["end_point"].value("y", request.y);
        request.scroll_x = arguments.value("scroll_x", ZERO_COORDINATE);
        request.scroll_y = arguments.contains("scroll_y")
            ? arguments.value("scroll_y", ZERO_COORDINATE)
            : -static_cast<double>(arguments.value("rotation", 0));
        request.button = arguments.value("button", 0U);
        request.keycode = arguments.value("key_code", 0U);

        const int consent_timeout_ms =
            std::clamp(arguments.value("consent_timeout_ms", 250), 0, 3000);
        WaylandInput::Result prepared = m_wayland_input.prepare(
            request, std::chrono::milliseconds(consent_timeout_ms));
        if (!prepared.ok)
            return failure(prepared.code, prepared.message, request_id);

        json verified = call_gui("__wayland_target", arguments, request_id);
        if (!verified.value("ok", false))
            return verified;
        if (verified["result"]["fingerprint"] != target_result["fingerprint"])
            return failure(
                ERROR_TARGET_NOT_FOREGROUND,
                "the target focus or bounds changed while preparing Wayland input",
                request_id);

        WaylandInput::Result performed =
            m_wayland_input.perform(request, std::chrono::milliseconds(0));
        if (!performed.ok)
            return failure(performed.code, performed.message, request_id);
        return success({ { FIELD_ACCEPTED, true }, { "backend", "wayland" } }, request_id);
    }

    json call_gui(const std::string& tool, const json& arguments, const std::string& request_id)
    {
        int timeout_ms = arguments.value("timeout_ms", static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(DEFAULT_GUI_TIMEOUT).count()));
        timeout_ms = std::clamp(timeout_ms, 50, 30000);
        const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);

        struct Pending
        {
            std::promise<json> promise;
            std::atomic_bool cancelled { false };
        };
        auto pending = std::make_shared<Pending>();
        std::future<json> future = pending->promise.get_future();

        m_app.CallAfter([this, pending, tool, arguments, request_id, deadline] {
            if (pending->cancelled.load())
                return;
            if (!m_running.load()) {
                pending->promise.set_value(failure(ERROR_DISABLED, "automation API is disabled", request_id));
                return;
            }
            if (Clock::now() > deadline) {
                pending->promise.set_value(failure("expired", "request expired before reaching the GUI thread", request_id));
                return;
            }
            try {
                pending->promise.set_value(dispatch_gui(tool, arguments, request_id));
            } catch (...) {
                pending->promise.set_value(failure(ERROR_OPERATION_FAILED, "GUI automation action failed", request_id));
            }
        });

        while (Clock::now() < deadline) {
            if (!m_running.load()) {
                pending->cancelled.store(true);
                return failure(ERROR_DISABLED, "automation API is disabled", request_id);
            }
            if (future.wait_for(std::chrono::milliseconds(20)) == std::future_status::ready)
                return future.get();
        }

        pending->cancelled.store(true);
        return failure("timeout", "GUI thread did not complete the request before its deadline", request_id);
    }

    json dispatch_gui(const std::string& tool, const json& arguments, const std::string& request_id)
    {
        if (tool == TOOL_STATUS)
            return gui_status(request_id);
        if (tool == TOOL_UI_SNAPSHOT)
            return gui_snapshot(arguments, request_id);
        if (tool == TOOL_UI_SCREENSHOT)
            return gui_screenshot(arguments, request_id);
        if (tool == "superslicer_ui_action")
            return gui_action(arguments, request_id);
        if (tool == TOOL_INPUT)
            return gui_input(arguments, request_id);
        if (tool == "superslicer_load_model")
            return gui_load_model(arguments, request_id);
        if (tool == "superslicer_slice")
            return gui_slice(request_id);
        if (tool == "superslicer_select_view")
            return gui_select_view(arguments, request_id);
        if (tool == "superslicer_set_preview")
            return gui_set_preview(arguments, request_id);
        if (tool == "superslicer_set_transform")
            return gui_set_transform(arguments, request_id);
        if (tool == "superslicer_export_gcode")
            return gui_export_gcode(arguments, request_id);
        if (tool == TOOL_ARM_FILE_DIALOG)
            return gui_arm_file_dialog(arguments, request_id);
        if (tool == TOOL_FILE_DIALOG_STATUS)
            return gui_file_dialog_status(request_id);
        if (tool == "__operation_status")
            return gui_operation_status(arguments, request_id);
        if (tool == "__wayland_target")
            return gui_wayland_target(arguments, request_id);
        return failure(ERROR_OPERATION_FAILED, "unknown service operation", request_id);
    }

    json gui_status(const std::string& request_id)
    {
        Plater* plater = m_app.plater();
        json status = {
            { "pid", ::getpid() },
            { "api_enabled", m_running.load() },
            { "port", m_port },
            { "bind_address", "127.0.0.1" },
            { "gui_ready", m_app.initialized() && m_app.mainframe != nullptr && plater != nullptr },
            { "app_mode", m_app.is_editor() ? "editor" : "gcode_viewer" },
            { "slicing", plater != nullptr && plater->is_background_process_running() },
            { "exporting", plater != nullptr && plater->is_export_gcode_scheduled() },
            { "preview_loaded", plater != nullptr && plater->is_preview_loaded() },
            { "last_network_error", m_last_network_error }
        };
        return success(std::move(status), request_id);
    }

    #include "AutomationServerGui.inl"

private:
    GUI_App& m_app;
    asio::io_context m_io;
    WaylandInput m_wayland_input;
    std::unique_ptr<tcp::acceptor> m_acceptor;
    std::shared_ptr<beast::tcp_stream> m_active_stream;
    std::mutex m_stream_mutex;
    std::thread m_network_thread;

    std::atomic_bool m_running { false };
    std::atomic_bool m_stopping { false };
    std::uint16_t m_port { 0 };
    std::string m_token;
    std::string m_last_network_error;
    std::atomic_uint64_t m_request_sequence { 0 };
    std::atomic_uint64_t m_operation_sequence { 0 };

    std::mutex m_registry_mutex;
    std::uint64_t m_registry_generation { 0 };
    std::unordered_map<std::string, AutomationRegistryEntry> m_registry;

    std::mutex m_operation_mutex;
    std::unordered_map<std::string, Operation> m_operations;

    std::mutex m_dedup_mutex;
    std::map<std::string, json> m_completed_requests;
};

AutomationServer::AutomationServer(GUI_App& app)
    : m_impl(std::make_unique<Impl>(app))
{
}

AutomationServer::~AutomationServer() = default;

bool AutomationServer::start(Config config, std::string& error)
{
    return m_impl->start(std::move(config), error);
}

void AutomationServer::stop()
{
    m_impl->stop();
}

bool AutomationServer::running() const noexcept
{
    return m_impl->running();
}

std::uint16_t AutomationServer::port() const noexcept
{
    return m_impl->port();
}

} // namespace Slic3r::GUI
