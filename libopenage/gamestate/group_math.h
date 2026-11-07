// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>


namespace openage::gamestate::groups {

/**
 * Control groups and selection (XR fork W8, spec 08): ctrl plus a digit
 * assigns the selection to a group, a unit is only in one group and
 * leaves every group when it dies, the same digit twice within
 * DOUBLE_PRESS_SECONDS jumps the camera, ctrl click toggles one unit
 * in the selection and the idle key cycles through the idle units.
 *
 * This header has no engine dependencies (host test gamestate/group_check.cpp).
 */

/// time window in which the same digit counts as a double press (assumption)
constexpr double DOUBLE_PRESS_SECONDS = 0.5;

/// the ten control groups, entities addressed by their id (spec 08)
class ControlGroups {
public:
	/**
	 * ctrl + digit: assign ids to the group. Every id appears at most
	 * once and the order stays. A unit is only in one group (presumed
	 * by the spec), so the ids leave all other groups. A digit outside
	 * 0-9 does nothing.
	 */
	void assign(int digit, std::vector<uint64_t> ids) {
		if (digit < 0 or digit > 9) {
			return;
		}
		std::vector<uint64_t> unique;
		for (const auto id : ids) {
			if (std::find(unique.begin(), unique.end(), id) == unique.end()) {
				unique.push_back(id);
			}
		}
		const auto moved = [&unique](uint64_t id) {
			return std::find(unique.begin(), unique.end(), id) != unique.end();
		};
		for (int d = 0; d <= 9; ++d) {
			if (d != digit) {
				auto &other = this->groups[static_cast<size_t>(d)];
				other.erase(std::remove_if(other.begin(), other.end(), moved), other.end());
			}
		}
		this->groups[static_cast<size_t>(digit)] = std::move(unique);
	}

	/// the ids of one group, empty for a digit outside 0-9
	const std::vector<uint64_t> &group(int digit) const {
		static const std::vector<uint64_t> empty;
		if (digit < 0 or digit > 9) {
			return empty;
		}
		return this->groups[static_cast<size_t>(digit)];
	}

	/// a dead unit leaves every group
	void remove(uint64_t id) {
		for (auto &members : this->groups) {
			members.erase(std::remove(members.begin(), members.end(), id), members.end());
		}
	}

	/**
	 * digit: select the group. Returns true when the camera should
	 * jump, that is the same digit was pressed again within
	 * DOUBLE_PRESS_SECONDS; the third press counts as a first one again.
	 * A digit outside 0-9 does nothing.
	 */
	bool select(int digit, double now) {
		if (digit < 0 or digit > 9) {
			return false;
		}
		if (this->pressed and this->last_digit == digit
		    and now - this->last_press <= DOUBLE_PRESS_SECONDS) {
			this->pressed = false;
			return true;
		}
		this->pressed = true;
		this->last_digit = digit;
		this->last_press = now;
		return false;
	}

private:
	std::array<std::vector<uint64_t>, 10> groups;
	bool pressed = false;
	int last_digit = 0;
	double last_press = 0.0;
};

/// ctrl + click: add id to the selection when it is missing, remove it otherwise
inline std::vector<uint64_t> toggle(std::vector<uint64_t> selection, uint64_t id) {
	const auto it = std::find(selection.begin(), selection.end(), id);
	if (it == selection.end()) {
		selection.push_back(id);
	}
	else {
		selection.erase(it);
	}
	return selection;
}

/**
 * Idle key "."/",": the idle unit after current in the list, the first
 * one again after the last. Without current, or when current is not in
 * the list, the first one; an empty list has no idle unit.
 */
inline std::optional<uint64_t> next_idle(const std::vector<uint64_t> &idle,
                                         std::optional<uint64_t> current) {
	if (idle.empty()) {
		return std::nullopt;
	}
	if (not current.has_value()) {
		return idle.front();
	}
	auto it = std::find(idle.begin(), idle.end(), *current);
	if (it == idle.end()) {
		return idle.front();
	}
	++it;
	if (it == idle.end()) {
		it = idle.begin();
	}
	return *it;
}

} // namespace openage::gamestate::groups
