# VS IDE Bridge issues observed during SuperSlicer cleanup

Observed while using VS IDE Bridge against:

- Repository: `C:\Users\elsto\source\repos\SuperSlicer`
- Solution: `C:\Users\elsto\source\repos\SuperSlicer\build-default\Slic3r.slnx`
- Configuration/platform: `RelWithDebInfo|x64`
- Original bridge during the first cleanup observations: `3.0.6`
- Live bridge during resumed verification and cleanup: `3.0.7`

Checked against:

- `C:\Users\elsto\source\repos\vs-ide-bridge\CHANGELOG.md`
- Bridge changelog version: `3.0.7`

The `3.0.7` entries below now distinguish changelog-confirmed fixes from items that were live-verified while rebound to the upgraded bridge.

## Current stopping point, 2026-07-02

Warning cleanup stopped after these fixes:

- Cleared BP1008 C-style-cast warnings that were actionable or analyzer false positives.
- Cleared BP1025 in `GCodeProcessor.cpp`.
- Fixed several `GCodeProcessor.cpp` messages/linter findings and kept `libslic3r` building.
- Cleared BP1002 in `BridgeDetector.hpp`.
- Removed the old private undeclared assignment-operator idiom in `BridgeDetector`; the class remains non-assignable because it has reference and `const` members.
- Cleared BP1002 in `PolylineStitcher.hpp` by naming the snap/stitch epsilon.
- Fixed a real `lnt-logical-bitwise-mismatch` in `PolylineStitcher.hpp`: boolean `&` became logical `&&`.
- After rebinding to bridge `3.0.7`, cleared BP1001 in `Preset.cpp` by extracting repeated configuration-key literals into file-local constants.
- Fixed `Preset.cpp` messages by making `Preset::save()` const-correct, making `deep_diff` internal linkage, and avoiding two accidental `auto` copies.
- Added the missing `PhysicalPrinter::print_host_options()` definition for the declaration in `Preset.hpp`.

Verification performed:

- `exif` build passed after C-source cast cleanup.
- `libslic3r` build passed after `GCodeProcessor`, `BridgeDetector`, and `PolylineStitcher` changes.
- `compile_file` on `PolylineStitcher.cpp` passed when retried with a bridge handle.
- `compile_file(file="src/libslic3r/Preset.cpp")` passed under bridge `3.0.7` using the source-relative path.
- `Preset.cpp` refreshed to zero BP1001 warnings after the config-key constant cleanup.
- `Preset.cpp` refreshed to zero messages after compiling the file.
- Error count was `0`.

Diagnostic caveat at stop:

- The build post-check showed `1721` warnings and `2` messages.
- A later fresh grouped warning refresh jumped to `2024` warnings and `2` messages, without corresponding code churn. This reinforces the still-open diagnostics freshness issue.
- The two remaining messages were `VCR001` rows for `PolylineStitcher.hpp` (`canConnect` and `isOdd`). Explicit template specializations exist in `PolylineStitcher.cpp`, and the file compile passed.
- During the `Preset.cpp` pass, warning totals also moved through `2023`, `2028`, and back to `2023` while file-scoped source fixes and compile results were consistent.
- The two current global messages are `VCR001` rows in `Preset.hpp` for `save` and `print_host_options`; bridge search finds both matching definitions in `Preset.cpp`, and `Preset.cpp` compiles successfully.

## Status checked against bridge `3.0.7` changelog

| Issue | Status from changelog |
| --- | --- |
| 1. `build_errors` missed linker/build-output errors | Fixed in `3.0.7` with `BuildOutputErrorParser` and `buildOutputErrors`. |
| 2. Summaries said "fix all before building" when only warnings/messages remained | Fixed in `3.0.7`; live-verified. Summaries now say the build can succeed when only warnings/messages remain. |
| 3. Error List and Build output disagreed after failed builds | Mostly fixed in `3.0.7` by parsing Build output and adding next-step guidance. |
| 4. Watchdog probe timeouts during long foreground commands | Still open; changelog lists this as not yet addressed. |
| 5. BP1008 flagged comment text as casts | Fixed in `3.0.7`; BP1008 now skips comments and strings. |
| 6. BP1008 flagged unnamed parameters before `const` | Fixed in `3.0.7`; declaration-context lookahead was added. |
| 7. BP1008 gave C++ guidance for `.c` files | Fixed in `3.0.7`; `.c` files get C-appropriate guidance. |
| 8. Warning totals fluctuate across refreshes | Still open; live-reproduced with totals moving between `1721`, `2024`, `2028`, and `2023`. |
| 9. Grouped diagnostic counts were ambiguous | Fixed in `3.0.7`; live-verified. Grouped responses now expose `countScope`, `groupCount`, and `diagnosticCount`. |
| 10. Full refresh can retain stale rows cleared by file refresh | Still open; changelog lists diagnostics caching/freshness work as not yet addressed. |
| 11. File-handle and filename filters can disagree | Still open; changelog lists handle-vs-filename freshness paths as not yet addressed. |
| 12. MSVC-header compiler warnings lack origin context | Still open; changelog says this needs Build-pane include-stack parsing. |
| 13. Multi-edit `apply_diff` can partially apply edits | Fixed in `3.0.7`; live-verified. A failing first edit applied `0/2`, skipped the later edit, and reported `partialMutation:false`. |
| 14. `compile_file` source-relative path resolution used build directory | Fixed in `3.0.7`; live-verified with `compile_file(file="src/libslic3r/Preset.cpp")` and `compile_file(file="src/libslic3r/Arachne/utils/PolylineStitcher.cpp")`. |
| 15. `find_text` path filters can miss valid repo-relative matches | Fixed in `3.0.7`; live-verified with `find_text(query="canConnect", path="src/libslic3r")`. |
| 16. VCR001 misses modern C++ definitions/specializations | Still open; live-reproduced for `Preset.hpp` definitions that bridge search finds and MSVC compiles. |
| 17. Timed-out `apply_diff` can still mutate and leave the document unsaved | Newly observed in `3.0.7`; `Preset.hpp` edit timed out after 45s, later inspection showed the edit landed but the tab was unsaved. |
| 18. `save_document` does not resolve repo-relative paths or bridge handles | Newly observed in `3.0.7`; saving `src/libslic3r/Preset.hpp` and `f:29` failed, while the absolute path succeeded. |
| 19. Diagnostic side-channel warning text contains mojibake | Still open in `3.0.7`; command summaries are fixed, but advisory text still contains a replacement-character glyph before "run warnings". |

## Issue details

### Issue 1: `build_errors` did not return linker errors as structured errors

`build_errors` reported failed builds with `LastBuildInfo != 0`, but structured errors were empty. The actual failures were only visible in the Build output pane, including `LNK2001` and `LNK1120` rows.

Status: fixed in bridge `3.0.7`; live-verified.

### Issue 2: Summary text overstated warning/message severity

`errors(refresh=true)` ended with "fix all before building" even when there were zero errors and the build could succeed with warnings/messages remaining.

Status: fixed in bridge `3.0.7`.

### Issue 3: Error List and Build output disagreement was not surfaced clearly

After a failed build, the Error List could show zero errors while the Build output contained the actionable failure.

Status: mostly fixed in bridge `3.0.7` by Build-pane parsing and explicit next-step guidance.

### Issue 4: Watchdog timeouts during long foreground commands

Long diagnostic/build operations can register watchdog probe timeouts even when the command succeeds. These should be classified as busy/foreground work rather than health failures.

Status: still open per changelog and live observations.

### Issue 5: BP1008 flagged comment text as C-style casts

Examples included comment prose such as `image(float)` and `(float) height`.

Status: fixed in bridge `3.0.7`; live-verified.

### Issue 6: BP1008 flagged unnamed parameters followed by qualifiers

Example:

```cpp
size_t vertexCount(size_t) const { return 3; }
```

Status: fixed in bridge `3.0.7`; live-verified.

### Issue 7: BP1008 gave C++ named-cast advice for `.c` files

For C source, the right fix may be a correct format/type pairing rather than `static_cast`.

Status: fixed in bridge `3.0.7`; live-verified.

### Issue 8: Warning totals fluctuate across refreshes

Warning counts changed substantially across refreshes and post-checks. Latest example:

- Build post-check: `1721` warnings, `2` messages.
- Later grouped refresh: `2024` warnings, `2` messages.

No code change explains that jump. The diagnostics provider appears to repopulate or mix sources at different times.

Status: still open per changelog.

### Issue 9: Grouped diagnostic counts were ambiguous

In grouped mode, `count` could mean group rows while severity counts meant diagnostic rows.

Status: fixed in bridge `3.0.7`.

### Issue 10: Full diagnostics refresh can retain stale rows

A global refresh retained stale `GCodeProcessor.cpp` and `ExPolygon.hpp` rows that file-scoped refreshes cleared.

Status: still open per changelog.

### Issue 11: File-handle and filename diagnostics filters can disagree

For the same file, handle-based and filename-based diagnostics filters produced different/stale results.

Status: still open per changelog.

### Issue 12: Compiler warnings in MSVC headers lack local origin context

Grouped warnings included `C4244`/`C4267` attributed to MSVC's `optional` header without the project-code instantiation site.

Status: still open per changelog.

### Issue 13: Multi-edit `apply_diff` could partially apply edits

A four-edit `apply_diff` against `PolylineStitcher.hpp` applied three edits and failed the namespace constant insertion, briefly leaving later references to an undefined constant.

Status: fixed in bridge `3.0.7`.

### Issue 14: `compile_file` resolved source-relative paths under the build directory

`compile_file(file="src/libslic3r/Arachne/utils/PolylineStitcher.cpp")` looked under `build-default\src\...` and failed. Retrying with a bridge handle compiled successfully.

Status: fixed in bridge `3.0.7`.

### Issue 15: `find_text` path filtering missed valid matches

`find_text(query="canConnect", path="src/libslic3r")` returned no matches. The same search without the path filter found four matches in `PolylineStitcher.cpp` and `.hpp`.

Status: fixed in bridge `3.0.7`.

### Issue 16: VCR001 misses valid modern C++ definitions

Two examples:

- `BridgeDetector& operator=(const BridgeDetector &) = delete;` was still reported as "definition not found".
- `PolylineStitcher` static template members were reported missing even though explicit specializations exist in `PolylineStitcher.cpp`.

Status: still open per changelog; likely Visual Studio linter limitation.

### Issue 17: Timed-out `apply_diff` can still mutate and leave the document unsaved

While changing `Preset.hpp`, an `apply_diff` call timed out after 45 seconds. Later bridge reads showed that the edit had landed:

```cpp
void                save() const;
```

However, `list_tabs` showed `Preset.hpp` as unsaved. The next model step had to detect the unsaved tab and explicitly save it.

Status: newly observed in bridge `3.0.7`.

### Issue 18: `save_document` does not use the same path/handle resolution as editor tools

After the timed-out header edit, `save_document` failed for both:

- `file="src/libslic3r/Preset.hpp"`
- `file="f:29"`

The failure payload listed the open absolute path, and retrying with that absolute path succeeded:

- `file="C:\Users\elsto\source\repos\SuperSlicer\src\libslic3r\Preset.hpp"`

This differs from `read_file` and `apply_diff`, which resolved the repo-relative path or file handle correctly.

Status: newly observed in bridge `3.0.7`.

### Issue 19: Advisory warning text still contains mojibake

The main diagnostic summaries are improved in `3.0.7`, but side-channel advisory warnings still show replacement characters, for example:

```text
Also found 6 message(s) [replacement character] run warnings with severity=Message.
```

Status: still open in bridge `3.0.7`.
