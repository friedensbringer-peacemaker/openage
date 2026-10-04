// Copyright 2024-2024 the openage authors. See copying.md for legal info.

#pragma once

#include <memory>
#include <unordered_map>
#include <vector>

#include <nyan/nyan.h>

#include "coord/phys.h"
#include "coord/tile.h"
#include "gamestate/heightmap.h"
#include "pathfinding/types.h"
#include "time/time.h"
#include "util/vector.h"


namespace openage {
namespace path {
class Pathfinder;
} // namespace path

namespace gamestate {
class GameState;
class Terrain;

class Map {
public:
	/**
	 * Create a new map from existing terrain.
	 *
	 * Initializes the pathfinder with the terrain path costs.
	 *
	 * @param state Game state.
	 * @param terrain Terrain object.
	 */
	Map(const std::shared_ptr<GameState> &state,
	    const std::shared_ptr<Terrain> &terrain);

	/**
	 * Create a new map with elevation and blocked tiles (XR fork, random maps).
	 *
	 * @param state Game state.
	 * @param terrain Terrain object.
	 * @param heightmap Terrain elevation (same corner heights as the terrain chunks).
	 * @param blocked Tiles occupied by objects (trees, mines, buildings); they are
	 *                impassable for all path types except air.
	 */
	Map(const std::shared_ptr<GameState> &state,
	    const std::shared_ptr<Terrain> &terrain,
	    Heightmap &&heightmap,
	    const std::vector<coord::tile> &blocked);

	~Map() = default;

	/**
	 * Get the size of the map.
	 *
	 * @return Map size (width x height).
	 */
	const util::Vector2s &get_size() const;

	/**
	 * Get the terrain of the map.
	 *
	 * @return Terrain.
	 */
	const std::shared_ptr<Terrain> &get_terrain() const;

	/**
	 * Get the pathfinder for the map.
	 *
	 * @return Pathfinder.
	 */
	const std::shared_ptr<path::Pathfinder> &get_pathfinder() const;

	/**
	 * Get the grid ID associated with a nyan path grid object.
	 *
	 * @param path_grid Path grid object fqon.
	 *
	 * @return Grid ID.
	 */
	path::grid_id_t get_grid_id(const nyan::fqon_t &path_grid) const;

	/**
	 * Get the terrain elevation of the map (flat for the test map).
	 */
	const Heightmap &get_heightmap() const;

	/**
	 * Put a position onto the terrain surface (up = elevation at ne/se).
	 *
	 * @param pos Position.
	 *
	 * @return Position with the terrain height.
	 */
	coord::phys3 on_terrain(const coord::phys3 &pos) const;

	/**
	 * Terrain point under a screen position.
	 *
	 * Positions from mouse input (coord::input::to_phys3) are intersections of
	 * the view ray with the plane up = 0. On hills the visible terrain point
	 * lies further towards the camera; this follows the ray of the default
	 * camera (Heightmap::pick). Positions with up != 0 are returned unchanged.
	 *
	 * @param plane_hit Intersection of the view ray with the plane up = 0.
	 *
	 * @return Visible terrain point.
	 */
	coord::phys3 pick_terrain(const coord::phys3 &plane_hit) const;

	/**
	 * Waypoints that follow the terrain: every segment is split into steps
	 * of at most half a tile and all points get the terrain height (no change
	 * on flat maps).
	 *
	 * @param waypoints Path waypoints.
	 *
	 * @return Waypoints on the terrain surface.
	 */
	std::vector<coord::phys3> follow_terrain(const std::vector<coord::phys3> &waypoints) const;

	/**
	 * Check if a tile can be crossed on a path grid (XR fork, economy).
	 *
	 * @param grid_id Path grid.
	 * @param tile Tile coordinates.
	 *
	 * @return true if the tile is on the map and not impassable.
	 */
	bool is_passable(path::grid_id_t grid_id, const coord::tile &tile) const;

	/**
	 * Restore the terrain path costs of a tile that was blocked by an object
	 * (XR fork, economy: a tree or mine that was gathered completely).
	 *
	 * @param tile Tile coordinates.
	 * @param time Time of the change.
	 */
	void unblock_tile(const coord::tile &tile, const time::time_t &time);

	/**
	 * Make tiles passable again (XR fork: a building was destroyed): restore
	 * the path costs of their terrain on all grids except air.
	 *
	 * @param tiles Tiles (outside the map: ignored).
	 * @param time Time of the change.
	 */
	void unblock_tiles(const std::vector<coord::tile> &tiles, const time::time_t &time);

private:
	/**
	 * Initialize the pathfinder from the terrain path costs and blocked tiles.
	 */
	void init_pathfinding(const std::shared_ptr<GameState> &state,
	                      const std::vector<coord::tile> &blocked);

	/**
	 * Terrain elevation.
	 */
	Heightmap heightmap;

	/**
	 * Terrain.
	 */
	std::shared_ptr<Terrain> terrain;

	/**
	 * Pathfinder.
	 */
	std::shared_ptr<path::Pathfinder> pathfinder;

	/**
	 * Lookup table for mapping path grid objects in nyan to grid indices.
	 */
	std::unordered_map<nyan::fqon_t, path::grid_id_t> grid_lookup;
};

} // namespace gamestate
} // namespace openage
