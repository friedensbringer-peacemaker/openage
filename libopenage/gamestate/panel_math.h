// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>


namespace openage::gamestate::panel {

/**
 * Selection field (XR fork W8, spec 08 section 8.3): the selected units
 * in a fixed order, clicks on a portrait and the double click. No
 * engine dependencies (host test gamestate/panel_check.cpp).
 */

/// one unit of the selection field: entity id and unit type
struct PanelUnit {
	uint64_t id;
	int type;
	bool operator==(const PanelUnit &) const = default;
};

/// click modes on a portrait of the selection field (spec 08 section 8.3)
enum class click_t {
	PLAIN,       ///< select only this unit
	CTRL,        ///< deselect this unit
	SHIFT,       ///< keep all units of this type
	CTRL_SHIFT,  ///< deselect all units of this type
};

/// maximum number of selected units. The spec states 60 only for
/// movement groups, for the selection it is presumed (assumption).
constexpr size_t SELECTION_LIMIT = 60;

/// click on a portrait (see click_t); an id outside the selection changes
/// nothing, the order of the remaining units stays
inline std::vector<PanelUnit> panel_click(const std::vector<PanelUnit> &selection,
                                          uint64_t clicked,
                                          click_t click) {
	const auto it = std::find_if(selection.begin(), selection.end(),
	                             [clicked](const PanelUnit &u) { return u.id == clicked; });
	if (it == selection.end()) {
		return selection;
	}
	const int type = it->type;
	const auto same_type = [type](const PanelUnit &u) { return u.type == type; };
	std::vector<PanelUnit> result = selection;
	switch (click) {
	case click_t::PLAIN:
		return {*it};
	case click_t::CTRL:
		result.erase(result.begin() + (it - selection.begin()));
		return result;
	case click_t::SHIFT:
		result.erase(std::remove_if(result.begin(), result.end(),
		                            [same_type](const PanelUnit &u) { return not same_type(u); }),
		             result.end());
		return result;
	case click_t::CTRL_SHIFT:
		result.erase(std::remove_if(result.begin(), result.end(), same_type),
		             result.end());
		return result;
	}
	return result;
}

/// double click: all own units of the type on the screen, at most limit
inline std::vector<PanelUnit> double_click(const std::vector<PanelUnit> &own_on_screen,
                                           int type, size_t limit) {
	std::vector<PanelUnit> result;
	for (const auto &unit : own_on_screen) {
		if (unit.type == type and result.size() < limit) {
			result.push_back(unit);
		}
	}
	return result;
}

/// the first limit units of the selection, the order stays
inline std::vector<PanelUnit> limit_selection(std::vector<PanelUnit> selection, size_t limit) {
	selection.resize(std::min(selection.size(), limit));
	return selection;
}

} // namespace openage::gamestate::panel
