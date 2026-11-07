// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <array>
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
 * Check if a game entity can work on a foundation (XR fork W5: or repair a damaged
 * finished building of its owner, docs/aoe2-spec 04 §4.5).
 */
bool can_build(const std::shared_ptr<GameEntity> &builder,
               const std::shared_ptr<GameEntity> &foundation,
               const time::time_t &time);

/**
 * XR fork W5: a damaged finished building of \p player under the cursor (repair target), like
 * pick_foundation().
 */
std::optional<entity_id_t> pick_repairable(const std::shared_ptr<GameState> &state,
                                           const coord::phys3 &ground_hit,
                                           player_id_t player,
                                           const time::time_t &time);

/// XR fork W5: a finished building of a player is damaged and alive (repair possible)
bool is_damaged(const std::shared_ptr<GameState> &state,
                const std::shared_ptr<GameEntity> &building,
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

/**
 * Sprite path of an animation as the systems use it (the converter writes shared
 * sprites as "../shared/..." relative to the unit directory; XR fork).
 */
std::string sprite_path(const std::string &path);

// ---- ai (XR fork): queries for the computer opponent (simulation thread) ----

/// population (units + units in training) and limit of a player
struct PlayerPopulation {
	size_t used = 0;
	size_t cap = 0;
};

PlayerPopulation population_of(const std::shared_ptr<GameState> &state,
                               player_id_t player,
                               const time::time_t &time);

/**
 * XR fork: training cost of the unit \p name (repair of siege and ships, spec 04 §4.5): from a
 * standing building of \p player, else from the data of the buildings villagers can build (the
 * workshop may be gone). Zero if nothing trains it.
 */
resource_amounts_t unit_cost(const std::shared_ptr<GameState> &state,
                             player_id_t player,
                             const std::string &name,
                             const time::time_t &time);

/**
 * XR fork: a foundation of the building \p id would be accepted at \p where with the
 * villager \p builder: the same placement rules as a placement order (terrain, occupied
 * tiles, explored, elevation), without its costs and unlock conditions.
 * True if the villager does not know the building (the order reports that).
 */
bool can_place(const std::shared_ptr<GameState> &state,
               entity_id_t builder,
               const std::string &id,
               const coord::phys3 &where,
               const time::time_t &time);

/// units queued in a building (0 without a training queue)
size_t queued_in(const std::shared_ptr<GameEntity> &building);

/**
 * Set the rally point of a building (XR fork, right click with a building selected):
 * on a resource (new villagers gather it), an own entity (units walk there), an
 * enemy (military attacks it) or the ground. Simulation thread.
 *
 * @param target_entity Entity under the cursor (resource, own entity or enemy), if any.
 *
 * @return false if the entity has no training queue.
 */
bool set_rally_point(const std::shared_ptr<GameState> &state,
                     const std::shared_ptr<GameEntity> &building,
                     const coord::phys3 &ground,
                     const std::shared_ptr<GameEntity> &target_entity,
                     const time::time_t &time);

/// the building is finished (no foundation)
bool is_finished(const std::shared_ptr<GameEntity> &building);

/**
 * Technologies a building of p player offers now (XR fork W1, computer opponent):
 * short names of the researchables that are available (age, conditions, not
 * researched, not queued anywhere).
 */
std::vector<std::string> available_research(const std::shared_ptr<GameState> &state,
                                            player_id_t player,
                                            const std::shared_ptr<GameEntity> &building,
                                            const time::time_t &time);

/// a technology is queued in a building of the player (XR fork W1)
bool research_queued(const std::shared_ptr<GameState> &state, player_id_t player, const time::time_t &time);

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
	/// XR fork W1: a technology (research in the selected building)
	bool research = false;
	/// short German effect for the tooltip (technologies), may be empty
	std::string effect;
};

/// one entry of a training queue
struct QueueEntry {
	std::string id;
	std::string label;
	/// icon hint ("villager", "sword", ...)
	std::string icon;
	/// XR fork W1: a technology
	bool research = false;
};

/// a running training or construction of the player (XR fork, "Bestellungen")
struct Order {
	/// building (or foundation) entity
	entity_id_t building = 0;
	/// German label of the building ("Dorfzentrum") and of what is made ("Dorfbewohner")
	std::string building_label;
	std::string label;
	std::string icon;
	/// true: foundation under construction, false: training queue
	bool construction = false;
	/// queued units (training) or 1 (construction)
	size_t count = 0;
	/// seconds left for the first item (negative: waits for a population slot)
	double remaining = -1.0;
	/// progress 0..1 of the first item
	double progress = 0.0;
	/// position of the building (tiles), for jumping there
	double ne = 0.0;
	double se = 0.0;
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
	/// rally point (tiles ne, se, up), shown as a flag while the building is selected
	std::optional<std::array<double, 3>> rally{};
	/// what the rally point is on: "ground", "resource", "entity", "enemy"
	std::string rally_kind{};
};

/// state for the HUD, refreshed by the simulation thread
struct Snapshot {
	player_id_t player = 0;
	/// XR fork W7: the other players and the stance towards them (diplomacy::stance_t as int), sorted
	std::vector<std::pair<player_id_t, int>> stances{};
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
	/// XR fork W3: resource left in the first selected entity (any owner), e.g. "Feld – Nahrung 175",
	/// "Wild – Nahrung 120 (verdirbt)", "Holz 100"; empty if it has none
	std::string selection_resource;

	/// what the selection can train or build (sorted, at most one row of the HUD)
	std::vector<Option> options;
	/// training queue of the selected building
	std::optional<QueueState> queue;

	/// all trainings and constructions of the player (sorted by remaining time)
	std::vector<Order> orders;

	/// XR fork W1: age of the player (1 Dark … 4 Imperial), its German name, the civilisation
	int age = 1;
	std::string age_label = "Dunkles Zeitalter";
	std::string civ;
	std::string civ_label;
	/// German names of the player's unit types that differ from the catalog (line upgrades),
	/// short nyan name -> label ("Militia" -> "Schwertkämpfer")
	std::vector<std::pair<std::string, std::string>> unit_labels;

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
	/// XR fork W3 (docs/aoe2-spec 13): no picture of its own (farm, fishing trap): only a white
	/// diamond outline of the footprint, red where it cannot be placed
	bool outline_only = false;

	/// XR fork W5: the anchor is not on the grid of the footprint (gates: centre of 4 tiles on a wall),
	/// the presenter draws the ghost at the anchor of the simulation
	bool fixed_anchor = false;

	/// XR fork W5 (docs/aoe2-spec 13 L31): segments of a wall drag (button held), each with its own
	/// colour; empty while not dragging
	struct Segment {
		double ne = 0.0;
		double se = 0.0;
		double up = 0.0;
		bool valid = false;
	};
	std::vector<Segment> segments{};
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

	/**
	 * XR fork W7: the player changes the stance towards \p other (diplomacy::stance_t as
	 * int). Every other player is a computer without a diplomacy of its own yet, it answers
	 * with the same stance (assumption until W9). Executed in the simulation thread.
	 */
	void request_stance(player_id_t other, int stance);

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

	/**
	 * Research a technology in the selected buildings (XR fork W1; short name, e.g.
	 * "Loom"): queued like a unit in the first building that offers it, costs are
	 * paid now and refunded when it is cancelled.
	 */
	void research(const std::string &id);

	/**
	 * XR fork W3: mill command "Feld neu einsäen" (HUD code CMD_RESEED_FARM): the costs of a
	 * farm are paid now, the reseed waits in the mill queue until a farm of the player runs
	 * empty while its farmer is there (the farmer builds it again). A selected mill shows the
	 * queued reseeds after its research items; cancel_training_at() of such an index refunds it.
	 */
	void reseed();

	/// cancel the last queued unit of the selected building (costs are refunded)
	void cancel_training();

	/// cancel the queued unit at an index of the selected building (costs are refunded)
	void cancel_training_at(size_t index);

	/// show a status message in the HUD (e.g. "Spielstand gespeichert")
	void notify(const std::string &text, status_t kind);

	/// a foundation created outside of place() (XR fork, save games): report when it is done
	void track_foundation(entity_id_t building);

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

	/**
	 * XR fork W5: the button went down in the placement mode (VR beam: trigger, desktop: left
	 * button). For walls this starts the drag (docs/aoe2-spec 13 L31): the preview shows the
	 * segments from here to the cursor, place_at() at the release places them all.
	 *
	 * @return true if a wall drag started.
	 */
	bool placement_press(const coord::phys3 &ground_hit);

	/// XR fork W5: place a wall drag of the selection directly (scripts, tests)
	void place_wall(const std::string &id, const coord::phys3 &start, const coord::phys3 &end);

	/// the building of the placement mode is a wall (drag) – any thread
	bool placement_is_wall() const;

	// ---- ai (XR fork): requests of a computer opponent, independent of the HUD
	// player and its selection; they never change the HUD status message

	/// train a unit in a building of \p player
	void train_for(player_id_t player, entity_id_t building, const std::string &id);

	/// research a technology in a building of player (XR fork W1)
	void research_for(player_id_t player, entity_id_t building, const std::string &id);

	/// queue a farm reseed for \p player (XR fork W3, needs a finished mill of the player)
	void reseed_for(player_id_t player);

	/// test option (XR fork W3, openage-native --replay-food): \p amount of every resource for \p player
	void grant_resources(player_id_t player, double amount);

	/**
	 * Test option (XR fork W3, replays): a finished game entity of \p player at a tile, e.g. "Mill",
	 * "Dock", "FishingShip", "Villager", "Deer" (nyan object hd_base…generic.<snake>.<snake>.<name>).
	 */
	void test_spawn(player_id_t player, const std::string &name, double ne, double se);

	/**
	 * Test option (XR fork W1, openage-native --replay-tech): p player gets the technology at once,
	 * without costs and conditions, as if it had been researched (patches, refresh, message).
	 */
	void grant_research(player_id_t player, const std::string &id);

	/// place a building of \p player and let \p builders (villagers of that player) build it
	void place_for(player_id_t player,
	               const std::vector<entity_id_t> &builders,
	               const std::string &id,
	               const coord::phys3 &ground_hit);

	/// XR fork W5: a wall drag of \p player from \p start to \p end, built by \p builders
	void place_wall_for(player_id_t player,
	                    const std::vector<entity_id_t> &builders,
	                    const std::string &id,
	                    const coord::phys3 &start,
	                    const coord::phys3 &end);
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
	/// XR fork W5: ground point where the wall drag started (button held)
	std::optional<coord::phys3> wall_start{};
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
			CANCEL_AT,
			PLACE,
			RESEARCH,
			GRANT_RESEARCH,
			/// XR fork W3: queue a farm reseed at the mill
			RESEED,
			/// XR fork W3 test options (replays): resources, a finished entity at a tile
			TEST_RESOURCES,
			TEST_SPAWN,
			/// XR fork W5: wall drag from start to pos
			PLACE_WALL,
		};
		kind_t kind = kind_t::TRAIN;
		std::string id;
		entity_id_t building = 0;
		size_t index = 0;
		coord::phys3 pos{0, 0, 0};
		/// XR fork W5: start of a wall drag
		coord::phys3 start{0, 0, 0};
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
	std::vector<std::pair<player_id_t, int>> stance_requests;
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
