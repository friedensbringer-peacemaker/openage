// Copyright 2026-2026 the openage authors. See copying.md for legal info.

/*
 * Host test of the control groups (XR fork W8, gamestate/group_math.h):
 *
 *   openage-group-check
 *
 * Assigning with duplicates and order, the move of a unit between
 * groups, digits outside 0-9, removing a dead unit, the double press
 * with its time window (inside, outside, another digit in between, the
 * third press), ctrl click toggle and the idle cycle with wrap around.
 * Exit code 0 if all checks pass. No dependencies besides the standard library.
 */

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "gamestate/group_math.h"

using namespace openage::gamestate;
using namespace openage::gamestate::groups;

namespace {

int failures = 0;
int checks = 0;

void check(bool ok, const std::string &what) {
	checks += 1;
	if (not ok) {
		failures += 1;
		if (failures <= 40) {
			std::cerr << "FAIL: " << what << std::endl;
		}
	}
}

void check_assign() {
	ControlGroups g;
	// duplicates appear once, the order stays
	g.assign(1, {5, 7, 5, 9});
	check(g.group(1) == std::vector<uint64_t>{5, 7, 9}, "duplicates once, order stays");
	// a unit is only in one group: 7 moves to group 2
	g.assign(2, {7, 8});
	check(g.group(1) == std::vector<uint64_t>{5, 9}, "unit leaves the old group");
	check(g.group(2) == std::vector<uint64_t>{7, 8}, "unit joins the new group");
	// assigning again replaces the group
	g.assign(2, {8});
	check(g.group(2) == std::vector<uint64_t>{8}, "reassign replaces the group");
	check(g.group(1) == std::vector<uint64_t>{5, 9}, "other group unchanged");
	// digits outside 0-9 do nothing, groups 0 and 9 exist
	g.assign(15, {1});
	g.assign(-1, {2});
	check(g.group(15).empty() and g.group(-3).empty(), "digits outside 0-9 are empty");
	check(g.group(1) == std::vector<uint64_t>{5, 9} and g.group(2) == std::vector<uint64_t>{8},
	      "digits outside 0-9 change nothing");
	g.assign(0, {11});
	g.assign(9, {12});
	check(g.group(0) == std::vector<uint64_t>{11} and g.group(9) == std::vector<uint64_t>{12},
	      "groups 0 and 9 exist");
}

void check_remove() {
	ControlGroups g;
	g.assign(1, {5, 9});
	g.assign(2, {7, 9});
	g.remove(9);
	check(g.group(1) == std::vector<uint64_t>{5}, "dead unit leaves group 1");
	check(g.group(2) == std::vector<uint64_t>{7}, "dead unit leaves group 2");
	g.remove(42);
	check(g.group(1) == std::vector<uint64_t>{5}, "unknown id removes nothing");
}

void check_select() {
	// inside the window, the third press counts as a first one again
	ControlGroups g;
	check(not g.select(1, 0.0), "first press only selects");
	check(g.select(1, 0.3), "second press jumps");
	check(not g.select(1, 0.4), "third press counts as first");
	check(g.select(1, 0.8), "fourth press jumps again");
	// outside the window: 0.6 s later, then the new first press counts
	check(not g.select(2, 10.0), "first press of another digit");
	check(not g.select(2, 10.6), "too late, no jump");
	check(g.select(2, 11.0), "0.4 s after the new first press jumps");
	// exactly 0.5 s still counts as a double press
	check(not g.select(3, 20.0), "first press");
	check(g.select(3, 20.5), "0.5 s still counts as double press");
	// another digit in between breaks the chain
	ControlGroups h;
	check(not h.select(4, 0.0), "press 4");
	check(not h.select(5, 0.1), "press 5 in between");
	check(not h.select(4, 0.2), "4 again is a first press");
	check(h.select(4, 0.4), "next 4 within the window jumps");
	// digits outside 0-9 do nothing and leave no state
	ControlGroups k;
	check(not k.select(15, 0.0), "digit 15 ignored");
	check(not k.select(1, 0.1), "ignored press leaves no state");
	check(k.select(1, 0.2), "double press still works");
}

void check_toggle_idle() {
	const std::vector<uint64_t> sel = {1, 2, 3};
	check(toggle(sel, 4) == std::vector<uint64_t>{1, 2, 3, 4}, "ctrl click adds at the end");
	check(toggle(sel, 2) == std::vector<uint64_t>{1, 3}, "ctrl click removes");
	check(toggle(toggle(sel, 4), 4) == sel, "toggle twice restores");
	check(toggle(std::vector<uint64_t>{}, 7) == std::vector<uint64_t>{7}, "empty selection adds");

	const std::vector<uint64_t> idle = {10, 20, 30};
	check(next_idle(idle, std::nullopt) == 10, "no current starts at the first");
	check(next_idle(idle, 10) == 20, "next after the first");
	check(next_idle(idle, 30) == 10, "wrap around to the first");
	check(next_idle(idle, 99) == 10, "unknown current starts at the first");
	check(next_idle(std::vector<uint64_t>{7}, 7) == 7, "one idle unit repeats");
	check(not next_idle(std::vector<uint64_t>{}, 5).has_value(), "no idle unit");
}

} // namespace

int main() {
	check_assign();
	check_remove();
	check_select();
	check_toggle_idle();
	std::cout << "group check: " << checks << " checks, " << failures << " failures"
	          << std::endl;
	return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
