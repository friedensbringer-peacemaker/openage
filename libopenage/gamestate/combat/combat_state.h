// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <utility>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nyan/nyan.h>

#include "coord/phys.h"
#include "coord/tile.h"
#include "event/evententity.h"
#include "gamestate/combat/behavior.h"
#include "gamestate/combat/rules.h"
#include "gamestate/diplomacy_math.h"
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

namespace vision {
class VisionState;
} // namespace vision

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
	/// units (without buildings) left per participant, e.g. the population of a HUD;
	/// updated with the periodic scan (once per simulated second) and on deaths
	std::map<player_id_t, size_t> units{};
};

/**
 * Combat state of one entity for the HUD (XR fork W4, thread-safe snapshot):
 * stance, order, formation, garrison.
 */
struct CombatInfo {
	stance_t stance = stance_t::AGGRESSIVE;
	order_t order = order_t::NONE;
	formation_t formation = formation_t::NONE;
	/// the entity chooses stances / formations (data abilities)
	bool stances = false;
	bool formations = false;
	/// garrisoned in a building (hidden)
	bool inside = false;
	/// building: places, occupants (short nyan names), arrows of the next volley
	int capacity = 0;
	std::vector<std::string> occupants{};
	int arrows = 0;
	/// town center: the town bell rings
	bool bell = false;
};

/**
 * A projectile in flight for the renderer (XR fork W4, thread-safe snapshot).
 */
struct ProjectileView {
	Flight flight{};
	/// simulation time of the launch (s); the projectile is drawn from then on
	double launched = 0.0;
	/// extra arrow of a garrison (secondary projectile)
	bool secondary = false;
	/// owner (player colour)
	player_id_t owner = 0;
};

/**
 * Combat values of an entity in a save game (XR fork W4, gamestate/save_game.cpp).
 */
struct SavedCombat {
	stance_t stance = stance_t::AGGRESSIVE;
	formation_t formation = formation_t::NONE;
	order_t order = order_t::NONE;
	/// patrol route, attack move destination
	std::vector<Point> route{};
	/// guarded / followed entity
	std::optional<entity_id_t> order_entity{};
	/// building it is garrisoned in, entered with the town bell
	std::optional<entity_id_t> inside{};
	bool by_bell = false;
	Point bell_from{};
	/// job before the town bell
	std::optional<entity_id_t> resume_gather{};
	std::optional<entity_id_t> resume_build{};
	/// town center: the bell rings
	bool bell = false;
};

/**
 * Combat of the XR fork: attacks, damage, death, auto attack and the victory
 * condition. Lives in the game state, all non-const methods run in the
 * simulation thread (event handlers); the query methods marked thread-safe
 * may be called from any thread (input, HUD).
 *
 * Simplifications (not time-travel safe, unlike the curves of the engine):
 * the per-entity combat data is plain state.
 *
 * Flow: order_attack() / the periodic scan set a target and schedule an
 * attack tick ("game.combat_tick") for the attacker. A tick walks into range
 * (move commands through the command queue, so the regular move system and
 * pathfinding are used), turns to the target and attacks once per reload
 * time: melee hits at once (hit roll with the accuracy), ranged attackers
 * launch projectiles ("game.combat_projectile" at the impact). Health 0 ->
 * death animation, then "game.combat_remove" removes the entity from the game
 * state and the renderer and frees its tiles.
 *
 * Round W4 (docs/aoe2-spec 05, 06, 07): projectiles with flight time, arc,
 * accuracy, dispersion and lead; elevation factor; stances with post and
 * leash; patrol, guard, follow, attack move, stop; target choice by score
 * (distance, time to kill, threat memory, best three, 10 s lock); garrison
 * in buildings (type mask, capacity, arrows, healing, ungarrison, ejection
 * of damaged buildings); town bell; flight of villagers; formations.
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
	 * Stance of player \p a towards player \p b (XR fork W7, spec 06). Everyone
	 * starts as an enemy; each side has its own stance. Simulation thread only.
	 */
	diplomacy::stance_t stance(player_id_t a, player_id_t b) const;

	/**
	 * Set the stance of player \p a towards player \p b. Simulation thread only.
	 */
	void set_stance(player_id_t a, player_id_t b, diplomacy::stance_t stance);

	/**
	 * Stances that are not "enemy" (save games).
	 */
	const std::map<std::pair<player_id_t, player_id_t>, diplomacy::stance_t> &get_stances() const;

	/**
	 * Fog of war (XR fork, gamestate/vision): auto attacks only pick enemies in
	 * sight, a right click only picks enemies the player sees (or remembered
	 * buildings in the fog). Without a vision everything counts as visible.
	 */
	void set_vision(const std::shared_ptr<vision::VisionState> &vision);

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

	// ---- orders of round W4 (simulation thread)

	/**
	 * A plain command of the player (move, gather, build, idle): attacks and
	 * orders end, the post is the destination (stances measure from there).
	 */
	void player_moved(entity_id_t id, const std::optional<coord::phys3> &destination, const time::time_t &time);

	/// stop (06 §6.1): no target, no order, stand where it is
	void stop(const std::shared_ptr<GameState> &state, entity_id_t id, const time::time_t &time);

	/// stance of a unit (06 §6.2); false if it has no stances
	bool set_stance(entity_id_t id, stance_t stance);

	/// formation of a unit (07 §7.5), used by the next move of several units
	bool set_formation(entity_id_t id, formation_t formation);

	/**
	 * Move several units in their formation (07 §7.5): slots around the
	 * destination facing the way, every unit with the speed of the slowest.
	 *
	 * @return ids of the units that got a slot (the others need a plain move).
	 */
	std::vector<entity_id_t> formation_move(const std::shared_ptr<GameState> &state,
	                                        const std::vector<entity_id_t> &ids,
	                                        const coord::phys3 &destination,
	                                        const time::time_t &time);

	/// patrol between the current position and \p point (06 §6.3)
	bool order_patrol(const std::shared_ptr<GameState> &state, entity_id_t id, const coord::phys3 &point,
	                  const time::time_t &time);
	/// guard an own entity (06 §6.3)
	bool order_guard(const std::shared_ptr<GameState> &state, entity_id_t id, entity_id_t guarded,
	                 const time::time_t &time);
	/// follow an entity at a distance (06 §6.3)
	bool order_follow(const std::shared_ptr<GameState> &state, entity_id_t id, entity_id_t followed,
	                  const time::time_t &time);
	/// walk to \p point, attacking every enemy on the way (06 §6.3)
	bool order_attack_move(const std::shared_ptr<GameState> &state, entity_id_t id, const coord::phys3 &point,
	                       const time::time_t &time);

	/**
	 * Walk into an own building and stay there (05 §5.4, 06 §6.5).
	 *
	 * @param by_bell Entered with the town bell (comes out again with "back to work").
	 * @return false if the building does not take the unit (mask, places, owner).
	 */
	bool order_garrison(const std::shared_ptr<GameState> &state, entity_id_t id, entity_id_t building,
	                    const time::time_t &time, bool by_bell = false);

	/// let the whole garrison of a building out ("Alle raus")
	void ungarrison_all(const std::shared_ptr<GameState> &state, entity_id_t building, const time::time_t &time);

	/**
	 * Town bell of a town center (06 §6.4): on = villagers within +-25 tiles
	 * go into the nearest town centers, towers and castles with free places;
	 * off = they come out and go back to work.
	 *
	 * @return false if \p tc is no own town center.
	 */
	bool ring_bell(const std::shared_ptr<GameState> &state, entity_id_t tc, bool on, const time::time_t &time);

	/// the town bell of the town center rings
	bool bell_rings(entity_id_t tc) const;

	/**
	 * Building under the cursor that \p player can garrison units in, or nullptr.
	 */
	std::shared_ptr<GameEntity> pick_shelter(const std::shared_ptr<GameState> &state,
	                                         const time::time_t &time,
	                                         player_id_t player,
	                                         const PickRequest &request) const;

	/**
	 * Unit or building under the cursor of any player except gaia (guard,
	 * follow), or nullptr.
	 */
	std::shared_ptr<GameEntity> pick_unit(const std::shared_ptr<GameState> &state,
	                                      const time::time_t &time,
	                                      const PickRequest &request,
	                                      entity_id_t except = 0) const;

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

	/// time of the current event or command (simulation thread)
	void note_time(const time::time_t &time) {
		if (time > this->clock) {
			this->clock = time;
		}
	}

	// ---- XR fork W3 (hunting, gamestate/food.h): gaia animals as targets of players

	/// result of strike()
	struct StrikeResult {
		/// a hit was applied now
		bool struck = false;
		/// the target is dead (now or before)
		bool killed = false;
		/// seconds until the attacker may strike again (reload)
		double ready_in = 0.0;
	};

	/**
	 * One hit of a hunter on an animal (or a herdable of its own player) without an attack
	 * order: the gather system walks the hunter into range and calls this once per step.
	 * Respects the reload time of the attacker; a predator (boar) fights back like a
	 * military unit. No owner check (the caller decides what may be hunted).
	 */
	StrikeResult strike(const std::shared_ptr<GameState> &state,
	                    entity_id_t attacker,
	                    entity_id_t target,
	                    const time::time_t &time);

	/**
	 * Called when an entity dies; returning true keeps it in the game as a dead entity
	 * (a carcass that is gathered, gamestate/food.h): no removal after the death animation,
	 * the handler removes it later with remove().
	 */
	void on_death(std::function<bool(const std::shared_ptr<GameEntity> &, const time::time_t &)> handler);

	/// called after every hit (attacker, target), e.g. a prey flees
	void on_hit(std::function<void(const std::shared_ptr<GameEntity> &, const std::shared_ptr<GameEntity> &,
	                               const time::time_t &)> handler);
	// ---- end XR fork W3

	/**
	 * XR fork W5: footprint of buildings that are no square (gates: 4 tiles in a line); remove()
	 * frees these tiles instead of the square of the hitbox. Nothing = the square.
	 */
	void set_footprint_hook(std::function<std::optional<std::vector<coord::tile>>(entity_id_t)> hook) {
		this->footprint_hook = std::move(hook);
	}

	// ---- XR fork W6 (special abilities, gamestate/special.h): new functions and a hook only,
	// the W4 flows stay as they are

	/// impact of an attack: attacker id, its owner and stats, the main target if it was hit,
	/// the impact point (tiles); melee hits and every projectile (also a miss) report it
	using impact_handler_t = std::function<void(entity_id_t, player_id_t, const CombatStats &,
	                                            std::optional<entity_id_t>, double, double, const time::time_t &)>;

	/// called after a melee hit and after every projectile impact (area damage, W6)
	void on_impact(impact_handler_t handler);

	/// may the attacker take this target (attacker id, target id)? false refuses orders and automatic targets
	using target_filter_t = std::function<bool(entity_id_t, entity_id_t)>;

	/// add a target rule (W6: rams attack buildings only)
	void on_target(target_filter_t filter);

	/// XR fork: what a unit that packs (trebuchet) does this tick
	enum class siege_gate_t {
		/// shoot or walk as usual
		GO,
		/// packing or unpacking: try again next tick
		WAIT,
		/// give up the target
		DROP,
	};
	/// unit, target in range, explicit order, time
	using siege_gate_fn_t = std::function<siege_gate_t(entity_id_t, bool, bool, const time::time_t &)>;

	/// the rule for units that pack, asked before they shoot or walk to a target
	void on_siege_gate(siege_gate_fn_t gate);

	/// damage of an attack against a target without elevation/terrain factors (area attacks)
	int64_t damage_against(const CombatStats &attacker, entity_id_t target) const;

	/**
	 * Side target of an area attack (W6): the hit with \p factor on the damage and the same
	 * reactions as a direct hit (death, flight, fighting back).
	 *
	 * @return false if the target cannot be hit (dead, inside, no health).
	 */
	bool area_hit(const std::shared_ptr<GameState> &state,
	              const CombatStats &attacker_stats,
	              entity_id_t attacker_id,
	              player_id_t attacker_owner,
	              entity_id_t target,
	              double factor,
	              const time::time_t &time);

	/// hit roll of an area attack with an accuracy (deterministic generator of the combat state)
	bool roll_accuracy(double accuracy);

	/**
	 * Put an entity into a carrier without the garrison rules (W6: transport ship, ram, monk
	 * carrying a relic, relic in a monastery): hidden like a garrison, no sight, no target,
	 * but not an occupant of the carrier (gamestate/special.h counts and releases them).
	 */
	bool stow(const std::shared_ptr<GameState> &state, entity_id_t id, entity_id_t carrier, const time::time_t &time);

	/**
	 * Let a stowed entity out on a free spot next to its carrier (passable for the entity).
	 *
	 * @param require_spot true: nothing happens without a passable spot (ship far from the shore)
	 * @return false if it was not stowed or (with require_spot) no spot is free
	 */
	bool unstow(const std::shared_ptr<GameState> &state, entity_id_t id, size_t slot, bool require_spot,
	            const time::time_t &time);

	/**
	 * Conversion (W6): the entity changes its owner – combat values of the new owner's data,
	 * no target, no order, the player colour follows. Other units drop it as a target.
	 */
	void change_owner(const std::shared_ptr<GameState> &state,
	                  entity_id_t id,
	                  player_id_t owner,
	                  const std::shared_ptr<nyan::View> &db_view,
	                  const nyan::fqon_t &nyan_entity,
	                  const time::time_t &time);

	/// stop moving at once (special orders: a trebuchet unpacks, a monk prays)
	void halt_entity(entity_id_t id, const time::time_t &time);
	// ---- end XR fork W6

	// ---- event handlers
	void tick(const std::shared_ptr<GameState> &state, entity_id_t attacker, const time::time_t &time);
	void scan(const std::shared_ptr<GameState> &state, const time::time_t &time);
	void remove(const std::shared_ptr<GameState> &state, entity_id_t id, const time::time_t &time);
	/// impact of a projectile (XR fork W4)
	void impact(const std::shared_ptr<GameState> &state, uint64_t projectile, const time::time_t &time);

	/**
	 * Health of an entity changed outside of combat (XR fork, production: a foundation
	 * gains health while it is built); refreshes the thread-safe health snapshot.
	 */
	void health_changed(entity_id_t id, int64_t health);

	/**
	 * Forget the cached combat values read from a nyan view (XR fork W1: a
	 * technology patched the view of a player). Call before refresh_entity().
	 */
	void invalidate_stats(const std::shared_ptr<nyan::View> &db_view);

	/**
	 * Read the combat values of a living entity again from the (patched) view of
	 * its owner (XR fork W1). Like in AoE II, a higher maximum health raises the
	 * current health by the same amount (lower: capped); a replacement of the type
	 * (p proportional: line upgrades, buildings of a new age; docs/aoe2-spec 03 §3.3)
	 * scales the health with the maximum. Buildings under construction keep their
	 * health (the construction raises it).
	 *
	 * @return true if the maximum health changed.
	 */
	bool refresh_entity(const std::shared_ptr<GameEntity> &entity,
	                    const std::shared_ptr<nyan::View> &db_view,
	                    const nyan::fqon_t &nyan_entity,
	                    bool under_construction,
	                    bool proportional,
	                    const time::time_t &time);

	// ---- save games (XR fork W4)

	/// combat values of an entity worth saving, nothing if all are defaults
	std::optional<SavedCombat> save_entity(entity_id_t id) const;

	/**
	 * Apply saved combat values after all entities of a save game exist
	 * (ids already translated): stance, formation, garrison (the unit is put
	 * into the building at once), bell, orders.
	 */
	void load_entity(const std::shared_ptr<GameState> &state, entity_id_t id, const SavedCombat &saved,
	                 const time::time_t &time);

	// ---- queries, simulation thread
	bool is_dead(entity_id_t id) const;
	std::shared_ptr<const CombatStats> get_stats(entity_id_t id) const;
	std::optional<entity_id_t> get_target(entity_id_t id) const;
	stance_t get_stance(entity_id_t id) const;
	order_t get_order(entity_id_t id) const;
	formation_t get_formation(entity_id_t id) const;
	/// building the unit is garrisoned in
	std::optional<entity_id_t> garrisoned_in(entity_id_t id) const;
	/// the unit walks to a building to garrison there
	std::optional<entity_id_t> entering(entity_id_t id) const;
	/// units in a building
	std::vector<entity_id_t> occupants(entity_id_t building) const;
	/// arrows of the next volley of a building (05 §5.4)
	int arrow_count_of(entity_id_t building) const;
	/// a villager flees from attackers (06 §6.4)
	bool is_fleeing(entity_id_t id) const;
	/// projectiles launched and their impacts (statistics, checks)
	struct ProjectileStatsCount {
		size_t launched = 0;
		size_t hits = 0;
		size_t misses = 0;
		size_t miss_hits = 0;
		size_t extra_arrows = 0;
		size_t in_flight = 0;
	};
	ProjectileStatsCount projectile_counts() const;

	// ---- queries, thread-safe
	/// the entity was removed from the game (died); selections drop it
	bool was_removed(entity_id_t id) const;
	/// the entity is garrisoned (not drawn, not selectable)
	bool is_hidden(entity_id_t id) const;
	/// garrisoned entities and a version that changes with the set (renderer)
	std::shared_ptr<const std::unordered_set<entity_id_t>> hidden_entities(uint64_t &version) const;
	/// health of the given entities (unknown ids: alive = false)
	std::vector<HealthInfo> get_health(const std::vector<entity_id_t> &ids) const;
	/// combat values (name, unit/building, ...) of a registered entity, nullptr if
	/// unknown or removed; the stats are immutable and may be kept by the caller
	std::shared_ptr<const CombatStats> get_stats_snapshot(entity_id_t id) const;
	/// stance, garrison, bell of an entity (XR fork W4)
	std::optional<CombatInfo> get_info(entity_id_t id) const;
	/// projectiles in flight (XR fork W4) and a version that changes with every launch/impact
	std::vector<ProjectileView> get_projectiles(uint64_t &version) const;
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

		// ---- XR fork W4
		stance_t stance = stance_t::AGGRESSIVE;
		formation_t formation = formation_t::NONE;
		/// where the unit got its last order or stopped (stances measure from here)
		std::optional<Point> post{};
		order_t order = order_t::NONE;
		/// patrol: two points; attack move: the destination
		std::vector<Point> route{};
		size_t leg = 0;
		std::optional<entity_id_t> order_entity{};
		/// last move command of the order logic
		time::time_t order_moved = time::TIME_MIN;
		std::optional<Point> order_dest{};
		/// garrison: inside a building, on the way into one, occupants of a building
		std::optional<entity_id_t> inside{};
		std::optional<entity_id_t> entering{};
		std::vector<entity_id_t> occupants{};
		double heal_carry = 0.0;
		/// entered with the town bell: where it came from and what it did
		bool by_bell = false;
		Point bell_from{};
		std::optional<entity_id_t> resume_gather{};
		std::optional<entity_id_t> resume_build{};
		/// fought automatically in a defensive stance: walks back to the post afterwards
		bool return_to_post = false;
		/// town center: the bell rings
		bool bell = false;
		/// flight of villagers: attackers (id, time of the last attack report)
		std::vector<std::pair<entity_id_t, double>> attackers{};
		bool fleeing = false;
		double flee_ordered = -1.0e9;
		/// target lock after a failed attempt (06 §6.2): id -> until (s)
		std::unordered_map<entity_id_t, double> locks{};
	};

	struct Projectile {
		uint64_t id = 0;
		entity_id_t shooter = 0;
		player_id_t owner = 0;
		std::shared_ptr<const CombatStats> stats;
		/// hit roll passed: can only hit this target
		std::optional<entity_id_t> target{};
		Flight flight{};
		double launched = 0.0;
		/// elevation factor of the shooter at the launch
		double elevation = 1.0;
		bool secondary = false;
	};

	Combatant *find(entity_id_t id);
	const Combatant *find(entity_id_t id) const;
	player_id_t owner_of(const std::shared_ptr<GameEntity> &entity, const time::time_t &time) const;
	bool is_enemy(const Combatant &a, const Combatant &b, const time::time_t &time) const;
	/// the stance of the owner of \p self allows an automatic attack on \p other (06: neutral spares villagers)
	bool auto_target_ok(const Combatant &self, const Combatant &other, const time::time_t &time) const;
	/// XR fork W3: a player's entity and a gaia animal (explicit hunt, a boar fighting back)
	bool is_hunt(const Combatant &a, const Combatant &b, const time::time_t &time) const;
	coord::phys3 position_of(const std::shared_ptr<GameEntity> &entity, const time::time_t &time) const;
	bool is_moving(const std::shared_ptr<GameEntity> &entity, const time::time_t &time) const;
	double edge_between(const Combatant &a, const Combatant &b, const time::time_t &time) const;
	std::optional<entity_id_t> nearest_enemy(const Combatant &self, double range, const time::time_t &time) const;
	/// automatic target choice (06 §6.2), honouring the stance; nothing if none
	std::optional<entity_id_t> choose_target(const Combatant &self, const time::time_t &time) const;
	/// distance from the post (or route, guarded object) for the stance rules
	double from_post(const Combatant &self, const Point &p, const time::time_t &time) const;

	/// the target rules (on_target) allow this target
	bool target_allowed(entity_id_t attacker, entity_id_t target) const;
	void engage(const std::shared_ptr<GameState> &state, Combatant &attacker, entity_id_t target,
	            bool explicit_order, const time::time_t &time);
	void schedule_tick(const std::shared_ptr<GameState> &state, Combatant &attacker, const time::time_t &time);
	void drop_target(Combatant &attacker, const time::time_t &time);
	void halt(const std::shared_ptr<GameEntity> &entity, const time::time_t &time);
	/// one attack: melee hit or projectiles
	void attack(const std::shared_ptr<GameState> &state, Combatant &attacker, Combatant &target, const time::time_t &time);
	void launch(const std::shared_ptr<GameState> &state, Combatant &attacker, Combatant &target, const time::time_t &time);
	/// damage of one hit; \p attacker may be gone already (projectiles)
	void apply_hit(const std::shared_ptr<GameState> &state,
	               const CombatStats &attacker_stats,
	               entity_id_t attacker_id,
	               player_id_t attacker_owner,
	               Combatant &target,
	               double factor,
	               const time::time_t &time);
	void die(const std::shared_ptr<GameState> &state, Combatant &victim, const time::time_t &time,
	         const std::string &cause);
	void update_match(const time::time_t &time);
	void set_health_snapshot(entity_id_t id, int64_t health, int64_t max_health, bool alive);
	bool allow_hit_log(const time::time_t &time);

	/// elevation factor of an attack (05 §5.2)
	double elevation_between(const std::shared_ptr<GameState> &state, const Point3 &a, const Point3 &b,
	                         double high) const;
	/// terrain factor g of a target (05 §5.2)
	double terrain_factor_of(const std::shared_ptr<GameState> &state, const Combatant &target,
	                         const time::time_t &time);
	/// height of the terrain (scene up units)
	double ground_height(const std::shared_ptr<GameState> &state, double ne, double se) const;

	/// orders (patrol, guard, follow, attack move, stance leash) once per scan
	void update_order(const std::shared_ptr<GameState> &state, Combatant &c, const time::time_t &time);
	/// move command of the order logic (rate limited)
	void order_move(const std::shared_ptr<GameState> &state, Combatant &c, const Point &dest, const time::time_t &time,
	                bool force = false);
	void clear_order(Combatant &c);
	/// a passable tile centre near p p for the unit (the point itself if passable)
	Point passable_near(const std::shared_ptr<GameState> &state, const Combatant &c, const Point &p) const;
	/// enter / leave buildings
	bool accepts(const Combatant &building, const Combatant &unit, const time::time_t &time) const;
	void enter(const std::shared_ptr<GameState> &state, Combatant &unit, Combatant &building, const time::time_t &time);
	void leave(const std::shared_ptr<GameState> &state, Combatant &unit, Combatant &building, size_t slot,
	           const time::time_t &time);
	void eject_all(const std::shared_ptr<GameState> &state, Combatant &building, bool bell_only, const time::time_t &time);
	/// running arrow value of a building (05 §5.4)
	double running_arrows(const Combatant &building, const time::time_t &time) const;
	/// villagers: react to an attack report (06 §6.4)
	void attacked(const std::shared_ptr<GameState> &state, Combatant &victim, entity_id_t attacker,
	              player_id_t attacker_owner, const time::time_t &time);
	void update_flight(const std::shared_ptr<GameState> &state, Combatant &c, const time::time_t &time);
	/// remember and resume the economic job of a villager (bell, flight)
	void remember_job(Combatant &c);
	void resume_job(const std::shared_ptr<GameState> &state, Combatant &c, const time::time_t &time);
	/// the building is complete (no foundation)
	bool finished(const Combatant &c) const;

	void publish_info(const Combatant &c);
	void publish_hidden();
	void publish_projectiles();

	std::shared_ptr<openage::event::EventLoop> loop;

	/// combat data per entity (entities with a Live health attribute)
	std::unordered_map<entity_id_t, Combatant> combatants;
	/// stats per nyan entity (and owner view)
	std::unordered_map<std::string, std::shared_ptr<const CombatStats>> stats_cache;

	std::unordered_set<player_id_t> neutral;
	/// XR fork W7: stances that are not "enemy"
	std::map<std::pair<player_id_t, player_id_t>, diplomacy::stance_t> stances;
	/// XR fork W3: death and hit handlers (food state)
	std::vector<std::function<bool(const std::shared_ptr<GameEntity> &, const time::time_t &)>> death_handlers;
	std::vector<std::function<void(const std::shared_ptr<GameEntity> &, const std::shared_ptr<GameEntity> &,
	                               const time::time_t &)>>
		hit_handlers;
	/// XR fork W5: footprint of gates (set_footprint_hook)
	std::function<std::optional<std::vector<coord::tile>>(entity_id_t)> footprint_hook;
	/// XR fork W6: impact handlers (area damage)
	std::vector<impact_handler_t> impact_handlers;
	/// XR fork W6: target rules
	std::vector<target_filter_t> target_filters;
	siege_gate_fn_t siege_gate;
	/// fog of war (XR fork), may be nullptr
	std::shared_ptr<vision::VisionState> vision;
	/// players that ever had units or buildings
	std::set<player_id_t> participants;
	bool started = false;
	bool match_over = false;

	/// hit logs per simulation second (throttled)
	int64_t log_second = -1;
	size_t logs_in_second = 0;
	size_t logs_suppressed = 0;

	// ---- XR fork W4, simulation thread
	std::map<uint64_t, Projectile> projectiles;
	uint64_t next_projectile = 1;
	ProjectileStatsCount projectile_count{};
	/// threat memory per player (06 §6.2)
	std::map<player_id_t, ThreatMemory> threats;
	/// deterministic randomness (hit rolls, dispersion, flight directions)
	std::mt19937_64 rng{0x5734};
	double last_scan = -1.0;
	/// damage terrain restrictions of the data: restriction -> genie terrain id -> factor,
	/// terrain fqon -> genie id (loaded once, empty without engine.xr data)
	bool terrain_loaded = false;
	std::unordered_map<int, std::unordered_map<int, double>> terrain_restrictions;
	std::unordered_map<std::string, int> terrain_ids;
	/// modpack of the game entities (GenieData lookup)
	std::string modpack = "hd_base";
	/// players with crenellations (infantry adds garrison arrows, 05 §5.4)
	std::unordered_set<player_id_t> crenellations_players;
	/// players with herbal medicine (garrison heals 4 times faster, public knowledge: the converted
	/// technology has no patches)
	std::unordered_set<player_id_t> herbal_players;
	/// time of the last event or command (snapshots outside of an event)
	time::time_t clock = time::TIME_ZERO;

	// ---- shared with other threads
	mutable std::mutex shared_mutex;
	std::unordered_set<entity_id_t> removed;
	std::unordered_map<entity_id_t, HealthInfo> health_snapshot;
	std::unordered_map<entity_id_t, std::shared_ptr<const CombatStats>> stats_snapshot;
	std::unordered_map<entity_id_t, CombatInfo> info_snapshot;
	std::shared_ptr<const std::unordered_set<entity_id_t>> hidden_snapshot;
	uint64_t hidden_version = 0;
	std::vector<ProjectileView> projectile_snapshot;
	uint64_t projectile_version = 0;
	MatchStatus match_status{};
	std::vector<std::function<void(const MatchStatus &)>> match_callbacks;
};

} // namespace combat
} // namespace gamestate
} // namespace openage
