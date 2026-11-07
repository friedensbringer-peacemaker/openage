// Copyright 2026-2026 the openage authors. See copying.md for legal info.

/*
 * Host test of the selection field (XR fork W8, gamestate/panel_math.h):
 * openage-panel-check. All four click modes, an id outside the
 * selection, the double click with its limit and the selection limit.
 * Exit code 0 if all checks pass. Only the standard library.
 */

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "gamestate/panel_math.h"

using namespace openage::gamestate;
using namespace openage::gamestate::panel;

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

std::vector<PanelUnit> sel() {
	return {{1, 10}, {2, 20}, {3, 10}, {4, 30}};
}

void check_click() {
	check(panel_click(sel(), 2, click_t::PLAIN) == std::vector<PanelUnit>{{2, 20}},
	      "plain selects only this unit");
	check(panel_click(sel(), 3, click_t::CTRL) == std::vector<PanelUnit>{{1, 10}, {2, 20}, {4, 30}},
	      "ctrl deselects this unit, order stays");
	check(panel_click(sel(), 1, click_t::SHIFT) == std::vector<PanelUnit>{{1, 10}, {3, 10}},
	      "shift keeps all of the type");
	check(panel_click(sel(), 4, click_t::CTRL_SHIFT) == std::vector<PanelUnit>{{1, 10}, {2, 20}, {3, 10}},
	      "ctrl shift deselects all of the type");
	// a clicked id outside the selection changes nothing, in every mode
	for (const auto mode : {click_t::PLAIN, click_t::CTRL, click_t::SHIFT, click_t::CTRL_SHIFT}) {
		check(panel_click(sel(), 99, mode) == sel(), "unknown id changes nothing");
	}
	check(panel_click({}, 1, click_t::PLAIN).empty(), "empty selection stays empty");
}

void check_double() {
	const std::vector<PanelUnit> screen = {{5, 30}, {1, 10}, {2, 20}, {3, 10}, {4, 10}};
	check(double_click(screen, 10, SELECTION_LIMIT)
	          == std::vector<PanelUnit>{{1, 10}, {3, 10}, {4, 10}},
	      "all of the type in their order");
	check(double_click(screen, 10, 2) == std::vector<PanelUnit>{{1, 10}, {3, 10}},
	      "at most limit units");
	check(double_click(screen, 10, 0).empty(), "limit 0 selects nobody");
	check(double_click(screen, 20, SELECTION_LIMIT) == std::vector<PanelUnit>{{2, 20}},
	      "other type only its own units");
	check(double_click(screen, 99, SELECTION_LIMIT).empty(), "missing type selects nobody");
	check(double_click({}, 10, SELECTION_LIMIT).empty(), "empty screen selects nobody");
}

void check_limit() {
	const std::vector<PanelUnit> five{{1, 1}, {2, 2}, {3, 3}, {4, 4}, {5, 5}};
	check(limit_selection(five, 3) == std::vector<PanelUnit>{{1, 1}, {2, 2}, {3, 3}},
	      "first limit units");
	check(limit_selection(five, 5) == five, "limit equal to the size keeps all");
	check(limit_selection(five, 60) == five, "limit above the size keeps all");
	check(limit_selection(five, 0).empty(), "limit 0 empties the selection");
}

} // namespace

int main() {
	check_click();
	check_double();
	check_limit();
	std::cout << "panel check: " << checks << " checks, " << failures << " failures"
	          << std::endl;
	return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
