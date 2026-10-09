// Copyright 2023-2024 the openage authors. See copying.md for legal info.

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "engine/hud_info.h"
#include "gamestate/map_settings.h"
#include "gamestate/save_format.h"
#include "gamestate/simulation.h"
#include "renderer/window.h"
#include "util/path.h"


// TODO: Remove custom jthread definition when clang/libc++ finally supports it
#if __llvm__
	#if !__cpp_lib_jthread
namespace std {
class jthread : public thread {
public:
	using thread::thread; // needed constructors
	jthread(const jthread &) = delete;
	jthread &operator=(const jthread &) = delete;
	jthread(jthread &&) = default;
	jthread &operator=(jthread &&) = default;
	~jthread() {
		if (this->joinable()) {
			this->join();
		}
	}
};
} // namespace std
	#endif
#else
	#include <stop_token>
#endif


namespace openage {

namespace cvar {
class CVarManager;
} // namespace cvar

namespace gamestate {
class GameSimulation;
namespace prod {
class Production;
} // namespace prod
} // namespace gamestate

namespace presenter {
class GameControllerSlot;
class Presenter;
} // namespace presenter

namespace time {
class Clock;
class TimeLoop;
} // namespace time


namespace engine {

/**
 * Encapsulates all subcomponents needed for a run.
 */
class Engine {
public:
	enum class mode {
		LEGACY,
		HEADLESS,
		FULL,
	};

	/**
	 * Create the engine instance for this run.
	 *
	 * @param mode The run mode to use.
	 * @param root_dir openage root directory.
	 * @param mods The mods to load.
	 * @param window_settings The settings to customize the display window (e.g. size, display mode, vsync).
	 * @param map_settings Map of the game (XR fork; default: fixed test map).
	 */
	Engine(mode mode,
	       const util::Path &root_dir,
	       const std::vector<std::string> &mods,
	       const renderer::window_settings &window_settings = {},
	       const gamestate::MapSettings &map_settings = {});

	// engine should not be copied or moved
	Engine(const Engine &) = delete;
	Engine &operator=(const Engine &) = delete;
	Engine(Engine &&) = delete;
	Engine &operator=(Engine &&) = delete;
	/**
	 * Wait for the time loop and presenter threads (XR fork). They use the
	 * members of the engine until they finish, so they have to end before
	 * the members are destroyed. Call stop() first to make them finish.
	 */
	~Engine();


	/**
	 * Run the main loop.
	 */
	void loop();

	/**
	 * Ask the engine to shut down: stops the game simulation
	 * (which ends loop()), the time loop thread and the presenter's draw loop.
	 * Safe to call from any thread.
	 */
	void stop();

	/**
	 * Clock of the time loop (XR fork), e.g. for an embedder that pauses the
	 * game while its own menu is open or the headset is taken off.
	 * Safe to call from any thread.
	 *
	 * @return The clock, nullptr after the time loop has finished.
	 */
	std::shared_ptr<time::Clock> get_clock() const;

	/**
	 * Snapshot of resources, own units, selection and match state of a player
	 * for a HUD (XR fork).
	 *
	 * Safe to call from any thread. Never waits for the simulation: while the
	 * game is still being created (modpacks, map), the result has game = false.
	 * Only short internal locks are taken (resource stock, combat snapshot,
	 * selection), never the event loop.
	 *
	 * @param player Player ID (0 = first player, the one the presenter controls).
	 *
	 * @return HUD values (all empty without a game).
	 */
	HudInfo query_hud(uint64_t player) const;

	/**
	 * Production interface of the game (XR fork): what the selection can train or
	 * build, training queues, placement mode, population, status messages.
	 * Thread-safe, without Qt (gamestate/production.h).
	 *
	 * @return Production interface (valid for the lifetime of the engine).
	 */
	std::shared_ptr<gamestate::prod::Production> get_production() const;

	/**
	 * Stop the engine and remember map settings for a new game (XR fork, game
	 * menu "Neue Karte"): the embedder (openage-native, the Quest app) reads
	 * them with take_restart() after loop() returned and starts a new engine.
	 * Safe to call from any thread.
	 */
	void request_restart(const gamestate::MapSettings &settings);

	/**
	 * Map settings of a requested restart (once), nothing if the engine was
	 * stopped for another reason.
	 */
	std::optional<gamestate::MapSettings> take_restart();

	// ---- save games (XR fork, gamestate/save_game.h) ----

	/**
	 * Save the game into a slot of the save directory (window_settings::save_dir).
	 * Runs in the simulation thread; a HUD status message reports the result.
	 * Safe to call from any thread.
	 *
	 * @param slot 1..SLOT_COUNT or AUTOSAVE_SLOT.
	 *
	 * @return false if there is no save directory or no game.
	 */
	bool save_slot(int slot);

	/**
	 * Save the game into a file (checks, scripts). Safe to call from any thread.
	 *
	 * @param file Save file.
	 * @param title Title for the slot list.
	 * @param done Called in the simulation thread with the result (may be empty).
	 *
	 * @return false if there is no game.
	 */
	bool save_file(const std::string &file,
	               const std::string &title,
	               std::function<void(bool ok, const std::string &message)> done = {});

	/**
	 * Load a slot: stop the engine and request a restart with the map settings
	 * of the file; the new engine restores the state (MapSettings::load_file).
	 *
	 * @return Empty if the restart was requested, else the error message.
	 */
	std::string load_slot(int slot);

	/**
	 * Same for a file.
	 */
	std::string load_file(const std::string &file);

	/**
	 * Slots of the save directory (empty without one).
	 */
	std::vector<gamestate::save::SlotInfo> list_save_slots() const;

	/**
	 * Camera to an entity and select it (XR fork, orders bar). Safe to call
	 * from any thread; nothing without a presenter.
	 */
	void focus_entity(uint64_t id);

	/**
	 * Map settings of this run.
	 */
	const gamestate::MapSettings &get_map_settings() const;

	/**
	 * Work for the simulation thread (checks, scripts): runs after the next
	 * simulation step. Safe to call from any thread.
	 *
	 * @return false without a simulation.
	 */
	bool post(gamestate::GameSimulation::Job job);

	/**
	 * current simulation state variable.
	 * to be set to false to stop the simulation loop.
	 */
	bool running;

private:
	/**
	 * The run mode to use.
	 */
	mode run_mode;

	/**
	 * openage root directory.
	 */
	util::Path root_dir;

	/**
	 * The threads used by the engine.
	 */
	std::vector<std::jthread> threads;

	/**
	 * Environment variables.
	 */
	std::shared_ptr<cvar::CVarManager> cvar_manager;

	/**
	 * Controls and update the clock for time-based measurements.
	 */
	std::shared_ptr<time::TimeLoop> time_loop;

	/**
	 * Gameplay simulation.
	 */
	std::shared_ptr<gamestate::GameSimulation> simulation;

	/**
	 * Video/audio/input management. Can be nullptr in headless mode.
	 */
	std::shared_ptr<presenter::Presenter> presenter;

	// never reassigned after construction, so stop() can read them from any
	// thread while the owning threads reset the shared_ptrs above
	std::weak_ptr<gamestate::GameSimulation> stop_simulation;
	std::weak_ptr<time::TimeLoop> stop_time_loop;
	std::shared_ptr<std::atomic<bool>> stop_presenter;
	// selection of the presenter for query_hud() (XR fork), nullptr without presenter
	std::shared_ptr<presenter::GameControllerSlot> controller_slot;

	// XR fork (production): kept for the HUD after the simulation is gone
	std::shared_ptr<gamestate::prod::Production> production;

	// XR fork: restart requested by the game user interface
	std::mutex restart_mutex;
	std::optional<gamestate::MapSettings> restart_request;

	// XR fork (save games): map of this run and the slot directory
	gamestate::MapSettings map_settings;
	std::string save_dir;
	std::weak_ptr<presenter::Presenter> focus_presenter;
};

} // namespace engine
} // namespace openage
