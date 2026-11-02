#ifndef ORIENT_HPP
#define ORIENT_HPP

#include "libslic3r/Model.hpp"

namespace Slic3r {

namespace orientation {


/// A logical bed representing an object not being orientd. Either the orient
/// has not yet successfully run on this OrientPolygon or it could not fit the
/// object due to overly large size or invalid geometry.
static const constexpr int UNORIENTD = -1;

/// Input/Output structure for the orient() function. The mesh field will not
/// be modified during orientment. Instead, the translation and rotation fields
/// will mark the needed transformation for the polygon to be in the orientd
/// position. These can also be set to an initial offset and rotation.
///
/// The bed_idx field will indicate the logical bed into which the
/// polygon belongs: UNORIENTD means no place for the polygon
/// (also the initial state before orient), 0..N means the index of the bed.
/// Zero is the physical bed, larger than zero means a virtual bed.
struct OrientMesh {
    TriangleMesh mesh;              /// The real mesh data
    double overhang_angle = 30;
    double angle{ 0 };
    Vec3d axis{ 0,0,1 };
    Vec3d orientation{ 0,0,1 };
    Matrix3d rotation_matrix;
    Vec3d euler_angles;
    std::string name;

    /// Optional setter function which can store arbitrary data in its closure
    std::function<void(const OrientMesh&)> setter = nullptr;

    /// Helper function to call the setter with the orient data arguments
    void apply() const { if (setter) setter(*this); }

};

// params for minimizing support area
struct OrientParamsArea {
    float TAR_A = 0.015f;
    float TAR_B = 0.177f;
    float RELATIVE_F = 20;
    float CONTOUR_F = 0.5f;
    float BOTTOM_F = 2.5f;
    float BOTTOM_HULL_F = 0.1f;
    float TAR_C = 0.1f;
    float TAR_D = 1;
    float TAR_E = 0.0115f;
    float FIRST_LAY_H = 0.2f;//0.0475;
    float VECTOR_TOL = -0.00083f;
    float NEGL_FACE_SIZE = 0.01f;
    float ASCENT = -0.5f;
    float PLAFOND_ADV = 0.0599f;
    float CONTOUR_AMOUNT = 0.0182427f;
    float OV_H = 2.574f;
    float height_offset = 2.3728f;
    float height_log = 0.041375f;
    float height_log_k = 1.9325457f;
    float LAF_MAX = 0.999f; // cos(1.4\degree) for low angle face 0.9997f
    float LAF_MIN = 0.97f;  // cos(14\degree) 0.9703f
    float TAR_LAF = 0.001f; //0.01f
    float TAR_PROJ_AREA = 0.1f;
    float BOTTOM_MIN = 0.1f;  // min bottom area. If lower than it the object may be unstable
    float BOTTOM_MAX = 2000;  // max bottom area. If get to it the object is stable enough (further increase bottom area won't do more help)
    float height_to_bottom_hull_ratio_MIN = 1;
    float BOTTOM_HULL_MAX = 2000;// max bottom hull area
    float APPERANCE_FACE_SUPP=3; // penalty of generating supports on appearance face

    float overhang_angle = 60.f;
    bool use_low_angle_face = true;
    bool min_volume = false;
    Eigen::Vector3f fun_dir = Eigen::Vector3f::Zero();

    /// Allow parallel execution.
    bool parallel = true;

    /// Progress indicator callback called when an object gets packed.
    /// The unsigned argument is the number of items remaining to pack.
    std::function<void(unsigned, std::string)> progressind = {};

    /// A predicate returning true if abort is needed.
    std::function<bool(void)>     stopcondition = {};

    OrientParamsArea() = default;
};

struct OrientParams {
    float TAR_A = 0.01f;//0.128f;
    float TAR_B = 0.177f;
    float RELATIVE_F= 6.610621027964314f;
    float CONTOUR_F = 0.23228623269775997f;
    float BOTTOM_F = 1.167152017941474f;
    float BOTTOM_HULL_F = 0.1f;
    float TAR_C = 0.24308070476924726f;
    float TAR_D = 0.6284515508160871f;
    float TAR_E = 0;//0.032157292647062234;
    float FIRST_LAY_H = 0.2f;//0.029;
    float VECTOR_TOL = -0.0011163303070972383f;
    float NEGL_FACE_SIZE = 0.1f;
    float ASCENT= -0.5f;
    float PLAFOND_ADV = 0.04079208948120519f;
    float CONTOUR_AMOUNT = 0.0101472219892684f;
    float OV_H = 1.0370178217794535f;
    float height_offset = 2.7417608343142073f;
    float height_log = 0.06442030687034085f;
    float height_log_k = 0.3933594673063997f;
    float LAF_MAX = 0.999f; // cos(1.4\degree) for low angle face //0.9997f;
    float LAF_MIN= 0.9703f;  // cos(14\degree) 0.9703f;
    float TAR_LAF = 0.01f; //0.1f
    float TAR_PROJ_AREA = 0.1f;
    float BOTTOM_MIN = 0.1f;  // min bottom area. If lower than it the objects may be unstable
    float BOTTOM_MAX = 2000; //400
    float height_to_bottom_hull_ratio_MIN = 1;
    float BOTTOM_HULL_MAX = 2000;// max bottom hull area to clip //600
    float APPERANCE_FACE_SUPP=3; // penalty of generating supports on appearance face

    float overhang_angle = 60.f;
    bool use_low_angle_face = true;
    bool min_volume = false;
    Eigen::Vector3f fun_dir = Eigen::Vector3f::Zero();


    /// Allow parallel execution.
    bool parallel = false;

    /// Progress indicator callback called when an object gets packed.
    /// The unsigned argument is the number of items remaining to pack.
    std::function<void(unsigned, std::string)> progressind = {};

    /// A predicate returning true if abort is needed.
    std::function<bool(void)>     stopcondition = {};

    static OrientParams for_minimum_support_area()
    {
        const OrientParamsArea area;
        OrientParams params;
        params.TAR_A                           = area.TAR_A;
        params.TAR_B                           = area.TAR_B;
        params.RELATIVE_F                      = area.RELATIVE_F;
        params.CONTOUR_F                       = area.CONTOUR_F;
        params.BOTTOM_F                        = area.BOTTOM_F;
        params.BOTTOM_HULL_F                   = area.BOTTOM_HULL_F;
        params.TAR_C                           = area.TAR_C;
        params.TAR_D                           = area.TAR_D;
        params.TAR_E                           = area.TAR_E;
        params.FIRST_LAY_H                     = area.FIRST_LAY_H;
        params.VECTOR_TOL                      = area.VECTOR_TOL;
        params.NEGL_FACE_SIZE                  = area.NEGL_FACE_SIZE;
        params.ASCENT                          = area.ASCENT;
        params.PLAFOND_ADV                     = area.PLAFOND_ADV;
        params.CONTOUR_AMOUNT                  = area.CONTOUR_AMOUNT;
        params.OV_H                            = area.OV_H;
        params.height_offset                   = area.height_offset;
        params.height_log                      = area.height_log;
        params.height_log_k                    = area.height_log_k;
        params.LAF_MAX                         = area.LAF_MAX;
        params.LAF_MIN                         = area.LAF_MIN;
        params.TAR_LAF                         = area.TAR_LAF;
        params.TAR_PROJ_AREA                   = area.TAR_PROJ_AREA;
        params.BOTTOM_MIN                      = area.BOTTOM_MIN;
        params.BOTTOM_MAX                      = area.BOTTOM_MAX;
        params.height_to_bottom_hull_ratio_MIN = area.height_to_bottom_hull_ratio_MIN;
        params.BOTTOM_HULL_MAX                 = area.BOTTOM_HULL_MAX;
        params.APPERANCE_FACE_SUPP              = area.APPERANCE_FACE_SUPP;
        params.overhang_angle                  = area.overhang_angle;
        params.use_low_angle_face              = area.use_low_angle_face;
        params.min_volume                      = area.min_volume;
        params.fun_dir                         = area.fun_dir;
        params.parallel                        = area.parallel;
        params.progressind                     = area.progressind;
        params.stopcondition                   = area.stopcondition;
        return params;
    }

    OrientParams() = default;
};

using OrientMeshs = std::vector<OrientMesh>;

/**
 * \brief Orients the input polygons.

 * \param items Input vector of OrientMeshs. The transformation, rotation
 * and bin_idx fields will be changed after the call finished and can be used
 * to apply the result on the input polygon.
 */
void orient(OrientMeshs &items, const OrientMeshs &excludes, const OrientParams &params = {});

// this function should be deleted, since rotating objects are so complicated that its inherited transformation may be a trouble
void orient(ModelObject* obj);

void orient(ModelInstance* instance);

}} // namespace Slic3r::orientment

#endif // MODELORIENT_HPP
