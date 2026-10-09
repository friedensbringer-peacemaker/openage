// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>


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
 * Landscape preset of the random map generator (XR fork).
 *
 * GRASSLAND is the original random map (same seed = same map as before the
 * presets existed). The other presets are our own layouts, inspired by the
 * classic map kinds of the genre (no map scripts or texts of the original game).
 */
enum class map_biome_t : uint8_t {
	/// green land, a few lakes, sometimes a river (the original random map)
	GRASSLAND,
	/// open dry land, little water, small groves, many low hills
	STEPPE,
	/// high hills with ridges and steep slopes, small flat start areas
	HILLS,
	/// dense forest everywhere, clearings at the starts, narrow paths between them
	FOREST,
	/// several rivers with fords divide the map
	RIVERS,
	/// sea along one side of the map
	COAST,
	/// large sea in the middle, land route around it
	INLAND_SEA,
	/// much water, islands connected by fords
	ISLANDS,
	/// central forest with a large gold field in the middle
	GOLD_RUSH,
	/// sand and dirt, oases with palms, cacti
	DESERT,
	/// snow, snowy conifers, frozen lakes, deer
	WINTER,
	/// lush green, dense jungle and bamboo, many lakes
	JUNGLE,
	COUNT,
};

/**
 * Short name of a landscape preset (command line, logs, settings files).
 */
inline const char *map_biome_name(map_biome_t biome) {
	switch (biome) {
	case map_biome_t::GRASSLAND:
		return "grass";
	case map_biome_t::STEPPE:
		return "steppe";
	case map_biome_t::HILLS:
		return "hills";
	case map_biome_t::FOREST:
		return "forest";
	case map_biome_t::RIVERS:
		return "rivers";
	case map_biome_t::COAST:
		return "coast";
	case map_biome_t::INLAND_SEA:
		return "inland-sea";
	case map_biome_t::ISLANDS:
		return "water";
	case map_biome_t::GOLD_RUSH:
		return "gold-rush";
	case map_biome_t::DESERT:
		return "desert";
	case map_biome_t::WINTER:
		return "winter";
	case map_biome_t::JUNGLE:
		return "jungle";
	default:
		return "?";
	}
}

/**
 * Landscape preset from its short name (map_biome_name, also "grassland", "islands").
 *
 * @return true if the name is known.
 */
inline bool map_biome_parse(std::string_view name, map_biome_t &biome) {
	if (name == "grassland") {
		biome = map_biome_t::GRASSLAND;
		return true;
	}
	if (name == "islands") {
		biome = map_biome_t::ISLANDS;
		return true;
	}
	for (size_t k = 0; k < static_cast<size_t>(map_biome_t::COUNT); ++k) {
		auto candidate = static_cast<map_biome_t>(k);
		if (name == map_biome_name(candidate)) {
			biome = candidate;
			return true;
		}
	}
	return false;
}

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

// ---- ai (XR fork) ----

/**
 * Computer opponent switch (gamestate/ai).
 */
enum class ai_mode_t {
	/// on for random maps with two or more start positions, off otherwise
	AUTO,
	ON,
	OFF,
};

/**
 * Difficulty of the computer opponent (gamestate/ai/ai_rules.h: params_for()).
 */
enum class ai_difficulty_t {
	/// slow reactions, first attack after 8 minutes at the earliest
	EASY,
	/// faster reactions, earlier and smaller attacks
	NORMAL,
};

/**
 * Settings of the computer opponent. Unset optionals keep the values of the difficulty.
 */
struct AiSettings {
	ai_mode_t mode = ai_mode_t::AUTO;
	ai_difficulty_t difficulty = ai_difficulty_t::EASY;
	/// controlled player; default: the last player before gaia (player 1 of two)
	std::optional<uint64_t> player{};
	/// earliest first attack (simulation seconds)
	std::optional<double> first_attack{};
	/// army size that starts an attack wave
	std::optional<size_t> attack_size{};
	/// seed of the decisions (0: map seed)
	uint32_t seed = 0;
};

// ---- end ai (XR fork) ----

/**
 * Settings for the map of a new game.
 */
struct MapSettings {
	/// map type, the fixed test map is the default
	map_type_t type = map_type_t::TEST;
	/// landscape preset of the random map (GRASSLAND = original random map)
	map_biome_t biome = map_biome_t::GRASSLAND;
	/// seed of the random map generator; same seed and size = same map
	uint32_t seed = 1;
	/// edge length of the random map in tiles (rounded to a multiple of 16, 48..256)
	size_t size = 64;
	/// upper limit for tree entities on the random map (performance on standalone headsets;
	/// FOREST and JUNGLE allow more, see map_biome_limits() in map_generator.h)
	size_t max_trees = 800;
	/// maximum terrain elevation of hills on the random map (tiles, 0 = flat;
	/// scaled per preset, e.g. HILLS x2.25, see map_biome_limits())
	float max_elevation = 4.0f;
	/// initial camera view; random maps look at the first start position if unset
	std::optional<MapView> view{};
	/// XR fork test option: a small army per player between the starts
	/// (gamestate/combat/skirmish.h), the camera looks at the battlefield
	bool skirmish = false;
	// ai (XR fork): computer opponent
	AiSettings ai{};
	/// XR fork (save games): restore this save file after the map was generated
	/// (gamestate/save_game.h); the other fields must be the map settings of the
	/// file. Empty: a new game.
	std::string load_file{};
};

} // namespace openage::gamestate
