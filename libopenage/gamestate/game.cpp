// Copyright 2018-2024 the openage authors. See copying.md for legal info.

#include "game.h"

#include <array>
#include <chrono>
#include <optional>
#include <vector>

#include <nyan/nyan.h>

#include "log/log.h"
#include "log/message.h"

#include "assets/mod_manager.h"
#include "assets/modpack.h"
#include "gamestate/api/terrain.h"
#include "gamestate/combat/combat_state.h"
#include "gamestate/combat/skirmish.h"
#include "gamestate/component/internal/activity.h"
#include "gamestate/component/internal/ownership.h"
#include "gamestate/component/internal/position.h"
#include "gamestate/component/types.h"
#include "gamestate/entity_factory.h"
#include "gamestate/game_entity.h"
#include "gamestate/game_state.h"
#include "gamestate/heightmap.h"
#include "gamestate/manager.h"
#include "gamestate/map.h"
#include "gamestate/map_generator.h"
#include "gamestate/player.h"
#include "gamestate/terrain.h"
#include "gamestate/terrain_chunk.h"
#include "gamestate/terrain_factory.h"
#include "gamestate/terrain_tile.h"
#include "gamestate/types.h"
#include "gamestate/universe.h"
#include "time/time.h"
#include "util/path.h"
#include "util/strings.h"

#include "coord/tile.h"

namespace openage::gamestate {

Game::Game(const std::shared_ptr<openage::event::EventLoop> &event_loop,
           const std::shared_ptr<assets::ModManager> &mod_manager,
           const std::shared_ptr<EntityFactory> &entity_factory,
           const std::shared_ptr<TerrainFactory> &terrain_factory,
           const MapSettings &map_settings) :
	db{nyan::Database::create()},
	state{std::make_shared<GameState>(this->db, event_loop)},
	universe{std::make_shared<Universe>(state)} {
	this->load_data(mod_manager);

	// TODO: Testing player creation
	auto player1 = entity_factory->add_player(event_loop, state, "");
	auto player2 = entity_factory->add_player(event_loop, state, "");
	state->add_player(player1);
	state->add_player(player2);

	// TODO: This lets the spawner event check which modpacks are loaded,
	//       so that it can decide which entities it can spawn.
	//       This can be removed when we spawn based on game logic rather than
	//       hardcoded entity types.
	this->state->set_mod_manager(mod_manager);

	// XR fork: auto attacks and the victory condition (gamestate/combat)
	this->state->get_combat()->start(this->state, time::TIME_ZERO);

	if (map_settings.type == map_type_t::RANDOM
	    and this->generate_random_map(event_loop, entity_factory, terrain_factory, map_settings)) {
		return;
	}
	this->generate_terrain(terrain_factory);
}

const std::shared_ptr<GameState> &Game::get_state() const {
	return this->state;
}

const std::optional<MapView> &Game::get_start_view() const {
	return this->start_view;
}

std::optional<resource_amounts_t> Game::get_player_resources(player_id_t player) const {
	if (not this->state->has_player(player)) {
		return std::nullopt;
	}
	return this->state->get_player(player)->get_resources().get();
}

void Game::attach_renderer(const std::shared_ptr<renderer::RenderFactory> &render_factory) {
	this->universe->attach_renderer(render_factory);
	this->state->get_map()->get_terrain()->attach_renderer(render_factory);
}

void Game::load_data(const std::shared_ptr<assets::ModManager> &mod_manager) {
	auto load_order = mod_manager->get_load_order();

	for (auto &mod_id : load_order) {
		auto mod = mod_manager->get_modpack(mod_id);
		auto info = mod->get_info();

		auto includes = info.includes;
		for (const auto &include : includes) {
			// handle wildcards
			auto parts = util::split(include, '/');
			auto last_part = parts.back();
			bool recursive = false;
			auto search = include;
			if (last_part == "**") {
				recursive = true;
				if (parts.size() == 1) {
					// include = "**"
					// start in root directory
					search = "";
				}
				else {
					// include = "path/to/somewhere/**"
					// remove the wildcard '**' and the slash '/'
					search = include.substr(0, include.size() - 3);
				}
			}

			this->load_path(info.path.get_parent(), info.path.get_name(), search, recursive);
		}
	}
}

void Game::load_path(const util::Path &base_dir,
                     const std::string &mod_dir,
                     const std::string &search,
                     bool recursive) {
	auto base_path = base_dir.resolve_native_path();
	auto search_path = base_dir / mod_dir / search;

	auto fileload_func = [&base_path](const std::string &filename) {
		// nyan wants a string filepath, so we have to construct it from the
		// path and subpath parameters
		log::log(INFO << "Loading .nyan file: " << filename);
		auto loc = base_path + "/" + filename;
		return std::make_shared<nyan::File>(loc);
	};

	// file loading
	if (search_path.is_file() and search_path.get_suffix() == ".nyan") {
		auto loc = mod_dir + "/" + search;
		this->db->load(loc, fileload_func);
		return;
	}

	// directory loading
	if (search_path.is_dir()) {
		// load all files in a directory
		for (auto p : search_path.iterdir()) {
			if (p.is_dir() and not recursive) {
				// folders are skipped unless we read recursively
				continue;
			}

			auto new_search = search + "/" + p.get_name();
			this->load_path(base_dir, mod_dir, new_search, recursive);
		}
	}
}

void Game::generate_terrain(const std::shared_ptr<TerrainFactory> &terrain_factory) {
	auto chunk0 = terrain_factory->add_chunk(this->state,
	                                         util::Vector2s{10, 10},
	                                         coord::tile_delta{0, 0});
	auto chunk1 = terrain_factory->add_chunk(this->state,
	                                         util::Vector2s{10, 10},
	                                         coord::tile_delta{10, 0});
	auto chunk2 = terrain_factory->add_chunk(this->state,
	                                         util::Vector2s{10, 10},
	                                         coord::tile_delta{0, 10});
	auto chunk3 = terrain_factory->add_chunk(this->state,
	                                         util::Vector2s{10, 10},
	                                         coord::tile_delta{10, 10});

	auto terrain = terrain_factory->add_terrain({20, 20}, {chunk0, chunk1, chunk2, chunk3});

	auto map = std::make_shared<Map>(this->state, terrain);
	this->state->set_map(map);
}

namespace {

/**
 * nyan objects of the random map for a modpack with the AoE II naming of the converter.
 */
struct RandomMapObjects {
	std::array<nyan::fqon_t, static_cast<size_t>(map_terrain_t::COUNT)> terrain;
	std::array<nyan::fqon_t, static_cast<size_t>(map_object_t::COUNT)> objects;
};

RandomMapObjects random_map_objects(const std::string &modpack) {
	auto terrain = [&](const std::string &dir, const std::string &name) {
		return modpack + ".data.terrain." + dir + "." + dir + "." + name;
	};
	auto entity = [&](const std::string &dir, const std::string &name) {
		return modpack + ".data.game_entity.generic." + dir + "." + dir + "." + name;
	};
	RandomMapObjects result;
	auto t = [&](map_terrain_t kind) -> nyan::fqon_t & {
		return result.terrain[static_cast<size_t>(kind)];
	};
	t(map_terrain_t::GRASS) = terrain("grass", "Grass");
	t(map_terrain_t::GRASS2) = terrain("grass2", "Grass2");
	t(map_terrain_t::GRASS3) = terrain("grass3", "Grass3");
	t(map_terrain_t::DIRT) = terrain("dirt", "Dirt");
	// dirt2 has no texture in the converted HD data
	t(map_terrain_t::DIRT2) = terrain("dirt3", "Dirt3");
	t(map_terrain_t::DIRT3) = terrain("dirt3", "Dirt3");
	// forest floor: "leaves" has the texture ("forest" itself has none after conversion)
	t(map_terrain_t::FOREST) = terrain("leaves", "Leaves");
	t(map_terrain_t::BEACH) = terrain("beach", "Beach");
	t(map_terrain_t::SHALLOWS) = terrain("shallows", "Shallows");
	t(map_terrain_t::WATER) = terrain("water", "Water");
	t(map_terrain_t::WATER_MEDIUM) = terrain("water3", "Water3");
	t(map_terrain_t::WATER_DEEP) = terrain("water2", "Water2");
	// landscape presets (map_biome_t); "desert" has no texture, palm_desert is the sand
	t(map_terrain_t::SAND) = terrain("palm_desert", "PalmDesert");
	t(map_terrain_t::SNOW) = terrain("snow", "Snow");
	t(map_terrain_t::SNOW_GRASS) = terrain("snow_grass", "SnowGrass");
	t(map_terrain_t::SNOW_DIRT) = terrain("snow_desert", "SnowDesert");
	t(map_terrain_t::SNOW_FOREST) = terrain("snow_forest", "SnowForest");
	t(map_terrain_t::ICE) = terrain("ice", "Ice");

	auto o = [&](map_object_t kind) -> nyan::fqon_t & {
		return result.objects[static_cast<size_t>(kind)];
	};
	o(map_object_t::TREE_PINE) = entity("conifer", "Conifer");
	o(map_object_t::TREE_JUNGLE) = entity("jungle_tree", "JungleTree");
	o(map_object_t::GOLD) = entity("gold_mine", "GoldMine");
	o(map_object_t::STONE) = entity("stone_mine", "StoneMine");
	o(map_object_t::BERRIES) = entity("berry_bush", "BerryBush");
	o(map_object_t::TOWN_CENTER) = entity("town_center", "TownCenter");
	o(map_object_t::VILLAGER) = entity("villager", "Villager");
	// landscape presets: trees are wood (Harvestable), deer and fish food, cactus decoration
	o(map_object_t::TREE_PALM) = entity("palm_tree", "PalmTree");
	o(map_object_t::TREE_SNOW) = entity("snowy_conifer", "SnowyConifer");
	o(map_object_t::TREE_BAMBOO) = entity("bamboo_forest", "BambooForest");
	o(map_object_t::CACTUS) = entity("cactus", "Cactus");
	o(map_object_t::DEER) = entity("deer", "Deer");
	o(map_object_t::FISH_SHORE) = entity("shore_fish", "Shorefish");
	o(map_object_t::FISH_OCEAN) = entity("ocean_fish", "OceanFish");
	return result;
}

} // namespace

bool Game::generate_random_map(const std::shared_ptr<openage::event::EventLoop> &event_loop,
                               const std::shared_ptr<EntityFactory> &entity_factory,
                               const std::shared_ptr<TerrainFactory> &terrain_factory,
                               const MapSettings &settings) {
	auto t0 = std::chrono::steady_clock::now();

	// the generator needs the AoE II terrain and objects (hd_base, aoe2_base)
	auto db_view = this->state->get_db_view();
	std::optional<RandomMapObjects> names;
	for (const auto &modpack : this->state->get_mod_manager()->get_load_order()) {
		if (modpack != "hd_base" and modpack != "aoe2_base") {
			continue;
		}
		auto candidate = random_map_objects(modpack);
		try {
			for (const auto &fqon : candidate.terrain) {
				db_view->get_object(fqon);
			}
			for (const auto &fqon : candidate.objects) {
				db_view->get_object(fqon);
			}
			names = candidate;
			break;
		}
		catch (std::exception &err) {
			log::log(WARN << "Random map: modpack " << modpack << " lacks objects: " << err.what());
		}
	}
	if (not names) {
		log::log(WARN << "Random map: no modpack with AoE II terrain (hd_base, aoe2_base), using the test map");
		return false;
	}

	auto generated = generate_map(settings);
	auto t1 = std::chrono::steady_clock::now();

	// terrain objects and texture paths per kind
	std::array<nyan::Object, static_cast<size_t>(map_terrain_t::COUNT)> terrain_objs;
	std::array<std::string, static_cast<size_t>(map_terrain_t::COUNT)> terrain_paths;
	for (size_t k = 0; k < terrain_objs.size(); ++k) {
		terrain_objs[k] = db_view->get_object(names->terrain[k]);
		terrain_paths[k] = api::APITerrain::get_terrain_path(terrain_objs[k]);
	}

	// chunks of 16x16 tiles (MAX_CHUNK_WIDTH), rows from left to right, top to bottom
	const size_t width = generated.width;
	const size_t height = generated.height;
	const size_t chunk = std::min(MAX_CHUNK_WIDTH, MAX_CHUNK_HEIGHT);
	std::vector<std::shared_ptr<TerrainChunk>> chunks;
	for (size_t cy = 0; cy < height; cy += chunk) {
		for (size_t cx = 0; cx < width; cx += chunk) {
			std::vector<TerrainTile> tiles;
			tiles.reserve(chunk * chunk);
			std::vector<float> corners;
			corners.reserve((chunk + 1) * (chunk + 1));
			for (size_t y = 0; y < chunk; ++y) {
				for (size_t x = 0; x < chunk; ++x) {
					auto kind = static_cast<size_t>(generated.tiles[(cx + x) + (cy + y) * width]);
					auto corner = [&](size_t dx, size_t dy) {
						return generated.corners[(cx + x + dx) + (cy + y + dy) * (width + 1)];
					};
					float mean = 0.25f * (corner(0, 0) + corner(1, 0) + corner(0, 1) + corner(1, 1));
					tiles.push_back({terrain_objs[kind],
					                 terrain_paths[kind],
					                 terrain_elevation_t::from_float(mean)});
				}
			}
			for (size_t y = 0; y <= chunk; ++y) {
				for (size_t x = 0; x <= chunk; ++x) {
					corners.push_back(generated.corners[(cx + x) + (cy + y) * (width + 1)]);
				}
			}
			chunks.push_back(terrain_factory->add_chunk(
				util::Vector2s{chunk, chunk},
				coord::tile_delta{static_cast<coord::tile_t>(cx), static_cast<coord::tile_t>(cy)},
				std::move(tiles),
				std::move(corners)));
		}
	}
	auto terrain = terrain_factory->add_terrain({width, height}, std::move(chunks));

	std::vector<coord::tile> blocked;
	blocked.reserve(generated.blocked.size());
	for (auto idx : generated.blocked) {
		blocked.push_back(coord::tile{static_cast<coord::tile_t>(idx % width),
		                              static_cast<coord::tile_t>(idx / width)});
	}
	auto map = std::make_shared<Map>(this->state,
	                                 terrain,
	                                 Heightmap{width, height, generated.corners},
	                                 blocked);
	this->state->set_map(map);
	auto t2 = std::chrono::steady_clock::now();

	// XR fork (economy): trees and resources belong to gaia, the neutral player after the
	// players (map_generator.h); create it (and missing players) before spawning
	for (const auto &object : generated.objects) {
		auto owner = static_cast<player_id_t>(object.owner);
		while (not this->state->has_player(owner)) {
			auto player = entity_factory->add_player(event_loop, this->state, "");
			this->state->add_player(player);
			if (player->get_id() >= owner) {
				break;
			}
		}
	}

	// XR fork (combat): gaia is never an enemy and does not count for the victory condition
	this->state->get_combat()->set_neutral_players({static_cast<player_id_t>(generated.starts.size())});

	// objects as entities (villagers gather from trees, mines and bushes, see econ.h)
	const auto time = time::TIME_ZERO;
	for (const auto &object : generated.objects) {
		const auto &fqon = names->objects[static_cast<size_t>(object.kind)];
		auto owner = static_cast<player_id_t>(object.owner);
		auto entity = entity_factory->add_game_entity(event_loop, this->state, owner, fqon);

		auto entity_pos = std::dynamic_pointer_cast<component::Position>(
			entity->get_component(component::component_t::POSITION));
		coord::phys3 pos{coord::phys_t{object.ne}, coord::phys_t{object.se}, coord::phys_t{0.0}};
		entity_pos->set_position(time, map->on_terrain(pos));
		entity_pos->set_angle(time, coord::phys_angle_t::from_int(object.angle));

		auto entity_owner = std::dynamic_pointer_cast<component::Ownership>(
			entity->get_component(component::component_t::OWNERSHIP));
		entity_owner->set_owner(time, owner);

		auto activity = std::dynamic_pointer_cast<component::Activity>(
			entity->get_component(component::component_t::ACTIVITY));
		activity->init(time);
		entity->get_manager()->run_activity_system(time);

		this->state->add_game_entity(entity);
	}

	// XR fork: skirmish test option, a small army per player between the starts
	std::optional<combat::SkirmishLayout> skirmish;
	if (settings.skirmish) {
		skirmish = combat::skirmish_layout(generated);
		if (not skirmish->placed) {
			log::log(WARN << "Random map: no room for the skirmish armies");
		}
		const auto &modpack = names->objects[0].substr(0, names->objects[0].find('.'));
		for (const auto &unit : skirmish->units) {
			auto dir = std::string{combat::to_string(unit.kind)};
			auto cls = dir == "knight" ? "Knight" : (dir == "militia" ? "Militia" : "Archer");
			auto fqon = modpack + ".data.game_entity.generic." + dir + "." + dir + "." + cls;
			auto owner = static_cast<player_id_t>(unit.owner);
			auto entity = entity_factory->add_game_entity(event_loop, this->state, owner, fqon);
			auto entity_pos = std::dynamic_pointer_cast<component::Position>(
				entity->get_component(component::component_t::POSITION));
			coord::phys3 pos{coord::phys_t{unit.ne}, coord::phys_t{unit.se}, coord::phys_t{0.0}};
			entity_pos->set_position(time, map->on_terrain(pos));
			coord::phys3_delta face{coord::phys_t{unit.face_ne}, coord::phys_t{unit.face_se}, coord::phys_t{0.0}};
			entity_pos->set_angle(time, face.to_angle());
			auto entity_owner = std::dynamic_pointer_cast<component::Ownership>(
				entity->get_component(component::component_t::OWNERSHIP));
			entity_owner->set_owner(time, owner);
			auto activity = std::dynamic_pointer_cast<component::Activity>(
				entity->get_component(component::component_t::ACTIVITY));
			activity->init(time);
			entity->get_manager()->run_activity_system(time);
			this->state->add_game_entity(entity);
		}
		log::log(INFO << "Random map: skirmish, " << skirmish->units.size() << " units around tile ("
		              << skirmish->center_ne << ", " << skirmish->center_se << ")");
	}

	auto t3 = std::chrono::steady_clock::now();

	// camera: settings, else the first start position
	if (settings.view) {
		this->start_view = settings.view;
	}
	else if (skirmish and skirmish->placed) {
		MapView view;
		view.ne = skirmish->center_ne;
		view.se = skirmish->center_se;
		this->start_view = view;
	}
	else if (not generated.starts.empty()) {
		MapView view;
		view.ne = generated.starts[0][0];
		view.se = generated.starts[0][1];
		this->start_view = view;
	}

	using ms = std::chrono::duration<double, std::milli>;
	log::log(INFO << "Random map (seed " << settings.seed << "): " << generated.summary());
	log::log(INFO << "Random map: generator " << ms(t1 - t0).count() << " ms, terrain + pathfinding "
	              << ms(t2 - t1).count() << " ms, " << generated.objects.size() << " entities "
	              << ms(t3 - t2).count() << " ms");
	return true;
}

} // namespace openage::gamestate
