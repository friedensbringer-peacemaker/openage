// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>


namespace openage::gamestate {

/**
 * Which map a new game uses (XR fork).
 *
 * This header has no dependencies so that embedders (e.g. the Quest app) can
 * fill it without pulling in the game state.
 */
enum class map_type_t {
	/// fixed 20x20 test map of upstream openage (default, used by the render checks)
	TEST,
	/// map from the random map generator (gamestate/map_generator.h)
	RANDOM,
};

/**
 * Initial camera view, in scene coordinates of the map.
 */
struct MapView {
	/// tile coordinates the camera looks at
	double ne = 0.0;
	double se = 0.0;
	/// camera zoom (1.0 = default, > 1.0 = zoomed out)
	float zoom = 1.0f;
	/// height of the camera above the ground (world units, default camera: 10)
	float height = 10.0f;
};

/**
 * Settings for the map of a new game.
 */
struct MapSettings {
	/// map type, the fixed test map is the default
	map_type_t type = map_type_t::TEST;
	/// seed of the random map generator; same seed and size = same map
	uint32_t seed = 1;
	/// edge length of the random map in tiles (rounded to a multiple of 16, 48..256)
	size_t size = 64;
	/// upper limit for tree entities on the random map (performance on standalone headsets)
	size_t max_trees = 800;
	/// maximum terrain elevation of hills on the random map (tiles, 0 = flat)
	float max_elevation = 4.0f;
	/// initial camera view; random maps look at the first start position if unset
	std::optional<MapView> view{};
	/// XR fork test option: a small army per player between the starts
	/// (gamestate/combat/skirmish.h), the camera looks at the battlefield
	bool skirmish = false;
};

} // namespace openage::gamestate
