// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nyan/nyan.h>

#include "coord/phys.h"
#include "event/evententity.h"
#include "gamestate/combat/rules.h"
#include "gamestate/combat/stats.h"
#include "gamestate/pick.h"
#include "gamestate/types.h"
#include "time/time.h"


namespace openage {

namespace event {
class Event;
class EventLoop;
} // namespace event

namespace gamestate {
class GameEntity;
class GameState;

namespace combat {

/**
 * Health of an entity for the HUD (thread-safe snapshot).
 */
struct HealthInfo {
	entity_id_t id = 0;
	int64_t health = 0;
	int64_t max_health = 0;
	/// false once the entity died (death animation) or was removed
	bool alive = false;
};

/**
 * Match status for the HUD / a VR loading screen (thread-safe snapshot).
 */
struct MatchStatus {
	MatchResult result{};
	/// simulation time when the match was decided
	time::time_t decided_at = time::TIME_ZERO;
	/// units + buildings left per participant
	std::map<player_id_t, size_t> alive{};
};

/**
 * Combat of the XR fork: attacks, damage, death, auto attack and the victory
 * condition. Lives in the game state, all non-const methods run in the
 * simulation thread (event handlers); the query methods marked thread-safe
 * may be called from any thread (input, HUD).
 *
 * Simplifications (not time-travel safe, unlike the curves of the engine):
 * the per-entity combat data is plain state, ranged attacks hit instantly
 * (no projectile entities), no attack bonuses from elevation, no stances.
 *
 * Flow: order_attack() / the periodic scan set a target and schedule an
 * attack tick ("game.combat_tick") for the attacker. A tick walks into range
 * (move commands through the command queue, so the regular move system and
 * pathfinding are used), turns to the target and applies one hit per reload
 * time. Health 0 -> death animation, then "game.combat_remove" removes the
 * entity from the game state and the renderer and frees its tiles.
 */
class CombatState : public openage::event::EventEntity {
public:
	explicit CombatState(const std::shared_ptr<openage::event::EventLoop> &loop);
	~CombatState() = default;

	size_t id() const override;
	std::string idstr() const override;

	/**
	 * Read the combat values of a new entity (EntityFactory, before the owner
	 * and position are set). Buildings placed by the map start with full health.
	 */
	void register_entity(const std::shared_ptr<GameEntity> &entity,
	                     const std::shared_ptr<nyan::View> &db_view,
	                     const nyan::fqon_t &nyan_entity);

	/**
	 * Start the periodic scan for auto attacks (once per game, after the
	 * event handlers exist).
	 */
	void start(const std::shared_ptr<GameState> &state, const time::time_t &time);

	/**
	 * Players that never count for the victory condition (Gaia).
	 */
	void set_neutral_players(const std::unordered_set<player_id_t> &players);

	/**
	 * Order an attack (explicit command): walk into range, attack until the
	 * target is dead, then attack enemies in sight.
	 *
	 * @return false if the attacker cannot attack this target.
	 */
	bool order_attack(const std::shared_ptr<GameState> &state,
	                  entity_id_t attacker,
	                  entity_id_t target,
	                  const time::time_t &time);

	/**
	 * Stop attacking (a new move/idle command of the player).
	 */
	void cancel_attack(entity_id_t attacker, const time::time_t &time);

	/**
	 * Enemy of \p player under the cursor (right click), or nullptr.
	 */
	std::shared_ptr<GameEntity> pick_enemy(const std::shared_ptr<GameState> &state,
	                                       const time::time_t &time,
	                                       player_id_t player,
	                                       const PickRequest &request) const;

	/**
	 * Kill an entity (health 0), e.g. for tests or a delete command.
	 */
	void kill(const std::shared_ptr<GameState> &state,
	          entity_id_t id,
	          const time::time_t &time);

	// ---- event handlers
	void tick(const std::shared_ptr<GameState> &state, entity_id_t attacker, const time::time_t &time);
	void scan(const std::shared_ptr<GameState> &state, const time::time_t &time);
	void remove(const std::shared_ptr<GameState> &state, entity_id_t id, const time::time_t &time);

	// ---- queries, simulation thread
	bool is_dead(entity_id_t id) const;
	std::shared_ptr<const CombatStats> get_stats(entity_id_t id) const;
	std::optional<entity_id_t> get_target(entity_id_t id) const;

	// ---- queries, thread-safe
	/// the entity was removed from the game (died); selections drop it
	bool was_removed(entity_id_t id) const;
	/// health of the given entities (unknown ids: alive = false)
	std::vector<HealthInfo> get_health(const std::vector<entity_id_t> &ids) const;
	/// current match status
	MatchStatus get_match_status() const;
	/// state of the match for \p player (VICTORY/DEFEAT/DRAW once decided)
	match_state_t get_match_state(player_id_t player) const;
	/**
	 * Called once in the simulation thread when the match is decided.
	 */
	void on_match_over(std::function<void(const MatchStatus &)> callback);

private:
	struct Combatant {
		std::shared_ptr<GameEntity> entity;
		std::shared_ptr<const CombatStats> stats;
		bool dead = false;
		std::optional<entity_id_t> target{};
		/// ordered by the player (keeps chasing, no leash)
		bool explicit_order = false;
		/// pending attack tick
		std::shared_ptr<openage::event::Event> tick{};
		time::time_t ready_at = time::TIME_MIN;
		/// last move order towards the target
		std::optional<coord::phys3> chase_destination{};
		time::time_t chase_ordered = time::TIME_MIN;
		int chase_failures = 0;
		double last_edge = 0.0;
		/// first hit was logged
		bool hit_logged = false;
	};

	Combatant *find(entity_id_t id);
	const Combatant *find(entity_id_t id) const;
	player_id_t owner_of(const std::shared_ptr<GameEntity> &entity, const time::time_t &time) const;
	bool is_enemy(const Combatant &a, const Combatant &b, const time::time_t &time) const;
	coord::phys3 position_of(const std::shared_ptr<GameEntity> &entity, const time::time_t &time) const;
	bool is_moving(const std::shared_ptr<GameEntity> &entity, const time::time_t &time) const;
	double edge_between(const Combatant &a, const Combatant &b, const time::time_t &time) const;
	std::optional<entity_id_t> nearest_enemy(const Combatant &self, double range, const time::time_t &time) const;

	void engage(const std::shared_ptr<GameState> &state, Combatant &attacker, entity_id_t target,
	            bool explicit_order, const time::time_t &time);
	void schedule_tick(const std::shared_ptr<GameState> &state, Combatant &attacker, const time::time_t &time);
	void drop_target(Combatant &attacker, const time::time_t &time);
	void halt(const std::shared_ptr<GameEntity> &entity, const time::time_t &time);
	void hit(const std::shared_ptr<GameState> &state, Combatant &attacker, Combatant &target, const time::time_t &time);
	void die(const std::shared_ptr<GameState> &state, Combatant &victim, const time::time_t &time,
	         const std::string &cause);
	void update_match(const time::time_t &time);
	void set_health_snapshot(entity_id_t id, int64_t health, int64_t max_health, bool alive);
	bool allow_hit_log(const time::time_t &time);

	std::shared_ptr<openage::event::EventLoop> loop;

	/// combat data per entity (entities with a Live health attribute)
	std::unordered_map<entity_id_t, Combatant> combatants;
	/// stats per nyan entity (and owner view)
	std::unordered_map<std::string, std::shared_ptr<const CombatStats>> stats_cache;

	std::unordered_set<player_id_t> neutral;
	/// players that ever had units or buildings
	std::set<player_id_t> participants;
	bool started = false;
	bool match_over = false;

	/// hit logs per simulation second (throttled)
	int64_t log_second = -1;
	size_t logs_in_second = 0;
	size_t logs_suppressed = 0;

	// ---- shared with other threads
	mutable std::mutex shared_mutex;
	std::unordered_set<entity_id_t> removed;
	std::unordered_map<entity_id_t, HealthInfo> health_snapshot;
	MatchStatus match_status{};
	std::vector<std::function<void(const MatchStatus &)>> match_callbacks;
};

} // namespace combat
} // namespace gamestate
} // namespace openage
