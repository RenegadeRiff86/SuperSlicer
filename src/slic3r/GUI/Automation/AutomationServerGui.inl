// GUI-thread automation implementation included inside AutomationServer::Impl.
// Kept separate from the HTTP/MCP transport to keep both responsibilities navigable.

    wxWindow* active_scope() const
    {
        wxWindow* main = m_app.mainframe;
        if (main == nullptr)
            return nullptr;

        for (wxWindow* top : wxTopLevelWindows) {
            auto* dialog = dynamic_cast<wxDialog*>(top);
            if (dialog != nullptr && dialog->IsShown() && dialog->IsModal() &&
                is_descendant_or_self(dialog, main))
                return dialog;
        }
        return main;
    }

    // A modal dialog owns the UI, so active_scope() makes it the whole scope. But
    // SuperSlicer's calibration dialogs are deliberately NOT modal (they live in
    // GUI_App::not_modal_dialog and are shown with Show()): they sit beside the main
    // window, which stays usable. Those are extra roots rather than a new scope,
    // otherwise a snapshot would have to hide half the application to show a dialog.
    std::vector<wxWindow*> snapshot_roots(wxWindow* scope) const
    {
        std::vector<wxWindow*> roots { scope };
        if (scope != m_app.mainframe)
            return roots;
        // Every shown dialog the application owns is a root, modal or not. A top-level dialog is
        // reachable upwards through GetParent() but is NOT part of its parent's GetChildren(), so
        // walking the scope alone never descends into one - that is why a "Delete all" confirmation
        // was absent from the snapshot and could not be answered. Do not filter on IsModal() here
        // either: active_scope() already claims the ones wx reports as modal, and the ones it does
        // not report would otherwise fall through both branches and vanish. collect_windows()
        // de-duplicates, so naming a window that the walk also reaches costs nothing.
        for (wxWindow* top : wxTopLevelWindows) {
            auto* dialog = dynamic_cast<wxDialog*>(top);
            if (dialog == nullptr || dialog == scope || !dialog->IsShown())
                continue;
            if (is_descendant_or_self(dialog, m_app.mainframe))
                roots.push_back(dialog);
        }
        return roots;
    }

    bool within_snapshot_roots(wxWindow* window, wxWindow* scope) const
    {
        for (wxWindow* root : snapshot_roots(scope))
            if (is_descendant_or_self(window, root))
                return true;
        return false;
    }

    void collect_windows(wxWindow* window, bool include_hidden, std::vector<wxWindow*>& output,
                         std::set<wxWindow*>& seen) const
    {
        if (window == nullptr || !seen.insert(window).second)
            return;
        if (include_hidden || window->IsShownOnScreen())
            output.push_back(window);
        for (wxWindow* child : window->GetChildren())
            collect_windows(child, include_hidden, output, seen);
    }

    wxRect element_client_rect(
        wxWindow* window, AutomationElementKind kind) const
    {
        if (auto* slider = dynamic_cast<DoubleSlider::Control*>(window)) {
            if (kind == AutomationElementKind::SliderLowerHandle) {
                const wxRect rect = slider->GetThumbRect(DoubleSlider::ssLower);
                if (!rect.IsEmpty())
                    return rect;
            } else if (kind == AutomationElementKind::SliderUpperHandle) {
                const wxRect rect = slider->GetThumbRect(DoubleSlider::ssHigher);
                if (!rect.IsEmpty())
                    return rect;
            }
        }
        return wxRect(wxPoint(0, 0), window->GetClientSize());
    }

    void append_snapshot_element(
        json& elements,
        std::size_t& ordinal,
        wxWindow* window,
        AutomationElementKind kind,
        const std::string& automation_id,
        const std::string& role,
        const std::string& name,
        const wxRect& screen_bounds,
        json value,
        json actions,
        const wxMenuItem* menu_item = nullptr)
    {
        const std::string ref =
            std::to_string(m_registry_generation) + ":" +
            std::to_string(++ordinal);
        m_registry.emplace(ref, AutomationRegistryEntry {
            wxWeakRef<wxWindow>(window), kind, automation_id,
            menu_item != nullptr ? menu_item->GetId() : wxID_NONE
        });
        // A menu item owns no window, so it borrows the frame it hangs off for
        // visibility while its own enabled state has to be read from the item.
        const bool enabled =
            menu_item != nullptr ? menu_item->IsEnabled() : window->IsEnabled();
        elements.push_back({
            { "ref", ref },
            { "automation_id", automation_id },
            { "role", role },
            { "name", name },
            { "state", {
                { "shown", window->IsShownOnScreen() },
                { "enabled", enabled },
                { "focused", menu_item == nullptr && wxWindow::FindFocus() == window }
            } },
            { "bounds", {
                { "x", screen_bounds.x },
                { "y", screen_bounds.y },
                { "width", screen_bounds.width },
                { "height", screen_bounds.height }
            } },
            { "value", std::move(value) },
            { "actions", std::move(actions) }
        });
    }

    void append_virtual_snapshot_elements(
        json& elements,
        std::size_t& ordinal,
        wxWindow* window,
        const std::string& automation_id)
    {
        if (automation_id.empty())
            return;
        const auto append_virtual = [&](AutomationElementKind kind,
                                        const std::string& suffix,
                                        const std::string& role,
                                        const std::string& name,
                                        json value,
                                        json actions) {
            const wxRect client_bounds = element_client_rect(window, kind);
            const wxRect screen_bounds(
                window->ClientToScreen(client_bounds.GetPosition()),
                client_bounds.GetSize());
            append_snapshot_element(
                elements, ordinal, window, kind, automation_id + suffix,
                role, name, screen_bounds, std::move(value), std::move(actions));
        };
        if (auto* slider = dynamic_cast<DoubleSlider::Control*>(window)) {
            append_virtual(
                AutomationElementKind::SliderLowerHandle,
                ".lower_handle", "slider_handle", "Lower range handle",
                slider->GetLowerValue(), json::array({ "focus", "set_value", "input" }));
            append_virtual(
                AutomationElementKind::SliderUpperHandle,
                ".upper_handle", "slider_handle", "Upper range handle",
                slider->GetHigherValue(), json::array({ "focus", "set_value", "input" }));
        } else if (dynamic_cast<wxGLCanvas*>(window) != nullptr) {
            append_virtual(
                AutomationElementKind::CanvasRegion,
                ".scene", "canvas_region", "Canvas scene region",
                nullptr, json::array({ "focus", "input" }));
        }
    }

    void append_view_selectors(
        json& elements, std::size_t& ordinal, wxWindow* scope)
    {
        if (scope != m_app.mainframe)
            return;
        const wxRect semantic_bounds;
        append_snapshot_element(
            elements, ordinal, scope, AutomationElementKind::View3DSelector,
            "superslicer.view.select.3d", "button", "Select 3D view",
            semantic_bounds, nullptr, json::array({ "invoke" }));
        append_snapshot_element(
            elements, ordinal, scope, AutomationElementKind::ViewPreviewSelector,
            "superslicer.view.select.preview", "button", "Select sliced preview",
            semantic_bounds, nullptr, json::array({ "invoke" }));
    }

    void append_menu_elements(
        json& elements,
        std::size_t& ordinal,
        wxWindow* scope,
        wxMenu* menu,
        const std::string& prefix)
    {
        std::map<std::string, int> used_fragments;
        for (const wxMenuItem* item : menu->GetMenuItems()) {
            if (item->IsSeparator())
                continue;
            std::string fragment = AutomationIds::slug(item->GetItemLabel());
            if (fragment.empty())
                fragment = "item";
            const int seen = ++used_fragments[fragment];
            if (seen > 1)
                fragment += "_" + std::to_string(seen);
            const std::string automation_id = prefix + "." + fragment;
            if (item->IsSubMenu()) {
                append_menu_elements(
                    elements, ordinal, scope, item->GetSubMenu(), automation_id);
                continue;
            }
            json value;
            if (item->IsCheckable())
                value = item->IsChecked();
            append_snapshot_element(
                elements, ordinal, scope, AutomationElementKind::MenuItem,
                automation_id,
                item->IsCheckable() ? "menu_check_item" : "menu_item",
                into_u8(wxMenuItem::GetLabelText(item->GetItemLabel())),
                wxRect(), std::move(value), json::array({ "invoke" }), item);
        }
    }

    void append_menu_bar(json& elements, std::size_t& ordinal, wxWindow* scope)
    {
        if (scope != m_app.mainframe)
            return;
        wxMenuBar* menu_bar = m_app.mainframe->GetMenuBar();
        if (menu_bar == nullptr)
            return;
        for (std::size_t index = 0; index < menu_bar->GetMenuCount(); ++index) {
            wxMenu* menu = menu_bar->GetMenu(index);
            // Enable and check state live in wxEVT_UPDATE_UI handlers that only
            // run when the user opens the menu; drive them here so the snapshot
            // reports what invoking the item would actually find.
            menu->UpdateUI(m_app.mainframe);
            std::string fragment = AutomationIds::slug(menu_bar->GetMenuLabel(index));
            if (fragment.empty())
                fragment = "menu_" + std::to_string(index);
            append_menu_elements(
                elements, ordinal, scope, menu, "superslicer.menu." + fragment);
        }
    }

    json build_snapshot(bool include_hidden)
    {
        wxWindow* scope = active_scope();
        if (scope == nullptr)
            return failure(ERROR_OPERATION_FAILED, "SuperSlicer main window is not ready");

        std::vector<wxWindow*> windows;
        std::set<wxWindow*> seen;
        for (wxWindow* root : snapshot_roots(scope))
            collect_windows(root, include_hidden, windows, seen);

        std::lock_guard lock(m_registry_mutex);
        ++m_registry_generation;
        if (m_registry_generation == 0)
            ++m_registry_generation;
        m_registry.clear();

        json elements = json::array();
        std::size_t ordinal = 0;
        for (wxWindow* window : windows) {
            const wxString label = window->GetLabel();
            const std::string automation_id = into_u8(window->GetName());
            const std::string accessible_name =
                !label.empty() ? into_u8(label) : automation_id;
            append_snapshot_element(
                elements, ordinal, window, AutomationElementKind::Window,
                automation_id, window_role(window), accessible_name,
                wxRect(window->GetScreenPosition(), window->GetSize()),
                window_value(window), supported_actions(window));
            append_virtual_snapshot_elements(
                elements, ordinal, window, automation_id);
        }
        append_view_selectors(elements, ordinal, scope);
        append_menu_bar(elements, ordinal, scope);

        return {
            { "generation", m_registry_generation },
            { "modal", dynamic_cast<wxDialog*>(scope) != nullptr },
            { "scope_automation_id", into_u8(scope->GetName()) },
            { "elements", std::move(elements) }
        };
    }

    json gui_snapshot(const json& arguments, const std::string& request_id)
    {
        json snapshot = build_snapshot(arguments.value("include_hidden", false));
        if (snapshot.value("ok", true) == false) {
            snapshot[FIELD_REQUEST_ID] = request_id;
            return snapshot;
        }
        return success(std::move(snapshot), request_id);
    }

    std::pair<ResolvedAutomationElement, json> resolve_window(
        const json& arguments)
    {
        wxWindow* scope = active_scope();
        if (scope == nullptr)
            return {
                {},
                failure(ERROR_OPERATION_FAILED, "SuperSlicer main window is not ready")
            };

        const std::string ref = arguments.value("ref", std::string());
        const std::string automation_id =
            arguments.value("automation_id", std::string());
        if (ref.empty() && automation_id.empty())
            return {
                {},
                failure(
                    ERROR_STALE_REF,
                    "a generation-scoped ref or automation_id is required")
            };

        std::lock_guard lock(m_registry_mutex);
        const AutomationRegistryEntry* selected = nullptr;
        if (!ref.empty()) {
            const auto found = m_registry.find(ref);
            if (found == m_registry.end())
                return { {}, failure(ERROR_STALE_REF, "element reference is stale") };
            selected = &found->second;
        } else {
            for (const auto& [entry_ref, entry] : m_registry) {
                static_cast<void>(entry_ref);
                if (entry.automation_id == automation_id) {
                    selected = &entry;
                    break;
                }
            }
        }

        if (selected == nullptr)
            return { {}, failure(ERROR_STALE_REF, "automation_id was not found in the current snapshot") };
        wxWindow* window = selected->window.get();
        if (window == nullptr)
            return { {}, failure(ERROR_STALE_REF, "referenced control no longer exists") };
        if (!within_snapshot_roots(window, scope))
            return { {}, failure("modal_blocked", "an owned modal dialog blocks this element") };
        return {
            ResolvedAutomationElement {
                window, selected->kind, selected->automation_id,
                selected->command_id
            },
            json()
        };
    }

    static bool semantic_only(AutomationElementKind kind)
    {
        return kind == AutomationElementKind::View3DSelector ||
               kind == AutomationElementKind::ViewPreviewSelector;
    }

    json gui_wayland_target(const json& arguments, const std::string& request_id)
    {
        auto [element, error] = resolve_window(arguments);
        wxWindow* window = element.window;
        if (window == nullptr) {
            error[FIELD_REQUEST_ID] = request_id;
            return error;
        }
        if (semantic_only(element.kind))
            return failure(
                ERROR_OPERATION_FAILED,
                "semantic-only elements do not support physical Wayland input",
                request_id);
        if (!window->IsShownOnScreen() || !window->IsEnabled())
            return failure(ERROR_OPERATION_FAILED, "the Wayland input target is not visible and enabled", request_id);

        wxWindow* active = wxGetActiveWindow();
        if (active == nullptr || !is_descendant_or_self(active, m_app.mainframe))
            return failure(
                ERROR_TARGET_NOT_FOREGROUND,
                "SuperSlicer or its owned dialog is not the foreground window",
                request_id);

        const double x = arguments.value("x", 0.5);
        const double y = arguments.value("y", 0.5);
        const double end_x = arguments.value("end_x", x);
        const double end_y = arguments.value("end_y", y);
        const auto relative_coordinate = [](double value) {
            return value >= ZERO_COORDINATE && value <= 1.0;
        };
        if (!relative_coordinate(x) || !relative_coordinate(y) ||
            !relative_coordinate(end_x) || !relative_coordinate(end_y))
            return failure(
                ERROR_OPERATION_FAILED,
                "Wayland coordinates must be element-relative values from 0 through 1",
                request_id);

        const wxRect target_rect =
            element_client_rect(window, element.kind);
        const wxPoint origin =
            window->ClientToScreen(target_rect.GetPosition());
        const wxSize size = target_rect.GetSize();
        const auto screen_point = [origin, size](double relative_x, double relative_y) {
            return json {
                { "x", origin.x + relative_x * std::max(0, size.x - 1) },
                { "y", origin.y + relative_y * std::max(0, size.y - 1) }
            };
        };
        const json fingerprint = {
            { "ref", arguments.value("ref", std::string()) },
            { "automation_id", element.automation_id },
            { "element_kind", static_cast<int>(element.kind) },
            { "scope", into_u8(active_scope()->GetName()) },
            { "active", into_u8(active->GetName()) },
            { "x", origin.x },
            { "y", origin.y },
            { "width", size.x },
            { "height", size.y }
        };
        return success({
            { "point", screen_point(x, y) },
            { "end_point", screen_point(end_x, end_y) },
            { "fingerprint", fingerprint }
        }, request_id);
    }

    json invoke_menu_item(
        const ResolvedAutomationElement& element,
        const std::string& request_id)
    {
        wxMenuBar* menu_bar =
            m_app.mainframe == nullptr ? nullptr : m_app.mainframe->GetMenuBar();
        wxMenu* owner = nullptr;
        wxMenuItem* item = menu_bar == nullptr
            ? nullptr : menu_bar->FindItem(element.command_id, &owner);
        if (item == nullptr || owner == nullptr)
            return failure(ERROR_OPERATION_FAILED, "menu item no longer exists", request_id);
        owner->UpdateUI(m_app.mainframe);
        if (!item->IsEnabled())
            return failure(ERROR_OPERATION_FAILED, "menu item is disabled", request_id);

        const int command_id = element.command_id;
        // A menu command that opens a modal dialog does not return until the
        // dialog closes, so the click is queued and the caller observes the
        // result with a follow-up snapshot instead of blocking this request.
        m_app.CallAfter([this, command_id] {
            wxMenuBar* bar =
                m_app.mainframe == nullptr ? nullptr : m_app.mainframe->GetMenuBar();
            wxMenu* menu = nullptr;
            wxMenuItem* target =
                bar == nullptr ? nullptr : bar->FindItem(command_id, &menu);
            if (target == nullptr || menu == nullptr)
                return;
            int checked = -1;
            if (target->IsCheckable()) {
                // wx reports the state the item already carries, so mirror what a
                // real click does: flip the item, then report the new state.
                checked = target->IsChecked() ? 0 : 1;
                target->Check(checked != 0);
            }
            menu->SendEvent(command_id, checked);
        });
        return success({ { "invoked", true }, { "dispatched", true } }, request_id);
    }

    json invoke_action(
        const ResolvedAutomationElement& element,
        const std::string& request_id)
    {
        if (element.kind == AutomationElementKind::View3DSelector)
            return gui_select_view({ { "view", "3d" } }, request_id);
        if (element.kind == AutomationElementKind::ViewPreviewSelector)
            return gui_select_view({ { "view", "preview" } }, request_id);
        if (element.kind == AutomationElementKind::MenuItem)
            return invoke_menu_item(element, request_id);
        auto* button = dynamic_cast<wxButton*>(element.window);
        if (button == nullptr)
            return failure(ERROR_OPERATION_FAILED, "target does not support invoke", request_id);
        wxCommandEvent event(wxEVT_BUTTON, button->GetId());
        event.SetEventObject(button);
        button->GetEventHandler()->ProcessEvent(event);
        return success({ { "invoked", true } }, request_id);
    }

    json toggle_action(
        wxWindow* window,
        const json& arguments,
        const std::string& request_id)
    {
        bool value = arguments.contains("value")
            ? arguments["value"].get<bool>() : false;
        wxEventType event_type = wxEVT_NULL;
        if (auto* checkbox = dynamic_cast<wxCheckBox*>(window)) {
            if (!arguments.contains("value"))
                value = !checkbox->GetValue();
            checkbox->SetValue(value);
            event_type = wxEVT_CHECKBOX;
        } else if (auto* toggle = dynamic_cast<wxToggleButton*>(window)) {
            if (!arguments.contains("value"))
                value = !toggle->GetValue();
            toggle->SetValue(value);
            event_type = wxEVT_TOGGLEBUTTON;
        } else {
            return failure(ERROR_OPERATION_FAILED, "target does not support toggle", request_id);
        }
        wxCommandEvent event(event_type, window->GetId());
        event.SetInt(value ? 1 : 0);
        event.SetEventObject(window);
        window->GetEventHandler()->ProcessEvent(event);
        return success({ { "value", value } }, request_id);
    }

    json select_action(
        wxWindow* window,
        const json& arguments,
        const std::string& request_id)
    {
        int selection = arguments.value("index", wxNOT_FOUND);
        const std::string text = arguments.value("text", std::string());
        wxItemContainer* choices = dynamic_cast<wxChoice*>(window);
        wxEventType event_type = wxEVT_CHOICE;
        if (choices == nullptr) {
            choices = dynamic_cast<wxComboBox*>(window);
            event_type = wxEVT_COMBOBOX;
        }
        if (choices == nullptr) {
            // SuperSlicer's own dropdown - an item container, but not a wxComboBox.
            choices = dynamic_cast<ComboBox*>(window);
            event_type = wxEVT_COMBOBOX;
        }
        if (choices == nullptr)
            return failure(ERROR_OPERATION_FAILED, "target does not support select", request_id);
        if (selection == wxNOT_FOUND && !text.empty())
            selection = choices->FindString(from_u8(text));
        if (selection < 0 || selection >= static_cast<int>(choices->GetCount()))
            return failure(ERROR_OPERATION_FAILED, "selection is out of range", request_id);
        choices->SetSelection(selection);
        wxCommandEvent event(event_type, window->GetId());
        event.SetInt(selection);
        event.SetString(choices->GetStringSelection());
        event.SetEventObject(window);
        window->GetEventHandler()->ProcessEvent(event);
        return success({ { "selection", selection } }, request_id);
    }

    json set_value_action(
        const ResolvedAutomationElement& element,
        const json& value,
        const std::string& request_id)
    {
        wxWindow* window = element.window;
        if (auto* slider = dynamic_cast<DoubleSlider::Control*>(window)) {
            int lower = slider->GetLowerValue();
            int higher = slider->GetHigherValue();
            if (element.kind == AutomationElementKind::SliderLowerHandle)
                lower = value.get<int>();
            else if (element.kind == AutomationElementKind::SliderUpperHandle)
                higher = value.get<int>();
            else {
                lower = value.is_object() ? value.value("lower", lower) : value.get<int>();
                higher = value.is_object() ? value.value("higher", higher) : value.get<int>();
            }
            if (lower < slider->GetMinValue() ||
                higher > slider->GetMaxValue() || lower > higher)
                return failure(ERROR_OPERATION_FAILED, "slider range is invalid", request_id);
            slider->SetSelectionSpan(lower, higher);
            wxCommandEvent event(
                DoubleSlider::wxCUSTOMEVT_TICKSCHANGED, slider->GetId());
            event.SetEventObject(slider);
            slider->GetEventHandler()->ProcessEvent(event);
        } else if (auto* slider = dynamic_cast<wxSlider*>(window)) {
            slider->SetValue(value.get<int>());
            wxCommandEvent event(wxEVT_SLIDER, slider->GetId());
            event.SetInt(slider->GetValue());
            event.SetEventObject(slider);
            slider->GetEventHandler()->ProcessEvent(event);
        } else if (auto* spin = dynamic_cast<wxSpinCtrl*>(window)) {
            spin->SetValue(value.get<int>());
            wxSpinEvent event(wxEVT_SPINCTRL, spin->GetId());
            event.SetPosition(spin->GetValue());
            event.SetEventObject(spin);
            spin->GetEventHandler()->ProcessEvent(event);
        } else if (auto* spin = dynamic_cast<wxSpinCtrlDouble*>(window)) {
            spin->SetValue(value.get<double>());
            wxSpinDoubleEvent event(wxEVT_SPINCTRLDOUBLE, spin->GetId());
            event.SetValue(spin->GetValue());
            event.SetEventObject(spin);
            spin->GetEventHandler()->ProcessEvent(event);
        } else if (wxTextCtrl* text = text_control_for(window)) {
            // For a composite field this drives the inner control, so the field's own
            // bindings fire and the value reaches the config rather than just the screen.
            text->ChangeValue(from_u8(value_as_text(value)));
            wxCommandEvent changed(wxEVT_TEXT, text->GetId());
            changed.SetEventObject(text);
            text->GetEventHandler()->ProcessEvent(changed);
            wxCommandEvent committed(wxEVT_TEXT_ENTER, text->GetId());
            committed.SetEventObject(text);
            text->GetEventHandler()->ProcessEvent(committed);
        } else {
            return failure(ERROR_OPERATION_FAILED, "target does not support set_value", request_id);
        }
        json result_value = window_value(window);
        if (auto* slider = dynamic_cast<DoubleSlider::Control*>(window)) {
            if (element.kind == AutomationElementKind::SliderLowerHandle)
                result_value = slider->GetLowerValue();
            else if (element.kind == AutomationElementKind::SliderUpperHandle)
                result_value = slider->GetHigherValue();
        }
        return success({ { "value", std::move(result_value) } }, request_id);
    }

    json gui_action(const json& arguments, const std::string& request_id)
    {
        auto [element, error] = resolve_window(arguments);
        wxWindow* window = element.window;
        if (window == nullptr) {
            error[FIELD_REQUEST_ID] = request_id;
            return error;
        }
        if (!window->IsEnabled())
            return failure(ERROR_OPERATION_FAILED, "target control is disabled", request_id);

        const std::string action = arguments.value("action", std::string());
        if (action == "focus") {
            window->SetFocus();
            return success({ { "focused", true } }, request_id);
        }

        if (action == "invoke")
            return invoke_action(element, request_id);

        if (action == "toggle")
            return toggle_action(window, arguments, request_id);

        if (action == "select")
            return select_action(window, arguments, request_id);

        if (action == "set_value") {
            if (!arguments.contains("value"))
                return failure(ERROR_OPERATION_FAILED, "set_value requires value", request_id);
            return set_value_action(element, arguments["value"], request_id);
        }

        return failure(ERROR_OPERATION_FAILED, "unsupported semantic action", request_id);
    }

    json drag_input(
        wxWindow* window,
        const wxPoint& position,
        const wxPoint& end_position,
        const std::string& request_id)
    {
        wxMouseEvent down(wxEVT_LEFT_DOWN);
        down.SetPosition(position);
        down.SetEventObject(window);
        window->GetEventHandler()->ProcessEvent(down);

        constexpr int DRAG_STEPS = 8;
        for (int step = 1; step <= DRAG_STEPS; ++step) {
            wxMouseEvent move(wxEVT_MOTION);
            move.m_leftDown = true;
            move.SetPosition(wxPoint(
                position.x + (end_position.x - position.x) * step / DRAG_STEPS,
                position.y + (end_position.y - position.y) * step / DRAG_STEPS));
            move.SetEventObject(window);
            window->GetEventHandler()->ProcessEvent(move);
        }

        wxMouseEvent up(wxEVT_LEFT_UP);
        up.SetPosition(end_position);
        up.SetEventObject(window);
        window->GetEventHandler()->ProcessEvent(up);
        return success({ { FIELD_ACCEPTED, true } }, request_id);
    }

    json gui_input(const json& arguments, const std::string& request_id)
    {
        const std::string backend = arguments.value("backend", std::string("wx"));
        if (backend == "wayland")
            return failure("permission_required", "Wayland RemoteDesktop consent has not been granted for this enabled period", request_id);
        if (backend != "wx")
            return failure(ERROR_OPERATION_FAILED, "unknown input backend", request_id);

        auto [element, error] = resolve_window(arguments);
        wxWindow* window = element.window;
        if (window == nullptr) {
            error[FIELD_REQUEST_ID] = request_id;
            return error;
        }
        if (semantic_only(element.kind))
            return failure(
                ERROR_OPERATION_FAILED,
                "semantic-only elements do not support synthesized wx input",
                request_id);

        const std::string type = arguments.value("type", std::string());
        if (type == "focus") {
            window->SetFocus();
            return success({ { "focused", true } }, request_id);
        }
        if (type == "text") {
            auto* text = dynamic_cast<wxTextCtrl*>(window);
            if (text == nullptr)
                return failure(ERROR_OPERATION_FAILED, "text input requires a text control", request_id);
            text->WriteText(from_u8(arguments.value("text", std::string())));
            return success({ { FIELD_ACCEPTED, true } }, request_id);
        }

        const wxRect input_rect =
            element_client_rect(window, element.kind);
        const wxSize size = input_rect.GetSize();
        const double relative_x = arguments.value("x", 0.5);
        const double relative_y = arguments.value("y", 0.5);
        const double relative_end_x = arguments.value("end_x", relative_x);
        const double relative_end_y = arguments.value("end_y", relative_y);
        const auto valid_relative = [](double coordinate) {
            return coordinate >= ZERO_COORDINATE && coordinate <= 1.0;
        };
        if (!valid_relative(relative_x) || !valid_relative(relative_y) ||
            !valid_relative(relative_end_x) || !valid_relative(relative_end_y))
            return failure(ERROR_OPERATION_FAILED, "input coordinates must be element-relative values from 0 through 1", request_id);
        const auto client_point = [input_rect, size](double x, double y) {
            return wxPoint(
                input_rect.GetX() +
                    static_cast<int>(x * std::max(0, size.x - 1)),
                input_rect.GetY() +
                    static_cast<int>(y * std::max(0, size.y - 1)));
        };
        const wxPoint position = client_point(relative_x, relative_y);
        const wxPoint end_position = client_point(relative_end_x, relative_end_y);

        if (type == "click") {
            wxMouseEvent down(wxEVT_LEFT_DOWN);
            down.SetPosition(position);
            down.SetEventObject(window);
            window->GetEventHandler()->ProcessEvent(down);
            wxMouseEvent up(wxEVT_LEFT_UP);
            up.SetPosition(position);
            up.SetEventObject(window);
            window->GetEventHandler()->ProcessEvent(up);
            return success({ { FIELD_ACCEPTED, true } }, request_id);
        }
        if (type == "drag")
            return drag_input(window, position, end_position, request_id);
        if (type == "wheel") {
            wxMouseEvent wheel(wxEVT_MOUSEWHEEL);
            wheel.SetPosition(position);
            wheel.m_wheelRotation = arguments.value("rotation", 120);
            wheel.m_wheelDelta = 120;
            wheel.SetEventObject(window);
            window->GetEventHandler()->ProcessEvent(wheel);
            return success({ { FIELD_ACCEPTED, true } }, request_id);
        }
        if (type == "key") {
            const int key_code = arguments.value("key_code", 0);
            if (key_code == 0)
                return failure(ERROR_OPERATION_FAILED, "key input requires key_code", request_id);
            wxKeyEvent down(wxEVT_KEY_DOWN);
            down.m_keyCode = key_code;
            down.SetEventObject(window);
            window->GetEventHandler()->ProcessEvent(down);
            wxKeyEvent up(wxEVT_KEY_UP);
            up.m_keyCode = key_code;
            up.SetEventObject(window);
            window->GetEventHandler()->ProcessEvent(up);
            return success({ { FIELD_ACCEPTED, true } }, request_id);
        }

        return failure(ERROR_OPERATION_FAILED, "unsupported wx input type", request_id);
    }

    json encode_screenshot(wxImage& image, const std::string& request_id)
    {
        if (!image.IsOk())
            return failure(ERROR_OPERATION_FAILED, "captured image is invalid", request_id);

        wxMemoryOutputStream output;
        if (!image.SaveFile(output, wxBITMAP_TYPE_PNG))
            return failure(ERROR_OPERATION_FAILED, "PNG encoding failed", request_id);

        const std::size_t bytes = output.GetSize();
        std::vector<unsigned char> png(bytes);
        output.CopyTo(png.data(), bytes);
        return success({
            { "mime_type", "image/png" },
            { "width", image.GetWidth() },
            { "height", image.GetHeight() },
            { "data_base64", base64_encode(png.data(), png.size()) }
        }, request_id);
    }

    json capture_canvas(GLCanvas3D& canvas, const std::string& request_id)
    {
        CanvasScreenshot pixels;
        if (!canvas.capture_current_framebuffer(pixels))
            return failure(ERROR_OPERATION_FAILED, "OpenGL framebuffer capture failed", request_id);

        wxImage image(static_cast<int>(pixels.width), static_cast<int>(pixels.height));
        image.InitAlpha();
        unsigned char* rgb = image.GetData();
        unsigned char* alpha = image.GetAlpha();
        for (unsigned int y = 0; y < pixels.height; ++y) {
            const unsigned int source_y = pixels.height - y - 1;
            for (unsigned int x = 0; x < pixels.width; ++x) {
                const std::size_t source = 4U * (source_y * pixels.width + x);
                const std::size_t target = y * pixels.width + x;
                rgb[3U * target] = pixels.pixels[source];
                rgb[3U * target + 1U] = pixels.pixels[source + 1U];
                rgb[3U * target + 2U] = pixels.pixels[source + 2U];
                alpha[target] = pixels.pixels[source + 3U];
            }
        }
        return encode_screenshot(image, request_id);
    }

    json capture_active_window(wxWindow* scope, const std::string& request_id)
    {
        if (scope == nullptr || wxGetActiveWindow() != scope)
            return failure(
                ERROR_TARGET_NOT_FOREGROUND,
                "the requested SuperSlicer window is not foreground",
                request_id);

        const wxPoint original_position = scope->GetScreenPosition();
        const wxSize original_size = scope->GetSize();
        const wxString temporary_path =
            wxFileName::CreateTempFileName("superslicer-automation-");
        if (temporary_path.empty())
            return failure(ERROR_OPERATION_FAILED, "could not allocate screenshot path", request_id);
        wxRemoveFile(temporary_path);

        const SpectacleArguments values = {
            "spectacle", "--background", "--activewindow", "--nonotify",
            "--output", into_u8(temporary_path)
        };
        SpectacleArgumentPointers argv {};
        for (std::size_t index = 0; index < values.size(); ++index)
            argv[index] = values[index].c_str();

        const long exit_code = wxExecute(argv.data(), wxEXEC_SYNC);
        const bool foreground_unchanged =
            wxGetActiveWindow() == scope &&
            scope->GetScreenPosition() == original_position &&
            scope->GetSize() == original_size;
        if (!foreground_unchanged) {
            if (wxFileExists(temporary_path))
                wxRemoveFile(temporary_path);
            return failure(
                ERROR_TARGET_NOT_FOREGROUND,
                "window focus or bounds changed during capture",
                request_id);
        }

        wxImage image;
        const bool loaded =
            exit_code == 0 && image.LoadFile(temporary_path, wxBITMAP_TYPE_PNG);
        if (wxFileExists(temporary_path))
            wxRemoveFile(temporary_path);
        if (!loaded)
            return failure(
                ERROR_OPERATION_FAILED,
                "Spectacle active-window capture failed",
                request_id);
        return encode_screenshot(image, request_id);
    }

    json gui_screenshot(const json& arguments, const std::string& request_id)
    {
        wxWindow* target = active_scope();
        if (arguments.contains("ref") || arguments.contains("automation_id")) {
            auto [resolved, error] = resolve_window(arguments);
            if (resolved.window == nullptr) {
                error[FIELD_REQUEST_ID] = request_id;
                return error;
            }
            if (resolved.kind != AutomationElementKind::Window)
                return failure(
                    ERROR_OPERATION_FAILED,
                    "virtual elements cannot be screenshot targets",
                    request_id);
            target = resolved.window;
        }
        if (target == nullptr || !target->IsShownOnScreen())
            return failure(ERROR_OPERATION_FAILED, "screenshot target is not visible", request_id);

        GLCanvas3D* canvas = m_app.plater() != nullptr
            ? m_app.plater()->get_current_canvas3D()
            : nullptr;
        if (canvas != nullptr && target == canvas->get_wxglcanvas())
            return capture_canvas(*canvas, request_id);

        wxWindow* scope = active_scope();
        if (target != scope)
            return failure(
                ERROR_OPERATION_FAILED,
                "screenshot target must be the active SuperSlicer window or current canvas",
                request_id);
        return capture_active_window(scope, request_id);
    }

    json gui_load_model(const json& arguments, const std::string& request_id)
    {
        if (!m_app.is_editor() || m_app.plater() == nullptr)
            return failure(ERROR_OPERATION_FAILED, "model loading requires editor mode", request_id);
        const std::string path = arguments.value("path", std::string());
        if (path.empty())
            return failure(ERROR_OPERATION_FAILED, "path is required", request_id);

        wxArrayString files;
        files.Add(from_u8(path));
        const bool loaded = m_app.plater()->load_files(files);
        if (!loaded)
            return failure(ERROR_OPERATION_FAILED, "the normal GUI model loader rejected the file", request_id);
        return success({ { "loaded", true }, { "path", path } }, request_id);
    }

    std::string create_operation(const std::string& kind)
    {
        const std::string id = "op-" + std::to_string(::getpid()) + "-" + std::to_string(++m_operation_sequence);
        std::lock_guard lock(m_operation_mutex);
        m_operations[id] = Operation { kind, Clock::now() };
        return id;
    }

    json gui_slice(const std::string& request_id)
    {
        if (!m_app.is_editor() || m_app.plater() == nullptr)
            return failure(ERROR_OPERATION_FAILED, "slicing requires editor mode", request_id);
        if (m_app.model().objects.empty())
            return failure(ERROR_OPERATION_FAILED, "no model is loaded", request_id);

        const std::string operation_id = create_operation("slice");
        m_app.plater()->reslice();
        return success({
            { FIELD_OPERATION_ID, operation_id },
            { "state", "running" }
        }, request_id);
    }

    json gui_select_view(const json& arguments, const std::string& request_id)
    {
        if (m_app.plater() == nullptr)
            return failure(ERROR_OPERATION_FAILED, "plater is not ready", request_id);
        const std::string view = lower_ascii(arguments.value("view", std::string()));
        if (view == "3d" || view == "3d_view")
            m_app.plater()->select_view_3D("3D");
        else if (view == "sliced" || view == "preview" || view == "gcode")
            m_app.plater()->select_view_3D("Preview");
        else
            return failure(ERROR_OPERATION_FAILED, "view must be 3d, sliced, preview, or gcode", request_id);
        return success({ { "view", view } }, request_id);
    }

    json gui_set_preview(const json& arguments, const std::string& request_id)
    {
        if (m_app.plater() == nullptr || !m_app.plater()->is_preview_loaded())
            return failure(ERROR_OPERATION_FAILED, "preview is not loaded", request_id);

        if (arguments.contains("layer_lower") || arguments.contains("layer_upper")) {
            const int lower = arguments.value("layer_lower", 0);
            const int upper = arguments.value("layer_upper", UNBOUNDED_PREVIEW_RANGE);
            if (lower < 0 || upper < lower)
                return failure(ERROR_OPERATION_FAILED, "preview layer range is invalid", request_id);
            m_app.plater()->set_preview_layers_slider_values_range(lower, upper);
        }

        if (arguments.contains("move_lower") || arguments.contains("move_upper")) {
            const int lower = arguments.value("move_lower", 0);
            const int upper = arguments.value("move_upper", UNBOUNDED_PREVIEW_RANGE);
            if (lower < 0 || upper < lower)
                return failure(ERROR_OPERATION_FAILED, "preview move range is invalid", request_id);
            m_app.plater()->set_preview_moves_slider_values_range(lower, upper);
        }

        if (arguments.value("focus", false) && m_app.plater()->get_current_canvas3D() != nullptr)
            m_app.plater()->get_current_canvas3D()->get_wxglcanvas()->SetFocus();
        if (arguments.contains("zoom") && m_app.plater()->get_current_canvas3D() != nullptr)
            m_app.plater()->get_current_canvas3D()->zoom_to_bed();

        return success({ { "updated", true } }, request_id);
    }

    json gui_set_transform(const json& arguments, const std::string& request_id)
    {
        ObjectManipulation* manipulation = m_app.obj_manipul();
        if (!m_app.is_editor() || manipulation == nullptr || m_app.plater() == nullptr)
            return failure(ERROR_OPERATION_FAILED, "object transforms require editor mode", request_id);

        if (arguments.contains("coordinate_space")) {
            const std::string space = lower_ascii(arguments.value("coordinate_space", std::string()));
            if (space == "world")
                manipulation->set_coordinates_type(ECoordinatesType::World);
            else if (space == "instance" || space == "object")
                manipulation->set_coordinates_type(ECoordinatesType::Instance);
            else if (space == "local" || space == "part")
                manipulation->set_coordinates_type(ECoordinatesType::Local);
            else
                return failure(
                    ERROR_OPERATION_FAILED,
                    "coordinate_space must be world, instance, or local",
                    request_id);
        }

        bool changed = false;
        static constexpr const char* AXES[] = { "x", "y", "z" };
        for (const char* kind : { "position", "rotation", "scale" }) {
            if (!arguments.contains(kind))
                continue;
            const json& values = arguments[kind];
            if (!values.is_object())
                return failure(
                    ERROR_OPERATION_FAILED,
                    std::string(kind) + " must be an object with x, y, or z values",
                    request_id);

            for (int axis = 0; axis < 3; ++axis) {
                if (!values.contains(AXES[axis]))
                    continue;
                if (!values[AXES[axis]].is_number())
                    return failure(
                        ERROR_OPERATION_FAILED,
                        std::string(kind) + "." + AXES[axis] + " must be numeric",
                        request_id);

                std::string error;
                if (!manipulation->commit_automation_value(
                        kind, axis, values[AXES[axis]].get<double>(), error))
                    return failure(ERROR_OPERATION_FAILED, error, request_id);
                changed = true;
            }
        }

        if (!changed)
            return failure(
                ERROR_OPERATION_FAILED,
                "position, rotation, or scale values are required",
                request_id);
        return success({ { "updated", true } }, request_id);
    }

    json gui_export_gcode(const json& arguments, const std::string& request_id)
    {
        if (!m_app.is_editor() || m_app.plater() == nullptr)
            return failure(ERROR_OPERATION_FAILED, "G-code export requires editor mode", request_id);
        const std::string path = arguments.value("path", std::string());
        if (path.empty())
            return failure(ERROR_OPERATION_FAILED, "path is required", request_id);

        std::string error;
        if (!m_app.plater()->export_gcode_to_path(
                boost::filesystem::path(path), arguments.value("overwrite", false), error))
            return failure(ERROR_OPERATION_FAILED, error, request_id);

        const std::string operation_id = create_operation("export");
        return success({
            { FIELD_OPERATION_ID, operation_id },
            { "path", path },
            { "state", "running" }
        }, request_id);
    }

    // Queue an answer for a file dialog the app has not raised yet. Arming has to
    // happen BEFORE the action that opens the dialog, because ShowModal() blocks the
    // GUI thread this handler runs on -- once the native chooser is up, nothing here
    // gets a turn to answer it.
    json gui_arm_file_dialog(const json& arguments, const std::string& request_id)
    {
        if (arguments.value("clear", false)) {
            FileDialog::disarm_all();
            FileDialog::clear_record();
            return success({ { "armed", FileDialog::armed_count() } }, request_id);
        }

        FileDialogResponse response;
        const std::string answer = lower_ascii(arguments.value("answer", std::string("cancel")));
        if (answer == "ok")
            response.return_code = wxID_OK;
        else if (answer == "cancel")
            response.return_code = wxID_CANCEL;
        else
            return failure(ERROR_OPERATION_FAILED, "answer must be ok or cancel", request_id);

        if (arguments.contains("paths")) {
            if (!arguments["paths"].is_array())
                return failure(ERROR_OPERATION_FAILED, "paths must be an array of strings", request_id);
            for (const json& entry : arguments["paths"]) {
                if (!entry.is_string())
                    return failure(ERROR_OPERATION_FAILED, "paths must be an array of strings", request_id);
                response.paths.push_back(entry.get<std::string>());
            }
        }
        const std::string single_path = arguments.value("path", std::string());
        if (!single_path.empty())
            response.paths.push_back(single_path);

        if (response.return_code == wxID_OK && response.paths.empty())
            return failure(
                ERROR_OPERATION_FAILED,
                "an ok response needs path or paths, because the caller reads the chosen file back",
                request_id);

        response.title_contains = arguments.value("title_contains", std::string());
        response.filter_index   = arguments.value("filter_index", 0);
        response.checkbox       = arguments.value("checkbox", false);

        FileDialog::arm(std::move(response));
        return success({ { "armed", FileDialog::armed_count() } }, request_id);
    }

    // Reports the dialog the app raised most recently. Arming with result=cancel and
    // then reading this is how a menu item gets audited: it proves which dialog the
    // item opened -- title, wildcard, save-vs-open -- without writing any files.
    json gui_file_dialog_status(const std::string& request_id)
    {
        json status = { { "armed", FileDialog::armed_count() } };

        FileDialogRecord record;
        if (!FileDialog::last_record(record)) {
            status["last"] = nullptr;
            return success(std::move(status), request_id);
        }

        status["last"] = {
            { "title", record.title },
            { "wildcard", record.wildcard },
            { "directory", record.directory },
            { "filename", record.filename },
            { "save", record.save },
            { "multiple", record.multiple },
            { "intercepted", record.intercepted },
            { "accepted", record.return_code == wxID_OK },
            { "paths", record.paths }
        };
        return success(std::move(status), request_id);
    }

    json gui_operation_status(const json& arguments, const std::string& request_id)
    {
        const std::string operation_id = arguments.value(FIELD_OPERATION_ID, std::string());
        Operation operation;
        {
            std::lock_guard lock(m_operation_mutex);
            const auto found = m_operations.find(operation_id);
            if (found == m_operations.end())
                return failure(ERROR_OPERATION_FAILED, "unknown operation_id", request_id);
            operation = found->second;
        }

        bool running = false;
        bool succeeded = false;
        if (operation.kind == "slice" && m_app.plater() != nullptr) {
            running = m_app.plater()->is_background_process_running() ||
                      m_app.plater()->is_background_process_update_scheduled();
            succeeded = !running && m_app.plater()->is_preview_loaded();
        } else if (operation.kind == "export" && m_app.plater() != nullptr) {
            running = m_app.plater()->is_export_gcode_scheduled() ||
                      m_app.plater()->is_background_process_running();
            succeeded = !running;
        }

        return success({
            { FIELD_OPERATION_ID, operation_id },
            { "kind", operation.kind },
            { "state", running ? "running" : (succeeded ? "succeeded" : "failed") }
        }, request_id);
    }

    json wait_for_operation(const json& arguments, const std::string& request_id)
    {
        const std::string operation_id = arguments.value(FIELD_OPERATION_ID, std::string());
        if (operation_id.empty())
            return failure(ERROR_OPERATION_FAILED, "operation_id is required", request_id);
        const int timeout_ms = std::clamp(arguments.value("timeout_ms", 10000), 1, 60000);
        const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);

        while (Clock::now() < deadline && m_running.load()) {
            json state = call_gui("__operation_status", { { FIELD_OPERATION_ID, operation_id }, { "timeout_ms", 1000 } }, request_id);
            if (!state.value("ok", false))
                return state;
            const std::string value = state["result"].value("state", std::string());
            if (value != "running")
                return state;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        if (!m_running.load())
            return failure(ERROR_DISABLED, "automation API was disabled while waiting", request_id);
        return failure("timeout", "operation did not finish before the wait deadline", request_id);
    }

    json run_batch(const json& arguments, const std::string& request_id)
    {
        const json items = arguments.value("items", json::array());
        if (!items.is_array() || items.size() > 50)
            return failure(ERROR_OPERATION_FAILED, "batch items must be an array with at most 50 entries", request_id);

        json results = json::array();
        for (std::size_t index = 0; index < items.size(); ++index) {
            const json& item = items[index];
            std::string tool = item.value("tool", std::string());
            if (tool.empty() && item.contains("workflow"))
                tool = workflow_tool(item.value("workflow", std::string()));
            if (tool.empty() || tool == TOOL_BATCH)
                return failure(ERROR_OPERATION_FAILED, "batch contains an invalid or nested tool", request_id);

            const std::string child_id = request_id + "-" + std::to_string(index + 1);
            json result = call_service(tool, item.value("arguments", json::object()), child_id);
            results.push_back(result);
            if (!result.value("ok", false) && !arguments.value("continue_on_error", false))
                break;
        }
        return success({ { "results", std::move(results) } }, request_id);
    }
