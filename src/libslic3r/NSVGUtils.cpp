///|/ Copyright (c) Prusa Research 2021 - 2022 Filip Sykala @Jony01
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "NSVGUtils.hpp"
#include <array>
#include <charconv> // to_chars

#include <boost/nowide/iostream.hpp>
#include <boost/nowide/fstream.hpp>
#include "ClipperUtils.hpp"
#include "Emboss.hpp" // heal for shape

namespace {    
using namespace Slic3r; // Polygon

constexpr size_t nsvg_shape_id_stride = 2; // fill and stroke IDs occupy two slots per SVG shape
constexpr size_t nsvg_bounds_max_x_index = 2; // NanoSVG bounds order: min x, min y, max x, max y
constexpr size_t nsvg_bounds_max_y_index = 3; // NanoSVG bounds order: min x, min y, max x, max y
constexpr size_t nsvg_coordinate_pair_size = 2; // x and y values per NanoSVG point
constexpr size_t nsvg_cubic_segment_stride = 3; // each cubic segment adds three points after its start
constexpr float nsvg_cubic_degree = 3.f; // cubic Bezier curve degree
constexpr size_t nsvg_cubic_control1_x_index = 2; // first control point x in NanoSVG's cubic array
constexpr size_t nsvg_cubic_control1_y_index = 3; // first control point y in NanoSVG's cubic array
constexpr size_t nsvg_cubic_control2_x_index = 4; // second control point x in NanoSVG's cubic array
constexpr size_t nsvg_cubic_control2_y_index = 5; // second control point y in NanoSVG's cubic array
constexpr size_t nsvg_cubic_end_x_index = 6; // endpoint x in NanoSVG's cubic array
constexpr size_t nsvg_cubic_end_y_index = 7; // endpoint y in NanoSVG's cubic array
constexpr float nsvg_doubled_dash_pattern = 2.0f; // odd SVG dash lists repeat once to form an even pattern
constexpr double cubic_root_exponent = 1.0 / 3.0; // cubic root used for round-join tolerance
constexpr float half_stroke_width = 0.5f; // stroke expansion uses the radius, not the full width

// see function nsvg__lineTo(NSVGparser* p, float x, float y)
bool is_line(const float *p, float precision = 1e-4f);
// convert curve in path to lines
struct LinesPath{
    Polygons polygons;
    Polylines polylines; };
LinesPath linearize_path(NSVGpath *first_path, const NSVGLineParams &param);
HealedExPolygons fill_to_expolygons(const LinesPath &lines_path, const NSVGshape &shape, const NSVGLineParams &param);
HealedExPolygons stroke_to_expolygons(const LinesPath &lines_path, const NSVGshape &shape, const NSVGLineParams &param);
} // namespace

namespace Slic3r {

ExPolygonsWithIds create_shape_with_ids(const NSVGimage &image, const NSVGLineParams &param)
{
    ExPolygonsWithIds result;
    size_t shape_id = 0;
    for (NSVGshape *shape_ptr = image.shapes; shape_ptr != NULL; shape_ptr = shape_ptr->next, ++shape_id) {
        const NSVGshape &shape = *shape_ptr;
        if (!(shape.flags & NSVG_FLAGS_VISIBLE))
            continue;

        bool is_fill_used = shape.fill.type != NSVG_PAINT_NONE;
        bool is_stroke_used = 
            shape.stroke.type != NSVG_PAINT_NONE &&
            shape.strokeWidth > 1e-5f;

        if (!is_fill_used && !is_stroke_used)
            continue;

        const LinesPath lines_path = linearize_path(shape.paths, param);

        if (is_fill_used) {
            unsigned unique_id = static_cast<unsigned>(nsvg_shape_id_stride * shape_id);
            HealedExPolygons expoly = fill_to_expolygons(lines_path, shape, param);
            result.push_back({unique_id, expoly.expolygons, expoly.is_healed});
        }        
        if (is_stroke_used) {
            unsigned unique_id = static_cast<unsigned>(nsvg_shape_id_stride * shape_id + 1);
            HealedExPolygons expoly = stroke_to_expolygons(lines_path, shape, param);
            result.push_back({unique_id, expoly.expolygons, expoly.is_healed});
        }
    }

    // SVG is used as centered
    // Do not disturb user by settings of pivot position
    center(result);
    return result;
}

Polygons to_polygons(const NSVGimage &image, const NSVGLineParams &param)
{
    Polygons result;
    for (NSVGshape *shape = image.shapes; shape != NULL; shape = shape->next) {
        if (!(shape->flags & NSVG_FLAGS_VISIBLE))
            continue;
        if (shape->fill.type == NSVG_PAINT_NONE)
            continue;
        const LinesPath lines_path = linearize_path(shape->paths, param);
        polygons_append(result, lines_path.polygons);
        // close polyline to create polygon
        polygons_append(result, to_polygons(lines_path.polylines));        
    }
    return result;
}

void bounds(const NSVGimage &image, Vec2f& min, Vec2f &max)
{
    for (const NSVGshape *shape = image.shapes; shape != NULL; shape = shape->next)
        for (const NSVGpath *path = shape->paths; path != NULL; path = path->next) {
            if (min.x() > path->bounds[0])
                min.x() = path->bounds[0];
            if (min.y() > path->bounds[1])
                min.y() = path->bounds[1];
            if (max.x() < path->bounds[nsvg_bounds_max_x_index])
                max.x() = path->bounds[nsvg_bounds_max_x_index];
            if (max.y() < path->bounds[nsvg_bounds_max_y_index])
                max.y() = path->bounds[nsvg_bounds_max_y_index];
        }
}

NSVGimage_ptr nsvgParseFromFile(const std::string &filename, const char *units, float dpi)
{
    NSVGimage *image = ::nsvgParseFromFile(filename.c_str(), units, dpi);
    return {image, &nsvgDelete};
}

std::unique_ptr<std::string> read_from_disk(const std::string &path)
{
    boost::nowide::ifstream fs{path};
    if (!fs.is_open())
        return nullptr;
    std::stringstream ss;
    ss << fs.rdbuf();
    return std::make_unique<std::string>(ss.str());
}

NSVGimage_ptr nsvgParse(const std::string& file_data, const char *units, float dpi){
    // NOTE: nsvg parser consumes mutable, null-terminated input.
    size_t size = file_data.size();
    // file data could be big, so it is allocated on heap
    auto data_copy = std::make_unique<char[]>(size + 1);
    memcpy(data_copy.get(), file_data.c_str(), size);
    data_copy[size]  = '\0'; // data for nsvg must be null terminated
    NSVGimage *image = ::nsvgParse(data_copy.get(), units, dpi);
    return {image, &nsvgDelete};
}

NSVGimage *init_image(EmbossShape::SvgFile &svg_file){
    // is already initialized?
    if (svg_file.image.get() != nullptr)
        return svg_file.image.get();

    if (svg_file.file_data == nullptr) {
        // chech if path is known
        if (svg_file.path.empty())
            return nullptr;
        svg_file.file_data = read_from_disk(svg_file.path);
        if (svg_file.file_data == nullptr)
            return nullptr;
    }

    // init svg image
    svg_file.image = nsvgParse(*svg_file.file_data);
    if (svg_file.image.get() == NULL)
        return nullptr;

    return svg_file.image.get();
}

size_t get_shapes_count(const NSVGimage &image)
{
    size_t count = 0;
    for (NSVGshape * s = image.shapes; s != NULL; s = s->next)
        ++count;
    return count;
}

} // namespace Slic3r

namespace {
using namespace Slic3r; // Polygon + Vec2f

Point::coord_type to_coor(float val, double scale) { return static_cast<Point::coord_type>(std::round(val * scale)); }

bool need_flattening(float tessTol, const Vec2f &p1, const Vec2f &p2, const Vec2f &p3, const Vec2f &p4) {
    // f .. first
    // s .. second
    auto det = [](const Vec2f &f, const Vec2f &s) {
        return std::fabs(f.x() * s.y() - f.y() * s.x()); 
    };

    Vec2f pd  = (p4 - p1);
    Vec2f pd2 = (p2 - p4);
    float d2  = det(pd2, pd);
    Vec2f pd3 = (p3 - p4);
    float d3  = det(pd3, pd);
    float d23 = d2 + d3;

    return (d23 * d23) >= tessTol * pd.squaredNorm();
}

// see function nsvg__lineTo(NSVGparser* p, float x, float y)
bool is_line(const float *p, float precision){
    //Vec2f p1(p[0], p[1]);
    //Vec2f p2(p[2], p[3]);
    //Vec2f p3(p[4], p[5]);
    //Vec2f p4(p[6], p[7]);
    float dx_3 = (p[nsvg_cubic_end_x_index] - p[0]) / nsvg_cubic_degree;
    float dy_3 = (p[nsvg_cubic_end_y_index] - p[1]) / nsvg_cubic_degree;

    return 
        is_approx(p[nsvg_cubic_control1_x_index], p[0] + dx_3, precision) && 
        is_approx(p[nsvg_cubic_control2_x_index], p[nsvg_cubic_end_x_index] - dx_3, precision) && 
        is_approx(p[nsvg_cubic_control1_y_index], p[1] + dy_3, precision) &&
        is_approx(p[nsvg_cubic_control2_y_index], p[nsvg_cubic_end_y_index] - dy_3, precision);
}

/// <summary>
/// Convert cubic curve to lines
/// Inspired by nanosvgrast.h function nsvgRasterize -> nsvg__flattenShape -> nsvg__flattenCubicBez
/// https://github.com/memononen/nanosvg/blob/f0a3e1034dd22e2e87e5db22401e44998383124e/src/nanosvgrast.h#L335
/// </summary>
/// <param name="polygon">Result points</param>
/// <param name="tessTol">Tesselation tolerance</param>
/// <param name="p1">Curve point</param>
/// <param name="p2">Curve point</param>
/// <param name="p3">Curve point</param>
/// <param name="p4">Curve point</param>
/// <param name="level">Actual depth of recursion</param>
void flatten_cubic_bez(Points &points, float tessTol, const Vec2f& p1, const Vec2f& p2, const Vec2f& p3, const Vec2f& p4, int level)
{
    if (!need_flattening(tessTol, p1, p2, p3, p4)) {
        Point::coord_type x = static_cast<Point::coord_type>(std::round(p4.x()));
        Point::coord_type y = static_cast<Point::coord_type>(std::round(p4.y()));
        points.emplace_back(x, y);
        return;
    }

    --level;
    if (level == 0)
        return;
    
    Vec2f p12  = (p1 + p2) * 0.5f;
    Vec2f p23  = (p2 + p3) * 0.5f;
    Vec2f p34  = (p3 + p4) * 0.5f;
    Vec2f p123 = (p12 + p23) * 0.5f;
    Vec2f p234  = (p23 + p34) * 0.5f;
    Vec2f p1234 = (p123 + p234) * 0.5f;
    flatten_cubic_bez(points, tessTol, p1, p12, p123, p1234, level);
    flatten_cubic_bez(points, tessTol, p1234, p234, p34, p4, level);
}

LinesPath linearize_path(NSVGpath *first_path, const NSVGLineParams &param)
{
    LinesPath result;
    Polygons  &polygons  = result.polygons;
    Polylines &polylines = result.polylines;

    // multiple use of allocated memmory for points between paths
    Points points;
    for (NSVGpath *path = first_path; path != NULL; path = path->next) {
        // Flatten path
        Point::coord_type x = to_coor(path->pts[0], param.scale);
        Point::coord_type y = to_coor(path->pts[1], param.scale);
        points.emplace_back(x, y);
        size_t path_size = (path->npts > 1) ? static_cast<size_t>(path->npts - 1) : 0;
        for (size_t i = 0; i < path_size; i += nsvg_cubic_segment_stride) {
            const float *p = &path->pts[i * nsvg_coordinate_pair_size];
            if (is_line(p)) {
                // point p4
                Point::coord_type xx = to_coor(p[nsvg_cubic_end_x_index], param.scale);
                Point::coord_type yy = to_coor(p[nsvg_cubic_end_y_index], param.scale);
                points.emplace_back(xx, yy);
                continue;
            }
            Vec2f p1(p[0], p[1]);
            Vec2f p2(p[nsvg_cubic_control1_x_index], p[nsvg_cubic_control1_y_index]);
            Vec2f p3(p[nsvg_cubic_control2_x_index], p[nsvg_cubic_control2_y_index]);
            Vec2f p4(p[nsvg_cubic_end_x_index], p[nsvg_cubic_end_y_index]);
            flatten_cubic_bez(points, param.tesselation_tolerance, 
                p1 * param.scale, p2 * param.scale, p3 * param.scale, p4 * param.scale, 
                param.max_level);
        }
        assert(!points.empty());
        if (points.empty()) 
            continue;

        if (param.is_y_negative)
            for (Point &p : points)
                p.y() = -p.y();

        if (path->closed) {
            polygons.emplace_back(points);
        } else {
            polylines.emplace_back(points);
        }
        // prepare for new path - recycle alocated memory
        points.clear();
    }
    remove_same_neighbor(polygons);
    remove_same_neighbor(polylines);
    return result;
}

HealedExPolygons fill_to_expolygons(const LinesPath &lines_path, const NSVGshape &shape, const NSVGLineParams &param)
{
    Polygons fill = lines_path.polygons; // copy

    // close polyline to create polygon
    polygons_append(fill, to_polygons(lines_path.polylines));
    if (fill.empty())
        return {};

    // if (shape->fillRule == NSVGfillRule::NSVG_FILLRULE_NONZERO)
    bool is_non_zero = true;
    if (shape.fillRule == NSVGfillRule::NSVG_FILLRULE_EVENODD)
        is_non_zero = false;

    return Emboss::heal_polygons(fill, is_non_zero, param.max_heal_iteration);
}

struct DashesParam{
    // first dash length
    float dash_length = 1.f; // scaled

    // is current dash .. true
    // is current space .. false
    bool is_line = true;

    // current index to array
    unsigned char dash_index = 0;
    static constexpr size_t max_dash_array_size = 8; // limitation of nanosvg strokeDashArray
    std::array<float, max_dash_array_size> dash_array;     // scaled
    unsigned char dash_count = 0; // count of values in array

    explicit DashesParam(const NSVGshape &shape, double scale) :
        dash_count(shape.strokeDashCount)
    {
        assert(dash_count > 0);
        assert(dash_count <= max_dash_array_size); // limitation of nanosvg strokeDashArray
        for (size_t i = 0; i < dash_count; ++i)
            dash_array[i] = static_cast<float>(shape.strokeDashArray[i] * scale);
        
        // Figure out dash offset.
        float all_dash_length = 0;
        for (unsigned char j = 0; j < dash_count; ++j)
            all_dash_length += dash_array[j];

        if (dash_count%2 == 1) // (shape.strokeDashCount & 1)
            all_dash_length *= nsvg_doubled_dash_pattern;

        // Find location inside pattern
        float dash_offset = fmodf(static_cast<float>(shape.strokeDashOffset * scale), all_dash_length);
        if (dash_offset < 0.0f)
            dash_offset += all_dash_length;

        while (dash_offset > dash_array[dash_index]) {
            dash_offset -= dash_array[dash_index];
            dash_index = (dash_index + 1) % shape.strokeDashCount;
            is_line    = !is_line;
        }

        dash_length = dash_array[dash_index] - dash_offset;
    }
};

Polylines to_dashes(const Polyline &polyline, const DashesParam& param)
{
    Polylines dashes;
    Polyline dash; // cache for one dash in dashed line
    Point prev_point;        
    
    bool is_line = param.is_line;
    unsigned char dash_index = param.dash_index;
    float dash_length = param.dash_length; // current rest of dash distance
    for (const Point &point : polyline.points) {
        if (&point == &polyline.points.front()) {
            // is first point
            prev_point = point; // copy
            continue;
        }

        Point diff = point - prev_point;
        float line_segment_length = diff.cast<float>().norm();
        while (dash_length < line_segment_length) {
            // Calculate intermediate point
            float d = dash_length / line_segment_length;
            Point move_point   = diff * d;
            Point intermediate = prev_point + move_point;
            
            // add Dash in stroke
            if (is_line) {
                if (dash.empty()) {
                    dashes.emplace_back(Points{prev_point, intermediate});
                } else {
                    dash.append(prev_point);
                    dash.append(intermediate);
                    dashes.push_back(dash);
                    dash.clear();
                }
            }

            diff -= move_point;
            line_segment_length -= dash_length;
            prev_point = intermediate;

            // Advance dash pattern
            is_line = !is_line;
            dash_index = (dash_index + 1) % param.dash_count;
            dash_length = param.dash_array[dash_index];
        }

        if (is_line)
            dash.append(prev_point);
        dash_length -= line_segment_length;
        prev_point = point; // copy
    }

    // add last dash
    if (is_line){
        assert(!dash.empty());
        dash.append(prev_point); // prev_point == polyline.points.back()
        dashes.push_back(dash);
    }
    return dashes;
}

HealedExPolygons stroke_to_expolygons(const LinesPath &lines_path, const NSVGshape &shape, const NSVGLineParams &param)
{
    // convert stroke to polygon
    ClipperLib::JoinType join_type = ClipperLib::JoinType::jtSquare;
    switch (static_cast<NSVGlineJoin>(shape.strokeLineJoin)) {
    case NSVGlineJoin::NSVG_JOIN_BEVEL: join_type = ClipperLib::JoinType::jtSquare; break;
    case NSVGlineJoin::NSVG_JOIN_MITER: join_type = ClipperLib::JoinType::jtMiter; break;
    case NSVGlineJoin::NSVG_JOIN_ROUND: join_type = ClipperLib::JoinType::jtRound; break;
    }

    double mitter = shape.miterLimit * param.scale;
    if (join_type == ClipperLib::JoinType::jtRound) {
        // mitter is used as ArcTolerance
        // http://www.angusj.com/delphi/clipper/documentation/Docs/Units/ClipperLib/Classes/ClipperOffset/Properties/ArcTolerance.htm
        mitter = std::pow(param.tesselation_tolerance, cubic_root_exponent);
    }
    float stroke_width = static_cast<float>(shape.strokeWidth * param.scale);

    ClipperLib::EndType end_type = ClipperLib::EndType::etOpenButt;
    switch (static_cast<NSVGlineCap>(shape.strokeLineCap)) {
    case NSVGlineCap::NSVG_CAP_BUTT: end_type = ClipperLib::EndType::etOpenButt; break;
    case NSVGlineCap::NSVG_CAP_ROUND: end_type = ClipperLib::EndType::etOpenRound; break;
    case NSVGlineCap::NSVG_CAP_SQUARE: end_type = ClipperLib::EndType::etOpenSquare; break;
    }

    Polygons result;
    if (shape.strokeDashCount > 0) {
        DashesParam params(shape, param.scale);
        Polylines dashes;
        for (const Polyline &polyline : lines_path.polylines)
            polylines_append(dashes, to_dashes(polyline, params));
        for (const Polygon &polygon : lines_path.polygons)
            polylines_append(dashes, to_dashes(to_polyline(polygon), params));
        result = offset(dashes, stroke_width * half_stroke_width, join_type, mitter, end_type);
    } else {
        result = contour_to_polygons(lines_path.polygons, stroke_width, join_type, mitter);
        polygons_append(result, offset(lines_path.polylines, stroke_width * half_stroke_width, join_type, mitter, end_type));    
    }

    bool is_non_zero = true;
    return Emboss::heal_polygons(result, is_non_zero, param.max_heal_iteration);
}

} // namespace
