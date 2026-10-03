// Copyright 2024-2026 the openage authors. See copying.md for legal info.

#include "map.h"

#include <algorithm>
#include <cmath>

#include <nyan/nyan.h>

#include "gamestate/api/terrain.h"
#include "gamestate/game_state.h"
#include "gamestate/terrain.h"
#include "gamestate/terrain_chunk.h"
#include "pathfinding/cost_field.h"
#include "pathfinding/definitions.h"
#include "pathfinding/grid.h"
#include "pathfinding/pathfinder.h"
#include "pathfinding/sector.h"


namespace openage::gamestate {
Map::Map(const std::shared_ptr<GameState> &state,
         const std::shared_ptr<Terrain> &terrain) :
	heightmap{},
	terrain{terrain},
	pathfinder{std::make_shared<path::Pathfinder>()},
	grid_lookup{} {
	this->init_pathfinding(state, {});
}

Map::Map(const std::shared_ptr<GameState> &state,
         const std::shared_ptr<Terrain> &terrain,
         Heightmap &&heightmap,
         const std::vector<coord::tile> &blocked) :
	heightmap{std::move(heightmap)},
	terrain{terrain},
	pathfinder{std::make_shared<path::Pathfinder>()},
	grid_lookup{} {
	this->init_pathfinding(state, blocked);
}

void Map::init_pathfinding(const std::shared_ptr<GameState> &state,
                           const std::vector<coord::tile> &blocked) {
	// Create a grid for each path type
	// TODO: This is non-deterministic because of the unordered set. Is this a problem?
	auto nyan_db = state->get_db_view();
	std::unordered_set<nyan::fqon_t> path_types = nyan_db->get_obj_children_all("engine.util.path_type.PathType");
	size_t grid_idx = 0;
	auto chunk_size = this->terrain->get_chunk(0)->get_size();
	auto side_length = std::max(chunk_size[0], chunk_size[1]);
	auto grid_size = this->terrain->get_chunks_size();
	for (const auto &path_type : path_types) {
		auto grid = std::make_shared<path::Grid>(grid_idx, grid_size, side_length);
		this->pathfinder->add_grid(grid);

		this->grid_lookup.emplace(path_type, grid_idx);
		grid_idx += 1;
	}

	// Set path costs
	for (size_t chunk_idx = 0; chunk_idx < this->terrain->get_chunks().size(); ++chunk_idx) {
		auto chunk_terrain = this->terrain->get_chunk(chunk_idx);
		for (size_t tile_idx = 0; tile_idx < chunk_terrain->get_tiles().size(); ++tile_idx) {
			auto tile = chunk_terrain->get_tile(tile_idx);
			auto path_costs = api::APITerrain::get_path_costs(tile.terrain);

			for (const auto &path_cost : path_costs) {
				auto grid_id = this->grid_lookup.at(path_cost.first);
				auto grid = this->pathfinder->get_grid(grid_id);

				auto sector = grid->get_sector(chunk_idx);
				auto cost_field = sector->get_cost_field();
				cost_field->set_cost(tile_idx, path_cost.second, time::TIME_ZERO);
			}
		}
	}

	// XR fork: tiles occupied by objects (trees, mines, buildings) block
	// everything that is not flying
	if (not blocked.empty()) {
		const auto chunks_ne = static_cast<coord::tile_t>(grid_size[0]);
		const auto side = static_cast<coord::tile_t>(side_length);
		for (const auto &path_type : this->grid_lookup) {
			const auto &name = path_type.first;
			if (name.size() >= 4 and name.compare(name.size() - 4, 4, ".Air") == 0) {
				continue;
			}
			auto grid = this->pathfinder->get_grid(path_type.second);
			for (const auto &tile : blocked) {
				auto chunk_idx = (tile.ne / side) + (tile.se / side) * chunks_ne;
				auto tile_idx = (tile.ne % side) + (tile.se % side) * side;
				auto sector = grid->get_sector(static_cast<size_t>(chunk_idx));
				sector->get_cost_field()->set_cost(static_cast<size_t>(tile_idx),
				                                   path::COST_IMPASSABLE,
				                                   time::TIME_ZERO);
			}
		}
	}

	// Connect sectors with portals
	for (const auto &path_type : this->grid_lookup) {
		auto grid = this->pathfinder->get_grid(path_type.second);
		grid->init_portals();
		grid->init_portal_nodes();
	}
}

const util::Vector2s &Map::get_size() const {
	return this->terrain->get_size();
}

const std::shared_ptr<Terrain> &Map::get_terrain() const {
	return this->terrain;
}

const std::shared_ptr<path::Pathfinder> &Map::get_pathfinder() const {
	return this->pathfinder;
}

path::grid_id_t Map::get_grid_id(const nyan::fqon_t &path_grid) const {
	return this->grid_lookup.at(path_grid);
}

const Heightmap &Map::get_heightmap() const {
	return this->heightmap;
}

coord::phys3 Map::on_terrain(const coord::phys3 &pos) const {
	if (this->heightmap.is_flat()) {
		return pos;
	}
	auto up = this->heightmap.at(pos.ne.to_double(), pos.se.to_double());
	return coord::phys3{pos.ne, pos.se, coord::phys_t{up}};
}

coord::phys3 Map::pick_terrain(const coord::phys3 &plane_hit) const {
	if (this->heightmap.is_flat() or plane_hit.up.get_raw_value() != 0) {
		return plane_hit;
	}
	auto hit = this->heightmap.pick(plane_hit.ne.to_double(), plane_hit.se.to_double());
	return coord::phys3{coord::phys_t{hit.first.first},
	                    coord::phys_t{hit.first.second},
	                    coord::phys_t{hit.second}};
}

std::vector<coord::phys3> Map::follow_terrain(const std::vector<coord::phys3> &waypoints) const {
	if (this->heightmap.is_flat() or waypoints.empty()) {
		return waypoints;
	}

	std::vector<coord::phys3> result;
	result.reserve(waypoints.size() * 2);
	result.push_back(this->on_terrain(waypoints.front()));
	for (size_t i = 1; i < waypoints.size(); ++i) {
		double ne0 = waypoints[i - 1].ne.to_double();
		double se0 = waypoints[i - 1].se.to_double();
		double ne1 = waypoints[i].ne.to_double();
		double se1 = waypoints[i].se.to_double();
		double length = std::hypot(ne1 - ne0, se1 - se0);
		auto steps = std::max<size_t>(1, static_cast<size_t>(std::ceil(length / 0.5)));
		for (size_t k = 1; k < steps; ++k) {
			double f = static_cast<double>(k) / static_cast<double>(steps);
			double ne = ne0 + f * (ne1 - ne0);
			double se = se0 + f * (se1 - se0);
			result.emplace_back(coord::phys_t{ne}, coord::phys_t{se}, coord::phys_t{this->heightmap.at(ne, se)});
		}
		result.push_back(this->on_terrain(waypoints[i]));
	}
	return result;
}

} // namespace openage::gamestate
