// Copyright 2026-2026 the openage authors. See copying.md for legal info.

/*
 * Game state check of the combat (XR fork), headless, with the converted
 * modpack but without renderer:
 *
 *   openage-combat-check --root <dir> [--modpack hd_base] [--seed N] [--size N]
 *
 * Random map with --map-skirmish armies, simulated time advanced in steps of
 * 50 ms through the event loop (like GameSimulation::run, but as fast as possible):
 *
 *  1. values: knight, militia, archer, town center read from nyan (health,
 *     attack, armor, reload, range); buildings start with full health
 *  1b. villagers (economy activity): a right click on a gaia resource gathers
 *     (no attack), an attack command makes them stop gathering, walk over and
 *     hit an enemy villager; gaia does not count for the victory
 *  2. auto attack: a militia of player 0 walks (move command) between the
 *     armies; enemies in sight attack it automatically and it dies
 *  3. attack command: the knights of player 0 get a right click (move command
 *     on the ground position of an enemy knight, picked without camera) ->
 *     they attack, the target loses health and is removed from the game state
 *     after its death animation; commands for the dead id are ignored
 *  4. HUD queries: health of the selection, removed ids
 *  5. victory: all other entities of player 1 are killed -> the town center
 *     tiles become passable, the match is decided once (callback), player 0
 *     has VICTORY, player 1 DEFEAT
 *
 * Prints "combat check ok" and exits with 0 if everything passes.
 */

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "assets/mod_manager.h"
#include "event/event_loop.h"
#include "gamestate/combat/combat_state.h"
#include "gamestate/combat/events.h"
#include "gamestate/component/api/live.h"
#include "gamestate/component/internal/activity.h"
#include "gamestate/component/internal/commands/types.h"
#include "gamestate/component/internal/ownership.h"
#include "gamestate/component/internal/position.h"
#include "gamestate/component/types.h"
#include "gamestate/entity_factory.h"
#include "gamestate/event/drag_select.h"
#include "gamestate/event/process_command.h"
#include "gamestate/event/send_command.h"
#include "gamestate/event/spawn_entity.h"
#include "gamestate/event/wait.h"
#include "gamestate/game.h"
#include "gamestate/game_entity.h"
#include "gamestate/game_state.h"
#include "gamestate/manager.h"
#include "gamestate/map.h"
#include "gamestate/player.h"
#include "gamestate/tech.h"
#include "gamestate/terrain_factory.h"
#include "log/log.h"
#include "pathfinding/cost_field.h"
#include "pathfinding/definitions.h"
#include "pathfinding/grid.h"
#include "pathfinding/pathfinder.h"
#include "pathfinding/sector.h"
#include "util/fslike/directory.h"
#include "util/path.h"

using namespace openage;
using namespace openage::gamestate;

namespace {

int failures = 0;

void check(bool ok, const std::string &what) {
	std::cout << (ok ? "  ok   " : "  FAIL ") << what << std::endl;
	if (not ok) {
		failures += 1;
	}
}

struct World {
	std::shared_ptr<openage::event::EventLoop> loop;
	std::shared_ptr<Game> game;
	std::shared_ptr<GameState> state;
	std::shared_ptr<gamestate::event::Commander> commander;
	time::time_t now = time::TIME_ZERO;

	void advance(double seconds) {
		auto until = this->now + time::time_t::from_double(seconds);
		while (this->now < until) {
			this->now = std::min(until, this->now + time::time_t::from_double(0.05));
			this->loop->reach_time(this->now, this->state);
		}
	}

	/// advance until done() or the timeout; returns true if done
	template <typename F>
	bool advance_until(double timeout, F done) {
		auto until = this->now + time::time_t::from_double(timeout);
		while (this->now < until) {
			if (done()) {
				return true;
			}
			this->advance(0.05);
		}
		return done();
	}

	void command(component::command::command_t type,
	             const std::vector<entity_id_t> &ids,
	             const coord::phys3 &target) {
		openage::event::EventHandler::param_map::map_t params{
			{"type", type},
			{"target", target},
			{"entity_ids", ids},
		};
		this->loop->create_event("game.send_command", this->commander, this->state, this->now, params);
		this->advance(0.05);
	}

	std::shared_ptr<combat::CombatState> combat() const {
		return this->state->get_combat();
	}

	bool exists(entity_id_t id) const {
		return this->state->get_game_entities().contains(id);
	}

	player_id_t owner(entity_id_t id) const {
		auto owner = std::dynamic_pointer_cast<component::Ownership>(
			this->state->get_game_entity(id)->get_component(component::component_t::OWNERSHIP));
		return owner->get_owners().get(this->now);
	}

	coord::phys3 position(entity_id_t id) const {
		auto pos = std::dynamic_pointer_cast<component::Position>(
			this->state->get_game_entity(id)->get_component(component::component_t::POSITION));
		return pos->get_positions().get(this->now);
	}

	int64_t health(entity_id_t id) const {
		auto stats = this->combat()->get_stats(id);
		auto live = std::dynamic_pointer_cast<component::Live>(
			this->state->get_game_entity(id)->get_component(component::component_t::LIVE));
		return live->get_attribute(this->now, stats->health_attribute).value_or(-1);
	}

	/// entities of a player with the given stats name, sorted by id
	std::vector<entity_id_t> find(player_id_t player, const std::string &name) const {
		std::vector<entity_id_t> result;
		for (const auto &[id, entity] : this->state->get_game_entities()) {
			auto stats = this->combat()->get_stats(id);
			if (stats and stats->name == name and this->owner(id) == player and not this->combat()->is_dead(id)) {
				result.push_back(id);
			}
		}
		std::sort(result.begin(), result.end());
		return result;
	}
};

double dist(const coord::phys3 &a, const coord::phys3 &b) {
	return std::hypot(a.ne.to_double() - b.ne.to_double(), a.se.to_double() - b.se.to_double());
}

// ---------------------------------------------------------------- round W4 (XR fork)

/// a fresh game for the W4 checks (skirmish, everything visible, no computer player)
struct W4World : World {
	std::shared_ptr<EntityFactory> factory;
	std::string modpack;

	entity_id_t spawn(player_id_t owner, const std::string &dir, const std::string &cls, double ne, double se) {
		auto fqon = this->modpack + ".data.game_entity.generic." + dir + "." + dir + "." + cls;
		auto entity = this->factory->add_game_entity(this->loop, this->state, owner, fqon);
		auto pos = std::dynamic_pointer_cast<component::Position>(entity->get_component(component::component_t::POSITION));
		coord::phys3 p{coord::phys_t{ne}, coord::phys_t{se}, coord::phys_t{0.0}};
		pos->set_position(this->now, this->state->get_map()->on_terrain(p));
		auto own = std::dynamic_pointer_cast<component::Ownership>(entity->get_component(component::component_t::OWNERSHIP));
		own->set_owner(this->now, owner);
		auto activity = std::dynamic_pointer_cast<component::Activity>(entity->get_component(component::component_t::ACTIVITY));
		activity->init(this->now);
		entity->get_manager()->run_activity_system(this->now);
		this->state->add_game_entity(entity);
		return entity->get_id();
	}

	void teleport(entity_id_t id, double ne, double se) {
		auto pos = std::dynamic_pointer_cast<component::Position>(
			this->state->get_game_entity(id)->get_component(component::component_t::POSITION));
		coord::phys3 p{coord::phys_t{ne}, coord::phys_t{se}, coord::phys_t{0.0}};
		pos->set_position(this->now, this->state->get_map()->on_terrain(p));
	}

	void set_health(entity_id_t id, int64_t health) {
		auto stats = this->combat()->get_stats(id);
		auto live = std::dynamic_pointer_cast<component::Live>(
			this->state->get_game_entity(id)->get_component(component::component_t::LIVE));
		live->set_attribute(this->now, stats->health_attribute, health);
	}

	void order(component::command::command_t type, const std::vector<entity_id_t> &ids, const coord::phys3 &target,
	           std::optional<entity_id_t> entity = std::nullopt, int arg = 0) {
		openage::event::EventHandler::param_map::map_t params{
			{"type", type},
			{"target", target},
			{"entity_ids", ids},
			{"combat_arg", arg},
		};
		if (entity) {
			params.emplace("target_entity", *entity);
		}
		this->loop->create_event("game.send_command", this->commander, this->state, this->now, params);
		this->advance(0.05);
	}

	bool moving(entity_id_t id) const {
		auto pos = std::dynamic_pointer_cast<component::Position>(
			this->state->get_game_entity(id)->get_component(component::component_t::POSITION));
		auto a = pos->get_positions().get(this->now);
		auto b = pos->get_positions().get(this->now + time::time_t::from_double(0.1));
		return dist(a, b) > 1e-3;
	}

	/// a passable land spot near (ne, se), searched on a spiral
	std::optional<std::pair<double, double>> free_spot(double ne, double se) const {
		auto map = this->state->get_map();
		auto land = map->get_grid_id(this->modpack + ".data.util.path_type.types.Land");
		const auto size = map->get_size();
		for (int r = 0; r < 8; ++r) {
			for (int i = 0; i < 16; ++i) {
				const double a = 0.3927 * i;
				const double n = ne + r * std::cos(a), s = se + r * std::sin(a);
				if (n < 2 or s < 2 or n > static_cast<double>(size[0]) - 3 or s > static_cast<double>(size[1]) - 3) {
					continue;
				}
				coord::phys3 p{coord::phys_t{n}, coord::phys_t{s}, coord::phys_t{0}};
				if (map->is_passable(land, p.to_tile())) {
					return std::pair{std::floor(n) + 0.5, std::floor(s) + 0.5};
				}
			}
		}
		return std::nullopt;
	}
};

coord::phys3 at(double ne, double se) {
	return coord::phys3{coord::phys_t{ne}, coord::phys_t{se}, coord::phys_t{0}};
}

void w4_checks(const util::Path &root, const std::string &modpack, uint32_t seed, size_t size) {
	using component::command::command_t;
	std::cout << "W4. projectiles, stances, orders, garrison, town bell, flight, formations" << std::endl;
	W4World w;
	w.modpack = modpack;
	w.loop = std::make_shared<openage::event::EventLoop>();
	auto mod_manager = std::make_shared<assets::ModManager>(root / "assets" / "converted");
	for (const auto &mod : assets::ModManager::enumerate_modpacks(root / "assets" / "converted")) {
		mod_manager->register_modpack(mod);
	}
	mod_manager->activate_modpacks({"engine", modpack});
	w.factory = std::make_shared<EntityFactory>();
	auto terrain_factory = std::make_shared<TerrainFactory>();
	w.loop->add_event_handler(std::make_shared<gamestate::event::DragSelectHandler>());
	w.loop->add_event_handler(std::make_shared<gamestate::event::SpawnEntityHandler>(w.loop, w.factory));
	w.loop->add_event_handler(std::make_shared<gamestate::event::SendCommandHandler>());
	w.loop->add_event_handler(std::make_shared<gamestate::event::ProcessCommandHandler>());
	w.loop->add_event_handler(std::make_shared<gamestate::event::WaitHandler>());
	combat::add_event_handlers(w.loop);
	w.commander = std::make_shared<gamestate::event::Commander>(w.loop);
	MapSettings settings;
	settings.type = map_type_t::RANDOM;
	settings.seed = seed;
	settings.size = size;
	settings.skirmish = true;
	settings.vision = vision_mode_t::ALL;
	settings.ai.mode = ai_mode_t::OFF;
	w.game = std::make_shared<Game>(w.loop, mod_manager, w.factory, terrain_factory, settings);
	w.state = w.game->get_state();
	w.loop->reach_time(w.now, w.state);
	w.advance(0.5);
	auto combat = w.combat();

	auto knights0 = w.find(0, "Knight");
	auto knights1 = w.find(1, "Knight");
	auto militia0 = w.find(0, "Militia");
	auto militia1 = w.find(1, "Militia");
	auto archers0 = w.find(0, "Archer");
	auto tc0 = w.find(0, "TownCenter");
	auto villagers0 = w.find(0, "Villager");
	if (knights0.size() < 3 or knights1.size() < 3 or militia0.size() < 3 or militia1.size() < 3 or archers0.size() < 3
	    or tc0.empty() or villagers0.size() < 3) {
		check(false, "W4: skirmish armies and a town center");
		return;
	}

	// the armies stay calm (no attack) unless a check gives a unit another stance: every check
	// sees only the fights it starts
	for (const auto &list : {knights0, knights1, militia0, militia1, archers0, w.find(1, "Archer")}) {
		for (auto id : list) {
			combat->set_stance(id, combat::stance_t::NO_ATTACK);
		}
	}

	// ---- data values
	auto as = combat->get_stats(archers0[0]);
	auto ts = combat->get_stats(tc0[0]);
	auto ks = combat->get_stats(knights0[0]);
	auto vs = combat->get_stats(villagers0[0]);
	std::cout << "  archer projectile: speed " << as->projectile.speed << ", arc " << as->projectile.arc
	          << ", accuracy " << as->projectile.accuracy << ", dispersion " << as->projectile.dispersion
	          << ", lead " << as->projectile.lead << ", class " << as->unit_class << std::endl;
	std::cout << "  town center: garrison " << ts->garrison_capacity << " mask " << ts->garrison_mask << " heal "
	          << ts->garrison_heal << ", arrows " << ts->min_projectiles << "-" << ts->max_projectiles
	          << ", secondary " << ts->has_secondary << ", dps " << ts->dps() << std::endl;
	check(as->ranged and as->projectile.speed > 1.0 and as->projectile.accuracy > 0.0 and as->projectile.dispersion > 0.0,
	      "archer projectile: speed, accuracy, dispersion from the data");
	check(not as->projectile.lead, "archer without ballistics: no lead");
	check(as->unit_class == 0 and vs->unit_class == 4 and ks->unit_class == 12, "unit classes archer 0, villager 4, knight 12");
	check(ts->garrison_capacity == 15 and ts->garrison_mask == 11, "town center: 15 places, mask 11 (data variant)");
	check(ts->min_projectiles == 1 and ts->max_projectiles >= 10, "town center: 1 base arrow, max from the data");
	check(ts->can_attack and ts->shoots_arrows(), "town center shoots");
	check(std::abs(ts->garrison_heal - 0.1) < 1e-6, "town center heals 0.1 HP/s");
	check(ks->stances and ks->formations and ks->military(), "knight: stances and formations");
	check(not vs->military() and not vs->stances, "villager: no stances");

	// ---- projectiles: flight time before the damage
	std::cout << "W4.1 projectiles" << std::endl;
	{
		auto shooter = archers0[0];
		auto victim = knights1[0];
		auto spot = w.free_spot(w.position(victim).ne.to_double() - 3.0, w.position(victim).se.to_double());
		if (spot) {
			w.teleport(shooter, spot->first, spot->second);
		}
		combat->set_stance(victim, combat::stance_t::NO_ATTACK);
		auto before = combat->projectile_counts();
		auto health = w.health(victim);
		w.order(command_t::ATTACK, {shooter}, w.position(victim), victim);
		bool launched = w.advance_until(6.0, [&] { return combat->projectile_counts().launched > before.launched; });
		check(launched, "the archer launches a projectile");
		uint64_t version = 0;
		auto flying = combat->get_projectiles(version);
		check(not flying.empty() and flying.front().flight.duration > 0.2, "projectile in flight (snapshot)");
		check(w.health(victim) == health, "no damage at the launch");
		bool hit = w.advance_until(3.0, [&] {
			auto c = combat->projectile_counts();
			return c.hits + c.misses > before.hits + before.misses;
		});
		check(hit, "impact after the flight time");
		w.advance_until(20.0, [&] { return w.health(victim) < health; });
		check(w.health(victim) < health, "the knight loses health from arrows");
		w.order(command_t::STOP, {shooter}, w.position(shooter));
		auto counts = combat->projectile_counts();
		std::cout << "  projectiles: " << counts.launched << " launched, " << counts.hits << " hits, " << counts.misses
		          << " misses (" << counts.miss_hits << " hit something else)" << std::endl;
		check(counts.launched >= counts.hits + counts.misses, "every impact belongs to a launch");
	}

	// ---- stances
	std::cout << "W4.2 stances" << std::endl;
	{
		auto unit = militia0[0];
		auto enemy = militia1[0];
		combat->set_stance(enemy, combat::stance_t::NO_ATTACK);
		auto upos = w.position(unit);
		auto spot = w.free_spot(upos.ne.to_double() + 3.0, upos.se.to_double());
		check(spot.has_value(), "free spot next to the militia");
		if (spot) {
			// no attack: never
			w.order(command_t::STANCE, {unit}, upos, std::nullopt, static_cast<int>(combat::stance_t::NO_ATTACK));
			check(combat->get_stance(unit) == combat::stance_t::NO_ATTACK, "stance command: no attack");
			w.teleport(enemy, spot->first, spot->second);
			w.advance(3.0);
			check(not combat->get_target(unit), "no attack: an enemy next to it is not attacked");
			// stand ground: only in range, no chasing
			w.order(command_t::STANCE, {unit}, upos, std::nullopt, static_cast<int>(combat::stance_t::STAND_GROUND));
			w.advance(3.0);
			check(not combat->get_target(unit) and dist(w.position(unit), upos) < 0.6,
			      "stand ground: an enemy 3 tiles away is not chased");
			// defensive: within R + 5 of the post -> attacks
			w.order(command_t::STANCE, {unit}, upos, std::nullopt, static_cast<int>(combat::stance_t::DEFENSIVE));
			bool engaged = w.advance_until(4.0, [&] { return combat->get_target(unit).has_value(); });
			check(engaged, "defensive: the enemy within R + 5 is attacked");
			// the enemy is taken far away: the unit comes back to its post
			auto far = w.free_spot(upos.ne.to_double() + 12.0, upos.se.to_double() + 6.0);
			if (far) {
				w.teleport(enemy, far->first, far->second);
			}
			bool back = w.advance_until(25.0, [&] {
				return not combat->get_target(unit) and not w.moving(unit) and dist(w.position(unit), upos) < 1.5;
			});
			check(back, "defensive: back to the post when the enemy leaves");
			w.order(command_t::STANCE, {unit}, upos, std::nullopt, static_cast<int>(combat::stance_t::AGGRESSIVE));
			combat->kill(w.state, enemy, w.now);
			w.advance(1.0);
		}
	}

	// ---- orders: patrol, attack move, guard, follow, stop
	std::cout << "W4.3 orders" << std::endl;
	{
		auto unit = militia0[1];
		auto start = w.position(unit);
		auto end = w.free_spot(start.ne.to_double() - 5.0, start.se.to_double() + 1.0);
		if (end) {
			w.order(command_t::PATROL, {unit}, at(end->first, end->second));
			check(combat->get_order(unit) == combat::order_t::PATROL, "patrol order");
			double near_end = 1e9, near_start_after = 1e9;
			bool reached_end = false;
			for (int i = 0; i < 400; ++i) {
				w.advance(0.1);
				const double de = dist(w.position(unit), at(end->first, end->second));
				near_end = std::min(near_end, de);
				if (de < 1.0) {
					reached_end = true;
				}
				if (reached_end) {
					near_start_after = std::min(near_start_after, dist(w.position(unit), start));
				}
			}
			check(near_end < 1.0 and near_start_after < 1.0, "patrol: reaches the end and walks back");
			w.order(command_t::STOP, {unit}, start);
			check(combat->get_order(unit) == combat::order_t::NONE and not w.moving(unit), "stop: no order, stands");
		}
		// guard a villager: follows it
		auto guard = militia0[2];
		auto villager = villagers0[2];
		auto vpos = w.position(villager);
		w.teleport(guard, vpos.ne.to_double() + 1.0, vpos.se.to_double());
		w.order(command_t::GUARD, {guard}, vpos, villager);
		check(combat->get_order(guard) == combat::order_t::GUARD, "guard order");
		auto away = w.free_spot(vpos.ne.to_double() + 7.0, vpos.se.to_double() + 3.0);
		if (away) {
			w.command(command_t::MOVE, {villager}, at(away->first, away->second));
			w.advance(20.0);
			check(dist(w.position(guard), w.position(villager)) < 4.0, "guard: stays near the guarded villager");
		}
		w.order(command_t::FOLLOW, {guard}, w.position(villager), villager);
		check(combat->get_order(guard) == combat::order_t::FOLLOW, "follow order");
		w.order(command_t::STOP, {guard}, w.position(guard));
		// attack move: the knights walk towards the enemy army and fight on the way
		w.order(command_t::STANCE, knights0, w.position(knights0[0]), std::nullopt,
		        static_cast<int>(combat::stance_t::AGGRESSIVE));
		w.order(command_t::ATTACK_MOVE, knights0, w.position(knights1[2]));
		check(combat->get_order(knights0[0]) == combat::order_t::ATTACK_MOVE, "attack move order");
		bool fighting = w.advance_until(30.0, [&] {
			for (auto id : knights0) {
				if (w.exists(id) and combat->get_target(id)) {
					return true;
				}
			}
			return false;
		});
		check(fighting, "attack move: enemies on the way are attacked");
		w.advance(3.0);
		std::vector<entity_id_t> alive_knights;
		for (auto id : knights0) {
			if (w.exists(id) and not combat->is_dead(id)) {
				alive_knights.push_back(id);
			}
		}
		if (not alive_knights.empty()) {
			w.order(command_t::STOP, alive_knights, w.position(alive_knights[0]));
			w.order(command_t::STANCE, alive_knights, w.position(alive_knights[0]), std::nullopt,
			        static_cast<int>(combat::stance_t::NO_ATTACK));
			check(not combat->get_target(alive_knights[0]) and combat->get_order(alive_knights[0]) == combat::order_t::NONE,
			      "stop ends the attack move");
		}
	}

	// ---- garrison
	std::cout << "W4.4 garrison" << std::endl;
	auto tc = tc0[0];
	{
		std::vector<entity_id_t> in{villagers0[0], villagers0[1]};
		w.set_health(in[0], 10);
		const auto h0 = w.health(in[0]);
		w.order(command_t::GARRISON, in, w.position(tc), tc);
		bool inside = w.advance_until(40.0, [&] {
			return combat->garrisoned_in(in[0]) == tc and combat->garrisoned_in(in[1]) == tc;
		});
		check(inside, "two villagers walk into the town center");
		check(combat->is_hidden(in[0]) and combat->occupants(tc).size() == 2, "garrisoned: hidden, 2 occupants");
		check(combat->arrow_count_of(tc) == 3, "town center: 1 + 2 villagers = 3 arrows");
		auto info = combat->get_info(tc);
		check(info and info->capacity == 15 and info->occupants.size() == 2 and info->arrows == 3,
		      "HUD info: garrison 2/15, 3 arrows");
		w.order(command_t::MOVE, {in[0]}, w.position(tc));
		check(combat->garrisoned_in(in[0]) == tc, "garrisoned units take no commands");
		w.advance(30.0);
		const auto h1 = w.health(in[0]);
		std::cout << "  healing in 30 s: " << h0 << " -> " << h1 << std::endl;
		check(h1 >= h0 + 2 and h1 <= h0 + 4, "healing 0.1 HP/s in the town center");
		// an enemy comes close: the town center shoots its garrison arrows
		auto before = combat->projectile_counts();
		auto tpos = w.position(tc);
		auto near = w.free_spot(tpos.ne.to_double() + 4.0, tpos.se.to_double() + 1.0);
		auto target_knight = w.find(1, "Knight");
		if (near and not target_knight.empty()) {
			combat->set_stance(target_knight[0], combat::stance_t::NO_ATTACK);
			w.teleport(target_knight[0], near->first, near->second);
			bool volley = w.advance_until(6.0, [&] {
				return combat->projectile_counts().launched >= before.launched + 3;
			});
			check(volley, "town center with 2 villagers shoots 3 arrows per volley");
			check(combat->projectile_counts().extra_arrows > before.extra_arrows, "extra arrows on the spawning area");
		}
		else {
			check(false, "spot for an enemy next to the town center");
		}
		w.order(command_t::UNGARRISON, {tc}, tpos);
		check(combat->occupants(tc).empty() and not combat->garrisoned_in(in[0]) and not combat->is_hidden(in[0]),
		      "ungarrison: everybody out");
		const auto out = w.position(in[0]);
		const double gap = std::max(std::abs(out.ne.to_double() - tpos.ne.to_double()),
		                            std::abs(out.se.to_double() - tpos.se.to_double()));
		check(gap > ts->radius, "ungarrisoned villager stands outside the footprint");
		if (not target_knight.empty() and w.exists(target_knight[0])) {
			combat->kill(w.state, target_knight[0], w.now);
		}
		w.advance(2.0);
	}

	// ---- town bell
	std::cout << "W4.5 town bell" << std::endl;
	{
		auto vill = w.find(0, "Villager");
		w.order(command_t::BELL, {tc}, w.position(tc), std::nullopt, 1);
		check(combat->bell_rings(tc), "bell rings");
		bool all_in = w.advance_until(45.0, [&] {
			for (auto id : vill) {
				if (w.exists(id) and not combat->garrisoned_in(id)) {
					return false;
				}
			}
			return true;
		});
		check(all_in, "all villagers near the town center go inside");
		check(combat->occupants(tc).size() == vill.size(), "occupants = villagers");
		w.order(command_t::BELL, {tc}, w.position(tc), std::nullopt, 0);
		check(not combat->bell_rings(tc) and combat->occupants(tc).empty(), "back to work: the town center is empty");
		w.advance(2.0);
	}

	// ---- flight of villagers
	std::cout << "W4.6 flight of villagers" << std::endl;
	{
		std::vector<entity_id_t> v;
		for (auto id : w.find(0, "Villager")) {
			if (w.health(id) == combat->get_stats(id)->max_health) {
				v.push_back(id);
			}
		}
		auto enemy = w.find(1, "Militia");
		if (enemy.empty()) {
			enemy = w.find(1, "Knight");
		}
		if (not v.empty() and not enemy.empty()) {
			auto vpos = w.position(v[0]);
			auto spot = w.free_spot(vpos.ne.to_double() + 1.5, vpos.se.to_double());
			if (spot) {
				w.teleport(enemy[0], spot->first, spot->second);
			}
			w.order(command_t::ATTACK, {enemy[0]}, w.position(v[0]), v[0]);
			bool fled = w.advance_until(10.0, [&] { return combat->is_fleeing(v[0]); });
			check(fled, "an attacked villager flees");
			bool ran = w.advance_until(3.0, [&] { return w.exists(v[0]) and dist(w.position(v[0]), vpos) > 0.8; });
			std::cout << "  villager " << v[0] << " moved " << (w.exists(v[0]) ? dist(w.position(v[0]), vpos) : -1.0)
			          << " tiles, health " << (w.exists(v[0]) ? w.health(v[0]) : -1) << std::endl;
			check(ran, "the villager runs away");
			combat->kill(w.state, enemy[0], w.now);
			bool calm = w.advance_until(15.0, [&] { return not w.exists(v[0]) or not combat->is_fleeing(v[0]); });
			check(calm, "the villager calms down once the attacks stop");
		}
	}

	// ---- formations
	std::cout << "W4.7 formations" << std::endl;
	{
		std::vector<entity_id_t> army;
		for (auto id : w.find(0, "Knight")) {
			army.push_back(id);
		}
		for (auto id : w.find(0, "Archer")) {
			army.push_back(id);
		}
		if (army.size() >= 3) {
			auto first = w.position(army[0]);
			auto dest = w.free_spot(first.ne.to_double() - 4.0, first.se.to_double() - 4.0);
			w.order(command_t::FORMATION, army, first, std::nullopt, static_cast<int>(combat::formation_t::LINE));
			check(combat->get_formation(army[0]) == combat::formation_t::LINE, "formation command: line");
			if (dest) {
				std::vector<coord::phys3> starts;
				for (auto id : army) {
					starts.push_back(w.position(id));
				}
				w.command(command_t::MOVE, army, at(dest->first, dest->second));
				w.advance(2.0);
				double slowest = 1e9;
				for (auto id : army) {
					slowest = std::min(slowest, combat->get_stats(id)->speed);
				}
				bool within = true;
				for (size_t i = 0; i < army.size(); ++i) {
					within = within and dist(w.position(army[i]), starts[i]) <= slowest * 2.05 + 0.3;
				}
				check(within, "formation moves with the slowest member");
				w.advance(20.0);
				std::vector<coord::phys3> ends;
				for (auto id : army) {
					ends.push_back(w.position(id));
				}
				double min_gap = 1e9;
				for (size_t i = 0; i < ends.size(); ++i) {
					for (size_t j = i + 1; j < ends.size(); ++j) {
						min_gap = std::min(min_gap, dist(ends[i], ends[j]));
					}
				}
				std::cout << "  formation: " << army.size() << " units, smallest gap " << min_gap << std::endl;
				check(min_gap > 0.4, "formation: units stand apart in their slots");
			}
		}
	}

	// ---- watch tower shoots
	std::cout << "W4.8 watch tower" << std::endl;
	{
		// the converted data binds the tower to its own completion: the data's requirement instead
		const auto tower_fqon = modpack + ".data.game_entity.generic.tower.tower.Tower";
		auto view = w.state->get_player(0)->get_db_view();
		auto fixed = tech::fix_self_condition(view, tower_fqon, tech::Condition::built(tower_fqon));
		std::cout << "  tower condition: kind " << static_cast<int>(fixed.kind) << " " << fixed.ref << std::endl;
		check(fixed.kind == tech::Condition::kind_t::TECH and fixed.ref.find("FeudalAge") != std::string::npos,
		      "watch tower: unlocked by the Feudal Age (data requirement of its research)");
		auto other = tech::fix_self_condition(view, tower_fqon, tech::Condition::tech("x.CastleAge"));
		check(other.kind == tech::Condition::kind_t::TECH and other.ref == "x.CastleAge", "other conditions unchanged");
	}
	{
		auto enemies = w.find(1, "Archer");
		if (enemies.empty()) {
			enemies = w.find(1, "Militia");
		}
		if (not enemies.empty()) {
			auto epos = w.position(enemies[0]);
			auto spot = w.free_spot(epos.ne.to_double() + 5.0, epos.se.to_double() + 1.0);
			if (spot) {
				auto tower = w.spawn(0, "tower", "Tower", spot->first, spot->second);
				auto st = combat->get_stats(tower);
				check(st and st->building and st->shoots_arrows() and st->unit_class == 52, "watch tower: class 52, shoots");
				combat->set_stance(enemies[0], combat::stance_t::NO_ATTACK);
				auto before = combat->projectile_counts();
				bool shot = w.advance_until(8.0, [&] { return combat->projectile_counts().launched > before.launched; });
				check(shot, "the watch tower shoots at an enemy in range");
			}
		}
	}

	// ---- delete, save values
	std::cout << "W4.9 delete and save values" << std::endl;
	{
		auto m = w.find(0, "Militia");
		if (not m.empty()) {
			combat->set_stance(m[0], combat::stance_t::DEFENSIVE);
			auto saved = combat->save_entity(m[0]);
			check(saved and saved->stance == combat::stance_t::DEFENSIVE, "save: stance");
			w.order(command_t::KILL, {m[0]}, w.position(m[0]));
			check(combat->is_dead(m[0]) or not w.exists(m[0]), "delete: the unit dies");
		}
		auto v = w.find(0, "Villager");
		if (not v.empty()) {
			w.order(command_t::GARRISON, {v[0]}, w.position(tc), tc);
			w.advance_until(30.0, [&] { return combat->garrisoned_in(v[0]).has_value(); });
			auto saved = combat->save_entity(v[0]);
			check(saved and saved->inside == tc, "save: garrison building");
			w.order(command_t::UNGARRISON, {tc}, w.position(tc));
			if (saved) {
				combat->load_entity(w.state, v[0], *saved, w.now);
			}
			check(combat->garrisoned_in(v[0]) == tc, "load: back in the building at once");
		}
	}
}

} // namespace


int main(int argc, char **argv) {
	std::string root_dir;
	std::string modpack = "hd_base";
	uint32_t seed = 1;
	size_t size = 64;
	for (int i = 1; i + 1 < argc; i += 2) {
		std::string arg = argv[i];
		if (arg == "--root") {
			root_dir = argv[i + 1];
		}
		else if (arg == "--modpack") {
			modpack = argv[i + 1];
		}
		else if (arg == "--seed") {
			seed = static_cast<uint32_t>(std::stoul(argv[i + 1]));
		}
		else if (arg == "--size") {
			size = std::stoul(argv[i + 1]);
		}
	}
	if (root_dir.empty()) {
		std::cerr << "usage: " << argv[0] << " --root <dir> [--modpack hd_base] [--seed N] [--size N]\n";
		return EXIT_FAILURE;
	}
	log::set_level(log::level::info);

	try {
		util::Path root{std::make_shared<util::fslike::Directory>(root_dir), {}};
		World w;
		w.loop = std::make_shared<openage::event::EventLoop>();
		auto mod_manager = std::make_shared<assets::ModManager>(root / "assets" / "converted");
		for (const auto &mod : assets::ModManager::enumerate_modpacks(root / "assets" / "converted")) {
			mod_manager->register_modpack(mod);
		}
		mod_manager->activate_modpacks({"engine", modpack});
		auto entity_factory = std::make_shared<EntityFactory>();
		auto terrain_factory = std::make_shared<TerrainFactory>();
		// same handlers as GameSimulation::init_event_handlers
		w.loop->add_event_handler(std::make_shared<gamestate::event::DragSelectHandler>());
		w.loop->add_event_handler(std::make_shared<gamestate::event::SpawnEntityHandler>(w.loop, entity_factory));
		w.loop->add_event_handler(std::make_shared<gamestate::event::SendCommandHandler>());
		w.loop->add_event_handler(std::make_shared<gamestate::event::ProcessCommandHandler>());
		w.loop->add_event_handler(std::make_shared<gamestate::event::WaitHandler>());
		combat::add_event_handlers(w.loop);
		w.commander = std::make_shared<gamestate::event::Commander>(w.loop);

		MapSettings settings;
		settings.type = map_type_t::RANDOM;
		settings.seed = seed;
		settings.size = size;
		settings.skirmish = true;
		// vision (XR fork): this check commands units against enemies they may not see; the fog
		// rules have their own checks (openage-vision-check, 93-vision-check.sh)
		settings.vision = vision_mode_t::ALL;
		// ai (XR fork): this check commands both players itself
		settings.ai.mode = ai_mode_t::OFF;
		w.game = std::make_shared<Game>(w.loop, mod_manager, entity_factory, terrain_factory, settings);
		w.state = w.game->get_state();
		w.loop->reach_time(w.now, w.state);

		size_t decided = 0;
		w.combat()->on_match_over([&decided](const combat::MatchStatus &) { decided += 1; });

		// ---- 1. values from nyan
		std::cout << "1. combat values" << std::endl;
		auto knights0 = w.find(0, "Knight");
		auto knights1 = w.find(1, "Knight");
		auto militia0 = w.find(0, "Militia");
		auto archers1 = w.find(1, "Archer");
		auto tc1 = w.find(1, "TownCenter");
		check(knights0.size() == 3 and knights1.size() == 3 and militia0.size() == 3 and archers1.size() == 3,
		      "skirmish armies: 3 knights, militia, archers per player");
		check(tc1.size() == 1, "town center of player 1");
		if (knights0.empty() or knights1.empty() or militia0.empty() or archers1.empty() or tc1.empty()) {
			std::cout << "combat check FAILED (no skirmish)" << std::endl;
			return EXIT_FAILURE;
		}
		auto ks = w.combat()->get_stats(knights0[0]);
		auto ms = w.combat()->get_stats(militia0[0]);
		auto as = w.combat()->get_stats(archers1[0]);
		auto ts = w.combat()->get_stats(tc1[0]);
		std::cout << "  knight: health " << ks->max_health << ", attack " << combat::attack_sum(ks->attack)
		          << ", reload " << ks->reload_time << ", sight " << ks->line_of_sight << std::endl;
		std::cout << "  militia: health " << ms->max_health << ", attack " << combat::attack_sum(ms->attack)
		          << ", reload " << ms->reload_time << std::endl;
		std::cout << "  archer: health " << as->max_health << ", attack " << combat::attack_sum(as->attack)
		          << " ranged " << as->ranged << ", range " << as->max_range << ", reload " << as->reload_time << std::endl;
		std::cout << "  town center: health " << ts->max_health << ", shoots " << ts->can_attack
		          << ", radius " << ts->radius << std::endl;
		check(ks->max_health == 100 and combat::attack_sum(ks->attack) == 10 and ks->can_attack and not ks->ranged,
		      "knight 100 health, 10 melee attack");
		check(std::abs(ks->reload_time - 1.8) < 1e-6, "knight reload 1.8 s");
		check(as->ranged and as->max_range >= 4.0 and combat::attack_sum(as->attack) > 0, "archer ranged");
		check(combat::compute_damage(ks->attack, w.combat()->get_stats(knights1[0])->armor) == 8,
		      "knight vs knight: 10 - 2 = 8");
		check(ts->building and w.health(tc1[0]) == ts->max_health, "town center starts with full health");
		check(ts->radius >= 1.9, "town center footprint radius 2");
		
		// ---- 1b. villagers (economy activity) and gaia
		std::cout << "1b. villagers and gaia" << std::endl;
		auto villagers0 = w.find(0, "Villager");
		auto villagers1 = w.find(1, "Villager");
		check(villagers0.size() >= 2 and not villagers1.empty(), "villagers of both players");
		const player_id_t gaia = 2;
		std::optional<entity_id_t> resource;
		double best = 1e9;
		for (const auto &[id, entity] : w.state->get_game_entities()) {
			if (w.owner(id) == gaia and entity->has_component(component::component_t::HARVESTABLE)) {
				double d = dist(w.position(id), w.position(villagers0[0]));
				if (d < best) {
					best = d;
					resource = id;
				}
			}
		}
		check(resource.has_value(), "gaia owns the resources");
		if (resource and villagers0.size() >= 2 and not villagers1.empty()) {
			// right click on a resource: gather (no attack on gaia)
			w.command(component::command::command_t::MOVE, {villagers0[0]}, w.position(*resource));
			check(not w.combat()->get_target(villagers0[0]).has_value(), "right click on a resource: no attack");
			check(w.combat()->pick_enemy(w.state, w.now, 0, PickRequest{w.position(*resource)}) == nullptr,
			      "resources of gaia are no enemies");
			w.advance(5.0);
			// attack command for a villager (gather activity): walks over and hits
			auto victim = villagers1.front();
			auto victim_health = w.health(victim);
			{
				// test setup: the enemy villager stands 4 tiles from the attackers (not through the armies)
				auto pos = std::dynamic_pointer_cast<component::Position>(
					w.state->get_game_entity(victim)->get_component(component::component_t::POSITION));
				auto near = w.position(villagers0[1]);
				for (auto [dne, dse] : {std::pair{4.0, 0.0}, std::pair{0.0, 4.0}, std::pair{-4.0, 0.0}, std::pair{0.0, -4.0}}) {
					coord::phys3 spot{near.ne + coord::phys_t{dne}, near.se + coord::phys_t{dse}, coord::phys_t{0}};
					auto land = w.state->get_map()->get_grid_id(modpack + ".data.util.path_type.types.Land");
					if (w.state->get_map()->is_passable(land, spot.to_tile())) {
						pos->set_position(w.now, w.state->get_map()->on_terrain(spot));
						break;
					}
				}
			}
			openage::event::EventHandler::param_map::map_t params{
				{"type", component::command::command_t::ATTACK},
				{"target", w.position(victim)},
				{"entity_ids", std::vector<entity_id_t>{villagers0[0], villagers0[1]}},
				{"target_entity", victim},
			};
			w.loop->create_event("game.send_command", w.commander, w.state, w.now, params);
			w.advance(0.05);
			check(w.combat()->get_target(villagers0[0]) == victim, "villager attacks on command");
			bool hit = w.advance_until(120.0, [&] { return not w.exists(victim) or w.health(victim) < victim_health; });
			check(hit, "villager reaches the enemy villager and hits it");
		}
		auto match = w.combat()->get_match_status();
		check(not match.alive.contains(gaia), "gaia does not count for the victory");


		// ---- 2. auto attack
		std::cout << "2. auto attack" << std::endl;
		auto layout_center = coord::phys3{
			(w.position(knights0[1]).ne + w.position(knights1[1]).ne) / 2,
			(w.position(knights0[1]).se + w.position(knights1[1]).se) / 2,
			coord::phys_t{0}};
		auto runner = militia0[1];
		w.command(component::command::command_t::MOVE, {runner}, layout_center);
		check(not w.combat()->get_target(runner).has_value(), "move to the ground: no attack");
		bool engaged = w.advance_until(20.0, [&] {
			if (w.combat()->get_target(runner)) {
				return true;
			}
			for (auto id : knights1) {
				if (w.combat()->get_target(id) and *w.combat()->get_target(id) == runner) {
					return true;
				}
			}
			return false;
		});
		check(engaged, "enemies in sight attack automatically");
		bool runner_dead = w.advance_until(60.0, [&] { return not w.exists(runner); });
		check(runner_dead, "the militia between the armies dies and is removed");
		check(w.combat()->was_removed(runner), "removed id reported (selection)");

		// ---- 3. attack command (right click on an enemy, ground picking)
		std::cout << "3. attack command" << std::endl;
		std::vector<entity_id_t> targets;
		for (auto id : w.find(1, "Knight")) {
			targets.push_back(id);
		}
		for (auto id : w.find(1, "Militia")) {
			targets.push_back(id);
		}
		check(not targets.empty(), "enemy units left");
		auto target = targets.front();
		auto target_health = w.health(target);
		auto t0 = w.now;
		w.command(component::command::command_t::MOVE, knights0, w.position(target));
		size_t ordered = 0;
		for (auto id : knights0) {
			auto t = w.combat()->get_target(id);
			ordered += (t and *t == target) ? 1 : 0;
		}
		check(ordered == knights0.size(), "right click on the enemy: all knights attack it");
		bool hurt = w.advance_until(30.0, [&] { return not w.exists(target) or w.health(target) < target_health; });
		check(hurt, "the target loses health");
		bool gone = w.advance_until(60.0, [&] { return not w.exists(target); });
		std::cout << "  target removed after " << (w.now - t0).to_double() << " s" << std::endl;
		check(gone, "the target dies and is removed from the game state");
		// commands with the dead id are ignored (no exception)
		w.command(component::command::command_t::MOVE, {target}, layout_center);
		check(true, "command for a removed entity ignored");

		// ---- 4. HUD queries
		std::cout << "4. HUD queries" << std::endl;
		auto alive_knights = w.find(0, "Knight");
		std::vector<entity_id_t> selection = alive_knights;
		selection.push_back(target);
		auto health = w.combat()->get_health(selection);
		bool consistent = health.size() == selection.size();
		for (size_t i = 0; i < alive_knights.size() and consistent; ++i) {
			consistent = health[i].alive and health[i].health == w.health(alive_knights[i])
			             and health[i].max_health == 100;
		}
		check(consistent, "health of the selection matches the Live component");
		check(not health.back().alive, "removed target is not alive");
		check(w.combat()->get_match_state(0) == combat::match_state_t::RUNNING, "match still running");

		// let the battle go on for a while (auto attacks of both armies)
		w.advance(20.0);

		// ---- 5. victory
		std::cout << "5. victory" << std::endl;
		auto land = w.state->get_map()->get_grid_id(modpack + ".data.util.path_type.types.Land");
		auto tc_pos = w.position(tc1[0]);
		auto tile_cost = [&]() {
			auto grid = w.state->get_map()->get_pathfinder()->get_grid(land);
			auto side = static_cast<long>(grid->get_sector_size());
			auto ne = static_cast<long>(std::floor(tc_pos.ne.to_double() - 0.5));
			auto se = static_cast<long>(std::floor(tc_pos.se.to_double() - 0.5));
			auto sector = grid->get_sector(static_cast<size_t>(ne / side), static_cast<size_t>(se / side));
			return sector->get_cost_field()->get_cost(static_cast<size_t>(ne % side),
			                                         static_cast<size_t>(se % side));
		};
		check(tile_cost() == path::COST_IMPASSABLE, "town center tiles blocked");
		std::vector<entity_id_t> rest;
		for (const auto &[id, entity] : w.state->get_game_entities()) {
			auto stats = w.combat()->get_stats(id);
			if (stats and stats->counts_for_victory() and w.owner(id) == 1 and not w.combat()->is_dead(id)) {
				rest.push_back(id);
			}
		}
		std::sort(rest.begin(), rest.end());
		std::cout << "  player 1 has " << rest.size() << " units and buildings left" << std::endl;
		// the town center last
		std::erase(rest, tc1[0]);
		for (auto id : rest) {
			w.combat()->kill(w.state, id, w.now);
		}
		w.advance(2.0);
		check(w.combat()->get_match_state(0) == combat::match_state_t::RUNNING and decided == 0,
		      "town center left: not decided");
		w.combat()->kill(w.state, tc1[0], w.now);
		check(decided == 1, "match decided once (callback)");
		w.advance(3.0);
		auto status = w.combat()->get_match_status();
		check(status.result.over and status.result.winner and *status.result.winner == 0, "player 0 wins");
		check(w.combat()->get_match_state(0) == combat::match_state_t::VICTORY, "state player 0: victory");
		check(w.combat()->get_match_state(1) == combat::match_state_t::DEFEAT, "state player 1: defeat");
		check(not w.exists(tc1[0]), "town center removed");
		check(tile_cost() != path::COST_IMPASSABLE, "town center tiles passable again");
		w.advance(3.0);
		check(decided == 1, "decided only once");

		// ---- round W4 (XR fork): own game
		w4_checks(root, modpack, seed, size);
	}
	catch (std::exception &err) {
		std::cout << "combat check FAILED: " << err.what() << std::endl;
		return EXIT_FAILURE;
	}

	std::cout << (failures == 0 ? "combat check ok" : "combat check FAILED") << std::endl;
	return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
