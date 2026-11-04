#ifndef slic3r_BridgeOverInfill_hpp_
#define slic3r_BridgeOverInfill_hpp_

#include "Flow.hpp"
#include "Line.hpp"
#include "Polygon.hpp"

namespace Slic3r {

// Grow a bridged area along the bridge direction out to the anchors.
// A run of only a few lines that stretches past the rest of the bridge is
// pulled back. An anchor the whole bridge reaches together is kept.
Polygons expand_bridged_area_to_anchors(Polygons bridged_area, Lines anchors, const Flow &bridging_flow, double bridging_angle);

} // namespace Slic3r

#endif // slic3r_BridgeOverInfill_hpp_
