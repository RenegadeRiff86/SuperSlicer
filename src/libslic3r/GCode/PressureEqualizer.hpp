///|/ Copyright (c) Prusa Research 2016 - 2023 Vojtěch Bubník @bubnikv, Lukáš Hejl @hejllukas
///|/ Copyright (c) SuperSlicer 2023 Remi Durand @supermerill
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_GCode_PressureEqualizer_hpp_
#define slic3r_GCode_PressureEqualizer_hpp_

#include "../libslic3r.h"
#include "../PrintConfig.hpp"
#include "../ExtrusionRole.hpp"

#include <memory>
#include <queue>

namespace Slic3r {

struct LayerResult;

class GCodeG1Formatter;

//#define PRESSURE_EQUALIZER_STATISTIC
//#define PRESSURE_EQUALIZER_DEBUG

// Processes a G-code. Finds changes in the volumetric extrusion speed and adjusts the transitions
// between these paths to limit fast changes in the volumetric extrusion speed.
class PressureEqualizer
{
public:
    PressureEqualizer() = delete;
    explicit PressureEqualizer(const Slic3r::GCodeConfig &config);
    ~PressureEqualizer();

    // Process a next batch of G-code lines.
    // The last LayerResult must be LayerResult::make_nop_layer_result() because it always returns GCode for the previous layer.
    // When process_layer is called for the first layer, then LayerResult::make_nop_layer_result() is returned.
    LayerResult process_layer(LayerResult &&input);
private:

    void process_layer(const std::string &gcode);

#ifdef PRESSURE_EQUALIZER_STATISTIC
    struct Statistics
    {
        void reset()
        {
            volumetric_extrusion_rate_min = std::numeric_limits<float>::max();
            volumetric_extrusion_rate_max = 0.f;
            volumetric_extrusion_rate_avg = 0.f;
            extrusion_length              = 0.f;
        }
        void update(float volumetric_extrusion_rate, float length)
        {
            volumetric_extrusion_rate_min  = std::min(volumetric_extrusion_rate_min, volumetric_extrusion_rate);
            volumetric_extrusion_rate_max  = std::max(volumetric_extrusion_rate_max, volumetric_extrusion_rate);
            volumetric_extrusion_rate_avg += volumetric_extrusion_rate * length;
            extrusion_length              += length;
        }
        float volumetric_extrusion_rate_min;
        float volumetric_extrusion_rate_max;
        float volumetric_extrusion_rate_avg;
        float extrusion_length;
    };

    struct Statistics m_stat;
#endif

    // Private configuration values
    // How fast could the volumetric extrusion rate increase / decrase? mm^3/sec^2
    struct ExtrusionRateSlope {
        float positive;
        float negative;
    };
    ExtrusionRateSlope              m_max_volumetric_extrusion_rate_slopes[size_t(GCodeExtrusionRole::Count)];
    float                           m_max_volumetric_extrusion_rate_slope_positive;
    float                           m_max_volumetric_extrusion_rate_slope_negative;

    // Configuration extracted from config.
    // Area of the crossestion of each filament. Necessary to calculate the volumetric flow rate.
    std::vector<float>              m_filament_crossections;

    // Internal data.
    // X,Y,Z,E,F
    static constexpr size_t         position_component_count = 5;
    float                           m_current_pos[position_component_count];
    size_t                          m_current_extruder;
    std::vector<std::string>        m_extruder_names;
    GCodeExtrusionRole              m_current_extrusion_role;
    bool                            m_retracted;
    bool                            m_use_relative_e_distances;
    int                             m_gcode_precision_xyz = 3;
    int                             m_gcode_precision_e = 5;

    // Indicate if extrude set speed block was opened using the tag ";_EXTRUDE_SET_SPEED"
    // or not (not opened, or it was closed using the tag ";_EXTRUDE_END").
    bool                            opened_extrude_set_speed_block = false;

    enum GCodeLineType {
        GCODELINETYPE_INVALID,
        GCODELINETYPE_NOOP,
        GCODELINETYPE_OTHER,
        GCODELINETYPE_RETRACT,
        GCODELINETYPE_UNRETRACT,
        GCODELINETYPE_TOOL_CHANGE,
        GCODELINETYPE_MOVE,
        GCODELINETYPE_EXTRUDE,
    };

    struct GCodeLine
    {
        static constexpr size_t x_component = 0;
        static constexpr size_t y_component = 1;
        static constexpr size_t z_component = 2;
        static constexpr size_t e_component = 3;
        static constexpr size_t feedrate_component = 4;

        GCodeLine() : 
            type(GCODELINETYPE_INVALID),
            raw_length(0),
            modified(false),
            extruder_id(0), 
            volumetric_extrusion_rate(0.f), 
            volumetric_extrusion_rate_start(0.f), 
            volumetric_extrusion_rate_end(0.f) 
            {}

        bool        moving_xy()     const { return fabs(pos_end[x_component] - pos_start[x_component]) > 0.f || fabs(pos_end[y_component] - pos_start[y_component]) > 0.f; }
        bool        moving_z ()     const { return fabs(pos_end[z_component] - pos_start[z_component]) > 0.f; }
        bool        extruding()     const { return moving_xy() && pos_end[e_component] > pos_start[e_component]; }
        bool        retracting()    const { return pos_end[e_component] < pos_start[e_component]; }
        bool        deretracting()  const { return ! moving_xy() && pos_end[e_component] > pos_start[e_component]; }

        float dist_xy2() const
        {
            const float delta_x = pos_end[x_component] - pos_start[x_component];
            const float delta_y = pos_end[y_component] - pos_start[y_component];
            return delta_x * delta_x + delta_y * delta_y;
        }
        float dist_xyz2() const
        {
            const float delta_z = pos_end[z_component] - pos_start[z_component];
            return dist_xy2() + delta_z * delta_z;
        }
        float       dist_xy()       const { return sqrt(dist_xy2()); }
        float       dist_xyz()      const { return sqrt(dist_xyz2()); }
        float       dist_e()        const { return fabs(pos_end[e_component] - pos_start[e_component]); }

        float       feedrate()      const { return pos_end[feedrate_component]; }
        float       time()          const { return dist_xyz() / feedrate(); }
        float       time_inv()      const { return feedrate() / dist_xyz(); }
        float       volumetric_correction_avg() const { 
            float avg_correction = 0.5f * (volumetric_extrusion_rate_start + volumetric_extrusion_rate_end) / volumetric_extrusion_rate; 
            assert(avg_correction > 0.f);
            assert(avg_correction <= 1.00000001f);
            return avg_correction;
        }
        float       time_corrected()  const { return time() * volumetric_correction_avg(); }

        GCodeLineType type;

        // We try to keep the string buffer once it has been allocated, so it will not be reallocated over and over.
        std::vector<char>   raw;
        size_t              raw_length;
        // If modified, the raw text has to be adapted by the new extrusion rate,
        // or maybe the line needs to be split into multiple lines.
        bool                modified;

        // X,Y,Z,E,F. Storing the state of the currently active extruder only.
        float       pos_start[position_component_count]{};
        float       pos_end[position_component_count]{};
        // Was the axis found on the G-code line? X,Y,Z,E,F
        bool        pos_provided[position_component_count]{};

        // Index of the active extruder.
        size_t      extruder_id;
        // Extrusion role of this segment.
        GCodeExtrusionRole extrusion_role { GCodeExtrusionRole::None };

        // Current volumetric extrusion rate.
        float       volumetric_extrusion_rate;
        // Volumetric extrusion rate at the start of this segment.
        float       volumetric_extrusion_rate_start;
        // Volumetric extrusion rate at the end of this segment.
        float       volumetric_extrusion_rate_end;

        // Volumetric extrusion rate slope limiting this segment.
        // If set to zero, the slope is unlimited.
        float       max_volumetric_extrusion_rate_slope_positive = 0.f;
        float       max_volumetric_extrusion_rate_slope_negative = 0.f;

        bool        adjustable_flow       = false;

        bool        extrude_set_speed_tag = false;
        bool        extrude_end_tag       = false;
    };

    // Output buffer will only grow. It will not be reallocated over and over.
    std::vector<char>               output_buffer;
    size_t                          output_buffer_length;
    size_t                          output_buffer_prev_length;

#ifdef PRESSURE_EQUALIZER_DEBUG
    // For debugging purposes. Index of the G-code line processed.
    size_t                          line_idx;
#endif

    bool process_line(const char *line, const char *line_end, GCodeLine &buf);
    void parse_axis_values(const char *&line, const char *line_end, float *new_pos, bool *changed, GCodeLine &buf);
    void process_gcode(int gcode, const char *&line, const char *line_end, GCodeLine &buf,
                       bool found_extrude_set_speed_tag, bool found_extrude_end_tag);
    void output_gcode_line(size_t line_idx);
    void parse_activate_extruder(const std::string&);

    // Go back from the current circular_buffer_pos and lower the feedtrate to decrease the slope of the extrusion rate changes.
    // Then go forward and adjust the feedrate to decrease the slope of the extrusion rate changes.
    void adjust_volumetric_rate();

    // Push the text to the end of the output_buffer.
    inline void push_to_output(GCodeG1Formatter &formatter);
    inline void push_to_output(const std::string &text, bool add_eol);
    inline void push_to_output(const char *text, size_t len, bool add_eol = true);
    // Push a G-code line to the output.
    void push_line_to_output(size_t line_idx, float new_feedrate, const char *comment);

public:
    std::queue<std::unique_ptr<LayerResult>> m_layer_results;

    std::vector<GCodeLine> m_gcode_lines;
};

} // namespace Slic3r

#endif /* slic3r_GCode_PressureEqualizer_hpp_ */
