// Copyright 2026-2026 the openage authors. See copying.md for legal info.

/*
 * Game state check of the computer opponent (XR fork), headless, with the
 * converted modpack but without renderer:
 *
 *   openage-ai-check --root <dir> [--modpack hd_base] [--seed N] [--size N] [--limit S]
 *
 * Random map with --map-skirmish armies; simulated time advances in steps of
 * 50 ms through the event loop as fast as possible (like GameSimulation::run).
 * The AI controls player 1, player 0 is the human.
 *
 *  A. passive human (easy AI): the army of player 0 is removed at t=0, its
 *     villagers stand still. The AI villagers gather (resources of player 1
 *     rise, player 0 unchanged), no attack before 8 min, then an attack wave
 *     on the town center; the match ends within --limit (default 1800 s
 *     simulation time) with victory for player 1.
 *  B. determinism: same seed again up to 150 s -> same resources, orders and
 *     unit positions as A.
 *  C. AI off (MapSettings::ai.mode = OFF): resources of player 1 unchanged,
 *     no AI.
 *  D. defence (normal AI): the knights of player 0 walk to the AI town center
 *     at t=20 s -> the AI defends; then the match runs to its end (result
 *     reported, any result accepted).
 *
 * Prints "ai check ok" and exits with 0 if everything passes.
 */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "assets/mod_manager.h"
#include "event/event_loop.h"
#include "gamestate/ai/ai_player.h"
#include "gamestate/combat/combat_state.h"
#include "gamestate/combat/events.h"
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
#include "gamestate/player.h"
#include "gamestate/terrain_factory.h"
#include "log/log.h"
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

struct Options {
	util::Path root;
	std::string modpack = "hd_base";
	uint32_t seed = 1;
	size_t size = 64;
	double limit = 1800.0;
};

struct World {
	std::shared_ptr<openage::event::EventLoop> loop;
	std::shared_ptr<Game> game;
	std::shared_ptr<GameState> state;
	std::shared_ptr<gamestate::event::Commander> commander;
	time::time_t now = time::TIME_ZERO;

	World(const Options &opt, const AiSettings &ai) {
		this->loop = std::make_shared<openage::event::EventLoop>();
		auto mod_manager = std::make_shared<assets::ModManager>(opt.root / "assets" / "converted");
		for (const auto &mod : assets::ModManager::enumerate_modpacks(opt.root / "assets" / "converted")) {
			mod_manager->register_modpack(mod);
		}
		mod_manager->activate_modpacks({"engine", opt.modpack});
		auto entity_factory = std::make_shared<EntityFactory>();
		auto terrain_factory = std::make_shared<TerrainFactory>();
		// same handlers as GameSimulation::init_event_handlers (the AI registers its own)
		this->loop->add_event_handler(std::make_shared<gamestate::event::DragSelectHandler>());
		this->loop->add_event_handler(std::make_shared<gamestate::event::SpawnEntityHandler>(this->loop, entity_factory));
		this->loop->add_event_handler(std::make_shared<gamestate::event::SendCommandHandler>());
		this->loop->add_event_handler(std::make_shared<gamestate::event::ProcessCommandHandler>());
		this->loop->add_event_handler(std::make_shared<gamestate::event::WaitHandler>());
		combat::add_event_handlers(this->loop);
		this->commander = std::make_shared<gamestate::event::Commander>(this->loop);

		MapSettings settings;
		settings.type = map_type_t::RANDOM;
		settings.seed = opt.seed;
		settings.size = opt.size;
		settings.skirmish = true;
		settings.ai = ai;
		this->game = std::make_shared<Game>(this->loop, mod_manager, entity_factory, terrain_factory, settings);
		this->state = this->game->get_state();
		this->loop->reach_time(this->now, this->state);
	}

	void advance(double seconds) {
		auto until = this->now + time::time_t::from_double(seconds);
		while (this->now < until) {
			this->now = std::min(until, this->now + time::time_t::from_double(0.05));
			this->loop->reach_time(this->now, this->state);
		}
	}

	/// advance until done() or the absolute simulation time \p limit
	template <typename F>
	bool advance_until(double limit, F done) {
		while (this->now.to_double() < limit) {
			if (done()) {
				return true;
			}
			this->advance(0.5);
		}
		return done();
	}

	std::shared_ptr<combat::CombatState> combat() const {
		return this->state->get_combat();
	}

	std::shared_ptr<ai::AiPlayer> ai() const {
		const auto &players = this->game->get_ai_players();
		return players.empty() ? nullptr : players.front();
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

	resource_amounts_t resources(player_id_t player) const {
		return this->game->get_player_resources(player).value_or(resource_amounts_t{});
	}

	/// living entities of a player; military: units that attack and are no villagers
	std::vector<entity_id_t> find(player_id_t player, bool military_only) const {
		std::vector<entity_id_t> result;
		for (const auto &[id, entity] : this->state->get_game_entities()) {
			auto stats = this->combat()->get_stats(id);
			if (not stats or not stats->counts_for_victory() or this->combat()->is_dead(id)
			    or this->owner(id) != player) {
				continue;
			}
			if (military_only and not (stats->can_attack and stats->movable and not stats->villager)) {
				continue;
			}
			result.push_back(id);
		}
		std::sort(result.begin(), result.end());
		return result;
	}

	std::optional<entity_id_t> town_center(player_id_t player) const {
		for (auto id : this->find(player, false)) {
			if (this->combat()->get_stats(id)->name == "TownCenter") {
				return id;
			}
		}
		return std::nullopt;
	}

	/// state fingerprint: resources, AI counters, positions of all units
	std::string fingerprint() const {
		std::ostringstream out;
		out.precision(17);
		for (auto r : this->resources(1)) {
			out << r << " ";
		}
		auto s = this->ai()->get_status();
		out << "| " << s.gather_orders << " " << s.attack_orders << " " << s.thinks << " |";
		std::vector<entity_id_t> ids;
		for (const auto &[id, entity] : this->state->get_game_entities()) {
			ids.push_back(id);
		}
		std::sort(ids.begin(), ids.end());
		double sum = 0.0;
		for (auto id : ids) {
			auto p = this->position(id);
			sum += (p.ne.to_double() * 1.7 + p.se.to_double() * 3.1) * static_cast<double>(id % 97 + 1);
		}
		out << " " << ids.size() << " " << sum;
		return out.str();
	}
};

double total(const resource_amounts_t &r) {
	return r[0] + r[1] + r[2] + r[3];
}

std::string amounts(const resource_amounts_t &r) {
	std::ostringstream out;
	out << "food " << std::lround(r[0]) << ", wood " << std::lround(r[1]) << ", gold " << std::lround(r[2])
	    << ", stone " << std::lround(r[3]);
	return out.str();
}

} // namespace


int main(int argc, char **argv) {
	std::string root_dir;
	Options opt;
	for (int i = 1; i + 1 < argc; i += 2) {
		std::string arg = argv[i];
		if (arg == "--root") {
			root_dir = argv[i + 1];
		}
		else if (arg == "--modpack") {
			opt.modpack = argv[i + 1];
		}
		else if (arg == "--seed") {
			opt.seed = static_cast<uint32_t>(std::stoul(argv[i + 1]));
		}
		else if (arg == "--size") {
			opt.size = std::stoul(argv[i + 1]);
		}
		else if (arg == "--limit") {
			opt.limit = std::stod(argv[i + 1]);
		}
	}
	if (root_dir.empty()) {
		std::cerr << "usage: " << argv[0] << " --root <dir> [--modpack hd_base] [--seed N] [--size N] [--limit S]\n";
		return EXIT_FAILURE;
	}
	log::set_level(log::level::info);
	opt.root = util::Path{std::make_shared<util::fslike::Directory>(root_dir), {}};
	using clock = std::chrono::steady_clock;

	try {
		std::string fingerprint_a;
		// ---- A. passive human, easy AI
		{
			std::cout << "A. passive human, easy AI (seed " << opt.seed << ")" << std::endl;
			auto t0 = clock::now();
			World w{opt, AiSettings{}};
			auto ai = w.ai();
			check(ai != nullptr and ai->get_player() == 1, "AI on by default for two players, controls player 1");
			if (not ai) {
				std::cout << "ai check FAILED (no AI)" << std::endl;
				return EXIT_FAILURE;
			}
			const auto &p = ai->get_params();
			std::cout << "  params: " << ai::to_string(p.difficulty) << ", reaction " << p.think_period
			          << " s, threshold " << p.attack_threshold << ", earliest " << p.first_attack_earliest
			          << " s, any army after " << p.attack_anyway_after << " s" << std::endl;
			// passive human: no army
			auto army0 = w.find(0, true);
			for (auto id : army0) {
				w.combat()->kill(w.state, id, w.now);
			}
			std::cout << "  player 0 army removed: " << army0.size() << " units, left "
			          << w.find(0, false).size() << " (villagers + town center)" << std::endl;
			for (player_id_t pl : {0, 1}) {
				auto tc = w.town_center(pl);
				if (tc) {
					auto pos = w.position(*tc);
					std::cout << "  tc" << pl << " " << pos.ne.to_double() << "," << pos.se.to_double() << std::endl;
				}
			}
			auto start1 = w.resources(1);
			auto start0 = w.resources(0);
			auto army1 = w.find(1, true);
			check(army1.size() >= p.attack_threshold, "AI army reaches the threshold from the start");

			w.advance(120.0);
			auto r1 = w.resources(1);
			auto r0 = w.resources(0);
			std::cout << "  t=120 s player 1: " << amounts(r1) << " (start " << amounts(start1) << ")" << std::endl;
			size_t rising = 0;
			for (size_t k = 0; k < 3; ++k) {
				rising += r1[k] > start1[k] ? 1 : 0;
			}
			auto st = ai->get_status();
			check(st.gather_orders > 0, "AI orders idle villagers to gather (" + std::to_string(st.gather_orders) + ")");
			check(total(r1) > total(start1) and rising >= 2, "AI resources rise (" + std::to_string(rising) + " kinds)");
			check(r0 == start0, "passive player 0 resources unchanged");
			check(st.production_requests > 0 and st.production_done == 0,
			      "production wishes logged, no effect (stub)");

			w.advance(30.0);
			fingerprint_a = w.fingerprint();

			bool early = w.advance_until(p.first_attack_earliest - 2.0, [&] { return ai->get_status().waves > 0; });
			check(not early, "no attack before " + std::to_string(static_cast<int>(p.first_attack_earliest)) + " s");
			bool attacked = w.advance_until(p.first_attack_earliest + 60.0, [&] { return ai->get_status().waves > 0; });
			st = ai->get_status();
			check(attacked and st.first_attack and *st.first_attack >= p.first_attack_earliest,
			      "attack wave after the earliest time (t=" + std::to_string(st.first_attack.value_or(-1.0)) + " s)");
			auto tc0 = w.town_center(0);
			bool tc_targeted = false;
			for (auto id : w.find(1, true)) {
				auto t = w.combat()->get_target(id);
				tc_targeted = tc_targeted or (t and tc0 and *t == *tc0);
			}
			check(tc_targeted, "wave attacks the town center of player 0");

			bool over = w.advance_until(opt.limit, [&] { return w.combat()->get_match_status().result.over; });
			auto match = w.combat()->get_match_status();
			st = ai->get_status();
			std::cout << "  match " << (over ? "over" : "running") << " at t=" << w.now.to_double()
			          << " s (decided at " << match.decided_at.to_double() << " s), waves " << st.waves
			          << ", attack orders " << st.attack_orders << ", gather orders " << st.gather_orders
			          << ", AI " << amounts(w.resources(1)) << std::endl;
			check(over, "match ends within " + std::to_string(static_cast<int>(opt.limit)) + " s");
			check(match.result.winner and *match.result.winner == 1, "AI (player 1) wins against the passive player");
			check(w.combat()->get_match_state(1) == combat::match_state_t::VICTORY, "state player 1: victory");
			w.advance(5.0);
			check(not ai->get_status().running and ai->get_status().phase == "over", "AI stops after the match");
			std::cout << "  game A: " << std::chrono::duration<double>(clock::now() - t0).count() << " s" << std::endl;
		}

		// ---- B. determinism
		{
			std::cout << "B. determinism" << std::endl;
			World w{opt, AiSettings{}};
			for (auto id : w.find(0, true)) {
				w.combat()->kill(w.state, id, w.now);
			}
			w.advance(150.0);
			auto fb = w.fingerprint();
			std::cout << "  A: " << fingerprint_a << "\n  B: " << fb << std::endl;
			check(fb == fingerprint_a, "same seed, same game after 150 s");
		}

		// ---- C. AI off
		{
			std::cout << "C. AI off" << std::endl;
			AiSettings off;
			off.mode = ai_mode_t::OFF;
			World w{opt, off};
			check(w.game->get_ai_players().empty(), "no AI with mode OFF");
			auto start1 = w.resources(1);
			w.advance(60.0);
			check(w.resources(1) == start1, "player 1 idle without AI");
		}

		// ---- D. defence, normal AI
		{
			std::cout << "D. defence, normal AI" << std::endl;
			auto t0 = clock::now();
			AiSettings normal;
			normal.difficulty = ai_difficulty_t::NORMAL;
			World w{opt, normal};
			auto ai = w.ai();
			check(ai and ai->get_params().difficulty == ai::difficulty_t::NORMAL, "normal difficulty");
			w.advance(20.0);
			auto tc1 = w.town_center(1);
			std::vector<entity_id_t> raiders;
			for (auto id : w.find(0, true)) {
				if (w.combat()->get_stats(id)->name == "Knight") {
					raiders.push_back(id);
				}
			}
			check(tc1.has_value() and not raiders.empty(), "knights of player 0 and AI town center");
			if (tc1 and not raiders.empty()) {
				auto target = w.position(*tc1);
				target.ne += coord::phys_t{3.0};
				openage::event::EventHandler::param_map::map_t params{
					{"type", component::command::command_t::MOVE},
					{"target", target},
					{"entity_ids", raiders},
				};
				w.loop->create_event("game.send_command", w.commander, w.state, w.now, params);
				bool defended = w.advance_until(w.now.to_double() + 90.0, [&] { return ai->get_status().defenses > 0; });
				check(defended, "AI defends against knights near its base (t=" + std::to_string(w.now.to_double()) + " s)");
			}
			bool over = w.advance_until(opt.limit, [&] { return w.combat()->get_match_status().result.over; });
			auto match = w.combat()->get_match_status();
			auto st = ai->get_status();
			std::cout << "  match " << (over ? "over" : "running") << " at t=" << w.now.to_double() << " s, winner "
			          << (match.result.winner ? std::to_string(*match.result.winner) : std::string{"-"})
			          << ", waves " << st.waves << ", first attack "
			          << (st.first_attack ? std::to_string(*st.first_attack) : std::string{"-"}) << " s, defences "
			          << st.defenses << std::endl;
			check(st.waves == 0 or (st.first_attack and *st.first_attack >= ai->get_params().first_attack_earliest),
			      "normal: no wave before its earliest time");
			std::cout << "  game D: " << std::chrono::duration<double>(clock::now() - t0).count() << " s" << std::endl;
		}
	}
	catch (std::exception &err) {
		std::cout << "ai check FAILED: " << err.what() << std::endl;
		return EXIT_FAILURE;
	}

	std::cout << (failures == 0 ? "ai check ok" : "ai check FAILED") << std::endl;
	return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
