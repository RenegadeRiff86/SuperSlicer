#include <catch_main.hpp>

#include <cmath>
#include <string>

#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/PrintConfig.hpp"

using namespace Slic3r;

namespace {

PrintConfig klipper_config(float max_velocity, float max_acceleration, float square_corner_velocity, float minimum_cruise_ratio)
{
    PrintConfig config(static_cast<const PrintConfig&>(FullPrintConfig::defaults()));
    config.gcode_flavor.value = gcfKlipper;
    config.machine_limits_usage.value = MachineLimitsUsage::TimeEstimateOnly;
    config.machine_klipper_max_velocity.value = max_velocity;
    config.machine_klipper_max_acceleration.value = max_acceleration;
    config.machine_klipper_square_corner_velocity.value = square_corner_velocity;
    config.machine_min_cruise_ratio.value = minimum_cruise_ratio;
    config.time_start_gcode.value = 0.0;
    return config;
}

float estimated_time(const PrintConfig& config, const std::string& gcode)
{
    GCodeProcessor processor;
    processor.apply_config(config);
    processor.process_string(gcode);
    processor.finalize(false);
    return processor.get_time(PrintEstimatedStatistics::ETimeMode::Normal);
}

void require_near(float actual, float expected, float tolerance = 0.001f)
{
    REQUIRE(std::abs(actual - expected) <= tolerance);
}

} // namespace

TEST_CASE("Klipper timing uses square corner velocity at a right-angle junction", "[gcodeprocessor][klipper][time]")
{
    const PrintConfig config = klipper_config(100.0f, 1000.0f, 5.0f, 0.0f);
    const float time = estimated_time(config,
        "G90\n"
        "G1 X100 F6000\n"
        "G1 Y100 F6000\n");

    require_near(time, 2.19025f);
}

TEST_CASE("Klipper timing applies minimum cruise ratio to short reversing moves", "[gcodeprocessor][klipper][time]")
{
    const PrintConfig config = klipper_config(100.0f, 1000.0f, 0.0f, 0.5f);
    const float time = estimated_time(config,
        "G90\n"
        "G1 X1 F6000\n"
        "G1 X0 F6000\n");

    require_near(time, 0.134164f);
}

TEST_CASE("Klipper SET_VELOCITY_LIMIT updates subsequent timing limits", "[gcodeprocessor][klipper][time]")
{
    const PrintConfig config = klipper_config(100.0f, 1000.0f, 5.0f, 0.0f);
    const float time = estimated_time(config,
        "G90\n"
        "SET_VELOCITY_LIMIT VELOCITY=20 ACCEL=1000 SQUARE_CORNER_VELOCITY=5 MINIMUM_CRUISE_RATIO=0\n"
        "G1 X100 F6000\n"
        "G1 X200 F6000\n");

    require_near(time, 10.02f);
}

TEST_CASE("Klipper timing applies kinematics Z limits to the path", "[gcodeprocessor][klipper][time]")
{
    PrintConfig unrestricted = klipper_config(100.0f, 1000.0f, 5.0f, 0.0f);
    PrintConfig limited = unrestricted;
    limited.machine_klipper_max_z_velocity.value = 5.0;
    limited.machine_klipper_max_z_acceleration.value = 50.0;

    const std::string gcode = "G90\nG1 Z20 F6000\nG1 Z40 F6000\n";
    REQUIRE(estimated_time(limited, gcode) > estimated_time(unrestricted, gcode) + 1.0f);
}

TEST_CASE("Klipper timing applies extrude-only velocity and acceleration limits", "[gcodeprocessor][klipper][time]")
{
    PrintConfig unrestricted = klipper_config(100.0f, 1000.0f, 5.0f, 0.0f);
    PrintConfig limited = unrestricted;
    limited.machine_klipper_max_extrude_only_velocity.value = 10.0;
    limited.machine_klipper_max_extrude_only_acceleration.value = 100.0;

    const std::string gcode = "M83\nG1 E100 F6000\nG1 E100 F6000\n";
    REQUIRE(estimated_time(limited, gcode) > estimated_time(unrestricted, gcode) + 5.0f);
}

TEST_CASE("Klipper timing derives omitted extrude-only limits like the firmware", "[gcodeprocessor][klipper][time]")
{
    PrintConfig derived = klipper_config(100.0f, 1000.0f, 5.0f, 0.0f);
    PrintConfig explicit_limits = derived;
    constexpr float DEFAULT_MAX_EXTRUDE_CROSS_SECTION_FACTOR = 4.0f;
    const float nozzle_diameter = float(derived.nozzle_diameter.get_at(0));
    const float filament_radius = float(derived.filament_diameter.get_at(0)) / 2.0f; // A radius is half its diameter.
    const float filament_area = float(PI) * filament_radius * filament_radius;
    const float default_max_extrude_ratio =
        DEFAULT_MAX_EXTRUDE_CROSS_SECTION_FACTOR * nozzle_diameter * nozzle_diameter / filament_area;
    explicit_limits.machine_klipper_max_extrude_only_velocity.value =
        derived.machine_klipper_max_velocity.value * default_max_extrude_ratio;
    explicit_limits.machine_klipper_max_extrude_only_acceleration.value =
        derived.machine_klipper_max_acceleration.value * default_max_extrude_ratio;

    const std::string gcode = "M83\nG1 E100 F6000\nG1 E100 F6000\n";
    require_near(estimated_time(derived, gcode), estimated_time(explicit_limits, gcode));
}

TEST_CASE("Klipper timing limits junctions when the extrusion ratio changes", "[gcodeprocessor][klipper][time]")
{
    PrintConfig unrestricted = klipper_config(100.0f, 1000.0f, 5.0f, 0.0f);
    PrintConfig limited = unrestricted;
    unrestricted.machine_klipper_instantaneous_corner_velocity.value = 1000.0;
    limited.machine_klipper_instantaneous_corner_velocity.value = 1.0;

    const std::string gcode =
        "G90\n"
        "M83\n"
        "G1 X100 E5 F6000\n"
        "G1 X200 E20 F6000\n";
    REQUIRE(estimated_time(limited, gcode) > estimated_time(unrestricted, gcode));
}

TEST_CASE("Klipper step-generation calibration does not add toolhead move time", "[gcodeprocessor][klipper][time]")
{
    PrintConfig baseline = klipper_config(100.0f, 1000.0f, 5.0f, 0.0f);
    PrintConfig calibrated = baseline;
    calibrated.machine_klipper_rotation_distance.value = 7.25;
    calibrated.machine_klipper_pressure_advance.value = 0.08;
    calibrated.machine_klipper_pressure_advance_smooth_time.value = 0.05;
    calibrated.machine_klipper_shaper_freq_x.value = 55.0;
    calibrated.machine_klipper_shaper_freq_y.value = 42.0;
    calibrated.machine_klipper_damping_ratio_x.value = 0.12;
    calibrated.machine_klipper_damping_ratio_y.value = 0.15;

    const std::string gcode = "G90\nG1 X100 E5 F6000\n";
    require_near(estimated_time(calibrated, gcode), estimated_time(baseline, gcode));
}

