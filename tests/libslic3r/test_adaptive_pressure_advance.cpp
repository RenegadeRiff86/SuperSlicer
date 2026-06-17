#include <catch2/catch.hpp>

#include <libslic3r/GCode/AdaptivePressureAdvance.hpp>

using namespace Slic3r;

// A 2x2 calibration grid: two acceleration bands (1000, 2000 mm/s^2),
// each with two volumetric-flow points (3.84, 7.68 mm^3/s).
//   accel 1000:  flow 3.84 -> PA 0.040,  flow 7.68 -> PA 0.030
//   accel 2000:  flow 3.84 -> PA 0.030,  flow 7.68 -> PA 0.020
static const char* k_grid_model =
    "PA, volumetric_flow, acceleration\n"   // header line must be ignored
    "0.040, 3.84, 1000\n"
    "0.030, 7.68, 1000\n"
    "\n"                                     // blank line must be ignored
    "0.030, 3.84, 2000\n"
    "0.020, 7.68, 2000\n";

TEST_CASE("AdaptivePAModel parses Orca-format rows", "[AdaptivePA]") {
    AdaptivePAModel model(k_grid_model);
    REQUIRE_FALSE(model.empty());
    // Header and blank lines are skipped; only the four data rows remain.
    REQUIRE(model.size() == 4);
}

TEST_CASE("AdaptivePAModel recovers the measured grid points", "[AdaptivePA]") {
    AdaptivePAModel model(k_grid_model);
    CHECK(model.evaluate(3.84, 1000) == Approx(0.040));
    CHECK(model.evaluate(7.68, 1000) == Approx(0.030));
    CHECK(model.evaluate(3.84, 2000) == Approx(0.030));
    CHECK(model.evaluate(7.68, 2000) == Approx(0.020));
}

TEST_CASE("AdaptivePAModel interpolates linearly across flow", "[AdaptivePA]") {
    AdaptivePAModel model(k_grid_model);
    // Midpoint flow (5.76) within the accel=1000 band: (0.040 + 0.030)/2 = 0.035.
    CHECK(model.evaluate(5.76, 1000) == Approx(0.035));
    // Midpoint flow within the accel=2000 band: (0.030 + 0.020)/2 = 0.025.
    CHECK(model.evaluate(5.76, 2000) == Approx(0.025));
}

TEST_CASE("AdaptivePAModel interpolates linearly across acceleration", "[AdaptivePA]") {
    AdaptivePAModel model(k_grid_model);
    // At flow 3.84, accel midpoint 1500: (0.040 + 0.030)/2 = 0.035.
    CHECK(model.evaluate(3.84, 1500) == Approx(0.035));
    // Full bilinear at flow 5.76, accel 1500:
    //   accel1000,flow5.76 -> 0.035 ; accel2000,flow5.76 -> 0.025 ; mid -> 0.030.
    CHECK(model.evaluate(5.76, 1500) == Approx(0.030));
}

TEST_CASE("AdaptivePAModel clamps inputs outside the measured range", "[AdaptivePA]") {
    AdaptivePAModel model(k_grid_model);
    // Flow below/above the measured range clamps to the nearest measured flow.
    CHECK(model.evaluate(0.5, 1000)  == Approx(0.040)); // -> flow 3.84
    CHECK(model.evaluate(99.0, 1000) == Approx(0.030)); // -> flow 7.68
    // Acceleration below/above the measured range clamps to the nearest band.
    CHECK(model.evaluate(3.84, 100)   == Approx(0.040)); // -> accel 1000 band
    CHECK(model.evaluate(3.84, 99999) == Approx(0.030)); // -> accel 2000 band
    // Both axes out of range -> corner value.
    CHECK(model.evaluate(0.5, 100)    == Approx(0.040));
    CHECK(model.evaluate(99.0, 99999) == Approx(0.020));
}

TEST_CASE("AdaptivePAModel rounds the result to the requested step", "[AdaptivePA]") {
    AdaptivePAModel model(k_grid_model);
    // flow 5.12 sits 1/3 of the way from 3.84 to 7.68 in the accel=1000 band:
    //   0.040 + (0.030 - 0.040) * (1/3) = 0.036667 -> rounds to 0.037 at step 1e-3.
    CHECK(model.evaluate(5.12, 1000) == Approx(0.037));
}

TEST_CASE("AdaptivePAModel returns the fallback for an empty model", "[AdaptivePA]") {
    AdaptivePAModel empty;
    REQUIRE(empty.empty());
    // Whatever the inputs, an empty model yields the caller-supplied fallback unchanged.
    CHECK(empty.evaluate(5.0, 1500, 0.123) == Approx(0.123));
    CHECK(empty.evaluate(5.0, 1500, 0.0)   == Approx(0.0));

    // A string with no numeric rows is also empty.
    AdaptivePAModel header_only("PA, flow, accel\n# comment\n");
    CHECK(header_only.empty());
    CHECK(header_only.evaluate(5.0, 1500, 0.456) == Approx(0.456));
}

TEST_CASE("AdaptivePAModel handles a single measured point", "[AdaptivePA]") {
    AdaptivePAModel model("0.025, 5.0, 1500");
    REQUIRE(model.size() == 1);
    // With a single point the model is constant regardless of the query.
    CHECK(model.evaluate(1.0, 500)    == Approx(0.025));
    CHECK(model.evaluate(5.0, 1500)   == Approx(0.025));
    CHECK(model.evaluate(50.0, 50000) == Approx(0.025));
}

TEST_CASE("AdaptivePAModel handles a single acceleration level", "[AdaptivePA]") {
    // Only flow varies; acceleration is constant across the measurements.
    AdaptivePAModel model("0.040, 3.84, 1000\n0.020, 7.68, 1000\n");
    REQUIRE(model.size() == 2);
    // Flow interpolation still works; acceleration is effectively ignored (clamped).
    CHECK(model.evaluate(5.76, 1000)  == Approx(0.030));
    CHECK(model.evaluate(5.76, 50000) == Approx(0.030));
    CHECK(model.evaluate(3.84, 1)     == Approx(0.040));
    CHECK(model.evaluate(7.68, 1)     == Approx(0.020));
}
