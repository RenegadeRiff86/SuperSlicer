# GUI menu audit

Every menu item in the main window, checked against one question: **does it do what its label says it does?**

This exists because bugs in this fork have repeatedly been mislabelled UI rather than broken logic - a dropdown entry that silently reverted, a help page listing "known bugs" that were already fixed, a control strip painted with the wrong theme. Assume nothing is correct until it has been driven.

## Scope

**Every menu in the application, not just the menubar.** There are four surfaces:

1. **Main menubar** - 101 items / 8 menus. Enumerable from the running app.
2. **Right-click context menus** - 9 distinct menus built on demand by `MenuFactory` (`src/slic3r/GUI/GUI_Factories.cpp`). These never appear in an automation snapshot because they are constructed at right-click time, so they are enumerated from source.
3. **G-code viewer menubar** - a completely different menubar built by `MainFrame::init_menubar_as_gcodeviewer` (`MainFrame.cpp:2158`) when the app runs in viewer mode. Not reachable from the normal build's menus.
4. **Preset combo dropdowns** - the sidebar and tab preset selectors append action entries (`Add/Remove presets`, `Add physical printer`, `Edit preset`) that behave like menu items (`PresetComboBoxes.cpp`).

Menubar inventory taken 2026-07-31 from the running build:

```
python3 tests/automation/gui_drive.py --attach menus
```

If the count changes, re-run the dump and reconcile this file before continuing.

**Labels are not static.** `MainFrame::update_menubar` (`MainFrame.cpp:2220`) rewrites menu labels when the printer technology changes - `Export G-code` becomes `Export`, `Filament Settings Tab` becomes `Material Settings Tab`. Every label-dependent check must be repeated under both FFF and SLA, because "does it do what the label says" has two answers when the label has two forms.

## How to work this list

- Drive the real GUI, never the CLI. See `tests/automation/gui_drive.py` and the notes in `doc/slice-verification.md`.
- `SUPERSLICER_AUTOMATION_TOKEN` must be set or the automation server silently does not start.
- Quit through `superslicer.menu.file.quit`, never SIGTERM - a killed instance makes the next launch open a modal crash-recovery dialog that blocks every menu id.
- **Close the GUI when you stop working.**

### Answering file dialogs

A file dialog will not answer itself. The GTK chooser is not a wx widget tree, so no snapshot sees inside it and no invoke reaches its buttons — an unanswered one wedges the GUI thread until the request times out. Arm the answer **before** triggering the item:

```
python3 tests/automation/gui_drive.py --attach arm-file-dialog --answer cancel
python3 tests/automation/gui_drive.py --attach menu superslicer.menu.file.export.export_config
python3 tests/automation/gui_drive.py --attach file-dialog
```

The last call reports the dialog that was raised — title, wildcard, save-vs-open, and the paths handed back. Arming `cancel` and reading that record is the cheapest honest check for most `File >` items: it proves which dialog the item opened without writing a byte. Switch to `--answer ok --path <file>` (repeat `--path` for a multi-select dialog) when the export or import itself is what needs proving; the armed path is used verbatim, so wx's extension fixup and overwrite prompt are skipped.

Arming also protects a run from a dialog you did not expect. An armed `cancel` left in the queue is harmless if nothing opens, and saves the session if something does.

## Status key

| Mark | Meaning |
|---|---|
| `[ ]` | not yet checked |
| `[x]` | driven, behaves as labelled |
| `[!]` | driven, does **not** match its label - file it and link the fix |
| `[~]` | partially checked; note what is left |
| `[L]` | **deferred** - cannot be tested here; reason required |

A `[L]` is a promise, not a dismissal. Every one states why, so the list stays honest about its own coverage.

Record findings inline under the item. Keep the reason for a `[L]` in the same line so it survives skimming.

---

## Standing reasons for deferral

These recur; reference them by tag instead of restating.

- **NEEDS-HW** - requires a physical printer, SD card, or USB device.
- **NEEDS-NET** - contacts a remote host (update checks, upload queues, web links).
- **NEEDS-FILE** - needs an input artifact not in the repo (SLA archive, HFP, Prusa bundle, binary G-code).
- **DESTRUCTIVE** - would overwrite or discard the user's real configuration; needs an isolated data dir first.
- **OS-INTEGRATION** - changes desktop/system state outside the app.

---

# Part 1 - Main menubar

## File (30)

Swept 2026-08-01 with the armed-cancel method above: every item below was invoked and the dialog it
raised was read back. Items marked `[~]` had their dialog confirmed but not their effect.

### Project
- [x] `file.new_project` - New Project — correctly **disabled** on an empty plate; with objects loaded it prompts before discarding (modal `SuperSlicer`, Cancel available). Verified 2026-08-01.
- [x] `file.open_project` - Open Project… — raises `Choose one file (3MF/AMF):` with the project wildcards (3mf/amf/zip.amf). Verified 2026-08-01.
- [~] `file.save_project` - Save Project — raises `Save file as:` (3MF), i.e. identical to Save Project as, which is correct for a project that has never been saved. **Left to check: that it stops prompting once the project has a path** — that is the only thing distinguishing it from Save as.
- [x] `file.save_project_as` - Save Project as… — raises `Save file as:` with the 3MF wildcards. Verified 2026-08-01. On an *empty* plate the same item raises a `Save project` modal instead of a file dialog.

### Import
- [~] `file.import.import_stl_3mf_step_obj_amf` - Import STL/3MF/STEP/OBJ/AMF… — 2026-08-01: raises the right dialog (`Choose one or more files (STL/3MF/STEP/OBJ/AMF/SVG):`, open, multi-select) and two armed STL paths both loaded, reaching the normal `Multi-part object detected` prompt. Still to do: 3MF, STEP, OBJ, AMF, SVG — the label names them, so each needs its own load.
- [x] `file.import.import_stl_imperial_units` - Import STL (Imperial Units) — **applies exactly 25.4**, verified 2026-08-01 on a purpose-built 4mm cube: 4.0 → 101.6 on all three axes, deviation from 25.4 is 0. Uses the same generic import dialog as the plain item, so the dialog alone cannot tell them apart. See the measurement trap below.
- [L] `file.import.import_sla_archive` - Import SLA Archive… — NEEDS-FILE. Raises its own `Import SLA archive` modal rather than a plain file dialog, so it needs different handling than the rest.
- [~] `file.import.import_zip_archive` - Import ZIP Archive… — raises `Choose ZIP file:` (*.zip). Dialog correct; an actual archive import is still to do.
- [L] `file.import.import_hfp` - Import HFP… — NEEDS-FILE. Dialog confirmed: `Choose your modifier file (HFP):` (*.hfp).
- [~] `file.import.import_config` - Import Config… — raises `Select configuration to load:` (*.ini, *.gcode, *.bgcode). Dialog correct; loading a config back is still to do.
- [~] `file.import.import_prusa_config` - Import Prusa Config… — raises an **identical** dialog to Import Config, and that is correct by design: both are `MainFrame::load_config_file(bool from_prusa)` (`MainFrame.cpp:2311`), one dialog, with the flag passed to `PresetBundle::load_config_file`. The Prusa conversion itself is NEEDS-FILE.
- [x] `file.import.import_config_from_project` - Import Config from Project… — raises `Choose one file (3MF/AMF):`, matching the label. Verified 2026-08-01.
- [~] `file.import.import_config_bundle` - Import Config Bundle… — raises `Select configuration to load:` (*.ini). Dialog correct. Actually importing is **DESTRUCTIVE** (it writes presets into the user's config dir via `SaveImported`), so it needs an isolated datadir first.
- [~] `file.import.import_prusa_config_bundle` - Import Prusa Config Bundle… — identical dialog to the item above, correct by design: both are `MainFrame::load_configbundle(file, bool from_prusa)` (`MainFrame.cpp:2391`), the flag adding `ConvertFromPrusa`. DESTRUCTIVE + NEEDS-FILE for the conversion itself.

### Export
- [!] `file.export.export_g_code` - Export G-code… — normally raises `Save G-code file as:` with a templated default name (`cube4mm_2m_0.20mm_200C_PLA_ENDER3.gcode`), both unsliced and sliced. **But it was observed silently dead**: in one session, on a valid single-object plate with the item reading enabled, invoking it produced no dialog, no slicing, no export and no error for 25s, and only came back after an explicit successful slice. See finding 9 — trigger not yet isolated.
- [L] `file.export.send_g_code` - Send G-code… — NEEDS-NET
- [L] `file.export.export_g_code_to_sd_card_flash_drive` - Export G-code to SD Card / Flash Drive… — NEEDS-HW
- [x] `file.export.export_plate` - Export Plate… — raises `Export Platter:` offering STL/OBJ/3MF/AMF. Verified 2026-08-01. Wording drift, but the **menu is the outlier, not the dialog**: "platter" is this app's own vocabulary (82 uses, including `_L("Platter")` as a section heading in the shortcuts dialog and `Plater::export_platter()`), inherited from Slic3r. Renaming either way orphans translations, and "plate" is what every current slicer says, so which direction to migrate is a **product decision** - left open.
- [x] `file.export.export_toolpaths_as_obj` - Export Toolpaths as OBJ… — correctly **disabled** until the plate has been sliced (an invoke is refused with "menu item is disabled"); toolpaths do not exist before that. Verified 2026-08-01.
- [x] `file.export.export_config` - Export Config… — verified 2026-08-01: raises `Save configuration as:` with the INI wildcard and default name `config.ini`, and an armed path wrote a 662-line config containing the live preset values.
- [x] `file.export.export_config_bundle` - Export Config Bundle… — raises `Save presets bundle as:` (*.ini) and wrote a 790-key bundle. Verified 2026-08-01.
- [x] `file.export.export_config_bundle_with_physical_printers` - Export Config Bundle With Physical Printers… — **exactly what the label claims**, verified 2026-08-01 by diffing it against the plain bundle: 805 keys vs 790, the 15 extra all under `[physical_printer:FrankenPrinter]` (`preset_name`, `print_host`, `printhost_apikey`, …), **zero** other added, removed or changed keys.
- [x] `file.export.export_to_prusa_config` - Export to Prusa Config… — genuinely converts, verified 2026-08-01 by diff: 345 keys vs the plain export's 661. It drops 319 SuperSlicer-only keys (`arc_fitting_*`, `avoid_travel_*`, …), adds 3 PrusaSlicer spellings (`elefant_foot_compensation`, `min_fan_speed`, `support_material_interface_pattern`) and changes 33 values. Round-tripping through a real PrusaSlicer is still worth doing.

### Convert
- [~] `file.convert.convert_ascii_g_code_to_binary` - Convert ASCII G-code to binary… — raises `Choose one file (GCODE/GCO/G/BGCODE/BGC/NGC):`. Dialog correct; the conversion itself is still to do.
- [~] `file.convert.convert_binary_g_code_to_ascii` - Convert binary G-code to ASCII… — raises an **identical** dialog to the item above, which is reasonable (either direction picks any G-code file). Round-trip and diff still to do.

### Other
- [L] `file.eject_sd_card_flash_drive` - Eject SD Card / Flash Drive… — NEEDS-HW
- [ ] `file.re_slice_now` - (Re)Slice Now
- [~] `file.repair_stl_file` - Repair STL file… — raises `Select the STL file to repair:` (*.stl), matching the label. Whether it repairs is the open half: hole filling is `#if 0` on Linux (see `superslicer_model_repair_linux_port`), so the label may still overpromise.
- [x] `file.g_code_preview` - G-code Preview… — raises `Open G-code file:` with the G-code wildcards, matching the label. Verified 2026-08-01.
- [x] `file.quit` - Quit — verified 2026-07-31, exits cleanly and releases the automation port

## Edit (10)

Tested 2026-07-31 with `pig.stl` loaded. The object list is opaque to automation, so the signals used were the app's own enable predicates plus **slicing as an oracle** (an empty plate fails to slice).

> **Do not trust menu enabled-flags alone.** wx refreshes them on menu-open/idle, so after a programmatic invoke they are often stale - `delete_all` left `delete_selected` reading enabled on an empty plate. The slice oracle is what settled it.

- [x] `edit.select_all` — re-enables `copy` / `delete_selected` after a deselect. Verified.
- [x] `edit.deselect_all` — correctly disables `deselect_all`, `delete_selected` and `copy`. Verified.
- [~] `edit.delete_selected` — not isolated yet; blocked behind the confirmation-dialog issue below.
- [x] `edit.delete_all` — empties the plate (slice fails afterwards, succeeds again after undo). Prompts "All objects will be removed, continue?" first, offering **New Project / Erase all objects / Cancel**; "Erase all objects" empties the plate. Verified after the dialog fix below.
- [x] `edit.undo` — restores the deleted objects (slice succeeds again). Verified.
- [x] `edit.redo` — re-applies the undone delete. Verified.
- [x] `edit.copy` — enables `paste`. Verified.
- [~] `edit.paste` — invoked without error; object-count confirmation still owed.
- [ ] `edit.reload_from_disk` (modify the source file on disk and confirm the change is picked up)
- [ ] `edit.search`

### ✅ Fixed: confirmation dialogs were invisible AND mislabelled

`Delete All` opens a `MessageDialog` (`Plater.cpp:6880`). Two defects, one root cause.

**Root cause:** `MsgDialog.hpp` selected the dialog class on `#ifdef _WIN32`. Windows got the custom
`MsgDialog`-based widget; **Linux and macOS got the native `wxMessageDialog`**, whose buttons are GTK
widgets rather than wxWindows. Consequences:

1. `MessageDialog::SetButtonLabel` looked its buttons up with `FindWindowById`, which can never find a
   native widget, so it silently did nothing. `reset_with_confirm` sets the labels to **"New Project"**
   (`wxID_YES` -> `new_project()`) and **"Erase all objects"** (`wxID_NO` -> `delete_all_objects_from_model()`),
   but the dialog rendered stock **Yes / No / Cancel**. Answering **"No"** to *"All objects will be removed,
   continue?"* is what erased the plate, and "Yes" silently started a new project. Inverted and dangerous.
2. A native dialog exposes no wx child windows, so it never appeared in the automation snapshot and the
   prompt could not be answered - the app wedged until clicked by hand.

**Fix:** `MessageDialog` now derives from `MsgDialog` on every platform (`MsgDialog.hpp`, `MsgDialog.cpp`).
That is the widget Windows already shipped, and `WarningDialog` / `RichMessageDialogBase` already used it on
Linux, so the look stays consistent. Verified 2026-07-31: the prompt now reports `modal: True`, scopes to the
dialog, exposes **'New Project' / 'Erase all objects' / '&Cancel'**, and can be answered programmatically -
clicking "Erase all objects" empties the plate.

Also hardened `snapshot_roots()` (`AutomationServerGui.inl`) to treat every shown app-owned dialog as a root
regardless of `IsModal()`, with de-duplication moved into `collect_windows()`. A top-level dialog is reachable
upward via `GetParent()` but is not in its parent's `GetChildren()`, so walking the scope alone never descends
into one.

> **Blast radius:** this changes the appearance of *every* message box on Linux/macOS from the GTK native
> dialog to SuperSlicer's themed one. That is the intended direction (it is what Windows does and what makes
> custom button labels work at all), but it is a visible change - revert by restoring the `#ifdef _WIN32`
> split if unwanted.

## Window (9)

- [x] `window.3d_platter_tab` — selects `superslicer.view.3d` / `canvas.3d`. Verified 2026-07-31.
- [!] `window.layer_preview_tab` — **does the same thing as GCode Preview Tab.** See below.
- [!] `window.gcode_preview_tab` — **duplicate of Layer preview Tab.** Both land on `superslicer.view.preview` / `canvas.preview` at runtime, and `MainFrame.cpp:2719-2723` shows why: the `PlaterPreview` and `PlaterGcode` branches both call `m_plater->select_view_3D("Preview")`. Their labels and tooltips differ ("Show the layers from the slicing process" vs "Show the preview of the gcode output") but the behaviour does not. They only diverge under `ESettingsLayout::Tabs`; on Linux with no layout key in `SuperSlicer.ini` the ternary at `MainFrame.cpp:648-657` falls through to `ESettingsLayout::Old`, so the duplicate path is what users get. **Decide: implement the layer view, or merge the two menu items.**
- [x] `window.print_settings_tab` — shows 72 print options (`perimeters`, `perimeter_generator`, …). Verified.
- [x] `window.filament_settings_tab` — shows 19 filament options (`filament_colour`, `filament_diameter`, …). Verified.
- [x] `window.printer_settings_tab` — shows 41 printer options (`max_print_height`, `z_step`, …). Verified.
- [L] `window.print_host_upload_queue` — NEEDS-NET
- [ ] `window.open_new_instance` (confirm the second instance gets its own config lock and does not fight over the automation port)
- [ ] `window.compare_presets`

## View (11)

The seven camera presets are quick to check as a group: each should land on its named axis.

- [x] `view.iso_0` / `top_1` / `bottom_2` / `front_3` / `rear_4` / `left_5` / `right_6` — each passes a distinct, correctly-named string to `select_view()` (`MainFrame.cpp:1711-1731`): iso/top/bottom/front/rear/left/right. No duplicates or crossed wires. Top spot-checked visually (camera looks straight down Z, axis gizmo shows only X/Y). Verified 2026-07-31.

> The in-app screenshot API (`superslicer_ui_screenshot`) requires the window to be **foreground**, which cannot be held while driving from another process - it fails with `target_not_foreground`. Use `capture_vs_window` (which activates via KWin) for visual checks, and note that it only sees windows in the bound editor's **process tree**, so launch the slicer from the integrated terminal, not from `python_exec`.
- [x] `view.show_labels_e` — check state toggles False -> True -> False cleanly. Verified 2026-07-31.
- [x] `view.show_legend_l` — correctly **disabled** on the 3D view and enabled (and on) in preview, matching its `is_preview_shown()` gate. Verified.
- [x] `view.collapse_sidebar` — objectively collapses: 3D canvas width 3252 -> 3840 -> 3252 across toggles. Verified.
- [x] `view.fullscreen` — window bounds (0,34,3840,2071) -> (0,0,3840,2160) -> restored; genuinely covers the panel. Verified.

## Calibration (14)

The highest-risk menu - every generator writes a model *and* a config, and each has its own help page making claims. For each: does the generated plate match the help page, and does the help page match the code?

- [ ] `calibration.introduction`
- [ ] `calibration.bed_extruder_leveling`
- [ ] `calibration.klipper_z_offset_calibration` (see `z_offset_calibration`)
- [ ] `calibration.apply_z_offset_calibration_result`
- [ ] `calibration.filament_flow_calibration`
- [ ] `calibration.extruder_flow_calibration`
- [ ] `calibration.filament_temperature_calibration`
- [ ] `calibration.extruder_retraction_calibration`
- [x] `calibration.pressure_calibration` - **audited 2026-07-31.** Theme colours fixed, help page rewritten against the code, the permanently-disabled "Classic line sweep" style removed, combo widths fixed. Generate → slice → export verified end to end.
- [ ] `calibration.adaptive_pressure_advance_calibration` (see `adaptive_pa_39_status` - Phase B was in progress)
- [ ] `calibration.adaptive_pressure_advance_results`
- [ ] `calibration.bridge_flow_calibration`
- [ ] `calibration.ironing_pattern_calibration`
- [ ] `calibration.calibration_cube`

## Generate (2)

- [ ] `generate.shape_gallery`
- [ ] `generate.mosaic_from_picture`

## Configuration (15)

- [L] `configuration.install_and_upgrade_vendor_bundles` — NEEDS-NET
- [~] `configuration.configuration_wizard` — DESTRUCTIVE until an isolated data dir is set up; the dialog can be opened and cancelled safely
- [ ] `configuration.configuration_snapshots`
- [ ] `configuration.take_configuration_snapshot`
- [L] `configuration.check_for_application_updates` — NEEDS-NET
- [L] `configuration.desktop_integration` — OS-INTEGRATION
- [ ] `configuration.preferences` (large surface; audit separately - it is where `dark_color_mode` and the colour templates live)
- [x] `configuration.mode.simple` / `mode.advanced` / `mode.expert` — correct. Print Settings shows **13 / 37 / 72** options respectively, and the check state is mutually exclusive (exactly one true). `modefn` = `save_mode(mode)` (`GUI_App.cpp:3651`). Verified 2026-07-31.
- [!] `configuration.tags.simple` / `tags.advanced` / `tags.expert` — **does not match its labels.** The Tags submenu is generated from the *same* `tags()` list as Mode, with the *same* item labels, the *same* check predicate (`(get_mode() & tag.tag) == tag.tag`), and the *same* tooltip - which reads **"%s View Mode"** even though the submenu is called Tags (`GUI_App.cpp:3469-3484`). The only difference is the handler: `tagfn` does `save_mode(get_mode() ^ mode)` (`:3658`), XOR-ing bits into the mode bitmask. Observed: invoking these leaves the option count pinned at 9 regardless of combination, and the checks are not exclusive. Two concrete defects:
>   1. The guard `if (get_mode() != mode)` makes a tag impossible to un-toggle when it is the only one set - a toggle should be unconditional.
>   2. Arbitrary XOR combinations produce mode bitmasks that correspond to no documented view mode, so "Simple/Advanced/Expert" under Tags does not mean what it says.
>
>   Needs a product decision on what Tags is *for* before it can be fixed - left unfixed deliberately.
- [ ] `configuration.language`
- [L] `configuration.prusa_wi_fi_configuration_file` — NEEDS-HW

## Help (10)

Audited 2026-08-01. The four link items were resolved rather than deferred: each URL was read out of the
source and then fetched, because "does the link go where the label says" is answerable without a printer.

- [~] `help.superslicer_website` — opens `https://www.superslicer.org/`, which matches the label. **The site's TLS
  certificate expired 2025-10-14** (Let's Encrypt R10, `CN=superslicer.org`; `curl` reports
  `ssl_verify_result=10`, `CERT_HAS_EXPIRED`), so the browser shows a security interstitial instead of the site.
  Upstream infrastructure, not fixable in this tree - recorded so nobody re-diagnoses it as a slicer bug.
  **Its tooltip read "Open the Slic3r website in your browser"** - the wrong application - while both sibling
  items templated `SLIC3R_APP_NAME`. Fixed (`MainFrame.cpp:1666`).
- [x] `help.superslicer_releases` — `SLIC3R_DOWNLOAD` = `https://github.com/supermerill/SuperSlicer/releases`, HTTP 200. Matches label.
- [x] `help.superslicer_wiki` — `github_url() + "/wiki"` = `https://github.com/supermerill/SuperSlicer/wiki`, HTTP 200. Matches label.
- [x] `help.slic3r_manual` — `http://manual.slic3r.org/` redirects to HTTPS and serves the real "Slic3r Manual". Label honest: it is Slic3r's manual, not SuperSlicer's.
- [x] `help.system_info` — modal `sysinfo` dialog; version/arch/kernel/RAM all matched the host. The clipped
  "OpenGL installation" heading is **not** a defect: that pane holds the memory block *and* the GL info
  (`SysInfoDialog.cpp:180`) at a `16 * em` minimum height with `wxHW_SCROLLBAR_AUTO`, so the rest scrolls.
- [x] `help.show_configuration_folder` — opened `~/.config/SuperSlicer/SuperSlicer_2.7.62.0-beta2+fork`, i.e. the
  versioned datadir of the *running* build, confirmed against the version in the About dialog.
- [~] `help.report_an_issue` — opens `github_url() + "/issues/new"` = `supermerill/SuperSlicer` (HTTP 200). Correct
  for the label, but this is a **hard fork** (see `project_hard_fork`), so user-reported issues land on
  upstream's tracker for code upstream does not have. Overridable via the `github_url` app-config key.
  **Decide** whether to repoint it.
- [x] `help.about_superslicer` — modal `about2` dialog, version matches the build, themed correctly.
- [x] `help.show_tip_of_the_day` — verified **both** clauses of its tooltip. Closed → pushes the notification
  bottom-right; already open → `push_hint_notification` takes the early branch and calls `open_next()`
  (`NotificationManager.cpp:2794-2799`), so it does show another tip.
- [~] `help.keyboard_shortcuts` — dialog opens and lists the shortcuts. **Cannot confirm each listed key
  actually fires**: the automation API exposes no key-injection command, so the hand-maintained
  `KBShortcutsDialog` list cannot be diffed against real behaviour from here. Left partial deliberately.

> **Accelerator vs hint - read before auditing any shortcut.** A menu label built with `\t` registers a real wx
> accelerator (File/Edit: `Ctrl+N`, `Ctrl+Alt+S`, …); one built with `sep` (`" - "`) is **only printed text**, and
> the key is handled by `GLCanvas3D::on_char()` instead - deliberate, per the comment at `MainFrame.cpp:1713`.
> That is why an automation `menus` dump shows keys for View/Help but not for File/Edit: wx strips a real
> accelerator from the label text and keeps a `sep` hint. Do not read the difference as a missing shortcut.

> **Automation limitation found here.** Controls with no wx name fall back to their class name as the automation
> id, so the System Info dialog exposes two elements both called `button` and `invoke` cannot address them
> individually (it also has no label-based lookup - `invoke '&OK'` fails). Dialogs whose buttons are addressable
> by label, like the Delete All prompt, only work because `MsgDialog` names them. Worth fixing in the
> automation layer before the context-menu passes, which depend on addressing many similar controls.

---

# Part 2 - Right-click context menus

Enumerated from `MenuFactory` in `src/slic3r/GUI/GUI_Factories.cpp`. Each menu is assembled from shared `append_menu_item_*` builders, so **the same label appears in several menus and must be checked in each** - the enabling condition and the acting target differ (an object vs one part vs a multi-selection).

Trigger each by right-clicking the relevant thing in the 3D scene or the object list.

## Empty platter - `default_menu()` (`:1422`)
- [ ] `Add Shape` submenu - the generic primitives from `append_submenu_add_generic`. Check each shape actually loads the shape it names.

## Object - `object_menu()` (`:1427`)
- [ ] `Add part` / `Add negative volume` / `Add modifier` / `Add support blocker` / `Add support enforcer` submenus (`append_menu_items_add_volume`) - each with Box/Cylinder/Sphere/Slab/Gallery/Text/SVG entries. Verify the volume **type** matches the submenu it came from; a modifier that lands as a part is exactly this audit's target.
- [ ] `Add instance` / `Remove instance` / `Set number of instances`
- [ ] `Delete`
- [ ] `Set as a Separated Object` (`append_menu_item_instance_to_object`)
- [ ] `Printable` (check item - toggles the printable flag)
- [ ] `Reload from disk`
- [ ] `Replace with STL`
- [ ] `Export as STL`
- [ ] `Fix through Netfabb` / repair (`append_menu_item_repair_model`) - **suspect on Linux**, see `file.repair_stl_file` above
- [ ] `Simplify model`
- [ ] `Scale` submenu + `Convert units` entries (`append_menu_items_convert_unit`) - imperial/metre conversion, verify the factor
- [ ] `Mirror` submenu - along X / Y / Z
- [ ] `Split` submenu - `To objects` / `To parts` (two different operations behind similar labels)
- [ ] `Add settings` submenu (`append_menu_item_settings`) - dynamically built category list; large surface, audit separately
- [ ] `Change extruder` submenu
- [ ] `Invalidate cut info`
- [ ] `Edit text` / `Edit SVG`

## SLA object - `sla_object_menu()` (`:1441`)
- [L] whole menu — needs an SLA printer profile selected; same builders as the FFF object menu minus `Change extruder`

## Part - `part_menu()` (`:1454`)
- [ ] `Delete`, `Reload from disk`, `Replace with STL`, `Export as STL`, repair, `Simplify model`
- [ ] `Split` (parts only - note this is a bare item here, not the submenu the object menu gets)
- [ ] `Mirror` submenu
- [ ] `Change type` (`append_menu_item_change_type`) - part/modifier/blocker/enforcer
- [ ] `Add settings`, `Change extruder`, `Convert units`

## Text part - `text_part_menu()` (`:1463`)
- [ ] `Edit text`, `Delete`, repair, `Simplify model`, `Mirror`, `Change type`, `Add settings`, `Change extruder`

## SVG part - `svg_part_menu()` (`:1470`)
- [ ] `Edit SVG`, `Delete`, repair, `Simplify model`, `Mirror`, `Change type`, `Add settings`, `Change extruder`

## Instance - `instance_menu()` (`:1476`)
- [ ] `Set as a Separated Object`, `Printable`, `Delete`

## Layer range - `layer_menu()` (`:1481`)
- [ ] `Add settings` (this is the layer-range path that the PA generator manipulates - exercise it after a calibration generate)

## Multi-selection - `multi_selection_menu()` (`:1489`)
- [ ] repair, `Reload from disk`, `Convert units`
- [ ] `Merge to multipart object`
- [ ] `Change extruder` (only shown with >1 extruder)
- [ ] `Printable`, `Set number of instances` (hidden in Simple mode - verify that gating too)

---

# Part 3 - G-code viewer menubar

Built by `MainFrame::init_menubar_as_gcodeviewer` (`MainFrame.cpp:2158`). Launch the viewer binary (`Slic3r` symlinked as the G-code viewer) rather than the slicer.

- [ ] `File > Open G-code`
- [ ] `File > Reload from Disk` (F5)
- [ ] `File > Convert ASCII G-code to binary` / `Convert binary G-code to ASCII`
- [ ] `File > Export Toolpaths as OBJ`
- [ ] `File > Open SuperSlicer` (starts the full slicer)
- [ ] `File > Quit`
- [ ] `View >` the seven camera presets + `Show Legend`
- [ ] `Help >` shared with the main app's help menu (`generate_help_menu`) - re-check here since availability differs

---

# Part 4 - Preset combo dropdowns

These live in the sidebar and on each settings tab (`PresetComboBoxes.cpp`). They mix preset selection with action entries, which is where mislabelling hides.

- [ ] `Add/Remove presets` (`:805`, `:856`)
- [ ] `Add/Remove filaments` / `Add/Remove materials` / `Add/Remove printers` (`:1080-1084`, `:1354`) - confirm each opens the wizard on the page it names
- [ ] `Add physical printer` / `Edit physical printer` / `Delete physical printer`
- [ ] Tab preset combo equivalents (separate class from the plater combo - check both)

---

## Running tally

Counted from the checkboxes on 2026-08-01. A few lines cover a group of sibling items (the seven camera presets, the mode/tags triplets), so **checklist lines are fewer than menu items**: Part 1's 91 lines cover the 101 menubar items.

| Part | Lines | `[x]` | `[!]` | `[~]` | `[L]` | Todo |
|---|---|---|---|---|---|---|
| 1 - Main menubar (101 items) | 91 | 37 | 4 | 16 | 10 | 24 |
| 2 - Context menus | 32 | 0 | 0 | 0 | 1 | 31 |
| 3 - G-code viewer menubar | 8 | 0 | 0 | 0 | 0 | 8 |
| 4 - Preset combo dropdowns | 4 | 0 | 0 | 0 | 0 | 4 |
| **All** | **135** | **37** | **4** | **16** | **11** | **67** |

The File menu went from 20 untouched lines to 1 in a single pass on 2026-08-01, once file dialogs
became answerable. Two of its `[L]`s were downgraded to `[~]`: the Prusa import pair turned out to
share one function with its sibling, so the dialog and dispatch are checkable here and only the
conversion needs an external file.

## Findings so far

1. **Layer preview Tab / GCode Preview Tab are the same command** on the default Linux layout - see the Window section. **Open**, needs a product decision.
2. **"Delete all" prompt was mislabelled on Linux/macOS**, so "No" was the button that erased the plate - **fixed**, see the Edit section. Same fix restored automation's ability to answer confirmation dialogs, which unblocks the File menu. The prompt's default button was also moved from "New Project" to **Cancel**, so a stray Enter no longer discards the plate (`Plater.cpp:6882`).
3. **Configuration > Tags duplicates Configuration > Mode's labels but XOR-toggles the mode bitmask** - see the Configuration section. **Open**, needs a product decision.
4. **Help > SuperSlicer website named the wrong application in its tooltip** ("Open the Slic3r website") while both
   sibling items templated `SLIC3R_APP_NAME` - **fixed** (`MainFrame.cpp:1666`). Separately, `superslicer.org`
   has served an **expired TLS certificate since 2025-10-14**; upstream infrastructure, not fixable here.
5. **Help > Report an Issue points at upstream's tracker** on a hard fork. **Open**, needs a product decision;
   the `github_url` app-config key already overrides it without a code change.
6. **Automation ids are not unique** - unnamed controls fall back to their wx class name, so sibling buttons
   collide and cannot be invoked individually. **Half of this was a false alarm**: the underlying API is
   ref-based and `/api/v1/ui/action` already takes a `ref` resolved from *any* element field, so
   `elements(name="&Cancel")` disambiguates fine. Only the `gui_drive.py invoke` CLI lacked a way to pass it.
   **Fixed 2026-08-01**: `invoke` gained `--label`, so `invoke button --label '&Cancel'` picks the right one.
   The ids themselves are still not unique, which is worth fixing at the source eventually, but it no longer
   blocks Part 2.
7. **File dialogs could not be answered by automation at all** - the GTK chooser is not a wx widget tree, so
   no snapshot saw inside it and no invoke reached its buttons; roughly 18 `File >` items plus several
   context-menu items were untestable, and any run that reached one wedged the GUI thread. **Fixed**: every
   file dialog in the app is now `Slic3r::GUI::FileDialog`
   (`src/slic3r/GUI/Automation/AutomationFileDialog.{hpp,cpp}`), which lets the automation API arm the answer
   and read back which dialog was raised. See "Answering file dialogs" above.
8. **Both file dialogs opened in a CGAL dependency's test-mesh folder**
   (`deps/build-linux-x86_64-release/.../dep_CGAL/Data/data/meshes`) - observed 2026-08-01 on `Export Config`
   (save) and `Import STL/3MF/STEP/OBJ/AMF` (open). This is most likely a stale "last used directory" in this
   machine's app config, which would be correct behaviour. **Unconfirmed** - reproduce against a clean data
   dir before treating it as a bug.
9. **`File > Export > Export G-code` was observed silently dead.** On a valid plate holding one
   small in-bounds cube, with the menu item reporting **enabled**, invoking it produced no dialog,
   no slicing, no export and no error for 25s. It recovered only after an explicit successful
   slice. A fresh session does *not* show this - there, the item raises `Save G-code file as:` on an
   unsliced plate straight away. **Trigger not isolated.** Two hypotheses were tested against a
   fresh session and BOTH failed to reproduce it: (a) an out-of-bounds object left on the plate,
   (b) a slice forced while such an object was present. `Plater::export_gcode` has two silent
   early-returns that fit the symptom - `p->process_completed_with_error` (`Plater.cpp:7249`) and
   `UPDATE_BACKGROUND_PROCESS_INVALID` (`Plater.cpp:7279`) - and neither tells the user anything.
   Whatever the trigger, an enabled menu item that does nothing and says nothing is a defect in its
   own right. **Partly fixed 2026-08-01**: both silent returns now call `show_error` and name the
   reason, and `Plater::send_gcode` had the identical silent return, so it was given the same
   treatment. Verified by forcing an object off the bed and invoking Export G-code - previously
   nothing at all happened, now the refusal is stated. The *trigger* of the original stuck state is
   still unknown, so this makes the symptom self-explaining rather than curing it; if it recurs the
   dialog will now say which of the two guards fired.
   Left alone deliberately: `update_restart_background_process` returns early on a validation error
   (`Plater.cpp:3743`) and so skips the `process_completed_with_error = false` reset ten lines below
   (`:3759`). That is a plausible route to a stuck flag, but changing control flow in the slicing
   path on a hunch is not worth the risk - it needs a debugger session, not a guess.
10. **Measurement trap: a big model cannot verify a scale factor.** Checking Import STL (Imperial
    Units) with the bunny gave a uniform 2.0625x, not 25.4x, which reads exactly like a bug. It is
    not: 25.4x puts the bunny far outside the bed, the app offers to scale it down to fit, and the
    fit factor lands on top of the conversion. The same check on a 4mm cube - small enough that
    25.4x still fits - gives exactly 25.4 on all three axes. **Verify scale claims with a model
    small enough that the result stays in bounds**, or the bed-fit silently rewrites the answer.

Recount with:

```
grep -c '^- \[ \]' doc/gui-menu-audit.md
```

Update this table whenever the list changes so the coverage claim cannot drift from the checkboxes.
