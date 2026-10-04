// Copyright 2018-2024 the openage authors. See copying.md for legal info.

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "gamestate/map_settings.h"
#include "gamestate/resources.h"
#include "gamestate/types.h"

namespace nyan {
class Database;
}

namespace openage {

namespace assets {
class ModManager;
}

namespace event {
class EventLoop;
}

namespace renderer {
class RenderFactory;
}

namespace util {
class Path;
}

namespace gamestate {
class GameState;
class EntityFactory;
class TerrainFactory;
class Universe;

// ai (XR fork)
namespace ai {
class AiPlayer;
}
namespace prod {
class Production;
}

/**
 * Manages a game session (settings, win conditions, etc.).
 *
 * TODO: Create sensible structure of gamestate classes. Right now it's
 *
 *       Game->Universe|->World->GameEntity
 *                     |->Terrain
 *
 * 		which can be confusing.
 */
class Game {
public:
	/**
	 * Create a new game.
	 *
	 * @param event_loop Event simulation loop for the gamestate.
	 * @param mod_manager Mod manager.
	 * @param entity_factory Factory for creating entities. Used for creating the players.
	 * @param terrain_factory Factory for creating terrain objects.
	 * @param map_settings Map of the game (XR fork; default: fixed test map).
	 */
	Game(const std::shared_ptr<openage::event::EventLoop> &event_loop,
	     const std::shared_ptr<assets::ModManager> &mod_manager,
	     const std::shared_ptr<EntityFactory> &entity_factory,
	     const std::shared_ptr<TerrainFactory> &terrain_factory,
	     const MapSettings &map_settings = {});
	~Game() = default;

	/**
	 * Get the current game state.
	 */
	const std::shared_ptr<GameState> &get_state() const;

	/**
	 * Initial camera view for the map (XR fork).
	 *
	 * @return View for random maps (settings or first start position), nothing
	 *         for the test map (the presenter keeps its default camera).
	 */
	const std::optional<MapView> &get_start_view() const;

	/**
	 * Resource stockpile of a player (XR fork, economy; for HUDs).
	 *
	 * Thread-safe: may be called from any thread while the game exists.
	 *
	 * @param player Player ID (0 = first player).
	 *
	 * @return Amounts indexed by resource_t (food, wood, gold, stone), nothing if
	 *         there is no such player.
	 */
	std::optional<resource_amounts_t> get_player_resources(player_id_t player) const;

	/**
	 * Attach a renderer to the game which enables graphical display options for
	 * all ingame entities.
	 *
	 * @param render_factory Factory for creating connector objects for gamestate->renderer
	 *                       communication.
	 */
	void attach_renderer(const std::shared_ptr<renderer::RenderFactory> &render_factory);

	// ---- ai (XR fork) ----
	/**
	 * Computer opponents of the game (empty if MapSettings::ai is off).
	 * The objects live as long as the game; get_status() is thread-safe.
	 */
	const std::vector<std::shared_ptr<ai::AiPlayer>> &get_ai_players() const;

	/**
	 * Let the computer opponents train and build through the production of
	 * the simulation (before that they only log their wishes).
	 */
	void connect_ai_production(const std::shared_ptr<prod::Production> &production);
	// ---- end ai (XR fork) ----

private:
	// ---- ai (XR fork) ----
	/**
	 * Create and start the computer opponents (random maps, see MapSettings::ai).
	 */
	void start_ai(const std::shared_ptr<openage::event::EventLoop> &event_loop,
	              const MapSettings &settings);

	/// start positions of the random map (0: test map)
	size_t start_count = 0;

	std::vector<std::shared_ptr<ai::AiPlayer>> ai_players;
	// ---- end ai (XR fork) ----

	/**
	 * Load game data from the filesystem.
	 *
	 * @param mod_manager Mod manager.
	 */
	void load_data(const std::shared_ptr<assets::ModManager> &mod_manager);

	/**
	 * Load game data from the filesystem recursively.
	 *
	 * TODO: Move this into nyan.
	 *
	 * @param base_dir Base directory where mods are stored.
	 * @param mod_dir Name of the mod directory.
	 * @param search Search path relative to the mod directory.
	 * @param recursive if true, recursively search subfolders if the the search path is a directory.
	 */
	void load_path(const util::Path &base_dir,
	               const std::string &mod_dir,
	               const std::string &search,
	               bool recursive = false);

	/**
	 * Generate the terrain for the current game.
	 *
	 * TODO: Use a real map generator.
	 *
	 * @param terrain_factory Factory for creating terrain objects.
	 */
	void generate_terrain(const std::shared_ptr<TerrainFactory> &terrain_factory);

	/**
	 * Generate a random map (gamestate/map_generator.h): terrain, elevation and
	 * the objects (trees, resources, town centers, villagers) as entities
	 * without game logic.
	 *
	 * @param event_loop Event loop of the game state.
	 * @param entity_factory Factory for the map objects.
	 * @param terrain_factory Factory for creating terrain objects.
	 * @param settings Map settings.
	 *
	 * @return false if the modpack lacks the required terrain/objects (nothing created).
	 */
	bool generate_random_map(const std::shared_ptr<openage::event::EventLoop> &event_loop,
	                         const std::shared_ptr<EntityFactory> &entity_factory,
	                         const std::shared_ptr<TerrainFactory> &terrain_factory,
	                         const MapSettings &settings);

	/**
	 * Initial camera view (random maps only).
	 */
	std::optional<MapView> start_view;

	/**
	 * Nyan game data database.
	 */
	std::shared_ptr<nyan::Database> db;

	/**
	 * State of the current game.
	 */
	std::shared_ptr<GameState> state;

	/**
	 * Object that controls entities in the game world.
	 */
	std::shared_ptr<Universe> universe;
};

} // namespace gamestate
} // namespace openage
