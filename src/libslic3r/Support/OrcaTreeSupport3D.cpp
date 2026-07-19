// Tree supports by Thomas Rahm, losely based on Tree Supports by CuraEngine.
// Original source of Thomas Rahm's tree supports:
// https://github.com/ThomasRahm/CuraEngine
//
// Original CuraEngine copyright:
// Copyright (c) 2021 Ultimaker B.V.
// CuraEngine is released under the terms of the AGPLv3 or higher.

#include "OrcaTreeSupport3D.hpp"
#include "AABBTreeIndirect.hpp"
#include "AABBTreeLines.hpp"
#include "BuildVolume.hpp"
#include "ClipperUtils.hpp"
#include "EdgeGrid.hpp"
#include "Fill/FillBase.hpp"
#include "Layer.hpp"
#include "Print.hpp"
#include "MultiPoint.hpp"
#include "Polygon.hpp"
#include "Polyline.hpp"
#include "MutablePolygon.hpp"
#include "SupportCommon.hpp"
#include "TriangleMeshSlicer.hpp"
#include "OrcaTreeSupport.hpp"
#include "I18N.hpp"

#include <cassert>
#include <chrono>
#include <fstream>
#include <optional>
#include <limits>
#include <stdexcept>
#include <stdio.h>
#include <string>
#include <string_view>

#include <boost/log/trivial.hpp>

#include <tbb/parallel_for.h>
#include <tbb/parallel_for_each.h>
#include <tbb/spin_mutex.h>

#if defined(TREE_SUPPORT_SHOW_ERRORS) && defined(_WIN32)
    #define TREE_SUPPORT_SHOW_ERRORS_WIN32
#endif

// Define TREE_SUPPORT_ORGANIC_NUDGE_LEGACY to compile the slower OpenVDB implementation.
#ifdef TREE_SUPPORT_ORGANIC_NUDGE_LEGACY
// Old version using OpenVDB, works but it is extremely slow for complex meshes.
#include "../OpenVDBUtilsLegacy.hpp"
#include <openvdb/tools/VolumeToSpheres.h>
#endif // TREE_SUPPORT_ORGANIC_NUDGE_LEGACY

#ifndef _L
#define _L(s) Slic3r::I18N::translate(s)
#endif

 //#define TREESUPPORT_DEBUG_SVG

namespace Slic3r
{

// Named constants extracted for BP1002 (magic-number) cleanup.
constexpr double CLIPPER_MITER_LIMIT = 1.2;   // miter limit for Clipper offset() calls
constexpr double MICROSECONDS_TO_MS  = 0.001; // microseconds -> milliseconds for timing logs
constexpr double HALF                = 0.5;   // one-half factor (midpoints, half widths, PI*0.5)

namespace OrcaTreeSupport3D
{

using LineInformation = std::vector<std::pair<Point, LineStatus>>;
using LineInformations = std::vector<LineInformation>;
using namespace std::literals;

static size_t checked_layer_index(LayerIndex layer_idx)
{
    if (layer_idx < 0)
        throw std::out_of_range("Tree support layer index cannot be negative");
    return static_cast<size_t>(layer_idx);
}

static LayerIndex previous_layer_id(LayerIndex layer_idx)
{
    if (layer_idx <= 0)
        throw std::out_of_range("Tree support layer has no preceding layer");
    return layer_idx - 1;
}

static size_t previous_layer_index(LayerIndex layer_idx)
{
    return static_cast<size_t>(previous_layer_id(layer_idx));
}

static size_t next_layer_index(LayerIndex layer_idx)
{
    if (layer_idx < 0 || layer_idx == std::numeric_limits<LayerIndex>::max())
        throw std::out_of_range("Tree support layer has no following layer");
    return static_cast<size_t>(layer_idx) + 1;
}

static size_t layer_index_distance(LayerIndex upper_layer_idx, LayerIndex lower_layer_idx)
{
    const int64_t distance = static_cast<int64_t>(upper_layer_idx) - static_cast<int64_t>(lower_layer_idx);
    if (distance < 0)
        throw std::out_of_range("Tree support layer range is reversed");
    return static_cast<size_t>(distance);
}

static coord_t saturating_add_coord(coord_t lhs, coord_t rhs)
{
    constexpr coord_t min_coord = std::numeric_limits<coord_t>::min();
    constexpr coord_t max_coord = std::numeric_limits<coord_t>::max();
    if (rhs > 0 && lhs > max_coord - rhs)
        return max_coord;
    if (rhs < 0 && lhs < min_coord - rhs)
        return min_coord;
    return lhs + rhs;
}

static coord_t saturating_add_coord(coord_t first, coord_t second, coord_t third)
{
    return saturating_add_coord(saturating_add_coord(first, second), third);
}

static coord_t saturating_subtract_coord(coord_t lhs, coord_t rhs)
{
    constexpr coord_t min_coord = std::numeric_limits<coord_t>::min();
    constexpr coord_t max_coord = std::numeric_limits<coord_t>::max();
    if (rhs > 0 && lhs < min_coord + rhs)
        return min_coord;
    if (rhs < 0 && lhs > max_coord + rhs)
        return max_coord;
    return lhs - rhs;
}

static size_t checked_triangle_index_reserve_size(
    size_t current_size, size_t max_size, size_t first_face_count, size_t second_face_count = 0)
{
    if (first_face_count > max_size || second_face_count > max_size - first_face_count)
        throw std::length_error("Tree support triangle count exceeds container capacity");

    const size_t face_count = first_face_count + second_face_count;
    if (current_size > max_size || face_count > (max_size - current_size) / 3) // Each triangle contributes three vertex indices.
        throw std::length_error("Tree support index count exceeds container capacity");

    return current_size + face_count * 3; // Each triangle contributes three vertex indices.
}

static inline void validate_range(const Point &pt)
{
    static constexpr const int32_t hi = 65536 * 16384;
    if (pt.x() > hi || pt.y() > hi || -pt.x() > hi || -pt.y() > hi)
      throw ClipperLib::clipperException("Coordinate outside allowed range");
}

static inline void validate_range(const Points &points)
{
    for (const Point &p : points)
        validate_range(p);
}

static inline void validate_range(const MultiPoint &mp)
{
    validate_range(mp.points);
}

static inline void validate_range(const Polygons &polygons)
{
    for (const Polygon &p : polygons)
        validate_range(p);
}

static inline void validate_range(const Polylines &polylines)
{
    for (const Polyline &p : polylines)
        validate_range(p);
}

static inline void validate_range(const LineInformation &lines)
{
    for (const auto& p : lines)
        validate_range(p.first);
}

static inline void validate_range(const LineInformations &lines)
{
    for (const LineInformation &l : lines)
        validate_range(l);
}

static inline void check_self_intersections(const Polygons &polygons, const std::string_view message)
{
#ifdef TREE_SUPPORT_SHOW_ERRORS_WIN32
    if (!intersecting_edges(polygons).empty())
        ::MessageBoxA(nullptr, (std::string("OrcaTreeSupport infill self intersections: ") + std::string(message)).c_str(), "Bug detected!", MB_OK | MB_SYSTEMMODAL | MB_SETFOREGROUND | MB_ICONWARNING);
#endif // TREE_SUPPORT_SHOW_ERRORS_WIN32
}
static inline void check_self_intersections(const ExPolygon &expoly, const std::string_view message)
{
#ifdef TREE_SUPPORT_SHOW_ERRORS_WIN32
    check_self_intersections(to_polygons(expoly), message);
#endif // TREE_SUPPORT_SHOW_ERRORS_WIN32
}

static std::vector<std::pair<OrcaTreeSupportSettings, std::vector<size_t>>> group_meshes(const Print &print, const std::vector<size_t> &print_object_ids)
{
    std::vector<std::pair<OrcaTreeSupportSettings, std::vector<size_t>>> grouped_meshes;

    // Possible refactor: this validation/filtering does not belong here.
    for (size_t object_id : print_object_ids) {
        const PrintObject       &print_object  = *print.get_object(object_id);
        const PrintObjectConfig &object_config = print_object.config();
        if (object_config.support_material_contact_distance_type.value == zdNone)
            // || min_feature_size < scaled<coord_t>(0.1) that is the minimum line width
            OrcaTreeSupportSettings::soluble = true;
    }

    size_t largest_printed_mesh_idx = 0;

    // Group all meshes that can be processed together. NOTE this is different from mesh-groups! Only one setting object is needed per group,
    // as different settings in the same group may only occur in the tip, which uses the original settings objects from the meshes.
    for (size_t object_id : print_object_ids) {
        const PrintObject       &print_object  = *print.get_object(object_id);

        bool found_existing_group = false;
        OrcaTreeSupportSettings next_settings{ OrcaTreeSupportMeshGroupSettings{ print_object }, print_object.slicing_parameters() };
        // Known limitation: only a single object per group is enabled for now (the grouping code below is disabled).
#if 0
        for (size_t idx = 0; idx < grouped_meshes.size(); ++ idx)
            if (next_settings == grouped_meshes[idx].first) {
                found_existing_group = true;
                grouped_meshes[idx].second.emplace_back(object_id);
                // handle some settings that are only used for performance reasons. This ensures that a horrible set setting intended to improve performance can not reduce it drastically.
                grouped_meshes[idx].first.performance_interface_skip_layers = std::min(grouped_meshes[idx].first.performance_interface_skip_layers, next_settings.performance_interface_skip_layers);
            }
#endif
        if (! found_existing_group)
            grouped_meshes.emplace_back(next_settings, std::vector<size_t>{ object_id });

        // no need to do this per mesh group as adaptive layers and raft setting are not setable per mesh.
        if (print.get_object(largest_printed_mesh_idx)->layers().back()->print_z < print_object.layers().back()->print_z)
            largest_printed_mesh_idx = object_id;
    }

#if 0
    {
        std::vector<coord_t> known_z(storage.meshes[largest_printed_mesh_idx].layers.size());
        for (size_t z = 0; z < storage.meshes[largest_printed_mesh_idx].layers.size(); z++)
            known_z[z] = storage.meshes[largest_printed_mesh_idx].layers[z].printZ;
        for (size_t idx = 0; idx < grouped_meshes.size(); ++ idx)
            grouped_meshes[idx].first.setActualZ(known_z);
    }
#endif

    return grouped_meshes;
}

static float overhang_lower_layer_offset(const Layer &lower_layer, bool enforced_layer, bool support_threshold_auto, double tan_threshold)
{
    if (enforced_layer)
        return 0.f;
    if (! support_threshold_auto)
        return scaled<float>(lower_layer.height / tan_threshold);

    float external_perimeter_width = 0.f;
    for (const LayerRegion *layer_region : lower_layer.regions())
        external_perimeter_width += layer_region->flow(frExternalPerimeter).scaled_width();
    external_perimeter_width /= lower_layer.region_count();
    return float(HALF * external_perimeter_width);
}

struct OverhangGenerationContext
{
    const PrintConfig                    &print_config;
    const PrintObjectConfig              &config;
    bool                                  support_auto;
    int                                   support_enforce_layers;
    bool                                  support_threshold_auto;
    double                                tan_threshold;
    double                                enforcer_overhang_offset;
    std::vector<ExPolygons>              &enforcers_layers;
    std::vector<ExPolygons>              &blockers_layers;
    const std::vector<Polygons>          &enforcers_custom_facets;
    const std::vector<Polygons>          &blockers_custom_facets;
};

static Polygons generate_layer_overhangs(
    const OverhangGenerationContext &context,
    const Layer                     &current_layer,
    const Layer                     &lower_layer,
    size_t                           layer_id)
{
    ExPolygons raw_overhangs;
    bool       raw_overhangs_calculated = false;
    ExPolygons overhangs;

    const bool enforced_layer = layer_id < context.support_enforce_layers;
    if (context.support_auto || enforced_layer) {
        const float lower_layer_offset = overhang_lower_layer_offset(
            lower_layer, enforced_layer, context.support_threshold_auto, context.tan_threshold);

        overhangs = lower_layer_offset == 0 ?
            diff_ex(current_layer.lslices(), lower_layer.lslices()) :
            diff_ex(current_layer.lslices(), offset_ex(lower_layer.lslices(), lower_layer_offset));
        if (lower_layer_offset == 0) {
            raw_overhangs = overhangs;
            raw_overhangs_calculated = true;
        }

        if (! enforced_layer) {
            if (! context.blockers_custom_facets.empty() && ! context.blockers_custom_facets[layer_id].empty())
                append(context.blockers_layers[layer_id], union_ex(context.blockers_custom_facets[layer_id]));
            if (! context.blockers_layers[layer_id].empty())
                overhangs = diff_ex(overhangs, context.blockers_layers[layer_id]);
        }
        if (context.config.dont_support_bridges) {
            for (const LayerRegion *layer_region : current_layer.regions())
                FFFSupport::remove_bridges_from_contacts(
                    context.print_config, lower_layer, *layer_region,
                    float(layer_region->flow(frExternalPerimeter).scaled_width()), overhangs);
        }
    }

    if (! context.enforcers_custom_facets.empty() && ! context.enforcers_custom_facets[layer_id].empty())
        append(context.enforcers_layers[layer_id], union_ex(context.enforcers_custom_facets[layer_id]));
    if (! context.enforcers_layers[layer_id].empty()) {
        ExPolygons enforced_overhangs = intersection_ex(
            raw_overhangs_calculated ? raw_overhangs : diff_ex(current_layer.lslices(), lower_layer.lslices()),
            context.enforcers_layers[layer_id]);
        if (! enforced_overhangs.empty()) {
            ExPolygons to_union_enforced_overhangs = enforced_overhangs;
            for (const ExPolygon &enforced_overhang : enforced_overhangs) {
                ExPolygons grown_enforced_overhangs = diff_ex(
                    offset_ex(enforced_overhang, context.enforcer_overhang_offset), lower_layer.lslices());
                append(to_union_enforced_overhangs,
                    offset2_ex(grown_enforced_overhangs,
                        -context.enforcer_overhang_offset / 2, context.enforcer_overhang_offset / 2));  // half the enforcer overhang offset (both sides)
            }
            enforced_overhangs = union_ex(to_union_enforced_overhangs);
            overhangs = overhangs.empty() ? std::move(enforced_overhangs) : union_ex(overhangs, enforced_overhangs);
        }
    }

    return to_polygons(overhangs);
}

[[nodiscard]] static const std::vector<Polygons> generate_overhangs(const OrcaTreeSupportSettings &settings, const PrintObject &print_object, std::function<void()> throw_on_cancel)
{
    const size_t num_raft_layers   = settings.raft_layers.size();
    const size_t num_object_layers = print_object.layer_count();
    const size_t num_layers        = num_object_layers + num_raft_layers;
    std::vector<Polygons> out(num_layers, Polygons{});

    const PrintConfig       &print_config           = print_object.print()->config();
    const PrintObjectConfig &config                 = print_object.config();
    const bool               support_auto           = config.support_material.value && config.support_material_auto.value;
    const int                support_enforce_layers = config.support_material_enforce_layers.value;
    std::vector<ExPolygons>  enforcers_layers{ print_object.slice_support_enforcers() };
    std::vector<ExPolygons>  blockers_layers{ print_object.slice_support_blockers() };
    const std::vector<Polygons> enforcers_custom_facets = print_object.project_and_append_custom_facets(false, EnforcerBlockerType::ENFORCER);
    const std::vector<Polygons> blockers_custom_facets  = print_object.project_and_append_custom_facets(false, EnforcerBlockerType::BLOCKER);
    const int                support_threshold      = config.support_material_threshold.value;
    const bool               support_threshold_auto = support_threshold == 0;
    const double             tan_threshold          = support_threshold_auto ? 0. : tan(M_PI * double(support_threshold + 1) / 180.);
    const double             enforcer_overhang_offset = scaled<double>(config.support_tree_tip_diameter.value);
    const size_t             num_overhang_layers = support_auto ?
        num_object_layers :
        std::min(num_object_layers,
                 std::max(size_t(support_enforce_layers),
                          std::max(enforcers_layers.size(), enforcers_custom_facets.size())));

    if (enforcers_layers.size() < num_overhang_layers)
        enforcers_layers.resize(num_overhang_layers);
    if (blockers_layers.size() < num_overhang_layers)
        blockers_layers.resize(num_overhang_layers);

    const OverhangGenerationContext context{
        print_config,
        config,
        support_auto,
        support_enforce_layers,
        support_threshold_auto,
        tan_threshold,
        enforcer_overhang_offset,
        enforcers_layers,
        blockers_layers,
        enforcers_custom_facets,
        blockers_custom_facets
    };

    tbb::parallel_for(tbb::blocked_range<size_t>(1, num_overhang_layers),
        [&](const tbb::blocked_range<size_t> &range) {
            for (size_t layer_id = range.begin(); layer_id < range.end(); ++layer_id) {
                const Layer &current_layer = *print_object.get_layer(layer_id);
                const Layer &lower_layer   = *print_object.get_layer(layer_id - 1);
                out[layer_id + num_raft_layers] = generate_layer_overhangs(
                    context, current_layer, lower_layer, layer_id);
                throw_on_cancel();
            }
        });

    return out;
}

/*!
 * \brief Precalculates all avoidances, that could be required.
 *
 * \param storage[in] Background storage to access meshes.
 * \param currently_processing_meshes[in] Indexes of all meshes that are processed in this iteration
 */
[[nodiscard]] static LayerIndex precalculate(const Print &print, const std::vector<Polygons> &overhangs, const OrcaTreeSupportSettings &config, const std::vector<size_t> &object_ids,
    OrcaTreeModelVolumes &volumes, std::function<void()> throw_on_cancel)
{
    // calculate top most layer that is relevant for support
    LayerIndex max_layer = 0;
    for (size_t object_id : object_ids) {
        const PrintObject &print_object      = *print.get_object(object_id);
        const int       num_raft_layers      = int(config.raft_layers.size());
        const int       num_layers           = int(print_object.layer_count()) + num_raft_layers;
        int             max_support_layer_id = 0;
        for (int layer_id = std::max<int>(num_raft_layers, 1); layer_id < num_layers; ++ layer_id)
            if (! overhangs[layer_id].empty())
                max_support_layer_id = layer_id;
        max_layer = std::max(max_support_layer_id - int(config.z_distance_top_layers), 0);
    }
    if (max_layer > 0)
        // The actual precalculation happens in OrcaTreeModelVolumes.
        volumes.precalculate(*print.get_object(object_ids.front()), max_layer, throw_on_cancel);
    return max_layer;
}

// picked from convert_lines_to_internal()
[[nodiscard]] static LineStatus get_avoidance_status(const Point& p, coord_t radius, LayerIndex layer_idx,
    const OrcaTreeModelVolumes& volumes, const OrcaTreeSupportSettings& config)
{
    const bool min_xy_dist = config.xy_distance > config.xy_min_distance;

    LineStatus type = LineStatus::INVALID;

    if (!contains(volumes.getAvoidance(radius, layer_idx, OrcaTreeModelVolumes::AvoidanceType::FastSafe, false, min_xy_dist), p))
        type = LineStatus::TO_BP_SAFE;
    else if (!contains(volumes.getAvoidance(radius, layer_idx, OrcaTreeModelVolumes::AvoidanceType::Fast, false, min_xy_dist), p))
        type = LineStatus::TO_BP;
    else if (config.support_rests_on_model && !contains(volumes.getAvoidance(radius, layer_idx, OrcaTreeModelVolumes::AvoidanceType::FastSafe, true, min_xy_dist), p))
        type = LineStatus::TO_MODEL_GRACIOUS_SAFE;
    else if (config.support_rests_on_model && !contains(volumes.getAvoidance(radius, layer_idx, OrcaTreeModelVolumes::AvoidanceType::Fast, true, min_xy_dist), p))
        type = LineStatus::TO_MODEL_GRACIOUS;
    else if (config.support_rests_on_model && !contains(volumes.getCollision(radius, layer_idx, min_xy_dist), p))
        type = LineStatus::TO_MODEL;

    return type;
}

/*!
 * \brief Converts a Polygons object representing a line into the internal format.
 *
 * \param polylines[in] The Polyline that will be converted.
 * \param layer_idx[in] The current layer.
 * \return All lines of the \p polylines object, with information for each point regarding in which avoidance it is currently valid in.
 */
// Called by generate_initial_areas()
[[nodiscard]] static LineInformations convert_lines_to_internal(
    const OrcaTreeModelVolumes &volumes, const OrcaTreeSupportSettings &config,
    const Polylines &polylines, LayerIndex layer_idx)
{
    const bool min_xy_dist = config.xy_distance > config.xy_min_distance;

    LineInformations result;
    // Also checks if the position is valid, if it is NOT, it deletes that point
    for (const Polyline &line : polylines) {
        LineInformation res_line;
        for (Point p : line) {
            if (! contains(volumes.getAvoidance(config.getRadius(0), layer_idx, OrcaTreeModelVolumes::AvoidanceType::FastSafe, false, min_xy_dist), p))
                res_line.emplace_back(p, LineStatus::TO_BP_SAFE);
            else if (! contains(volumes.getAvoidance(config.getRadius(0), layer_idx, OrcaTreeModelVolumes::AvoidanceType::Fast, false, min_xy_dist), p))
                res_line.emplace_back(p, LineStatus::TO_BP);
            else if (config.support_rests_on_model && ! contains(volumes.getAvoidance(config.getRadius(0), layer_idx, OrcaTreeModelVolumes::AvoidanceType::FastSafe, true, min_xy_dist), p))
                res_line.emplace_back(p, LineStatus::TO_MODEL_GRACIOUS_SAFE);
            else if (config.support_rests_on_model && ! contains(volumes.getAvoidance(config.getRadius(0), layer_idx, OrcaTreeModelVolumes::AvoidanceType::Fast, true, min_xy_dist), p))
                res_line.emplace_back(p, LineStatus::TO_MODEL_GRACIOUS);
            else if (config.support_rests_on_model && ! contains(volumes.getCollision(config.getRadius(0), layer_idx, min_xy_dist), p))
                res_line.emplace_back(p, LineStatus::TO_MODEL);
            else if (!res_line.empty()) {
                result.emplace_back(res_line);
                res_line.clear();
            }
        }
        if (!res_line.empty()) {
            result.emplace_back(res_line);
            res_line.clear();
        }
    }

    validate_range(result);
    return result;
}

#if 0
/*!
 * \brief Converts lines in internal format into a Polygons object representing these lines.
 *
 * \param lines[in] The lines that will be converted.
 * \return All lines of the \p lines object as a Polygons object.
 */
[[nodiscard]] static Polylines convert_internal_to_lines(LineInformations lines)
{
    Polylines result;
    for (LineInformation line : lines) {
        Polyline path;
        for (auto point_data : line)
            path.points.emplace_back(point_data.first);
        result.emplace_back(std::move(path));
    }
    validate_range(result);
    return result;
}
#endif

/*!
 * \brief Evaluates if a point has to be added now. Required for a split_lines call in generate_initial_areas().
 *
 * \param current_layer[in] The layer on which the point lies, point and its status.
 * \return whether the point is valid.
 */
[[nodiscard]] static bool evaluate_point_for_next_layer_function(
    const OrcaTreeModelVolumes &volumes, const OrcaTreeSupportSettings &config,
    size_t current_layer, const std::pair<Point, LineStatus> &p)
{
    using AvoidanceType = OrcaTreeModelVolumes::AvoidanceType;
    const bool min_xy_dist = config.xy_distance > config.xy_min_distance;
    if (! contains(volumes.getAvoidance(config.getRadius(0), current_layer - 1, p.second == LineStatus::TO_BP_SAFE ? AvoidanceType::FastSafe : AvoidanceType::Fast, false, min_xy_dist), p.first))
        return true;
    if (config.support_rests_on_model && (p.second != LineStatus::TO_BP && p.second != LineStatus::TO_BP_SAFE))
        return ! contains(
            p.second == LineStatus::TO_MODEL_GRACIOUS || p.second == LineStatus::TO_MODEL_GRACIOUS_SAFE ?
                volumes.getAvoidance(config.getRadius(0), current_layer - 1, p.second == LineStatus::TO_MODEL_GRACIOUS_SAFE ? AvoidanceType::FastSafe : AvoidanceType::Fast, true, min_xy_dist) :
                volumes.getCollision(config.getRadius(0), current_layer - 1, min_xy_dist),
            p.first);
    return false;
}

/*!
 * \brief Evaluates which points of some lines are not valid one layer below and which are. Assumes all points are valid on the current layer. Validity is evaluated using supplied lambda.
 *
 * \param lines[in] The lines that have to be evaluated.
 * \param evaluatePoint[in] The function used to evaluate the points.
 * \return A pair with which points are still valid in the first slot and which are not in the second slot.
 */
template<typename EvaluatePointFn>
[[nodiscard]] static std::pair<LineInformations, LineInformations> split_lines(const LineInformations &lines, EvaluatePointFn evaluatePoint)
{
    // assumes all Points on the current line are valid

    LineInformations keep;
    LineInformations set_free;
    for (const std::vector<std::pair<Point, LineStatus>> &line : lines) {
        bool            current_keep = true;
        LineInformation resulting_line;
        for (const std::pair<Point, LineStatus> &me : line) {
            if (evaluatePoint(me) != current_keep) {
                if (! resulting_line.empty())
                    (current_keep ? &keep : &set_free)->emplace_back(std::move(resulting_line));
                current_keep = !current_keep;
            }
            resulting_line.emplace_back(me);
        }
        if (! resulting_line.empty())
            (current_keep ? &keep : &set_free)->emplace_back(std::move(resulting_line));
    }
    validate_range(keep);
    validate_range(set_free);
    return std::pair<std::vector<std::vector<std::pair<Point, LineStatus>>>, std::vector<std::vector<std::pair<Point, LineStatus>>>>(keep, set_free);
}

// Ported from CURA's PolygonUtils::getNextPointWithDistance()
// Sample a next point at distance "dist" from start_pt on polyline segment (start_idx, start_idx + 1).
// Returns sample point and start index of its segment on polyline if such sample exists.
static std::optional<std::pair<Point, size_t>> polyline_sample_next_point_at_distance(const Points &polyline, const Point &start_pt, size_t start_idx, double dist)
{
    const double                dist2  = sqr(dist);
    const auto                  dist2i = int64_t(dist2);
    const auto eps    = scaled<double>(0.01);

    for (size_t i = start_idx + 1; i < polyline.size(); ++ i) {
        const Point p1 = polyline[i];
        if ((p1 - start_pt).cast<int64_t>().squaredNorm() < dist2i)
            continue;

        // The end point is outside the circle with center "start_pt" and radius "dist".
        const Point p0  = polyline[i - 1];
        Vec2d       v   = (p1 - p0).cast<double>();
        double      l2v = v.squaredNorm();
        if (l2v < sqr(eps)) {
            // Very short segment.
            Point c = (p0 + p1) / 2;  // segment midpoint = (p0 + p1) / 2
            if (std::abs((start_pt - c).cast<double>().norm() - dist) < eps)
                return std::pair<Point, size_t>{ c, i - 1 };
            continue;
        }

        Vec2d p0f = (start_pt - p0).cast<double>();
        // Foot point of start_pt into v.
        Vec2d foot_pt = v * (p0f.dot(v) / l2v);
        // Vector from foot point of "start_pt" to "start_pt".
        Vec2d xf = p0f - foot_pt;
        // Squared distance of "start_pt" from the ray (p0, p1).
        double l2_from_line = xf.squaredNorm();
        // Squared distance of an intersection point of a circle with center at the foot point.
        double l2_intersection = dist2 - l2_from_line;
        if (l2_intersection <= - SCALED_EPSILON)
            continue;

        // The ray (p0, p1) touches or intersects a circle centered at "start_pt" with radius "dist".
        // Distance of the circle intersection point from the foot point.
        l2_intersection = std::max(l2_intersection, 0.);
        if (! ((v - foot_pt).cast<double>().squaredNorm() >= l2_intersection))
            continue;

        // Intersection of the circle with the segment (p0, p1) is on the right side (close to p1) from the foot point.
        Point p = p0 + (foot_pt + v * sqrt(l2_intersection / l2v)).cast<coord_t>();
        validate_range(p);
        return std::pair<Point, size_t>{ p, i - 1 };
    }
    return {};
}

static std::pair<size_t, size_t> farthest_polyline_vertices(const Polyline &closed_polyline)
{
    const size_t vertex_count = closed_polyline.size() - 1; // exclude the repeated closing vertex
    size_t optimal_start_index = 0;
    size_t optimal_end_index   = 0;
    double max_squared_distance = 0.;

    for (size_t idx = 0; idx < vertex_count; ++ idx) {
        for (size_t inner_idx = 0; inner_idx < vertex_count; ++ inner_idx) {
            const double squared_distance = (closed_polyline[idx] - closed_polyline[inner_idx]).cast<double>().squaredNorm();
            if (squared_distance <= max_squared_distance)
                continue;
            optimal_start_index  = idx;
            optimal_end_index    = inner_idx;
            max_squared_distance = squared_distance;
        }
    }
    return { optimal_start_index, optimal_end_index };
}

static Polyline sample_polyline_points(const Polyline &part, size_t optimal_end_index, double current_distance, size_t min_points)
{
    Polyline line;
    while (line.size() < min_points && current_distance >= scaled<double>(0.1)) {
        line.clear();
        Point current_point = part.front();
        line.points.emplace_back(current_point);
        if (min_points > 1 || (current_point - part[optimal_end_index]).cast<double>().norm() > current_distance)
            line.points.emplace_back(part[optimal_end_index]);

        size_t current_index = 0;
        std::optional<std::pair<Point, size_t>> next_point;
        double next_distance = current_distance;
        while ((next_point = polyline_sample_next_point_at_distance(part.points, current_point, current_index, next_distance))) {
            double min_distance_to_existing_point = std::numeric_limits<double>::max();
            for (const Point &point : line)
                min_distance_to_existing_point = std::min(min_distance_to_existing_point, (point - next_point->first).cast<double>().norm());

            if (min_distance_to_existing_point >= current_distance) {
                line.points.emplace_back(next_point->first);
                current_point = next_point->first;
                current_index = next_point->second;
                next_distance = current_distance;
                continue;
            }
            if (current_point == next_point->first) {
                BOOST_LOG_TRIVIAL(warning) << "Tree Support: Encountered a fixpoint in polyline_sample_next_point_at_distance. This is expected to happen if the distance (currently "
                    << next_distance << ") is smaller than 100";
                tree_supports_show_error("Encountered issue while placing tips. Some tips may be missing."sv, true);
                if (next_distance > 2 * current_distance) // more than double the current spacing
                    break;
                next_distance += current_distance;
                continue;
            }

            next_distance = std::max(current_distance - min_distance_to_existing_point, scaled<double>(0.1));
            current_point = next_point->first;
            current_index = next_point->second;
        }
        current_distance *= 0.9;
    }
    return line;
}

/*!
 * \brief Ensures that every line segment is about distance in length. The resulting lines may differ from the original but all points are on the original
 *
 * \param input[in] The lines on which evenly spaced points should be placed.
 * \param distance[in] The distance the points should be from each other.
 * \param min_points[in] The amount of points that have to be placed. If not enough can be placed the distance will be reduced to place this many points.
 * \return A Polygons object containing the evenly spaced points. Does not represent an area, more a collection of points on lines.
 */
[[nodiscard]] static Polylines ensure_maximum_distance_polyline(const Polylines &input, double distance, size_t min_points)
{
    Polylines result;
    for (Polyline part : input) {
        if (part.empty())
            continue;

        const double len = length(part.points);
        const double initial_distance = std::max(distance, scaled<double>(0.1));
        if (len < 2 * distance && min_points <= 1) { // shorter than twice the spacing
            Polyline opposite_point(part);
            opposite_point.clip_end(len / 2); // clip to half the length
            result.emplace_back(Polyline{ opposite_point.points.back() });
            continue;
        }

        size_t optimal_end_index = part.size() - 1;
        if (part.size() > 1 && part.front() == part.back()) {
            auto [optimal_start_index, farthest_end_index] = farthest_polyline_vertices(part);
            std::rotate(part.begin(), part.begin() + optimal_start_index, part.end() - 1);
            part[part.size() - 1] = part.front(); // restore the repeated closing vertex
            optimal_end_index = (part.size() + farthest_end_index - optimal_start_index - 1) % (part.size() - 1);
        }

        result.emplace_back(sample_polyline_points(part, optimal_end_index, initial_distance, min_points));
    }
    validate_range(result);
    return result;
}

/*!
 * \brief Returns Polylines representing the (infill) lines that will result in slicing the given area
 *
 * \param area[in] The area that has to be filled with infill.
 * \param roof[in] Whether the roofing or regular support settings should be used.
 * \param layer_idx[in] The current layer index.
 * \param support_infill_distance[in] The distance that should be between the infill lines.
 *
 * \return A Polygons object that represents the resulting infill lines.
 */
[[nodiscard]] static Polylines generate_support_infill_lines(
    // Polygon to fill in with a zig-zag pattern supporting an overhang.
    const Polygons          &polygon,
    const SupportParameters &support_params,
    bool roof, LayerIndex layer_idx, coord_t support_infill_distance)
{
#if 0
    Polygons gaps;
    // as we effectivly use lines to place our supportPoints we may use the Infill class for it, while not made for it it works perfect

    const EFillMethod pattern = roof ? config.roof_pattern : config.support_pattern;

//    const bool zig_zaggify_infill = roof ? pattern == EFillMethod::ZIG_ZAG : config.zig_zaggify_support;
    const bool connect_polygons = false;
    constexpr coord_t support_roof_overlap = 0;
    constexpr size_t infill_multiplier = 1;
    constexpr coord_t outline_offset = 0;
    const int support_shift = roof ? 0 : support_infill_distance / 2;  // half the infill spacing
    const size_t wall_line_count = include_walls && !roof ? config.support_wall_count : 0;
    const Point infill_origin;
    constexpr Polygons* perimeter_gaps = nullptr;
    constexpr bool use_endpieces = true;
    const bool connected_zigzags = roof ? false : config.connect_zigzags;
    const size_t zag_skip_count = roof ? 0 : config.zag_skip_count;
    constexpr coord_t pocket_size = 0;
    std::vector<AngleRadians> angles = roof ? config.support_roof_angles : config.support_infill_angles;
    std::vector<VariableWidthLines> toolpaths;

    const coord_t z = config.getActualZ(layer_idx);
    int divisor = static_cast<int>(angles.size());
    int index = ((layer_idx % divisor) + divisor) % divisor;
    const AngleRadians fill_angle = angles[index];
    Infill roof_computation(pattern, true /* zig_zaggify_infill */, connect_polygons, polygon,
        roof ? config.support_roof_line_width : config.support_line_width, support_infill_distance, support_roof_overlap, infill_multiplier,
        fill_angle, z, support_shift, config.resolution, wall_line_count, infill_origin,
        perimeter_gaps, connected_zigzags, use_endpieces, false /* skip_some_zags */, zag_skip_count, pocket_size);
    Polygons polygons;
    Polygons lines;
    roof_computation.generate(toolpaths, polygons, lines, config.settings);
    append(lines, to_polylines(polygons));
    return lines;
#else
//    const bool connected_zigzags = roof ? false : config.connect_zigzags;
//    const int support_shift = roof ? 0 : support_infill_distance / 2;

    const Flow            &flow   = roof ? support_params.support_material_interface_flow : support_params.support_material_flow;
    std::unique_ptr<Fill>  filler = std::unique_ptr<Fill>(Fill::new_from_type(roof ? support_params.interface_fill_pattern : support_params.base_fill_pattern));
    FillParams             fill_params;

    filler->layer_id = layer_idx;
    filler->angle = roof ?
        // Note (possible refactor): could use an interface-relative index instead of the absolute layer_idx here (layer_idx isn't guaranteed contiguous across interface layers).
        (support_params.interface_angle + ((layer_idx & 1) ? float(- M_PI / 4.) : float(+ M_PI / 4.))) :
        support_params.base_angle;

    fill_params.density     = float(roof ? support_params.interface_density : scaled<float>(flow.spacing()) / (scaled<float>(flow.spacing()) + float(support_infill_distance)));
    fill_params.dont_adjust = true;
    fill_params.config = &support_params.default_region_config;

    filler->init_spacing(flow.spacing(), fill_params);

    Polylines out;
    for (ExPolygon &expoly : ensure_valid(union_ex(polygon))) {
        // The surface type does not matter.
        assert(area(expoly) > 0.);
#ifdef TREE_SUPPORT_SHOW_ERRORS_WIN32
        if (area(expoly) <= 0.)
            ::MessageBoxA(nullptr, "OrcaTreeSupport infill negative area", "Bug detected!", MB_OK | MB_SYSTEMMODAL | MB_SETFOREGROUND | MB_ICONWARNING);
#endif // TREE_SUPPORT_SHOW_ERRORS_WIN32
        assert(intersecting_edges(to_polygons(expoly)).empty());
        check_self_intersections(expoly, "generate_support_infill_lines");
        Surface surface(stPosInternal | stDensSparse, std::move(expoly));
        try {
            Polylines pl = filler->fill_surface(&surface, fill_params);
            assert(pl.empty() || get_extents(surface.expolygon).inflated(SCALED_EPSILON).contains(get_extents(pl)));
#ifdef TREE_SUPPORT_SHOW_ERRORS_WIN32
            if (! pl.empty() && ! get_extents(surface.expolygon).inflated(SCALED_EPSILON).contains(get_extents(pl)))
                ::MessageBoxA(nullptr, "OrcaTreeSupport infill failure", "Bug detected!", MB_OK | MB_SYSTEMMODAL | MB_SETFOREGROUND | MB_ICONWARNING);
#endif // TREE_SUPPORT_SHOW_ERRORS_WIN32
            append(out, std::move(pl));
        } catch (InfillFailedException &) {
        }
    }
    validate_range(out);
    return out;
#endif
}

/*!
 * \brief Unions two Polygons. Ensures that if the input is non empty that the output also will be non empty.
 * \param first[in] The first Polygon.
 * \param second[in] The second Polygon.
 * \return The union of both Polygons
 */
[[nodiscard]] static Polygons safe_union(const Polygons first, const Polygons second = {})
{
    // unionPolygons can slowly remove Polygons under certain circumstances, because of rounding issues (Polygons that have a thin area).
    // This does not cause a problem when actually using it on large areas, but as influence areas (representing centerpoints) can be very thin, this does occur so this ugly workaround is needed
    // Here is an example of a Polygons object that will loose vertices when unioning, and will be gone after a few times unionPolygons was called:
    /*
    Polygons example;
    Polygon exampleInner;
    exampleInner.add(Point(120410,83599));//A
    exampleInner.add(Point(120384,83643));//B
    exampleInner.add(Point(120399,83618));//C
    exampleInner.add(Point(120414,83591));//D
    exampleInner.add(Point(120423,83570));//E
    exampleInner.add(Point(120419,83580));//F
    example.add(exampleInner);
    for(int i=0;i<10;i++){
         log("Iteration %d Example area: %f\n",i,area(example));
         example=example.unionPolygons();
    }
*/

    Polygons result;
    if (! first.empty() || ! second.empty()) {
        result = union_(first, second);
        if (result.empty()) {
            BOOST_LOG_TRIVIAL(debug) << "Caught an area destroying union, enlarging areas a bit.";
            // just take the few lines we have, and offset them a tiny bit. Needs to be offsetPolylines, as offset may aleady have problems with the area.
            result = union_(offset(to_polylines(first), scaled<float>(0.002), jtMiter, CLIPPER_MITER_LIMIT), offset(to_polylines(second), scaled<float>(0.002), jtMiter, CLIPPER_MITER_LIMIT));
        }
    }

    return result;
}

/*!
 * \brief Offsets (increases the area of) a polygons object in multiple steps to ensure that it does not lag through over a given obstacle.
 * \param me[in] Polygons object that has to be offset.
 * \param distance[in] The distance by which me should be offset. Expects values >=0.
 * \param collision[in] The area representing obstacles.
 * \param last_step_offset_without_check[in] The most it is allowed to offset in one step.
 * \param min_amount_offset[in] How many steps have to be done at least. As this uses round offset this increases the amount of vertices, which may be required if Polygons get very small. Required as
 * arcTolerance is not exposed in offset, which should result with a similar result.
 * \return The resulting Polygons object.
 */
[[nodiscard]] static Polygons safe_offset_inc(const Polygons& me, coord_t distance, const Polygons& collision, coord_t safe_step_size, coord_t last_step_offset_without_check, size_t min_amount_offset)
{
    bool do_final_difference = last_step_offset_without_check == 0;
    Polygons ret = safe_union(me); // ensure sane input

    // Trim the collision polygons with the region of interest for diff() efficiency.
    Polygons collision_trimmed_buffer;
    auto collision_trimmed = [&collision_trimmed_buffer, &collision, &ret, distance]() -> const Polygons& {
        if (collision_trimmed_buffer.empty() && ! collision.empty())
            collision_trimmed_buffer = ClipperUtils::clip_clipper_polygons_with_subject_bbox(collision, get_extents(ret).inflated(std::max((coord_t)0, distance) + SCALED_EPSILON));
        return collision_trimmed_buffer;
    };

    if (distance == 0)
        return do_final_difference ? diff(ret, collision_trimmed()) : union_(ret);
    if (safe_step_size < 0 || last_step_offset_without_check < 0) {
        BOOST_LOG_TRIVIAL(warning) << "Offset increase got invalid parameter!";
        tree_supports_show_error("Negative offset distance... How did you manage this ?"sv, true);
        return do_final_difference ? diff(ret, collision_trimmed()) : union_(ret);
    }

    coord_t step_size = safe_step_size;
    int     steps = distance > last_step_offset_without_check ? (distance - last_step_offset_without_check) / step_size : 0;
    if (distance - steps * step_size > last_step_offset_without_check) {
        if (steps < distance / step_size)
            // This will be the case when last_step_offset_without_check >= safe_step_size
            ++ steps;
        else
            do_final_difference = true;
    }
    if (steps + (distance < last_step_offset_without_check || (distance % step_size) != 0) < int(min_amount_offset) && min_amount_offset > 1) {
        // yes one can add a bool as the standard specifies that a result from compare operators has to be 0 or 1
        // reduce the stepsize to ensure it is offset the required amount of times
        step_size = distance / min_amount_offset;
        if (step_size >= safe_step_size) {
            // effectivly reduce last_step_offset_without_check
            step_size = safe_step_size;
            steps = min_amount_offset;
        } else
            steps = distance / step_size;
    }
    // offset in steps
    for (int i = 0; i < steps; ++ i) {
        ret = diff(offset(ret, step_size, ClipperLib::jtRound, scaled<float>(0.01)), collision_trimmed());
        // ensure that if many offsets are done the performance does not suffer extremely by the new vertices of jtRound.
        if (i % 10 == 7)  // every 10th sample, offset by 7
            ret = polygons_simplify(ret, scaled<double>(0.015), polygons_strictly_simple);
    }
    // offset the remainder
    float last_offset = distance - steps * step_size;
    if (last_offset > SCALED_EPSILON)
        ret = offset(ret, distance - steps * step_size, ClipperLib::jtRound, scaled<float>(0.01));
    ret = polygons_simplify(ret, scaled<double>(0.015), polygons_strictly_simple);

    if (do_final_difference)
        ret = diff(ret, collision_trimmed());
    return union_(ret);
}

class RichInterfacePlacer : public InterfacePlacer {
public:
    RichInterfacePlacer(
        const InterfacePlacer        &interface_placer,
        const OrcaTreeModelVolumes       &volumes,
        bool                          force_tip_to_roof,
        size_t                        num_support_layers,
        std::vector<SupportElements> &move_bounds)
    :
        InterfacePlacer(interface_placer),
        volumes(volumes), force_tip_to_roof(force_tip_to_roof), move_bounds(move_bounds)
    {
        m_already_inserted.assign(num_support_layers, {});
        this->min_xy_dist = this->config.xy_distance > this->config.xy_min_distance;
        m_base_radius = scaled<coord_t>(0.01);
        m_base_circle = Polygon{ make_circle(m_base_radius, SUPPORT_TREE_CIRCLE_RESOLUTION) };

    }
    const OrcaTreeModelVolumes                             &volumes;
    // Radius of the tree tip is large enough to be covered by an interface.
    const bool                                          force_tip_to_roof;
    bool                                                min_xy_dist;

public:
    // called by sample_overhang_area()
    void add_points_along_lines(
        // Insert points (tree tips or top contact interfaces) along these lines.
        LineInformations    lines,
        // Start at this layer.
        LayerIndex          insert_layer_idx,
        // Insert this number of interface layers.
        size_t              roof_tip_layers,
        // True if an interface is already generated above these lines.
        size_t              supports_roof_layers,
        // The element tries to not move until this dtt is reached.
        size_t              dont_move_until)
    {
        validate_range(lines);
        // Add tip area as roof (happens when minimum roof area > minimum tip area) if possible
        size_t dtt_roof_tip;
        for (dtt_roof_tip = 0; dtt_roof_tip < roof_tip_layers && insert_layer_idx - dtt_roof_tip >= 1; ++ dtt_roof_tip) {
            size_t this_layer_idx = insert_layer_idx - dtt_roof_tip;
            const size_t roof_recovery_depth = dtt_roof_tip + supports_roof_layers;
            auto evaluateRoofWillGenerate = [&](const std::pair<Point, LineStatus> &p) {
                // Note (Vojtech): the circle is just shifted and has a known size, so the infill is assumed
                // to always fit (an explicit generate_support_infill_lines check was removed here).
                return true;
            };

            {
                std::pair<LineInformations, LineInformations> split =
                    // keep all lines that are still valid on the next layer
                    split_lines(lines, [this, this_layer_idx](const std::pair<Point, LineStatus> &p)
                        { return evaluate_point_for_next_layer_function(volumes, config, this_layer_idx, p); });
                LineInformations points = std::move(split.second);
                // Not all roofs are guaranteed to actually generate lines, so filter these out and add them as points.
                split = split_lines(split.first, evaluateRoofWillGenerate);
                lines = std::move(split.first);
                append(points, split.second);
                // add all points that would not be valid
                for (const LineInformation &line : points)
                    for (const std::pair<Point, LineStatus> &point_data : line)
                        add_point_as_influence_area(point_data, this_layer_idx,
                            // don't move until
                            roof_tip_layers - dtt_roof_tip,
                            // supports roof
                            roof_recovery_depth > 0,
                            // recovered roof/contact depth for this slice
                            roof_recovery_depth,
                            // disable ovalization
                            false);
            }

            // add all tips as roof to the roof storage
            Polygons new_roofs;
            for (const LineInformation &line : lines)
                // Possible enhancement: sweep the tip radius along the line?
                for (const std::pair<Point, LineStatus> &p : line) {
                    Polygon roof_circle{ m_base_circle };
                    roof_circle.scale(double(config.min_radius) / m_base_radius);
                    roof_circle.translate(p.first);
                    new_roofs.emplace_back(std::move(roof_circle));
                }
            this->add_roof(std::move(new_roofs), this_layer_idx, roof_recovery_depth);
        }

        const size_t roof_recovery_depth = dtt_roof_tip + supports_roof_layers;
        for (const LineInformation &line : lines) {
            // If a line consists of enough tips, the assumption is that it is not a single tip, but part of a simulated support pattern.
            // Ovalisation should be disabled for these to improve the quality of the lines when tip_diameter=line_width
            bool disable_ovalistation = config.min_radius < 3 * config.support_line_width && roof_tip_layers == 0 && dtt_roof_tip == 0 && line.size() > 5;  // radius below 3x the line width
            for (const std::pair<Point, LineStatus> &point_data : line)
                add_point_as_influence_area(point_data, insert_layer_idx - dtt_roof_tip,
                    // don't move until
                    dont_move_until > dtt_roof_tip ? dont_move_until - dtt_roof_tip : 0,
                    // supports roof
                    roof_recovery_depth > 0,
                    // recovered roof/contact depth for this slice
                    roof_recovery_depth,
                    disable_ovalistation);
        }
    }

private:
    // called by this->add_points_along_lines()
    void add_point_as_influence_area(std::pair<Point, LineStatus> p, LayerIndex insert_layer, size_t dont_move_until, bool roof, size_t roof_recovery_dtt, bool skip_ovalisation)
    {
        bool to_bp = p.second == LineStatus::TO_BP || p.second == LineStatus::TO_BP_SAFE;
        bool gracious = to_bp || p.second == LineStatus::TO_MODEL_GRACIOUS || p.second == LineStatus::TO_MODEL_GRACIOUS_SAFE;
        bool safe_radius = p.second == LineStatus::TO_BP_SAFE || p.second == LineStatus::TO_MODEL_GRACIOUS_SAFE;
        if (! config.support_rests_on_model && ! to_bp) {
            BOOST_LOG_TRIVIAL(warning) << "Tried to add an invalid support point";
            tree_supports_show_error("Unable to add tip. Some overhang may not be supported correctly."sv, true);
            return;
        }
        Polygons circle{ m_base_circle };
        circle.front().translate(p.first);
        {
            Point hash_pos = p.first / ((config.min_radius + 1) / 10);  // grid cell ~ min_radius / 10
            std::lock_guard<std::mutex> critical_section_movebounds(m_mutex_movebounds);
            if (!m_already_inserted[insert_layer].count(hash_pos)) {
                // normalize the point a bit to also catch points which are so close that inserting it would achieve nothing
                m_already_inserted[insert_layer].emplace(hash_pos);
                static constexpr const size_t dtt = 0;
                SupportElementState state;
                state.target_height = insert_layer;
                state.target_position = p.first;
                state.next_position = p.first;
                state.layer_idx = insert_layer;
                state.effective_radius_height = dtt;
                state.to_buildplate = to_bp;
                state.distance_to_top = dtt;
                state.result_on_layer = p.first;
                assert(state.result_on_layer_is_set());
                state.increased_to_model_radius = 0;
                state.to_model_gracious = gracious;
                state.elephant_foot_increases = 0;
                state.use_min_xy_dist = min_xy_dist;
                state.supports_roof = roof;
                state.dont_move_until = dont_move_until;
                state.can_use_safe_radius = safe_radius;
                state.set_pending_roof_recovery(force_tip_to_roof ? dont_move_until : 0, roof_recovery_dtt);
                state.skip_ovalisation = skip_ovalisation;
                move_bounds[insert_layer].emplace_back(state, std::move(circle));
            }
        }
    }

    // Outputs
    std::vector<SupportElements>                       &move_bounds;

    // Temps
    coord_t m_base_radius;
    Polygon                                       m_base_circle;

    // Mutexes, guards
    std::mutex                                          m_mutex_movebounds;
    std::vector<std::unordered_set<Point, PointHash>>   m_already_inserted;
};

static int generate_raft_contact(
    const PrintObject               &print_object,
    const OrcaTreeSupportSettings       &config,
    InterfacePlacer                 &interface_placer)
{
    int raft_contact_layer_idx = -1;
    if (print_object.has_raft() && print_object.layer_count() > 0) {
        // Produce raft contact layer outside of the tree support loop, so that no trees will be generated for the raft contact layer.
        // Raft layers supporting raft contact interface will be produced by the classic raft generator.
        // Find the raft contact layer.
        raft_contact_layer_idx = int(config.raft_layers.size()) - 1;
        while (raft_contact_layer_idx > 0 && config.raft_layers[raft_contact_layer_idx] > print_object.slicing_parameters().raft_contact_top_z + EPSILON)
            -- raft_contact_layer_idx;
        // Create the raft contact layer.
        const ExPolygons &lslices   = print_object.get_layer(0)->lslices();
        double            expansion = print_object.config().raft_expansion.value;
        interface_placer.add_roof_unguarded(expansion > 0 ? expand(lslices, scaled<float>(expansion)) : to_polygons(lslices), raft_contact_layer_idx, 0);
    }
    return raft_contact_layer_idx;
}

static void finalize_raft_contact(
    const PrintObject               &print_object,
    const int                        raft_contact_layer_idx,
    SupportGeneratorLayersPtr       &top_contacts,
    std::vector<SupportElements>    &move_bounds)
{
    if (raft_contact_layer_idx < 0)
        return;

    const size_t first_tree_layer = print_object.slicing_parameters().raft_layers() - 1;
    // Remove tree tips that start below the raft contact,
    // remove interface layers below the raft contact.
    for (size_t i = 0; i < first_tree_layer; ++i) {
        top_contacts[i] = nullptr;
        move_bounds[i].clear();
    }
    if (print_object.config().raft_expansion.value <= 0)
        return;

    // If any tips at first_tree_layer now are completely inside the expanded raft layer, remove them as well before they are propagated to the ground.
    Polygons &raft_polygons = top_contacts[raft_contact_layer_idx]->polygons;
    EdgeGrid::Grid grid(get_extents(raft_polygons).inflated(SCALED_EPSILON));
    grid.create(raft_polygons, Polylines{}, coord_t(scale_(10.)));  // 10 mm raft grid resolution
    SupportElements &first_layer_move_bounds = move_bounds[first_tree_layer];
    const double threshold = scaled<double>(print_object.config().raft_expansion.value) * 2.;  // raft expansion counted on both sides
    first_layer_move_bounds.erase(std::remove_if(first_layer_move_bounds.begin(), first_layer_move_bounds.end(),
        [&grid, threshold](const SupportElement &el) {
            coordf_t dist;
            if (! grid.signed_distance_edges(el.state.result_on_layer, threshold, dist))
                return false;

            assert(std::abs(dist) < threshold + SCALED_EPSILON);
            // Support point is inside the expanded raft, remove it.
            return dist < - 0.;
        }), first_layer_move_bounds.end());
}

// Called by generate_initial_areas(), used in parallel by multiple layers.
// Produce
// 1) Maximum num_support_roof_layers roof (top interface & contact) layers.
// 2) Tree tips supporting either the roof layers or the object itself.
// num_support_roof_layers should always be respected:
// If the requested roof/contact stack cannot be generated directly, the affected tree tips
// carry explicit pending roof recovery metadata so the sliced branch geometry can later be
// promoted back to top contacts / interfaces at the correct contact depth.
static void sample_overhang_area(
    // Area to support
    Polygons                           &&overhang_area,
    // If true, then the overhang_area is likely large and wide, thus it is worth to try
    // to cover it with continuous interfaces supported by zig-zag patterned tree tips.
    const bool                           large_horizontal_roof,
    // Index of the top suport layer generated by this function.
    const size_t                         layer_idx,
    // Maximum number of roof (contact, interface) layers between the overhang and tree tips to be generated.
    const size_t                         num_support_roof_layers,
    //
    const coord_t                        connect_length,
    // Configuration classes
    const OrcaTreeSupportMeshGroupSettings& mesh_group_settings,
    // Configuration & Output
    RichInterfacePlacer& interface_placer)
{
    // Assumption is that roof will support roof further up to avoid a lot of unnecessary branches. Each layer down it is checked whether the roof area
    // is still large enough to be a roof and aborted as soon as it is not. This part was already reworked a few times, and there could be an argument
    // made to change it again if there are actual issues encountered regarding supporting roofs.
    // Main problem is that some patterns change each layer, so just calculating points and checking if they are still valid an layer below is not useful,
    // as the pattern may be different one layer below. Same with calculating which points are now no longer being generated as result from
    // a decreasing roof, as there is no guarantee that a line will be above these points. Implementing a separate roof support behavior
    // for each pattern harms maintainability as it very well could be >100 LOC
    auto generate_roof_lines = [&interface_placer, &mesh_group_settings](const Polygons &area, LayerIndex layer_idx) -> Polylines {
        return generate_support_infill_lines(area, interface_placer.support_parameters, true, layer_idx, mesh_group_settings.support_roof_line_distance);
    };

    LineInformations        overhang_lines;
    // Track how many top contact / interface layers were already generated.
    size_t                  dtt_roof             = 0;
    size_t                  layer_generation_dtt = 0;

    if (large_horizontal_roof) {
        assert(num_support_roof_layers > 0);
        // Sometimes roofs could be empty as the pattern does not generate lines if the area is narrow enough (i am looking at you, concentric infill).
        // To catch these cases the added roofs are saved to be evaluated later.
        std::vector<Polygons>   added_roofs(num_support_roof_layers);
        Polygons                last_overhang = overhang_area;
        for (dtt_roof = 0; dtt_roof < num_support_roof_layers && layer_idx - dtt_roof >= 1; ++ dtt_roof) {
            // here the roof is handled. If roof can not be added the branches will try to not move instead
            Polygons forbidden_next;
            {
                const bool min_xy_dist = interface_placer.config.xy_distance > interface_placer.config.xy_min_distance;
                const Polygons &forbidden_next_raw = interface_placer.config.support_rests_on_model ?
                    interface_placer.volumes.getCollision(interface_placer.config.getRadius(0), layer_idx - (dtt_roof + 1), min_xy_dist) :
                    interface_placer.volumes.getAvoidance(interface_placer.config.getRadius(0), layer_idx - (dtt_roof + 1), OrcaTreeModelVolumes::AvoidanceType::Fast, false, min_xy_dist);
                // prevent rounding errors down the line
                // Note: SafetyOffset::Yes at the following diff() might be an alternative.
                forbidden_next = offset(union_ex(forbidden_next_raw), scaled<float>(0.005), jtMiter, CLIPPER_MITER_LIMIT);
            }
            Polygons overhang_area_next = diff(overhang_area, forbidden_next);
            if (area(overhang_area_next) >= mesh_group_settings.minimum_roof_area) {
                added_roofs[dtt_roof] = overhang_area;
                last_overhang = std::move(overhang_area);
                overhang_area = std::move(overhang_area_next);
                continue;
            }

            // Next layer down the roof area would be to small so we have to insert our roof support here.
            if (dtt_roof > 0) {
                const size_t dtt_before = dtt_roof - 1;
                // Produce support head points supporting an interface layer: First produce the interface lines, then sample them.
                overhang_lines = split_lines(
                    convert_lines_to_internal(interface_placer.volumes, interface_placer.config,
                        ensure_maximum_distance_polyline(generate_roof_lines(last_overhang, layer_idx - dtt_before), connect_length, 1), layer_idx - dtt_before),
                    [&interface_placer, layer_idx, dtt_before](const std::pair<Point, LineStatus> &p)
                        { return evaluate_point_for_next_layer_function(interface_placer.volumes, interface_placer.config, layer_idx - dtt_before, p); })
                    .first;
            }
            break;
        }

        layer_generation_dtt = std::max(dtt_roof, size_t(1)) - 1; // 1 inside max and -1 outside to avoid underflow. layer_generation_dtt=dtt_roof-1 if dtt_roof!=0;
        // if the roof should be valid, check that the area does generate lines. This is NOT guaranteed.
        if (overhang_lines.empty() && dtt_roof != 0 && generate_roof_lines(overhang_area, layer_idx - layer_generation_dtt).empty())
            for (size_t idx = 0; idx < dtt_roof; idx++) {
                // check for every roof area that it has resulting lines. Remember idx 1 means the 2. layer of roof => higher idx == lower layer
                if (generate_roof_lines(added_roofs[idx], layer_idx - idx).empty()) {
                    dtt_roof = idx;
                    layer_generation_dtt = std::max(dtt_roof, size_t(1)) - 1;
                    break;
                }
            }
        added_roofs.erase(added_roofs.begin() + dtt_roof, added_roofs.end());
        interface_placer.add_roofs(std::move(added_roofs), layer_idx);
    }

    if (overhang_lines.empty()) {
        // support_line_width to form a line here as otherwise most will be unsupported. Technically this violates branch distance, but not only is this the only reasonable choice,
        // but it ensures consistant behaviour as some infill patterns generate each line segment as its own polyline part causing a similar line forming behaviour.
        // This is not doen when a roof is above as the roof will support the model and the trees only need to support the roof
        bool supports_roof = dtt_roof > 0;
        bool continuous_tips = !supports_roof && large_horizontal_roof;
        Polylines polylines = ensure_maximum_distance_polyline(
            generate_support_infill_lines(overhang_area, interface_placer.support_parameters, supports_roof, layer_idx - layer_generation_dtt,
                supports_roof ? mesh_group_settings.support_roof_line_distance : mesh_group_settings.support_tree_branch_distance),
            continuous_tips ? interface_placer.config.min_radius / 2.0 : connect_length, 1);
        size_t point_count = 0;
        for (const Polyline &poly : polylines)
            point_count += poly.size();
        const size_t min_support_points = std::max(coord_t(1), std::min(coord_t(3), coord_t(total_length(overhang_area) / connect_length)));  // at least 1, at most 3 support points
        if (point_count <= min_support_points) {
            // add the outer wall (of the overhang) to ensure it is correct supported instead. Try placing the support points in a way that they fully support the outer wall, instead of just the with half of the the support line width.
            // I assume that even small overhangs are over one line width wide, so lets try to place the support points in a way that the full support area generated from them
            // will support the overhang (if this is not done it may only be half). This WILL NOT be the case when supporting an angle of about < 60 degrees so there is a fallback,
            // as some support is better than none.
            Polygons reduced_overhang_area = offset(union_ex(overhang_area), -interface_placer.config.support_line_width / 2.2, jtMiter, CLIPPER_MITER_LIMIT);
            polylines = ensure_maximum_distance_polyline(
                to_polylines(
                    ! reduced_overhang_area.empty() &&
                        area(offset(diff_ex(overhang_area, reduced_overhang_area), std::max(interface_placer.config.support_line_width, connect_length), jtMiter, CLIPPER_MITER_LIMIT)) < sqr(scaled<double>(0.001)) ?
                    reduced_overhang_area :
                    overhang_area),
                connect_length, min_support_points);
        }
        overhang_lines = convert_lines_to_internal(interface_placer.volumes, interface_placer.config, polylines, layer_idx - dtt_roof);
    }

    assert(dtt_roof <= layer_idx);
    if (dtt_roof >= layer_idx && large_horizontal_roof)
        // Reached buildplate when generating contact, interface and base interface layers.
        interface_placer.add_roof_build_plate(std::move(overhang_area), dtt_roof);
    else {
        // normal trees have to be generated
        const bool roof_enabled = num_support_roof_layers > 0;
        interface_placer.add_points_along_lines(
            // Sample along these lines
            overhang_lines,
            // First layer index to insert the tree tips or interfaces.
            layer_idx - dtt_roof,
            // Remaining roof tip layers.
            interface_placer.force_tip_to_roof ? num_support_roof_layers - dtt_roof : 0,
            // Supports roof already? How many roof layers were already produced above these tips?
            dtt_roof,
            // Don't move until the following distance to top is reached.
            roof_enabled ? num_support_roof_layers - dtt_roof : 0);
    }
}

static void expand_overhang_for_tip_radius(
    const OrcaTreeModelVolumes &volumes,
    const OrcaTreeSupportSettings &config,
    size_t layer_idx,
    coord_t extra_outset,
    coord_t circle_length_to_half_linewidth_change,
    const Polygons &relevant_forbidden,
    Polygons &overhang_regular,
    Polygons &remaining_overhang)
{
    const coord_t minimum_offset_step = config.support_line_width / 8; // One-eighth of the support line width.
    const Polygons &raw_collision = volumes.getCollision(0, layer_idx, true);
    const coord_t collision_offset = config.xy_min_distance + config.support_line_width;
    static constexpr double REDUCTION_OFFSET_FACTOR = 1.5;

    for (coord_t accumulated_offset = 0;
         ! remaining_overhang.empty() && accumulated_offset + minimum_offset_step < extra_outset;) {
        const coord_t current_offset = std::min(
            accumulated_offset + 2 * config.support_line_width > config.min_radius ? // Twice the support line width.
                minimum_offset_step :
                circle_length_to_half_linewidth_change,
            extra_outset - accumulated_offset);
        accumulated_offset += current_offset;

        // Remove regions already covered by support, then inflate the remainder toward the required tip radius.
        remaining_overhang = diff(
            remaining_overhang,
            safe_offset_inc(
                overhang_regular,
                REDUCTION_OFFSET_FACTOR * accumulated_offset,
                raw_collision,
                collision_offset,
                0,
                1));
        overhang_regular = union_(
            overhang_regular,
            diff(
                safe_offset_inc(
                    remaining_overhang,
                    accumulated_offset,
                    raw_collision,
                    collision_offset,
                    0,
                    1),
                relevant_forbidden));
    }
}

static void sample_roof_overhangs(
    const Polygons                           &overhang_raw,
    const Polygons                           &relevant_forbidden,
    Polygons                                 &overhang_regular,
    const OrcaTreeSupportSettings            &config,
    const OrcaTreeSupportMeshGroupSettings   &mesh_group_settings,
    size_t                                    layer_idx,
    size_t                                    num_support_roof_layers,
    coord_t                                   connect_length,
    RichInterfacePlacer                      &interface_placer,
    const std::function<void()>              &throw_on_cancel)
{
    static constexpr coord_t SUPPORT_ROOF_OFFSET = 0;
    Polygons overhang_roofs = safe_offset_inc(
        overhang_raw,
        SUPPORT_ROOF_OFFSET,
        relevant_forbidden,
        config.min_radius * 2 + config.xy_min_distance, // Twice the minimum radius plus XY clearance.
        0,
        1);
    if (mesh_group_settings.minimum_support_area > 0)
        remove_small(overhang_roofs, mesh_group_settings.minimum_roof_area);

    overhang_regular = diff(overhang_regular, overhang_roofs, ApplySafetyOffset::Yes);
    for (ExPolygon &roof_part : union_ex(overhang_roofs)) {
        sample_overhang_area(
            to_polygons(std::move(roof_part)),
            true,
            layer_idx,
            num_support_roof_layers,
            connect_length,
            mesh_group_settings,
            interface_placer);
        throw_on_cancel();
    }
}

/*!
 * \brief Creates the initial influence areas (that can later be propagated down) by placing them below the overhang.
 *
 * Generates Points where the Model should be supported and creates the areas where these points have to be placed.
 *
 * \param mesh[in] The mesh that is currently processed.
 * \param move_bounds[out] Storage for the influence areas.
 * \param storage[in] Background storage, required for adding roofs.
 */
static void generate_initial_areas(
    const PrintObject               &print_object,
    const OrcaTreeModelVolumes          &volumes,
    const OrcaTreeSupportSettings       &config,
    const std::vector<Polygons>     &overhangs,
    std::vector<SupportElements>    &move_bounds,
    InterfacePlacer                 &interface_placer,
    std::function<void()>            throw_on_cancel)
{
    using                           AvoidanceType = OrcaTreeModelVolumes::AvoidanceType;
    OrcaTreeSupportMeshGroupSettings    mesh_group_settings(print_object);

    // To ensure z_distance_top_layers are left empty between the overhang (zeroth empty layer), the support has to be added z_distance_top_layers+1 layers below
    const size_t z_distance_delta = config.z_distance_top_layers + 1;

    const bool min_xy_dist = config.xy_distance > config.xy_min_distance;

    const coord_t connect_length = (config.support_line_width * 100. / mesh_group_settings.support_tree_top_rate) + std::max(2. * config.min_radius - 1.0 * config.support_line_width, 0.0);  // connect length: 2x min radius minus line width
    // As r*r=x*x+y*y (circle equation): If a circle with center at (0,0) the top most point is at (0,r) as in y=r.
    // This calculates how far one has to move on the x-axis so that y=r-support_line_width/2.
    // In other words how far does one need to move on the x-axis to be support_line_width/2 away from the circle line.
    // As a circle is round this length is identical for every axis as long as the 90 degrees angle between both remains.
    const coord_t circle_length_to_half_linewidth_change = config.min_radius < config.support_line_width ?
        config.min_radius / 2 :  // half the min radius
        scale_(sqrt(sqr(unscale<double>(config.min_radius)) - sqr(unscale<double>(config.min_radius - config.support_line_width / 2))));  // half line width (circle offset via Pythagoras)
    // Extra support offset to compensate for larger tip radiis. Also outset a bit more when z overwrites xy, because supporting something with a part of a support line is better than not supporting it at all.
    // Known limitation (Vojtech): this is not sufficient for support enforcers to work; it accounts for
    // neither the support overhang angle nor the width of the collision regions. A disabled heuristic
    // (+ 10 * support_line_width) existed to make enforcers work.
    const coord_t extra_outset = std::max(coord_t(0), config.min_radius - config.support_line_width / 2) + (min_xy_dist ? config.support_line_width / 2 : 0);  // half line width outset
    const size_t  num_support_roof_layers = mesh_group_settings.support_roof_layers;
    const bool    roof_enabled        = num_support_roof_layers > 0;
    const bool    force_tip_to_roof   = roof_enabled && (interface_placer.support_parameters.soluble_interface || coord_sqr(config.min_radius) * M_PI > mesh_group_settings.minimum_roof_area);
    size_t                                          num_support_layers;
    int                                             raft_contact_layer_idx;
    // Layers with their overhang regions.
    std::vector<std::pair<size_t, const Polygons*>>  raw_overhangs;

    {
        const size_t num_raft_layers     = config.raft_layers.size();
        const size_t first_support_layer = std::max(int(num_raft_layers) - int(z_distance_delta), 1);
        num_support_layers  = size_t(std::max(0, int(print_object.layer_count()) + int(num_raft_layers) - int(z_distance_delta)));
        raft_contact_layer_idx = generate_raft_contact(print_object, config, interface_placer);
        // Enumerate layers for which the support tips may be generated from overhangs above.
        raw_overhangs.reserve(num_support_layers - first_support_layer);
        for (size_t layer_idx = first_support_layer; layer_idx < num_support_layers; ++ layer_idx)
            if (const size_t overhang_idx = layer_idx + z_distance_delta; ! overhangs[overhang_idx].empty())
                raw_overhangs.push_back({ layer_idx, &overhangs[overhang_idx] });
    }

    RichInterfacePlacer rich_interface_placer{ interface_placer, volumes, force_tip_to_roof, num_support_layers, move_bounds };

    tbb::parallel_for(tbb::blocked_range<size_t>(0, raw_overhangs.size()),
        [&volumes, &config, &raw_overhangs, &mesh_group_settings,
         min_xy_dist, roof_enabled, num_support_roof_layers, extra_outset, circle_length_to_half_linewidth_change, connect_length,
         &rich_interface_placer, &throw_on_cancel](const tbb::blocked_range<size_t> &range) {
        for (size_t raw_overhang_idx = range.begin(); raw_overhang_idx < range.end(); ++ raw_overhang_idx) {
            size_t           layer_idx    = raw_overhangs[raw_overhang_idx].first;
            const Polygons  &overhang_raw = *raw_overhangs[raw_overhang_idx].second;

            // take the least restrictive avoidance possible
            Polygons relevant_forbidden;
            {
                const Polygons &relevant_forbidden_raw = config.support_rests_on_model ?
                    volumes.getCollision(config.getRadius(0), layer_idx, min_xy_dist) :
                    volumes.getAvoidance(config.getRadius(0), layer_idx, AvoidanceType::Fast, false, min_xy_dist);
                // prevent rounding errors down the line, points placed directly on the line of the forbidden area may not be added otherwise.
                relevant_forbidden = offset(union_ex(relevant_forbidden_raw), scaled<float>(0.005), jtMiter, CLIPPER_MITER_LIMIT);
            }

            // every overhang has saved if a roof should be generated for it. This can NOT be done in the for loop as an area may NOT have a roof
            // even if it is larger than the minimum_roof_area when it is only larger because of the support horizontal expansion and
            // it would not have a roof if the overhang is offset by support roof horizontal expansion instead. (At least this is the current behavior of the regular support)
            // With no support offset, safe_offset_inc reduces to the required collision clipping.
            Polygons overhang_regular = safe_offset_inc(
                overhang_raw,
                mesh_group_settings.support_offset,
                relevant_forbidden,
                config.min_radius * 1.75 + config.xy_min_distance,
                0,
                1);

            // Exclude areas already reachable by half a support line.
            Polygons remaining_overhang = intersection(
                diff(
                    mesh_group_settings.support_offset == 0 ?
                        overhang_raw :
                        offset(union_ex(overhang_raw), mesh_group_settings.support_offset, jtMiter, CLIPPER_MITER_LIMIT),
                    offset(union_ex(overhang_regular), config.support_line_width * HALF, jtMiter, CLIPPER_MITER_LIMIT)),
                relevant_forbidden);
            expand_overhang_for_tip_radius(
                volumes,
                config,
                layer_idx,
                extra_outset,
                circle_length_to_half_linewidth_change,
                relevant_forbidden,
                overhang_regular,
                remaining_overhang);

            throw_on_cancel();

            if (roof_enabled) {
                // Cover the bottom-most dense interface with tree tips.
                sample_roof_overhangs(
                    overhang_raw,
                    relevant_forbidden,
                    overhang_regular,
                    config,
                    mesh_group_settings,
                    layer_idx,
                    num_support_roof_layers,
                    connect_length,
                    rich_interface_placer,
                    throw_on_cancel);
            }
            // Either the roof is not enabled, then these are all the overhangs to be supported,
            // or roof is enabled and these are the thin overhangs at object slopes (not horizontal overhangs).
            if (mesh_group_settings.minimum_support_area > 0)
                remove_small(overhang_regular, mesh_group_settings.minimum_support_area);

            for (ExPolygon &support_part : union_ex(overhang_regular)) {
                sample_overhang_area(to_polygons(std::move(support_part)),
                    false, layer_idx, num_support_roof_layers, connect_length,
                    mesh_group_settings, rich_interface_placer);
                throw_on_cancel();
            }
        }
    });

    finalize_raft_contact(print_object, raft_contact_layer_idx, interface_placer.top_contacts_mutable(), move_bounds);
}

struct MoveInsideCandidate
{
    Point        point;
    double       distance_squared = std::numeric_limits<double>::max();
    unsigned int polygon_index = static_cast<unsigned int>(-1);
    bool         already_on_correct_side = false;
};

static void update_move_inside_vertex_candidate(
    const Point &previous_vertex,
    const Point &vertex,
    const Vec2i64 &next_segment,
    const Point &source,
    int distance,
    unsigned int polygon_index,
    MoveInsideCandidate &best)
{
    const auto distance_squared = (vertex - source).cast<int64_t>().squaredNorm();
    if (distance_squared >= best.distance_squared)
        return;

    best.distance_squared = distance_squared;
    best.polygon_index = polygon_index;
    if (distance == 0) {
        best.point = vertex;
        return;
    }

    const Vec2d next_segment_d = next_segment.cast<double>();
    const Vec2d previous_segment_d = (vertex - previous_vertex).cast<double>();
    const double next_length = next_segment_d.norm();
    const double previous_length = previous_segment_d.norm();
    // Use a scaled bisector to retain precision through the eventual normalization.
    const Vec2d inward_direction = perp(
        next_segment_d * (scaled<double>(10.0) / next_length) +
        previous_segment_d * (scaled<double>(10.0) / previous_length));
    best.point = vertex + (inward_direction * (distance / inward_direction.norm())).cast<coord_t>();
    best.already_on_correct_side =
        inward_direction.dot((source - vertex).cast<double>()) * distance >= 0;
}

static void update_move_inside_segment_candidate(
    const Point &segment_start,
    const Vec2i64 &segment,
    int64_t projection,
    int64_t segment_length_squared,
    const Point &source,
    int distance,
    unsigned int polygon_index,
    MoveInsideCandidate &best)
{
    const Point projected = segment_start +
        (segment.cast<double>() * (double(projection) / double(segment_length_squared))).cast<coord_t>();
    const auto distance_squared = (source - projected).cast<int64_t>().squaredNorm();
    if (distance_squared >= best.distance_squared)
        return;

    best.distance_squared = distance_squared;
    best.polygon_index = polygon_index;
    if (distance == 0) {
        best.point = projected;
        return;
    }

    const Vec2d segment_d = segment.cast<double>();
    const Vec2d inward_direction =
        perp(segment_d * (distance / segment_d.norm())); // The distance sign selects inward or outward.
    best.point = projected + inward_direction.cast<coord_t>();
    best.already_on_correct_side =
        inward_direction.dot((source - projected).cast<double>()) >= 0;
}

static unsigned int move_inside(const Polygons &polygons, Point &from, int distance = 0, int64_t maxDist2 = std::numeric_limits<int64_t>::max())
{
    MoveInsideCandidate best { from };
    for (unsigned int poly_idx = 0; poly_idx < polygons.size(); ++ poly_idx) {
        const Polygon &poly = polygons[poly_idx];
        if (poly.size() < 2)  // A polygon needs at least two points.
            continue;

        Point previous_vertex = poly[poly.size() - 2];
        Point vertex = poly.back();
        // Compare squared lengths throughout to avoid division and integer-rounding edge cases.
        bool projected_beyond_previous_segment =
            (vertex - previous_vertex).cast<int64_t>().dot((from - previous_vertex).cast<int64_t>()) >=
            (vertex - previous_vertex).cast<int64_t>().squaredNorm();
        for (const Point &next_vertex : poly) {
            const Vec2i64 segment = (next_vertex - vertex).cast<int64_t>();
            const int64_t segment_length_squared = segment.squaredNorm();
            if (segment_length_squared <= 0) {
                // Skip one of two adjacent coincident points without changing the preceding edge.
                vertex = next_vertex;
                continue;
            }

            const int64_t projection = segment.dot((from - vertex).cast<int64_t>());
            if (projection <= 0) {
                if (projected_beyond_previous_segment)
                    update_move_inside_vertex_candidate(
                        previous_vertex, vertex, segment, from, distance, poly_idx, best);
                projected_beyond_previous_segment = false;
            } else if (projection >= segment_length_squared) {
                projected_beyond_previous_segment = true;
            } else {
                projected_beyond_previous_segment = false;
                update_move_inside_segment_candidate(
                    vertex, segment, projection, segment_length_squared, from, distance, poly_idx, best);
            }

            previous_vertex = vertex;
            vertex = next_vertex;
        }
    }

    // Preserve a point that is already far enough inside or outside the boundary.
    const double required_distance_squared = double(distance) * double(distance);
    if (best.already_on_correct_side) {
        if (best.distance_squared < required_distance_squared)
            from = best.point;
        return best.polygon_index;
    }
    if (best.distance_squared < maxDist2) {
        from = best.point;
        return best.polygon_index;
    }
    return static_cast<unsigned int>(-1);
}

static Point move_inside_if_outside(const Polygons &polygons, Point from, int distance = 0, int64_t maxDist2 = std::numeric_limits<int64_t>::max())
{
    if (! contains(polygons, from))
        move_inside(polygons, from, distance, maxDist2);
    return from;
}

/*!
 * \brief Checks if an influence area contains a valid subsection and returns the corresponding metadata and the new Influence area.
 *
 * Calculates an influence areas of the layer below, based on the influence area of one element on the current layer.
 * Increases every influence area by maximum_move_distance_slow. If this is not enough, as in we would change our gracious or to_buildplate status the influence areas are instead increased by maximum_move_distance_slow.
 * Also ensures that increasing the radius of a branch, does not cause it to change its status (like to_buildplate ). If this were the case, the radius is not increased instead.
 *
 * Warning: The used format inside this is different as the SupportElement does not have a valid area member. Instead this area is saved as value of the dictionary. This was done to avoid not needed heap allocations.
 *
 * \param settings[in] Which settings have to be used to check validity.
 * \param layer_idx[in] Number of the current layer.
 * \param parent[in] The metadata of the parents influence area.
 * \param relevant_offset[in] The maximal possible influence area. No guarantee regarding validity with current layer collision required, as it is ensured in-function!
 * \param to_bp_data[out] The part of the Influence area that can reach the buildplate.
 * \param to_model_data[out] The part of the Influence area that do not have to reach the buildplate. This has overlap with new_layer_data.
 * \param increased[out]  Area than can reach all further up support points. No assurance is made that the buildplate or the model can be reached in accordance to the user-supplied settings.
 * \param overspeed[in] How much should the already offset area be offset again. Usually this is 0.
 * \param mergelayer[in] Will the merge method be called on this layer. This information is required as some calculation can be avoided if they are not required for merging.
 * \return A valid support element for the next layer regarding the calculated influence areas. Empty if no influence are can be created using the supplied influence area and settings.
 */
[[nodiscard]] static std::optional<SupportElementState> increase_single_area(
    const OrcaTreeModelVolumes      &volumes,
    const OrcaTreeSupportSettings   &config,
    const AreaIncreaseSettings  &settings,
    const LayerIndex             layer_idx,
    const SupportElement        &parent,
    const Polygons              &relevant_offset,
    Polygons                    &to_bp_data,
    Polygons                    &to_model_data,
    Polygons                    &increased,
    const coord_t                overspeed,
    const bool                   mergelayer)
{
    SupportElementState current_elem{ SupportElementState::propagate_down(parent.state) };
    Polygons check_layer_data;
    if (settings.increase_radius)
        current_elem.effective_radius_height += 1;
    coord_t radius = support_element_collision_radius(config, current_elem);
    const auto _tiny_area_threshold = tiny_area_threshold();
    if (settings.move) {
        increased = relevant_offset;
        if (overspeed > 0) {
            const coord_t safe_movement_distance =
                (current_elem.use_min_xy_dist ? config.xy_min_distance : config.xy_distance) +
                (std::min(config.z_distance_top_layers, config.z_distance_bottom_layers) > 0 ? config.min_feature_size : 0);
            // The difference to ensure that the result not only conforms to wall_restriction, but collision/avoidance is done later.
            // The higher last_safe_step_movement_distance comes exactly from the fact that the collision will be subtracted later.
            increased = safe_offset_inc(increased, overspeed, volumes.getWallRestriction(support_element_collision_radius(config, parent.state), layer_idx, parent.state.use_min_xy_dist),
                safe_movement_distance, safe_movement_distance + radius, 1);
        }
        if (settings.no_error && settings.move)
            // as ClipperLib::jtRound has to be used for offsets this simplify is VERY important for performance.
            polygons_simplify(increased, scaled<float>(0.025), polygons_strictly_simple);
    } else
        // if no movement is done the areas keep parent area as no move == offset(0)
        increased = parent.influence_area;

    if (mergelayer || current_elem.to_buildplate) {
        to_bp_data = safe_union(diff_clipped(increased, volumes.getAvoidance(radius, layer_idx - 1, settings.type, false, settings.use_min_distance)));
        if (! current_elem.to_buildplate && area(to_bp_data) > _tiny_area_threshold) {
            // mostly happening in the tip, but with merges one should check every time, just to be sure.
            current_elem.to_buildplate = true; // sometimes nodes that can reach the buildplate are marked as cant reach, tainting subtrees. This corrects it.
            BOOST_LOG_TRIVIAL(debug) << "Corrected taint leading to a wrong to model value on layer " << layer_idx - 1 << " targeting " <<
                current_elem.target_height << " with radius " << radius;
        }
    }
    if (config.support_rests_on_model) {
        if (mergelayer || current_elem.to_model_gracious)
            to_model_data = safe_union(diff_clipped(increased, volumes.getAvoidance(radius, layer_idx - 1, settings.type, true, settings.use_min_distance)));

        if (!current_elem.to_model_gracious) {
            if (mergelayer && area(to_model_data) >= _tiny_area_threshold) {
                current_elem.to_model_gracious = true;
                BOOST_LOG_TRIVIAL(debug) << "Corrected taint leading to a wrong non gracious value on layer " << layer_idx - 1 << " targeting " <<
                    current_elem.target_height << " with radius " << radius;
            } else
                // Cannot route to gracious areas. Push the tree away from object and route it down anyways.
                to_model_data = safe_union(diff_clipped(increased, volumes.getCollision(radius, layer_idx - 1, settings.use_min_distance)));
        }
    }

    check_layer_data = current_elem.to_buildplate ? to_bp_data : to_model_data;

    if (settings.increase_radius && area(check_layer_data) > _tiny_area_threshold) {
        auto validWithRadius = [&](coord_t next_radius) {
            if (volumes.ceilRadius(next_radius, settings.use_min_distance) <= volumes.ceilRadius(radius, settings.use_min_distance))
                return true;

            Polygons to_bp_data_2;
            if (current_elem.to_buildplate)
                // regular union as output will not be used later => this area should always be a subset of the safe_union one (i think)
                to_bp_data_2 = diff_clipped(increased, volumes.getAvoidance(next_radius, layer_idx - 1, settings.type, false, settings.use_min_distance));
            Polygons to_model_data_2;
            if (config.support_rests_on_model && !current_elem.to_buildplate)
                to_model_data_2 = diff_clipped(increased,
                    current_elem.to_model_gracious ?
                        volumes.getAvoidance(next_radius, layer_idx - 1, settings.type, true, settings.use_min_distance) :
                        volumes.getCollision(next_radius, layer_idx - 1, settings.use_min_distance));
            Polygons check_layer_data_2 = current_elem.to_buildplate ? to_bp_data_2 : to_model_data_2;
            return area(check_layer_data_2) > _tiny_area_threshold;
        };
        coord_t ceil_radius_before = volumes.ceilRadius(radius, settings.use_min_distance);

        if (support_element_collision_radius(config, current_elem) < config.increase_radius_until_radius && support_element_collision_radius(config, current_elem) < support_element_radius(config, current_elem)) {
            coord_t target_radius = std::min(support_element_radius(config, current_elem), config.increase_radius_until_radius);
            coord_t current_ceil_radius = volumes.getRadiusNextCeil(radius, settings.use_min_distance);

            while (current_ceil_radius < target_radius && validWithRadius(volumes.getRadiusNextCeil(current_ceil_radius + 1, settings.use_min_distance)))
                current_ceil_radius = volumes.getRadiusNextCeil(current_ceil_radius + 1, settings.use_min_distance);
            size_t resulting_eff_dtt = current_elem.effective_radius_height;
            while (resulting_eff_dtt + 1 < current_elem.distance_to_top &&
                config.getRadius(resulting_eff_dtt + 1, current_elem.elephant_foot_increases) <= current_ceil_radius &&
                config.getRadius(resulting_eff_dtt + 1, current_elem.elephant_foot_increases) <= support_element_radius(config, current_elem))
                ++ resulting_eff_dtt;
            current_elem.effective_radius_height = resulting_eff_dtt;
        }
        radius = support_element_collision_radius(config, current_elem);

        const coord_t foot_radius_increase = std::max(config.bp_radius_increase_per_layer - config.branch_radius_increase_per_layer, 0.0);
        // Is nearly all of the time 1, but sometimes an increase of 1 could cause the radius to become bigger than recommendedMinRadius,
        // which could cause the radius to become bigger than precalculated.
        double planned_foot_increase = std::min(1.0, double(config.recommendedMinRadius(layer_idx - 1) - support_element_radius(config, current_elem)) / foot_radius_increase);
        bool increase_bp_foot = planned_foot_increase > 0 && current_elem.to_buildplate;

        if (increase_bp_foot && support_element_radius(config, current_elem) >= config.branch_radius && support_element_radius(config, current_elem) >= config.increase_radius_until_radius)
            if (validWithRadius(config.getRadius(current_elem.effective_radius_height, current_elem.elephant_foot_increases + planned_foot_increase))) {
                current_elem.elephant_foot_increases += planned_foot_increase;
                radius = support_element_collision_radius(config, current_elem);
            }

        if (ceil_radius_before != volumes.ceilRadius(radius, settings.use_min_distance)) {
            if (current_elem.to_buildplate)
                to_bp_data = safe_union(diff_clipped(increased, volumes.getAvoidance(radius, layer_idx - 1, settings.type, false, settings.use_min_distance)));
            if (config.support_rests_on_model && (!current_elem.to_buildplate || mergelayer))
                to_model_data = safe_union(diff_clipped(increased,
                    current_elem.to_model_gracious ?
                        volumes.getAvoidance(radius, layer_idx - 1, settings.type, true, settings.use_min_distance) :
                        volumes.getCollision(radius, layer_idx - 1, settings.use_min_distance)
                ));
            check_layer_data = current_elem.to_buildplate ? to_bp_data : to_model_data;
            if (area(check_layer_data) < _tiny_area_threshold) {
                BOOST_LOG_TRIVIAL(debug) << "Lost area by doing catch up from " << ceil_radius_before << " to radius " <<
                    volumes.ceilRadius(support_element_collision_radius(config, current_elem), settings.use_min_distance);
                tree_supports_show_error("Area lost catching up radius. May not cause visible malformation."sv, true);
            }
        }
    }

    return area(check_layer_data) > _tiny_area_threshold ? std::optional<SupportElementState>(current_elem) : std::optional<SupportElementState>();
}

struct SupportElementInfluenceAreas {
    // All influence areas: both to build plate and model.
    Polygons                        influence_areas;
    // Influence areas just to build plate.
    Polygons                        to_bp_areas;
    // Influence areas just to model.
    Polygons                        to_model_areas;

    void clear() {
        this->influence_areas.clear();
        this->to_bp_areas.clear();
        this->to_model_areas.clear();
    }
};

struct SupportElementMerging {
    SupportElementState                     state;
    /*!
     * \brief All elements in the layer above the current one that are supported by this element
     */
    SupportElement::ParentIndices           parents;

    SupportElementInfluenceAreas            areas;
    // Bounding box of all influence areas.
    Eigen::AlignedBox<coord_t, 2>           bbox_data;  // 2D bounding box

    const Eigen::AlignedBox<coord_t, 2>&    bbox() const { return bbox_data;}  // 2D bounding box
    const Point                             centroid() const { return (bbox_data.min() + bbox_data.max()) / 2; }  // bbox center (min + max) / 2
    void                                    set_bbox(const BoundingBox& abbox)
        { Point eps { coord_t(SCALED_EPSILON), coord_t(SCALED_EPSILON) }; bbox_data = { abbox.min - eps, abbox.max + eps }; }

    // Called by the AABBTree builder to get an index into the vector of source elements.
    // Not needed, thus zero is returned.
    static size_t                           idx() { return 0; }
};

static void update_movement_offsets(
    const SupportElement &parent,
    const Polygons &wall_restriction,
    const OrcaTreeSupportSettings &config,
    const AreaIncreaseSettings &settings,
    coord_t safe_movement_distance,
    coord_t radius,
    coord_t slow_speed,
    coord_t fast_speed,
    coord_t extra_slow_speed,
    bool calculate_fast_independently,
    Polygons &offset_slow,
    Polygons &offset_fast,
    [[maybe_unused]] LayerIndex layer_idx,
    [[maybe_unused]] size_t merging_area_idx)
{
    if (! settings.move)
        return;

    if (offset_slow.empty() &&
        (settings.increase_speed == slow_speed || ! calculate_fast_independently)) {
        // Two offset steps keep the influence area rounder at single-digit micron precision.
        offset_slow = safe_offset_inc(
            parent.influence_area,
            slow_speed,
            wall_restriction,
            safe_movement_distance,
            calculate_fast_independently ? saturating_add_coord(safe_movement_distance, radius) : 0,
            2);
#ifdef TREESUPPORT_DEBUG_SVG
        SVG::export_expolygons(
            debug_out_path("treesupport-increase_areas_one_layer-slow-%d-%ld.svg", layer_idx, int(merging_area_idx)),
            { { { union_ex(wall_restriction) }, { "wall_restricrictions", "gray", 0.5f } },
              { { union_ex(offset_slow) },      { "offset_slow", "red",  "black", "", scaled<coord_t>(0.1f), 0.5f } } });
#endif // TREESUPPORT_DEBUG_SVG
    }

    if (! offset_fast.empty() || settings.increase_speed == slow_speed)
        return;

    if (calculate_fast_independently) {
        offset_fast = safe_offset_inc(
            parent.influence_area,
            fast_speed,
            wall_restriction,
            safe_movement_distance,
            saturating_add_coord(safe_movement_distance, radius),
            1);
    } else {
        const coord_t delta_slow_fast = saturating_subtract_coord(
            config.maximum_move_distance,
            saturating_add_coord(config.maximum_move_distance_slow, extra_slow_speed));
        offset_fast = safe_offset_inc(
            offset_slow,
            delta_slow_fast,
            wall_restriction,
            safe_movement_distance,
            saturating_add_coord(safe_movement_distance, radius),
            1);
    }
#ifdef TREESUPPORT_DEBUG_SVG
    SVG::export_expolygons(
        debug_out_path("treesupport-increase_areas_one_layer-fast-%d-%ld.svg", layer_idx, int(merging_area_idx)),
        { { { union_ex(wall_restriction) }, { "wall_restricrictions", "gray", 0.5f } },
          { { union_ex(offset_fast) },      { "offset_fast", "red",  "black", "", scaled<coord_t>(0.1f), 0.5f } } });
#endif // TREESUPPORT_DEBUG_SVG
}

/*!
 * \brief Increases influence areas as far as required.
 *
 * Calculates influence areas of the layer below, based on the influence areas of the current layer.
 * Increases every influence area by maximum_move_distance_slow. If this is not enough, as in it would change the gracious or to_buildplate status, the influence areas are instead increased by maximum_move_distance.
 * Also ensures that increasing the radius of a branch, does not cause it to change its status (like to_buildplate ). If this were the case, the radius is not increased instead.
 *
 * Warning: The used format inside this is different as the SupportElement does not have a valid area member. Instead this area is saved as value of the dictionary. This was done to avoid not needed heap allocations.
 *
 * \param to_bp_areas[out] Influence areas that can reach the buildplate
 * \param to_model_areas[out] Influence areas that do not have to reach the buildplate. This has overlap with new_layer_data, as areas that can reach the buildplate are also considered valid areas to the model.
 * This redundancy is required if a to_buildplate influence area is allowed to merge with a to model influence area.
 * \param influence_areas[out] Area than can reach all further up support points. No assurance is made that the buildplate or the model can be reached in accordance to the user-supplied settings.
 * \param bypass_merge_areas[out] Influence areas ready to be added to the layer below that do not need merging.
 * \param last_layer[in] Influence areas of the current layer.
 * \param layer_idx[in] Number of the current layer.
 * \param mergelayer[in] Will the merge method be called on this layer. This information is required as some calculation can be avoided if they are not required for merging.
 */
static void increase_areas_one_layer(
    const OrcaTreeModelVolumes              &volumes,
    const OrcaTreeSupportSettings           &config,
    // New areas at the layer below layer_idx
    std::vector<SupportElementMerging>  &merging_areas,
    // Layer above merging_areas.
    const LayerIndex                     layer_idx,
    // Layer elements above merging_areas.
    SupportElements                     &layer_elements,
    // If false, the merging_areas will not be merged for performance reasons.
    const bool                           mergelayer,
    std::function<void()>                throw_on_cancel)
{
    using AvoidanceType = OrcaTreeModelVolumes::AvoidanceType;

    auto increase_area_at = [&](size_t merging_area_idx) {
            SupportElementMerging   &merging_area   = merging_areas[merging_area_idx];
            assert(merging_area.parents.size() == 1);
            SupportElement          &parent         = layer_elements[merging_area.parents.front()];
            SupportElementState      elem           = SupportElementState::propagate_down(parent.state);
            const Polygons          &wall_restriction =
                // Abstract representation of the model outline. If an influence area would move through it, it could teleport through a wall.
                volumes.getWallRestriction(support_element_collision_radius(config, parent.state), layer_idx, parent.state.use_min_xy_dist);

#ifdef TREESUPPORT_DEBUG_SVG
            SVG::export_expolygons(debug_out_path("treesupport-increase_areas_one_layer-%d-%ld.svg", layer_idx, int(merging_area_idx)),
                { { { union_ex(wall_restriction) },      { "wall_restricrictions", "gray", 0.5f } },
                  { { union_ex(parent.influence_area) }, { "parent", "red",  "black", "", scaled<coord_t>(0.1f), 0.5f } } });
#endif // TREESUPPORT_DEBUG_SVG

            Polygons to_bp_data, to_model_data;
            coord_t radius = support_element_collision_radius(config, elem);

            // When the radius increases, the outer "support wall" of the branch will have been moved farther away from the center (as this is the definition of radius).
            // As it is not specified that the support_tree_angle has to be one of the center of the branch, it is here seen as the smaller angle of the outer wall of the branch, to the outer wall of the same branch one layer above.
            // As the branch may have become larger the distance between these 2 walls is smaller than the distance of the center points.
            // These extra distance is added to the movement distance possible for this layer.

            coord_t extra_speed = 5; // The extra speed is added to both movement distances. Also move 5 microns faster than allowed to avoid rounding errors, this may cause issues at VERY VERY small layer heights.
            coord_t extra_slow_speed = 0; // Only added to the slow movement distance.
            const coord_t ceiled_parent_radius = volumes.ceilRadius(support_element_collision_radius(config, parent.state), parent.state.use_min_xy_dist);
            coord_t projected_radius_increased = config.getRadius(size_t(parent.state.effective_radius_height) + 1, parent.state.elephant_foot_increases);
            coord_t projected_radius_delta = projected_radius_increased - support_element_collision_radius(config, parent.state);

            // When z distance is more than one layer up and down the Collision used to calculate the wall restriction will always include the wall (and not just the xy_min_distance) of the layer
            // above and below like this (d = blocked area because of z distance):
            /*
             *  layer z+1:dddddiiiiiioooo
             *  layer z+0:xxxxxdddddddddd
             *  layer z-1:dddddxxxxxxxxxx
             *  For more detailed visualisation see calculateWallRestrictions
             */
            const coord_t safe_movement_distance = saturating_add_coord(
                elem.use_min_xy_dist ? config.xy_min_distance : config.xy_distance,
                std::min(config.z_distance_top_layers, config.z_distance_bottom_layers) > 0 ? config.min_feature_size : 0);
            if (ceiled_parent_radius == volumes.ceilRadius(projected_radius_increased, parent.state.use_min_xy_dist) ||
                projected_radius_increased < config.increase_radius_until_radius)
                // If it is guaranteed possible to increase the radius, the maximum movement speed can be increased, as it is assumed that the maximum movement speed is the one of the slower moving wall
                extra_speed = saturating_add_coord(extra_speed, projected_radius_delta);
            else {
                // if a guaranteed radius increase is not possible, only increase the slow speed
                // Ensure that the slow movement distance can not become larger than the fast one.
                const coord_t available_slow_increase = saturating_subtract_coord(
                    saturating_add_coord(config.maximum_move_distance, extra_speed),
                    saturating_add_coord(config.maximum_move_distance_slow, extra_slow_speed));
                extra_slow_speed = saturating_add_coord(extra_slow_speed, std::min(projected_radius_delta, available_slow_increase));
            }

            if (config.layer_start_bp_radius > layer_idx &&
                config.recommendedMinRadius(layer_idx - 1) < config.getRadius(size_t(elem.effective_radius_height) + 1, elem.elephant_foot_increases)) {
                // can guarantee elephant foot radius increase
                if (ceiled_parent_radius == volumes.ceilRadius(config.getRadius(size_t(parent.state.effective_radius_height) + 1, parent.state.elephant_foot_increases + 1), parent.state.use_min_xy_dist))
                    extra_speed = saturating_add_coord(extra_speed, config.bp_radius_increase_per_layer);
                else {
                    const coord_t available_slow_increase = saturating_subtract_coord(
                        config.maximum_move_distance,
                        saturating_add_coord(config.maximum_move_distance_slow, extra_slow_speed));
                    extra_slow_speed = saturating_add_coord(
                        extra_slow_speed, std::min(coord_t(config.bp_radius_increase_per_layer), available_slow_increase));
                }
            }

            const coord_t fast_speed = saturating_add_coord(config.maximum_move_distance, extra_speed);
            const coord_t slow_speed = saturating_add_coord(config.maximum_move_distance_slow, extra_speed, extra_slow_speed);

            Polygons offset_slow, offset_fast;

            bool add = false;
            bool bypass_merge = false;
            constexpr bool increase_radius = true, no_error = true, use_min_radius = true, move = true; // aliases for better readability

            // Determine in which order configurations are checked if they result in a valid influence area. Check will stop if a valid area is found
            std::vector<AreaIncreaseSettings> order;
            auto insertSetting = [&](AreaIncreaseSettings settings, bool back) {
                if (std::find(order.begin(), order.end(), settings) == order.end()) {
                    if (back)
                        order.emplace_back(settings);
                    else
                        order.insert(order.begin(), settings);
                }
            };

            const bool parent_moved_slow = elem.last_area_increase.increase_speed < config.maximum_move_distance;
            const bool avoidance_speed_mismatch = parent_moved_slow && elem.last_area_increase.type != AvoidanceType::Slow;
            if (elem.last_area_increase.move && elem.last_area_increase.no_error && elem.can_use_safe_radius && !mergelayer &&
                !avoidance_speed_mismatch && (elem.distance_to_top >= config.tip_layers || parent_moved_slow)) {
                // assume that the avoidance type that was best for the parent is best for me. Makes this function about 7% faster.
                insertSetting({ elem.last_area_increase.type, elem.last_area_increase.increase_speed < config.maximum_move_distance ? slow_speed : fast_speed,
                    increase_radius, elem.last_area_increase.no_error, !use_min_radius, elem.last_area_increase.move }, true);
                insertSetting({ elem.last_area_increase.type, elem.last_area_increase.increase_speed < config.maximum_move_distance ? slow_speed : fast_speed,
                    !increase_radius, elem.last_area_increase.no_error, !use_min_radius, elem.last_area_increase.move }, true);
            }
            // branch may still go though a hole, so a check has to be done whether the hole was already passed, and the regular avoidance can be used.
            if (!elem.can_use_safe_radius) {
                // if the radius until which it is always increased can not be guaranteed, move fast. This is to avoid holes smaller than the real branch radius.
                // This does not guarantee the avoidance of such holes, but ensures they are avoided if possible.
                // order.emplace_back(AvoidanceType::Slow,!increase_radius,no_error,!use_min_radius,move);
                insertSetting({ AvoidanceType::Slow, slow_speed, increase_radius, no_error, !use_min_radius, !move }, true); // did we go through the hole
                // in many cases the definition of hole is overly restrictive, so to avoid unnecessary fast movement in the tip, it is ignored there for a bit.
                // This CAN cause a branch to go though a hole it otherwise may have avoided.
                if (elem.distance_to_top < round_up_divide(config.tip_layers, size_t(2)))  // half the tip layers
                    insertSetting({ AvoidanceType::Fast, slow_speed, increase_radius, no_error, !use_min_radius, !move }, true);
                insertSetting({ AvoidanceType::FastSafe, fast_speed, increase_radius, no_error, !use_min_radius, !move }, true); // did we manage to avoid the hole
                insertSetting({ AvoidanceType::FastSafe, fast_speed, !increase_radius, no_error, !use_min_radius, move }, true);
                insertSetting({ AvoidanceType::Fast, fast_speed, !increase_radius, no_error, !use_min_radius, move }, true);
            } else {
                insertSetting({ AvoidanceType::Slow, slow_speed, increase_radius, no_error, !use_min_radius, move }, true);
                // while moving fast to be able to increase the radius (b) may seems preferable (over a) this can cause the a sudden skip in movement,
                // which looks similar to a layer shift and can reduce stability.
                // as such idx have chosen to only use the user setting for radius increases as a friendly recommendation.
                insertSetting({ AvoidanceType::Slow, slow_speed, !increase_radius, no_error, !use_min_radius, move }, true); // a
                if (elem.distance_to_top < config.tip_layers)
                    insertSetting({ AvoidanceType::FastSafe, slow_speed, increase_radius, no_error, !use_min_radius, move }, true);
                insertSetting({ AvoidanceType::FastSafe, fast_speed, increase_radius, no_error, !use_min_radius, move }, true); // b
                insertSetting({ AvoidanceType::FastSafe, fast_speed, !increase_radius, no_error, !use_min_radius, move }, true);
            }

            if (elem.use_min_xy_dist) {
                std::vector<AreaIncreaseSettings> new_order;
                // if the branch currently has to use min_xy_dist check if the configuration would also be valid
                // with the regular xy_distance before checking with use_min_radius (Only happens when Support Distance priority is z overrides xy )
                for (AreaIncreaseSettings settings : order) {
                    new_order.emplace_back(settings);
                    new_order.push_back({ settings.type, settings.increase_speed, settings.increase_radius, settings.no_error, use_min_radius, settings.move });
                }
                order = new_order;
            }
            if (elem.to_buildplate || (elem.to_model_gracious && intersection(parent.influence_area, volumes.getPlaceableAreas(radius, layer_idx, throw_on_cancel)).empty())) {
                // error case
                // it is normal that we wont be able to find a new area at some point in time if we wont be able to reach layer 0 aka have to connect with the model
                insertSetting({ AvoidanceType::Fast, fast_speed, !increase_radius, !no_error, elem.use_min_xy_dist, move }, true);
            }
            if (elem.distance_to_top < elem.dont_move_until && elem.can_use_safe_radius) // only do not move when holes would be avoided in every case.
                // Only do not move when already in a no hole avoidance with the regular xy distance.
                insertSetting({ AvoidanceType::Slow, 0, increase_radius, no_error, !use_min_radius, !move }, false);

            Polygons inc_wo_collision;
            // Compare the work saved by calculating the fast offset independently with reusing the slow offset.
            const coord_t independent_step_count = radius / safe_movement_distance -
                                                   int(fast_speed < saturating_add_coord(radius, safe_movement_distance));
            const bool calculate_fast_independently =
                independent_step_count > round_up_divide(slow_speed, safe_movement_distance);

            auto increase_lost_area = [&](const AreaIncreaseSettings &settings) {
                constexpr float  DEGENERATE_LINE_OFFSET = 0.005f;
                constexpr double LOST_BRANCH_MOVE_SCALE = 1.5;
                const Polygons lines_offset = offset(
                    to_polylines(parent.influence_area),
                    scaled<float>(DEGENERATE_LINE_OFFSET),
                    jtMiter,
                    CLIPPER_MITER_LIMIT);
                const Polygons base_error_area = union_(parent.influence_area, lines_offset);
                std::optional<SupportElementState> result = increase_single_area(
                    volumes,
                    config,
                    settings,
                    layer_idx,
                    parent,
                    base_error_area,
                    to_bp_data,
                    to_model_data,
                    inc_wo_collision,
                    (config.maximum_move_distance + extra_speed) * LOST_BRANCH_MOVE_SCALE,
                    mergelayer);
#ifdef TREE_SUPPORT_SHOW_ERRORS
                BOOST_LOG_TRIVIAL(error)
#else // TREE_SUPPORT_SHOW_ERRORS
                BOOST_LOG_TRIVIAL(warning)
#endif // TREE_SUPPORT_SHOW_ERRORS
                    << "Influence area could not be increased! Data about the Influence area: "
                       "Radius: " << radius << " at layer: " << layer_idx - 1 << " NextTarget: " << elem.layer_idx << " Distance to top: " << elem.distance_to_top <<
                       " Elephant foot increases " << elem.elephant_foot_increases << " use_min_xy_dist " << elem.use_min_xy_dist << " to buildplate " << elem.to_buildplate <<
                       " gracious " << elem.to_model_gracious << " safe " << elem.can_use_safe_radius << " until move " << elem.dont_move_until << " \n "
                       "Parent " << &parent << ": Radius: " << support_element_collision_radius(config, parent.state) << " at layer: " << layer_idx << " NextTarget: " << parent.state.layer_idx <<
                       " Distance to top: " << parent.state.distance_to_top << " Elephant foot increases " << parent.state.elephant_foot_increases << "  use_min_xy_dist " << parent.state.use_min_xy_dist <<
                       " to buildplate " << parent.state.to_buildplate << " gracious " << parent.state.to_model_gracious << " safe " << parent.state.can_use_safe_radius << " until move " << parent.state.dont_move_until;
                tree_supports_show_error("Potentially lost branch!"sv, true);
#ifdef TREE_SUPPORTS_TRACK_LOST
                if (result)
                    result->lost = true;
#endif // TREE_SUPPORTS_TRACK_LOST
                return result;
            };

            auto increase_area = [&](const AreaIncreaseSettings &settings) {
                return settings.no_error ?
                    increase_single_area(
                        volumes,
                        config,
                        settings,
                        layer_idx,
                        parent,
                        settings.increase_speed == slow_speed ? offset_slow : offset_fast,
                        to_bp_data,
                        to_model_data,
                        inc_wo_collision,
                        0,
                        mergelayer) :
                    increase_lost_area(settings);
            };

            for (const AreaIncreaseSettings &settings : order) {
                update_movement_offsets(
                    parent,
                    wall_restriction,
                    config,
                    settings,
                    safe_movement_distance,
                    radius,
                    slow_speed,
                    fast_speed,
                    extra_slow_speed,
                    calculate_fast_independently,
                    offset_slow,
                    offset_fast,
                    layer_idx,
                    merging_area_idx);
                std::optional<SupportElementState> result = increase_area(settings);

                if (!result) {
                    if (!settings.no_error)
                        BOOST_LOG_TRIVIAL(warning) << "Trying to keep area by moving faster than intended: FAILURE! WRONG BRANCHES LIKLY!";
                    continue;
                }

                elem = *result;
                radius = support_element_collision_radius(config, elem);
                elem.last_area_increase = settings;
                add = true;
                // Do not merge when the branch should not move or moving away from the model has priority.
                bypass_merge = !settings.move || (settings.use_min_distance && elem.distance_to_top < config.tip_layers);
                if (settings.move)
                    elem.dont_move_until = 0;
                else
                    elem.result_on_layer = parent.state.result_on_layer;

                elem.can_use_safe_radius = settings.type != AvoidanceType::Fast;
                if (!settings.use_min_distance)
                    elem.use_min_xy_dist = false;
                if (!settings.no_error) {
#ifdef TREE_SUPPORT_SHOW_ERRORS
                    BOOST_LOG_TRIVIAL(error)
#else // TREE_SUPPORT_SHOW_ERRORS
                    BOOST_LOG_TRIVIAL(info)
#endif // TREE_SUPPORT_SHOW_ERRORS
                        << "Trying to keep area by moving faster than intended: Success";
                }
                break;
            }

            if (add) {
                // Union seems useless, but some rounding errors somewhere can cause to_bp_data to be slightly bigger than it should be.
                assert(! inc_wo_collision.empty() || ! to_bp_data.empty() || ! to_model_data.empty());
                Polygons max_influence_area = safe_union(
                    diff_clipped(inc_wo_collision, volumes.getCollision(radius, layer_idx - 1, elem.use_min_xy_dist)),
                    safe_union(to_bp_data, to_model_data));
                merging_area.state = elem;
                assert(!max_influence_area.empty());
                merging_area.set_bbox(get_extents(max_influence_area));
                merging_area.areas.influence_areas = std::move(max_influence_area);
                if (! bypass_merge) {
                    if (elem.to_buildplate)
                        merging_area.areas.to_bp_areas = std::move(to_bp_data);
                    if (config.support_rests_on_model)
                        merging_area.areas.to_model_areas = std::move(to_model_data);
                }
            } else {
                // If the bottom most point of a branch is set, later functions will assume that the position is valid, and ignore it.
                // But as branches connecting with the model that are to small have to be culled, the bottom most point has to be not set.
                // A point can be set on the top most tip layer (maybe more if it should not move for a few layers).
                parent.state.result_on_layer_reset();
                parent.state.to_model_gracious = false;
#ifdef TREE_SUPPORTS_TRACK_LOST
                parent.state.verylost = true;
#endif // TREE_SUPPORTS_TRACK_LOST
            }

            throw_on_cancel();
    };

    tbb::parallel_for(tbb::blocked_range<size_t>(0, merging_areas.size(), 1),
        [&](const tbb::blocked_range<size_t> &range) {
            for (size_t merging_area_idx = range.begin(); merging_area_idx < range.end(); ++ merging_area_idx)
                increase_area_at(merging_area_idx);
        }, tbb::simple_partitioner());
}

[[nodiscard]] static SupportElementState merge_support_element_states(
    const SupportElementState &first, const SupportElementState &second, const Point &next_position, const coord_t layer_idx,
    const OrcaTreeSupportSettings &config)
{
    SupportElementState out;
    out.next_position   = next_position;
    out.layer_idx       = layer_idx;
    out.use_min_xy_dist = first.use_min_xy_dist || second.use_min_xy_dist;
    out.supports_roof   = first.supports_roof || second.supports_roof;
    out.dont_move_until = std::max(first.dont_move_until, second.dont_move_until);
    out.can_use_safe_radius = first.can_use_safe_radius || second.can_use_safe_radius;
    // Preserve the deepest outstanding roof recovery request across merged sub-branches.
    out.set_pending_roof_recovery(
        std::max(first.missing_roof_layers, second.missing_roof_layers),
        std::max(first.roof_recovery_dtt, second.roof_recovery_dtt));
    out.skip_ovalisation = false;
    if (first.target_height > second.target_height) {
        out.target_height   = first.target_height;
        out.target_position = first.target_position;
    } else {
        out.target_height   = second.target_height;
        out.target_position = second.target_position;
    }
    out.effective_radius_height = std::max(first.effective_radius_height, second.effective_radius_height);
    out.distance_to_top = std::max(first.distance_to_top, second.distance_to_top);

    out.to_buildplate = first.to_buildplate && second.to_buildplate;
    out.to_model_gracious = first.to_model_gracious && second.to_model_gracious; // valid as we do not merge non-gracious with gracious

    out.elephant_foot_increases = 0;
    if (config.bp_radius_increase_per_layer > 0) {
        coord_t foot_increase_radius = std::abs(std::max(support_element_collision_radius(config, second), support_element_collision_radius(config, first)) - support_element_collision_radius(config, out));
        // elephant_foot_increases has to be recalculated, as when a smaller tree with a larger elephant_foot_increases merge with a larger branch
        // the elephant_foot_increases may have to be lower as otherwise the radius suddenly increases. This results often in a non integer value.
        out.elephant_foot_increases = foot_increase_radius / (config.bp_radius_increase_per_layer - config.branch_radius_increase_per_layer);
    }

    // set last settings to the best out of both parents. If this is wrong, it will only cause a small performance penalty instead of weird behavior.
    out.last_area_increase = {
        std::min(first.last_area_increase.type, second.last_area_increase.type),
        std::min(first.last_area_increase.increase_speed, second.last_area_increase.increase_speed),
        first.last_area_increase.increase_radius || second.last_area_increase.increase_radius,
        first.last_area_increase.no_error || second.last_area_increase.no_error,
        first.last_area_increase.use_min_distance && second.last_area_increase.use_min_distance,
        first.last_area_increase.move || second.last_area_increase.move };

    return out;
}

static bool merge_influence_areas_two_elements(
    const OrcaTreeModelVolumes &volumes, const OrcaTreeSupportSettings &config, const LayerIndex layer_idx,
    SupportElementMerging &dst, SupportElementMerging &src)
{
    // Don't merge gracious with a non gracious area as bad placement could negatively impact reliability of the whole subtree.
    const bool merging_gracious_and_non_gracious = dst.state.to_model_gracious != src.state.to_model_gracious;
    // Could cause some issues with the increase of one area, as it is assumed that if the smaller is increased
    // by the delta to the larger it is engulfed by it already. But because a different collision
    // may be removed from the in draw_area() generated circles, this assumption could be wrong.
    const bool merging_min_and_regular_xy        = dst.state.use_min_xy_dist != src.state.use_min_xy_dist;

    if (merging_gracious_and_non_gracious || merging_min_and_regular_xy)
        return false;

    const bool dst_radius_bigger = support_element_collision_radius(config, dst.state) > support_element_collision_radius(config, src.state);
    const SupportElementMerging &smaller_rad = dst_radius_bigger ? src : dst;
    const SupportElementMerging &bigger_rad  = dst_radius_bigger ? dst : src;
    const coord_t real_radius_delta = std::abs(support_element_radius(config, bigger_rad.state) - support_element_radius(config, smaller_rad.state));
    {
        // Testing intersection of bounding boxes.
        // Expand the smaller radius branch bounding box to match the lambda intersect_small_with_bigger() below.
        // Because the lambda intersect_small_with_bigger() applies a rounded offset, a snug offset of the bounding box
        // is sufficient. On the other side, if a mitered offset was used by the lambda,
        // the bounding box expansion would have to account for the mitered extension of the sharp corners.
        Eigen::AlignedBox<coord_t, 2> smaller_bbox = smaller_rad.bbox();  // 2D bounding box
        smaller_bbox.min() -= Point{ real_radius_delta, real_radius_delta };
        smaller_bbox.max() += Point{ real_radius_delta, real_radius_delta };
        if (! smaller_bbox.intersects(bigger_rad.bbox()))
            return false;
    }

    // Accumulator of a radius increase of a "to model" branch by merging in a "to build plate" branch.
    coord_t increased_to_model_radius = 0;
    const bool merging_to_bp                     = dst.state.to_buildplate && src.state.to_buildplate;
    if (! merging_to_bp) {
        // Get the real radius increase as the user does not care for the collision model.
        if (dst.state.to_buildplate != src.state.to_buildplate) {
            // Merging a "to build plate" branch with a "to model" branch.
            // Don't allow merging a thick "to build plate" branch into a thinner "to model" branch.
            const coord_t rdst = support_element_radius(config, dst.state);
            const coord_t rsrc = support_element_radius(config, src.state);
            if (dst.state.to_buildplate) {
                if (rsrc < rdst)
                    increased_to_model_radius = src.state.increased_to_model_radius + rdst - rsrc;
            } else {
                if (rsrc > rdst)
                    increased_to_model_radius = dst.state.increased_to_model_radius + rsrc - rdst;
            }
            if (increased_to_model_radius > config.max_to_model_radius_increase)
                return false;
        }
        // if a merge could place a stable branch on unstable ground, would be increasing the radius further
        // than allowed to when merging to model and to_bp trees or would merge to model before it is known
        // they will even been drawn the merge is skipped
        if (! dst.state.supports_roof && ! src.state.supports_roof &&
            std::max(src.state.distance_to_top, dst.state.distance_to_top) < config.min_dtt_to_model)
            return false;
    }

    // Area of the bigger radius is used to ensure correct placement regarding the relevant avoidance,
    // so if that would change an invalid area may be created.
    if (! bigger_rad.state.can_use_safe_radius && smaller_rad.state.can_use_safe_radius)
        return false;

    // the bigger radius is used to verify that the area is still valid after the increase with the delta.
    // If there were a point where the big influence area could be valid with can_use_safe_radius
    // the element would already be can_use_safe_radius.
    // the smaller radius, which gets increased by delta may reach into the area where use_min_xy_dist is no longer required.
    const bool use_min_radius = bigger_rad.state.use_min_xy_dist && smaller_rad.state.use_min_xy_dist;

    // The idea is that the influence area with the smaller collision radius is increased by the radius difference.
    // If this area has any intersections with the influence area of the larger collision radius, a branch (of the larger collision radius) placed in this intersection, has already engulfed the branch of the smaller collision radius.
    // Because of this a merge may happen even if the influence areas (that represent possible center points of branches) do not intersect yet.
    // Remember that collision radius <= real radius as otherwise this assumption would be false.
    const coord_t   smaller_collision_radius    = support_element_collision_radius(config, smaller_rad.state);
    const Polygons &collision                   = volumes.getCollision(smaller_collision_radius, layer_idx - 1, use_min_radius);
    auto            intersect_small_with_bigger = [real_radius_delta, smaller_collision_radius, &collision, &config](const Polygons &small, const Polygons &bigger) {
        return intersection(
            safe_offset_inc(
                small, real_radius_delta, collision,
                // -3 avoids possible rounding errors
                2 * (config.xy_distance + smaller_collision_radius - 3), 0, 0),  // -3 scaled epsilon
            bigger);
    };
//#define TREES_MERGE_RATHER_LATER
    Polygons intersect = 
#ifdef TREES_MERGE_RATHER_LATER
        intersection(
#else
        intersect_small_with_bigger(            
#endif
        merging_to_bp ? smaller_rad.areas.to_bp_areas : smaller_rad.areas.to_model_areas,
        merging_to_bp ? bigger_rad.areas.to_bp_areas : bigger_rad.areas.to_model_areas);

    const auto _tiny_area_threshold = tiny_area_threshold();
    // dont use empty as a line is not empty, but for this use-case it very well may be (and would be one layer down as union does not keep lines)
    // check if the overlap is large enough (Small ares tend to attract rounding errors in clipper).
    if (area(intersect) <= _tiny_area_threshold)
        return false;

    // While 0.025 was guessed as enough, i did not have reason to change it.
    if (area(offset(intersect, scaled<float>(-0.025), jtMiter, CLIPPER_MITER_LIMIT)) <= _tiny_area_threshold)
        return false;

#ifdef TREES_MERGE_RATHER_LATER
    intersect = 
        intersect_small_with_bigger(
            merging_to_bp ? smaller_rad.areas.to_bp_areas : smaller_rad.areas.to_model_areas,
            merging_to_bp ? bigger_rad.areas.to_bp_areas : bigger_rad.areas.to_model_areas);
#endif

    // Do the actual merge now that the branches are confirmed to be able to intersect.
    // calculate which point is closest to the point of the last merge (or tip center if no merge above it has happened)
    // used at the end to estimate where to best place the branch on the bottom most layer
    // could be replaced with a random point inside the new area
    Point new_pos = move_inside_if_outside(intersect, dst.state.next_position);

    SupportElementState new_state = merge_support_element_states(dst.state, src.state, new_pos, previous_layer_id(layer_idx), config);
    new_state.increased_to_model_radius = increased_to_model_radius == 0 ?
        // increased_to_model_radius was not set yet. Propagate maximum.
        std::max(dst.state.increased_to_model_radius, src.state.increased_to_model_radius) :
        increased_to_model_radius;

    // Rather unioning with "intersect" due to some rounding errors.
    Polygons influence_areas = safe_union(
        intersect_small_with_bigger(smaller_rad.areas.influence_areas, bigger_rad.areas.influence_areas),
        intersect);

    Polygons to_model_areas;
    if (merging_to_bp && config.support_rests_on_model)
        to_model_areas = new_state.to_model_gracious ?
            // Rather unioning with "intersect" due to some rounding errors.
            safe_union(
                intersect_small_with_bigger(smaller_rad.areas.to_model_areas, bigger_rad.areas.to_model_areas),
                intersect) :
            influence_areas;

    dst.parents.insert(dst.parents.end(), src.parents.begin(), src.parents.end());
    dst.state = new_state;
    dst.areas.influence_areas = std::move(influence_areas);
    dst.areas.to_bp_areas.clear();
    dst.areas.to_model_areas.clear();
    if (merging_to_bp) {
        dst.areas.to_bp_areas = std::move(intersect);
        if (config.support_rests_on_model)
            dst.areas.to_model_areas = std::move(to_model_areas);
    } else
        dst.areas.to_model_areas = std::move(intersect);
    // Update the bounding box.
    BoundingBox bbox(get_extents(dst.areas.influence_areas));
    bbox.merge(get_extents(dst.areas.to_bp_areas));
    bbox.merge(get_extents(dst.areas.to_model_areas));
    dst.set_bbox(bbox);
    // Clear the source data.
    src.areas.clear();
    src.parents.clear();
    return true;
}

/*!
 * \brief Merges Influence Areas if possible.
 *
 * Branches which do overlap have to be merged. This helper merges all elements in input with the elements into reduced_new_layer.
 * Elements in input_aabb are merged together if possible, while elements reduced_new_layer_aabb are not checked against each other.
 *
 * \param reduced_aabb[in,out] The already processed elements.
 * \param input_aabb[in] Not yet processed elements
 * \param to_bp_areas[in] The Elements of the current Layer that will reach the buildplate. Value is the influence area where the center of a circle of support may be placed.
 * \param to_model_areas[in] The Elements of the current Layer that do not have to reach the buildplate. Also contains main as every element that can reach the buildplate is not forced to.
 * Value is the influence area where the center of a circle of support may be placed.
 * \param influence_areas[in] The influence areas without avoidance removed.
 * \param insert_bp_areas[out] Elements to be inserted into the main dictionary after the Helper terminates.
 * \param insert_model_areas[out] Elements to be inserted into the secondary dictionary after the Helper terminates.
 * \param insert_influence[out] Elements to be inserted into the dictionary containing the largest possibly valid influence area (ignoring if the area may not be there because of avoidance)
 * \param erase[out] Elements that should be deleted from the above dictionaries.
 * \param layer_idx[in] The Index of the current Layer.
 */

static SupportElementMerging* merge_influence_areas_leaves(
    const OrcaTreeModelVolumes &volumes, const OrcaTreeSupportSettings &config, const LayerIndex layer_idx,
    SupportElementMerging * const dst_begin, SupportElementMerging *dst_end)
{
    // Merging at the lowest level of the AABB tree. Checking one against each other, O(n^2).
    assert(dst_begin < dst_end);
    for (SupportElementMerging *i = dst_begin; i + 1 < dst_end;) {
        for (SupportElementMerging *j = i + 1; j != dst_end;)
            if (merge_influence_areas_two_elements(volumes, config, layer_idx, *i, *j)) {
                // i was merged with j, j is empty.
                if (j != -- dst_end)
                    *j = std::move(*dst_end);
                goto merged;
            } else
                ++ j;
        // not merged
        ++ i;
    merged:
        ;
    }
    return dst_end;
}

static SupportElementMerging* merge_influence_areas_two_sets(
    const OrcaTreeModelVolumes &volumes, const OrcaTreeSupportSettings &config, const LayerIndex layer_idx,
    SupportElementMerging * const dst_begin, SupportElementMerging *       dst_end,
    SupportElementMerging *       src_begin, SupportElementMerging * const src_end)
{
    // Merging src into dst.
    // Areas of src should not overlap with areas of another elements of src.
    // Areas of dst should not overlap with areas of another elements of dst.
    // The memory from dst_begin to src_end is reserved for the merging operation,
    // src follows dst.
    assert(src_begin < src_end);
    assert(dst_begin < dst_end);
    assert(dst_end <= src_begin);
    for (SupportElementMerging *src = src_begin; src != src_end; ++ src) {
        SupportElementMerging         *dst      = dst_begin;
        SupportElementMerging         *merged   = nullptr;
        for (; dst != dst_end; ++ dst)
            if (merge_influence_areas_two_elements(volumes, config, layer_idx, *dst, *src)) {
                merged = dst ++;
                if (src != src_begin)
                    // Compactify src.
                    *src = std::move(*src_begin);
                ++ src_begin;
                break;
            }
        for (; dst != dst_end;)
            if (merge_influence_areas_two_elements(volumes, config, layer_idx, *merged, *dst)) {
                // Compactify dst.
                if (dst != -- dst_end)
                    *dst = std::move(*dst_end);
            } else
                ++ dst;
    }
    // Compactify src elements that were not merged with dst to the end of dst.
    assert(dst_end <= src_begin);
    if (dst_end == src_begin)
        dst_end = src_end;
    else
        while (src_begin != src_end)
            *dst_end ++ = std::move(*src_begin ++);

    return dst_end;
}

/*!
 * \brief Merges Influence Areas at one layer if possible.
 *
 * Branches which do overlap have to be merged. This manages the helper and uses a divide and conquer approach to parallelize this problem. This parallelization can at most accelerate the merging by a factor of 2.
 *
 * \param to_bp_areas[in] The Elements of the current Layer that will reach the buildplate.
 *  Value is the influence area where the center of a circle of support may be placed.
 * \param to_model_areas[in] The Elements of the current Layer that do not have to reach the buildplate. Also contains main as every element that can reach the buildplate is not forced to.
 *  Value is the influence area where the center of a circle of support may be placed.
 * \param influence_areas[in] The Elements of the current Layer without avoidances removed. This is the largest possible influence area for this layer.
 *  Value is the influence area where the center of a circle of support may be placed.
 * \param layer_idx[in] The current layer.
 */
static void merge_influence_areas(
    const OrcaTreeModelVolumes             &volumes,
    const OrcaTreeSupportSettings          &config,
    const LayerIndex                    layer_idx,
    std::vector<SupportElementMerging> &influence_areas,
    std::function<void()>               throw_on_cancel)
{
    const size_t input_size = influence_areas.size();
    if (input_size == 0)
        return;

    // Merging by divide & conquer.
    // The majority of time is consumed by Clipper polygon operations, intersection is accelerated by bounding boxes.
    // Sorting input into an AABB tree helps to perform most of the intersections at first iterations,
    // thus reducing computation when merging larger subtrees.
    // The actual merge logic is found in merge_influence_areas_two_sets.

    // Build an AABB tree over the influence areas.
    // Note: a full tree does not need to be built - the lowest-level branches will always be bucketed.
    // However the additional time consumed is negligible.
    AABBTreeIndirect::Tree<2, coord_t> tree;  // 2D AABB tree
    // Sort influence_areas in place.
    tree.build_modify_input(influence_areas);

    throw_on_cancel();

    // Prepare the initial buckets as ranges of influence areas. The initial buckets contain power of 2 influence areas to follow
    // the branching of the AABB tree.
    // Vectors of ranges of influence areas, following the branching of the AABB tree:
    std::vector<std::pair<SupportElementMerging*, SupportElementMerging*>> buckets;
    // Initial number of buckets for 1st round of merging.
    size_t num_buckets_initial;
    {
        // How many buckets per first merge iteration?
        const size_t num_threads     = tbb::this_task_arena::max_concurrency();
        // 4 buckets per thread if possible,
        const size_t num_buckets_min = (input_size + 2) / 4;  // ceil-divide into buckets
        // 2 buckets per thread otherwise.
        const size_t num_buckets_max = input_size / 2;  // half the input size
        num_buckets_initial          = num_buckets_min >= num_threads ? num_buckets_min : num_buckets_max;
        const size_t bucket_size     = num_buckets_min >= num_threads ? 4 : 2;  // 2 or 4 buckets per thread
        // Fill in the buckets.
        SupportElementMerging *it = influence_areas.data();
        // Reserve one more bucket to keep a single influence area which will not be merged in the first iteration.
        buckets.reserve(num_buckets_initial + 1);
        for (size_t i = 0; i < num_buckets_initial; ++ i, it += bucket_size)
            buckets.emplace_back(std::make_pair(it, it + bucket_size));
        SupportElementMerging *it_end = influence_areas.data() + influence_areas.size();
        if (buckets.back().second >= it_end) {
            // Last bucket is less than size 4, but bigger than size 1.
            buckets.back().second = std::min(buckets.back().second, it_end);
        } else {
            // Last bucket is size 1, it will not be merged in the first iteration.
            assert(it + 1 == it_end);
            buckets.emplace_back(std::make_pair(it, it_end));
        }
    }

    // 1st merge iteration, merge one with each other.
    tbb::parallel_for(tbb::blocked_range<size_t>(0, num_buckets_initial),
        [&](const tbb::blocked_range<size_t> &range) {
        for (size_t idx = range.begin(); idx < range.end(); ++ idx) {
            // Merge bucket_count adjacent to each other, merging uneven bucket numbers into even buckets
            buckets[idx].second = merge_influence_areas_leaves(volumes, config, layer_idx, buckets[idx].first, buckets[idx].second);
            throw_on_cancel();
        }
    });

    // Further merge iterations, merging one AABB subtree with another one, hopefully minimizing intersections between the elements
    // of each of the subtree.
    while (buckets.size() > 1) {
        tbb::parallel_for(tbb::blocked_range<size_t>(0, buckets.size() / 2),  // process bucket pairs (half the count)
            [&](const tbb::blocked_range<size_t> &range) {
            for (size_t idx = range.begin(); idx < range.end(); ++ idx) {
                const size_t bucket_pair_idx = idx * 2;  // bucket-pair start index = idx * 2
                // Merge bucket_count adjacent to each other, merging uneven bucket numbers into even buckets
                buckets[bucket_pair_idx].second = merge_influence_areas_two_sets(volumes, config, layer_idx,
                    buckets[bucket_pair_idx].first, buckets[bucket_pair_idx].second,
                    buckets[bucket_pair_idx + 1].first, buckets[bucket_pair_idx + 1].second);
                throw_on_cancel();
            }
        });
        // Remove odd buckets, which were merged into even buckets.
        size_t new_size = (buckets.size() + 1) / 2;  // half the count, rounded up
        for (size_t i = 1; i < new_size; ++ i)
            buckets[i] = std::move(buckets[i * 2]);  // even-indexed bucket of each pair
        buckets.erase(buckets.begin() + new_size, buckets.end());
    }
}

/*!
 * \brief Propagates influence downwards, and merges overlapping ones.
 *
 * \param move_bounds[in,out] All currently existing influence areas
 */
static void create_layer_pathing(const OrcaTreeModelVolumes &volumes, const OrcaTreeSupportSettings &config, std::vector<SupportElements> &move_bounds, std::function<void()> throw_on_cancel)
{
#ifdef SLIC3R_TREESUPPORTS_PROGRESS
    const double data_size_inverse = 1 / double(move_bounds.size());
    double progress_total = TREE_PROGRESS_PRECALC_AVO + TREE_PROGRESS_PRECALC_COLL + TREE_PROGRESS_GENERATE_NODES;
#endif // SLIC3R_TREESUPPORTS_PROGRESS

    auto dur_inc   = std::chrono::duration_values<std::chrono::nanoseconds>::zero();
    auto dur_total = std::chrono::duration_values<std::chrono::nanoseconds>::zero();

    LayerIndex last_merge_layer_idx = move_bounds.size();
    bool new_element = false;
    const auto _tiny_area_threshold = tiny_area_threshold();

    // Ensures at least one merge operation per 3mm height, 50 layers, 1 mm movement of slow speed or 5mm movement of fast speed (whatever is lowest). Values were guessed.
    size_t max_merge_every_x_layers = std::min(std::min(5000 / (std::max(config.maximum_move_distance, coord_t(100))), 1000 / std::max(config.maximum_move_distance_slow, coord_t(20))), 3000 / config.layer_height);
    size_t merge_every_x_layers = 1;
    // Calculate the influence areas for each layer below (Top down)
    // This is done by first increasing the influence area by the allowed movement distance, and merging them with other influence areas if possible
    for (int layer_idx = int(move_bounds.size()) - 1; layer_idx > 0; -- layer_idx)
        if (SupportElements &prev_layer = move_bounds[layer_idx]; ! prev_layer.empty()) {
            // merging is expensive and only parallelized to a max speedup of 2. As such it may be useful in some cases to only merge every few layers to improve performance.
            bool had_new_element = new_element;
            const bool merge_this_layer = had_new_element || size_t(last_merge_layer_idx - layer_idx) >= merge_every_x_layers;
            if (had_new_element)
                merge_every_x_layers = 1;
            const auto ta               = std::chrono::high_resolution_clock::now();

            // ### Increase the influence areas by the allowed movement distance
            std::vector<SupportElementMerging> influence_areas;
            influence_areas.reserve(prev_layer.size());
            for (int32_t element_idx = 0; element_idx < int32_t(prev_layer.size()); ++ element_idx) {
                SupportElement &el = prev_layer[element_idx];
                assert(!el.influence_area.empty());
                SupportElement::ParentIndices parents;
                parents.emplace_back(element_idx);
                influence_areas.push_back({ el.state, parents });
            }
            increase_areas_one_layer(volumes, config, influence_areas, layer_idx, prev_layer, merge_this_layer, throw_on_cancel);

            // Place already fully constructed elements to the output, remove them from influence_areas.
            SupportElements &this_layer = move_bounds[previous_layer_index(layer_idx)];
            influence_areas.erase(std::remove_if(influence_areas.begin(), influence_areas.end(),
                [&this_layer, &_tiny_area_threshold, layer_idx](SupportElementMerging &elem) {
                    if (elem.areas.influence_areas.empty())
                        // This area was removed completely due to collisions.
                        return true;
                    if (! elem.areas.to_bp_areas.empty() || ! elem.areas.to_model_areas.empty())
                        // Keep the area.
                        return false;

                    if (area(elem.areas.influence_areas) < _tiny_area_threshold) {
                        BOOST_LOG_TRIVIAL(warning) << "Insert Error of Influence area bypass on layer " << layer_idx - 1;
                        tree_supports_show_error("Insert error of area after bypassing merge.\n"sv, true);
                    }
                    // Move the area to output.
                    this_layer.emplace_back(elem.state, std::move(elem.parents), std::move(elem.areas.influence_areas));
                    return true;
                }),
                influence_areas.end());

            dur_inc += std::chrono::high_resolution_clock::now() - ta;
            new_element = ! move_bounds[previous_layer_index(layer_idx)].empty();
            if (merge_this_layer) {
                bool reduced_by_merging = false;
                if (size_t count_before_merge = influence_areas.size(); count_before_merge > 1) {
                    // ### Calculate which influence areas overlap, and merge them into a new influence area (simplified: an intersection of influence areas that have such an intersection)
                    merge_influence_areas(volumes, config, layer_idx, influence_areas, throw_on_cancel);
                    reduced_by_merging = count_before_merge > influence_areas.size();
                }
                last_merge_layer_idx = layer_idx;
                if (! reduced_by_merging && ! had_new_element)
                    merge_every_x_layers = std::min(max_merge_every_x_layers, merge_every_x_layers + 1);
            }

            dur_total += std::chrono::high_resolution_clock::now() - ta;

            // Save calculated elements to output, and allocate Polygons on heap, as they will not be changed again.
            for (SupportElementMerging &elem : influence_areas)
                if (! elem.areas.influence_areas.empty()) {
                    Polygons new_area = safe_union(elem.areas.influence_areas);
                    if (area(new_area) < _tiny_area_threshold) {
                        BOOST_LOG_TRIVIAL(warning) << "Insert Error of Influence area on layer " << layer_idx - 1 << ". Origin of " << elem.parents.size() << " areas. Was to bp " << elem.state.to_buildplate;
                        tree_supports_show_error("Insert error of area after merge.\n"sv, true);
                    }
                    this_layer.emplace_back(elem.state, std::move(elem.parents), std::move(new_area));
                }

    #ifdef SLIC3R_TREESUPPORTS_PROGRESS
            progress_total += data_size_inverse * TREE_PROGRESS_AREA_CALC;
            Progress::messageProgress(Progress::Stage::SUPPORT, progress_total * m_progress_multiplier + m_progress_offset, TREE_PROGRESS_TOTAL);
    #endif
            throw_on_cancel();
        }

    BOOST_LOG_TRIVIAL(info) << "Time spent with creating influence areas' subtasks: Increasing areas " << dur_inc.count() / 1000000 <<
        " ms merging areas: " << (dur_total - dur_inc).count() / 1000000 << " ms";
}

/*!
 * \brief Sets the result_on_layer for all parents based on the SupportElement supplied.
 *
 * \param elem[in] The SupportElements, which parent's position should be determined.
 */
static void set_points_on_areas(const SupportElement &elem, SupportElements *layer_above)
{
    assert(!elem.state.deleted);
    assert(layer_above != nullptr || elem.parents.empty());

    // Based on the branch center point of the current layer, the point on the next (further up) layer is calculated.
    if (! elem.state.result_on_layer_is_set()) {
        BOOST_LOG_TRIVIAL(warning) << "Uninitialized support element";
        tree_supports_show_error("Uninitialized support element. A branch may be missing.\n"sv, true);
        return;
    }

    if (layer_above)
        for (int32_t next_elem_idx : elem.parents) {
            assert(next_elem_idx >= 0);
            SupportElement &next_elem = (*layer_above)[next_elem_idx];
            assert(! next_elem.state.deleted);
            // if the value was set somewhere else it it kept. This happens when a branch tries not to move after being unable to create a roof.
            if (! next_elem.state.result_on_layer_is_set()) {
                // Move inside has edgecases (see tests) so DONT use Polygons.inside to confirm correct move, Error with distance 0 is <= 1
                // it is not required to check if how far this move moved a point as is can be larger than maximum_movement_distance.
                // While this seems like a problem it may for example occur after merges.
                next_elem.state.result_on_layer = move_inside_if_outside(next_elem.influence_area, elem.state.result_on_layer);
                // do not call recursive because then amount of layers would be restricted by the stack size
            }
            // Mark the parent element as accessed from a valid child element.
            next_elem.state.marked = true;
        }
}

static void set_to_model_contact_simple(SupportElement &elem)
{
    const Point best = move_inside_if_outside(elem.influence_area, elem.state.next_position);
    elem.state.result_on_layer = best;
    BOOST_LOG_TRIVIAL(debug) << "Added NON gracious Support On Model Point (" << best.x() << "," << best.y() << "). The current layer is " << elem.state.layer_idx;
}

/*!
 * \brief Get the best point to connect to the model and set the result_on_layer of the relevant SupportElement accordingly.
 *
 * \param move_bounds[in,out] All currently existing influence areas
 * \param first_elem[in,out] SupportElement that did not have its result_on_layer set meaning that it does not have a child element.
 * \param layer_idx[in] The current layer.
 */
static void set_to_model_contact_to_model_gracious(
    const OrcaTreeModelVolumes          &volumes,
    const OrcaTreeSupportSettings       &config,
    std::vector<SupportElements>    &move_bounds,
    SupportElement                  &first_elem,
    std::function<void()>            throw_on_cancel)
{
    SupportElement *last_successfull_layer = nullptr;

    // check for every layer upwards, up to the point where this influence area was created (either by initial insert or merge) if the branch could be placed on it, and highest up layer index.
    {
        SupportElement *elem = &first_elem;
        for (LayerIndex layer_check = elem->state.layer_idx;
            ! intersection(elem->influence_area, volumes.getPlaceableAreas(support_element_collision_radius(config, elem->state), layer_check, throw_on_cancel)).empty();
            elem = &move_bounds[++ layer_check][elem->parents.front()]) {
            assert(elem->state.layer_idx == layer_check);
            assert(! elem->state.deleted);
            assert(elem->state.to_model_gracious);
            last_successfull_layer = elem;
            if (elem->parents.size() != 1)
                // Reached merge point.
                break;
        }
    }

    // Could not find valid placement, even though it should exist => error handling
    if (last_successfull_layer == nullptr) {
        BOOST_LOG_TRIVIAL(warning) << "No valid placement found for to model gracious element on layer " << first_elem.state.layer_idx;
        tree_supports_show_error("Could not fine valid placement on model! Just placing it down anyway. Could cause floating branches."sv, true);
        first_elem.state.to_model_gracious = false;
        set_to_model_contact_simple(first_elem);
    } else {
        // Found a gracious area above first_elem. Remove all below last_successfull_layer.
        {
            LayerIndex parent_layer_idx = first_elem.state.layer_idx;
            for (SupportElement *elem = &first_elem; elem != last_successfull_layer; elem = &move_bounds[++ parent_layer_idx][elem->parents.front()]) {
                assert(! elem->state.deleted);
                elem->state.deleted = true;
            }
        }
        // Guess a point inside the influence area, in which the branch will be placed in.
        const Point best = move_inside_if_outside(last_successfull_layer->influence_area, last_successfull_layer->state.next_position);
        last_successfull_layer->state.result_on_layer = best;
        BOOST_LOG_TRIVIAL(debug) << "Added gracious Support On Model Point (" << best.x() << "," << best.y() << "). The current layer is " << last_successfull_layer;
    }
}

static void remap_parent_indices(
    SupportElement              &element,
    const std::vector<int32_t>  &parent_map)
{
    if (parent_map.empty())
        return;
    for (int32_t &parent_idx : element.parents)
        parent_idx = parent_map[parent_idx];
}

// Remove elements marked as "deleted", update indices to parents.
static void remove_deleted_elements(std::vector<SupportElements> &move_bounds)
{
    std::vector<int32_t> map_parents;
    std::vector<int32_t> map_current;
    for (LayerIndex layer_idx = LayerIndex(move_bounds.size()) - 1; layer_idx >= 0; -- layer_idx) {
        SupportElements &layer = move_bounds[layer_idx];
        map_current.clear();
        for (int32_t i = 0; i < int32_t(layer.size());) {
            if (! layer[i].state.deleted) {
                // Current element is not deleted. Update its parent indices.
                remap_parent_indices(layer[i], map_parents);
                ++ i;
                continue;
            }

            if (map_current.empty()) {
                // Initialize with identity map.
                map_current.assign(layer.size(), 0);
                std::iota(map_current.begin(), map_current.end(), 0);
            }

            // Delete all "deleted" elements from the end of the layer vector.
            while (i < int32_t(layer.size()) && layer.back().state.deleted) {
                layer.pop_back();
                // Mark as deleted in the map.
                map_current[layer.size()] = -1;
            }
            assert(i == layer.size() || i + 1 < layer.size());
            if (i + 1 >= int32_t(layer.size()))
                continue;

            layer[i] = std::move(layer.back());
            layer.pop_back();
            // Mark the current element as deleted.
            map_current[i] = -1;
            // Mark the moved element as moved to index i.
            map_current[layer.size()] = i;
        }
        std::swap(map_current, map_parents);
    }
}

#ifndef NDEBUG
static void validate_element_connectivity(
    const SupportElement  &element,
    const SupportElements &above)
{
    assert(!element.state.deleted);
    for (int32_t parent_idx : element.parents) {
        const SupportElement &parent = above[parent_idx];
        assert(!parent.state.deleted);
        assert(
            element.state.result_on_layer_is_set() ==
            parent.state.result_on_layer_is_set());
    }
}

static void validate_tree_connectivity(
    const std::vector<SupportElements> &move_bounds,
    bool                                skip_deleted)
{
    for (LayerIndex layer_idx = 0; layer_idx + 1 < LayerIndex(move_bounds.size()); ++layer_idx) {
        const SupportElements &layer = move_bounds[layer_idx];
        const SupportElements &above = move_bounds[layer_idx + 1];
        for (const SupportElement &element : layer) {
            if (skip_deleted && element.state.deleted)
                continue;
            validate_element_connectivity(element, above);
        }
    }
}
#endif // NDEBUG

/*!
 * \brief Set the result_on_layer point for all influence areas
 *
 * \param move_bounds[in,out] All currently existing influence areas
 */
static void create_nodes_from_area(
    const OrcaTreeModelVolumes       &volumes,
    const OrcaTreeSupportSettings    &config,
    std::vector<SupportElements> &move_bounds,
    std::function<void()>         throw_on_cancel)
{
    // Initialize points on layer 0, with a "random" point in the influence area.
    // Point is chosen based on an inaccurate estimate where the branches will split into two, but every point inside the influence area would produce a valid result.
    {
        SupportElements *layer_above = move_bounds.size() > 1 ? &move_bounds[1] : nullptr;
        if (layer_above) {
            for (SupportElement &elem : *layer_above)
                elem.state.marked = false;
        }
        for (SupportElement &init : move_bounds.front()) {
            init.state.result_on_layer = move_inside_if_outside(init.influence_area, init.state.next_position);
            // Also set the parent nodes, as these will be required for the first iteration of the loop below and mark the parent nodes.
            set_points_on_areas(init, layer_above);
        }
    }

    throw_on_cancel();

    auto initialize_node = [&](
        SupportElement &element,
        LayerIndex layer_idx)
    {
        if (element.state.result_on_layer_is_set())
            return;

        const bool cannot_reach_model =
            element.state.distance_to_top < config.min_dtt_to_model && !element.state.supports_roof;
        if (element.state.to_buildplate || cannot_reach_model) {
            if (element.state.to_buildplate) {
                BOOST_LOG_TRIVIAL(warning)
                    << "Uninitialized Influence area targeting "
                    << element.state.target_position.x() << ","
                    << element.state.target_position.y() << ") "
                    << "at target_height: " << element.state.target_height
                    << " layer: " << layer_idx;
                tree_supports_show_error(
                    "Uninitialized support element! A branch could be missing or exist partially."sv,
                    true);
            }
            // Parents have lower distance-to-top and are invalidated when this element is removed.
            element.state.deleted = true;
            return;
        }

        if (element.state.to_model_gracious) {
            set_to_model_contact_to_model_gracious(
                volumes,
                config,
                move_bounds,
                element,
                throw_on_cancel);
        } else {
            set_to_model_contact_simple(element);
        }
    };

    for (LayerIndex layer_idx = 1; layer_idx < LayerIndex(move_bounds.size()); ++ layer_idx) {
        const size_t layer_index       = checked_layer_index(layer_idx);
        const size_t layer_above_index = next_layer_index(layer_idx);
        auto        &layer             = move_bounds[layer_index];
        auto        *layer_above       = layer_above_index < move_bounds.size() ? &move_bounds[layer_above_index] : nullptr;
        if (layer_above)
            for (SupportElement &elem : *layer_above)
                elem.state.marked = false;
        for (SupportElement &elem : layer) {
            assert(! elem.state.deleted);
            assert(elem.state.layer_idx == layer_idx);
            initialize_node(elem, layer_idx);
            if (!elem.state.deleted && !elem.state.marked && elem.state.target_height == layer_idx)
                // Just a tip surface with no supporting element.
                elem.state.deleted = true;

            if (! elem.state.deleted) {
                // Element is valid; set points in the layer above and mark its parents.
                set_points_on_areas(elem, layer_above);
                continue;
            }

            for (int32_t parent_idx : elem.parents) {
                // Invalidate parents when a partially generated roof branch is removed.
                (*layer_above)[parent_idx].state.result_on_layer_reset();
            }
        }
        throw_on_cancel();
    }

#ifndef NDEBUG
    validate_tree_connectivity(move_bounds, true);
#endif // NDEBUG

    remove_deleted_elements(move_bounds);

#ifndef NDEBUG
    validate_tree_connectivity(move_bounds, false);
#endif // NDEBUG
}

template<bool flip_normals>
void triangulate_fan(indexed_triangle_set &its, int ifan, int ibegin, int iend)
{
    // at least 3 vertices, increasing order.
    assert(ibegin + 3 <= iend);  // need at least 3 vertices (a triangle)
    assert(ibegin >= 0 && iend <= its.vertices.size());
    assert(ifan >= 0 && ifan < its.vertices.size());
    int num_faces = iend - ibegin;
    its.indices.reserve(checked_triangle_index_reserve_size(
        its.indices.size(), its.indices.max_size(), static_cast<size_t>(num_faces)));
    for (int v = ibegin, u = iend - 1; v < iend; u = v ++) {
        if (flip_normals)
            its.indices.push_back({ ifan, u, v });
        else
            its.indices.push_back({ ifan, v, u });
    }
}

static void triangulate_strip(indexed_triangle_set &its, int ibegin1, int iend1, int ibegin2, int iend2)
{
    // at least 3 vertices, increasing order.
    assert(ibegin1 + 3 <= iend1);  // need at least 3 vertices (a triangle)
    assert(ibegin1 >= 0 && iend1 <= its.vertices.size());
    assert(ibegin2 + 3 <= iend2);  // need at least 3 vertices (a triangle)
    assert(ibegin2 >= 0 && iend2 <= its.vertices.size());
    int n1 = iend1 - ibegin1;
    int n2 = iend2 - ibegin2;
    its.indices.reserve(checked_triangle_index_reserve_size(
        its.indices.size(), its.indices.max_size(), static_cast<size_t>(n1), static_cast<size_t>(n2)));

    // For the first vertex of 1st strip, find the closest vertex on the 2nd strip.
    int istart2 = ibegin2;
    {
        const Vec3f &p1    = its.vertices[ibegin1];
        auto         d2min = std::numeric_limits<float>::max();
        for (int i = ibegin2; i < iend2; ++ i) {
            const Vec3f &p2 = its.vertices[i];
            const float d2  = (p2 - p1).squaredNorm();
            if (d2 < d2min) {
                d2min = d2;
                istart2 = i;
            }
        }
    }

    // Now triangulate the strip zig-zag fashion taking always the shortest connection if possible.
    for (int u = ibegin1, v = istart2; n1 > 0 || n2 > 0;) {
        bool take_first;
        int u2 = u;
        int v2 = v;
        auto update_u2 = [&u2, u, ibegin1, iend1]() {
            u2 = u;
            if (++ u2 == iend1)
                u2 = ibegin1;
        };
        auto update_v2 = [&v2, v, ibegin2, iend2]() {
            v2 = v;
            if (++ v2 == iend2)
                v2 = ibegin2;
        };
        if (n1 == 0) {
            take_first = false;
            update_v2();
        } else if (n2 == 0) {
            take_first = true;
            update_u2();
        } else {
            update_u2();
            update_v2();
            float l1 = (its.vertices[u2] - its.vertices[v]).squaredNorm();
            float l2 = (its.vertices[v2] - its.vertices[u]).squaredNorm();
            take_first = l1 < l2;
        }
        if (take_first) {
            its.indices.push_back({ u, u2, v });
            -- n1;
            u = u2;
        } else {
            its.indices.push_back({ u, v2, v });
            -- n2;
            v = v2;
        }
    }
}

// Discretize 3D circle, append to output vector, return ranges of indices of the points added.
static std::pair<int, int> discretize_circle(const Vec3f &center, const Vec3f &normal, const float radius, const float eps, std::vector<Vec3f> &pts)
{
    // Calculate discretization step and number of steps.
    float angle_step = 2. * acos(1. - eps / radius);  // full arc angle = 2 * acos(...)
    auto  nsteps     = int(ceil(2 * M_PI / angle_step));  // number of steps around a full circle (2*pi)
    angle_step = 2 * M_PI / nsteps;  // even angle step over 2*pi

    // Prepare coordinate system for the circle plane.
    Vec3f x = normal.cross(Vec3f(0.f, -1.f, 0.f)).normalized();
    Vec3f y = normal.cross(x).normalized();
    assert(std::abs(x.cross(y).dot(normal) - 1.f) < EPSILON);

    // Discretize the circle.
    int begin = int(pts.size());
    pts.reserve(pts.size() + nsteps);
    float angle = 0;
    x *= radius;
    y *= radius;
    for (int i = 0; i < nsteps; ++ i) {
        pts.emplace_back(center + x * cos(angle) + y * sin(angle));
        angle += angle_step;
    }
    return { begin, int(pts.size()) };
}

// Returns Z span of the generated mesh.
static std::pair<float, float> extrude_branch(
    const std::vector<const SupportElement*>&path,
    const OrcaTreeSupportSettings               &config,
    const SlicingParameters                 &slicing_params,
    const std::vector<SupportElements>      &move_bounds,
    indexed_triangle_set                    &result)
{
    Vec3d p1, p2, p3;
    Vec3d v1, v2;
    Vec3d nprev;
    Vec3d ncurrent;
    assert(path.size() >= 2);  // need at least 2 points
    static constexpr const float eps = 0.015f;
    std::pair<int, int> prev_strip;
    float zmin = 0;
    float zmax = 0;

    for (size_t ipath = 1; ipath < path.size(); ++ ipath) {
        const SupportElement &prev    = *path[ipath - 1];
        const SupportElement &current = *path[ipath];
        assert(prev.state.layer_idx + 1 == current.state.layer_idx);
        p1 = to_3d(unscaled<double>(prev   .state.result_on_layer), layer_z(slicing_params, config, prev   .state.layer_idx));
        p2 = to_3d(unscaled<double>(current.state.result_on_layer), layer_z(slicing_params, config, current.state.layer_idx));
        v1 = (p2 - p1).normalized();
        if (ipath == 1) {
            nprev = v1;
            // Extrude the bottom half sphere.
            float radius     = unscaled<float>(support_element_radius(config, prev));
            float angle_step = 2. * acos(1. - eps / radius);  // full arc angle = 2 * acos(...)
            auto  nsteps     = int(ceil(M_PI / (2. * angle_step)));  // half-circle step count
            angle_step       = M_PI / (2. * nsteps);  // half-circle step size
            int   ifan       = int(result.vertices.size());
            result.vertices.emplace_back((p1 - nprev * radius).cast<float>());
            zmin = result.vertices.back().z();
            float angle = angle_step;
            for (int i = 1; i < nsteps; ++ i, angle += angle_step) {
                std::pair<int, int> strip = discretize_circle((p1 - nprev * radius * cos(angle)).cast<float>(), nprev.cast<float>(), radius * sin(angle), eps, result.vertices);
                if (i == 1)
                    triangulate_fan<false>(result, ifan, strip.first, strip.second);
                else
                    triangulate_strip(result, prev_strip.first, prev_strip.second, strip.first, strip.second);
//                sprintf(fname, "d:\\temp\\meshes\\tree-partial-%d.obj", ++ irun);
//                its_write_obj(result, fname);
                prev_strip = strip;
            }
        }
        if (ipath + 1 == path.size()) {
            // End of the tube.
            ncurrent = v1;
            // Extrude the top half sphere.
            float radius = unscaled<float>(support_element_radius(config, current));
            float angle_step = 2. * acos(1. - eps / radius);  // full arc angle = 2 * acos(...)
            auto  nsteps = int(ceil(M_PI / (2. * angle_step)));  // half-circle step count
            angle_step = M_PI / (2. * nsteps);  // half-circle step size
            auto angle = float(M_PI / 2.);  // 90 deg (pi/2)
            for (int i = 0; i < nsteps; ++ i, angle -= angle_step) {
                std::pair<int, int> strip = discretize_circle((p2 + ncurrent * radius * cos(angle)).cast<float>(), ncurrent.cast<float>(), radius * sin(angle), eps, result.vertices);
                triangulate_strip(result, prev_strip.first, prev_strip.second, strip.first, strip.second);
//                sprintf(fname, "d:\\temp\\meshes\\tree-partial-%d.obj", ++ irun);
//                its_write_obj(result, fname);
                prev_strip = strip;
            }
            int ifan = int(result.vertices.size());
            result.vertices.emplace_back((p2 + ncurrent * radius).cast<float>());
            zmax = result.vertices.back().z();
            triangulate_fan<true>(result, ifan, prev_strip.first, prev_strip.second);
//            sprintf(fname, "d:\\temp\\meshes\\tree-partial-%d.obj", ++ irun);
//            its_write_obj(result, fname);
        } else {
            const SupportElement &next = *path[ipath + 1];
            assert(current.state.layer_idx + 1 == next.state.layer_idx);
            p3 = to_3d(unscaled<double>(next.state.result_on_layer), layer_z(slicing_params, config, next.state.layer_idx));
            v2 = (p3 - p2).normalized();
            ncurrent = (v1 + v2).normalized();
            float radius = unscaled<float>(support_element_radius(config, current));
            std::pair<int, int> strip = discretize_circle(p2.cast<float>(), ncurrent.cast<float>(), radius, eps, result.vertices);
            triangulate_strip(result, prev_strip.first, prev_strip.second, strip.first, strip.second);
            prev_strip = strip;
//            sprintf(fname, "d:\\temp\\meshes\\tree-partial-%d.obj", ++irun);
//            its_write_obj(result, fname);
        }
#if 0
        if (circles_intersect(p1, nprev, support_element_radius(settings, prev), p2, ncurrent, support_element_radius(settings, current))) {
            // Cannot connect previous and current slice using a simple zig-zag triangulation,
            // because the two circles intersect.

        } else {
            // Continue with chaining.

        }
#endif
    }

    return std::make_pair(zmin, zmax);
}


#ifndef TREE_SUPPORT_ORGANIC_NUDGE_LEGACY

// New version using per layer AABB trees of lines for nudging spheres away from an object.
static void organic_smooth_branches_avoid_collisions(
    const PrintObject                                   &print_object,
    const OrcaTreeModelVolumes                              &volumes,
    const OrcaTreeSupportSettings                           &config,
    std::vector<SupportElements>                        &move_bounds,
    const std::vector<std::pair<SupportElement*, int>>  &elements_with_link_down,
    const std::vector<size_t>                           &linear_data_layers,
    std::function<void()>                                throw_on_cancel)
{
    struct LayerCollisionCache {
        coord_t          min_element_radius{ std::numeric_limits<coord_t>::max() };
        bool             min_element_radius_known() const { return this->min_element_radius != std::numeric_limits<coord_t>::max(); }
        coord_t          collision_radius{ 0 };
        std::vector<Linef> lines;
        AABBTreeIndirect::Tree<2, double> aabbtree_lines;  // 2D AABB tree
        bool             empty() const { return this->lines.empty(); }
    };
    std::vector<LayerCollisionCache> layer_collision_cache;
    layer_collision_cache.reserve(1024);
    const SlicingParameters &slicing_params = print_object.slicing_parameters();
    for (const std::pair<SupportElement*, int>& element : elements_with_link_down) {
        LayerIndex layer_idx = element.first->state.layer_idx;
        if (size_t num_layers = layer_idx + 1; num_layers > layer_collision_cache.size()) {
            if (num_layers > layer_collision_cache.capacity())
                reserve_power_of_2(layer_collision_cache, num_layers);
            layer_collision_cache.resize(num_layers, {});
        }
        auto& l = layer_collision_cache[layer_idx];
        l.min_element_radius = std::min(l.min_element_radius, support_element_radius(config, *element.first));
    }

    throw_on_cancel();

    for (LayerIndex layer_idx = 0; layer_idx < LayerIndex(layer_collision_cache.size()); ++layer_idx)
        if (LayerCollisionCache& l = layer_collision_cache[layer_idx]; !l.min_element_radius_known())
            l.min_element_radius = 0;
        else {
            std::optional<std::pair<coord_t, std::reference_wrapper<const Polygons>>> res =
                volumes.get_collision_lower_bound_area(layer_idx, l.min_element_radius);
            assert(res.has_value());
            l.collision_radius = res->first;
            Lines alines = to_lines(res->second.get());
            l.lines.reserve(alines.size());
            for (const Line &line : alines)
                l.lines.push_back({ unscaled<double>(line.a), unscaled<double>(line.b) });
            l.aabbtree_lines = AABBTreeLines::build_aabb_tree_over_indexed_lines(l.lines);
            throw_on_cancel();
        }

    struct CollisionSphere {
        const SupportElement& element;
        int                   element_below_id;
        const bool            locked;
        float                 radius;
        // Current position, when nudged away from the collision.
        Vec3f                 position;
        // Previous position, for Laplacian smoothing.
        Vec3f                 prev_position;
        //
        Vec3f                 last_collision;
        double                last_collision_depth;
        // Minimum Z for which the sphere collision will be evaluated.
        // Limited by the minimum sloping angle and by the bottom of the tree.
        float                 min_z{ -std::numeric_limits<float>::max() };
        // Maximum Z for which the sphere collision will be evaluated.
        // Limited by the minimum sloping angle and by the tip of the current branch.
        float                 max_z{ std::numeric_limits<float>::max() };
        uint32_t              layer_begin;
        uint32_t              layer_end;
    };

    std::vector<CollisionSphere> collision_spheres;
    collision_spheres.reserve(elements_with_link_down.size());
    for (const std::pair<SupportElement*, int> &element_with_link : elements_with_link_down) {
        const SupportElement &element   = *element_with_link.first;
        const int             link_down = element_with_link.second;
        collision_spheres.push_back({
            element,
            link_down,
            // locked
            element.parents.empty() || (link_down == -1 && element.state.layer_idx > 0),
            unscaled<float>(support_element_radius(config, element)),
            // 3D position
            to_3d(unscaled<float>(element.state.result_on_layer), float(layer_z(slicing_params, config, element.state.layer_idx)))
        });
        // Update min_z coordinate to min_z of the tree below.
        CollisionSphere &collision_sphere = collision_spheres.back();
        if (link_down != -1) {
            const size_t offset_below = linear_data_layers[previous_layer_index(element.state.layer_idx)];
            collision_sphere.min_z = collision_spheres[offset_below + link_down].min_z;
        } else
            collision_sphere.min_z = collision_sphere.position.z();
    }
    // Update max_z by propagating max_z from the tips of the branches.
    for (int collision_sphere_id = int(collision_spheres.size()) - 1; collision_sphere_id >= 0; -- collision_sphere_id) {
        CollisionSphere &collision_sphere = collision_spheres[collision_sphere_id];
        if (collision_sphere.element.parents.empty())
            // Tip
            collision_sphere.max_z = collision_sphere.position.z();
        else {
            // Below tip
            const size_t offset_above = linear_data_layers[next_layer_index(collision_sphere.element.state.layer_idx)];
            for (auto iparent : collision_sphere.element.parents) {
                float parent_z = collision_spheres[offset_above + iparent].max_z;
//                    collision_sphere.max_z = collision_sphere.max_z == std::numeric_limits<float>::max() ? parent_z : std::max(collision_sphere.max_z, parent_z);
                collision_sphere.max_z = std::min(collision_sphere.max_z, parent_z);
            }
        }
    }
    // Update min_z / max_z to limit the search Z span of a given sphere for collision detection.
    for (CollisionSphere &collision_sphere : collision_spheres) {
        // Possible optimization: limit the collision span by the tree slope.
        collision_sphere.min_z = std::max(collision_sphere.min_z, collision_sphere.position.z() - collision_sphere.radius);
        collision_sphere.max_z = std::min(collision_sphere.max_z, collision_sphere.position.z() + collision_sphere.radius);
        collision_sphere.layer_begin = std::min(collision_sphere.element.state.layer_idx, layer_idx_ceil(slicing_params, config, collision_sphere.min_z));
        assert(collision_sphere.layer_begin < layer_collision_cache.size());
        collision_sphere.layer_end   = std::min(LayerIndex(layer_collision_cache.size()), std::max(collision_sphere.element.state.layer_idx, layer_idx_floor(slicing_params, config, collision_sphere.max_z)) + 1);
    }

    throw_on_cancel();

    static constexpr const double collision_extra_gap = 0.1;
    static constexpr const double max_nudge_collision_avoidance = 0.5;
    static constexpr const double max_nudge_smoothing = 0.2;
    static constexpr const size_t num_iter = 100; // 1000;

    auto update_collision_sphere = [&](
        size_t collision_sphere_id,
        std::atomic<size_t> &num_moved)
    {
        CollisionSphere &collision_sphere = collision_spheres[collision_sphere_id];
        if (collision_sphere.locked)
            return;

        // Calculate collision of multiple 2D layers against a collision sphere.
        collision_sphere.last_collision_depth = -std::numeric_limits<double>::max();
        for (uint32_t layer_id = collision_sphere.layer_begin; layer_id != collision_sphere.layer_end; ++layer_id) {
            const double dz =
                (LayerIndex(layer_id) - collision_sphere.element.state.layer_idx) * slicing_params.layer_height;
            const double radius_squared = sqr(collision_sphere.radius) - sqr(dz);
            if (radius_squared <= 0)
                continue;

            const LayerCollisionCache &cache = layer_collision_cache[layer_id];
            if (cache.empty())
                continue;

            size_t hit_idx_out = 0;
            Vec2d hit_point_out = Vec2d::Zero();
            const double distance = sqrt(AABBTreeLines::squared_distance_to_indexed_lines(
                cache.lines,
                cache.aabbtree_lines,
                Vec2d(to_2d(collision_sphere.position).cast<double>()),
                hit_idx_out,
                hit_point_out,
                radius_squared));
            if (distance < 0.)
                continue;

            const double collision_depth = sqrt(radius_squared) - distance;
            if (collision_depth <= collision_sphere.last_collision_depth)
                continue;

            collision_sphere.last_collision_depth = collision_depth;
            collision_sphere.last_collision =
                to_3d(hit_point_out.cast<float>(), float(layer_z(slicing_params, config, layer_id)));
        }

        if (collision_sphere.last_collision_depth > 0) {
            // Collision detected to be removed.
            if (collision_sphere.last_collision_depth > EPSILON)
                ++num_moved;
            const double nudge_dist = std::min(
                std::max(0., collision_sphere.last_collision_depth + collision_extra_gap),
                max_nudge_collision_avoidance);
            const Vec2d nudge_vector =
                (to_2d(collision_sphere.position) - to_2d(collision_sphere.last_collision))
                    .cast<double>()
                    .normalized() * nudge_dist;
            collision_sphere.position.head<2>() += (nudge_vector * nudge_dist).cast<float>(); // xy components (first 2)
        }

        // Laplacian smoothing.
        Vec2d avg{ 0, 0 };
        const size_t offset_above =
            linear_data_layers[next_layer_index(collision_sphere.element.state.layer_idx)];
        double weight = 0.;
        for (auto parent_id : collision_sphere.element.parents) {
            const double parent_weight = collision_sphere.radius;
            avg += parent_weight *
                   to_2d(collision_spheres[offset_above + parent_id].prev_position.cast<double>());
            weight += parent_weight;
        }
        if (collision_sphere.element_below_id != -1) {
            const size_t offset_below =
                linear_data_layers[previous_layer_index(collision_sphere.element.state.layer_idx)];
            const double child_weight = weight;
            avg += child_weight *
                   to_2d(collision_spheres[offset_below + collision_sphere.element_below_id]
                             .prev_position.cast<double>());
            weight += child_weight;
        }

        avg /= weight;
        static constexpr double smoothing_factor = 0.5;
        const Vec2d old_pos = to_2d(collision_sphere.position).cast<double>();
        const Vec2d new_pos = (1. - smoothing_factor) * old_pos + smoothing_factor * avg;
        const Vec2d shift = new_pos - old_pos;
        const double nudge_dist_max = shift.norm();
        const double nudge_dist =
            std::min(std::max(0., nudge_dist_max), max_nudge_smoothing);
        collision_sphere.position.head<2>() += (shift.normalized() * nudge_dist).cast<float>(); // xy components (first 2)

        throw_on_cancel();
    };

    for (size_t iter = 0; iter < num_iter; ++ iter) {
        // Back up prev position before Laplacian smoothing.
        for (CollisionSphere &collision_sphere : collision_spheres)
            collision_sphere.prev_position = collision_sphere.position;
        std::atomic<size_t> num_moved{ 0 };
        tbb::parallel_for(
            tbb::blocked_range<size_t>(0, collision_spheres.size()),
            [&update_collision_sphere, &num_moved](const tbb::blocked_range<size_t> range) {
                for (size_t collision_sphere_id = range.begin(); collision_sphere_id < range.end(); ++collision_sphere_id)
                    update_collision_sphere(collision_sphere_id, num_moved);
            });
        if (num_moved == 0)
            break;
    }

    for (size_t i = 0; i < collision_spheres.size(); ++ i)
        elements_with_link_down[i].first->state.result_on_layer = scaled<coord_t>(to_2d(collision_spheres[i].position));
}
#else // TREE_SUPPORT_ORGANIC_NUDGE_LEGACY
// Old version using OpenVDB, works but it is extremely slow for complex meshes.
static void organic_smooth_branches_avoid_collisions(
    const PrintObject                                   &print_object,
    const OrcaTreeModelVolumes                              &volumes,
    const OrcaTreeSupportSettings                           &config,
    std::vector<SupportElements>                        &move_bounds,
    const std::vector<std::pair<SupportElement*, int>>  &elements_with_link_down,
    const std::vector<size_t>                           &linear_data_layers,
    std::function<void()>                                throw_on_cancel)
{
    TriangleMesh mesh = print_object.model_object()->raw_mesh();
    mesh.transform(print_object.trafo_centered());
    double scale = 10.;  // 10x scale for the debug SVG
    openvdb::FloatGrid::Ptr grid = mesh_to_grid(mesh.its, openvdb::math::Transform{}, scale, 0., 0.);
    std::unique_ptr<openvdb::tools::ClosestSurfacePoint<openvdb::FloatGrid>> closest_surface_point = openvdb::tools::ClosestSurfacePoint<openvdb::FloatGrid>::create(*grid);
    std::vector<openvdb::Vec3R> pts, prev, projections;
    std::vector<float> distances;
    for (const std::pair<SupportElement*, int>& element : elements_with_link_down) {
        Vec3d pt = to_3d(unscaled<double>(element.first->state.result_on_layer), layer_z(print_object.slicing_parameters(), config, element.first->state.layer_idx)) * scale;
        pts.push_back({ pt.x(), pt.y(), pt.z() });
    }

    const double collision_extra_gap = 1. * scale;
    const double max_nudge_collision_avoidance = 2. * scale;  // twice the scale
    const double max_nudge_smoothing = 1. * scale;

    auto nudge_collision = [&](size_t i, const SupportElement &element) {
        // Nudge the circle center away from the collision.
        const Vec3d v{ projections[i].x() - pts[i].x(), projections[i].y() - pts[i].y(), projections[i].z() - pts[i].z() };
        const double depth = v.norm();
        assert(std::abs(distances[i] - depth) < EPSILON);
        const double radius = unscaled<double>(support_element_radius(config, element)) * scale;
        if (depth >= radius)
            return false;

        const double dxy = sqrt(sqr(radius) - sqr(v.z()));
        const double nudge_dist_max = dxy - std::hypot(v.x(), v.y())
            // Note: collision_extra_gap is an arbitrary 1mm safety gap.
            + collision_extra_gap;
        // Shift by maximum 2mm.
        const double nudge_dist = std::min(std::max(0., nudge_dist_max), max_nudge_collision_avoidance);
        const Vec2d nudge_v = to_2d(v).normalized() * (- nudge_dist);
        pts[i].x() += nudge_v.x();
        pts[i].y() += nudge_v.y();
        return true;
    };

    auto smooth_point = [&](size_t i, const SupportElement &element, int below) {
        Vec2d avg{ 0, 0 };
        const SupportElements &above = move_bounds[element.state.layer_idx + 1];
        const size_t           offset_above = linear_data_layers[element.state.layer_idx + 1];
        double weight = 0.;
        for (auto iparent : element.parents) {
            const double w = support_element_radius(config, above[iparent]);
            avg.x() += w * prev[offset_above + iparent].x();
            avg.y() += w * prev[offset_above + iparent].y();
            weight += w;
        }
        if (below != -1) {
            const size_t offset_below = linear_data_layers[previous_layer_index(element.state.layer_idx)];
            const double w = weight; //  support_element_radius(config, move_bounds[element.state.layer_idx - 1][below]);
            avg.x() += w * prev[offset_below + below].x();
            avg.y() += w * prev[offset_below + below].y();
            weight += w;
        }
        avg /= weight;
        static constexpr const double smoothing_factor = 0.5;
        const Vec2d old_pos{ pts[i].x(), pts[i].y() };
        const Vec2d new_pos = (1. - smoothing_factor) * old_pos + smoothing_factor * avg;
        const Vec2d shift = new_pos - old_pos;
        const double nudge_dist_max = shift.norm();
        // Shift by maximum 1mm, less than the collision avoidance factor.
        const double nudge_dist = std::min(std::max(0., nudge_dist_max), max_nudge_smoothing);
        const Vec2d nudge_v = shift.normalized() * nudge_dist;
        pts[i].x() += nudge_v.x();
        pts[i].y() += nudge_v.y();
    };

    static constexpr const size_t num_iter = 100; // 1000;
    for (size_t iter = 0; iter < num_iter; ++ iter) {
        prev = pts;
        projections = pts;
        distances.assign(pts.size(), std::numeric_limits<float>::max());
        closest_surface_point->searchAndReplace(projections, distances);
        size_t num_moved = 0;
        for (size_t i = 0; i < projections.size(); ++ i) {
            const SupportElement &element = *elements_with_link_down[i].first;
            const int            below    = elements_with_link_down[i].second;
            const bool           locked   = (below == -1 && element.state.layer_idx > 0) || element.state.locked();
            if (! locked && pts[i] != projections[i] && nudge_collision(i, element))
                ++ num_moved;
            // Laplacian smoothing
            if (! locked && ! element.parents.empty())
                smooth_point(i, element, below);
        }
        if (num_moved == 0)
            break;
    }

    for (size_t i = 0; i < projections.size(); ++ i) {
        elements_with_link_down[i].first->state.result_on_layer.x() = scaled<coord_t>(pts[i].x()) / scale;
        elements_with_link_down[i].first->state.result_on_layer.y() = scaled<coord_t>(pts[i].y()) / scale;
    }
}
#endif // TREE_SUPPORT_ORGANIC_NUDGE_LEGACY

struct GeneratedTreeSupportLayers
{
    SupportGeneratorLayerStorage storage;
    SupportGeneratorLayersPtr    top_contacts;
    SupportGeneratorLayersPtr    bottom_contacts;
    SupportGeneratorLayersPtr    interface_layers;
    SupportGeneratorLayersPtr    base_interface_layers;
    SupportGeneratorLayersPtr    intermediate_layers;

    GeneratedTreeSupportLayers(
        size_t num_support_layers,
        const SupportParameters &support_params,
        bool has_raft)
        : intermediate_layers(num_support_layers, nullptr)
    {
        if (support_params.has_top_contacts || has_raft)
            top_contacts.assign(num_support_layers, nullptr);
        if (support_params.has_bottom_contacts)
            bottom_contacts.assign(num_support_layers, nullptr);
        if (support_params.has_interfaces() || has_raft)
            interface_layers.assign(num_support_layers, nullptr);
        if (support_params.has_base_interfaces() || has_raft)
            base_interface_layers.assign(num_support_layers, nullptr);
    }

    void remove_undefined()
    {
        auto remove_nulls = [](SupportGeneratorLayersPtr &layers) {
            layers.erase(
                std::remove(layers.begin(), layers.end(), nullptr),
                layers.end());
        };

        remove_nulls(bottom_contacts);
        remove_nulls(top_contacts);
        remove_nulls(interface_layers);
        remove_nulls(base_interface_layers);
        remove_nulls(intermediate_layers);
    }
};

struct TreeSupportGenerationContext
{
    Print                                          &print;
    PrintObject                                    &print_object;
    const std::vector<size_t>                      &mesh_ids;
    OrcaTreeModelVolumes                           &volumes;
    const OrcaTreeSupportSettings                  &config;
    const std::vector<Polygons>                    &overhangs;
    size_t                                          num_support_layers;
    InterfacePlacer                                &interface_placer;
    const SupportParameters                        &support_params;
    GeneratedTreeSupportLayers                     &layers;
    std::chrono::high_resolution_clock::time_point  started_at;
    const std::function<void()>                    &throw_on_cancel;
};

#ifdef TREESUPPORT_DEBUG_SVG
static void export_initial_support_area_layer(
    const SupportElements        &layer,
    size_t                        layer_idx,
    OrcaTreeModelVolumes         &volumes,
    const OrcaTreeSupportSettings &config)
{
    if (layer.empty())
        return;

    Polygons polys;
    for (const SupportElement &area : layer)
        append(polys, area.influence_area);
    const SupportElement &first = layer.front();
    SVG::export_expolygons(
        debug_out_path("treesupport-initial_areas-%d.svg", layer_idx),
        { { { union_ex(volumes.getWallRestriction(
                    support_element_collision_radius(config, first.state),
                    layer_idx,
                    first.state.use_min_xy_dist)) },
            { "wall_restricrictions", "gray", 0.5f } },
          { { union_ex(polys) },
            { "parent", "red", "black", "", scaled<coord_t>(0.1f), 0.5f } } });
}

static void export_initial_support_areas(
    const std::vector<SupportElements> &move_bounds,
    OrcaTreeModelVolumes               &volumes,
    const OrcaTreeSupportSettings      &config)
{
    for (size_t layer_idx = 0; layer_idx < move_bounds.size(); ++layer_idx)
        export_initial_support_area_layer(move_bounds[layer_idx], layer_idx, volumes, config);
}
#endif // TREESUPPORT_DEBUG_SVG

static void generate_tree_support_layers(TreeSupportGenerationContext &context)
{
    auto t_precalc = std::chrono::high_resolution_clock::now();

    // This is the area where support may be placed. Path creation calculates it,
    // then branch drawing reuses it.
    std::vector<SupportElements> move_bounds(context.num_support_layers);
    for (size_t mesh_idx : context.mesh_ids) {
        generate_initial_areas(
            *context.print.get_object(mesh_idx),
            context.volumes,
            context.config,
            context.overhangs,
            move_bounds,
            context.interface_placer,
            context.throw_on_cancel);
    }
    auto t_gen = std::chrono::high_resolution_clock::now();

#ifdef TREESUPPORT_DEBUG_SVG
    export_initial_support_areas(move_bounds, context.volumes, context.config);
#endif // TREESUPPORT_DEBUG_SVG

    // Propagate influence areas downwards. This is inherently serial.
    context.print.set_status(60, _L("Generating support"));
    create_layer_pathing(context.volumes, context.config, move_bounds, context.throw_on_cancel);
    auto t_path = std::chrono::high_resolution_clock::now();

    create_nodes_from_area(context.volumes, context.config, move_bounds, context.throw_on_cancel);
    auto t_place = std::chrono::high_resolution_clock::now();

    organic_draw_branches(
        context.print_object,
        context.volumes,
        context.config,
        move_bounds,
        context.layers.bottom_contacts,
        context.layers.top_contacts,
        context.interface_placer,
        context.layers.intermediate_layers,
        context.layers.storage,
        context.throw_on_cancel);

    context.layers.remove_undefined();
    std::tie(context.layers.interface_layers, context.layers.base_interface_layers) =
        generate_interface_layers(
            context.print_object.config(),
            context.support_params,
            context.layers.bottom_contacts,
            context.layers.top_contacts,
            context.layers.interface_layers,
            context.layers.base_interface_layers,
            context.layers.intermediate_layers,
            context.layers.storage);

    auto t_draw = std::chrono::high_resolution_clock::now();
    auto dur_pre_gen = MICROSECONDS_TO_MS * std::chrono::duration_cast<std::chrono::microseconds>(t_precalc - context.started_at).count();
    auto dur_gen = MICROSECONDS_TO_MS * std::chrono::duration_cast<std::chrono::microseconds>(t_gen - t_precalc).count();
    auto dur_path = MICROSECONDS_TO_MS * std::chrono::duration_cast<std::chrono::microseconds>(t_path - t_gen).count();
    auto dur_place = MICROSECONDS_TO_MS * std::chrono::duration_cast<std::chrono::microseconds>(t_place - t_path).count();
    auto dur_draw = MICROSECONDS_TO_MS * std::chrono::duration_cast<std::chrono::microseconds>(t_draw - t_place).count();
    auto dur_total = MICROSECONDS_TO_MS * std::chrono::duration_cast<std::chrono::microseconds>(t_draw - context.started_at).count();
    BOOST_LOG_TRIVIAL(info) <<
        "Total time used creating Tree support for the currently grouped meshes: " << dur_total << " ms. "
        "Different subtasks:\nCalculating Avoidance: " << dur_pre_gen << " ms "
        "Creating inital influence areas: " << dur_gen << " ms "
        "Influence area creation: " << dur_path << "ms "
        "Placement of Points in InfluenceAreas: " << dur_place << "ms "
        "Drawing result as support " << dur_draw << " ms";
}

static void emit_tree_support_toolpaths(
    Print &print,
    PrintObject &print_object,
    OrcaTreeModelVolumes &volumes,
    const SupportParameters &support_params,
    GeneratedTreeSupportLayers &layers)
{
    SupportGeneratorLayersPtr raft_layers = generate_raft_base(
        print_object,
        support_params,
        print_object.slicing_parameters(),
        layers.top_contacts,
        layers.interface_layers,
        layers.base_interface_layers,
        layers.intermediate_layers,
        layers.storage);
    SupportGeneratorLayersPtr layers_sorted = generate_support_layers(
        print_object,
        raft_layers,
        layers.bottom_contacts,
        layers.top_contacts,
        layers.intermediate_layers,
        layers.interface_layers,
        layers.base_interface_layers);

    // Avoid generating support outside the bed area. See #4769.
    tbb::parallel_for_each(layers_sorted.begin(), layers_sorted.end(), [&volumes](SupportGeneratorLayer *layer) {
        if (layer)
            layer->polygons = intersection(layer->polygons, Polygons{ volumes.m_bed_area });
    });

    print.set_status(69, _L("Generating support"));
    generate_support_toolpaths(
        print_object.edit_support_layers(),
        print_object.config(),
        support_params,
        print_object.slicing_parameters(),
        raft_layers,
        layers.bottom_contacts,
        layers.top_contacts,
        layers.intermediate_layers,
        layers.interface_layers,
        layers.base_interface_layers);
}

/*!
 * \brief Generate organic support areas and toolpaths for the selected print objects.
 */
static void generate_support_areas(Print &print, const BuildVolume &build_volume, const std::vector<size_t> &print_object_ids, std::function<void()> throw_on_cancel)
{
    // Settings with the indexes of meshes that use these settings.
    std::vector<std::pair<OrcaTreeSupportSettings, std::vector<size_t>>> grouped_meshes = group_meshes(print, print_object_ids);
    if (grouped_meshes.empty())
        return;

    size_t counter = 0;

    // Process every mesh group. These groups can not be processed parallel, as the processing in each group is parallelized, and nested parallelization is disables and slow.
    for (std::pair<OrcaTreeSupportSettings, std::vector<size_t>> &processing : grouped_meshes)
    {
        // process each combination of meshes
        // this struct is used to easy retrieve setting. No other function except those in OrcaTreeModelVolumes and generate_initial_areas() have knowledge of the existence of multiple meshes being processed.
        // Contains config settings to avoid loading them in every function. This was done to improve readability of the code.
        const OrcaTreeSupportSettings &config = processing.first;
        BOOST_LOG_TRIVIAL(info) << "Processing support tree mesh group " << counter + 1 << " of " << grouped_meshes.size() << " containing " << grouped_meshes[counter].second.size() << " meshes.";
        auto t_start = std::chrono::high_resolution_clock::now();
#ifdef SLIC3R_TREESUPPORTS_PROGRESS
        m_progress_multiplier = 1.0 / double(grouped_meshes.size());
        m_progress_offset = counter == 0 ? 0 : TREE_PROGRESS_TOTAL * (double(counter) * m_progress_multiplier);
#endif // SLIC3R_TREESUPPORT_PROGRESS
        PrintObject &print_object = *print.get_object(processing.second.front());
        // Generator for model collision, avoidance and internal guide volumes.
        OrcaTreeModelVolumes volumes{ print_object, build_volume, config.maximum_move_distance, config.maximum_move_distance_slow, processing.second.front(),
#ifdef SLIC3R_TREESUPPORTS_PROGRESS
            m_progress_multiplier, m_progress_offset,
#endif // SLIC3R_TREESUPPORTS_PROGRESS
            /* additional_excluded_areas */{} };

        // Known limitation: overhangs are generated just for the first mesh of the group (groups currently hold a single object).
        assert(processing.second.size() == 1);

std::vector<Polygons> overhangs =
            generate_overhangs(config, *print.get_object(processing.second.front()), throw_on_cancel);
        // ### Precalculate avoidances, collision etc.
        size_t num_support_layers = precalculate(print, overhangs, processing.first, processing.second, volumes, throw_on_cancel);
        bool   has_support = num_support_layers > 0;
        bool   has_raft    = config.raft_layers.size() > 0;
        num_support_layers = std::max(num_support_layers, config.raft_layers.size());

        if (num_support_layers == 0)
            continue;

        SupportParameters support_params(print_object);
        support_params.with_sheath = true;
        // Keep support density for raft generation; tree path generation clears its own density.
        GeneratedTreeSupportLayers layers(num_support_layers, support_params, has_raft);
        InterfacePlacer interface_placer{
            print_object.slicing_parameters(),
            support_params,
            config,
            layers.storage,
            layers.top_contacts,
            layers.interface_layers,
            layers.base_interface_layers };

        if (has_support) {
            TreeSupportGenerationContext context{
                print,
                print_object,
                processing.second,
                volumes,
                config,
                overhangs,
                num_support_layers,
                interface_placer,
                support_params,
                layers,
                t_start,
                throw_on_cancel };
            generate_tree_support_layers(context);
            
        } else if (generate_raft_contact(print_object, config, interface_placer) >= 0) {
            layers.remove_undefined();
        } else
            // No raft.
            continue;

        emit_tree_support_toolpaths(print, print_object, volumes, support_params, layers);

        auto t_end = std::chrono::high_resolution_clock::now();
        BOOST_LOG_TRIVIAL(info) << "Total time of organic tree support: " << MICROSECONDS_TO_MS * std::chrono::duration_cast<std::chrono::microseconds>(t_end - t_start).count() << " ms";

        ++ counter;
    }

}

static void recover_pending_branch_roofs(
    InterfacePlacer        &interface_placer,
    const std::vector<const SupportElement*> &branch_path,
    const LayerIndex        layer_begin,
    std::vector<Polygons>  &slices)
{
    if (! interface_placer.support_parameters.has_top_contacts)
        return;

    for (auto it = branch_path.rbegin(); it != branch_path.rend(); ++ it) {
        const SupportElement &el = **it;
        if (! el.state.has_pending_roof_recovery())
            break;

        const LayerIndex slice_idx = el.state.layer_idx - layer_begin;
        if (slice_idx < 0 || slice_idx >= LayerIndex(slices.size()))
            continue;
        if (slices[size_t(slice_idx)].empty())
            continue;
        if (el.state.roof_recovery_dtt > interface_placer.support_parameters.num_top_interface_layers)
            continue;

        interface_placer.add_roof(std::move(slices[size_t(slice_idx)]), el.state.layer_idx, el.state.roof_recovery_dtt);
    }
}

static int find_child_element_index(
    const std::vector<std::pair<SupportElement*, int>> &map_downwards,
    const SupportElement                              &element)
{
    auto it = std::lower_bound(
        map_downwards.begin(),
        map_downwards.end(),
        &element,
        [](const auto &entry, const SupportElement *candidate) { return entry.first < candidate; });
    if (it == map_downwards.end() || it->first != &element)
        return -1;

    const int child = it->second;
    // Only one link points to a node above from below.
    assert(!(++it != map_downwards.end() && it->first == &element));
    return child;
}

static void collect_linked_support_elements(
    std::vector<SupportElements>                 &move_bounds,
    std::vector<std::pair<SupportElement*, int>> &elements_with_link_down,
    std::vector<size_t>                          &linear_data_layers)
{
    std::vector<std::pair<SupportElement*, int>> map_downwards_old;
    std::vector<std::pair<SupportElement*, int>> map_downwards_new;
    linear_data_layers.emplace_back(0);
    for (LayerIndex layer_idx = 0; layer_idx < LayerIndex(move_bounds.size()); ++ layer_idx) {
        const size_t     layer_above_idx = next_layer_index(layer_idx);
        SupportElements *layer_above     = layer_above_idx < move_bounds.size() ? &move_bounds[layer_above_idx] : nullptr;
        map_downwards_new.clear();
        std::sort(map_downwards_old.begin(), map_downwards_old.end(), [](auto& l, auto& r) { return l.first < r.first;  });
        SupportElements &layer = move_bounds[layer_idx];
        for (size_t elem_idx = 0; elem_idx < layer.size(); ++ elem_idx) {
            SupportElement &elem = layer[elem_idx];
            int child = -1;
            if (layer_idx > 0) {
                child = find_child_element_index(map_downwards_old, elem);
#ifndef NDEBUG
                const SupportElement *pchild = child == -1 ? nullptr : &move_bounds[layer_idx - 1][child];
                assert(pchild ? pchild->state.result_on_layer_is_set() : elem.state.target_height > layer_idx);
#endif // NDEBUG
            }
            for (int32_t parent_idx : elem.parents) {
                SupportElement &parent = (*layer_above)[parent_idx];
                if (parent.state.result_on_layer_is_set())
                    map_downwards_new.emplace_back(&parent, int(elem_idx));
            }

            elements_with_link_down.push_back({ &elem, int(child) });
        }
        std::swap(map_downwards_old, map_downwards_new);
        linear_data_layers.emplace_back(elements_with_link_down.size());
    }
}

// Organic specific: Smooth branches and produce one cumulative mesh to be sliced.
void organic_draw_branches(
    PrintObject                     &print_object,
    OrcaTreeModelVolumes                &volumes, 
    const OrcaTreeSupportSettings       &config,
    std::vector<SupportElements>    &move_bounds,

    // I/O:
    SupportGeneratorLayersPtr       &bottom_contacts,
    SupportGeneratorLayersPtr       &top_contacts,
    InterfacePlacer                 &interface_placer,

    // Output:
    SupportGeneratorLayersPtr       &intermediate_layers,
    SupportGeneratorLayerStorage    &layer_storage,

    std::function<void()>            throw_on_cancel)
{
    // All SupportElements are put into a layer independent storage to improve parallelization.
    std::vector<std::pair<SupportElement*, int>> elements_with_link_down;
    std::vector<size_t>                          linear_data_layers;
    collect_linked_support_elements(move_bounds, elements_with_link_down, linear_data_layers);

    throw_on_cancel();

    organic_smooth_branches_avoid_collisions(print_object, volumes, config, move_bounds, elements_with_link_down, linear_data_layers, throw_on_cancel);

    // Reduce memory footprint. After this point only finalize_interface_and_support_areas() will use volumes and from that only collisions with zero radius will be used.
    volumes.clear_all_but_object_collision();

    // Unmark all nodes.
    for (SupportElements &elements : move_bounds)
        for (SupportElement &element : elements)
            element.state.marked = false;

    // Traverse all nodes, generate tubes.
    // Traversal stack with nodes and their current parent

    struct Branch {
        std::vector<const SupportElement*> path;
        bool                               has_root{ false };
        bool                               has_tip { false };
    };

    struct Slice {
        Polygons polygons;
        Polygons bottom_contacts;
        size_t   num_branches{ 0 };
    };

    struct Tree {
        std::vector<Branch>  branches;

        std::vector<Slice>   slices;
        LayerIndex           first_layer_id{ -1 };
    };

    std::vector<Tree>        trees;

    struct TreeVisitor {
        static SupportElement *follow_branch(
            std::vector<SupportElements> &move_bounds,
            Branch &branch,
            SupportElement &first_parent)
        {
            if (first_parent.parents.size() > 1)
                return &first_parent;
            if (first_parent.parents.empty())
                return nullptr;

            for (SupportElement *parent = &first_parent;;) {
                assert(parent->state.marked);
                SupportElement &next_parent =
                    move_bounds[next_layer_index(parent->state.layer_idx)][parent->parents.front()];
                assert(!next_parent.state.marked);
                assert(branch.path.back()->state.layer_idx + 1 == next_parent.state.layer_idx);
                branch.path.emplace_back(&next_parent);
                if (next_parent.parents.size() > 1)
                    return &next_parent;

                next_parent.state.marked = true;
                if (next_parent.parents.empty())
                    return nullptr;
                parent = &next_parent;
            }
        }

        static void visit_recursive(
            std::vector<SupportElements> &move_bounds,
            SupportElement &start_element,
            Tree &out)
        {
            assert(!start_element.state.marked && !start_element.parents.empty());
            start_element.state.marked = true;
            SupportElements &layer_above =
                move_bounds[next_layer_index(start_element.state.layer_idx)];
            const bool root = out.branches.empty();

            for (size_t parent_idx = 0; parent_idx < start_element.parents.size(); ++parent_idx) {
                Branch branch;
                branch.path.emplace_back(&start_element);
                SupportElement &first_parent = layer_above[start_element.parents[parent_idx]];
                assert(!first_parent.state.marked);
                assert(branch.path.back()->state.layer_idx + 1 == first_parent.state.layer_idx);
                branch.path.emplace_back(&first_parent);
                if (first_parent.parents.size() < 2) // Zero or one parent is not a branch point.
                    first_parent.state.marked = true;

                SupportElement *next_branch = follow_branch(move_bounds, branch, first_parent);
                assert(branch.path.size() >= 2); // A branch includes its start node and first parent.
                assert(next_branch == nullptr || !next_branch->state.marked);
                branch.has_root = root;
                branch.has_tip = next_branch == nullptr;
                out.branches.emplace_back(std::move(branch));
                if (next_branch)
                    visit_recursive(move_bounds, *next_branch, out);
            }
        }
    };

    auto prepare_branch_root_slices = [&](
        const Branch &branch,
        LayerIndex &layer_begin,
        std::vector<Polygons> &slices,
        std::vector<Polygons> &bottom_contacts) -> size_t {
        if (slices.front().empty()) {
            // Some of the initial layers are empty.
            return size_t(std::find_if(
                slices.begin(),
                slices.end(),
                [](const Polygons &slice) { return !slice.empty(); }) - slices.begin());
        }

        if (! branch.has_root) {
            recover_pending_branch_roofs(interface_placer, branch.path, layer_begin, slices);
            return 0;
        }

        const SupportElement &root = *branch.path.front();
        const bool gracious_root = config.support_rests_on_model && root.state.to_model_gracious;
        if (gracious_root && config.settings.support_floor_layers > 0) {
            // Possible enhancement: use the whole tree slice as bottom interface.
            bottom_contacts.emplace_back(intersection_clipped(
                slices.front(),
                volumes.getPlaceableAreas(0, layer_begin, [] {})));
        } else if (! gracious_root && layer_begin > 0) {
            // Drop non-gracious roots until the branch rests on something.
            std::vector<Polygons> bottom_extra_slices;
            Polygons              rest_support;
            const coord_t         bottom_radius = support_element_radius(config, root);
            // Search up to five bottom-radius heights.
            const LayerIndex layers_propagate_max = 5 * bottom_radius / config.layer_height;
            const LayerIndex layer_bottommost = root.state.verylost
                ? 0 // Very-lost roots search to the bed.
                : std::max(0, layer_begin - layers_propagate_max);
            const double support_area_min_radius = M_PI * sqr(double(config.branch_radius));
            const double support_area_stop = std::max(
                0.2 * M_PI * sqr(double(bottom_radius)),
                HALF * support_area_min_radius);

            for (LayerIndex layer_idx = layer_begin - 1; layer_idx >= layer_bottommost; --layer_idx) {
                rest_support = diff_clipped(
                    rest_support.empty() ? slices.front() : rest_support,
                    volumes.getCollision(0, layer_idx, false));
                if (area(rest_support) < support_area_stop)
                    break;
                bottom_extra_slices.push_back(rest_support);
            }

            const int last_bottom_contact_idx =
                config.support_rests_on_model && config.settings.support_floor_layers > 0 ?
                    int(bottom_extra_slices.size()) - 2 : -1; // Start from the second-to-last slice.
            for (int i = last_bottom_contact_idx; i >= 0; --i) {
                bottom_contacts.emplace_back(intersection_clipped(
                    bottom_extra_slices[i],
                    volumes.getPlaceableAreas(0, layer_begin - i - 1, [] {})));
            }

            layer_begin -= LayerIndex(bottom_extra_slices.size());
            slices.insert(slices.begin(), bottom_extra_slices.size(), {});
            auto destination = slices.begin();
            for (auto source = bottom_extra_slices.rbegin(); source != bottom_extra_slices.rend(); ++source)
                *destination++ = std::move(*source);
        }

        recover_pending_branch_roofs(interface_placer, branch.path, layer_begin, slices);
        return 0;
    };

    auto merge_branch_slices = [](
        Tree &tree,
        LayerIndex layer_begin,
        LayerIndex layer_end,
        size_t num_empty,
        std::vector<Polygons> &slices,
        std::vector<Polygons> &bottom_contacts) {
        layer_begin += LayerIndex(num_empty);
        while (!slices.empty() && slices.back().empty()) {
            slices.pop_back();
            --layer_end;
        }
        if (layer_begin >= layer_end)
            return;

        const LayerIndex new_begin = tree.first_layer_id == -1
            ? layer_begin
            : std::min(tree.first_layer_id, layer_begin);
        const LayerIndex new_end = tree.first_layer_id == -1
            ? layer_end
            : std::max(tree.first_layer_id + LayerIndex(tree.slices.size()), layer_end);
        const size_t new_size = size_t(new_end - new_begin);

        if (tree.first_layer_id != -1) {
            if (tree.slices.capacity() < new_size) {
                std::vector<Slice> new_slices;
                new_slices.reserve(new_size);
                const size_t difference = layer_index_distance(tree.first_layer_id, new_begin);
                if (difference > 0)
                    new_slices.insert(new_slices.end(), difference, {});
                append(new_slices, std::move(tree.slices));
                tree.slices.swap(new_slices);
            } else {
                const size_t difference = layer_index_distance(tree.first_layer_id, new_begin);
                if (difference > 0)
                    tree.slices.insert(tree.slices.begin(), difference, {});
            }
        }

        tree.slices.insert(tree.slices.end(), new_size - tree.slices.size(), {});
        layer_begin -= LayerIndex(num_empty);
        for (LayerIndex layer_idx = layer_begin; layer_idx != layer_end; ++layer_idx) {
            const size_t slice_idx = layer_index_distance(layer_idx, layer_begin);
            Polygons &source = slices[slice_idx];
            if (source.empty())
                continue;

            Slice &destination = tree.slices[layer_index_distance(layer_idx, new_begin)];
            if (++destination.num_branches > 1) {
                append(destination.polygons, std::move(source));
                if (slice_idx < bottom_contacts.size())
                    append(destination.bottom_contacts, std::move(bottom_contacts[slice_idx]));
            } else {
                destination.polygons = std::move(source);
                if (slice_idx < bottom_contacts.size())
                    destination.bottom_contacts = std::move(bottom_contacts[slice_idx]);
            }
        }
        tree.first_layer_id = new_begin;
    };

    for (LayerIndex layer_idx = 0; layer_idx + 1 < LayerIndex(move_bounds.size()); ++layer_idx) {
        for (SupportElement &start_element : move_bounds[layer_idx]) {
            if (start_element.state.marked || start_element.parents.empty())
                continue;
            trees.push_back({});
            TreeVisitor::visit_recursive(move_bounds, start_element, trees.back());
            assert(!trees.back().branches.empty());
        }
    }
    const SlicingParameters &slicing_params = print_object.slicing_parameters();
    MeshSlicingParams mesh_slicing_params;
    mesh_slicing_params.mode = MeshSlicingParams::SlicingMode::Positive;

    auto slice_branch = [&](
        Tree &tree,
        const Branch &branch,
        indexed_triangle_set &partial_mesh,
        std::vector<float> &slice_z,
        std::vector<Polygons> &bottom_contacts) {
        // Triangulate the tube.
        partial_mesh.clear();
        const std::pair<float, float> zspan = extrude_branch(branch.path, config, slicing_params, move_bounds, partial_mesh);
        LayerIndex layer_begin = branch.has_root ?
            branch.path.front()->state.layer_idx :
            std::min(branch.path.front()->state.layer_idx, layer_idx_ceil(slicing_params, config, zspan.first));
        const LayerIndex layer_end = (branch.has_tip ?
            branch.path.back()->state.layer_idx :
            std::max(branch.path.back()->state.layer_idx, layer_idx_floor(slicing_params, config, zspan.second))) + 1;
        slice_z.clear();
        for (LayerIndex layer_idx = layer_begin; layer_idx < layer_end; ++ layer_idx) {
            const double print_z  = layer_z(slicing_params, config, layer_idx);
            const double bottom_z = layer_idx > 0 ? layer_z(slicing_params, config, previous_layer_id(layer_idx)) : 0.;
            slice_z.emplace_back(float(HALF * (bottom_z + print_z)));
        }
        std::vector<Polygons> slices = slice_mesh(partial_mesh, slice_z, mesh_slicing_params, throw_on_cancel);
        bottom_contacts.clear();
        // Possible optimization: parallelize this loop.
        for (LayerIndex i = 0; i < LayerIndex(slices.size()); ++i) {
            slices[i] = diff_clipped(slices[i], volumes.getCollision(0, layer_begin + i, true)); // FIXME parent_uses_min || draw_area.element->state.use_min_xy_dist);
            slices[i] = intersection(slices[i], Polygons{ volumes.m_bed_area });
        }
        const size_t num_empty = prepare_branch_root_slices(
            branch,
            layer_begin,
            slices,
            bottom_contacts);

        merge_branch_slices(
            tree,
            layer_begin,
            layer_end,
            num_empty,
            slices,
            bottom_contacts);
    };

    tbb::parallel_for(tbb::blocked_range<size_t>(0, trees.size(), 1),
        [&trees, &slice_branch](const tbb::blocked_range<size_t> &range) {
            indexed_triangle_set    partial_mesh;
            std::vector<float>      slice_z;
            std::vector<Polygons>   bottom_contacts;
            for (size_t tree_id = range.begin(); tree_id < range.end(); ++ tree_id) {
                Tree &tree = trees[tree_id];
                for (const Branch &branch : tree.branches)
                    slice_branch(tree, branch, partial_mesh, slice_z, bottom_contacts);
            }
        }, tbb::simple_partitioner());

    tbb::parallel_for(tbb::blocked_range<size_t>(0, trees.size(), 1),
        [&trees, &throw_on_cancel](const tbb::blocked_range<size_t> &range) {
        for (size_t tree_id = range.begin(); tree_id < range.end(); ++ tree_id) {
            Tree &tree = trees[tree_id];
            for (Slice &slice : tree.slices)
                if (slice.num_branches > 1) {
                    slice.polygons        = union_(slice.polygons);
                    slice.bottom_contacts = union_(slice.bottom_contacts);
                    slice.num_branches = 1;
                }
            throw_on_cancel();
        }
    }, tbb::simple_partitioner());

    size_t num_layers = 0;
    for (Tree &tree : trees)
        if (tree.first_layer_id >= 0)
            num_layers = std::max(num_layers, size_t(tree.first_layer_id + tree.slices.size()));

    std::vector<Slice> slices(num_layers, Slice{});
    for (Tree &tree : trees)
        if (tree.first_layer_id >= 0) {
            for (LayerIndex i = tree.first_layer_id; i != tree.first_layer_id + LayerIndex(tree.slices.size()); ++ i)
                if (Slice &src = tree.slices[i - tree.first_layer_id]; ! src.polygons.empty()) {
                    Slice &dst = slices[i];
                    if (++ dst.num_branches > 1) {
                        append(dst.polygons,        std::move(src.polygons));
                        append(dst.bottom_contacts, std::move(src.bottom_contacts));
                    } else {
                        dst.polygons        = std::move(src.polygons);
                        dst.bottom_contacts = std::move(src.bottom_contacts);
                    }
                }
        }

    tbb::parallel_for(tbb::blocked_range<size_t>(0, std::min(move_bounds.size(), slices.size()), 1),
        [&print_object, &config, &slices, &bottom_contacts, &top_contacts, &intermediate_layers, &layer_storage, &throw_on_cancel](const tbb::blocked_range<size_t> &range) {
        for (size_t layer_idx = range.begin(); layer_idx < range.end(); ++layer_idx) {
            Slice &slice = slices[layer_idx];
            assert(intermediate_layers[layer_idx] == nullptr);
            Polygons base_layer_polygons     = slice.num_branches > 1 ? union_(slice.polygons) : std::move(slice.polygons);
            Polygons bottom_contact_polygons = slice.num_branches > 1 ? union_(slice.bottom_contacts) : std::move(slice.bottom_contacts);

            if (! base_layer_polygons.empty()) {
                // Most of the time in this function is this union call. Can take 300+ ms when a lot of areas are to be unioned.
                base_layer_polygons = smooth_outward(union_(base_layer_polygons), config.support_line_width); // Note: was .smooth(50) in the original implementation.
                //smooth_outward(closing(std::move(bottom), closing_distance + minimum_island_radius, closing_distance, SUPPORT_SURFACES_OFFSET_PARAMETERS), smoothing_distance) :
                // simplify a bit, to ensure the output does not contain outrageous amounts of vertices. Should not be necessary, just a precaution.
                base_layer_polygons = polygons_simplify(base_layer_polygons, std::min(scaled<double>(0.03), double(config.resolution)), polygons_strictly_simple);
            }

            // Subtract top contact layer polygons from support base.
            SupportGeneratorLayer *top_contact_layer = top_contacts.empty() ? nullptr : top_contacts[layer_idx];
            if (top_contact_layer && ! top_contact_layer->polygons.empty() && ! base_layer_polygons.empty()) {
                base_layer_polygons = diff(base_layer_polygons, top_contact_layer->polygons);
                if (! bottom_contact_polygons.empty())
                    // Possible refactor: it may be better to clip bottom contacts with top contacts first after they are propagated to produce interface layers.
                    bottom_contact_polygons = diff(bottom_contact_polygons, top_contact_layer->polygons);
            }
            if (! bottom_contact_polygons.empty()) {
                base_layer_polygons = diff(base_layer_polygons, bottom_contact_polygons);
                SupportGeneratorLayer *bottom_contact_layer = bottom_contacts[layer_idx] = &layer_allocate(
                    layer_storage, SupporLayerType::BottomContact, print_object.slicing_parameters(), config, layer_idx);
                bottom_contact_layer->polygons = std::move(bottom_contact_polygons);
            }
            if (! base_layer_polygons.empty()) {
                SupportGeneratorLayer *base_layer = intermediate_layers[layer_idx] = &layer_allocate(
                    layer_storage, SupporLayerType::Base, print_object.slicing_parameters(), config, layer_idx);
                base_layer->polygons = union_(base_layer_polygons);
            }

            throw_on_cancel();
        }
    }, tbb::simple_partitioner());
}

} // namespace OrcaTreeSupport3D

void generate_tree_support_3D(PrintObject &print_object, OrcaTreeSupport* tree_support, std::function<void()> throw_on_cancel)
{
    size_t idx = 0;
    for (const PrintObject *po : print_object.print()->objects()) {
        if (po == &print_object)
            break;
        ++idx;
    }

    Points bedpts = tree_support->m_machine_border.contour.points;
    Pointfs bedptsf;
    std::transform(bedpts.begin(), bedpts.end(), std::back_inserter(bedptsf), [](const Point &p) { return unscale(p); });
    BuildVolume build_volume{ bedptsf, tree_support->m_print_config->max_print_height.value };

    OrcaTreeSupport3D::generate_support_areas(*print_object.print(), build_volume, { idx }, throw_on_cancel);
}

} // namespace Slic3r
