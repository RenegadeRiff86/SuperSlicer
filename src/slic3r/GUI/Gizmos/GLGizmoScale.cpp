///|/ Copyright (c) Prusa Research 2019 - 2023 Oleksandra Iushchenko @YuSanka, Lukáš Matěna @lukasmatena, Enrico Turri @enricoturri1966, Filip Sykala @Jony01, Vojtěch Bubník @bubnikv
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "GLGizmoScale.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectManipulation.hpp"
#include "slic3r/GUI/Plater.hpp"

#include <GL/glew.h>

#include <wx/utils.h>

namespace Slic3r {
namespace GUI {

namespace {

constexpr char kDashedThickLinesShaderName[] = "dashed_thick_lines";
constexpr char kViewModelMatrixUniform[]     = "view_model_matrix";
constexpr char kProjectionMatrixUniform[]    = "projection_matrix";
constexpr char kViewportSizeUniform[]        = "viewport_size";
constexpr char kGapSizeUniform[]             = "gap_size";

// Decimal places shown for a scale percentage in the gizmo's tooltip.
constexpr int kScaleTooltipPrecision = 4;

} // namespace

const double GLGizmoScale3D::Offset = 5.0;

GLGizmoScale3D::GLGizmoScale3D(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id)
    : GLGizmoBase(parent, icon_filename, sprite_id)
    , m_scale(Vec3d::Ones())
    , m_snap_step(0.05)
    , m_base_color(DEFAULT_BASE_COLOR)
    , m_drag_color(DEFAULT_DRAG_COLOR)
    , m_highlight_color(DEFAULT_HIGHLIGHT_COLOR)
{
    m_grabber_connections[ConnectionXAxis].grabber_indices      = { GrabberXMin, GrabberXMax };
    m_grabber_connections[ConnectionYAxis].grabber_indices      = { GrabberYMin, GrabberYMax };
    m_grabber_connections[ConnectionZAxis].grabber_indices      = { GrabberZMin, GrabberZMax };
    // The square's sides, each joining the next pair of corners round the loop.
    m_grabber_connections[ConnectionSquareYMin].grabber_indices = { GrabberCornerXMinYMin, GrabberCornerXMaxYMin };
    m_grabber_connections[ConnectionSquareXMax].grabber_indices = { GrabberCornerXMaxYMin, GrabberCornerXMaxYMax };
    m_grabber_connections[ConnectionSquareYMax].grabber_indices = { GrabberCornerXMaxYMax, GrabberCornerXMinYMax };
    m_grabber_connections[ConnectionSquareXMin].grabber_indices = { GrabberCornerXMinYMax, GrabberCornerXMinYMin };
}

std::string GLGizmoScale3D::get_tooltip() const
{
    const Vec3d scale = 100.0 * m_scale;

    if (m_hover_id == GrabberXMin || m_hover_id == GrabberXMax || m_grabbers[GrabberXMin].dragging || m_grabbers[GrabberXMax].dragging)
        return "X: " + format(scale.x(), kScaleTooltipPrecision) + "%";
    else if (m_hover_id == GrabberYMin || m_hover_id == GrabberYMax || m_grabbers[GrabberYMin].dragging || m_grabbers[GrabberYMax].dragging)
        return "Y: " + format(scale.y(), kScaleTooltipPrecision) + "%";
    else if (m_hover_id == GrabberZMin || m_hover_id == GrabberZMax || m_grabbers[GrabberZMin].dragging || m_grabbers[GrabberZMax].dragging)
        return "Z: " + format(scale.z(), kScaleTooltipPrecision) + "%";
    else if (m_hover_id >= GrabberFirstUniform ||
        m_grabbers[GrabberCornerXMinYMin].dragging || m_grabbers[GrabberCornerXMaxYMin].dragging ||
        m_grabbers[GrabberCornerXMaxYMax].dragging || m_grabbers[GrabberCornerXMinYMax].dragging)
    {
        std::string tooltip = "X: " + format(scale.x(), kScaleTooltipPrecision) + "%\n";
        tooltip += "Y: " + format(scale.y(), kScaleTooltipPrecision) + "%\n";
        tooltip += "Z: " + format(scale.z(), kScaleTooltipPrecision) + "%";
        return tooltip;
    }
    else
        return "";
}

// The grabber a constrained (Ctrl) drag holds still: the one opposite the one being dragged. For
// an axis pair that is its partner; for a corner it is the diagonal, not either neighbour.
static int constraint_id(int grabber_id)
{
  static const std::vector<int> id_map = {
      GLGizmoScale3D::GrabberXMax,           GLGizmoScale3D::GrabberXMin,
      GLGizmoScale3D::GrabberYMax,           GLGizmoScale3D::GrabberYMin,
      GLGizmoScale3D::GrabberZMax,           GLGizmoScale3D::GrabberZMin,
      GLGizmoScale3D::GrabberCornerXMaxYMax, GLGizmoScale3D::GrabberCornerXMinYMax,
      GLGizmoScale3D::GrabberCornerXMinYMin, GLGizmoScale3D::GrabberCornerXMaxYMin };
  return (0 <= grabber_id && grabber_id < static_cast<int>(id_map.size())) ? id_map[grabber_id] : -1;
}

bool GLGizmoScale3D::on_mouse(const wxMouseEvent &mouse_event)
{
    if (mouse_event.Dragging()) {
        if (m_dragging) {
            // Apply new temporary scale factors
            TransformationType transformation_type;
            if (wxGetApp().obj_manipul()->is_local_coordinates())
                transformation_type.set_local();
            else if (wxGetApp().obj_manipul()->is_instance_coordinates())
                transformation_type.set_instance();

            transformation_type.set_relative();

            if (mouse_event.AltDown())
                transformation_type.set_independent();

            Selection& selection = m_parent.get_selection();
            selection.scale(m_scale, transformation_type);
            if (m_starting.ctrl_down) {
                // constrained scale:
                // uses the performed scale to calculate the new position of the constrained grabber
                // and from that calculates the offset (in world coordinates) to be applied to fullfill the constraint
                update_render_data();
                const Vec3d constraint_position = m_grabbers_transform * m_grabbers[constraint_id(m_hover_id)].center;
                // re-apply the scale because the selection always applies the transformations with respect to the initial state 
                // set into on_start_dragging() with the call to selection.setup_cache()
                m_parent.get_selection().scale_and_translate(m_scale, m_starting.constraint_position - constraint_position, transformation_type);
            }
        }
    }
    return use_grabbers(mouse_event);
}

void GLGizmoScale3D::enable_ununiversal_scale(bool enable)
{
    for (int i = 0; i < GrabberFirstUniform; ++i)
        m_grabbers[i].enabled = enable;
}

void GLGizmoScale3D::data_changed(bool is_serializing) {
    set_scale(Vec3d::Ones());
}

bool GLGizmoScale3D::on_init()
{
    for (int i = 0; i < GrabberCount; ++i) {
        m_grabbers.push_back(Grabber());
    }

    m_shortcut_key = WXK_CONTROL_S;
    return true;
}

std::string GLGizmoScale3D::on_get_name() const
{
    return _u8L("Scale");
}

bool GLGizmoScale3D::on_is_activable() const
{
    const Selection& selection = m_parent.get_selection();
    return !selection.is_any_cut_volume() && !selection.is_any_connector() && !selection.is_empty() && !selection.is_wipe_tower();
}

void GLGizmoScale3D::on_start_dragging()
{
    assert(m_hover_id != -1);
    m_starting.ctrl_down = wxGetKeyState(WXK_CONTROL);
    m_starting.drag_position = m_grabbers_transform * m_grabbers[m_hover_id].center;
    m_starting.box = m_bounding_box;
    m_starting.center = m_center;
    m_starting.instance_center = m_instance_center;
    m_starting.constraint_position = m_grabbers_transform * m_grabbers[constraint_id(m_hover_id)].center;
}

void GLGizmoScale3D::on_stop_dragging()
{
    m_parent.do_scale(L("Gizmo-Scale"));
    m_starting.ctrl_down = false;
}

void GLGizmoScale3D::on_dragging(const UpdateData& data)
{
    if (m_hover_id == GrabberXMin || m_hover_id == GrabberXMax)
        do_scale_along_axis(X, data);
    else if (m_hover_id == GrabberYMin || m_hover_id == GrabberYMax)
        do_scale_along_axis(Y, data);
    else if (m_hover_id == GrabberZMin || m_hover_id == GrabberZMax)
        do_scale_along_axis(Z, data);
    else if (m_hover_id >= GrabberFirstUniform)
        do_scale_uniform(data);
}

void GLGizmoScale3D::on_render()
{
    glsafe(::glClear(GL_DEPTH_BUFFER_BIT));
    glsafe(::glEnable(GL_DEPTH_TEST));

    update_render_data();

    if (OpenGLManager::get_gl_info().get_max_line_width() > 1) {
        float min = std::min(1.5f, OpenGLManager::get_gl_info().get_min_line_width());
        float bigger = min <= 1.5f ? 2.f : min + 1.f;
        glsafe(::glLineWidth((m_hover_id != -1) ? bigger : min));
    }

    const float grabber_mean_size = static_cast<float>((m_bounding_box.size().x() + m_bounding_box.size().y() + m_bounding_box.size().z()) / 3.0);

    if (m_hover_id == -1) {
        // draw connections
#if ENABLE_GL_CORE_PROFILE
        GLShaderProgram* shader = OpenGLManager::get_gl_info().is_core_profile() ? wxGetApp().get_shader(kDashedThickLinesShaderName) : wxGetApp().get_shader("flat");
#else
        GLShaderProgram* shader = wxGetApp().get_shader("flat");
#endif // ENABLE_GL_CORE_PROFILE
        if (shader != nullptr) {
            shader->start_using();
            const Camera& camera = wxGetApp().plater()->get_camera();
            shader->set_uniform(kViewModelMatrixUniform, camera.get_view_matrix() * m_grabbers_transform);
            shader->set_uniform(kProjectionMatrixUniform, camera.get_projection_matrix());
#if ENABLE_GL_CORE_PROFILE
            const std::array<int, GLViewportComponents>& viewport = camera.get_viewport();
            shader->set_uniform(kViewportSizeUniform, Vec2d(double(viewport[ViewportWidth]), double(viewport[ViewportHeight])));
            shader->set_uniform("width", 0.25f);
            shader->set_uniform(kGapSizeUniform, 0.0f);
#endif // ENABLE_GL_CORE_PROFILE
            if (m_grabbers[GrabberXMin].enabled && m_grabbers[GrabberXMax].enabled)
                render_grabbers_connection(GrabberXMin, GrabberXMax, m_grabbers[GrabberXMin].color);
            if (m_grabbers[GrabberYMin].enabled && m_grabbers[GrabberYMax].enabled)
                render_grabbers_connection(GrabberYMin, GrabberYMax, m_grabbers[GrabberYMin].color);
            if (m_grabbers[GrabberZMin].enabled && m_grabbers[GrabberZMax].enabled)
                render_grabbers_connection(GrabberZMin, GrabberZMax, m_grabbers[GrabberZMin].color);
            render_grabbers_connection(GrabberCornerXMinYMin, GrabberCornerXMaxYMin, m_base_color);
            render_grabbers_connection(GrabberCornerXMaxYMin, GrabberCornerXMaxYMax, m_base_color);
            render_grabbers_connection(GrabberCornerXMaxYMax, GrabberCornerXMinYMax, m_base_color);
            render_grabbers_connection(GrabberCornerXMinYMax, GrabberCornerXMinYMin, m_base_color);
            shader->stop_using();
        }

        // draw grabbers
        render_grabbers(grabber_mean_size);
    }
    else if ((m_hover_id == GrabberXMin || m_hover_id == GrabberXMax) && m_grabbers[GrabberXMin].enabled && m_grabbers[GrabberXMax].enabled) {
        // draw connections
#if ENABLE_GL_CORE_PROFILE
        GLShaderProgram* shader = OpenGLManager::get_gl_info().is_core_profile() ? wxGetApp().get_shader(kDashedThickLinesShaderName) : wxGetApp().get_shader("flat");
#else
        GLShaderProgram* shader = wxGetApp().get_shader("flat");
#endif // ENABLE_GL_CORE_PROFILE
        if (shader != nullptr) {
            shader->start_using();
            const Camera& camera = wxGetApp().plater()->get_camera();
            shader->set_uniform(kViewModelMatrixUniform, camera.get_view_matrix() * m_grabbers_transform);
            shader->set_uniform(kProjectionMatrixUniform, camera.get_projection_matrix());
#if ENABLE_GL_CORE_PROFILE
            const std::array<int, GLViewportComponents>& viewport = camera.get_viewport();
            shader->set_uniform(kViewportSizeUniform, Vec2d(double(viewport[ViewportWidth]), double(viewport[ViewportHeight])));
            shader->set_uniform("width", 0.25f);
            shader->set_uniform(kGapSizeUniform, 0.0f);
#endif // ENABLE_GL_CORE_PROFILE
            render_grabbers_connection(GrabberXMin, GrabberXMax, m_grabbers[GrabberXMin].color);
            shader->stop_using();
        }

        // draw grabbers
        shader = wxGetApp().get_shader("gouraud_light");
        if (shader != nullptr) {
            shader->start_using();
            shader->set_uniform("emission_factor", 0.1f);
            render_grabbers(GrabberXMin, GrabberXMax, grabber_mean_size, true);
            shader->stop_using();
        }
    }
    else if ((m_hover_id == GrabberYMin || m_hover_id == GrabberYMax) && m_grabbers[GrabberYMin].enabled && m_grabbers[GrabberYMax].enabled) {
        // draw connections
#if ENABLE_GL_CORE_PROFILE
        GLShaderProgram* shader = OpenGLManager::get_gl_info().is_core_profile() ? wxGetApp().get_shader(kDashedThickLinesShaderName) : wxGetApp().get_shader("flat");
#else
        GLShaderProgram* shader = wxGetApp().get_shader("flat");
#endif // ENABLE_GL_CORE_PROFILE
        if (shader != nullptr) {
            shader->start_using();
            const Camera& camera = wxGetApp().plater()->get_camera();
            shader->set_uniform(kViewModelMatrixUniform, camera.get_view_matrix() * m_grabbers_transform);
            shader->set_uniform(kProjectionMatrixUniform, camera.get_projection_matrix());
#if ENABLE_GL_CORE_PROFILE
            const std::array<int, GLViewportComponents>& viewport = camera.get_viewport();
            shader->set_uniform(kViewportSizeUniform, Vec2d(double(viewport[ViewportWidth]), double(viewport[ViewportHeight])));
            shader->set_uniform("width", 0.25f);
            shader->set_uniform(kGapSizeUniform, 0.0f);
#endif // ENABLE_GL_CORE_PROFILE
            render_grabbers_connection(GrabberYMin, GrabberYMax, m_grabbers[GrabberYMin].color);
            shader->stop_using();
        }

        // draw grabbers
        shader = wxGetApp().get_shader("gouraud_light");
        if (shader != nullptr) {
            shader->start_using();
            shader->set_uniform("emission_factor", 0.1f);
            render_grabbers(GrabberYMin, GrabberYMax, grabber_mean_size, true);
            shader->stop_using();
        }
    }
    else if ((m_hover_id == GrabberZMin || m_hover_id == GrabberZMax) && m_grabbers[GrabberZMin].enabled && m_grabbers[GrabberZMax].enabled) {
        // draw connections
#if ENABLE_GL_CORE_PROFILE
        GLShaderProgram* shader = OpenGLManager::get_gl_info().is_core_profile() ? wxGetApp().get_shader(kDashedThickLinesShaderName) : wxGetApp().get_shader("flat");
#else
        GLShaderProgram* shader = wxGetApp().get_shader("flat");
#endif // ENABLE_GL_CORE_PROFILE
        if (shader != nullptr) {
            shader->start_using();
            const Camera& camera = wxGetApp().plater()->get_camera();
            shader->set_uniform(kViewModelMatrixUniform, camera.get_view_matrix() * m_grabbers_transform);
            shader->set_uniform(kProjectionMatrixUniform, camera.get_projection_matrix());
#if ENABLE_GL_CORE_PROFILE
            const std::array<int, GLViewportComponents>& viewport = camera.get_viewport();
            shader->set_uniform(kViewportSizeUniform, Vec2d(double(viewport[ViewportWidth]), double(viewport[ViewportHeight])));
            shader->set_uniform("width", 0.25f);
            shader->set_uniform(kGapSizeUniform, 0.0f);
#endif // ENABLE_GL_CORE_PROFILE
            render_grabbers_connection(GrabberZMin, GrabberZMax, m_grabbers[GrabberZMin].color);
            shader->stop_using();
        }

        // draw grabbers
        shader = wxGetApp().get_shader("gouraud_light");
        if (shader != nullptr) {
            shader->start_using();
            shader->set_uniform("emission_factor", 0.1f);
            render_grabbers(GrabberZMin, GrabberZMax, grabber_mean_size, true);
            shader->stop_using();
        }
    }
    else if (m_hover_id >= GrabberFirstUniform) {
        // draw connections
#if ENABLE_GL_CORE_PROFILE
        GLShaderProgram* shader = OpenGLManager::get_gl_info().is_core_profile() ? wxGetApp().get_shader(kDashedThickLinesShaderName) : wxGetApp().get_shader("flat");
#else
        GLShaderProgram* shader = wxGetApp().get_shader("flat");
#endif // ENABLE_GL_CORE_PROFILE
        if (shader != nullptr) {
            shader->start_using();
            const Camera& camera = wxGetApp().plater()->get_camera();
            shader->set_uniform(kViewModelMatrixUniform, camera.get_view_matrix() * m_grabbers_transform);
            shader->set_uniform(kProjectionMatrixUniform, camera.get_projection_matrix());
#if ENABLE_GL_CORE_PROFILE
            const std::array<int, GLViewportComponents>& viewport = camera.get_viewport();
            shader->set_uniform(kViewportSizeUniform, Vec2d(double(viewport[ViewportWidth]), double(viewport[ViewportHeight])));
            shader->set_uniform("width", 0.25f);
            shader->set_uniform(kGapSizeUniform, 0.0f);
#endif // ENABLE_GL_CORE_PROFILE
            render_grabbers_connection(GrabberCornerXMinYMin, GrabberCornerXMaxYMin, m_drag_color);
            render_grabbers_connection(GrabberCornerXMaxYMin, GrabberCornerXMaxYMax, m_drag_color);
            render_grabbers_connection(GrabberCornerXMaxYMax, GrabberCornerXMinYMax, m_drag_color);
            render_grabbers_connection(GrabberCornerXMinYMax, GrabberCornerXMinYMin, m_drag_color);
            shader->stop_using();
        }

        // draw grabbers
        shader = wxGetApp().get_shader("gouraud_light");
        if (shader != nullptr) {
            shader->start_using();
            shader->set_uniform("emission_factor", 0.1f);
            render_grabbers(GrabberFirstUniform, GrabberCount - 1, grabber_mean_size, true);
            shader->stop_using();
        }
    }
}

void GLGizmoScale3D::on_register_raycasters_for_picking()
{
    // the gizmo grabbers are rendered on top of the scene, so the raytraced picker should take it into account
    m_parent.set_raycaster_gizmos_on_top(true);
}

void GLGizmoScale3D::on_unregister_raycasters_for_picking()
{
    m_parent.set_raycaster_gizmos_on_top(false);
}

void GLGizmoScale3D::render_grabbers_connection(unsigned int id_1, unsigned int id_2, const ColorRGBA& color)
{
    auto grabber_connection = [this](unsigned int id_1, unsigned int id_2) {
        for (int i = 0; i < int(m_grabber_connections.size()); ++i) {
            if (m_grabber_connections[i].grabber_indices.first == id_1 && m_grabber_connections[i].grabber_indices.second == id_2)
                return i;
        }
        return -1;
    };

    const int id = grabber_connection(id_1, id_2);
    if (id == -1)
        return;

    if (!m_grabber_connections[id].model.is_initialized() ||
        !m_grabber_connections[id].old_v1.isApprox(m_grabbers[id_1].center) ||
        !m_grabber_connections[id].old_v2.isApprox(m_grabbers[id_2].center)) {
        m_grabber_connections[id].old_v1 = m_grabbers[id_1].center;
        m_grabber_connections[id].old_v2 = m_grabbers[id_2].center;
        m_grabber_connections[id].model.reset();

        GLModel::Geometry init_data;
        init_data.format = { GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3 };
        init_data.reserve_vertices(2);
        init_data.reserve_indices(2);

        // vertices
        init_data.add_vertex((Vec3f)m_grabbers[id_1].center.cast<float>());
        init_data.add_vertex((Vec3f)m_grabbers[id_2].center.cast<float>());

        // indices
        init_data.add_line(0, 1);

        m_grabber_connections[id].model.init_from(std::move(init_data));
    }

    m_grabber_connections[id].model.set_color(color);
    m_grabber_connections[id].model.render();
}

void GLGizmoScale3D::do_scale_along_axis(Axis axis, const UpdateData& data)
{
    double ratio = calc_ratio(data);
    if (ratio > 0.0) {
        Vec3d curr_scale = m_scale;
        curr_scale(axis) = m_starting.scale(axis) * ratio;
        m_scale = curr_scale;
    }
}

void GLGizmoScale3D::do_scale_uniform(const UpdateData & data)
{
    const double ratio = calc_ratio(data);
    if (ratio > 0.0)
        m_scale = m_starting.scale * ratio;
}

double GLGizmoScale3D::calc_ratio(const UpdateData& data) const
{
    double ratio = 0.0;
    const Vec3d starting_vec = m_starting.drag_position - m_starting.center;

    const double len_starting_vec = starting_vec.norm();

    if (len_starting_vec != 0.0) {
        const Vec3d mouse_dir = data.mouse_ray.unit_vector();
        // finds the intersection of the mouse ray with the plane parallel to the camera viewport and passing throught the starting position
        // use ray-plane intersection see i.e. https://en.wikipedia.org/wiki/Line%E2%80%93plane_intersection algebric form
        // in our case plane normal and ray direction are the same (orthogonal view)
        // when moving to perspective camera the negative z unit axis of the camera needs to be transformed in world space and used as plane normal
        const Vec3d inters = data.mouse_ray.a + (m_starting.drag_position - data.mouse_ray.a).dot(mouse_dir) * mouse_dir;
        // vector from the starting position to the found intersection
        const Vec3d inters_vec = inters - m_starting.drag_position;

        // finds projection of the vector along the staring direction
        const double proj = inters_vec.dot(starting_vec.normalized());

        ratio = (len_starting_vec + proj) / len_starting_vec;
    }

    if (wxGetKeyState(WXK_SHIFT))
        ratio = m_snap_step * static_cast<double>(std::round(ratio / m_snap_step));

    return ratio;
}

void GLGizmoScale3D::update_render_data()
{
    const Selection& selection = m_parent.get_selection();
    const auto& [box, box_trafo] = selection.get_bounding_box_in_current_reference_system();
    m_bounding_box = box;
    m_center = box_trafo.translation();
    m_grabbers_transform = box_trafo;
    m_instance_center = (selection.is_single_full_instance() || selection.is_single_volume_or_modifier()) ? selection.get_first_volume()->get_instance_offset() : m_center;

    const Vec3d box_half_size = 0.5 * m_bounding_box.size();
    bool use_constrain = wxGetKeyState(WXK_CONTROL);

    // Each grabber shows the constrained colour when the grabber OPPOSITE it is hovered, because
    // that is the one a Ctrl-drag will hold still - the same pairing constraint_id() encodes.

    // x axis
    m_grabbers[GrabberXMin].center = { -(box_half_size.x() + Offset), 0.0, 0.0 };
    m_grabbers[GrabberXMin].color = (use_constrain && m_hover_id == GrabberXMax) ? CONSTRAINED_COLOR : AXES_COLOR[X];
    m_grabbers[GrabberXMax].center = { box_half_size.x() + Offset, 0.0, 0.0 };
    m_grabbers[GrabberXMax].color = (use_constrain && m_hover_id == GrabberXMin) ? CONSTRAINED_COLOR : AXES_COLOR[X];

    // y axis
    m_grabbers[GrabberYMin].center = { 0.0, -(box_half_size.y() + Offset), 0.0 };
    m_grabbers[GrabberYMin].color = (use_constrain && m_hover_id == GrabberYMax) ? CONSTRAINED_COLOR : AXES_COLOR[Y];
    m_grabbers[GrabberYMax].center = { 0.0, box_half_size.y() + Offset, 0.0 };
    m_grabbers[GrabberYMax].color = (use_constrain && m_hover_id == GrabberYMin) ? CONSTRAINED_COLOR : AXES_COLOR[Y];

    // z axis
    m_grabbers[GrabberZMin].center = { 0.0, 0.0, -(box_half_size.z() + Offset) };
    m_grabbers[GrabberZMin].color = (use_constrain && m_hover_id == GrabberZMax) ? CONSTRAINED_COLOR : AXES_COLOR[Z];
    m_grabbers[GrabberZMax].center = { 0.0, 0.0, box_half_size.z() + Offset };
    m_grabbers[GrabberZMax].color = (use_constrain && m_hover_id == GrabberZMin) ? CONSTRAINED_COLOR : AXES_COLOR[Z];

    // uniform - the corners pair with the diagonal, not the neighbour
    m_grabbers[GrabberCornerXMinYMin].center = { -(box_half_size.x() + Offset), -(box_half_size.y() + Offset), 0.0 };
    m_grabbers[GrabberCornerXMinYMin].color = (use_constrain && m_hover_id == GrabberCornerXMaxYMax) ? CONSTRAINED_COLOR : m_highlight_color;
    m_grabbers[GrabberCornerXMaxYMin].center = { box_half_size.x() + Offset, -(box_half_size.y() + Offset), 0.0 };
    m_grabbers[GrabberCornerXMaxYMin].color = (use_constrain && m_hover_id == GrabberCornerXMinYMax) ? CONSTRAINED_COLOR : m_highlight_color;
    m_grabbers[GrabberCornerXMaxYMax].center = { box_half_size.x() + Offset, box_half_size.y() + Offset, 0.0 };
    m_grabbers[GrabberCornerXMaxYMax].color = (use_constrain && m_hover_id == GrabberCornerXMinYMin) ? CONSTRAINED_COLOR : m_highlight_color;
    m_grabbers[GrabberCornerXMinYMax].center = { -(box_half_size.x() + Offset), box_half_size.y() + Offset, 0.0 };
    m_grabbers[GrabberCornerXMinYMax].color = (use_constrain && m_hover_id == GrabberCornerXMaxYMin) ? CONSTRAINED_COLOR : m_highlight_color;

    for (int i = 0; i < GrabberCount; ++i) {
        m_grabbers[i].matrix = m_grabbers_transform;
    }
}

} // namespace GUI
} // namespace Slic3r