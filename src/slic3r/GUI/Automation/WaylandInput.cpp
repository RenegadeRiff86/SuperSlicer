// This file is part of SuperSlicer.
//
// SuperSlicer is free software: you can redistribute it and/or modify it under
// the terms of the GNU Affero General Public License as published by the Free
// Software Foundation, version 3 of the License.

#include "WaylandInput.hpp"

#include <libei.h>
#include <liboeffis.h>

#include <linux/input-event-codes.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <vector>

namespace Slic3r::GUI {

namespace {

using Clock = std::chrono::steady_clock;

constexpr char ERROR_OPERATION_FAILED[] = "operation_" "failed";
constexpr char ERROR_PERMISSION_REQUIRED[] = "permission_" "required";

WaylandInput::Result input_success()
{
    return { true, {}, {} };
}

WaylandInput::Result input_failure(const std::string& code, const std::string& message)
{
    return { false, code, message };
}

int remaining_timeout_ms(const Clock::time_point deadline)
{
    if (deadline <= Clock::now())
        return 0;
    return std::max(1, static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count()));
}

bool poll_readable(int fd, const Clock::time_point deadline, std::string& error)
{
    pollfd descriptor { fd, POLLIN, 0 };
    int result;
    do {
        result = ::poll(&descriptor, 1, remaining_timeout_ms(deadline));
    } while (result < 0 && errno == EINTR);

    if (result < 0) {
        error = std::strerror(errno);
        return false;
    }
    if (result == 0)
        return false;
    if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        error = "the emulated-input connection closed";
        return false;
    }
    return (descriptor.revents & POLLIN) != 0;
}

} // namespace

class WaylandInput::Impl
{
public:
    ~Impl()
    {
        stop();
    }

    Result prepare(const Request& request, const std::chrono::milliseconds connection_timeout)
    {
        std::lock_guard lock(m_mutex);
        return ensure_session(Clock::now() + connection_timeout, request);
    }

    Result perform(const Request& request, const std::chrono::milliseconds connection_timeout)
    {
        std::lock_guard lock(m_mutex);
        const Clock::time_point deadline = Clock::now() + connection_timeout;
        Result result = ensure_session(deadline, request);
        if (!result.ok)
            return result;

        pump_ei(Clock::now());
        if (!m_connection_error.empty())
            return input_failure(ERROR_OPERATION_FAILED, m_connection_error);

        if (request.type == "move")
            return move_pointer(request.x, request.y);
        if (request.type == "click")
            return click(request);
        if (request.type == "drag")
            return drag(request);
        if (request.type == "wheel")
            return wheel(request);
        if (request.type == "key")
            return key(request);
        if (request.type == "text")
            return text(request);
        return input_failure(ERROR_OPERATION_FAILED, "unsupported Wayland input type");
    }

    void stop()
    {
        std::lock_guard lock(m_mutex);
        stop_locked();
    }

    bool active() const
    {
        std::lock_guard lock(m_mutex);
        return m_ei != nullptr && m_connected && m_connection_error.empty();
    }

private:
    struct Device
    {
        ei_device* handle { nullptr };
        bool resumed { false };
        bool emulating { false };
    };

    Result ensure_session(const Clock::time_point deadline, const Request& request)
    {
        if (m_portal == nullptr) {
            m_portal = oeffis_new(nullptr);
            if (m_portal == nullptr)
                return input_failure(ERROR_OPERATION_FAILED, "could not create a RemoteDesktop portal context");
            oeffis_create_session(
                m_portal,
                static_cast<std::uint32_t>(OEFFIS_DEVICE_POINTER | OEFFIS_DEVICE_KEYBOARD));
        }

        while (!m_portal_connected) {
            std::string poll_error;
            if (!poll_readable(oeffis_get_fd(m_portal), deadline, poll_error)) {
                if (!poll_error.empty()) {
                    stop_locked();
                    return input_failure(ERROR_PERMISSION_REQUIRED, poll_error);
                }
                return input_failure(
                    ERROR_PERMISSION_REQUIRED,
                    "RemoteDesktop consent is pending; approve it and repeat the request");
            }

            oeffis_dispatch(m_portal);
            for (oeffis_event_type event = oeffis_get_event(m_portal);
                 event != OEFFIS_EVENT_NONE;
                 event = oeffis_get_event(m_portal)) {
                if (event == OEFFIS_EVENT_CONNECTED_TO_EIS)
                    m_portal_connected = true;
                else if (event == OEFFIS_EVENT_CLOSED) {
                    stop_locked();
                    return input_failure(ERROR_PERMISSION_REQUIRED, "RemoteDesktop consent was closed");
                } else if (event == OEFFIS_EVENT_DISCONNECTED) {
                    const char* message = oeffis_get_error_message(m_portal);
                    const std::string error = message == nullptr
                        ? "RemoteDesktop portal disconnected"
                        : message;
                    stop_locked();
                    return input_failure(ERROR_PERMISSION_REQUIRED, error);
                }
            }
        }

        if (m_ei == nullptr) {
            const int eis_fd = oeffis_get_eis_fd(m_portal);
            if (eis_fd < 0) {
                const std::string error = std::strerror(errno);
                stop_locked();
                return input_failure(ERROR_OPERATION_FAILED, error);
            }

            m_ei = ei_new_sender(nullptr);
            if (m_ei == nullptr) {
                ::close(eis_fd);
                stop_locked();
                return input_failure(ERROR_OPERATION_FAILED, "could not create a libei sender");
            }
            ei_configure_name(m_ei, "SuperSlicer model automation");
            const int setup_result = ei_setup_backend_fd(m_ei, eis_fd);
            if (setup_result < 0) {
                stop_locked();
                return input_failure(ERROR_OPERATION_FAILED, "could not connect libei to the portal");
            }
        }

        while (!has_devices_for(request)) {
            Result result = pump_ei(deadline);
            if (!result.ok)
                return result;
            if (Clock::now() >= deadline)
                return input_failure(ERROR_PERMISSION_REQUIRED, "the compositor has not provided the required input device yet");
        }
        return input_success();
    }

    void bind_seat(ei_event* event)
    {
        ei_seat_bind_capabilities(
            ei_event_get_seat(event),
            EI_DEVICE_CAP_POINTER,
            EI_DEVICE_CAP_POINTER_ABSOLUTE,
            EI_DEVICE_CAP_BUTTON,
            EI_DEVICE_CAP_SCROLL,
            EI_DEVICE_CAP_KEYBOARD,
            EI_DEVICE_CAP_TEXT,
            nullptr);
    }

    void resume_device(ei_device* handle)
    {
        Device* device = find_device(handle);
        if (device == nullptr)
            return;
        device->resumed = true;
        if (device->emulating)
            return;
        ei_device_start_emulating(device->handle, ++m_sequence);
        device->emulating = true;
    }

    void pause_device(ei_device* handle)
    {
        Device* device = find_device(handle);
        if (device == nullptr)
            return;
        device->resumed = false;
        device->emulating = false;
    }

    void remove_device(ei_device* handle)
    {
        const auto found = std::find_if(
            m_devices.begin(), m_devices.end(),
            [handle](const Device& candidate) { return candidate.handle == handle; });
        if (found == m_devices.end())
            return;
        ei_device_unref(found->handle);
        m_devices.erase(found);
    }

    void process_ei_event(ei_event* event)
    {
        ei_device* event_device = ei_event_get_device(event);
        switch (ei_event_get_type(event)) {
        case EI_EVENT_CONNECT:
            m_connected = true;
            break;
        case EI_EVENT_DISCONNECT:
            m_connection_error = "the compositor disconnected the emulated-input session";
            break;
        case EI_EVENT_SEAT_ADDED:
            bind_seat(event);
            break;
        case EI_EVENT_DEVICE_ADDED:
            m_devices.push_back({ ei_device_ref(event_device), false, false });
            break;
        case EI_EVENT_DEVICE_RESUMED:
            resume_device(event_device);
            break;
        case EI_EVENT_DEVICE_PAUSED:
            pause_device(event_device);
            break;
        case EI_EVENT_DEVICE_REMOVED:
            remove_device(event_device);
            break;
        default:
            break;
        }
    }

    Result pump_ei(const Clock::time_point deadline)
    {
        if (m_ei == nullptr)
            return input_failure(ERROR_OPERATION_FAILED, "libei is not connected");

        std::string poll_error;
        if (!poll_readable(ei_get_fd(m_ei), deadline, poll_error)) {
            if (!poll_error.empty()) {
                m_connection_error = poll_error;
                return input_failure(ERROR_OPERATION_FAILED, poll_error);
            }
            return input_success();
        }

        ei_dispatch(m_ei);
        for (ei_event* event = ei_get_event(m_ei); event != nullptr;
             event = ei_get_event(m_ei)) {
            process_ei_event(event);
            ei_event_unref(event);
        }

        if (!m_connection_error.empty())
            return input_failure(ERROR_OPERATION_FAILED, m_connection_error);
        return input_success();
    }

    Device* find_device(ei_device* handle)
    {
        const auto found = std::find_if(
            m_devices.begin(), m_devices.end(),
            [handle](const Device& candidate) { return candidate.handle == handle; });
        return found == m_devices.end() ? nullptr : &*found;
    }

    Device* device_with(ei_device_capability capability)
    {
        const auto found = std::find_if(
            m_devices.begin(), m_devices.end(),
            [capability](const Device& candidate) {
                return candidate.resumed && candidate.emulating &&
                       ei_device_has_capability(candidate.handle, capability);
            });
        return found == m_devices.end() ? nullptr : &*found;
    }

    bool has_devices_for(const Request& request)
    {
        if (!m_connected)
            return false;
        if (request.type == "move")
            return device_with(EI_DEVICE_CAP_POINTER_ABSOLUTE) != nullptr;
        if (request.type == "click" || request.type == "drag")
            return device_with(EI_DEVICE_CAP_POINTER_ABSOLUTE) != nullptr &&
                   device_with(EI_DEVICE_CAP_BUTTON) != nullptr;
        if (request.type == "wheel")
            return device_with(EI_DEVICE_CAP_POINTER_ABSOLUTE) != nullptr &&
                   device_with(EI_DEVICE_CAP_SCROLL) != nullptr;
        if (request.type == "key")
            return device_with(EI_DEVICE_CAP_KEYBOARD) != nullptr;
        if (request.type == "text")
            return device_with(EI_DEVICE_CAP_TEXT) != nullptr;
        return true;
    }

    void frame(Device& device)
    {
        ei_device_frame(device.handle, ei_now(m_ei));
    }

    Result move_pointer(double x, double y)
    {
        Device* pointer = device_with(EI_DEVICE_CAP_POINTER_ABSOLUTE);
        if (pointer == nullptr)
            return input_failure(ERROR_OPERATION_FAILED, "no absolute pointer device is available");
        ei_device_pointer_motion_absolute(pointer->handle, x, y);
        frame(*pointer);
        return input_success();
    }

    Result click(const Request& request)
    {
        Result moved = move_pointer(request.x, request.y);
        if (!moved.ok)
            return moved;

        Device* button = device_with(EI_DEVICE_CAP_BUTTON);
        if (button == nullptr)
            return input_failure(ERROR_OPERATION_FAILED, "no pointer button device is available");
        const std::uint32_t code = request.button == 0 ? BTN_LEFT : request.button;
        ei_device_button_button(button->handle, code, true);
        frame(*button);
        m_pressed_buttons.push_back(code);
        ei_device_button_button(button->handle, code, false);
        frame(*button);
        m_pressed_buttons.clear();
        return input_success();
    }

    Result drag(const Request& request)
    {
        Result moved = move_pointer(request.x, request.y);
        if (!moved.ok)
            return moved;

        Device* button = device_with(EI_DEVICE_CAP_BUTTON);
        if (button == nullptr)
            return input_failure(ERROR_OPERATION_FAILED, "no pointer button device is available");
        const std::uint32_t code = request.button == 0 ? BTN_LEFT : request.button;
        ei_device_button_button(button->handle, code, true);
        frame(*button);
        m_pressed_buttons.push_back(code);

        moved = move_pointer(request.end_x, request.end_y);
        ei_device_button_button(button->handle, code, false);
        frame(*button);
        m_pressed_buttons.clear();
        return moved;
    }

    Result wheel(const Request& request)
    {
        Result moved = move_pointer(request.x, request.y);
        if (!moved.ok)
            return moved;

        Device* scroll = device_with(EI_DEVICE_CAP_SCROLL);
        if (scroll == nullptr)
            return input_failure(ERROR_OPERATION_FAILED, "no scroll device is available");
        ei_device_scroll_delta(scroll->handle, request.scroll_x, request.scroll_y);
        ei_device_scroll_stop(scroll->handle, request.scroll_x != 0.0, request.scroll_y != 0.0);
        frame(*scroll);
        return input_success();
    }

    Result key(const Request& request)
    {
        if (request.keycode == 0)
            return input_failure(ERROR_OPERATION_FAILED, "key input requires an evdev keycode");
        Device* keyboard = device_with(EI_DEVICE_CAP_KEYBOARD);
        if (keyboard == nullptr)
            return input_failure(ERROR_OPERATION_FAILED, "no keyboard device is available");

        ei_device_keyboard_key(keyboard->handle, request.keycode, true);
        frame(*keyboard);
        m_pressed_keys.push_back(request.keycode);
        ei_device_keyboard_key(keyboard->handle, request.keycode, false);
        frame(*keyboard);
        m_pressed_keys.clear();
        return input_success();
    }

    Result text(const Request& request)
    {
        Device* text_device = device_with(EI_DEVICE_CAP_TEXT);
        if (text_device == nullptr)
            return input_failure(ERROR_OPERATION_FAILED, "no compositor text-input device is available");
        ei_device_text_utf8_with_length(
            text_device->handle, request.text.data(), request.text.size());
        frame(*text_device);
        return input_success();
    }

    void release_pressed()
    {
        if (m_ei == nullptr)
            return;
        if (Device* button = device_with(EI_DEVICE_CAP_BUTTON); button != nullptr) {
            for (std::uint32_t code : m_pressed_buttons)
                ei_device_button_button(button->handle, code, false);
            if (!m_pressed_buttons.empty())
                frame(*button);
        }
        if (Device* keyboard = device_with(EI_DEVICE_CAP_KEYBOARD); keyboard != nullptr) {
            for (std::uint32_t code : m_pressed_keys)
                ei_device_keyboard_key(keyboard->handle, code, false);
            if (!m_pressed_keys.empty())
                frame(*keyboard);
        }
        m_pressed_buttons.clear();
        m_pressed_keys.clear();
    }

    void stop_locked()
    {
        release_pressed();
        for (Device& device : m_devices) {
            if (device.emulating)
                ei_device_stop_emulating(device.handle);
            ei_device_unref(device.handle);
        }
        m_devices.clear();

        if (m_ei != nullptr) {
            ei_disconnect(m_ei);
            m_ei = ei_unref(m_ei);
        }
        if (m_portal != nullptr)
            m_portal = oeffis_unref(m_portal);

        m_portal_connected = false;
        m_connected = false;
        m_connection_error.clear();
    }

    mutable std::mutex m_mutex;
    oeffis* m_portal { nullptr };
    ei* m_ei { nullptr };
    std::vector<Device> m_devices;
    std::vector<std::uint32_t> m_pressed_buttons;
    std::vector<std::uint32_t> m_pressed_keys;
    std::uint32_t m_sequence { 0 };
    bool m_portal_connected { false };
    bool m_connected { false };
    std::string m_connection_error;
};

WaylandInput::WaylandInput()
    : m_impl(std::make_unique<Impl>())
{
}

WaylandInput::~WaylandInput() = default;

WaylandInput::Result WaylandInput::prepare(
    const Request& request, const std::chrono::milliseconds connection_timeout)
{
    return m_impl->prepare(request, connection_timeout);
}

WaylandInput::Result WaylandInput::perform(
    const Request& request, const std::chrono::milliseconds connection_timeout)
{
    return m_impl->perform(request, connection_timeout);
}

void WaylandInput::stop()
{
    m_impl->stop();
}

bool WaylandInput::active() const
{
    return m_impl->active();
}

} // namespace Slic3r::GUI
