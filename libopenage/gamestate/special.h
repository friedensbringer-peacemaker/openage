// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nyan/nyan.h>

#include "coord/phys.h"
#include "event/evententity.h"
#include "event/eventhandler.h"
#include "gamestate/component/internal/commands/types.h"
#include "gamestate/special_math.h"
#include "gamestate/types.h"
#include "time/time.h"


namespace openage {
namespace event {
class EventLoop;
} // namespace event

namespace gamestate {
class GameEntity;
class GameState;

namespace combat {
struct CombatStats;
} // namespace combat

namespace special {

/// what a carrier takes in (W6, spec 06 §6.5)
enum class cargo_t : uint8_t {
	NONE,
	/// transport ship: land units
	TRANSPORT,
	/// ram: infantry only (protection)
	RAM,
};

/**
 * Special values of a game entity type from the nyan data (XR fork W6) in the view of
 * its owner (technologies patch ranges and rates). Read once per type and view.
 */
struct SpecialType {
	/// short name of the nyan object ("Monk")
	std::string name;
	/// GenieUnitData of the head unit (engine.xr, W0), -1 if missing
	int unit_class = -1;
	int unit_id = -1;
	int unit_level = -1;
	bool unit = false;
	bool building = false;
	/// ships (types Ship/Warship/TransportShip/FishingShip or classes 2, 20, 21, 22, 53)
	bool ship = false;
	/// siege (types SiegeWeapon/Trebuchet or classes 13, 51, 54, 55)
	bool siege = false;
	/// relic (class 42) and monastery (unit 104 / Monastery): relic gold (02 §2.10)
	bool relic = false;
	bool monastery = false;
	/// scout/eagle warrior line (resistance +8, 05 §5.7)
	bool scout_line = false;
	/// buildings a conversion never takes (town center, castle, wonder, walls, gates, fields)
	bool never_converted = false;

	// ---- monks (05 §5.7, 5.8)
	/// Convert ability (RangedDiscreteEffect with conversion effects)
	bool monk = false;
	double convert_range = 9.0;
	double convert_reload = 1.0;
	/// chance of the conversion effect × 100 ("accuracy" 25)
	double convert_chance = 25.0;
	/// work values 1/2 of the conversion task 104 (4 s / 10 s)
	double convert_min = 4.0;
	double convert_max = 10.0;
	/// allowed types of the Convert ability
	bool convert_units = true;
	bool convert_buildings = false;
	/// blacklisted entity fqons (siege, monks; Redemption/Atonement lift them)
	std::vector<std::string> convert_blacklist{};
	/// Heal ability (RangedContinuousEffect)
	bool heals = false;
	double heal_range = 4.0;
	/// heal task value a₁ (rate of the heal effect, 2)
	double heal_task = 2.0;
	/// GenieUnitData.work_rate (1.25 for monks)
	double work_rate = 1.0;
	/// search range of the stances (automatic healing)
	double search_range = 0.0;
	/// RegenerateFaith rate (0: no faith)
	double faith_rate = 0.0;
	/// CollectStorage: picks up relics
	bool carries_relics = false;

	// ---- area damage (05 §5.5)
	double blast_range = 0.0;
	int blast_offence = 0;
	int blast_defense = 0;
	/// kills itself with its attack (demolition ship, petard)
	bool self_destruct = false;

	// ---- carriers (06 §6.5)
	cargo_t cargo = cargo_t::NONE;
	int cargo_slots = 0;

	// ---- trebuchet (transform unit of the data, packed/unpacked)
	bool packable = false;
	/// melee attack (ApplyDiscreteEffect "Attack"); rams = melee siege: they attack buildings only
	bool melee = false;
	bool ram = false;
};

/// what the HUD shows of an entity (W6)
struct SpecialInfo {
	bool monk = false;
	bool heals = false;
	/// faith 0..100 (monks)
	int faith = 0;
	/// "Konvertiert", "Heilt", "Hebt Reliquie auf", "Bringt Reliquie", "Packt", ... (empty: nothing)
	std::string job{};
	/// carries a relic
	bool relic = false;
	/// monastery: relics inside and gold per second
	int relics = 0;
	double relic_gold = 0.0;
	/// carrier: units inside / places
	int cargo = 0;
	int cargo_max = 0;
	bool transport = false;
	/// trebuchet: packed (moves) or unpacked (shoots); packing in progress
	bool packable = false;
	bool unpacked = false;
	bool packing = false;
};

/// saved values of one entity (save game version 6, all optional)
struct SavedSpecial {
	std::optional<double> faith{};
	/// conversion in progress: target and faith time t
	std::optional<entity_id_t> convert_target{};
	double convert_time = 0.0;
	/// heal target
	std::optional<entity_id_t> heal_target{};
	/// carried relic (monk)
	std::optional<entity_id_t> relic_carry{};
	/// relics in a monastery
	std::vector<entity_id_t> relic_store{};
	/// units in a carrier
	std::vector<entity_id_t> cargo{};
	/// trebuchet: "packed" or "unpacked" (empty: packed)
	std::string siege_mode{};
};

/**
 * Special abilities of round W6 (XR fork, docs/aoe2-spec 05 §5.5/5.7–5.9, 06 §6.5, 07 §7.4):
 * faith and conversions of monks, healing, relics (carry, monastery, gold), area damage of
 * siege and demolition ships, trebuchets (pack/unpack), carriers (transport ship, ram).
 *
 * Rules: gamestate/special_math.h. Values from the nyan data of the owner (ranges, rates,
 * chance, work values, carrier places) and the player resources of the data
 * (GenieData.civs, engine.xr); researched technologies of the spec (Redemption, Atonement,
 * Heresy, Theocracy, Faith, Illumination) switch the rules that the data keeps in player
 * resources.
 *
 * Runs in the simulation thread (periodic event "game.special_tick" every TICK seconds of
 * simulation time); the HUD reads snapshots under a mutex.
 */
class SpecialState : public openage::event::EventEntity
	, public std::enable_shared_from_this<SpecialState> {
public:
	/// period of the special tick (simulation seconds)
	static constexpr double TICK = 0.25;
	/// trebuchet: time to pack or unpack (spec without value; assumption, CHANGELOG)
	static constexpr double PACK_TIME = 4.0;

	explicit SpecialState(const std::shared_ptr<openage::event::EventLoop> &loop);
	~SpecialState() = default;

	size_t id() const override;
	std::string idstr() const override;

	/**
	 * Start: hooks into the combat state (death, impact) and the periodic tick.
	 *
	 * @param neutral Players without monks of their own (gaia).
	 * @param seed Seed of the deterministic generator (map seed).
	 */
	void start(const std::shared_ptr<GameState> &state,
	           const std::unordered_set<player_id_t> &neutral,
	           uint64_t seed,
	           const time::time_t &time);
	bool is_started() const;

	/// periodic tick (event handler): faith, jobs, relic gold, trebuchets, carriers
	void tick(const std::shared_ptr<GameState> &state, const time::time_t &time);

	// ---- orders (empty string: accepted, else the German reason for the HUD)
	std::string order_convert(const std::shared_ptr<GameState> &state, entity_id_t monk, entity_id_t target,
	                          const time::time_t &time);
	std::string order_heal(const std::shared_ptr<GameState> &state, entity_id_t monk, entity_id_t target,
	                       const time::time_t &time);
	std::string order_pickup(const std::shared_ptr<GameState> &state, entity_id_t monk, entity_id_t relic,
	                         const time::time_t &time);
	std::string order_deliver(const std::shared_ptr<GameState> &state, entity_id_t monk, entity_id_t monastery,
	                          const time::time_t &time);
	std::string drop_relic(const std::shared_ptr<GameState> &state, entity_id_t monk, const time::time_t &time);
	std::string order_board(const std::shared_ptr<GameState> &state, entity_id_t unit, entity_id_t carrier,
	                        const time::time_t &time);
	/// let the cargo out: at once next to the carrier (ram, or a ship at the shore), or the ship
	/// sails towards \p point first
	std::string unload(const std::shared_ptr<GameState> &state, entity_id_t carrier,
	                   const std::optional<coord::phys3> &point, const time::time_t &time);
	std::string set_packed(const std::shared_ptr<GameState> &state, entity_id_t trebuchet, bool packed,
	                       const time::time_t &time);
	/// a player command: the special job of the entity ends (moves elsewhere, stop, attack)
	void cancel_job(entity_id_t id);
	/// an unpacked trebuchet packs before it walks to \p destination
	bool pack_and_move(const std::shared_ptr<GameState> &state, entity_id_t id, const coord::phys3 &destination,
	                   const time::time_t &time);

	// ---- queries (simulation thread)
	/// special values of the entity's type in its owner's view
	const SpecialType *type_of(const std::shared_ptr<GameState> &state, const std::shared_ptr<GameEntity> &entity,
	                           const time::time_t &time);
	std::optional<double> faith(entity_id_t id) const;
	/// target of the current special job (convert, heal, relic, carrier), if any
	std::optional<entity_id_t> get_job_target(entity_id_t id) const;
	/// a relic of the game
	bool is_relic(entity_id_t id) const {
		return this->relic_ids.contains(id);
	}
	/// gaia (no conversions of or by it)
	bool neutral_player(player_id_t player) const {
		return this->neutral.contains(player);
	}
	/// stowed in a carrier (ship, ram, monastery, monk): the carrier
	std::optional<entity_id_t> stowed_in(entity_id_t id) const;
	std::vector<entity_id_t> stowed(entity_id_t carrier) const;
	std::optional<entity_id_t> carried_relic(entity_id_t monk) const;
	bool is_unpacked(entity_id_t id) const;
	/// whether a target may be converted by the monk (empty) or why not
	std::string convert_check(const std::shared_ptr<GameState> &state, entity_id_t monk, entity_id_t target,
	                          const time::time_t &time);
	/// player resource of the data (GenieData.civs of the player's civilisation), fallback if missing
	double player_resource(const std::shared_ptr<GameState> &state, player_id_t player, int index, double fallback);

	// ---- snapshot for the HUD (any thread)
	std::optional<SpecialInfo> get_info(entity_id_t id) const;

	// ---- hooks of the combat state
	/// a carrier dies: ship cargo dies with it, a ram or monastery lets it out; a relic falls
	bool on_death(const std::shared_ptr<GameState> &state, const std::shared_ptr<GameEntity> &entity,
	              const time::time_t &time);
	/// area damage around an impact (05 §5.5), self destruction of demolition ships
	void on_impact(const std::shared_ptr<GameState> &state, entity_id_t attacker, player_id_t owner,
	               const combat::CombatStats &stats, std::optional<entity_id_t> main_target, double x0, double y0,
	               const time::time_t &time);

	// ---- save games (version 6)
	std::optional<SavedSpecial> save_entity(entity_id_t id) const;
	/// restore after all entities exist; \p mapped turns saved ids into the new ones
	void load_entity(const std::shared_ptr<GameState> &state, entity_id_t id, const SavedSpecial &saved,
	                 const std::function<entity_id_t(entity_id_t)> &mapped, const time::time_t &time);

	/// statistics for logs and checks
	struct Counters {
		size_t conversions = 0;
		size_t conversion_attempts = 0;
		size_t heresy_deaths = 0;
		double healed = 0.0;
		size_t relics_picked = 0;
		size_t relics_delivered = 0;
		size_t relics_dropped = 0;
		double relic_gold = 0.0;
		size_t splash_hits = 0;
		size_t friendly_hits = 0;
		size_t boarded = 0;
		size_t unloaded = 0;
		size_t cargo_lost = 0;
		size_t packs = 0;
		size_t self_destructs = 0;
	};
	Counters counters() const;

private:
	enum class job_t : uint8_t {
		NONE,
		CONVERT,
		HEAL,
		PICKUP,
		DELIVER,
		BOARD,
		UNLOAD,
	};

	struct Job {
		job_t kind = job_t::NONE;
		entity_id_t target = 0;
		/// ordered by the player (else automatic healing)
		bool explicit_order = false;
		/// conversion: faith time t starts with the prayer (in range); next attempt
		std::optional<double> prayer_since{};
		double next_attempt = 0.0;
		/// heal: fractional HP not yet applied
		double heal_carry = 0.0;
		/// walking: last move order, its destination, failed tries
		double moved_at = -1.0e9;
		std::optional<std::pair<double, double>> move_dest{};
		int failures = 0;
		double started = 0.0;
		/// unload: point of the order
		std::optional<coord::phys3> point{};
	};

	struct Unit {
		double faith = 100.0;
		bool has_faith = false;
		Job job{};
		std::optional<entity_id_t> relic{};
		/// trebuchet
		bool unpacked = false;
		std::optional<double> pack_until{};
		bool pack_target = false;
		std::optional<coord::phys3> after_pack{};
		double last_auto = -1.0e9;
	};

	Unit &unit(entity_id_t id);
	Unit *find_unit(entity_id_t id);
	const Unit *find_unit(entity_id_t id) const;
	void discover(const std::shared_ptr<GameState> &state, const time::time_t &time);
	void run_job(const std::shared_ptr<GameState> &state, entity_id_t id, Unit &u, double dt, const time::time_t &time);
	/// walk into reach of a target (edge distance); true when within \p reach
	bool approach(const std::shared_ptr<GameState> &state, entity_id_t id, Job &job, entity_id_t target, double reach,
	              const time::time_t &time);
	void attempt_conversion(const std::shared_ptr<GameState> &state, entity_id_t monk, Unit &u,
	                        const time::time_t &time);
	void heal_step(const std::shared_ptr<GameState> &state, entity_id_t monk, Unit &u, double dt,
	               const time::time_t &time);
	void auto_heal(const std::shared_ptr<GameState> &state, entity_id_t monk, Unit &u, const time::time_t &time);
	void relic_gold(const std::shared_ptr<GameState> &state, const time::time_t &time);
	void stow_into(const std::shared_ptr<GameState> &state, entity_id_t id, entity_id_t carrier,
	               const time::time_t &time);
	bool release(const std::shared_ptr<GameState> &state, entity_id_t id, size_t slot, bool require_spot,
	             const time::time_t &time);
	std::string board_check(const std::shared_ptr<GameState> &state, entity_id_t unit, entity_id_t carrier,
	                        const time::time_t &time);
	bool researched(const std::shared_ptr<GameState> &state, player_id_t player, const std::string &tech) const;
	void publish(const std::shared_ptr<GameState> &state, const time::time_t &time);

	std::shared_ptr<openage::event::EventLoop> loop;
	std::shared_ptr<openage::event::EventHandler> handler;
	std::unordered_set<player_id_t> neutral;
	bool started = false;
	double last_tick = 0.0;
	double last_log = -1.0e9;
	/// whole seconds of relic gold already paid
	int64_t gold_second = -1;
	std::mt19937_64 rng{0x57365};
	std::unordered_map<entity_id_t, Unit> units;
	/// stowed entity -> carrier, carrier -> stowed entities (order of entering)
	std::unordered_map<entity_id_t, entity_id_t> inside;
	std::map<entity_id_t, std::vector<entity_id_t>> carried;
	/// entities whose type was looked at (discover)
	std::unordered_set<entity_id_t> seen;
	/// relics of the game (save games: relic_store instead of cargo)
	std::unordered_set<entity_id_t> relic_ids;
	/// rams (target rule: buildings only)
	std::unordered_set<entity_id_t> ram_ids;
	std::unordered_map<std::string, SpecialType> types;
	std::unordered_map<player_id_t, std::unordered_map<int, double>> civ_resources;
	Counters stats{};

	mutable std::mutex snapshot_mutex;
	std::unordered_map<entity_id_t, SpecialInfo> snapshot;
};

/**
 * Commands of round W6 for the selection (called by the send-command event before the
 * combat commands): convert, heal, pack/unpack, unload, drop relic, and right clicks of
 * monks (enemy: convert, own wounded unit: heal, relic: pick up, own monastery: deliver),
 * of land units on an own transport ship or ram (board) and of loaded ships on land
 * (unload there). Handled ids are removed from \p ids.
 *
 * @return true if the command was a W6 command (nothing else to do for any id).
 */
bool handle_command(const std::shared_ptr<GameState> &state,
                    const time::time_t &time,
                    component::command::command_t type,
                    std::vector<entity_id_t> &ids,
                    const coord::phys3 &target,
                    const openage::event::EventHandler::param_map &params);

/**
 * Event handler of the special tick ("game.special_tick"), created by the special state.
 */
class SpecialEventHandler : public openage::event::OnceEventHandler {
public:
	SpecialEventHandler();
	~SpecialEventHandler() = default;

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

} // namespace special
} // namespace gamestate
} // namespace openage
