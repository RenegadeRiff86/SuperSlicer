///|/ Copyright (c) Prusa Research 2021 - 2023 Vojtěch Bubník @bubnikv, Lukáš Matěna @lukasmatena, Lukáš Hejl @hejllukas
///|/ Copyright (c) SuperSlicer 2023 Remi Durand @supermerill
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "Brim.hpp"

#include "clipper/clipper_z.hpp"
#include "ClipperUtils.hpp"
#include "EdgeGrid.hpp"
#include "ExtrusionEntityCollection.hpp"
#include "Flow.hpp"
#include "Layer.hpp"
#include "Print.hpp"
#include "PrintConfig.hpp"
#include "ShortestPath.hpp"
#include "libslic3r.h"

#include <algorithm>
#include <numeric>
#include <unordered_set>
#include <mutex>

#include <oneapi/tbb/parallel_for.h>
#include <boost/thread/lock_guard.hpp>

#ifndef NDEBUG
    // #define BRIM_DEBUG_TO_SVG
#endif

#if defined(BRIM_DEBUG_TO_SVG)
    #include "SVG.hpp"
#endif

namespace Slic3r {

// (An '#if 0' copy of the historical PrusaSlicer brim generator lived here; removed, see git history.)
//superslicer

// Nest contour loops (same as in perimetergenerator): reparent each loop under the loop that
// contains it, so children are extruded right after their parent.
static void nest_brim_loops(std::vector<std::vector<BrimLoop>>& loops, bool reversed) {
    for (int d = loops.size() - 1; d >= 1; --d) {
        std::vector<BrimLoop>& contours_d = loops[d];
        // loop through all contours having depth == d
        for (int i = 0; i < static_cast<int>(contours_d.size()); ++i) {
            const BrimLoop& loop = contours_d[i];
            // find the contour loop that contains it
            for (int t = d - 1; t >= 0; --t) {
                for (size_t j = 0; j < loops[t].size(); ++j) {
                    BrimLoop& candidate_parent = loops[t][j];
                    bool test = reversed
                        ? loop.polygon().contains(candidate_parent.lines.front().first_point())
                        : candidate_parent.polygon().contains(loop.lines.front().first_point());
                    if (test) {
                        candidate_parent.children.push_back(loop);
                        contours_d.erase(contours_d.begin() + i);
                        --i;
                        goto NEXT_CONTOUR;
                    }
                }
            }
            //didn't find a contour: add it as a root loop
            loops[0].push_back(loop);
            contours_d.erase(contours_d.begin() + i);
            --i;
        NEXT_CONTOUR:;
        }
    }
    for (int i = loops.size() - 1; i > 0; --i) {
        if (loops[i].empty()) {
            loops.erase(loops.begin() + i);
        }
    }
}

// Cut loops where they enter a forbidden region, children before their parent.
static void cut_loops_to_frontiers(std::vector<std::vector<BrimLoop>>& loops, const Polygons& frontiers, const Flow& flow, bool reversed) {
    //def
    //cut loops if they go inside a forbidden region
    std::function<void(BrimLoop&)> cut_loop = [&frontiers, &flow, reversed](BrimLoop& to_cut) {
        Polylines result;
        if (to_cut.is_loop) {
            to_cut.polygon().assert_valid();
            for(auto& poly : frontiers) poly.assert_valid();
            result = intersection_pl(Polygons{ to_cut.polygon() }, frontiers);
        } else {
            result = intersection_pl(to_cut.lines, frontiers);
        }
        //remove too small segments
        for (int i = 0; i < result.size(); i++) {
            if (result[i].length() < flow.scaled_width() * 2) {
                result.erase(result.begin() + i);
                i--;
            }
        }
        for(auto& poly : result) poly.assert_valid();
        if (result.empty()) {
            to_cut.lines.clear();
        } else {
            to_cut.lines = result;
            if (reversed) {
                std::reverse(to_cut.lines.begin(), to_cut.lines.end());
            }
            to_cut.is_loop = false;
        }
    };
    //calls, deep-first
    std::list< std::pair<BrimLoop*, int>> cut_child_first;
    for (std::vector<BrimLoop>& loop_level : loops) {
        for (BrimLoop& loop : loop_level) {
            cut_child_first.emplace_front(&loop, 0);
            //flat recurtion
            while (!cut_child_first.empty()) {
                if (cut_child_first.front().first->children.size() <= cut_child_first.front().second) {
                    //if no child to cut, cut ourself and pop
                    cut_loop(*cut_child_first.front().first);
                    cut_child_first.pop_front();
                } else {
                    // more child to cut, push the next
                    cut_child_first.front().second++;
                    cut_child_first.emplace_front(&cut_child_first.front().first->children[cut_child_first.front().second - 1], 0);
                }
            }
        }
    }
}

// Push one brim loop into extrusions: its own lines first, then its children, in the right order.
static void extrude_brim_loop(BrimLoop& to_cut, ExtrusionEntityCollection* parent, float mm3_per_mm, float width, float height, bool extrude_cw) {
    DEBUG_VISIT(*parent, LoopAssertVisitor())
    bool i_have_line = to_cut.lines.size() > 0 && to_cut.lines.front().size() > 0 && to_cut.lines.front().is_valid();
    if (!i_have_line && to_cut.children.empty()) {
        //nothing
    } else if (i_have_line && to_cut.children.empty()) {
        ExtrusionEntitiesPtr to_add;
        for (Polyline& pline : to_cut.lines) {
            pline.assert_valid();
            assert(pline.size() > 0);
            if (extrude_cw) {
                pline.reverse();
            }
            if (pline.back() == pline.front()) {
                ExtrusionPath path({ExtrusionRole::Skirt, {mm3_per_mm, width, height}}, false);
                path.polyline = pline;
                to_add.push_back(new ExtrusionLoop(std::move(path), elrSkirt));
            } else {
                ExtrusionPath *extrusion_path = new ExtrusionPath({ExtrusionRole::Skirt, {mm3_per_mm, width, height}}, false);
                extrusion_path->polyline = pline;
                to_add.push_back(extrusion_path);
            }
            DEBUG_VISIT(*to_add.back(), LoopAssertVisitor())
        }
        parent->append(std::move(to_add));
    } else if (!i_have_line && !to_cut.children.empty()) {
        if (to_cut.children.size() == 1) {
            extrude_brim_loop(to_cut.children[0], parent, mm3_per_mm, width, height, extrude_cw);
        } else {
            ExtrusionEntityCollection* mycoll = new ExtrusionEntityCollection();
            for (BrimLoop& child : to_cut.children)
                extrude_brim_loop(child, mycoll, mm3_per_mm, width, height, extrude_cw);
            //remove un-needed collection if possible
            if (mycoll->entities().size() == 1) {
                parent->append(*mycoll->entities().front()); //add clone
                DEBUG_VISIT(*parent->entities().back(), LoopAssertVisitor())
                delete mycoll; // remove coll & content
            } else if (mycoll->entities().size() == 0) {
                delete mycoll;// remove coll & content
            } else {
                parent->append(ExtrusionEntitiesPtr{ mycoll });
                DEBUG_VISIT(*parent->entities().back(), LoopAssertVisitor())
            }
        }
    } else {
        ExtrusionEntityCollection* print_me_first = new ExtrusionEntityCollection();
        print_me_first->set_can_sort_reverse(false, false);
        parent->append(ExtrusionEntitiesPtr{ print_me_first });
        ExtrusionEntitiesPtr to_add;
        for (Polyline& pline : to_cut.lines) {
            assert(pline.size() > 0);
            pline.assert_valid();
            if (extrude_cw) {
                pline.reverse();
            }
            if (pline.back() == pline.front()) {
                ExtrusionPath path({ExtrusionRole::Skirt, {mm3_per_mm, width, height}}, false);
                path.polyline = pline;
                to_add.push_back(new ExtrusionLoop(std::move(path), elrSkirt));
            } else {
                ExtrusionPath *extrusion_path = new ExtrusionPath({ExtrusionRole::Skirt, {mm3_per_mm, width, height}}, false);
                extrusion_path->polyline = pline;
                to_add.push_back(extrusion_path);
            }
            DEBUG_VISIT(*to_add.back(), LoopAssertVisitor())
        }
        print_me_first->append(std::move(to_add));
        if (to_cut.children.size() == 1) {
            extrude_brim_loop(to_cut.children[0], print_me_first, mm3_per_mm, width, height, extrude_cw);
        } else {
            ExtrusionEntityCollection* children = new ExtrusionEntityCollection();
            for (BrimLoop& child : to_cut.children)
                extrude_brim_loop(child, children, mm3_per_mm, width, height, extrude_cw);
            DEBUG_VISIT(*children, LoopAssertVisitor())
            //remove un-needed collection if possible
            if (children->entities().size() == 1) {
                print_me_first->append(*children->entities().front());
                delete children;
            } else if (children->entities().size() == 0) {
                delete children;
            } else {
                print_me_first->append(ExtrusionEntitiesPtr{ children });
            }
        }
        assert(print_me_first->entities().size() > 0);
    }
    DEBUG_VISIT(*parent, LoopAssertVisitor())
}

void extrude_brim_from_tree(const Print& print, std::vector<std::vector<BrimLoop>>& loops, const Polygons& frontiers, const Flow& flow, ExtrusionEntityCollection& out, bool reversed/*= false*/) {
    if (loops.empty())
        return;

    bool extrude_cw = print.default_region_config().perimeter_direction.value == pdCW_CCW ||
                print.default_region_config().perimeter_direction.value == pdCW_CW;

    nest_brim_loops(loops, reversed);

    cut_loops_to_frontiers(loops, frontiers, flow, reversed);

    print.throw_if_canceled();

    if (loops.empty()) {
        BOOST_LOG_TRIVIAL(error) << "Failed to extrude brim: no loops to extrude, are you sure your settings are ok?";
        return;
    }

    //push into extrusions, in the right order
    float mm3_per_mm = float(flow.mm3_per_mm());
    float width = float(flow.width());
    float height = float(print.get_min_first_layer_height());
    for (BrimLoop& loop : loops[0]) {
        extrude_brim_loop(loop, &out, mm3_per_mm, width, height, extrude_cw);
    }
}

/// reorder & join polyline if their ending are near enough, then extrude the brim from the polyline into 'out'.
Polylines reorder_brim_polyline(Polylines lines, ExtrusionEntityCollection& out, const Flow& flow) {
    //reorder them
    std::sort(lines.begin(), lines.end(), [](const Polyline& a, const Polyline& b)->bool { return a.closest_point(Point(0, 0))->y() < b.closest_point(Point(0, 0))->y(); });
    Polylines lines_sorted;
    Polyline* previous = NULL;
    Polyline* best = NULL;
    double best_dist = -1;
    size_t best_idx = 0;
    while (lines.size() > 0) {
        if (previous == NULL) {
            lines_sorted.push_back(lines.back());
            previous = &lines_sorted.back();
            lines.erase(lines.end() - 1);
        } else {
            best = NULL;
            best_dist = -1;
            best_idx = 0;
            for (size_t i = 0; i < lines.size(); ++i) {
                Polyline& viewed_line = lines[i];
                double dist = viewed_line.points.front().distance_to(previous->points.front());
                dist = std::min(dist, viewed_line.points.front().distance_to(previous->points.back()));
                dist = std::min(dist, viewed_line.points.back().distance_to(previous->points.front()));
                dist = std::min(dist, viewed_line.points.back().distance_to(previous->points.back()));
                if (dist < best_dist || best == NULL) {
                    best = &viewed_line;
                    best_dist = dist;
                    best_idx = i;
                }
            }
            if (best != NULL) {
                //copy new line inside the sorted array.
                lines_sorted.push_back(lines[best_idx]);
                lines.erase(lines.begin() + best_idx);

                //connect if near enough
                if (lines_sorted.size() > 1) {
                    size_t idx = lines_sorted.size() - 2;
                    bool connect = false;
                    if (lines_sorted[idx].points.back().distance_to(lines_sorted[idx + 1].points.front()) < flow.scaled_spacing() * 2) {
                        connect = true;
                    } else if (lines_sorted[idx].points.back().distance_to(lines_sorted[idx + 1].points.back()) < flow.scaled_spacing() * 2) {
                        lines_sorted[idx + 1].reverse();
                        connect = true;
                    } else if (lines_sorted[idx].points.front().distance_to(lines_sorted[idx + 1].points.front()) < flow.scaled_spacing() * 2) {
                        lines_sorted[idx].reverse();
                        connect = true;
                    } else if (lines_sorted[idx].points.front().distance_to(lines_sorted[idx + 1].points.back()) < flow.scaled_spacing() * 2) {
                        lines_sorted[idx].reverse();
                        lines_sorted[idx + 1].reverse();
                        connect = true;
                    }

                    if (connect) {
                        //connect them
                        lines_sorted[idx].points.insert(
                            lines_sorted[idx].points.end(),
                            lines_sorted[idx + 1].points.begin(),
                            lines_sorted[idx + 1].points.end());
                        lines_sorted.erase(lines_sorted.begin() + idx + 1);
                        idx--;
                    }
                }

                //update last position
                previous = &lines_sorted.back();
            }
        }
    }

    return lines_sorted;
}

// Collect the first-layer contours of one object, optionally grown by the brim separation,
// with or without the holes depending on the brim configuration.
static ExPolygons object_first_layer_islands(const PrintObject& object, const PrintObjectConfig& brim_config, coord_t brim_offset) {
    ExPolygons object_islands;
    for (const ExPolygon& expoly : object.layers().front()->lslices()) {
        if (brim_config.brim_inside_holes && brim_config.brim_width_interior == 0) {
            if (brim_offset == 0) {
                object_islands.push_back(expoly);
            } else {
                for (ExPolygon& grown_expoly : offset_ex(expoly, brim_offset)) {
                    object_islands.push_back(std::move(grown_expoly));
                }
            }
        } else {
            if (brim_offset == 0) {
                object_islands.push_back(to_expolygon(expoly.contour));
            } else {
                for (ExPolygon& grown_expoly : offset_ex(to_expolygon(expoly.contour), brim_offset)) {
                    object_islands.push_back(std::move(grown_expoly));
                }
            }
        }
    }
    return object_islands;
}

// Simplify the islands, merge them, then re-simplify (union_safety_offset_ex can shorten segments below epsilon).
// Note: 'accumulated' deliberately keeps the pass-1 simplified islands too — the caller uses the whole
// accumulated set as its "do not brim over" area, exactly as the historical inline code did.
static void simplify_and_merge_islands(ExPolygons& islands, ExPolygons& accumulated, coordf_t scaled_resolution_brim) {
    for (ExPolygon& expoly : islands) {
        for (ExPolygon& simple_expoly : expoly.simplify(scaled_resolution_brim)) {
            simple_expoly.assert_valid();
            accumulated.emplace_back(std::move(simple_expoly));
        }
    }
    for (ExPolygon& expoly : accumulated) expoly.assert_valid();
    islands = union_safety_offset_ex(accumulated);
    // union_safety_offset_ex can shorten segments below epsilon. So we need to re-simplify a bit.
    for (ExPolygon& expoly : islands) {
        for (ExPolygon& simple_expoly : expoly.simplify(SCALED_EPSILON)) {
            simple_expoly.assert_valid();
            accumulated.emplace_back(std::move(simple_expoly));
        }
    }
    islands = accumulated;
    for (ExPolygon& expoly : islands) expoly.assert_valid();
}

// From the islands, build the concentric brim loops (deepest level first, reversed at the end) and collect
// the polygons the brim lines must not cross (island contours and the holes of the grown islands).
static std::vector<std::vector<BrimLoop>> build_brim_loops(const Print& print, ExPolygons islands, size_t num_loops,
        coord_t scaled_spacing, coordf_t scaled_resolution_brim, Polygons& unbrimmable_polygons, ExPolygons& last_islands) {
    std::vector<std::vector<BrimLoop>> loops;
    ExPolygons bigger_islands;
    //grow a half of spacing, to go to the first extrusion polyline.
    for (ExPolygon& expoly : islands) {
        expoly.contour.assert_valid();
        unbrimmable_polygons.push_back(expoly.contour);
        //do it separately because we don't want to union them
        for (ExPolygon& big_expoly : ensure_valid(scaled_resolution_brim, offset_ex(expoly, double(scaled_spacing) * 0.5, jtSquare))) {
            big_expoly.assert_valid();
            bigger_islands.emplace_back(big_expoly);
            unbrimmable_polygons.insert(unbrimmable_polygons.end(), big_expoly.holes.begin(), big_expoly.holes.end());
        }
    }
    islands = bigger_islands;
    for (size_t i = 0; i < num_loops; ++i) {
        loops.emplace_back();
        print.throw_if_canceled();
        // only grow the contour, not holes
        bigger_islands.clear();
        if (i > 0) {
            for (ExPolygon &expoly : last_islands) {
                expoly.assert_valid();
                for (ExPolygon &big_contour : ensure_valid(scaled_resolution_brim, offset_ex(expoly, double(scaled_spacing), jtSquare))) {
                    big_contour.assert_valid();
                    bigger_islands.push_back(big_contour);
                    Polygons simplifiesd_big_contour = big_contour.contour.simplify(scaled_resolution_brim);
                    if (simplifiesd_big_contour.size() == 1) {
                        bigger_islands.back().contour = simplifiesd_big_contour.front();
                    }
                }
            }
        } else {
            bigger_islands = islands;
        }
        last_islands = union_ex(bigger_islands);
        ensure_valid(last_islands, scaled_resolution_brim);
        for (ExPolygon &expoly : last_islands) {
            expoly.assert_valid();
            loops.back().emplace_back(expoly.contour);
            // also add hole, in case of it's merged with a contour. see supermerill/SuperSlicer/issues/3050
            for (Polygon &hole : expoly.holes) {
                hole.assert_valid();
                // but remove the points that are inside the holes of islands
                for (ExPolygon &pl : diff_ex(Polygons{hole}, unbrimmable_polygons)) {
                    pl.assert_valid();
                    loops[i].emplace_back(pl.contour);
                }
            }
        }
    }

    std::reverse(loops.begin(), loops.end());
    return loops;
}

//note: unbrimmable must keep its ordering. don't union_ex it.

//TODO: test if no regression vs old _make_brim.
// this new one can extrude brim for an object inside an other object.
void make_brim(const Print& print, const Flow& flow, const PrintObjectPtrs& objects, ExPolygons& unbrimmable, ExtrusionEntityCollection& out) {
    const coord_t scaled_spacing = flow.scaled_spacing();
    const PrintObjectConfig& brim_config = objects.front()->config();
    coord_t brim_offset = scale_t(brim_config.brim_separation.value);
    ExPolygons    islands;
    for (PrintObject* object : objects) {
        ExPolygons object_islands = object_first_layer_islands(*object, brim_config, brim_offset);
        if (!object->support_layers().empty()) {
            ExPolygons polys = union_ex(object->support_layers().front()->support_fills.polygons_covered_by_spacing(flow.spacing_ratio(), float(SCALED_EPSILON)));
            for (ExPolygon& poly : polys) {
                if (brim_offset == 0) {
                    object_islands.push_back(std::move(poly));
                } else {
                    append(object_islands, offset_ex(ExPolygons{ poly }, brim_offset));
                }
            }
        }
        islands.reserve(islands.size() + object_islands.size() * object->instances().size());
        for (const PrintInstance& pt : object->instances()) {
            for (ExPolygon& poly : object_islands) {
                islands.push_back(poly);
                islands.back().translate(pt.shift.x(), pt.shift.y());
            }
        }
    }

    print.throw_if_canceled();

    //simplify & merge
    //get brim resolution (lower resolution if no arc fitting)
    coordf_t scaled_resolution_brim = (print.config().arc_fitting.value != ArcFittingType::Disabled)? scale_d(print.config().resolution) : scale_d(print.config().resolution_internal) / 10;
    scaled_resolution_brim = std::max(scaled_resolution_brim, coordf_t(SCALED_EPSILON * 10));
    ExPolygons unbrimmable_areas;
    simplify_and_merge_islands(islands, unbrimmable_areas, scaled_resolution_brim);

    //get the brimmable area
    const size_t num_loops = size_t(floor(std::max(0., (brim_config.brim_width.value - brim_config.brim_separation.value)) / flow.spacing()));
    ExPolygons brimmable_areas;
    for (ExPolygon& expoly : islands) {
        expoly.contour.assert_valid();
        for (Polygon &poly : ensure_valid(scaled_resolution_brim, offset(expoly.contour, num_loops * scaled_spacing, jtSquare))) {
            poly.assert_valid();
            brimmable_areas.emplace_back();
            brimmable_areas.back().contour = poly;
            brimmable_areas.back().contour.make_counter_clockwise();
            brimmable_areas.back().holes.push_back(expoly.contour);
            brimmable_areas.back().holes.back().make_clockwise();
        }
    }
    brimmable_areas = union_ex(brimmable_areas);
    print.throw_if_canceled();

    //don't collide with objects
    brimmable_areas = diff_ex(brimmable_areas, unbrimmable_areas,   ApplySafetyOffset::Yes);
    brimmable_areas = diff_ex(brimmable_areas, unbrimmable,         ApplySafetyOffset::Yes);

    print.throw_if_canceled();

    //now get all holes, use them to create loops
    Polygons unbrimmable_polygons;
    ExPolygons last_islands;
    std::vector<std::vector<BrimLoop>> loops = build_brim_loops(print, std::move(islands), num_loops,
        scaled_spacing, scaled_resolution_brim, unbrimmable_polygons, last_islands);

    // Using offset(expoly.contour, num_loops* scaled_spacing, jtSquare) create a different result than the incremental 'loops' creation.
    //   so i have to restrict with the biggest (first) loop from loops (last_islands).
    //intersection
    brimmable_areas = intersection_ex(brimmable_areas, offset_ex(last_islands, double(scaled_spacing) * 0.5, jtSquare));
    Polygons frontiers;
    ensure_valid(brimmable_areas, scaled_resolution_brim);
    //use contour from brimmable_areas (external frontier)
    for (ExPolygon& expoly : brimmable_areas) {
        expoly.assert_valid();
        frontiers.push_back(expoly.contour);
        frontiers.back().make_counter_clockwise();
    }
    // add internal frontier
    frontiers.insert(frontiers.begin(), unbrimmable_polygons.begin(), unbrimmable_polygons.end());

    extrude_brim_from_tree(print, loops, frontiers, flow, out, false);
    DEBUG_VISIT(out, LoopAssertVisitor())

    unbrimmable.insert(unbrimmable.end(), brimmable_areas.begin(), brimmable_areas.end());
}

// Create the round ear shape, centered on each of the given points.
static ExPolygons make_mouse_ears(const Points& pt_ears, coord_t size_ear) {
    Polygon point_round;
    for (size_t i = 0; i < POLY_SIDES; i++) {
        double angle = (2.0 * PI * i) / POLY_SIDES;
        point_round.points.emplace_back(size_ear * cos(angle), size_ear * sin(angle));
    }
    ExPolygons mouse_ears_ex;
    for (Point pt : pt_ears) {
        mouse_ears_ex.emplace_back();
        mouse_ears_ex.back().contour = point_round;
        mouse_ears_ex.back().contour.translate(pt);
    }
    return mouse_ears_ex;
}

// Collect the (per-instance translated) first-layer islands of all objects, the ear anchor points on
// their contours, and the support areas that must not receive brim.
static ExPolygons collect_brim_ears_islands(const PrintObjectPtrs& objects, const PrintObjectConfig& brim_config,
        const Flow& flow, coord_t brim_offset, Points& pt_ears, ExPolygons& unbrimmable_with_support) {
    ExPolygons islands;
    for (PrintObject* object : objects) {
        ExPolygons object_islands = object_first_layer_islands(*object, brim_config, brim_offset);
        ExPolygons support_island;
        if (!object->support_layers().empty()) {
            ExPolygons polys = union_ex(object->support_layers().front()->support_fills.polygons_covered_by_spacing(flow.spacing_ratio(), float(SCALED_EPSILON)));
            //put ears over supports unless it's more than 30% fill
            if (object->config().raft_first_layer_density.get_abs_value(1.) > 0.3) {
                for (ExPolygon& poly : polys) {
                    if (brim_offset == 0) {
                        object_islands.push_back(std::move(poly));
                    } else {
                        append(object_islands, offset_ex(ExPolygons{ poly }, brim_offset));
                    }
                }
            } else {
                // offset2+- to avoid bits of brim inside the raft
                append(support_island, closing_ex(polys, flow.scaled_width() * 2));
            }
        }
        islands.reserve(islands.size() + object_islands.size() * object->instances().size());
        coord_t ear_detection_length = std::max(scale_t(object->config().brim_ears_detection_length.value), SCALED_EPSILON);
        // duplicate & translate for each instance
        for (const PrintInstance& copy_pt : object->instances()) {
            for (const ExPolygon& poly : object_islands) {
                islands.push_back(poly);
                islands.back().translate(copy_pt.shift.x(), copy_pt.shift.y());
                Polygon decimated_polygon;
                // brim_ears_detection_length codepath
                if (ear_detection_length > 0) {
                    //decimate polygon
                    Points points = poly.contour.points;
                    points.push_back(points.front());
                    points = MultiPoint::douglas_peucker(points, ear_detection_length);
                    if (points.size() > 4) { //don't decimate if it's going to be below 4 points, as it's surely enough to fill everything anyway
                        points.erase(points.end() - 1);
                        decimated_polygon.points = points;
                    } else {
                        decimated_polygon.points = MultiPoint::douglas_peucker(poly.contour.points, SCALED_EPSILON);
                    }
                }
                Points pts = decimated_polygon.convex_points(0, brim_config.brim_ears_max_angle.value * PI / 180.0);
                for (const Point& p : pts) {
                    pt_ears.push_back(p);
                    pt_ears.back() += (copy_pt.shift);
                }
            }
            // also for support-fobidden area
            for (const ExPolygon& poly : support_island) {
                unbrimmable_with_support.push_back(poly);
                unbrimmable_with_support.back().translate(copy_pt.shift.x(), copy_pt.shift.y());
            }
        }
    }
    return islands;
}

void make_brim_ears(const Print& print, const Flow& flow, const PrintObjectPtrs& objects, ExPolygons& unbrimmable, ExtrusionEntityCollection& out) {
    const PrintObjectConfig& brim_config = objects.front()->config();
    coord_t brim_offset = scale_t(brim_config.brim_separation.value);
    Points pt_ears;
    ExPolygons unbrimmable_with_support = unbrimmable;
    ExPolygons islands = collect_brim_ears_islands(objects, brim_config, flow, brim_offset, pt_ears, unbrimmable_with_support);

    islands = union_safety_offset_ex(islands);

    //get the brimmable area (for the return value only)
    const size_t num_loops = size_t(floor((brim_config.brim_width.value - brim_config.brim_separation.value) / flow.spacing()));
    ExPolygons brimmable_areas;
    Polygons contours;
    Polygons holes;
    for (ExPolygon& expoly : islands) {
        for (Polygon poly : offset(expoly.contour, num_loops* flow.scaled_width(), jtSquare)) {
            contours.push_back(poly);
        }
        holes.push_back(expoly.contour);
    }
    brimmable_areas = diff_ex(union_(contours), union_(holes));
    brimmable_areas = diff_ex(brimmable_areas, unbrimmable_with_support, ApplySafetyOffset::Yes);

    print.throw_if_canceled();

    //get brim resolution (low resolution if no arc fitting)
    coordf_t scaled_resolution_brim = (print.config().arc_fitting.value != ArcFittingType::Disabled) ? scale_d(print.config().resolution) : scale_d(print.config().resolution_internal) / 10;
    scaled_resolution_brim = std::max(scaled_resolution_brim, coordf_t(SCALED_EPSILON * 10));

    //create ear pattern (identical for both fill patterns)
    const coord_t size_ear = (scale_t((brim_config.brim_width.value - brim_config.brim_separation.value)) - flow.scaled_spacing());
    const ExPolygons mouse_ears_ex = make_mouse_ears(pt_ears, size_ear);

    if (brim_config.brim_ears_pattern.value == InfillPattern::ipConcentric) {

        //create loops (same as standard brim)
        Polygons loops;
        islands = offset_ex(islands, -0.5f * double(flow.scaled_spacing()));
        for (size_t i = 0; i < num_loops; ++i) {
            print.throw_if_canceled();
            islands = offset_ex(islands, double(flow.scaled_spacing()), jtSquare);
            for (ExPolygon& expoly : islands) {
                Polygon poly = expoly.contour;
                poly.points.push_back(poly.points.front());
                Points p = MultiPoint::douglas_peucker(poly.points, scaled_resolution_brim);
                p.pop_back();
                poly.points = std::move(p);
                loops.push_back(poly);
            }
        }
        //order path with least travel possible
        loops = union_pt_chained_outside_in(loops);

        //intersection
        ExPolygons mouse_ears_area = intersection_ex(mouse_ears_ex, brimmable_areas);
        Polylines lines = intersection_pl(loops, to_polygons(mouse_ears_area));
        print.throw_if_canceled();

        //push into extrusions
        extrusion_entities_append_paths(
            out,
            //reorder & extrude them;
            reorder_brim_polyline(lines, out, flow),
            ExtrusionAttributes{ExtrusionRole::Skirt,
                                {float(flow.mm3_per_mm()), float(flow.width()),
                                    float(print.get_min_first_layer_height())}}
        );

        append(unbrimmable, offset_ex(mouse_ears_ex, flow.scaled_spacing() / 2));

    } else /* brim_config.brim_ears_pattern.value == InfillPattern::ipRectilinear */ {

        ExPolygons new_brim_area = intersection_ex(brimmable_areas, mouse_ears_ex);

        std::unique_ptr<Fill> filler = std::unique_ptr<Fill>(Fill::new_from_type(ipRectiWithPerimeter));
        filler->angle = 0;

        FillParams fill_params;
        fill_params.density = 1.f;
        fill_params.fill_exactly = true;
        fill_params.flow = flow;
        fill_params.role = ExtrusionRole::Skirt;
        filler->init_spacing(flow.spacing(), fill_params);
        for (const ExPolygon& expoly : new_brim_area) {
            Surface surface(stPosInternal | stDensSparse, expoly);
            filler->fill_surface_extrusion(&surface, fill_params, out.set_entities());
        }

        unbrimmable.insert(unbrimmable.end(), new_brim_area.begin(), new_brim_area.end());
    }

}

void make_brim_patch(const Print &print,
                     const Flow &flow,
                     const Polygons &patches,
                     ExPolygons &unbrimmable_areas,
                     ExtrusionEntityCollection &out)
{
    coordf_t scaled_resolution_brim = (print.config().arc_fitting.value != ArcFittingType::Disabled) ? scale_d(print.config().resolution) : scale_d(print.config().resolution_internal) / 10;
    scaled_resolution_brim = std::max(scaled_resolution_brim, coordf_t(SCALED_EPSILON * 10));
    for (const Polygon &contour : patches) {
        contour.simplify(scaled_resolution_brim);
        if (contour.empty()) {
            continue;
        }
        // remove unbrimable area
        ExPolygons next = diff_ex({ExPolygon(contour)}, unbrimmable_areas);
        ensure_valid(next, scaled_resolution_brim);

        // create polygons
        std::vector<std::vector<BrimLoop>> loops;
        loops.emplace_back();
        next = offset2_ex(next, -flow.scaled_width() / 2 - flow.scaled_spacing() / 4, flow.scaled_spacing() / 4, jtSquare);
        while (!next.empty()) {
            for (ExPolygon &expoly : next) {
                loops.back().emplace_back(expoly.contour);
                for (Polygon &poly : expoly.holes) {
                    poly.reverse();
                    loops.back().emplace_back(poly);
                    poly.reverse();
                }
            }
            next = ensure_valid(offset2_ex(next, - (flow.scaled_spacing() * 5) / 4, flow.scaled_spacing() / 4, jtSquare), scaled_resolution_brim);
        }

        // create extrusions
        extrude_brim_from_tree(print, loops, {Polygon(contour)}, flow, out, true);

        unbrimmable_areas.push_back(ExPolygon(contour));
        union_ex(unbrimmable_areas);
    }
}

void make_brim_interior(const Print& print, const Flow& flow, const PrintObjectPtrs& objects, ExPolygons& unbrimmable_areas, ExtrusionEntityCollection& out) {
    // Brim is only printed on first layer and uses perimeter extruder.

    const PrintObjectConfig& brim_config = objects.front()->config();
    coord_t brim_offset = scale_t(brim_config.brim_separation.value);
    ExPolygons    islands;
    coordf_t spacing;
    for (PrintObject* object : objects) {
        ExPolygons object_islands;
        for (const ExPolygon& expoly : object->layers().front()->lslices()){
            if (brim_offset == 0) {
                object_islands.push_back(expoly);
            } else {
                for (const ExPolygon& grown_expoly : offset_ex(ExPolygons{ expoly }, brim_offset)) {
                    object_islands.push_back(std::move(grown_expoly));
                }
            }
        }
        if (!object->support_layers().empty()) {
            spacing = scaled(object->config().support_material_interface_spacing.value) + support_material_flow(object, float(print.get_min_first_layer_height())).scaled_width() * 1.5;
            ExPolygons polys = closing_ex(
                union_ex(object->support_layers().front()->support_fills.polygons_covered_by_spacing(flow.spacing_ratio(), float(SCALED_EPSILON)))
                , spacing);
            for (ExPolygon& poly : polys) {
                if (brim_offset == 0) {
                    object_islands.push_back(std::move(poly));
                } else {
                    append(object_islands, offset_ex(ExPolygons{ poly }, brim_offset));
                }
            }
        }
        islands.reserve(islands.size() + object_islands.size() * object->instances().size());
        for (const PrintInstance& instance : object->instances())
            for (ExPolygon& poly : object_islands) {
                islands.push_back(poly);
                islands.back().translate(instance.shift.x(), instance.shift.y());
            }
    }

    islands = union_ex(islands);

    //to have the brimmable areas, get all holes, use them as contour , add smaller hole inside and make a diff with unbrimmable
    const size_t num_loops = size_t(floor((brim_config.brim_width_interior.value - brim_config.brim_separation.value) / flow.spacing()));
    ExPolygons brimmable_areas;
    Polygons islands_to_loops;
    for (const ExPolygon& expoly : islands) {
        for (const Polygon& hole : expoly.holes) {
            brimmable_areas.emplace_back();
            brimmable_areas.back().contour = hole;
            brimmable_areas.back().contour.make_counter_clockwise();
            for (Polygon poly : offset(brimmable_areas.back().contour, -flow.scaled_width() * static_cast<double>(num_loops), jtSquare)) {
                brimmable_areas.back().holes.push_back(poly);
                brimmable_areas.back().holes.back().make_clockwise();
            }
            islands_to_loops.insert(islands_to_loops.begin(), brimmable_areas.back().contour);
        }
    }

    brimmable_areas = diff_ex(brimmable_areas, islands, ApplySafetyOffset::Yes);
    brimmable_areas = diff_ex(brimmable_areas, unbrimmable_areas, ApplySafetyOffset::Yes);

    //now get all holes, use them to create loops
    //get brim resolution (low resolution if no arc fitting)
    coordf_t scaled_resolution_brim = (print.config().arc_fitting.value != ArcFittingType::Disabled) ? scale_d(print.config().resolution) : scale_d(print.config().resolution_internal) / 10;
    scaled_resolution_brim = std::max(scaled_resolution_brim, coordf_t(SCALED_EPSILON * 10));
    std::vector<std::vector<BrimLoop>> loops;
    for (size_t i = 0; i < num_loops; ++i) {
        print.throw_if_canceled();
        loops.emplace_back();
        Polygons islands_to_loops_offseted;
        for (Polygon& poly : islands_to_loops) {
            Polygons temp = offset(poly, double(-flow.scaled_spacing()), jtSquare);
            for (Polygon& poly : temp) {
                poly.points.push_back(poly.points.front());
                Points p = MultiPoint::douglas_peucker(poly.points, scaled_resolution_brim);
                p.pop_back();
                poly.points = std::move(p);
            }
            for (Polygon& poly : offset(temp, 0.5f * double(flow.scaled_spacing())))
                loops[i].emplace_back(poly);
            islands_to_loops_offseted.insert(islands_to_loops_offseted.end(), temp.begin(), temp.end());
        }
        islands_to_loops = islands_to_loops_offseted;
    }
    //loops = union_pt_chained_outside_in(loops, false);
    std::reverse(loops.begin(), loops.end());

    //intersection
    Polygons frontiers;
    for (ExPolygon& expoly : brimmable_areas) {
        for (Polygon& big_contour : offset(expoly.contour, 0.1f * flow.scaled_width())) {
            frontiers.push_back(big_contour);
            for (Polygon& hole : expoly.holes) {
                frontiers.push_back(hole);
                //don't reverse it! back! or it will be ignored by intersection_pl. 
                //frontiers.back().reverse();
            }
        }
    }

    extrude_brim_from_tree(print, loops, frontiers, flow, out, true);

    unbrimmable_areas.insert(unbrimmable_areas.end(), brimmable_areas.begin(), brimmable_areas.end());
}


} // namespace Slic3r
