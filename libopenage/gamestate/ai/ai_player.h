// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "event/evententity.h"
#include "gamestate/ai/ai_rules.h"
#include "gamestate/resources.h"
#include "gamestate/types.h"
#include "time/time.h"


namespace openage {

namespace event {
class EventHandler;
class EventLoop;
} // namespace event

namespace gamestate {
class GameState;

namespace ai {
class ProductionPort;

/**
 * Status of a computer opponent (thread-safe snapshot, for checks and HUDs).
 */
struct AiStatus {
	player_id_t player = 0;
	difficulty_t difficulty = difficulty_t::EASY;
	/// false once the match is over or the AI was defeated
	bool running = false;
	/// "economy", "attack", "defend", "over"
	std::string phase = "economy";
	size_t thinks = 0;
	/// gather orders for idle villagers
	size_t gather_orders = 0;
	/// attack orders for single units (waves and defence)
	size_t attack_orders = 0;
	size_t waves = 0;
	/// start of the first wave (simulation seconds)
	std::optional<double> first_attack{};
	/// defence starts (threat seen after a quiet period)
	size_t defenses = 0;
	/// production wishes passed to the production port
	size_t production_requests = 0;
	/// of those, accepted (0 with the stub)
	size_t production_done = 0;
	size_t villagers = 0;
	size_t military = 0;
	resource_amounts_t resources{};
};

/**
 * Simple computer opponent for one player (XR fork).
 *
 * Runs in the simulation thread as a periodic event ("game.ai_think", every
 * AiParams::think_period seconds of simulation time); all decisions are
 * deterministic for the same map and seed. Per decision:
 *
 *  1. observe: own villagers, army, buildings; enemies; health changes
 *  2. defend: enemy units near own buildings/villagers or hitting own
 *     entities -> army units at home attack the nearest threat
 *  3. attack: army at home >= threshold (or after a timeout), not before
 *     AiParams::first_attack_earliest -> wave against the nearest enemy town
 *     center (else nearest enemy); wave units attack enemy units in sight
 *     first ("attack move"), then the objective
 *  4. economy: idle villagers gather (split food/wood/gold), empty resources
 *     -> next resource of the wished kind (the gather system itself continues
 *     with nearby resources of the same kind)
 *  5. production: villagers up to 15, houses at the population limit,
 *     barracks, militia through the ProductionPort (a stub until the
 *     production branch is merged: decisions are only logged)
 *  6. report: status line every AiParams::report_period seconds
 *
 * Commands go directly to the components (gather command queue, combat
 * order_attack), like the send command handler does for the human player.
 */
class AiPlayer : public openage::event::EventEntity
	, public std::enable_shared_from_this<AiPlayer> {
public:
	/**
	 * @param loop Event loop of the game.
	 * @param player Controlled player.
	 * @param neutral Neutral players (gaia): never enemies, owner of the resources.
	 * @param params Parameters (params_for() + overrides).
	 * @param seed Seed of the decisions.
	 * @param production Training/building (stub until xr-prod is merged).
	 */
	AiPlayer(const std::shared_ptr<openage::event::EventLoop> &loop,
	         player_id_t player,
	         const std::unordered_set<player_id_t> &neutral,
	         const AiParams &params,
	         uint64_t seed,
	         const std::shared_ptr<ProductionPort> &production);
	~AiPlayer() = default;

	size_t id() const override;
	std::string idstr() const override;

	/**
	 * Schedule the first decision (after one think period).
	 */
	void start(const std::shared_ptr<GameState> &state, const time::time_t &time);

	/**
	 * One decision round (event handler), schedules the next one.
	 */
	void think(const std::shared_ptr<GameState> &state, const time::time_t &time);

	/// controlled player
	player_id_t get_player() const;

	/// parameters in use
	const AiParams &get_params() const;

	/// thread-safe status snapshot
	AiStatus get_status() const;

private:
	/// own or enemy entity seen in one decision round
	struct Seen {
		entity_id_t id = 0;
		player_id_t owner = 0;
		double ne = 0.0;
		double se = 0.0;
		double radius = 0.0;
		double sight = 0.0;
		std::string name;
		bool building = false;
		bool villager = false;
		bool military = false;
		bool town_center = false;
	};

	struct World {
		std::vector<Seen> villagers;
		std::vector<Seen> army;
		std::vector<Seen> buildings;
		std::vector<Seen> enemies;
		/// own entities that lost health since the last round
		std::vector<Seen> hurt;
		/// home: own town center, else centre of the own entities
		double home_ne = 0.0;
		double home_se = 0.0;
		bool has_home = false;
	};

	World observe(const std::shared_ptr<GameState> &state, const time::time_t &time);
	void defend(const std::shared_ptr<GameState> &state, const World &world, const time::time_t &time);
	void attack(const std::shared_ptr<GameState> &state, const World &world, const time::time_t &time);
	void gather(const std::shared_ptr<GameState> &state, const World &world, const time::time_t &time);
	void produce(const std::shared_ptr<GameState> &state, const World &world, const time::time_t &time);
	void report(const std::shared_ptr<GameState> &state, const World &world, const time::time_t &time);
	void schedule(const std::shared_ptr<GameState> &state, const time::time_t &time);

	/// order an attack (cooldown per unit), true if ordered
	bool order_attack(const std::shared_ptr<GameState> &state, const Seen &unit, const Seen &target,
	                  const time::time_t &time);
	/// candidates for choose_objective()/choose_unit_target() seen from a point
	std::vector<TargetCandidate> candidates(const World &world, double ne, double se) const;
	const Seen *find_enemy(const World &world, entity_id_t id) const;
	/// spot for a building next to home (passable tiles), nothing if none found
	std::optional<std::pair<double, double>> building_spot(const std::shared_ptr<GameState> &state,
	                                                       const World &world,
	                                                       double side,
	                                                       const time::time_t &time);

	std::shared_ptr<openage::event::EventLoop> loop;
	std::shared_ptr<openage::event::EventHandler> handler;
	player_id_t player;
	std::unordered_set<player_id_t> neutral;
	AiParams params;
	Rng rng;
	std::shared_ptr<ProductionPort> production;

	bool running = false;
	bool started = false;

	/// health of own entities in the last round
	std::unordered_map<entity_id_t, int64_t> last_health;
	/// last order per own unit (simulation seconds)
	std::unordered_map<entity_id_t, double> last_order;
	/// units of the running waves
	std::set<entity_id_t> wave;
	/// objective of the wave (enemy entity)
	std::optional<entity_id_t> objective;
	size_t waves = 0;
	double last_wave = 0.0;
	bool defending = false;
	double last_threat = -1e30;
	/// last logged decision per topic (log when it changes)
	std::string last_attack_wait;
	std::string last_plan;
	/// path grid of land units (passability of building spots), -1 = unknown
	long land_grid = -2;

	LogThrottle gather_log{5.0};
	LogThrottle defend_log{10.0};
	LogThrottle plan_log{30.0};
	double next_report = 0.0;

	// ---- shared with other threads
	mutable std::mutex status_mutex;
	AiStatus status;
};

} // namespace ai
} // namespace gamestate
} // namespace openage
