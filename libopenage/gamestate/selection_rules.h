// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <algorithm>
#include <cstdlib>
#include <vector>


/**
 * Selection by mouse (XR fork), dependency-free for host tests
 * (gamestate/selection_check.cpp).
 *
 * - A left click without noticeable dragging selects the entity under the
 *   cursor (own or foreign), a click on the ground clears the selection.
 * - Dragging selects the own entities in the rectangle.
 * - Shift adds: a click toggles an own entity in the selection, a rectangle
 *   adds its entities; foreign entities are never added to a selection.
 * - A foreign entity (enemy, gaia) is selected alone and only displayed,
 *   commands ignore it.
 */
namespace openage::gamestate::select {

/// largest distance (pixels, per axis) between press and release of a click
constexpr int click_threshold_px = 6;

/**
 * Whether a press and release form a click instead of a drag.
 *
 * @param dx Horizontal distance between press and release (pixels).
 * @param dy Vertical distance between press and release (pixels).
 * @param threshold Largest distance of a click (pixels, per axis, exclusive).
 */
inline bool is_click(int dx, int dy, int threshold = click_threshold_px) {
	return std::abs(dx) < threshold and std::abs(dy) < threshold;
}

/**
 * Selection after a click or drag.
 *
 * @param current Current selection.
 * @param current_foreign Whether the current selection is a foreign entity (alone).
 * @param hits Entities under the click (0 or 1) or in the rectangle.
 * @param hits_foreign Whether the hits are a foreign entity (click only).
 * @param additive Shift held: add to the selection instead of replacing it.
 * @param click Click (toggles with \p additive) instead of a rectangle.
 *
 * @return New selection.
 */
template <typename Id>
std::vector<Id> next_selection(const std::vector<Id> &current,
                               bool current_foreign,
                               const std::vector<Id> &hits,
                               bool hits_foreign,
                               bool additive,
                               bool click) {
	if (not additive) {
		return hits;
	}

	// additive: a foreign entity never joins, a foreign selection is replaced
	std::vector<Id> result = current_foreign ? std::vector<Id>{} : current;
	if (hits_foreign) {
		return current;
	}
	for (const auto &id : hits) {
		auto it = std::find(result.begin(), result.end(), id);
		if (it == result.end()) {
			result.push_back(id);
		}
		else if (click) {
			// shift + click on a selected entity deselects it
			result.erase(it);
		}
	}
	return result;
}

/**
 * Whether a point in normalized device coordinates is on the screen
 * (double click: all own entities of the type in view).
 */
inline bool on_screen(float ndc_x, float ndc_y) {
	return ndc_x >= -1.0f and ndc_x <= 1.0f and ndc_y >= -1.0f and ndc_y <= 1.0f;
}

} // namespace openage::gamestate::select
