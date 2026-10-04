// Copyright 2013-2024 the openage authors. See copying.md for legal info.

#pragma once

#include <atomic>
#include <shared_mutex>

#include "gamestate/map_settings.h"
#include "util/path.h"

namespace openage {

namespace assets {
class ModManager;
}

namespace cvar {
class CVarManager;
}

namespace event {
class EventLoop;
} // namespace event

namespace renderer {
class RenderFactory;
}

namespace time {
class TimeLoop;
} // namespace time

namespace gamestate {
class EntityFactory;
class Game;
class TerrainFactory;

namespace event {
class Commander;
class Spawner;
} // namespace event

/**
 * Gameplay subsystem of the engine.
 *
 * Manages the state of the game world and the event loop.
 */
class GameSimulation final {
public:
	/**
	 * Create the game simulation subsystems depending on the requested run mode.
	 *
	 * @param root_dir openage root directory.
	 * @param cvar_manager Environment variable manager.
	 * @param time_loop Time management loop.
	 */
	GameSimulation(const util::Path &root_dir,
	               const std::shared_ptr<cvar::CVarManager> &cvar_manager,
	               const std::shared_ptr<openage::time::TimeLoop> time_loop);

	// game simulation should not be copied or moved
	GameSimulation(const GameSimulation &copy) = delete;
	GameSimulation &operator=(const GameSimulation &copy) = delete;
	GameSimulation(GameSimulation &&other) = delete;
	GameSimulation &operator=(GameSimulation &&other) = delete;
	~GameSimulation() = default;

	/**
	 * Run the simulation loop.
	 */
	void run();

	/**
	 * Start the simulation loop.
	 */
	void start();

	/**
	 * Stop of the simulation loop.
	 */
	void stop();

	/**
	 * Get the root directory of the simulation executable.
	 */
	const util::Path &get_root_dir();

	/**
	 * Get this simulation's cvar manager.
	 *
	 * @return CVarManager instance.
	 */
	const std::shared_ptr<cvar::CVarManager> get_cvar_manager();

	/**
	 * Get the game running in the simulation.
	 *
	 * @return Game instance.
	 */
	const std::shared_ptr<gamestate::Game> get_game();

	/**
	 * Get the game without waiting (XR fork): start() holds the lock while the
	 * game is created (modpacks, map), a HUD in another thread must not block.
	 *
	 * @return Game instance, nullptr if there is none yet or the lock is busy.
	 */
	std::shared_ptr<gamestate::Game> try_get_game();

	/**
	 * Get the event loop for the gamestate.
	 *
	 * @return Event loop.
	 */
	const std::shared_ptr<openage::event::EventLoop> get_event_loop();

	/**
	 * Get the event entity for spawing game entities.
	 *
	 * TODO: Move somewhere else or remove.
	 *
	 * @return Spawner for entity creation.
	 */
	const std::shared_ptr<gamestate::event::Spawner> get_spawner();

	/**
	 * Get the event entity for sending commands.
	 *
	 * TODO: Move somewhere else or remove.
	 *
	 * @return Commander for sending commands.
	 */
	const std::shared_ptr<gamestate::event::Commander> get_commander();

	/**
	 * Attach a renderer to the simulation.
	 *
	 * @param factory Factory for creating render entities.
	 */
	void attach_renderer(const std::shared_ptr<renderer::RenderFactory> &render_factory);

	/**
	 * Set the modpacks to load for a game.
	 *
	 * @param modpacks IDs of the modpacks to load.
	 */
	void set_modpacks(const std::vector<std::string> &modpacks);

	/**
	 * Set the map of the game (XR fork). Must be called before run()/start().
	 *
	 * @param settings Map settings (default: fixed test map).
	 */
	void set_map_settings(const MapSettings &settings);

	/**
	 * current simulation state variable.
	 * to be set to false to stop the simulation loop.
	 */
	std::atomic<bool> running;

	/**
	 * Set by stop(). Keeps a stop that arrives before start() from being
	 * overwritten when the loop starts.
	 */
	std::atomic<bool> stop_requested{false};

private:
	/**
	 * Initialize event handlers.
	 */
	void init_event_handlers();

	/**
	 * The simulation root directory.
	 * Uses the openage fslike path abstraction that can mount paths into one.
	 *
	 * This means that this path does simultaneously lead to global assets,
	 * home-folder-assets, settings, and basically the whole filesystem access.
	 *
	 * TODO: move this to a settings class, which then also hosts cvar and the options system.
	 */
	util::Path root_dir;

	/**
	 * the simulation's cvar manager.
	 */
	std::shared_ptr<cvar::CVarManager> cvar_manager;

	/**
	 * Time loop for getting the current simulation time and changing speed.
	 */
	std::shared_ptr<openage::time::TimeLoop> time_loop;

	/**
	 * Event loop for processing events in the game.
	 */
	std::shared_ptr<openage::event::EventLoop> event_loop;

	/**
	 * Factory for creating game entities.
	 */
	std::shared_ptr<gamestate::EntityFactory> entity_factory;

	/**
	 * Factory for creating terrain.
	 */
	std::shared_ptr<gamestate::TerrainFactory> terrain_factory;

	/**
	 * Mod manager.
	 */
	std::shared_ptr<assets::ModManager> mod_manager;

	// TODO: move somewhere sensible or remove
	std::shared_ptr<gamestate::event::Spawner> spawner;
	std::shared_ptr<gamestate::event::Commander> commander;

	// TODO: The game run by the engine
	std::shared_ptr<gamestate::Game> game;

	/**
	 * Map of the next game (XR fork).
	 */
	MapSettings map_settings;

	/**
	 * Renderer attached before the game was created (XR fork); attached in start().
	 */
	std::shared_ptr<renderer::RenderFactory> pending_render_factory;

	/**
	 * Mutex for thread-safe access to the simulation.
	 */
	std::shared_mutex mutex;
};

} // namespace gamestate
} // namespace openage
