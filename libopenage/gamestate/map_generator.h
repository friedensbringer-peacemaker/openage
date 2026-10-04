// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "gamestate/map_settings.h"


namespace openage::gamestate {

/**
 * Terrain kinds of the random map generator (XR fork).
 * Game::generate_terrain maps them to the terrain objects of the modpack.
 */
enum class map_terrain_t : uint8_t {
	GRASS,
	GRASS2,
	GRASS3,
	DIRT,
	DIRT2,
	DIRT3,
	FOREST,       ///< forest floor (trees are separate objects)
	BEACH,        ///< land next to water
	SHALLOWS,     ///< ford, passable for land units
	WATER,        ///< water next to the shore
	WATER_MEDIUM, ///< 2-3 tiles from the shore
	WATER_DEEP,   ///< 4+ tiles from the shore
	// landscape presets (map_biome_t), appended so the values above stay the same
	SAND,        ///< desert sand
	SNOW,        ///< snow
	SNOW_GRASS,  ///< snow with grass
	SNOW_DIRT,   ///< snow with sand/dirt (winter shore)
	SNOW_FOREST, ///< snowy forest floor
	ICE,         ///< frozen lake, passable for land units
	COUNT,
};

/**
 * Objects placed by the random map generator.
 */
enum class map_object_t : uint8_t {
	TREE_PINE,
	TREE_JUNGLE,
	GOLD,
	STONE,
	BERRIES,
	TOWN_CENTER,
	VILLAGER,
	// landscape presets (map_biome_t), appended so the values above stay the same
	TREE_PALM,   ///< wood, desert and coast
	TREE_SNOW,   ///< wood, winter
	TREE_BAMBOO, ///< wood, jungle
	CACTUS,      ///< decoration, blocks the tile
	DEER,        ///< food (animal, gaia), not blocking
	FISH_SHORE,  ///< food on water next to the shore
	FISH_OCEAN,  ///< food on deep water (big ocean fish)
	COUNT,
};

/**
 * One object on the generated map.
 */
struct MapObject {
	map_object_t kind;
	/// position in tiles (the entity position, i.e. its anchor point)
	double ne;
	double se;
	/// facing in degrees (for trees: selects the sprite variant)
	int angle;
	/// owning player (town centres, villagers: the start's player; trees and resources:
	/// gaia = number of starts, the neutral player after the players, see game.cpp)
	size_t owner;
};

/**
 * Result of the random map generator.
 *
 * Tile (ne, se) has the index ne + se * width (like the terrain chunks and
 * the path cost fields), corner (ne, se) the index ne + se * (width + 1).
 */
struct GeneratedMap {
	size_t width = 0;
	size_t height = 0;
	std::vector<map_terrain_t> tiles{};
	/// (width + 1) * (height + 1) corner elevations (see gamestate/heightmap.h)
	std::vector<float> corners{};
	std::vector<MapObject> objects{};
	/// tiles that land units cannot cross (trees, mines, bushes, town centers)
	std::vector<size_t> blocked{};
	/// start position (town center) per player
	std::vector<std::array<double, 2>> starts{};
	/// true if the map has a river (with fords)
	bool river = false;
	/// landscape preset the map was generated with
	map_biome_t biome = map_biome_t::GRASSLAND;
	/// terrain kind under the forests of this preset
	map_terrain_t forest_floor = map_terrain_t::FOREST;
	/// fords or forest paths carved afterwards to connect the starts (tiles; 0 for most maps)
	size_t carved = 0;

	/// number of tiles per terrain kind
	std::array<size_t, static_cast<size_t>(map_terrain_t::COUNT)> terrain_count() const;
	/// number of objects per kind
	std::array<size_t, static_cast<size_t>(map_object_t::COUNT)> object_count() const;
	/// one-line summary for logs
	std::string summary() const;
};

/**
 * @return true for the tree objects (wood).
 */
bool map_object_is_tree(map_object_t object);

/**
 * @return true for objects that stand on water (fish).
 */
bool map_object_on_water(map_object_t object);

/**
 * Limits of a landscape preset, the same values the generator uses (for checks and menus).
 */
struct MapBiomeLimits {
	/// highest corner elevation (settings.max_elevation scaled by the preset)
	float max_elevation = 0.0f;
	/// largest height difference of neighbouring corners
	float max_slope = 0.0f;
	/// radius around a start position that is flat and dry
	double flat_radius = 0.0;
	/// upper limit for tree objects (settings.max_trees scaled by the preset)
	size_t max_trees = 0;
	/// expected share of open water tiles (water kinds, without shallows and ice)
	double water_min = 0.0;
	double water_max = 1.0;
	/// expected share of forest floor tiles
	double forest_min = 0.0;
	double forest_max = 1.0;
};

/**
 * Limits of the preset settings.biome for these settings.
 */
MapBiomeLimits map_biome_limits(const MapSettings &settings);

/**
 * Generate a random map (deterministic: same settings = same map on every platform).
 *
 * Layout: two players on opposite sides (left/right on screen) with a flat
 * start area (town center, 3 villagers, berries, gold, stone), lakes and
 * optionally a river with fords between the players (shore: beach, water
 * depth by distance), grass/dirt variants, forests with tree objects (at most
 * settings.max_trees) and hills from smooth noise (no elevation on water, the
 * shore or the start areas, slopes limited).
 *
 * The landscape preset settings.biome changes water, hills, forests, terrain
 * and objects (see map_biome_t); GRASSLAND is the layout described above. All
 * presets keep the rules: two starts with town center, villagers and
 * resources, gaia owns trees/resources/animals, the starts are connected for
 * land units (fords or forest paths are carved if necessary).
 *
 * @param settings Seed, size, preset and limits (type is ignored).
 *
 * @return Generated map.
 */
GeneratedMap generate_map(const MapSettings &settings);

/**
 * Edge length the generator uses for a requested size (multiple of 16, 48..256).
 */
size_t map_generator_size(size_t requested);

/**
 * Names for logs.
 */
const char *to_string(map_terrain_t terrain);
const char *to_string(map_object_t object);

} // namespace openage::gamestate
