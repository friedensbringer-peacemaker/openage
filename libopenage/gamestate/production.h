// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nyan/nyan.h>

#include "coord/phys.h"
#include "gamestate/resources.h"
#include "gamestate/types.h"
#include "time/time.h"


namespace openage {
namespace event {
class EventLoop;
}

namespace gamestate {
class EntityFactory;
class GameEntity;
class GameState;

/**
 * Production of the XR fork: training units in buildings (ProductionQueue),
 * placing foundations and constructing them with villagers (Constructable,
 * Builder), the population limit, and a thread-safe interface for the HUD.
 *
 * Kept apart from the entity factory, the command handling and the economy so
 * that other gameplay branches (combat) can be merged with few conflicts.
 */
namespace prod {

/**
 * Create the production components of a game entity from its nyan abilities:
 * ProductionQueue (Create ability with units), Builder (Create ability with
 * buildings, build times from the Construct ability), Constructable (complete).
 */
void init_components(const std::shared_ptr<openage::event::EventLoop> &loop,
                     const std::shared_ptr<nyan::View> &owner_db_view,
                     const std::shared_ptr<GameEntity> &entity,
                     const nyan::fqon_t &nyan_entity);

/**
 * Find a foundation of a player under the cursor (picking ray through the
 * clicked ground point, like econ::pick_resource()).
 */
std::optional<entity_id_t> pick_foundation(const std::shared_ptr<GameState> &state,
                                           const coord::phys3 &ground_hit,
                                           player_id_t player,
                                           const time::time_t &time);

/**
 * Check if a game entity can work on a foundation.
 */
bool can_build(const std::shared_ptr<GameEntity> &builder,
               const std::shared_ptr<GameEntity> &foundation,
               const time::time_t &time);

/**
 * A building is complete: its drop site works, it shows its idle animation.
 * Called by the build system (system/build.h).
 */
void complete_building(const std::shared_ptr<GameEntity> &building,
                       const time::time_t &time);

/**
 * Health of a building under construction (AoE II): a new foundation starts at 1,
 * every building step adds its share of the maximum (damage taken meanwhile stays).
 * The combat system reads the Live attribute; its HUD snapshot is refreshed.
 *
 * @param previous_progress Progress before the step, negative for a new foundation.
 */
void update_building_health(const std::shared_ptr<GameState> &state,
                            const std::shared_ptr<GameEntity> &building,
                            double previous_progress,
                            const time::time_t &time);

/**
 * Short name of the nyan game entity of an entity ("House"), from its abilities.
 */
std::string entity_name(const std::shared_ptr<GameEntity> &entity);

// ---- ai (XR fork): queries for the computer opponent (simulation thread) ----

/// population (units + units in training) and limit of a player
struct PlayerPopulation {
	size_t used = 0;
	size_t cap = 0;
};

PlayerPopulation population_of(const std::shared_ptr<GameState> &state,
                               player_id_t player,
                               const time::time_t &time);

/// units queued in a building (0 without a training queue)
size_t queued_in(const std::shared_ptr<GameEntity> &building);

/// the building is finished (no foundation)
bool is_finished(const std::shared_ptr<GameEntity> &building);

// ---- end ai (XR fork) ----


/// kind of a HUD status message
enum class status_t {
	info,
	warn,
	good,
};

/// something the selection can train or build (HUD button)
struct Option {
	/// HUD command code (production_math.h, KNOWN): pass to Production::command()
	int code = 0;
	/// short name of the game entity ("Villager", "House")
	std::string id;
	/// German label ("Dorfbewohner", "Haus")
	std::string label;
	/// icon hint ("villager", "house", "sword", "bow", "horse", "hammer")
	std::string icon;
	/// true: building (placement mode), false: unit (training)
	bool building = false;
	resource_amounts_t cost{};
	/// training or build time (seconds)
	double time = 0.0;
	/// false: greyed out, reason says why
	bool available = true;
	std::string reason;
};

/// one entry of a training queue
struct QueueEntry {
	std::string id;
	std::string label;
};

/// training queue of a building
struct QueueState {
	entity_id_t building = 0;
	std::string label;
	std::vector<QueueEntry> items;
	/// progress of the first item 0..1
	double progress = 0.0;
	/// the first item waits for a free population slot
	bool waiting_for_housing = false;
};

/// state for the HUD, refreshed by the simulation thread
struct Snapshot {
	player_id_t player = 0;
	/// simulation time (seconds)
	double time = 0.0;
	resource_amounts_t resources{};
	size_t population = 0;
	size_t population_cap = 0;

	/// selected entities of the player
	size_t selection_count = 0;
	/// short name and label of the first selected entity ("Villager", "Dorfbewohner")
	std::string selection_id;
	std::string selection_label;
	/// construction progress 0..1 of a selected foundation
	std::optional<double> construction;

	/// what the selection can train or build (sorted, at most one row of the HUD)
	std::vector<Option> options;
	/// training queue of the selected building
	std::optional<QueueState> queue;

	/// building being placed (empty: no placement mode)
	std::string placement;

	/// last status message (empty: none), its kind, sequence number (counts up) and time
	std::string status;
	status_t status_kind = status_t::info;
	uint64_t status_seq = 0;
	double status_time = 0.0;
};

/**
 * Preview of the placement mode (XR fork, ghost of the building under the
 * cursor): computed by the simulation thread with the same rules as a placed
 * foundation (Production::place_at), read by the presenter.
 */
struct PlacementPreview {
	/// placement mode active and a cursor position known
	bool active = false;
	/// short name of the building ("House")
	std::string id;
	/// idle animation of the building (sprite of the ghost), empty if none
	std::string animation;
	/// hitbox radius (tiles); footprint side = footprint_side(radius)
	double radius = 1.0;
	/// footprint centre snapped to the tile grid, terrain height at the anchor
	double anchor_ne = 0.0;
	double anchor_se = 0.0;
	double anchor_up = 0.0;
	/// the foundation may be placed there
	bool valid = false;
	/// why not (placement_message), empty if valid
	std::string reason;
};

/**
 * Thread-safe production interface (no Qt): the HUD and the input bindings ask
 * for a snapshot and send requests; the simulation thread executes the requests
 * and advances the training queues in update().
 */
class Production {
public:
	Production() = default;
	~Production() = default;

	// --- any thread ---

	/// latest state (copy)
	Snapshot snapshot() const;

	/// player controlled by the HUD (default 0)
	void set_player(player_id_t player);

	/// selected entities (input controller, HUD)
	void set_selection(const std::vector<entity_id_t> &ids);

	/// HUD button: train the unit or start placing the building of an Option::code
	void command(int code);

	/// train a unit in the selected buildings (empty: the first one they offer)
	void train(const std::string &id = {});

	/// train a unit in a building
	void train_in(entity_id_t building, const std::string &id);

	/// cancel the last queued unit of the selected building (costs are refunded)
	void cancel_training();

	/**
	 * Start placing a building with the selected villagers. Empty id: the next
	 * building they offer (cycles while placing). Fails with a status message
	 * if the selection cannot build it or the player cannot afford it.
	 *
	 * @return true if the placement mode is active afterwards.
	 */
	bool start_placement(const std::string &id = {});

	/// leave the placement mode
	void cancel_placement();

	bool placement_active() const;

	/**
	 * Place the foundation of the placement mode at a clicked ground point
	 * (plane hit like the other input commands); leaves the placement mode.
	 *
	 * @return false if no placement mode was active.
	 */
	bool place_at(const coord::phys3 &ground_hit);

	/// place a building directly (scripts, tests)
	void place(const std::string &id, const coord::phys3 &ground_hit);

	// ---- ai (XR fork): requests of a computer opponent, independent of the HUD
	// player and its selection; they never change the HUD status message

	/// train a unit in a building of \p player
	void train_for(player_id_t player, entity_id_t building, const std::string &id);

	/// place a building of \p player and let \p builders (villagers of that player) build it
	void place_for(player_id_t player,
	               const std::vector<entity_id_t> &builders,
	               const std::string &id,
	               const coord::phys3 &ground_hit);
	// ---- end ai (XR fork)

	// --- simulation thread ---

	/**
	 * Execute requests, advance the training queues, refresh the snapshot.
	 */
	void update(const std::shared_ptr<GameState> &state,
	            const std::shared_ptr<openage::event::EventLoop> &loop,
	            const std::shared_ptr<EntityFactory> &factory,
	            const time::time_t &now);

	// ---- placement preview (XR fork): ghost and footprint under the cursor

	/// ground point under the cursor in the placement mode (any thread)
	void set_placement_cursor(const coord::phys3 &ground_hit);

	/// latest preview of the placement mode (any thread, copy)
	PlacementPreview placement_preview() const;

private:
	/// recompute the preview (simulation thread, without side effects on the game)
	void update_placement_preview(const std::shared_ptr<GameState> &state, const time::time_t &now);

	std::optional<coord::phys3> preview_cursor{};
	bool preview_dirty = false;
	PlacementPreview preview{};
	// simulation thread only
	double preview_time = -1.0;
	std::unordered_map<std::string, std::string> preview_animations{};

	struct Request {
		enum class kind_t {
			TRAIN,
			TRAIN_IN,
			CANCEL,
			PLACE,
		};
		kind_t kind = kind_t::TRAIN;
		std::string id;
		entity_id_t building = 0;
		coord::phys3 pos{0, 0, 0};
		// ai (XR fork): request of a computer opponent (no HUD status) and its builders
		std::optional<player_id_t> for_player{};
		std::vector<entity_id_t> builders{};
	};

	/// set the status message (simulation thread or API, mutex held)
	void set_status_locked(const std::string &text, status_t kind);

	/// set the status message (mutex not held)
	void set_status(const std::string &text, status_t kind);

	void push(Request &&request);

	mutable std::mutex mutex;
	player_id_t player = 0;
	std::vector<entity_id_t> selection;
	std::vector<Request> requests;
	std::string placement;
	Snapshot current;
	bool dirty = true;

	// simulation thread only
	double last_tick = -1.0;
	double now_seconds = 0.0;
	std::unordered_set<entity_id_t> foundations;
	std::unordered_set<entity_id_t> waiting;
	std::string last_logged_status;
	double last_status_log = -100.0;
};

} // namespace prod
} // namespace gamestate
} // namespace openage
