#include <catch2/catch.hpp>

#include <cmath>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/BridgeOverInfill.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "test_data.hpp"

using namespace Slic3r;

static Flow bridge_flow()
{
    // Width and spacing are both 0.5 mm, so the sample lines are easy to place.
    return Flow::bridging_flow(0.5f, 0.4f);
}

static Polygon rect_mm(double x0, double y0, double x1, double y1)
{
    Polygon polygon;
    polygon.points = {
        Point::new_scale(x0, y0),
        Point::new_scale(x1, y0),
        Point::new_scale(x1, y1),
        Point::new_scale(x0, y1),
    };
    return polygon;
}

static Line line_mm(double x0, double y0, double x1, double y1)
{
    return Line{Point::new_scale(x0, y0), Point::new_scale(x1, y1)};
}

// Lowest point of the expanded bridge. Bridge angle is pi/2, so lines run in Y
// and a downward extension makes this smaller.
static double lowest_mm(const Polygons &polys)
{
    REQUIRE(!polys.empty());
    return unscale<double>(get_extents(polys).min.y());
}

TEST_CASE("Internal bridge expansion does not leave a three line tongue", "[BridgeSupport]")
{
    const Flow flow = bridge_flow();
    // Vertical bridge lines. Samples land on x = 0, 0.5, ..., 19.5.
    const Polygons bridge{rect_mm(0, 0, 20, 8)};
    const double angle = PI / 2.;

    SECTION("three samples that alone reach a far anchor are pulled back") {
        // The near anchor stops short of x = 9, 9.5 and 10. Only those three
        // lines can see the anchor 6 mm further down.
        // The line above the bridge is the boundary the real slice passes in.
        // Without it the probes only cover the anchors and never cross the bridge.
        const Lines anchors{
            line_mm(-1, 9, 21, 9),
            line_mm(-1, -0.25, 8.75, -0.25),
            line_mm(10.25, -0.25, 21, -0.25),
            line_mm(8.8, -6, 10.2, -6),
        };
        const Polygons expanded = expand_bridged_area_to_anchors(bridge, anchors, flow, angle);
        const double lowest = lowest_mm(expanded);
        INFO("lowest y mm " << lowest);
        CHECK(lowest < -0.2);
        CHECK(lowest > -2.0);
        const Polygons far{rect_mm(-1, -7, 21, -2)};
        CHECK(std::abs(area(intersection(expanded, far))) < double(scale_(0.5)) * double(scale_(0.5)));
    }

    SECTION("three samples at the end of the bridge are pulled back too") {
        const Lines anchors{
            line_mm(-1, 9, 21, 9),
            line_mm(-1, -0.25, 18.25, -0.25),
            line_mm(18.4, -6, 21, -6),
        };
        const Polygons expanded = expand_bridged_area_to_anchors(bridge, anchors, flow, angle);
        const double lowest = lowest_mm(expanded);
        INFO("lowest y mm " << lowest);
        CHECK(lowest > -2.0);
    }

    SECTION("the whole bridge reaching one anchor is kept") {
        const Lines anchors{
            line_mm(-1, 9, 21, 9),
            line_mm(-1, -6, 21, -6),
        };
        const Polygons expanded = expand_bridged_area_to_anchors(bridge, anchors, flow, angle);
        const double lowest = lowest_mm(expanded);
        INFO("lowest y mm " << lowest);
        CHECK(lowest < -5.0);
        const Polygons far{rect_mm(-1, -7, 21, -5)};
        CHECK(std::abs(area(intersection(expanded, far))) > double(scale_(10)) * double(scale_(1)));
    }
}

namespace {

struct FloatingInfill {
    int    layer;
    double z;
    double length_mm;
    bool   solid;
};

// Non-bridge infill that never touches the previous layer. A line that merely
// crosses a sparse line, or ends on a perimeter, is supported and is ignored.
std::vector<FloatingInfill> floating_infill(const Print &print)
{
    std::vector<FloatingInfill> found;
    for (const PrintObject *object : print.objects()) {
        const Layer *previous = nullptr;
        int          index    = 0;
        for (const Layer *layer : object->layers()) {
            if (previous != nullptr) {
                Polygons covered;
                for (const LayerRegion *region : previous->regions()) {
                    region->perimeters().polygons_covered_by_width(covered, float(SCALED_EPSILON));
                    region->fills().polygons_covered_by_width(covered, float(SCALED_EPSILON));
                    region->thin_fills().polygons_covered_by_width(covered, float(SCALED_EPSILON));
                }
                covered = expand(union_(covered), float(scale_(0.15)));

                struct Collect : ExtrusionVisitorRecursiveConst {
                    using ExtrusionVisitorRecursiveConst::use;
                    std::vector<std::pair<ExtrusionRole, Polyline>> paths;
                    void use(const ExtrusionPath &path) override
                    {
                        if (path.role().is_infill() && !path.role().is_bridge() && !path.empty())
                            paths.emplace_back(path.role(), path.polyline.to_polyline());
                    }
                    void use(const ExtrusionPath3D &path) override { use(static_cast<const ExtrusionPath &>(path)); }
                } collect;
                for (const LayerRegion *region : layer->regions())
                    region->fills().visit(collect);

                for (const auto &path : collect.paths) {
                    const double length_mm = unscale<double>(path.second.length());
                    const bool   solid     = path.first.is_solid_infill();
                    // Nothing under the line at all: it does not meet the previous layer anywhere.
                    if (intersection_pl(path.second, covered).empty()) {
                        if (length_mm > 2.0)
                            found.push_back(FloatingInfill{index, layer->print_z, length_mm, solid});
                        continue;
                    }
                    // Solid infill may touch a wall at its ends and still span an open gap.
                    // That span is unsupported. Sparse infill is allowed to cross open cells.
                    if (!solid)
                        continue;
                    for (const Polyline &gap : diff_pl(path.second, covered)) {
                        const double gap_mm = unscale<double>(gap.length());
                        if (gap_mm > 2.0)
                            found.push_back(FloatingInfill{index, layer->print_z, gap_mm, true});
                    }
                }
            }
            previous = layer;
            ++index;
        }
    }
    return found;
}

void require_supported(const std::vector<FloatingInfill> &found)
{
    std::ostringstream report;
    for (const FloatingInfill &item : found) {
        report << "layer " << item.layer << " z=" << item.z
               << " mm  " << (item.solid ? "solid" : "sparse")
               << " infill " << item.length_mm << " mm with nothing under it\n";
    }
    INFO(report.str());
    REQUIRE(found.empty());
}

DynamicPrintConfig bridge_support_config()
{
    DynamicPrintConfig config;
    config.set_deserialize_strict({
        {"layer_height", "0.2"},
        {"first_layer_height", "0.2"},
        {"fill_density", "18%"},
        {"fill_pattern", "adaptivecubic"},
        {"top_solid_layers", "3"},
        {"bottom_solid_layers", "3"},
        {"perimeters", "2"},
        {"internal_bridge_expansion", "1"},
        {"internal_bridge_min_width", "300%"},
        {"ensure_vertical_shell_thickness", "enabled_old"},
        {"infill_anchor", "600%"},
        {"fill_angle_cross", "1"},
    });
    return config;
}

} // namespace

TEST_CASE("Solid and sparse infill is not extruded over empty space", "[BridgeSupport]")
{
    Model model;
    Print print;
    const DynamicPrintConfig config = bridge_support_config();
    // A block tall enough for sparse infill under a solid top. Solid infill that
    // spans a gap in that infill, with nothing on the layer below, fails the print.
    Test::init_print({make_cube(30., 30., 8.)}, print, model, config);
    print.process();
    require_supported(floating_infill(print));
}

TEST_CASE("Bowflex slice has no infill with nothing under it", "[BridgeSupport]")
{
    TriangleMesh mesh;
    REQUIRE(mesh.ReadSTLFile(BRIDGE_SUPPORT_MODEL));
    Model model;
    Print print;
    const DynamicPrintConfig config = bridge_support_config();
    Test::init_print({mesh}, print, model, config);
    print.process();
    require_supported(floating_infill(print));
}
