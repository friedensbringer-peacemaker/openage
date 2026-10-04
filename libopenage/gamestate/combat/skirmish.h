// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "gamestate/map_generator.h"


namespace openage::gamestate::combat {

/**
 * Military units of the skirmish test option (XR fork, MapSettings::skirmish).
 */
enum class skirmish_unit_t {
	KNIGHT,
	MILITIA,
	ARCHER,
};

/**
 * One unit of the skirmish layout.
 */
struct SkirmishUnit {
	skirmish_unit_t kind;
	/// position (tiles)
	double ne = 0.0;
	double se = 0.0;
	/// owning player
	size_t owner = 0;
	/// facing: direction towards the enemy (tiles)
	double face_ne = 0.0;
	double face_se = 0.0;
};

/**
 * Layout of the skirmish armies (dependency-free, deterministic).
 */
struct SkirmishLayout {
	std::vector<SkirmishUnit> units{};
	/// battlefield centre between the armies (tiles)
	double center_ne = 0.0;
	double center_se = 0.0;
	/// false if no free spot was found (no units)
	bool placed = false;
};

/// gap between the front rows (tiles): beyond the sight of knights and archers
inline constexpr double SKIRMISH_GAP = 8.0;

/**
 * Place a small army per player (3 knights in front, 3 militia, 3 archers
 * behind) between the start positions of a generated map: both armies face
 * each other SKIRMISH_GAP (up to + 4) tiles apart, out of each other's sight, on land
 * tiles that are not blocked, at least 7 tiles from the town centers.
 * Tries spots along and across the line between the starts.
 *
 * @param map Generated map (two starts).
 *
 * @return Layout (placed = false if the map has no room).
 */
SkirmishLayout skirmish_layout(const GeneratedMap &map);

/**
 * Short name of a unit kind (game entity directory of the AoE II modpacks).
 */
const char *to_string(skirmish_unit_t kind);

} // namespace openage::gamestate::combat
