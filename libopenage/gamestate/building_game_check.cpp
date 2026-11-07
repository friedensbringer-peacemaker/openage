// Copyright 2026-2026 the openage authors. See copying.md for legal info.

/*
 * Game state check of the buildings (XR fork W5, docs/aoe2-spec 04), headless, with the
 * converted modpack but without renderer:
 *
 *   openage-building-game-check --root <dir> [--modpack hd_base] [--seed N] [--size N] [--only part]
 *
 * Random maps (64 tiles), simulated time advances in steps of 50 ms through the event loop with
 * the production of the simulation (like GameSimulation::run).
 *
 *  B1. Several builders: three villagers build a house 1.66 times as fast as one (f(n) of spec
 *      04 §4.3), build speed k = 2 doubles it; after a house the builders look for a foundation
 *      nearby (L33), the builder of a lumber camp cuts wood next to it.
 *  B2. Palisade drag: placement mode, press, cursor, release – the preview shows every segment of
 *      the tile line (red on the town centre), only valid segments become foundations, each costs
 *      the wood of the data, the villager builds them one after the other.
 *  B3. Gate: on a straight own stone wall (replaces 4 segments), built; own villagers walk through,
 *      an enemy stops in front of it, locked (command) it blocks everyone, unlocked it opens again.
 *  B4. Repair: command on a damaged house raises its HP and costs wood (half the build cost for a
 *      full repair), two repairers are faster (f(n) with the half factor); without wood the repair
 *      stops with a message.
 *  B5. Garrison ejection below 20 % HP; castle and bombard tower buildable with the data, with
 *      garrison places and arrows; placement rule 3 (unexplored ground is rejected).
 *  B6. Save game version 6: a locked gate and the wall drag of a builder; save -> load -> save
 *      keeps them, a version 5 file still loads.
 *  B7. Computer opponent: palisades around its town centre in the Feudal Age, villagers repair a
 *      damaged building.
 *
 * Prints "building game check ok" and exits with 0 if everything passes.
 */

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "assets/mod_manager.h"
#include "event/event_loop.h"
#include "gamestate/ai/ai_player.h"
#include "gamestate/building_math.h"
#include "gamestate/catalog.h"
#include "gamestate/buildings.h"
#include "gamestate/combat/combat_state.h"
#include "gamestate/combat/events.h"
#include "gamestate/component/api/builder.h"
#include "gamestate/component/api/constructable.h"
#include "gamestate/component/api/gather.h"
#include "gamestate/component/api/harvestable.h"
#include "gamestate/component/api/live.h"
#include "gamestate/component/internal/activity.h"
#include "gamestate/component/internal/command_queue.h"
#include "gamestate/component/internal/commands/build.h"
#include "gamestate/component/internal/commands/idle.h"
#include "gamestate/component/internal/commands/move.h"
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
#include "gamestate/production.h"
#include "gamestate/save_format.h"
#include "gamestate/save_game.h"
#include "gamestate/tech.h"
#include "gamestate/terrain_factory.h"
#include "gamestate/vision/vision_state.h"
#include "log/log.h"
#include "util/fslike/directory.h"
#include "util/path.h"

using namespace openage;
using namespace openage::gamestate;

namespace {

int failures = 0;
int passed = 0;

void check(bool ok, const std::string &what) {
	std::cout << (ok ? "  ok   " : "  FAIL ") << what << std::endl;
	if (ok) {
		passed += 1;
	}
	else {
		failures += 1;
	}
}

std::string fmt(double v) {
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%.2f", v);
	return buf;
}

struct Options {
	util::Path root;
	std::string modpack = "hd_base";
	uint32_t seed = 1;
	size_t size = 64;
};

const std::string GE = "hd_base.data.game_entity.generic.";

std::string ge(const std::string &dir, const std::string &name) {
	return GE + dir + "." + dir + "." + name;
}

struct World {
	std::shared_ptr<openage::event::EventLoop> loop;
	std::shared_ptr<Game> game;
	std::shared_ptr<GameState> state;
	std::shared_ptr<EntityFactory> factory;
	std::shared_ptr<prod::Production> production;
	std::shared_ptr<gamestate::event::Commander> commander;
	MapSettings settings;
	time::time_t now = time::TIME_ZERO;

	World(const Options &opt, const MapSettings &base) {
		this->loop = std::make_shared<openage::event::EventLoop>();
		auto mod_manager = std::make_shared<assets::ModManager>(opt.root / "assets" / "converted");
		for (const auto &mod : assets::ModManager::enumerate_modpacks(opt.root / "assets" / "converted")) {
			mod_manager->register_modpack(mod);
		}
		mod_manager->activate_modpacks({"engine", opt.modpack});
		this->factory = std::make_shared<EntityFactory>();
		auto terrain_factory = std::make_shared<TerrainFactory>();
		this->loop->add_event_handler(std::make_shared<gamestate::event::DragSelectHandler>());
		this->loop->add_event_handler(std::make_shared<gamestate::event::SpawnEntityHandler>(this->loop, this->factory));
		this->loop->add_event_handler(std::make_shared<gamestate::event::SendCommandHandler>());
		this->loop->add_event_handler(std::make_shared<gamestate::event::ProcessCommandHandler>());
		this->loop->add_event_handler(std::make_shared<gamestate::event::WaitHandler>());
		combat::add_event_handlers(this->loop);
		this->commander = std::make_shared<gamestate::event::Commander>(this->loop);

		this->settings = base;
		this->settings.type = map_type_t::RANDOM;
		this->settings.seed = opt.seed;
		this->settings.size = opt.size;
		this->game = std::make_shared<Game>(this->loop, mod_manager, this->factory, terrain_factory, this->settings);
		this->state = this->game->get_state();
		this->production = std::make_shared<prod::Production>();
		this->game->connect_ai_production(this->production);
		this->production->set_player(0);
		this->loop->reach_time(this->now, this->state);
		this->production->update(this->state, this->loop, this->factory, this->now);
	}

	void advance(double seconds) {
		auto until = this->now + time::time_t::from_double(seconds);
		while (this->now < until) {
			this->now = std::min(until, this->now + time::time_t::from_double(0.05));
			this->loop->reach_time(this->now, this->state);
			this->production->update(this->state, this->loop, this->factory, this->now);
		}
	}

	template <typename F>
	bool advance_until(double limit, F &&done) {
		const double end = this->now.to_double() + limit;
		while (this->now.to_double() < end) {
			if (done()) {
				return true;
			}
			this->advance(0.25);
		}
		return done();
	}

	std::shared_ptr<building::BuildingState> buildings() const {
		return this->state->get_buildings();
	}

	player_id_t owner(const std::shared_ptr<GameEntity> &e) const {
		return std::dynamic_pointer_cast<component::Ownership>(e->get_component(component::component_t::OWNERSHIP))
		    ->get_owners()
		    .get(this->now);
	}

	coord::phys3 position(const std::shared_ptr<GameEntity> &e) const {
		return std::dynamic_pointer_cast<component::Position>(e->get_component(component::component_t::POSITION))
		    ->get_positions()
		    .get(this->now);
	}

	bool exists(const std::shared_ptr<GameEntity> &e) const {
		return e != nullptr and this->state->get_game_entities().contains(e->get_id())
		       and not this->state->get_combat()->is_dead(e->get_id());
	}

	std::vector<std::shared_ptr<GameEntity>> named(const std::string &name) const {
		std::vector<std::shared_ptr<GameEntity>> out;
		for (const auto &[id, e] : this->state->get_game_entities()) {
			if (prod::entity_name(e) == name and not this->state->get_combat()->is_dead(id)) {
				out.push_back(e);
			}
		}
		std::sort(out.begin(), out.end(), [](const auto &a, const auto &b) { return a->get_id() < b->get_id(); });
		return out;
	}

	std::vector<std::shared_ptr<GameEntity>> of(player_id_t player, const std::string &name) const {
		std::vector<std::shared_ptr<GameEntity>> out;
		for (const auto &e : this->named(name)) {
			if (this->owner(e) == player) {
				out.push_back(e);
			}
		}
		return out;
	}

	std::shared_ptr<GameEntity> town_center(player_id_t player) const {
		auto tcs = this->of(player, "TownCenter");
		return tcs.empty() ? nullptr : tcs.front();
	}

	std::shared_ptr<component::Builder> builder(const std::shared_ptr<GameEntity> &e) const {
		return std::dynamic_pointer_cast<component::Builder>(e->get_component(component::component_t::BUILDER));
	}

	std::shared_ptr<component::Constructable> constructable(const std::shared_ptr<GameEntity> &e) const {
		if (e == nullptr or not e->has_component(component::component_t::CONSTRUCTABLE)) {
			return nullptr;
		}
		return std::dynamic_pointer_cast<component::Constructable>(e->get_component(component::component_t::CONSTRUCTABLE));
	}

	double progress(const std::shared_ptr<GameEntity> &e) const {
		auto c = this->constructable(e);
		return c ? c->get_progress() : 0.0;
	}

	int64_t health(const std::shared_ptr<GameEntity> &e) const {
		auto stats = this->state->get_combat()->get_stats(e->get_id());
		auto live = std::dynamic_pointer_cast<component::Live>(e->get_component(component::component_t::LIVE));
		if (stats == nullptr or live == nullptr) {
			return -1;
		}
		return live->get_attribute(this->now, stats->health_attribute).value_or(-1);
	}

	int64_t max_health(const std::shared_ptr<GameEntity> &e) const {
		auto stats = this->state->get_combat()->get_stats(e->get_id());
		return stats ? stats->max_health : -1;
	}

	void set_health(const std::shared_ptr<GameEntity> &e, int64_t value) {
		auto stats = this->state->get_combat()->get_stats(e->get_id());
		auto live = std::dynamic_pointer_cast<component::Live>(e->get_component(component::component_t::LIVE));
		live->set_attribute(this->now, stats->health_attribute, value);
		this->state->get_combat()->health_changed(e->get_id(), value);
	}

	void order(const std::shared_ptr<GameEntity> &unit, const std::shared_ptr<component::command::Command> &command) {
		auto queue = std::dynamic_pointer_cast<component::CommandQueue>(
			unit->get_component(component::component_t::COMMANDQUEUE));
		queue->add_command(this->now, command);
	}

	void move_to(const std::shared_ptr<GameEntity> &unit, double ne, double se) {
		auto dest = this->state->get_map()->on_terrain(coord::phys3{coord::phys_t{ne}, coord::phys_t{se}, coord::phys_t{0.0}});
		this->order(unit, std::make_shared<component::command::MoveCommand>(dest));
	}

	/// a command like the input sends it (game.send_command)
	void command(component::command::command_t type, const std::vector<entity_id_t> &ids, const coord::phys3 &target,
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

	void select(const std::vector<std::shared_ptr<GameEntity>> &entities) {
		std::vector<entity_id_t> ids;
		for (const auto &e : entities) {
			ids.push_back(e->get_id());
		}
		this->production->set_selection(ids);
		this->advance(0.2);
	}

	void add_resources(player_id_t player, double amount) {
		auto &stock = this->state->get_player(player)->get_resources();
		for (size_t i = 0; i < RESOURCE_COUNT; ++i) {
			stock.add(static_cast<resource_t>(i), amount);
		}
	}

	double resource(player_id_t player, resource_t type) const {
		return this->state->get_player(player)->get_resources().get(type);
	}

	void set_resource(player_id_t player, resource_t type, double amount) {
		auto &stock = this->state->get_player(player)->get_resources();
		auto all = stock.get();
		all[static_cast<size_t>(type)] = amount;
		stock.set(all);
	}

	/// a finished entity of a player at a tile (production test option, blocks building tiles)
	std::shared_ptr<GameEntity> spawn(player_id_t player, const std::string &name, double ne, double se) {
		auto before = this->of(player, name);
		this->production->test_spawn(player, name, ne, se);
		this->advance(0.1);
		auto after = this->of(player, name);
		for (const auto &e : after) {
			if (std::find(before.begin(), before.end(), e) == before.end()) {
				return e;
			}
		}
		return nullptr;
	}

	path::grid_id_t land() const {
		return this->state->get_map()->get_grid_id("hd_base.data.util.path_type.types.Land");
	}

	bool passable(long ne, long se) const {
		return this->state->get_map()->is_passable(this->land(), coord::tile{static_cast<coord::tile_t>(ne),
		                                                                     static_cast<coord::tile_t>(se)});
	}

	/// all tiles of the rectangle are passable land without units
	bool free_rect(long ne0, long se0, long ne1, long se1) const {
		std::set<std::pair<long, long>> units;
		for (const auto &[id, e] : this->state->get_game_entities()) {
			if (e->has_component(component::component_t::MOVE)) {
				auto p = this->position(e);
				units.insert({static_cast<long>(std::floor(p.ne.to_double())), static_cast<long>(std::floor(p.se.to_double()))});
			}
		}
		const long n = static_cast<long>(this->state->get_map()->get_size()[0]);
		for (long y = se0; y <= se1; ++y) {
			for (long x = ne0; x <= ne1; ++x) {
				if (x < 1 or y < 1 or x >= n - 1 or y >= n - 1 or not this->passable(x, y) or units.contains({x, y})) {
					return false;
				}
			}
		}
		return true;
	}

	/// place a building of a player with these builders near a point (rings of spots)
	std::shared_ptr<GameEntity> place_near(player_id_t player, const std::vector<std::shared_ptr<GameEntity>> &builders,
	                                       const std::string &name, double ne, double se, int max_ring = 8) {
		std::vector<entity_id_t> ids;
		for (const auto &b : builders) {
			ids.push_back(b->get_id());
		}
		const size_t before = this->of(player, name).size();
		for (int r = 0; r <= max_ring; ++r) {
			for (int dy = -r; dy <= r; ++dy) {
				for (int dx = -r; dx <= r; ++dx) {
					if (std::max(std::abs(dx), std::abs(dy)) != r) {
						continue;
					}
					coord::phys3 at{coord::phys_t{ne + dx}, coord::phys_t{se + dy}, coord::phys_t{0.0}};
					this->production->place_for(player, ids, name, at);
					this->advance(0.15);
					auto now_list = this->of(player, name);
					if (now_list.size() > before) {
						return now_list.back();
					}
				}
			}
		}
		return nullptr;
	}
};

coord::phys3 at(double ne, double se) {
	return coord::phys3{coord::phys_t{ne}, coord::phys_t{se}, coord::phys_t{0.0}};
}

/// a free horizontal run of \p length tiles with \p margin free rows above and below, near (cx, cy)
std::optional<std::pair<long, long>> free_row(const World &w, double cx, double cy, long length, long margin) {
	std::optional<std::pair<long, long>> best;
	double best_d = 1e9;
	const long n = static_cast<long>(w.state->get_map()->get_size()[0]);
	for (long y = margin + 1; y + margin < n - 1; ++y) {
		for (long x = 1; x + length < n - 1; ++x) {
			const double d = std::hypot(x + 0.5 * length - cx, y - cy);
			if (d >= best_d or d < 6.0) {
				continue;
			}
			if (w.free_rect(x - 1, y - margin, x + length, y + margin)) {
				best_d = d;
				best = std::pair{x, y};
			}
		}
	}
	return best;
}

// ------------------------------------------------------------------ B1

void check_builders(const Options &opt) {
	std::cout << "== B1 Mehrere Bauer, Baugeschwindigkeit, Folgeverhalten (L33)" << std::endl;
	MapSettings base;
	base.ai.mode = ai_mode_t::OFF;
	base.vision = vision_mode_t::ALL;
	World w{opt, base};
	w.advance(1.0);
	w.add_resources(0, 2000.0);
	auto tc = w.town_center(0);
	check(tc != nullptr, "town centre of player 0");
	if (tc == nullptr) {
		return;
	}
	const auto tcp = w.position(tc);
	const double tx = tcp.ne.to_double();
	const double ty = tcp.se.to_double();
	// villagers: the start villagers and new ones from the test option
	std::vector<std::shared_ptr<GameEntity>> v = w.of(0, "Villager");
	for (int i = 0; v.size() < 6 and i < 20; ++i) {
		auto spot = free_row(w, tx + 3.0 + i, ty + 3.0, 1, 0);
		if (spot) {
			w.spawn(0, "Villager", spot->first + 0.5, spot->second + 0.5);
		}
		v = w.of(0, "Villager");
	}
	check(v.size() >= 6, "six villagers (" + std::to_string(v.size()) + ")");
	if (v.size() < 6) {
		return;
	}
	auto one = w.place_near(0, {v[0]}, "House", tx - 6.0, ty - 1.0);
	auto three = w.place_near(0, {v[1], v[2], v[3]}, "House", tx - 6.0, ty + 4.0);
	check(one != nullptr and three != nullptr, "two house foundations");
	if (one == nullptr or three == nullptr) {
		return;
	}
	auto building = [&](const std::shared_ptr<GameEntity> &u, const std::shared_ptr<GameEntity> &target) {
		const auto &job = w.builder(u)->get_job();
		return job.phase == component::Builder::phase_t::BUILDING and job.target == target->get_id();
	};
	w.advance_until(40.0, [&]() {
		return building(v[0], one) and building(v[1], three) and building(v[2], three) and building(v[3], three);
	});
	check(building(v[0], one) and building(v[1], three) and building(v[2], three) and building(v[3], three),
	      "all builders at work");
	const double a1 = w.progress(one);
	const double a3 = w.progress(three);
	w.advance(5.0);
	const double r1 = w.progress(one) - a1;
	const double r3 = w.progress(three) - a3;
	const double ratio = r1 > 0.0 ? r3 / r1 : 0.0;
	check(std::abs(ratio - (1.0 + 2.0 * building::BUILD_TEAM_FACTOR)) < 0.12,
	      "three builders build " + fmt(ratio) + " times as fast as one (f(n): 1.66)");
	// health grows with the progress (spec 04 §4.3)
	const double share = static_cast<double>(w.health(one)) / static_cast<double>(w.max_health(one));
	check(w.progress(one) < 0.99 and std::abs(share - w.progress(one)) < 0.05,
	      "health follows the progress (" + fmt(share) + " / " + fmt(w.progress(one)) + ")");
	// build speed k (player resource 195): 2 doubles the rate of the single builder
	w.buildings()->set_build_speed(0, 2.0);
	const double b1 = w.progress(one);
	w.advance(4.0);
	const double k_rate = (w.progress(one) - b1) / 4.0;
	w.buildings()->set_build_speed(0, 1.0);
	check(std::abs(k_rate / (r1 / 5.0) - 2.0) < 0.2, "build speed 2 doubles the rate (" + fmt(k_rate / (r1 / 5.0)) + ")");

	// L33: the team of the finished house looks for the other foundation nearby
	w.advance_until(60.0, [&]() { return prod::is_finished(three); });
	check(prod::is_finished(three), "house of three builders finished");
	const bool other_open = not prod::is_finished(one);
	bool joined = false;
	w.advance_until(4.0, [&]() {
		for (size_t i = 1; i <= 3; ++i) {
			const auto &job = w.builder(v[i])->get_job();
			joined = joined or job.target == one->get_id();
		}
		return joined;
	});
	check(not other_open or joined, "a builder of the finished house builds the foundation nearby (L33)");
	w.advance_until(60.0, [&]() { return prod::is_finished(one); });
	w.advance(4.0);
	bool idle = true;
	for (size_t i = 0; i <= 3; ++i) {
		idle = idle and w.builder(v[i])->get_job().phase == component::Builder::phase_t::NONE;
	}
	check(idle, "without a foundation nearby the builders go idle after the search (3 s)");

	// L33: the builder of a lumber camp cuts wood next to it
	std::shared_ptr<GameEntity> tree;
	double best = 1e9;
	for (const auto &[id, e] : w.state->get_game_entities()) {
		if (not e->has_component(component::component_t::HARVESTABLE)) {
			continue;
		}
		auto h = std::dynamic_pointer_cast<component::Harvestable>(e->get_component(component::component_t::HARVESTABLE));
		if (h->get_resource() != resource_t::WOOD or h->is_depleted()) {
			continue;
		}
		auto p = w.position(e);
		const double d = std::hypot(p.ne.to_double() - tx, p.se.to_double() - ty);
		if (d < best and d > 6.0) {
			best = d;
			tree = e;
		}
	}
	check(tree != nullptr, "a tree for the lumber camp");
	if (tree == nullptr) {
		return;
	}
	auto treep = w.position(tree);
	const double dx = tx - treep.ne.to_double();
	const double dy = ty - treep.se.to_double();
	const double dl = std::max(1.0, std::hypot(dx, dy));
	auto camp = w.place_near(0, {v[4]}, "LumberCamp", treep.ne.to_double() + 3.0 * dx / dl,
	                         treep.se.to_double() + 3.0 * dy / dl, 6);
	check(camp != nullptr, "lumber camp placed");
	if (camp == nullptr) {
		return;
	}
	w.advance_until(120.0, [&]() { return prod::is_finished(camp); });
	check(prod::is_finished(camp), "lumber camp finished");
	auto gather = std::dynamic_pointer_cast<component::Gather>(v[4]->get_component(component::component_t::GATHER));
	w.advance_until(5.0, [&]() { return gather->get_job().target.has_value(); });
	bool wood = false;
	if (gather->get_job().target and w.state->get_game_entities().contains(*gather->get_job().target)) {
		auto t = w.state->get_game_entity(*gather->get_job().target);
		auto h = std::dynamic_pointer_cast<component::Harvestable>(t->get_component(component::component_t::HARVESTABLE));
		wood = h != nullptr and h->get_resource() == resource_t::WOOD;
	}
	check(wood, "the camp builder cuts wood next to it (L33)");
}

// ------------------------------------------------------------------ B2

void check_palisade(const Options &opt) {
	std::cout << "== B2 Palisade ziehen: Vorschau, Segmente, Kosten, Bau der Reihe nach" << std::endl;
	MapSettings base;
	base.ai.mode = ai_mode_t::OFF;
	base.vision = vision_mode_t::ALL;
	World w{opt, base};
	w.advance(1.0);
	w.add_resources(0, 2000.0);
	auto tc = w.town_center(0);
	auto villagers = w.of(0, "Villager");
	check(tc != nullptr and villagers.size() >= 2, "town centre and villagers");
	if (tc == nullptr or villagers.size() < 2) {
		return;
	}
	auto palisade = w.builder(villagers[0])->find("PalisadeWall");
	check(palisade != nullptr, "villagers build palisades (data)");
	if (palisade == nullptr) {
		return;
	}
	const double cost = palisade->cost[static_cast<size_t>(resource_t::WOOD)];
	check(cost > 0.0, "palisade costs wood (" + fmt(cost) + " per segment)");
	w.select({villagers[0]});
	auto snap = w.production->snapshot();
	bool offered = false;
	for (const auto &o : snap.options) {
		offered = offered or (o.id == "PalisadeWall" and o.available);
	}
	check(offered, "the villager offers the palisade (catalog: WALLS freed)");

	// a drag across the town centre: its tiles are invalid and skipped
	const auto tcp = w.position(tc);
	const long ty = static_cast<long>(std::floor(tcp.se.to_double()));
	const long tx = static_cast<long>(std::floor(tcp.ne.to_double()));
	const auto start = at(tx - 6 + 0.5, ty + 0.5);
	const auto end = at(tx + 6 + 0.5, ty + 0.5);
	check(w.production->start_placement("PalisadeWall"), "placement mode of the palisade");
	check(w.production->placement_is_wall(), "the palisade is dragged");
	check(w.production->placement_press(start), "press starts the drag");
	w.production->set_placement_cursor(end);
	w.advance(0.5);
	auto preview = w.production->placement_preview();
	size_t valid = 0;
	for (const auto &s : preview.segments) {
		valid += s.valid ? 1 : 0;
	}
	check(preview.segments.size() == 13, "preview shows 13 segments of the line (" + std::to_string(preview.segments.size()) + ")");
	check(valid >= 5 and valid <= 9, "segments on the town centre are red (" + std::to_string(13 - valid) + " invalid)");
	const double wood_before = w.resource(0, resource_t::WOOD);
	const size_t walls_before = w.of(0, "PalisadeWall").size();
	check(w.production->place_at(end), "release places the wall");
	w.advance(0.5);
	auto walls = w.of(0, "PalisadeWall");
	const size_t placed = walls.size() - walls_before;
	check(placed == valid, "valid segments became foundations (" + std::to_string(placed) + " of " + std::to_string(valid) + ")");
	check(std::abs(wood_before - w.resource(0, resource_t::WOOD) - cost * static_cast<double>(placed)) < 1e-6,
	      "costs per placed segment paid at once (" + fmt(wood_before - w.resource(0, resource_t::WOOD)) + " wood)");
	check(w.builder(villagers[0])->wall_queue.size() + 1 == placed, "the villager queues the other segments");
	// it builds one after the other; record the order of completion
	std::vector<entity_id_t> order;
	w.advance_until(placed * 30.0, [&]() {
		for (const auto &wall : walls) {
			if (prod::is_finished(wall) and std::find(order.begin(), order.end(), wall->get_id()) == order.end()) {
				order.push_back(wall->get_id());
			}
		}
		return order.size() == walls.size();
	});
	check(order.size() == walls.size(), "the villager built all segments (" + std::to_string(order.size()) + ")");
	// nearest first: the villager works along the line; it jumps at most twice (over the town centre, and back
	// to the other end when it started in the middle)
	size_t jumps = 0;
	std::string seq;
	for (size_t i = 0; i < order.size(); ++i) {
		auto b = w.position(w.state->get_game_entity(order[i]));
		seq += " " + std::to_string(static_cast<long>(std::floor(b.ne.to_double())) - tx);
		if (i > 0) {
			auto a = w.position(w.state->get_game_entity(order[i - 1]));
			jumps += std::abs(a.ne.to_double() - b.ne.to_double()) > 1.5 ? 1 : 0;
		}
	}
	check(jumps <= 2, "segments built nearest first (order" + seq + ")");
	check(w.builder(villagers[0])->wall_queue.empty(), "wall queue done");
	// a plain click on a wall: one segment
	w.production->start_placement("PalisadeWall");
	const auto single = at(tx + 0.5, ty - 6 + 0.5);
	w.production->place_at(single);
	w.advance(0.5);
	check(w.of(0, "PalisadeWall").size() == walls.size() + 1, "a click without drag places one segment");
}

// ------------------------------------------------------------------ B3

void check_gate(const Options &opt) {
	std::cout << "== B3 Tor: auf Mauer setzen, Durchlass (L32), Verriegeln" << std::endl;
	MapSettings base;
	base.ai.mode = ai_mode_t::OFF;
	base.vision = vision_mode_t::ALL;
	World w{opt, base};
	w.advance(1.0);
	w.add_resources(0, 5000.0);
	w.production->grant_research(0, "FeudalAge");
	w.advance(1.0);
	auto tc = w.town_center(0);
	auto villagers = w.of(0, "Villager");
	if (tc == nullptr or villagers.size() < 2) {
		check(false, "town centre and villagers");
		return;
	}
	const auto tcp = w.position(tc);
	const long length = 15;
	auto row = free_row(w, tcp.ne.to_double(), tcp.se.to_double() - 8.0, length, 3);
	check(row.has_value(), "a free row of 15 tiles for a stone wall");
	if (not row) {
		return;
	}
	const auto [x0, y] = *row;
	for (long i = 0; i < length; ++i) {
		w.spawn(0, "StoneWall", x0 + i + 0.5, y + 0.5);
	}
	check(w.of(0, "StoneWall").size() == static_cast<size_t>(length), "stone wall of 15 segments");
	// the gate needs an own straight wall: not on open ground
	w.select({villagers[0]});
	check(w.production->start_placement("StoneGate"), "placement mode of the gate");
	w.production->set_placement_cursor(at(x0 + 7.5, y + 3.5));
	w.advance(0.5);
	check(not w.production->placement_preview().valid, "no gate on open ground");
	w.production->set_placement_cursor(at(x0 + 7.5, y + 0.5));
	w.advance(0.5);
	auto preview = w.production->placement_preview();
	check(preview.valid and preview.fixed_anchor, "gate fits on the wall (preview)");
	w.production->place_at(at(x0 + 7.5, y + 0.5));
	w.advance(0.5);
	auto gates = w.of(0, "StoneGate");
	check(gates.size() == 1, "gate foundation placed");
	check(w.of(0, "StoneWall").size() == static_cast<size_t>(length - building::GATE_LENGTH), "the gate replaced 4 segments");
	if (gates.size() != 1) {
		return;
	}
	auto gate = gates.front();
	const auto *g = w.buildings()->gate(gate->get_id());
	check(g != nullptr and g->dir == building::gate_dir_t::EAST_WEST, "gate along the wall (east-west)");
	if (g == nullptr) {
		return;
	}
	bool blocked = true;
	for (const auto &[gx, gy] : g->tiles()) {
		blocked = blocked and not w.passable(gx, gy);
	}
	check(blocked, "the gate foundation blocks its tiles");
	w.select({villagers[0], villagers[1]});
	w.advance_until(200.0, [&]() { return prod::is_finished(gate); });
	check(prod::is_finished(gate), "gate built");
	bool open = true;
	for (const auto &[gx, gy] : g->tiles()) {
		open = open and w.passable(gx, gy) and not w.buildings()->blocks(0, gx, gy) and w.buildings()->blocks(1, gx, gy);
	}
	check(open, "open gate: passable for the owner, blocked for player 1 (L32)");
	// tiles next to the gate are still walls
	check(not w.passable(x0 + 2, y) and not w.passable(x0 + 12, y), "wall segments beside the gate still block");

	// an own villager walks through, an enemy stops in front of it
	const double gx = x0 + 7.5;
	auto walker = villagers[0];
	w.order(walker, std::make_shared<component::command::IdleCommand>());
	auto enemy = w.spawn(1, "Militia", gx + 0.5, y - 2.0 + 0.5);
	check(enemy != nullptr, "enemy militia north of the gate");
	if (enemy == nullptr) {
		return;
	}
	w.state->get_combat()->set_stance(enemy->get_id(), combat::stance_t::NO_ATTACK);
	auto own = w.spawn(0, "Villager", gx - 0.5, y - 2.0 + 0.5);
	// walk for a while; true if the unit stood on a tile of the gate on its way
	std::set<std::pair<long, long>> gate_tiles;
	for (const auto &[tx, ty] : g->tiles()) {
		gate_tiles.insert({tx, ty});
	}
	auto walk = [&](const std::vector<std::shared_ptr<GameEntity>> &units, double seconds) {
		std::vector<bool> on(units.size(), false);
		for (double t = 0.0; t < seconds; t += 0.25) {
			w.advance(0.25);
			for (size_t i = 0; i < units.size(); ++i) {
				auto p = w.position(units[i]);
				on[i] = on[i] or gate_tiles.contains({static_cast<long>(std::floor(p.ne.to_double())),
				                                      static_cast<long>(std::floor(p.se.to_double()))});
			}
		}
		return on;
	};
	w.move_to(own, gx - 0.5, y + 3.5);
	w.move_to(enemy, gx + 0.5, y + 3.5);
	auto through = walk({own, enemy}, 20.0);
	check(through[0] and w.position(own).se.to_double() > y + 1.0,
	      "own villager walked through the gate (se " + fmt(w.position(own).se.to_double()) + ")");
	check(not through[1] and w.position(enemy).se.to_double() < y,
	      "enemy stopped in front of the gate (se " + fmt(w.position(enemy).se.to_double()) + ")");

	// locked (command "Verriegeln"): blocks everyone
	w.command(component::command::command_t::GATE_LOCK, {gate->get_id()}, w.position(gate), std::nullopt, 1);
	w.advance(0.2);
	check(g->locked and w.buildings()->blocks(0, g->anchor.first, g->anchor.second)
	          and not w.passable(g->anchor.first, g->anchor.second),
	      "locked gate blocks the owner too");
	check(w.buildings()->is_locked_shared(gate->get_id()) == std::optional<bool>{true}, "HUD sees the lock");
	w.move_to(own, gx - 0.5, y - 3.5);
	// it may walk around the end of the wall, but never over the locked gate
	check(not walk({own}, 15.0)[0], "own villager cannot pass the locked gate");
	w.command(component::command::command_t::GATE_LOCK, {gate->get_id()}, w.position(gate), std::nullopt, 0);
	w.advance(0.2);
	check(not g->locked and w.passable(g->anchor.first, g->anchor.second), "unlocked gate opens again");
	auto second = w.spawn(0, "Villager", gx - 0.5, y + 2.5);
	w.move_to(second, gx - 0.5, y - 3.5);
	check(walk({second}, 15.0)[0] and w.position(second).se.to_double() < y, "own villager walks through the unlocked gate");
	// a destroyed gate frees exactly its 4 tiles
	w.state->get_combat()->kill(w.state, gate->get_id(), w.now);
	w.advance(10.0);
	check(not w.passable(x0 + 2, y) and not w.passable(x0 + 12, y) and w.passable(x0 + 7, y),
	      "destroyed gate frees its tiles, the walls stay");
	check(w.buildings()->gate(gate->get_id()) == nullptr, "gate forgotten after its removal");
}

// ------------------------------------------------------------------ B4

void check_repair(const Options &opt) {
	std::cout << "== B4 Reparatur: LP, Kosten, mehrere Reparierer, Abbruch" << std::endl;
	MapSettings base;
	base.ai.mode = ai_mode_t::OFF;
	base.vision = vision_mode_t::ALL;
	World w{opt, base};
	w.advance(1.0);
	w.add_resources(0, 2000.0);
	auto tc = w.town_center(0);
	auto villagers = w.of(0, "Villager");
	if (tc == nullptr or villagers.size() < 3) {
		check(false, "town centre and villagers");
		return;
	}
	const auto tcp = w.position(tc);
	auto house = w.place_near(0, {villagers[0]}, "House", tcp.ne.to_double() - 5.0, tcp.se.to_double() - 2.0);
	check(house != nullptr, "house placed");
	if (house == nullptr) {
		return;
	}
	w.advance_until(120.0, [&]() { return prod::is_finished(house); });
	w.advance(4.0);
	const int64_t max = w.max_health(house);
	check(prod::is_finished(house) and w.health(house) == max, "house finished at full health (" + std::to_string(max) + ")");
	check(not prod::is_damaged(w.state, house, w.now), "a whole house needs no repair");
	w.set_health(house, max / 4);
	check(prod::is_damaged(w.state, house, w.now), "damaged house can be repaired");
	const auto *cost = w.builder(villagers[0])->find("House");
	const double house_wood = cost ? cost->cost[static_cast<size_t>(resource_t::WOOD)] : 0.0;
	const double wood_before = w.resource(0, resource_t::WOOD);
	// one repairer (button "Reparieren", then the house)
	w.command(component::command::command_t::REPAIR, {villagers[0]->get_id()}, w.position(house), house->get_id());
	w.advance_until(20.0, [&]() {
		const auto &job = w.builder(villagers[0])->get_job();
		return job.repair and job.phase == component::Builder::phase_t::BUILDING;
	});
	check(w.builder(villagers[0])->get_job().repair, "the villager repairs");
	int64_t h0 = w.health(house);
	w.advance(4.0);
	const double rate1 = static_cast<double>(w.health(house) - h0) / 4.0;
	check(std::abs(rate1 - 12.5) < 1.5, "one repairer: " + fmt(rate1) + " HP/s (12.5)");
	// a second repairer: 1.5 times (f(n) with the half factor)
	w.command(component::command::command_t::REPAIR, {villagers[1]->get_id()}, w.position(house), house->get_id());
	w.advance_until(20.0, [&]() {
		const auto &job = w.builder(villagers[1])->get_job();
		return job.repair and job.phase == component::Builder::phase_t::BUILDING;
	});
	h0 = w.health(house);
	w.advance(4.0);
	const double rate2 = static_cast<double>(w.health(house) - h0) / 4.0;
	check(std::abs(rate2 / rate1 - (1.0 + building::REPAIR_TEAM_FACTOR)) < 0.15,
	      "two repairers: " + fmt(rate2 / rate1) + " times (1.5)");
	w.advance_until(120.0, [&]() { return w.health(house) >= max; });
	check(w.health(house) == max, "house repaired to full health");
	const double paid = wood_before - w.resource(0, resource_t::WOOD);
	const double expected = house_wood * static_cast<double>(max - max / 4) / (2.0 * static_cast<double>(max));
	// whole units are paid; each repairer may still owe less than one unit
	check(paid <= expected + 1e-6 and expected - paid < 2.0,
	      "repair cost " + fmt(paid) + " wood (formula " + fmt(expected) + ", two repairers owe < 1 each)");
	w.advance(1.0);
	check(not w.builder(villagers[0])->get_job().repair, "repairers stop at full health");

	// without wood the repair stops with a message
	w.set_health(house, max / 4);
	w.set_resource(0, resource_t::WOOD, 0.0);
	w.command(component::command::command_t::REPAIR, {villagers[2]->get_id()}, w.position(house), house->get_id());
	w.advance(15.0);
	check(w.health(house) < max / 2 and not w.builder(villagers[2])->get_job().repair,
	      "repair stops without wood (health " + std::to_string(w.health(house)) + ")");
	check(w.production->snapshot().status.find("Reparatur abgebrochen") != std::string::npos,
	      "message: " + w.production->snapshot().status);
	// a right click on the damaged house (move command) also repairs
	w.add_resources(0, 500.0);
	w.command(component::command::command_t::MOVE, {villagers[2]->get_id()}, w.position(house));
	w.advance_until(20.0, [&]() { return w.builder(villagers[2])->get_job().repair; });
	check(w.builder(villagers[2])->get_job().repair, "right click on the damaged house repairs it");
}

// ------------------------------------------------------------------ B5

void check_castle(const Options &opt) {
	std::cout << "== B5 Auswurf < 20 %, Burg, Bombardenturm, Erkundung" << std::endl;
	MapSettings base;
	base.ai.mode = ai_mode_t::OFF;
	base.vision = vision_mode_t::NORMAL;
	World w{opt, base};
	w.advance(1.0);
	w.add_resources(0, 5000.0);
	auto tc = w.town_center(0);
	auto villagers = w.of(0, "Villager");
	if (tc == nullptr or villagers.size() < 3) {
		check(false, "town centre and villagers");
		return;
	}
	auto combat = w.state->get_combat();
	// garrison ejection (spec 04 §4.6)
	for (size_t i = 0; i < 2; ++i) {
		combat->order_garrison(w.state, villagers[i]->get_id(), tc->get_id(), w.now);
	}
	w.advance_until(30.0, [&]() {
		return combat->garrisoned_in(villagers[0]->get_id()) and combat->garrisoned_in(villagers[1]->get_id());
	});
	check(combat->garrisoned_in(villagers[0]->get_id()).has_value(), "villagers in the town centre");
	const int64_t max = w.max_health(tc);
	w.set_health(tc, max / 5 + 2);
	const auto tcp = w.position(tc);
	auto enemy = w.spawn(1, "Militia", tcp.ne.to_double() + 3.0, tcp.se.to_double() + 3.0);
	if (enemy != nullptr) {
		combat->set_stance(enemy->get_id(), combat::stance_t::NO_ATTACK);
		for (int i = 0; i < 6 and w.health(tc) >= max / 5; ++i) {
			combat->strike(w.state, enemy->get_id(), tc->get_id(), w.now);
			w.advance(2.5);
		}
	}
	check(w.health(tc) < max / 5, "town centre below 20 % (" + std::to_string(w.health(tc)) + "/" + std::to_string(max) + ")");
	w.advance(1.0);
	check(not combat->garrisoned_in(villagers[0]->get_id()) and not combat->garrisoned_in(villagers[1]->get_id()),
	      "garrison ejected below 20 % (building_math must_eject_garrison)");

	// castle and bombard tower: buildable with the data (age), garrison and arrows
	for (const auto *name : {"Castle", "BombardTower"}) {
		const auto *b = w.builder(villagers[2])->find(name);
		check(b != nullptr, std::string{name} + " in the build list of the villager (data)");
		check(prod::has_mechanic(name), std::string{name} + " offered by the catalog");
	}
	w.production->grant_research(0, "FeudalAge");
	w.production->grant_research(0, "CastleAge");
	w.production->grant_research(0, "ImperialAge");
	w.advance(1.0);
	auto vision = w.state->get_vision();
	for (const auto *name : {"Castle", "BombardTower"}) {
		auto spot = free_row(w, tcp.ne.to_double(), tcp.se.to_double() + 9.0, 5, 3);
		std::shared_ptr<GameEntity> b;
		if (spot) {
			b = w.spawn(0, name, spot->first + 2.5, spot->second + 0.5);
		}
		auto stats = b ? combat->get_stats(b->get_id()) : nullptr;
		check(stats != nullptr and stats->garrison_capacity > 0 and stats->can_attack,
		      std::string{name} + ": garrison " + (stats ? std::to_string(stats->garrison_capacity) : "-") + ", shoots "
		          + (stats and stats->can_attack ? "yes" : "no"));
		if (b) {
			w.state->get_combat()->remove(w.state, b->get_id(), w.now);
			w.advance(0.2);
		}
	}
	w.select({villagers[2]});
	w.advance(0.5);
	auto snap = w.production->snapshot();
	bool castle = false;
	for (const auto &o : snap.options) {
		castle = castle or (o.id == "Castle" and o.available);
	}
	check(castle, "castle offered in the Castle Age");

	// placement rule 3: unexplored ground is rejected (vision normal)
	check(w.production->start_placement("House"), "placement mode of a house");
	const double far_ne = tcp.ne.to_double() > 32.0 ? 3.5 : 60.5;
	const double far_se = tcp.se.to_double() > 32.0 ? 3.5 : 60.5;
	check(vision->footprint_state(0, far_ne, far_se, 0.75) == vision::tile_state_t::UNEXPLORED, "far corner unexplored");
	w.production->set_placement_cursor(at(far_ne, far_se));
	w.advance(0.5);
	auto preview = w.production->placement_preview();
	check(not preview.valid and preview.reason.find("unerkundet") != std::string::npos,
	      "no house on unexplored ground (" + preview.reason + ")");
	w.production->set_placement_cursor(at(tcp.ne.to_double() - 6.0, tcp.se.to_double() - 6.0));
	w.advance(0.5);
	preview = w.production->placement_preview();
	check(preview.reason.find("unerkundet") == std::string::npos, "explored ground near the town centre (" + preview.reason + ")");
	w.production->cancel_placement();
}

// ------------------------------------------------------------------ B6

void check_save(const Options &opt) {
	std::cout << "== B6 Spielstand Version 6: Tor, Mauerzug" << std::endl;
	MapSettings base;
	base.ai.mode = ai_mode_t::OFF;
	base.vision = vision_mode_t::ALL;
	World w{opt, base};
	w.advance(1.0);
	w.add_resources(0, 5000.0);
	w.production->grant_research(0, "FeudalAge");
	w.advance(1.0);
	auto tc = w.town_center(0);
	auto villagers = w.of(0, "Villager");
	if (tc == nullptr or villagers.size() < 2) {
		check(false, "town centre and villagers");
		return;
	}
	const auto tcp = w.position(tc);
	auto row = free_row(w, tcp.ne.to_double(), tcp.se.to_double() - 8.0, 8, 2);
	auto row2 = free_row(w, tcp.ne.to_double(), tcp.se.to_double() + 8.0, 8, 1);
	if (not row or not row2) {
		check(false, "free rows for the walls");
		return;
	}
	for (long i = 0; i < 6; ++i) {
		w.spawn(0, "StoneWall", row->first + i + 0.5, row->second + 0.5);
	}
	w.select({villagers[0]});
	w.production->start_placement("StoneGate");
	w.production->place_at(at(row->first + 2.5, row->second + 0.5));
	w.advance(0.5);
	auto gates = w.of(0, "StoneGate");
	check(gates.size() == 1, "gate for the save");
	if (gates.empty()) {
		return;
	}
	w.state->get_buildings()->set_locked(w.state, gates.front()->get_id(), 0, true, w.now);
	// a palisade drag of a second villager, still in work
	w.select({villagers[1]});
	w.production->start_placement("PalisadeWall");
	w.production->placement_press(at(row2->first + 0.5, row2->second + 0.5));
	w.production->place_at(at(row2->first + 7.5, row2->second + 0.5));
	w.advance(3.0);
	const size_t queued = w.builder(villagers[1])->wall_queue.size();
	check(queued >= 5, "wall drag in work (" + std::to_string(queued) + " queued)");
	auto data = save::capture(w.state, w.settings, w.game->get_generated_entity_range(), w.now, "W5");
	const auto text = save::serialize(data);
	check(text.find("version = 8") != std::string::npos, "save version 8");
	check(text.find("gate_dir = ") != std::string::npos and text.find("gate_locked = 1") != std::string::npos, "gate stored");
	check(text.find("wall_queue = ") != std::string::npos, "wall drag stored");
	save::SaveData parsed;
	std::string error;
	check(save::parse(text, parsed, error), "parsed: " + error);
	World l{opt, w.settings};
	auto result = save::restore(parsed, l.state, l.loop, l.factory, l.production, l.game->get_generated_entity_range(), l.now);
	check(result.ok, "restored: " + result.error);
	l.advance(1.0);
	auto lgates = l.of(0, "StoneGate");
	const building::Gate *lg = lgates.size() == 1 ? l.buildings()->gate(lgates.front()->get_id()) : nullptr;
	check(lg != nullptr and lg->locked and lg->dir == building::gate_dir_t::EAST_WEST
	          and lg->anchor == building::tile_t{static_cast<int>(row->first) + 1, static_cast<int>(row->second)},
	      "gate restored (locked, direction, tiles)");
	if (lg != nullptr) {
		bool blocks = true;
		std::string tiles;
		for (const auto &[gx, gy] : lg->tiles()) {
			blocks = blocks and not l.passable(gx, gy);
			tiles += l.passable(gx, gy) ? " open" : " blocked";
		}
		check(blocks, "restored gate blocks its tiles (" + tiles + ", progress " + fmt(l.progress(lgates.front()))
		                  + (lg->finished ? ", finished" : ", foundation") + ")");
	}
	size_t queue_after = 0;
	for (const auto &v : l.of(0, "Villager")) {
		queue_after = std::max(queue_after, l.builder(v)->wall_queue.size());
	}
	check(queue_after + 1 >= queued, "wall drag restored (" + std::to_string(queue_after) + " queued)");
	l.advance_until(200.0, [&]() {
		size_t done = 0;
		for (const auto &p : l.of(0, "PalisadeWall")) {
			done += prod::is_finished(p) ? 1 : 0;
		}
		return done == l.of(0, "PalisadeWall").size();
	});
	size_t done = 0;
	for (const auto &p : l.of(0, "PalisadeWall")) {
		done += prod::is_finished(p) ? 1 : 0;
	}
	check(done == l.of(0, "PalisadeWall").size() and done >= 6, "the drag is finished after loading (" + std::to_string(done) + ")");
	// save -> load -> save keeps the W5 keys
	auto again = save::serialize(save::capture(l.state, l.settings, l.game->get_generated_entity_range(), l.now, "W5"));
	check(again.find("gate_locked = 1") != std::string::npos, "gate lock after save -> load -> save");
	// version 5 file: loads without W5 keys
	std::string v5 = text;
	v5.replace(v5.find("version = 8"), 11, "version = 5");
	save::SaveData old;
	check(save::parse(v5, old, error), "version 5 file parses: " + error);
}

// ------------------------------------------------------------------ B7

void check_ai(const Options &opt) {
	std::cout << "== B7 Computer-Gegner: Palisaden, Reparatur" << std::endl;
	MapSettings base;
	base.ai.mode = ai_mode_t::ON;
	base.ai.player = 1;
	base.ai.first_attack = 100000.0;
	base.vision = vision_mode_t::ALL;
	World w{opt, base};
	w.advance(2.0);
	w.production->grant_research(1, "FeudalAge");
	w.add_resources(1, 1500.0);
	w.advance(1.0);
	bool walls = w.advance_until(120.0, [&]() { return w.of(1, "PalisadeWall").size() >= 6; });
	check(walls, "AI drags palisades in the Feudal Age (" + std::to_string(w.of(1, "PalisadeWall").size()) + " segments)");
	auto tc = w.town_center(1);
	if (tc == nullptr) {
		check(false, "AI town centre");
		return;
	}
	const auto tcp = w.position(tc);
	double far = 0.0;
	for (const auto &p : w.of(1, "PalisadeWall")) {
		auto pp = w.position(p);
		far = std::max(far, std::max(std::abs(pp.ne.to_double() - tcp.ne.to_double()), std::abs(pp.se.to_double() - tcp.se.to_double())));
	}
	check(far > 8.0 and far < 16.0, "palisades on a ring around the town centre (" + fmt(far) + " tiles)");
	w.advance_until(240.0, [&]() {
		size_t done = 0;
		for (const auto &p : w.of(1, "PalisadeWall")) {
			done += prod::is_finished(p) ? 1 : 0;
		}
		return done >= 6;
	});
	size_t done = 0;
	for (const auto &p : w.of(1, "PalisadeWall")) {
		done += prod::is_finished(p) ? 1 : 0;
	}
	check(done >= 6, "AI villagers built palisades (" + std::to_string(done) + ")");
	// repair: a damaged house of the AI (or the town centre)
	auto houses = w.of(1, "House");
	auto target = houses.empty() ? tc : houses.front();
	w.advance_until(120.0, [&]() { return prod::is_finished(target); });
	w.set_health(target, w.max_health(target) / 2);
	bool repaired = w.advance_until(120.0, [&]() {
		for (const auto &v : w.of(1, "Villager")) {
			const auto &job = w.builder(v)->get_job();
			if (job.repair and job.target == target->get_id()) {
				return true;
			}
		}
		return false;
	});
	check(repaired, "AI villager repairs its damaged " + prod::entity_name(target));
	w.advance_until(120.0, [&]() { return w.health(target) >= w.max_health(target); });
	check(w.health(target) == w.max_health(target), "AI building repaired to full health");
}

} // namespace

int main(int argc, char **argv) {
	Options opt;
	bool have_root = false;
	std::string only;
	for (int i = 1; i < argc; ++i) {
		std::string a = argv[i];
		auto next = [&]() -> std::string {
			if (i + 1 >= argc) {
				std::cerr << "missing value for " << a << std::endl;
				std::exit(2);
			}
			return argv[++i];
		};
		if (a == "--root") {
			opt.root = util::Path{std::make_shared<util::fslike::Directory>(next()), {}};
			have_root = true;
		}
		else if (a == "--modpack") {
			opt.modpack = next();
		}
		else if (a == "--seed") {
			opt.seed = static_cast<uint32_t>(std::stoul(next()));
		}
		else if (a == "--size") {
			opt.size = std::stoul(next());
		}
		else if (a == "--only") {
			only = next();
		}
		else {
			std::cerr << "usage: openage-building-game-check --root <dir> [--modpack id] [--seed n] [--size n]"
			             " [--only builders|palisade|gate|repair|castle|save|ai]"
			          << std::endl;
			return 2;
		}
	}
	if (not have_root) {
		std::cerr << "--root fehlt" << std::endl;
		return 2;
	}
	log::set_level(log::level::info);
	try {
		if (only.empty() or only == "builders") {
			check_builders(opt);
		}
		if (only.empty() or only == "palisade") {
			check_palisade(opt);
		}
		if (only.empty() or only == "gate") {
			check_gate(opt);
		}
		if (only.empty() or only == "repair") {
			check_repair(opt);
		}
		if (only.empty() or only == "castle") {
			check_castle(opt);
		}
		if (only.empty() or only == "save") {
			check_save(opt);
		}
		if (only.empty() or only == "ai") {
			check_ai(opt);
		}
	}
	catch (std::exception &err) {
		std::cout << "building game check FAILED: " << err.what() << std::endl;
		return 1;
	}
	if (failures == 0) {
		std::cout << "building game check ok (" << passed << " checks)" << std::endl;
		return 0;
	}
	std::cout << "building game check: " << failures << " failures, " << passed << " ok" << std::endl;
	return 1;
}
