// Copyright (c) 2026 Stan Elston (RenegadeRiff86)
//
// SuperSlicer is released under the terms of the AGPLv3 or higher.
//
// Standalone, zero-dependency test harness for AdaptivePAModel (issue #39).
//
// AdaptivePressureAdvance.{hpp,cpp} only depend on the C++ standard library, so this
// builds in ~2 seconds without the rest of libslic3r. Build + run with run_tests.bat in
// this directory. Add new cases by writing another test_* function and calling it from
// main(); use CHECK / CHECK_NEAR for assertions.

#include <cmath>
#include <cstdio>
#include <string>

#include "AdaptivePressureAdvance.hpp"

using namespace Slic3r;

static int g_checks = 0;
static int g_fails  = 0;

static void check(bool ok, const char *expr, const char *file, int line)
{
    ++g_checks;
    if (!ok) {
        ++g_fails;
        std::printf("  FAIL %s:%d  %s\n", file, line, expr);
    }
}

static void check_near(double got, double want, double eps, const char *expr, const char *file, int line)
{
    ++g_checks;
    if (std::fabs(got - want) > eps) {
        ++g_fails;
        std::printf("  FAIL %s:%d  %s  (got %.6f, want %.6f)\n", file, line, expr, got, want);
    }
}

#define CHECK(cond)        check((cond), #cond, __FILE__, __LINE__)
#define CHECK_NEAR(a, b)   check_near((a), (b), 1e-6, #a " ~= " #b, __FILE__, __LINE__)

// A 2x2 calibration grid: two acceleration bands (1000, 2000 mm/s^2), each with two
// volumetric-flow points (3.84, 7.68 mm^3/s).
//   accel 1000:  flow 3.84 -> PA 0.040,  flow 7.68 -> PA 0.030
//   accel 2000:  flow 3.84 -> PA 0.030,  flow 7.68 -> PA 0.020
static const char *k_grid_model =
    "PA, volumetric_flow, acceleration\n"   // header line must be ignored
    "0.040, 3.84, 1000\n"
    "0.030, 7.68, 1000\n"
    "\n"                                     // blank line must be ignored
    "0.030, 3.84, 2000\n"
    "0.020, 7.68, 2000\n";

static void test_parse()
{
    AdaptivePAModel model(k_grid_model);
    CHECK(!model.empty());
    CHECK(model.size() == 4);   // header + blank skipped, four data rows kept
}

static void test_grid_recovery()
{
    AdaptivePAModel model(k_grid_model);
    CHECK_NEAR(model.evaluate(3.84, 1000), 0.040);
    CHECK_NEAR(model.evaluate(7.68, 1000), 0.030);
    CHECK_NEAR(model.evaluate(3.84, 2000), 0.030);
    CHECK_NEAR(model.evaluate(7.68, 2000), 0.020);
}

static void test_flow_interpolation()
{
    AdaptivePAModel model(k_grid_model);
    CHECK_NEAR(model.evaluate(5.76, 1000), 0.035);   // midpoint flow, accel 1000
    CHECK_NEAR(model.evaluate(5.76, 2000), 0.025);   // midpoint flow, accel 2000
}

static void test_accel_interpolation()
{
    AdaptivePAModel model(k_grid_model);
    CHECK_NEAR(model.evaluate(3.84, 1500), 0.035);   // flow 3.84, accel midpoint
    CHECK_NEAR(model.evaluate(5.76, 1500), 0.030);   // full bilinear centre
}

static void test_edge_clamping()
{
    AdaptivePAModel model(k_grid_model);
    CHECK_NEAR(model.evaluate(0.5, 1000),  0.040);   // flow below range -> 3.84
    CHECK_NEAR(model.evaluate(99.0, 1000), 0.030);   // flow above range -> 7.68
    CHECK_NEAR(model.evaluate(3.84, 100),   0.040);  // accel below range -> 1000 band
    CHECK_NEAR(model.evaluate(3.84, 99999), 0.030);  // accel above range -> 2000 band
    CHECK_NEAR(model.evaluate(0.5, 100),    0.040);  // both low  -> corner
    CHECK_NEAR(model.evaluate(99.0, 99999), 0.020);  // both high -> corner
}

static void test_rounding()
{
    AdaptivePAModel model(k_grid_model);
    // flow 5.12 is 1/3 of the way from 3.84 to 7.68 in the accel=1000 band:
    //   0.040 + (0.030 - 0.040) * (1/3) = 0.036667 -> rounds to 0.037 at step 1e-3.
    CHECK_NEAR(model.evaluate(5.12, 1000), 0.037);
}

static void test_empty_fallback()
{
    AdaptivePAModel empty;
    CHECK(empty.empty());
    CHECK_NEAR(empty.evaluate(5.0, 1500, 0.123), 0.123);
    CHECK_NEAR(empty.evaluate(5.0, 1500, 0.0),   0.0);

    AdaptivePAModel header_only("PA, flow, accel\n# comment\n");
    CHECK(header_only.empty());
    CHECK_NEAR(header_only.evaluate(5.0, 1500, 0.456), 0.456);
}

static void test_single_point()
{
    AdaptivePAModel model("0.025, 5.0, 1500");
    CHECK(model.size() == 1);
    CHECK_NEAR(model.evaluate(1.0, 500),    0.025);
    CHECK_NEAR(model.evaluate(5.0, 1500),   0.025);
    CHECK_NEAR(model.evaluate(50.0, 50000), 0.025);
}

static void test_single_accel_level()
{
    AdaptivePAModel model("0.040, 3.84, 1000\n0.020, 7.68, 1000\n");
    CHECK(model.size() == 2);
    CHECK_NEAR(model.evaluate(5.76, 1000),  0.030);   // flow interpolation
    CHECK_NEAR(model.evaluate(5.76, 50000), 0.030);   // accel ignored (clamped)
    CHECK_NEAR(model.evaluate(3.84, 1),     0.040);
    CHECK_NEAR(model.evaluate(7.68, 1),     0.020);
}

int main()
{
    test_parse();
    test_grid_recovery();
    test_flow_interpolation();
    test_accel_interpolation();
    test_edge_clamping();
    test_rounding();
    test_empty_fallback();
    test_single_point();
    test_single_accel_level();

    std::printf("\nAdaptivePA: %d checks, %d failed\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
