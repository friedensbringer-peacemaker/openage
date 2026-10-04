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
#include <sstream>
#include <string>
#include <vector>

#include "assets/mod_manager.h"
#include "event/event_loop.h"
#include "gamestate/combat/combat_state.h"
#include "gamestate/combat/events.h"
#include "gamestate/component/api/live.h"
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
#include "gamestate/map.h"
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
	}
	catch (std::exception &err) {
		std::cout << "combat check FAILED: " << err.what() << std::endl;
		return EXIT_FAILURE;
	}

	std::cout << (failures == 0 ? "combat check ok" : "combat check FAILED") << std::endl;
	return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
