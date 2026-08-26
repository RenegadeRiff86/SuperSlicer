#pragma once

#include "PrusaSlicer.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/ModelArrange.hpp"
#include "libslic3r/PresetBundle.hpp"

#include <boost/filesystem/path.hpp>

#include <memory>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r::CLI_detail {

bool user_profiles_requested(const DynamicPrintAndCLIConfig& config);
std::unique_ptr<PresetBundle> load_user_profiles(
    const DynamicPrintAndCLIConfig& config,
    ForwardCompatibilitySubstitutionRule compatibility_rule,
    std::string& error);
void write_profiles_json(std::ostream& out, const PresetBundle& bundle);
bool write_requested_slice_artifacts(
    const GCodeProcessorResult& result,
    const DynamicPrintConfig& config,
    const std::string& report_path,
    const std::string& preview_path,
    int preview_layer);

enum class CliModelLoadResult {
    loaded,
    empty,
    error
};

PrinterTechnology get_printer_technology(const DynamicConfig& config);
#if defined(SLIC3R_GUI) && ENABLE_GL_CORE_PROFILE
std::pair<int, int> parse_requested_opengl_version(const std::string& version_text);
#endif
CliModelLoadResult load_model_for_cli(
    const std::string& file,
    ForwardCompatibilitySubstitutionRule substitution_rule,
    PrinterTechnology& printer_technology,
    DynamicPrintConfig& print_config,
    Model& model);
void configure_cli_extruders(DynamicPrintConfig& print_config, unsigned int extruder_count);
void merge_cli_models(std::vector<Model>& models);
void prepare_cli_duplicates(std::vector<Model>& models);
void center_cli_models(std::vector<Model>& models, const Vec2d& center);
void align_cli_models(std::vector<Model>& models, const Vec2d& point);
void scale_cli_models(std::vector<Model>& models, double factor);
bool scale_cli_models_to_fit(std::vector<Model>& models, const Vec3d& volume);
void cut_cli_models(std::vector<Model>& models, double height);
void split_cli_models(std::vector<Model>& models);
void ensure_cli_models_on_bed(std::vector<Model>& models);
void request_default_stl_export(std::vector<std::string>& actions);
bool process_cli_models(
    std::vector<Model>& models,
    bool make_copy,
    PrinterTechnology printer_technology,
    const DynamicPrintAndCLIConfig& cli_config,
    const DynamicPrintConfig& print_config,
    int duplicate_count,
    bool user_center_specified,
    const arr2::ArrangeBed& bed,
    bool slice_for_gcodeviewer,
    std::vector<std::string>& gcodeviewer_input_files);
#if !defined(_WIN32) && !defined(__APPLE__)
boost::filesystem::path appimage_install_path(const boost::filesystem::path& fallback);
#endif

} // namespace Slic3r::CLI_detail
