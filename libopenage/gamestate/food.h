// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nyan/nyan.h>

#include "coord/phys.h"
#include "event/evententity.h"
#include "event/eventhandler.h"
#include "gamestate/food_math.h"
#include "gamestate/resources.h"
#include "gamestate/terrain_tile.h"
#include "gamestate/types.h"
#include "time/time.h"


namespace openage {

namespace event {
class EventLoop;
} // namespace event

namespace gamestate {
class GameEntity;
class GameState;

namespace food {

/**
 * Food economy of the XR fork (W3, docs/aoe2-spec 02 §2.4–2.6, 13 L13/L14): what the
 * gather and build systems need beyond gathering trees and mines.
 *
 *  - herdables (sheep, turkey) belong to the player whose unit sees them and change
 *    owner when only a foreign unit is near (food_math.h herd_owner)
 *  - dead animals stay as carcasses (the combat state keeps them, on_death); their food
 *    decays with the rate of the animal also while it is gathered; empty carcasses are removed
 *  - a hit prey (deer) flees; a boar fights back (combat state)
 *  - farms and fishing traps ("fields"): one worker each, food of the data when finished,
 *    terrain of the data on their tiles (construction stages, crops, harvested), they decay
 *    when empty; a queued reseed of the mill ("Feld neu einsäen") rebuilds the farm at once
 *    for the farmer; a decayed field can be built again (right click, costs of the field)
 *
 * Runs in the simulation thread (periodic event "game.food_tick" every TICK seconds of
 * simulation time, plus the calls of the systems). The reseed queues are read by the
 * production (also simulation thread).
 */
class FoodState : public openage::event::EventEntity
	, public std::enable_shared_from_this<FoodState> {
public:
	/// period of the food tick (simulation seconds)
	static constexpr double TICK = 0.5;

	explicit FoodState(const std::shared_ptr<openage::event::EventLoop> &loop);
	~FoodState() = default;

	size_t id() const override;
	std::string idstr() const override;

	/**
	 * Start (once per game, after the map exists): hooks into the combat state, first tick.
	 *
	 * @param neutral Gaia player(s) (owner of the herdables at the start).
	 */
	void start(const std::shared_ptr<GameState> &state,
	           const std::unordered_set<player_id_t> &neutral,
	           const time::time_t &time);

	bool is_started() const;

	/// the player is gaia (owner of wild animals and of herdables nobody has seen yet)
	bool is_gaia(player_id_t player) const {
		return this->neutral.contains(player);
	}

	/// periodic tick (event handler): herdables, carcass decay, decayed fields
	void tick(const std::shared_ptr<GameState> &state, const time::time_t &time);

	// ---- hooks of the combat state

	/// an entity died: animals with food stay as carcasses (true)
	bool on_death(const std::shared_ptr<GameState> &state,
	              const std::shared_ptr<GameEntity> &entity,
	              const time::time_t &time);

	/// an entity was hit: a prey runs away from its attacker
	void on_hit(const std::shared_ptr<GameState> &state,
	            const std::shared_ptr<GameEntity> &attacker,
	            const std::shared_ptr<GameEntity> &target,
	            const time::time_t &time);

	// ---- carcasses

	/// a carcass was gathered empty: remove it from the game
	void carcass_empty(const std::shared_ptr<GameState> &state,
	                   const std::shared_ptr<GameEntity> &entity,
	                   const time::time_t &time);

	// ---- fields (farms, fishing traps)

	/**
	 * A field foundation was placed (production) or loaded: its tiles are known, the ground
	 * shows the first construction stage. Fields do not block their tiles (farmers walk on them).
	 */
	void field_placed(const std::shared_ptr<GameState> &state,
	                  const std::shared_ptr<GameEntity> &field,
	                  const time::time_t &time);

	/// construction progress of a field changed (build system): the terrain follows the stages
	void field_progress(const std::shared_ptr<GameState> &state,
	                    const std::shared_ptr<GameEntity> &field,
	                    const time::time_t &time);

	/**
	 * A field is finished (build system): its food is the starting amount of the data in the
	 * owner's view now (technologies of the mill count for new and reseeded farms), crops terrain.
	 */
	void field_complete(const std::shared_ptr<GameState> &state,
	                    const std::shared_ptr<GameEntity> &field,
	                    const time::time_t &time);

	/**
	 * A worker gathered a field empty: reseed from the mill queue (the worker builds it
	 * again) or let it decay.
	 *
	 * @return RESEED if the worker got a build command for the field, DECAY otherwise.
	 */
	empty_t field_empty(const std::shared_ptr<GameState> &state,
	                    const std::shared_ptr<GameEntity> &field,
	                    const std::shared_ptr<GameEntity> &worker,
	                    const time::time_t &time);

	/**
	 * Build a decayed field again (right click of a villager on it): pays the costs of the
	 * field (villager's build list), the field becomes a foundation, the builder builds it.
	 *
	 * @return Empty string on success, else the reason (German, HUD message).
	 */
	std::string rebuild(const std::shared_ptr<GameState> &state,
	                    const std::shared_ptr<GameEntity> &field,
	                    const std::shared_ptr<GameEntity> &builder,
	                    const time::time_t &time);

	/// the field is decayed (empty, waits for a rebuild until it is removed)
	bool is_decayed(entity_id_t field) const;

	/// a field covers the tile (placement of other buildings)
	bool field_tile(long ne, long se) const;

	/// a decayed own field under the cursor (plane hit like econ::pick_resource), if any
	std::optional<entity_id_t> pick_decayed(const std::shared_ptr<GameState> &state,
	                                        const coord::phys3 &ground_hit,
	                                        player_id_t player,
	                                        const time::time_t &time) const;

	/// the worker holds the field (one each); false if another worker holds it
	bool claim(const std::shared_ptr<GameEntity> &field, entity_id_t worker);
	/// the worker lets the field go (it gathers elsewhere, died)
	void release(const std::shared_ptr<GameEntity> &field, entity_id_t worker);

	// ---- reseed queue of the mill (spec 13 L14)

	/// queue a reseed: costs of the farm are paid now. Empty on success, else the reason.
	std::string queue_reseed(const std::shared_ptr<GameState> &state, player_id_t player,
	                         const resource_amounts_t &cost);
	/// cancel the reseed at index with a full refund; false if there is none
	bool cancel_reseed(const std::shared_ptr<GameState> &state, player_id_t player, size_t index);
	/// number of queued reseeds
	size_t reseeds(player_id_t player) const;
	/// queued reseeds with their costs (save games)
	std::vector<resource_amounts_t> reseed_costs(player_id_t player) const;
	/// restore the queue of a player (save games)
	void set_reseeds(player_id_t player, const std::vector<resource_amounts_t> &costs);

	// ---- statistics for logs and checks

	struct Counters {
		size_t conversions = 0;
		size_t carcasses = 0;
		size_t carcasses_removed = 0;
		double decayed_food = 0.0;
		size_t flights = 0;
		size_t fields_completed = 0;
		size_t fields_emptied = 0;
		size_t reseeds_used = 0;
		size_t fields_decayed = 0;
		size_t fields_rebuilt = 0;
		size_t fields_removed = 0;
	};
	Counters counters() const;

private:
	struct Field {
		entity_id_t id = 0;
		player_id_t owner = 0;
		source_t kind = source_t::FARM;
		field_t state = field_t::BUILDING;
		double decayed_at = 0.0;
		/// tiles of the footprint
		std::vector<std::pair<long, long>> tiles;
		/// terrain before the field (restored when it is removed)
		std::vector<TerrainTile> original;
		/// shown terrain stage (construction index, or crops/harvested)
		int shown = -100;
	};

	/// terrain objects of a field type (from its nyan abilities)
	struct FieldTerrain {
		std::vector<nyan::fqon_t> construction;
		nyan::fqon_t crops;
		nyan::fqon_t harvested;
	};

	void herd(const std::shared_ptr<GameState> &state, const time::time_t &time);
	void decay_carcasses(const std::shared_ptr<GameState> &state, double dt, const time::time_t &time);
	void fields_tick(const std::shared_ptr<GameState> &state, const time::time_t &time);
	/// register fields that exist without field_placed (map, save game)
	void adopt_fields(const std::shared_ptr<GameState> &state, const time::time_t &time);
	Field *field_of(entity_id_t id);
	const FieldTerrain &terrain_of(const std::shared_ptr<GameState> &state, const nyan::fqon_t &fqon);
	void show_terrain(const std::shared_ptr<GameState> &state, Field &field, int stage, const time::time_t &time);
	void restore_terrain(const std::shared_ptr<GameState> &state, Field &field, const time::time_t &time);
	void remove_field(const std::shared_ptr<GameState> &state, entity_id_t id, const time::time_t &time);
	/// build time and costs of a field type in the builder's build list (nothing if it cannot build it)
	std::optional<std::pair<double, resource_amounts_t>> field_build_data(const std::shared_ptr<GameEntity> &builder,
	                                                                     const std::string &name) const;
	void start_foundation(const std::shared_ptr<GameState> &state, const std::shared_ptr<GameEntity> &field,
	                      double build_time, const time::time_t &time);

	std::shared_ptr<openage::event::EventLoop> loop;
	std::shared_ptr<openage::event::EventHandler> handler;
	std::unordered_set<player_id_t> neutral;
	bool started = false;
	double last_tick = -1.0;

	std::map<entity_id_t, Field> fields;
	std::unordered_map<nyan::fqon_t, FieldTerrain> field_terrain;
	/// carcasses: id -> time of death, despawn sprite shown
	struct Carcass {
		double died_at = 0.0;
		double death_time = 0.0;
		std::string despawn_sprite;
		bool switched = false;
	};
	std::map<entity_id_t, Carcass> carcasses;
	std::map<player_id_t, ReseedQueue> queues;
	Counters stats{};
	/// log throttle (simulation seconds)
	double last_log = -100.0;
};

/**
 * Event handler of the food tick ("game.food_tick"), created by the food state itself.
 */
class FoodEventHandler : public openage::event::OnceEventHandler {
public:
	FoodEventHandler();
	~FoodEventHandler() = default;

	void setup_event(const std::shared_ptr<openage::event::Event> &event,
	                 const std::shared_ptr<openage::event::State> &state) override;

	void invoke(openage::event::EventLoop &loop,
	            const std::shared_ptr<openage::event::EventEntity> &target,
	            const std::shared_ptr<openage::event::State> &state,
	            const time::time_t &time,
	            const param_map &params) override;

	time::time_t predict_invoke_time(const std::shared_ptr<openage::event::EventEntity> &target,
	                                 const std::shared_ptr<openage::event::State> &state,
	                                 const time::time_t &at) override;
};

} // namespace food
} // namespace gamestate
} // namespace openage
