/**
 * @file src/session/virtual_display_transition.h
 * @brief Bounded virtual-display snapshots and verified live-transition rollback.
 */
#pragma once

#include "../plank_topology.h"

#include <algorithm>
#include <charconv>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace plank::session::virtual_display {
  /** @brief One RandR connector; EDID binds restoration to the observed monitor. */
  struct output_t {
    std::string name;
    bool connected {};
    bool primary {};
    int width {}, height {}, x {}, y {};
    std::string mode, rate, edid;
    std::optional<int> non_desktop;
    bool operator==(const output_t &) const = default;
  };

  /** @brief A bounded observation, including inactive connectors and framebuffer. */
  struct snapshot_t {
    int width {}, height {};
    std::vector<output_t> outputs;
    bool operator==(const snapshot_t &) const = default;
  };

  /** @brief Parse a bounded unsigned decimal without overflow or trailing input. */
  inline std::optional<int> integer(std::string_view value) {
    int result {};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc {} || parsed.ptr != value.data() + value.size() ||
        result < 0 || result > 32767) return std::nullopt;
    return result;
  }

  /** @brief Read `xrandr --current --prop`; malformed/rotated layouts fail closed.
   * @param text Complete bounded command output, never a shell command.
   * @return Observed connectors, or no value for an unrecognizable response.
   */
  inline std::optional<snapshot_t> parse(std::string_view text) {
    if (text.empty() || text.size() > 65536 || text.find('\0') != text.npos) return std::nullopt;
    static const std::regex screen {R"(^Screen [0-9]+: minimum [0-9]+ x [0-9]+, current ([0-9]+) x ([0-9]+), maximum [0-9]+ x [0-9]+$)"};
    static const std::regex connector {R"(^([A-Za-z0-9_-]+) (connected|disconnected)( primary)?( ([0-9]+)x([0-9]+)\+([0-9]+)\+([0-9]+))? \(normal[^)]*\).*$)"};
    static const std::regex edid_line {R"(^[0-9a-fA-F]{32}$)"};
    static const std::regex rate_value {R"(^[0-9]{1,3}(\.[0-9]{1,3})?$)"};
    std::istringstream input {std::string {text}};
    snapshot_t result;
    output_t *current = nullptr;
    bool reading_edid = false, have_edid = false, have_screen = false;
    std::string line;
    while (std::getline(input, line)) {
      if (line.size() > 2048) return std::nullopt;
      std::smatch match;
      if (std::regex_match(line, match, screen)) {
        if (have_screen || current) return std::nullopt;
        const auto w = integer(match[1].str()), h = integer(match[2].str());
        if (!w || !h || !*w || !*h) return std::nullopt;
        result.width = *w; result.height = *h; have_screen = true;
        continue;
      }
      if (!line.empty() && line.front() != ' ' && line.front() != '\t') {
        if (!have_screen || !std::regex_match(line, match, connector) ||
            result.outputs.size() >= 16) return std::nullopt;
        output_t output;
        output.name = match[1]; output.connected = match[2] == "connected";
        output.primary = match[3].matched;
        if (std::any_of(result.outputs.begin(), result.outputs.end(), [&](const auto &item) {
              return item.name == output.name;
            })) return std::nullopt;
        if (match[4].matched) {
          const auto w = integer(match[5].str()), h = integer(match[6].str());
          const auto x = integer(match[7].str()), y = integer(match[8].str());
          if (!output.connected || !w || !h || !x || !y || !*w || !*h ||
              *x + *w > result.width || *y + *h > result.height) return std::nullopt;
          output.width = *w; output.height = *h; output.x = *x; output.y = *y;
        }
        result.outputs.push_back(output); current = &result.outputs.back();
        reading_edid = have_edid = false;
        continue;
      }
      if (!current) { if (!line.empty()) return std::nullopt; else continue; }
      const auto begin = line.find_first_not_of(" \t");
      const auto end = line.find_last_not_of(" \t\r");
      if (begin == line.npos) continue;
      const auto value = line.substr(begin, end - begin + 1);
      if (reading_edid && std::regex_match(value, edid_line)) {
        if (current->edid.size() + value.size() > 4096) return std::nullopt;
        current->edid += value;
        continue;
      }
      if (reading_edid && line.starts_with("\t\t")) return std::nullopt;
      reading_edid = value == "EDID:";
      if (reading_edid) {
        if (have_edid) return std::nullopt;
        have_edid = true;
      } else if (value.starts_with("non-desktop:")) {
        const auto property = value.substr(value.find(':') + 1);
        const auto pos = property.find_first_not_of(" \t");
        if (current->non_desktop || pos == property.npos) return std::nullopt;
        const auto flag = integer(property.substr(pos));
        if (!flag || *flag > 1) return std::nullopt;
        current->non_desktop = *flag;
      } else if (line.front() == ' ' && value.find('*') != value.npos) {
        std::istringstream modes {value};
        std::string mode, rate;
        modes >> mode;
        while (modes >> rate) {
          const auto star = rate.find('*');
          if (star == rate.npos) continue;
          if (!current->mode.empty()) return std::nullopt;
          const auto suffix = rate.substr(star);
          rate.resize(star);
          if ((suffix != "*" && suffix != "*+") || !std::regex_match(rate, rate_value) ||
              std::stod(rate) <= 0) return std::nullopt;
          current->mode = mode; current->rate = rate;
        }
      }
    }
    if (!have_screen || result.outputs.empty()) return std::nullopt;
    for (const auto &output : result.outputs) {
      if ((output.width > 0) != !output.mode.empty()) return std::nullopt;
    }
    std::sort(result.outputs.begin(), result.outputs.end(), [](const auto &a, const auto &b) {
      return a.name < b.name;
    });
    return result;
  }

  /** @brief Locate a connector by exact name in an immutable observation. */
  inline const output_t *find(const snapshot_t &snapshot, std::string_view name) {
    const auto it = std::find_if(snapshot.outputs.begin(), snapshot.outputs.end(),
      [&](const auto &output) { return output.name == name; });
    return it == snapshot.outputs.end() ? nullptr : &*it;
  }

  /** @brief Require a restorable, qualified one/two-output virtual desktop. */
  inline bool usable(const snapshot_t &snapshot) {
    const auto first = find(snapshot, "DP-0"), second = find(snapshot, "DP-2");
    if (!first || !second || !first->width || !first->primary || !first->connected ||
        first->non_desktop != 0 || !second->non_desktop ||
        first->edid.empty() || second->edid.empty() ||
        first->edid.size() % 256 != 0 || second->edid.size() % 256 != 0) return false;
    int active = 0, right = 0, height = 0;
    for (const auto &output : snapshot.outputs) {
      if (!output.width) continue;
      if ((output.name != "DP-0" && output.name != "DP-2") || output.y != 0 ||
          (output.primary && output.name != "DP-0") || output.non_desktop != 0) return false;
      const auto mode = topology::virtual_mode_size(output.mode);
      if (mode.width != output.width || mode.height != output.height || output.rate.empty()) return false;
      ++active; right = std::max(right, output.x + output.width);
      height = std::max(height, output.height);
    }
    if (active == 1 && (first->x != 0 || second->primary || second->non_desktop != 1)) return false;
    if (active == 2 && !((first->x == 0 && second->x == first->width) ||
                        (second->x == 0 && first->x == second->width))) return false;
    // Xorg's configured virtual framebuffer may exceed the active modes.
    // Preserve that observed size for rollback; a requested result is stricter.
    return right <= snapshot.width && height <= snapshot.height &&
           snapshot.width <= topology::maximum_virtual_canvas_width && snapshot.height <= 2160;
  }

  /** @brief Reject connector/EDID changes and any unrelated-output alteration. */
  inline bool same_scope(const snapshot_t &before, const snapshot_t &after) {
    if (before.outputs.size() != after.outputs.size()) return false;
    for (std::size_t i = 0; i < before.outputs.size(); ++i) {
      const auto &a = before.outputs[i], &b = after.outputs[i];
      if (a.name != b.name || a.edid != b.edid) return false;
      // DP-2's connection status can change with its non-desktop property.
      if (a.name == "DP-0") { if (a.connected != b.connected) return false; }
      else if (a.name != "DP-2" && a != b) return false;
    }
    return true;
  }

  /** @brief Build argv to restore only the two previously validated virtual outputs. */
  inline std::vector<std::string> restore_arguments(const snapshot_t &snapshot) {
    if (!usable(snapshot)) return {};
    std::vector<std::string> args {"--fb", std::to_string(snapshot.width) + "x" + std::to_string(snapshot.height)};
    for (const auto name : {"DP-0", "DP-2"}) {
      const auto &output = *find(snapshot, name);
      args.insert(args.end(), {"--output", name});
      if (output.width) {
        args.insert(args.end(), {"--mode", output.mode, "--rate", output.rate,
          "--pos", std::to_string(output.x) + "x" + std::to_string(output.y)});
        if (output.primary) args.push_back("--primary");
      } else args.push_back("--off");
      args.insert(args.end(), {"--set", "non-desktop", std::to_string(*output.non_desktop)});
    }
    return args;
  }

  /** @brief Compare the observed usable layout with the requested qualified geometry. */
  inline bool matches_request(const snapshot_t &snapshot, std::string_view layout,
                              std::string_view mode_1, std::string_view mode_2, int primary) {
    if (!usable(snapshot) || !topology::valid_virtual_layout_modes(layout, mode_1, mode_2) ||
        !topology::valid_primary_output(layout, primary)) return false;
    const bool dual = layout == "dual-horizontal", right = dual && primary == 1;
    const auto first = topology::virtual_mode_size(mode_1), second = topology::virtual_mode_size(mode_2);
    if (snapshot.width != first.width + (dual ? second.width : 0) ||
        snapshot.height != (dual ? std::max(first.height, second.height) : first.height)) return false;
    const auto left_output = find(snapshot, right ? "DP-2" : "DP-0");
    const auto right_output = find(snapshot, right ? "DP-0" : "DP-2");
    if (left_output->mode != mode_1 || left_output->x != 0 || std::stod(left_output->rate) != 60) return false;
    return dual ? (right_output->mode == mode_2 && right_output->x == left_output->width &&
                   std::stod(right_output->rate) == 60) : right_output->width == 0;
  }

  /** @brief Distinguish a verified change, refused change and each recovery outcome. */
  enum class result_t { applied, no_snapshot, ownership_changed, scope_changed, restored, restore_failed };

  /** @brief Apply once and restore once on failure, always checking authority and observations.
   * @param owned Revalidates the same active user session and X-server identity.
   * @param observe Returns a fresh bounded RandR snapshot, including an empty desktop.
   * @param apply Runs the requested display command exactly once.
   * @param matches Validates the requested result rather than trusting command exit status.
   * @param restore Runs argv for the captured layout, never a guessed safe mode.
   * @return Verified outcome; ownership/scope changes never receive stale restoration.
   */
  template<class Owned, class Observe, class Apply, class Matches, class Restore>
  result_t transition(Owned owned, Observe observe, Apply apply, Matches matches, Restore restore) {
    if (!owned()) return result_t::ownership_changed;
    const auto before = observe();
    if (!owned()) return result_t::ownership_changed;
    if (!before || !usable(*before)) return result_t::no_snapshot;
    if (!owned()) return result_t::ownership_changed;
    const bool applied = apply();
    if (!owned()) return result_t::ownership_changed;
    const auto after = observe();
    if (!owned()) return result_t::ownership_changed;
    if (!after) return result_t::restore_failed; // Identity cannot be verified; do not guess.
    if (!same_scope(*before, *after)) return result_t::scope_changed;
    if (applied && matches(*after)) return result_t::applied;
    if (!owned()) return result_t::ownership_changed;
    const bool restored = restore(restore_arguments(*before));
    if (!owned()) return result_t::ownership_changed;
    const auto verified = observe();
    if (!owned()) return result_t::ownership_changed;
    if (!verified) return result_t::restore_failed;
    if (!same_scope(*before, *verified)) return result_t::scope_changed;
    // Connected is advisory for hidden DP-2; geometry/properties/primary must be exact.
    auto expected = *before;
    auto actual = *verified;
    for (auto &output : expected.outputs) if (output.name == "DP-2") output.connected = false;
    for (auto &output : actual.outputs) if (output.name == "DP-2") output.connected = false;
    return restored && usable(actual) && actual == expected ? result_t::restored : result_t::restore_failed;
  }
} // namespace plank::session::virtual_display
