#ifndef slic3r_GCode_FanMover_hpp_
#define slic3r_GCode_FanMover_hpp_


#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/GCode/GCodeWriter.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/libslic3r.h"

#include <regex>

namespace Slic3r {

class BufferData {
public:
    //raw string, contains end position
    std::string raw;
    // time to go from start to end
    float time;
    int16_t fan_speed;
    bool is_kickstart;
    bool force_emit;
    // start position
    float x = 0, y = 0, z = 0, e = 0;
    // delta to go to end position
    float dx = 0, dy = 0, dz = 0, de = 0;
    BufferData(const std::string& line, float time = 0, int16_t fan_speed = 0, bool is_kickstart = false, bool force_emit = false)
        : raw(line), time(time), fan_speed(fan_speed), is_kickstart(is_kickstart), force_emit(force_emit) {
        //avoid double \n
        if(!raw.empty() && raw.back() == '\n') raw.pop_back();
    }
};

class FanMover
{
private:
    const std::regex regex_fan_speed;
    const float nb_seconds_delay; // in s
    const bool with_D_option;
    const bool relative_e;
    const bool only_overhangs;
    const float kickstart; // in s
    // When true, instead of pre-starting the overhang fan early, keep the fan command at
    // the overhang and slow the approach moves so the fan reaches target speed in time.
    const bool slowdown_for_fan;
    // Floor for the approach slowdown (= overhangs_speed). If percent, it is a fraction of
    // the approach move's own speed; otherwise an absolute mm/s value. The approach is never
    // slowed below this.
    const float overhang_speed_value;
    const bool overhang_speed_percent;

    GCodeReader m_parser{};
    const GCodeWriter& m_writer;

    //current value (at the back of the buffer), when parsing a fresh line
    GCodeExtrusionRole current_role = GCodeExtrusionRole::Custom;
    // in unit/second
    double m_current_speed = 1000 / 60.0;
    bool m_is_custom_gcode = false;
    uint16_t m_current_extruder = 0;

    // variable for when you add a line (front of the buffer)
    int m_front_buffer_fan_speed = 1;
    int m_back_buffer_fan_speed = 1;
    int m_output_fan_speed = 1;
    int m_last_overhang_min_fan_speed = -1;
    int m_overhang_fan_hold_speed = -1;
    bool m_overhang_fan_hold_until_extrusion = false;
    // Fan target suppressed by the overhang hold (a fan decrease that arrived during the
    // post-overhang transition gap, before the next extrusion). It is deferred and re-applied
    // when extrusion resumes so the fan drops to the intended feature speed instead of being
    // lost -- losing it left the fan pinned at the overhang speed for the whole next feature
    // (e.g. a 100% dynamic-overhang override smearing 100% fan across normal print).
    int m_overhang_fan_hold_pending_speed = -1;
    std::string m_overhang_fan_hold_pending_raw;
    BufferData m_current_kickstart{"",-1,0};
    float m_current_kickstart_duration = 0;
    // Continues an overhang fan-readiness slowdown on moves after the SET_FAN_SPEED marker
    // when the delay buffer does not contain enough approach distance.
    float m_fan_slowdown_remaining = 0.f;
    float m_fan_slowdown_total     = 0.f;
    float m_fan_slowdown_approach_speed = 0.f;
    float m_fan_slowdown_v_floor   = 0.f;
    // True after the first step-up fan command in the current ;TYPE:Overhang perimeter block.
    // Adaptive graph markers after that always emit at the marker; approach F-slowdown runs once.
    bool m_overhang_block_approach_slowdown_done = false;

    //buffer
    std::list<BufferData> m_buffer;
    double m_buffer_time_size = 0;

    // The output of process_layer()
    std::string m_process_output;
    std::string m_pending_output_fan_command;

public:
    FanMover(const GCodeWriter& writer, const float nb_seconds_delay, const bool with_D_option, const bool relative_e,
        const bool only_overhangs, const float kickstart, const bool slowdown_for_fan = false,
        const float overhang_speed_value = 0.f, const bool overhang_speed_percent = false)
        : regex_fan_speed("S[0-9]+"), 
        nb_seconds_delay(nb_seconds_delay>0 ? std::max(0.01f,nb_seconds_delay) : 0),
        with_D_option(with_D_option)
        , relative_e(relative_e), only_overhangs(only_overhangs), kickstart(kickstart), slowdown_for_fan(slowdown_for_fan)
        , overhang_speed_value(overhang_speed_value), overhang_speed_percent(overhang_speed_percent), m_writer(writer){}

    // Adds the gcode contained in the given string to the analysis and returns it after removing the workcodes
    const std::string& process_gcode(const std::string& gcode, bool flush);

private:
    BufferData& put_in_buffer(BufferData&& data) {
        assert(data.time >= 0 && data.time < 1000000 && !std::isnan(data.time));
         m_buffer_time_size += data.time;
        if (data.fan_speed >= 0 && !m_buffer.empty() && m_buffer.back().fan_speed >= 0) {
            // erase last item
            m_buffer.back() = data;
        } else {
            m_buffer.emplace_back(data);
        }
        return m_buffer.back();
    }
    std::list<BufferData>::iterator remove_from_buffer(std::list<BufferData>::iterator data) {
        assert(data->time >= 0 && data->time < 1000000 && !std::isnan(data->time));
        m_buffer_time_size -= data->time;
        return m_buffer.erase(data);
    }
    // Processes the given gcode line
    void _process_gcode_line(GCodeReader& reader, const GCodeReader::GCodeLine& line);
    void _process_ACTIVATE_EXTRUDER(const std::string_view command);
    void _process_T(const std::string_view command);
    void _handle_g_command(GCodeReader& reader, const GCodeReader::GCodeLine& line, double& time, int16_t& fan_speed);
    void _handle_m_command(GCodeReader& reader, const GCodeReader::GCodeLine& line, double& time, int16_t& fan_speed, bool& need_flush);
    void _put_in_middle_G1(std::list<BufferData>::iterator item_to_split, float nb_sec, BufferData&& line_to_write, float max_time);
    void _print_in_middle_G1(BufferData& line_to_split, float nb_sec, const std::string& line_to_write);
    void _remove_slow_fan(int16_t min_speed, float past_sec, bool include_kickstart_targets = false);
    void _append_fan_command(const std::string& gcode, int16_t fan_speed);
    void _append_gcode_line(const std::string& gcode);
    void _drop_immediate_lower_fan_commands();
    void write_buffer_data();
    void _coalesce_redundant_buffer_fans();
    void _insert_buffered_fan(std::list<BufferData>::iterator pos, BufferData &&data);
    void _queue_fan_at_marker(const std::string &gcode, int16_t fan_speed, bool force_emit = false);
    std::string _set_fan(int16_t speed, std::string_view comment);
    int16_t _fan_speed_percent(const std::string &gcode) const;
    bool _apply_active_fan_slowdown(BufferData &data);
    float _fan_slowdown_target_speed(float elapsed) const;
    // Seconds needed for the fan to spin from from_speed to to_speed, derived from
    // fan_speedup_time and fan_kickstart (both scale with the speed delta).
    float _fan_spinup_time_seconds(int16_t from_speed, int16_t to_speed) const;
    int16_t _kickstart_min_delta() const;

    // Member functions extracted from _process_gcode_line to address BP1013 (long 'if' 362 lines / 'switch' 216 lines)
    // and BP1015 (nesting 7 levels). The G case (move dist/time calc + overhang hold re-apply) and
    // the entire M fan processing (kickstart thresholds to avoid noise, slowdowns, hold pending for
    // extrusion alignment via buffer, erase lower fans, cherry-pick, "can't place in buffer -> current_kickstart")
    // are now in focused helpers. All owner comments inside were read for intent and preserved.
    // See the original local lambda comments and the detailed kickstart/hold comments for rationale.
    void _handle_g_command(const std::string& cmd, GCodeReader& reader, const GCodeReader::GCodeLine& line, double& time);
    void _handle_m_command(const std::string& cmd, const GCodeReader::GCodeLine& line, int16_t& fan_speed, double& time, bool& need_flush);

    // Further extracted from the "; overhang fan : SET_FAN_SPEED" marker handling inside
    // _process_gcode_line to address BP1015 (nesting up to 9 levels).
    void _process_overhang_fan_marker(GCodeReader& reader, const GCodeReader::GCodeLine& line);
    void _apply_overhang_approach_slowdown(int overhang_fan_speed);
    // Slows buffered approach moves within t_req seconds of the buffer's back towards v_floor,
    // ramping from approach_speed. Returns the total time actually covered by adjusted moves.
    float _slow_buffer_approach_moves(float t_req, float v_floor, float approach_speed);
    // Scans m_process_output backwards for the fan speed of the last emitted M106/M107 line.
    int16_t _find_last_emitted_fan_speed() const;

    // Further extracted from _handle_m_command's immediately-invoked "handle_delayed_kickstart"
    // lambda to address BP1015 (nesting up to 10 levels). Delays this M106 by kickstarting the
    // fan target from the speed already in the delay buffer (or from a kickstart already
    // running), so a big fan increase reaches speed by the time this line reaches the front.
    void _handle_delayed_kickstart(const GCodeReader::GCodeLine& line, int16_t fan_speed, int fan_baseline, double& time);
    // The non-delayed-buffer fan-increase path: stop or extend an in-flight kickstart, or
    // start a new deferred kickstart for this M106, depending on kickstart_min_delta.
    void _cherry_pick_kickstart(const GCodeReader::GCodeLine& line, int16_t fan_speed, double& time);
};

} // namespace Slic3r


#endif /* slic3r_GCode_FanMover_hpp_ */
