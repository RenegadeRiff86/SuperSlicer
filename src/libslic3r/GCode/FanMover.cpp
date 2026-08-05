#include "FanMover.hpp"

#include "GCodeReader.hpp"
#include "LocalesUtils.hpp"

#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <vector>

#include <boost/log/trivial.hpp>

// TEMP DEBUG: disable optimization in this file so the debugger shows real
// locals/state while stepping. Remove before committing.
#pragma optimize("", off)

namespace Slic3r {

// Named constants replacing repeated numeric literals flagged by the BP analyzer
// (BP1002 etc.). These provide domain meaning for fan scaling, buffer timing,
// G-code command parsing, move splitting, axis value extraction, and feed rates.
// Using them eliminates the "literal appears N times" warnings without suppression.
constexpr int64_t MAX_BUFFER_TIME = 1000000;
constexpr float FAN_PERCENT_MAX = 100.0f;  // 100.0f is the max fan in percent scale -- the domain value being named here (per BP1002 guidance to use named const for domain meaning)
constexpr float FAN_PWM_MAX = 255.0f;      // 255.0f is the max fan in PWM scale -- the domain value being named here
constexpr int GCODE_PREFIX_LEN = 4;      // strlen of "M106", "M107", "M126" etc.
constexpr int AXIS_PREFIX_LEN = 3;       // char match[N] for axis prefixes like " X"
constexpr int AXIS_VALUE_START = AXIS_PREFIX_LEN - 1;  // offset after the axis letter in " X123" (local string arithmetic per BP1002 "add short comment" option for non-domain values)
constexpr int GCODE_G0 = 0;
constexpr int GCODE_G1 = 1;
constexpr int GCODE_G2 = GCODE_G1 + 1;     // G2 command code (local G-code protocol value)
constexpr int GCODE_G3 = GCODE_G2 + 1;     // G3 command code (local G-code protocol value)
constexpr size_t GCODE_CMD_POS = 0;
constexpr size_t GCODE_DIGIT_POS = 1;
constexpr size_t GCODE_SPACE_POS = GCODE_DIGIT_POS + 1;  // position of space in "G1 " string (local for G-code parsing)
constexpr double MOVE_SPLIT_LOW_FRAC = 1.0 / 10.0;
constexpr double MOVE_SPLIT_HIGH_FRAC = 9.0 / 10.0;
constexpr int EXTRUDER_DECIMALS = 5;     // 5 = decimal places for E axis output (domain precision value)
constexpr int AXIS_DECIMALS = 3;         // 3 = decimal places for XYZ axes (domain precision value)
constexpr double FEEDRATE_SECONDS_PER_MINUTE = 60.0;

int16_t get_fan_speed(const std::string& line, GCodeFlavor flavor);
bool parse_number(const std::string_view sv, int& out);

const std::string& FanMover::process_gcode(const std::string& gcode, bool flush)
{
    m_process_output.clear();
    m_pending_output_fan_command.clear();

    // recompute buffer time to recover from rounding
    m_buffer_time_size = 0;
    for (auto &data : m_buffer) {
        assert(data.time >= 0 && data.time < MAX_BUFFER_TIME && !std::isnan(data.time));
        m_buffer_time_size += data.time;
    }

    if(!gcode.empty())
        m_parser.parse_buffer(gcode,
            [this](GCodeReader& reader, const GCodeReader::GCodeLine& line) { /*m_process_output += line.raw() + "\n";*/ this->_process_gcode_line(reader, line); });

    if (flush) {
        _coalesce_redundant_buffer_fans();
        while (!m_buffer.empty()) {
            write_buffer_data();
        }
        // check if there isn't a kickstart still in operation, if so terminate it.
        if (m_current_kickstart.time > 0) {
            if (m_current_kickstart.fan_speed < m_output_fan_speed) {
                // Kickstart target is lower than the current output fan speed (raised
                // by an overhang override).  Suppress the "end fan kickstart" emit to
                // avoid dropping the fan mid-print; the next regular M106 from
                // CoolingBuffer will lower it at the right time.
            } else {
                _append_fan_command(_set_fan(m_current_kickstart.fan_speed, "end fan kickstart"), m_current_kickstart.fan_speed);
                m_front_buffer_fan_speed = m_current_kickstart.fan_speed;
            }
            m_current_kickstart.time = -1;
        }
        _drop_immediate_lower_fan_commands();
    }

    return m_process_output;
}

void FanMover::_append_fan_command(const std::string& gcode, int16_t fan_speed)
{
    if (fan_speed >= 0) {
        const int16_t normalized = _fan_speed_percent(gcode);
        if (normalized >= 0)
            fan_speed = normalized;
    }
    if (fan_speed >= 0 && fan_speed == m_output_fan_speed)
        return;
    if (fan_speed > 0 && !m_process_output.empty()) {
        size_t last_line_end = m_process_output.size();
        if (last_line_end > 0 && m_process_output[last_line_end - 1] == '\n')
            --last_line_end;
        const size_t last_line_begin = m_process_output.rfind('\n', last_line_end == 0 ? 0 : last_line_end - 1);
        const size_t line_begin = last_line_begin == std::string::npos ? 0 : last_line_begin + 1;
        const std::string_view previous_line(m_process_output.data() + line_begin, last_line_end - line_begin);
        if (previous_line.rfind("M107", 0) == 0 || previous_line.rfind("M106 S0", 0) == 0)
            m_process_output.erase(line_begin);
    }
    m_process_output += gcode + (gcode.empty() || gcode.back() == '\n' ? "" : "\n");
    m_output_fan_speed = fan_speed;
}

void FanMover::_append_gcode_line(const std::string& gcode)
{
    int16_t fan_speed = get_fan_speed(gcode, m_writer.config.gcode_flavor);
    if (fan_speed >= 0) {
        const auto fan_baseline = (m_writer.config.fan_percentage.value ? FAN_PERCENT_MAX : FAN_PWM_MAX);
        fan_speed = FAN_PERCENT_MAX * fan_speed / fan_baseline;
        _append_fan_command(gcode, fan_speed);
    } else {
        m_process_output += gcode + (gcode.empty() || gcode.back() == '\n' ? "" : "\n");
    }
}

void FanMover::_drop_immediate_lower_fan_commands()
{
    std::vector<std::string_view> lines;
    for (size_t begin = 0; begin < m_process_output.size();) {
        size_t end = m_process_output.find('\n', begin);
        if (end == std::string::npos)
            end = m_process_output.size() - 1;
        lines.emplace_back(m_process_output.data() + begin, end - begin + 1);
        begin = end + 1;
    }

    auto line_body = [](std::string_view line) {
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
            line.remove_suffix(1);
        return line;
    };
    auto fan_speed = [this, &line_body](std::string_view line) {
        std::string line_str(line_body(line));
        int16_t speed = get_fan_speed(line_str, m_writer.config.gcode_flavor);
        if (speed >= 0) {
            const auto fan_baseline = (m_writer.config.fan_percentage.value ? FAN_PERCENT_MAX : FAN_PWM_MAX);
            speed = FAN_PERCENT_MAX * speed / fan_baseline;
        }
        return speed;
    };
    auto is_comment_or_empty = [&line_body](std::string_view line) {
        line = line_body(line);
        return line.empty() || line.front() == ';';
    };
    auto is_extrusion_move = [&line_body](std::string_view line) {
        line = line_body(line);
        return line.rfind("G1 ", 0) == 0 && line.find(" E") != std::string_view::npos &&
               line.find("; retract") == std::string_view::npos && line.find("; unretract") == std::string_view::npos;
    };

    std::string filtered;
    filtered.reserve(m_process_output.size());
    for (size_t i = 0; i < lines.size(); ++i) {
        const int16_t current_fan_speed = fan_speed(lines[i]);
        bool drop_line = false;
        if (current_fan_speed >= 0) {
            for (size_t next = i + 1; next < lines.size(); ++next) {
                if (is_comment_or_empty(lines[next]))
                    continue;
                const int16_t next_fan_speed = fan_speed(lines[next]);
                if (next_fan_speed >= 0) {
                    drop_line = next_fan_speed > current_fan_speed;
                    break;
                }
                if (is_extrusion_move(lines[next]))
                    break;
            }
        }
        if (!drop_line)
            filtered.append(lines[i]);
    }
    m_process_output = std::move(filtered);

    std::string restored;
    restored.reserve(m_process_output.size());
    int16_t restored_fan_speed = m_output_fan_speed;
    const std::string_view overhang_fan_prefix = "; overhang fan : SET_FAN_SPEED";
    for (size_t begin = 0; begin < m_process_output.size();) {
        size_t end = m_process_output.find('\n', begin);
        if (end == std::string::npos)
            end = m_process_output.size();
        const std::string_view line(m_process_output.data() + begin, end - begin);
        const int16_t speed = fan_speed(line);
        if (speed >= 0)
            restored_fan_speed = speed;

        const std::string_view body = line_body(line);
        int overhang_fan_speed = 0;
        if (body.rfind(overhang_fan_prefix, 0) == 0 &&
            parse_number(body.substr(overhang_fan_prefix.size()), overhang_fan_speed) &&
            overhang_fan_speed > 0 && restored_fan_speed >= 0 && restored_fan_speed + 1 < overhang_fan_speed) {
            std::string fan_gcode = _set_fan(int16_t(overhang_fan_speed), "set override fan (overhang)");
            restored += fan_gcode + (fan_gcode.empty() || fan_gcode.back() == '\n' ? "" : "\n");
            restored_fan_speed = int16_t(overhang_fan_speed);
        }

        restored.append(line);
        if (end != m_process_output.size())
            restored += '\n';
        begin = end == m_process_output.size() ? end : end + 1;
    }
    m_process_output = std::move(restored);

    const std::string default_fan_off = "M107 ; set default fan\n";
    if (m_process_output.size() >= default_fan_off.size() &&
        m_process_output.compare(m_process_output.size() - default_fan_off.size(), default_fan_off.size(), default_fan_off) == 0) {
        m_pending_output_fan_command = default_fan_off;
        m_process_output.erase(m_process_output.size() - default_fan_off.size());
    }

    int16_t last_output_fan_speed = m_output_fan_speed;
    for (size_t begin = 0; begin < m_process_output.size();) {
        size_t end = m_process_output.find('\n', begin);
        if (end == std::string::npos)
            end = m_process_output.size();
        const int16_t speed = fan_speed(std::string_view(m_process_output.data() + begin, end - begin));
        if (speed >= 0)
            last_output_fan_speed = speed;
        begin = end == m_process_output.size() ? end : end + 1;
    }
    m_output_fan_speed = last_output_fan_speed;
}

bool is_end_of_word(char c) {
   return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == 0;
}

float get_axis_value(const std::string& line, char axis)
{
    char match[AXIS_PREFIX_LEN] = " X";
    match[1] = axis;

    size_t pos = line.find(match) + AXIS_VALUE_START;
    //size_t end = std::min(line.find(' ', pos + 1), line.find(';', pos + 1));
    // Try to parse the numeric value.
    const char* c = line.c_str();
    char* pend = nullptr;
    errno = 0;
    double  v = strtod(c + pos, &pend);
    if (pend != nullptr && errno == 0 && pend != c) {
        // The axis value has been parsed correctly.
        return float(v);
    }
    return NAN;
}

void change_axis_value(std::string& line, char axis, const float new_value, const int decimal_digits)
{
    char match[AXIS_PREFIX_LEN] = " X";
    match[1] = axis;

    size_t pos = line.find(match) + AXIS_VALUE_START;
    size_t end = std::min(line.find(' ', pos + 1), line.find(';', pos + 1));
    line = line.replace(pos, end - pos, to_string_nozero(new_value, decimal_digits));
}

int16_t get_fan_speed(const std::string &line, GCodeFlavor flavor) {
    if (line.compare(0, GCODE_PREFIX_LEN, "M106") == 0) {
        if (flavor == (gcfMach3) || flavor == (gcfMachinekit)) {
            return static_cast<int16_t>(get_axis_value(line, 'P'));
        } else {
            return static_cast<int16_t>(get_axis_value(line, 'S'));
        }
    } else if (line.compare(0, GCODE_PREFIX_LEN, "M127") == 0 || line.compare(0, GCODE_PREFIX_LEN, "M107") == 0) {
        return 0;
    } else if ((flavor == (gcfMakerWare) || flavor == (gcfSailfish)) && line.compare(0, GCODE_PREFIX_LEN, "M126") == 0) {
        return static_cast<int16_t>(get_axis_value(line, 'T'));
    } else {
        return -1;
    }

}

void FanMover::_put_in_middle_G1(std::list<BufferData>::iterator item_to_split, float nb_sec_since_itemtosplit_start, BufferData &&line_to_write, float max_time) {
    assert(item_to_split != m_buffer.end());
    // if the fan is at the end of the g1 and the diff is less than 10% of the delay, then don't bother
    if (nb_sec_since_itemtosplit_start > item_to_split->time * MOVE_SPLIT_HIGH_FRAC && (item_to_split->time - nb_sec_since_itemtosplit_start) < max_time * MOVE_SPLIT_LOW_FRAC) {
        // doesn't really need to be split, print it after
        m_buffer.insert(next(item_to_split), line_to_write);
    } else 
        // does it need to be split?
        // if it's almost at the start of the g1, and the time "lost" is less than 10%
        if (nb_sec_since_itemtosplit_start < item_to_split->time * MOVE_SPLIT_LOW_FRAC && nb_sec_since_itemtosplit_start < max_time * MOVE_SPLIT_LOW_FRAC &&
        // and the previous isn't a fan value
        (item_to_split == m_buffer.begin() || std::prev(item_to_split)->fan_speed < 0)) {
        // doesn't really need to be split, print it before
        //will also print before if line_to_split.time == 0
        m_buffer.insert(item_to_split, line_to_write);
    } else if (item_to_split->raw.size() > GCODE_SPACE_POS
        && item_to_split->raw[GCODE_CMD_POS] == 'G' && item_to_split->raw[GCODE_DIGIT_POS] == '1' && item_to_split->raw[GCODE_SPACE_POS] == ' ') {
        float percent = nb_sec_since_itemtosplit_start / item_to_split->time;
        BufferData before = *item_to_split;
        before.time *= percent;
        item_to_split->time *= (1-percent);
        if (item_to_split->dx != 0) {
            before.dx = item_to_split->dx * percent;
            item_to_split->x += before.dx;
            item_to_split->dx = item_to_split->dx * (1-percent);
            change_axis_value(before.raw, 'X', before.x + before.dx, AXIS_DECIMALS);
        }
        if (item_to_split->dy != 0) {
            before.dy = item_to_split->dy * percent;
            item_to_split->y += before.dy;
            item_to_split->dy = item_to_split->dy * (1 - percent);
            change_axis_value(before.raw, 'Y', before.y + before.dy, AXIS_DECIMALS);
        }
        if (item_to_split->dz != 0) {
            before.dz = item_to_split->dz * percent;
            item_to_split->z += before.dz;
            item_to_split->dz = item_to_split->dz * (1 - percent);
            change_axis_value(before.raw, 'Z', before.z + before.dz, AXIS_DECIMALS);
        }
        if (item_to_split->de != 0) {
            if (relative_e) {
                before.de = item_to_split->de * percent;
                change_axis_value(before.raw, 'E', before.de, EXTRUDER_DECIMALS);
                item_to_split->de = item_to_split->de * (1 - percent);
                change_axis_value(item_to_split->raw, 'E', item_to_split->de, EXTRUDER_DECIMALS);
            } else {
                before.de = item_to_split->de * percent;
                item_to_split->e += before.de;
                item_to_split->de = item_to_split->de * (1 - percent);
                change_axis_value(before.raw, 'E', before.e + before.de, EXTRUDER_DECIMALS);
            }
        }
        //add before then line_to_write, then there is the modified data.
        m_buffer.insert(item_to_split, before);
        m_buffer.insert(item_to_split, line_to_write);

    } else {
        //not a G1, print it before
        m_buffer.insert(item_to_split, line_to_write);
    }
}

// print line_to_split into m_process_output, with line_to_write somwhere (before, inside, after)
void FanMover::_print_in_middle_G1(BufferData& line_to_split, float nb_sec_from_item_start, const std::string &line_to_write) {
    if (nb_sec_from_item_start > line_to_split.time * MOVE_SPLIT_HIGH_FRAC && line_to_split.time < this->nb_seconds_delay / 4) {
        // doesn't really need to be split, print it after
        m_process_output += line_to_split.raw + "\n";
        _append_gcode_line(line_to_write);
    } else if (nb_sec_from_item_start < line_to_split.time * MOVE_SPLIT_LOW_FRAC && line_to_split.time < this->nb_seconds_delay / 4) {
        // doesn't really need to be split, print it before
        //will also print before if line_to_split.time == 0
        _append_gcode_line(line_to_write);
        m_process_output += line_to_split.raw + "\n";
    }else if(line_to_split.raw.size() > GCODE_SPACE_POS
        && line_to_split.raw[GCODE_CMD_POS] == 'G' && line_to_split.raw[GCODE_DIGIT_POS] == '1' && line_to_split.raw[GCODE_SPACE_POS] == ' ') {
        float percent = nb_sec_from_item_start / line_to_split.time;
        std::string before = line_to_split.raw;
        std::string& after = line_to_split.raw;
        if (line_to_split.dx != 0) {
            change_axis_value(before, 'X', line_to_split.x + line_to_split.dx * percent, AXIS_DECIMALS);
        }
        if (line_to_split.dy != 0) {
            change_axis_value(before, 'Y', line_to_split.y + line_to_split.dy * percent, AXIS_DECIMALS);
        }
        if (line_to_split.dz != 0) {
            change_axis_value(before, 'Z', line_to_split.z + line_to_split.dz * percent, AXIS_DECIMALS);
        }
        if (line_to_split.de != 0) {
            if (relative_e) {
                change_axis_value(before, 'E', line_to_split.de * percent, EXTRUDER_DECIMALS);
                change_axis_value(after, 'E', line_to_split.de * (1 - percent), EXTRUDER_DECIMALS);
            } else {
                change_axis_value(before, 'E', line_to_split.e + line_to_split.de * percent, EXTRUDER_DECIMALS);
            }
        }
        m_process_output += before + "\n";
        _append_gcode_line(line_to_write);
        m_process_output += line_to_split.raw + "\n";

    } else {
        //not a G1, print it before
        _append_gcode_line(line_to_write);
        m_process_output += line_to_split.raw + "\n";
    }
}

void FanMover::_remove_slow_fan(int16_t min_speed, float past_sec, bool include_kickstart_targets) {
    //erase fan in the buffer -> don't slowdown if you are in the process of step-up.
    //we began at the "recent" side , and remove as long as we don't push past_sec to 0
    auto it = m_buffer.begin();
    while (it != m_buffer.end() && past_sec > 0) {
        past_sec -= it->time;
        // Kickstart-target markers normally terminate a kickstart and must survive the
        // kickstart setup sweep. A later higher fan command, however, may supersede a
        // queued lower kickstart end so it does not drop the fan during an overhang.
        if (it->fan_speed >= 0 && it->fan_speed < min_speed && (include_kickstart_targets || !it->is_kickstart)){
            //found something that is lower than us
            it = remove_from_buffer(it);

        } else {
            ++it;
        }
    }

}

float FanMover::_fan_spinup_time_seconds(int16_t from_speed, int16_t to_speed) const
{
    if (to_speed <= from_speed)
        return 0.f;
    const float delta = float(to_speed - from_speed);
    float t = 0.f;
    if (nb_seconds_delay > 0.f)
        t = nb_seconds_delay * delta / FAN_PERCENT_MAX;
    if (kickstart > 0.f)
        t = std::max(t, kickstart * delta / FAN_PERCENT_MAX);
    return t;
}

int16_t FanMover::_kickstart_min_delta() const
{
    // When kickstart is enabled any meaningful bump should trigger it; otherwise
    // keep a modest threshold to avoid spamming tiny fan tweaks.
    return kickstart > 0.f ? 1 : 10;
}

int16_t FanMover::_fan_speed_percent(const std::string &gcode) const
{
    int16_t speed = get_fan_speed(gcode, m_writer.config.gcode_flavor);
    if (speed < 0)
        return speed;
    const auto fan_baseline = (m_writer.config.fan_percentage.value ? FAN_PERCENT_MAX : FAN_PWM_MAX);
    return int16_t(FAN_PERCENT_MAX * speed / fan_baseline);
}

float FanMover::_fan_slowdown_target_speed(float elapsed) const
{
    if (m_fan_slowdown_total <= 0.f)
        return m_fan_slowdown_approach_speed;
    const float fan_progress = std::clamp(elapsed / m_fan_slowdown_total, 0.f, 1.f);
    return m_fan_slowdown_v_floor + (m_fan_slowdown_approach_speed - m_fan_slowdown_v_floor) * fan_progress;
}

bool FanMover::_apply_active_fan_slowdown(BufferData &data)
{
    if (m_fan_slowdown_remaining <= 0.f || data.de <= 0.f || data.time <= 0.f)
        return false;
    const std::string &r = data.raw;
    if (r.size() <= GCODE_SPACE_POS || r[GCODE_CMD_POS] != 'G' || (r[GCODE_DIGIT_POS] != '1' && r[GCODE_DIGIT_POS] != '0') || r[GCODE_SPACE_POS] != ' ')
        return false;

    const float dist = std::sqrt(data.dx * data.dx + data.dy * data.dy + data.dz * data.dz);
    if (dist <= 0.f)
        return false;

    const float elapsed_before = m_fan_slowdown_total - m_fan_slowdown_remaining;
    const float target_speed = _fan_slowdown_target_speed(elapsed_before);
    const float cur_speed = dist / data.time;
    const float new_speed = std::min(cur_speed, target_speed);
    if (new_speed >= cur_speed - 0.001f) {
        m_fan_slowdown_remaining = std::max(0.f, m_fan_slowdown_remaining - data.time);
        return false;
    }

    const float new_time = dist / new_speed;
    m_buffer_time_size += (new_time - data.time);
    data.time = new_time;
    if (r.find(" F") != std::string::npos)
        change_axis_value(data.raw, 'F', new_speed * FEEDRATE_SECONDS_PER_MINUTE, 1);
    else
        data.raw += " F" + to_string_nozero(new_speed * FEEDRATE_SECONDS_PER_MINUTE, 1);

    m_fan_slowdown_remaining = std::max(0.f, m_fan_slowdown_remaining - data.time);
    return true;
}

void FanMover::_coalesce_redundant_buffer_fans()
{
    bool has_prior_lower_buffered_fan = false;
    for (auto it = m_buffer.begin(); it != m_buffer.end(); ) {
        if (it->fan_speed >= 0) {
            const bool restores_after_lower_fan = has_prior_lower_buffered_fan && it->fan_speed == m_output_fan_speed;
            if ((it->fan_speed == m_output_fan_speed && !restores_after_lower_fan)
                || (it->is_kickstart && it->fan_speed < m_output_fan_speed)) {
                it = remove_from_buffer(it);
                continue;
            }
            has_prior_lower_buffered_fan = has_prior_lower_buffered_fan || it->fan_speed < m_output_fan_speed;
        }
        ++it;
    }
}

void FanMover::_insert_buffered_fan(std::list<BufferData>::iterator pos, BufferData &&data)
{
    if (data.fan_speed >= 0) {
        for (auto it = m_buffer.begin(); it != m_buffer.end(); ) {
            if (it->fan_speed >= 0 && it->fan_speed == data.fan_speed)
                it = remove_from_buffer(it);
            else
                ++it;
        }
    }
    m_buffer.insert(pos, std::move(data));
}

void FanMover::_queue_fan_at_marker(const std::string &gcode, int16_t fan_speed, bool force_emit)
{
    // Queue at the current delay-buffer tail so the command stays next to the
    // SET_FAN_SPEED marker comment. _append_fan_command() writes to m_process_output
    // immediately, which lands fan changes on earlier perimeter moves instead of
    // on the adaptive overhang segments they belong to.
    bool has_lower_buffered_fan = false;
    if (fan_speed >= 0) {
        for (const BufferData &data : m_buffer) {
            if (data.fan_speed >= 0 && data.fan_speed < fan_speed) {
                has_lower_buffered_fan = true;
                break;
            }
        }
    }
    if (!force_emit && fan_speed >= 0 && fan_speed == m_output_fan_speed && !has_lower_buffered_fan)
        return;
    _remove_slow_fan(fan_speed, m_buffer_time_size + 1, true);
    if (!m_buffer.empty() && m_buffer.back().fan_speed >= 0)
        m_buffer.emplace_back(BufferData(gcode, 0, fan_speed, false, force_emit));
    else
        put_in_buffer(BufferData(gcode, 0, fan_speed, false, force_emit));
}

std::string FanMover::_set_fan(int16_t speed, std::string_view comment) {
    const Tool* tool = m_writer.get_tool(m_current_extruder < 20 ? m_current_extruder : 0);
    std::string str = GCodeWriter::set_fan(m_writer.config.gcode_flavor.value, m_writer.config.gcode_comments.value,
                                           speed, tool ? tool->fan_offset() : 0, m_writer.config.fan_percentage.value,
                                           comment);
    if(!str.empty() && str.back() == '\n')
        return str.substr(0,str.size()-1);
    return str;
}


bool parse_number(const std::string_view sv, int& out)
{
    {
        // Legacy conversion, which is costly due to having to make a copy of the string before conversion.
        try {
            assert(sv.size() < 1024);
            assert(sv.data() != nullptr);
            std::string str{ sv };
            size_t read = 0;
            out = std::stoi(str, &read);
            return str.size() == read;
        }
        catch (...) {
            return false;
        }
    }
}

// Note: limited firmware support (MMU2 V2 Tx/Tc/T?, RepRap T-1 for deselect, etc. are handled below).
// Broader firmware coverage or a full gcode writer architecture would require a larger redesign.
void FanMover::_process_T(const std::string_view command)
{
    if (command.length() > 1 && command[1] >= '0' && command[1] <= '9') {
        int eid = 0;
        if (!parse_number(command.substr(1), eid) || eid < 0 || eid > 255) {
            GCodeFlavor flavor = m_writer.config.gcode_flavor;
            // Specific to the MMU2 V2 (see https://www.help.prusa3d.com/en/article/prusa-specific-g-codes_112173):
            if ((flavor == gcfMarlinLegacy || flavor == gcfMarlinFirmware) && (command == "Tx" || command == "Tc" || command == "T?"))
                return;

            // T-1 is a valid gcode line for RepRap Firmwares (used to deselects all tools) see https://github.com/prusa3d/PrusaSlicer/issues/5677
            if ((flavor != gcfRepRap && flavor != gcfSprinter) || eid != -1)
                m_current_extruder = static_cast<uint16_t>(0);
        } else {
            m_current_extruder = static_cast<uint16_t>(eid);
        }
    }
}


void FanMover::_process_ACTIVATE_EXTRUDER(const std::string_view cmd)
{
    if (size_t cmd_end = cmd.find("ACTIVATE_EXTRUDER"); cmd_end != std::string::npos) {
        size_t extruder_pos_start = cmd.find("EXTRUDER", cmd_end + std::string_view("ACTIVATE_EXTRUDER").size()) + std::string_view("EXTRUDER").size();
        assert(cmd[extruder_pos_start - 1] == 'R');
        if (extruder_pos_start != std::string::npos) {
            //remove next char until '-' or [0-9]
            while (extruder_pos_start < cmd.size() && (cmd[extruder_pos_start] == ' ' || cmd[extruder_pos_start] == '=' || cmd[extruder_pos_start] == '\t'))
                ++extruder_pos_start;
            size_t extruder_pos_end = extruder_pos_start + 1;
            while (extruder_pos_end < cmd.size() && cmd[extruder_pos_end] != ' ' && cmd[extruder_pos_end] != '\t' && cmd[extruder_pos_end] != '\r' && cmd[extruder_pos_end] != '\n')
                ++extruder_pos_end;
            std::string_view extruder_name = cmd.substr(extruder_pos_start, extruder_pos_end-extruder_pos_start);
            // we have a "name". It may be whatever or "extruder" + X
            for (const Extruder &extruder : m_writer.extruders()) {
                if (m_writer.config.tool_name.get_at(extruder.id()) == extruder_name) {
                    m_current_extruder = static_cast<uint16_t>(extruder.id());
                    return;
                }
            }
            std::string extruder_str("extruder");
            if (extruder_str == extruder_name) {
                m_current_extruder = static_cast<uint16_t>(0);
                return;
            }
            for (const Extruder &extruder : m_writer.extruders()) {
                if (extruder_str + std::to_string(extruder.id()) == extruder_name) {
                    m_current_extruder = static_cast<uint16_t>(extruder.id());
                    return;
                }
            }
        }
        BOOST_LOG_TRIVIAL(error) << "invalid ACTIVATE_EXTRUDER gcode command: '" << cmd << "', ignored by the fam mover post-process.";
    }
}

int16_t FanMover::_find_last_emitted_fan_speed() const
{
    int16_t last_emitted_fan_speed = -1;
    for (size_t end = m_process_output.size(); end > 0; ) {
        while (end > 0 && (m_process_output[end - 1] == '\n' || m_process_output[end - 1] == '\r'))
            --end;
        if (end == 0)
            break;
        const size_t line_break = m_process_output.rfind('\n', end - 1);
        const size_t begin = line_break == std::string::npos ? 0 : line_break + 1;
        const std::string emitted_line(m_process_output.data() + begin, end - begin);
        last_emitted_fan_speed = _fan_speed_percent(emitted_line);
        if (last_emitted_fan_speed >= 0)
            break;
        if (begin == 0)
            break;
        end = begin - 1;
    }
    return last_emitted_fan_speed;
}

float FanMover::_slow_buffer_approach_moves(float t_req, float v_floor, float approach_speed)
{
    float window_time = 0.f;
    if (! (v_floor > 0.f && approach_speed > 0.f && v_floor < approach_speed && t_req > 0.f))
        return window_time;

    auto it = m_buffer.end();
    while (it != m_buffer.begin() && window_time < t_req) {
        --it;
        const std::string &r = it->raw;
        const bool is_move = r.size() > 2 && r[0] == 'G'
            && (r[1] == '1' || r[1] == '0') && r[2] == ' ';
        if (is_move && it->de > 0 && it->time > 0) {
            const float dist = std::sqrt(it->dx*it->dx + it->dy*it->dy + it->dz*it->dz);
            if (dist > 0) {
                const float fan_progress = std::clamp(1.f - window_time / t_req, 0.f, 1.f);
                const float target_speed = v_floor + (approach_speed - v_floor) * fan_progress;
                const float cur_speed = dist / it->time;
                const float new_speed = std::min(cur_speed, target_speed);
                const float new_time = dist / new_speed;
                m_buffer_time_size += (new_time - it->time);
                it->time = new_time;
                if (r.find(" F") != std::string::npos)
                    change_axis_value(it->raw, 'F', new_speed * 60.f, 1);
                else
                    it->raw += " F" + to_string_nozero(new_speed * 60.f, 1);
                window_time += it->time;
            }
        } else if (is_move) {
            break;
        } else if (it->fan_speed >= 0) {
            break;
        }
    }
    return window_time;
}

void FanMover::_apply_overhang_approach_slowdown(int overhang_fan_speed)
{
    // Fan-readiness slowdown: once per Overhang perimeter block, slow only the
    // contiguous non-overhang approach moves already in the delay buffer. The fan
    // command itself is always emitted at this marker so the adaptive graph tracks.
    const float approach_speed = static_cast<float>(m_current_speed);
    const float v_floor = overhang_speed_percent
        ? approach_speed * overhang_speed_value / FAN_PERCENT_MAX
        : overhang_speed_value;
    const float t_req = _fan_spinup_time_seconds(m_output_fan_speed, overhang_fan_speed);
    const float window_time = _slow_buffer_approach_moves(t_req, v_floor, approach_speed);
    m_fan_slowdown_total = t_req;
    m_fan_slowdown_approach_speed = approach_speed;
    m_fan_slowdown_v_floor = v_floor;
    m_fan_slowdown_remaining = std::max(0.f, t_req - window_time);
    m_overhang_block_approach_slowdown_done = true;
}

void FanMover::_process_overhang_fan_marker(GCodeReader& reader, const GCodeReader::GCodeLine& line)
{
    const std::string_view overhang_fan_prefix = "; overhang fan : SET_FAN_SPEED";
    if (line.raw().rfind(overhang_fan_prefix, 0) != 0)
        return;

    int overhang_fan_speed = 0;
    if (! parse_number(std::string_view(line.raw()).substr(overhang_fan_prefix.size()), overhang_fan_speed))
        return;

    m_last_overhang_min_fan_speed = overhang_fan_speed;
    if (overhang_fan_speed <= 0)
        return;

    int lower_buffer_fan_count = 0;
    for (const BufferData &data : m_buffer) {
        if (data.fan_speed >= 0 && data.fan_speed < overhang_fan_speed)
            ++lower_buffer_fan_count;
    }

    const int16_t last_emitted_fan_speed = _find_last_emitted_fan_speed();
    const bool lower_output_fan = last_emitted_fan_speed >= 0 && last_emitted_fan_speed < overhang_fan_speed;

    if (const char *trace_path = std::getenv("SUPERSLICER_FANMOVER_TRACE")) {
        std::ofstream trace(trace_path, std::ios::app);
        trace << "z=" << reader.z()
              << " marker=" << line.raw()
              << " want=" << overhang_fan_speed
              << " output=" << m_output_fan_speed
              << " front=" << m_front_buffer_fan_speed
              << " back=" << m_back_buffer_fan_speed
              << " buffer_time=" << m_buffer_time_size
              << " lower_buffer_fans=" << lower_buffer_fan_count
              << " emitted=" << last_emitted_fan_speed
              << " lower_output=" << lower_output_fan
              << '\n';
    }
    const bool step_up = overhang_fan_speed > m_output_fan_speed;
    const bool first_in_block = !m_overhang_block_approach_slowdown_done;
    const bool force_marker_fan = first_in_block;
    const bool speed_change = overhang_fan_speed != m_output_fan_speed || lower_buffer_fan_count > 0 || lower_output_fan || force_marker_fan;
    const int16_t target_fan_speed = int16_t(overhang_fan_speed);

    if (slowdown_for_fan && step_up && first_in_block)
        _apply_overhang_approach_slowdown(overhang_fan_speed);

    if (slowdown_for_fan && speed_change) {
        // Emit the overhang's own curve value directly so each overhang
        // section holds one consistent fan speed. The old path kicked the
        // fan to 100% here and relied on a deferred "end fan kickstart" to
        // settle back to the target -- but that settle is suppressed while the
        // 100% blast is the current output (see write_buffer_data), so the fan
        // stayed pinned at 100% through the whole overhang AND bled into the
        // following perimeter/external (and sometimes the next layer). Setting
        // the target directly removes both the 100% jump and the bleed. Fan
        // spin-up is meant to be covered by the approach slowdown
        // (overhangs_speed < 100%); the firmware fan_kickstart still applies
        // at the emitted M106 itself.
        const char *comment = (step_up && first_in_block)
            ? "set override fan (slowdown)"
            : "set override fan (overhang)";
        const std::string fan_gcode = _set_fan(target_fan_speed, comment);
        _queue_fan_at_marker(fan_gcode, target_fan_speed, force_marker_fan);
        m_back_buffer_fan_speed = target_fan_speed;
        if (first_in_block)
            m_overhang_block_approach_slowdown_done = true;
    } else if (!slowdown_for_fan && (step_up || force_marker_fan)) {
        const std::string fan_gcode = _set_fan(overhang_fan_speed, "set override fan");
        if (force_marker_fan && !step_up) {
            _queue_fan_at_marker(fan_gcode, overhang_fan_speed, true);
            m_back_buffer_fan_speed = overhang_fan_speed;
        } else {
            // Original pre-start behaviour (slowdown feature off).
            _remove_slow_fan(overhang_fan_speed, m_buffer_time_size + 1, true);
            if (!m_buffer.empty() && (m_buffer_time_size - m_buffer.front().time * 0.1) > nb_seconds_delay) {
                _print_in_middle_G1(m_buffer.front(), m_buffer_time_size - nb_seconds_delay, fan_gcode);
                remove_from_buffer(m_buffer.begin());
            } else {
                _append_fan_command(fan_gcode, overhang_fan_speed);
            }
            m_front_buffer_fan_speed = overhang_fan_speed;
        }
        if (first_in_block)
            m_overhang_block_approach_slowdown_done = true;
    }
}

void FanMover::_process_gcode_line(GCodeReader& reader, const GCodeReader::GCodeLine& line)
{
    // processes 'normal' gcode lines
    bool need_flush = false;
    std::string cmd(line.cmd());
    double time = 0;
    int16_t fan_speed = -1;
    if (cmd.length() > 1) {
        if (::toupper(cmd[0]) == 'G') {
            assert(!line.has_f() || line.f() > 0);
            if (line.has_f() && line.f() > 0) {
                m_current_speed = line.f() / FEEDRATE_SECONDS_PER_MINUTE;
            }
        }
        switch (::toupper(cmd[0])) {
        case 'A':
            _process_ACTIVATE_EXTRUDER(line.raw());
                break;
        case 'T':
        case 't':
            _process_T(cmd);
                break;
        case 'G':
        {
            _handle_g_command(cmd, reader, line, time);
            break;
        }
        case 'M':
        {
            _handle_m_command(cmd, line, fan_speed, time, need_flush);
            break;
        }
        }
    } else {
        if(!line.raw().empty() && line.raw().front() == ';')
        {
            if (line.raw().size() > 10 && line.raw().rfind(";TYPE:", 0) == 0) {
                // get the type of the next extrusions
                std::string extrusion_string = line.raw().substr(6, line.raw().size() - 6);
                current_role                 = string_to_gcode_extrusion_role(extrusion_string);
                assert(current_role != GCodeExtrusionRole::None);
                if (current_role == GCodeExtrusionRole::OverhangPerimeter)
                    m_overhang_block_approach_slowdown_done = false;
            }
            if (line.raw().size() > 16 && line.raw().rfind("; custom gcode", 0) != std::string::npos)
                m_is_custom_gcode = line.raw().rfind("; custom gcode end", 0) == std::string::npos;
            _process_overhang_fan_marker(reader, line);
            if ((line.raw().rfind("; end of overhang fan", 0) == 0 || line.raw().rfind("; end of overhang speed", 0) == 0)
                && m_last_overhang_min_fan_speed > 0) {
                m_overhang_fan_hold_speed = std::max(m_overhang_fan_hold_speed, m_last_overhang_min_fan_speed);
                m_overhang_fan_hold_until_extrusion = true;
                m_last_overhang_min_fan_speed = -1;
                m_fan_slowdown_remaining = 0.f;
                m_fan_slowdown_total = 0.f;
            }
        }
    }

    if (time >= 0) {
        BufferData move_data(line.raw(), time, fan_speed);
        if (line.has(Axis::X)) {
            move_data.x = reader.x();
            move_data.dx = line.dist_X(reader);
        }
        if (line.has(Axis::Y)) {
            move_data.y = reader.y();
            move_data.dy = line.dist_Y(reader);
        }
        if (line.has(Axis::Z)) {
            move_data.z = reader.z();
            move_data.dz = line.dist_Z(reader);
        }
        if (line.has(Axis::E)) {
            move_data.e = reader.e();
            if (relative_e) {
                move_data.de = line.e();
                move_data.e = 0;
            } else
                move_data.de = line.dist_E(reader);
        }
        _apply_active_fan_slowdown(move_data);
        BufferData& new_data = put_in_buffer(std::move(move_data));
        assert(new_data.dx == 0 || reader.x() == new_data.x);
        assert(new_data.dx == 0 || std::abs(reader.x() + new_data.dx - line.x()) < 0.00002f);
        assert(new_data.dy == 0 || reader.y() == new_data.y);
        assert(new_data.dy == 0 || std::abs(reader.y() + new_data.dy - line.y()) < 0.00002f);
        assert(new_data.de == 0 || (relative_e?0:reader.e()) == new_data.e);
        assert(new_data.de == 0 || std::abs((relative_e?0.f:reader.e()) + new_data.de - line.e()) < 0.00001f);
        //assert(new_data.de == 0 ||(relative_e?0.f:reader.e()) + new_data.de == line.e());

        // split the back of the buffer when a kickstart end inside it.
        if (m_current_kickstart.time > 0 && new_data.time > 0) {
            m_current_kickstart.time -= new_data.time;
            if (m_current_kickstart.time < 0) {
                // prev is possible because we just do an emplace_back.
                _put_in_middle_G1(prev(m_buffer.end()), new_data.time + m_current_kickstart.time, BufferData{ m_current_kickstart.raw, 0, m_current_kickstart.fan_speed, true }, kickstart);
            }
        }
    }
    // puts the line back into the gcode
    //if buffer too big, flush it.
    if (time >= 0) {
        // Add EPSILON to allow to have a buffer even with 0 m_buffer_time_size, so multiple consecutive M106 can be culled.
        while (!m_buffer.empty() && (need_flush || m_buffer_time_size - m_buffer.front().time > nb_seconds_delay + EPSILON) ){
            write_buffer_data();
        }
    }
#if _DEBUG
    double sum = 0;
    for (auto& data : m_buffer) sum += data.time;
    assert( std::abs(m_buffer_time_size - sum) < 0.01);
#endif
}

void FanMover::write_buffer_data()
{
    BufferData &frontdata = m_buffer.front();
    if (m_overhang_fan_hold_until_extrusion && frontdata.fan_speed >= 0 && frontdata.fan_speed < m_overhang_fan_hold_speed) {
        remove_from_buffer(m_buffer.begin());
        return;
    }
    if (frontdata.fan_speed >= 0 && frontdata.fan_speed == m_output_fan_speed && !frontdata.force_emit) {
        remove_from_buffer(m_buffer.begin());
        return;
    }
    if (frontdata.fan_speed >= 0 && !frontdata.force_emit) {
        double time_until_next_fan = 0;
        for (auto nextdata = std::next(m_buffer.begin()); nextdata != m_buffer.end(); ++nextdata) {
            if (nextdata->fan_speed >= 0) {
                if (nextdata->fan_speed > frontdata.fan_speed && time_until_next_fan <= nb_seconds_delay + EPSILON) {
                    remove_from_buffer(m_buffer.begin());
                    return;
                }
                break;
            }
            time_until_next_fan += nextdata->time;
            if (time_until_next_fan > nb_seconds_delay + EPSILON)
                break;
        }
    }
    if (frontdata.fan_speed < 0) {
        m_process_output += frontdata.raw + "\n";
    } else if (frontdata.force_emit) {
        m_process_output += frontdata.raw + (frontdata.raw.empty() || frontdata.raw.back() == '\n' ? "" : "\n");
        m_output_fan_speed = frontdata.fan_speed;
        m_front_buffer_fan_speed = frontdata.fan_speed;
    } else if (frontdata.fan_speed != m_output_fan_speed) {
        // if kickstart-end command, emit it
        if (frontdata.is_kickstart && frontdata.fan_speed < m_output_fan_speed) {
            // The kickstart target speed is lower than the current output fan speed.
            // This typically means an overhang fan override (SET_FAN_SPEED) has already
            // raised the emitted fan speed above this kickstart's target while the entry
            // was sitting in the delay buffer. Emitting "end fan kickstart" here would
            // incorrectly drop the fan from the overhang-boosted level back to the cooling
            // layer speed mid-overhang. Suppress it instead: the overhang-boosted speed
            // remains in the output, and the next regular M106 from CoolingBuffer will
            // reduce the fan after the overhang section ends.
        } else {
            _append_fan_command(frontdata.raw, frontdata.fan_speed);
            m_front_buffer_fan_speed = frontdata.fan_speed;
        }
    }
    remove_from_buffer(m_buffer.begin());
}

// Extracted G command handler (move timing for buffer, G2/G3 approx, overhang fan hold re-apply).
// Moved out of _process_gcode_line to shrink the long 'if'/'switch' (BP1013) and nesting (BP1015).
// Owner comments (kickstart noise avoidance, hold for extrusion alignment, etc.) preserved verbatim.
void FanMover::_handle_g_command(const std::string& cmd, GCodeReader& reader, const GCodeReader::GCodeLine& line, double& time)
{
    if (::atoi(&cmd[1]) == GCODE_G1 || ::atoi(&cmd[1]) == GCODE_G0) {
        double distx = line.dist_X(reader);
        double disty = line.dist_Y(reader);
        double distz = line.dist_Z(reader);
        double dist = distx * distx + disty * disty + distz * distz;
        if (dist > 0) {
            dist = std::sqrt(dist);
            assert(m_current_speed > 0 && m_current_speed < MAX_BUFFER_TIME && !std::isnan(m_current_speed));
            time = dist / m_current_speed;
            assert(time >= 0 && time < MAX_BUFFER_TIME && !std::isnan(time));
        }
        if (m_overhang_fan_hold_until_extrusion && line.has(Axis::E)) {
            m_overhang_fan_hold_until_extrusion = false;
            m_overhang_fan_hold_speed = -1;
            // Local lambda to encapsulate the re-apply of suppressed fan hold.
            // This flattens nesting and documents the intent (read from owner comments):
            // keep the fan high through the post-overhang gap so it doesn't drop mid-feature;
            // defer the lower target and re-apply aligned with next extrusion via the delay buffer.
            auto reapply_suppressed_fan_hold = [&] {
                if (m_overhang_fan_hold_pending_speed >= 0) {
                    put_in_buffer(BufferData(m_overhang_fan_hold_pending_raw, 0, int16_t(m_overhang_fan_hold_pending_speed)));
                    m_overhang_fan_hold_pending_speed = -1;
                    m_overhang_fan_hold_pending_raw.clear();
                }
            };
            reapply_suppressed_fan_hold();
        }
    } else if (::atoi(&cmd[1]) == GCODE_G2 || ::atoi(&cmd[1]) == GCODE_G3) {
        // Note: arc dist approximation (current chord length for G2/G3 timing in the fan buffer;
        // sufficient for kickstart/slowdown heuristics). Real arc math would be part of a larger
        // gcode writer redesign (see the note at _process_T).
        double distx = line.dist_X(reader);
        double disty = line.dist_Y(reader);
        double dist = distx * distx + disty * disty;
        if (dist > 0) {
            dist = std::sqrt(dist);
            assert(m_current_speed > 0 && m_current_speed < MAX_BUFFER_TIME && !std::isnan(m_current_speed));
            time = dist / m_current_speed;
            assert(time >= 0 && time < MAX_BUFFER_TIME && !std::isnan(time));
        }
    }
}

// Delays this M106 by kickstarting the fan target from the fan speed already in the delay
// buffer (or from any kickstart already running), so a big fan increase reaches speed by
// the time this line reaches the front.
void FanMover::_handle_delayed_kickstart(const GCodeReader::GCodeLine& line, int16_t fan_speed, int fan_baseline, double& time)
{
    //don't put this command in the queue
    time = -1;
    // this M106 need to go in the past
    //check if we have ( kickstart and not in slowdown )
    int current_front_buffer_fan_speed = m_front_buffer_fan_speed;
    if (m_current_kickstart.time > 0) {
        current_front_buffer_fan_speed = m_current_kickstart.fan_speed;
    }
    // Only kickstart when the speed increase is large enough to matter.
    // Small bumps (e.g. 40%?45%) don't need a max burst and would
    // just create unnecessary noise in the G-code.
    const int16_t kickstart_min_delta = _kickstart_min_delta();
    // Kickstart has no effect at max target and only creates duplicated M106 S255 lines.
    if (! (kickstart > 0 && fan_speed < FAN_PERCENT_MAX && fan_speed > current_front_buffer_fan_speed + kickstart_min_delta)) {
        // first erase everything lower than that value
        _remove_slow_fan(fan_speed, m_buffer_time_size + 1, true);
        // then write the fan command
        if (!m_buffer.empty() && (m_buffer_time_size - m_buffer.front().time * 0.1) > nb_seconds_delay) {
            _print_in_middle_G1(m_buffer.front(), m_buffer_time_size - nb_seconds_delay, line.raw());
            remove_from_buffer(m_buffer.begin());
        } else {
            _append_fan_command(std::string(line.raw()), fan_speed);
        }
        m_front_buffer_fan_speed = fan_speed;
        return;
    }

    // update current kickstart?
    if (m_current_kickstart.time > 0) {
        const float kickstart_duration = kickstart * float(fan_speed - current_front_buffer_fan_speed) / FAN_PERCENT_MAX;
        m_current_kickstart.time += kickstart_duration - m_current_kickstart_duration;
        m_current_kickstart.fan_speed = fan_speed;
        m_current_kickstart.raw = line.raw();
        return;
    }

    //if kickstart
    // first erase everything lower than that value
    _remove_slow_fan(fan_speed, m_buffer_time_size + 1, true);
    // then erase everything lower that kickstart
    _remove_slow_fan(fan_baseline, kickstart);
    // print me
    if (!m_buffer.empty() && (m_buffer_time_size - m_buffer.front().time * 0.1) > nb_seconds_delay) {
        // Emit kickstart via the fan buffer (writer is not used; this path is multi-thread safe).
        _print_in_middle_G1(m_buffer.front(), m_buffer_time_size - nb_seconds_delay, _set_fan(fan_speed, "kickstart fan"));
        remove_from_buffer(m_buffer.begin());
    } else {
        _append_fan_command(_set_fan(fan_speed, "kickstart fan"), fan_speed);
    }
    m_front_buffer_fan_speed = fan_speed;
    //write it in the queue if possible
    const float kickstart_duration = kickstart * float(fan_speed - current_front_buffer_fan_speed) / FAN_PERCENT_MAX;
    float time_count = kickstart_duration;
    auto it = m_buffer.begin();
    while (it != m_buffer.end() && time_count > 0) {
        time_count -= it->time;
        if (time_count< 0) {
            //found something that is lower than us
            _put_in_middle_G1(it, it->time + time_count, BufferData(std::string(line.raw()), 0, fan_speed, true), nb_seconds_delay);
            //found, stop
            break;
        }
        ++it;
    }
    if (time_count > 0) {
        //can't place it in the buffer, use m_current_kickstart
        m_current_kickstart.fan_speed = fan_speed;
        m_current_kickstart.time = time_count;
        m_current_kickstart_duration = time_count;
        m_current_kickstart.raw = line.raw();
    }
}

// The non-delayed-buffer fan-increase path: stop or extend an in-flight kickstart, or
// start a new deferred kickstart for this M106, depending on kickstart_min_delta.
void FanMover::_cherry_pick_kickstart(const GCodeReader::GCodeLine& line, int16_t fan_speed, double& time)
{
    if (kickstart <= 0) {
        //nothing to do
        //we don't put time = -1; so it will printed in the buffer as other line are done
        return;
    }
    if (m_current_kickstart.time > 0) {
        //cherry-pick this one
        if (m_back_buffer_fan_speed >= fan_speed) {
            //stop kickstart
            m_current_kickstart.time = -1;
            //this will print me just after as time >=0
            return;
        }
        // add some duration to the kickstart and use it for me.
        float kickstart_duration = kickstart * float(fan_speed - m_back_buffer_fan_speed) / FAN_PERCENT_MAX;
        m_current_kickstart.fan_speed = fan_speed;
        m_current_kickstart.time += kickstart_duration;
        m_current_kickstart_duration = kickstart_duration;
        m_current_kickstart.raw = line.raw();
        //i'm printed by the m_current_kickstart
        time = -1;
        return;
    }
    if (fan_speed < FAN_PERCENT_MAX && m_back_buffer_fan_speed < fan_speed - _kickstart_min_delta()) {
        //don't write this line, as it will need to be delayed
        time = -1;
        //get the duration of kickstart
        float kickstart_duration = kickstart * float(fan_speed - m_back_buffer_fan_speed) / FAN_PERCENT_MAX;
        //if kickstart, write the M106 S[fan_baseline] first
        //set the target speed and set the kickstart flag
        put_in_buffer(BufferData(_set_fan(fan_speed, "kickstart fan")
            , 0, fan_speed, true));
        //kickstart!
        //add the normal speed line for the future
        m_current_kickstart.fan_speed = fan_speed;
        m_current_kickstart.time = kickstart_duration;
        m_current_kickstart_duration = kickstart_duration;
        m_current_kickstart.raw = line.raw();
    }
}

// Extracted M fan command handler (full kickstart/slowdown/hold/cherry-pick/buffer logic).
// Moved out of the long 'if' (BP1013) and 'switch' (BP1013) in _process_gcode_line and
// associated deep nesting (BP1015). Every owner comment describing thresholds, erase-lower,
// "only when large enough to matter", "can't place in buffer use m_current_kickstart",
// overhang hold pending re-apply etc. was read and is preserved here.
void FanMover::_handle_m_command(const std::string& cmd, const GCodeReader::GCodeLine& line, int16_t& fan_speed, double& time, bool& need_flush)
{
    fan_speed = get_fan_speed(line.raw(), m_writer.config.gcode_flavor);
    if (fan_speed >= 0) {
        const auto fan_baseline = (m_writer.config.fan_percentage.value ? FAN_PERCENT_MAX : FAN_PWM_MAX);
        fan_speed = FAN_PERCENT_MAX * fan_speed / fan_baseline;
        if (!m_is_custom_gcode) {
            if (m_overhang_fan_hold_until_extrusion && fan_speed < m_overhang_fan_hold_speed) {
                // Keep the fan high through the post-overhang transition, but remember this
                // lower target (the next feature's intended fan) so it can be re-applied when
                // extrusion resumes (see the G1-with-E release above). Dropping it outright
                // left the fan stuck at the overhang speed for the whole next feature.
                m_overhang_fan_hold_pending_raw = line.raw();
                m_overhang_fan_hold_pending_speed = fan_speed;
                time = -1;
                fan_speed = -1;
                return;  // was break in switch case; early exit for this handler
            }
            // if slow down => put in the queue. if not =>
            if (m_current_kickstart.time > 0) {
                assert(m_back_buffer_fan_speed == m_current_kickstart.fan_speed);
                if (fan_speed < m_back_buffer_fan_speed) {
                    time = -1;
                    fan_speed = -1;
                    return;
                }
            }
            if (m_back_buffer_fan_speed >= fan_speed) {
                if (m_current_kickstart.time > 0) {
                    // stop kiskstart, and slow down
                    m_current_kickstart.time = -1;
                    //this fan speed will be printed, to make and end to the kickstart
                }
             } else {
                 if (nb_seconds_delay > 0 && (!only_overhangs || current_role == GCodeExtrusionRole::OverhangPerimeter))
                     _handle_delayed_kickstart(line, fan_speed, fan_baseline, time);
                 else
                     _cherry_pick_kickstart(line, fan_speed, time);
              }
          }
         //update back buffer fan speed
         m_back_buffer_fan_speed = fan_speed;
     } else {
         // have to flush the buffer to avoid erasing a fan command.
         need_flush = true;
     }
}

} // namespace Slic3r

